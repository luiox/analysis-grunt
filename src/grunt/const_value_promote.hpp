/// @file        const_value_promote.hpp
/// @brief       残留 `const_value_promote`：clinit 单点常量写入回填
///              ConstantValue 属性。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把 `static final` 常量的字面量抹掉，改成 `<clinit>` 里 putstatic：
///
///     static { aGjy = 89737686; aaKO = 855681668; aGUJ = 424665907; }
///
/// 反编译出来一坨静态块，常量无法内联传播。写入若全容器唯一且是纯常量
/// 装载，这就是 JVM 标准的 ConstantValue 形态（S1：值在类初始化时唯一确
/// 定，与执行顺序无关）。
///
/// ## 判据（scan 的细找）
///
///   1. 字段 static，描述符是基本类型或 Ljava/lang/String;（ConstantValue
///      的合法形态），且尚未带 ConstantValue；
///   2. 访问面普查（promote 精确口径）：全容器该字段只有一次写，落在本类
///      `<clinit>`（读的次数不限）；
///   3. putstatic 紧邻前驱（NOP 落点跳过）就是完整产生该槽位数的常量装载
///      （int 族 / ldc String / ldc2_w long/double；ACONST_NULL 不是合法
///      ConstantValue 形态，拒绝）。
///
/// ## 改写（rewrite）
///
/// 常量装载置 NOP、putstatic 擦除（布局缩短交 lower，孤儿帧先修剪）；
/// 字段补 ACC_FINAL（ConstantValue 只对 static final 生效）并挂
/// ConstantValue 属性（RawAttr 2 字节 CP 索引，libjbc 0.4.15 未建模该属性）。
///
/// ## 统计
///
/// DirtySet.promoted 计数字段数；uri 列表是被改写的类。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "grunt/knowledge/const_args.hpp"
#include "mjbase/model/resource_container.hpp"

namespace grunt::const_value_promote {

/// @brief 一个可回填字段。字段按 uri+name 定位，值随站点携带（复核用）。
struct Site
{
    std::string              uri;
    ca::usize                clinitIndex = 0;
    std::string              fieldName;
    std::string              fieldDesc;
    ca::u32                  putSrcPc = 0;
    const_args::ValueLoad    value;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;
    ca::usize         verifierDead   = 0;   ///< 验证器判死方法数（BadInstructions，普查豁免）

    bool censusIncomplete = false;   ///< 访问面普查不完整（fail-closed 整轮放弃）
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize promoted  = 0;   ///< 回填的字段数
    ca::usize attempted = 0;
    ca::usize failed    = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出可回填字段（含容器级访问面普查）。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。按 uri+字段名+putSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::const_value_promote
