/// @file        sim_probe.hpp
/// @brief       CLI 探针：对容器里某个真方法跑一遍 libjsim。
///
/// 这是桥（jsim_bridge）的唯一验证入口——不接探针就没法证明「真 CodeUnit
/// 转出来的方法」和手搭字节码行为一致。探针只读：不改 DOM、不 lower、不写回。
#pragma once

#include <string>

namespace grunt::cli {

/// @brief 跑一个方法并打印结果。
///
/// @param jarPath   输入 jar
/// @param target    `类uri#方法名`，方法名可按描述符后缀精确化：`foo(II)V`
/// @param argsSpec  逗号分隔的实参：十进制整数 / `s:文本` / `u:`（Unknown）
/// @return 进程退出码（0 成功执行完，1 失败）
int runSimProbe(const std::string& jarPath, const std::string& target,
                const std::string& argsSpec);

}   // namespace grunt::cli
