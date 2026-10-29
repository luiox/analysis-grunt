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

bool allI32(const std::vector<jsim::SimValue>& args)
{
    for (const jsim::SimValue& a : args) {
        if (a.type != jsim::ValType::I32 || !a.known) return false;
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
        std::vector<jsim::SimValue>  args;
        if (!const_args::collectArgs(s, cu->dom, order, pos, operandCount(in->op),
                                     producers, args)) {
            continue;
        }
        if (!allI32(args)) continue;
        result.sites.push_back(Site{uri, methodIndex, in->srcPc, in->op});
    }
}

/// @brief 单方法 REWRITE。@return 改写成功的站点数；failed 走出参。
///
/// cu 由调用方 promote：DOM 改写落在本 unit，随后由调用方统一 lowerMethod
/// （原 SMT/行表由 lower 的 pcmap 重定位——本改写不新增合并点：TAKEN 落
/// [ICONST_0, IFEQ→T] 恒真条件跳转、NOT-TAKEN 锚 NOP 原地保 srcPc，被擦
/// 指令若被帧/标签锚定 eraseBatch 会拒收整站放弃，不产生需要再生的帧）。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed)
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
        std::vector<jsim::SimValue>  args;
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
            const ca::i32 key = static_cast<ca::i32>(args.back().intBits);
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
            const ca::i32 v1 = args.size() == 2 ? static_cast<ca::i32>(args[0].intBits) : 0;
            if (!branchVerdict(site->branchOp, v1, static_cast<ca::i32>(args.back().intBits), taken)) {
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
        // 目标标签值先拷出：switch 侧 gotoTarget 指向 branch 自身字段。
        const ir::LabelId takenTarget = *gotoTarget;

        ir::Insn*     anchor  = slotR.unwrap();
        const ir::Insn original = *anchor;
        const ca::u32 keepPc = anchor->srcPc;

        // TAKEN：恒真条件跳转形态 [ICONST_0, IFEQ→T]——锚改常量、分支本体
        // 原地换 op+目标、其余操作数擦除。不插无条件 GOTO：HotSpot 线性走查
        // 要求无条件跳转之后的段首带帧，而被擦区域的延续段在原 SMT 中无帧
        // （非合并点，实测 VerifyError "Expecting a stack map frame"）；条件
        // 跳转让延续段保持 fallthrough 可达，帧面零改动。NOT-TAKEN：锚 NOP、
        // 区域整段消失，fallthrough 直续。
        ir::Insn      fresh{};
        fresh.op    = taken ? Op::ICONST_0 : Op::NOP;
        fresh.srcPc = keepPc;
        *anchor     = fresh;

        ir::Insn* branchMut      = nullptr;
        ir::Insn  branchOriginal{};
        if (taken) {
            auto branchR = dom.get(order[branchPos]);
            if (branchR.is_err()) {
                *anchor = original;
                ++failed;
                continue;
            }
            branchMut      = branchR.unwrap();
            branchOriginal = *branchMut;
            ir::Insn branchFresh{};
            branchFresh.op     = Op::IFEQ;
            branchFresh.target = takenTarget;
            branchFresh.srcPc  = branchOriginal.srcPc;
            *branchMut         = branchFresh;
        }

        // 其余操作数（与 NOT-TAKEN 下的分支本体）擦除。eraseBatch 是
        // kRejectIfUsed：受害者被异常表 start/end 引用时拒收（上面的标签门
        // 只盖跳转标签）——拒收即整站放弃，anchor/分支回滚原样，不留
        // 「常量装载已被覆盖、分支还在」的半改写态（栈下溢 VerifyError）。
        std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
        if (!taken) victims.push_back(order[branchPos]);
        if (!victims.empty() && dom.eraseBatch(victims).is_err()) {
            *anchor = original;
            if (branchMut != nullptr) *branchMut = branchOriginal;
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
        for (const ca::usize mi : methodIdxs) {
            const std::vector<const Site*> sitesOfMethod = [&] {
                std::vector<const Site*> out;
                for (const Site* site : sitesOf) {
                    if (site->methodIndex == mi) out.push_back(site);
                }
                return out;
            }();
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit = 0;
            {
                if (!ir::hasCode(s, mi)) continue;
                auto promoted = ir::promoteMethod(s, mi);
                if (promoted.is_err()) continue;
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr) continue;
                methodHit = rewriteMethod(s, cu, sitesOfMethod, failed);
            }
            if (methodHit == 0) continue;   // 该方法无改动，其余方法不受影响
            hit += methodHit;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        // 全部方法 lower 成功才落盘：任一失败整类放弃（session 随返回丢弃，
        // 半改写态不外泄）。
        for (const ca::usize mi : methodIdxs) {
            auto lowered = ir::lowerMethod(s, mi);
            if (lowered.is_err()) return;
        }
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
