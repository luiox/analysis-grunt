/// @file        predicate_fold.cpp
/// @brief       残留 `predicate_fold` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/predicate_fold.hpp"

#include <set>
#include <string>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::predicate_fold {

namespace {

namespace ev = mjbase::asm_evaluator;
namespace ir = jbc::ir;
using jbc::core::Op;
using proof_tools::roundTripSafe;

/// @brief if 系比较语义：IFEQ..IFLE 是 v 与 0 比，IF_ICMP* 是 v1 与 v2 比。
bool branchVerdict(Op op, ca::i32 v1, ca::i32 v2, bool& taken)
{
    switch (op) {
    case Op::IFEQ:         taken = v1 == 0; return true;
    case Op::IFNE:         taken = v1 != 0; return true;
    case Op::IFLT:         taken = v1 < 0;  return true;
    case Op::IFGE:         taken = v1 >= 0; return true;
    case Op::IFGT:         taken = v1 > 0;  return true;
    case Op::IFLE:         taken = v1 <= 0; return true;
    case Op::IF_ICMPEQ:    taken = v1 == v2; return true;
    case Op::IF_ICMPNE:    taken = v1 != v2; return true;
    case Op::IF_ICMPLT:    taken = v1 < v2;  return true;
    case Op::IF_ICMPGE:    taken = v1 >= v2; return true;
    case Op::IF_ICMPGT:    taken = v1 > v2;  return true;
    case Op::IF_ICMPLE:    taken = v1 <= v2; return true;
    default: return false;
    }
}

bool isSwitch(Op op)
{
    return op == Op::TABLESWITCH || op == Op::LOOKUPSWITCH;
}

/// @brief 操作数个数：if 系 1/2，switch 的键 1。
ca::u32 operandCount(Op op)
{
    if (op >= Op::IF_ICMPEQ && op <= Op::IF_ICMPLE) return 2;
    return 1;
}

bool isCandidate(Op op)
{
    return (op >= Op::IFEQ && op <= Op::IFLE)
           || (op >= Op::IF_ICMPEQ && op <= Op::IF_ICMPLE) || isSwitch(op);
}

bool allI32(const std::vector<ev::Value>& args)
{
    for (const ev::Value& a : args) {
        if (a.kind != ev::Value::Kind::I32) return false;
    }
    return true;
}

void scanMethod(ir::ClassSession& s, ca::usize methodIndex, const std::string& uri,
                ScanResult& result)
{
    if (!ir::hasCode(s, methodIndex)) return;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || !isCandidate(in->op)) continue;

        std::vector<ir::InsnId> producers;
        std::vector<ev::Value>  args;
        if (!const_args::collectArgs(s, cu->dom, order, pos, operandCount(in->op),
                                     producers, args)) {
            continue;
        }
        if (!allI32(args)) continue;
        result.sites.push_back(Site{uri, methodIndex, in->srcPc, in->op});
    }
}

/// @brief 单方法 REWRITE。@return 改写成功的站点数；failed/skipped 走出参。
///
/// cu 是 mutateMethodWithFrames 提升好的当前 unit：promote 只发生一次，
/// DOM 改写、prepare、lower 都落在同一 unit 上（再 promote 即弃改写）。
/// 受害者一律 eraseBatch——SMT 由 rewireFramesAfterLower 严格重算，不存在
/// 需要保锚的帧。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed,
                        ca::usize& skipped)
{
    ir::CodeDom& dom = cu->dom;

    const std::vector<ir::InsnId>& order = dom.order();
    ca::usize                    hit  = 0;

    for (const Site* site : sites) {
        // 按 srcPc+op 重定位（跨会话稳定，InsnId 不带出去）。
        ca::usize branchPos = static_cast<ca::usize>(-1);
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = dom.peek(order[pos]);
            if (in != nullptr && in->srcPc == site->branchSrcPc && in->op == site->branchOp) {
                branchPos = pos;
                break;
            }
        }
        const ir::Insn* branch = branchPos < order.size() ? dom.peek(order[branchPos])
                                                          : nullptr;
        if (branch == nullptr) {
            ++failed;
            continue;
        }

        // 操作数区复核（scan 之后字节可能已被同方法其他站点改写，不盲改）。
        std::vector<ir::InsnId> producers;
        std::vector<ev::Value>  args;
        if (!const_args::collectArgs(s, dom, order, branchPos, operandCount(site->branchOp),
                                     producers, args)
            || producers.empty() || !allI32(args)) {
            ++failed;
            continue;
        }

        // 裁决与目标标签解析。
        bool              taken      = false;
        const ir::LabelId* gotoTarget = nullptr;
        if (isSwitch(site->branchOp)) {
            const ca::i32 key = args.back().i32;
            if (site->branchOp == Op::TABLESWITCH) {
                const ca::i32 lo    = branch->a;
                const ca::i32 span  = static_cast<ca::i32>(branch->targets.size());
                gotoTarget = key >= lo && key < lo + span
                                 ? &branch->targets[static_cast<ca::usize>(key - lo)]
                                 : &branch->target;
            } else {
                gotoTarget = &branch->target;
                for (ca::usize i = 0; i < branch->keys.size(); ++i) {
                    if (branch->keys[i] == key) {
                        gotoTarget = &branch->targets[i];
                        break;
                    }
                }
            }
            taken = true;
        } else {
            const ca::i32 v1 = args.size() == 2 ? args[0].i32 : 0;
            if (!branchVerdict(site->branchOp, v1, args.back().i32, taken)) {
                ++failed;
                continue;
            }
            gotoTarget = &branch->target;
        }

        // 标签门：区域中段或分支本体带标签 = 合并点，外部到达路径的栈形状与
        // 区域产出耦合，整站放弃；区域首条（anchor）的标签安全。
        bool blocked = !dom.labelsAt(order[branchPos]).empty();
        for (ca::usize i = 1; i < producers.size(); ++i) {
            if (!dom.labelsAt(producers[i]).empty()) blocked = true;
        }
        if (blocked) {
            ++failed;
            continue;
        }

        auto slotR = dom.get(producers.front());
        if (slotR.is_err()) {
            ++failed;
            continue;
        }
        ir::Insn*     anchor  = slotR.unwrap();
        const ca::u32 keepPc = anchor->srcPc;
        ir::Insn      fresh{};
        fresh.op    = taken ? Op::GOTO_W : Op::NOP;
        if (taken) fresh.target = *gotoTarget;
        fresh.srcPc = keepPc;
        *anchor     = fresh;

        // 其余操作数与分支本体：全部擦除（帧由 rewire 严格重算，无需保锚）。
        std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
        victims.push_back(order[branchPos]);
        if (dom.eraseBatch(victims).is_err()) {
            ++failed;
            continue;
        }
        ++hit;
    }
    return hit;
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
            scanMethod(s, mi, std::string(entry.uri), result);
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
        ca::usize           hit = 0;
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

        // 前置门：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit      = 0;
        ca::usize failed   = 0;
        ca::usize skipped = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                skipped += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(), [&](ir::CodeUnit& cu) {
                    methodHit = rewriteMethod(s, &cu, sitesOfMethod, failed, skipped);
                    return methodHit > 0;
                });
            if (!ok) continue;   // fail-closed：该方法放弃，其余方法已写回的不受影响
            hit += methodHit;
        }
        dirty.failed += failed;
        if (hit == 0) return;
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.sites += item.hit;
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

}   // namespace grunt::predicate_fold
