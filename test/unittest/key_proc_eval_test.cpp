/// @file        key_proc_eval_test.cpp
/// @brief       残留 `key_proc_eval` 的判据测试。
///
/// 夹具策略：jasm 汇编双类夹具（caller + proc，gate_processor_fold_test 同
/// 款装配面）。夹心形状取 Grunteon v3 生成面逐字（emitProcessorStateProgram
/// ：[INT; ISTORE s; ILOAD s; INT salt; INVOKESTATIC proc (II)I; ISTORE s]，
/// jsoup v3_full DataUtil.parseInputStream 实证形）；proc 体按 IndyRestore
/// 解锁后的直线纯算术形 + rotateLeft 盐旋转变体。
///
/// 覆盖分工：
/// - SCAN：夹心钉 + 跨方法求值（纯算术 / rotateLeft mock）/ clinit 暴露拒 /
///   TLR 体 bail 拒 / 非夹心非候选（不进桶）；
/// - REWRITE：夹心塌缩成 [常量; ISTORE]（调用消失、锚改规范常量装载、
///   proof ok + residual 归零）；干净 jar noop。
#include "grunt/key_proc_eval.hpp"

#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace grunt::key_proc_eval {
namespace {

namespace model = mjbase::model;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        sourceName = utf16(name + ".jasm");
    auto                        text       = utf16(source);
    auto                        assembled  = service.assemble(sourceName.ref(), text.ref());
    if (!assembled.isOk()) throw std::runtime_error("jasm 夹具汇编失败：" + name);
    return std::move(assembled.value->bytes);
}

std::vector<ca::u8> asJava17(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("夹具无法解析");
    session->backing().major = 61;
    auto encoded = jbc::core::writeClass(session->backing());
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
    return std::move(encoded).unwrap();
}

void addClass(model::ResourceContainer& container, const std::string& name,
              const std::string& source)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, asJava17(assemble(name, source))));
}

/// 纯算术 proc：proc(x, y) = (x + y) * 2 - 3（IndyRestore 解锁后的直线体形
/// ，jsoup v3_full KeyProcessor 实证）。
constexpr const char* kProcSource =
    ".super java/lang/Object\n"
    ".class public super fixture/KpeProc {\n"
    "  .method public static proc (II)I {\n"
    "    code: {\n"
    "      iload 0\n"
    "      iload 1\n"
    "      iadd\n"
    "      iconst_2\n"
    "      imul\n"
    "      iconst_3\n"
    "      isub\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// caller 夹具：m() 里一条生成面逐字夹心（slot 1），尾 iload/ireturn 让方法
/// 值语义可见（改写前后都 = proc(100, 20)）。
std::string sandwichCaller()
{
    return ".super java/lang/Object\n"
           ".class public super fixture/KpeCaller {\n"
           "  .method public static m ()I {\n"
           "    code: {\n"
           "      bipush 100\n"
           "      istore 1\n"
           "      iload 1\n"
           "      bipush 20\n"
           "      invokestatic fixture/KpeProc.proc (II)I\n"
           "      istore 1\n"
           "      iload 1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 直线双参调用（javac 形）：同 desc、同 proc，无 store-load 夹心——非候选。
std::string plainCaller()
{
    return ".super java/lang/Object\n"
           ".class public super fixture/KpeCaller {\n"
           "  .method public static m ()I {\n"
           "    code: {\n"
           "      bipush 100\n"
           "      bipush 20\n"
           "      invokestatic fixture/KpeProc.proc (II)I\n"
           "      istore 1\n"
           "      iload 1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

TEST(KeyProcEvalScan, FindsSandwichAndEvaluatesProc)
{
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc", kProcSource);
    addClass(container, "fixture/KpeCaller", sandwichCaller());

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const Site& site = scanned.sites[0];
    EXPECT_EQ(site.uri, "fixture/KpeCaller.class");
    EXPECT_EQ(site.targetOwner, "fixture/KpeProc");
    EXPECT_EQ(site.targetName, "proc");
    EXPECT_EQ(site.stateSlot, 1u);
    EXPECT_EQ(site.inputKey, 100);
    EXPECT_EQ(site.salt, 20);
    EXPECT_EQ(site.result, 237) << "跨方法求值应得 (100+20)*2-3";
    EXPECT_EQ(scanned.totalShape, 1u);
    EXPECT_TRUE(scanned.rejects.empty());
}

TEST(KeyProcEvalScan, RotateLeftMockEvaluates)
{
    // proc(x, y) = Integer.rotateLeft(x, y)：jsim 核心/num 包无 JDK 数学面，
    // 接管缺失时整站 EvalBadState（gate_processor_fold 实证同因）。
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc",
             ".super java/lang/Object\n"
             ".class public super fixture/KpeProc {\n"
             "  .method public static proc (II)I {\n"
             "    code: {\n"
             "      iload 0\n"
             "      iload 1\n"
             "      invokestatic java/lang/Integer.rotateLeft (II)I\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");
    addClass(container, "fixture/KpeCaller",
             ".super java/lang/Object\n"
             ".class public super fixture/KpeCaller {\n"
             "  .method public static m ()I {\n"
             "    code: {\n"
             "      bipush 8\n"
             "      istore 1\n"
             "      iload 1\n"
             "      iconst_3\n"
             "      invokestatic fixture/KpeProc.proc (II)I\n"
             "      istore 1\n"
             "      iload 1\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].result, 64) << "rotateLeft(8, 3) 应得 64";
    EXPECT_TRUE(scanned.rejects.empty());
}

TEST(KeyProcEvalScan, CrossClassClinitPresentRejected)
{
    // 目标类 clinit 非中性（clinit 读外类触发外部初始化，删调用移时机）：
    // 跨类调用拒绝（constant_call / gate_processor_fold 同源口径）。
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc",
             ".super java/lang/Object\n"
             ".class public super fixture/KpeProc {\n"
             "  .field public static counter I\n"
             "  .method static <clinit> ()V {\n"
             "    code: {\n"
             "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
             "      pop\n"
             "      iconst_1\n"
             "      putstatic fixture/KpeProc.counter I\n"
             "      return\n"
             "    }\n"
             "  }\n"
             "  .method public static proc (II)I {\n"
             "    code: {\n"
             "      iload 0\n"
             "      iload 1\n"
             "      iadd\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");
    addClass(container, "fixture/KpeCaller", sandwichCaller());

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    bool found = false;
    for (const Reject& rej : scanned.rejects) {
        if (rej.reason == RejectReason::ClassInitPresent) found = true;
    }
    EXPECT_TRUE(found) << "rejects 数=" << scanned.rejects.size();
}

TEST(KeyProcEvalScan, TlrProcBodyBailsClosed)
{
    // proc 体引用 ThreadLocalRandom（运行期随机面）：求值 Bail → EvalBadState
    // 桶，站点不成形，字节不动。
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc",
             ".super java/lang/Object\n"
             ".class public super fixture/KpeProc {\n"
             "  .method public static proc (II)I {\n"
             "    code: {\n"
             "      invokestatic java/util/concurrent/ThreadLocalRandom.current"
             " ()Ljava/util/concurrent/ThreadLocalRandom;\n"
             "      pop\n"
             "      iload 0\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");
    addClass(container, "fixture/KpeCaller", sandwichCaller());

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    bool found = false;
    for (const Reject& rej : scanned.rejects) {
        if (rej.reason == RejectReason::EvalBadState) found = true;
    }
    EXPECT_TRUE(found) << "rejects 数=" << scanned.rejects.size();
}

TEST(KeyProcEvalScan, NonSandwichCallNotACandidate)
{
    // 直线双参调用（javac 形）：desc 指纹同、夹心形不同——静默非候选，不进
    // 桶不计数（防止把普通业务调用误折成常量）。
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc", kProcSource);
    addClass(container, "fixture/KpeCaller", plainCaller());

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.totalShape, 0u);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_TRUE(scanned.rejects.empty());
}

TEST(KeyProcEvalRewrite, CollapsesSandwichToConstantLoad)
{
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc", kProcSource);
    addClass(container, "fixture/KpeCaller", sandwichCaller());
    const ca::usize before = countClasses(container);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    // 改写必须真的落盘：INVOKESTATIC 消失，锚改规范常量装载（237 → SIPUSH
    // ），尾 ISTORE 原样——桥退化为纯 span。proc 本体不碰（交下游死码回收）。
    const model::IResource* raw = container.getResource("fixture/KpeCaller.class");
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);
    auto session = openSession(cls->get());
    ASSERT_TRUE(session != nullptr);
    const jbc::ir::ClassUnit& unit = session->unit();
    ca::usize            mIndex = 0;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) == "m") mIndex = i;
    }
    auto promoted = jbc::ir::promoteMethod(*session, mIndex);
    ASSERT_TRUE(promoted.is_ok());
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();

    ca::usize            invokes   = 0;
    const jbc::ir::Insn* firstInsn = nullptr;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (firstInsn == nullptr) firstInsn = in;
        if (in->op == jbc::core::Op::INVOKESTATIC) ++invokes;
    }
    EXPECT_EQ(invokes, 0u) << "夹心调用未擦除";
    ASSERT_NE(firstInsn, nullptr);
    EXPECT_EQ(firstInsn->op, jbc::core::Op::SIPUSH) << "锚未改 237 的规范装载";
    ca::i32 folded = 0;
    ASSERT_TRUE(const_args::decodeConstInt(*session, *firstInsn, folded));
    EXPECT_EQ(folded, 237) << "折叠值必须等于 proc(100, 20)";

    // proof：类数不增 + 脏类可重解析 + residual 归零（再扫无站点）。
    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u) << "塌缩后同一检查不得再成站点";
}

TEST(KeyProcEvalRewrite, NoopOnCleanJar)
{
    model::ResourceContainer container;
    addClass(container, "fixture/KpeProc", kProcSource);

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.uris.size(), 0u);
}

}   // namespace
}   // namespace grunt::key_proc_eval
