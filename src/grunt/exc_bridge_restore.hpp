/// @file        exc_bridge_restore.hpp
/// @brief       残留 `exc_bridge_restore`：Grunt v3 ControlflowJump 异常桥
///              还原——合成 throw/catch 蹦床边摘除，原边直连真实目标。
///
/// ## 形态（artifact 侧；源锚 `grunt3@e41e852` FlowExceptionBridgeInserter.kt）
///
/// v3 ControlflowJump（`exceptionBridgeChance > 0`）把一条 Real 边改道进合
/// 成 trap 块、再经异常表跳回原目标（skidfuscator 同源面）。导出字节码形：
///
///     原边入口（GOTO / IFxx 分支臂 / tableswitch case）:
///       ... goto Ltrap                    // 原边被改指（edge.to = trap）
///     Ltrap:                              // trap 块（生成面整块追加布局尾）
///       new java/lang/RuntimeException
///       dup
///       invokespecial java/lang/RuntimeException.<init> ()V
///       athrow                            // 异常表项 [Ltrap, Lafter) → Lh
///     Lh:                                 // bridge handler 块
///       pop                               // 接住 RuntimeException 丢弃
///       goto Lx                           // Synthetic goto → 原目标
///
/// 异常表项 catch_type = java/lang/RuntimeException，保护区恰为 trap 块四条
/// 指令（[NEW, ATHROW]，end 锚在 ATHROW 后边界），handler 恰为 POP + GOTO。
/// 语义恒等：任何到达 trap 头的执行路径（jump/位置落入）都无条件 ATHROW →
/// POP → GOTO Lx，还原 = 全部前驱直连 Lx + trap/handler 摘除 + 表项删除。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：形状钉（trap 四条逐字 + handler POP;GOTO 两块互证）∧
///   可达性互证（真实 try 区不可能同构：保护区恰为生成面 trap 形、handler
///   除异常派发外无入口——直接跳入 handler 会 POP 空栈，校验器拒绝）。
///   catch_type 全为 RuntimeException、handler 全为 Synthetic goto，家族锚
///   自证，无 Era 门（v2 Controlflow 无异常桥面）。
/// - **反误伤门**：真实 `catch (RuntimeException)` 的保护区是用户代码段
///   （非四条 trap 形）或 handler 带异常槽存储（javac 形 ASTORE 非 POP），
///   形状钉天然拒绝；另有覆盖门（其他表项保护区覆盖 trap 头 → 派发歧义拒）
///   与 handler 碰撞门（其他表项 handler 锚在受害者上 → 拒）。
///
/// ## 改写（erase 系口径）
///
/// 1. `replaceAllUses(trap 头, X)`——全部 jump/switch 字段直连原目标；
/// 2. 位置落入守卫——trap 头的布局前驱可落入（生成面 goto 省略形，pred =
///    state span 尾 istore/iinc）时补 GOTO 钉住（cff_unflatten fallPins 同
///    形：钉点 = 原「落入 trap 抛出经桥回 X」的同址语义）；
/// 3. 异常表——本桥表项删除（start/end/handler 三标签销毁）；存活表项锚在
///    受害者上的 start/end 前移吸附（dead_code 同形），handler 锚被占整方法
///    拒（fail-closed）；
/// 4. 受害者（trap 4 条 + handler 2 条）单批 `eraseBatch`（kRejectIfUsed 拒
///    收即整方法回滚）；帧链走 mutateMethodWithFrames 整体重算。
///
/// 与 CffUnflatten 的链序：罐 A config 里 ControlflowFlattening 先于
/// ControlflowJump（桥插在已摊平代码上），本残留排在 CffUnflatten **之前**
/// ——桥摘除后 CFF 的 state span goto 恢复整块形，岛识别更干净（实测 X 落
/// 岛头的桥 5657 个，trap 头无一在岛头，互不阻塞）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::exc_bridge_restore {

/// @brief 一个可还原异常桥。`posTrapHead` 跨会话稳定（DOM 序由 backing 字节
///        唯一决定），rewrite 靠它重定位后复核再改。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::usize   excIndex     = 0;   ///< 桥异常表项下标
    ca::usize   posTrapHead  = 0;   ///< trap 头（NEW）在 DOM 序中的位置
    ca::usize   users        = 0;   ///< 桥入口引用数（jump/switch 字段，trace 用）
    bool        needsPin     = false;  ///< 布局前驱可位置落入 trap 头（补 GOTO 钉）
};

/// @brief 拒绝（fail-closed 记账：拒绝原因进 trace，桥原样保留）。
struct Reject
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorPos   = 0;   ///< trap 头位置（或 0，定位辅助）
    std::string why;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize bridgeClasses = 0;  ///< 含桥站点的方法所在类数（scan 口径）
    ca::usize shapeMiss     = 0;  ///< RTE 表项但形状钉不中（含真实 try 区，反误伤面）
    ca::usize covered       = 0;  ///< 其他表项保护区覆盖 trap 头（派发歧义）
    ca::usize handlerCollide= 0;  ///< 其他表项 handler 锚在本桥受害者上
    ca::usize xInvalid      = 0;  ///< 桥目标不可解析 / 落在受害者 / handler goto 或非头受害者被引用
    ca::usize rebindBlocked = 0;  ///< 存活表项锚在受害者上且无法吸附（handler 被占 / start 吸附越界）——整方法放弃
    ca::usize classesSkipped= 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize methods  = 0;   ///< 实际动过的方法数
    ca::usize bridges  = 0;   ///< 还原的桥数（表项删除数）
    ca::usize pins     = 0;   ///< 补的位置落入钉数
    ca::usize insns    = 0;   ///< 擦除的指令数（trap 4 + handler 2 每桥）
    ca::usize failed   = 0;   ///< 定位/复核/erase 拦下的站点数
};

/// @brief 验收结果。residual = 改写后容器里仍能通过形状钉 + 互证门的桥站
///        点数（语料单罐门要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。异常表逐项形状钉 + 互证门 → 可还原桥站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。replaceAllUses 直连 + 位置钉 + 表项删除 + 桥块擦除。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::exc_bridge_restore
