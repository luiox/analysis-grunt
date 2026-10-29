/// @file        dead_code.cpp
/// @brief       残留 `dead_code` 的三段实现：SCAN / REWRITE / PROOF。
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
/// 并进入 handler，故 handler 以「区间与可达集相交」为根，迭代到不动点
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
/// 受害者一律 eraseBatch——SMT 由 rewireFramesAfterLower 严格重算，无需
/// 为帧锚保活任何指令。异常表不再整体跳过：可达性含异常边（见
/// reachability），全死区间删表项，端点锚在死指令上时向前吸附到存活
/// 指令（区间吞并被删段，与 frame_rewire 的 LNT/LVT 吸附规则一致），
/// 尾部全死落到方法末尾哨兵。
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

    // 异常表收敛（先于擦除：吸附目标须在存活指令上，擦前全部可解析）。
    std::vector<ir::CodeUnit::Exc> liveExc;
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
            continue;
        }
        if (firstLive != sBgn) {
            auto lbl = cu->dom.createLabel(order[firstLive]);
            if (lbl.is_err()) return 0;
            e.start = lbl.unwrap();
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
            auto lbl = clamp == kNo ? cu->dom.createLabel()
                                    : cu->dom.createLabel(order[clamp]);
            if (lbl.is_err()) return 0;
            e.end = lbl.unwrap();
            excChanged = true;
        }
        liveExc.push_back(e);
    }

    std::vector<ir::InsnId> victims;
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (reach[i] == 0) victims.push_back(order[i]);
    }
    if (victims.empty() && !excChanged) return 0;
    if (excChanged) cu->exc = std::move(liveExc);
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

        // 前置门：原样往返不了的方法不进改写面。
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
