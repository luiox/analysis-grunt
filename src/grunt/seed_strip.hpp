/// @file        seed_strip.hpp
/// @brief       残留 `seed_strip`：`<clinit>` 里的 int[] 种子建表拆除。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把跨类常量藏进静态 int[]，在 `<clinit>` 里建表：
///
///     static {
///         int n = (int) 422104587L;
///         yA.bfAT[0] = n; yA.bfAT[1] = ...;   // 或 MBA 算术
///     }
///
/// 建表段形状固定（grunt 生成器形态）：头 = `NEWARRAY + PUTSTATIC F`，随后
/// 任意多个 `[GETSTATIC F, 值链, IASTORE]` 存储组，组内栈深平衡、以 IASTORE
/// 归零收尾（S2 结构事实）。
///
/// ## 判据（scan 的细找）
///
///   1. 字段是本类声明的 `static [I`；
///   2. 访问面普查：该字段的**全部写**在本类 `<clinit>`，**全部读**也在本类
///      `<clinit>`（读面必须整体参与建表组；类外/类内其他方法的任何访问即放
///      弃，读面未被上游 array_fold 折光时删建表会破坏语义）；
///   3. 建表段按上述全有或全无形状扫描成功，且每组值链都在词汇表内（常量
///      装载 / 栈整形 / 整长算术与转换；除法族可能抛算术异常，排除；局部
///      变量读写不进词汇表，跨度内写、跨度外读会破语义）；中段有标签放弃。
///
/// ## 改写（rewrite）
///
/// 全部跨度擦除（首条 NOP 保标签锚、保 srcPc，其余 eraseBatch）。擦除后
/// **必剪 SMT 孤儿帧**再 lower（v3 sB? verifyFail 回归的根因修复：帧锚在
/// 被擦指令上时 offset 遗留旧布局域，落到别的指令内部）。
///
/// ## 统计
///
/// DirtySet.fields 计数拆除的种子字段；clinit 拆空后由 dead_member 按
/// 形状事实删除整方法。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::seed_strip {

/// @brief 一个可拆除的种子字段。
struct Site
{
    std::string uri;
    std::string fieldName;   ///< 本类声明的 static [I 字段名
    ca::usize   spanCount = 0;   ///< scan 时的建表跨度数
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;

    bool censusIncomplete = false;   ///< 访问面普查不完整（fail-closed 整轮放弃）
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize fields     = 0;   ///< 拆除的种子字段数
    ca::usize attempted  = 0;
    ca::usize failed     = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出可拆除字段（含容器级访问面普查）。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。按 uri+字段名复核访问面与建表形状后拆除。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::seed_strip
