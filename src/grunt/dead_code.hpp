/// @file        dead_code.hpp
/// @brief       残留 `dead_code`：不可达指令码清理 + 冗余无条件跳转穿线。
///
/// ## 形态（artifact 侧）
///
/// predicate_fold 裁决掉假分支后，诱饵块从此再无控制流入口；throw/return
/// 之后的残段、永不命中的 switch 臂同理。这些指令在字节布局里还在，反编译
/// 出来就是用户看到的死块。另有冗余无条件跳转：goto/goto_w 的目标是紧邻
/// 落点指令——混淆器用这对（goto + 落点标签）把常量表达式 / 调用实参区
/// 从中间劈开，NF/ArithFold 的常量折叠与 PAF 三元组都被落点上的真标签
/// 挡死（u/b 族池读 + L 族 junk 谓词实证）；跳转与顺序执行语义全等，
/// 擦除即穿线。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实：从入口做控制流可达性 BFS，顺序后继 + 分支/switch 全目标；
/// return/athrow/ret 无后继。可达集合之外的指令即不可达（fall-through 已计
/// 入，不存在布局上够不到的歧义）。异常表是隐藏控制流边：handler 以
/// 区间与可达集相交为根入 BFS，迭代到不动点，带异常表的方法不再整体
/// 跳过（grunt 方法几乎全带 try/catch，跳过等于永不清死区）。冗余 goto
/// 与死指令同入站点面（collectRedundantGotos，scan/rewrite 共用单源）。
///
/// 保守检查（不是判据，是放弃理由）：方法原样往返不了（roundTripSafe 检查）、
/// 异常表异形或端点/标签处置失败，该方法 fail-closed 放弃，字节不动。
///
/// ## 改写（rewrite）
///
/// 先收敛异常表再 `eraseBatch` 整批擦除（布局缩短交 lower 的 pcmap 重定向）：
/// 全死区间删表项，孤儿端点/handler 标签销毁；端点锚在死指令上时
/// `bindLabel` 把旧 Boundary 标签搬移到吸附目标（句柄身份不变，表项引用
/// 不动），只换新引用不销毁旧标签会被 eraseBatch 的锚定保护整批拒绝。
/// 冗余 goto 独立成批先行穿线；其目标标签锚在顺序后继（存活）上无涉，
/// 锚在 victim 本体上的存活入边标签先搬到落点（rebindThreadingLabels，
/// 句柄身份不变），落点到达性经顺序后继不变。帧优先由 Strict 求解整体
/// 重算；junk 栈形合并点让 Strict 推断失败的方法（b.main 族）退化到
/// 声明帧口径穿线——擦 goto-to-fallthrough 不改剩余边的栈形，声明帧
/// relocation 后仍与代码真值一致，CC 同构路径实证可落地。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::dead_code {

/// @brief 一个含不可达码的方法。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::usize   victimCount = 0;   ///< scan 时的不可达指令数
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize methods   = 0;   ///< 实际动过的方法数
    ca::usize insns     = 0;   ///< 清掉的指令数（擦除或 NOP 化）
    ca::usize attempted = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出含不可达码的方法。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。异常表收敛（吸附/删表项）后 eraseBatch 整批擦除。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::dead_code
