/// @file        material_key_restore.hpp
/// @brief       残留 `material_key_restore`：Grunt v3 RuntimeMaterial 材料通道
///              解密器 key 静态折回（indy_restore `materialLane` 拒绝桶的闭环）。
///
/// ## 形态（artifact 侧，boar-main.jar 逐字节验证；源锚 `grunt3@e41e852`
///    `RuntimeMaterial.kt` / `ReferenceObfuscate.kt`）
///
/// v3 `ReferenceObfuscate` 在带 RuntimeMaterial 草稿的类里生成材料通道解密器
/// （`(String;)String`，窗口 `charAt; LDC K; INVOKESTATIC <self> ()I; IXOR;
/// IXOR; I2C`）与材料取值方法 `materialKey()`（`createMaterialKeyMethod`
/// 逐字 9 条：`GETSTATIC keyField(J); DUP2; INT(32); LUSHR; LXOR; L2I;
/// GETSTATIC poisonField(I); IXOR; IRETURN`），有效 key = `K ^ materialKey()`。
/// 每类恰好一个材料解密器、`materialKey()` 恰被它调用一次（boar xref 验证）。
///
/// 材料初值链（boar 验证，上游供给链 `grunt.IndyRestore` + `grunt.
/// StringPoolFold` 之后全静态）：
///
///     <clinit>:  ldc <池明文>; INT(32);
///                INVOKESTATIC <parse 蹦床或直连> (Ljava/lang/String;I)J
///                ldc2_w C; lxor; putstatic <keyField> J      ← keyField 初值
///     poison 初值：纯常量走廊（boar 里 `ldc2_w X; l2i; ldc Y; ixor` ≡ 0）
///
/// parse 面是 `java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J`
/// （直连，或体恰为 `aload_0; iload_1; invokestatic <parse>; lreturn` 的单跳
/// 蹦床，IndyRestore 第一轮已把蹦床里的 plain-lane indy 解直）。生成面源锚：
/// RuntimeMaterial 的 clinitMaterialInit 本来发 `LDC2_W` 常量，boar 的
/// number-reobf 层把它包进基 32 字符串通道；`simpleDecryptKey = decryptKey
/// xor seedFold`（ReferenceObfuscate 构建期计算）钉死清洁运行期字段态 ==
/// 初值常量这一 S1 等价。
///
/// ## 还原口径（明确写死的语义边界）
///
/// RuntimeMaterial 是 AntiDebug 面：clinit/`<init>` 里有守卫支路
/// （`INVOKESTATIC guard()I; dup; ifeq clean`，guard 读 ManagementFactory/
/// 环境变量找调试证据），可疑路径才写 keyField/poison（扰动窗口形：
/// `GETSTATIC F; <expr>; LXOR|IOR [; IOR]; PUTSTATIC F`）。**还原目标 =
/// 清洁运行环境语义**（guard 未命中：keyField 恒初值、poison 恒初值），
/// 这不是对运行期岔路的静态裁定，而是 deobf 的既定口径：反调试扰动路径
/// （guard 命中时故意用错 key 自毁解密）不在还原范围。结构性边界照旧
/// fail-closed：初值表达式不可静态求值、字段出现非扰动窗口形的额外写、
/// 多个初值写、初值写不在 clinit、守卫指纹缺席 → 整类拒绝。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：解密器窗口形（与 indy_restore 同一窗口回溯）+
///   `materialKey()` 体 9 条逐字 + 材料字段 private static + 写者普查
///   （唯一初值写 + 其余写全为扰动窗口形 + 所在方法带守卫指纹）+ 初值
///   跨度逐条钉形（Contiguous 无合并点）；
/// - **S1 语义后果**：`materialKey = fold(keyField 初值) ^ poison 初值`
///   （`fold(K) = (int)(K ^ K>>>32)`，源锚 `Long.foldToInt`）；核对 =
///   路由到本材料解密器的全部 BSM indy 站点按候选 key 解出合法
///   owner/name/desc 且与 indy desc 核对全过（错 key 解出垃圾必死在这步，
///   与 indy_restore simple 面同一自证逻辑）；
/// - **无 Era 检查**：判据全是结构自证事实（同 string_pool_fold 口径）；
///   类级 Grunt BSM desc 指纹做家族锚（无 BootstrapMethods 指纹的类不进面）。
///
/// ## 改写（与 indy_restore 的协作面）
///
/// **零侵入协作**：本残留只把材料解密器窗口里的 `INVOKESTATIC <self>
/// materialKey()I` 原地换 `LDC <materialKey>`（srcPc 保留，帧链正统写回）
/// ，窗口随即变成 indy_restore 已有的 reobf 折叠键形（`LDC K; LDC MK;
/// IXOR; IXOR; I2C`，key = K ^ MK），indiy_restore 第二轮按既有 Plain 通道
/// 折回全部 materialLane 站点。indy_restore 本体一个字节不动。链序（外部
/// 知识）：`grunt.IndyRestore,grunt.StringPoolFold,grunt.MaterialKeyRestore,
/// grunt.IndyRestore`，材料初值跨度依赖 StringPoolFold 折回的池明文 ldc；
/// 池读未折回的类 fail-closed（`initBlocked` 独立计数，报链序缺位）。
/// materialKey() 本体、材料字段与守卫支路不删（折回后 materialKey 零引用，
/// 交下游 DeadMember/DeadCode 回收）。
///
/// ## 统计
///
/// DirtySet 按类/站点计数 + failed/skippedByRoundTrip；ProofResult 在类数检查
/// 之外统计 residual（改写后容器里仍带材料解密器窗口形的类数，语料单 jar
/// 检查要求 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::material_key_restore {

/// @brief SCAN 注入面：本残留无外部旁证（判据全为容器自证事实），结构占位
///        与三段签名保持同形。
struct ScanInput
{
};

/// @brief 一个可折回的材料解密器。`invokeSrcPc` 跨会话稳定，rewrite 靠它
///        重定位。
struct Site
{
    std::string uri;                    ///< 材料解密器所在类
    ca::usize   decryptorMethodIndex = 0;  ///< (String;)String 材料解密器槽
    ca::u32     invokeSrcPc          = 0;  ///< INVOKESTATIC `<self>`()I 旧布局 pc
    ca::i32     decryptorRawKey      = 0;  ///< 窗口 LDC K（异或前原始 key）
    ca::i32     materialKey          = 0;  ///< 静态折回值（= K ^ 有效 key 的补数面）
    ca::usize   sitesVerified        = 0;  ///< S1 核对的路由站点数
};

/// @brief 拒绝（fail-closed 计数：拒绝原因进 trace，站点不动）。
struct Reject
{
    std::string uri;
    ca::usize   methodIndex = 0;   ///< 材料解密器槽（定不下来时为 0）
    std::string why;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize materialClasses = 0;  ///< 材料解密器窗口在场的类数
    ca::usize keyBodyMismatch = 0;  ///< materialKey() 体非 9 条逐字形（或字段不 private）
    ca::usize initBlocked     = 0;  ///< 初值跨度不可静态求值（含池读未折回的链序缺位）
    ca::usize initConflict    = 0;  ///< 初值写多个/不在 clinit（多写冲突）
    ca::usize writerShape     = 0;  ///< 字段存在非扰动窗口形的额外写
    ca::usize writerUnguarded = 0;  ///< 扰动写所在方法缺守卫指纹
    ca::usize keyRefuted      = 0;  ///< S1 核对失败（候选 key 解不出合法目标）
    ca::usize decryptorAmbiguous = 0;  ///< 一类多个材料解密器窗口 / materialKey 调用点多于 1
    ca::usize classesSkipped = 0;   ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/帧链拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数
};

/// @brief 验收结果。residual = 改写后容器里仍带材料解密器窗口形的类数
///        （还原不彻底的残留，语料单 jar 检查要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。窗口定位 + materialKey 体形 + 初值链求值 + S1 核对，
///        产出可折回站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 invokeSrcPc 重定位后复核再改
///        （INVOKESTATIC 原地换 LDC）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::material_key_restore
