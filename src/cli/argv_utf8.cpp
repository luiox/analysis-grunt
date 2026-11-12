/// @file        argv_utf8.cpp
/// @brief       CLI argv 的编码根修实现（见 argv_utf8.hpp 头注释）。
#include "cli/argv_utf8.hpp"

#include "libca/core/datatype.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace grunt::cli {

bool wideArgToUtf8(const wchar_t* arg, std::string& out)
{
    if (arg == nullptr) return false;
    // WC_ERR_INVALID_CHARS：失配代理对直接判败，不做 U+FFFD 静默替换，
    // 替换后的路径必然 NotFound，比显式报错更难排查。
    const int need = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg, -1,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::string buf(static_cast<ca::usize>(need - 1), '\0');   ///< 去掉结尾 NUL
    const int got = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, arg, -1,
                                        buf.data(), need, nullptr, nullptr);
    if (got != need) return false;
    out = std::move(buf);
    return true;
}

bool utf8Args(int argc, const wchar_t* const* argv, std::vector<std::string>& out)
{
    out.clear();
    out.reserve(static_cast<ca::usize>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        std::string item;
        if (!wideArgToUtf8(argv[i], item)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

}   // namespace grunt::cli

#else

#include <iterator>

namespace grunt::cli {

void utf8Args(int argc, const char* const* argv, std::vector<std::string>& out)
{
    out.clear();
    out.reserve(static_cast<ca::usize>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) {
        out.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
}

}   // namespace grunt::cli

#endif
