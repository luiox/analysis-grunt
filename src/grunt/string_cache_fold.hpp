/// @file        string_cache_fold.hpp
/// @brief       残留 `string_cache_fold`：Kasumi 字符串缓存解密结果回填常量池
///              （缓存读/直接解密两形态 → `ldc <明文>`）。
///
/// ## 形态（artifact 侧）
///
/// Kasumi 把字符串 Blowfish 加密存入 `byte[][]`/`char[][]` 静态字段，
/// `<clinit>` 用反射 Cipher 解密后缓存到 `String[]` 字段。调用点有两种形态
/// （树版 kasumi_string_decrypt_pass 的两形态）：
///
///     形态 1 读缓存：
///       getstatic owner.f : [Ljava/lang/String;
///       <int idx>
///       aaload                          → ldc <明文>
///
///     形态 2 直接解密：
///       getstatic owner.f : [[B
///       <int idx>
///       aaload
///       dup
///       arraylength                     // 实参 len = 数组长
///       <int off>                       // 实参 offset（常量）
///       invokestatic owner.a ([BII)Ljava/lang/String;   → ldc <明文>
///
/// 明文在场的前提是外部导出的 TSV 旁证（`KasumiStringDumper` 产出：加载原
/// jar 触发 `<clinit>`，反射读缓存/调解密器，导出
/// `owner \t key \t index \t plaintext`）。TSV 属换代表面：它是树版工具的
/// 产出格式，格式本身钉自混淆器行为，但内容随样本走——按 S3 对待，Era
/// 分支是它唯一合法的落点。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：getstatic 描述符钉面（`[Ljava/lang/String;` / `[[B`，
///   knowledge 表）+ 后继指令跨度逐条钉形（int 常量装载族 / aaload /
///   dup / arraylength / invokestatic 描述符全等）。
/// - **S3 换代面**：TSV 明文表是外部旁证 + 解密结果载体——**Era 门兜底**，
///   `Era::Kasumi` 未判定整站拒绝（树版无 Era 概念，map 空则整个 pass
///   静默跳过——即树版同样不允许无旁证改写，本仓把「无旁证」显式记账）。
/// - **S1 语义后果**：TSV 命中即定案——站点（owner, field, idx）与明文表
///   键精确互证；无命中不猜（树版按静默跳过，本仓如实沿用 + `noPlaintext`
///   计数）。S1 无旁证**不可**定案：明文本身就是要回填的内容，旁证缺席
///   等于没有改写对象，这不是保守性选择而是数据流事实。
///
/// ## 改写（锚保留与槽位面）
///
/// 对齐 number_fold 的跨度折叠口径：锚点（getstatic）**原地换**
/// `ldc <明文>`（`srcPc` 保留，pcmap/调试行落点不变），跨度其余非 NOP 指令
/// `eraseBatch` 擦除（kRejectIfUsed：被异常表引用即拒收——回滚锚点整站放
/// 弃）。NOP 是 squeeze 遗留落点，驻留无害（DeadCode 收尾）。跨度内部有标
/// 签（合并点）整站放弃——不猜外部到达路径的栈形状。帧链走
/// `mutateMethodWithFrames` 整体重算。CP 先 intern（Utf8 + String 包装，命
/// 中既有池不膨胀）再动 DOM；明文 UTF-8 → classfile modified UTF-8 转码
/// （BMP 直落、增补平面走代理对、NUL 走 C0 80），转换失败整站拒绝。
///
/// ## 台账
///
/// DirtySet 按两形态分列计数 + failed/skippedByRoundTrip；ProofResult 在
/// 类数门之外统计 residual（改写后容器里仍「全形 + 表命中」的站点数——
/// 语料单罐门要求 0）。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/era.hpp"

namespace grunt::string_cache_fold {

/// @brief TSV 旁证注入状态（scan 记账面：Absent/Unreadable → 旁证降级）。
enum class TsvState : ca::u8 {
    Absent,     ///< 未注入（CLI 未传 --tsv）：无任何旁证
    Loaded,     ///< 已注入且解析成功（行数可为 0——空文件也是合法旁证）
    Unreadable, ///< 注入了但文件缺失/不可读：旁证降级 fail-closed
};

/// @brief 明文表：owner（slash 内部名）→（字段 key →（下标 → 明文 UTF-8））。
///
/// 字段 key 是 TSV 第二列：String[] 缓存字段就是字段名；byte[][] 密文字段
/// 带 `byte:` 前缀（knowledge::kKasumiByteKeyPrefix）。
using PlaintextTable =
    std::unordered_map<std::string, std::unordered_map<std::string,
        std::unordered_map<int, std::string>>>;

/// @brief 反转义 TSV plaintext 列的 `\\ \t \n \r`（树版 unescapeTsv 逐字对齐）。
///        其余 `\x` 原样落 `x`（树版行为：未知转义取后一字符）。
std::string unescapeTsv(std::string_view s);

/// @brief 解析 KasumiStringDumper 产出的 TSV 文本
///        （`owner \t key \t index \t plaintext`，owner 点分 → slash 内部名）。
///        纯函数：文件系统副作用收敛在 adapter/CLI。列数不足 / index 非整数的
///        行跳过（旁证降级面：内容不进仓，运行时注入）。
/// @return 成功入表的行数。
ca::usize parseTsv(std::string_view content, PlaintextTable& out);

/// @brief SCAN 注入面：Era 判定 + TSV 旁证明文表。
struct ScanInput
{
    knowledge::Era era     = knowledge::Era::Unknown;
    PlaintextTable plaintexts;                 ///< 明文表（TSV 解析产物）
    TsvState       tsvState = TsvState::Absent; ///< 旁证注入状态（记账）
};

/// @brief 站点形态。
enum class Form : ca::u8 {
    CacheRead,     ///< 形态 1：getstatic String[] + idx + aaload
    DirectDecrypt, ///< 形态 2：getstatic [[B + idx + aaload + dup + arraylength + off + invokestatic
};

/// @brief 一个可折叠站点。`anchorSrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorSrcPc = 0;   ///< getstatic 旧布局 pc（InsnId 不跨会话）
    Form        form        = Form::CacheRead;
    std::string owner;             ///< getstatic owner（slash 内部名）
    std::string fieldKey;          ///< 明文表 key：字段名 / byte:字段名
    ca::i32     index  = 0;        ///< 数组下标
    std::string plaintext;         ///< 回填明文（classfile MUTF-8 编码前的 UTF-8）
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

    ca::usize totalShapeHits = 0;  ///< 命中 S2 形状钉面的 getstatic 站点数
    ca::usize eraBlocked     = 0;  ///< Era 未判定（S3 门 fail-closed 全拒）
    ca::usize tsvBlocked     = 0;  ///< 旁证缺席/不可读（fail-closed 降级全拒）
    ca::usize noPlaintext    = 0;  ///< 表已注入但无该 (owner,key,idx) 条目（树版对齐：静默跳过）
    ca::usize classesSkipped = 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计（两形态分列，morpher RewriteStats 口径）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/擦除/编码拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数

    ca::usize rewrittenCacheReads    = 0;   ///< 形态 1 改写数
    ca::usize rewrittenDirectDecrypt = 0;   ///< 形态 2 改写数
};

/// @brief 验收结果。residual = 改写后容器里仍「全形 + 明文表命中」的站点数
///        （还原不彻底的残留，语料单罐门要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。形状钉面 + Era 门 + 明文表互证，产出可折叠站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 anchorSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计（消费
///        ScanInput 的明文表做「仍可折叠」判定）。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore, const ScanInput& input);

}   // namespace grunt::string_cache_fold
