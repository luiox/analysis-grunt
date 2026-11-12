/// @file        const_method_test.cpp
/// @brief       残留 `const_method` 的三段判据测试。
///
/// 夹具一律 jasm 现场汇编，覆盖分工：
/// - SCAN：TYPE 取值器 / int / null 三个正例；非 TYPE getstatic、所有者
///   clinit 不纯、类别错配三条拒绝面；非常量生产的静默跳过；
/// - REWRITE：调用点替换成对应常量装载，目标类不进脏集；
/// - PROOF：类数不增。
#include "grunt/const_method.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::const_method {
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

/// TYPE 取值器工具类（形态与样本里的 `t()` 一致）。
const char* kTypeUtilSource =
    ".super java/lang/Object\n"
    ".class public super fixture/TypeUtil {\n"
    "  .method public static t ()Ljava/lang/Class; {\n"
    "    code: {\n"
    "      getstatic java/lang/Long.TYPE Ljava/lang/Class;\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "}\n";

const RejectedMethod* findReject(const ScanResult& result, RejectReason reason)
{
    for (const RejectedMethod& r : result.rejects) {
        if (r.reason == reason) return &r;
    }
    return nullptr;
}

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(ConstMethodScan, InlinesPrimitiveTypeGetter)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil", kTypeUtilSource},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.targetOwner, "fixture/TypeUtil");
    EXPECT_EQ(site.callerOwner, "fixture/Caller");
    ASSERT_EQ(site.value.kind, ConstValue::Kind::PrimClass);
    EXPECT_EQ(site.value.prim, 'J');
}

TEST(ConstMethodScan, InlinesIntAndNullBodies)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Consts",
         ".super java/lang/Object\n"
         ".class public super fixture/Consts {\n"
         "  .method public static answer ()I {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "  .method public static nothing ()Ljava/lang/Object; {\n"
         "    code: {\n"
         "      aconst_null\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()I {\n"
         "    code: {\n"
         "      invokestatic fixture/Consts.answer ()I\n"
         "      invokestatic fixture/Consts.nothing ()Ljava/lang/Object;\n"
         "      pop\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 2u);
    const Site* i32Site  = nullptr;
    const Site* nullSite = nullptr;
    for (const Site& s : result.sites) {
        if (s.value.kind == ConstValue::Kind::I32) i32Site = &s;
        if (s.value.kind == ConstValue::Kind::Null) nullSite = &s;
    }
    ASSERT_NE(i32Site, nullptr);
    EXPECT_EQ(i32Site->value.i32, 42);
    ASSERT_NE(nullSite, nullptr);
}

TEST(ConstMethodScan, RejectsNonTypeGetstatic)
{
    // Boolean.TRUE 是堆对象常量：没有可静态编码的等价形式。
    model::ResourceContainer container = makeContainer({
        {"fixture/Flags",
         ".super java/lang/Object\n"
         ".class public super fixture/Flags {\n"
         "  .method public static truth ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      getstatic java/lang/Boolean.TYPE Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "  .method public static box ()Ljava/lang/Object; {\n"
         "    code: {\n"
         "      getstatic java/lang/Boolean.TRUE Ljava/lang/Boolean;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Object; {\n"
         "    code: {\n"
         "      invokestatic fixture/Flags.box ()Ljava/lang/Object;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    // TYPE 取值器本身没有调用点（Caller 只调 box），定义在 defs 里但不产站点。
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::GetstaticUnsupported), nullptr);
}

TEST(ConstMethodScan, RejectsImpureOwnerClinit)
{
    // 所有者 <clinit> 含 athrow（白名单外）：内联会推迟初始化，整方法拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil",
         ".super java/lang/Object\n"
         ".class public super fixture/TypeUtil {\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      aconst_null\n"
         "      athrow\n"
         "    }\n"
         "  }\n"
         "  .method public static t ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      getstatic java/lang/Long.TYPE Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::OwnerClassInitImpure), nullptr);
}

TEST(ConstMethodScan, AcceptsPureSelfWritingOwnerClinit)
{
    // 所有者 <clinit> 只做常量算术 + 写本类字段：白名单内，允许内联。
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil",
         ".super java/lang/Object\n"
         ".class public super fixture/TypeUtil {\n"
         "  .field public static seed I\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 7\n"
         "      bipush 5\n"
         "      imul\n"
         "      putstatic fixture/TypeUtil.seed I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "  .method public static t ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      getstatic java/lang/Long.TYPE Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_TRUE(result.rejects.empty());
}

TEST(ConstMethodScan, SilentlySkipsNonConstantProducers)
{
    // iload 起头的普通方法不构成形状：不进站点也不刷拒绝面。
    model::ResourceContainer container = makeContainer({
        {"fixture/Plain",
         ".super java/lang/Object\n"
         ".class public super fixture/Plain {\n"
         "  .method public static echo (I)I {\n"
         "    parameters: { a },\n"
         "    code: {\n"
         "      iload a\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_TRUE(result.rejects.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(ConstMethodRewrite, ReplacesCallWithLdcClass)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil", kTypeUtilSource},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), std::string("fixture/Caller.class"));

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_NE(after.find("ldc"), std::string::npos) << after;
    EXPECT_NE(after.find("areturn"), std::string::npos) << after;
}

TEST(ConstMethodRewrite, ReplacesCallWithIntLiteral)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Consts",
         ".super java/lang/Object\n"
         ".class public super fixture/Consts {\n"
         "  .method public static answer ()I {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()I {\n"
         "    code: {\n"
         "      invokestatic fixture/Consts.answer ()I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(after.find("invokestatic"), std::string::npos) << after;
    EXPECT_NE(after.find("bipush 42"), std::string::npos) << after;
}

/// rewrite 预算超限：站点复核指令数超限的站点保守放弃，产物保持原样。
TEST(ConstMethodRewrite, OverBudgetLeavesCallUntouched)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil", kTypeUtilSource},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    // 复核计数单位是方法指令数（调用方法 2 条）；预算 1 必在首站点即超限。
    const DirtySet dirty = rewrite(container, scanned, iter_budget::ClassBudget{1});
    EXPECT_EQ(dirty.applied, 0u) << "超限类不许被改写";
    EXPECT_EQ(dirty.skippedByBudget, 1u);

    const std::string after = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_NE(after.find("invokestatic"), std::string::npos) << after;
}

/// scan 预算超限：位置数超限的类保守跳过（计数进 ScanResult）。
TEST(ConstMethodScan, OverBudgetSkipsClassConservatively)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil", kTypeUtilSource},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    // 预算 1：两个类各扫 1 个位置即超限（首位置上的站点被保留），其后不再扫，
    // 两类都记预算跳过（预算语义：超限前的产出保留，超限后保守放弃）。
    const ScanResult starved = scan(container, iter_budget::ClassBudget{1});
    EXPECT_EQ(starved.sites.size(), 1u);
    EXPECT_EQ(starved.classesSkippedByBudget, 2u);

    // 默认预算：正常夹具绝不触限。
    const ScanResult normal = scan(container);
    EXPECT_EQ(normal.sites.size(), 1u);
    EXPECT_EQ(normal.classesSkippedByBudget, 0u);
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(ConstMethodProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/TypeUtil", kTypeUtilSource},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static caller ()Ljava/lang/Class; {\n"
         "    code: {\n"
         "      invokestatic fixture/TypeUtil.t ()Ljava/lang/Class;\n"
         "      areturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

}   // namespace
}   // namespace grunt::const_method
