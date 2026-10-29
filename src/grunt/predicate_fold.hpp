/// @file        predicate_fold.hpp
/// @brief       残留 `predicate_fold`：操作数全为常量的分支/switch 静态裁决。
///
/// ## 形态（artifact 侧）
///
/// 混淆器往直线代码里插「常量 vs 常量」的假分支：真分支是原逻辑，假分支是
/// 诱饵死块（常调 JDK 方法再 throw，凑栈形状）。例如
///
///     ldc 138795133 ; ldc 21992595 ; if_icmple  <dead_block>
///
/// 操作数两侧都是纯常量装载时，分支走向是**静态事实**（S1）：混淆器必须
/// 让它恒走真分支，否则自己的payload 都到不了。
///
/// TABLESWITCH/LOOKUPSWITCH 的键为常量时同理：目标分支唯一确定。
///
/// ## 判据（scan 的细找）
///
///   1. 分支指令是 IFEQ..IFLE（1 个操作数）、IF_ICMP*（2 个操作数）或
///      TABLESWITCH/LOOKUPSWITCH（1 个键）；
///   2. 操作数由**紧邻的纯常量装载**产生且槽数严格吻合（`const_args` 工具
///      箱；NOP 落点跳过，squeeze 先行后被压住的操作数区重新可见）；
///   3. 操作数全部是 I32（引用/长整型常量不在本残留裁决范围）。
///
/// ## 改写（rewrite）
///
/// taken → 区域首条操作数原地改 GOTO_W（保 srcPc）直奔目标；not-taken →
/// 首条置 NOP；其余操作数与分支本体在非 indy 类擦除、indy 类 NOP 化（帧锚
/// 保真）。区域中段或分支本体带标签即整站放弃——那是合并点，外部到达路径
/// 的栈形状与区域产出耦合，不能动；区域首条的标签安全（外部到达起点即拿
/// 到折叠结果）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

namespace grunt::predicate_fold {

/// @brief 一个可裁决站点。`branchSrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string        uri;
    ca::usize          methodIndex = 0;
    ca::u32            branchSrcPc = 0;
    jbc::core::Op      branchOp    = jbc::core::Op::NOP;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;   ///< 会话开不了的类（fail-closed 跳过）
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites      = 0;   ///< 改写成功的站点数
    ca::usize attempted  = 0;   ///< 进入 rewrite 的站点数
    ca::usize failed     = 0;   ///< 复核/标签门/roundtrip 拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出可裁决站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 branchSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::predicate_fold
