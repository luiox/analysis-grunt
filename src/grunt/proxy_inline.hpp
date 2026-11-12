/// @file        proxy_inline.hpp
/// @brief       残留 `proxy_inline`：把方法体 = 实参逐槽过手 + 单次 invoke +
///              匹配返回的 static 代理方法的调用点改指回真实目标。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把每个方法调用点包一层 static 代理：调用点改成
/// `invokestatic proxy(...)`，代理方法体只是把实参原样装载、转发一次真实
/// invoke、按返回类型返回。生成面（InvokeProxy 一族）的实测形状：
///
///     // static 目标：代理描述符 == 目标描述符
///     private static bridge synthetic add_redirected_x (Lrecv;II)I {
///         aload_0
///         iload_1
///         iload_2
///         invokevirtual recv.add (II)I
///         ireturn
///     }
///     // 调用点：invokestatic C.add_redirected_x (Lrecv;II)I
///     //           （virtual 目标的接收者被降成第一个实参）
///
/// 代理落在调用者类内（`private static`）或生成的外置类
/// `C$InvokeProxy`（`public static`，owner+method 都 public 时）；virtual
/// 目标的调用点同步改写成 invokestatic、描述符前置接收者。interface/
/// special 目标不生成代理。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实，**只钉方法体形状 + 引用普查，不依赖家族名/命名模式**：
///
///   1. 定义侧：`static`、有方法体、非初始化器、非 `synchronized`；方法体
///      恰为每个实参槽一条匹配类型的 `xload n`（long/double 占两槽）+
///      单条 invoke + 匹配描述符返回类型的 `*return`的直线；无标签、无
///      异常表。转发目标只认 `invokestatic`（代理描述符 == 目标描述符）与
///      `invokevirtual`（代理描述符首参 == `L目标owner;` 且余部 == 目标
///      描述符）；`invokespecial`/`invokeinterface` 不收。
///   2. 引用普查：只对容器内**有调用点**的代理动手，零引用代理是死码，
///      是 DeadMember 的业务，本残留不产站点。
///   3. 初始化暴露检查：`invokestatic proxy(C)` 是 C 的主动使用。改指真实
///      目标后这层触发消失。生成面里调用点全在代理 owner 类自身（外置类
///      无 `<clinit>`），判据把这写成可检查的事实：**代理的调用点必须全
///      部落在 owner 类内，否则要求 owner `<clinit>` 缺失或只剩
///      NOP/RETURN**（与 constant_call 的中性口径一致），不满足整代理放弃。
///
/// ## 改写
///
/// 调用点**原地换 op + 换成员引用**（保 `srcPc`）：static 目标落
/// `invokestatic`，virtual 目标落 `invokevirtual`（栈上首实参就是接收者，
/// 弹栈/压栈集合不变，栈帧无需修补）。不改代理本体，内联后代理变零引用
/// 死码，交下游 DeadMember/DeadCode 清理。
///
/// 保守面：代理体带标签/异常表/非转发指令、描述符错配、返回错配、
/// `synchronized`、初始化暴露 → 整代理拒绝。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::proxy_inline {

/// @brief 一个可改指的调用点。`callSrcPc` 是原始字节布局下的 pc，跨会话稳定。
struct Site
{
    std::string callerUri;
    std::string callerOwner;
    std::string callerMethod;
    std::string callerDesc;
    ca::usize   callerMethodIndex = 0;
    ca::u32     callSrcPc         = 0;

    std::string proxyOwner;
    std::string proxyName;
    std::string proxyDesc;

    std::string targetOwner;
    std::string targetName;
    std::string targetDesc;
    bool        virtualTarget = false;   ///< true → 落 invokevirtual，首实参是接收者
};

/// @brief 定义侧拒绝原因（方法沾代理形状但不成立的才记；其余静默跳过）。
enum class RejectReason : ca::u8 {
    BodyNotProxyShape = 0,   ///< 体形对不上（多余指令 / 装载槽错 / 类型错）
    TargetKindUnsupported,   ///< 转发是 invokespecial / invokeinterface
    TargetDescMismatch,      ///< static 代理描述符 != 目标描述符 / virtual 首参错配
    ReturnMismatch,          ///< 返回指令与描述符返回类型不符
    ProxySynchronized,       ///< 监视器语义，内联会丢锁
    OwnerInitExposed,        ///< 调用点越出 owner 类且 owner `<clinit>` 非中性
};

/// @brief 一条定义侧拒绝记录。
struct RejectedProxy
{
    std::string  owner;
    std::string  name;
    std::string  desc;
    RejectReason reason = RejectReason::BodyNotProxyShape;
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<Site>           sites;
    std::vector<RejectedProxy>  rejects;
    ca::usize                   proxiesFound = 0;   ///< 体形成立的代理定义数（含零引用）
};

/// @brief 被改写的类与站点级统计（trace 用）。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize attempted = 0;
    ca::usize applied   = 0;
    std::vector<ca::usize> failedByReason;   ///< 按 rewrite 失败原因分类计数
    ca::usize skippedByRoundTrip = 0;
    ca::usize proxies            = 0;        ///< 动过的不同代理数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。代理定义表 + 调用点普查 + 初始化暴露检查。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只消费 ScanResult，按 callSrcPc 原地换 op + 成员引用。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::proxy_inline
