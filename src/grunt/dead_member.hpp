/// @file        dead_member.hpp
/// @brief       残留 `dead_member`：零引用方法 / 字段 / 空 clinit 删除。
///
/// ## 形态（artifact 侧）
///
/// 内联与折叠完成后，大量成员只剩空壳：纯算术辅助方法（调用点已折成常量）、
/// `/* bridge */ /* synthetic */` 转发器（调用点已内联）、private 辅助字段、
/// 拆空后的 `<clinit>`。反编译出来就是用户点名要删的空壳。
///
/// ## 判据（scan 的细找）
///
/// 形状候选（scan，S2 结构事实）：
///   - 方法：`static` 且方法体纯算术形态（常量/栈/算术/分支/return 词汇表），
///     或带 ACC_SYNTHETIC/ACC_BRIDGE，或 `private`；跳过 main / native /
///     abstract / 无码 / `<init>` 特例不作形状豁免；
///   - 字段：`private`（public/包级零引用可能是按名反射面，宁留勿删），或
///     `static [I` 零访问孤儿数组（seed_strip 拆除建表后的形状，独立强证
///     明）；serialVersionUID / 带 ConstantValue 的不删；
///   - `<clinit>`：方法体只剩 NOP/RETURN（拆空形状）。
///
/// 引用检查（rewrite 复核）：活引用普查按**全容器对外引用全集**口径，每个类
/// 都必须成为引用方，缺一个类就是漏计（验证案例：压缩失败类被排除在普查外，
/// `w` 对 `t.aa` 的 Methodref 从集合里蒸发，`t` 类 93 方法全删、运行期
/// NoSuchMethodError）。三类来源分层：
///   - 压缩成功的类：池是闭包，枚举即活引用；
///   - 压缩失败 / 会话打不开的类（Raw 属性面等封闭集之外）：池未重编号，
///     按原始字节骨架解析（readClass）后枚举原池，可能含死条目，枚举结果
///     是活引用的超集，宁误留不误删；
///   - 骨架也解析不了的类：引用证据完全不可得，任何候选都无法被证明死，
///     本轮成员删除整体放弃（零删除），宁整轮不删不冒删错。
/// 普查是效力检查，挂起时间片跑全量（proof 类数检查同一检查），不许超墙截短出
/// 晚到的类不在普查里。压缩失败的类仍不作删除候选（字节面封闭集之外，
/// 两边宁留勿删），只是引用照入普查。静态普查之外的两类运行时引用面（scan
/// 时整类/按名豁免）：类内 BootstrapMethods 的盲区（BSM 可经解密字符串
/// findStatic 本类任意成员）、注解字符串按名引用（`@Redirect`）；另有跨类
/// indy 目标，scan 内嵌 indy_fold::scan 跑一遍全容器 BSM，解析出的目标方法
/// 按 owner+name 保护（Grunt 形态：BSM 在 A 类，目标在 B 类）。反射是理论
/// 逃逸面（按名反射方法拦不住），统计如实记录每一次删除。
///
/// ## 改写（rewrite）
///
/// 先全容器 CP 压缩（引用闭包前提）→ 普查活引用 → 逐类删成员（backing
/// 序倒序 erase）→ 落盘 → 空壳类回收（全成员删净且全容器零引用的类整体
/// 摘除）→ 再压缩一次带走死描述符。方法删除不动任何方法体，无布局/SMT 面。
///
/// 空壳类回收的判据（保守检查）：删后 methods/fields 全空 ∧ 非 module-info /
/// package-info（含 main 的类天然有成员，被前置检查挡住）∧ 全容器其余类的 CP
/// 无 Classref、无同名 utf8（按名字符串/SourceFile 面）、无 `L<name>;`
/// 描述符引用。注解类/父类/接口/异常表都走 CP Class 条目，普查看得见；
/// jar 级非类资源（MANIFEST、外部按名配置）是理论逃逸面，含 main 的主类
/// 不可能为空壳，Main-Class 检查天然成立，其余记录统计。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::dead_member {

/// @brief 方法候选的形状检查。
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
    bool        orphanArray = false;   ///< static [I 零访问孤儿（否则 private 检查）
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
    std::vector<std::string> removedUris;   ///< 整类摘除的空壳（proof 检查跳过重解析）

    ca::usize methods   = 0;
    ca::usize fields    = 0;
    ca::usize clinits   = 0;
    ca::usize classes   = 0;   ///< 整类摘除数（空壳回收）
    ca::usize compacted = 0;   ///< 仅压缩变小的类数
    ca::usize attempted = 0;
    ca::usize rejectedByRef = 0;   ///< 形状过关但引用检查拦下

    ca::usize censusRawSources = 0;   ///< 引用普查走原始字节路径的类数（压缩失败/会话打不开）
    ca::usize censusBlind      = 0;   ///< 连骨架都解析不了的类数（>0 则本轮零删除）
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出形状候选（引用检查在 rewrite，需先压缩闭包）。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：压缩 → 引用普查 → 删成员 → 落盘 → 再压缩。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 落盘类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::dead_member
