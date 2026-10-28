/// @file        knowledge/era.hpp
/// @brief       Grunt 家族知识：版本（Era）与家族常量。
///
/// CONVENTION §6.3：版本分支只允许出现在 scan 的细找里，且必须是 S3 级判据。
/// 本文件是**唯一**允许出现版本相关字面量的地方。
#pragma once

#include "libca/core/datatype.hpp"

namespace grunt::knowledge {

/// @brief 家族版本。
enum class Era : ca::u8 {
    Unknown = 0,  ///< 判据不足：fail-closed，不做猜测性改写
    Grunt2,       ///< Gruntpocalypse 2.x
    Grunt3,       ///< Grunteon
};

/// @brief 版本探测（每家族仓固定提供这一个函数）。
///
/// 当前**没有可靠判据**，恒返回 Unknown，理由如下：
///
/// - 类/成员名的 `U+200E` 尾缀在两代里都存在（Grunt2 的
///   `ClassRenameTransformer::suffix` 与 Grunt3 的 `FieldRenamer::suffix`
///   同源），不能当版本判据；
/// - 调用点模板、数字混淆形状在两代之间没有观测到稳定差异。
///
/// `constant_call` 不消费 Era：它的判据是语义事实（纯函数 + 常量实参 +
/// 求值成功），与混淆器版本无关。等第一个真正版本敏感的残留出现时，再把
/// 探测规则补在这里，而不是散进各残留。
inline Era probeEra() noexcept
{
    return Era::Unknown;
}

}   // namespace grunt::knowledge
