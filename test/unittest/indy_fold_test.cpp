/// @file        indy_fold_test.cpp
/// @brief       残留 `indy_fold` 的三段判据测试。
///
/// 夹具是「缩小版 Grunt indy 形态」：BSM 不读前三个 JVM 合成形参（Lookup/name/
/// MethodType——折叠灌的是 Unknown），目标信息全部来自静态参数表；体感与真
/// 混淆器一致：forName → findStatic → ConstantCallSite。
///
/// 覆盖分工：
/// - SCAN：BSM 收敛 → 站点 foldable、目标四元组正确；BSM 不收敛 → bsmFailed；
/// - REWRITE：indy 就地变 invokestatic，目标引用正确；
/// - PROOF：类数不变 + 脏类可重解析。
#include "grunt/indy_fold.hpp"

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

namespace grunt::indy_fold {
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
    if (!assembled.isOk()) {
        std::string detail;
        for (const auto& d : assembled.diagnostics) {
            detail += "\n  " + d.message.to_utf8_string().to_std_string();
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail);
    }
    return std::move(assembled.value->bytes);
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
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

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

/// BSM 前三个形参是 JVM 合成的（Lookup/name/MethodType），折叠灌 Unknown——
/// 所以夹具 BSM 只从静态参数取信息：静态参数 = 目标名 / 目标类名 / 目标描述符。
/// 目标类经 Class.forName 造 Class 对象，findStatic 出句柄，包 ConstantCallSite。
std::string hostSource(const std::string& bsmBody)
{
    return ".super java/lang/Object\n"
           ".class public super IndyHost {\n"
           "  .method public static t (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;"
           "Ljava/lang/String;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
           ")Ljava/lang/invoke/CallSite; {\n"
           "    parameters: { lk, nm, mt, targetName, targetCls, targetMt },\n"
           "    code: {\n" +
           bsmBody +
           "    }\n"
           "  }\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 4\n"
           "      invokedynamic t (II)I { invokestatic, IndyHost.a, "
           "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
           "{ \"t\", \"IndyHost\", (II)I }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string foldableBsmBody()
{
    return "      new java/lang/invoke/ConstantCallSite\n"
           "      dup\n"
           "      aload lk\n"
           "      aload targetCls\n"
           "      invokestatic java/lang/Class.forName (Ljava/lang/String;)Ljava/lang/Class;\n"
           "      aload targetName\n"
           "      aload targetMt\n"
           "      invokestatic java/lang/invoke/MethodHandles$Lookup.findStatic "
           "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
           "Ljava/lang/invoke/MethodHandle;\n"
           "      invokespecial java/lang/invoke/ConstantCallSite.<init> "
           "(Ljava/lang/invoke/MethodHandle;)V\n"
           "      areturn\n";
}

std::string divergentBsmBody()
{
    // BSM 返回 null：收敛但不是 CallSite——另一类拒绝面（载荷不完整）。
    return "      aconst_null\n"
           "      areturn\n";
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(IndyFoldScan, ResolvesTargetByRunningBsm)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSource(foldableBsmBody())}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    ASSERT_EQ(result.foldable, 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.bsmOwner, "IndyHost");
    EXPECT_EQ(site.bsmName, "a");
    EXPECT_EQ(site.indyDesc, "(II)I");
    EXPECT_TRUE(site.foldable);
    EXPECT_EQ(site.target.kind, jsim::indy::Target::Kind::Static);
    EXPECT_EQ(site.target.owner, "IndyHost");
    EXPECT_EQ(site.target.name, "t");
    EXPECT_EQ(site.target.desc, "(II)I");
}

TEST(IndyFoldScan, RejectsSiteWhenBsmDoesNotConverge)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSource(divergentBsmBody())}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.foldable, 0u);
    EXPECT_EQ(result.bsmFailed, 1u);
    const Site& site = result.sites.front();
    EXPECT_FALSE(site.foldable);
    EXPECT_FALSE(site.why.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(IndyFoldRewrite, SwapsInvokedynamicForInvokestatic)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSource(foldableBsmBody())}});
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.foldable, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), std::string("IndyHost.class"));

    const std::string after = disassemble(classBytes(container, "IndyHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokestatic IndyHost.t (II)I"), std::string::npos) << after;
}

TEST(IndyFoldRewrite, LeavesContainerUntouchedWhenNothingFolds)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSource(divergentBsmBody())}});
    const std::vector<ca::u8> before = classBytes(container, "IndyHost.class");

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 0u);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "IndyHost.class"), before);
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(IndyFoldProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSource(foldableBsmBody())}});
    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
}

}   // namespace
}   // namespace grunt::indy_fold
