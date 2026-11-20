/// @file        noise_attr_strip.hpp
/// @brief       残留 `noise_attr_strip`：Crasher / AntiLLM 噪声属性摘除
///              （obf-behaviors 缺口 #13 的属性面）。
///
/// ## 形态（artifact 侧）
///
/// Grunteon v3 两类噪声属性：
/// - **DecompilerCrasher 的 massive Signature**：类/方法/字段挂内容非法的
///   ``Signature`` 属性（空串、空白块、缺 ``;`` 的类型串）。JVMS 4.7.9.1
///   的签名文法是上下文有关文法，非法串让 CFR 0.152 的签名解析器在类载入
///   即抛 ``ConfusedCFRException: Prototype is invalid``——整个类反编译不
///   出来（t3_full 152/152 类全带，逐类扫描 0/152 实证）。JVM 对 Signature
///   只做反射 generics 视图消费（getGenericType 对非法串本就抛
///   GenericSignatureFormatError），字节码执行从不读它，摘除不碰行为。
/// - **AntiLLM 的 SourceDebugExtension**：JVMS 4.7.11 纯字节串调试属性，
///   体是反分析文本（模拟 AI 摘要器的注入指令与策略噪声）。仅调试器消费，
///   运行时零语义。
///
/// ## 判据（scan 的细找）
///
/// ``Signature`` 按**槽位形**严格过 JVMS 4.7.9.1 文法（类槽 ClassSignature、
/// 方法槽 MethodSignature、字段槽 FieldSignature），文法过 = 合法保留
/// （jsoup 真泛型 ``Ljava/util/List<Lorg/jsoup/nodes/Element;>;`` 不伤），
/// 文法不过 = Crasher 串摘除。``SourceDebugExtension`` 整属性无条件摘除。
///
/// 保守面：Signature 文法合法的一律不动（哪怕 owner 是混淆器改名的类）；
/// 其余属性不碰。摘除后 cp_compact 带走死 Utf8（Crasher 串动辄数百 KB）。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

namespace grunt::noise_attr_strip {

/// @brief 一张属性表上的摘除站点：表归属 + 槽下标（同一份输入字节下稳定）。
struct AttrSite
{
    std::string uri;
    bool        isMethod    = false;   ///< 类表 / 方法表 / 字段表
    ca::usize   memberIndex = 0;      ///< 方法或字段下标（类表为 0）
    ca::usize   slotIndex   = 0;
    bool        isSignature = true;   ///< Signature（文法非法）/ SourceDebugExtension
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<AttrSite> sites;
    ca::usize             classesSkipped = 0;   ///< 会话打不开的类数
    ca::usize             sigInvalid     = 0;   ///< 非法 Signature 属性数
    ca::usize             sdeCount       = 0;   ///< SourceDebugExtension 属性数
};

/// @brief 被改写的类与属性级统计。
struct DirtySet
{
    std::vector<std::string> uris;
    ca::usize                attrsRemoved    = 0;
    ca::usize                sitesOutOfRange = 0;   ///< 索引越界（字节不同源）的防御计数
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。产出待摘属性站点。
ScanResult scan(const mjbase::model::ResourceContainer& container);

/// @brief REWRITE：纯写。只按 ScanResult 的站点摘属性，不重新判据。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result);

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::noise_attr_strip
