/// @file        dead_code.cpp
/// @brief       残留 `dead_code` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流与定位
///
/// `Site{uri, methodIndex, victimCount}` → `DirtySet{uris, methods, insns,
/// attempted}`。SCAN 独立成段、管线里放最后：不依赖其他残留的脏标记，把
/// 全部上游改写产生的死块一次收走。可达性 BFS 需要 `InsnId → order 下标`
/// 映射，用 `mjbase::asm_cfg::InsnIdHash`（mjbase 公开头，本仓不自带哈希）。
///
/// ## 异常表收敛（擦除之前）
///
/// `rewriteMethod` 先扫一遍 `cu->exc`：
///
/// - 区间内无存活指令 → 丢表项，start/end/handler 三枚标签记入孤儿集；
/// - start 不在存活指令上 → `bindLabel` 搬移旧 Boundary 标签（句柄身份
///   不变，表项引用不动）；
/// - end 不在存活指令上 → 吸附到其后第一条存活指令，找不到就 `bindLabel`
///   无参末哨兵形态。
///
/// 任一端点解析失败（`sBgn == kNo || sEnd == kNo || sEnd <= sBgn`）整方法
/// 放弃，不做半截改写。`excChanged` 但 `victims` 为空时也写回 `cu->exc`
/// ，异常表本身就是改动。孤儿标签销毁前清点：句柄无效（含 kNoLabel）
/// 跳过、孤儿集内重复句柄只销毁一次、被存活表项共享的保留，按 `cu->exc`
/// 向量序线性清点，无集合遍历，双跑产物不引入遍历序依赖。受害者内部互指
/// （死块内 goto）的 ControlFlow 标签不保护锚点；若收敛逻辑漏处置某枚
/// Boundary 标签，`eraseBatch` 按锚定保护契约返回 Err，本 pass 保守放弃
/// ，fail-closed 兜底即断言。
///
/// 端点吸附必须**搬移**旧标签而不是造新标签：旧 Boundary 标签仍锚在被擦
/// 指令上时，`eraseBatch` 的锚定保护会整批拒绝，改写整体不可达（历史教训，
/// 单测 kEndAnchorDeadSource 族存证）。
///
/// ## JSR/RET
///
/// `JSR` 的后继 = 目标 + 顺序下一条（返回地址压栈）；`RET` 无静态后继
/// （目标在局部变量里），按终止符处理，保守不越。Strict 帧重算对 jsr/ret
/// 抛异常，`mutateMethodWithFrames` 收敛为 false（整方法放弃）。
///
/// ## 与 HotSpot 线性 walk 的关系
///
/// HotSpot 校验器对死代码并非跳过：线性 walk 走遍全部帧，且每个分支
/// 目标 / 异常 handler 都必须有帧。所以死区必须在导出前物理消失，而不是
/// 留 NOP 或留帧，本残留擦掉死区，帧由 `rewireFramesAfterLower` 的 Strict
/// 求解从代码真值整体重算，不存在锚在被擦指令上的孤儿帧。
#include "grunt/dead_code.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "mjbase/asm/cfg/basic_block.hpp"

namespace grunt::dead_code {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

/// @brief 单方法可达性 BFS（异常表感知）：返回逐指令可达标记（order 序）。
///
/// 入口 = 首条指令。异常表是隐藏控制流边：范围内任一可达指令都可能抛出
/// 并进入 handler，故 handler 以区间与可达集相交为根，迭代到不动点
/// （handler 自己落在别的区间里时继续传导）。
std::vector<ca::u8> reachability(const ir::CodeUnit& cu)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    std::vector<ca::u8>            reach(order.size(), 0);
    if (order.empty()) return reach;

    constexpr ca::usize kNo = static_cast<ca::usize>(-1);

    std::unordered_map<ir::InsnId, ca::usize, mjbase::asm_cfg::InsnIdHash> posOf;
    posOf.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) posOf.emplace(order[i], i);

    const auto follow = [&](const ir::LabelId& label, std::vector<ca::usize>& succ) {
        auto resolved = cu.dom.resolveLabel(label);
        if (resolved.is_err()) return;
        const ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) return;
        const auto it = posOf.find(t.insn);
        if (it != posOf.end()) succ.push_back(it->second);
    };

    std::vector<ca::usize> work;
    const auto mark = [&](ca::usize pos) {
        if (pos < order.size() && reach[pos] == 0) {
            reach[pos] = 1;
            work.push_back(pos);
        }
    };
    const auto sweep = [&]() {
        while (!work.empty()) {
            const ca::usize  pos = work.back();
            work.pop_back();
            const ir::Insn* in = cu.dom.peek(order[pos]);
            if (in == nullptr) continue;
            std::vector<ca::usize> succ;
            switch (in->op) {
            case Op::GOTO:
            case Op::GOTO_W: follow(in->target, succ); break;
            case Op::IFEQ: case Op::IFNE: case Op::IFLT:
            case Op::IFGE: case Op::IFGT: case Op::IFLE:
            case Op::IF_ICMPEQ: case Op::IF_ICMPNE: case Op::IF_ICMPLT:
            case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE:
            case Op::IFNULL: case Op::IFNONNULL:
            case Op::IF_ACMPEQ: case Op::IF_ACMPNE:
            case Op::JSR: case Op::JSR_W:
                follow(in->target, succ);
                succ.push_back(pos + 1);
                break;
            case Op::TABLESWITCH:
            case Op::LOOKUPSWITCH:
                for (const ir::LabelId& label : in->targets) follow(label, succ);
                follow(in->target, succ);
                break;
            case Op::IRETURN: case Op::LRETURN: case Op::FRETURN:
            case Op::DRETURN: case Op::ARETURN: case Op::RETURN:
            case Op::ATHROW: case Op::RET:
                break;
            default:
                succ.push_back(pos + 1);
                break;
            }
            for (const ca::usize sp : succ) mark(sp);
        }
    };

    const auto labelPos = [&](const ir::LabelId& label, bool allowEnd) {
        auto resolved = cu.dom.resolveLabel(label);
        if (resolved.is_err()) return kNo;
        const ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) return allowEnd ? order.size() : kNo;
        const auto it = posOf.find(t.insn);
        return it != posOf.end() ? it->second : kNo;
    };

    mark(0);
    sweep();
    for (;;) {
        bool any = false;
        for (const ir::CodeUnit::Exc& e : cu.exc) {
            const ca::usize h = labelPos(e.handler, false);
            if (h == kNo || reach[h] != 0) continue;
            const ca::usize sBgn = labelPos(e.start, false);
            const ca::usize sEnd = labelPos(e.end, true);
            if (sBgn == kNo || sEnd == kNo) continue;
            for (ca::usize i = sBgn; i < sEnd; ++i) {
                if (reach[i] != 0) {
                    mark(h);
                    any = true;
                    break;
                }
            }
        }
        if (!any) break;
        sweep();
    }
    return reach;
}

/// @brief 单方法 REWRITE。@return 清掉的指令数（0 = 没动）。
///
/// cu 是 mutateMethodWithFrames 提升好的当前 unit：promote 只发生一次，
/// DOM 改写、prepare、lower 都落在同一 unit 上（再 promote 即弃改写）。
/// 受害者一律 eraseBatch，SMT 由 rewireFramesAfterLower 严格重算，无需
/// 为帧锚保活任何指令。异常表不再整体跳过：可达性含异常边（见
/// reachability），全死区间删表项，端点锚在死指令上时向前吸附到存活
/// 指令（区间吞并被删段，与 frame_rewire 的 LNT/LVT 吸附规则一致），
/// 尾部全死落到方法末尾哨兵。
///
/// 端点吸附用 bindLabel 搬移旧 Boundary 标签（句柄身份不变，表项引用
/// 不动）；createLabel 换新引用而不销毁旧标签的形态不可用，旧标签仍
/// 锚在被擦指令上，eraseBatch 按批内成员被存活 Boundary 标签锚定 →
/// Err 且整批不删契约拒绝，方法整体 fail-closed（2026-11-12 修复的
/// 根因）。删表项的孤儿端点/handler 标签在销毁前按句柄身份清点存活
/// 表项引用。
ca::usize rewriteMethod(ir::CodeUnit* cu)
{
    if (cu == nullptr) return 0;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    if (order.empty()) return 0;
    const std::vector<ca::u8> reach = reachability(*cu);

    constexpr ca::usize kNo = static_cast<ca::usize>(-1);
    std::unordered_map<ir::InsnId, ca::usize, mjbase::asm_cfg::InsnIdHash> posOf;
    posOf.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) posOf.emplace(order[i], i);
    const auto labelPos = [&](const ir::LabelId& label, bool allowEnd) {
        auto resolved = cu->dom.resolveLabel(label);
        if (resolved.is_err()) return kNo;
        const ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) return allowEnd ? order.size() : kNo;
        const auto it = posOf.find(t.insn);
        return it != posOf.end() ? it->second : kNo;
    };

    // 异常表收敛（先于擦除：Boundary 标签保护锚点，吸附必须在擦除前落
    // 定）。遍历与处置均按 cu->exc 向量序、孤儿标签线性清点，无集合
    // 遍历，双跑产物确定性不引入遍历序依赖。
    std::vector<ir::CodeUnit::Exc> liveExc;
    std::vector<ir::LabelId>       orphanLabels;   // 删表项的端点/handler 标签
    bool                           excChanged = false;
    for (ir::CodeUnit::Exc e : cu->exc) {
        const ca::usize sBgn = labelPos(e.start, false);
        const ca::usize sEnd = labelPos(e.end, true);
        if (sBgn == kNo || sEnd == kNo || sEnd <= sBgn) return 0;   // 异形，保守放弃
        ca::usize firstLive = kNo;
        for (ca::usize i = sBgn; i < sEnd; ++i) {
            if (reach[i] != 0) {
                firstLive = i;
                break;
            }
        }
        if (firstLive == kNo) {
            excChanged = true;   // 区间全死：异常无从抛出，表项删除
            orphanLabels.push_back(e.start);
            orphanLabels.push_back(e.end);
            orphanLabels.push_back(e.handler);
            continue;
        }
        if (firstLive != sBgn) {
            // start 锚在死指令上：旧 Boundary 标签搬移到区间内第一条存活
            // 指令，表项引用不动。
            if (cu->dom.bindLabel(e.start, order[firstLive]).is_err()) return 0;
            excChanged = true;
        }
        if (sEnd < order.size() && reach[sEnd] == 0) {
            ca::usize clamp = kNo;
            for (ca::usize i = sEnd; i < order.size(); ++i) {
                if (reach[i] != 0) {
                    clamp = i;
                    break;
                }
            }
            // end 锚在死指令上：搬移到其后第一条存活指令；尾部全死落方法
            // 末哨兵（bindLabel 无参形态）。
            const ir::InsnId anchor = clamp == kNo ? ir::kNoInsn : order[clamp];
            if (cu->dom.bindLabel(e.end, anchor).is_err()) return 0;
            excChanged = true;
        }
        liveExc.push_back(e);
    }

    // 孤儿标签销毁：句柄无效（含 kNoLabel）跳过，无效标签不锚定任何
    // 指令，无害；同一句柄在孤儿集里重复出现或被存活表项共享时保留
    // （线性清点，序确定）。
    for (ca::usize oi = 0; oi < orphanLabels.size(); ++oi) {
        const ir::LabelId& lbl = orphanLabels[oi];
        bool               keep = !cu->dom.validLabel(lbl);
        for (ca::usize oj = 0; !keep && oj < oi; ++oj) {
            keep = orphanLabels[oj] == lbl;   // 孤儿集内重复句柄只销毁一次
        }
        for (ca::usize ki = 0; !keep && ki < liveExc.size(); ++ki) {
            const ir::CodeUnit::Exc& kept = liveExc[ki];
            keep = kept.start == lbl || kept.end == lbl || kept.handler == lbl;
        }
        if (keep) continue;
        if (cu->dom.destroyLabel(lbl).is_err()) return 0;
    }

    std::vector<ir::InsnId> victims;
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (reach[i] == 0) victims.push_back(order[i]);
    }
    if (victims.empty() && !excChanged) return 0;
    if (excChanged) cu->exc = std::move(liveExc);
    // 断言注记（不另设遍历）：被存活 Boundary 标签锚定或被存活指令跳转
    // 引用的指令必然可达（异常边已入 reachability），不会进受害者集合；
    // 受害者内部互指（死块内 goto 等）的 ControlFlow 标签不保护锚点，
    // 按 eraseBatch 契约随批消亡。若收敛逻辑漏处置某枚 Boundary 标签，
    // eraseBatch 返回 Err，本函数保守放弃，fail-closed 兜底即断言。
    if (!victims.empty() && cu->dom.eraseBatch(victims).is_err()) return 0;
    return victims.size();
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;   // 异常表方法不再跳过：可达性含异常边
            const std::vector<ca::u8> reach = reachability(*cu);
            ca::usize                 dead  = 0;
            for (const ca::u8 r : reach) dead += (r == 0 ? 1 : 0);
            if (dead != 0) result.sites.push_back(Site{std::string(entry.uri), mi, dead});
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           methods = 0;
        ca::usize           insns   = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        std::vector<const Site*> sitesOf;
        for (const Site& site : result.sites) {
            if (site.uri == uri) sitesOf.push_back(&site);
        }

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        // 前置检查：原样往返不了的方法不进改写面。
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(
            *entry.bytes,
            std::vector<ca::usize>(methodIdxs.begin(), methodIdxs.end()));

        ca::usize            hitMethods = 0;
        ca::usize            hitInsns   = 0;
        for (const ca::usize mi : methodIdxs) {
            dirty.attempted += 1;
            if (safe.count(mi) == 0) continue;
            ca::usize killed = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    killed = rewriteMethod(&cu);
                    return killed > 0;
                });
            if (!ok) continue;   // fail-closed：该方法放弃，其余方法已写回的不受影响
            hitMethods += 1;
            hitInsns += killed;
        }
        if (hitMethods == 0) return;
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(
            PendingWrite{uri, std::move(encoded).unwrap(), hitMethods, hitInsns});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.methods += item.methods;
        dirty.insns += item.insns;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::dead_code
