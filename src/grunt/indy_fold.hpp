/// @file        indy_fold.hpp
/// @brief       残留 `indy_fold`：invokedynamic 解密折叠。
///
/// ## 形态
///
/// 混淆器把每个字符串调用点改写成 `invokedynamic`，引导方法（BSM）在第一次
/// 链接时用类内的解密方法把静态参数解开，再 `findStatic`/`findVirtual` 出真正的
/// 目标句柄，包成 `ConstantCallSite` 返回：
///
///     invokedynamic #x   // 静态参数 = 三段密文 + 一个 Integer 开关
///     ↓ BSM
///     Class.forName(decrypt(a)) → getClassLoader
///     MethodType.fromMethodDescriptorString(decrypt(c), loader)
///     Lookup.findStatic|findVirtual(类, decrypt(b), 类型)
///     ConstantCallSite(句柄.asType(调用点类型))
///
/// 结果是**每个调用点都是明文目标，但字节码里一个明文都没有**——密文在 BSM
/// 参数表里，靠运行时解密。所以按字符串常量做匹配的规则全部失效。
///
/// ## 判据
///
/// 不去匹配某个混淆器的 BSM 形状（那样换一版就得改一次），而是**把 BSM 真跑
/// 一遍**：喂它自己的静态参数，读它返回的 CallSite 里那个句柄。目标是 BSM
/// 自己算出来的，不是我们猜的。
///
/// 折叠条件（三条同时成立才改）：
///
///   1. BSM 跑到底并返回 CallSite，且句柄载荷完整；
///   2. 句柄描述符 == 调用点描述符（否则 `asType` 做了类型适配，换指令后语义会变）；
///   3. 句柄是 Static 或 Virtual（Special/Interface 换过去栈语义不同）。
///
/// ## 改写
///
/// 把 `invokedynamic` 就地改成 `invokestatic` / `invokevirtual`，CP 操作数换成
/// 目标方法引用。栈效应不变：调用点描述符与句柄描述符相同时，弹出的实参集合
/// 与压回的返回值完全一致。
///
/// **不压缩常量池**：BootstrapMethods 属性仍引用着 BSM 的 MethodHandle 与那几段
/// 密文，压缩会把还活着的条目当死数据搬走。要清得等属性本身被摘掉之后，
/// 那是另一个 pass 的事。
///
/// ## 台账
///
/// DirtySet.sites 计数折叠成功的调用点、classes 计数动过的类。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "grunt/iter_budget.hpp"

#include "jsim/indy.hpp"

namespace grunt::indy_fold {

/// 单类 scan 迭代预算默认值：每类处理的 indy 站点数上限。每站点要真跑一次
/// BSM（jsim 引擎单次上限 4096 步），万站点类是分钟级尾部——超限类保守跳过
/// （语料实测：Grunteon 3362 类 10285 站点，单类均值约 3，此值 3000 倍余量）。
inline constexpr ca::usize kScanClassBudget = 10'000;

/// BSM 静态参数的值。只收折叠用得到的形态——收不全就整站放弃，不猜。
struct BsmValue
{
    enum class Kind : ca::u8 {
        Str,
        Int,
        Class,
        MethodType,
        MethodHandle,
        Unsupported,
    };

    Kind        kind = Kind::Unsupported;
    std::string text;   ///< Str 内容 / Class 内部名 / MethodType 描述符
    ca::i32     i = 0;  ///< Int
};

/// 一个 invokedynamic 调用点。
struct Site
{
    std::string uri;
    std::string owner;               ///< 宿主类内部名
    ca::usize   methodIndex = 0;
    std::string methodName;
    std::string methodDesc;
    ca::usize   insnPc = 0;          ///< 该方法 order() 里的位置

    std::string bsmOwner;
    std::string bsmName;
    std::string bsmDesc;
    std::vector<BsmValue> args;

    std::string indyName;
    std::string indyDesc;

    // ---- scan 的结论 ----
    bool              foldable = false;
    jsim::indy::Target target;
    std::string       why;           ///< 不可折叠原因（给 trace 用）
};

struct ScanResult
{
    std::vector<Site> sites;         ///< 全部调用点（含不可折叠的，便于观测）
    ca::usize         classes   = 0;
    ca::usize         foldable  = 0;
    ca::usize         bsmFailed = 0;
    bool              engineReady = true;  ///< 执行器是否可用（容器能开类）
    ca::usize         classesSkippedByBudget = 0;   ///< 预算超限的类（保守跳过）
};

struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites   = 0;
    ca::usize classes = 0;
};

struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。普查调用点 + 跑 BSM 求目标（模拟不写容器）。
/// 单类站点数超预算时该类剩余站点保守跳过（BSM 不再跑）。
ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budget
                = iter_budget::ClassBudget{kScanClassBudget});

/// @brief REWRITE：纯写。按 scan 的结论就地改指令。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析 + 无 indy 残留。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::indy_fold
