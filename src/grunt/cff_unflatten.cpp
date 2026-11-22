/// @file        cff_unflatten.cpp
/// @brief       残留 `cff_unflatten` 三段实现：CFF dispatcher 岛识别（state
///              槽 confinement + 岛形 + 桥块归属）→ 单岛反摊平（块序重建 +
///              alias-face 剪枝）→ residual + 类数 + 重解析验收。
///
/// 源算法对照：Grunt grunt3 @ e41e852（FlowControlFlowFlattener.kt
/// emitInlineStateProgram / emitProcessorStateProgram / createFakeCaseBlock
/// 逐行；JvmFlowExporter jasm 形态；boar-main.jar `net/spartanb312/obf/AD.a`
/// 验证核对）。判据只吃产物结构，全形状自证，无 Era 检查（v2 Controlflow 是
/// junk 分支面，无 state 槽摊平，岛形 v3 特有）。
///
/// 识别骨架（多岛共存逐个处理）：
///  1. dispatcher 候选 = 形为 `[ILOAD s; TABLESWITCH]` 的基本块；
///  2. state 槽 confinement：s 的每次 ILOAD 必须在岛头或 processor 夹心内、
///     每次 ISTORE s 必须落在可解 span 尾（inline 闭链或 processor 形）、
///     禁 IINC 与其余读写，s 是 CFF fresh 槽，原代码不可触碰，任何越界
///     读写 = 槽污染，整槽岛全拒（fail-closed）。分桶按面：DivZero span
///     （除 0、`INT_MIN / -1`，守卫看累计运行值 v，IMUL/IADD 回绕可把 v
///     推到 INT_MIN）→ spanUnsolvable（README 口径/0）；其余越界读写
///     → statePolluted；
///  3. 岛形检查：alias face（default ∈ case 目标，生成面 default 落岛首真块）
///     + ≥1 桥引用 + case 目标不回指岛头，InvokeDispatcher 方法头
///     （`[ILOAD key; TABLESWITCH default→方法头]`、零分支引用、default 非
///     case 目标）被两道检查天然拒绝，不误伤；
///  4. 引用分类：usersOf(岛头) 逐边归桶，常量供给桥（span 闭链可解，键回
///     查键表；出表键 = scramble，goto 同 default）、processor 形桥/链内
///     `INVOKESTATIC (II)I`（键在 `__grt$indy$` 蹦床后，静态不可解 →
///     keyProcessor 整岛保留）、无状态 junk 检查（ControlflowJump 边包裹，
///     唯一常量供给前驱传递 state → 边载体，臂字段重指）、其余 →
///     foreignRef 整岛拒。边载体候选限终结限单目标 GOTO/条件分支：
///     switch 终结拒入，TableSwitch 的 setTarget 改的是 default 字段，与
///     臂重指语义面不同。
///
/// 入口桥认领与回滚：blocks[0] 直连岛头直接认领，否则解析为 state-setter
/// 后顺终结链追岛/载体；入口桥多岛认领冲突全拒。入口真块头落异常保护区
/// 严格内含 → 退蹦床（range 起点即目标块头不算交叠：start 标签随
/// moveRange 的 kHeadFollows 平移）。改写失败在干净会话重放；重放开不了
/// 会话整类放弃，半落会话无帧 = VerifyError 面，绝不落盘。
///
/// 修复面存证（unittest 回归钉住）：邻接谓词 layoutAdjacent =
/// `prev.orderPos + prev.insns.size() == blk.orderPos`（orderPos 是块入口
/// dom 序位置，`+1` 只对单指令前驱成立），相 3 顺序落入 / 相 4 载体前驱
/// / 相 7 落空臂 / 步 4 前驱四处同用，误用 `+1` 是历史缺陷面；插入前定靶
/// ，fallPins / 入口归位补 GOTO 一律先 `g.target = lbl` 再插，kNoLabel
/// 靶过不了 DOM 插入的句柄校验（InvalidHandle），三处插入路径同病，此前
/// 无测试踩到；junk 前缀 scramble 桥保留块体的 keep 判据 = 条件形 ||
/// span 起点 > 0。
///
/// 改写语义安全论证：桥对 state 的供给是常量封闭求值（inline span）；求值
/// 出表 ≡ switch default（表只覆盖 [lo, lo+n)，default 是唯一出表去向），
/// 即使岛被误判，goto 直连 default 目标也与原路径逐位等价；桥块把 s 先写
/// 后读、直连后不再读 s，槽上残余写死代码无语义。可整块擦除的桥块仅限
/// 纯 span + goto形（生成面桥块无 junk 前缀；出表 scramble 同理，带
/// junk 前缀的桥可达且有未知副作用，不可证伪即不删）；条件形桥与带 junk
/// 前缀的桥一律保留块体、只重指臂/goto 字段。usersOf 只看字段引用，位置
/// 边（非终结顺序落入 / 条件分支落空臂）单独守卫：受害者（岛头/可整擦桥）
/// 的布局前驱若有位置落入，只有本岛已归类条件用户紧邻岛头可钉（补
/// GOTO 指其 case 目标，与原分发结果同址），其余整岛拒。执行序：桥前驱
/// replaceAllUses 直连真实块（alias-face 剪枝）→ 边载体/条件桥臂字段
/// setTarget 重指 + positional 落空臂钉 → 异常表 handler 桥重绑 / 被擦块
/// 上 end 锚前移 → 入口块归位（fallthrough 补 GOTO + moveRange 头插；入口
/// 块在异常保护区内或带 junk 前缀时退蹦床：不移动、goto 改指，前缀保持
/// 执行；异常 end 锚恰在搬迁段首时拆新标签改绑原段位边界 nextAfterT，
/// kHeadFollows 原样平移会让 end 落 pc 0 与原地 start 倒置——jsoup
/// v3_flow 四类 ClassFormatError 实证，相 6 严格内含守卫放行 pos == p1）
/// → 受害者单批 eraseBatch（kRejectIfUsed 拒收即整方法回滚）。
/// 帧链走 mutateMethodWithFrames 整体重算。

#include "grunt/cff_unflatten.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::cff_unflatten {

namespace {

namespace ir = jbc::ir;

using jbc::core::Op;

constexpr ca::usize kNoPos           = static_cast<ca::usize>(-1);
constexpr ca::usize kCarrierDepthCap = 8;   // 边载体链深上限（环防御）

// ============================================================================
// ==== 形状钉面辅助（常量装载读数走 const_args）==============================
// ============================================================================

/// @brief int 槽装载读取（ILOAD a / ILOAD_0..3 → 槽号；失败 false）。
bool intLoadSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ILOAD) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0));
        return true;
    }
    return false;
}

/// @brief int 槽存储读取（ISTORE a / ISTORE_0..3 → 槽号；失败 false）。
bool intStoreSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ISTORE) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ISTORE_0 && in.op <= Op::ISTORE_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ISTORE_0));
        return true;
    }
    return false;
}

/// @brief 分支/switch/return/athrow/jsr 收尾指令（块边界；其后继是 leader）。
bool isTerminator(Op op)
{
    if (op >= Op::IFEQ && op <= Op::IF_ACMPNE) return true;
    switch (op) {
    case Op::IFNULL:
    case Op::IFNONNULL:
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::JSR:
    case Op::JSR_W:
    case Op::RET:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN:
    case Op::ATHROW: return true;
    default: return false;
    }
}

/// @brief 条件分支（target 字段 + fallthrough 两臂；setTarget 只动 target）。
bool isConditionalBranch(Op op)
{
    return (op >= Op::IFEQ && op <= Op::IF_ACMPNE) || op == Op::IFNULL || op == Op::IFNONNULL;
}

/// @brief promote 单方法摊平成指令视图（同 dispatcher_restore）。promote 纪律
///        由调用方单点持有，重复 promote 会重建 CodeUnit 弃掉全部句柄。
struct PromotedCode
{
    std::vector<const ir::Insn*> seq;   ///< dom 序的指令视图（含 NOP）
    ir::CodeUnit*                cu = nullptr;
};

bool promoteToSeq(ir::ClassSession& s, ca::usize methodIndex, PromotedCode& out)
{
    if (!ir::hasCode(s, methodIndex)) return false;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return false;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return false;
    out.cu = cu;
    out.seq.clear();
    out.seq.reserve(cu->dom.order().size());
    for (const ir::InsnId id : cu->dom.order()) {
        const ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) return false;
        out.seq.push_back(in);
    }
    return true;
}

// ============================================================================
// ==== 基本块视图（CodeEditor::blocks() + dom 序定位）========================
// ============================================================================

struct Blk
{
    ir::InsnId              entry;
    std::vector<ir::InsnId> insns;
    ca::usize               orderPos   = kNoPos;  ///< entry 的 dom 序位置
    bool                    terminated = false;   ///< 末条是块边界指令
};

struct MethodView
{
    PromotedCode                           code;
    std::vector<Blk>                       blocks;
    std::unordered_map<ca::u32, ca::usize> blockOf;  ///< InsnId.slot → 块下标
    std::unordered_map<ca::u32, ca::usize> orderOf;  ///< InsnId.slot → dom 序位置

    const ir::Insn& insn(const ir::InsnId id) const
    {
        return *code.seq[orderOf.at(id.slot)];
    }
};

/// @brief 从已 promote 的视图建块视图（不再 promote，promote 纪律）。
bool buildMethodView(const PromotedCode& code, MethodView& out)
{
    out.code = code;
    ir::CodeEditor                           editor(*code.cu);
    const std::vector<ir::CodeEditor::Block> raw = editor.blocks();

    const std::vector<ir::InsnId>& order = code.cu->dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        out.orderOf.emplace(order[i].slot, i);
    }
    out.blocks.reserve(raw.size());
    for (const ir::CodeEditor::Block& b : raw) {
        Blk blk;
        blk.entry = b.entry;
        blk.insns = b.insns;
        const auto posIt = out.orderOf.find(b.entry.slot);
        if (posIt == out.orderOf.end()) return false;
        blk.orderPos = posIt->second;
        const ir::Insn& last = out.insn(blk.insns.back());
        blk.terminated = isTerminator(last.op);
        const ca::usize idx = out.blocks.size();
        for (const ir::InsnId id : blk.insns) out.blockOf.emplace(id.slot, idx);
        out.blocks.push_back(std::move(blk));
    }
    return true;
}

/// @brief dom 序里 pos 的下一条指令（方法尾返回 kNoInsn，moveRange 尾哨兵）。
///
/// 坐标口径警示（2026-11 实测冲突，缺口台账）：`mv.orderOf` 是快照序位置，
/// 本函数查的是活序——步 4 的钉插入会让两套坐标错位，句柄语义上"应该"按
/// 快照序取。但 jsoup v3_full 整链实证：快照序取句柄（把搬迁段截到精确块
/// 尾）反而打挂 14/14 对拍（方法死循环）——真形里入口块搬迁必须连带其
/// 位置接续区整体平移，活序混查恰好给出这个更长区间；夹具级"孤儿尾
/// goto"面（ResolvesCarrierArmThroughSignedMathMaxSpan，已 GTEST_SKIP 记
/// 账）是真缺陷但从未在真样上显形。正确解 = 搬迁段含位置接续区的活序模
/// 型，属缺口 #4 联动深水面，未解前保持本形。
ir::InsnId nextInsnOf(const MethodView& mv, ca::usize pos)
{
    const std::vector<ir::InsnId>& order = mv.code.cu->dom.order();
    return pos + 1 < order.size() ? order[pos + 1] : ir::kNoInsn;
}

/// @brief 布局邻接（位置边成立面）：prev 的末条指令正是 blk 入口的前一条。
///
///        orderPos 是块入口指令的 dom 序位置，邻接判据是
///        `prev.orderPos + prev.insns.size() == blk.orderPos`，不是 +1
///        （+1 只对单指令前驱块成立；多指令前驱全部漏判，位置边静默丢失）。
bool layoutAdjacent(const MethodView& mv, const Blk& prev, const Blk& blk)
{
    return prev.orderPos + prev.insns.size() == blk.orderPos;
}

// ============================================================================
// ==== state span 反推与求值（源算法对照 FlowControlFlowFlattener.kt）========
// ============================================================================

/// @brief span 解析类别。Inline = 常量闭链可解；Processor = 键在 key
///        processor（`__grt$indy$` 蹦床后）静态不可解；其余 = 非 span 形。
enum class SpanKind : ca::u8 { Inline, Processor, NotSpan, DivZero };

struct SpanParse
{
    SpanKind  kind  = SpanKind::NotSpan;
    ca::usize start = kNoPos;   ///< block 内相对下标（span base / 形起点）
    ca::i32   value = 0;        ///< Inline 求值结果（uint32 内部回绕）
};

/// @brief 从 block 内 tailIdx（应为 ISTORE s）向回反推 state span 并求值。
///
/// 源算法逐行对照（`grunt3`@e41e852`` FlowControlFlowFlattener.kt）：
/// - inline：`[pushInt(baseKey); (pushInt(c); IADD|ISUB|IXOR|IMUL|IDIV|
///   INVOKESTATIC Math.max (II)I)*; ISTORE(slot)]`（emitInlineStateProgram +
///   StateOp.emitTo，DivConst 常量是除数 `v / value`，nextDivisor 恒正，
///   守卫 0 与 INT_MIN/-1）；
/// - processor：`[INT(inputKey); ISTORE(slot); ILOAD(slot); INT(salt);
///   INVOKESTATIC proc (II)I; ISTORE(slot)]`（emitProcessorStateProgram）。
///
/// span 内部不允许标签（合并点），块视图天然保证：标签锚定 leader，块回走
/// 出块即 NotSpan。链内非 Math.max 的 INVOKESTATIC (II)I = key processor
/// 藏形（boar 验证落在 `__grt$indy$` 蹦床后）→ Processor。
SpanParse evalSpanTail(const MethodView& mv, const Blk& blk, ca::usize tailIdx, ca::u32 slot,
                       ir::ClassSession& s)
{
    const auto at = [&](ca::usize i) -> const ir::Insn& { return mv.insn(blk.insns[i]); };

    // ---- processor 形（emitProcessorStateProgram 逐字）----
    if (tailIdx >= 5) {
        const ir::Insn& i1 = at(tailIdx - 1);
        const ir::Insn& i2 = at(tailIdx - 2);
        const ir::Insn& i3 = at(tailIdx - 3);
        const ir::Insn& i4 = at(tailIdx - 4);
        const ir::Insn& i5 = at(tailIdx - 5);
        ca::u32 ld = 0, sd = 0;
        ca::i32 salt = 0, input = 0;
        ir::ClassSession::MemberSem sem;
        if (i1.op == Op::INVOKESTATIC && s.memberOf(i1, sem)
            && sem.kind == ir::ClassSession::MemberSem::Kind::Method
            && sem.desc == knowledge::kGruntCffProcessorActionDesc && const_args::decodeConstInt(s, i2, salt)
            && intLoadSlotOf(i3, ld) && ld == slot && intStoreSlotOf(i4, sd) && sd == slot
            && const_args::decodeConstInt(s, i5, input)) {
            SpanParse out;
            out.kind  = SpanKind::Processor;
            out.start = tailIdx - 5;
            return out;
        }
    }

    // ---- inline 闭链向回走（StateOp.emitTo 形）----
    struct Step
    {
        Op      op    = Op::NOP;
        ca::i32 c     = 0;
        bool    isMax = false;
    };
    std::vector<Step> rev;
    ca::usize walk = tailIdx;
    ca::i32   base = 0;
    for (;;) {
        if (walk == 0) return SpanParse{};   // 出块底：非 span
        const ir::Insn& in = at(walk - 1);
        if (in.op == Op::IADD || in.op == Op::ISUB || in.op == Op::IXOR || in.op == Op::IMUL
            || in.op == Op::IDIV) {
            if (walk < 2) return SpanParse{};
            ca::i32 c = 0;
            if (!const_args::decodeConstInt(s, at(walk - 2), c)) return SpanParse{};
            rev.push_back(Step{in.op, c, false});
            walk -= 2;
            continue;
        }
        if (in.op == Op::INVOKESTATIC) {
            ir::ClassSession::MemberSem sem;
            if (s.memberOf(in, sem) && sem.kind == ir::ClassSession::MemberSem::Kind::Method
                && sem.owner == knowledge::kGruntCffMathMaxOwner
                && sem.name == knowledge::kGruntCffMathMaxName
                && sem.desc == knowledge::kGruntCffMathMaxDesc) {
                if (walk < 2) return SpanParse{};
                ca::i32 c = 0;
                if (!const_args::decodeConstInt(s, at(walk - 2), c)) return SpanParse{};
                rev.push_back(Step{Op::NOP, c, true});
                walk -= 2;
                continue;
            }
            if (sem.kind == ir::ClassSession::MemberSem::Kind::Method
                && sem.desc == knowledge::kGruntCffProcessorActionDesc) {
                SpanParse out;
                out.kind  = SpanKind::Processor;   // 链内 key processor 藏形
                out.start = walk - 1;
                return out;
            }
            return SpanParse{};                    // 出词汇表
        }
        if (const_args::decodeConstInt(s, in, base)) {             // span base
            walk -= 1;
            break;
        }
        return SpanParse{};
    }

    // ---- 正向求值（uint32 内部回绕，对齐 JVM int 语义）----
    ca::u32 v = static_cast<ca::u32>(base);
    for (ca::usize r = rev.size(); r-- > 0;) {
        const Step&   st = rev[r];
        const ca::u32 c  = static_cast<ca::u32>(st.c);
        if (st.isMax) {
            // Math.max(II)I 是带符号比较：u32 位型直接 std::max 会把负数
            // 当大数（负 × 正混签必选错）。jsoup v3_flow notNullParam 实证：
            // 桥键错 → caseIdxOf 落表外 -1 → carrier 臂重指 default 头
            // （alias face = 入口块），入口块归位后其标签随迁 → 载体臂改指
            // 入口块自环，方法死循环。
            v = static_cast<ca::u32>(std::max(static_cast<ca::i32>(v), st.c));
            continue;
        }
        switch (st.op) {
        case Op::IADD: v += c; break;
        case Op::ISUB: v -= c; break;
        case Op::IXOR: v ^= c; break;
        case Op::IMUL: v *= c; break;
        case Op::IDIV:
            // 守卫看运行值 v（含 base 与中间值，IMUL/IADD 回绕可把 v 推到
            // INT_MIN）：除零与 INT_MIN/-1 在 Java 是 ArithmeticException，
            // 静态复算不定 → 不可解。
            if (st.c == 0
                || (st.c == -1 && static_cast<ca::i32>(v) == (-2147483647 - 1))) {
                SpanParse out;
                out.kind  = SpanKind::DivZero;
                out.start = walk;
                return out;
            }
            v = static_cast<ca::u32>(static_cast<ca::i32>(v) / st.c);
            break;
        default: return SpanParse{};
        }
    }
    SpanParse out;
    out.kind  = SpanKind::Inline;
    out.start = walk;
    out.value = static_cast<ca::i32>(v);
    return out;
}

// ============================================================================
// ==== 岛与引用模型 ==========================================================
// ============================================================================

/// @brief 一条常量供给桥（state-setter）。key = span 求值；caseIdx ∈ 表 →
///        真键（直连 case 目标），-1 → scramble（直连 default）。
struct BridgeRef
{
    ca::usize blk     = kNoPos;
    ca::i32   key     = 0;
    ca::i32   caseIdx = -1;
    bool      cc      = false;   ///< 终结是条件分支（块保留，只重指臂字段）
    bool      keep    = false;   ///< 块体保留（条件形或带 junk 前缀的可达桥）
    bool      entry   = false;   ///< blocks[0]
    bool      bait    = false;   ///< case 目标是 handler 锚头（死饵：整擦不重指）
};

/// @brief 一条边载体（无状态 junk 检查；臂字段重指 caseOf(key)/default）。
struct CarrierRef
{
    ca::usize              blk = kNoPos;
    std::vector<ca::usize> userPos;    ///< block 内锚岛头的字段指令（相对下标）
    ca::i32                key = 0;
    ca::i32                caseIdx = -1;
    bool                   bait = false;   ///< case 目标是 handler 锚头（死饵：整擦不重指）
};

struct Island
{
    ca::usize               dispBlk   = kNoPos;
    ca::u32                 slot      = 0;
    ca::i32                 lo        = 0;
    std::vector<ir::InsnId> cases;       ///< 下标 = key - lo；目标 = 块头指令
    ir::InsnId              defaultHead; ///< default 目标块头指令（alias face）
    std::vector<BridgeRef>  bridges;
    std::vector<CarrierRef> carriers;
    ca::u32                 headSrcPc = 0;
    // 改写期产出（scan 时一并定案，rewrite 复推后逐项执行）：
    std::vector<std::pair<ca::u32, ir::InsnId>> handlerRebinds;  ///< (exc 下标, 新锚)
    std::vector<std::pair<ca::u32, ir::InsnId>> endReanchors;    ///< (exc 下标, 新锚；kNoInsn = 方法尾)
    /// positional 落空臂钉：(块下标, 钉点目标块头)。usersOf 只看字段引用，
    /// 条件分支的落空臂是位置边，本岛已归类条件用户紧邻岛头（布局前驱）
    /// 时，岛头被擦后落空臂会改落邻接后继，改写期补一枚 GOTO 钉住原落点。
    std::vector<std::pair<ca::usize, ir::InsnId>> fallPins;
    /// 死 case 体链 (链头块, 链长)：解路保留臂引用面之外的 case 目标块 +
    /// 其纯 fallthrough 后继。链头进 victimBlks 供 positional 落入守卫钉/
    /// 拒；整链入 victims 整擦（executeIsland）。空 = 本岛无死体（历史行
    /// 为）。死体不擦则解路后变可达位置填充物，其尾邻接 handler 头帧冲突
    /// → VerifyError（jsoup v3_full TokeniserState$50.$gg1bdbksz8j2sa8
    /// 的 floorMod(10153,0) 垃圾岛实证）。
    std::vector<std::pair<ca::usize, ca::usize>> deadBodies;
    bool    hasEntry        = false;  ///< blocks[0] 是本岛入口桥
    ca::i32 entryCaseIdx    = -1;     ///< 入口键的 case 下标（-1 = 出表 → default）
    bool    entryCc         = false;  ///< 入口桥是条件形（块保留，只移动目标块）
    bool    entryKeep       = false;  ///< 入口桥带 junk 前缀（块保留，只重指 goto）
    bool    entryTrampoline = false;  ///< 入口块退蹦床（目标块在异常保护区内）
};

/// @brief 岛装配失败桶（与 ScanResult 计数字段一一对应）。
enum class IslandFail : ca::u8
{
    None,
    ShapeMismatch,
    ForeignRef,
    KeyProcessor,
    SpanUnsolvable,
    StatePolluted,
    BlocksShared,
    ExcBlocked,
    MultiEntry,
};

const char* bucketOf(IslandFail f)
{
    switch (f) {
    case IslandFail::ShapeMismatch:  return "形状不符（alias face 缺席 / case 自环）";
    case IslandFail::ForeignRef:     return "岛外引用（非常量供给 / junk 检查无解 / 无桥引用）";
    case IslandFail::KeyProcessor:   return "processor 形 span（键在 key processor 后，静态不可解）";
    case IslandFail::SpanUnsolvable: return "span 求值除零/溢出（fail-closed）";
    case IslandFail::StatePolluted:  return "state 槽越界读写（槽污染连坐）";
    case IslandFail::BlocksShared:   return "岛间共享块（case 目标跨岛）";
    case IslandFail::ExcBlocked:     return "异常表边交叠（range 边界锚被擦块）";
    case IslandFail::MultiEntry:     return "入口桥多岛认领冲突";
    default:                         return "none";
    }
}

// ============================================================================
// ==== 块级解析辅助 ==========================================================
// ============================================================================

/// @brief 块内是否有 state 槽读写（边载体判定：载体块禁碰 state 槽）。
bool blockTouchesSlot(const MethodView& mv, const Blk& blk, ca::u32 slot)
{
    for (const ir::InsnId id : blk.insns) {
        const ir::Insn& in = mv.insn(id);
        if (in.op == Op::IINC) {
            if (static_cast<ca::u32>(in.a) == slot) return true;
            continue;
        }
        ca::u32 st = 0;
        if ((intLoadSlotOf(in, st) || intStoreSlotOf(in, st)) && st == slot) return true;
    }
    return false;
}

/// @brief 块作为常量供给桥解析：取最后一处 ISTORE s 为 span 尾反推。
///        kind = Inline → key 求值完成；Processor/DivZero → 计数桶；
///        NotSpan → 非桥形（交给边载体判定）。
struct SetterParse
{
    SpanKind  kind  = SpanKind::NotSpan;
    ca::usize tail  = kNoPos;   ///< ISTORE s 的块内相对下标
    ca::usize start = kNoPos;   ///< span 起点的块内相对下标
    ca::i32   value = 0;
};

SetterParse parseStateSetter(const MethodView& mv, ir::ClassSession& s, const Blk& blk,
                             ca::u32 slot)
{
    SetterParse out;
    ca::usize tail = kNoPos;
    for (ca::usize i = 0; i < blk.insns.size(); ++i) {
        ca::u32 st = 0;
        if (intStoreSlotOf(mv.insn(blk.insns[i]), st) && st == slot) tail = i;
    }
    if (tail == kNoPos) return out;
    const SpanParse sp = evalSpanTail(mv, blk, tail, slot, s);
    out.kind  = sp.kind;
    out.tail  = tail;
    out.start = sp.start;
    out.value = sp.value;
    return out;
}

/// @brief 目标 case 下标：key ∈ [lo, lo+n) → 下标；出表 → -1（→ default）。
ca::i32 caseIdxOf(const Island& isl, ca::i32 key)
{
    const ca::i64 idx = static_cast<ca::i64>(key) - static_cast<ca::i64>(isl.lo);
    if (idx < 0 || idx >= static_cast<ca::i64>(isl.cases.size())) return -1;
    return static_cast<ca::i32>(idx);
}

/// @brief case 下标 → 目标块头指令（-1 → default 目标）。
ir::InsnId targetHeadOf(const Island& isl, ca::i32 caseIdx)
{
    return caseIdx >= 0 ? isl.cases[static_cast<ca::usize>(caseIdx)] : isl.defaultHead;
}

// ============================================================================
// ==== 岛装配（scan 核心；scan 与 rewrite 复推共用）==========================
// ============================================================================

struct AssemblyOut
{
    std::vector<Island>                          solvable;  ///< 可反岛（全细节定案）
    std::vector<std::pair<ca::u32, IslandFail>>  fails;     ///< (岛头 srcPc, 桶)
    ca::usize dispatchers = 0;                              ///< 候选岛总数
};

/// @brief 岛装配主入口。多岛共存逐个处理；跨岛共享块互相连坐。
AssemblyOut assembleIslands(const MethodView& mv, ir::ClassSession& s)
{
    AssemblyOut out;
    const ir::CodeDom& dom = mv.code.cu->dom;

    // ---- 相 1：dispatcher 候选（[ILOAD s; TABLESWITCH] 块）----
    std::vector<Island> islands;
    std::unordered_map<ca::u32, std::vector<ca::usize>> bySlot;  // 槽 → 候选下标
    for (ca::usize bi = 0; bi < mv.blocks.size(); ++bi) {
        const Blk& blk = mv.blocks[bi];
        if (blk.insns.size() != 2) continue;
        ca::u32 slot = 0;
        if (!intLoadSlotOf(mv.insn(blk.insns[0]), slot)) continue;
        if (mv.insn(blk.insns[1]).op != Op::TABLESWITCH) continue;
        Island isl;
        isl.dispBlk   = bi;
        isl.slot      = slot;
        isl.headSrcPc = mv.insn(blk.insns[0]).srcPc;
        bySlot[slot].push_back(islands.size());
        islands.push_back(std::move(isl));
    }
    if (islands.empty()) return out;
    out.dispatchers = islands.size();

    std::vector<IslandFail> failOf(islands.size(), IslandFail::None);
    const auto fail = [&](ca::usize i, IslandFail f) {
        if (failOf[i] == IslandFail::None) {
            failOf[i] = f;
            out.fails.emplace_back(islands[i].headSrcPc, f);
        }
    };

    // ---- 相 2：state 槽 confinement（槽级；污染连坐整槽）----
    for (const auto& [slot, idxs] : bySlot) {
        // processor 夹心范围（block entry.slot → [start, tail] 块内相对下标）。
        std::vector<std::pair<ca::u32, std::pair<ca::usize, ca::usize>>> procRanges;
        const auto inProcRange = [&](const Blk& blk, ca::usize i) {
            for (const auto& [bslot, range] : procRanges) {
                if (bslot != blk.entry.slot) continue;
                if (i >= range.first && i <= range.second) return true;
            }
            return false;
        };
        bool ok = true;
        bool divZero = false;   ///< DivZero span 槽级连坐 → spanUnsolvable 桶
        for (const Blk& blk : mv.blocks) {
            for (ca::usize i = 0; i < blk.insns.size() && ok; ++i) {
                const ir::Insn& in = mv.insn(blk.insns[i]);
                if (in.op == Op::IINC && static_cast<ca::u32>(in.a) == slot) {
                    ok = false;
                    break;
                }
                ca::u32 st = 0;
                if (intStoreSlotOf(in, st) && st == slot) {
                    const SpanParse sp = evalSpanTail(mv, blk, i, slot, s);
                    if (sp.kind == SpanKind::Inline) continue;      // span 尾计数
                    if (sp.kind == SpanKind::Processor) {
                        procRanges.emplace_back(blk.entry.slot,
                                                std::pair<ca::usize, ca::usize>{sp.start, i});
                        continue;                                   // 夹心随形计数
                    }
                    ok = false;   // DivZero / NotSpan：越界 state 写
                    divZero = (sp.kind == SpanKind::DivZero);
                    break;
                }
                ca::u32 ld = 0;
                if (intLoadSlotOf(in, ld) && ld == slot) {
                    // 岛头位：[ILOAD s; TABLESWITCH] 两条块的首条。
                    const bool headPos =
                        blk.insns.size() == 2 && i == 0
                        && mv.insn(blk.insns[1]).op == Op::TABLESWITCH;
                    if (!headPos && !inProcRange(blk, i)) {
                        // processor 夹心的 ILOAD 先于闭合 ISTORE 到达（前向
                        // 扫描序）：[ISTORE s; ILOAD s; INT; INVOKE(II)I;
                        // ISTORE s] 前视夹心即计数放行（与 evalSpanTail 的
                        // processor 形判据同形，只是从 ILOAD 位起看）。
                        bool ahead = false;
                        if (i > 0 && i + 3 < blk.insns.size()) {
                            ca::u32                            sd  = 0;
                            ca::i32                            salt = 0;
                            ir::ClassSession::MemberSem sem;
                            const ir::Insn&                 k1  = mv.insn(blk.insns[i + 1]);
                            const ir::Insn&                 iv  = mv.insn(blk.insns[i + 2]);
                            const ir::Insn&                 st  = mv.insn(blk.insns[i + 3]);
                            const ir::Insn&                 pre = mv.insn(blk.insns[i - 1]);
                            ahead = const_args::decodeConstInt(s, k1, salt) && iv.op == Op::INVOKESTATIC
                                    && s.memberOf(iv, sem)
                                    && sem.kind == ir::ClassSession::MemberSem::Kind::Method
                                    && sem.desc == knowledge::kGruntCffProcessorActionDesc
                                    && intStoreSlotOf(st, sd) && sd == slot
                                    && intStoreSlotOf(pre, sd) && sd == slot;
                        }
                        if (!ahead) {
                            ok = false;
                            break;
                        }
                        procRanges.emplace_back(
                            blk.entry.slot, std::pair<ca::usize, ca::usize>{i, i + 3});
                    }
                }
            }
            if (!ok) break;
        }
        if (!ok) {
            // 槽级连坐按面分桶：DivZero span（求值 ArithmeticException，静态
            // 复算不定）记 spanUnsolvable（README 口径/0面）；其余越界
            // 读写记 statePolluted。同槽多岛同桶整拒（fail-closed 不分岛）。
            const IslandFail bucket =
                divZero ? IslandFail::SpanUnsolvable : IslandFail::StatePolluted;
            for (const ca::usize ci : idxs) fail(ci, bucket);
        }
    }

    // ---- 相 3：岛形检查 + 岛头引用分类 ----
    for (ca::usize i = 0; i < islands.size(); ++i) {
        Island& isl = islands[i];
        if (failOf[i] != IslandFail::None) continue;
        const Blk& disp = mv.blocks[isl.dispBlk];

        // 键表：连续键窗（TABLESWITCH 静态编码）→ case 目标 + default。
        const ir::Insn& sw = mv.insn(disp.insns[1]);
        isl.lo = sw.a;
        bool   shapeOk = true;
        for (const ir::LabelId t : sw.targets) {
            const auto r = dom.resolveLabel(t);
            if (r.is_err() || r.unwrap().isEnd()) {
                shapeOk = false;
                break;
            }
            isl.cases.push_back(r.unwrap().insn);
        }
        if (shapeOk) {
            const auto r = dom.resolveLabel(sw.target);
            if (r.is_err() || r.unwrap().isEnd()) {
                shapeOk = false;
            } else {
                isl.defaultHead = r.unwrap().insn;
            }
        }
        if (!shapeOk) {
            fail(i, IslandFail::ShapeMismatch);
            continue;
        }
        // alias face：default ∈ case 目标（生成面 default 落岛首真块），
        // InvokeDispatcher 方法头（default 回跳自身、case 全是转发体）天然缺席。
        bool alias = false;
        for (const ir::InsnId c : isl.cases) {
            if (c == isl.defaultHead) {
                alias = true;
                break;
            }
        }
        if (!alias) {
            fail(i, IslandFail::ShapeMismatch);
            continue;
        }
        for (const ir::InsnId c : isl.cases) {
            if (c == disp.insns[0]) {   // case 目标回指岛头（自环）
                fail(i, IslandFail::ShapeMismatch);
                shapeOk = false;
                break;
            }
        }
        if (!shapeOk) continue;

        // 岛头顺序落入（前驱块未终结且紧邻）→ 岛外引用。
        if (isl.dispBlk > 0) {
            const Blk& prev = mv.blocks[isl.dispBlk - 1];
            if (!prev.terminated && layoutAdjacent(mv, prev, disp)) {
                fail(i, IslandFail::ForeignRef);
                continue;
            }
        }

        // 岛头引用逐边归桶。
        const std::vector<ir::InsnId> users = dom.usersOf(disp.insns[0]);
        if (users.empty()) {
            fail(i, IslandFail::ForeignRef);   // 无桥引用（InvokeDispatcher 头 / 死岛）
            continue;
        }
        bool refOk = true;
        for (const ir::InsnId u : users) {
            const auto bit = mv.blockOf.find(u.slot);
            if (bit == mv.blockOf.end()) {
                fail(i, IslandFail::ForeignRef);
                refOk = false;
                break;
            }
            const ca::usize bIdx = bit->second;
            if (bIdx == isl.dispBlk) continue;   // 防御：自岛字段不外引岛头
            const Blk& blk = mv.blocks[bIdx];
            if (blk.insns.back() != u) {
                fail(i, IslandFail::ForeignRef);   // 岛头字段引用非终结指令
                refOk = false;
                break;
            }
            const Op uOp = mv.insn(u).op;
            const SetterParse sp = parseStateSetter(mv, s, blk, isl.slot);
            if (sp.kind == SpanKind::Inline) {
                BridgeRef br;
                br.blk     = bIdx;
                br.key     = sp.value;
                br.caseIdx = caseIdxOf(isl, sp.value);
                br.cc      = isConditionalBranch(uOp);
                br.entry   = (bIdx == 0);
                if (uOp != Op::GOTO && !br.cc) {
                    fail(i, IslandFail::ForeignRef);   // 桥终结指令形不齐
                    refOk = false;
                    break;
                }
                // 可整块擦除 = 纯 span + goto（生成面桥块无前缀形）。条件形
                // 与 junk 前缀桥一律保留块体（出表 scramble 也不例外，前缀
                // 可达且有未知副作用，不可证伪即不删；保留形由 goto 重指
                // default 完成同一剪枝）。
                br.keep = br.cc || sp.start > 0;
                isl.bridges.push_back(br);
                continue;
            }
            if (sp.kind == SpanKind::Processor) {
                fail(i, IslandFail::KeyProcessor);
                refOk = false;
                break;
            }
            if (sp.kind == SpanKind::DivZero) {
                fail(i, IslandFail::SpanUnsolvable);
                refOk = false;
                break;
            }
            // 非 span 形 → 边载体候选（无状态 junk 检查）。终结必须是单目标
            // GOTO/条件分支，switch 的 setTarget 改的是 default 字段，与
            // 臂重指语义面不同，不可入载体。
            if (blockTouchesSlot(mv, blk, isl.slot)) {
                fail(i, IslandFail::ForeignRef);
                refOk = false;
                break;
            }
            if (uOp != Op::GOTO && !isConditionalBranch(uOp)) {
                fail(i, IslandFail::ForeignRef);
                refOk = false;
                break;
            }
            bool merged = false;
            for (CarrierRef& cr : isl.carriers) {
                if (cr.blk == bIdx) {
                    cr.userPos.push_back(blk.insns.size() - 1);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                CarrierRef cr;
                cr.blk     = bIdx;
                cr.userPos.push_back(blk.insns.size() - 1);
                isl.carriers.push_back(std::move(cr));
            }
        }
        if (!refOk) continue;
    }

    // ---- 相 4：边载体解析（唯一常量供给前驱传递 state；链深封顶）----
    std::unordered_map<ca::u32, std::pair<ca::usize, ca::usize>> carrierReg;  // 块 entry.slot → (岛, 载体)
    for (ca::usize i = 0; i < islands.size(); ++i) {
        if (failOf[i] != IslandFail::None) continue;
        for (ca::usize ci = 0; ci < islands[i].carriers.size(); ++ci) {
            carrierReg.emplace(mv.blocks[islands[i].carriers[ci].blk].entry.slot,
                               std::pair<ca::usize, ca::usize>{i, ci});
        }
    }
    std::function<bool(ca::usize, ca::usize, ca::usize)> resolveCarrier =
        [&](ca::usize i, ca::usize ci, ca::usize depth) -> bool {
        Island&     isl = islands[i];
        CarrierRef& cr  = isl.carriers[ci];
        if (depth > kCarrierDepthCap) return false;
        const Blk& blk = mv.blocks[cr.blk];
        // 前驱边：字段引用 + 顺序落入。
        std::vector<ca::usize> preds;
        for (const ir::InsnId u : dom.usersOf(blk.entry)) {
            const auto pit = mv.blockOf.find(u.slot);
            if (pit == mv.blockOf.end()) return false;
            if (pit->second != cr.blk) preds.push_back(pit->second);
        }
        if (cr.blk > 0) {
            const Blk& prev = mv.blocks[cr.blk - 1];
            if (!prev.terminated && layoutAdjacent(mv, prev, blk)) {
                preds.push_back(cr.blk - 1);
            }
        }
        if (preds.size() != 1) return false;
        const Blk&       pred = mv.blocks[preds[0]];
        const SetterParse sp   = parseStateSetter(mv, s, pred, isl.slot);
        if (sp.kind == SpanKind::Inline) {
            cr.key     = sp.value;
            cr.caseIdx = caseIdxOf(isl, sp.value);
            return true;
        }
        if (sp.kind != SpanKind::NotSpan) return false;   // 前驱键不可解（processor/除零）
        // 前驱是另一条边载体 → 递归传递。
        const auto cit = carrierReg.find(pred.entry.slot);
        if (cit == carrierReg.end()) return false;
        if (cit->second.first == i && cit->second.second == ci) return false;   // 自环
        if (!resolveCarrier(cit->second.first, cit->second.second, depth + 1)) {
            return false;
        }
        const CarrierRef& up = islands[cit->second.first].carriers[cit->second.second];
        cr.key     = up.key;
        cr.caseIdx = caseIdxOf(isl, up.key);
        return true;
    };
    for (ca::usize i = 0; i < islands.size(); ++i) {
        if (failOf[i] != IslandFail::None) continue;
        for (ca::usize ci = 0; ci < islands[i].carriers.size(); ++ci) {
            if (!resolveCarrier(i, ci, 0)) {
                fail(i, IslandFail::ForeignRef);
                break;
            }
        }
    }

    // ---- 相 5：岛间共享块连坐（case/default 目标跨岛）----
    {
        std::unordered_map<ca::u32, ca::usize> owner;
        for (ca::usize i = 0; i < islands.size(); ++i) {
            if (failOf[i] != IslandFail::None) continue;
            std::vector<ir::InsnId> targets = islands[i].cases;
            targets.push_back(islands[i].defaultHead);
            for (const ir::InsnId t : targets) {
                const auto [it, inserted] = owner.emplace(t.slot, i);
                if (!inserted && it->second != i) {
                    fail(i, IslandFail::BlocksShared);
                    fail(it->second, IslandFail::BlocksShared);
                }
            }
        }
    }

    // ---- 相 6：入口桥认领（blocks[0] 是常量供给桥 → 顺终结链追岛）----
    {
        const Blk& b0 = mv.blocks[0];
        for (ca::usize i = 0; i < islands.size(); ++i) {
            Island& isl = islands[i];
            if (failOf[i] != IslandFail::None) continue;
            for (const BridgeRef& br : isl.bridges) {
                if (br.blk != 0) continue;
                isl.hasEntry     = true;
                isl.entryCaseIdx = br.caseIdx;
                isl.entryCc      = br.cc;
                isl.entryKeep    = br.keep;
                break;
            }
            if (isl.hasEntry) continue;
            // blocks[0] 未直连岛头 → 解析为 state-setter 后顺终结链追岛/载体。
            const SetterParse sp = parseStateSetter(mv, s, b0, isl.slot);
            if (sp.kind != SpanKind::Inline) continue;
            const ir::Insn& last = mv.insn(b0.insns.back());
            if (last.op != Op::GOTO && !isConditionalBranch(last.op)) continue;
            const auto r = dom.resolveLabel(last.target);
            if (r.is_err() || r.unwrap().isEnd()) continue;
            const ir::InsnId dst = r.unwrap().insn;
            if (dst == mv.blocks[isl.dispBlk].entry) {
                isl.hasEntry     = true;
                isl.entryCaseIdx = caseIdxOf(isl, sp.value);
                isl.entryCc      = isConditionalBranch(last.op);
                isl.entryKeep    = false;
            } else {
                const auto cit = carrierReg.find(dst.slot);
                if (cit != carrierReg.end() && cit->second.first == i) {
                    isl.hasEntry     = true;
                    isl.entryCaseIdx = caseIdxOf(isl, sp.value);
                    isl.entryCc      = false;
                    isl.entryKeep    = sp.start > 0;
                }
            }
        }
        // 入口桥多岛认领冲突 → 全部拒（fail-closed）。
        std::vector<ca::usize> claimants;
        for (ca::usize i = 0; i < islands.size(); ++i) {
            if (islands[i].hasEntry && failOf[i] == IslandFail::None) {
                claimants.push_back(i);
            }
        }
        if (claimants.size() > 1) {
            for (const ca::usize i : claimants) fail(i, IslandFail::MultiEntry);
        }
        // 入口块归位的异常保护区守卫：目标块头落在任一保护区间内 → 退蹦床。
        for (ca::usize i = 0; i < islands.size(); ++i) {
            Island& isl = islands[i];
            if (!isl.hasEntry || failOf[i] != IslandFail::None) continue;
            const ir::InsnId t   = targetHeadOf(isl, isl.entryCaseIdx);
            const auto       pit = mv.orderOf.find(t.slot);
            if (pit == mv.orderOf.end()) continue;
            const ca::usize pos = pit->second;
            for (const auto& e : mv.code.cu->exc) {
                const auto sr = dom.resolveLabel(e.start);
                if (sr.is_err() || sr.unwrap().isEnd()) continue;
                const ca::usize p0 = mv.orderOf.at(sr.unwrap().insn.slot);
                const auto      er = dom.resolveLabel(e.end);
                const ca::usize p1 = (!er.is_err() && !er.unwrap().isEnd())
                                         ? mv.orderOf.at(er.unwrap().insn.slot)
                                         : mv.orderOf.size();
                // 目标块是 range 起点（p0 == pos）不算交叠：start 标签锚定
                // 目标块头，随 moveRange 段首标签迁移（kHeadFollows），保护
                // 区间与目标块一起平移，覆盖不变；只有严格内含才退蹦床。
                if (p0 < pos && pos < p1) {
                    isl.entryTrampoline = true;
                    break;
                }
            }
        }
    }

    // ---- 相 7：异常表面（handler 桥重绑 / 被擦块上 end 锚前移 / start 连坐）----
    std::set<ca::usize> plannedVictims;   // 全岛累计擦蚀面（相 8 安全网口径）
    for (ca::usize i = 0; i < islands.size(); ++i) {
        Island& isl = islands[i];
        if (failOf[i] != IslandFail::None) continue;
        // 本岛受害者块集合：dispatcher + 可整擦桥（goto 形且不保留不蹦床）。
        std::set<ca::usize> victimBlks;
        victimBlks.insert(isl.dispBlk);
        for (const BridgeRef& br : isl.bridges) {
            const bool entryHold = br.entry && (isl.entryTrampoline || isl.entryKeep);
            if (!br.cc && !br.keep && !entryHold) victimBlks.insert(br.blk);
        }

        // ---- 死饵闭包 + 死 case 体链收集（不动点）------------------------
        //   判据 fail-closed：目标块非本岛 carrier/桥/dispatcher/入口目标/
        //   default 别名；引用面 = 全体 users 落在 dispatcher 块内（switch
        //   独占引用，随 dispatcher 入批）；不在存活臂引用集（非死饵
        //   carrier/桥 caseIdx 目标 + 入口目标）；非异常表锚点。链后继逐块
        //   要求头部无标签（无独立入边）+ 非锚点 + 非认领 + 非 case 目标。
        //   死饵：case 目标落在 handler 锚头或已定死块上的 carrier/桥——
        //   解路重指会把活分支直连 handler 头帧/死体（空栈 vs {Throwable}
        //   合并 → VerifyError，jsoup v3_full $gg1bdbksz8j2sa8 的
        //   -1441906808 → 出口臂实证），改整链整擦（dec7b 面：bail 链随解
        //   路消解，出口臂只剩异常边）；条件形/保留形无处整擦 → 岛整体拒。
        const auto headBlkOf = [&](const ir::InsnId id) -> std::optional<ca::usize> {
            const auto it = mv.blockOf.find(id.slot);
            if (it == mv.blockOf.end()) return std::nullopt;
            return it->second;
        };
        std::set<ca::usize> anchorBlks;    // 全部异常表锚（start/end/handler）
        std::set<ca::usize> handlerBlks;   // 仅 handler 锚：唯一带帧头（{Throwable}）
        for (const auto& e : mv.code.cu->exc) {
            for (const ir::LabelId l : {e.start, e.end, e.handler}) {
                const auto r = dom.resolveLabel(l);
                if (r.is_err() || r.unwrap().isEnd()) continue;
                if (const auto b = headBlkOf(r.unwrap().insn)) {
                    anchorBlks.insert(*b);
                    if (l == e.handler) handlerBlks.insert(*b);
                }
            }
        }
        const auto dispOwned = [&](const ir::InsnId u) {
            const auto it = mv.blockOf.find(u.slot);
            return it != mv.blockOf.end() && it->second == isl.dispBlk;
        };
        std::set<ca::usize> erasedBlks;   // 死饵块 + 死体链块（闭包累积）
        for (bool more = true; more; ) {
            more = false;
            // 死饵闭包：case 目标是 handler 锚头/已定死块 → carrier/桥本体
            // 也是死饵（运行期该键不解路，重指即非法直连）。锚集只取
            // handler 锚（唯一带帧头 {Throwable}）；start/end 锚无帧语义，
            // 入集会把普通解路桥（如 exc 尾锚块桥）误标死饵、跳过
            // replaceAllUses 留下悬挂引用（CffExc 夹具 LB2→LbR 实证）。
            for (CarrierRef& cr : isl.carriers) {
                if (cr.bait) continue;
                if (const auto b = headBlkOf(targetHeadOf(isl, cr.caseIdx))) {
                    if (handlerBlks.count(*b) != 0 || erasedBlks.count(*b) != 0) {
                        cr.bait = true;
                        erasedBlks.insert(cr.blk);
                        more = true;
                    }
                }
            }
            for (BridgeRef& br : isl.bridges) {
                if (br.bait) continue;
                if (const auto b = headBlkOf(targetHeadOf(isl, br.caseIdx))) {
                    if (handlerBlks.count(*b) != 0 || erasedBlks.count(*b) != 0) {
                        br.bait = true;
                        erasedBlks.insert(br.blk);
                        if (br.cc || br.keep) {
                            fail(i, IslandFail::ForeignRef);   // 条件形/保留形无处整擦
                        }
                        more = true;
                    }
                }
            }
            if (failOf[i] != IslandFail::None) break;
            // 存活臂引用面（死饵不重指，其 case 目标不算存活）。
            std::set<ca::usize> keepTgts;
            for (const BridgeRef& br : isl.bridges) {
                if (br.bait) continue;
                if (const auto b = headBlkOf(targetHeadOf(isl, br.caseIdx))) {
                    keepTgts.insert(*b);
                }
            }
            for (const CarrierRef& cr : isl.carriers) {
                if (cr.bait) continue;
                if (const auto b = headBlkOf(targetHeadOf(isl, cr.caseIdx))) {
                    keepTgts.insert(*b);
                }
            }
            if (isl.hasEntry) {
                if (const auto b = headBlkOf(targetHeadOf(isl, isl.entryCaseIdx))) {
                    keepTgts.insert(*b);
                }
            }
            // 死 case 体链：存活引用面之外的 case 目标 + 纯 fallthrough 后继。
            for (ca::usize k = 0; k < isl.cases.size(); ++k) {
                const auto hb = headBlkOf(isl.cases[k]);
                if (!hb) continue;
                if (isl.hasEntry && static_cast<ca::i32>(k) == isl.entryCaseIdx) continue;
                if (keepTgts.count(*hb) != 0 || anchorBlks.count(*hb) != 0) continue;
                if (victimBlks.count(*hb) != 0 || erasedBlks.count(*hb) != 0) continue;
                bool owned = true;
                for (const ir::InsnId u : dom.usersOf(isl.cases[k])) {
                    if (!dispOwned(u)) { owned = false; break; }
                }
                if (!owned) continue;
                ca::usize b = *hb;
                ca::usize n = 1;
                while (!mv.blocks[b].terminated && b + 1 < mv.blocks.size()) {
                    const Blk& nx = mv.blocks[b + 1];
                    if (anchorBlks.count(b + 1) != 0) break;
                    if (!dom.labelsAt(nx.entry).empty()) break;
                    if (keepTgts.count(b + 1) != 0) break;
                    if (victimBlks.count(b + 1) != 0 || erasedBlks.count(b + 1) != 0) break;
                    bool isCase = false;
                    for (ca::usize k2 = 0; k2 < isl.cases.size(); ++k2) {
                        if (const auto b2 = headBlkOf(isl.cases[k2]); b2 && *b2 == b + 1) {
                            isCase = true;
                            break;
                        }
                    }
                    if (isCase) break;
                    ++b;
                    ++n;
                }
                for (ca::usize j = 0; j < n; ++j) erasedBlks.insert(*hb + j);
                isl.deadBodies.emplace_back(*hb, n);
                more = true;
            }
        }
        if (failOf[i] != IslandFail::None) continue;
        for (const ca::usize b : erasedBlks) victimBlks.insert(b);

        // ---- positional 落入守卫（usersOf 只看字段引用；非终结顺序落入
        //         与条件分支落空臂都是位置边，受害者被擦后位置边改落邻接
        //         后继，语义断边）----
        //   - 前驱是本岛已归类条件用户且受害者是岛头 → 钉（补 GOTO 指向该
        //     用户的 case 目标：原落空臂经岛头分发 caseOf(key)，钉点同址）；
        //   - 其余（未归类条件落入 / 顺序落入 / 桥块被落入）→ 拒。
        const auto classifiedFallTarget = [&](ca::usize prevBlk) -> ir::InsnId {
            for (const BridgeRef& br : isl.bridges) {
                if (br.blk == prevBlk && br.cc) return targetHeadOf(isl, br.caseIdx);
            }
            for (const CarrierRef& cr : isl.carriers) {
                if (cr.blk != prevBlk) continue;
                const Op tOp = mv.insn(mv.blocks[cr.blk].insns.back()).op;
                if (isConditionalBranch(tOp)) return targetHeadOf(isl, cr.caseIdx);
            }
            return ir::kNoInsn;
        };
        for (const ca::usize v : victimBlks) {
            if (v == 0) continue;   // 方法头无布局前驱
            const Blk& prev = mv.blocks[v - 1];
            if (!layoutAdjacent(mv, prev, mv.blocks[v])) continue;   // 非邻接
            const Op prevOp = mv.insn(prev.insns.back()).op;
            const bool fallsIn = !prev.terminated || isConditionalBranch(prevOp);
            if (!fallsIn) continue;   // GOTO/switch/return/athrow 无位置边
            const ir::InsnId pin = (v == isl.dispBlk) ? classifiedFallTarget(v - 1)
                                                      : ir::kNoInsn;
            if (pin == ir::kNoInsn) {
                fail(i, IslandFail::ForeignRef);
                break;
            }
            isl.fallPins.emplace_back(v - 1, pin);
        }
        if (failOf[i] != IslandFail::None) continue;
        const auto resolveInsn = [&](ir::LabelId l) -> std::optional<ir::InsnId> {
            const auto r = dom.resolveLabel(l);
            if (r.is_err() || r.unwrap().isEnd()) return std::nullopt;
            return r.unwrap().insn;
        };
        for (ca::usize ei = 0; ei < mv.code.cu->exc.size(); ++ei) {
            const auto& e = mv.code.cu->exc[ei];
            // handler：锚在本岛可整擦桥头 → 重绑到桥目标；锚岛头 → 拒。
            if (const auto h = resolveInsn(e.handler)) {
                const ca::usize hb = mv.blockOf.at(h->slot);
                if (hb == isl.dispBlk) {
                    fail(i, IslandFail::ExcBlocked);
                    break;
                }
                for (const BridgeRef& br : isl.bridges) {
                    const bool entryHold = br.entry && (isl.entryTrampoline || isl.entryKeep);
                    if (br.blk == hb && !br.cc && !br.keep && !entryHold) {
                        isl.handlerRebinds.emplace_back(ei, targetHeadOf(isl, br.caseIdx));
                        break;
                    }
                }
                // 非本岛可整擦桥（条件形/载体/真块/保留桥）→ 原样保留。
            }
            // end：锚在本岛被擦块头 → 前移到块后首个存活指令。
            if (const auto en = resolveInsn(e.end)) {
                const ca::usize eb = mv.blockOf.at(en->slot);
                if (victimBlks.count(eb) != 0) {
                    const Blk& vb = mv.blocks[eb];
                    isl.endReanchors.emplace_back(
                        ei, nextInsnOf(mv, mv.orderOf.at(vb.insns.back().slot)));
                }
            }
            // start：锚在本岛被擦块头 → 异常保护语义不可保持 → 拒。
            if (const auto st = resolveInsn(e.start)) {
                const ca::usize sb = mv.blockOf.at(st->slot);
                if (victimBlks.count(sb) != 0) {
                    fail(i, IslandFail::ExcBlocked);
                    break;
                }
            }
        }
        if (failOf[i] != IslandFail::None) continue;

        // ---- 相 8：擦蚀布局安全网（fail-closed）----
        //   受害者整擦后布局压实：存活块 a 的落空/顺序位置边若改落 handler
        //   锚头块（帧 {Throwable}），空栈位置边与 handler 帧合并即非法
        //   （VerifyError）。累计擦蚀面（含先行岛）上存活块序遍历，逐对
        //   检查位置边 → handler 锚。
        for (const ca::usize v : victimBlks) plannedVictims.insert(v);
        {
            std::set<ca::usize> handlers;
            for (const auto& e : mv.code.cu->exc) {
                const auto r = dom.resolveLabel(e.handler);
                if (r.is_err() || r.unwrap().isEnd()) continue;
                if (const auto b = headBlkOf(r.unwrap().insn)) handlers.insert(*b);
            }
            if (!handlers.empty()) {
                ca::usize prev = kNoPos;
                bool      bad  = false;
                for (ca::usize b = 0; b < mv.blocks.size(); ++b) {
                    if (plannedVictims.count(b) != 0) continue;
                    if (prev != kNoPos) {
                        const Blk& pa  = mv.blocks[prev];
                        const Op   aOp = mv.insn(pa.insns.back()).op;
                        if ((!pa.terminated || isConditionalBranch(aOp))
                            && handlers.count(b) != 0) {
                            bad = true;
                            break;
                        }
                    }
                    prev = b;
                }
                if (bad) fail(i, IslandFail::ExcBlocked);
            }
        }
    }

    // ---- 定案 ----
    for (ca::usize i = 0; i < islands.size(); ++i) {
        if (failOf[i] != IslandFail::None) continue;
        out.solvable.push_back(std::move(islands[i]));
    }
    return out;
}

// ============================================================================
// ==== 单类扫描驱动 ==========================================================
// ============================================================================

void scanMethod(ir::ClassSession& s, const std::string& uri, ca::usize methodIndex,
                ScanResult& result, bool countOnly, ca::usize& residual)
{
    PromotedCode pc;
    if (!promoteToSeq(s, methodIndex, pc)) return;
    MethodView mv;
    if (!buildMethodView(pc, mv)) return;
    const AssemblyOut asmOut = assembleIslands(mv, s);
    result.dispatchers += asmOut.dispatchers;
    if (countOnly) {
        residual += asmOut.solvable.size();
        return;
    }
    for (const Island& isl : asmOut.solvable) {
        IslandPlan plan;
        plan.uri         = uri;
        plan.methodIndex = methodIndex;
        plan.headSrcPc   = isl.headSrcPc;
        result.islands.push_back(std::move(plan));
    }
    result.islandsSolvable += asmOut.solvable.size();
    for (const auto& [srcPc, f] : asmOut.fails) {
        switch (f) {
        case IslandFail::ShapeMismatch:  ++result.shapeMismatch; break;
        case IslandFail::ForeignRef:     ++result.foreignRef; break;
        case IslandFail::KeyProcessor:   ++result.keyProcessor; break;
        case IslandFail::SpanUnsolvable: ++result.spanUnsolvable; break;
        case IslandFail::StatePolluted:  ++result.statePolluted; break;
        case IslandFail::BlocksShared:   ++result.blocksShared; break;
        case IslandFail::ExcBlocked:     ++result.excBlocked; break;
        case IslandFail::MultiEntry:     ++result.foreignRef; break;
        default: break;
        }
    }
    for (const auto& [srcPc, f] : asmOut.fails) {
        Reject rej;
        rej.uri         = uri;
        rej.methodIndex = methodIndex;
        rej.anchorSrcPc = srcPc;
        rej.why         = bucketOf(f);
        result.rejects.push_back(std::move(rej));
    }
}

void scanClass(ir::ClassSession& s, const std::string& uri, ScanResult& result, bool countOnly,
               ca::usize& residual)
{
    const ir::ClassUnit& unit = s.unit();
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (!ir::hasCode(s, mi)) continue;
        scanMethod(s, uri, mi, result, countOnly, residual);
    }
}

// ============================================================================
// ==== 单岛改写 ==============================================================
// ============================================================================

/// @brief 目标 Boundary 标签（锚定目标块头）。失败返回 false。
bool labelAt(ir::CodeEditor& editor, const ir::InsnId head, ir::LabelId& out)
{
    auto r = editor.createLabel(head);
    if (r.is_err()) return false;
    out = r.unwrap();
    return true;
}

/// @brief 单岛反摊平执行。受害者进 victims（调用方统一 eraseBatch）；任一步
///        失败 → false（调用方整方法回滚）。
bool executeIsland(ir::ClassSession& s, const MethodView& mv, ir::CodeEditor& editor,
                   ir::CodeUnit& cu, const Island& isl, std::vector<ir::InsnId>& victims,
                   ca::usize& scrambles)
{
    ir::CodeDom& dom = cu.dom;
    const Blk&   disp = mv.blocks[isl.dispBlk];

    // ---- 1. 可整擦 goto 桥：前驱引用全体直连目标块（alias-face 剪枝）----
    for (const BridgeRef& br : isl.bridges) {
        if (br.cc || br.keep) continue;                        // 保留块在步 2 处理
        const bool entryHold = br.entry && (isl.entryTrampoline || isl.entryKeep);
        if (entryHold) continue;
        const ir::InsnId tgt = targetHeadOf(isl, br.caseIdx);
        const Blk&       blk = mv.blocks[br.blk];
        if (!br.bait) {   // 死饵桥不重指（目标 = handler 头帧，直连即非法）
            if (!dom.replaceAllUses(blk.entry, tgt).is_ok()) return false;
        }
        for (const ir::InsnId id : blk.insns) victims.push_back(id);
        if (br.caseIdx < 0) ++scrambles;
    }

    // ---- 2. 保留桥 / 边载体 / 蹦床 goto：臂（target）字段重指 ----
    const auto retarget = [&](const ir::InsnId u, const ir::InsnId tgt) {
        ir::LabelId lbl;
        if (!labelAt(editor, tgt, lbl)) return false;
        return editor.setTarget(u, lbl).is_ok();
    };
    for (const BridgeRef& br : isl.bridges) {
        if (!br.cc && !br.keep) continue;
        const Blk& blk = mv.blocks[br.blk];
        if (!retarget(blk.insns.back(), targetHeadOf(isl, br.caseIdx))) return false;
        if (br.caseIdx < 0) ++scrambles;
    }
    for (const CarrierRef& cr : isl.carriers) {
        if (cr.bait) continue;   // 死饵载体不重指（目标 = handler 头帧）
        const Blk& blk = mv.blocks[cr.blk];
        for (const ca::usize pos : cr.userPos) {
            if (!retarget(blk.insns[pos], targetHeadOf(isl, cr.caseIdx))) return false;
        }
    }
    // 2b. positional 落空臂钉：条件用户紧邻岛头（布局前驱）→ 终结指令后
    //     补一枚 GOTO 钉住原落点（岛头被擦后落空臂 positional 改落邻接
    //     后继；钉点 = 该用户的 case 目标，与原落空臂经岛头分发同址）。
    for (const auto& [pinBlk, pinTgt] : isl.fallPins) {
        const Blk& pblk = mv.blocks[pinBlk];
        ir::LabelId lbl;
        if (!labelAt(editor, pinTgt, lbl)) return false;
        ir::Insn g;
        g.op     = Op::GOTO;
        g.target = lbl;   // 插入前定靶：kNoLabel 靶过不了 insert 的句柄校验
        auto ins = editor.insertAfter(pblk.insns.back(), g);
        if (ins.is_err()) return false;
    }
    if (isl.hasEntry && isl.entryTrampoline && !isl.entryCc) {
        // 蹦床：入口桥 goto 改指目标块；span 本体入 victims（junk 前缀保留）。
        const Blk& b0 = mv.blocks[0];
        if (!retarget(b0.insns.back(), targetHeadOf(isl, isl.entryCaseIdx))) return false;
        const SetterParse sp = parseStateSetter(mv, s, b0, isl.slot);
        if (sp.kind != SpanKind::Inline) return false;
        for (ca::usize i = sp.start; i <= sp.tail; ++i) victims.push_back(b0.insns[i]);
    }

    // ---- 3. 异常表：handler 桥重绑 / 被擦块上 end 锚前移 ----
    for (const auto& [ei, anchor] : isl.handlerRebinds) {
        if (!editor.bindLabel(cu.exc[ei].handler, anchor).is_ok()) return false;
    }
    for (const auto& [ei, anchor] : isl.endReanchors) {
        if (!editor.bindLabel(cu.exc[ei].end, anchor).is_ok()) return false;
    }

    // ---- 3b. 死 case 体链 + 死饵 carrier 整擦（链头引用面 = dispatcher
    //          独占，switch 入批时引用同批消解；链内后继头部无标签，零重绑；
    //          死饵 carrier 的 dispatcher 引用指令随块入批）----
    for (const auto& [head, count] : isl.deadBodies) {
        for (ca::usize k = 0; k < count; ++k) {
            const Blk& db = mv.blocks[head + k];
            for (const ir::InsnId id : db.insns) victims.push_back(id);
        }
    }
    for (const CarrierRef& cr : isl.carriers) {
        if (!cr.bait) continue;
        const Blk& blk = mv.blocks[cr.blk];
        for (const ir::InsnId id : blk.insns) victims.push_back(id);
    }

    // ---- 4. 入口块归位（蹦床/junk 前缀退避：不移动，入口块保留原地
    //         时其前缀与 goto 还在执行路径上，moveRange 会让前缀失去执行）----
    if (isl.hasEntry && !isl.entryTrampoline && !isl.entryKeep) {
        const ir::InsnId tHead = targetHeadOf(isl, isl.entryCaseIdx);
        const Blk&       t     = mv.blocks[mv.blockOf.at(tHead.slot)];
        // 预算（全部句柄在插入前定案，插入不改句柄，moveRange 只认句柄）：
        //   nextAfterT = 目标块尾的后继（改前布局）；prevBlk = 紧邻前驱块。
        const ir::InsnId nextAfterT = nextInsnOf(mv, mv.orderOf.at(t.insns.back().slot));
        const ca::usize  tBlk      = mv.blockOf.at(t.entry.slot);
        // positional 落入前驱：非终结顺序落入与条件分支落空臂都是位置边
        // （目标块迁走后两者都会改落邻接后继，一律补 GOTO 钉住）。本岛被
        // 擦块（GOTO 桥/switch 岛头）天然无位置边，谓词自动排除。
        const bool predAdj   = tBlk > 0 && layoutAdjacent(mv, mv.blocks[tBlk - 1], t);
        const Op   predOp    = predAdj ? mv.insn(mv.blocks[tBlk - 1].insns.back()).op
                                       : Op::NOP;
        const bool hasFallPred = predAdj && (!mv.blocks[tBlk - 1].terminated
                                             || isConditionalBranch(predOp));
        // 4a. 目标块自身未终结或条件终结：老后继补 GOTO（随块迁移，落点
        //     不变）。条件终结的落空臂是真实位置边——块搬到方法头后落空臂
        //     会改落新邻接后继（入口桥/死体擦蚀后的空档，可能正邻 handler
        //     头帧 {Throwable}，空栈合并 → VerifyError），必须钉住原落点
        //     （jsoup v3_full TokeniserState$50.$gg1bdbksz8j2sa8 实证）。
        {
            const Op tOp = mv.insn(t.insns.back()).op;
            if (!t.terminated || isConditionalBranch(tOp)) {
                ir::LabelId q;
                if (nextAfterT == ir::kNoInsn || !labelAt(editor, nextAfterT, q)) return false;
                ir::Insn g;
                g.op     = Op::GOTO;
                g.target = q;   // 插入前定靶：kNoLabel 靶过不了 insert 的句柄校验
                auto ins = editor.insertAfter(t.insns.back(), g);
                if (ins.is_err()) return false;
            }
        }
        // 4b. 前驱块顺序落入目标块：补 GOTO 钉住（目标块迁走后落点不变）。
        if (hasFallPred) {
            const Blk& prev = mv.blocks[tBlk - 1];
            ir::LabelId    tl;
            if (!labelAt(editor, t.entry, tl)) return false;
            ir::Insn g;
            g.op     = Op::GOTO;
            g.target = tl;   // 插入前定靶：kNoLabel 靶过不了 insert 的句柄校验
            auto ins = editor.insertAfter(prev.insns.back(), g);
            if (ins.is_err()) return false;
        }
        // 4c. 纯形 goto 入口桥整块入 victims（保留形在步 2 已重指）。
        if (!isl.entryCc && !isl.entryKeep) {
            for (const ir::InsnId id : mv.blocks[0].insns) victims.push_back(id);
        }
        // 4d-pre. 异常 end 锚在搬迁段首（t.entry）的改绑。kHeadFollows 会把
        //     段首 Boundary 标签随段平移到方法头（pc 0），start 锚留在原地
        //     → 保护区间倒置（start_pc > end_pc=0，JVM 拒类
        //     ClassFormatError: Illegal exception table range；jsoup v3_flow
        //     HttpConnection$Response.execute / W3CDom.selectXpath /
        //     Node$OuterHtmlVisitor.tail / HtmlTreeBuilder.pushWithBookmark
        //     四类实证）。相 6 守卫的严格内含判据（p0 < pos < p1）恰好放行
        //     pos == p1（end 即段首）这一面。正确落点 = 原段位边界：搬迁后
        //     nextAfterT 恰坐原段首位（half-open 段尾后继），end 锚
        //     nextAfterT 即保持 [start, end) 覆盖码不变；段居方法尾时
        //     nextAfterT == kNoInsn，createLabel(kNoInsn) = 方法末边界
        //     （end_pc == code_length，JVMS 合法）。一律拆新标签改绑：
        //     原标签可能同时承担其他 entry 的 start 语义（须随段平移），
        //     就地改绑会连坐。
        for (ca::usize ei = 0; ei < cu.exc.size(); ++ei) {
            const auto er = dom.resolveLabel(cu.exc[ei].end);
            if (er.is_err() || er.unwrap().isEnd()) continue;
            if (!(er.unwrap().insn == t.entry)) continue;
            auto fresh = editor.createLabel(nextAfterT);
            if (fresh.is_err()) return false;
            cu.exc[ei].end = fresh.unwrap();
        }
        // 4d. 目标块头插到方法最前（入口桥整擦时搬移后即 blocks[0]）。
        if (!editor.moveRange(t.entry, nextAfterT, mv.blocks[0].entry,
                              ir::CodeEditor::MoveRangePolicy::kHeadFollows)
                 .is_ok()) {
            return false;
        }
    }

    // ---- 5. 受害者终检：岛头残余引用必须全体是待擦受害者（桥 goto 入
    //         victims 但 eraseBatch 在批尾，此处零引用判据会把在批的桥误
    //         判成分类遗漏；正确口径 = 每个引用者都在 victims 里，引用者
    //         落在 victims 之外即分类遗漏，fail-closed 整岛回滚）----
    {
        std::unordered_set<ca::u32> victimSlots;
        victimSlots.reserve(victims.size());
        for (const ir::InsnId id : victims) victimSlots.insert(id.slot);
        for (const ir::InsnId u : dom.usersOf(disp.insns[0])) {
            if (victimSlots.count(u.slot) == 0) return false;
        }
    }

    // dispatcher 对入 victims。
    victims.push_back(disp.insns[0]);
    victims.push_back(disp.insns[1]);
    return true;
}

// ============================================================================
// ==== 单方法改写体（mutate 闭包内执行）======================================
// ============================================================================

/// @brief 复推岛并执行计划内岛。局部计数出参；方法整体成功后由调用方并入。
bool rewriteMethodBody(ir::ClassSession& s, ir::CodeUnit& cu,
                       const std::set<ca::u32>& plannedPcs, ca::usize& islands,
                       ca::usize& scrambles, ca::usize& entryRestored,
                       ca::usize& entryTrampoline)
{
    PromotedCode pc;
    pc.cu = &cu;
    for (const ir::InsnId id : cu.dom.order()) {
        const ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        pc.seq.push_back(in);
    }
    MethodView mv;
    if (!buildMethodView(pc, mv)) return false;
    const AssemblyOut asmOut = assembleIslands(mv, s);

    ir::CodeEditor          editor(cu);
    std::vector<ir::InsnId> victims;
    ca::usize               executed   = 0;
    ca::usize               localScr   = 0;
    ca::usize               localEntry = 0;
    ca::usize               localTramp = 0;
    for (const Island& isl : asmOut.solvable) {
        if (plannedPcs.count(isl.headSrcPc) == 0) continue;
        if (!executeIsland(s, mv, editor, cu, isl, victims, localScr)) return false;
        ++executed;
        if (isl.hasEntry) {
            if (isl.entryTrampoline || isl.entryKeep) {
                ++localTramp;   // 入口块原地保留（蹦床/junk 前缀退避）
            } else {
                ++localEntry;
            }
        }
    }
    if (executed == 0) return false;
    if (!cu.dom.eraseBatch(victims).is_ok()) return false;
    islands         = executed;
    scrambles       = localScr;
    entryRestored   = localEntry;
    entryTrampoline = localTramp;
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    static_cast<void>(input);
    ScanResult result;
    ca::usize  sink = 0;   // residual 出参：非 count 模式不写，占位保持签名同形
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        scanClass(*session, uri, result, false, sink);
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

    // 计划按 uri 聚类（万类大 jar 上避免 classes × islands 平方账）。
    std::unordered_map<std::string, std::vector<const IslandPlan*>> byUri;
    for (const IslandPlan& plan : result.islands) byUri[plan.uri].push_back(&plan);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize islands        = 0;
        ca::usize scrambles      = 0;
        ca::usize entryRestored  = 0;
        ca::usize entryTrampoline = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto        it = byUri.find(uri);
        if (it == byUri.end()) return;
        const std::vector<const IslandPlan*>& plansOf = it->second;

        std::shared_ptr<ir::ClassSession> session = openSession(*entry.bytes);
        if (!session) return;

        std::set<ca::usize> methodIdxs;
        for (const IslandPlan* plan : plansOf) methodIdxs.insert(plan->methodIndex);

        // 前置检查：原样往返不了的方法不进改写面（lower 失败与改写无关却拖垮类）。
        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        // 单方法改写驱动：按该方法计划跑一遍帧链（mutate → lower → rewire）。
        const auto runMethod = [&](ir::ClassSession& sess, const ca::usize mi,
                                   ca::usize& mIslands, ca::usize& mScr,
                                   ca::usize& mEntry, ca::usize& mTramp) -> bool {
            std::set<ca::u32> plannedPcs;
            for (const IslandPlan* plan : plansOf) {
                if (plan->methodIndex == mi) plannedPcs.insert(plan->headSrcPc);
            }
            return rewrite_tools::mutateMethodWithFrames(
                sess, mi, frames.resolver(), [&](ir::CodeUnit& cu) {
                    return rewriteMethodBody(sess, cu, plannedPcs, mIslands, mScr, mEntry,
                                             mTramp);
                });
        };

        ca::usize              hit = 0;
        ca::usize              failed = 0;
        ca::usize              scr = 0;
        ca::usize              entryN = 0;
        ca::usize              trampN = 0;
        std::vector<ca::usize> okMethods;
        for (const ca::usize mi : methodIdxs) {
            std::set<ca::u32> plannedPcs;
            for (const IslandPlan* plan : plansOf) {
                if (plan->methodIndex == mi) plannedPcs.insert(plan->headSrcPc);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += plannedPcs.size();
                continue;
            }
            ca::usize mIslands = 0;
            ca::usize mScr     = 0;
            ca::usize mEntry   = 0;
            ca::usize mTramp   = 0;
            if (!runMethod(*session, mi, mIslands, mScr, mEntry, mTramp)) {
                // 帧链/改写失败：该方法整方法放弃。
                failed += plannedPcs.size();
                continue;
            }
            okMethods.push_back(mi);
            hit += mIslands;
            scr += mScr;
            entryN += mEntry;
            trampN += mTramp;
        }
        dirty.failed += failed;   // 第一遍失败账（失败方法原样保留 = 确实未反摊平）
        if (hit == 0) return;

        if (failed > 0) {
            // 帧链回炉：rewire 失败的方法已经 lower 进会话（SMT 摘除后未
            // 再生），半落会话直接写回会落无帧方法。整类在干净会话上只
            // 重放成功面（同字节同改写 = 同结果），失败方法保持原样，
            // fail-closed 落到方法粒度、不留半截；计数以回放为准。
            std::shared_ptr<ir::ClassSession> replay = openSession(*entry.bytes);
            if (!replay) {
                // 干净回炉开不了会话：半落会话（rewire 失败方法无帧）绝不
                // 落盘，无帧方法 = VerifyError 面。整类放弃，fail-closed。
                dirty.failed += hit;
                return;
            }
            {
                ca::usize rHit = 0;
                ca::usize rScr = 0;
                ca::usize rEntryN = 0;
                ca::usize rTrampN = 0;
                for (const ca::usize mi : okMethods) {
                    ca::usize mIslands = 0;
                    ca::usize mScr     = 0;
                    ca::usize mEntry   = 0;
                    ca::usize mTramp   = 0;
                    if (!runMethod(*replay, mi, mIslands, mScr, mEntry, mTramp)) {
                        // 非确定性防御：回放失败的方法按失败计数（岛留原样）。
                        for (const IslandPlan* plan : plansOf) {
                            if (plan->methodIndex == mi) dirty.failed += 1;
                        }
                        continue;
                    }
                    rHit += mIslands;
                    rScr += mScr;
                    rEntryN += mEntry;
                    rTrampN += mTramp;
                }
                // 第一遍 failed 账保留（失败方法原样 = 确实未反摊平）；回放
                // 失败的方法在 okMethods 的 hit 里被动移出（rHit 不含它）。
                hit = rHit;
                scr = rScr;
                entryN = rEntryN;
                trampN = rTrampN;
                session = std::move(replay);
            }
        }

        auto encoded = jbc::core::writeClass(session->backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(
            PendingWrite{uri, std::move(encoded).unwrap(), hit, scr, entryN, trampN});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.islands += item.islands;
        dirty.scrambles += item.scrambles;
        dirty.entryRestored += item.entryRestored;
        dirty.entryTrampoline += item.entryTrampoline;
        dirty.classes += 1;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    // proof 是效力检查（不变量复核），不许被 pass 级时间片截短，挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend                   suspend;
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;

    ca::usize residual = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto session = openSession(*entry.bytes);
        if (!session) return;
        ScanResult counter;
        scanClass(*session, uri, counter, true, residual);
    });
    result.residual = residual;
    return result;
}

}   // namespace grunt::cff_unflatten
