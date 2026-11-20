/// @file        number_fold.hpp
/// @brief       残留 `number_fold`：藏形数值常量还原（XOR 隐匿跨度 + 基 32
///              长整跨度 + 位形包装调用 + v2 位拼跨度）。
///
/// ## 形态（artifact 侧）
///
/// Grunt 把字面数值藏进等价运算跨度，字节码里看不到立即数。跨度由纯常量
/// 装载与封闭运算构成，求值结果是静态事实：
///
///     int 跨度 1：  <i32 a> I2L <i32 b> I2L LXOR L2I   → a ^ b
///     int 跨度 2：  <i64 a> L2I <i32 b> IXOR           → (int)a ^ b
///     int 位拼 3：  <i32 a> <i32 b> IAND               → a & b
///     long 位拼 4： <i64 a> <i64 b> LAND               → a & b
///     long 跨度：   LDC "key" <i32 32>
///                   INVOKESTATIC Long.parseUnsignedLong(String,I)J
///                   <i64 obf> LXOR                     → key ^ obf
///     float 包装：  int 跨度 + INVOKESTATIC
///                   Float.intBitsToFloat(I)F           → 位形 f32
///     double 包装： long 跨度 + INVOKESTATIC
///                   Double.longBitsToDouble(J)D        → 位形 f64
///
/// 位拼两式是 v2 Gruntpocalypse NumberEncrypt 的产出（Int 第二式 / Long
/// 全式，生成侧 `first & second = value`）；v3 生成侧无此形态，但 v2 语料
/// 与 junk builder 混合同形人群，模板按形状吃、不分来源。位与是纯位运算，
/// 折叠值直接取 a & b 的 JVM 位形逐位复算，不依赖生成侧构造意图。
///
/// ## 判据（scan 的细找）
///
///   1. 跨度逐条吻合（允许 NOP 夹层，squeeze 遗留落点不驻栈）；
///   2. 操作数全部由纯常量装载产生（`const_args` 工具箱；字符串键只认基 32
///      数字串，radix 恒 32，超 64 位拒绝；位拼第二操作数类型钉死，IAND
///      钉 I32、LAND 钉 I64，栈形破裂即拒绝）；
///   3. 包装调用的 owner/name/desc 精确吻合，且实参类型与跨度产出吻合；
///   4. 跨度内除锚点外无标签（合并点 = 外部到达路径与跨度产出耦合，放弃）。
///   5. **通用封闭跨度兜底**（命名形状全拒后接手）：拼装走廊族
///      （`parseUL(key) ^ ((int跨度 i2l mask land 32 lshl) | (…))`，Grunt v3
///      池填充）高/低位两半可交换、goto/NOP 夹层任意（含 goto 跨 junk 死尾
///      nop/athrow 块——死尾不进栈机，改写期随跨度一并 NOP 化），形状表组
///      合爆炸——改钉结构事实（跨度 = 纯常量封闭计算，栈机求值即静态事
///      实）。词汇表 = 常量装载 + 数值转换（F2I/F2L/D2I/D2L 截断边不收）+
///      整型/长整型算术位运算移位（不含除/余）+ 三个精确吻合包装调用。
///      栈机走停：词汇内失败（欠栈/类别不符）回落**最后一个 ≥ 2 实体指令
///      的深度归点**（最长封闭前缀，裸锚点不算——归点前缀是空栈模型起走
///      的纯走廊局部计算，失败只说明后续词汇运算在消费外部操作数，前缀折
///      叠栈形等价）；无归点 = 硬拒；词汇外边界 = 落最后一个归点。产出必须
///      落数值四形。标签普查只看跨度区间内的锚点：收集窗（≤64 条实体指令）
///      常大于跨度，窗外指令的标签（如紧邻下一条走廊的 goto 目标）不是本
///      跨度的合并点，算进来会被合法窗外引用者误杀（b.main 实证）。
///
/// 同锚点多式分派旧式先试（int 跨度 1 先于位拼 3、long 跨度 2 先于位拼
/// 4），通用兜底只接全部命名形状拒绝的站点，旧式折叠数不受兜底影响。
///
/// ## 改写（rewrite）
///
/// 按 `srcPc+op` 重定位锚点后**全量复核**：重新钉形状、重新求值、值与站点
/// 一致才动，同方法前站改写后不盲改。锚点（跨度首条装载）原地改成结果
/// 常量装载（保 srcPc，下游 pcmap/调试行落点不变），跨度其余非 NOP 指令
/// 出列，两种改写口径，各带自己的属性配套面：
///   - 命名形状**擦除**（eraseBatch kRejectIfUsed：受害者被异常表引用即整站
///     放弃）：受害者旧 pc 从 pcmap 蒸发，LNT/LVT 行锚 miss 即 lower Err
///     （B#5/c#3 实证）——先剪锚在受害者 pc 上的 LNT/LVT 行（调试面删行无
///     损语义）再擦除；SMT 帧不会锚在擦除受害者上（带 DOM 标签的受害者被
///     labelBlocked 整站拒掉）。
///   - 通用兜底**NOP 化**（透明 goto 的目标 ControlFlow 标签锚在跨度内部
///     成员上，擦除后标签悬垂，lower 重定位报 relocation bad target——拼装
///     走廊实证；NOP 化保住全部旧 pc，LNT/LVT/异常表原地落点不变）：配套
///     剪掉锚在**跨度内部全 pc（含既有 NOP——obfuscator squeeze 遗留也常
///     是分支目标也带帧，漏剪即 stale 帧残存 VerifyError，b.b 实证）**上的
///     SMT 分支目标帧（幸存帧 delta 链同步重算，delta 落不进压缩形编码域
///     即整站放弃 fail-closed）；exc handler 落点例外保留（异常分派 JVM 规
///     范先清栈，帧内容 [exc] 与走廊中间栈形无关）；handler 落在真实受害者
///     上 = handler 体在跨度里，NOP 化会毁掉 handler 逻辑，整站放弃。
/// 遗留 NOP 落点不动，DeadCode 收尾。帧链走 promote → 改写 →
/// `ir::lowerMethod`
/// 保真写回（pcmap 重定位 SMT/LVT/LNT，帧内容不动；折叠是栈形等价的语义
/// 恒等替换，Strict 帧再生的 CFG 推导对 junk 谓词走廊方法会把 append 帧
/// 的 locals 基算丢——Grunt 样本实证 B.c/u.b VerifyError，jsim 池求值因此
/// Bail 连锁破坏下游证据面，故不走推导，pool_array_fold 同一修复）；
/// 前置检查 `roundTripSafe`，原样往返不了的方法不进改写面。改写不在本轮
/// 展开嵌套跨度（rewrite 只消费 ScanResult），整链重复跑收敛。
///
/// 类字节**流式就地回写**：折叠成功即 `ClassResource::set` 原位换字节
/// （容器映射不动），不设待写缓冲，万类大 jar 上全量脏类字节驻留是 GB 级
/// 账，流式写回削平峰值；`uris` 聚类条目处理后即弃。
///
/// ## 已知边界
///
/// - `parseUnsignedLong` 的 `+`/`-` 号前缀语义（Java 8+）不收：生成面不
///   产出，收了反而给判据开口子。
/// - NaN 位形不特判：`intBitsToFloat` 对 NaN 载荷的规范化 JVM 有自由度，
///   LDC 位形与运行期换算在载荷位上可差，控制流不受影响，不设检查。
/// - 嵌套跨度（折叠产物再成跨度）靠整链重复跑到站点归零，不在单轮内迭代。
/// - fconst/dconst 起头的跨度不存在（生成面只从整型/字符串键起锚），
///   锚点类型检查直接排除。
/// - GETSTATIC 喂给位拼不收：v2 语料的跨度常量被 ConstPoolEncrypt 改写成
///   伴生池字段读取，跨度不再封闭（值在池字段里），缺口 #5 实现前
///   fail-closed；语料余量逐桶计数见 corpus-ledger §2 #8。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/const_args.hpp"
#include "grunt/iter_budget.hpp"

namespace grunt::number_fold {

/// 单类迭代预算默认值（语料实测：正常类单段工作量在数万操作内，取 20 倍以
/// 上余量；快照 63c4f33 实测 NumberFold 在 3362 类 jar 上单段 26s，超时尾部是
/// 个别类失控）。limit 口径：scan 记扫描位置数，rewrite 记站点复核指令数。
inline constexpr ca::usize kScanClassBudget    = 1'000'000;
inline constexpr ca::usize kRewriteClassBudget = 4'000'000;

/// @brief 站点折叠值的紧凑存储形（POD：kind + 位形，无字符串驻留）。
///
/// scan 收进站点的值恒为数值四形（I32/I64/F32/F64 位形），基 32 跨度的字符串键在匹配期就地折叠成 i64 位形并清串（matchBase32Long），跨段数据流里不需要字符串。访问器与 ValueLoad 同形（i32/i64/f32/f64 位形口径一致），rewrite 的复核比较（kind + bits）与单测断言逐位不动；slots 是 kind 的纯函数（I64/F64 双槽），不重复存。
struct SiteValue
{
    const_args::ValueLoad::Kind kind = const_args::ValueLoad::Kind::None;
    ca::u64                     bits = 0;   ///< 数值（符号扩展存）；浮点的 IEEE 位形

    ca::i32 i32() const noexcept { return static_cast<ca::i32>(bits); }
    ca::i64 i64() const noexcept { return static_cast<ca::i64>(bits); }
    ca::f32 f32() const noexcept
    {
        return rewrite_tools::bitCast<ca::f32>(static_cast<ca::u32>(bits));
    }
    ca::f64 f64() const noexcept { return rewrite_tools::bitCast<ca::f64>(bits); }

    /// 压栈槽数：kind 的纯函数（I64/F64 双槽），与 ValueLoad::slots 同口径。
    ca::u32 slots() const noexcept
    {
        return (kind == const_args::ValueLoad::Kind::I64
                || kind == const_args::ValueLoad::Kind::F64)
                   ? 2
                   : 1;
    }
};

/// @brief 一个可折叠站点。`anchorSrcPc`/`anchorOp` 跨会话稳定，rewrite 靠它重定位。`classIdx` 是 scan/rewrite 同一轮里容器 visitResources 的类序号（两段同序遍历，见 rewrite 注），万类大 jar 上站点不带 uri 字符串，数十万站点只按索引聚类；折叠值落 SiteValue 紧凑位形（样本 B 实测 77 万站点，POD 存储是 scan 期驻留的紧凑面）。
struct Site
{
    ca::u32       classIdx    = 0;
    ca::usize     methodIndex = 0;
    ca::u32       anchorSrcPc = 0;
    jbc::core::Op anchorOp    = jbc::core::Op::NOP;
    SiteValue     value;   ///< 折叠结果（I32 / I64 / F32 / F64 位形，无字符串）
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         classesSkipped = 0;   ///< 会话开不了的类（fail-closed 跳过）
    ca::usize         classesSkippedByBudget = 0;   ///< 迭代预算超限的类（保守跳过）
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites             = 0;   ///< 折叠成功的站点数
    ca::usize attempted         = 0;   ///< 进入 rewrite 的站点数
    ca::usize failed            = 0;   ///< 复核/标签检查/擦除/帧链拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数
    ca::usize skippedByBudget   = 0;   ///< 迭代预算超限后放弃尝试的站点数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出可折叠站点。单类迭代超预算时该类剩余方法保守跳过。
ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budget
                = iter_budget::ClassBudget{kScanClassBudget});

/// @brief REWRITE：纯写。只消费 ScanResult，按 anchorSrcPc 重定位后复核再改。
/// 单类迭代超预算时该类剩余站点保守放弃（已完成的站点保留，类照常写回）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budget
                 = iter_budget::ClassBudget{kRewriteClassBudget});

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::number_fold
