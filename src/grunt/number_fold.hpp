/// @file        number_fold.hpp
/// @brief       残留 `number_fold`：藏形数值常量还原（XOR 隐匿跨度 + 基 32
///              长整跨度 + 位形包装调用）。
///
/// ## 形态（artifact 侧）
///
/// Grunt 把字面数值藏进等价运算跨度，字节码里看不到立即数。跨度由纯常量
/// 装载与封闭运算构成，求值结果是静态事实：
///
///     int 跨度 1：  <i32 a> I2L <i32 b> I2L LXOR L2I   → a ^ b
///     int 跨度 2：  <i64 a> L2I <i32 b> IXOR           → (int)a ^ b
///     long 跨度：   LDC "key" <i32 32>
///                   INVOKESTATIC Long.parseUnsignedLong(String,I)J
///                   <i64 obf> LXOR                     → key ^ obf
///     float 包装：  int 跨度 + INVOKESTATIC
///                   Float.intBitsToFloat(I)F           → 位形 f32
///     double 包装： long 跨度 + INVOKESTATIC
///                   Double.longBitsToDouble(J)D        → 位形 f64
///
/// 跨度运算封闭、无栈外输入，包装调用的 JDK 语义确定（位形换算）——判据
/// 钉的是「跨度求值即常量」这一结构事实（S2）：混淆器无法在保持功能的前提
/// 下改变跨度产出的值。
///
/// ## 判据（scan 的细找）
///
///   1. 跨度逐条吻合（允许 NOP 夹层——squeeze 遗留落点不驻栈）；
///   2. 操作数全部由纯常量装载产生（`const_args` 工具箱；字符串键只认基 32
///      数字串，radix 恒 32，超 64 位拒绝）；
///   3. 包装调用的 owner/name/desc 精确吻合，且实参类型与跨度产出吻合；
///   4. 跨度内除锚点外无标签（合并点 = 外部到达路径与跨度产出耦合，放弃）。
///
/// ## 改写（rewrite）
///
/// 锚点（跨度首条装载）原地改成结果常量装载（保 srcPc），跨度其余非 NOP
/// 指令擦除（eraseBatch kRejectIfUsed：受害者被异常表引用即整站放弃）。
/// 不新增合并点、不插跳转；帧链走 mutateMethodWithFrames 正统写回。改写不
/// 在本轮展开嵌套跨度（rewrite 只消费 ScanResult），整链重复跑收敛。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/const_args.hpp"

namespace grunt::number_fold {

/// @brief 一个可折叠站点。`anchorSrcPc`/`anchorOp` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string           uri;
    ca::usize             methodIndex = 0;
    ca::u32               anchorSrcPc = 0;
    jbc::core::Op         anchorOp    = jbc::core::Op::NOP;
    const_args::ValueLoad value;   ///< 折叠结果（I32 / I64 / F32 / F64 位形）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;   ///< 会话开不了的类（fail-closed 跳过）
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites             = 0;   ///< 折叠成功的站点数
    ca::usize attempted         = 0;   ///< 进入 rewrite 的站点数
    ca::usize failed            = 0;   ///< 复核/标签门/擦除/帧链拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出可折叠站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 anchorSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::number_fold
