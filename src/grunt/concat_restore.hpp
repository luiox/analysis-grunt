/// @file        concat_restore.hpp
/// @brief       残留 `concat_restore`：Grunt v2/v3 字符串拼接 BSM 包装收口
///              （拼接 indy 的 bootstrap 还原成 JDK 标准单形）。
///
/// ## 形态（jsoup v3_full 28 类实证；v2 同族转发形）
///
/// 混淆器把 `makeConcatWithConstants` 的 recipe 拆成两半：indy 站点的
/// bootstrap 静态参数只留 tag 串（`U+0001`=实参位、`U+0002`=常量位），
/// 常量段搬进 BSM 体里的 `Object[]`，BSM 本体是一个纯转发包装：
///
///     CallSite <wrapper>(Lookup l, String name, MethodType t, String tags) {
///         return StringConcatFactory.makeConcatWithConstants(
///             l, name, t, tags, new Object[]{". Status=", ", URL=[", "]"});
///     }
///
/// CFR 0.152 对这个形态析不出 `+` 号（站点 recipe 是纯 tag 串，常量藏在
/// 方法体里，跨 CP 两半拼不回来），restored 面 `Exception`/残差全部堆积。
///
/// ## 还原（语义等价改写，无 Era 门）
///
/// 结构自证三件套全过才改：BSM 句柄 → 本类 private static 包装方法；
/// 包装体逐字节形状（aload×4 → [iconst_N; anewarray Object; N×(dup;
/// iconst_i; ldc String; aastore)] → invokestatic
/// StringConcatFactory.makeConcatWithConstants 五参形 → areturn）；
/// recipe 只含 tag 字节且 `U+0002` 数 == 常量数。改写 = 把 `U+0002` 位用
/// BSM 体常量按序回填成完整 recipe，句柄改指 JDK 标准
/// `StringConcatFactory.makeConcatWithConstants(Lookup,String,MethodType,
/// String)CallSite`，静态参数表换成 [完整 recipe]——站点零改动（站点只
/// 引 BSM 序号）。语义等价依据：makeConcatWithConstants 对 `U+0002` 的
/// 解析就是按序吃 varargs，包装转发与标准形产出同一 CallSite。
///
/// 包装方法还原后零引用，DeadMember 引用普查收走；体里的常量串由压池
/// 面带走。形状不符 / recipe 与常量数不齐 → fail-closed 计数不动。
///
/// ## 统计
///
/// DirtySet 按 entries/classes 分列 + failed；ProofResult 在类数检查之外
/// 统计 residual（还原后仍是包装形的 BSM 条目数，语料单 jar 检查要求 0）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::concat_restore {

/// @brief 一个可还原的包装 BSM 条目。
struct Site
{
    std::string uri;
    ca::usize   entryIndex   = 0;   ///< BootstrapMethods 条目序号
    std::string mergedRecipe;          ///< U+0002 回填后的完整 recipe（运行时拼 UTF-8 字节）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;

    ca::usize totalBsmEntries   = 0;   ///< 全容器 BootstrapMethods 条目数（盘点口径）
    ca::usize wrapperShapeBad   = 0;   ///< 包装体形状不符（非转发形/读不开）
    ca::usize recipeMismatch    = 0;   ///< recipe 非 tag 串 / U+0002 数与常量数不齐
    ca::usize classesSkipped    = 0;   ///< 会话开不了的类
};

/// @brief 被改写的类 + 条目级统计。
struct DirtySet
{
    std::vector<std::string> uris;
    ca::usize                entries = 0;
    ca::usize                classes = 0;
    ca::usize                failed  = 0;   ///< 复核/编码拦下的条目数
};

/// @brief 验收结果。residual = 还原后容器里仍是包装形的 BSM 条目数。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。逐类逐条目形状判定 + recipe 回填预演。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，条目复核后改句柄与参数表。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::concat_restore
