/// @file        constant_call.hpp
/// @brief       残留 `constant_call`：把全常量实参 + 纯算术目标方法的
///              `invokestatic` 折叠成一条常量装载。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把字面量常量换成一个静态辅助方法的调用，实参全为常量，方法体是
/// 直线算术（MBA 恒等式、掩码、移位）。例如
///
///     ldc -1720838488 ; ldc -1424803483 ; iand ; ixor
///     iload 1 ; swap ; dup_x1 ; iconst_m1 ; ixor ; ior ; swap ; iconst_m1 ; ixor ; isub
///
/// 这样的方法对常量实参的返回值是**唯一确定**的，与运行环境无关；调用点
/// 因而等价于一个常量。把它折掉之后，外围的 `== 常量`、`if` 分支、死支
/// 诱饵才会被下游的常量折叠/死码清理接手。
///
/// ## 判据（scan 的细找）
///
/// S1 语义事实，三条同时成立：
///   1. 目标方法是本容器内声明的 `static`、有方法体、非 `<init>`/`<clinit>`、
///      非 `synchronized`；
///   2. 调用点的实参由**紧邻的纯常量装载**产生，且槽数与描述符严格相等；
///   3. 用 jsim 执行器（跨方法、封闭环境）在**常量实参**下能跑出一个值，
///      环境外调用/字段/分配一律 bail，求值成功本身就是这条执行路径
///      可静态重放且无外部副作用的证明。
///
/// 额外的保守面（不算判据，是拒绝理由）：被调类的 `<clinit>` 必须缺失或
/// 只剩 `return`，否则删掉调用会改变类初始化时机。**同类豁免**（JVMS 5.5）：
/// 本类方法开始执行时类已经在初始化中/已完成，同类 `INVOKESTATIC` 不触发
/// 初始化，删调用不移时机，豁免站点的纯性仍由封闭环境求值独立证明。
/// （boar 取证：processor 键解锁链的全部可折转发站点都落在非中性 clinit
/// 类里，检查不豁免则整条链空转。）
///
/// 求值环境接管（本残留自带）：`java/lang/Math.max(II)I`，IndyRestore 还原
/// `__grt$indy$` 转发体内 indy 后，CFF inline 桥的 MaxConst 站点落在
/// `java/lang/Math.max(II)I` 上；jsim 核心/num 包无 JDK 数学面，缺接管整条
/// 面在 EvalBadState 拒。max 是全序纯函数，双常量实参下值唯一确定，
/// unknown 按未知口径传播。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "jsim/value.hpp"

namespace grunt::constant_call {

/// @brief 一个可折叠站点（ScanResult::sites 的元素）。
///
/// `caller*` 定位调用点，`target*` 是证据，`args`/`value` 是判定结果。
/// `callSrcPc` 是原始字节布局下的 pc，跨会话稳定，rewrite 用它重新定位
/// 指令，而不是把 InsnId（会话内句柄）带出去。
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
    ca::usize   targetMethodIndex = 0;

    std::vector<jsim::SimValue> args;   ///< 逐实参（含 null 引用）
    jsim::SimValue              value;  ///< 求值结果
};

/// @brief 拒绝原因（站点级证据链的一部分，供 trace 与文档引用）。
enum class RejectReason : ca::u8 {
    TargetNotInContainer = 0,  ///< 目标不在本容器（JDK / 依赖 jar）
    TargetNotStatic,           ///< 非 static
    TargetNoCode,              ///< abstract / native / 无方法体
    TargetInitializer,         ///< `<init>` / `<clinit>`
    TargetSynchronized,        ///< 监视器语义，删调用会改变可观测行为
    TargetVoid,                ///< 返回值 void，无值可折
    ClassInitPresent,          ///< 被调类的 `<clinit>` 非空，删调用会改变初始化时机
    ArgsNotConstant,           ///< 实参不是紧邻的纯常量装载
    ArgsKindUnsupported,       ///< 实参里有非数值常量（String/Class 等）
    EvalUnsupported,           ///< 求值器不支持该指令形状
    EvalStepLimit,             ///< 步数熔断（循环/不可达 return）
    EvalBadState,              ///< 栈/局部变量状态不符（含除零）
    ValueKindUnsupported,      ///< 结果不可折叠（引用，或环境未知值传播成 unknown）
};

/// @brief 一条拒绝记录。
struct RejectedSite
{
    std::string  callerOwner;
    std::string  callerMethod;
    ca::u32      callSrcPc = 0;
    RejectReason reason    = RejectReason::ArgsNotConstant;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>         sites;
    std::vector<RejectedSite> rejects;
};

/// @brief 被改写的类（uri 列表）+ 站点级统计（trace 用，不参与语义）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize              attempted = 0;   ///< 进入 rewrite 的站点数
    ca::usize              applied   = 0;   ///< 实际改写的站点数
    std::vector<ca::usize> failedByReason;  ///< 按 rewrite 失败原因分类计数

    /// 因原样往返不了而整方法放弃的站点数。这类放弃与改写无关：方法在
    /// 未修改状态下就走不完一次 lower，改写只会把类一起拖坏（见 roundTripSafe）。
    ca::usize skippedByRoundTrip = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;   ///< 失败的类 uri 或检查名
};

/// @brief SCAN：纯读。产出站点与拒绝面。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，不重新找站点（按 callSrcPc 定位）。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::constant_call
