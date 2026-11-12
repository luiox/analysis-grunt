/// @file        iter_budget.hpp
/// @brief       单类迭代预算：scan / rewrite 的逐类工作量上限与超限保守跳过。
///
/// 语料实测（快照 63c4f33，2026-11）：单个 pass 在大 jar（3000+ 类）上耗时
/// 15~45s，链式 13 pass 远超单 jar 120s 上限；尾部是个别类的迭代工作量失控
/// 被全部 jar 连带失败。预算按类计数：某类的迭代操作数超限即保守跳过该类剩余迭代
/// （站点不产出 / 站点不尝试），类保持原样，stderr 点名 + 计数进各段结果面。
/// 预算值取语料实测类级工作量的 20 倍以上余量，正常 jar 不触限。
#pragma once

#include <cstdio>
#include <cstddef>
#include <string>

#include "libca/core/datatype.hpp"

namespace grunt::iter_budget {

/// @brief 一个类在一段（scan 或 rewrite）里的迭代预算。
///
/// `limit` 是操作数口径（1 单位 = 多少工作由各残留自定义：scan 记扫描的
/// 指令位置数，rewrite 记站点复核扫过的指令数）。`limit == 0` 表示不设防。
/// 超限后 `tick` 恒假：调用方就此打住该类的剩余迭代。
struct ClassBudget
{
    ca::usize limit    = 0;   ///< 操作数上限；0 = 不设防
    ca::usize used     = 0;   ///< 已消耗操作数
    bool      exhausted = false;

    /// @brief 计数 @param units 本步消耗的操作数。@return 预算内才真。
    bool tick(ca::usize units = 1)
    {
        if (limit == 0) return true;
        if (exhausted) return false;
        used += units;
        if (used > limit) exhausted = true;
        return !exhausted;
    }
};

/// @brief 点名上限：超限类逐个 stderr 点名，超过此数只报计数（防病 jar 刷屏）。
constexpr ca::usize kMaxNamedClasses = 20;

/// @brief 超限点名（stderr，不受 GRUNT_TRACE 检查控，语料回归按 .err 计数）。
///
/// @param pass  pass 名（如 "grunt.NumberFold"）
/// @param seg   段名（"scan" / "rewrite"）
/// @param uri   超限类
/// @param budget 计数器（打印上限与已耗量）
/// @param named  点名计数器（跨类传同一变量，达到上限后只报不点）
inline void reportExhausted(const char* pass, const char* seg, const std::string& uri,
                            const ClassBudget& budget, ca::usize& named)
{
    ++named;
    if (named > kMaxNamedClasses) return;
    std::fprintf(stderr, "[%s] %s 预算超限：%s（%zu > %zu），保守跳过该类剩余迭代\n",
                 pass, seg, uri.c_str(), budget.used, budget.limit);
}

}   // namespace grunt::iter_budget
