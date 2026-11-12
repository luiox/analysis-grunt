/// @file        indy_decode_test.cpp
/// @brief       残留 `indy_decode` 的三段判据测试（参考实现 2 case 迁移 + 拒绝面反证）。
///
/// 夹具是「缩小版 Kasumi 形态」（jasm 现场汇编，不落二进制、不依赖语料目录）：
/// 每类自带 8 参 BSM + `(String;)String` XOR 解密器（双层 `charAt; LDC K1;
/// LDC K2; IXOR; IXOR; i2c` 形状，key 逐字取参考实现测试的 0x29432d6c）；密文串
/// 以 \uXXXX 转义落 jasm 源（XOR 结果的码元任意，转义面无损）。
///
/// 覆盖分工：
/// - SCAN：key 链 + 解密 + 7 形态表 → 站点成立；Era 未判 / key 缺失 /
///   bsmArgs 畸形 / desc 互证不符 / 接口无旁证 → 拒绝面逐类记账；
/// - REWRITE：static 原地换 invokestatic；virtual 插 spill/checkcast 后落
///   invokeinterface（接口旁证注入面）；拒绝面上容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零。
#include "grunt/indy_decode.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::indy_decode {
namespace {

namespace model = mjbase::model;

constexpr ca::i32 kKey      = static_cast<ca::i32>(0x29432d6c);   // 参考实现测试同 key
constexpr ca::i32 kKeyPartA = 0x11111111;
constexpr ca::i32 kKeyPartB = kKey ^ kKeyPartA;   // 双层 ixor 的另一半

const std::string kKasumiBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/Integer;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
    "Ljava/lang/Integer;)Ljava/lang/invoke/CallSite;";

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief 明文 → 密文的 jasm 字面量：每个 UTF-16 码元 XOR key 低 16 位后落
///        `\uXXXX` 转义（夹具明文全 ASCII，码元不进增补平面）。
std::string xorEscape(const std::string& plain, ca::i32 key)
{
    const ca::u16 lo = static_cast<ca::u16>(key);
    std::string out;
    char buf[8];
    for (const char c : plain) {
        const ca::u16 enc = static_cast<ca::u16>(static_cast<ca::u16>(c) ^ lo);
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(enc));
        out += buf;
    }
    return out;
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

/// @brief Kasumi 形状夹具。`withDecryptor=false` = key 缺失反证（BSM 仍指向
///        dec，但解密器体不在场）。类名必须每夹具唯一：key 链按内部类名
///        归账，重名会在容器里串账（同 key 蒙混过关）。
std::string hostSource(const std::string& hostName, bool withDecryptor,
                       const std::string& staticArgs, const std::string& virtualArgs)
{
    std::string src = ".super java/lang/Object\n"
                      ".class public super " + hostName + " {\n";
    if (withDecryptor) {
        src += "  .method private static dec (Ljava/lang/String;)Ljava/lang/String; {\n"
               "    code: {\n"
               "      aload 0\n"
               "      iconst_0\n"
               "      invokevirtual java/lang/String.charAt (I)C\n"
               "      ldc " + std::to_string(kKeyPartA) + "\n"
               "      ldc " + std::to_string(kKeyPartB) + "\n"
               "      ixor\n"
               "      ixor\n"
               "      i2c\n"
               "      pop\n"
               "      aconst_null\n"
               "      areturn\n"
               "    }\n"
               "  }\n";
    }
    src += "  .method public static bsm (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Integer;"
           "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;"
           ")Ljava/lang/invoke/CallSite; {\n"
           "    code: {\n"
           "      aload 4\n"
           "      invokestatic " + hostName +
           ".dec (Ljava/lang/String;)Ljava/lang/String;\n"
           "      pop\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n";
    if (!staticArgs.empty()) {
        src += "  .method public static caller (I)I {\n"
               "    code: {\n"
               "      iload 0\n"
               "      invokedynamic bsm (I)I { invokestatic, " + hostName + ".bsm, " +
               kKasumiBsmDesc + " } { " + staticArgs + " }\n"
               "      ireturn\n"
               "    }\n"
               "  }\n";
    }
    if (!virtualArgs.empty()) {
        src += "  .method public static virtualCaller (Ljava/util/Iterator;)Z {\n"
               "    code: {\n"
               "      aload 0\n"
               "      invokedynamic bsm (Ljava/util/Iterator;)Z { invokestatic, " + hostName +
               ".bsm, " + kKasumiBsmDesc + " } { " + virtualArgs + " }\n"
               "      ireturn\n"
               "    }\n"
               "  }\n";
    }
    src += "}\n";
    return src;
}

/// static 站点实参：mode=0，owner/name/desc 解出 java.lang.Math.abs (I)I。
std::string staticArgsOk()
{
    return "0, \"" + xorEscape("java.lang.Math", kKey) + "\", \""
           + xorEscape("abs", kKey) + "\", \"" + xorEscape("(I)I", kKey) + "\", 0";
}

/// virtual 站点实参：mode=0 extra=1，owner 解出 java.util.Iterator（JDK 接口）。
std::string virtualArgsOk()
{
    return "0, \"" + xorEscape("java.util.Iterator", kKey) + "\", \""
           + xorEscape("hasNext", kKey) + "\", \"" + xorEscape("()Z", kKey) + "\", 1";
}

ScanInput kasumiInput(const std::unordered_set<std::string>& jdkInterfaces
                      = std::unordered_set<std::string>{})
{
    ScanInput input;
    input.era = knowledge::Era::Kasumi;
    input.jdkInterfaces = jdkInterfaces;
    return input;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(IndyDecodeScan, DecodesStaticSiteThroughKeyChain)
{
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, staticArgsOk(), "")}});

    const ScanResult result = scan(container, kasumiInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.totalIndy, 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "KasumiHost.class");
    EXPECT_EQ(site.form, knowledge::KasumiIndyForm::Static);
    EXPECT_EQ(site.targetOwner, "java/lang/Math");
    EXPECT_EQ(site.targetName, "abs");
    EXPECT_EQ(site.targetDesc, "(I)I");
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyDecodeScan, RejectsSiteWhenEraUnknown)
{
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, staticArgsOk(), "")}});

    ScanInput input;   // era 默认 Unknown
    const ScanResult result = scan(container, input);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.totalIndy, 1u);
    EXPECT_EQ(result.eraBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_FALSE(result.rejects.front().why.empty());
}

TEST(IndyDecodeScan, RejectsSiteWhenDecryptorKeyMissing)
{
    // BSM 体仍指向 dec，但解密器方法不在场：key 链在最后一环断开。
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", false, staticArgsOk(), "")}});

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.keyMissing, 1u);
}

TEST(IndyDecodeScan, RejectsSiteWhenBsmArgsMalformed)
{
    // 首参给 String（应为 Integer mode）：参数种类不符。
    model::ResourceContainer container = makeContainer(
        {{"KasumiHost",
          hostSource("KasumiHost", true, "\"x\", \"enc\", \"nm\", \"ed\", 0", "")}});

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.malformedArguments, 1u);
}

TEST(IndyDecodeScan, RejectsSiteWhenDescCrossCheckFails)
{
    // 解密 desc 与 indy desc 互证不符：S1 拒绝（(II)V vs (I)I）。
    const std::string args = "0, \"" + xorEscape("java.lang.Math", kKey) + "\", \""
                             + xorEscape("abs", kKey) + "\", \"" + xorEscape("(II)V", kKey)
                             + "\", 0";
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, args, "")}});

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.descMismatch, 1u);
}

TEST(IndyDecodeScan, RejectsVirtualSiteWhenInterfaceEvidenceAbsent)
{
    // 接口集空 = 无旁证：virtual 目标不在容器时 fail-closed。
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, "", virtualArgsOk())}});

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.interfaceUnattested, 1u);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(IndyDecodeRewrite, SwapsStaticIndyForInvokestatic)
{
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, staticArgsOk(), "")}});
    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.rewrittenStatic, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string after = disassemble(classBytes(container, "KasumiHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokestatic java/lang/Math.abs (I)I"), std::string::npos) << after;
}

TEST(IndyDecodeRewrite, SpillsReceiverAndDispatchesInvokeinterface)
{
    // 参考实现 case 2：virtual 目标是 JDK 接口（java/util/Iterator.hasNext）时必须
    // 走 invokeinterface——旁证由注入面给（穷举普查，成员与否双向作证）。
    model::ResourceContainer container = makeContainer(
        {{"KasumiHost", hostSource("KasumiHost", true, "", virtualArgsOk())}});
    const ScanResult scanned =
        scan(container, kasumiInput(std::unordered_set<std::string>{"java/util/Iterator"}));
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_TRUE(scanned.sites.front().ownerIsInterface);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.rewrittenVirtual, 1u);

    const std::string after = disassemble(classBytes(container, "KasumiHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("checkcast java/util/Iterator"), std::string::npos) << after;
    EXPECT_NE(after.find("invokeinterface java/util/Iterator.hasNext ()Z"), std::string::npos)
        << after;
}

TEST(IndyDecodeRewrite, LeavesContainerUntouchedOnRejections)
{
    // 拒绝面四态（key 缺失 / 参数畸形 / desc 互证不符 / 接口无旁证）：字节逐位不动。
    std::vector<std::pair<std::string, std::string>> fixtures;
    fixtures.push_back(
        {"noKey", hostSource("NoKeyHost", false, staticArgsOk(), "")});
    fixtures.push_back(
        {"badArgs", hostSource("BadArgsHost", true, "\"x\", \"enc\", \"nm\", \"ed\", 0", "")});
    fixtures.push_back(
        {"badDesc",
         hostSource("BadDescHost", true,
                    "0, \"" + xorEscape("java.lang.Math", kKey) + "\", \""
                        + xorEscape("abs", kKey) + "\", \""
                        + xorEscape("(II)V", kKey) + "\", 0",
                    "")});
    fixtures.push_back(
        {"noIface", hostSource("NoIfaceHost", true, "", virtualArgsOk())});

    std::vector<std::vector<ca::u8>> before;
    model::ResourceContainer         container;
    for (const auto& [name, source] : fixtures) {
        const std::string uri = name + ".class";
        const std::vector<ca::u8> bytes = assemble(name, source);
        before.push_back(bytes);
        container.addResource(uri,
                              std::make_unique<model::ClassResource>(uri, bytes));
    }

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.keyMissing, 1u);
    EXPECT_EQ(scanned.malformedArguments, 1u);
    EXPECT_EQ(scanned.descMismatch, 1u);
    EXPECT_EQ(scanned.interfaceUnattested, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 0u);
    EXPECT_TRUE(dirty.uris.empty());

    for (ca::usize i = 0; i < fixtures.size(); ++i) {
        EXPECT_EQ(classBytes(container, fixtures[i].first + ".class"), before[i])
            << fixtures[i].first << " 被改写";
    }
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(IndyDecodeProof, KeepsClassCountReparsesAndResidualZero)
{
    model::ResourceContainer container =
        makeContainer({{"KasumiHost", hostSource("KasumiHost", true, staticArgsOk(), virtualArgsOk())}});
    const ScanResult scanned =
        scan(container, kasumiInput(std::unordered_set<std::string>{"java/util/Iterator"}));
    ASSERT_EQ(scanned.sites.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 2u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

}   // namespace
}   // namespace grunt::indy_decode
