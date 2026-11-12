/// @file        booger_anno_strip.hpp
/// @brief       残留 `booger_anno_strip`：`@Booger` 运行时注解剥离。
///
/// ## 形态（artifact 侧）
///
/// 混淆器在被保护类上挂运行时注解：
///
///     @Booger(d2 = {...}, mv = 100)
///     public class Fx { ... }
///
/// 注解**类名随机**，但描述符与参数键固定（d2 = 密钥表、mv = 轮数），它
/// 是字符串解密的运行时锚点（S1：decrypt 体读这个注解拿参数）。当解密体
/// 已被常量折叠清空、注解类无任何 CP 引用方后，注解条目本身就是死数据。
///
/// ## 判据（scan 的细找）
///
///   1. 注解类型描述符 == `Ldev/realme/Booger;`（家族知识 booger_names）；
///   2. 容器检查：全容器 CP 无 `dev/realme/Booger` 的 Class 引用（解密体若
///      还活着必有引用；注解类自身 thisClass 自引不算）。有引用即整站放弃
///      ，剥注解会让还活着的解密体拿不到参数。
///
/// ## 改写（rewrite）
///
/// 类/方法/字段属性表里的 RuntimeVisibleAnnotations /
/// RuntimeInvisibleAnnotations 槽内剔除该注解条目；表空则整槽删除；落盘后
/// 压缩一次带走死 Utf8。
///
/// ## 统计
///
/// DirtySet.annos 计数剔除的注解条目数、classes 计数动过的类。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::booger_anno_strip {

/// @brief 一个含 `@Booger` 注解的类。
struct Site
{
    std::string uri;
    ca::usize   annoCount = 0;   ///< scan 时的注解条目数
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;
    bool              gateBlocked    = false;   ///< 容器仍有 Booger CP 引用，或引用普查不完整
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize annos    = 0;
    ca::usize classes  = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。容器检查 + 注解条目普查。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。逐属性表剔除注解条目，落盘后压缩。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::booger_anno_strip
