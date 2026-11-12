/// @file        string_cache_fold_test.cpp
/// @brief       残留 `string_cache_fold` 的三段判据测试（参考实现两形态迁移 +
///              拒绝面反证）。
///
/// 夹具是缩小版 Kasumi 字符串缓存形态（jasm 现场汇编，不落二进制、不
/// 依赖语料目录）：每类带 `String[]` 缓存字段与 `byte[][]` 密文字段各一，
/// 调用点各一个，
///
///     形态 1（读缓存）：getstatic a:[Ljava/lang/String;; iconst_1; aaload
///     形态 2（直接解密）：getstatic b:[[B; iconst_1; aaload; dup; arraylength;
///                        iconst_0; invokestatic dec ([BII)Ljava/lang/String;
///
/// 旁证（明文表）按 ScanInput 直注（parseTsv 纯函数面直测转义/坏行/点分
/// 转换；CLI `--tsv` 文件读取是 adapter 面，不在本测）。
///
/// 覆盖分工：
/// - SCAN：两形态形状钉面 + 表命中定案；Era 未判 / 旁证缺席 / 旁证不可读 /
///   表无该条目 / 形状畸形 → 拒绝面（或静默跳过）逐桶计数；
/// - REWRITE：两形态锚点原地换 ldc + 跨度擦除；拒绝面上容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零。
#include "grunt/string_cache_fold.hpp"

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

namespace grunt::string_cache_fold {
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

/// @brief Kasumi 字符串缓存形状夹具。两形态站点各一个；`malformed=true` 时
///        reader 的 aaload 换成 pop（形状畸形的反证面）。
std::string hostSource(const std::string& hostName, bool malformed)
{
    const std::string readerTail =
        malformed
            ? "      pop\n"
              "      aconst_null\n"
              "      areturn\n"
            : "      aaload\n"
              "      areturn\n";
    return ".super java/lang/Object\n"
           ".class public super " + hostName + " {\n"
           "  .field public static a [Ljava/lang/String;\n"
           "  .field public static b [[B\n"
           "\n"
           "  .method public static dec ([BII)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static reader (I)Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + hostName + ".a [Ljava/lang/String;\n"
           "      iconst_1\n" + readerTail +
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static directReader (I)Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + hostName + ".b [[B\n"
           "      iconst_1\n"
           "      aaload\n"
           "      dup\n"
           "      arraylength\n"
           "      iconst_0\n"
           "      invokestatic " + hostName + ".dec ([BII)Ljava/lang/String;\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 旁证注入（命中面：两站点各一条）。
ScanInput kasumiInput()
{
    ScanInput input;
    input.era     = knowledge::Era::Kasumi;
    input.tsvState = TsvState::Loaded;
    input.plaintexts["CacheHost"]["a"][1]       = "cached-hello";
    input.plaintexts["CacheHost"]["byte:b"][1]  = "direct-plain";
    return input;
}

// ============================================================================
// ==== TSV 旁证解析（纯函数面）===============================================
// ============================================================================

TEST(StringCacheTsv, ParsesEscapedRowsAndConvertsOwner)
{
    PlaintextTable table;
    const ca::usize rows = parseTsv(
        "dev.kasumi.obfuscator.w\ta\t1\thello\\tworld\\\\end\n"
        "dev.kasumi.obfuscator.w\tbyte:b\t0\t\\nplain\n",
        table);
    EXPECT_EQ(rows, 2u);
    EXPECT_EQ(table.at("dev/kasumi/obfuscator/w").at("a").at(1), "hello\tworld\\end");
    EXPECT_EQ(table.at("dev/kasumi/obfuscator/w").at("byte:b").at(0), "\nplain");
}

TEST(StringCacheTsv, SkipsMalformedRows)
{
    PlaintextTable table;
    // 列数不足 / index 非整数 / 空行。
    const ca::usize rows = parseTsv(
        "owner\tfield\tnot-an-int\ttext\n"
        "owner\tfield\n"
        "\n"
        "owner\tfield\t2\tkept\n",
        table);
    EXPECT_EQ(rows, 1u);
    EXPECT_EQ(table.at("owner").at("field").at(2), "kept");
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(StringCacheScan, AcceptsCacheReadSiteOnTableHit)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    const ScanResult result = scan(container, kasumiInput());
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(result.totalShapeHits, 2u);
    // 夹具 reader 在 directReader 之前，站点按布局序产出：形态 1 在前。
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "CacheHost.class");
    EXPECT_EQ(site.form, Form::CacheRead);
    EXPECT_EQ(site.owner, "CacheHost");
    EXPECT_EQ(site.fieldKey, "a");
    EXPECT_EQ(site.index, 1);
    EXPECT_EQ(site.plaintext, "cached-hello");
    EXPECT_EQ(result.sites.back().form, Form::DirectDecrypt);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(StringCacheScan, AcceptsDirectDecryptSiteOnTableHit)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    ScanInput input = kasumiInput();
    input.plaintexts.clear();   // 只留 byte:b 侧：单形态定案
    input.plaintexts["CacheHost"]["byte:b"][1] = "direct-plain";

    const ScanResult result = scan(container, input);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.form, Form::DirectDecrypt);
    EXPECT_EQ(site.fieldKey, "byte:b");
    EXPECT_EQ(site.plaintext, "direct-plain");
    EXPECT_EQ(result.noPlaintext, 1u);   // 形态 1 无表条目：参考实现静默跳过 + 计数
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(StringCacheScan, RejectsSitesWhenEraUnknown)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    ScanInput input = kasumiInput();
    input.era     = knowledge::Era::Unknown;   // S3 检查：旁证表不可用
    const ScanResult result = scan(container, input);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.totalShapeHits, 2u);
    EXPECT_EQ(result.eraBlocked, 2u);
    ASSERT_EQ(result.rejects.size(), 2u);
    EXPECT_FALSE(result.rejects.front().why.empty());
}

TEST(StringCacheScan, RejectsSitesWhenTsvAbsentOrUnreadable)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    // 旁证缺席（CLI 未传）。
    ScanInput absent;
    absent.era     = knowledge::Era::Kasumi;
    absent.tsvState = TsvState::Absent;
    ScanResult result = scan(container, absent);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.tsvBlocked, 2u);

    // 旁证注入了但文件不可读（旁证降级 fail-closed）。
    ScanInput unreadable;
    unreadable.era     = knowledge::Era::Kasumi;
    unreadable.tsvState = TsvState::Unreadable;
    unreadable.plaintexts["CacheHost"]["a"][1] = "stale-evidence";   // 表内容不可信
    result = scan(container, unreadable);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.tsvBlocked, 2u);
}

TEST(StringCacheScan, CountsNoPlaintextWhenTableMisses)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    // 表已注入但下标对不上：参考实现静默跳过对齐，站点不产出。
    ScanInput input = kasumiInput();
    input.plaintexts.clear();
    input.plaintexts["CacheHost"]["a"][7]      = "wrong-index";
    input.plaintexts["OtherHost"]["byte:b"][1] = "wrong-owner";
    const ScanResult result = scan(container, input);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.noPlaintext, 2u);
}

TEST(StringCacheScan, SkipsMalformedShapeSilently)
{
    // 形态 1 半截形状（aaload 换成了 pop）：非本家站点，不进形状命中也不拒绝；
    // 形态 2 不受牵连，照常定案。
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", true)}});

    const ScanResult result = scan(container, kasumiInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().form, Form::DirectDecrypt);
    EXPECT_EQ(result.totalShapeHits, 1u);
    EXPECT_EQ(result.rejects.size(), 0u);
    EXPECT_EQ(result.noPlaintext, 0u);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(StringCacheRewrite, FoldsCacheReadToLdc)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});
    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 2u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.rewrittenCacheReads, 1u);
    EXPECT_EQ(dirty.rewrittenDirectDecrypt, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string after = disassemble(classBytes(container, "CacheHost.class"));
    EXPECT_NE(after.find("ldc \"cached-hello\""), std::string::npos) << after;
    EXPECT_EQ(after.find("aaload"), std::string::npos) << after;
    EXPECT_EQ(after.find("getstatic CacheHost.a"), std::string::npos) << after;
}

TEST(StringCacheRewrite, FoldsDirectDecryptToLdc)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});

    ScanInput input = kasumiInput();
    input.plaintexts.clear();
    input.plaintexts["CacheHost"]["byte:b"][1] = "direct-plain";

    const ScanResult scanned = scan(container, input);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.rewrittenDirectDecrypt, 1u);

    const std::string after = disassemble(classBytes(container, "CacheHost.class"));
    EXPECT_NE(after.find("ldc \"direct-plain\""), std::string::npos) << after;
    EXPECT_EQ(after.find("arraylength"), std::string::npos) << after;
    EXPECT_EQ(after.find("invokestatic CacheHost.dec"), std::string::npos) << after;
    EXPECT_EQ(after.find("getstatic CacheHost.b"), std::string::npos) << after;
}

TEST(StringCacheRewrite, KeepsBytesUntouchedOnRejections)
{
    // 拒绝面：表无条目（noPlaintext 静默跳过，含形状畸形类），单容器单输入
    // 全拒，字节逐位不动。Era 未判 / 旁证缺席的独占检查在各自 SCAN 测里。
    std::vector<std::pair<std::string, std::string>> fixtures;
    fixtures.push_back({"plainHost", hostSource("PlainHost", false)});
    fixtures.push_back({"badHost", hostSource("BadHost", true)});

    std::vector<std::vector<ca::u8>> before;
    model::ResourceContainer         container;
    for (const auto& [name, source] : fixtures) {
        const std::string         uri   = name + ".class";
        const std::vector<ca::u8> bytes = assemble(name, source);
        before.push_back(bytes);
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    }

    ScanInput input;
    input.era     = knowledge::Era::Kasumi;
    input.tsvState = TsvState::Loaded;
    input.plaintexts["Nobody"]["x"][0] = "unrelated";   // 两类全部 miss

    const ScanResult scanned = scan(container, input);
    EXPECT_EQ(scanned.totalShapeHits, 3u);   // badHost 的形态 1 畸形不钉
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.noPlaintext, 3u);
    EXPECT_EQ(scanned.rejects.size(), 0u);

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

TEST(StringCacheProof, KeepsClassCountReparsesAndResidualZero)
{
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});
    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 2u);

    const ProofResult verdict = proof(container, dirty, 1, kasumiInput());
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(StringCacheProof, CountsResidualWhenSitesUntouched)
{
    // 旁证命中但没跑 rewrite：residual 如实报告本可折叠的站点数。
    model::ResourceContainer container =
        makeContainer({{"CacheHost", hostSource("CacheHost", false)}});
    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 2u);

    DirtySet dirty;   // 空：没改任何东西
    const ProofResult verdict = proof(container, dirty, 1, kasumiInput());
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 2u);
}

}   // namespace
}   // namespace grunt::string_cache_fold
