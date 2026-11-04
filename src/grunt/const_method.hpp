/// @file        const_method.hpp
/// @brief       残留 `const_method`：把「方法体 = 单条常量生产 + 返回」的
///              static 方法的调用点内联成常量装载。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把「取原始类型 Class 常量」这类表达式包一层静态方法：
///
///     public static Class t() { return Long.TYPE; }
///     // 字节码：getstatic java/lang/Long.TYPE Ljava/lang/Class; ; areturn
///
/// 调用点与目标一一对应、方法体无分支，返回值与运行环境无关——调用点等价
/// 于一条常量装载。实测样本规模：686 处 getstatic+areturn、少量 int/null 形态。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实：
///   1. 目标是本容器内 `static`、有方法体、非初始化器、非 `synchronized`；
///   2. 方法体恰为两条指令：单条常量生产 + 匹配的返回。生产形态：
///      a) `getstatic <九大原始类型包装类>.TYPE` → 原始类型 Class 常量
///         （JVMS 允许 CONSTANT_Class 引用原始类型描述符名，可落 `ldc`）；
///      b) iconst*/bipush/sipush/ldc 整数 + `ireturn` → int 值；
///      c) `aconst_null` + `areturn` → null。
///      其余 getstatic（`Boolean.TRUE` 这类堆对象常量）没有可静态编码的
///      等价形式，拒绝；
///   3. **所有者类初始化纯度门**：`invokestatic` 是主动使用，删调用会推迟
///      目标类初始化。所有者的 `<clinit>` 必须缺失，或整体落在纯常量白名单
///      内（常量装载/数值运算/转换/比较 + 对**本类**静态字段的 `putstatic`，
///      直线无标签）。此时跳过初始化只推迟「常量写入自己字段」，后续任何
///      `getstatic` 读取者自己就是主动使用，仍会触发初始化拿到同样的值。
///
/// 保守面：跨类 `putstatic`、分支/循环、一切堆读取 → 所有者判不纯，整方法
/// 拒绝。类级 `ACC_SYNTHETIC` 之类的标志清理不在本残留（见 flags_strip）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/iter_budget.hpp"

namespace grunt::const_method {

/// 单类迭代预算默认值（口径与余量依据同 grunt::number_fold 的同名常量；
/// scan 记扫描位置数，rewrite 记站点复核指令数）。
inline constexpr ca::usize kScanClassBudget    = 1'000'000;
inline constexpr ca::usize kRewriteClassBudget = 4'000'000;

/// @brief 内联值：能落成调用点上一条指令的静态常量。
struct ConstValue
{
    enum class Kind : ca::u8 {
        I32 = 0,     ///< int 值（`i32` 有效）
        Null,        ///< null 引用（落 `aconst_null`）
        PrimClass,   ///< 原始类型 Class 常量（`prim` 是描述符字符，落 `ldc`）
    };

    Kind    kind = Kind::I32;
    ca::i32 i32  = 0;
    char    prim = 0;   ///< PrimClass：'B'/'S'/'I'/'J'/'F'/'D'/'C'/'Z'/'V'
};

/// @brief 一个可内联调用点。`callSrcPc` 是原始字节布局下的 pc，跨会话稳定。
struct Site
{
    std::string callerUri;
    std::string callerOwner;
    std::string callerMethod;
    std::string callerDesc;
    ca::usize   callerMethodIndex = 0;
    ca::u32     callSrcPc         = 0;

    std::string targetOwner;
    std::string targetName;
    std::string targetDesc;
    ConstValue  value;
};

/// @brief 定义侧拒绝原因（方法不满足内联判据；调用侧只在改写失败时计数）。
enum class RejectReason : ca::u8 {
    BodyNotSingleConst = 0,  ///< 方法体不是「单常量生产 + 匹配返回」的形状
    TargetSynchronized,      ///< 监视器语义，删调用会改变可观测行为
    GetstaticUnsupported,    ///< getstatic 目标不是九大原始 TYPE
    OwnerClassInitImpure,    ///< 所有者 <clinit> 不在纯常量白名单内
};

/// @brief 一条定义侧拒绝记录。
struct RejectedMethod
{
    std::string  owner;
    std::string  name;
    std::string  desc;
    RejectReason reason = RejectReason::BodyNotSingleConst;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>           sites;
    std::vector<RejectedMethod> rejects;
    ca::usize                   classesSkippedByBudget = 0;   ///< 预算超限的类（保守跳过）
};

/// @brief 被改写的类与站点级统计（trace 用）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize              attempted = 0;
    ca::usize              applied   = 0;
    std::vector<ca::usize> failedByReason;   ///< 按 rewrite 失败原因分类计数
    ca::usize              skippedByRoundTrip = 0;
    ca::usize              skippedByBudget    = 0;   ///< 预算超限后放弃尝试的站点数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出内联站点与定义侧拒绝面。
/// 单类迭代超预算时该类剩余方法保守跳过。
ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budget
                = iter_budget::ClassBudget{kScanClassBudget});

/// @brief REWRITE：纯写。只消费 ScanResult，按 callSrcPc 原地替换调用指令。
/// 单类迭代超预算时该类剩余站点保守放弃（已完成的站点保留）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budget
                 = iter_budget::ClassBudget{kRewriteClassBudget});

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::const_method
