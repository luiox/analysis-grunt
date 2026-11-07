/// @file        dispatcher_restore.hpp
/// @brief       残留 `dispatcher_restore`：Grunt v3 InvokeDispatcher 调用点
///              反查回填真实 invoke。
///
/// ## 形态（artifact 侧；doc/obf-behaviors/grunt-v3-behaviors.rst §3.1）
///
/// v3 InvokeDispatcher（`grunt3@e41e852`，`redirect/InvokeDispatcher.kt`）把同
/// 返回类型的多个方法调用点合并进一个分发器：按返回类型分组（每组 ≥2 个
/// 调用点、参数含 primitive 的调用点跳过），每类生成 `PRIVATE + STATIC` 分
/// 发器方法、desc `(N × Object; I) ret`（N = 组内最大参数槽位数），体为：
///
///     分发器体（生成面逐字）：
///       Lhead: iload N                        // key 参数槽 = Object 参数个数
///       tableswitch { min: startCase, max: endCase,
///                     cases: { Lc0 .. Lcn },  // 键连续 startCase..endCase
///                     default: Lhead }        // 判据锚点：default 回跳方法头
///       Lci:
///         { aload k; checkcast <实参型> }×    // case 转发：载实参 + CHECKCAST
///         invokestatic|invokevirtual <真目标>  //   后转发真实目标（键 → 目标
///         <ret 型 return>                     //   映射编译期可重建）
///
///     调用点（生成器就地改写原调用指令的 owner/name/desc，opcode 不变——
///     静态源调用点保持 INVOKESTATIC，是唯一可验证形）：
///       <原调用的实参栈形不动>
///       { aconst_null }×addNulls               // 补齐 maxLoadOp 缺参
///       <int key>                              // access key 常量
///       invokestatic owner.<分发器> (N × Object; I) ret
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：键表静态在场——tableswitch 连续键区间 + **default 回
///   跳方法头**（生成器防死循环的习惯形，识别锚点）+ case 体转发序列逐形
///   （载实参 + CHECKCAST + 唯一 invoke + ret 型 return）+ 调用点 key 常量
///   紧邻压栈。键 → case → 真目标映射全为容器自证。
/// - **无 Era 门**：v3-only 形态（grunt2 无 InvokeDispatcher，redirect/ 下
///   只有 FieldScramble/InvokeDynamic/MethodExtractor/MethodScramble/
///   StringEqualsRedirect），判据内无版本敏感面；形态锚自证家族。
/// - **纯常量键表直查，不需要 jsim**：key 是调用点紧邻常量、键区间在
///   tableswitch 静态编码里。key 经算术混淆（NumberBasicEncrypt 打过）时
///   常量缺席 → 站点 fail-closed 保留，按链序由 NumberFold 先折。
///
/// ## 改写（锚保留与槽位面）
///
/// 对齐 proxy_inline 的调用点改指口径：**调用指令原地换**（srcPc 保留，
/// pcmap/调试行落点不变；opcode 换 case 转发的 invoke 形，成员引用 intern
/// 成真目标），跨度内 [补位 ACONST_NULL×k; key] `eraseBatch` 擦除
/// （kRejectIfUsed 拒收即回滚整站放弃）。回填目标的实参就是调用点原栈形
/// ——生成器只插入不删除，跨度擦除后栈形恰好还原原调用。**分发器本体与
/// 键表不在此删**——引用归零后由 DeadCode / DeadMember / 压池统一回收（与
/// string_pool_fold 同口径）。帧链走 mutateMethodWithFrames 整体重算。
///
/// ## 台账
///
/// DirtySet 按回填 opcode 分列计数 + failed/skippedByRoundTrip；ProofResult
/// 在类数门 + 脏类重解析门之外统计 residual（改写后容器里仍「分发器识别
/// 通过 + 调用点全形状命中键表」的站点数——语料单罐门要求 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::dispatcher_restore {

/// @brief SCAN 注入面：判据全为容器自证事实（键表结构 + 调用点形状），无
///        外部旁证；结构占位与三段签名保持同形。
struct ScanInput
{
};

/// @brief 键表一条转发目标（case 体唯一 invoke 的成员引用）。
struct Target
{
    bool        isVirtual = false;   ///< case 转发 opcode（static→INVOKESTATIC）
    std::string owner;               ///< 真目标 owner（slash 内部名）
    std::string name;
    std::string desc;
};

/// @brief 一个可回填调用点。`callSrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     callSrcPc   = 0;   ///< INVOKESTATIC 分发器调用旧布局 pc
    std::string dispName;          ///< 分发器方法名（owner = 本类）
    std::string dispDesc;
    ca::i32     key       = 0;     ///< 调用点 key 常量（键表命中值）
    ca::u32     padCount  = 0;     ///< 擦除的补位 ACONST_NULL 数 = N - 目标消费
                                   ///< （生成面 addNulls；调用点 null 跑更长说明
                                   ///< 原实参尾带 null 字面量——保留不动）
    Target      target;               ///< 键表查出的转发目标
};

/// @brief 拒绝（fail-closed 记账：拒绝原因进 trace，站点不动）。
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
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize dispatchers        = 0;  ///< 识别通过的分发器方法数
    ca::usize dispatcherRejected = 0;  ///< 头锚过但 case 体不合规的分发器数
    ca::usize keyNotConst        = 0;  ///< key 位非常量（含 NumberBasicEncrypt 藏形）
    ca::usize keyNotInTable      = 0;  ///< key 不在键表区间
    ca::usize padMismatch        = 0;  ///< 补位数与目标栈形互证不符
    ca::usize dispatcherBlocked  = 0;  ///< 站点指向不合规分发器（多候选等）
    ca::usize classesSkipped     = 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计（回填 opcode 分列，morpher RewriteStats 口径）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/擦除/intern 拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数

    ca::usize rewrittenStatic  = 0;   ///< 回填 invokestatic 数
    ca::usize rewrittenVirtual = 0;   ///< 回填 invokevirtual 数
};

/// @brief 验收结果。residual = 改写后容器里仍「分发器在场 + 调用点全形状
///        命中键表」的站点数（还原不彻底的残留，语料单罐门要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。分发器识别（头锚 + 键表 + case 体形状）→ 调用点反查
///        定案，产出可回填站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 callSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::dispatcher_restore
