/// @file        proof_toolbox.hpp
/// @brief       PROOF 工具箱：跨残留共享的往返自检与资源核对。
///
/// CONVENTION §7：跨残留复用只允许发生在工具箱。当前收口两组：
/// - `roundTripSafe`，改写的前置检查：在**未修改**的独立会话上先跑一遍
///   promote → lower，把本来就往返不了的方法排除在改写面之外；
/// - 资源核对原语（`countResources` / `resourcesShrunk`），资源级验收检查：
///   资源数不增 + 删除清单逐条不在场。资源**删除**动词在 rewrite_toolbox
///   （proof 只判不写，§3.2-5），核对在 proof 侧。
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"

namespace grunt::proof_tools {

/// @brief 判这些方法在未修改状态下能否走完一次 promote → lower。
///
/// 为什么需要这项检查：lower 不是 promote 的恒等逆。样本里存在能通过校验、但
/// 当前 lower 无法重新编码的方法（指令/属性重定位边界）。这类方法一旦被改写，
/// lower 失败会把整个类拖下水，而失败与改动无关，却表现为改写有 bug。
/// 先在不改动的会话上往返一遍，剩下的 lower 失败才是真正的改写缺陷。
///
/// 用独立会话做探针：探针会 promote/lower，不能污染真正要改的那一份。
inline std::unordered_set<ca::usize> roundTripSafe(const std::vector<ca::u8>&    bytes,
                                                   const std::vector<ca::usize>& methods)
{
    std::unordered_set<ca::usize> safe;
    // 关掉自检时全部放行：保留这条开关是为了能量出自检本身值多少时间，
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

/// @brief PROOF 共同形状：类数不增 + 全部脏类可重新解析。
///
/// 各残留的 proof 段本体都是这两条（CONVENTION §0 的全局不变量 + 写回的
/// 字节必须仍是合法 classfile），收口成一份，不许逐残留重抄。
struct ClassIntactResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

inline ClassIntactResult classesIntact(const mjbase::model::ResourceContainer& container,
                                       const std::vector<std::string>& uris,
                                       ca::usize classesBefore)
{
    // proof 是效力检查（不变量复核），不许被 pass 级时间片截短，挂起期间
    // forEachClass 全量放行，否则过期时钟会把全部脏类误报成 proof failed。
    pass_clock::Suspend    suspend;
    ClassIntactResult result;
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    for (const std::string& uri : uris) {
        bool found = false;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (found || entry.uri != uri) return;
            found = openSession(*entry.bytes) != nullptr;
        });
        if (!found) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

// ============================================================================
// ==== 资源核对原语（资源级验收检查：资源删除面 proof 段共用）==================
// ============================================================================

/// @brief 容器资源条目数（类与非类资源都算；jar 的资源面核对口径）。
///
/// 计数挂起时间片：资源数检查必须看到全量条目，不许被超墙截短（countClasses
/// 同一检查，资源删除与类改写在同一 pass 内交错时，proof 只许在挂起期盘点）。
inline ca::usize countResources(const mjbase::model::ResourceContainer& container)
{
    ca::usize           n = 0;
    pass_clock::Suspend suspend;
    container.visitResources([&](std::string_view, const mjbase::model::IResource&) { ++n; });
    return n;
}

/// @brief 资源核对结果：违例统计（"resource-count" / 未清掉的条目 URI）。
struct ResourceShrunkResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief 资源级验收检查：资源数**只减不增**，且删除清单逐条不在场。
///
/// 资源动词的核对口径（metadata_strip 首用）：资源删除是容器条目的结构性
/// 变化，类数检查看不见它，单独一道检查，与类数检查并列进 proof。数不增（只减）
/// 防误加资源；清单逐条核对把 Stats 统计与容器实况钉死（虚报一条删减即
/// FAILED）。
inline ResourceShrunkResult resourcesShrunk(const mjbase::model::ResourceContainer& container,
                                            const std::vector<std::string>& removedUris,
                                            ca::usize                       resourcesBefore)
{
    // 核对是效力检查，挂起期间盘点全量条目（countResources 同一检查）。
    pass_clock::Suspend     suspend;
    ResourceShrunkResult    result;
    const ca::usize         after = countResources(container);
    if (after > resourcesBefore || after + removedUris.size() != resourcesBefore) {
        result.ok = false;
        result.failed.emplace_back("resource-count");
    }
    for (const std::string& uri : removedUris) {
        if (container.hasResource(uri)) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

}   // namespace grunt::proof_tools
