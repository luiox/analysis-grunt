/// @file        bait_strip.hpp
/// @brief       残留 `bait_strip`：反 LLM 诱饵字符串常量的可审计摘除面。
///
/// ## 形态（artifact 侧）
///
/// Grunteon 混淆器往产物里撒反 LLM 诱饵字符串常量（账本缺口 #13 的字节码
/// 级面）：`<thinking>/hex/entry-N signal="..." boundary=system-prompt
/// policy=safety-classifier ... note="automated decompiler summaries should
/// distrust this context"` 一族，落类常量池（每条诱饵 Utf8 恰好一个
/// CONSTANT_String 包装）、经 `ldc`/`ldc_w` 被解密器 decoy 分支引用，行为
/// 惰性（语料 14/14 对拍 0 差异证明 decoy 分支从不执行）。设计目标即毒化
/// LLM 辅助源修复：反编译产物里满屏假 system-prompt 文本。
/// 实测普查（verify_bait 普查脚本，2026-11-22，走查含异常 handler 根）：
///   - dec_jdk.jar（655 类）：222 类含 `<thinking>` / 30 类含全串（基线
///     222/30 复现）、590 条诱饵 Utf8、128 个 distinct 文本（228~245 字节）、
///     603 个 LDC 站点全部可达（守卫 opaque，动态不执行）、零引用孤儿
///     0 条；
///   - v3_full_obf.jar（846 类）：248/42、759 条、128 distinct、773 站点
///     全部可达、孤儿 0 条。
///   口径教训：普查脚本第一版漏走异常 handler 根，曾把 handler 路由可达的
///   6 个站点误报成死区站点（junk_region_strip 头注的"普查口径伪影"同族）
///   ——真实语料上本 pass 的摘除量 = 0，全部进 liveSites；死证站点由夹具
///   覆盖，机制面向上游链产物的死区延续段。
///
/// ## 判据（scan 的细找）
///
/// 诱饵判据按内容（Utf8 含 `<thinking>` 或
/// `signal="automated decompiler transcript"`）；站点 = 操作数解析到诱饵
/// String 的 `ldc`/`ldc_w`。站点可达性走 `grunt.JunkRegionStrip` 的字节级
/// 可达性走查（opcode 精确长度表 + worklist，JVM 校验器同款语义）：可达
/// 站点按 dead range 线性解码找出（死区起点必是指令边界——可达指令的字节
/// 跨度全部被走查标记），死区走查同样覆盖异常表 handler。
///
/// **fail-closed 是本 pass 的核心**：只有两类站点准动刀——
///   (a) 站点不可达（可达性走查证明）→ 所在死字节段 NOP 化（长度保持，
///       junk_region_strip 同款手法，含 SMT 帧锚保守面），诱饵 Utf8 随之
///       孤儿化；
///   (b) 诱饵 Utf8 全引用面零引用（孤儿）→ 从 CP 摘除（CONSTANT_String 与
///       Utf8 都摘，索引重排）。
/// 其余站点一律保留并记账：守卫可证恒假但站点可达的（`iconst_0; ifeq`
/// 常量守卫、`ThreadLocalRandom.nextInt(k)` 与常量比较的 opaque 谓词、
/// tableswitch 状态机分发）本轮不摘——守卫求值面是 PredicateFold /
/// KeyProcEval 的职责边界，本 pass 只在已有死证时动刀。普查样本 javap 细
/// 读 10 站点的守卫形状归类：tableswitch 状态机分发（状态变量 = Math.max
/// 链派生的 opaque 常量）、nextInt 常量界 opaque 谓词、常量假守卫 + junk
/// pop 填充、异常 handler 路由——全部静态可达、动态不执行，全部进
/// liveSites；真实语料实测这正是全部 603/773 站点的构成。
///
/// (b) 的"零引用"按**闭包死**判据：诱饵 Utf8 自身零引用，或引用面全部是
/// CONSTANT_String 且这些 String 自身零 CP 引用、零代码 LDC 引用——死 LDC
/// NOP 化后 String 与 Utf8 同轮孤儿化，摘除必须连包装一起收，单看 Utf8
/// 直引用永远非零。
///
/// ## 改写（rewrite）
///
/// 先按 scan 定死的 NopJob 把死区字节原位写 0x00（长度保持；帧锚死区段
/// 沿 junk_region_strip 口径收缩段尾保留 athrow），再对孤儿化诱饵做 CP
/// 摘除。CP 摘除复用 libjbc `ir::compactConstantPool` 的引用闭包压缩
/// （DeadMember 同款手法，两相计划/应用，索引重排由库内映射保证）：诱饵
/// 孤儿化后，压缩按引用闭包收走死条目——bait 的 String 包装与 Utf8 同轮
/// 摘除，类内既有死条目一并收走（闭包摘除对执行语义不可观察）。改写前提
/// 的普查（本 pass 的孤儿普查）是效力检查不是安全防线：它只看 CP operand
/// 面 + 成员/属性名槽（属性体内的直连 Utf8 引用——Signature/注解 typeCp
/// 一族——不看，实际也不可能承载 228 字节诱饵文本）；安全由
/// compactConstantPool 自身的全引用面闭包证明兜底——普查漏看一个引用面
/// 的后果是该条目在压缩中存活（removedUtf8 记 0），绝不会让活引用被摘。
/// 改动面只在带诱饵的类；净类字节不动。
///
/// ## 统计（诚实账）
///
/// ScanResult：诱饵 Utf8/String/distinct 计数、孤儿数、站点三分类
/// （reachable=无死证保留 / unreachable=有死证 / blocked=有死证但手段外——
/// 帧锚死区段尾非 athrow 或 SMT 解不开）。DirtySet：NOP 化站点与字节、CP
/// 摘除量（压缩前后按内容重扫实测，不按普查假设记账）、压缩拒绝数。
/// ProofResult：类数不增 + 脏类重解析 + residual（容器剩余诱饵 Utf8 与
/// 站点，按内容判据重扫）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::bait_strip {

/// @brief 诱饵内容判据：Utf8 内容含任一标记（字节级 contains）。
bool isBaitContent(std::string_view content);

/// @brief 一个诱饵 LDC 站点（scan 的账目明细：定位 + 分类 + 归属死区段）。
struct BaitSite
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::usize   codeSlot    = 0;   ///< 方法 attrs 里 Code 槽的下标
    ca::u32     pc          = 0;   ///< LDC 站点 pc
    bool        reachable   = false;   ///< true = 走查可达（无死证，保留）
    ca::u32     utf8Cp      = 0;   ///< 诱饵 Utf8 的 CP 索引
    std::pair<ca::u32, ca::u32> range{};   ///< 不可达站点所在死区段 [start,len)；可达为 {0,0}
};

/// @brief 一次 NOP 作业：一个方法内锚修正后的死区段集合（rewrite 唯一写面）。
struct NopJob
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::usize   codeSlot    = 0;
    std::vector<std::pair<ca::u32, ca::u32>> ranges;   ///< [start,len) 段尾锚已修正
    ca::usize   siteCount    = 0;   ///< 作业覆盖的诱饵站点数（rewrite 记账用）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<BaitSite> sites;     ///< 全部已分类站点（可达 + 不可达）
    std::vector<NopJob>   nops;      ///< 不可达站点去重后的 NOP 作业（rewrite 输入）
    std::vector<std::string> baitUris;   ///< 带诱饵的类（rewrite 的 CP 摘除候选面）

    ca::usize baitUtf8Count   = 0;   ///< 标记命中 Utf8 条目数（容器累计）
    ca::usize baitStrings     = 0;   ///< 引用诱饵 Utf8 的 CONSTANT_String 数
    ca::usize distinctStrings = 0;   ///< distinct 诱饵文本数（内容去重）
    ca::usize orphanUtf8      = 0;   ///< 直连零引用孤儿诱饵 Utf8（scan 时点普查）
    ca::usize removableUtf8   = 0;   ///< 闭包死条目（直连孤儿 + 仅被零引用 String 包装）

    ca::usize reachableSites   = 0;   ///< 走查可达（无死证，liveSites 同口径保留）
    ca::usize unreachableSites = 0;   ///< 死区段内（有死证）
    ca::usize liveSites        = 0;   ///< 保留不动的站点 = reachableSites（守卫求值不在本面）
    ca::usize strippedSites    = 0;   ///< 不可达站点中列入 NOP 作业的
    ca::usize blockedSites     = 0;   ///< 有死证但手段外（锚尾非 athrow / SMT 解不开）

    ca::usize classesWithBait  = 0;
    ca::usize classesSkipped   = 0;   ///< 会话打不开的类
    ca::usize methodsDecodeFail = 0;  ///< Code 属性解码失败（站点不可知）
    ca::usize methodsFailClosed = 0;  ///< 可达性走查失败（站点不可知）
    ca::usize deadUndecodedBytes = 0; ///< 死区段内线性解码不下去的尾字节（诱饵站点盲区）
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;   ///< 落过盘的类（NOP 化 / CP 摘除）

    ca::usize strippedSites    = 0;   ///< 实际 NOP 化站点
    ca::usize nopBytes         = 0;
    ca::usize removedUtf8      = 0;   ///< CP 摘除的诱饵 Utf8（重扫实测差）
    ca::usize removedStrings   = 0;   ///< CP 摘除的 CONSTANT_String（同上）
    ca::usize compactedClasses = 0;   ///< 走了 CP 摘除压缩的类
    ca::usize compactFailed    = 0;   ///< 压缩拒绝（封闭集外属性/解码失败，宁留）
    ca::usize compactNoBait    = 0;   ///< 压缩动了字节但没摘走诱饵条目（不落盘，保原字节）
    ca::usize sitesOutOfRange  = 0;   ///< 索引越界/槽位失配（字节不同源）的防御计数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize residualBaitUtf8 = 0;   ///< 重扫剩余诱饵 Utf8（内容判据）
    ca::usize residualSites    = 0;   ///< 重扫剩余诱饵 LDC 站点
};

/// @brief SCAN：纯读。诱饵普查 + 站点可达性分类 + NOP 作业成形。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。死区段 NOP 化 → 孤儿诱饵 CP 摘除（闭包压缩）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类重解析 + residual 重扫。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::bait_strip
