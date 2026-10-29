/// @file        sim_probe.hpp
/// @brief       CLI 探针：对容器里某个真方法跑一遍 libjsim。
///
/// 这是桥（jsim_bridge）的唯一验证入口——不接探针就没法证明「真 CodeUnit
/// 转出来的方法」和手搭字节码行为一致。探针只读：不改 DOM、不 lower、不写回。
#pragma once

#include <string>

namespace grunt::cli {

/// @brief 探针入参。用结构体而不是位置参数——布尔位一多就没人读得懂调用点。
struct SimProbeOptions
{
    std::string jarPath;              ///< 输入 jar
    std::string target;               ///< `类uri#方法名`，方法名可按描述符后缀精确化
    std::string argsSpec;             ///< 逗号分隔实参：十进制 / `s:` / `x:` / `u:`
    bool        executor = false;     ///< true = 跨方法执行器；false = 单方法裸跑
    bool        dump     = false;     ///< 打印桥产物的反汇编视图（只读）
};

/// @brief 跑一个方法并打印结果。
/// @return 进程退出码（0 成功执行完，1 失败）
int runSimProbe(const SimProbeOptions& options);

}   // namespace grunt::cli
