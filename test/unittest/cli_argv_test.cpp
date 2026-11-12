/// @file        cli_argv_test.cpp
/// @brief       CLI argv 编码根修的判据测试：宽字符 → UTF-8 往返与拒绝面。
#include "cli/argv_utf8.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

#if defined(_WIN32)

using grunt::cli::utf8Args;
using grunt::cli::wideArgToUtf8;

TEST(CliArgv, AsciiPassthrough)
{
    std::string out;
    ASSERT_TRUE(wideArgToUtf8(L"-i", out));
    EXPECT_EQ(out, "-i");
}

TEST(CliArgv, ChineseJarNameExactUtf8)
{
    // 语料 D 类代表 jar 名（GBK ANSI argv 的错配面）：虚拟化混淆样本。
    std::string out;
    ASSERT_TRUE(wideArgToUtf8(L"虚拟化混淆样本-target-obf-z6-obfuscated", out));
    EXPECT_EQ(out,
              "\xE8\x99\x9A\xE6\x8B\x9F\xE5\x8C\x96\xE6\xB7\xB7\xE6\xB7\x86\xE6\xA0\xB7\xE6\x9C\xAC"
              "-target-obf-z6-obfuscated");
}

TEST(CliArgv, SurrogatePairEmoji)
{
    // U+1F600（代理对 D83D DE00）→ 4 字节 UTF-8。
    std::string out;
    ASSERT_TRUE(wideArgToUtf8(L"a\U0001F600b", out));
    EXPECT_EQ(out, "a\xF0\x9F\x98\x80" "b");
}

TEST(CliArgv, UnpairedSurrogateRejected)
{
    // 失配代理对（高代理无后继）：拒绝，不静默替换。
    std::string out;
    EXPECT_FALSE(wideArgToUtf8(L"bad\xD800path", out));
    EXPECT_FALSE(wideArgToUtf8(L"bad\xDC00path", out));
}

TEST(CliArgv, NullArgRejected)
{
    std::string out;
    EXPECT_FALSE(wideArgToUtf8(nullptr, out));
}

TEST(CliArgv, ArgsVectorRoundTrip)
{
    const wchar_t* raw[] = {L"prog", L"-i", L"虚拟化混淆样本.jar", L"-o", L"out.jar"};
    std::vector<std::string> args;
    ASSERT_TRUE(utf8Args(5, raw, args));
    ASSERT_EQ(args.size(), 5u);
    EXPECT_EQ(args[0], "prog");
    EXPECT_EQ(args[2], "\xE8\x99\x9A\xE6\x8B\x9F\xE5\x8C\x96\xE6\xB7\xB7\xE6\xB7\x86\xE6\xA0\xB7\xE6\x9C\xAC.jar");
    EXPECT_EQ(args[4], "out.jar");
}

TEST(CliArgv, ArgsVectorRejectsOnBadItem)
{
    const wchar_t* raw[] = {L"prog", L"-i", L"bad\xD800.jar"};
    std::vector<std::string> args;
    EXPECT_FALSE(utf8Args(3, raw, args));
}

#else

TEST(CliArgv, PosixPassthrough)
{
    const char* raw[] = {"prog", "-i", "x.jar"};
    std::vector<std::string> args;
    grunt::cli::utf8Args(3, raw, args);
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[2], "x.jar");
}

#endif

}   // namespace
