/// @file        cff_unflatten.hpp
/// @brief       残留 `cff_unflatten`：Grunt v3 ControlflowFlattening 反摊平。
///
/// ## 形态（artifact 侧；doc/obf-behaviors/grunt-v3-behaviors.md §2.2）
///
/// v3 ControlflowFlattening（`grunt3@e41e852`，FlowControlFlowFlattener 逐行，
/// jbc jasm 样例 boar-main.jar `net/spartanb312/obf/AD.a` 互证）把 maximal
/// region 块序打乱收缩：每方法一个 fresh state 槽 s（`method.locals.nextSlot`），
/// 每个出口一个 dispatcher 岛 `[ILOAD s; TABLESWITCH]`（键窗 [lo, lo+n) 连续
/// 乱序，default 落岛首真块 = alias face）；每条入边一个桥块
/// `[pushInt key; 变换链; ISTORE s; GOTO 岛头]`（inline span：
/// AddConst/SubConst/XorConst/MulConst/DivConst/Math.max 闭链）；Mixed 模式
/// 50% 桥走 processor 形 `[INT; ISTORE s; ILOAD s; INT salt; INVOKESTATIC
/// proc(II)I; ISTORE s]`（boar 侧 proc 即 `__grt$indy$` 蹦床）；scramble 假桥
/// 键落窗外 goto 同一 default；方法头入口桥、handler 桥同形。反摊平 = 常量
/// 供给桥求值回查键表 → goto 直连真实块 + 擦岛 + 擦桥 + 入口块归位 +
/// handler 桥重绑。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：state 槽全方法读写 confined（ILOAD 只在岛头或 processor
///   夹心内、ISTORE 只在 span 尾、无第三类操作）；岛形 `[ILOAD s; TABLESWITCH]`
///   键窗静态编码；桥块常量供给闭链静态可解；default ∈ case 目标（alias face）。
/// - **无 Era 门**：v3-only 形态（v2 Controlflow 是 junk 分支面，无 state 槽
///   摊平），岛形自证家族。SSA 变体不同构（LookupSwitch + per-case stub +
///   passArgs 编组）→ v1 挂账，不在此面。
/// - **processor 桥 fail-closed**：键在 indy 蹦床后（boar：`__grt$indy$` 桥 +
///   Rename），静态不可解；整岛保留，解锁路径记 doc（缺口 #4 联动面）。
///
/// ## 改写（块序重建 + alias-face 剪枝）
///
/// 语义安全核心：桥对 state 的供给是常量封闭求值，求值出表 ≡ switch default
/// （表只覆盖 [lo, lo+n)，default 是唯一出表去向）——即使岛被误判，goto 直连
/// default 目标也与原路径逐位等价。可整擦桥块仅限纯 span+goto 形；条件形与
/// 带 junk 前缀的桥一律保留块体只重指（junk 可达且不可证伪，保留形由 goto
/// 重指完成同一剪枝）。usersOf 只看字段引用，位置边单独守卫：受害者的布局
/// 前驱若有位置落入（非终结顺序落入/条件分支落空臂），只有「本岛已归类条件
/// 用户紧邻岛头」可钉（补 GOTO 指其 case 目标），其余整岛拒。执行序：handler
/// 桥重绑（bindLabel）→ 桥前驱 replaceAllUses 直连真实块（alias-face 剪枝 =
/// scramble/leftover goto 全部改指 default 目标）→ 臂字段重指 + positional
/// 落空臂钉 → 入口块归位（擦入口桥 + fallthrough 补 GOTO + moveRange 头插；
/// 入口块在异常保护区内或带 junk 前缀时退蹦床：不移动、goto 改指，前缀保持
/// 执行）→ 受害者单批 eraseBatch（kRejectIfUsed 拒收即整方法回滚）。帧链走
/// mutateMethodWithFrames 整体重算。键处理器藏形（NumberFold 先解锁）不在本批。
///
/// ## 台账
///
/// DirtySet 按执行动词分列（岛数/入口归位/蹦床回退/scramble 剪枝/failed）；
/// ProofResult 在类数门 + 脏类重解析门之外统计 residual（改写后容器里仍
/// 「岛全形状可反」的岛数——语料单罐门要求 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::cff_unflatten {

/// @brief SCAN 注入面：判据全为容器自证事实（state 槽 confinement + 岛形 +
///        桥块形状），无外部旁证；结构占位与三段签名保持同形。
struct ScanInput
{
};

/// @brief 一个可反摊平岛。`headSrcPc` 跨会话稳定，rewrite 靠它重定位复推。
struct IslandPlan
{
    std::string uri;                 ///< 类资源 uri
    ca::usize   methodIndex = 0;     ///< 方法序号
    ca::u32     headSrcPc   = 0;     ///< 岛头（ILOAD s）源码 pc——重定位锚
};

/// @brief 拒绝（fail-closed 记账：拒绝原因进 trace，整岛不动）。
struct Reject
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorSrcPc = 0;
    std::string why;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<IslandPlan> islands;
    std::vector<Reject>     rejects;

    ca::usize dispatchers     = 0;  ///< 识别出的 dispatcher 岛总数（含 fail-closed）
    ca::usize islandsSolvable = 0;  ///< 进入重写计划的岛数
    ca::usize keyProcessor    = 0;  ///< processor 形 span 静态不可解（Mixed 桥/入口桥）
    ca::usize spanUnsolvable  = 0;  ///< span 变换链出静态词汇表（/0、非常量供给）
    ca::usize foreignRef      = 0;  ///< 岛外引用（非常量供给桥、junk 门、杂引用）
    ca::usize statePolluted   = 0;  ///< state 槽被岛外读写污染（IINC/越界读写）
    ca::usize shapeMismatch   = 0;  ///< 表形不齐（无 alias face/出表 case 目标等）
    ca::usize blocksShared    = 0;  ///< 岛间共享块（case 目标跨岛）
    ca::usize excBlocked      = 0;  ///< 异常表边交叠（handler/start 锚岛头或被擦块）
    ca::usize classesSkipped  = 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 执行级统计（动词分列）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize islands        = 0;  ///< 执行反摊平的岛数
    ca::usize classes        = 0;  ///< 实际改写的类数
    ca::usize failed         = 0;  ///< 重定位/复推/擦除/帧链拦下的岛数
    ca::usize skippedByRoundTrip = 0;  ///< 原样往返不了的方法上的岛数
    ca::usize entryRestored  = 0;  ///< 入口桥擦除 + 入口块归位（方法数）
    ca::usize entryTrampoline = 0; ///< 入口块异常保护区内 → 蹦床回退（方法数）
    ca::usize scrambles      = 0;  ///< scramble 假桥剪枝数（goto→default 直连）
};

/// @brief 验收结果。residual = 改写后容器里仍「岛全形状可反」的岛数（语料
///        单罐门要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。state 槽 confinement + 岛识别 + 桥块归属定案，产出可
///        反岛计划。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 headSrcPc 重定位复推再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::cff_unflatten
