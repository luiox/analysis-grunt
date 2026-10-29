/// @file        proof_toolbox.hpp
/// @brief       PROOF 工具箱：跨残留共享的往返自检。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口的是 `roundTripSafe`
/// ——改写的前置门：在**未修改**的独立会话上先跑一遍 promote → lower，
/// 把「本来就往返不了」的方法排除在改写面之外。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"

namespace grunt::proof_tools {

/// @brief 判「这些方法在未修改状态下能否走完一次 promote → lower」。
///
/// 为什么需要这道门：lower 不是 promote 的恒等逆。样本里存在能通过校验、但
/// 当前 lower 无法重新编码的方法（指令/属性重定位边界）。这类方法一旦被改写，
/// lower 失败会把整个类拖下水——而失败与改动无关，却表现为「改写有 bug」。
/// 先在不改动的会话上往返一遍，剩下的 lower 失败才是真正的改写缺陷。
///
/// 用独立会话做探针：探针会 promote/lower，不能污染真正要改的那一份。
inline std::unordered_set<ca::usize> roundTripSafe(const std::vector<ca::u8>&    bytes,
                                                   const std::vector<ca::usize>& methods)
{
    std::unordered_set<ca::usize> safe;
    // 关掉自检时全部放行：保留这条开关是为了能量出「自检本身值多少时间」，
    // 以及在确认某个 lower 失败与改写无关之后做对照。
    if (std::getenv("GRUNT_NO_ROUNDTRIP") != nullptr) {
        for (const ca::usize mi : methods) safe.insert(mi);
        return safe;
    }
    auto probe = openSession(bytes);
    if (!probe) return safe;
    for (const ca::usize mi : methods) {
        if (!jbc::ir::hasCode(*probe, mi)) continue;
        if (jbc::ir::promoteMethod(*probe, mi).is_err()) continue;
        if (jbc::ir::lowerMethod(*probe, mi).is_ok()) {
            safe.insert(mi);
        } else if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
            std::fprintf(stderr, "[roundtrip-fail] %s method#%zu\n",
                         std::string(probe->backing().thisName()).c_str(), mi);
        }
    }
    return safe;
}

}   // namespace grunt::proof_tools
