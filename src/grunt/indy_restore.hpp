/// @file        indy_restore.hpp
/// @brief       残留 `indy_restore`：Grunt v2/v3 indy 调用点直接解密回填
///              （simple/heavy 两形，缺口 #2 闭环）。
///
/// ## 形态（artifact 侧，源锚 `grunt3@e41e852` `ReferenceObfuscate.kt` /
/// grunt2@36c0eef `InvokeDynamicTransformer.kt`，两代逐字同源）
///
/// Grunt 把非 INVOKESPECIAL 的方法调用（INVOKESTATIC / INVOKEVIRTUAL /
/// INVOKEINTERFACE）换成 `invokedynamic`，目标信息加密进 bootstrap 静态参数：
///
///     invokedynamic [encOwner, encName, encDesc, isVirtual]     ← simple 形
///     invokedynamic [encOwner, magic1, magic2, isVirtual]       ← heavy 形
///     ↓ bootstrap（每类一个，随机名；名字不进判据，desc 是唯一指纹）
///     simple: 三段串各自 `charAt ^ key`（key 在同类 (String;)String 解密器体
///             的 LDC 里，v3 RuntimeMaterial 联动时另有材料通道，见下）；
///             owner 是点分名（java.lang.Math），virtual 时 indy desc 首参
///             补 Object receiver；
///     heavy:  encOwner 经 BSM 体内 LDC generalKey 解一层；name&lt;&gt;desc 编在
///             owner 类 @Metadata 注解的 d1/d2 数组里，遍历 d1，第 i 项经
///             magic2 解出 data1 整数串，`data1 % 59557 == magic2` 命中即取
///             d2[i] 经 magic1 解出 `name&lt;&gt;desc`。
///
/// `encrypt` 是逐 UTF-16 码元自逆 XOR（ `(code ^ xor).toChar()` ），纯算术，
/// C++ 转写直解（无 jsim 面）。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：BSM desc 指纹（simple/heavy 7 参，v2/v3 逐字同源）+
///   解密器形状（`charAt; LDC K; IXOR; I2C` 单层窗口，或 v3 材料通道双层
///   `LDC K; INVOKESTATIC ()I; IXOR; IXOR; I2C`）；
/// - **S1 语义后果**：解密产物与调用点吻合，simple 三段串解出合法
///   owner/name/desc 且与 indy desc 核对（static 全等；virtual 首参
///   Object receiver + 其余逐位一致）；heavy 的 `data1 % 59557 == magic2`
///   完整性自证 + `name&lt;&gt;desc` 拆分 + 同一道 indy desc 核对。isVirtual 只
///   认 0/1，越界即拒。
/// - **S3 版本敏感面**：heavy 的 `@Metadata` 数组解释（mv 检查 + 59557 模数）
///   挂 Era 检查（probeEra 判 Grunt3 才激活）；simple 面判据全部结构自证，
///   不挂 Era（理由：key 静态在场 + 产物核对，误判需要"随机密文恰好解出
///   与 indy desc 吻合的合法描述符"，概率上排除）。
///
/// ## v3 RuntimeMaterial 联动（材料通道边界）
///
/// v3 `createDecryptMethod` 在类带 RuntimeMaterial 草稿时生成材料通道解密器
/// （`LDC key; INVOKESTATIC <self> ()I; IXOR; IXOR`），有效 key =
/// `decryptKey ^ materialKey()`，而 materialKey 读 canonicalKey/poison 静态
/// 字段，boar 验证其运行期值由解密后的字符串池经 parseLong 转出（clinit
/// 内 CFF 摊平），纯静态不可解（须模拟执行 + 上游供给链，原语缺失）。
/// 本批材料通道一律 fail-closed（`materialLane` 桶），只在解密器体形状能
/// 自证时计数，不猜。
///
/// ## BSM→解密器键链（三路，按现场可达性）
///
/// 1. **同名直连**（v3 material 配对类，源锚 `plainDecryptName ==
///    plainBsmName`，两者同为 `bsmName1.substring(1, len-3)`）：simple BSM
///    名与一个在场 `(String;)String` 解密器同名 → 直取其 LDC key；
/// 2. **BSM 体跟随**（v2/v3 常态，BSM 体在场）：BSM 体内首条
///    `INVOKESTATIC <self> (String;)String` 即解密器；
/// 3. **名字族推导**（v3，BSM 被 NativePipeline 摘成 ACC_NATIVE 空壳时，
///    boar 全量现场）：同代生成面把解密器名派生自 BSM 名
///    （`decryptName = bsmName1.substring(2, len-1)`）：候选解密器名 D 满足
///    `len(D) == len(BSM名) - 3 && BSM名.substr(2) 前缀 == D` 且体为
///    解密器窗口时认亲；heavy 的 generalKey 在 BSM 体内（LDC），壳死则
///    heavy 面无键（fail-closed，boar 验证 heavy 站点为 0）。
///
/// ## 改写（插入式站点的锚保留与槽位面）
///
/// v2/v3 只重定向方法调用（无字段/构造器形态），分派只有三态：
/// static → INVOKESTATIC（纯原地）；virtual → INVOKEVIRTUAL /
/// INVOKEINTERFACE（纯原地，receiver 槽不插不删，indy desc 的 Object 首参
/// 就是原 receiver 槽，JVM 校验语义与原 INVOKEVIRTUAL 一致）。原地换指令
/// 保 srcPc，锚保留/帧链写回与 indy_decode 同源（`mutateMethodWithFrames`
/// 正统帧链）。接口落点靠旁证：容器普查优先，CLI `--jdk` 注入的 JDK 接口集
/// 双向作证（在集 = 接口；不在集 = 类；两处皆无 = fail-closed）。
///
/// ## 统计
///
/// DirtySet 按 static/virtual 分列 + failed/skippedByRoundTrip；ProofResult
/// 在类数检查之外统计 residual（改写后容器里仍带 Grunt simple/heavy BSM desc
/// 指纹的 indy 调用点数）。residual 只计指纹命中站点，LambdaMetafactory
/// / StringConcatFactory 等非 Grunt indy 与材料通道站点不在 residual 口径
/// （前者非本家、后者本批明确 fail-closed 计数在 ScanResult）。
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/era.hpp"

namespace grunt::indy_restore {

/// @brief SCAN 注入面：Era 判定 + JDK 接口旁证。
///
/// 文件系统副作用收敛在 scan 之前（adapter/CLI）：残留本体不做任何文件系统
/// 遍历。`jdkInterfaces` 由 CLI `--jdk` 扫描注入（adapter 在 Era 判定非
/// Unknown 时激活）。virtual 落点分派：容器普查优先（权威）；JDK 集是穷举
/// 普查，**成员与否双向作证**（在集 = 接口；不在集 = 类）；**空集 = 无任何
/// 旁证**，virtual 目标不在容器时整站 fail-closed 拒绝（invokevirtual 调
/// 接口方法运行时 IncompatibleClassChangeError）。
struct ScanInput
{
    knowledge::Era era = knowledge::Era::Unknown;
    std::unordered_set<std::string> jdkInterfaces;   ///< JDK 接口内部名全集（穷举旁证）
};

/// @brief 站点形态（v2/v3 只有方法调用两态，字段/构造器是 Kasumi 面）。
enum class Form : ca::u8
{
    Static = 0,   ///< isVirtual == 0 → INVOKESTATIC
    Virtual,      ///< isVirtual == 1 → INVOKEVIRTUAL / INVOKEINTERFACE（旁证分派）
};

/// @brief 一个可还原站点。`indySrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     indySrcPc   = 0;   ///< indy 旧布局 pc（InsnId 不跨会话）
    Form        form        = Form::Static;
    std::string targetOwner;         ///< 解密结果（slash 内部名）
    std::string targetName;
    std::string targetDesc;
    bool        ownerIsInterface = false;   ///< virtual 落点分派
};

/// @brief 拒绝（fail-closed 计数：拒绝原因进 trace，站点逐位不动）。
struct Reject
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     indySrcPc   = 0;
    std::string why;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize totalIndy          = 0;   ///< 命中 Grunt simple/heavy BSM 指纹的调用点
    ca::usize keyMissing         = 0;   ///< BSM→解密器→key 链三路全断（含壳死无亲）
    ca::usize malformedArguments = 0;   ///< 静态参数个数/种类不符
    ca::usize decodeFailed       = 0;   ///< 解密失败 / 产物空 / isVirtual 越界
    ca::usize descMismatch       = 0;   ///< S1 交叉校验失败
    ca::usize magicMismatch      = 0;   ///< heavy：d1 全扫无 `data1 % 59557 == magic2` 命中
    ca::usize metadataMissing    = 0;   ///< heavy：owner 类无 mv/d1/d2 形注解（或 Era 未判）
    ca::usize materialLane       = 0;   ///< v3 材料通道解密器（运行期材料字段，静态不可解）
    ca::usize interfaceUnattested = 0;  ///< virtual 目标接口无旁证（fail-closed）
    ca::usize classesSkipped     = 0;   ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/帧链/编码拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数

    ca::usize rewrittenStatic  = 0;
    ca::usize rewrittenVirtual = 0;
};

/// @brief 验收结果。residual = 改写后容器里仍带 Grunt simple/heavy BSM 指纹
///        的 indy 调用点数（还原不彻底的残留，语料单 jar 检查要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。键链三路 + 参数解密 + 判据分级，产出可还原站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 indySrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::indy_restore
