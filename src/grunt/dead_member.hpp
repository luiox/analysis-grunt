/// @file        dead_member.hpp
/// @brief       残留 `dead_member`：零引用方法 / 字段 / 空 clinit 删除。
///
/// ## 形态（artifact 侧）
///
/// 内联与折叠完成后，大量成员只剩空壳：纯算术辅助方法（调用点已折成常量）、
/// `/* bridge */ /* synthetic */` 转发器（调用点已内联）、private 辅助字段、
/// 拆空后的 <clinit>。反编译出来就是用户点名要删的空壳。
///
/// ## 判据（scan 的细找）
///
/// 形状候选（scan，S2 结构事实）：
///   - 方法：`static` 且方法体纯算术形态（常量/栈/算术/分支/return 词汇表），
///     或带 ACC_SYNTHETIC/ACC_BRIDGE，或 `private`；跳过 main / native /
///     abstract / 无码 / <init> 特例不作形状豁免；
///   - 字段：`private`（public/包级零引用可能是按名反射面，宁留勿删），或
///     `static [I` 零访问孤儿数组（seed_strip 拆除建表后的形状，独立强证
///     明）；serialVersionUID / 带 ConstantValue 的不删；
///   - <clinit>：方法体只剩 NOP/RETURN（拆空形状）。
///
/// 引用门（rewrite 复核）：全类 CP 先压缩（闭包保证枚举即活引用，压缩失败
/// 的类整类放弃——死条目会污染零引用判定，宁留勿删），全容器枚举
/// Fieldref/Methodref 得活引用集；候选必须不在集合内。静态普查之外的两类
/// 运行时引用面（scan 时整类/按名豁免）：类内 BootstrapMethods 的盲区（BSM
/// 可经解密字符串 findStatic 本类任意成员）、注解字符串按名引用（@Redirect）；
/// 另有跨类 indy 目标——scan 内嵌 indy_fold::scan 跑一遍全容器 BSM，解析出的
/// 目标方法按 owner+name 保护（Grunt 形态：BSM 在 A 类，目标在 B 类）。反射是
/// 理论逃逸面（按名反射方法拦不住），台账如实记录每一次删除。
///
/// ## 改写（rewrite）
///
/// 先全容器 CP 压缩（引用闭包前提）→ 普查活引用 → 逐类删成员（backing
/// 序倒序 erase）→ 落盘 → 再压缩一次带走死描述符。方法删除不动任何方法
/// 体，无布局/SMT 面。
///
/// ## 台账
///
/// DirtySet.methods / fields / clinits 分项计数；compacted 是仅压缩变小的
/// 类数；被删成员的 owner::name::desc 在 trace 面逐条可开。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::dead_member {

/// @brief 方法候选的形状门。
enum class MethodGate : ca::u8 { PureArith = 0, SyntheticBridge, Private };

/// @brief 一个方法候选（scan 时的 backingIndex + 名字描述，rewrite 双重定位）。
struct MethodCand
{
    ca::usize   backingIndex = 0;
    std::string name;
    std::string desc;
    MethodGate  gate = MethodGate::Private;
};

/// @brief 一个字段候选。
struct FieldCand
{
    ca::usize   backingIndex = 0;
    std::string name;
    std::string desc;
    bool        orphanArray = false;   ///< static [I 零访问孤儿（否则 private 门）
};

/// @brief 一类的候选集。
struct ClassCand
{
    std::string             uri;
    std::string             owner;
    std::vector<MethodCand> methods;
    std::vector<FieldCand>  fields;
    ca::usize               emptyClinitIndex =
        static_cast<ca::usize>(-1);   ///< backing index，npos = 无
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<ClassCand> classes;
    ca::usize              classesSkipped = 0;
    ca::usize              indyTargets    = 0;   ///< indy 普查解析出的跨类运行时目标数
    ca::usize              indyBsmFailed  = 0;   ///< indy 普查里 BSM 未收敛的站点数（目标未知）
};

/// @brief 被改写的类 + 统计。
struct DirtySet
{
    std::vector<std::string> uris;   ///< 落过盘的类（含仅压缩）

    ca::usize methods   = 0;
    ca::usize fields    = 0;
    ca::usize clinits   = 0;
    ca::usize compacted = 0;   ///< 仅压缩变小的类数
    ca::usize attempted = 0;
    ca::usize rejectedByRef = 0;   ///< 形状过关但引用门拦下
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出形状候选（引用门在 rewrite，需先压缩闭包）。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：压缩 → 引用普查 → 删成员 → 落盘 → 再压缩。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 落盘类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::dead_member
