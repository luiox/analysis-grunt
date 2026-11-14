/// @file        arith_fold.hpp
/// @brief       残留 `arith_fold`：算术替换 junk 还原 + 常量对折叠
///              （v2 ArithmeticEncrypt / v2 MutateJumps 直线形 / v3
///              ArithmeticSubstitute，缺口 #7 + #18 同面）。
///
/// ## 形态（artifact 侧）
///
/// 两族输入，一个 pass 承载：
///
/// 1. 替换形状（生成侧 replaceINEG/replaceIAND/replaceIOR/replaceIXOR 的
///    逐字输出形，v2 与 v3 生成源逐字相同）。操作数是栈上既有值（通配），
///    窗口本身是纯固定指令序列：
///
///         INEG 形 0：  ICONST_M1 IXOR ICONST_1 IADD                → INEG
///         INEG 形 1：  ICONST_1 ISUB ICONST_M1 IXOR               → INEG
///         IAND 形：    SWAP DUP_X1 ICONST_M1 IXOR IOR SWAP
///                      ICONST_M1 IXOR ISUB                        → IAND
///         IOR 形：     DUP_X1 ICONST_M1 IXOR IAND IADD            → IOR
///         IXOR 形 0：  DUP2 IOR DUP_X2 POP IAND ISUB              → IXOR
///         IXOR 形 1：  DUP2 ICONST_M1 IXOR IAND DUP_X2 POP
///                      SWAP ICONST_M1 IXOR IAND IOR               → IXOR
///         IXOR 形 2：  DUP2 IOR DUP_X2 POP ICONST_M1 IXOR
///                      SWAP ICONST_M1 IXOR IOR IAND               → IXOR
///         IXOR 形 3：  DUP2 IOR DUP_X2 POP IAND ICONST_M1 IXOR
///                      IAND                                       → IXOR
///
///    八式都是精确代数恒等式（生成侧自证：替换前后值逐位相等），窗口
///    折回单条指令是纯代数改写，不依赖生成侧意图，用户代码里恰好出现
///    同序列时折叠同样保语义。
///
/// 2. 常量对折叠（v2 语料主面：ConstPoolEncrypt 把常量改写成伴生池字段
///    读取后，替换形状与 junk builder 的常量输入整片变成 GETSTATIC 喂给；
///    `grunt.ConstPoolRestore` 回填 ldc 后这些窗口重新封闭成纯常量算术）。
///    窗口从常量装载锚起，纯常量装载 + 封闭数值运算逐条求值，整窗归一
///    （净栈效恰一个值且窗口内全部非 NOP 指令都参与该值）才折成单条
///    常量装载。求值按 JVM 精确语义：int/long 全指令回绕、移位按 5/6 位
///    掩码、除零不折（保留原地异常）、F2I/D2I 族 NaN 归零与饱和钳位、
///    浮点按 IEEE 运算逐位复算。
///
///    MutateJumps 直线形（`INT(v±k); INT(k); IADD/ISUB` /
///    `INT(v^k); INT(k); IXOR`）与 TRIP1 形（`A ICONST_M1 IXOR B IAND`
///    常量喂给）都是本族的实例，不单列形状。
///
/// ## 判据（scan 的细找）
///
///   1. 替换形状逐条 op 吻合（含固定常量 ICONST_M1/ICONST_1），窗口内除
///      锚点外无标签（合并点 = 外部到达路径栈形耦合，放弃）；
///   2. 常量窗口的每条指令可求值（纯常量装载 + 封闭运算白名单），窗口
///      上限 16 条非 NOP 指令，白名单外指令即停窗；
///   3. 常量窗口整窗归一：净栈效恰一个逻辑值 + 全部非 NOP 指令参与，
///      二者缺一不收（窗口里留死值的形态不折）。
///
/// ## 改写（rewrite）
///
/// 按 `srcPc+op` 重定位锚点后全量复核（同方法前站改写后不盲改）。替换
/// 形状锚点原地改单条目标指令（保 srcPc），常量窗口锚点原地改结果常量
/// 装载；窗口其余非 NOP 指令擦除（eraseBatch kRejectIfUsed 拒收即整站
/// 还原放弃）。帧链走 mutateMethodWithFrames 正统写回，前置 roundTripSafe。
/// 替换形状还原后暴露的新常量窗口（如 `LDC a; LDC b;` + 还原出的 IAND）
/// 本轮不展开，链上重复 `grunt.ArithFold` 收敛（与 IndyRestore 第二轮
/// 同一口径）。
///
/// ## 链序
///
/// 建议排在 `grunt.NumberFold` 与 `grunt.ConstPoolRestore` 之后：NumberFold
/// 的六式跨度先吃自己的面（旧式站点数不受本 pass 影响，加性恒等），
/// ConstPoolRestore 回填的常量是本 pass 常量窗口的主要燃料。
///
/// ## 已知边界
///
/// - 栈操作（DUP/SWAP/POP 族）不进常量窗口白名单：常量喂给的替换形状由
///   形状族先行还原（还原后剩纯常量对，下一轮折掉），不在窗口里模拟
///   栈洗牌。
/// - FCMP/DCMP/LCMP 不进白名单（确定性可折但语料无此面，不扩）。
/// - 除零（IDIV/IREM/LDIV/LREM 除数常量 0）不折：原地保留 ArithmeticException
///   语义。
/// - 用户代码恰好含替换形状序列时同样折叠：八式是精确恒等式，值逐位
///   相等，无改错风险（如实登记）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/const_args.hpp"
#include "grunt/iter_budget.hpp"

namespace grunt::arith_fold {

/// 单类迭代预算（对齐 number_fold 量级：常量窗口模拟是逐位置的常数时间）。
inline constexpr ca::usize kScanClassBudget    = 1'000'000;
inline constexpr ca::usize kRewriteClassBudget = 4'000'000;

/// @brief 站点折叠值的紧凑存储形（与 number_fold::SiteValue 同口径）。
struct SiteValue
{
    const_args::ValueLoad::Kind kind = const_args::ValueLoad::Kind::None;
    ca::u64                     bits = 0;

    ca::i32 i32() const noexcept { return static_cast<ca::i32>(bits); }
    ca::i64 i64() const noexcept { return static_cast<ca::i64>(bits); }
    ca::f32 f32() const noexcept
    {
        return grunt::rewrite_tools::bitCast<ca::f32>(static_cast<ca::u32>(bits));
    }
    ca::f64 f64() const noexcept
    {
        return grunt::rewrite_tools::bitCast<ca::f64>(bits);
    }
};

/// @brief 一个站点。`shapeIdx` 是替换形状编号（0..7），255 = 常量窗口站点
///        （此时 value 有效）。`classIdx` 是容器 visitResources 类序号，
///        scan/rewrite 两段同序遍历。
struct Site
{
    ca::u32       classIdx    = 0;
    ca::usize     methodIndex = 0;
    ca::u32       anchorSrcPc = 0;
    jbc::core::Op anchorOp    = jbc::core::Op::NOP;
    ca::u8        shapeIdx    = 255;
    SiteValue     value;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site> sites;
    ca::usize         shapeSites          = 0;   ///< 替换形状站点数
    ca::usize         constSites          = 0;   ///< 常量窗口站点数
    ca::usize         classesSkipped      = 0;
    ca::usize         classesSkippedByBudget = 0;
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize attempted          = 0;
    ca::usize failed             = 0;
    ca::usize skippedByRoundTrip = 0;
    ca::usize skippedByBudget    = 0;
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

/// @brief REWRITE：纯写。只消费 ScanResult，复核后改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budget
                 = iter_budget::ClassBudget{kRewriteClassBudget});

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::arith_fold
