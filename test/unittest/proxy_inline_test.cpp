/// @file        proxy_inline_test.cpp
/// @brief       残留 `proxy_inline` 的三段判据测试。
///
/// 夹具一律 jasm 现场汇编，覆盖分工：
/// - SCAN：static / virtual 目标、long+double 槽走、零引用静默四类正例；
///   体形多指令、描述符错配、synchronized、外类调用点 + owner clinit 非中性
///   四类拒绝面；
/// - REWRITE：调用点原地改指（op + 成员引用换、srcPc 保）、代理本体不动；
/// - PROOF：类数不增 + 收敛哨兵（改写后再扫无站点）。
#include "grunt/proxy_inline.hpp"

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

namespace grunt::proxy_inline {
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

/// static 目标代理（形如语料里的 `*_redirected_*`，判据不认名字）：
/// private static bridge synthetic 体 = 实参过手 + invokestatic + ireturn。
const char* kStaticProxySource =
    ".super java/lang/Object\n"
    ".class public super fixture/Proxies {\n"
    "  .method private static redirected_add (II)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iload_1\n"
    "      invokestatic fixture/Calc.calc (II)I\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

const char* kCalcSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Calc {\n"
    "  .method public static calc (II)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iload_1\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

const char* kStaticCallerSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Caller {\n"
    "  .method public static run (II)I {\n"
    "    parameters: { a, b },\n"
    "    code: {\n"
    "      iload a\n"
    "      iload b\n"
    "      invokestatic fixture/Proxies.redirected_add (II)I\n"
    "      ireturn\n"
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

TEST(ProxyInlineScan, FindsStaticTargetProxy)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies", kStaticProxySource},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.proxyOwner, "fixture/Proxies");
    EXPECT_EQ(site.proxyName, "redirected_add");
    EXPECT_EQ(site.callerOwner, "fixture/Caller");
    EXPECT_EQ(site.targetOwner, "fixture/Calc");
    EXPECT_EQ(site.targetName, "calc");
    EXPECT_FALSE(site.virtualTarget);
    // 两个定义成立：redirected_add 本体，以及 Caller.run——它的体恰好也是
    // 「实参过手 + 单次 invokestatic + 返回」（代理的代理，下一轮收敛面）。
    EXPECT_EQ(result.proxiesFound, 2u);
    EXPECT_TRUE(result.rejects.empty());
}

TEST(ProxyInlineScan, FindsVirtualTargetProxy)
{
    // virtual 目标：接收者降成首实参（生成面的 invokevirtual 形状）。
    model::ResourceContainer container = makeContainer({
        {"fixture/Box",
         ".super java/lang/Object\n"
         ".class public super fixture/Box {\n"
         "  .method public get (I)I {\n"
         "    parameters: { v },\n"
         "    code: {\n"
         "      iload v\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static redirected_get (Lfixture/Box;I)I {\n"
         "    code: {\n"
         "      aload_0\n"
         "      iload_1\n"
         "      invokevirtual fixture/Box.get (I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static run (Lfixture/Box;I)I {\n"
         "    parameters: { box, v },\n"
         "    code: {\n"
         "      aload box\n"
         "      iload v\n"
         "      invokestatic fixture/Proxies.redirected_get (Lfixture/Box;I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_TRUE(site.virtualTarget);
    EXPECT_EQ(site.targetOwner, "fixture/Box");
    EXPECT_EQ(site.targetName, "get");
    EXPECT_EQ(site.targetDesc, "(I)I");
}

TEST(ProxyInlineScan, WalksWideSlots)
{
    // (JD)I：long 占槽 0-1，double 占槽 2-3——槽走必须跳过半个宽类型。
    model::ResourceContainer container = makeContainer({
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static redirected_mix (JD)I {\n"
         "    code: {\n"
         "      lload_0\n"
         "      dload_2\n"
         "      invokestatic fixture/Calc.mix (JD)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Calc",
         ".super java/lang/Object\n"
         ".class public super fixture/Calc {\n"
         "  .method public static mix (JD)I {\n"
         "    parameters: { a, d },\n"
         "    code: {\n"
         "      lload a\n"
         "      dload d\n"
         "      l2i\n"
         "      d2i\n"
         "      iadd\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static run (JD)I {\n"
         "    parameters: { a, d },\n"
         "    code: {\n"
         "      lload a\n"
         "      dload d\n"
         "      invokestatic fixture/Proxies.redirected_mix (JD)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().targetName, "mix");
    EXPECT_FALSE(result.sites.front().virtualTarget);
}

TEST(ProxyInlineScan, SilentlySkipsZeroReferenceProxies)
{
    // 形状成立但零调用点：死码是 DeadMember 的业务，本残留不产站点不刷拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies", kStaticProxySource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_TRUE(result.rejects.empty());
    EXPECT_EQ(result.proxiesFound, 1u);
}

TEST(ProxyInlineScan, RejectsBrokenBodyShape)
{
    // 体里多一条 pop：实参过手区多出非装载指令，形状失效。
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static redirected_add (II)I {\n"
         "    code: {\n"
         "      iload_0\n"
         "      iload_1\n"
         "      iconst_0\n"
         "      pop\n"
         "      invokestatic fixture/Calc.calc (II)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::BodyNotProxyShape), nullptr);
}

TEST(ProxyInlineScan, RejectsDescMismatch)
{
    // static 转发目标的描述符与代理不一致：换过去栈效应会变，拒绝。
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static redirected_bad (II)I {\n"
         "    code: {\n"
         "      iload_0\n"
         "      iload_1\n"
         "      invokestatic fixture/Calc.wrong (I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::TargetDescMismatch), nullptr);
}

TEST(ProxyInlineScan, RejectsSynchronizedProxy)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static synchronized redirected_add (II)I {\n"
         "    code: {\n"
         "      iload_0\n"
         "      iload_1\n"
         "      invokestatic fixture/Calc.calc (II)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::ProxySynchronized), nullptr);
}

TEST(ProxyInlineScan, RejectsExposedOwnerInit)
{
    // 调用点在 owner 类外，且 owner <clinit> 有副作用（athrow）：
    // 改指会丢掉对 owner 的主动使用，整代理放弃。
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      aconst_null\n"
         "      athrow\n"
         "    }\n"
         "  }\n"
         "  .method public static redirected_add (II)I {\n"
         "    code: {\n"
         "      iload_0\n"
         "      iload_1\n"
         "      invokestatic fixture/Calc.calc (II)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_NE(findReject(result, RejectReason::OwnerInitExposed), nullptr);
}

TEST(ProxyInlineScan, AllowsExternalCallerWithNeutralOwner)
{
    // 调用点在 owner 类外，但 owner 无 <clinit>（初始化无副作用）：
    // 门放行——生成面的外置 `C$InvokeProxy` 类就是这个形态。
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method public static redirected_add (II)I {\n"
         "    code: {\n"
         "      iload_0\n"
         "      iload_1\n"
         "      invokestatic fixture/Calc.calc (II)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_TRUE(result.rejects.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(ProxyInlineRewrite, RetargetsCallSiteInPlace)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies", kStaticProxySource},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.applied, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), std::string("fixture/Caller.class"));
    EXPECT_EQ(dirty.proxies, 1u);

    // 调用点改成真实目标；代理本体保持原样（死码交下游 DeadMember/DeadCode）。
    const std::string caller = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(caller.find("redirected_add"), std::string::npos) << caller;
    EXPECT_NE(caller.find("invokestatic fixture/Calc.calc"), std::string::npos) << caller;

    const std::string proxies = disassemble(classBytes(container, "fixture/Proxies.class"));
    EXPECT_NE(proxies.find("redirected_add"), std::string::npos) << proxies;
}

TEST(ProxyInlineRewrite, RetargetsVirtualCallSite)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Box",
         ".super java/lang/Object\n"
         ".class public super fixture/Box {\n"
         "  .method public get (I)I {\n"
         "    parameters: { v },\n"
         "    code: {\n"
         "      iload v\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Proxies",
         ".super java/lang/Object\n"
         ".class public super fixture/Proxies {\n"
         "  .method private static redirected_get (Lfixture/Box;I)I {\n"
         "    code: {\n"
         "      aload_0\n"
         "      iload_1\n"
         "      invokevirtual fixture/Box.get (I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Caller",
         ".super java/lang/Object\n"
         ".class public super fixture/Caller {\n"
         "  .method public static run (Lfixture/Box;I)I {\n"
         "    parameters: { box, v },\n"
         "    code: {\n"
         "      aload box\n"
         "      iload v\n"
         "      invokestatic fixture/Proxies.redirected_get (Lfixture/Box;I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.applied, 1u);

    const std::string caller = disassemble(classBytes(container, "fixture/Caller.class"));
    EXPECT_EQ(caller.find("redirected_get"), std::string::npos) << caller;
    EXPECT_NE(caller.find("invokevirtual fixture/Box.get"), std::string::npos) << caller;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(ProxyInlineProof, KeepsClassCountAndConverges)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Calc", kCalcSource},
        {"fixture/Proxies", kStaticProxySource},
        {"fixture/Caller", kStaticCallerSource},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_FALSE(dirty.uris.empty());

    const ProofResult verdict = proof(container, dirty, 3);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 收敛哨兵：改写后的容器再扫，站点归零（代理体里转发的是真目标，
    // 不构成新的代理调用点）。
    const ScanResult rescanned = scan(container);
    EXPECT_TRUE(rescanned.sites.empty()) << "改写一轮后不应残留可改站点";
}

}   // namespace
}   // namespace grunt::proxy_inline
