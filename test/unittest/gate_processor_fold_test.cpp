/// @file        gate_processor_fold_test.cpp
/// @brief       残留 `gate_processor_fold` 的三段判据测试：gate 形状钉、跨方法
///              求值（含 rotateLeft 接管与转发器递归）、恒真裁决检查与 TAKEN 形
///              折叠，逐桶反证。
#include "grunt/gate_processor_fold.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace grunt::gate_processor_fold {
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

const Reject* findReject(const ScanResult& result, RejectReason reason)
{
    for (const Reject& rej : result.rejects) {
        if (rej.reason == reason) return &rej;
    }
    return nullptr;
}

/// 失败诊断面：拒绝桶逐条列出（RejectReason 序号见枚举声明）。
std::string rejectReasons(const ScanResult& result)
{
    std::string out;
    for (const Reject& rej : result.rejects) {
        out += std::to_string(static_cast<int>(rej.reason));
        out += " ";
    }
    return out;
}

/// gate 方法体：三常量喂 proc、compareValue 比较、真臂 Lreal / junk 臂
/// fallthrough（生成面布局）。
std::string gateBody(const std::string& procRef, const std::string& argInsns,
                     const std::string& compareInsn)
{
    return "    code: {\n" + argInsns +
           "      invokestatic " + procRef + " (III)I\n"
           "      " + compareInsn + "\n"
           "      if_icmpeq Lreal\n"
           "      iconst_0\n"
           "      ireturn\n"
           "    Lreal:\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n";
}

std::string gateClass(const std::string& name, const std::string& procBody,
                      const std::string& argInsns, const std::string& compareInsn,
                      const std::string& procRef)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .method public static proc (III)I {\n"
           "    code: {\n" + procBody +
           "    }\n"
           "  }\n"
           "  .method public static m ()I {\n" +
           gateBody(procRef, argInsns, compareInsn) +
           "  }\n"
           "}\n";
}

/// 纯算术 proc 体：slot0 ^ slot1 + slot2。
const char* kXorAddProc =
    "      iload_0\n"
    "      iload_1\n"
    "      ixor\n"
    "      iload_2\n"
    "      iadd\n"
    "      ireturn\n";

/// proc(10,20,30) = (10^20)+30 = 60 == compareValue 60 → 恒真。
TEST(GateProcessorFoldScan, FindsGateSiteAndEvaluates)
{
    model::ResourceContainer container;
    addClass(container, "fixture/Gate",
             gateClass("fixture/Gate", kXorAddProc,
                       "      bipush 10\n      bipush 20\n      bipush 30\n",
                       "bipush 60", "fixture/Gate.proc"));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const Site& site = scanned.sites[0];
    EXPECT_EQ(site.uri, "fixture/Gate.class");
    EXPECT_EQ(site.targetOwner, "fixture/Gate");
    EXPECT_EQ(site.targetName, "proc");
    EXPECT_EQ(site.targetDesc, "(III)I");
    EXPECT_EQ(site.branchOp, jbc::core::Op::IF_ICMPEQ);
    ASSERT_EQ(site.args.size(), 3u);
    EXPECT_EQ(static_cast<ca::i32>(site.args[0].intBits), 10);
    EXPECT_EQ(static_cast<ca::i32>(site.args[1].intBits), 20);
    EXPECT_EQ(static_cast<ca::i32>(site.args[2].intBits), 30);
    EXPECT_EQ(site.compareValue, 60);
    EXPECT_EQ(site.result, 60) << "跨方法求值应得 (10^20)+30";
}

TEST(GateProcessorFoldRewrite, FoldsToAlwaysTrueForm)
{
    model::ResourceContainer container;
    addClass(container, "fixture/Gate",
             gateClass("fixture/Gate", kXorAddProc,
                       "      bipush 10\n      bipush 20\n      bipush 30\n",
                       "bipush 60", "fixture/Gate.proc"));
    const ca::usize before = countClasses(container);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource("fixture/Gate.class");
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    // 改写必须真的落盘：INVOKESTATIC 与 IF_ICMPEQ 消失，锚变 ICONST_0，恒真
    // IFEQ 直指原真臂 Lreal（iconst_1）；proc 本体不碰（交下游死码回收）。
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
    ca::usize            icmps     = 0;
    ca::usize            ifeqs     = 0;
    const jbc::ir::Insn* firstInsn = nullptr;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (firstInsn == nullptr) firstInsn = in;
        if (in->op == jbc::core::Op::INVOKESTATIC) ++invokes;
        if (in->op == jbc::core::Op::IF_ICMPEQ) ++icmps;
        if (in->op == jbc::core::Op::IFEQ) ++ifeqs;
    }
    EXPECT_EQ(invokes, 0u) << "gate 调用未擦除";
    EXPECT_EQ(icmps, 0u) << "gate 比较未折叠";
    EXPECT_EQ(ifeqs, 1u) << "TAKEN 未落恒真 IFEQ";
    ASSERT_NE(firstInsn, nullptr);
    EXPECT_EQ(firstInsn->op, jbc::core::Op::ICONST_0) << "锚未改 ICONST_0";

    // IFEQ 目标 = 原真臂头（iconst_1）。
    const jbc::ir::Insn* ifeq = nullptr;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in != nullptr && in->op == jbc::core::Op::IFEQ) ifeq = in;
    }
    ASSERT_NE(ifeq, nullptr);
    auto tgt = cu->dom.resolveLabel(ifeq->target);
    ASSERT_TRUE(tgt.is_ok());
    const jbc::ir::Insn* tgtInsn = cu->dom.peek(tgt.unwrap().insn);
    ASSERT_NE(tgtInsn, nullptr);
    EXPECT_EQ(tgtInsn->op, jbc::core::Op::ICONST_1) << "IFEQ 未指向原真臂";

    // proof：类数不增 + 脏类可重解析 + residual 归零（再扫无站点）。
    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u) << "折叠后同一检查不得再成站点";
}

/// proc 体走 Integer.rotateLeft 盐旋转步（PredicateStep AddSaltRotate 生成
/// 面）：rotateLeft(3,1)=6，6+2=8==compareValue。mock 语义走样（如错成
/// rotateRight）则判非恒真、不折叠，本测试同时是接管面的哨兵。
TEST(GateProcessorFoldRewrite, RotateLeftBodyFolds)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateRot",
             gateClass("fixture/GateRot",
                       "      iload_0\n"
                       "      iload_1\n"
                       "      invokestatic java/lang/Integer.rotateLeft (II)I\n"
                       "      iload_2\n"
                       "      iadd\n"
                       "      ireturn\n",
                       "      iconst_3\n      iconst_1\n      iconst_2\n",
                       "bipush 8", "fixture/GateRot.proc"));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u) << "rotateLeft 接管缺失则此桶 EvalBadState";
    EXPECT_EQ(scanned.sites[0].result, 8);
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);
}

/// 跨类 proc（H3 面）：gate 在 GateX、真体在 GateY（无 clinit，初始化检查放
/// 行），跨方法求值必须穿透类边界。
TEST(GateProcessorFoldRewrite, CrossClassProcFolds)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateY",
             ".super java/lang/Object\n"
             ".class public super fixture/GateY {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "}\n");
    addClass(container, "fixture/GateX",
             ".super java/lang/Object\n"
             ".class public super fixture/GateX {\n"
             "  .method public static m ()I {\n" +
             gateBody("fixture/GateY.proc",
                      "      bipush 10\n      bipush 20\n      bipush 30\n",
                      "bipush 60") +
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.uris.size(), 1u);
}

/// `__grt$indy$` 转发还原产物形（IndyRestore 上游）：gate 调转发器，转发器
/// 体 = 纯 iload 前缀 + 单 invoke + ireturn，求值必须经编排层递归穿透。
TEST(GateProcessorFoldRewrite, ForwarderRecursionFolds)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateF",
             ".super java/lang/Object\n"
             ".class public super fixture/GateF {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "  .method public static fwd (III)I {\n"
             "    code: {\n"
             "      iload_0\n"
             "      iload_1\n"
             "      iload_2\n"
             "      invokestatic fixture/GateF.proc (III)I\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "  .method public static m ()I {\n" +
             gateBody("fixture/GateF.fwd",
                      "      bipush 10\n      bipush 20\n      bipush 30\n",
                      "bipush 60") +
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].targetName, "fwd");
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.uris.size(), 1u);
}

/// TLR 面（RandomBound 变体，运行期随机，明确排除）：proc 体引用
/// ThreadLocalRandom，不在容器内、无 mock，求值 EvalBadState 整站拒绝。
TEST(GateProcessorFoldScan, TlrBodyFailsClosed)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateTlr",
             gateClass("fixture/GateTlr",
                       "      invokestatic java/util/concurrent/ThreadLocalRandom.current"
                       " ()Ljava/util/concurrent/ThreadLocalRandom;\n"
                       "      pop\n" + std::string(kXorAddProc),
                       "      bipush 10\n      bipush 20\n      bipush 30\n",
                       "bipush 60", "fixture/GateTlr.proc"));

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::EvalBadState), nullptr)
        << "运行期随机面必须 fail-closed（EvalBadState 桶）";
}

/// TLR 窗口变体：TLR.current 取数进前窗，停点具名识别，TlrWindow 桶单独
/// 计数（与走廊走样的 ForwardShape 区分，RandomBound 排除面在 trace 可见）。
TEST(GateProcessorFoldScan, TlrWindowRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateTlrW",
             ".super java/lang/Object\n"
             ".class public super fixture/GateTlrW {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "  .method public static m ()I {\n"
             "    code: {\n"
             "      bipush 10\n"
             "      bipush 20\n"
             "      bipush 30\n"
             "      invokestatic fixture/GateTlrW.proc (III)I\n"
             "      invokestatic java/util/concurrent/ThreadLocalRandom.current"
             " ()Ljava/util/concurrent/ThreadLocalRandom;\n"
             "      pop\n"
             "      bipush 60\n"
             "      if_icmpeq Lreal\n"
             "      iconst_0\n"
             "      ireturn\n"
             "    Lreal:\n"
             "      iconst_1\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::TlrWindow), nullptr);
}

/// 生成面恒真保证的防御面：proc(1,2,3) = (1^2)+3 = 6 != compareValue 7，
/// 判非 taken 即拒绝，绝不按非真方向改写。
TEST(GateProcessorFoldScan, NotAlwaysTrueRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateLie",
             gateClass("fixture/GateLie", kXorAddProc,
                       "      iconst_1\n      iconst_2\n      iconst_3\n",
                       "bipush 7", "fixture/GateLie.proc"));

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::EvalNotTrue), nullptr);
}

/// 实参面反证：salt 来自槽装载（非紧邻常量）→ ArgsNotConstant。
TEST(GateProcessorFoldScan, NonConstantArgsRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateArg",
             ".super java/lang/Object\n"
             ".class public super fixture/GateArg {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "  .method public static m (I)I {\n"
             "    code: {\n"
             "      bipush 10\n"
             "      bipush 20\n"
             "      iload_0\n"
             "      invokestatic fixture/GateArg.proc (III)I\n"
             "      bipush 60\n"
             "      if_icmpeq Lreal\n"
             "      iconst_0\n"
             "      ireturn\n"
             "    Lreal:\n"
             "      iconst_1\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::ArgsNotConstant), nullptr);
}

/// 前窗面反证：调用与比较之间夹了多余栈操作（aconst_null; pop），非生成面
/// 逐字形，ForwardShape 整站拒绝。
TEST(GateProcessorFoldScan, PollutedWindowRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateWin",
             ".super java/lang/Object\n"
             ".class public super fixture/GateWin {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "  .method public static m ()I {\n"
             "    code: {\n"
             "      bipush 10\n"
             "      bipush 20\n"
             "      bipush 30\n"
             "      invokestatic fixture/GateWin.proc (III)I\n"
             "      aconst_null\n"
             "      pop\n"
             "      bipush 60\n"
             "      if_icmpeq Lreal\n"
             "      iconst_0\n"
             "      ireturn\n"
             "    Lreal:\n"
             "      iconst_1\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::ForwardShape), nullptr);
}

/// 目标面反证：(III)I 调用目标不在容器 → TargetOutside。
TEST(GateProcessorFoldScan, OutsideTargetRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateOut",
             gateClass("fixture/GateOut", kXorAddProc,
                       "      bipush 10\n      bipush 20\n      bipush 30\n",
                       "bipush 60", "fixture/Missing.proc"));

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::TargetOutside), nullptr);
}

/// 初始化暴露检查（constant_call 同源）：目标类 clinit 非中性时跨类调用拒绝
/// （删调用会改变被调类初始化时机）。
TEST(GateProcessorFoldScan, CrossClassClinitPresentRejected)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateY",
             ".super java/lang/Object\n"
             ".class public super fixture/GateY {\n"
             "  .field public static counter I\n"
             "  .method static <clinit> ()V {\n"
             "    code: {\n"
             "      iconst_1\n"
             "      putstatic fixture/GateY.counter I\n"
             "      return\n"
             "    }\n"
             "  }\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "}\n");
    addClass(container, "fixture/GateX",
             ".super java/lang/Object\n"
             ".class public super fixture/GateX {\n"
             "  .method public static m ()I {\n" +
             gateBody("fixture/GateY.proc",
                      "      bipush 10\n      bipush 20\n      bipush 30\n",
                      "bipush 60") +
             "  }\n"
             "}\n");

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_NE(findReject(scanned, RejectReason::ClassInitPresent), nullptr)
        << "rejects: " << rejectReasons(scanned);
}

/// 标签检查：操作数区中段（salt 常量上）带真实合并点标签（有入边、栈形相容）
/// = 合并点，rewrite 整站放弃、字节不动（scan 不设此检查，站点照常入改写面，
/// 由复核段拦；未引用标签在 promote 后不驻 DOM，测不出该检查）。
TEST(GateProcessorFoldRewrite, MidRegionLabelBlocked)
{
    model::ResourceContainer container;
    addClass(container, "fixture/GateLbl",
             ".super java/lang/Object\n"
             ".class public super fixture/GateLbl {\n"
             "  .method public static proc (III)I {\n"
             "    code: {\n" + std::string(kXorAddProc) +
             "    }\n"
             "  }\n"
             "  .method public static m (I)I {\n"
             "    code: {\n"
             "      iload_0\n"
             "      ifeq Lelse\n"
             "      bipush 10\n"
             "      bipush 20\n"
             "      goto Lmid\n"
             "    Lelse:\n"
             "      bipush 10\n"
             "      bipush 20\n"
             "    Lmid:\n"
             "      bipush 30\n"
             "      invokestatic fixture/GateLbl.proc (III)I\n"
             "      bipush 60\n"
             "      if_icmpeq Lreal\n"
             "      iconst_0\n"
             "      ireturn\n"
             "    Lreal:\n"
             "      iconst_1\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n");
    const ca::usize before = countClasses(container);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty()) << "合并点站点必须整站放弃";
    EXPECT_EQ(dirty.failed, 1u);
    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok);
}

}   // namespace
}   // namespace grunt::gate_processor_fold
