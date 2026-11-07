/// @file        string_pool_fold.hpp
/// @brief       残留 `string_pool_fold`：Grunt v2 StringEncrypt / v3
///              StringArrayedEncrypt 字符串池读点折回 `ldc` 明文。
///
/// ## 形态（artifact 侧；doc/obf-behaviors/grunt-v2-behaviors.md §1.1 /
///      grunt-v3-behaviors.md §1.1）
///
/// Grunt 两代给每类生成同一套字符串池三件套（解密算法两代逐字相同，差别只
/// 在密文形态配置）：
///
///     <clinit> 池分配前导（头插）：
///       <int poolLen>; ANEWARRAY java/lang/String;
///       PUTSTATIC owner.<pool> [Ljava/lang/String;
///       INVOKESTATIC owner.<arrayInit> ()V
///
///     arrayInit 填池（逐项 store 块，直线无标签）：
///       GETSTATIC owner.<pool> [Ljava/lang/String;
///       <int idx>
///       literal（v2 Arrayed=false）：ldc <密文串>;
///                 invokevirtual java/lang/String.toCharArray ()[C
///       arrayed（v3 Carray=true）： <int len>; newarray char;
///                 {dup; <int i>; <int code>; castore}×len
///       <long seed>; <int key>
///       invokestatic owner.<dec> ([CJI)Ljava/lang/String;
///       aastore
///
///     解密器（classKey 以 INT 常量烧在体内，desc 两代固定）：
///       <int classKey>; iload 3; ixor; istore n2   // n2 = classKey ^ key
///       循环：char 级 XOR 流（状态随 i & 0xFF 旋转），体是确定性生成面
///
///     调用点三元组（本残留的改写对象）：
///       GETSTATIC owner.<pool> [Ljava/lang/String;
///       <int idx>
///       aaload                            → ldc <明文>
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：池链锚——clinit 前导四连（ANEWARRAY 钉
///   `java/lang/String` + PUTSTATIC 钉 `[Ljava/lang/String;` + INVOKESTATIC
///   `()V`）、arrayInit 逐块 store 序列逐条钉形（直线无标签）、解密器体内
///   三窗口（init 四连 / 循环头 / 循环核 48 条逐字钉形，槽位互证）。
/// - **S1 语义后果**：seed/key/classKey 全为装载期常量、解密器纯函数（循环
///   核钉形保证）、store 序列固定——`owner.field[idx] → 明文` 映射编译期
///   可建，读取点换 `ldc` 明文语义恒等。判据不读解密算法语义细节，只读池
///   结构 + 调用点三元组（行为账 §1.1 口径）；算法本体是知识转写
///   （grunt2@36c0eef / `grunt3@e41e852` 逐行，见 .impl.md 摘录）。
/// - **无 Era 门**：v2/v3 共用全部形态常量（含解密算法），形态内无版本
///   敏感判据；Era 判别是缺口 #15，不在本残留（era.hpp 不消费本文件）。
///   家族门由池链结构自证：Kasumi 缓存字段同形三元组（`getstatic
///   String[]; idx; aaload`）不带 Grunt 池分配前导与 `([CJI)` 填池链，
///   结构上不可命中。
///
/// ## 改写（锚保留与槽位面）
///
/// 对齐 string_cache_fold 的读点折叠口径：锚点（getstatic）**原地换**
/// `ldc <明文>`（srcPc 保留，pcmap/调试行落点不变），三元组其余两条
/// `eraseBatch` 擦除（kRejectIfUsed 拒收即回滚锚点整站放弃）；三元组内部
/// 有标签（合并点）整站拒绝。**池字段 / arrayInit / 解密器不删**——读点
/// 归零后由 DeadCode / DeadMember / 压池统一回收（与 eskid opaque-table
/// 同口径）。帧链走 mutateMethodWithFrames 整体重算；明文 UTF-16 →
/// classfile MUTF-8 转码（代理对各落 3 字节形，与 javac 同构），转码失败
/// 整站拒绝。
///
/// ## 台账
///
/// DirtySet 按两密文形态分列计数 + failed/skippedByRoundTrip；ProofResult
/// 在类数门 + 脏类重解析门之外统计 residual（改写后容器里仍「池链在场 +
/// 三元组命中池表」的站点数——语料单罐门要求 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::string_pool_fold {

/// @brief 明文码元类型（Java String 的 UTF-16 code unit 语义）。
using PlainUnits = std::vector<ca::u16>;

/// @brief 密文形态（arrayInit 块内密文的承载方式；v2 默认 literal、v3 默认
///        arrayed，但都是配置选择不是版本键）。
enum class CipherForm : ca::u8 {
    Literal, ///< ldc 密文串 + toCharArray
    Arrayed, ///< NEWARRAY T_CHAR + 逐码元 CASTORE
};

/// @brief SCAN 注入面：本残留无外部旁证（判据全为容器自证事实），结构占位
///        与三段签名保持同形。
struct ScanInput
{
};

/// @brief 一个可折叠读点。`anchorSrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorSrcPc = 0;   ///< getstatic 旧布局 pc（InsnId 不跨会话）
    std::string owner;             ///< 池字段 owner（slash 内部名，= 本类）
    std::string fieldName;         ///< 池字段名
    ca::i32     index      = 0;    ///< 池下标
    CipherForm  form       = CipherForm::Literal;
    PlainUnits  plainUnits;        ///< 明文码元（改写面按 MUTF-8 编码 intern）
    std::string plaintext;         ///< 明文 UTF-8 记录形（trace/台账/单测；
                                   ///< 代理对重组 4 字节；孤立代理整站拒绝）
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

    ca::usize poolClasses      = 0;  ///< 池链三件套 + 解密器窗口全过的类数
    ca::usize tripleHits       = 0;  ///< 已验证池上的三元组形状命中数（含 idx 缺表）
    ca::usize idxMissing       = 0;  ///< 三元组下标不在池表（fail-closed 不折）
    ca::usize arrayInitBlocked = 0;  ///< 前导在场但 arrayInit 块序列不合规的类
    ca::usize decryptorBlocked = 0;  ///< 解密器窗口不合规/歧义的类
    ca::usize classesSkipped   = 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计（两形态分列，morpher RewriteStats 口径）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/擦除/编码拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数

    ca::usize foldedLiteral = 0;   ///< literal 密文形态改写数
    ca::usize foldedArrayed = 0;   ///< arrayed 密文形态改写数
};

/// @brief 验收结果。residual = 改写后容器里仍「池链在场 + 三元组命中池表」
///        的站点数（还原不彻底的残留，语料单罐门要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。池链识别（前导 + arrayInit + 解密器窗口 + 参数提取）
///        → 调用点三元组定案，产出可折叠站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 anchorSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::string_pool_fold
