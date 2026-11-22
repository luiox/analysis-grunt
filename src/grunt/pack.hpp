/// @file        pack.hpp
/// @brief       本仓唯一 pack 实例访问点（静态链接期入口；CLI 与测试共用）。
#pragma once

#include <string>

#include "mjbase/plugin/pack.hpp"

namespace grunt {

mjbase::plugin::IPassPack& grunt_pack();

/// @brief 默认主线程序（`--converge` 且未给 `-p` 时的每轮 pass 排序）。
///
/// 通用主线 = README 15-pass 主线扩上 v3 供给链面（DispatcherRestore /
/// IndyRestore / MaterialKeyRestore / ExcBridgeRestore / GateProcessorFold /
/// CffUnflatten）与折叠尾轮（ArithFold/ConstantCall 二次轮）；全家族
/// fail-closed 判家门控，非本家样本逐 pass no-op。同轮排序只保效率，跨轮
/// 约束（DC 后补折叠轮、IndyRestore 第二轮）由驱动器轮维度吸收——链序
/// 口径见 doc/obf-behaviors.rst §驱动器契约。
std::string defaultMainLine();

}   // namespace grunt
