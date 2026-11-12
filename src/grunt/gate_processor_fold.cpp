/// @file        gate_processor_fold.cpp
/// @brief       残留 `gate_processor_fold` 的三段实现：SCAN / REWRITE / PROOF。
///
/// 形态、判据与改写契约见头文件（PredicateFold TAKEN 形同款）。本文件补扫
/// 描两段与求值环境的实现口径。
///
/// ## SCAN（形状钉 → 求值，两段）
///
/// 形状钉逐方法：INVOKESTATIC 描述符恰 `(III)I`（生成面 ActionDesc 指纹，
/// 其余 desc 静默跳过不记账）→ 逆向三实参 const_args::collectArgs（NOP 跳
/// 过，全 known i32）→ 前窗 parseForwardWindow：走廊 `[INT; (INT;
/// IMUL|IAND|IOR|IXOR)*]` 按 JVM 栈语义折叠（交换律 + 结合律下左折即真
/// 值），NOP 跳过；终点恰为 IF_ICMPEQ..IF_ICMPLE 且折叠栈恰剩 1 个 i32 =
/// compareValue。停点具名识别：引用 ThreadLocalRandom 的窗口变体单独入
/// tlrWindow 桶（运行期随机排除），其余停点 → forwardShape。目标解析（容
/// 器方法索引，constant_call 同源 keyOf）：容器外 / 非 static /
/// synchronized 逐桶；**目标有码检查在求值段做**——loc.index 是目标类下
/// 标而 scan 会话开在调用者类上，两套下标空间错位，在 scan 段查是单测钉
/// 过的缺陷面。
///
/// 求值按目标 owner 分组：目标类会话上 classInitNeutral 门（非中性时只留
/// 同类调用——JVMS 5.5 同类豁免，constant_call 同源）→ SimExecutor.invoke
/// 跨方法求值（编排层递归穿透 `__grt$indy$` 转发器；结果缓存 + 深度 6 +
/// 步数预算）→ known i32 出值 → gateVerdict(op, R, compareValue) 必须
/// taken（生成面恒真自证门；判非 taken = 求值与生成面脱钩，fail-closed）。
///
/// **mock 面（最小申报）**：只接管 `java/lang/Integer.rotateLeft(II)I`
/// （PredicateStep 盐旋转步生成面；jsim 核心/num 包无 JDK 数学面，缺它整
/// 桶 EvalBadState）。JVM 语义 = ishl/iushr 位移掩码 5 位
/// `(i << d) | (i >>> (32 - d))`（d &= 31，d=0 恒等）；unknown 按未知口径
/// 传播不编值。**刻意不装 str/num/reflect/indy 扩展包**：TLR / material /
/// indy 蹬床面必须在这里 fail-closed 显形——装包会把它们误放进折叠面。
///
/// ## REWRITE（TAKEN 形）
///
/// 按类聚类 → 方法级 roundTripSafe 前置门 → promote 一次 → DOM 改写 →
/// lower 全部候选方法（任一失败整类放弃，半改写态不外泄）。逐站：分支按
/// branchSrcPc + branchOp 重定位后做操作数区复核——逆向越过前窗找到门点
/// 调用并全等比对成员与三实参，再**正向重跑前窗解析**，终点分支与
/// compareValue 必须与 scan 全等（权威复核）；裁决复核仍 taken（纯函数下
/// 恒成立，防御面）。标签门：操作数区中段（right/salt/调用/窗内指令）或
/// 分支本体带标签（合并点）整站放弃；区域首条（left 锚）的标签安全。改
/// 写：锚（left 常量装载）原地改 ICONST_0（保 srcPc），分支本体原地换
/// IFEQ（目标 = 原分支目标，保 srcPc），其余受害者（right/salt/调用/窗内
/// 走廊）单批 eraseBatch（kRejectIfUsed——异常表/帧锚占用即拒收，整站回
/// 滚 anchor 与分支，不留半改写态）。不新增合并点、不插无条件 GOTO
/// （HotSpot 线性走查帧约束，predicate_fold 同款）；junk 臂原地保留成不可
/// 达码交下游 DeadCode。
///
/// ## PROOF
///
/// 类数不增 + 脏类可重解析 + residual 复扫（脏类集上重跑全判据——形状 +
/// 求值 + 恒真裁决）。改写 fail-closed 放弃的站点如实计入 residual，求值
/// 失败面从未进改写面不计入。
#include "grunt/gate_processor_fold.hpp"

#include <cstdio>
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
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::gate_processor_fold {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using proof_tools::roundTripSafe;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

/// 生成面 ActionDesc 逐字指纹（OpaquePredicateProcessorRegistry）。
constexpr std::string_view kGateDesc = "(III)I";

/// 扫描面 fail-closed 包裹（constant_call 同源）：单个类里的任何异常只丢该
/// 类的候选，不放大成整条管线失败。
template <class Fn>
bool scanGuard(std::string_view stage, std::string_view uri, Fn&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[grunt.GateProcessorFold] %.*s skip class %.*s: %s\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[grunt.GateProcessorFold] %.*s skip class %.*s: unknown exception\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data());
    }
    return false;
}

/// @brief 门点分支语义：生成面 Comparison 全族落在 IF_ICMP*（v1 = proc 结果
///        （先压，深槽），v2 = compareValue（顶槽））。
bool gateVerdict(Op op, ca::i32 v1, ca::i32 v2, bool& taken)
{
    switch (op) {
    case Op::IF_ICMPEQ: taken = v1 == v2; return true;
    case Op::IF_ICMPNE: taken = v1 != v2; return true;
    case Op::IF_ICMPLT: taken = v1 < v2;  return true;
    case Op::IF_ICMPGE: taken = v1 >= v2; return true;
    case Op::IF_ICMPGT: taken = v1 > v2;  return true;
    case Op::IF_ICMPLE: taken = v1 <= v2; return true;
    default: return false;
    }
}

/// @brief 拒绝记账：逐条进 rejects，桶计数同步自增。
void bumpReject(ScanResult& result, std::string_view owner, std::string_view method,
                ca::u32 callSrcPc, RejectReason reason)
{
    Reject rej;
    rej.callerOwner = std::string(owner);
    rej.callerMethod = std::string(method);
    rej.callSrcPc = callSrcPc;
    rej.reason = reason;
    result.rejects.push_back(std::move(rej));
    switch (reason) {
    case RejectReason::TargetOutside:        ++result.targetOutside; break;
    case RejectReason::TargetNotStatic:      ++result.targetNotStatic; break;
    case RejectReason::TargetSynchronized:   ++result.targetSynchronized; break;
    case RejectReason::TargetNoCode:         ++result.targetNoCode; break;
    case RejectReason::ArgsNotConstant:      ++result.argsNotConstant; break;
    case RejectReason::ForwardShape:         ++result.forwardShape; break;
    case RejectReason::TlrWindow:            ++result.tlrWindow; break;
    case RejectReason::ClassInitPresent:     ++result.classInitPresent; break;
    case RejectReason::EvalBadState:         ++result.evalBadState; break;
    case RejectReason::EvalStepLimit:        ++result.evalStepLimit; break;
    case RejectReason::ValueKindUnsupported: ++result.valueKindUnsupported; break;
    case RejectReason::EvalNotTrue:          ++result.evalNotTrue; break;
    }
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 容器方法索引构建：scan_tools::buildMethodIndex 收口（constant_call 同源
/// 口径），本 pass 只钉两件口径——重名类撞键取后到（insert_or_assign 原形
/// = KeepLast）、stderr 前缀走本 pass 的 scanGuard。
void buildIndex(const mjbase::model::ResourceContainer& container,
                scan_tools::MethodIndex& index)
{
    auto guard = [](std::string_view stage, std::string_view uri, auto&& fn) {
        return scanGuard(stage, uri, fn);
    };
    scan_tools::buildMethodIndex(container, index, scan_tools::IndexDup::KeepLast, guard);
}

/// @brief 前窗成员判定（rewrite 逆向定位调用用，宽松形——权威复核在正向
///        重跑的前窗解析）：i32 常量装载与走廊折叠算术。
bool isWindowInsn(const ir::Insn& in)
{
    switch (in.op) {
    case Op::ICONST_M1:
    case Op::ICONST_0:
    case Op::ICONST_1:
    case Op::ICONST_2:
    case Op::ICONST_3:
    case Op::ICONST_4:
    case Op::ICONST_5:
    case Op::BIPUSH:
    case Op::SIPUSH:
    case Op::LDC:
    case Op::LDC_W:
    case Op::IMUL:
    case Op::IAND:
    case Op::IOR:
    case Op::IXOR: return true;
    default: return false;
    }
}

/// @brief 前窗解析结局。
enum class WindowVerdict : ca::u8
{
    Ok,    ///< 命中：走廊折叠恰剩 1 个 i32，终点是 IF_ICMP*
    Bad,   ///< 形状走样（非生成面形 / 栈不平衡 / 越方法尾）
    Tlr,   ///< 停点引用 ThreadLocalRandom（RandomBound 窗口变体）
};

/// @brief 前窗解析：compareValue 常量走廊 `[INT; (INT; IMUL|IAND|IOR|IXOR)*]`
///        栈机折叠，终点恰为 IF_ICMP* 分支。
///
/// 生成面 compareValue.emit() 走 IntConstChain——罐 A 实测多数门点是多段走
/// 廊而非单常量。折叠按 JVM 栈语义（imul/iand/ior/ixor 全交换 + 结合，左折
/// 即真值）；窗内指令原样记入 `windowInsns`（rewrite 的受害者清单），分支
/// srcPc 走出参。ThreadLocalRandom 是唯一的具名停点：RandomBound 窗口变体
/// 运行期随机，单独成桶明确排除（其余停点一律 Bad）。
WindowVerdict parseForwardWindow(ir::ClassSession& s, const ir::CodeDom& dom,
                                 const std::vector<ir::InsnId>& order, ca::usize fromPos,
                                 std::vector<ir::InsnId>& windowInsns, ca::i32& compareValue,
                                 Op& branchOp, ca::u32& branchSrcPc)
{
    std::vector<jsim::SimValue> stack;
    std::vector<ir::InsnId>     ids;

    ca::usize j = fromPos;
    while (j < order.size()) {
        const ir::Insn* in = dom.peek(order[j]);
        if (in == nullptr) return WindowVerdict::Bad;
        if (in->op == Op::NOP) {   // squeeze 遗留落点不驻栈
            ++j;
            continue;
        }
        if (in->op >= Op::IF_ICMPEQ && in->op <= Op::IF_ICMPLE) {
            if (stack.size() != 1) return WindowVerdict::Bad;
            compareValue = static_cast<ca::i32>(stack.front().intBits);
            branchOp     = in->op;
            branchSrcPc  = in->srcPc;
            windowInsns  = std::move(ids);
            return WindowVerdict::Ok;
        }

        jsim::SimValue v;
        ca::u32        slots = 0;
        if (const_args::decodePush(s, *in, v, slots) && slots == 1 && v.known
            && v.type == jsim::ValType::I32) {
            stack.push_back(v);
            ids.push_back(order[j]);
            ++j;
            continue;
        }

        switch (in->op) {
        case Op::IMUL:
        case Op::IAND:
        case Op::IOR:
        case Op::IXOR: {
            if (stack.size() < 2) return WindowVerdict::Bad;
            const jsim::SimValue b = stack.back();
            stack.pop_back();
            const jsim::SimValue a = stack.back();
            stack.pop_back();
            if (!a.known || !b.known) return WindowVerdict::Bad;
            const ca::i32 va = static_cast<ca::i32>(a.intBits);
            const ca::i32 vb = static_cast<ca::i32>(b.intBits);
            ca::i32 r = 0;
            switch (in->op) {
            case Op::IMUL: r = static_cast<ca::i32>(static_cast<ca::u32>(va)
                                                   * static_cast<ca::u32>(vb)); break;
            case Op::IAND: r = va & vb; break;
            case Op::IOR:  r = va | vb; break;
            default:       r = va ^ vb; break;   // IXOR
            }
            stack.push_back(jsim::SimValue::knownI32(r));
            ids.push_back(order[j]);
            ++j;
            continue;
        }
        default: {
            // 具名停点：ThreadLocalRandom 引用（直调 current 取数形）。
            if (in->op == Op::INVOKESTATIC) {
                ir::ClassSession::MemberSem enc;
                if (s.memberOf(*in, enc)
                    && (enc.owner == "java/util/concurrent/ThreadLocalRandom"
                        || enc.desc.find("ThreadLocalRandom") != std::string::npos)) {
                    return WindowVerdict::Tlr;
                }
            }
            return WindowVerdict::Bad;
        }
        }
    }
    return WindowVerdict::Bad;   // 越方法尾
}

/// @brief 形状钉：门点调用锚（INVOKESTATIC (III)I）逆向三常量 + 前窗
///        「i32 常量 → IF_ICMP*」逐字。命中产出 Site（未求值）。
void scanMethod(ir::ClassSession& s, ca::usize methodIndex, const std::string& uri,
                std::string_view self, const scan_tools::MethodIndex& index,
                ScanResult& result)
{
    if (!ir::hasCode(s, methodIndex)) return;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return;

    const ir::ClassUnit& unit = s.unit();
    const std::string_view name = unit.symbols().str(unit.methods[methodIndex].name);
    const std::vector<ir::InsnId>& order = cu->dom.order();

    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const ir::Insn* in = cu->dom.peek(order[pos]);
        if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

        ir::ClassSession::MemberSem enc;
        if (!s.memberOf(*in, enc)) continue;
        if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;
        if (enc.desc != kGateDesc) continue;   // 非门点 desc：非候选，不进桶

        const auto reject = [&](RejectReason r) {
            bumpReject(result, self, name, in->srcPc, r);
        };

        // 逆向三实参：紧邻纯常量装载（NOP 跳过，squeeze 先行后操作数区重新
        // 可见），槽数严格吻合。
        std::vector<ir::InsnId>     producers;
        std::vector<jsim::SimValue> args;
        if (!const_args::collectArgs(s, cu->dom, order, pos, 3, producers, args)) {
            reject(RejectReason::ArgsNotConstant);
            continue;
        }
        bool argsOk = args.size() == 3;
        for (const jsim::SimValue& a : args) {
            if (!a.known || a.type != jsim::ValType::I32) argsOk = false;
        }
        if (!argsOk) {
            reject(RejectReason::ArgsNotConstant);
            continue;
        }

        // 前窗：compareValue 常量走廊 → IF_ICMP* 分支（生成面逐字）。其它任
        // 何内容整站拒绝；停点引用 ThreadLocalRandom 的（RandomBound 窗口变
        // 体）单独入 TlrWindow 桶——运行期随机不进折叠面。
        std::vector<ir::InsnId> windowInsns;
        ca::i32                 compareValue = 0;
        Op                      branchOp     = Op::NOP;
        ca::u32                 branchSrcPc  = 0;
        const WindowVerdict     wv = parseForwardWindow(s, cu->dom, order, pos + 1,
                                                        windowInsns, compareValue, branchOp,
                                                        branchSrcPc);
        if (wv != WindowVerdict::Ok) {
            reject(wv == WindowVerdict::Tlr ? RejectReason::TlrWindow
                                            : RejectReason::ForwardShape);
            continue;
        }

        ++result.totalShape;

        // 目标解析：容器内 static 有码（(III)I 恒有返回值，无需 void 桶）。
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
        // 目标有码检查不在这里做：loc.index 是目标类的方法下标，本会话是
        // 调用者类——检查挪到求值段（目标类会话在手，constant_call 同位）。

        Site site;
        site.uri              = uri;
        site.methodIndex      = methodIndex;
        site.callerOwner      = std::string(self);
        site.callerMethod     = std::string(name);
        site.callSrcPc        = in->srcPc;
        site.branchSrcPc      = branchSrcPc;
        site.branchOp         = branchOp;
        site.targetOwner      = loc.owner;
        site.targetName       = std::string(enc.name);
        site.targetDesc       = std::string(enc.desc);
        site.targetMethodIndex = loc.index;
        site.args             = args;
        site.compareValue     = compareValue;
        result.sites.push_back(std::move(site));
    }
}

/// @brief 求值段：装好 rotateLeft 接管的 jsim 执行器逐站点跨方法跑 proc；
///        通过者写回 `result`，失败者转拒绝记录。类初始化门先判。
void evaluateSites(const mjbase::model::ResourceContainer& container, ScanResult& result)
{
    std::unordered_map<std::string, std::vector<ca::usize>> byOwner;
    for (ca::usize i = 0; i < result.sites.size(); ++i) {
        byOwner[result.sites[i].targetOwner].push_back(i);
    }

    std::vector<ca::u8> ok(result.sites.size(), 0);   // 0 待判 / 1 通过 / 2 拒绝

    SimExecutor exec(container);
    // 不装任何扩展包（str/num/reflect/indy 全不装）：门点 proc 体按生成面是
    // 纯算术 + Integer.rotateLeft，任何堆访问/字符串/indy 面都该在求值处
    // fail-closed（TLR / material-key 桶靠这条边界显形），装包会把这些面误
    // 放进来。
    //
    // Integer.rotateLeft (II)I 接管（本文件内最小扩面，申报理由：门点 proc
    // 体的 PredicateStep AddSaltRotate/RotateBySalt 生成面 emit
    // `INVOKESTATIC java/lang/Integer.rotateLeft(II)I`（IntExpression.kt 盐
    // 旋转步）；jsim 核心/num 包没有 JDK 数学面，缺这个接管整桶
    // EvalBadState 拒——罐 A 供给链实测 4282 个 const-fed 门点全部因此存活。
    // rotateLeft 是全序纯函数：JVM ishl/iushr 位移掩码 5 位，双常量实参下值
    // 唯一确定；unknown 按未知口径传播（折叠面 ValueKindUnsupported 拒），
    // 不编值。）
    exec.session().callMocks["java/lang/Integer.rotateLeft(II)I"] =
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

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("evaluate", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const std::string_view self = session->backing().thisName();

            const auto group = byOwner.find(std::string(self));
            if (group == byOwner.end()) return;

            if (!scan_tools::classInitNeutral(*session)) {
                // 类初始化门（constant_call 同源）：删掉指向本类的调用会改变
                // 被调类的初始化时机——照旧整组拒绝。同类调用豁免（门点 proc
                // 与蹬床和门点同类是生成面事实）：JVMS 5.5 本类方法开始执行
                // 时类已在初始化中/已完成，同类 INVOKESTATIC 不触发初始化，
                // 删调用不移时机；豁免站点的纯性由封闭环境求值独立证明。
                bool anyKept = false;
                for (const ca::usize si : group->second) {
                    const Site& site = result.sites[si];
                    if (site.targetOwner == site.callerOwner) {
                        anyKept = true;
                        continue;
                    }
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               RejectReason::ClassInitPresent);
                    ok[si] = 2;
                }
                if (!anyKept) return;
            }

            for (const ca::usize si : group->second) {
                Site& site = result.sites[si];
                if (ok[si] == 2) continue;   // 门组拒绝已记账

                if (!ir::hasCode(*session, site.targetMethodIndex)) {
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               RejectReason::TargetNoCode);
                    ok[si] = 2;
                    continue;
                }

                const SimOutcome out =
                    exec.invoke(site.targetOwner, site.targetName + site.targetDesc, site.args);
                if (out.kind == SimOutcome::Kind::Void) {
                    // (III)I 恒有返回值；到这里说明桥转换把体读成了 void，按
                    // 求值失败面记账。
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               RejectReason::EvalBadState);
                    ok[si] = 2;
                    continue;
                }
                if (!out.ok()) {
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               out.kind == SimOutcome::Kind::LimitHit
                                   ? RejectReason::EvalStepLimit
                                   : RejectReason::EvalBadState);
                    ok[si] = 2;
                    continue;
                }
                if (!out.value.known || out.value.type != jsim::ValType::I32) {
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               RejectReason::ValueKindUnsupported);
                    ok[si] = 2;
                    continue;
                }

                // 恒真裁决（生成面 require 保证）：判非 taken = 求值与生成面
                // 脱钩（mock 语义走样 / 目标错解），fail-closed 拒绝。
                bool taken = false;
                const ca::i32 r = static_cast<ca::i32>(out.value.intBits);
                if (!gateVerdict(site.branchOp, r, site.compareValue, taken) || !taken) {
                    bumpReject(result, site.callerOwner, site.callerMethod, site.callSrcPc,
                               RejectReason::EvalNotTrue);
                    ok[si] = 2;
                    continue;
                }
                site.result = r;
                ok[si]      = 1;
            }
        });
    });

    // 没被任何目标类覆盖的站点（目标类会话开不了 / 时间片止步）统一按
    // 目标无码记账（fail-closed 同口径）。
    std::vector<Site> kept;
    kept.reserve(result.sites.size());
    for (ca::usize i = 0; i < result.sites.size(); ++i) {
        if (ok[i] == 1) {
            kept.push_back(std::move(result.sites[i]));
        } else if (ok[i] == 0) {
            bumpReject(result, result.sites[i].callerOwner, result.sites[i].callerMethod,
                       result.sites[i].callSrcPc, RejectReason::TargetNoCode);
        }
    }
    result.sites = std::move(kept);
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

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("scan", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) {
                ++result.classesSkipped;
                return;
            }
            ir::ClassSession&         s    = *session;
            const std::string_view    self = s.backing().thisName();
            for (ca::usize mi = 0; mi < s.unit().methods.size(); ++mi) {
                scanMethod(s, mi, std::string(entry.uri), self, index, result);
            }
        });
    });

    evaluateSites(container, result);
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
/// （帧链由 lower 的 pcmap 重定位——本改写不新增合并点：TAKEN 落
/// [ICONST_0, IFEQ→T] 恒真条件跳转、锚原地改写保 srcPc，被擦指令若被帧/标
/// 签/异常表锚定 eraseBatch 会拒收整站放弃并回滚）。
ca::usize rewriteMethod(ir::ClassSession& s, ir::CodeUnit* cu,
                        const std::vector<const Site*>& sites, ca::usize& failed,
                        std::vector<ca::usize>& failedByReason)
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

        // 按 srcPc+op 重定位分支（跨会话稳定，InsnId 不带出去）。
        ca::usize branchPos = kNoIndex;
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const ir::Insn* in = dom.peek(order[pos]);
            if (in != nullptr && in->srcPc == site.branchSrcPc && in->op == site.branchOp) {
                branchPos = pos;
                break;
            }
        }
        if (branchPos == kNoIndex) {
            bumpFail(RwResult::BranchNotFound);
            continue;
        }
        const ir::InsnId branchId = order[branchPos];
        const ir::Insn*  branch   = dom.peek(branchId);
        if (branch == nullptr) {
            bumpFail(RwResult::BranchNotFound);
            continue;
        }
        // 目标标签值先拷出：分支本体随后要被变异。
        const ir::LabelId takenTarget = branch->target;

        // 操作数区复核（scan 之后字节可能已被同方法其他站点改写，不盲改）：
        // 从分支逆向越过前窗（走廊常量与折叠算术）定位门点调用，成员全等；
        // 再从调用正向重跑前窗解析，分支/compareValue/实参值与 scan 全等。
        ca::usize callPos = kNoIndex;
        {
            ca::i32 j = static_cast<ca::i32>(branchPos) - 1;
            while (j >= 0) {
                const ir::Insn* p = dom.peek(order[static_cast<ca::usize>(j)]);
                if (p == nullptr) break;
                if (p->op == Op::NOP || isWindowInsn(*p)) {
                    --j;
                    continue;
                }
                if (p->op == Op::INVOKESTATIC) callPos = static_cast<ca::usize>(j);
                break;
            }
        }
        if (callPos == kNoIndex) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        const ir::Insn* call = dom.peek(order[callPos]);
        if (call == nullptr || call->op != Op::INVOKESTATIC) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        ir::ClassSession::MemberSem enc;
        if (!s.memberOf(*call, enc) || enc.owner != site.targetOwner
            || enc.name != site.targetName || enc.desc != site.targetDesc) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        std::vector<ir::InsnId>     producers;
        std::vector<jsim::SimValue> args;
        if (!const_args::collectArgs(s, dom, order, callPos, 3, producers, args)
            || args.size() != site.args.size()) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        bool argsSame = true;
        for (ca::usize i = 0; i < args.size(); ++i) {
            if (!const_args::sameValue(args[i], site.args[i])) argsSame = false;
        }
        if (!argsSame) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }

        // 前窗复核：正向重跑解析，终点分支与 compareValue 必须与 scan 一致
        // （门点调用 + 窗内指令清单即受害者余量）。
        std::vector<ir::InsnId> windowInsns;
        ca::i32                 compareValue = 0;
        Op                      branchOp     = Op::NOP;
        ca::u32                 branchSrcPc  = 0;
        if (parseForwardWindow(s, dom, order, callPos + 1, windowInsns, compareValue,
                               branchOp, branchSrcPc) != WindowVerdict::Ok
            || branchSrcPc != site.branchSrcPc || branchOp != site.branchOp
            || compareValue != site.compareValue) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }
        producers.push_back(order[callPos]);
        for (const ir::InsnId id : windowInsns) producers.push_back(id);

        // 防御复核：裁决与 scan 一致（纯函数下恒成立；不一致说明求值面走样）。
        bool taken = false;
        if (!gateVerdict(site.branchOp, site.result, site.compareValue, taken) || !taken) {
            bumpFail(RwResult::VerdictChanged);
            continue;
        }

        // 标签门：区域中段或分支本体带标签 = 合并点，外部到达路径的栈形状
        // 与区域产出耦合，整站放弃；区域首条（anchor）的标签安全。
        bool blocked = !dom.labelsAt(branchId).empty();
        for (ca::usize i = 1; i < producers.size(); ++i) {
            if (!dom.labelsAt(producers[i]).empty()) blocked = true;
        }
        if (blocked) {
            bumpFail(RwResult::LabelBlocked);
            continue;
        }

        auto slotR = dom.get(producers.front());
        if (slotR.is_err()) {
            bumpFail(RwResult::RegionMismatch);
            continue;
        }

        // TAKEN：恒真条件跳转形态 [ICONST_0, IFEQ→T]——锚改常量、分支本体
        // 原地换 op+目标、其余操作数擦除。不插无条件 GOTO：HotSpot 线性走查
        // 要求无条件跳转之后的段首带帧，而被擦区域的延续段在原 SMT 中无帧；
        // 条件跳转让延续段保持 fallthrough 可达，帧面零改动（predicate_fold
        // 同款契约）。
        ir::Insn*     anchor  = slotR.unwrap();
        const ir::Insn original = *anchor;
        const ca::u32 keepPc = anchor->srcPc;

        ir::Insn fresh{};
        fresh.op    = Op::ICONST_0;
        fresh.srcPc = keepPc;
        *anchor     = fresh;

        ir::Insn*     branchMut      = nullptr;
        ir::Insn      branchOriginal{};
        auto branchR = dom.get(branchId);
        if (branchR.is_err()) {
            *anchor = original;
            bumpFail(RwResult::EraseFail);
            continue;
        }
        branchMut      = branchR.unwrap();
        branchOriginal = *branchMut;
        ir::Insn branchFresh{};
        branchFresh.op     = Op::IFEQ;
        branchFresh.target = takenTarget;
        branchFresh.srcPc  = branchOriginal.srcPc;
        *branchMut         = branchFresh;

        // 其余操作数（right/salt/调用/compareValue）单批擦除。eraseBatch 是
        // kRejectIfUsed：受害者被异常表 start/end 引用时拒收——拒收即整站
        // 放弃，anchor/分支回滚原样，不留「常量装载已被覆盖、分支还在」的
        // 半改写态（栈下溢 VerifyError）。
        std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
        if (!victims.empty() && dom.eraseBatch(victims).is_err()) {
            *anchor    = original;
            *branchMut = branchOriginal;
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
                methodHit = rewriteMethod(s, cu, sitesOfMethod, failed, failedByReason);
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

    // residual：脏类集上重跑全判据（形状 + 目标解析 + 跨方法求值 + 恒真裁
    // 决）仍成站点的数量。改写 fail-closed 放弃的站点如实入账；求值失败面
    // （TLR/material，从未进改写面）不计入。效力门挂时间片，全量盘点。
    if (!dirty.uris.empty()) {
        const pass_clock::Suspend suspend;
        const std::set<std::string> dirtySet(dirty.uris.begin(), dirty.uris.end());

        scan_tools::MethodIndex index;
        buildIndex(container, index);

        ScanResult rescan;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (dirtySet.count(std::string(entry.uri)) == 0) return;
            scanGuard("residual", entry.uri, [&] {
                auto session = openSession(*entry.bytes);
                if (!session) return;
                ir::ClassSession&      s    = *session;
                const std::string_view self = s.backing().thisName();
                for (ca::usize mi = 0; mi < s.unit().methods.size(); ++mi) {
                    scanMethod(s, mi, std::string(entry.uri), self, index, rescan);
                }
            });
        });
        evaluateSites(container, rescan);
        result.residual = rescan.sites.size();
    }
    return result;
}

}   // namespace grunt::gate_processor_fold
