/// @file        exc_bridge_restore.cpp
/// @brief       残留 `exc_bridge_restore` 三段实现：SCAN / REWRITE / PROOF。
///
/// 源算法对照：grunt3 @ e41e852 FlowExceptionBridgeInserter.kt（trap 块
/// `NEW; DUP; INVOKESPECIAL <init>; ATHROW`、handler 块 `POP` +
/// Synthetic `GOTO`、`edge.to = trap` 改指、异常区 catchType =
/// RuntimeException 且保护区恰为 trap 四条）；JvmFlowExporter
/// emitExceptionRegions 按连续 run 出表——单块保护区 → 表项恰罩 trap；
/// emitGoto 仅在 `mode==Fallthrough && nextBlock==target` 时省略 GOTO，
/// handler 位置由 exporter 显式补 GOTO 守卫（位置落入只发生在 trap 头）。
/// 罐 A 实测（census_r1.py，校验器式可达普查）：严格形桥 13461 / 726 类，
/// 覆盖门 / handler 碰撞门 / X 落点门命中全 0，位置落入前驱 =
/// istore/iinc/ifle 三形。
///
/// 改写全程句柄制：rewriteMethod 入口一次性把 plan 位置解析成 DOM 句柄
/// （promote 后未改动，位置 ↔ 句柄一一对应），此后插入/擦除不使句柄失效
/// （CodeDom 句柄带 owner/gen，插入只追加槽位），位置账不再参与。

#include "grunt/exc_bridge_restore.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "mjbase/asm/cfg/basic_block.hpp"

namespace grunt::exc_bridge_restore {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::usize kNoPos = static_cast<ca::usize>(-1);

/// @brief 常量池 Class 索引 → 内部名（一跳）；无效返回空。
std::string_view classNameOf(const jbc::core::ClassFile& cf, ca::u32 classCp)
{
    if (classCp == 0 || !cf.cp.valid(classCp)) return {};
    return cf.className(classCp);
}

/// @brief INVOKE 特征三元组（owner/name/desc）从 Methodref 池项解出。
bool methodRefOf(const jbc::core::ClassFile& cf, ca::u32 cpIndex,
                 std::string_view& owner, std::string_view& name, std::string_view& desc)
{
    if (!cf.cp.valid(cpIndex)) return false;
    const jbc::core::ConstantPool::Entry& ref = cf.cp.at(cpIndex);
    if (ref.tag != jbc::core::CpTag::Methodref) return false;
    if (!cf.cp.valid(ref.a) || !cf.cp.valid(ref.b)) return false;
    owner = classNameOf(cf, ref.a);
    const jbc::core::ConstantPool::Entry& nat = cf.cp.at(ref.b);
    if (nat.tag != jbc::core::CpTag::NameAndType) return false;
    if (!cf.cp.valid(nat.a) || !cf.cp.valid(nat.b)) return false;
    name = cf.cp.utf8Of(nat.a);
    desc = cf.cp.utf8Of(nat.b);
    return !owner.empty() && !name.empty() && !desc.empty();
}

/// @brief 无条件终结符：其后不存在位置落入边（fallthrough 不可达）。
bool unconditionalTerminator(Op op)
{
    switch (op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::IRETURN: case Op::LRETURN: case Op::FRETURN:
    case Op::DRETURN: case Op::ARETURN: case Op::RETURN:
    case Op::ATHROW:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::RET:
    case Op::JSR:
    case Op::JSR_W:
        return true;
    default:
        return false;
    }
}

/// @brief 单方法 DOM 位置映射（labelPos / posOf 同 dead_code 形）。
struct PosMap
{
    std::unordered_map<ir::InsnId, ca::usize, mjbase::asm_cfg::InsnIdHash> posOf;
    const std::vector<ir::InsnId>* order = nullptr;

    void build(const std::vector<ir::InsnId>& seq)
    {
        order = &seq;
        posOf.reserve(seq.size() * 2);
        for (ca::usize i = 0; i < seq.size(); ++i) posOf.emplace(seq[i], i);
    }

    ca::usize labelPos(const ir::CodeDom& dom, const ir::LabelId& label, bool allowEnd) const
    {
        auto resolved = dom.resolveLabel(label);
        if (resolved.is_err()) return kNoPos;
        const ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) return allowEnd ? order->size() : kNoPos;
        const auto it = posOf.find(t.insn);
        return it != posOf.end() ? it->second : kNoPos;
    }

    ca::usize posOfInsn(const ir::InsnId id) const
    {
        const auto it = posOf.find(id);
        return it != posOf.end() ? it->second : kNoPos;
    }
};

/// @brief 形状钉命中的站点几何（全是位置；rewrite 入口统一定位成句柄）。
struct ShapeHit
{
    ca::usize sp = 0;             ///< trap 头（NEW）位置
    ca::usize hp = 0;             ///< handler 头（POP）位置
    ca::usize xPos = 0;           ///< 桥目标（handler goto 靶）位置
    ca::usize users = 0;          ///< trap 头引用数（trace 用）
    ca::usize victimPos[6] = {};  ///< trap 4 条 + handler 2 条的位置
};

/// @brief 严格形状钉：异常表项是否为生成面异常桥。
///
/// 形状（逐字，源锚 createExceptionTrapBlock / createExceptionHandlerBlock）：
///   [sp, sp+4) 恰四条：NEW cp(→java/lang/RuntimeException); DUP;
///                      INVOKESPECIAL cp(→RuntimeException.<init> ()V); ATHROW
///   [hp, hp+2) 恰两条：POP; GOTO|GOTO_W（Synthetic goto）→ 目标 X
/// 互证（真实 throw 区不可能同构）：
///   - handler goto 无直接引用（跳入会 POP 空栈，校验器拒绝）；
///   - 非头受害者（DUP/INVOKESPECIAL/ATHROW/POP/goto）无字段引用——生成面
///     块只从 trap 头进入，中段入口对校验器是栈形错位。
/// 覆盖门 / handler 碰撞门 / X 落点门在 planMethod 里做（要看全表）。
bool shapePin(const ir::ClassSession& s, const ir::CodeDom& dom, const PosMap& pm,
              const ir::CodeUnit::Exc& e, ShapeHit& hit, const char*& why)
{
    const jbc::core::ClassFile&    cf    = s.backing();
    const std::vector<ir::InsnId>& order = *pm.order;

    const std::string_view catchName = classNameOf(cf, e.catchCp);
    if (catchName != "java/lang/RuntimeException") {
        why = "shape";
        return false;
    }
    const ca::usize sp = pm.labelPos(dom, e.start, false);
    const ca::usize ep = pm.labelPos(dom, e.end, true);
    const ca::usize hp = pm.labelPos(dom, e.handler, false);
    if (sp == kNoPos || ep == kNoPos || hp == kNoPos || ep != sp + 4 ||
        hp + 1 >= order.size()) {
        why = "shape";
        return false;
    }
    const ir::Insn* i0 = dom.peek(order[sp]);
    const ir::Insn* i1 = dom.peek(order[sp + 1]);
    const ir::Insn* i2 = dom.peek(order[sp + 2]);
    const ir::Insn* i3 = dom.peek(order[sp + 3]);
    const ir::Insn* h0 = dom.peek(order[hp]);
    const ir::Insn* h1 = dom.peek(order[hp + 1]);
    if (i0 == nullptr || i1 == nullptr || i2 == nullptr || i3 == nullptr ||
        h0 == nullptr || h1 == nullptr) {
        why = "shape";
        return false;
    }
    const std::string_view newCls = classNameOf(cf, i0->cp);
    std::string_view owner, name, desc;
    const bool trapShape = i0->op == Op::NEW && newCls == "java/lang/RuntimeException" &&
                           i1->op == Op::DUP && i2->op == Op::INVOKESPECIAL &&
                           methodRefOf(cf, i2->cp, owner, name, desc) &&
                           owner == "java/lang/RuntimeException" && name == "<init>" &&
                           desc == "()V" && i3->op == Op::ATHROW;
    const bool handlerShape =
        h0->op == Op::POP && (h1->op == Op::GOTO || h1->op == Op::GOTO_W);
    if (!trapShape || !handlerShape) {
        why = "shape";
        return false;
    }

    // X 局部门：可解析、非方法末、handler goto 无直接引用。
    auto xResolved = dom.resolveLabel(h1->target);
    if (xResolved.is_err() || xResolved.unwrap().isEnd()) {
        why = "xInvalid";
        return false;
    }
    const ca::usize xPos = pm.posOfInsn(xResolved.unwrap().insn);
    if (xPos == kNoPos || !dom.usersOf(order[hp + 1]).empty()) {
        why = "xInvalid";
        return false;
    }

    // 非头受害者无字段引用（生成面块只从 trap 头进入）。
    const ca::usize others[5] = {sp + 1, sp + 2, sp + 3, hp, hp + 1};
    for (const ca::usize p : others) {
        if (!dom.usersOf(order[p]).empty()) {
            why = "xInvalid";
            return false;
        }
    }

    hit.sp           = sp;
    hit.hp           = hp;
    hit.xPos         = xPos;
    hit.users        = dom.usersOf(order[sp]).size();
    hit.victimPos[0] = sp;
    hit.victimPos[1] = sp + 1;
    hit.victimPos[2] = sp + 2;
    hit.victimPos[3] = sp + 3;
    hit.victimPos[4] = hp;
    hit.victimPos[5] = hp + 1;
    return true;
}

/// @brief 单条改写计划（位置形；rewrite 入口统一定位成句柄）。
struct SitePlan
{
    ca::usize excIndex   = 0;
    ca::usize sp         = 0;       ///< trap 头位置
    ca::usize hp         = 0;       ///< handler 头位置
    ca::usize xPos       = 0;       ///< 桥目标位置
    ca::usize users      = 0;
    bool      needsPin   = false;
    ca::usize pinPredPos = kNoPos;  ///< 钉插入点（布局前驱）；kNoPos = 方法头
};

/// @brief 存活表项锚吸附安排（位置形）。
struct Rebind
{
    ca::usize ej      = 0;   ///< 存活表项下标
    bool      isStart = false;
    ca::usize snapPos = 0;   ///< 吸附目标位置；order.size() = 方法末哨兵（仅 end）
};

/// @brief 单方法桥站点计划（scan / rewrite 共用一套判据）。
struct MethodPlan
{
    std::vector<SitePlan> sites;
    std::vector<Rebind>   rebinds;
    bool                  methodFail = false;  ///< 吸附不可行，整方法放弃
};

/// @brief 单方法桥站点计划。判据与桶记账全程 fail-closed。
MethodPlan planMethod(const ir::ClassSession& s, const ir::CodeUnit& cu, ScanResult& out,
                      const std::string& uri, ca::usize mi)
{
    MethodPlan plan;
    const std::vector<ir::InsnId>& order = cu.dom.order();
    if (order.empty() || cu.exc.empty()) return plan;
    PosMap pm;
    pm.build(order);

    // 全表锚位预解析（覆盖/碰撞/吸附三门都要看全表）。
    const ca::usize        n = cu.exc.size();
    std::vector<ca::usize> posStarts(n, kNoPos), posEnds(n, kNoPos), posHandlers(n, kNoPos);
    for (ca::usize ei = 0; ei < n; ++ei) {
        posStarts[ei]   = pm.labelPos(cu.dom, cu.exc[ei].start, false);
        posEnds[ei]     = pm.labelPos(cu.dom, cu.exc[ei].end, true);
        posHandlers[ei] = pm.labelPos(cu.dom, cu.exc[ei].handler, false);
    }

    const auto reject = [&](ca::usize ei, const char* why) {
        const ca::usize anchor = posStarts[ei] == kNoPos ? 0 : posStarts[ei];
        out.rejects.push_back(Reject{uri, mi, static_cast<ca::u32>(anchor), why});
    };

    // ---- 一轮：形状钉 + 覆盖门 + handler 碰撞门。形状命中者受害位置全量
    //      进 xUnion（无论后续门放不放行——被判拒的桥原样保留，别的桥目标
    //      落进去同样是改指进保留块，一样拒）。
    std::vector<ShapeHit>         hits(n);
    std::vector<bool>             provisional(n, false);
    std::unordered_set<ca::usize> xUnion;
    for (ca::usize ei = 0; ei < n; ++ei) {
        const char* why = "shape";
        if (!shapePin(s, cu.dom, pm, cu.exc[ei], hits[ei], why)) {
            if (why[0] == 's') {
                ++out.shapeMiss;
            } else {
                ++out.xInvalid;
            }
            reject(ei, why);
            continue;
        }
        for (const ca::usize p : hits[ei].victimPos) xUnion.insert(p);

        bool blocked = false;
        for (ca::usize ej = 0; ej < n && !blocked; ++ej) {
            if (ej == ei) continue;
            if (posStarts[ej] != kNoPos && posEnds[ej] != kNoPos &&
                ((posStarts[ej] <= hits[ei].sp && hits[ei].sp < posEnds[ej]) ||
                 (posStarts[ej] <= hits[ei].hp && hits[ei].hp < posEnds[ej]))) {
                blocked = true;   // 保护区覆盖 trap 头 / handler 头：派发歧义
            }
        }
        if (blocked) {
            ++out.covered;
            reject(ei, "covered");
            continue;
        }
        for (ca::usize ej = 0; ej < n && !blocked; ++ej) {
            if (ej == ei || posHandlers[ej] == kNoPos) continue;
            for (const ca::usize p : hits[ei].victimPos) {
                if (posHandlers[ej] == p) {
                    blocked = true;   // 他人 handler 锚在本桥块内：擦即侵占
                    break;
                }
            }
        }
        if (blocked) {
            ++out.handlerCollide;
            reject(ei, "handlerCollide");
            continue;
        }
        provisional[ei] = true;
    }

    // ---- 二轮：X 落点门（xUnion 全量就绪后判）。
    for (ca::usize ei = 0; ei < n; ++ei) {
        if (!provisional[ei]) continue;
        if (xUnion.count(hits[ei].xPos) != 0) {
            provisional[ei] = false;
            ++out.xInvalid;
            reject(ei, "xInvalid");
        }
    }

    // ---- 受害者位置并集（仅改写面）+ 存活表项锚吸附安排。
    std::unordered_set<ca::usize> eraseUnion;
    std::set<ca::usize>           acceptedExcs;
    for (ca::usize ei = 0; ei < n; ++ei) {
        if (!provisional[ei]) continue;
        acceptedExcs.insert(ei);
        for (const ca::usize p : hits[ei].victimPos) eraseUnion.insert(p);
    }
    for (ca::usize ei = 0; ei < n; ++ei) {
        if (acceptedExcs.count(ei) != 0) continue;   // 自家表项整体删除，不吸附
        const auto snapFrom = [&](ca::usize p, ca::usize& q) {
            q = p + 1;
            while (q < order.size() && eraseUnion.count(q) != 0) ++q;
            return q < order.size();
        };
        if (posHandlers[ei] != kNoPos && eraseUnion.count(posHandlers[ei]) != 0) {
            plan.methodFail = true;   // 他人 handler 被擦：语义侵占，整方法拒
        }
        ca::usize q = kNoPos;
        if (posStarts[ei] != kNoPos && eraseUnion.count(posStarts[ei]) != 0) {
            if (!snapFrom(posStarts[ei], q)) {
                plan.methodFail = true;   // start 无吸附位（不能落方法末哨兵）
            } else {
                plan.rebinds.push_back(Rebind{ei, true, q});
            }
        }
        if (posEnds[ei] != kNoPos && eraseUnion.count(posEnds[ei]) != 0) {
            if (!snapFrom(posEnds[ei], q)) {
                q = order.size();   // end 吸附越界 → 方法末哨兵（合法）
            }
            plan.rebinds.push_back(Rebind{ei, false, q});
        }
    }
    if (plan.methodFail) {
        for (ca::usize ei = 0; ei < n; ++ei) {
            if (!provisional[ei]) continue;
            ++out.rebindBlocked;
            reject(ei, "rebind");
        }
        plan.sites.clear();
        plan.rebinds.clear();
        return plan;
    }

    for (ca::usize ei = 0; ei < n; ++ei) {
        if (!provisional[ei]) continue;
        SitePlan sp2;
        sp2.excIndex = ei;
        sp2.sp       = hits[ei].sp;
        sp2.hp       = hits[ei].hp;
        sp2.xPos     = hits[ei].xPos;
        sp2.users    = hits[ei].users;
        // 位置落入门：trap 头布局前驱可落入（生成面 goto 省略形）或 trap 头
        // 即方法头（入口位置边）。前驱为无条件终结符时无位置边。
        if (hits[ei].sp == 0) {
            sp2.needsPin   = true;
            sp2.pinPredPos = kNoPos;
        } else {
            const ir::Insn* pred = cu.dom.peek(order[hits[ei].sp - 1]);
            if (pred == nullptr) {
                plan.methodFail = true;   // 异形，整方法保守放弃
                break;
            }
            if (!unconditionalTerminator(pred->op)) {
                sp2.needsPin   = true;
                sp2.pinPredPos = hits[ei].sp - 1;
            }
        }
        plan.sites.push_back(sp2);
    }
    if (plan.methodFail) {
        for (const SitePlan& sp2 : plan.sites) {
            ++out.rebindBlocked;
            out.rejects.push_back(Reject{uri, mi, static_cast<ca::u32>(sp2.sp), "rebind"});
        }
        plan.sites.clear();
        plan.rebinds.clear();
        return plan;
    }
    return plan;
}

/// @brief 单方法 REWRITE。@return 还原桥数（0 = 没动或失败，调用方整方法回滚）。
///
/// cu 是 mutateMethodWithFrames 提升好的当前 unit。执行序：入口一次把全部
/// plan 位置定位成句柄（此刻零改动，位置 ↔ 句柄一一对应）→ 存活表项锚
/// 吸附（Boundary 标签保护锚点，必须先移走 eraseBatch 才不拒）→ 逐站
/// replaceAllUses 直连 + 位置落入钉 + 本桥三标签销毁 → 表项剔除 → 单批
/// eraseBatch（kRejectIfUsed，全有或全无）。
ca::usize rewriteMethod(ir::CodeUnit* cu, const MethodPlan& plan)
{
    if (cu == nullptr || plan.methodFail || plan.sites.empty()) return 0;
    ir::CodeDom&   dom    = cu->dom;
    ir::CodeEditor editor(*cu);
    const std::vector<ir::InsnId>& order = dom.order();

    // ---- 1. 位置 → 句柄（一次定位；插入/擦除不使句柄失效）。
    struct SiteHandles
    {
        ir::InsnId  trapHead   = ir::kNoInsn;
        ir::InsnId  x          = ir::kNoInsn;
        ir::InsnId  victims[6] = {};
        ir::InsnId  pinPred    = ir::kNoInsn;   ///< kNoInsn = 方法头插入
        bool        needsPin   = false;
        ir::LabelId startLbl   = ir::kNoLabel;
        ir::LabelId endLbl     = ir::kNoLabel;
        ir::LabelId handlerLbl = ir::kNoLabel;
    };
    std::vector<SiteHandles> handles;
    handles.reserve(plan.sites.size());
    for (const SitePlan& ps : plan.sites) {
        if (ps.sp + 3 >= order.size() || ps.hp + 1 >= order.size() ||
            ps.xPos >= order.size()) {
            return 0;
        }
        SiteHandles h;
        h.trapHead   = order[ps.sp];
        h.x          = order[ps.xPos];
        h.victims[0] = order[ps.sp];
        h.victims[1] = order[ps.sp + 1];
        h.victims[2] = order[ps.sp + 2];
        h.victims[3] = order[ps.sp + 3];
        h.victims[4] = order[ps.hp];
        h.victims[5] = order[ps.hp + 1];
        h.needsPin   = ps.needsPin;
        h.pinPred    = ps.pinPredPos == kNoPos ? ir::kNoInsn : order[ps.pinPredPos];
        const ir::CodeUnit::Exc& e = cu->exc[ps.excIndex];
        h.startLbl   = e.start;
        h.endLbl     = e.end;
        h.handlerLbl = e.handler;
        for (const ir::InsnId v : h.victims) {
            if (dom.peek(v) == nullptr) return 0;
        }
        if (h.pinPred != ir::kNoInsn && dom.peek(h.pinPred) == nullptr) return 0;
        // 复核：handler goto 靶确为 X（plan 与 DOM 同源，此处防漂移）。
        const ir::Insn* g = dom.peek(h.victims[5]);
        if (g == nullptr) return 0;
        auto xNow = dom.resolveLabel(g->target);
        if (xNow.is_err() || xNow.unwrap().isEnd() || xNow.unwrap().insn != h.x) return 0;
        handles.push_back(h);
    }

    // ---- 2. 存活表项锚吸附（先于一切删除：Boundary 标签保护锚点）。
    for (const Rebind& rb : plan.rebinds) {
        if (rb.ej >= cu->exc.size()) return 0;
        const ir::LabelId lbl = rb.isStart ? cu->exc[rb.ej].start : cu->exc[rb.ej].end;
        const ir::InsnId  anchor =
            rb.snapPos >= order.size() ? ir::kNoInsn : order[rb.snapPos];
        if (anchor == ir::kNoInsn && rb.isStart) return 0;   // start 不落方法末
        if (!editor.bindLabel(lbl, anchor).is_ok()) return 0;
    }

    // ---- 3. 逐站改写：直连 + 落入钉 + 本桥三标签销毁。
    for (const SiteHandles& h : handles) {
        if (!dom.replaceAllUses(h.trapHead, h.x).is_ok()) return 0;
        if (h.needsPin) {
            auto lbl = editor.createLabel(h.x);
            if (lbl.is_err()) return 0;
            ir::Insn g;
            g.op     = Op::GOTO;
            g.target = lbl.unwrap();   // 插入前定靶：kNoLabel 靶过不了句柄校验
            auto ins = h.pinPred == ir::kNoInsn ? editor.insertAtStart(g)
                                                : editor.insertAfter(h.pinPred, g);
            if (ins.is_err()) return 0;
        }
        if (!dom.destroyLabel(h.startLbl).is_ok()) return 0;
        if (!dom.destroyLabel(h.endLbl).is_ok()) return 0;
        if (!dom.destroyLabel(h.handlerLbl).is_ok()) return 0;
    }

    // ---- 4. 本桥异常表项剔除。
    {
        std::vector<ir::CodeUnit::Exc> kept;
        kept.reserve(cu->exc.size());
        for (ca::usize ei = 0; ei < cu->exc.size(); ++ei) {
            bool mine = false;
            for (const SitePlan& ps : plan.sites) {
                if (ps.excIndex == ei) {
                    mine = true;
                    break;
                }
            }
            if (!mine) kept.push_back(cu->exc[ei]);
        }
        cu->exc = std::move(kept);
    }

    // ---- 5. 受害者单批擦除（trap 4 + handler 2 每站；全有或全无）。
    std::vector<ir::InsnId> victims;
    victims.reserve(plan.sites.size() * 6);
    for (const SiteHandles& h : handles) {
        for (const ir::InsnId v : h.victims) victims.push_back(v);
    }
    if (dom.eraseBatch(victims).is_err()) return 0;
    return plan.sites.size();
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;
    std::set<std::string> classSet;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        bool any = false;
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr || cu->exc.empty()) continue;
            const std::string uri(entry.uri);
            MethodPlan plan = planMethod(s, *cu, result, uri, mi);
            for (const SitePlan& ps : plan.sites) {
                any = true;
                Site site;
                site.uri         = uri;
                site.methodIndex = mi;
                site.excIndex    = ps.excIndex;
                site.posTrapHead = ps.sp;
                site.users       = ps.users;
                site.needsPin    = ps.needsPin;
                result.sites.push_back(std::move(site));
            }
        }
        if (any) classSet.insert(std::string(entry.uri));
    });
    result.bridgeClasses = classSet.size();
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

    // 计划按 uri 聚类（万类大罐上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           methods = 0;
        ca::usize           bridges = 0;
        ca::usize           pins    = 0;
        ca::usize           insns   = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto it = byUri.find(uri);
        if (it == byUri.end()) return;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : it->second) methodIdxs.insert(site->methodIndex);

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        // 前置门：原样往返不了的方法不进改写面。
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(
            *entry.bytes,
            std::vector<ca::usize>(methodIdxs.begin(), methodIdxs.end()));

        ca::usize hitMethods = 0, hitBridges = 0, hitPins = 0, hitInsns = 0;
        for (const ca::usize mi : methodIdxs) {
            if (safe.count(mi) == 0) continue;

            // 单 promote 纪律：plan 在 mutate lambda 内做（此刻零改动），
            // 改写落在同一 unit 上；scan 侧的 plan 只记账不参与改写。
            ca::usize planned    = 0;
            ca::usize plannedPins = 0;
            ca::usize bridges    = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    ScanResult sink;
                    MethodPlan plan = planMethod(s, cu, sink, uri, mi);
                    planned     = plan.sites.size();
                    plannedPins = 0;
                    for (const SitePlan& ps : plan.sites) plannedPins += ps.needsPin ? 1 : 0;
                    if (plan.sites.empty()) return false;
                    bridges = rewriteMethod(&cu, plan);
                    if (bridges == 0 && std::getenv("GRUNT_EXC_BRIDGE_TRACE") != nullptr) {
                        std::fprintf(stderr,
                                     "[grunt.ExcBridgeRestore] rewrite-fail %s#%zu sites=%zu\n",
                                     uri.c_str(), mi, planned);
                    }
                    return bridges > 0;
                });
            if (!ok) {
                if (std::getenv("GRUNT_EXC_BRIDGE_TRACE") != nullptr && bridges == 0) {
                    std::fprintf(stderr, "[grunt.ExcBridgeRestore] method-fail %s#%zu sites=%zu\n",
                                 uri.c_str(), mi, planned);
                }
                dirty.failed += planned;   // fail-closed：该方法放弃
                continue;
            }
            hitMethods += 1;
            hitBridges += bridges;
            hitPins += plannedPins;
            hitInsns += bridges * 6;
        }
        if (hitMethods == 0) return;
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hitMethods,
                                       hitBridges, hitPins, hitInsns});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.methods += item.methods;
        dirty.bridges += item.bridges;
        dirty.pins += item.pins;
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

    // residual：改写过的类里仍能通过形状钉 + 互证门的桥站点数（含改写
    // fail-closed 放弃的站点）。效力门挂起时间片，全量盘点。
    if (!dirty.uris.empty()) {
        const pass_clock::Suspend suspend;
        std::set<std::string>     dirtySet(dirty.uris.begin(), dirty.uris.end());
        ScanResult                rescan;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (dirtySet.count(std::string(entry.uri)) == 0) return;
            auto session = openSession(*entry.bytes);
            if (!session) return;
            ir::ClassSession&    s    = *session;
            const ir::ClassUnit& unit = s.unit();
            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                if (!ir::hasCode(s, mi)) continue;
                auto promoted = ir::promoteMethod(s, mi);
                if (promoted.is_err()) continue;
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr || cu->exc.empty()) continue;
                MethodPlan plan = planMethod(s, *cu, rescan, std::string(entry.uri), mi);
                result.residual += plan.sites.size();
            }
        });
    }
    return result;
}

}   // namespace grunt::exc_bridge_restore
