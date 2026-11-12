/// @file        field_access_proxy_test.cpp
/// @brief       残留 `field_access_proxy` 的三段判据测试。
///
/// 夹具一律 jasm 现场汇编，覆盖分工：
/// - SCAN：getstatic / putstatic / getfield / putfield 四形状（含 long 宽槽
///   setter）、零引用静默四类正例；体形多指令、字段-描述符核对错配、
///   synchronized、外类调用点 + owner clinit 非中性四类拒绝面；
/// - REWRITE：调用点原地改回字段访问（op + 字段引用换、srcPc 保）、代理
///   本体不动；
/// - PROOF：类数不增 + 收敛哨兵（改写后再扫无站点）。
#include "grunt/field_access_proxy.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::field_access_proxy {
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
            if (d.severity != mjbase::jasm::DiagnosticSeverity::Error) continue;
            detail += d.message.to_utf8_string().to_std_string();
            if (d.location.has_value()) {
                detail += " @" + std::to_string(d.location->line);
            }
            break;
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + "：" + detail);
    }
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

/// 字段宿主：静态 int/long 各一 + 实例 int/long 各一（判据不认名字）。
const char* kDataSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Data {\n"
    "  .field public static sInt I\n"
    "  .field public static sLong J\n"
    "  .field public iInt I\n"
    "  .field public iLong J\n"
    "}\n";

/// 外置代理类（生成面 `C$FieldProxy` 形态，全 public static）。
const char* kProxiesSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Proxies {\n"
    "  .method public static get_sInt ()I {\n"
    "    code: {\n"
    "      getstatic fixture/Data.sInt I\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "  .method public static set_sInt (I)V {\n"
    "    code: {\n"
    "      iload_0\n"
    "      putstatic fixture/Data.sInt I\n"
    "      return\n"
    "    }\n"
    "  }\n"
    "  .method public static get_iInt (Lfixture/Data;)I {\n"
    "    code: {\n"
    "      aload_0\n"
    "      getfield fixture/Data.iInt I\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "  .method public static set_iLong (Lfixture/Data;J)V {\n"
    "    code: {\n"
    "      aload_0\n"
    "      lload_1\n"
    "      putfield fixture/Data.iLong J\n"
    "      return\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 调用者：四个代理各一个调用点（getstatic/putstatic/getfield/putfield 混排）。
const char* kCallerSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Caller {\n"
    "  .method public static run (Lfixture/Data;)I {\n"
    "    parameters: { data },\n"
    "    code: {\n"
    "      invokestatic fixture/Proxies.get_sInt ()I\n"
    "      iconst_1\n"
    "      invokestatic fixture/Proxies.set_sInt (I)V\n"
    "      aload data\n"
    "      invokestatic fixture/Proxies.get_iInt (Lfixture/Data;)I\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "  .method public static touch (Lfixture/Data;J)V {\n"
    "    parameters: { data, v },\n"
    "    code: {\n"
    "      aload data\n"
    "      lload v\n"
    "      invokestatic fixture/Proxies.set_iLong (Lfixture/Data;J)V\n"
    "      return\n"
    "    }\n"
    "  }\n"
    "}\n";

const RejectedProxy* findReject(const ScanResult& result, RejectReason reason)
{
    for (const RejectedProxy& r : result.rejects) {
        if (r.reason == reason) return &r;
    }
    return nullptr;
}

const Site* findSite(const ScanResult& result, const std::string& proxyName)
{
    for (const Site& s : result.sites) {
        if (s.proxyName == proxyName) return &s;
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

TEST(FieldAccessProxyScan, FindsAllFourShapes)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 4u);
    EXPECT_EQ(result.proxiesFound, 4u);
    EXPECT_TRUE(result.rejects.empty());

    const Site* getStatic = findSite(result, "get_sInt");
    ASSERT_NE(getStatic, nullptr);
    EXPECT_EQ(getStatic->kind, FieldKind::GetStatic);
    EXPECT_EQ(getStatic->fieldOwner, "fixture/Data");
    EXPECT_EQ(getStatic->fieldName, "sInt");
    EXPECT_EQ(getStatic->fieldDesc, "I");
    EXPECT_EQ(getStatic->callerOwner, "fixture/Caller");

    const Site* putStatic = findSite(result, "set_sInt");
    ASSERT_NE(putStatic, nullptr);
    EXPECT_EQ(putStatic->kind, FieldKind::PutStatic);
    EXPECT_EQ(putStatic->fieldDesc, "I");

    const Site* getField = findSite(result, "get_iInt");
    ASSERT_NE(getField, nullptr);
    EXPECT_EQ(getField->kind, FieldKind::GetField);
    EXPECT_EQ(getField->fieldOwner, "fixture/Data");
    EXPECT_EQ(getField->fieldDesc, "I");

    const Site* putField = findSite(result, "set_iLong");
    ASSERT_NE(putField, nullptr);
    EXPECT_EQ(putField->kind, FieldKind::PutField);
    EXPECT_EQ(putField->fieldDesc, "J");
}

TEST(FieldAccessProxyScan, SilentlySkipsZeroReferenceProxies)
{
    // 形状成立但零调用点：死码是 DeadMember 的业务，本残留不产站点不刷拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(result.proxiesFound, 4u);
}

TEST(FieldAccessProxyScan, RejectsBrokenBodyShape)
{
    // getter 体里多一条 iconst+pop：非逐槽过手 + 单次字段访问直线。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method public static get_sInt ()I {\n"
         "    code: {\n"
         "      iconst_0\n"
         "      pop\n"
         "      getstatic fixture/Data.sInt I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::BodyNotProxyShape), nullptr);
}

TEST(FieldAccessProxyScan, RejectsFieldDescMismatch)
{
    // 核对错配：描述符声称取 fixture/Data 的字段，体里转发的却是别的类，
    // 改回 getfield 会砸到描述符没承诺的类上，拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Other",
         ".super java/lang/Object\n"
         ".class public super fixture/Other {\n"
         "  .field public iInt I\n"
         "}\n"},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method public static get_iInt (Lfixture/Data;)I {\n"
         "    code: {\n"
         "      aload_0\n"
         "      getfield fixture/Other.iInt I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::FieldDescMismatch), nullptr);
}

TEST(FieldAccessProxyScan, RejectsSynchronizedProxy)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method public static synchronized get_sInt ()I {\n"
         "    code: {\n"
         "      getstatic fixture/Data.sInt I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::ProxySynchronized), nullptr);
}

TEST(FieldAccessProxyScan, RejectsExposedOwnerInit)
{
    // 调用点在 owner 类外，且 owner <clinit> 有副作用（athrow）：
    // 改回字段访问会丢掉对 owner 的主动使用，整代理放弃。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      aconst_null\n"
         "      athrow\n"
         "    }\n"
         "  }\n"
         "  .method public static get_sInt ()I {\n"
         "    code: {\n"
         "      getstatic fixture/Data.sInt I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::OwnerInitExposed), nullptr);
}

TEST(FieldAccessProxyScan, AllowsExternalCallerWithNeutralOwner)
{
    // 调用点在 owner 类外，但 owner 无 <clinit>（初始化无副作用）：
    // 检查放行，生成面的外置 `C$FieldProxy` 类就是这个形态。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 4u);
    EXPECT_TRUE(result.rejects.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(FieldAccessProxyRewrite, RewritesCallSitesInPlace)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 4u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.applied, 4u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), std::string("fixture/Caller.class"));
    EXPECT_EQ(dirty.proxies, 4u);

    // 调用点改回真实字段访问；代理本体保持原样（死码交下游 DeadMember/DeadCode）。
    const std::string caller = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(caller.find("Proxies.get_sInt"), std::string::npos) << caller;
    EXPECT_EQ(caller.find("Proxies.set_sInt"), std::string::npos) << caller;
    EXPECT_NE(caller.find("getstatic fixture/Data.sInt"), std::string::npos) << caller;
    EXPECT_NE(caller.find("putstatic fixture/Data.sInt"), std::string::npos) << caller;
    EXPECT_NE(caller.find("getfield fixture/Data.iInt"), std::string::npos) << caller;
    EXPECT_NE(caller.find("putfield fixture/Data.iLong"), std::string::npos) << caller;

    const std::string proxies = disassemble(classBytes(container, "fixture/Proxies.class"));
    EXPECT_NE(proxies.find("get_sInt"), std::string::npos) << proxies;
}

TEST(FieldAccessProxyRewrite, KeepsProxyBodyIntact)
{
    // 代理体里的字段访问不被动：改写只动调用点。
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_FALSE(dirty.uris.empty());

    const std::string proxies = disassemble(classBytes(container, "fixture/Proxies.class"));
    EXPECT_NE(proxies.find("getstatic fixture/Data.sInt"), std::string::npos) << proxies;
    EXPECT_NE(proxies.find("putstatic fixture/Data.sInt"), std::string::npos) << proxies;
    EXPECT_NE(proxies.find("getfield fixture/Data.iInt"), std::string::npos) << proxies;
    EXPECT_NE(proxies.find("putfield fixture/Data.iLong"), std::string::npos) << proxies;
    // 代理体内的 invokestatic 调用点应为零（本残留不产代理的代理）。
    EXPECT_EQ(proxies.find("invokestatic"), std::string::npos) << proxies;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(FieldAccessProxyProof, KeepsClassCountAndConverges)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Data", kDataSource},
        {"fixture/Proxies", kProxiesSource},
        {"fixture/Caller", kCallerSource},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_FALSE(dirty.uris.empty());

    const ProofResult verdict = proof(container, dirty, 3);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 收敛哨兵：改写后的容器再扫，站点归零（代理体里是字段访问，
    // 不构成新的代理调用点）。
    const ScanResult rescanned = scan(container);
    EXPECT_TRUE(rescanned.sites.empty()) << "改写一轮后不应残留可改站点";
}

}   // namespace
}   // namespace grunt::field_access_proxy
