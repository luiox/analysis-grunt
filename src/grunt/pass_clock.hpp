/// @file        pass_clock.hpp
/// @brief       pass 级 wall-clock 时间片：单 pass 在单罐上的时间墙与超时保守跳过。
///
/// 与 iter_budget（单类迭代预算）互补不重叠：iter_budget 按「操作数」封单类
/// 迭代尾——个别类工作量失控时不许全罐陪葬；本文件按「wall-clock」封整段
/// 时间墙——链式 13 pass 的广度成本（大罐单 pass 15~45s，Grunteon /
/// UNO-indy / zenith 实测）在罐级超时（corpus-regress JAR_TIMEOUT=120s）面前
/// 只能整罐 FAIL run；时间片让超限 pass 对余下类保守止步：已改写的类保留，
/// 未到的类整类不动，罐 salvage 出部分产物而不是全罐陪葬。
///
/// 接线三单点：FailClosedPass::run 装配（全 pass 单点收口）、forEachClass
/// 执行（类下发单点门）、countClasses 挂起（proof 类数门不许被截短）。
/// 上限从 GRUNT_SLICE_SEC 读（秒；0 = 不设防）；默认 60s——语料实测正常
/// 大罐单 pass 15~45s，60s 不触正常罐；salvage 场景由 harness 显式调小。
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "libca/core/datatype.hpp"

namespace grunt::pass_clock {

/// @brief 一个 pass 在一个罐上的时间片记账器（管线串行，无并发）。
class SegmentClock
{
public:
    /// @brief 记账开始。@param pass pass 名（stderr 点名用）
    ///                     @param limitSec wall-clock 上限秒；0 = 不设防
    void begin(const char* pass, ca::usize limitSec)
    {
        pass_   = pass;
        limit_  = limitSec;
        start_  = std::chrono::steady_clock::now();
    }

    /// @brief 到墙没：超上限即过期（测试钩子 @c forceExpire 强制过期）。
    bool expired() const
    {
        if (forced_) return true;
        if (limit_ == 0) return false;
        const auto elapse = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration_cast<std::chrono::seconds>(elapse).count() >=
               static_cast<long long>(limit_);
    }

    /// @brief 类下发门：未过期放行；过期后拒绝（首停类点名一次，逐类计数）。
    ///         proof 类数门（挂起期）恒放行。
    bool admit(std::string_view uri)
    {
        if (!expired() || suspended > 0) return true;
        if (skipped_ == 0) {
            std::fprintf(stderr,
                         "[%s] pass 时间片超限：%.*s 前止步（上限 %zus），余下类保守跳过\n",
                         pass_, static_cast<int>(uri.size()), uri.data(), limit_);
        }
        ++skipped_;
        return false;
    }

    /// @brief 测试钩子：不依赖睡眠的确定性过期。
    void forceExpire() { forced_ = true; }

    /// @brief 已被保守跳过的类数。
    ca::usize skipped() const { return skipped_; }

    /// @brief 挂起深度（countClasses 的 proof 类数门用）。
    ca::usize suspended = 0;

private:
    const char* pass_                                              = nullptr;
    ca::usize   limit_                                             = 0;
    ca::usize   skipped_                                           = 0;
    bool        forced_                                            = false;
    std::chrono::steady_clock::time_point start_                   = {};
};

/// @brief 活动时间片（nullptr = 无 pass 装配，forEachClass 不设门）。
inline SegmentClock* gActiveClock = nullptr;

/// @brief GRUNT_SLICE_SEC（秒；0 = 不设防）。读一次进程级固定。
inline ca::usize sliceLimitFromEnv()
{
    static const ca::usize limit = [] {
        const char* raw = std::getenv("GRUNT_SLICE_SEC");
        if (raw == nullptr || *raw == '\0') return ca::usize{60};
        const long parsed = std::strtol(raw, nullptr, 10);
        return parsed > 0 ? static_cast<ca::usize>(parsed) : ca::usize{0};
    }();
    return limit;
}

/// @brief pass 装配 RAII：进 pass 计时并接管活动片，出 pass 报跳过账。
class PassScope
{
public:
    /// @brief 正式装配：上限取 GRUNT_SLICE_SEC。
    explicit PassScope(const char* pass) : PassScope(pass, sliceLimitFromEnv()) {}

    /// @brief 测试装配：显式给上限（0 = 不设防）。
    PassScope(const char* pass, ca::usize limitSec)
    {
        passName_ = pass;
        clock_.begin(pass, limitSec);
        prev_        = gActiveClock;
        gActiveClock = &clock_;
    }

    ~PassScope()
    {
        gActiveClock = prev_;
        if (clock_.skipped() > 0) {
            std::fprintf(stderr, "[%s] 时间片止步：共保守跳过 %zu 类\n",
                         passName_, clock_.skipped());
        }
    }

    PassScope(const PassScope&)            = delete;
    PassScope& operator=(const PassScope&) = delete;

    /// @brief 供断言/trace 面读账与测试钩子（forceExpire）。
    SegmentClock& clock() { return clock_; }

private:
    const char*  passName_ = nullptr;   ///< pass 名（报账行用）
    SegmentClock* prev_    = nullptr;   ///< 嵌套装配恢复面（正常链不嵌套）
    SegmentClock  clock_;
};

/// @brief 挂起 RAII：proof 类数门（countClasses）计数期间不许被时间片截短。
class Suspend
{
public:
    Suspend()
    {
        if (gActiveClock != nullptr) ++gActiveClock->suspended;
    }
    ~Suspend()
    {
        if (gActiveClock != nullptr) --gActiveClock->suspended;
    }
    Suspend(const Suspend&)            = delete;
    Suspend& operator=(const Suspend&) = delete;
};

}   // namespace grunt::pass_clock
