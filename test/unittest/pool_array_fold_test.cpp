/// @file        pool_array_fold_test.cpp
/// @brief       残留 `pool_array_fold` 的三段判据测试（jasm 夹具 + clinit
///              求值通道）。
///
/// 求值夹具的最简池形：clinit（anewarray + putstatic + invokestatic fill）
/// → fill 直接 ldc 明文 aastore（不走解密器——求值通道吃任何计算链，最小
/// 夹具锚「运行期算什么明文就是什么」的语义）→ 读点方法 `GETSTATIC; idx;
/// AALOAD`。断言面走站点/拒绝桶计数（scan 三段的产物面）。
///
/// 覆盖分工：
/// - 折正例：clinit 求值收敛 → 读点折 `ldc` 明文；
/// - 拒绝面：clinit 求值 Bail（无 mock 的 JDK 调用）→ 站点不动；
/// - 拒绝面：非 clinit 闭包方法对池字段写（运行期改写嫌疑）→ 整组拒；
/// - 拒绝面：读点下标越界（不在求值终态）→ 站点不动。
#include "grunt/pool_array_fold.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::pool_array_fold {
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
        const std::string dump = "test/_jasm_fail_" + name + ".jasm";
        std::FILE*        f    = std::fopen(dump.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail + "（转储 " + dump
                                 + "）");
    }
    return std::move(assembled.value->bytes);
}

model::ResourceContainer makeContainer(
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

/// @brief 池类骨架：`a` 字段 + clinit（建数组 → fill）+ fill（fillBody 注
///        入）+ 读方法（readerBody 注入）+ 可选 rogue（rogueBody 注入）。
std::string poolSource(const std::string& fillBody, const std::string& rogueBody,
                       const std::string& readerBody)
{
    return ".super java/lang/Object\n"
           ".class public super Pool {\n"
           "  .field public static a [Ljava/lang/String;\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_2\n"
           "      anewarray java/lang/String\n"
           "      putstatic Pool.a [Ljava/lang/String;\n"
           "      invokestatic Pool.fill ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "  .method private static fill ()V {\n"
           "    code: {\n"
           "      " + fillBody +
           "    }\n"
           "  }\n"
           "  .method public static getAt ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      " + readerBody +
           "    }\n"
           "  }\n"
           "  .method public static rogue ()V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n" + rogueBody +
           "}\n";
}

const std::string kFillHello =
    "getstatic Pool.a [Ljava/lang/String;\n"
    "      iconst_0\n"
    "      ldc \"hello\"\n"
    "      aastore\n"
    "      return\n";
const std::string kReader0 =
    "getstatic Pool.a [Ljava/lang/String;\n"
    "      iconst_0\n"
    "      aaload\n"
    "      areturn\n";
const std::string kRogueWrite =
    "  .method public static rogueWrite ()V {\n"
    "    code: {\n"
    "      getstatic Pool.a [Ljava/lang/String;\n"
    "      iconst_0\n"
    "      ldc \"tampered\"\n"
    "      aastore\n"
    "      return\n"
    "    }\n"
    "  }\n";

}   // namespace

// ==== 折正例 ==================================================================

TEST(PoolArrayFoldRewrite, FoldsEvaluatedPoolReads)
{
    const std::vector<ca::u8> pool =
        assemble("Pool", poolSource(kFillHello, "", kReader0));
    model::ResourceContainer container = makeContainer({{"Pool", pool}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites.front().plaintext, "hello");
    EXPECT_EQ(scanned.evalBailed, 0u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.failed, 0u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u);
}

// ==== 拒绝面 ==================================================================

TEST(PoolArrayFoldScan, RejectsEvalBailClass)
{
    // fill 里调用无 mock 的 JDK 方法（currentTimeMillis，jsim v1 无接管）
    // → clinit 求值 Bail → 求值面类，读点不入终态表（站点不动）。
    const std::vector<ca::u8> pool = assemble(
        "Pool",
        poolSource(
            "invokestatic java/lang/System.currentTimeMillis ()J\n"
            "      pop2\n" + kFillHello,
            "", kReader0));
    model::ResourceContainer container = makeContainer({{"Pool", pool}});

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.evalBailed, 1u);
}

TEST(PoolArrayFoldScan, RejectsRuntimeWriteSuspect)
{
    // rogueWrite 不在 clinit 调用闭包上，对池字段做 GETSTATIC + AASTORE
    // （运行期改写嫌疑）→ 整组放弃。
    const std::vector<ca::u8> pool =
        assemble("Pool", poolSource(kFillHello, kRogueWrite, kReader0));
    model::ResourceContainer container = makeContainer({{"Pool", pool}});

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.writeSuspect, 1u);
}

TEST(PoolArrayFoldScan, RejectsIndexOutOfRange)
{
    // 读点下标 5 越出求值终态（数组长度 2）→ 站点不动。
    const std::vector<ca::u8> pool = assemble(
        "Pool",
        poolSource(
            kFillHello, "",
            "getstatic Pool.a [Ljava/lang/String;\n"
            "      iconst_5\n"
            "      aaload\n"
            "      areturn\n"));
    model::ResourceContainer container = makeContainer({{"Pool", pool}});

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.idxMissing, 1u);
}

}   // namespace grunt::pool_array_fold
