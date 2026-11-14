/// @file        junk_region_strip.hpp
/// @brief       残留 `junk_region_strip`：不可达死区垃圾码原位 NOP 化。
///
/// ## 形态（artifact 侧）
///
/// DecompilerCrasher 家族往方法体里撒两类反工具载荷（账本缺口 #13）。形态
/// 判据来自家族生成面与线上样本实证；fork 全链样本（样本 C）经字节级可达性
/// 走查实测 **无** 此残留形态（一次普查脚本把单字节操作码按 2 字节步进，其
/// "死区方法数" 是普查口径伪影，见账本 §3 方法注记），本 pass 面向家族其他
/// 样本与上游链产出的死区延续段：
///   1. 不可达死区的非法操作码（`0xff` impdep2 族）：JVM 校验器只沿可达帧
///      走查，加载零失败；javap/CFR/ASM 一类线性走查工具在读到 0xff 的瞬间
///      整体拒载——CFR 因此完全不可用；
///   2. 死区夹杂在真码之间，从任何分支目标/异常 handler 都进不去。
///
/// ## 判据（scan 的细找）
///
/// 字节级可达性走查（JVM 校验器同款语义）：worklist = {0} ∪ 全部异常表
/// handler pc，沿操作码精确长度表（含 switch 变长、wide、multianewarray）
/// 展开，分支全部目标入队。可达集合之外的字节段即死区。
///
/// 保守面（方法整条 fail-closed，字节不动）：
///   - 可达 pc 上出现表外操作码（JVM 可加载就不该发生，发生了说明走查口径
///     有洞，不猜）；
///   - `jsr`/`ret`/`jsr_w`（动态返回地址，字节级走查跟不了，major ≥ 51 也
///     不该有）；
///   - 任何越界目标 / 越界 handler / 空码。
///
/// ## 改写
///
/// 死区字节原位写 0x00（NOP）：**长度保持**——code_length、全部指令偏移、
/// StackMapTable 帧、LVT/LNT 行位全部不动，无帧重算面。可达指令一个字节不
/// 碰。改写只发生在死区字节存在的方法上；净类字节不变。
///
/// ## 统计
///
/// DirtySet：动过的类 uri、NOP 化的方法数与字节数。ScanResult 另报
/// fail-closed 方法数（保守跳过，宁留不猜）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::junk_region_strip {

/// @brief 三段之间唯一的数据流：方法定位用（uri, methodIndex, codeSlot），
///        死区字节段在 scan 定死，rewrite 只按段写 0x00、不回看判据。
struct JunkSite
{
    std::string              uri;
    ca::usize                methodIndex = 0;
    ca::usize                codeSlot    = 0;   ///< 方法 attrs 里 Code 槽的下标
    std::vector<std::pair<ca::u32, ca::u32>> ranges;  ///< [start, end) 死区段
};

struct ScanResult
{
    std::vector<JunkSite> sites;
    ca::usize             junkBytes        = 0;  ///< 死区字节总数
    ca::usize             methodsFailClosed = 0; ///< 可达路径解不开的方法（保守跳过）
    ca::usize             methodsDecodeFail = 0; ///< Code 属性解码失败的方法（保守跳过）
    ca::usize             classesSkipped   = 0;  ///< 解析失败的类
};

struct DirtySet
{
    std::vector<std::string> uris;
    ca::usize                methods  = 0;  ///< NOP 化的方法数
    ca::usize                nopBytes = 0;
    ca::usize                sitesOutOfRange = 0;  ///< 索引越界（字节不同源）的防御计数
};

struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。逐方法字节级可达性走查，产出死区段站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。按 scan 的死区段原位写 0x00，不回看判据。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::junk_region_strip
