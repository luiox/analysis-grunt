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

#include <cstdio>
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

/// @brief 冗余无条件跳转收集（跳转穿线退化形）：goto/goto_w 的目标 =
///        顺序后继（紧邻落点指令）时，跳转与顺序执行语义全等，擦除即穿线。
///
/// 混淆器用这对（goto + 落点标签）把常量表达式 / 调用实参区从中间劈开，
/// NF/ArithFold 的常量折叠与 PAF 三元组都被落点上的真标签挡死（u/b 族
/// 池读 + L 族 junk 谓词实证）。只收可达指令上的（死指令本就进受害者
/// 集）；victim 的目标标签锚在顺序后继（存活）上无涉，锚在 victim 本体
/// 上的存活入边标签由 rebindThreadingLabels 先行搬到落点，否则锚定保护
/// 整批拒绝。
/// @return 冗余 goto 数；out 非空时把受害者句柄按 order 序追加。
///
/// 冗余判定走工具箱单源 isFallthroughGoto（backing 布局真值）。
ca::usize collectRedundantGotos(const ir::CodeUnit& cu, const std::vector<ca::u8>& reach,
                                std::vector<ir::InsnId>* out)
{
    const std::vector<ir::InsnId>& order = cu.dom.order();
    ca::usize                      n     = 0;
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (reach[i] == 0) continue;
        const ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr || !rewrite_tools::isFallthroughGoto(cu, *in)) continue;
        ++n;
        if (out != nullptr) out->push_back(order[i]);
    }
    return n;
}

/// @brief 穿线批的落点重绑 + 无引用者销毁。
///
/// victim 自身可达，存活分支 / 异常表可以正跳到它——标签锚在 victim 上
/// 时 eraseBatch 锚定保护整批拒绝（b.main 族实证）。冗余 goto 语义 =
/// 直落落点，把入边标签重绑到落点 insn（句柄身份不变，分支 / 表项引用
/// 方不动）后跳转语义逐位等价。链形冗余（victim 的落点是批内另一
/// victim）沿目标链穿透到批外落点——每个冗余 goto 严格前进 3/5 字节，
/// 无环。
///
/// 无引用者销毁（2026-11-16 补）：victim 的目标标签锚在落点（存活指令）
/// 上，goto 擦除后若再无引用者则成为残渣——落点落在常量窗口中段时
/// ArithFold 的「窗口内除锚点外无标签」判据被这枚死标签挡死（b.main
/// hint 块池读实证，v27 链 const=0 复扫钉因）。引用者清点 = 批外分支
/// （in->target）+ 存活异常表（start/end/handler）；受害者自身的引用随
/// 批消亡，不保活标签。无引用者一律 destroyLabel，有引用者 bindLabel。
/// @return true = 处置完成可擦；false = 任一解析 / bindLabel / destroy
///          失败（调用方整体放弃本方法，fail-closed）。
bool rebindThreadingLabels(ir::CodeUnit& cu, const std::vector<ir::InsnId>& gotos)
{
    const auto inBatch = [&gotos](ir::InsnId id) {
        for (const ir::InsnId g : gotos) {
            if (g == id) return true;
        }
        return false;
    };
    // 引用者清点：批外分支 + 存活异常表。victim 自身的引用不算（随批
    // 消亡）。线性双扫，序确定。
    const auto referenced = [&cu, &inBatch](ir::LabelId lbl) {
        const std::vector<ir::InsnId>& order = cu.dom.order();
        for (const ir::InsnId id : order) {
            if (inBatch(id)) continue;
            const ir::Insn* in = cu.dom.peek(id);
            if (in != nullptr && in->target == lbl) return true;
        }
        for (const ir::CodeUnit::Exc& e : cu.exc) {
            if (e.start == lbl || e.end == lbl || e.handler == lbl) return true;
        }
        return false;
    };
    // 两阶段：先全量链解析（后一 victim 的链走查依赖前一 victim 尚未处置
    // 的目标标签），再统一处置标签。
    std::vector<ir::InsnId>         landings(gotos.size());
    std::vector<std::vector<ir::LabelId>> chainTargets(gotos.size());
    for (ca::usize gi = 0; gi < gotos.size(); ++gi) {
        const ir::Insn* cur = cu.dom.peek(gotos[gi]);
        if (cur == nullptr) return false;
        for (;;) {
            chainTargets[gi].push_back(cur->target);
            auto resolved = cu.dom.resolveLabel(cur->target);
            if (resolved.is_err()) return false;
            const ir::LabelTarget t = resolved.unwrap();
            if (t.isEnd()) return false;   // 收集面已保证非 End，防御口径
            landings[gi] = t.insn;
            if (!inBatch(t.insn)) break;
            cur = cu.dom.peek(t.insn);
            if (cur == nullptr) return false;
        }
    }
    for (ca::usize gi = 0; gi < gotos.size(); ++gi) {
        const ir::InsnId gid = gotos[gi];
        // 入边标签：锚在 victim 本体上的（存活分支 / 异常表跳到这里）。
        for (const ir::LabelId lbl : cu.dom.labelsAt(gid)) {
            if (referenced(lbl)) {
                if (cu.dom.bindLabel(lbl, landings[gi]).is_err()) return false;
            }
            else if (cu.dom.destroyLabel(lbl).is_err()) {
                return false;
            }
        }
        // 链上各成员的目标标签：锚在落点侧（存活指令），擦除后无引用者
        // 即残渣，销毁；有引用者搬到最终落点（链中段锚点会挡窗口判据）。
        for (const ir::LabelId lbl : chainTargets[gi]) {
            if (!cu.dom.validLabel(lbl)) continue;   // 链内共享已处置，跳过
            if (referenced(lbl)) {
                if (cu.dom.bindLabel(lbl, landings[gi]).is_err()) return false;
            }
            else if (cu.dom.destroyLabel(lbl).is_err()) {
                return false;
            }
        }
    }
    return true;
}

/// @brief 穿线面的声明帧退化路径（Strict 帧重算不支持时的唯一出路）。
///
/// b.main 族 20 法实证：junk 走廊在合并点留栈形不一致，frame_rewire 的
/// Strict 求解从代码真值重推断必炸（inconsistent stack depth at merge），
/// 但声明帧口径本就 verifier 有效（原 jar 可加载运行即证据），且同方法上
/// ConstantCall 的改写路径（prunePcAttrs + eraseBatch + lowerMethod，不重
/// 推断）实证可落地。擦 goto-to-fallthrough 不改任何剩余边的栈形：目标
/// 帧 = 落点帧，声明帧 relocation 后仍与代码真值一致。
///
/// 配套面只有 LNT/LVT 行剪（victim pc 上无 SMT 帧——分支指令不锚帧，帧
/// 锚在目标 pc 上而目标 = 落点存活指令，帧面原样保留 relocation）。
/// @return 穿线掉的 goto 数（0 = 未动）。
ca::usize threadGotosDeclaredFrames(ir::ClassSession& s, ca::usize methodIndex)
{
    const bool trace = std::getenv("GRUNT_DC_TRACE") != nullptr;
    auto       promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return 0;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return 0;

    const std::vector<ca::u8> reach = reachability(*cu);
    std::vector<ir::InsnId>   gotos;
    collectRedundantGotos(*cu, reach, &gotos);
    if (gotos.empty()) return 0;

    std::vector<ca::u32> victimPcs;
    victimPcs.reserve(gotos.size());
    for (const ir::InsnId id : gotos) {
        const ir::Insn* v = cu->dom.peek(id);
        if (v != nullptr && v->srcPc != jbc::ir::kNoPc) victimPcs.push_back(v->srcPc);
    }
    const ir::ClassUnit& unit = s.unit();
    const char*          fail = nullptr;
    if (methodIndex >= unit.methods.size()) fail = "method oob";
    else {
        const ca::usize backingIdx = unit.methods[methodIndex].backingIndex;
        if (backingIdx >= s.backing().methods.size()) fail = "backing oob";
        else if (!rewrite_tools::prunePcAttrs(s.backing(), s.backing().methods[backingIdx],
                                              victimPcs)) fail = "prunePcAttrs";
        else if (!rebindThreadingLabels(*cu, gotos)) fail = "rebind";
        else if (cu->dom.eraseBatch(gotos).is_err()) fail = "eraseBatch";
        else if (ir::lowerMethod(s, methodIndex).is_err()) fail = "lower";
    }
    if (fail != nullptr) {
        if (trace) {
            std::fprintf(stderr, "[dc] method#%zu 退化放弃: %s\n", methodIndex, fail);
        }
        return 0;
    }
    return gotos.size();
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
    // 冗余 goto 独立成批（穿线面与死区面各自 fail-closed，见批序注记）。
    std::vector<ir::InsnId> gotoVictims;
    const ca::usize redundant = collectRedundantGotos(*cu, reach, &gotoVictims);
    if (victims.empty() && gotoVictims.empty() && !excChanged) return 0;
    if (excChanged) cu->exc = std::move(liveExc);
    // 断言注记（不另设遍历，只关死区批）：被存活 Boundary 标签锚定或被
    // 存活指令跳转引用的指令必然可达（异常边已入 reachability），不会进
    // 死区受害者集合；受害者内部互指（死块内 goto 等）的 ControlFlow 标
    // 签不保护锚点，按 eraseBatch 契约随批消亡。穿线批的 victim 可达且可
    // 被存活分支正跳到，入边标签由 rebindThreadingLabels 先行搬移（见
    // 上）。若收敛逻辑漏处置某枚 Boundary 标签，eraseBatch 返回 Err，本
    // 函数保守放弃，fail-closed 兜底即断言。
    ca::usize killed = 0;
    // 穿线批先行（死区批异常时穿线成果不连坐）；入边标签先重绑到落点——
    // victim 可达，存活分支可正跳到它，锚在 victim 上的标签不重绑会被
    // 锚定保护整批拒绝（b.main 族实证）。
    if (!gotoVictims.empty()) {
        if (!rebindThreadingLabels(*cu, gotoVictims)) return 0;
        if (cu->dom.eraseBatch(gotoVictims).is_err()) return 0;
        killed += gotoVictims.size();
    }
    if (!victims.empty()) {
        // 死区批整批拒（锚定保护）不连坐穿线面：本轮回写只含穿线
        // 成果，死区留给下一轮 DC（链上多轮）。
        if (cu->dom.eraseBatch(victims).is_err()) return killed;
        killed += victims.size();
    }
    return killed;
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
            // 冗余无条件跳转与死指令同入改写面（混批 victims，eraseBatch
            // 一次清完）。
            const ca::usize redundant = collectRedundantGotos(*cu, reach, nullptr);
            if (dead + redundant != 0) {
                result.sites.push_back(Site{std::string(entry.uri), mi, dead + redundant});
            }
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
            if (safe.count(mi) == 0) {
                if (std::getenv("GRUNT_DC_TRACE") != nullptr) {
                    std::fprintf(stderr, "[dc] %s#%zu skip: roundTripUnsafe\n",
                                 uri.c_str(), mi);
                }
                continue;
            }
            ca::usize killed = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    killed = rewriteMethod(&cu);
                    return killed > 0;
                });
            if (!ok) {
                // Strict 帧重算不支持的方法（junk 栈形合并点）：穿线面退化
                // 为声明帧口径（CC 同构路径），死区面维持 fail-closed 不动。
                killed = threadGotosDeclaredFrames(s, mi);
                if (killed == 0) continue;
                if (std::getenv("GRUNT_DC_TRACE") != nullptr) {
                    std::fprintf(stderr, "[dc] %s#%zu 穿线退化路径 killed=%zu\n",
                                 uri.c_str(), mi, killed);
                }
            }
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
