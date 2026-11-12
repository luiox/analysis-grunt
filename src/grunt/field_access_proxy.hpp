/// @file        field_access_proxy.hpp
/// @brief       残留 `field_access_proxy`：把方法体 = 实参逐槽过手 + 单次
///              字段访问 + 匹配返回的 static 字段代理方法的调用点改回真实
///              字段访问。
///
/// ## 形态（artifact 侧）
///
/// 混淆器把每个字段访问点包一层 static 代理：调用点改成
/// `invokestatic proxy(...)`，代理方法体只是把实参原样装载、做一次字段
/// 访问、按字段类型返回/返回 void。生成面（Grunteon FieldAccessProxy，3.0.0
/// 实跑取证）的实测形状：
///
///     // getter（静态字段）：static ()T  = getstatic + 匹配返回
///     public static int get_x ()I {
///         getstatic Owner.x : I
///         ireturn
///     }
///     // setter（静态字段）：static (T)V = 值过手 + putstatic + return
///     public static void set_x (I)V {
///         iload_0
///         putstatic Owner.x : I
///         return
///     }
///     // getter（实例字段）：static (Lowner;)T = 接收者过手 + getfield
///     public static int get_y (LOwner;)I {
///         aload_0
///         getfield Owner.y : I
///         ireturn
///     }
///     // setter（实例字段）：static (Lowner;T)V = 逐槽过手 + putfield
///     public static void set_y (LOwner;I)V {
///         aload_0
///         iload_1
///         putfield Owner.y : I
///         return
///     }
///     // 调用点：invokestatic ProxyOwner.get_x ()I
///
/// 代理落在调用者类内或调用者类的生成外置类（`...$FieldProxy`，全
/// `public static`）；字段 owner 可以在容器外（`System.out` 也被代理）。
/// 后续改名 pass 会把代理名/外置类名抹掉，判据不能依赖名字、标志与家族名。
///
/// ## 判据（scan 的细找）
///
/// S2 结构事实，**只钉方法体形状 + 引用普查，不依赖家族名/命名模式**：
///
///   1. 定义侧：`static`、有方法体、非初始化器、非 `synchronized`；方法体
///      恰为每个形参槽一条匹配类型的 `xload n`（long/double 占两槽）+
///      单条字段访问 + 匹配描述符返回类型的 `*return`的直线；无标签、无
///      异常表。字段访问与代理描述符核对：getter 返回类型类 == 字段描述符
///      类型类；实例字段的字段 owner 必须与描述符接收者一致；getter 描述符
///      返回区 == 字段描述符（setter 的实参尾区 == 字段描述符）。
///   2. 引用普查：只对容器内**有调用点**的代理动手，零引用代理是死码，
///      是 DeadMember 的业务，本残留不产站点。
///   3. 初始化暴露检查：`invokestatic proxy(C)` 是 C 的主动使用。改回字段
///      访问后这层触发消失。生成面里调用点全在代理 owner 类自身（外置类
///      无 `<clinit>`），判据把这写成可检查的事实：**代理的调用点必须全
///      部落在 owner 类内，否则要求 owner `<clinit>` 缺失或只剩
///      NOP/RETURN**（scan_tools::classInitNeutral 中性口径），不满足整
///      代理放弃。字段 owner 的初始化时机不受影响：代理体内的字段访问本来
///      就触发它。
///
/// ## 改写
///
/// 调用点**原地换 op + 换字段引用**（保 `srcPc`）：按代理体字段指令落
/// `getstatic`/`putstatic`/`getfield`/`putfield`，栈效应不变（getter 弹
/// 0/1 参压 1 值、setter 弹实参不压值，与代理完全一致），栈帧无需修补。
/// 不改代理本体，改回后代理变零引用死码，交下游 DeadMember/DeadCode
/// 清理。
///
/// 保守面：代理体带标签/异常表/多余指令、装载槽错、字段描述符核对错配、
/// 返回错配、`synchronized`、初始化暴露 → 整代理拒绝。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::field_access_proxy {

/// @brief 调用点该落回的字段访问种类。
enum class FieldKind : ca::u8 {
    GetStatic = 0,
    PutStatic,
    GetField,
    PutField,
};

/// @brief 一个可改回的调用点。`callSrcPc` 是原始字节布局下的 pc，跨会话稳定。
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

    std::string fieldOwner;
    std::string fieldName;
    std::string fieldDesc;
    FieldKind   kind = FieldKind::GetStatic;
};

/// @brief 定义侧拒绝原因（方法沾代理形状但不成立的才记；其余静默跳过）。
enum class RejectReason : ca::u8 {
    BodyNotProxyShape = 0,   ///< 体形对不上（多余指令 / 装载槽错 / 非字段访问）
    FieldDescMismatch,       ///< 字段描述符与代理描述符核对不过（返回区/接收者）
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
    std::vector<Site>          sites;
    std::vector<RejectedProxy> rejects;
    ca::usize                  proxiesFound = 0;   ///< 体形成立的代理定义数（含零引用）
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

/// @brief REWRITE：纯写。只消费 ScanResult，按 callSrcPc 原地换 op + 字段引用。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::field_access_proxy
