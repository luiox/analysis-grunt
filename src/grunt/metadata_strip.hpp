/// @file        metadata_strip.hpp
/// @brief       残留 `metadata_strip`：Kasumi 元数据与水印残留清理三段
///              （`@Metadata` 注解三面摘除 + 水印字段删除 + 反 LLM 资源删除）。
///
/// ## 三面（对位树版 kasumi_metadata_cleanup_pass）
///
/// Kasumi 每类挂一个 `@Ldev/kasumi/internal/Metadata;`（mv/d1/d2）注解、
/// 留水印串与 `META-INF/Kasumi/cybersecurity/analysis-*.md` 反 LLM 注入资
/// 源。树版口径（逐字对齐）：
///
/// - **注解面**：类/字段/方法三张挂载面的 Runtime{Visible,Invisible}Annotations
///   里摘 `@Metadata` 条目（其余注解保留）；
/// - **字段面**：带 `@Metadata` 的字段与 ConstantValue 为水印串的字段整字段
///   删除（水印串判据：含 `PROTECTED BY Kasumi` / 含 `Gruntpocalypse` /
///   前缀 `kasumi_`，树版 isKasumiWatermarkString 三条全收）；
/// - **资源面**：URI 前缀 `META-INF/Kasumi/` + `.md` 后缀 + 内容探针
///   （anti-llm / developer-message / jailbreak / prompt injection /
///   PROTECTED BY Kasumi 五条任一子串在场）三证齐才删——树版
///   isKasumiAntiLlmResource 的三面判定，形状命中而探针缺席的资源**保留**
///   （树版返回 false 同口径），本仓以 `resourceCandidates` 如实记账。
///
/// 方法体不动：bootstrap/解密器在 indy 全还原后由下游 DeadMember/DeadCode
/// 清理，本残留不碰代码（树版同口径）。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 形状钉面**：注解描述符全等（`Ldev/kasumi/internal/Metadata;`），
///   资源 URI 形状（前缀 + `.md`），水印串形状（子串/前缀）。
/// - **S3 Era 门**：三面判据全是 Kasumi 当前版本的实现选择（注解描述符、
///   水印文本、资源前缀、探针表都换代可换），整个改写面落在
///   `Era::Kasumi` 分支——Era 未判即关面（`eraBlocked`，fail-closed）。
/// - **S1 互证**：资源删除要内容探针坐实（URI 形状命中 + 注入标记字节在
///   场才算注入内容）；水印字段删除要 ConstantValue 内容命中。注解摘除
///   走注解族深度预检（`attrSafeToDecode`，mjbase #158 守卫）：解不开的
///   注解槽 fail-closed 不摘（`annoBlocked` 记账），不留半摘态。
///
/// ## 资源动词（本仓首个非类条目写面）
///
/// 现役画布 `forEachClass`/`replaceClass` 只走类资源；资源删除走
/// rewrite_toolbox 的 `removeResource`（写侧原语），对账走 proof_toolbox 的
/// `countResources`/`resourcesShrunk`（读侧门）：资源数不增（只减）+ 删除
/// 清单逐条不在场 + 类数不变 + 脏类可重解析。删除清单逐条进 DirtySet
/// 台账（Stats 面），虚报删减即 proof FAILED。
///
/// ## 台账
///
/// DirtySet 分面计数（annos/fields/removedResources）+ annoBlocked（复核
/// 预检拒绝）+ failed（编码失败未归账）；ProofResult 在双门（类数 + 资源
/// 对账）之外统计三面 residual（语料单罐门要求全 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/era.hpp"

namespace grunt::metadata_strip {

/// @brief SCAN 注入面：Era 判定（adapter 调 probeEra 一次注入）。
struct ScanInput
{
    knowledge::Era era = knowledge::Era::Unknown;
};

/// @brief 一个待清理类：`@Metadata` 条目数（三面合计）+ 水印字段数。
struct Site
{
    std::string uri;
    ca::usize   annos  = 0;   ///< `@Metadata` 条目数（类/字段/方法三面合计）
    ca::usize   fields = 0;   ///< 水印字段数（带注解字段 + ConstantValue 水印字段）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>        sites;       ///< 待清理类（逐类条目数台账）
    std::vector<std::string> resources;   ///< 待删资源（URI 形状 + 内容探针双证齐）

    ca::usize totalAnnos         = 0;  ///< 三面 `@Metadata` 总条目数
    ca::usize totalFields        = 0;  ///< 水印字段总数
    ca::usize resourceCandidates = 0;  ///< 资源前缀命中但删除判据不全（树版保留，如实盘点）
    ca::usize annoBlocked        = 0;  ///< 深度预检拦下的注解槽数（fail-closed 不摘）
    bool      eraBlocked         = false;  ///< Era 未判：整个改写面关闭（fail-closed）
    ca::usize classesSkipped     = 0;  ///< 会话开不了的类
};

/// @brief 被清理的类 + 资源删除台账（Stats 面：删除清单逐条可审计）。
struct DirtySet
{
    std::vector<std::string> uris;             ///< 被清理的类
    std::vector<std::string> removedResources; ///< 被删除的资源清单（逐条 URI）

    ca::usize classes = 0;             ///< 清理的类数
    ca::usize annos   = 0;             ///< 摘除的 `@Metadata` 条目数（含随水印字段删除的）
    ca::usize fields  = 0;             ///< 删除的水印字段数
    ca::usize failed  = 0;             ///< 编码失败未归账的条目数（annos+fields）
    ca::usize annoBlocked = 0;         ///< rewrite 复核预检拦下的条目数（fail-closed 不摘）
    ca::usize resourceCandidates = 0;  ///< 删除时复核不再满足判据的资源数
};

/// @brief 验收结果。residual = 改写后容器里仍可判定的三面残留（语料单罐
///        门要求全 0；深度预检拦下的敌意槽不在 residual 视野内——那是
///        不可解面，不是清理缺陷，scan 的 annoBlocked 已如实记账）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;          ///< 违例台账（class-count/resource-count/条目 URI）
    ca::usize                residualAnnos     = 0;  ///< 残留 `@Metadata` 条目
    ca::usize                residualFields    = 0;  ///< 残留水印字段
    ca::usize                residualResources = 0;  ///< 残留待删资源
};

/// @brief SCAN：纯读。三面盘查（注解三面普查 + 水印字段普查 + 资源清单），
///        Era 门在面外先关（未判即空手而回，一个字节不看内容判据）。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult：资源先删（树版序），类按
///        uri 复核后清理（摘注解 → 删水印字段 → 压池带走死条目 → 写回）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重解析（`classesIntact`，与全部
///        残留同门）+ 资源对账（`resourcesShrunk`：数不增 + 清单逐条不在
///        场）+ 三面 residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore, ca::usize resourcesBefore);

}   // namespace grunt::metadata_strip
