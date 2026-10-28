/// @file        pack.hpp
/// @brief       本仓唯一 pack 实例访问点（静态链接期入口；CLI 与测试共用）。
#pragma once

#include "mjbase/plugin/pack.hpp"

namespace grunt {

mjbase::plugin::IPassPack& grunt_pack();

}   // namespace grunt
