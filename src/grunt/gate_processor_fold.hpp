/// @file        gate_processor_fold.hpp
/// @brief       残留 `gate_processor_fold`：Grunt v3 ControlflowJump 门点
///              processor 调用跨方法静态求值 + 恒真门比较折叠。
///
/// ## 形态（artifact 侧；源锚 `grunt3@e41e852`
///          OpaquePredicateProcessorRegistry.kt / IntExpression.kt）
///
/// v3 ControlflowJump 的 guard / mangled-gate 块在真分支边上插一道「processor
/// 门」：三个 int 常量（left/right/salt）喂一个生成面 processor（ActionDesc
/// 恰为 `(III)I`，与门点同类），返回值与 compareValue 常量比较后条件跳转。
/// 生成面逐字：
///
///     bipush <left>
///     bipush <right>
///     bipush <salt>
///     invokestatic <proc> (III)I      // 门点 processor 调用
///     <compareValue 常量走廊>          // compareValue.emit()：[INT; (INT; OP)*]
///     if_icmpxx  Lreal                // 真分支臂；fallthrough = junk 臂
///
/// compareValue 走生成面 IntConstChain 常量走廊：`INT v0; (INT v1; OP)*`，
/// OP ∈ {imul, iand, ior, ixor}（全交换律 + 结合律，左折即 JVM 语义）；罐 A
/// 实测多数门点的 compareValue 是多段走廊而非单常量。
///
/// 生成器带 `require(comparison.test(result, compareValue.value))`：门点必须
/// 恒走真分支，否则自己的 payload 到不了。所以「proc(left,right,salt) 与
/// compareValue 按分支语义比较 = taken」是**混淆器自证的语义事实**——只要
/// proc 体能在常量实参下静态求值，分支走向就是确定的。
///
/// ReferenceObfuscate（chance=1.0）把多数门点 proc 调用包进 indy，IndyRestore
/// 还原后成为同方法直线转发器（`__grt$indy$` 蹬床形：`iload×3;
/// invokestatic 真体; ireturn`）；真体 = 槽位解码链（`iload n; 常量走廊;
/// ixor; istore n`×3）+ BitAction 算术（含 `Integer.rotateLeft(II)I` 盐旋转
/// 步）+ `ireturn`，全程纯算术。
///
/// ## 为什么 ConstantCall 折不完全（本残留的存在理由）
///
/// ConstantCall 的跨方法求值器（jsim 编排层递归）已把纯算术真体的门点调用
/// 折成常量，但 proc 体里的 `INVOKESTATIC java/lang/Integer.rotateLeft(II)I`
/// 是 JDK 调用：libjsim 核心 / num 包都没有它，求值 EvalBadState 整站拒绝
/// ——罐 A 供给链实测剩余 4282 个 const-fed 门点全部因此存活。本残留自带
/// 同一枚举器并补 `Integer.rotateLeft(II)I` 接管（全序纯函数，位移掩码 5
/// 位，常量实参下值唯一），把这一桶吃掉，再按 PredicateFold 的 TAKEN 形把
/// 恒真比较折叠成 `[ICONST_0, IFEQ→Lreal]`。
///
/// ## 判据（scan 的细找）
///
///   1. `INVOKESTATIC` 描述符恰为 `(III)I`（生成面 ActionDesc 指纹；CFF key
///      processor 是 `(II)I`，不同构不进面）；
///   2. 目标解析在容器内 ∧ static ∧ 有码；
///   3. 逆向三个实参全为紧邻纯常量装载（`const_args` 工具箱，NOP 跳过）；
///   4. 前窗为「compareValue 常量走廊 → IF_ICMPEQ..IF_ICMPLE」（NOP 跳过）：
///      走廊 = `[INT; (INT; IMUL|IAND|IOR|IXOR)*]` 栈机折叠恰剩 1 个 i32——
///      其它任何内容整站拒绝，其中停点引用 `ThreadLocalRandom` 的（TLR 窗
///      口变体，运行期随机）单独入 `tlrWindow` 桶明确排除；
///   5. 类初始化门（constant_call 同源）：目标类 `<clinit>` 非中性时只留同
///      类调用（JVMS 5.5：本类方法执行时类已在初始化中，删调用不移时机；
///      门点 proc 与门点同类是生成面事实）；
///   6. 跨方法求值（jsim 编排层，过程内递归进转发器/真体）：返回 known i32
///      ∧ 按分支语义裁决 taken（生成面恒真保证；判非 taken = 求值与生成面
///      脱钩，fail-closed 拒绝）。
///
/// ## 改写（rewrite；PredicateFold TAKEN 形同款）
///
/// 锚（left 常量装载）原地改 `ICONST_0`（保 srcPc），分支本体原地换
/// `IFEQ → 原目标`，其余操作数（right/salt/invokestatic/compareValue）单批
/// `eraseBatch`（kRejectIfUsed，拒收即整站回滚）。不插无条件 GOTO（HotSpot
/// 线性走查要求无条件跳转后段首带帧）；条件跳转让延续段保持 fallthrough
/// 可达，帧面零改动。操作数区中段或分支本体带标签（合并点）整站放弃。
/// junk 臂原地保留成不可达码，交下游 DeadCode 清理。
///
/// ## 边界（fail-closed 面）
///
/// - **RandomBound / TLR（运行期随机）**：窗口变体（TLR.current 取数进前窗）
///   在前窗停点识别入 `tlrWindow` 桶；proc 体引用 `ThreadLocalRandom` 的
///   （RandomBound BitAction 变体）不在容器内、无 mock，求值 EvalBadState
///   拒绝——运行期随机面永久排除，与本批验收口径一致。
/// - **material-key 桶**：RuntimeMaterial 类里的门点蹬床体内 indy 走材料解
///   密器（key = decryptKey ^ materialKey()，运行期材料），IndyRestore
///   keyMissing 未还原，蹬床体内仍是 INVOKEDYNAMIC，求值 EvalBadState 拒绝
///   ——解锁属 material_key_restore / indy_restore 本体（禁碰面），链序实验
///   已证罐 A 上不可达。
/// - 判非恒真 / 求值未知值 / 步数熔断 / 形状走样逐桶记账，字节不动。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "jsim/value.hpp"

namespace grunt::gate_processor_fold {

/// @brief 一个可折叠门点。`callSrcPc` / `branchSrcPc` 跨会话稳定，rewrite 靠
///        它们重定位后复核再改。
struct Site
{
    std::string   uri;
    ca::usize     methodIndex = 0;
    std::string   callerOwner;
    std::string   callerMethod;
    ca::u32       callSrcPc   = 0;                       ///< INVOKESTATIC (III)I 锚
    ca::u32       branchSrcPc = 0;                       ///< IF_ICMPxx
    jbc::core::Op branchOp    = jbc::core::Op::NOP;

    std::string   targetOwner;
    std::string   targetName;
    std::string   targetDesc;
    ca::usize     targetMethodIndex = 0;

    std::vector<jsim::SimValue> args;      ///< 3 个 i32 实参（left/right/salt）
    ca::i32       compareValue = 0;        ///< 前窗 compareValue 常量
    ca::i32       result       = 0;        ///< proc 求值结果（scan 求值段填）
};

/// @brief 拒绝原因（fail-closed 记账：原因进 trace，站点字节不动）。
enum class RejectReason : ca::u8
{
    TargetOutside,          ///< (III)I 调用目标不在容器内
    TargetNotStatic,        ///< 目标非 static
    TargetSynchronized,     ///< 目标 synchronized
    TargetNoCode,           ///< 目标无码 / 目标类会话开不了
    ArgsNotConstant,        ///< 逆向三实参不是紧邻纯常量装载
    ForwardShape,           ///< 前窗不是 compareValue 走廊 → IF_ICMP* 逐字形
    TlrWindow,              ///< 前窗停点引用 ThreadLocalRandom（RandomBound
                            ///  窗口变体，运行期随机，明确排除）
    ClassInitPresent,       ///< 目标类 clinit 非中性且跨类（初始化时机门）
    EvalBadState,           ///< 跨方法求值 Bail（TLR/material/indy/堆访问面）
    EvalStepLimit,          ///< 求值步数熔断
    ValueKindUnsupported,   ///< 求值结果非 known i32
    EvalNotTrue,            ///< 裁决非 taken（生成面恒真保证被违反，fail-closed）
};

/// @brief 拒绝记录（trace 逐条面）。
struct Reject
{
    std::string callerOwner;
    std::string callerMethod;
    ca::u32     callSrcPc = 0;
    RejectReason reason = RejectReason::TargetOutside;
};

/// @brief 三段之间唯一的数据流。桶计数与 rejects 逐条同源（每拒一条两边各
///        记一次），trace 行吃计数、trace 门吃逐条。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize totalShape         = 0;  ///< 形状命中总数（进后续各桶 + sites）
    ca::usize targetOutside      = 0;
    ca::usize targetNotStatic    = 0;
    ca::usize targetSynchronized = 0;
    ca::usize targetNoCode       = 0;
    ca::usize argsNotConstant    = 0;
    ca::usize forwardShape       = 0;
    ca::usize tlrWindow          = 0;  ///< TLR 窗口变体（运行期随机，排除面）
    ca::usize classInitPresent   = 0;
    ca::usize evalBadState       = 0;
    ca::usize evalStepLimit      = 0;
    ca::usize valueKindUnsupported = 0;
    ca::usize evalNotTrue        = 0;
    ca::usize classesSkipped     = 0;  ///< 会话开不了的类
};

/// @brief 改写失败原因（trace 直方图）。
enum class RwResult : ca::u8
{
    Ok = 0,
    BranchNotFound,   ///< srcPc+op 重定位不到（同方法前站改写位移）
    RegionMismatch,   ///< 操作数区复核不符（成员/常量值被改动）
    VerdictChanged,   ///< 复核裁决与 scan 不一致（纯函数下不应发生，防御面）
    LabelBlocked,     ///< 操作数区中段或分支本体带标签（合并点）
    EraseFail,        ///< eraseBatch 拒收（异常表/帧锚占用），整站回滚
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;  ///< 折叠成功的门点数
    ca::usize attempted          = 0;  ///< 进入 rewrite 的门点数
    ca::usize failed             = 0;  ///< 复核/标签门/erase 拦下的门点数
    ca::usize skippedByRoundTrip = 0;  ///< 原样往返不了的方法上的门点数
    std::vector<ca::usize> failedByReason;  ///< 按 RwResult 直方图（Ok 位恒 0）
};

/// @brief 验收结果。residual = 脏类集上重跑全判据（形状 + 求值 + 恒真裁决）
///        仍成站点的数量——改写 fail-closed 放弃的站点如实入账，求值失败面
///        （TLR/material，从未进改写面）不计入。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。形状钉 + 目标解析 + 前窗钉 + 跨方法求值 + 恒真裁决。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 srcPc 重定位复核后再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 复扫。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::gate_processor_fold
