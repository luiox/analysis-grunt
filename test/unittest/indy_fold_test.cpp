/// @file        indy_fold_test.cpp
/// @brief       残留 `indy_fold` 的三段判据测试。
///
/// 夹具是缩小版 Grunt indy 形态：BSM 不读前三个 JVM 合成形参（Lookup/name/
/// MethodType，折叠灌的是 Unknown），目标信息全部来自静态参数表；体感与真
/// 混淆器一致：forName → findStatic → ConstantCallSite。
///
/// 覆盖分工：
/// - SCAN：BSM 收敛 → 站点 foldable、目标四元组正确；BSM 不收敛 → bsmFailed；
/// - REWRITE：indy 就地变 invokestatic，目标引用正确；
/// - PROOF：类数不变 + 脏类可重解析。
#include "grunt/indy_fold.hpp"

#include "jbc/core/writer.hpp"
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

/// BSM 前三个形参是 JVM 合成的（Lookup/name/MethodType），折叠灌 Unknown，
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
    // BSM 返回 null：收敛但不是 CallSite，另一类拒绝面（载荷不完整）。
    return "      aconst_null\n"
           "      areturn\n";
}

// ---- fork 变体夹具 -----------------------------------------------------------

/// 装箱变体：BSM 第 7 静态参数是 Integer 包装面，BSM 体按 intValue 分支选路。
/// 夹具判据自证：拆箱 mock 只认 `@v` 槽 Obj 载荷，值面不对（未装箱/未知）分支
/// 走不到真体，BSM 返回 null → bsmFailed。
std::string boxedHostSource()
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
           "Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; {\n"
           "    parameters: { lk, nm, mt, targetName, targetCls, targetMt, key },\n"
           "    code: {\n"
           "      aload key\n"
           "      invokevirtual java/lang/Integer.intValue ()I\n"
           "      bipush 7\n"
           "      if_icmpne Lbad\n"
           "      new java/lang/invoke/ConstantCallSite\n"
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
           "      areturn\n"
           "    Lbad:\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 4\n"
           "      invokedynamic t (II)I { invokestatic, IndyHost.a, "
           "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/Integer;)Ljava/lang/invoke/CallSite; } "
           "{ \"t\", \"IndyHost\", (II)I, 7 }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 接收者放宽变体：调用点 desc 首参加宽成 `Ljava/lang/Object;`（BSM 侧
/// findVirtual + asType 引用放宽），句柄 desc 保持实型。targetOwner 给
/// "Iface" 即接口拒绝变体（配合 withInterfaceFlag 把 Iface 挂上 ACC_INTERFACE）。
std::string widenedHostSource(const char* targetOwner)
{
    const std::string owner(targetOwner);
    std::string src = ".super java/lang/Object\n"
                      ".class public super IndyHost {\n";
    // 调用点实参要 new 一个接收者实例。
    src += "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n";
    src += "  .method public t (L" + owner + ";)I {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
    // jasm 按 indy 名解析栈形（indy 消费 (Ljava/lang/Object;)I 压回返回值），
    // 同签名方法必须在场。
    src += "  .method public static t (Ljava/lang/Object;)I {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
    src += "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;"
           "Ljava/lang/String;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
           ")Ljava/lang/invoke/CallSite; {\n"
           "    parameters: { lk, nm, mt, targetName, targetCls, targetMt },\n"
           "    code: {\n"
           "      new java/lang/invoke/ConstantCallSite\n"
           "      dup\n"
           "      aload lk\n"
           "      aload targetCls\n"
           "      invokestatic java/lang/Class.forName (Ljava/lang/String;)Ljava/lang/Class;\n"
           "      aload targetName\n"
           "      aload targetMt\n"
           "      invokestatic java/lang/invoke/MethodHandles$Lookup.findVirtual "
           "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
           "Ljava/lang/invoke/MethodHandle;\n"
           "      invokespecial java/lang/invoke/ConstantCallSite.<init> "
           "(Ljava/lang/invoke/MethodHandle;)V\n"
           "      areturn\n"
           "    }\n"
           "  }\n";
    // 目标是无参实例方法：findVirtual 的 MethodType 实参不含接收者（()I），
    // 运行期句柄实型接收者经 asType 加宽成 Object 落在调用点首参槽。
    src += "  .method public static call ()I {\n"
           "    code: {\n"
           "      new IndyHost\n"
           "      dup\n"
           "      invokespecial IndyHost.<init> ()V\n"
           "      invokedynamic t (Ljava/lang/Object;)I { invokestatic, IndyHost.a, "
           "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
           "{ \"t\", \"" + owner + "\", ()I }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
    return src;
}

/// 接口目标类：jasm 无接口语法面，汇编后程序化挂 ACC_INTERFACE|ACC_ABSTRACT。
std::string ifaceSource()
{
    return ".super java/lang/Object\n"
           ".class public super Iface {\n"
           "  .method public t (LIface;)I {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 挂接口标志（OwnerKindCache 读 ACC_INTERFACE 0x0200 原字节判型）。
std::vector<ca::u8> withInterfaceFlag(const std::vector<ca::u8>& bytes)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("夹具重解析失败");
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    cf.access |= 0x0200 | 0x0400;   // ACC_INTERFACE | ACC_ABSTRACT
    auto encoded = jbc::core::writeClass(cf);
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
    return std::move(encoded).unwrap();
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

/// 双调用点变体：同一类里再加一个 indy 调用方法（scan 预算按类计数的夹具）。
std::string hostSourceTwoCalls(const std::string& bsmBody)
{
    const std::string base = hostSource(bsmBody);
    const std::string call2 =
        "  .method public static call2 ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      bipush 4\n"
        "      invokedynamic t (II)I { invokestatic, IndyHost.a, "
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
        "Ljava/lang/String;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
        "Ljava/lang/invoke/CallSite; } "
        "{ \"t\", \"IndyHost\", (II)I }\n"
        "      ireturn\n"
        "    }\n"
        "  }\n";
    const ca::usize tail = base.rfind('}');
    return base.substr(0, tail) + call2 + "}\n";
}

/// scan 预算超限：站点数超限的类余下站点不再跑 BSM（保守跳过，计数上报）。
TEST(IndyFoldScan, OverBudgetStopsBsmRunsConservatively)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", hostSourceTwoCalls(foldableBsmBody())}});

    // 默认预算：两个站点都跑（正常 jar 不触限）。
    const ScanResult normal = scan(container);
    EXPECT_EQ(normal.sites.size(), 2u);
    EXPECT_EQ(normal.classesSkippedByBudget, 0u);

    // 预算 1：首站点跑完，次站点起超限，余下站点保守跳过。
    const ScanResult starved = scan(container, iter_budget::ClassBudget{1});
    EXPECT_EQ(starved.sites.size(), 1u);
    EXPECT_EQ(starved.classesSkippedByBudget, 1u);
    EXPECT_EQ(starved.foldable, 1u);
}

/// 装箱修复的直接判据：Integer 包装面静态参数必须装箱进 `@v` 槽，拆箱 mock
/// 才能取到值（裸值传播 Unknown → if_icmpne 未知 → Bail@分支）。
TEST(IndyFoldScan, BoxesIntegerBsmArgForUnboxingMock)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", boxedHostSource()}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.foldable, 1u)
        << "Integer 静态参数未装箱时 BSM 体分支求值失败，站点必 Bail";
    const Site& site = result.sites.front();
    EXPECT_TRUE(site.foldable);
    EXPECT_EQ(site.target.kind, jsim::indy::Target::Kind::Static);
    EXPECT_EQ(site.target.owner, "IndyHost");
    EXPECT_EQ(site.target.name, "t");
    EXPECT_EQ(site.target.desc, "(II)I");
}

/// 接收者放宽形放行：句柄 desc 带实型接收者、调用点 desc 首参 Object，
/// 原地换 invokevirtual，栈效应不变（引用放宽）。
TEST(IndyFoldScan, FoldsReceiverWidenedVirtualSite)
{
    model::ResourceContainer container =
        makeContainer({{"IndyHost", widenedHostSource("IndyHost")}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    ASSERT_TRUE(site.foldable);
    EXPECT_EQ(site.target.kind, jsim::indy::Target::Kind::Virtual);
    EXPECT_EQ(site.target.desc, "()I") << "findVirtual 的 MethodType 实参不含接收者";
    EXPECT_EQ(site.indyDesc, "(Ljava/lang/Object;)I");

    const DirtySet dirty = rewrite(container, result);
    EXPECT_EQ(dirty.sites, 1u);
    const std::string after = disassemble(classBytes(container, "IndyHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokevirtual IndyHost.t ()I"), std::string::npos) << after;
}

/// 接口 owner 拒绝（fail-closed）：放宽形只产 invokevirtual，接口 owner 要
/// invokeinterface（字节长度不同，超出不变 pc 布局面），整站拒绝。
TEST(IndyFoldScan, RejectsWidenedSiteOnInterfaceOwner)
{
    model::ResourceContainer container;
    container.addResource(
        "IndyHost.class",
        std::make_unique<model::ClassResource>(std::string("IndyHost.class"),
                                               assemble("IndyHost", widenedHostSource("Iface"))));
    container.addResource(
        "Iface.class",
        std::make_unique<model::ClassResource>(
            std::string("Iface.class"),
            withInterfaceFlag(assemble("Iface", ifaceSource()))));

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_FALSE(site.foldable);
    EXPECT_NE(site.why.find("接口"), std::string::npos)
        << "拒绝原因应点名接口 owner：" << site.why;

    const DirtySet dirty = rewrite(container, result);
    EXPECT_EQ(dirty.sites, 0u);
    EXPECT_TRUE(dirty.uris.empty());
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
