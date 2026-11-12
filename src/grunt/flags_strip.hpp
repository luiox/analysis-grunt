/// @file        flags_strip.hpp
/// @brief       残留 `flags_strip`：清除伪造的 `ACC_SYNTHETIC` / `ACC_BRIDGE`
///              访问标志。
///
/// ## 形态（artifact 侧）
///
/// 混淆器给海量成员打上编译器专用标志：static 方法带 ACC_BRIDGE（javac 只为
/// 实例方法生成桥接器，static 桥接不存在）、静态字段带 ACC_SYNTHETIC。
/// 反编译器把这些渲染成 `/* bridge */ /* synthetic */` 注释噪音，反射视图
/// （isSynthetic/isBridge）随之失真。实测样本规模：约 3.8 万方法 + 36 万字段。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实，边界收在编译器不可能生成的交集上：
///   1. 类不是 enum（ACC_ENUM == 0：`$VALUES` 等枚举机制字段是真实的
///      static synthetic，不能碰）；
///   2. 成员是 static，且名字不以 `$` 开头，各工具链的编译器合成物一律
///      带 `$`（`access$000`、`$SWITCH_TABLE$`、`$assertionsDisabled`），
///      混淆器注入的伪标志成员是改名后的普通名字，按名字形状可分；
///   3. static 方法带 ACC_BRIDGE 或 ACC_SYNTHETIC → 两标志一起清；
///   4. static 字段带 ACC_SYNTHETIC → 清。
///
/// 保守面（不动）：实例方法的 ACC_BRIDGE（可能是真实泛型桥接器）、实例成员
/// 的 ACC_SYNTHETIC（`this$0` 等）、类级 ACC_SYNTHETIC、一切 `$` 名字成员。
/// 语义边界：标志只影响反射视图，不影响指令执行语义，isSynthetic 返回值的
/// 变化是本残留的目的，不是副作用。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::flags_strip {

/// @brief 一个待清除站点：成员用 backing 索引定位（同一份输入字节下稳定），
///        `clearMask` 在 scan 定死，rewrite 只按掩码清位、不回看判据。
struct FlagSite
{
    std::string uri;
    bool        isMethod  = true;
    ca::usize   index     = 0;
    ca::u16     clearMask = 0;  ///< ACC_BRIDGE | ACC_SYNTHETIC 的子集
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<FlagSite> sites;
    ca::usize             classesSkipped = 0;  ///< 解析失败 / 版本或枚举检查外被跳过的类
};

/// @brief 被改写的类与成员级统计。
struct DirtySet
{
    std::vector<std::string> uris;
    ca::usize                membersCleared = 0;
    ca::usize                sitesOutOfRange = 0;  ///< 索引越界（字节不同源）的防御计数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出待清标志站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只按 ScanResult 的掩码清位，不重新判据。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::flags_strip
