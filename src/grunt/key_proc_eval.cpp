/// @file        key_proc_eval.cpp
/// @brief       残留 `key_proc_eval` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 夹心钉与求值分离：SCAN 形状/目标/初始化/求值四层逐站过，全过才成站点；
/// REWRITE 按 `callSrcPc` 重定位后逐项复核（夹心逐字 + 成员身份 + 常量值 +
/// 重求值全等）再改写，任何一项不符整站放弃（fail-closed，不留半改写态）。
/// 求值走 jsim 编排层 + `Integer.rotateLeft(II)I` 接管（gate_processor_fold
/// 同款 mock，申报理由见该头注；rotateLeft 全序纯函数，位移掩码 5 位）。
#include "grunt/key_proc_eval.hpp"

#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::key_proc_eval {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using proof_tools::roundTripSafe;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

/// 扫描面 fail-closed 包裹（constant_call 同款两级收口）：单类异常只丢该类
/// 候选，不放大成整条管线失败。
template <class Fn>
bool scanGuard(std::string_view stage, std::string_view uri, Fn&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[grunt.KeyProcEval] %.*s skip class %.*s: %s\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[grunt.KeyProcEval] %.*s skip class %.*s: unknown exception\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data());
    }
    return false;
}

/// @brief int 槽装载/存储读取（cff_unflatten 同形私有副本：IL…/ISTORE 长短
///        形 → 槽号；生成面 emitProcessorStateProgram 的 storeSlot/loadSlot
///        取最短形，两处判定必须同口径）。
bool intLoadSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ILOAD) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0));
        return true;
    }
    return false;
}

bool intStoreSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ISTORE) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ISTORE_0 && in.op <= Op::ISTORE_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ISTORE_0));
        return true;
    }
    return false;
}

/// @brief 拒绝记录（计数桶 + 逐条同源，gate_processor_fold 同款）。
void bumpReject(ScanResult& result, std::string_view owner, std::string_view method,
                ca::u32 pc, RejectReason reason)
{
    switch (reason) {
    case RejectReason::TargetOutside:        ++result.targetOutside; break;
    case RejectReason::TargetNotStatic:      ++result.targetNotStatic; break;
    case RejectReason::TargetSynchronized:   ++result.targetSynchronized; break;
    case RejectReason::ClassInitPresent:     ++result.classInitPresent; break;
    case RejectReason::EvalBadState:         ++result.evalBadState; break;
    case RejectReason::EvalStepLimit:        ++result.evalStepLimit; break;
    case RejectReason::ValueKindUnsupported: ++result.valueKindUnsupported; break;
    }
    result.rejects.push_back(
        Reject{std::string(owner), std::string(method), pc, reason});
}

/// 容器方法索引构建：scan_tools::buildMethodIndex 收口，重名类撞键取后到
/// （insert_or_assign = KeepLast）、stderr 前缀走本 pass 的 scanGuard。
void buildIndex(const mjbase::model::ResourceContainer& container,
                scan_tools::MethodIndex& index)
{
    auto guard = [](std::string_view stage, std::string_view uri, auto&& fn) {
        return scanGuard(stage, uri, fn);
    };
    scan_tools::buildMethodIndex(container, index, scan_tools::IndexDup::KeepLast, guard);
}

/// @brief 求值器：jsim 编排层 + rotateLeft 接管，不装扩展包（proc 体按生成
///        面是纯算术 + 盐旋转，任何堆/字符串/indy 面在求值处 fail-closed）。
class ProcEvaluator
{
public:
    explicit ProcEvaluator(const mjbase::model::ResourceContainer& container)
        : exec_(container)
    {
        exec_.session().callMocks["java/lang/Integer.rotateLeft(II)I"] =
            [](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
            jsim::Result<jsim::SimValue, jsim::Bail> d = frame.popChecked();
            if (!d.isOk()) return d;
            jsim::Result<jsim::SimValue, jsim::Bail> i = frame.popChecked();
            if (!i.isOk()) return i;
            if (!i.value().known || !d.value().known) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                    jsim::SimValue::unknownOf(jsim::ValType::I32));
            }
            const ca::u32 vi =
                static_cast<ca::u32>(static_cast<ca::i32>(i.value().intBits));
            const ca::u32 dist =
                static_cast<ca::u32>(static_cast<ca::i32>(d.value().intBits)) & 31u;
            const ca::u32 rotated = (vi << dist) | (vi >> ((32u - dist) & 31u));
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                jsim::SimValue::knownI32(static_cast<ca::i32>(rotated)));
        };
    }

    /// @brief proc(input, salt) 跨方法求值。@return true = out 填好；false =
    ///        why 填拒绝桶（EvalBadState / EvalStepLimit / ValueKindUnsupported）。
    bool eval(const std::string& owner, const std::string& name, ca::i32 input,
              ca::i32 salt, ca::i32& out, RejectReason& why)
    {
        const SimOutcome outcome =
            exec_.invoke(owner, name + std::string(knowledge::kGruntCffProcessorActionDesc),
                         std::vector<jsim::SimValue>{jsim::SimValue::knownI32(input),
                                                     jsim::SimValue::knownI32(salt)});
        if (outcome.kind == SimOutcome::Kind::Void) {
            why = RejectReason::EvalBadState;
            return false;
        }
        if (!outcome.ok()) {
            why = outcome.kind == SimOutcome::Kind::LimitHit ? RejectReason::EvalStepLimit
                                                             : RejectReason::EvalBadState;
            return false;
        }
        if (!outcome.value.known || outcome.value.type != jsim::ValType::I32) {
            why = RejectReason::ValueKindUnsupported;
            return false;
        }
        out = static_cast<ca::i32>(outcome.value.intBits);
        return true;
    }

private:
    SimExecutor exec_;
};

/// @brief 夹心形状钉（scan 与 rewrite 复核共用）：`pos` 处 INVOKESTATIC，
///        `pos-4..pos-1` 逐字为 `[int 常量; ISTORE(s); ILOAD(s); int 常量]`
///        （DOM 序紧邻，NOP 不跳），`pos+1` 为 `ISTORE(s)`。@return 命中时
///        填出参并 true。
bool sandwichAt(ir::ClassSession& s, const ir::CodeDom& dom,
                const std::vector<ir::InsnId>& order, ca::usize pos, ca::u32& stateSlot,
                ca::i32& inputKey, ca::i32& salt, ca::u32& anchorSrcPc)
{
    if (pos < 4 || pos + 1 >= order.size()) return false;
    const auto peek = [&](ca::usize p) -> const ir::Insn* {
        return dom.peek(order[p]);
    };
    const ir::Insn* i1 = peek(pos - 1);   // salt 常量
    const ir::Insn* i2 = peek(pos - 2);   // ILOAD s
    const ir::Insn* i3 = peek(pos - 3);   // ISTORE s
    const ir::Insn* i4 = peek(pos - 4);   // inputKey 常量
    const ir::Insn* i5 = peek(pos + 1);   // 尾 ISTORE s
    if (i1 == nullptr || i2 == nullptr || i3 == nullptr || i4 == nullptr
        || i5 == nullptr) {
        return false;
    }
    ca::u32 ld = 0, st = 0, tail = 0;
    if (!const_args::decodeConstInt(s, *i1, salt)) return false;
    if (!intLoadSlotOf(*i2, ld)) return false;
    if (!intStoreSlotOf(*i3, st) || ld != st) return false;
    if (!const_args::decodeConstInt(s, *i4, inputKey)) return false;
    if (!intStoreSlotOf(*i5, tail) || tail != st) return false;
    stateSlot   = st;
    anchorSrcPc = i4->srcPc;
    return true;
}

/// @brief result 的规范常量装载（小值直装，大值 LDC_W 内签；保 srcPc 由调
///        用方回填）。@return false = 内签失败（CP 下标越 u2），整站放弃。
bool makeConstLoad(ir::ClassSession& s, ca::i32 v, ir::Insn& out)
{
    switch (v) {
    case -1: out.op = Op::ICONST_M1; return true;
    case 0:  out.op = Op::ICONST_0; return true;
    case 1:  out.op = Op::ICONST_1; return true;
    case 2:  out.op = Op::ICONST_2; return true;
    case 3:  out.op = Op::ICONST_3; return true;
    case 4:  out.op = Op::ICONST_4; return true;
    case 5:  out.op = Op::ICONST_5; return true;
    default: break;
    }
    if (v >= -128 && v <= 127) {
        out.op = Op::BIPUSH;
        out.a  = static_cast<ca::u32>(v);
        return true;
    }
    if (v >= -32768 && v <= 32767) {
        out.op = Op::SIPUSH;
        out.a  = static_cast<ca::u32>(v);
        return true;
    }
    const ca::u32 cp = s.internInteger(v);
    if (cp == 0 || cp > 0xFFFF) return false;
    out.op = Op::LDC_W;
    out.cp = cp;
    return true;
}

/// @brief 单类扫描（scan 与 proof 的 residual 复扫共用，全判据：夹心形 +
///        目标解析 + 类初始化检查 + 跨方法求值）。@return false = 会话开不
///        了（调用方按 classesSkipped 口径计）。
bool scanClassInto(const mjbase::model::ResourceContainer& container,
                   const ClassEntry& entry, const scan_tools::MethodIndex& index,
                   ProcEvaluator& evaluator, ScanResult& result)
{
    auto session = openSession(*entry.bytes);
    if (!session) return false;
    ir::ClassSession&      s    = *session;
    const std::string_view self = s.backing().thisName();

    const ir::ClassUnit& unit = s.unit();
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (!ir::hasCode(s, mi)) continue;
        auto promoted = ir::promoteMethod(s, mi);
        if (promoted.is_err()) continue;
        ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;

        const std::string_view name = unit.symbols().str(unit.methods[mi].name);
        const std::vector<ir::InsnId>& order = cu->dom.order();

        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = cu->dom.peek(order[pos]);
            if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

            ir::ClassSession::MemberSem enc;
            if (!s.memberOf(*in, enc)) continue;
            if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;
            if (enc.desc != knowledge::kGruntCffProcessorActionDesc) continue;

            ca::u32 stateSlot = 0;
            ca::i32 inputKey = 0, salt = 0;
            ca::u32 anchorSrcPc = 0;
            if (!sandwichAt(s, cu->dom, order, pos, stateSlot, inputKey, salt,
                            anchorSrcPc)) {
                continue;   // 非夹心形：非候选，不进桶
            }
            ++result.totalShape;

            const auto reject = [&](RejectReason r) {
                bumpReject(result, self, name, in->srcPc, r);
            };

            // 目标解析：容器内 static 非 synchronized（有码检查在求值段：
            // loc.index 是目标类下标，本会话是调用者类）。
            const auto it = index.find(scan_tools::keyOf(enc.owner, enc.name, enc.desc));
            if (it == index.end()) {
                reject(RejectReason::TargetOutside);
                continue;
            }
            const scan_tools::MethodLoc& loc = it->second;
            if ((loc.access & kAccStatic) == 0) {
                reject(RejectReason::TargetNotStatic);
                continue;
            }
            if ((loc.access & kAccSynchronized) != 0) {
                reject(RejectReason::TargetSynchronized);
                continue;
            }

            // 类初始化检查（constant_call / gate_processor_fold 同源口径）：
            // 目标类 <clinit> 非中性时只留同类调用（JVMS 5.5 本类方法开始执
            // 行时类已在初始化中/已完成，改写调用不移时机）。目标字节缺失时
            // 跳过本检查，求值段按 eval 拒。
            if (loc.owner != self) {
                const std::vector<ca::u8>* targetBytes =
                    findClassBytes(container, loc.owner + ".class");
                if (targetBytes != nullptr) {
                    auto targetSession = openSession(*targetBytes);
                    if (targetSession && !scan_tools::classInitNeutral(*targetSession)) {
                        reject(RejectReason::ClassInitPresent);
                        continue;
                    }
                }
            }

            ca::i32 procResult = 0;
            RejectReason why = RejectReason::EvalBadState;
            if (!evaluator.eval(loc.owner, std::string(enc.name), inputKey, salt,
                                procResult, why)) {
                reject(why);
                continue;
            }

            Site site;
            site.uri          = std::string(entry.uri);
            site.methodIndex  = mi;
            site.callerOwner  = std::string(self);
            site.callerMethod = std::string(name);
            site.callSrcPc    = in->srcPc;
            site.anchorSrcPc  = anchorSrcPc;
            site.stateSlot    = stateSlot;
            site.targetOwner  = loc.owner;
            site.targetName   = std::string(enc.name);
            site.inputKey     = inputKey;
            site.salt         = salt;
            site.result       = procResult;
            result.sites.push_back(std::move(site));
        }
    }
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    scan_tools::MethodIndex index;
    buildIndex(container, index);

    ProcEvaluator evaluator(container);

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("scan", entry.uri, [&] {
            if (!scanClassInto(container, entry, index, evaluator, result)) {
                ++result.classesSkipped;
            }
        });
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

namespace {

/// @brief 单方法 REWRITE。@return 改写成功的站点数；failed / failedByReason
///        走出参。
///
/// cu 由调用方 promote：DOM 改写落在本 unit，随后由调用方统一 lowerMethod
/// （夹心改写不新增/不移动任何控制流：锚换常量装载、中段 eraseBatch，尾
/// ISTORE 原样——前窗无分支，帧面零改动）。中段带标签（合并点）整站放弃。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ProcEvaluator& evaluator,
                        ca::usize& failed, std::vector<ca::usize>& failedByReason)
{
    ir::CodeDom& dom = cu->dom;
    const std::vector<ir::InsnId>& order = dom.order();
    ca::usize hit = 0;

    const auto bumpFail = [&](RwResult r) {
        ++failed;
        const ca::usize idx = static_cast<ca::usize>(r);
        if (failedByReason.size() <= idx) failedByReason.resize(idx + 1, 0);
        ++failedByReason[idx];
    };

    for (const Site* sitePtr : sites) {
        const Site& site = *sitePtr;

        // 按 srcPc+op 重定位（跨会话稳定，InsnId 不带出去）。
        ca::usize pos = kNoIndex;
        for (ca::usize p = 0; p < order.size(); ++p) {
            const ir::Insn* in = dom.peek(order[p]);
            if (in != nullptr && in->srcPc == site.callSrcPc && in->op == Op::INVOKESTATIC) {
                pos = p;
                break;
            }
        }
        if (pos == kNoIndex) {
            bumpFail(RwResult::ProcNotFound);
            continue;
        }

        // 夹心逐字复核 + 成员身份 + 常量值 + 重求值全等（scan 之后字节可能
        // 已被同方法其他站点改写，不盲改）。
        ca::u32 stateSlot = 0;
        ca::i32 inputKey = 0, salt = 0;
        ca::u32 anchorSrcPc = 0;
        const ir::Insn* proc = dom.peek(order[pos]);
        if (proc == nullptr || !sandwichAt(s, dom, order, pos, stateSlot, inputKey, salt,
                                           anchorSrcPc)
            || stateSlot != site.stateSlot || inputKey != site.inputKey || salt != site.salt
            || anchorSrcPc != site.anchorSrcPc) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        ir::ClassSession::MemberSem enc;
        if (!s.memberOf(*proc, enc) || enc.owner != site.targetOwner
            || enc.name != site.targetName) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        ca::i32 reevaluated = 0;
        RejectReason why    = RejectReason::EvalBadState;
        if (!evaluator.eval(site.targetOwner, site.targetName, inputKey, salt, reevaluated,
                            why)
            || reevaluated != site.result) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }

        // 受害者清单：中段 4 条（ISTORE/ILOAD/salt/INVOKESTATIC）。锚带标签
        // 安全（原地改写不动标签）；中段任一带标签 = 合并点，外部到达路径
        // 的栈形状与夹心产出耦合，整站放弃。
        std::vector<ir::InsnId> victims;
        bool blocked = false;
        for (ca::usize k = pos - 3; k <= pos; ++k) {
            if (!dom.labelsAt(order[k]).empty()) blocked = true;
            victims.push_back(order[k]);
        }
        if (blocked) {
            bumpFail(RwResult::LabelBlocked);
            continue;
        }

        // 锚先内签好再动字节：失败整站放弃（无半改写态）。
        ir::Insn*     anchor = nullptr;
        auto anchorR = dom.get(order[pos - 4]);
        if (anchorR.is_err()) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        anchor = anchorR.unwrap();
        const ir::Insn original = *anchor;
        const ca::u32 keepPc = anchor->srcPc;

        ir::Insn fresh{};
        if (!makeConstLoad(s, site.result, fresh)) {
            bumpFail(RwResult::AnchorIntern);
            continue;
        }
        fresh.srcPc = keepPc;
        *anchor     = fresh;

        // 中段单批擦除。eraseBatch 是 kRejectIfUsed：受害者被异常表
        // start/end 引用时拒收，拒收即整站放弃，锚回滚原样。
        if (dom.eraseBatch(victims).is_err()) {
            *anchor = original;
            bumpFail(RwResult::EraseFail);
            continue;
        }
        ++hit;
    }
    return hit;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit = 0;
    };
    std::vector<PendingWrite> pending;

    ProcEvaluator evaluator(container);

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

        // 前置检查：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            roundTripSafe(*entry.bytes,
                          std::vector<ca::usize>(methodIdxs.begin(), methodIdxs.end()));

        ca::usize              hit    = 0;
        ca::usize              failed = 0;
        std::vector<ca::usize> failedByReason;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
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
                methodHit = rewriteMethod(s, cu, sitesOfMethod, evaluator, failed,
                                          failedByReason);
            }
            if (methodHit == 0) continue;   // 该方法无改动，其余方法不受影响
            hit += methodHit;
        }
        dirty.failed += failed;
        if (failedByReason.size() > dirty.failedByReason.size()) {
            dirty.failedByReason.resize(failedByReason.size(), 0);
        }
        for (ca::usize i = 0; i < failedByReason.size(); ++i) {
            dirty.failedByReason[i] += failedByReason[i];
        }
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

    // residual：脏类集上重跑全判据（夹心形 + 目标解析 + 类初始化检查 + 跨
    // 方法求值）仍成站点的数量。改写 fail-closed 放弃的站点如实入账；求值
    // 失败面（从未进改写面）不计入。效力检查挂时间片，全量盘点。
    if (!dirty.uris.empty()) {
        const pass_clock::Suspend suspend;
        const std::set<std::string> dirtySet(dirty.uris.begin(), dirty.uris.end());

        scan_tools::MethodIndex index;
        buildIndex(container, index);

        ProcEvaluator evaluator(container);
        ScanResult    rescan;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (dirtySet.count(std::string(entry.uri)) == 0) return;
            scanGuard("residual", entry.uri,
                      [&] { scanClassInto(container, entry, index, evaluator, rescan); });
        });
        result.residual = rescan.sites.size();
    }
    return result;
}

}   // namespace grunt::key_proc_eval
