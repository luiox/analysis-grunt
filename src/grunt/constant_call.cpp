/// @file        constant_call.cpp
/// @brief       残留 `constant_call` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 判据（三条同时成立才进 sites）
///
/// 粗找 = 全容器扫 INVOKESTATIC 且目标在容器内（不判实参、不判纯度，只保
/// 证不漏）。细找三条：
///
/// 1. **目标可折**：ACC_STATIC 置位；非 ACC_SYNCHRONIZED（监视器获取是可
///    观测行为，删调用就变了）；名字不以 `<` 开头（排除 <init>/<clinit>）；
///    返回类型非 void（无值可折）；在容器内有方法体。
/// 2. **实参定值**：从调用点向前逐条走，每条都是纯常量装载，累计槽数恰好
///    等于描述符参数槽数（long/double 双槽）；中途遇到任何非常量装载
///    （iload、getstatic、dup 等）即拒绝。ldc 经会话常量池解析：数值常量
///    接受，String/Class/MethodHandle 等一律拒绝（不猜内容）。
/// 3. **求值成功**：mjbase 的 asm_evaluator 在常量实参下跑目标方法。求值
///    子集只含数值运算、栈整形、分支与返回——不含字段访问、数组访问、方
///    法调用、对象分配、显式抛出，因此「跑出值」等价于「这条执行路径无堆
///    副作用」。
///
/// ## 类初始化门与同类豁免
///
/// 被调类的 <clinit> 必须缺失或只剩 NOP/RETURN 且无异常表：invokestatic 是
/// 类的主动使用，删掉调用会改变类初始化时机。同类调用豁免（JVMS 5.5）：本
/// 类方法开始执行时类已在初始化中/已完成，同类 INVOKESTATIC 不触发初始化，
/// 删调用不移时机。豁免只作用于初始化时机这一个门，纯性、常量实参、求值
/// 三条判据对同类站点照收不误。
///
/// ## 求值环境接管：java/lang/Math.max(II)I
///
/// IndyRestore 还原 `__grt$indy$` 蹬床体内的 indy 后，CFF inline 桥的
/// MaxConst 站点（grunt3 `StateOp.MaxConst.emitTo` 逐字）从「蹬床调用」变
/// 成「蹬床体内 invokestatic java/lang/Math.max(II)I」，求值蹬床体必然踩到
/// 这条 JDK 调用；而 jsim 核心/num 包没有 JDK 数学面，缺接管时整条面在
/// EvalBadState 拒。求值会话按签名注册 Math.max 接管：弹双参、全序取大；
/// unknown 按未知口径传播（结果落 ValueKindUnsupported），不编值。max 是
/// 全序纯函数，双常量实参下值唯一确定，接管不引入猜测面。
///
/// ## 为什么求值放在 scan 里
///
/// 三段的状态需求本来就不对称，只有 scan 需要跨站点的昂贵缓存。求值是判
/// 定的一部分（「这个调用是不是常量」），不是改写的一部分；放进 scan 才能
/// 让 rewrite 只消费结果、不回头找站点。
///
/// ## REWRITE（每站六步，任一步失败整类放弃）
///
/// 按 调用类 → 调用方法 → 站点 分组。改写前先过一道**往返自检**：在未改
/// 动的独立会话上把方法 promote → lower 走一遍，走不完的整方法跳过
/// （skippedByRoundTrip）。理由：lower 不是 promote 的恒等逆（编码器对少数
/// 布局/属性组合明确拒绝），这类方法与改写无关，但一旦被改写就会把整个类
/// 拖下水，表现为「改写有 bug」；先探一遍，失败面就只剩真正的改写缺陷。
///
/// 每站六步：按 callSrcPc 在重新 promote 的 DOM 里找回调用指令（InsnId 是
/// 会话内句柄不能跨会话带出，所以站点用原始 pc 定位）；校验成员引用与
/// ScanResult 一致；用与 scan 相同的规则重新收集实参并逐值比对（防 scan
/// 与 rewrite 之间 DOM 语义漂移，不一致放弃该站点）；造常量装载
/// （iconst/bipush/sipush 优先，放不下才落池，池索引一律经
/// CodeEditor::loadConst 选形态——LDC 操作数域只有 u1，超界编码器直接
/// Unencodable）；把首个实参装载**原地改写**成常量装载，eraseBatch 擦掉
/// 其余实参装载 + 调用；失败把首条写回原样，DOM 保持原样。
///
/// ### 为什么是「原地改写首条」而不是「插一条 + 删首条」
///
/// 不是风格选择，是唯一能走通的路：CodeDom::insertAt 把新指令的 srcPc 硬
/// 清成 kNoPc（合成指令没有旧位置，DOM 不变量），而 lowerMethod 的属性重
/// 定位全靠 pcmap——LineNumberTable 每行、LocalVariableTable 起止、
/// StackMapTable 每帧都要精确命中一个旧 pc，命中不了就 RelocationBadTarget
/// 整体失败。删掉首条会让锚在区间起点的行全部失去落点，整个方法连同类一
/// 起作废（注入的实参装载继承被替换指令的行号/帧位，样本里几乎必然）。
/// 原地改写保住首条的 srcPc：锚在它上面的行照常重定位，锚在它上面的标签
/// 不必迁移。区间其余指令的旧 pc 随之消失，因此额外要求这些指令上没有标
/// 签——有标签说明分支/switch/异常范围从区间中段进入，删掉会让目标悬垂
/// （LabelBlocked 整站放弃）；首条不受此限。
///
/// 栈形不变：原序列 n 条装载（合计 s 槽）+ invokestatic（弹 s 压 r，
/// r ∈ {1,2}），新序列 1 条装载（压 r 槽），调用前后栈深相同——
/// StackMapTable 按新布局重算即可，maxStack 沿用原值（新峰值 base+1 不超
/// 原峰值 base+s）。
///
/// ## PROOF
///
/// 两道门：类数不增；每个脏类重新开会话可解析（抓「写出的字节自己都读不
/// 回来」这类错误）。帧与校验器层面的验收在语料回归里做
/// （test/tools/VerifyJar.java 逐类过校验器——需要 JDK，不进单元测试）。
///
/// ## 已知边界
///
/// 不做跨方法常量传播：目标读字段或调别的纯方法时求值器 Unsupported，站
/// 点被拒。不做 <clinit> 常量传播：getstatic + iaload 的字段池化数字不在
/// 面内。不折引用结果：返回 String/对象的方法即使纯也不折——引用身份不
/// 可静态构造。processor 键解锁链是静态天花板：CFF 桥 key processor 形
/// （五条窗口 [INT; ISTORE; ILOAD; INT; INVOKESTATIC proc]）的 proc 恢复目
/// 标全部落在 native 摘除的 action 壳上（算术已随 native pipeline 出
/// jar），实参又是 ILOAD 夹心形非紧邻常量——两道静态墙叠加，这部分永远
/// 不可静态折，属 CffUnflatten key-processor 桶的永久余量；可折的只有
/// Math.max 蹬床面。
#include "grunt/constant_call.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/jsim_executor.hpp"
#include "grunt/knowledge/const_args.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

#include "jsim/indy.hpp"
#include "jsim/num.hpp"
#include "jsim/reflect.hpp"
#include "jsim/str.hpp"

namespace grunt::constant_call {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;

constexpr ca::u32 kAccStatic       = 0x0008;
constexpr ca::u32 kAccSynchronized = 0x0020;
constexpr ca::usize kNoIndex       = static_cast<ca::usize>(-1);

/// 扫描面 fail-closed 包裹：单个类里的任何异常只丢该类的候选，不放大成
/// 整条管线失败。本残留是全局 pass，异常会一路穿到 mjbase 管线的
/// `catch (...)`，CLI 直接 exit 3（「unknown exception」整包拒绝）——
/// 对齐仓内保守口径（rewrite_toolbox::mutateMethodWithFrames 的两级收口）：
/// std::exception 带 what()，其余兜底；该类跳过并打 stderr 日志。
template <class Fn>
bool scanGuard(std::string_view stage, std::string_view uri, Fn&& fn)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: %s\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[grunt.ConstantCall] %.*s skip class %.*s: unknown exception\n",
                     static_cast<int>(stage.size()), stage.data(),
                     static_cast<int>(uri.size()), uri.data());
    }
    return false;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

/// 返回类别（'V' 表示 void）。
char returnChar(std::string_view desc)
{
    const ca::usize rp = desc.find(')');
    if (rp == std::string_view::npos || rp + 1 >= desc.size()) return 'V';
    return desc[rp + 1];
}

/// 容器方法索引构建：scan_tools::buildMethodIndex 收口，本 pass 只钉两件
/// 口径——重名类撞键取先到（emplace 原形 = KeepFirst）、stderr 前缀走本
/// pass 的 scanGuard。
void buildIndex(const mjbase::model::ResourceContainer& container,
                scan_tools::MethodIndex& index)
{
    auto guard = [](std::string_view stage, std::string_view uri, auto&& fn) {
        return scanGuard(stage, uri, fn);
    };
    scan_tools::buildMethodIndex(container, index, scan_tools::IndexDup::KeepFirst, guard);
}

/// 求值段：装好扩展包的 jsim 执行器逐站点跑目标方法；通过者写回 `value`，
/// 失败者转成拒绝记录。类初始化门（scan_tools::classInitNeutral）先判。
void evaluateSites(const mjbase::model::ResourceContainer& container,
                   std::vector<Site>& sites, std::vector<RejectedSite>& rejects)
{
    std::unordered_map<std::string, std::vector<ca::usize>> byOwner;
    for (ca::usize i = 0; i < sites.size(); ++i) byOwner[sites[i].targetOwner].push_back(i);

    std::vector<ca::u8> ok(sites.size(), 0);   // 0 待判 / 1 通过 / 2 拒绝

    SimExecutor exec(container);
    jsim::str::install(exec.session());
    jsim::num::install(exec.session());
    // 折叠不需要注解/类元数据：目标按判据是纯算术，反射元数据无人读。
    jsim::reflect::install(exec.session(), [](const std::string&)
                               -> const jsim::reflect::ClassMeta* { return nullptr; });
    jsim::indy::install(exec.session());

    // Math.max (II)I 接管（本文件内最小扩面，申报理由： IndyRestore 还原
    // `__grt$indy$` 蹬床体内的 indy 后，CFF inline 桥的 MaxConst 站点从
    // 「蹬床调用」还原成「蹬床体内 invokestatic java/lang/Math.max(II)I」；
    // 求值蹬床体时必然踩到这条 JDK 调用，jsim 核心/num 包没有 JDK 数学面，
    // 缺这个接管整条面在 EvalBadState 拒——processor 键解锁链断在最后一环。
    // Math.max 是全序纯函数：双常量实参下值唯一确定；unknown 按未知口径
    // 传播（折叠面 ValueKindUnsupported 拒），不编值。）
    exec.session().callMocks["java/lang/Math.max(II)I"] =
        [](jsim::Frame& frame) -> jsim::Result<jsim::SimValue, jsim::Bail> {
            jsim::Result<jsim::SimValue, jsim::Bail> b = frame.popChecked();
            if (!b.isOk()) return b;
            jsim::Result<jsim::SimValue, jsim::Bail> a = frame.popChecked();
            if (!a.isOk()) return a;
            if (!a.value().known || !b.value().known) {
                return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                    jsim::SimValue::unknownOf(jsim::ValType::I32));
            }
            const ca::i32 va = static_cast<ca::i32>(a.value().intBits);
            const ca::i32 vb = static_cast<ca::i32>(b.value().intBits);
            return jsim::Result<jsim::SimValue, jsim::Bail>::ok(
                jsim::SimValue::knownI32(va > vb ? va : vb));
        };

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("evaluate", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            const std::string_view self = session->backing().thisName();

            const auto group = byOwner.find(std::string(self));
            if (group == byOwner.end()) return;

            if (!scan_tools::classInitNeutral(*session)) {
                // 类初始化门：跨类调用删掉会改变被调类的初始化时机——照旧
                // 整组拒绝。同类调用（boar proc 蹬床形态：NativePreProcessor
                // 的桥与蹬床同类）豁免——JVMS 5.5：本类方法开始执行时类已经
                // 在初始化中/已完成，同类 INVOKESTATIC 不触发初始化，删调用
                // 不移时机；豁免站点的纯性由下面的封闭环境求值独立证明
                // （boar 取证：全部 3262 个可折蹬床站点都落在非中性 clinit
                // 类里，门不豁免则整条 processor 键解锁链空转）。
                bool anyKept = false;
                for (const ca::usize si : group->second) {
                    const Site& site = sites[si];
                    if (site.targetOwner == site.callerOwner) {
                        anyKept = true;
                        continue;
                    }
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc,
                                                   RejectReason::ClassInitPresent});
                    ok[si] = 2;
                }
                if (!anyKept) return;
            }

            for (const ca::usize si : group->second) {
                Site& site = sites[si];

                if (ok[si] == 2) continue;   // 门组拒绝已记账

                if (!ir::hasCode(*session, site.targetMethodIndex)) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetNoCode});
                    ok[si] = 2;
                    continue;
                }

                const SimOutcome out =
                    exec.invoke(site.targetOwner, site.targetName + site.targetDesc, site.args);
                if (out.kind == SimOutcome::Kind::Void) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc, RejectReason::TargetVoid});
                    ok[si] = 2;
                    continue;
                }
                if (!out.ok()) {
                    rejects.push_back(RejectedSite{
                        site.callerOwner, site.callerMethod, site.callSrcPc,
                        out.kind == SimOutcome::Kind::LimitHit ? RejectReason::EvalStepLimit
                                                               : RejectReason::EvalBadState});
                    ok[si] = 2;
                    continue;
                }
                if (!out.value.known || out.value.type == jsim::ValType::Ref) {
                    rejects.push_back(RejectedSite{site.callerOwner, site.callerMethod,
                                                   site.callSrcPc,
                                                   RejectReason::ValueKindUnsupported});
                    ok[si] = 2;
                    continue;
                }
                site.value = out.value;
                ok[si]     = 1;
            }
        });
    });

    // 没被任何目标类覆盖的站点（目标类解析失败等）统一拒绝。
    std::vector<Site> kept;
    kept.reserve(sites.size());
    for (ca::usize i = 0; i < sites.size(); ++i) {
        if (ok[i] == 1) {
            kept.push_back(std::move(sites[i]));
        } else if (ok[i] == 0) {
            rejects.push_back(RejectedSite{sites[i].callerOwner, sites[i].callerMethod,
                                           sites[i].callSrcPc, RejectReason::TargetNoCode});
        }
    }
    sites = std::move(kept);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

/// 改写失败原因（trace 用）。
enum class RwResult : ca::u8 {
    Ok = 0,
    CallNotFound,
    MemberMismatch,
    ArgsMismatch,
    LoadFail,
    InsertFail,
    LabelBlocked,
    EraseFail,
};

// ---- 诊断：谁在指「将被擦除的 pc」（GRUNT_DEBUG_PCREFS）---------------------
//
// 擦掉指令后这些旧 pc 不在 lower 的 pcmap 里，fail-closed 重定位会直接拒
// （RelocationBadTarget）。这个转储把「哪类属性、多少行」摆出来，用来判
// 该由改写侧一并作废，还是该整站点放弃。

bool pcIn(const std::vector<ca::u32>& dead, ca::u32 pc)
{
    return std::find(dead.begin(), dead.end(), pc) != dead.end();
}

void dumpPcRefs(ir::ClassSession& session, ca::usize methodIndex,
                const std::vector<ca::u32>& dead)
{
    if (std::getenv("GRUNT_DEBUG_PCREFS") == nullptr || dead.empty()) return;

    jbc::core::ClassFile& cf = session.backing();
    const ir::ClassUnit&  u  = session.unit();
    if (methodIndex >= u.methods.size()) return;
    const ca::usize backing = u.methods[methodIndex].backingIndex;
    if (backing >= cf.methods.size()) return;

    auto&          attrs = cf.methods[backing].attrs;
    ca::usize      codeSlot = kNoIndex;
    for (ca::usize i = 0; i < attrs.size(); ++i) {
        if (cf.utf8(attrs[i].nameCp) == "Code") {
            codeSlot = i;
            break;
        }
    }
    if (codeSlot == kNoIndex) return;
    // Code 槽与嵌套属性统一先过注解族深度预检（mjbase #158）：Code 内可挂
    // 注解族（TypeAnnotations），libjbc 注解码码互递归无界，敌意深嵌输入
    // 会在 decodeAttr 内栈溢出；预检拒绝按「解不开原样保真」跳过，诊断面
    // 少打一行，进程不被打爆。
    if (!mjbase::util::attrSafeToDecode(cf, attrs[codeSlot])) return;
    auto d = cf.decodeAttr(attrs[codeSlot]);
    if (d.is_err()) return;
    auto& code = static_cast<jbc::core::CodeAttr&>(*d.unwrap());

    std::fprintf(stderr, "[pcrefs] dead=");
    for (const ca::u32 pc : dead) std::fprintf(stderr, " %u", pc);
    std::fprintf(stderr, " | codeLen=%zu nested=%zu\n", code.code.size(), code.attrs.size());

    for (jbc::core::AttrSlot& nested : code.attrs) {
        if (!mjbase::util::attrSafeToDecode(cf, nested)) continue;
        auto nd = cf.decodeAttr(nested);
        if (nd.is_err()) continue;
        jbc::core::AttrBody* nb = nd.unwrap();
        switch (nb->kind()) {
        case jbc::core::AttrKind::LineNumberTable: {
            auto& lnt = static_cast<jbc::core::LineNumberTableAttr&>(*nb);
            ca::usize hit = 0;
            for (const auto& row : lnt.rows) hit += pcIn(dead, row.startPc) ? 1 : 0;
            std::fprintf(stderr, "[pcrefs]   LNT rows=%zu dead=%zu\n", lnt.rows.size(), hit);
            break;
        }
        case jbc::core::AttrKind::LocalVariableTable: {
            auto& lvt = static_cast<jbc::core::LocalVariableTableAttr&>(*nb);
            ca::usize hitStart = 0, hitEnd = 0;
            for (const auto& row : lvt.rows) {
                hitStart += pcIn(dead, row.startPc) ? 1 : 0;
                hitEnd += pcIn(dead, row.startPc + row.length) ? 1 : 0;
            }
            std::fprintf(stderr, "[pcrefs]   LVT rows=%zu deadStart=%zu deadEnd=%zu\n",
                         lvt.rows.size(), hitStart, hitEnd);
            break;
        }
        case jbc::core::AttrKind::StackMapTable: {
            auto&     smt  = static_cast<jbc::core::StackMapTableAttr&>(*nb);
            ca::usize hit  = 0;
            ca::u32   prev = 0;   // 首帧的前一帧 pc 约定为 -1，故 prev = -1 的等价起点
            for (const auto& fr : smt.frames) {
                const ca::u32 abs = jbc::core::smtAbsPc(prev, fr.offset);
                hit += pcIn(dead, abs) ? 1 : 0;
                prev = abs;
            }
            std::fprintf(stderr, "[pcrefs]   SMT frames=%zu dead=%zu\n", smt.frames.size(), hit);
            break;
        }
        default:
            std::fprintf(stderr, "[pcrefs]   kind=%d\n", static_cast<int>(nb->kind()));
            break;
        }
    }
}

/// 在已 promote 的调用者方法里改写一个站点；失败不改动 DOM。
RwResult rewriteSite(ir::ClassSession& session, ir::CodeUnit& cu, const Site& site)
{
    ir::CodeDom&                   dom   = cu.dom;
    const std::vector<ir::InsnId>& order = dom.order();

    ca::usize callPos = kNoIndex;
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = dom.peek(order[i]);
        if (in != nullptr && in->op == Op::INVOKESTATIC && in->srcPc == site.callSrcPc) {
            callPos = i;
            break;
        }
    }
    if (callPos == kNoIndex) return RwResult::CallNotFound;

    const ir::Insn* call = dom.peek(order[callPos]);
    ir::ClassSession::MemberSem enc;
    if (!session.memberOf(*call, enc)) return RwResult::MemberMismatch;
    if (enc.owner != site.targetOwner || enc.name != site.targetName ||
        enc.desc != site.targetDesc) {
        return RwResult::MemberMismatch;
    }

    std::vector<ir::InsnId>     producers;
    std::vector<jsim::SimValue> args;
    if (!const_args::collectArgs(session, dom, order, callPos, const_args::argSlots(enc.desc),
                                 producers, args)) {
        return RwResult::ArgsMismatch;
    }
    if (args.size() != site.args.size()) return RwResult::ArgsMismatch;
    for (ca::usize i = 0; i < args.size(); ++i) {
        if (!const_args::sameValue(args[i], site.args[i])) return RwResult::ArgsMismatch;
    }

    ir::Insn load;
    if (!rewrite_tools::makeConstLoad(session, site.value, load)) return RwResult::LoadFail;

    // order 是 DOM 的活视图：任何结构变更之后下标都会位移，先把句柄取出来。
    const ir::InsnId callId  = order[callPos];
    const ir::InsnId firstId = producers.front();

    // 区间里除首条以外的指令上不许有标签：有标签说明分支/switch/异常范围从
    // 区间中段进入，擦掉会让目标悬垂。首条不受此限——它原地改写，身份不变。
    for (ca::usize i = 1; i < producers.size(); ++i) {
        if (!dom.labelsAt(producers[i]).empty()) return RwResult::LabelBlocked;
    }
    if (!dom.labelsAt(callId).empty()) return RwResult::LabelBlocked;

    // 首条**原地改写**成常量装载，而不是「前面插一条 + 删掉首条」：
    // 插入路径把 srcPc 强制清成 kNoPc（合成指令没有旧位置，CodeDom 的硬
    // 不变量），而 LNT/LVT/StackMapTable 的重定位全靠旧 pc 命中 pcmap——
    // 删掉首条会让锚在区间起点的行全部失去落点，lower 只能整体失败。
    // 原地改写保住它的 srcPc，锚在它上面的标签也因此不必迁移。
    auto slotR = dom.get(firstId);
    if (slotR.is_err()) return RwResult::InsertFail;
    ir::Insn*      first   = slotR.unwrap();
    const ir::Insn original = *first;
    const ca::u32  keepPc   = first->srcPc;
    *first                  = load;
    first->srcPc            = keepPc;

    std::vector<ir::InsnId> victims(producers.begin() + 1, producers.end());
    victims.push_back(callId);

    if (std::getenv("GRUNT_DEBUG_PCREFS") != nullptr) {
        std::vector<ca::u32> dead;
        for (const ir::InsnId id : victims) {
            const ir::Insn* v = dom.peek(id);
            if (v != nullptr && v->srcPc != jbc::ir::kNoPc) dead.push_back(v->srcPc);
        }
        dumpPcRefs(session, site.callerMethodIndex, dead);
    }

    if (dom.eraseBatch(victims).is_err()) {
        *first = original;   // 回滚：DOM 保持原样
        return RwResult::EraseFail;
    }
    return RwResult::Ok;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

}   // namespace

// ============================================================================
// ==== 对外三段 ==============================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;

    scan_tools::MethodIndex index;
    buildIndex(container, index);

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("scan", entry.uri, [&] {
            auto session = openSession(*entry.bytes);
            if (!session) return;
            ir::ClassSession&     s    = *session;
            const ir::ClassUnit&  unit = s.unit();
            const std::string_view self = s.backing().thisName();

            for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
                if (!ir::hasCode(s, mi)) continue;
                auto promoted = ir::promoteMethod(s, mi);
                if (promoted.is_err()) continue;
                ir::CodeUnit* cu = promoted.unwrap().get();
                if (cu == nullptr) continue;

                const std::string_view         name  = unit.symbols().str(unit.methods[mi].name);
                const std::string_view         desc  = unit.symbols().str(unit.methods[mi].desc);
                const std::vector<ir::InsnId>& order = cu->dom.order();

                for (ca::usize pos = 0; pos < order.size(); ++pos) {
                    const ir::Insn* in = cu->dom.peek(order[pos]);
                    if (in == nullptr || in->op != Op::INVOKESTATIC) continue;

                    ir::ClassSession::MemberSem enc;
                    if (!s.memberOf(*in, enc)) continue;
                    if (enc.kind != ir::ClassSession::MemberSem::Kind::Method) continue;

                    const auto it = index.find(scan_tools::keyOf(enc.owner, enc.name, enc.desc));
                    if (it == index.end()) continue;   // 容器外目标：不构成候选
                    const scan_tools::MethodLoc& loc = it->second;

                    const auto reject = [&](RejectReason r) {
                        result.rejects.push_back(RejectedSite{
                            std::string(self), std::string(name), in->srcPc, r});
                    };

                    if ((loc.access & kAccStatic) == 0) {
                        reject(RejectReason::TargetNotStatic);
                        continue;
                    }
                    if ((loc.access & kAccSynchronized) != 0) {
                        reject(RejectReason::TargetSynchronized);
                        continue;
                    }
                    if (!enc.name.empty() && enc.name.front() == '<') {
                        reject(RejectReason::TargetInitializer);
                        continue;
                    }
                    if (returnChar(enc.desc) == 'V') {
                        reject(RejectReason::TargetVoid);
                        continue;
                    }

                    std::vector<ir::InsnId>     producers;
                    std::vector<jsim::SimValue> args;
                    if (!const_args::collectArgs(s, cu->dom, order, pos,
                                                 const_args::argSlots(enc.desc),
                                                 producers, args)) {
                        reject(RejectReason::ArgsNotConstant);
                        continue;
                    }

                    Site site;
                    site.callerUri         = std::string(entry.uri);
                    site.callerOwner       = std::string(self);
                    site.callerMethod      = std::string(name);
                    site.callerDesc        = std::string(desc);
                    site.callerMethodIndex = mi;
                    site.callSrcPc         = in->srcPc;
                    site.targetOwner       = std::string(enc.owner);
                    site.targetName        = std::string(enc.name);
                    site.targetDesc        = std::string(enc.desc);
                    site.targetMethodIndex = loc.index;
                    site.args              = std::move(args);
                    result.sites.push_back(std::move(site));
                }
            }
        });
    });

    evaluateSites(container, result.sites, result.rejects);
    return result;
}

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    // 按调用类分组：一次会话内改完该类的全部站点，再 lower + 回写。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& s : result.sites) byUri[s.callerUri].push_back(&s);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        scanGuard("rewrite", entry.uri, [&] {
        const auto it = byUri.find(std::string(entry.uri));
        if (it == byUri.end()) return;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::unordered_map<ca::usize, std::vector<const Site*>> byMethod;
        for (const Site* site : it->second) byMethod[site->callerMethodIndex].push_back(site);

        // 探针必须看到未改动的字节，所以整类的自检先于任何 promote/改写。
        std::vector<ca::usize> candidates;
        candidates.reserve(byMethod.size());
        for (const auto& slot : byMethod) candidates.push_back(slot.first);
        const std::unordered_set<ca::usize> safe = proof_tools::roundTripSafe(*entry.bytes, candidates);

        bool changed = false;
        for (auto& [methodIndex, sites] : byMethod) {
            if (safe.find(methodIndex) == safe.end()) {
                dirty.skippedByRoundTrip += sites.size();
                continue;
            }
            auto promoted = ir::promoteMethod(s, methodIndex);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;

            bool methodChanged = false;
            for (const Site* site : sites) {
                const RwResult r = rewriteSite(s, *cu, *site);
                ++dirty.attempted;
                if (r == RwResult::Ok) {
                    ++dirty.applied;
                    methodChanged = true;
                    if (std::getenv("GRUNT_DEBUG_FOLD") != nullptr) {
                        // 逐条落「折成了什么」：用来和 JVM 反射求值对拍。
                        std::fprintf(stderr, "[fold] %s.%s pc=%u -> %s  = ", site->callerOwner.c_str(),
                                     site->callerMethod.c_str(), site->callSrcPc,
                                     site->targetName.c_str());
                        for (const jsim::SimValue& arg : site->args) {
                            switch (arg.type) {
                            case jsim::ValType::I32:
                                std::fprintf(stderr, "%d ", static_cast<ca::i32>(arg.intBits));
                                break;
                            case jsim::ValType::I64:
                                std::fprintf(stderr, "%lld ", static_cast<long long>(arg.intBits));
                                break;
                            case jsim::ValType::Ref:
                                if (arg.ref.kind == jsim::RefPayload::Kind::Null) {
                                    std::fprintf(stderr, "null ");
                                } else {
                                    std::fprintf(stderr, "? ");
                                }
                                break;
                            default: std::fprintf(stderr, "? "); break;
                            }
                        }
                        switch (site->value.type) {
                        case jsim::ValType::I32:
                            std::fprintf(stderr, "=> %d\n",
                                         static_cast<ca::i32>(site->value.intBits));
                            break;
                        case jsim::ValType::I64:
                            std::fprintf(stderr, "=> %lld\n",
                                         static_cast<long long>(site->value.intBits));
                            break;
                        default: std::fprintf(stderr, "=> ?\n"); break;
                        }
                    }
                } else {
                    const ca::usize idx = static_cast<ca::usize>(r);
                    if (dirty.failedByReason.size() <= idx) dirty.failedByReason.resize(idx + 1, 0);
                    ++dirty.failedByReason[idx];
                }
            }
            if (!methodChanged) continue;
            auto lowered = ir::lowerMethod(s, methodIndex);
            if (lowered.is_err()) {
                if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                    std::fprintf(stderr, "[lower-fail] %s method#%zu err=%s\n",
                                 std::string(s.backing().thisName()).c_str(), methodIndex,
                                 ir::to_string(lowered.unwrap_err()));
                }
                return;   // 整类放弃
            }
            changed = true;
        }
        if (!changed) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) {
            if (std::getenv("GRUNT_DEBUG_ERASE") != nullptr) {
                std::fprintf(stderr, "[write-fail] %s err=%s\n",
                             std::string(s.backing().thisName()).c_str(),
                             jbc::core::to_string(encoded.unwrap_err()));
            }
            return;
        }
        pending.push_back(PendingWrite{std::string(entry.uri), std::move(encoded).unwrap()});
        });
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
    }
    return dirty;
}

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    ProofResult result;
    if (countClasses(container) != classesBefore) {
        result.ok = false;
        result.failed.emplace_back("class-count");
    }
    for (const std::string& uri : dirty.uris) {
        bool found = false;
        forEachClass(container, [&](const ClassEntry& entry) {
            if (found || entry.uri != uri) return;
            found = openSession(*entry.bytes) != nullptr;
        });
        if (!found) {
            result.ok = false;
            result.failed.push_back(uri);
        }
    }
    return result;
}

}   // namespace grunt::constant_call
