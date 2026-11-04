/// @file        argv_utf8.hpp
/// @brief       CLI argv 的编码根修：Windows 宽字符 argv → UTF-8。
///
/// 背景（语料 D 类 19 罐）：Windows CRT 的窄字符 `main` 拿到的 argv 是按 ANSI
/// 代码页（GBK）转出来的字节，Git Bash / 语料驱动传 UTF-8 路径时非 ASCII 罐名
/// 必然错配——导入口拿错路径报 NotFound。根修是入口直接取宽字符 argv（wmain）
/// 再转 UTF-8（仓内字符串面统一 UTF-8），POSIX 侧 argv 本就是 UTF-8，直通。
#pragma once

#include <string>
#include <vector>

namespace grunt::cli {

#if defined(_WIN32)

/// @brief 单条宽字符参数 → UTF-8。
/// @return false = 含失配代理对等不可编码序列（调用方报错退出，不静默替换）。
bool wideArgToUtf8(const wchar_t* arg, std::string& out);

/// @brief 宽字符 argv 整体转 UTF-8（不含 argv[0] 的过滤由调用方决定）。
/// @return 转换全部成功为真；假时 out 内容不完整，调用方应放弃。
bool utf8Args(int argc, const wchar_t* const* argv, std::vector<std::string>& out);

#else

/// @brief POSIX 直通：argv 本就是 UTF-8 字节，拷贝进 vector。
void utf8Args(int argc, const char* const* argv, std::vector<std::string>& out);

#endif

}   // namespace grunt::cli
