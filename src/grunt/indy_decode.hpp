/// @file        indy_decode.hpp
/// @brief       残留 `indy_decode`：Kasumi indy 调用点静态还原（key 提取 +
///              bsmArgs 解密 + 7 形态改写）。
///
/// ## 形态（artifact 侧）
///
/// Kasumi 把 static/virtual 方法调用、字段读写、构造器全部改写成
/// `invokedynamic`，由每类自带的 bootstrap（8 参签名，见 knowledge 表）在
/// 运行时解析：bsmArgs 里的 encOwner/encName/encDesc 经本类一个
/// `(String;)String` 的 XOR 解密器（每类独立 int key，`char ^ key` 后 `i2c`）
/// 解密，再按 mode/extra 分派到 `findStatic/findVirtual/findGetter/findSetter/
/// findStaticGetter/findStaticSetter/findConstructor`：
///
///     invokedynamic [mode, encOwner, encName, encDesc, extra]
///     ↓ bootstrap（每类一个，随机名）
///     key = K1 ^ K2                    ← 解密器体内编译期常量
///     owner/name/desc = 解密(bsmArgs[1..3])   // owner 点分（java.lang.Math）
///     按 (mode, extra) 选 find* 出句柄 → ConstantCallSite
///
/// 结果是目标信息全部在场、但全部异或藏形，key 在解密器字节码里，也是
/// 编译期事实，所以可以整链静态复现。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：BSM 描述符（8 参指纹，`kKasumiBootstrapDesc`）+
///   解密器形状（`charAt; LDC K1; LDC K2; IXOR; IXOR; i2c` 双层，或
///   `LDC K; IXOR` 单层，定位 `i2c` 回溯两层 IXOR 取常量异或）；
/// - **S3 实现选择**：mode/extra 数值表（`knowledge::kasumiIndyForm`，换代
///   需复核，Era 分支唯一合法落点，`Era::Kasumi` 未判定整站拒绝）；
/// - **S1 语义后果**：indy desc 与解密结果交叉校验（static 全等；virtual
///   首参 receiver + 其余逐位吻合；字段 4 路按 `()T / (T)V /
///   (Object)T / (Object,T)V` 形状核对；ctor 返回 `Lowner;` 且参数段与
///   indy desc 一致），描述符核对不成立即拒绝，不猜。
///
/// ## 改写（插入式站点的锚保留与槽位面）
///
/// 7 形态分两类：
///
/// - **纯原地**（static / getstatic / putstatic）：换 op + 换 CP 操作数，
///   `srcPc` 保留，锚在 indy 上的调试行/标签照常重定位；
/// - **插入式**（virtual / getfield / putfield / ctor）：在 indy 之前插入
///   spill/reload（或 `NEW;DUP`）序列，**indy 本体仍然原地换指令**。
///
/// grunt 全局不变量插入会把 srcPc 清成 kNoPc在这里的解法：
///
/// 1. 被删指令不存在，本残留只插不改位，全方法没有失去落点的旧 pc，
///   `lowerMethod` 的 pcmap 重定位不会 `RelocationBadTarget`；
/// 2. 锚保留靠indy 原地换指令：锚在 indy 旧 pc 上的 LNT 行/标签
///   随 indy 一起迁到新布局，栈帧由帧链整体重算（`mutateMethodWithFrames`）；
/// 3. 槽位面显式设计：spill 槽从 `cu->maxLocals` 起排（receiver/value +
///   实参按 `slotWidth` 累计，cat2 双槽），`cu->maxLocals` 抬到新水位
///   （超 u16 整站放弃）；帧链写回时 maxes 再生，插入块内部无标签、无
///   分支点，不产生新合并点。
///
/// 改写前置检查与折叠系同源：未改会话先往返（`roundTripSafe`），走不完的
/// 方法整方法跳过；接口落点靠旁证（容器类 access 位 + CLI 注入的 JDK
/// 接口集），两处都没有的 virtual 目标 fail-closed 拒绝（invokevirtual 调
/// 接口方法运行时 `IncompatibleClassChangeError`）。
///
/// ## 统计
///
/// DirtySet 按 7 形态计数 + failed/skippedByRoundTrip；ProofResult 在
/// 类数检查之外统计 residual（改写后容器里仍带 8 参指纹的 indy 调用点数）。
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/era.hpp"
#include "grunt/knowledge/kasumi_names.hpp"

namespace grunt::indy_decode {

/// @brief SCAN 注入面：Era 判定 + JDK 接口旁证。
///
/// 文件系统副作用收敛在 scan 之前（adapter/CLI）：残留本体不做任何文件系统
/// 遍历。`jdkInterfaces` 由 CLI `--jdk` 扫描注入（adapter 在
/// `Era::Kasumi` 时才激活）。virtual 落点分派：容器普查优先（权威）；JDK
/// 集是穷举普查，**成员与否双向作证**（在集 = 接口；不在集 = 类）；**空集 =
/// 无任何旁证**，virtual 目标不在容器时整站 fail-closed 拒绝。
struct ScanInput
{
    knowledge::Era era = knowledge::Era::Unknown;
    std::unordered_set<std::string> jdkInterfaces;   ///< JDK 接口内部名全集（穷举旁证）
};

/// @brief 一个可还原站点。`indySrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string                      uri;
    ca::usize                        methodIndex = 0;
    ca::u32                          indySrcPc   = 0;   ///< indy 旧布局 pc（InsnId 不跨会话）
    knowledge::KasumiIndyForm        form = knowledge::KasumiIndyForm::Static;
    std::string                      targetOwner;   ///< 解密结果（slash 内部名）
    std::string                      targetName;    ///< 方法/字段名（ctor 恒 `<init>`）
    std::string                      targetDesc;    ///< 方法 desc / 字段类型 / ctor init desc
    bool                             ownerIsInterface = false;  ///< virtual 落点分派
};

/// @brief 拒绝（fail-closed 计数：拒绝原因进 trace，站点不动）。
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

    ca::usize totalIndy          = 0;   ///< 命中 8 参 BSM 指纹的调用点
    ca::usize eraBlocked         = 0;   ///< Era 未判定（fail-closed 全拒）
    ca::usize keyMissing         = 0;   ///< BSM→解密器→key 链断
    ca::usize malformedArguments = 0;   ///< bsmArgs 个数/种类不符
    ca::usize decodeFailed       = 0;   ///< 解密失败 / 产物空 / 7 形态表越界
    ca::usize descMismatch       = 0;   ///< S1 交叉校验失败
    ca::usize interfaceUnattested = 0;  ///< virtual 目标接口无旁证（fail-closed）
    ca::usize classesSkipped     = 0;   ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计（7 形态分列，参考实现 RewriteStats 口径）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/帧链/编码拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数

    ca::usize rewrittenStatic    = 0;
    ca::usize rewrittenVirtual   = 0;
    ca::usize rewrittenGetField  = 0;
    ca::usize rewrittenPutField  = 0;
    ca::usize rewrittenGetStatic = 0;
    ca::usize rewrittenPutStatic = 0;
    ca::usize rewrittenNew       = 0;
};

/// @brief 验收结果。residual = 改写后容器里仍带 8 参指纹的 indy 调用点
///        （还原不彻底的残留，语料单 jar 检查要求 0）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。key 提取 + bsmArgs 解密 + 判据分级，产出可还原站点。
ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input);

/// @brief REWRITE：纯写。只消费 ScanResult，按 indySrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::indy_decode
