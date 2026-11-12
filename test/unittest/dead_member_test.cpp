/// @file        dead_member_test.cpp
/// @brief       残留 `dead_member` 的运行时引用面测试。
///
/// CP 普查只有静态引用视野，两类运行时引用必须由保守面兜住：
/// 1. BootstrapMethods，BSM 运行时经解密字符串 findStatic 本类成员
///   （Grunt 形态，字节码里没有明文引用），命中则整类成员不删；
/// 2. 注解字符串值，mixin 框架按注解值的方法名反射定位（@Redirect），
///    按名命中即不删。
/// 无 BSM、无注解命中的私有方法照常删，pass 在无混淆面上仍然有效。
#include "grunt/dead_member.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::dead_member {
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
        // 夹具失败现场落盘：源码比对用（rundir = 工程根）。
        const std::string dump = "test/_jasm_fail_" + name + ".jasm";
        std::FILE* f = std::fopen(dump.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail + "（源码转储 " + dump
                                 + "）");
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

/// 首部公共段：宿主类带一个纯算术私有方法 `secret`（无任何 CP 引用）。
std::string hostPrelude()
{
    return ".super java/lang/Object\n"
           ".class public super Host {\n"
           "  .method private static secret (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           // 汇编器的栈分析按 indy 名解析栈形（indy 消费 (II)I 压回返回值），
           // 同签名方法必须在场，也贴合真实 Grunt 形态：目标真实存在。
           "  .method public static t (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// 尾部公共段：公开入口 + 类收尾。
std::string hostTail()
{
    return "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 4\n"
           "      invokestatic java/lang/Math.max (II)I\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

// ============================================================================
// ==== SCAN / REWRITE：运行时引用保守面 =======================================
// ============================================================================

TEST(DeadMemberRuntime, KeepsMethodsInClassWithBootstrapMethods)
{
    // 类带 BootstrapMethods（jasm 的 invokedynamic 生成）：BSM 运行时解密出
    // `secret` 并 findStatic，普查看不见，必须整类跳过成员删除。
    model::ResourceContainer container = makeContainer({
        {"Host",
         hostPrelude() +
             "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
             "Ljava/lang/invoke/CallSite; {\n"
             "    parameters: { lk, nm, mt, tn, tc, tm },\n"
             "    code: {\n"
             "      aconst_null\n"
             "      areturn\n"
             "    }\n"
             "  }\n"
             "  .method public static call2 ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokedynamic t (II)I { invokestatic, Host.a, "
             "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
             "{ \"t\", \"Host\", (II)I }\n"
             "      ireturn\n"
             "    }\n"
             "  }\n" +
             hostTail()},
    });

    const ScanResult scanned = scan(container);
    bool hasSecretCandidate = false;
    for (const ClassCand& c : scanned.classes) {
        for (const MethodCand& m : c.methods) hasSecretCandidate |= (m.name == "secret");
    }
    EXPECT_FALSE(hasSecretCandidate) << "BSM 类的私有方法不该成为删除候选";

    const DirtySet dirty = rewrite(container, scanned);
    const std::string after = disassemble(classBytes(container, "Host.class"));
    EXPECT_NE(after.find("secret"), std::string::npos) << after;
}

TEST(DeadMemberRuntime, KeepsMethodNamedByAnnotationValue)
{
    // @Redirect 形态：注解字符串值 "secret" 按名引用方法，mixin 框架运行时
    // 反射定位，普查看不见，按名保留。
    model::ResourceContainer container = makeContainer({
        {"Host",
         hostPrelude() +
             "  .visible-annotation Org/Spongepowered/Redirect {\n"
             "    method: \"secret\"\n"
             "  }\n"
             "  .method public static call2 ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokestatic java/lang/Math.abs (I)I\n"
             "      ireturn\n"
             "    }\n"
             "  }\n" +
             hostTail()},
    });

    const ScanResult scanned = scan(container);
    bool hasSecretCandidate = false;
    for (const ClassCand& c : scanned.classes) {
        for (const MethodCand& m : c.methods) hasSecretCandidate |= (m.name == "secret");
    }
    EXPECT_FALSE(hasSecretCandidate) << "注解值按名引用的方法不该成为删除候选";
}

TEST(DeadMemberRuntime, DeletesUnreferencedPrivateMethodOnPlainClass)
{
    // 无 BSM、无注解命中：私有零引用方法照常删，pass 的本职工作不被
    // 保守面吞掉。
    model::ResourceContainer container = makeContainer({
        {"Host", hostPrelude() + hostTail()},
    });

    const ScanResult scanned = scan(container);
    bool hasSecretCandidate = false;
    for (const ClassCand& c : scanned.classes) {
        for (const MethodCand& m : c.methods) hasSecretCandidate |= (m.name == "secret");
    }
    ASSERT_TRUE(hasSecretCandidate) << "普通类的零引用私有方法应是候选";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_GE(dirty.methods, 1u);
    const std::string after = disassemble(classBytes(container, "Host.class"));
    EXPECT_EQ(after.find("secret"), std::string::npos) << after;
}

TEST(DeadMemberRuntime, KeepsSyntheticBridgeWhenBsmUnresolved)
{
    // 跨类 indy 目标形态：BSM 在 Host（此处不收敛，aconst_null 返回值不是
    // CallSite，等价于语料里解密方法体由运行时 agent 填充的形态），目标
    // 桥方法在 Mate，名字是解密产物，CP 零引用，静态上不可知。BSM 未收敛
    // 时 synthetic/bridge 候选整档豁免；同类的 private 候选不豁免（findStatic
    // 跨类要求 public，private 永远不可能是跨类目标）。
    model::ResourceContainer container = makeContainer({
        {"Host",
         hostPrelude() +
             "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
             "Ljava/lang/invoke/CallSite; {\n"
             "    parameters: { lk, nm, mt, tn, tc, tm },\n"
             "    code: {\n"
             "      aconst_null\n"
             "      areturn\n"
             "    }\n"
             "  }\n"
             "  .method public static call2 ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokedynamic t (II)I { invokestatic, Host.a, "
             "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
             "{ \"t\", \"Host\", (II)I }\n"
             "      ireturn\n"
             "    }\n"
             "  }\n" +
             hostTail()},
        {"Mate",
         ".super java/lang/Object\n"
         ".class public super Mate {\n"
         "  .method public static synthetic tgt (II)I {\n"
         "    code: {\n"
         "      iload 0\n"
         "      invokestatic java/lang/Math.abs (I)I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "  .method private static helper ()I {\n"
         "    code: {\n"
         "      bipush 7\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    ASSERT_GE(scanned.indyBsmFailed, 1u) << "夹具应有未收敛的 BSM 站点";

    bool bridgeCandidate    = false;
    bool privateCandidate   = false;
    for (const ClassCand& c : scanned.classes) {
        if (c.owner != "Mate") continue;
        for (const MethodCand& m : c.methods) {
            bridgeCandidate |= (m.name == "tgt");
            privateCandidate |= (m.name == "helper");
        }
    }
    EXPECT_FALSE(bridgeCandidate) << "BSM 未收敛时 bridge 候选该整档豁免";
    EXPECT_TRUE(privateCandidate) << "private 候选不受 bridge 豁免影响";

    const DirtySet dirty = rewrite(container, scanned);
    const std::string after = disassemble(classBytes(container, "Mate.class"));
    EXPECT_NE(after.find("tgt"), std::string::npos) << after;
    EXPECT_EQ(after.find("helper"), std::string::npos) << after;
}

// ============================================================================
// ==== 空壳类回收：全成员死 + 全容器零引用 → 摘类 ==============================
// ============================================================================

TEST(DeadMemberShell, RemovesUnreferencedProxyShellClass)
{
    // 外置代理壳形态：C$InvokeProxy 全成员是死候选（零引用 private/合成），
    // 删净后零成员零字段，且全容器无 CP 引用，整类摘除。
    model::ResourceContainer container = makeContainer({
        {"Host", hostPrelude() + hostTail()},
        {"C$InvokeProxy",
         ".super java/lang/Object\n"
         ".class public super C$InvokeProxy {\n"
         "  .method private static p (II)I {\n"
         "    code: {\n"
         "      iload 0\n"
         "      iload 1\n"
         "      iadd\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ca::usize before = 2;
    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    ASSERT_EQ(dirty.classes, 1u) << "代理壳类应整类摘除";
    ASSERT_EQ(container.hasResource("C$InvokeProxy.class"), false);
    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

TEST(DeadMemberShell, KeepsShellClassReferencedByCp)
{
    // 反证：壳类形态但别类 CP 有引用（new/字段类型描述符），不摘。
    model::ResourceContainer container = makeContainer({
        {"Host",
         hostPrelude() +
             "  .method public static make ()LC$Proxy; {\n"
             "    code: {\n"
             "      aconst_null\n"
             "      areturn\n"
             "    }\n"
             "  }\n" +
             hostTail()},
        {"C$Proxy",
         ".super java/lang/Object\n"
         ".class public super C$Proxy {\n"
         "  .method private static p (II)I {\n"
         "    code: {\n"
         "      iload 0\n"
         "      iload 1\n"
         "      iadd\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_EQ(dirty.classes, 0u) << "CP 描述符引用的壳类不摘";
    EXPECT_EQ(container.hasResource("C$Proxy.class"), true);
}

TEST(DeadMemberShell, KeepsClassWithMainMethod)
{
    // 反证：main 所在类有成员（main 永不是删除候选），零成员检查天然挡住；
    // 只有 main 一个成员的类不摘。
    model::ResourceContainer container = makeContainer({
        {"Main",
         ".super java/lang/Object\n"
         ".class public super Main {\n"
         "  .method public static main ([Ljava/lang/String;)V {\n"
         "    code: {\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_EQ(dirty.classes, 0u) << "含 main 的主类不摘";
    EXPECT_EQ(container.hasResource("Main.class"), true);
}

TEST(DeadMemberRuntime, ProofHoldsOnRuntimeReferencedClass)
{
    model::ResourceContainer container = makeContainer({
        {"Host",
         hostPrelude() +
             "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;"
             "Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
             "Ljava/lang/invoke/CallSite; {\n"
             "    parameters: { lk, nm, mt, tn, tc, tm },\n"
             "    code: {\n"
             "      aconst_null\n"
             "      areturn\n"
             "    }\n"
             "  }\n"
             "  .method public static call2 ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokedynamic t (II)I { invokestatic, Host.a, "
             "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
             "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
             "{ \"t\", \"Host\", (II)I }\n"
             "      ireturn\n"
             "    }\n"
             "  }\n" +
             hostTail()},
    });

    const ca::usize before = 1;
    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}


// ============================================================================
// ==== 引用普查补全（普查漏计修复的拒证面）======================================
// ============================================================================
//
// 漏计现场（doc/kasumi-verify.rst §3）：Kasumi 专链产物 16 类带非零长 Raw 属性
// （cp_compact 封闭集拒绝，AttrNotModeled），旧普查把这批类整体排除在引用方
// 之外，调用方 `w` 池里对 `t.aa (III)I` 的 Methodref 从活引用集里蒸发，
// `t` 类 93 方法全删，运行期 NoSuchMethodError。缩样拒证三态：
//   A. 改写产物形态：引用方带未建模属性（压缩失败）+ 方法体 invokestatic
//      ， 目标方法不删；
//   B. 常量池间接引用形态：引用方方法体零引用、池里躺着 Methodref（压缩
//      失败类上池不清死，pool-only 条目存活）， 目标方法不删；
//   C. census 盲区：某类连骨架解析都失败（引用证据不可得）， 整轮零删除。

/// @brief 目标类夹具：private static secret（漏计案例里 `t` 类成员形态）。
std::string mateSource()
{
    return ".super java/lang/Object\n"
           ".class public super Mate {\n"
           "  .method private static secret (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 给汇编产物挂一条非零长未建模类属性（RawAttr）→ cp_compact 封闭集
///        拒绝（AttrNotModeled），类落 compactFailed、池不重编号，修复前改写
///        产物形态（jasm 表达不了任意属性，程序化补挂，metadata_strip 测试
///        同形）。@return 补挂后的类字节。
std::vector<ca::u8> attachUnmodeledClassAttr(const std::string& name,
                                             const std::string& source)
{
    const std::vector<ca::u8> bytes = assemble(name, source);
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("夹具重解析失败：" + name);
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    std::vector<ca::u8>   payload{0xBE, 0xEF, 0x00};   // 非零长：零长 Raw 会被封闭集容忍
    cf.attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf.cp.addUtf8("KasumiWatermark"),
        std::make_unique<jbc::core::RawAttr>(std::move(payload))));
    auto encoded = jbc::core::writeClass(cf);
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败：" + name);
    return std::move(encoded).unwrap();
}

model::ResourceContainer makeContainerFromBytes(
    std::initializer_list<std::pair<std::string, std::vector<ca::u8>>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, bytes] : classes) {
        const std::string uri = name + ".class";
        container.addResource(uri,
                              std::make_unique<model::ClassResource>(uri, bytes));
    }
    return container;
}

bool mateKeepsSecret(const model::ResourceContainer& container)
{
    const std::string after = disassemble(classBytes(container, "Mate.class"));
    return after.find("secret") != std::string::npos;
}

TEST(DeadMemberCensus, KeepsTargetWhenReferencerFailsCompaction)
{
    // 形态 A（漏计主通道）：调用方 Host 方法体 invokestatic Mate.secret，但
    // Host 带未建模属性 → 压缩失败 → 旧普查把 Host 排除在引用方之外，
    // Mate.secret 被误删；补全后 Host 的池照常入普查，secret 不删。
    model::ResourceContainer container = makeContainerFromBytes({
        {"Mate", assemble("Mate", mateSource())},
        {"Host",
         attachUnmodeledClassAttr(
             "Host",
             ".super java/lang/Object\n"
             ".class public super Host {\n"
             "  .method public static a ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokestatic Mate.secret (II)I\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n")},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_GE(dirty.rejectedByRef, 1u) << "引用检查应拦下 Mate.secret";
    EXPECT_TRUE(mateKeepsSecret(container)) << "压缩失败调用方的引用必须入普查";
}

TEST(DeadMemberCensus, KeepsTargetForPoolOnlyMethodref)
{
    // 形态 B（普查原语的直接拒证）：Host 方法体零引用，池里只躺着一条
    // Methodref（压缩失败类池不清死，pool-only 条目存活到普查面）。只要池里
    // 有指向，成员就不删，宁误留不误删。
    model::ResourceContainer container = makeContainerFromBytes({
        {"Mate", assemble("Mate", mateSource())},
        {"Host",
         attachUnmodeledClassAttr(
             "Host",
             ".super java/lang/Object\n"
             ".class public super Host {\n"
             "  .method public static a ()I {\n"
             "    code: {\n"
             "      bipush 3\n"
             "      bipush 4\n"
             "      invokestatic java/lang/Math.max (II)I\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"
             "}\n")},
    });

    // 程序化注入 pool-only Methodref：Mate.secret (II)I。
    {
        const model::IResource* raw = container.getResource("Host.class");
        const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
        ASSERT_NE(cls, nullptr);
        auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        ASSERT_FALSE(loaded.is_err());
        auto                  session = std::move(loaded).unwrap();
        jbc::core::ClassFile& cf      = session->backing();
        const ca::u32 clsIdx = cf.cp.addClass(cf.cp.addUtf8("Mate"));
        const ca::u32 natIdx
            = cf.cp.addNameAndType(cf.cp.addUtf8("secret"), cf.cp.addUtf8("(II)I"));
        ASSERT_NE(cf.cp.addRef(jbc::core::CpTag::Methodref, clsIdx, natIdx), 0u);
        auto encoded = jbc::core::writeClass(cf);
        ASSERT_FALSE(encoded.is_err());
        container.replaceResource(
            std::string("Host.class"),
            std::make_unique<model::ClassResource>(std::string("Host.class"),
                                                   std::move(encoded).unwrap()));
    }

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_GE(dirty.rejectedByRef, 1u) << "pool-only Methodref 也应入普查拦删";
    EXPECT_TRUE(mateKeepsSecret(container)) << "常量池间接引用指向的方法不删";
}

TEST(DeadMemberCensus, CensusBlindStopsWholeRound)
{
    // 形态 C（fail-closed 底座）：一个类连骨架解析都失败，它的引用证据完全
    // 不可得，任何候选都无法被证明死，本轮成员删除整体放弃（宁整轮不删）。
    std::vector<ca::u8> blind = assemble(
        "Blind",
        ".super java/lang/Object\n"
        ".class public super Blind {\n"
        "  .method public static x ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n");
    // 确定性畸形：magic + 版本 + cpCount=0xFFFF 后截断，readClass 必 Err。
    blind.resize(10);
    blind[8]  = 0xFF;
    blind[9]  = 0xFF;

    model::ResourceContainer container = makeContainerFromBytes({
        {"Mate", assemble("Mate", mateSource())},
        {"Blind", std::move(blind)},
    });

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 0u) << "census 盲区本轮应零删除";
    EXPECT_GE(dirty.censusBlind, 1u) << "盲区类应入账";
    EXPECT_TRUE(mateKeepsSecret(container)) << "引用证据不足不删";
}

}   // namespace
}   // namespace grunt::dead_member
