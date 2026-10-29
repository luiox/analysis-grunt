/// @file        booger_names.hpp
/// @brief       Grunt 家族知识：@Booger 运行时注解的名字形态。
///
/// CONVENTION §2：家族知识唯一落点。注解类名随机化，但注解描述符与参数键
/// 固定（d2 = 密钥表、mv = 轮数），是字符串解密的运行时锚点；类体全部走
/// 这个注解索引，剥掉后无消费方。
#pragma once

#include <string_view>

namespace grunt::knowledge {

/// @brief @Booger 注解的类型描述符（注解类内部名 = 去掉 L…; 外壳）。
inline constexpr std::string_view kBoogerAnnoDesc = "Ldev/realme/Booger;";
inline constexpr std::string_view kBoogerClassName = "dev/realme/Booger";

}   // namespace grunt::knowledge
