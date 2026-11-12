/// @file        dispatcher_restore.cpp
/// @brief       残留 `dispatcher_restore` 三段实现：分发器识别（头锚 +
///              键表 + case 体形状，S2）→ 调用点反查回填真实 invoke（原地
///              换 + 跨度擦除）→ residual + 类数 + 重解析验收。
///
/// 判据分级与锚保留/槽位面设计见头文件；骨架对齐 string_pool_fold（scan
/// 只读、rewrite 只写（mutateMethodWithFrames 正统帧链写回）、proof 只判）。
/// 分发器生成面是知识转写（grunt3@e41e852 `redirect/InvokeDispatcher.kt`
/// buildStageImpl 逐行，源锚点见各函数头注）；v2 分支（grunt2@36c0eef）
/// redirect/ 下无此 transformer——v3-only。键表是纯常量直查（tableswitch
/// 静态编码 + 调用点紧邻 key 常量），不需要 jsim；key 经算术混淆时常量缺
/// 席 → fail-closed 保留（NumberFold 先折）。
///
/// ## SCAN
///
/// 分发器识别（buildDispatcher，候选预筛 static + desc 形 `(Object×N; I)R`）：
/// 头锚 = 首条 ILOAD 恰为 key 槽 N + 第二条 TABLESWITCH + default 回跳方法
/// 首条指令 + 方法无异常表，任一缺席整表拒绝（`分发器形状不符：`前缀记
/// 账）。键表 lo = switch 下界，case 数 = 目标数（连续区间，下标 =
/// key - lo），case 数 < 2 拒。case 体从目标起直线走：允许 ALOAD 家族 /
/// CHECKCAST / NOP，恰一个 INVOKESTATIC|INVOKEVIRTUAL（多候选或成员引用不
/// 可解析即拒），收尾必须 ret 型 return；中段含标签（合并点）拒；转发目标
/// 返回类型与键表 ret 恒等互证。virtual 分支的实参从槽 0 起连载、无显式
/// receiver load——生成面源码 quirk，case 门两侧都不依赖它。
///
/// 调用点反查（分发器方法自身跳过）：INVOKESTATIC owner=self + desc 形命中
/// 已识别键表后，key 位回走 NOP 跳过（squeeze 遗留口径；途中标签 = 合并
/// 点 fail-closed）；key 非常量（含 NumberBasicEncrypt 藏形）→ keyNotConst，
/// 不在键表区间 → keyNotInTable。null 跑互证：key 前连续 ACONST_NULL 计数
/// 与补位数对表（padCount = N - 目标消费实参数，virtual 含 receiver 槽）；
/// null 跑长于补位数 = 原实参尾带 null 字面量，合法——站点记 padCount，
/// 擦除只动末段补位，保留前段 null 实参。
///
/// **promote 纪律**：每方法只 promote 一次——promoteMethod 重复调用幂等但
/// 重建 CodeUnit，旧 Insn* 全作废；scanClass 预循环单点持有，
/// buildDispatcher 复用调用方视图绝不自行 promote（boar 正例罐实证：双
/// promote 后调用点反查用旧视图，UAF 直接硬崩）。
///
/// ## REWRITE
///
/// 按类聚类，方法级 roundTripSafe 前置门。单站点：srcPc + INVOKESTATIC 重
/// 定位后形状复核（身份 / key / null 跑全部重钉——scan 之后字节可能已被同
/// 方法前站改写，不盲改），调用指令原地换真目标（srcPc 保留，opcode 换
/// case 转发形，成员 intern 成真目标），跨度 [NOP 夹层; 末段 padCount 个
/// 补位; key] 单批 eraseBatch（全有或全无，kRejectIfUsed 拒收即整体还原
/// 放弃）。逐站失败诊断走 `GRUNT_DISPATCHER_RESTORE_TRACE`，帧链失败走
/// `[grunt.Frame]`（rewrite_toolbox 共享面）。
///
/// ## PROOF
///
/// 类数不增 + 脏类可重解析 + residual（countOnly 复扫：仍「分发器识别通
/// 过 + 调用点全形状命中键表」的站点数）。语料单罐门要求 0。
///
/// ## 已知边界
///
/// 全链罐（boar）不产此面：InvokeDispatcher 排在 ControlflowFlattening 之
/// 前、调用点又被 ReferenceObfuscate indy 化，罐内真分发器全部被摊平成岛；
/// desc 形候选与 `__grt$indy$` 蹬床 / 数值走廊方法形状碰撞，全数形状拒绝
/// ——反误伤门工作正常，不是缺陷。正例罐（仅 InvokeDispatcher
/// chance=1.0）实证识别与回填链路（语料数字见 corpus-ledger）。帧重算
/// type conflict 的宿主方法 fail-closed 保留——无编辑帧链探针复现同一冲突
/// （原方法既有形态，与改写无关）；roundTripSafe 只测 promote→lower 不测
/// rewire，查不出该形态，不手剪 StackMapTable。

#include "grunt/dispatcher_restore.hpp"

#include <cstdio>
#include <cstdlib>
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
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/knowledge/grunt_names.hpp"
#include "grunt/pass_clock.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::dispatcher_restore {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32   kAccStatic = 0x0008;
constexpr ca::usize kNoSlot    = static_cast<ca::usize>(-1);
constexpr ca::usize kNoPos     = static_cast<ca::usize>(-1);

// ============================================================================
// ==== 形状钉面辅助（S2；常量装载读数走 const_args）==========================
// ============================================================================

/// @brief int 槽装载读取（ILOAD a / ILOAD_0..3 → 槽号进 slot；失败 false）。
bool intLoadSlotOf(const ir::Insn& in, ca::u32& slot)
{
    if (in.op == Op::ILOAD) { slot = static_cast<ca::u32>(in.a); return true; }
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3) {
        slot = static_cast<ca::u32>(static_cast<int>(in.op) - static_cast<int>(Op::ILOAD_0));
        return true;
    }
    return false;
}

/// @brief 引用槽装载（ALOAD a / ALOAD_0..3 快捷形——生成面实参全是引用型，
///        `it.getLoadType()` 对 L/[ 型一律 ALOAD，grunt3 util Type.kt）。
bool refLoadAt(const ir::Insn& in)
{
    if (in.op == Op::ALOAD) return true;
    return in.op >= Op::ALOAD_0 && in.op <= Op::ALOAD_3;
}

/// @brief 分发器 desc 解析：`(Ljava/lang/Object;×N;I)R` → (N, R)；非该形
///        返回 nullopt。逐字对齐生成面 `repeat(maxLoadOp) { desc +=
///        "Ljava/lang/Object;" } desc += "I)$returnDesc"`（grunt3
///        InvokeDispatcher.kt buildStageImpl）。
struct DispDescShape
{
    ca::i32          keySlot = 0;   ///< N：Object 参数个数 = key 槽位（static）
    std::string_view ret;           ///< 返回类型描述符
};

std::optional<DispDescShape> parseDispDesc(std::string_view desc)
{
    if (desc.size() < 3 || desc.front() != '(') return std::nullopt;
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos) return std::nullopt;
    std::string_view       params = desc.substr(1, close - 1);
    const std::string_view ret    = desc.substr(close + 1);
    if (ret.empty()) return std::nullopt;
    const std::string_view paramType = knowledge::kGruntDispatcherParamType;
    ca::i32                n         = 0;
    while (params.size() >= paramType.size()
           && params.substr(0, paramType.size()) == paramType) {
        params.remove_prefix(paramType.size());
        ++n;
    }
    if (params != knowledge::kGruntDispatcherKeyType) return std::nullopt;
    return DispDescShape{n, ret};
}

/// @brief 返回类型描述符 → 对应 return 指令（生成面
///        `+InsnNode(desc.getReturnType())`）。
std::optional<Op> returnOpOf(std::string_view ret)
{
    if (ret == "V") return Op::RETURN;
    if (ret == "J") return Op::LRETURN;
    if (ret == "F") return Op::FRETURN;
    if (ret == "D") return Op::DRETURN;
    if (!ret.empty() && (ret.front() == 'L' || ret.front() == '[')) return Op::ARETURN;
    if (ret.size() == 1) return Op::IRETURN;   // I/B/C/S/Z
    return std::nullopt;
}

/// @brief 目标 desc 的实参槽位总数（J/D 各占 2 槽）——调用点补位互证用；
///        描述符非法返回 -1。
ca::i32 paramSlotsOf(std::string_view desc)
{
    if (desc.empty() || desc.front() != '(') return -1;
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos) return -1;
    std::string_view params = desc.substr(1, close - 1);
    ca::i32          slots  = 0;
    while (!params.empty()) {
        switch (params.front()) {
        case 'J':
        case 'D':
            slots += 2;
            params.remove_prefix(1);
            break;
        case 'L': {
            const ca::usize semi = params.find(';');
            if (semi == std::string_view::npos) return -1;
            slots += 1;
            params.remove_prefix(semi + 1);
            break;
        }
        case '[': {
            std::string_view rest = params.substr(1);
            while (!rest.empty() && rest.front() == '[') rest.remove_prefix(1);
            if (rest.empty()) return -1;
            if (rest.front() == 'L') {
                const ca::usize semi = rest.find(';');
                if (semi == std::string_view::npos) return -1;
                rest.remove_prefix(semi + 1);
            } else {
                rest.remove_prefix(1);
            }
            slots += 1;
            params = rest;
            break;
        }
        default:
            slots += 1;
            params.remove_prefix(1);
            break;
        }
    }
    return slots;
}

/// @brief 目标调用面消费的栈值数：static = 实参槽数；virtual = receiver +
///        实参槽数（生成面 loadType 口径：`if (callingMethod.isStatic)
///        paramsType else [owner] + paramsType`）。
/// @return 槽数；描述符非法 -1。
ca::i32 targetStackConsume(bool isVirtual, std::string_view targetDesc)
{
    const ca::i32 slots = paramSlotsOf(targetDesc);
    if (slots < 0) return -1;
    return isVirtual ? slots + 1 : slots;
}

/// @brief promote 单方法并把指令序摊平成指令视图（同 string_pool_fold）。
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
// ==== 分发器识别（S2：头锚 + 键表 + case 体形状）============================
// ============================================================================

/// @brief 已验证的分发器：键区间 [lo, lo+cases.size()) → 转发目标。
struct Dispatcher
{
    ca::usize           methodIndex = 0;
    std::string         name;
    std::string         desc;
    ca::i32             keySlot = 0;    ///< key 参数槽 = Object 参数个数
    ca::i32             lo      = 0;
    std::vector<Target> cases;          ///< 下标 = key - lo
};

/// @brief 分发器识别失败原因（记账归类；None = 成功）。
enum class DispFail : ca::u8 { None, NotCandidate, ShapeMismatch, Ambiguous };

/// @brief 分发器识别：desc 形 → static → 头锚（ILOAD key 槽 + TABLESWITCH
///        default 回跳方法头）→ 逐 case 直线转发体（载实参 + 唯一 invoke +
///        ret 型 return）。
///
/// 生成面逐字锚（grunt3@e41e852 InvokeDispatcher.kt buildStageImpl）：
/// `LABEL(startLabel); ILOAD(maxLoadOp); TableSwitchInsnNode(startCase,
/// endCase, startLabel.node, labels...)`——default 即方法头；case 体
/// `VarInsnNode(getLoadType, stack); CHECKCAST; INVOKESTATIC|INVOKEVIRTUAL;
/// +InsnNode(returnType)`。头锚与 case 体任一不合规 → 整个分发器拒绝
/// （fail-closed，该表完全不用，residual 如实计）。
///
/// @param code 调用方已 promote 的视图（promoteMethod 重复调用会**重建**
///        CodeUnit——旧 Insn* 全作废，故本函数绝不自行 promote，promote 纪
///        律由 scanClass 的单点预循环持有）。
/// @return 失败桶（None = 成功，out 已填充）；失败原因进 why。
DispFail buildDispatcher(ir::ClassSession& s, ca::usize methodIndex, const PromotedCode& code,
                         Dispatcher& out, std::string& why)
{
    const ir::ClassUnit&  unit = s.unit();
    const ir::MethodUnit& mu   = unit.methods[methodIndex];

    const auto shape = parseDispDesc(unit.symbols().str(mu.desc));
    if (!shape.has_value()) return DispFail::NotCandidate;
    if ((mu.access & kAccStatic) == 0) return DispFail::NotCandidate;

    const std::vector<const ir::Insn*>& seq   = code.seq;
    const std::vector<ir::InsnId>&      order = code.cu->dom.order();

    // ---- 头锚：ILOAD keySlot + TABLESWITCH，default 回跳方法头 ----
    if (seq.size() < 2) {
        why = "体不足两条（无头锚）";
        return DispFail::ShapeMismatch;
    }
    ca::u32 keySlot = 0;
    if (!intLoadSlotOf(*seq[0], keySlot)
        || keySlot != static_cast<ca::u32>(shape->keySlot)) {
        why = "首条非 key 槽 ILOAD";
        return DispFail::ShapeMismatch;
    }
    if (seq[1]->op != Op::TABLESWITCH) {
        why = "头锚第二条非 TABLESWITCH";
        return DispFail::ShapeMismatch;
    }
    // 生成面无异常表（ControlflowJump 等上游在 Redirect 之前跑，加过异常桥
    // 的方法不在生成面上）。
    if (!code.cu->exc.empty()) {
        why = "分发器带异常表（非生成面）";
        return DispFail::ShapeMismatch;
    }
    const auto defaultRes = code.cu->dom.resolveLabel(seq[1]->target);
    if (defaultRes.is_err()
        || defaultRes.unwrap().kind != ir::LabelTargetKind::Instruction
        || defaultRes.unwrap().insn != order[0]) {
        why = "tableswitch default 未回跳方法头（识别锚点缺席）";
        return DispFail::ShapeMismatch;
    }

    const ca::i32           lo    = seq[1]->a;
    const ca::i64           count = static_cast<ca::i64>(seq[1]->targets.size());
    const std::optional<Op> retOp = returnOpOf(shape->ret);
    if (!retOp.has_value()) {
        why = "返回类型描述符不可解析";
        return DispFail::ShapeMismatch;
    }
    if (count < knowledge::kGruntDispatcherMinCases) {
        why = "键表 case 数不足（生成面分组 ≥2）";
        return DispFail::ShapeMismatch;
    }

    // ---- 逐 case 直线转发体 ----
    Dispatcher disp;
    disp.methodIndex = methodIndex;
    disp.name        = std::string(unit.symbols().str(mu.name));
    disp.desc        = std::string(unit.symbols().str(mu.desc));
    disp.keySlot     = shape->keySlot;
    disp.lo          = lo;
    disp.cases.resize(static_cast<ca::usize>(count));

    for (ca::i64 i = 0; i < count; ++i) {
        const auto caseRes =
            code.cu->dom.resolveLabel(seq[1]->targets[static_cast<ca::usize>(i)]);
        if (caseRes.is_err()
            || caseRes.unwrap().kind != ir::LabelTargetKind::Instruction) {
            why = "case 目标标签解析失败";
            return DispFail::ShapeMismatch;
        }
        // case 头在 dom 序里的位置。
        ca::usize q = kNoPos;
        for (ca::usize p = 0; p < order.size(); ++p) {
            if (order[p] == caseRes.unwrap().insn) {
                q = p;
                break;
            }
        }
        if (q == kNoPos) {
            why = "case 目标不在指令序内";
            return DispFail::ShapeMismatch;
        }
        ca::usize         invokes  = 0;
        ca::usize         invokePos = kNoPos;
        std::optional<ir::ClassSession::MemberSem> target;
        bool              closed   = false;
        for (ca::usize p = q; p < seq.size(); ++p) {
            if (p != q && !code.cu->dom.labelsAt(order[p]).empty()) {
                why = "case 体中段含合并点标签";
                return DispFail::ShapeMismatch;
            }
            const Op op = seq[p]->op;
            if (op == *retOp) {   // case 收尾（此后归下一 case / 方法尾）
                closed = true;
                break;
            }
            if (refLoadAt(*seq[p]) || op == Op::CHECKCAST || op == Op::NOP) continue;
            if (op == Op::INVOKESTATIC || op == Op::INVOKEVIRTUAL) {
                ir::ClassSession::MemberSem sem;
                if (!s.memberOf(*seq[p], sem)
                    || sem.kind != ir::ClassSession::MemberSem::Kind::Method) {
                    why = "case 转发 invoke 成员引用不可解析";
                    return DispFail::ShapeMismatch;
                }
                ++invokes;
                if (invokes > 1) {
                    why = "case 体转发目标不唯一（多候选，fail-closed 不猜）";
                    return DispFail::Ambiguous;
                }
                invokePos = p;
                target    = std::move(sem);
                continue;
            }
            why = "case 体含转发面外指令";
            return DispFail::ShapeMismatch;
        }
        if (!closed || invokes != 1 || !target.has_value()) {
            why = "case 体缺转发 invoke 或 ret 型收尾";
            return DispFail::ShapeMismatch;
        }
        // 转发目标返回类型与键表 ret 恒等（生成面按返回类型分组的事实互证：
        // case 转发 invoke 的返回类型 == 分发器 desc 的返回类型）。
        const std::string_view invokeRet = [&] {
            const std::string_view d = target->desc;
            const ca::usize        c = d.find(')');
            return c == std::string_view::npos ? std::string_view{} : d.substr(c + 1);
        }();
        if (invokeRet != shape->ret) {
            why = "case 转发目标返回类型与键表不符";
            return DispFail::ShapeMismatch;
        }
        disp.cases[static_cast<ca::usize>(i)] =
            Target{seq[invokePos]->op == Op::INVOKEVIRTUAL, std::string(target->owner),
                   std::string(target->name), std::string(target->desc)};
    }

    out = std::move(disp);
    return DispFail::None;
}

// ============================================================================
// ==== 单类扫描驱动：分发器识别 → 调用点反查 =================================
// ============================================================================

/// @brief 单类扫描。residual 模式（countOnly=true）只计数不改账（result 仅
///        作草稿，residual 进 outResidual）。
void scanClass(ir::ClassSession& s, const std::string& uri, ScanResult& result,
               bool countOnly, ca::usize& outResidual)
{
    const ir::ClassUnit& unit     = s.unit();
    const std::string    selfName(unit.symbols().str(unit.thisClass));

    // ---- promote 纪律：每方法只 promote 一次，两相共用同一视图 ----
    std::unordered_map<ca::usize, PromotedCode> promoted;
    std::vector<ca::usize>                      order0;
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (!ir::hasCode(s, mi)) continue;
        PromotedCode pc;
        if (!promoteToSeq(s, mi, pc)) continue;
        order0.push_back(mi);
        promoted.emplace(mi, std::move(pc));
    }

    // ---- 相 1：分发器识别 ----
    std::unordered_map<ca::usize, Dispatcher> dispatchers;   // methodIndex → 表
    std::unordered_map<std::string, ca::usize> byRefKey;     // name+"\0"+desc → methodIndex
    bool                                       refAmbiguous = false;
    for (const ca::usize mi : order0) {
        // 候选预筛：desc 形 + static（省掉与生成面无关方法的 promote 深检）。
        const ir::MethodUnit& mu = unit.methods[mi];
        if ((mu.access & kAccStatic) == 0) continue;
        if (!parseDispDesc(unit.symbols().str(mu.desc)).has_value()) continue;

        Dispatcher disp;
        std::string why;
        const DispFail fail = buildDispatcher(s, mi, promoted.at(mi), disp, why);
        if (fail == DispFail::NotCandidate) continue;
        if (fail != DispFail::None) {
            ++result.dispatcherRejected;
            if (!countOnly) {
                Reject rej;
                rej.uri         = uri;
                rej.methodIndex = mi;
                rej.why         = "分发器形状不符： " + why;
                result.rejects.push_back(std::move(rej));
            }
            continue;
        }
        const std::string refKey = disp.name + '\0' + disp.desc;
        const auto        [it, inserted] = byRefKey.emplace(refKey, mi);
        if (!inserted) {
            // 同 (name, desc) 两个分发器形——JVMS 不可能，防御性 fail-closed。
            refAmbiguous = true;
            continue;
        }
        ++result.dispatchers;
        dispatchers.emplace(mi, std::move(disp));
    }

    // ---- 相 2：调用点反查（分发器方法自身跳过——生成面 case 体不含分发器
    //      调用，嵌套分发器不存在）----
    for (const ca::usize mi : order0) {
        if (dispatchers.count(mi) != 0) continue;
        const PromotedCode&            code  = promoted.at(mi);
        const std::vector<const ir::Insn*>& seq  = code.seq;
        const std::vector<ir::InsnId>&      order = code.cu->dom.order();
        for (ca::usize p = 0; p < seq.size(); ++p) {
            const ir::Insn* call = seq[p];
            if (call->op != Op::INVOKESTATIC) continue;
            ir::ClassSession::MemberSem enc;
            if (!s.memberOf(*call, enc) || enc.kind != ir::ClassSession::MemberSem::Kind::Method
                || enc.owner != selfName) continue;
            const auto shape = parseDispDesc(enc.desc);
            if (!shape.has_value()) continue;

            // 引用必须命中已识别分发器（名字 + desc）；不命中 = 非本家，静默跳过。
            const std::string refKey =
                std::string(enc.name) + '\0' + std::string(enc.desc);
            const auto        dit    = byRefKey.find(refKey);
            if (dit == byRefKey.end() || dispatchers.count(dit->second) == 0) {
                if (refAmbiguous && !countOnly) {
                    // 同 (name, desc) 冲突时键表不可用：指向该引用形的站点全拒。
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = call->srcPc;
                    rej.why         = "分发器引用形歧义（同 (name, desc) 多候选）";
                    result.rejects.push_back(std::move(rej));
                    ++result.dispatcherBlocked;
                }
                continue;
            }
            const Dispatcher& disp = dispatchers.at(dit->second);

            const auto reject = [&](const std::string& why, ca::usize* bucket) {
                if (bucket != nullptr) ++*bucket;
                if (!countOnly) {
                    Reject rej;
                    rej.uri         = uri;
                    rej.methodIndex = mi;
                    rej.anchorSrcPc = call->srcPc;
                    rej.why         = why;
                    result.rejects.push_back(std::move(rej));
                }
            };

            // ---- key 位：紧邻常量（NOP 跳过与 squeeze 遗留口径一致；途中
            //      标签 = 合并点，fail-closed）----
            bool            labeled = false;
            ca::usize       keyQ    = kNoPos;
            const ir::Insn* keyInsn = nullptr;
            for (ca::usize q = p; q-- > 0;) {
                if (!code.cu->dom.labelsAt(order[q]).empty()) labeled = true;
                if (seq[q]->op != Op::NOP) {
                    keyQ    = q;
                    keyInsn = seq[q];
                    break;
                }
            }
            if (labeled) {
                reject("key 位含合并点标签（fail-closed）", nullptr);
                continue;
            }
            ca::i32 key = 0;
            if (keyQ == kNoPos || keyInsn == nullptr || !const_args::decodeConstInt(s, *keyInsn, key)) {
                reject("key 位非常量（含 NumberBasicEncrypt 藏形可能，fail-closed）",
                       &result.keyNotConst);
                continue;
            }
            // ---- 键表查询 ----
            const ca::i64 idx = static_cast<ca::i64>(key) - static_cast<ca::i64>(disp.lo);
            if (idx < 0 || idx >= static_cast<ca::i64>(disp.cases.size())) {
                reject("key " + std::to_string(key) + " 不在键表 ["
                           + std::to_string(disp.lo) + ", "
                           + std::to_string(disp.lo + static_cast<ca::i32>(disp.cases.size()))
                           + ")（fail-closed）",
                       &result.keyNotInTable);
                continue;
            }
            const Target& tgt = disp.cases[static_cast<ca::usize>(idx)];

            // ---- 补位 ACONST_NULL 跑计数（key 之前连续的 aconst_null）----
            // 生成面在 key 前插入 addNulls 个补位；调用点原实参尾部的 null
            // 字面量与补位同形不可分辨——null 跑 ≥ addNulls，只擦**末段**
            // addNulls 个（原实参原样保留）。
            ca::u32 nullRun = 0;
            const ca::u32 cap = static_cast<ca::u32>(disp.keySlot) + 1;
            for (ca::usize q = keyQ; q-- > 0;) {
                if (!code.cu->dom.labelsAt(order[q]).empty()) labeled = true;
                if (seq[q]->op == Op::NOP) continue;
                if (seq[q]->op != Op::ACONST_NULL) break;
                ++nullRun;
                if (nullRun > cap) break;
            }
            if (labeled) {
                reject("补位跨度含合并点标签（fail-closed）", nullptr);
                continue;
            }
            // ---- 补位互证：addNulls = N - 目标消费（生成面
            //      `addNulls = maxLoadOp - handle.loadType.size`）；null 跑
            //      ∈ [addNulls, N]（下界 = 无 null 实参，上界 = 验证器强制的
            //      满栈形）----
            const ca::i32 consume = targetStackConsume(tgt.isVirtual, tgt.desc);
            const ca::i32 padCount = disp.keySlot - consume;
            if (consume < 0 || padCount < 0
                || static_cast<ca::i32>(nullRun) < padCount
                || static_cast<ca::i32>(nullRun) > disp.keySlot) {
                reject("补位跨度与目标栈形互证不符（目标消费 " + std::to_string(consume)
                           + "，addNulls = " + std::to_string(padCount)
                           + "，null 跑 = " + std::to_string(nullRun) + "）",
                       &result.padMismatch);
                continue;
            }

            if (countOnly) {
                ++outResidual;
                continue;
            }
            Site site;
            site.uri         = uri;
            site.methodIndex = mi;
            site.callSrcPc   = call->srcPc;
            site.dispName    = disp.name;
            site.dispDesc    = disp.desc;
            site.key         = key;
            site.padCount    = static_cast<ca::u32>(padCount);
            site.target      = tgt;
            result.sites.push_back(std::move(site));
        }
    }
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

namespace {

/// @brief rewriteSite 失败步诊断（GRUNT_DISPATCHER_RESTORE_TRACE 门控；
///        恒返回 false，调用点写 `return siteFail(site, "步名")`）。
bool siteFail(const Site& site, const char* step)
{
    if (std::getenv("GRUNT_DISPATCHER_RESTORE_TRACE") != nullptr) {
        std::fprintf(stderr,
                     "[grunt.DispatcherRestore]   rewrite-fail %s#%zu pc=%u: %s\n",
                     site.uri.c_str(), site.methodIndex, site.callSrcPc, step);
    }
    return false;
}

/// @brief 单站点改写。@return false = 未动（failed 记账）。
///
/// 重定位（srcPc + INVOKESTATIC）→ 形状复核（scan 之后字节可能已被同方法
/// 前站改写，不盲改）→ 调用指令原地换真目标（srcPc 保留，opcode 换转发
/// 形，成员 intern 成真目标）→ 跨度 [补位×k; key] eraseBatch 擦除
/// （kRejectIfUsed 拒收即回滚，不留半改写态）。
bool rewriteSite(ir::ClassSession& s, ir::CodeUnit& cu, const Site& site)
{
    const std::vector<ir::InsnId>& order     = cu.dom.order();
    const std::string              selfName(s.unit().symbols().str(s.unit().thisClass));

    ca::usize callPos = kNoSlot;
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu.dom.peek(order[pos]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.callSrcPc) {
            callPos = pos;
            break;
        }
    }
    if (callPos == kNoSlot) return siteFail(site, "调用点重定位失败");

    // 形状复核：调用点身份 + key/补位跨度重钉。
    const ir::Insn* call = cu.dom.peek(order[callPos]);
    if (call == nullptr) return siteFail(site, "调用点 peek 空");
    ir::ClassSession::MemberSem enc;
    if (!s.memberOf(*call, enc) || enc.kind != ir::ClassSession::MemberSem::Kind::Method
        || enc.owner != selfName || enc.name != site.dispName || enc.desc != site.dispDesc) {
        return siteFail(site, "调用点身份复核不符");
    }
    bool      labeled = false;
    ca::usize keyQ    = kNoPos;
    for (ca::usize q = callPos; q-- > 0;) {
        if (!cu.dom.labelsAt(order[q]).empty()) labeled = true;
        const ir::Insn* in = cu.dom.peek(order[q]);
        if (in == nullptr) return siteFail(site, "key 回走 peek 空");
        if (in->op != Op::NOP) {
            keyQ = q;
            break;
        }
    }
    if (labeled || keyQ == kNoPos) return siteFail(site, "key 位标签/不可达");
    ca::i32 key = 0;
    {
        const ir::Insn* keyInsn = cu.dom.peek(order[keyQ]);
        if (keyInsn == nullptr || !const_args::decodeConstInt(s, *keyInsn, key) || key != site.key) {
            return siteFail(site, "key 复核不符");
        }
    }
    ca::u32 nullRun = 0;
    for (ca::usize q = keyQ; q-- > 0;) {
        if (!cu.dom.labelsAt(order[q]).empty()) labeled = true;
        const ir::Insn* in = cu.dom.peek(order[q]);
        if (in == nullptr) return false;
        if (in->op == Op::NOP) continue;
        if (in->op != Op::ACONST_NULL) break;
        ++nullRun;
    }
    if (labeled) return false;
    // 补位互证（与 scan 同式）：addNulls = N - 目标消费；null 跑 ∈ [addNulls, N]。
    const auto  shape    = parseDispDesc(site.dispDesc);
    const ca::i32 consume = targetStackConsume(site.target.isVirtual, site.target.desc);
    if (!shape.has_value() || consume < 0) return siteFail(site, "desc/目标消费复核不符");
    const ca::i32 padCount = shape->keySlot - consume;
    if (padCount < 0 || padCount != static_cast<ca::i32>(site.padCount)
        || static_cast<ca::i32>(nullRun) < padCount
        || static_cast<ca::i32>(nullRun) > shape->keySlot) {
        return siteFail(site, "补位互证复核不符");
    }

    // 受害者清单：null 跑的**末段** padCount 个补位 + key（原实参尾部如有
    // null 字面量在跑的前段，保留不动——生成面补位块紧邻 key）。
    std::vector<ir::InsnId> victims;
    victims.reserve(static_cast<ca::usize>(padCount) + 1);
    ca::i32 toCollect  = padCount;
    ca::i32 frontNulls = 0;
    for (ca::usize q = keyQ; q-- > 0;) {
        const ir::Insn* in = cu.dom.peek(order[q]);
        if (in == nullptr) return siteFail(site, "受害者回走 peek 空");
        if (in->op == Op::NOP) {
            victims.push_back(order[q]);   // NOP 夹层一并入清单（跨度内被压
            continue;                      // 指令一并擦除，string_pool_fold 口径）
        }
        if (in->op != Op::ACONST_NULL) break;
        if (toCollect > 0) {
            victims.push_back(order[q]);
            --toCollect;
        } else {
            ++frontNulls;   // null 跑前段：原实参 null 字面量，保留
        }
    }
    victims.push_back(order[keyQ]);
    if (toCollect != 0
        || frontNulls != static_cast<ca::i32>(nullRun) - padCount) {
        return siteFail(site, "受害者清单断账");
    }

    // 调用指令原地换真目标（srcPc 保留：pcmap/调试行落点不变）。
    auto slotR = cu.dom.get(order[callPos]);
    if (slotR.is_err()) return siteFail(site, "调用点 dom.get 失败");
    ir::Insn*     insn     = slotR.unwrap();
    const ir::Insn original = *insn;
    insn->op = site.target.isVirtual ? Op::INVOKEVIRTUAL : Op::INVOKESTATIC;
    if (!s.setMember(*insn, ir::ClassSession::MemberSem::Kind::Method, site.target.owner,
                     site.target.name, site.target.desc)) {
        *insn = original;   ///< 改不动就还原：失败不留半截指令
        return siteFail(site, "目标成员 intern 失败");
    }
    if (!cu.dom.eraseBatch(victims).is_ok()) {
        *insn = original;
        return siteFail(site, "跨度 eraseBatch 被拒（引用未清）");
    }
    return true;
}

}   // namespace

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // Strict 帧重算上下文：层级图盖全容器（构建一次，整个 rewrite 复用）。
    rewrite_tools::FrameContext frames(container);

    // 站点按 uri 聚类（万类大罐上避免 classes × sites 平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           hit     = 0;
        ca::usize           virtualHits = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto        it = byUri.find(uri);
        if (it == byUri.end()) return;
        const std::vector<const Site*>& sitesOf = it->second;

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

        ca::usize hit     = 0;
        ca::usize failed  = 0;
        ca::usize virtualHits = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit  = 0;
            ca::usize methodFail = 0;
            ca::usize methodVir = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    for (const Site* site : sitesOfMethod) {
                        if (rewriteSite(s, cu, *site)) {
                            ++methodHit;
                            if (site->target.isVirtual) ++methodVir;
                        } else {
                            ++methodFail;
                        }
                    }
                    return methodHit > 0;
                });
            if (!ok) {
                // 帧链失败：该方法整方法放弃（session 内半改写态不外泄）。
                if (std::getenv("GRUNT_DISPATCHER_RESTORE_TRACE") != nullptr) {
                    std::fprintf(stderr,
                                 "[grunt.DispatcherRestore]   rewrite-fail %s#%zu: 帧链失败"
                                 "（hit=%zu fail=%zu）\n",
                                 uri.c_str(), mi, methodHit, methodFail);
                }
                failed += methodHit + methodFail;
                continue;
            }
            hit += methodHit;
            failed += methodFail;
            virtualHits += methodVir;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            dirty.failed += hit;
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit, virtualHits});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.sites += item.hit;
        dirty.classes += 1;
        dirty.rewrittenVirtual += item.virtualHits;
        dirty.rewrittenStatic += item.hit - item.virtualHits;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    // proof 是效力门（不变量复核），不许被 pass 级时间片截短——挂起期间
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

}   // namespace grunt::dispatcher_restore
