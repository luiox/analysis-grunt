/// @file        dead_member_test.cpp
/// @brief       残留 `dead_member` 的运行时引用面测试。
///
/// CP 普查只有静态引用视野，两类运行时引用必须由保守面兜住：
/// 1. BootstrapMethods——BSM 运行时经解密字符串 findStatic 本类成员
///   （Grunt 形态，字节码里没有明文引用），命中则整类成员不删；
/// 2. 注解字符串值——mixin 框架按注解值的方法名反射定位（@Redirect），
///    按名命中即不删。
/// 无 BSM、无注解命中的私有方法照常删——pass 在无混淆面上仍然有效。
#include "grunt/dead_member.hpp"

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
           // 同签名方法必须在场——也贴合真实 Grunt 形态：目标真实存在。
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
    // `secret` 并 findStatic——普查看不见，必须整类跳过成员删除。
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
    // @Redirect 形态：注解字符串值 "secret" 按名引用方法——mixin 框架运行时
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
    // 无 BSM、无注解命中：私有零引用方法照常删——pass 的本职工作不被
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
    // 跨类 indy 目标形态：BSM 在 Host（此处不收敛——aconst_null 返回值不是
    // CallSite，等价于语料里「解密方法体由运行时 agent 填充」的形态），目标
    // 桥方法在 Mate——名字是解密产物，CP 零引用，静态上不可知。BSM 未收敛
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


}   // namespace
}   // namespace grunt::dead_member
