/// @file        key_proc_eval.hpp
/// @brief       残留 `key_proc_eval`：Grunt v3 ControlflowFlattening key
///              processor 夹心跨方法静态求值（cff_unflatten processor 桥的
///              fail-closed 解锁面，obf-behaviors 缺口 #4 联动）。
///
/// ## 形态（artifact 侧；源锚 `grunt3@e41e852`
///          FlowControlFlowFlattener.kt emitProcessorStateProgram）
///
/// v3 ControlflowFlattening Mixed 模式 50% 的桥走 processor 形（与 inline
/// span 同一 state 槽协议），生成面逐字：
///
///     [INT(inputKey); ISTORE(slot); ILOAD(slot); INT(salt);
///      INVOKESTATIC proc (II)I; ISTORE(slot)]
///
/// `proc` 是 CFF key processor（ActionDesc 恰为 ``(II)I``，与 gate processor
/// 的 ``(III)I`` 不同构）。键在 proc 调用后，cff_unflatten 的 span 求值器
/// 只认 inline 闭链，processor 形整岛 fail-closed（``SpanKind::Processor``）
/// ——岛留下，CFR 0.152 SwitchReplacer 对该形抛 ``ConfusedCFRException``，
/// 方法整体落 ``throw new IllegalStateException("Decompilation failed")`` 兜
/// 底（jsoup v3_full 实测 251 文件 295 方法，其中 246 个是 dispatcher 残岛
/// 直接触发）。本残留补的是 obf-behaviors 缺口 #4 明记的解锁路径：跨方法静
/// 态求值 proc(inputKey, salt)，夹心改写成 ``[INT(result); ISTORE(slot)]``
/// ——桥退化成纯 inline span，cff_unflatten 既有机制（converge 下一轮或同
/// 轮靠链序）自然折叠整岛。
///
/// ## 为什么 proc 体静态可求值（纯性链）
///
/// ReferenceObfuscate（chance=1.0）把多数 key processor 调用包进 indy，
/// IndyRestore 还原后 proc 体是直线纯算术（jsoup v3_full
/// ``DataUtil$KeyProcessor$*`` 逐类实证：``iload_0; iload_1; iadd; iload_1;
/// iadd; iload_1; isub; ldc; iadd; ireturn`` 形）；残存的 indy 转发体经
/// IndyRestore 轮次解锁后同形。求值走 jsim 编排层（过程内递归进转发体/真
/// 体）+ ``Integer.rotateLeft(II)I`` 接管（IntExpression 盐旋转步，gate
/// processor 同款），任何堆访问/字符串/indy 面在求值处 fail-closed。
///
/// ## 判据（scan 的细找）
///
///   1. `INVOKESTATIC` 描述符恰为 ``(II)I``（生成面 ActionDesc 指纹；gate
///      processor 是 ``(III)I``，不同构不进面）；
///   2. 夹心逐字紧邻（DOM 序，NOP 不跳——squeeze 遗留 NOP 出现即不生成面
///      形，静默非候选）：`prev4 = int 常量装载（inputKey）；prev3 =
///      ISTORE(slot)；prev2 = ILOAD(slot)；prev1 = int 常量装载（salt）；
///      next1 = ISTORE(slot)`，三处 slot 全等；
///   3. 目标解析在容器内 ∧ static ∧ 非 synchronized（scan_toolbox
///      MethodIndex，gate_processor_fold 同源）；
///   4. 类初始化检查（constant_call / gate_processor_fold 同源）：目标类
///      `<clinit>` 非中性时只留同类调用（JVMS 5.5 本类方法开始执行时类已
///      在初始化中/已完成，改写调用不移时机）；
///   5. 跨方法求值（jsim 编排层 + rotateLeft 接管）：返回 known i32。
///
/// ## 改写（rewrite）
///
/// 按 `callSrcPc` 重定位 INVOKESTATIC，操作数区复核（夹心逐字 + 成员身份 +
/// input/salt/result 与 scan 全等）后：锚（inputKey 常量装载）原地改成
/// result 的**规范常量装载**（ICONST/BIPUSH/SIPUSH 小值直装，大值 LDC_W 内
/// 签，保 srcPc），中段 4 条（ISTORE/ILOAD/salt/INVOKESTATIC）单批
/// `eraseBatch`（kRejectIfUsed，异常表锚占用拒收即整站回滚）；尾 ISTORE
/// 原样保留——夹心后桥正是 ``[INT(result); ISTORE(slot)]`` 纯 span。不插任
/// 何控制流，帧面零改动（前窗无分支）。操作数区中段带标签（合并点）整站放
/// 弃（LabelBlocked）。
///
/// ## 边界（fail-closed 面）
///
/// - 求值未知值 / 步数熔断 / Bail（TLR、material-key、indy 未解锁、堆访问
///   ）逐桶计数，字节不动；
/// - 方法值语义不变：result 是 proc(常量, 常量) 的纯函数值，规范常量装载
///   与原夹心在该桥的执行序里逐位等价；proc 方法本体不动（DeadMember 供给
///   链断后按死成员轮次带走）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

namespace grunt::key_proc_eval {

/// @brief 一个可求值夹心。`callSrcPc` 跨会话稳定，rewrite 靠它重定位后复核
///        再改；`stateSlot` / `inputKey` / `salt` / `result` 是夹心与求值的
///        全部事实。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    std::string callerOwner;
    std::string callerMethod;
    ca::u32     callSrcPc = 0;      ///< INVOKESTATIC (II)I 锚
    ca::u32     anchorSrcPc = 0;    ///< inputKey 常量装载（改写锚）
    ca::u32     stateSlot   = 0;    ///< state 槽（三处全等）

    std::string targetOwner;
    std::string targetName;
    ca::i32     inputKey = 0;
    ca::i32     salt     = 0;
    ca::i32     result   = 0;       ///< proc(inputKey, salt) 静态求值结果
};

/// @brief 拒绝原因（fail-closed 计数：原因进 trace，站点字节不动）。
enum class RejectReason : ca::u8
{
    TargetOutside,        ///< (II)I 调用目标不在容器内
    TargetNotStatic,      ///< 目标非 static
    TargetSynchronized,   ///< 目标 synchronized
    ClassInitPresent,     ///< 目标类 clinit 非中性且跨类（初始化时机检查）
    EvalBadState,         ///< 跨方法求值 Bail（堆访问/indy 未解锁/TLR 面）
    EvalStepLimit,        ///< 求值步数熔断
    ValueKindUnsupported, ///< 求值结果非 known i32
};

/// @brief 拒绝记录（trace 逐条面）。
struct Reject
{
    std::string  callerOwner;
    std::string  callerMethod;
    ca::u32      callSrcPc = 0;
    RejectReason reason = RejectReason::TargetOutside;
};

/// @brief 三段之间唯一的数据流。桶计数与 rejects 逐条同源（每拒一条两边各
///        记一次），trace 行吃计数、trace 检查吃逐条。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize totalShape         = 0;  ///< 夹心形状命中总数（进后续各桶 + sites）
    ca::usize targetOutside      = 0;
    ca::usize targetNotStatic    = 0;
    ca::usize targetSynchronized = 0;
    ca::usize classInitPresent   = 0;
    ca::usize evalBadState       = 0;
    ca::usize evalStepLimit      = 0;
    ca::usize valueKindUnsupported = 0;
    ca::usize classesSkipped     = 0;  ///< 会话开不了的类
};

/// @brief 改写失败原因（trace 直方图）。
enum class RwResult : ca::u8
{
    Ok = 0,
    ProcNotFound,     ///< srcPc 重定位不到（同方法前站改写位移）
    RegionMismatch,   ///< 夹心复核不符（成员/常量值/求值结果被改动）
    AnchorIntern,     ///< result 大值且 LDC_W 内签失败（CP 下标越 u2）
    LabelBlocked,     ///< 操作数区中段带标签（合并点）
    EraseFail,        ///< eraseBatch 拒收（异常表/帧锚占用），整站回滚
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;  ///< 夹心改写成功数
    ca::usize attempted          = 0;  ///< 进入 rewrite 的夹心数
    ca::usize failed             = 0;  ///< 复核/标签/erase 拦下的夹心数
    ca::usize skippedByRoundTrip = 0;  ///< 原样往返不了的方法上的夹心数
    std::vector<ca::usize> failedByReason;  ///< 按 RwResult 直方图（Ok 位恒 0）
};

/// @brief 验收结果。residual = 脏类集上重跑全判据仍成站点的数量，改写
///        fail-closed 放弃的站点如实入账，求值失败面（从未进改写面）不计。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。夹心钉 + 目标解析 + 类初始化检查 + 跨方法求值。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 srcPc 重定位复核后再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 复扫。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::key_proc_eval
