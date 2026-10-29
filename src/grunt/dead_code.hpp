/// @file        dead_code.hpp
/// @brief       残留 `dead_code`：不可达指令码清理。
///
/// ## 形态（artifact 侧）
///
/// predicate_fold 裁决掉假分支后，诱饵块从此再无控制流入口；throw/return
/// 之后的残段、永不命中的 switch 臂同理。这些指令在字节布局里还在，反编译
/// 出来就是用户看到的「死块」。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实：从入口做控制流可达性 BFS——顺序后继 + 分支/switch 全目标；
/// return/athrow/ret 无后继。可达集合之外的指令即不可达（fall-through 已计
/// 入，不存在「布局上够不到」的歧义）。
///
/// 保守门（不是判据，是放弃理由）：
///   - 方法带异常表：handler 是隐藏控制流边，本残留不碰这类方法；
///   - indy 类：擦除缩短布局会甩断 SMT 帧链，受害者只 NOP 化保布局。
///
/// ## 改写（rewrite）
///
/// 非indy 类 `eraseBatch` 整批擦除（布局缩短交 lower 的 pcmap 重定向）；indy
/// 类逐条置 NOP 保 srcPc（帧锚不动，校验器不访问不可达帧）。
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

/// @brief REWRITE：纯写。indy 类 NOP 保布局，其余擦除。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::dead_code
