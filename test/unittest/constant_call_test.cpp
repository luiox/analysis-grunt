/// @file        constant_call_test.cpp
/// @brief       残留 `constant_call` 的三段判据测试。
///
/// 夹具一律用 jasm 源码现场汇编（`JvmJasmService::assemble`），不落二进制：
/// 断言看的是这段代码该不该折、折完长什么样，源码形态比字节数组可读。
///
/// 覆盖分工：
/// - SCAN：折的条件逐条反证（非 static / void / synchronized / 实参非常量 /
///   目标不纯 / 目标不在容器 / 类初始化非空），加两条正例（短形态值、需 ldc 的值）；
/// - REWRITE：折完调用必须消失、字面量必须出现、实参装载必须全清；
/// - PROOF：类数不变。
#include "grunt/constant_call.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/jsim_executor.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/num.hpp"
#include "jsim/str.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::constant_call {
namespace {

namespace model = mjbase::model;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源码 → class 字节。汇编失败直接抛（夹具写错要立刻炸，不能静默空跑）。
std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        sourceName = utf16(name + ".jasm");
    auto                        text       = utf16(source);
    auto                        assembled  = service.assemble(sourceName.ref(), text.ref());
    if (!assembled.isOk()) throw std::runtime_error("jasm 夹具汇编失败：" + name);
    return std::move(assembled.value->bytes);
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

/// 容器装配：`{类名, jasm 源码}` 列表，uri 取 `<类名>.class`。
model::ResourceContainer makeContainer(
    std::initializer_list<std::pair<std::string, std::string>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, source] : classes) {
        const std::string uri = name + ".class";
        container.addResource(
            uri, std::make_unique<model::ClassResource>(uri, assemble(name, source)));
    }
    return container;
}

/// 只含一个静态方法 `pure` 的工具类：`pure(a, b) = (a + b) * 7`。
/// 纯算术 + 直线路径，正是求值子集能覆盖的形态。
std::string utilSource(const std::string& extraMethods = {})
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Util {\n"
           "  .method public static pure (II)I {\n"
           "    parameters: { a, b },\n"
           "    code: {\n"
           "      iload a\n"
           "      iload b\n"
           "      iadd\n"
           "      bipush 7\n"
           "      imul\n"
           "      ireturn\n"
           "    }\n"
           "  }\n" +
           extraMethods +
           "}\n";
}

/// 调用方：把常量喂给 `fixture/Util.pure`。
std::string callerSource(const std::string& instructions)
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Caller {\n"
           "  .method public static caller ()I {\n"
           "    code: {\n" +
           instructions +
           "    }\n"
           "  }\n"
           "}\n";
}

/// 标准双类夹具：Util + Caller（调用点用两条 bipush 喂常量）。
model::ResourceContainer standardFixture()
{
    return makeContainer({
        {"fixture/Util", utilSource()},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      bipush 4\n"
                      "      invokestatic fixture/Util.pure (II)I\n"
                      "      ireturn\n")},
    });
}

/// 子串出现次数。断言只剩一条 X必须用计数，`find("bipush 4")` 会被
/// `bipush 49` 命中，前缀匹配在反汇编文本上不可靠。
ca::usize countOf(const std::string& text, const std::string& needle)
{
    ca::usize n = 0;
    for (ca::usize at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

const RejectedSite* findReject(const ScanResult& result, RejectReason reason)
{
    for (const RejectedSite& r : result.rejects) {
        if (r.reason == reason) return &r;
    }
    return nullptr;
}

/// 取容器里某个类的当前字节（夹具缺类即抛，不静默返回空）。
const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(ConstantCallScan, FoldsPureCallWithConstantArgs)
{
    model::ResourceContainer container = standardFixture();
    const ScanResult         result    = scan(container);

    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.targetOwner, "fixture/Util");
    EXPECT_EQ(site.targetName, "pure");
    EXPECT_EQ(site.callerOwner, "fixture/Caller");
    ASSERT_EQ(site.args.size(), 2u);
    ASSERT_TRUE(site.value.known);
    ASSERT_EQ(site.value.type, jsim::ValType::I32);
    // (3 + 4) * 7
    EXPECT_EQ(static_cast<ca::i32>(site.value.intBits), 49);
}

TEST(ConstantCallScan, FoldsValueThatNeedsConstantPool)
{
    // 2 * 32767 = 65534：超出 sipush 域，折叠必须落 ldc。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public static wide (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      sipush 32767\n"
         "      imul\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 2\n"
                      "      invokestatic fixture/Util.wide (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(static_cast<ca::i32>(result.sites.front().value.intBits), 65534);
}

TEST(ConstantCallScan, EvaluatesTargetBodyThatLoadsPoolConstants)
{
    // 目标方法体自身带 ldc（值超 bipush/sipush 域）：求值器必须经会话常量池
    // 解析出 100000，才能算出 3 * 100000。这条钉的是**前置能力**，求值器
    // 不支持 ldc 的话，所有方法体里带池常量的目标都会被静默拒掉。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public static scaled (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      ldc 100000\n"
         "      imul\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic fixture/Util.scaled (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(static_cast<ca::i32>(result.sites.front().value.intBits), 300000);
}

TEST(ConstantCallScan, RejectsNonStaticTarget)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public pure (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic fixture/Util.pure (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::TargetNotStatic), nullptr);
}

TEST(ConstantCallScan, RejectsVoidTarget)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public static sink (I)V {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic fixture/Util.sink (I)V\n"
                      "      iconst_0\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::TargetVoid), nullptr);
}

TEST(ConstantCallScan, RejectsSynchronizedTarget)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public static synchronized lock (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic fixture/Util.lock (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::TargetSynchronized), nullptr);
}

TEST(ConstantCallScan, RejectsNonConstantArguments)
{
    // 实参来自局部变量：不是常量装载，整站点拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util", utilSource()},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      bipush 4\n"
         "      invokestatic fixture/Util.pure (II)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::ArgsNotConstant), nullptr);
}

TEST(ConstantCallScan, RejectsImpureTarget)
{
    // 目标读静态字段：执行器静态面没有真值，未知值向结果传播，结果不可折叠，
    // 必须拒绝而不是猜值。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .field public static counter I\n"
         "  .method public static read (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      getstatic fixture/Util.counter I\n"
         "      iload a\n"
         "      iadd\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic fixture/Util.read (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::ValueKindUnsupported), nullptr);
}

TEST(ConstantCallScan, IgnoresTargetsOutsideContainer)
{
    // 容器外的目标是常态（JDK 方法遍地都是），连候选都不该进，不进 sites，
    // 也不该刷进 rejects（否则拒绝面被无关调用淹没）。
    model::ResourceContainer container = makeContainer({
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic java/lang/Math.abs (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_TRUE(result.rejects.empty());
}

TEST(ConstantCallScan, AcceptsTargetWithPureConstClassInit)
{
    // watermark 面（scan_tools::classInitNeutral 纯常量初值口）：clinit 只由
    // 常量装载 + putstatic 本类字段构成时，初始化时机不可观察（字段值确定、
    // 有读者时读者本身就是主动使用），跨类调用照折。Grunt 每类撒
    // `ldc "PROTECTED BY …"; putstatic 自家字段`，这一面不放开则全部跨类
    // 站点空转（样本取证：junk 谓词助手类只有 watermark clinit）。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .field public static counter I\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      iconst_1\n"
         "      putstatic fixture/Util.counter I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "  .method public static pure (II)I {\n"
         "    parameters: { a, b },\n"
         "    code: {\n"
         "      iload a\n"
         "      iload b\n"
         "      iadd\n"
         "      bipush 7\n"
         "      imul\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      bipush 4\n"
                      "      invokestatic fixture/Util.pure (II)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_FALSE(result.sites.empty());
    EXPECT_EQ(findReject(result, RejectReason::ClassInitPresent), nullptr);
}

TEST(ConstantCallScan, RejectsTargetWhoseClassInitHasBody)
{
    // invokestatic 是类的主动使用：删掉调用会改变 <clinit> 的执行时机。
    // clinit 带纯常量初值面之外的实体指令（此处：对**外类**字段的读——
    // 触发外类初始化，时机可观察）时必须拒绝，**跨类**调用照旧。
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .field public static counter I\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
         "      pop\n"
         "      iconst_1\n"
         "      putstatic fixture/Util.counter I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "  .method public static pure (II)I {\n"
         "    parameters: { a, b },\n"
         "    code: {\n"
         "      iload a\n"
         "      iload b\n"
         "      iadd\n"
         "      bipush 7\n"
         "      imul\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      bipush 4\n"
                      "      invokestatic fixture/Util.pure (II)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::ClassInitPresent), nullptr);
}

/// proc 形夹具（boar 转发的最小闭环）：类带非中性 clinit，本类 helper 体是
/// `[iload a; iload b; invokestatic java/lang/Math.max (II)I; ireturn]`，
/// 本类 caller 喂双常量，同类检查豁免 + Math.max 接管两条扩面各钉一半。
std::string procSource(const std::string& helperBody)
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Proc {\n"
           "  .field public static counter I\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      putstatic fixture/Proc.counter I\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "  .method public static proc (II)I {\n"
           "    parameters: { a, b },\n"
           "    code: {\n" +
           helperBody +
           "    }\n"
           "  }\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 9\n"
           "      invokestatic fixture/Proc.proc (II)I\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

TEST(ConstantCallScan, KeepsSameClassCallWhenClassInitHasBody)
{
    // 同类调用不触发初始化（JVMS 5.5：本类方法执行时类已在初始化中/已完成），
    // 删调用不移时机，非中性 clinit 只豁免同类站点，跨类照旧拒绝。
    // 这是 processor 键解锁链的检查面：boar 全部可折转发站点都在非中性类里。
    model::ResourceContainer container = makeContainer({
        {"fixture/Proc", procSource("      iload a\n"
                                    "      iload b\n"
                                    "      iadd\n"
                                    "      bipush 7\n"
                                    "      imul\n"
                                    "      ireturn\n")},
        {"fixture/Util", utilSource()},
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      bipush 4\n"
                      "      invokestatic fixture/Util.pure (II)I\n"
                      "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    // 同类站点在 sites（Util 目标中性，跨类站点本来就在）；Proc 目标非中性
    // clinit，同类豁免是它还在的唯一理由。
    ASSERT_EQ(result.sites.size(), 2u);
    const Site* procSite = nullptr;
    for (const Site& s : result.sites) {
        if (s.targetOwner == "fixture/Proc") procSite = &s;
    }
    ASSERT_NE(procSite, nullptr);
    EXPECT_EQ(procSite->callerOwner, "fixture/Proc");
    EXPECT_EQ(static_cast<ca::i32>(procSite->value.intBits), 84);   // (3+9)*7
}

TEST(ConstantCallScan, FoldsMaxBackedHelper)
{
    // 转发体还原成 Math.max 调用（IndyRestore 后的 boar 形态）：求值器必须有
    // Math.max 接管才算得出；否则整面在 EvalBadState 拒。
    model::ResourceContainer container = makeContainer({
        {"fixture/Proc", procSource("      iload a\n"
                                    "      iload b\n"
                                    "      invokestatic java/lang/Math.max (II)I\n"
                                    "      ireturn\n")},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    ASSERT_TRUE(result.sites.front().value.known);
    EXPECT_EQ(static_cast<ca::i32>(result.sites.front().value.intBits), 9);
}

TEST(ConstantCallRewrite, FoldsSameClassMaxHelperInPlace)
{
    // 同类 + 非中性 clinit + Math.max 转发：三条扩面合流的 boar 最小闭环，
    // 折完调用消失、字面量出现、脏类是本类。
    model::ResourceContainer container = makeContainer({
        {"fixture/Proc", procSource("      iload a\n"
                                    "      iload b\n"
                                    "      invokestatic java/lang/Math.max (II)I\n"
                                    "      ireturn\n")},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), std::string("fixture/Proc.class"));

    const std::string after = disassemble(classBytes(container, "fixture/Proc.class"));
    EXPECT_EQ(countOf(after, "invokestatic fixture/Proc.proc"), 0u) << after;
    EXPECT_NE(after.find("bipush 9"), std::string::npos) << after;
    // proc 转发本体与 clinit 不在本次改写面（折叠只动调用点区间）。
    EXPECT_NE(after.find("invokestatic java/lang/Math.max"), std::string::npos) << after;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(ConstantCallRewrite, ReplacesCallWithFoldedLiteral)
{
    model::ResourceContainer container = standardFixture();
    const ScanResult         scanned   = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    // 调用方被改写，工具类原样（不是站点所在类，不该进脏集）。
    EXPECT_EQ(dirty.uris.front(), std::string("fixture/Caller.class"));

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_NE(after.find("bipush 49"), std::string::npos) << after;
}

TEST(ConstantCallRewrite, ClearsEveryArgumentPush)
{
    model::ResourceContainer container = standardFixture();
    const ScanResult         scanned   = scan(container);
    const DirtySet           dirty     = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    // 折完方法体只剩装载字面量 + ireturn：原来那两条 bipush 必须全没了。
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_EQ(after.find("iadd"), std::string::npos) << after;
    EXPECT_EQ(countOf(after, "bipush"), 1u) << after;
    EXPECT_NE(after.find("bipush 49"), std::string::npos) << after;
}

TEST(ConstantCallRewrite, EmitsLdcForValueBeyondSipush)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Util",
         ".super java/lang/Object\n"
         ".class public super fixture/Util {\n"
         "  .method public static wide (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      sipush 32767\n"
         "      imul\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         callerSource("      bipush 2\n"
                      "      invokestatic fixture/Util.wide (I)I\n"
                      "      ireturn\n")},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_NE(after.find("ldc 65534"), std::string::npos) << after;
}

TEST(ConstantCallRewrite, LeavesContainerUntouchedWhenNoSites)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Caller",
         callerSource("      bipush 3\n"
                      "      invokestatic java/lang/Math.abs (I)I\n"
                      "      ireturn\n")},
    });
    const std::vector<ca::u8> before = classBytes(container, "fixture/Caller.class");

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);

    EXPECT_EQ(dirty.applied, 0u);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "fixture/Caller.class"), before);
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(ConstantCallProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = standardFixture();
    const ScanResult         scanned   = scan(container);
    const DirtySet           dirty     = rewrite(container, scanned);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
}

TEST(ConstantCallProof, FailsWhenClassCountChanges)
{
    model::ResourceContainer container = standardFixture();
    const ScanResult         scanned   = scan(container);
    const DirtySet           dirty     = rewrite(container, scanned);

    // 传一个与现状不符的基线：类数检查必须拒。
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_FALSE(verdict.ok);
    ASSERT_FALSE(verdict.failed.empty());
    EXPECT_EQ(verdict.failed.front(), std::string("class-count"));
}

// ============================================================================
// ==== 解密器族 ==============================================================
// ============================================================================

// 合成解密器夹具：直线循环解密器（真身是 junk 状态机，形态同源：char[] 逐
// 元异或 + new String(char[]) 收尾）+ 同类池填充点（ldc 密文 · toCharArray ·
// ldc2_w key · bipush seed · invokestatic）。密文 "/$6" ^ 65 = "new"（独立
// 重算，不借实现常量）。
const char* kDecHost =
    ".super java/lang/Object\n"
    ".class public super fixture/DecHost {\n"
    "  .method private static a ([CJI)Ljava/lang/String; {\n"
    "    parameters: { arr, key, seed },\n"
    "    code: {\n"
    "      iconst_0\n"
    "      istore 4\n"
    "    Lloop:\n"
    "      iload 4\n"
    "      aload arr\n"
    "      arraylength\n"
    "      if_icmpge Ldone\n"
    "      aload arr\n"
    "      iload 4\n"
    "      aload arr\n"
    "      iload 4\n"
    "      caload\n"
    "      iload seed\n"
    "      ixor\n"
    "      i2c\n"
    "      castore\n"
    "      iinc 4 1\n"
    "      goto Lloop\n"
    "    Ldone:\n"
    "      new java/lang/String\n"
    "      dup\n"
    "      aload arr\n"
    "      invokespecial java/lang/String.<init> ([C)V\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "  .method public static fill ()Ljava/lang/String; {\n"
    "    code: {\n"
    "      ldc \"/$6\"\n"
    "      invokevirtual java/lang/String.toCharArray ()[C\n"
    "      ldc2_w 999L\n"
    "      bipush 65\n"
    "      invokestatic fixture/DecHost.a ([CJI)Ljava/lang/String;\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 跨类填充点：解密器与调用不同类——JVMS 5.5 豁免不适用（删调用可能改变
/// 被调类初始化时机），形态不命中即回落通用判据拒。
const char* kDecCrossCaller =
    ".super java/lang/Object\n"
    ".class public super fixture/DecCrossCaller {\n"
    "  .method public static fill ()Ljava/lang/String; {\n"
    "    code: {\n"
    "      ldc \"/$6\"\n"
    "      invokevirtual java/lang/String.toCharArray ()[C\n"
    "      ldc2_w 999L\n"
    "      bipush 65\n"
    "      invokestatic fixture/DecHost.a ([CJI)Ljava/lang/String;\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "}\n";

std::string decryptExpectPlain()
{
    // "/$6" 每个码元 ^ 65（seed）→ "new"。夹具密文设计成结果可见字符。
    std::string out;
    for (const char c : std::string("/$6")) out.push_back(static_cast<char>(c ^ 65));
    return out;
}

TEST(ConstantCallDecryptor, ScanMatchesFillShapeAndEvaluatesPlain)
{
    model::ResourceContainer container = makeContainer({{"fixture/DecHost", kDecHost}});
    const ScanResult         result    = scan(container);

    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_TRUE(site.isDecryptor);
    EXPECT_EQ(site.targetName, "a");
    EXPECT_EQ(site.key, 999);
    EXPECT_EQ(site.seed, 65);
    const std::vector<ca::u16> cipherUnits = {0x2F, 0x24, 0x36};
    EXPECT_EQ(site.cipherUnits, cipherUnits);
    // 求值明文 = 密文逐元 ^ seed（夹具解密器体逐元 ixor seed）。
    const std::vector<ca::u16> plainUnits = {'n', 'e', 'w'};
    ASSERT_EQ(site.plainUnits.size(), plainUnits.size());
    EXPECT_EQ(site.plainUnits, plainUnits);
    EXPECT_EQ(site.plaintext, decryptExpectPlain());
}

TEST(ConstantCallDecryptor, RewriteFoldsCallToPlaintextLdc)
{
    model::ResourceContainer container = makeContainer({{"fixture/DecHost", kDecHost}});
    const ScanResult         scanned   = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), "fixture/DecHost.class");

    const std::string after = disassemble(classBytes(container, "fixture/DecHost.class"));
    // 填充点只剩明文装载；解密器体仍在（zero-referenced 死成员由 DeadMember
    // 链上收尾，本残留只负责折叠调用点）。
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_EQ(after.find("toCharArray"), std::string::npos) << after;
    EXPECT_EQ(after.find("\"/$6\""), std::string::npos) << after;
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 语义闭环：折叠后的 fill 用 jsim 跑一遍，返回值 = 期望明文。
    grunt::SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::str::installStringAlloc(exec.session());
    jsim::num::install(exec.session());
    const SimOutcome out =
        exec.invoke("fixture/DecHost", "fill()Ljava/lang/String;", {});
    ASSERT_TRUE(out.ok()) << out.detail;
    ASSERT_EQ(out.value.type, jsim::ValType::Ref);
    ASSERT_EQ(out.value.ref.kind, jsim::RefPayload::Kind::Str);
    std::vector<ca::u16> folded;
    ASSERT_TRUE(scan_tools::mutf8ToUnits(out.value.ref.str, folded));
    std::string foldedText;
    ASSERT_TRUE(rewrite_tools::u16ToUtf8Record(folded, foldedText));
    EXPECT_EQ(foldedText, decryptExpectPlain());
}

TEST(ConstantCallDecryptor, RejectsCrossOwnerFillSite)
{
    model::ResourceContainer container =
        makeContainer({{"fixture/DecHost", kDecHost}, {"fixture/DecCrossCaller", kDecCrossCaller}});
    const ScanResult result = scan(container);

    // 同类的 DecHost.fill 仍是解密器站点；跨类调用者回落通用判据拒。
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().callerOwner, "fixture/DecHost");
    const RejectedSite* rejected = findReject(result, RejectReason::ArgsNotConstant);
    ASSERT_NE(rejected, nullptr) << "跨类填充点必须回落通用判据拒";
    EXPECT_EQ(rejected->callerOwner, "fixture/DecCrossCaller");
}

/// 空明文回归钉（u.class write-fail Unencodable 实证）：fill 先落 ldc2_w
/// （Long 条目 + placeholder 槽进池），fill2 的密文为空串——解密结果 "" 是
/// 合法明文。修复口径：findUtf8("") 命中的占位槽必须视作 miss，空 Utf8
/// 照常 intern，站点折成 ldc "" 且整类写盘成功。
const char* kDecHostEmptyPlain =
    ".super java/lang/Object\n"
    ".class public super fixture/DecHostEmpty {\n"
    "  .method private static a ([CJI)Ljava/lang/String; {\n"
    "    parameters: { arr, key, seed },\n"
    "    code: {\n"
    "      iconst_0\n"
    "      istore 4\n"
    "    Lloop:\n"
    "      iload 4\n"
    "      aload arr\n"
    "      arraylength\n"
    "      if_icmpge Ldone\n"
    "      aload arr\n"
    "      iload 4\n"
    "      aload arr\n"
    "      iload 4\n"
    "      caload\n"
    "      iload seed\n"
    "      ixor\n"
    "      i2c\n"
    "      castore\n"
    "      iinc 4 1\n"
    "      goto Lloop\n"
    "    Ldone:\n"
    "      new java/lang/String\n"
    "      dup\n"
    "      aload arr\n"
    "      invokespecial java/lang/String.<init> ([C)V\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "  .method public static fill ()Ljava/lang/String; {\n"
    "    code: {\n"
    "      ldc \"/$6\"\n"
    "      invokevirtual java/lang/String.toCharArray ()[C\n"
    "      ldc2_w 999L\n"
    "      bipush 65\n"
    "      invokestatic fixture/DecHostEmpty.a ([CJI)Ljava/lang/String;\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "  .method public static fill2 ()Ljava/lang/String; {\n"
    "    code: {\n"
    "      ldc \"\"\n"
    "      invokevirtual java/lang/String.toCharArray ()[C\n"
    "      ldc2_w 12345L\n"
    "      bipush 65\n"
    "      invokestatic fixture/DecHostEmpty.a ([CJI)Ljava/lang/String;\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "}\n";

TEST(ConstantCallDecryptor, FoldsEmptyPlaintextSitePastPlaceholderTrap)
{
    model::ResourceContainer container =
        makeContainer({{"fixture/DecHostEmpty", kDecHostEmptyPlain}});
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const Site* emptySite = nullptr;
    for (const Site& s : scanned.sites) {
        if (s.plainUnits.empty()) emptySite = &s;
    }
    ASSERT_NE(emptySite, nullptr) << "空明文站点必须被求值出空串";

    const DirtySet dirty = rewrite(container, scanned);
    // 两站都折（空明文 intern 不踩占位槽）；整类写盘成功才进 uris。
    ASSERT_EQ(dirty.applied, 2u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    // 池面闭环：重开写盘后的类，String 池里必须存在真空 Utf8（索引有效）。
    const std::string after = disassemble(classBytes(container, "fixture/DecHostEmpty.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 语义闭环：折叠后的 fill2 返回空串。
    grunt::SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::str::installStringAlloc(exec.session());
    jsim::num::install(exec.session());
    const SimOutcome out =
        exec.invoke("fixture/DecHostEmpty", "fill2()Ljava/lang/String;", {});
    ASSERT_TRUE(out.ok()) << out.detail;
    ASSERT_EQ(out.value.type, jsim::ValType::Ref);
    ASSERT_EQ(out.value.ref.kind, jsim::RefPayload::Kind::Str);
    std::vector<ca::u16> folded;
    ASSERT_TRUE(scan_tools::mutf8ToUnits(out.value.ref.str, folded));
    EXPECT_TRUE(folded.empty()) << "折叠后 fill2 必须返回空串";
}

/// 容器外内建站点夹具（u/b 族池读点实证）：parseUL 键面是池读下标的构造
/// 件，不折它 NF 铺不开后续算术、PAF 终态表查不到下标。白名单唯一成员
/// Long.parseUnsignedLong——全序纯函数，常量实参下值唯一。
const char* kParseUlHost =
    ".super java/lang/Object\n"
    ".class public super fixture/ParseUlHost {\n"
    "  .method public static grab ()J {\n"
    "    code: {\n"
    "      ldc \"50do1e7vn2una\"\n"
    "      bipush 32\n"
    "      invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

ca::i64 parseUlExpect()
{
    // 独立重算（u64 累进，同 JDK 无符号语义），不借实现常量。
    ca::u64 acc = 0;
    for (const char c : std::string("50do1e7vn2una")) {
        const ca::u32 d = (c >= '0' && c <= '9') ? static_cast<ca::u32>(c - '0')
                                                 : static_cast<ca::u32>(c - 'a') + 10;
        acc = acc * 32 + static_cast<ca::u64>(d);
    }
    return static_cast<ca::i64>(acc);
}

TEST(ConstantCallExternalBuiltin, ScansAndEvaluatesParseUlSite)
{
    model::ResourceContainer container = makeContainer({{"fixture/ParseUlHost", kParseUlHost}});
    const ScanResult         result    = scan(container);

    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_TRUE(site.isExternalBuiltin);
    EXPECT_EQ(site.targetOwner, "java/lang/Long");
    EXPECT_EQ(site.targetName, "parseUnsignedLong");
    ASSERT_EQ(site.args.size(), 2u);
    ASSERT_TRUE(site.value.known);
    ASSERT_EQ(site.value.type, jsim::ValType::I64);
    EXPECT_EQ(site.value.intBits, parseUlExpect());
}

TEST(ConstantCallExternalBuiltin, RewriteFoldsParseUlToLdc2W)
{
    model::ResourceContainer container = makeContainer({{"fixture/ParseUlHost", kParseUlHost}});
    const ScanResult         scanned   = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string after = disassemble(classBytes(container, "fixture/ParseUlHost.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_NE(after.find("ldc2_w"), std::string::npos) << after;
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 语义闭环：折叠后的 grab 用 jsim 复跑，返回值 = 独立重算期望。
    grunt::SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::num::install(exec.session());
    const SimOutcome out = exec.invoke("fixture/ParseUlHost", "grab()J", {});
    ASSERT_TRUE(out.ok()) << out.detail;
    ASSERT_EQ(out.value.type, jsim::ValType::I64);
    EXPECT_EQ(out.value.intBits, parseUlExpect());
}

}   // namespace
}   // namespace grunt::constant_call
