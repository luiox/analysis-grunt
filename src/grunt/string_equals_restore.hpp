/// @file        string_equals_restore.hpp
/// @brief       残留 `string_equals_restore`：v2 StringEqualsRedirect / v3
///              StringEqualsOptimize 的 hashCode 比较换回 equals（缺口 #11）。
///
/// ## 形态（artifact 侧）
///
/// 两代生成源输出的替换序列逐字相同。equals 面（6 条）：
///
///     INVOKEVIRTUAL java/lang/Object.hashCode ()I
///     INVOKESTATIC  java/lang/Integer.valueOf (I)Ljava/lang/Integer;
///     SWAP
///     INVOKEVIRTUAL java/lang/String.hashCode ()I
///     INVOKESTATIC  java/lang/Integer.valueOf (I)Ljava/lang/Integer;
///     INVOKEVIRTUAL java/lang/Integer.equals (Ljava/lang/Object;)Z
///                                                 → INVOKEVIRTUAL String.equals
///
/// equalsIgnoreCase 面（8 条）：两端各前置一次
/// `INVOKEVIRTUAL java/lang/String.toUpperCase ()Ljava/lang/String;`，
/// 第二端 hashCode 走 String.hashCode → INVOKEVIRTUAL String.equalsIgnoreCase。
///
/// 操作数（receiver 与比较串）在窗口下方，通配。窗口内 5/7 个成员引用
/// 逐字钉死（Integer.valueOf/equals、Object.hashCode、String.hashCode、
/// toUpperCase），S2 形状钉。
///
/// ## 语义口径
///
/// hashCode 比较对碰撞串会误判相等（生成侧自认的损失面），换回 equals 是
/// 生成面的逆映射（还原到原程序语义）。接收端/实参的验证器类型由窗口内
/// 的 String.hashCode / String.toUpperCase 调用自证（原代码在同一栈位上调
/// 过 String 成员，恢复 INVOKEVIRTUAL String 系成员验证器照过）。
///
/// ## 判据（scan 的细找）
///
///   1. 窗口逐条 op + 成员引用精确吻合（NOP 夹层跳过）；
///   2. 窗口内除锚点外无标签（合并点栈形耦合，放弃）。
///
/// ## 改写（rewrite）
///
/// 按 `srcPc` 重定位锚点后全量复核（同方法前站改写后不盲改），锚点原地换
/// 目标 invoke（setMember，保 srcPc），窗口其余非 NOP 指令擦除
/// （eraseBatch）。帧链走 mutateMethodWithFrames 正统写回，前置
/// roundTripSafe。
///
/// ## 已知边界
///
/// - 用户代码恰好含同序列时同样还原：序列语义与 equals 等价面只在哈希
///   碰撞串上分叉，还原方向是取严格语义，如实登记。
/// - 生成侧未来若换比较载体（Long.compare 之类），形状表不认，fail-closed。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "grunt/iter_budget.hpp"

namespace grunt::string_equals_restore {

inline constexpr ca::usize kScanClassBudget    = 1'000'000;
inline constexpr ca::usize kRewriteClassBudget = 4'000'000;

/// @brief 站点变体。
enum class Variant : ca::u8
{
    Equals         = 0,   ///< 6 条窗口 → String.equals
    EqualsIgnore   = 1,   ///< 8 条窗口 → String.equalsIgnoreCase
};

/// @brief 一个调用点站点。
struct Site
{
    ca::u32       classIdx    = 0;
    ca::usize     methodIndex = 0;
    ca::u32       anchorSrcPc = 0;
    Variant       variant     = Variant::Equals;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize attempted          = 0;
    ca::usize failed             = 0;
    ca::usize skippedByRoundTrip = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。
ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budget
                = iter_budget::ClassBudget{kScanClassBudget});

/// @brief REWRITE：纯写。只消费 ScanResult。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budget
                 = iter_budget::ClassBudget{kRewriteClassBudget});

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::string_equals_restore
