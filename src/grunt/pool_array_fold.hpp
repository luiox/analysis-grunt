/// @file        pool_array_fold.hpp
/// @brief       残留 `pool_array_fold`：字符串池数组读点的**求值通道**压池——
///              模拟执行池类 `<clinit>` 读回数组终态，`GETSTATIC; idx;
///              AALOAD` 三元组原地折 `ldc` 明文。
///
/// ## 与 string_pool_fold 的分工
///
/// `string_pool_fold` 是 Grunt vanilla 池三件套的**形状钉**通道（池分配前导
/// + arrayInit 逐块钉形 + 解密器三窗口静态解码），fork 变体（junk 包裹解密
/// 器、seed 走廊 `parseUnsignedLong` 计算、填充方法独立成体）钉不上。本
/// 残留是同一残留族（池读点 → ldc）的**求值通道**：不钉池链形状，判据只
/// 要求数组字段形态 + 读点三元组形状 + clinit 求值收敛（运行期算什么明文
/// 就是什么，与 ConstPoolRestore 求值通道、DeadMember 池明文提取同一哲学）。
/// 两通道可以共存：形状钉先吃 vanilla 面，求值通道收 fork 残余。
///
/// ## 形态（样本 C 实测口径）
///
/// 池类持有 `static String[]` 字段（伴生类伴生字段，全部同类读点——业务类
/// 调池类静态方法取串，数组读点不跨类），`<clinit>`（直连或经填充方法，
/// 编排层接力）以任意计算链填池：解密器调用、seed 走廊、junk 包裹、恒真
/// 谓词。读点统一三元组形：`GETSTATIC <池>; <int idx>; AALOAD`。
///
/// ## 判据分级（README 残留一览表口径）
///
/// - **S2 结构事实**：字段 static + 描述符 `[Ljava/lang/String;`；读点三元
///   组紧邻（NOP 跳过）且跨度无标签（合并点 = 外部到达路径栈形状不可知，
///   fail-closed）；下标常量。
/// - **S1 语义后果**：JVM 类初始化语义——任何主动使用先跑 `<clinit>`，读点
///   处的数组终态由 clinit 唯一确定；模拟执行忠实收敛即得终态。保守面（该
///   (owner, field) 整组放弃）：clinit 求值 Bail / 步数熔断；非 `<clinit>`
///   方法存在 `PUTSTATIC` 池字段或 `GETSTATIC` 池字段 + `AASTORE` 共存（运行
///   期改写嫌疑，无法排除就保留）；读点下标越界 / 元素 Unknown；明文含孤
///   立代理（`ldc` 常量池承载不了）。
/// - **无 Era 检查**：判据不依赖 Grunt 世代形状（这是求值通道的立意），
///   家族检查由三元组 + 数组字段形态自证。
///
/// ## 改写
///
/// 对齐 string_pool_fold 读点折叠口径：锚点（getstatic）**原地换** `ldc`
/// 明文（srcPc 保留，pcmap/调试行落点不变），三元组其余两条 eraseBatch 擦
/// 除（kRejectIfUsed 拒收即回滚锚点整站放弃）；帧链 mutateMethodWithFrames
/// 整体重算，前置 roundTripSafe。**池字段 / clinit / 解密器不删**，读点归
/// 零后交 DeadCode / DeadMember / 垃圾类面统一回收。
///
/// ## 统计
///
/// DirtySet：动过的类 uri、折掉的三元组数。ScanResult 另报池类数、三元组
/// 命中数、求值 Bail 的池类数、写嫌疑组数、越界下标数（fail-closed 计数，
/// 宁留不猜）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::pool_array_fold {

/// @brief 明文码元类型（Java String 的 UTF-16 code unit 语义）。
using PlainUnits = std::vector<ca::u16>;

/// @brief 一个可折叠读点。`anchorSrcPc` 跨会话稳定，rewrite 靠它重定位。
struct Site
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorSrcPc = 0;   ///< getstatic 旧布局 pc（InsnId 不跨会话）
    std::string owner;             ///< 池字段 owner（slash 内部名，= 本类）
    std::string fieldName;         ///< 池字段名
    ca::i32     index      = 0;    ///< 池下标
    PlainUnits  plainUnits;        ///< 明文码元（改写面按 MUTF-8 编码 intern）
    std::string plaintext;         ///< 明文 UTF-8 记录形（trace/统计/单测；
                                   ///< 代理对重组 4 字节；孤立代理整站拒绝）
};

/// @brief 拒绝（fail-closed 计数：拒绝原因进 trace，站点不动）。
struct Reject
{
    std::string uri;
    ca::usize   methodIndex = 0;
    ca::u32     anchorSrcPc = 0;
    std::string why;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>   sites;
    std::vector<Reject> rejects;

    ca::usize poolClasses  = 0;  ///< 候选池字段组数（owner, field）
    ca::usize tripleHits   = 0;  ///< 三元组形状命中数（含下标不在终态表的）
    ca::usize idxMissing   = 0;  ///< 下标不在求值终态表（越界/Unknown，不折）
    ca::usize evalBailed   = 0;  ///< clinit 求值未收敛的池类组数
    ca::usize writeSuspect = 0;  ///< 运行期改写嫌疑（写面普查不过）的池类组数
    ca::usize classesSkipped = 0;  ///< 会话开不了的类
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize classes            = 0;
    ca::usize failed             = 0;   ///< 复核/定位/擦除/编码拦下的站点数
    ca::usize skippedByRoundTrip = 0;   ///< 原样往返不了的方法上的站点数
};

/// @brief 验收结果。residual = 改写后仍可折而未折的站点数（语料单 jar 检查
///        要求 0；求值面类不重入 residual，只数已收敛池上的未折读点）。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
    ca::usize                residual = 0;
};

/// @brief SCAN：纯读。池字段普查 + 写面普查 → clinit 求值终态 → 三元组定案。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 anchorSrcPc 重定位后复核再改。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + residual 统计。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::pool_array_fold
