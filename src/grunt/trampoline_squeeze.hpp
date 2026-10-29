/// @file        trampoline_squeeze.hpp
/// @brief       残留 `trampoline_squeeze`：消解「跳到下一条」的冗余无条件跳转。
///
/// ## 形态（artifact 侧）
///
/// 混淆器做基本块乱序后，用无条件跳转把逻辑顺序缝回来。缝线落在块尾时，
/// 目标恰好是布局上的下一条指令——这条 goto 在语义上恒等于 NOP，却有两层
/// 实害：
///   1. 字节噪音：每条 3~5 字节，样本里数以千计；
///   2. 挡路：它夹在常量实参区/操作数区中段时，「紧邻纯常量装载」的区域
///      判定整站放弃（常量调用、常量谓词都被它挡住）。
///
/// ## 判据（scan 的细找）
///
/// S1 语义事实：`goto/goto_w` 的目标解析后**恰好是布局顺序后继**（同方法
/// 内、下一条指令的句柄）。跳转条件恒真且后继不变，NOP 化是恒等变换。
///
/// 保守面（不动）：目标在方法尾之外（isEnd）、目标解析失败、自身带标签
/// （标签锚定在蹦床上时，NOP 化后标签仍锚在 NOP 上，合法——所以标签不
/// 拒收；这是与擦除类改写的本质区别：本残留只原地置 NOP，不动布局之外的
/// 任何锚）。
///
/// ## 改写
///
/// 原地置 NOP（保 srcPc）。布局因此缩短（goto 3/5 字节 → nop 1 字节），
/// 方法的 LNT/LVT/SMT 帧重定位交由 `lowerMethod` 的 pcmap 精确重定向；
/// lower 失败整类放弃（fail-closed）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::trampoline_squeeze {

/// @brief 一个蹦床站点：方法用 backing 前的方法下标定位，`srcPc` 跨会话稳定。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     srcPc       = 0;
    bool        wide        = false;  ///< true = goto_w（判据复核用）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;
};

/// @brief 被改写的类与统计。
struct DirtySet
{
    std::vector<std::string> uris;
    ca::usize                squeezed  = 0;
    ca::usize                attempted = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出全部「目标即顺序后继」的蹦床站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。逐站点原地置 NOP；lower 失败整类放弃。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::trampoline_squeeze
