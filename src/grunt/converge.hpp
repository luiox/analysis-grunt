/// @file        converge.hpp
/// @brief       不动点驱动器：容器字节面指纹 + 收敛循环。
///
/// 链序约束的两维口径（obf-behaviors §驱动器契约的事实源）：`-p` 程序是
/// **每轮** 的 pass 排序（同轮排序保效率，约束见各 pass 分篇），驱动器加
/// **轮维度** ——整条程序重复执行到容器字节面不再变化。跨轮约束（DC 清完
/// 走廊后 goto 退化成冗余、MaterialKeyRestore 解锁第二轮 IndyRestore、
/// ArithFold ×N 收敛、DeadMember 轮次递减）全部被轮维度机械吸收，不再
/// 依赖手工排链。
///
/// 收敛判据是**字节真值**而非 PreservedAnalyses 聚合：runContainerPipeline
/// 不外露聚合 PA（mjbase SPI 面），且字节指纹比 pass 自报更强——任何 pass
/// 漏报的写回都逃不过字节比对。class/unknown 资源按内容 FNV1a-64 入账，
/// manifest 只记存在性（grunt 面无 manifest 写回面）。条目按 uri 排序消
/// unordered_map 遍历序，双跑产物逐条目 CRC 全等的台账口径不受影响。
///
/// 前置：`--converge` 必须禁用时间片（GRUNT_SLICE_SEC=0）。片钟截断会让
/// 整轮空转（墙钟尽后 admit 全拒）→ 指纹不变 → 假收敛；CLI 侧强制置 0。
/// 轮上限防病态震荡（实测样本 A 收敛于 ~9 轮，默认 12）；达上限未收敛
/// 如实报告（converged=false），导出仍是当前最优产物，不算失败。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/core/result.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

namespace grunt::converge {

/// @brief 单条资源指纹：uri + 类型 + 字节面 FNV1a-64（manifest 无字节面记 0）。
struct EntryFp
{
    std::string uri;
    ca::u8      type = 0;
    ca::u64     hash = 0;
};

/// @brief 容器字节面快照：排序后的逐条指纹 + 折叠总哈希。
struct Snapshot
{
    std::vector<EntryFp> entries;
    ca::u64              hash = 0;
};

/// @brief FNV-1a 64 位。
inline ca::u64 fnv1a64(const ca::u8* data, ca::usize n, ca::u64 seed)
{
    ca::u64 h = seed;
    for (ca::usize i = 0; i < n; ++i) {
        h ^= static_cast<ca::u64>(data[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

/// @brief 容器快照：visitResources 逐条入账（class/unknown 按内容哈希，
///        manifest 只记类型占位），按 uri 排序消遍历序，再折叠成总哈希。
inline Snapshot snapshot(const mjbase::model::IResourceContainer& container)
{
    Snapshot snap;
    container.visitResources([&snap](std::string_view uri, const mjbase::model::IResource& res) {
        EntryFp e;
        e.uri.assign(uri);
        e.type = static_cast<ca::u8>(res.getType());
        e.hash = 0;
        if (res.getType() == mjbase::model::ResourceType::Class) {
            const auto* cls = ca::core::tag_cast::cast<const mjbase::model::ClassResource>(&res);
            if (cls != nullptr) e.hash = fnv1a64(cls->get().data(), cls->get().size(), 0xcbf29ce484222325ULL);
        }
        else if (res.getType() == mjbase::model::ResourceType::Unknown) {
            const auto* raw = ca::core::tag_cast::cast<const mjbase::model::UnknownResource>(&res);
            if (raw != nullptr) e.hash = fnv1a64(raw->get().data(), raw->get().size(), 0xcbf29ce484222325ULL);
        }
        snap.entries.push_back(std::move(e));
    });
    std::sort(snap.entries.begin(), snap.entries.end(),
              [](const EntryFp& a, const EntryFp& b) { return a.uri < b.uri; });
    ca::u64 h = 0xcbf29ce484222325ULL;
    for (const EntryFp& e : snap.entries) {
        h = fnv1a64(reinterpret_cast<const ca::u8*>(e.uri.data()), e.uri.size(), h);
        h = fnv1a64(&e.type, 1, h);
        h = fnv1a64(reinterpret_cast<const ca::u8*>(&e.hash), sizeof(e.hash), h);
    }
    snap.hash = h;
    return snap;
}

/// @brief 两快照的条目差异数（uri 新增/删除、类型或内容哈希变化）。
inline ca::usize diffCount(const Snapshot& before, const Snapshot& after)
{
    ca::usize                    n      = 0;
    ca::usize                    i      = 0;
    ca::usize                    j      = 0;
    const std::vector<EntryFp>&  a      = before.entries;
    const std::vector<EntryFp>&  b      = after.entries;
    while (i < a.size() && j < b.size()) {
        if (a[i].uri < b[j].uri) {
            ++n;   // 删除
            ++i;
        }
        else if (b[j].uri < a[i].uri) {
            ++n;   // 新增
            ++j;
        }
        else {
            if (a[i].type != b[j].type || a[i].hash != b[j].hash) ++n;
            ++i;
            ++j;
        }
    }
    n += (a.size() - i) + (b.size() - j);
    return n;
}

/// @brief 轮执行器：跑一遍完整 `-p` 程序（每轮 rebuild 管线 + 重建层次图，
///        工厂 lambda 每次 build 重新 create，builder 可复用）。Err = 轮内
///        管线错误，收敛循环立即中止并透传。
using RoundRunner = std::function<ca::core::Result<void, std::string>()>;

/// @brief 收敛结果。
struct ConvergeResult
{
    ca::usize               rounds    = 0;    ///< 实际执行的轮数（含未变的那轮）
    bool                    converged = false;
    std::vector<ca::usize>  perRoundChanged;  ///< 每轮条目差异数（与轮前快照比）
    std::string             error;            ///< 轮执行器错误（converged=false）
};

/// @brief 收敛循环：整条程序重复执行到容器字节面不再变化。
///
/// 每轮：轮前快照 → runner → 轮后快照 → 差异计数。差异 0 = 收敛（该轮是
/// "确认轮"，rounds 含它）；达 maxRounds 仍未收敛如实返回。maxRounds==0
/// 不执行任何轮。
inline ConvergeResult runToConvergence(const RoundRunner&                      round,
                                       const mjbase::model::IResourceContainer& container,
                                       ca::usize                               maxRounds,
                                       std::FILE*                              traceOut)
{
    ConvergeResult out;
    for (ca::usize r = 1; r <= maxRounds; ++r) {
        const Snapshot before = snapshot(container);
        auto           rr     = round();
        if (rr.is_err()) {
            out.error = rr.unwrap_err();
            if (traceOut != nullptr) {
                std::fprintf(traceOut, "[grunt.converge] round %zu: 管线错误：%s\n", r,
                             out.error.c_str());
            }
            return out;
        }
        const ca::usize changed = diffCount(before, snapshot(container));
        out.rounds              = r;
        out.perRoundChanged.push_back(changed);
        if (traceOut != nullptr) {
            std::fprintf(traceOut, "[grunt.converge] round %zu: changed=%zu\n", r, changed);
        }
        if (changed == 0) {
            out.converged = true;
            return out;
        }
    }
    if (traceOut != nullptr && !out.perRoundChanged.empty()) {
        std::fprintf(traceOut, "[grunt.converge] 未收敛（达轮上限 %zu）\n", maxRounds);
    }
    return out;
}

}   // namespace grunt::converge
