/// @file        knowledge_era_test.cpp
/// @brief       knowledge::probeEra 的命中即判测试（Kasumi 三指纹）。
///
/// 夹具一律 jasm 现场汇编（不落二进制、不依赖语料目录）：
/// - 命名空间指纹：`dev/kasumi/obfuscator` 前缀类；
/// - 8 参 BSM 指纹：BootstrapMethods 条目挂 `kKasumiBootstrapDesc` 句柄；
/// - 反证：普通 6 参 BSM 类（indy_fold 形态）、空容器 → Unknown。
///
/// @Metadata 注解指纹不在此覆盖（jasm 夹具不带注解面）：该探针走与
/// booger_anno_strip 同一 decodeAttr 通路，语料单罐（SnakeGame-obf.jar）上
/// 以命名空间 + BSM 双指纹实证（doc/indy-decode.impl.md 语料账本）。
#include "grunt/knowledge/era.hpp"

#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::knowledge {
namespace {

namespace model = mjbase::model;

constexpr const char* kKasumiBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/Integer;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
    "Ljava/lang/Integer;)Ljava/lang/invoke/CallSite;";

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

/// 命名空间指纹夹具：普通类体，只靠 `dev/kasumi/obfuscator` 前缀命中。
std::string namespaceHostSource()
{
    return ".super java/lang/Object\n"
           ".class public super dev/kasumi/obfuscator/Widget {\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 8 参 BSM 指纹夹具：类名不带命名空间，只靠 BootstrapMethods 条目的
/// MethodHandle 描述符命中。BSM 体不必真返回 CallSite——指纹只读属性。
std::string bsmFingerprintSource()
{
    return ".super java/lang/Object\n"
           ".class public super BsmFpHost {\n"
           "  .method public static bsm (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Integer;"
           "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;"
           ")Ljava/lang/invoke/CallSite; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method public static caller ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      invokedynamic bsm (I)I { invokestatic, BsmFpHost.bsm, " +
           std::string(kKasumiBsmDesc) + " } { 0 }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 反证夹具：6 参 BSM（indy_fold 语料形态，非 Kasumi 签名）。
std::string plainIndySource()
{
    return ".super java/lang/Object\n"
           ".class public super PlainIndyHost {\n"
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
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 4\n"
           "      invokedynamic t (II)I { invokestatic, PlainIndyHost.a, "
           "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/String;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
           "{ \"t\", \"PlainIndyHost\", (II)I }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

// ============================================================================
// ==== probeEra ==============================================================
// ============================================================================

TEST(KnowledgeEra, NamespaceFingerprintHitsKasumi)
{
    model::ResourceContainer container = makeContainer({{"Widget", namespaceHostSource()}});
    EXPECT_EQ(probeEra(container), Era::Kasumi);
}

TEST(KnowledgeEra, BootstrapDescFingerprintHitsKasumi)
{
    model::ResourceContainer container = makeContainer({{"BsmFpHost", bsmFingerprintSource()}});
    EXPECT_EQ(probeEra(container), Era::Kasumi);
}

TEST(KnowledgeEra, PlainClassAndForeignBsmStayUnknown)
{
    EXPECT_EQ(probeEra(makeContainer({{"PlainIndyHost", plainIndySource()}})), Era::Unknown);

    model::ResourceContainer empty;
    EXPECT_EQ(probeEra(empty), Era::Unknown);
}

}   // namespace
}   // namespace grunt::knowledge
