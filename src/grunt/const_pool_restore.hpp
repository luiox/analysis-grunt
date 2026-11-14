/// @file        const_pool_restore.hpp
/// @brief       残留 `const_pool_restore`：v2 ConstPoolEncrypt 伴生池常量的
///              调用点回填（缺口 #5）。
///
/// ## 形态（artifact 侧，罐 B 语料 javap 实证）
///
/// ConstPoolEncrypt 把类内字面常量改写成对伴生池类静态字段的读取。罐面
/// 是两拨伴生类：
///
///     调用点：    GETSTATIC Companion.const_xxxxx : T   （原 ldc 常量位）
///     数值池类：  普通 public 类（非 final），super = java/lang/Object，
///                 字段全部 static 数值形且各带 ConstantValue 属性，无
///                 clinit，方法面只有一个水印方法（static ()Ljava/lang/String;，
///                 体两条 LDC 字符串 + ARETURN）
///     字符串伴生类：另一拨，数值字段走 clinit 算术写（不带 CV），字符串
///                 字段 LDC 密文 + INVOKESTATIC 自家解密器
///                 (Ljava/lang/String;)Ljava/lang/String; + PUTSTATIC（clinit），
///                 水印方法文字与池类不同（罐 B 实证 I AM WATERMARK）
///
/// 伴生类与解密器体在 heavy 配置下会被二次加密（HeavyEncrypt，默认关），
/// 本 pass 只消费解密链已折回的形态：数值面靠 ConstantValue 属性（原生
/// 属性与链上 ConstValuePromote 产物同面收，static + 零写 = JVM 权威值），
/// 字符串面靠解密器模板钉形 + 逐码元自逆 XOR 直解。
///
/// ## 判据（scan 的细找）
///
///   1. 数值面逐字段判据（不设类级门，伴生池字段族与 javac static final
///      常量族同面收走）：static、非 volatile、描述符 ∈ {B,C,I,S,Z,J,F,D}、
///      容器级写普查零写、普查有读、ConstantValue 属性在且池条目 tag 与
///      描述符相容（GETSTATIC 与 ldc 逐位等值，S1）；
///   2. 字符串面伴生类门：非 interface/abstract/enum/annotation、super =
///      java/lang/Object、字段全部 static 且描述符受援、方法面 ⊆
///      {`<clinit>`} ∪ {static 解密器 (Ljava/lang/String;)Ljava/lang/String;
///      且体逐字钉模板} ∪ {水印方法（static ()Ljava/lang/String;，体两条
///      LDC 字符串 + ARETURN）}；
///   3. 字符串字段：容器级写普查恰一次写且在伴生类 clinit、写点形状 =
///      [LDC 密文, INVOKESTATIC 自家解密器, PUTSTATIC 本字段]、解密器体
///      24 条指令逐字钉（StringBuilder 逐字符 charAt ^ key 链，key 是体内
///      唯一自由变量）、密文逐码元 XOR key 直解（S1 算术自证）；
///   4. 调用点：GETSTATIC 解析成员命中已登记字段。
///
/// 写普查走 field_census（promote 后指令面），普查不完整整轮放弃（宁误留
/// 不误删，与 const_value_promote 同一口径）；唯一豁免是验证器判死：promote
/// 以 BadInstructions 拒绝的方法链接期必被 VerifyError 拒掉，方法体永不
/// 执行，按死亡不计入完整性，豁免数以 verifierDead 带出。
///
/// ## 改写（rewrite）
///
/// 按 `srcPc` 重定位 GETSTATIC（op 复核）后成员复核（owner/name/desc 与
/// 站点一致才动），单条指令原地换常量装载（保 srcPc；数值走
/// makeConstLoad 短形态优先，字符串 intern 进池走 LDC/LDC_W）。无擦除、
/// 栈形逐位等（long/double 双槽两读同形），帧链走 mutateMethodWithFrames
/// 正统写回，前置 roundTripSafe。帧重算失败的方法整方法回滚（罐 B 语料
/// 单 pass 口径 20144/938982 站点，集中在巨型陷阱方法），站点保留
/// GETSTATIC 原样，本 pass 无擦除、字段不删，语义不变。
///
/// ## 链序
///
/// 数值面与 ConstValuePromote 分工（本面收 ConstantValue 属性族，
/// ConstValuePromote 收 clinit 写一次无 CV 族，互不重叠），放
/// `grunt.NumberFold` + `grunt.ConstValuePromote` 之后跑；字符串面只依赖
/// 解密器体未被二次加密。本 pass 之后 `grunt.ArithFold` 吃回填出的常量对。
///
/// ## 已知边界
///
/// - HeavyEncrypt 配置（默认关）下伴生 clinit 与解密器体被 NumberEncrypt/
///   StringEncrypt 二次加密，判据 3 的模板钉形失效，逐类 fail-closed。
/// - 数值面消费 ConstantValue 属性：JVMS 只规范 static final 字段的 CV
///   赋值，HotSpot 对非 final static 也执行（罐 B 语料 LinkRead 反射读
///   3/3 实证）；其他 JVM 实现若不执行，回填值与运行期值可能不符。
/// - 写面靠指令普查兜底，反射 / JNI 写不可见（与 const_value_promote
///   同一信息边界）。
/// - 伴生类本体不删：字段回填后成为零读死重，交 DeadMember/后续垃圾类
///   删除面（缺口 #14）。
/// - 字符串回填是 ldc 字面量（驻留池），伴生字段原值是 clinit 期新串：
///   内容相等、实例身份不同，与 StringPoolFold 的字符串池回填同一口径，
///   如实登记。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "mjbase/model/resource_container.hpp"

#include "grunt/knowledge/const_args.hpp"
#include "grunt/iter_budget.hpp"

namespace grunt::const_pool_restore {

inline constexpr ca::usize kScanClassBudget    = 1'000'000;
inline constexpr ca::usize kRewriteClassBudget = 4'000'000;

/// @brief 一个已定值的伴生字段。
struct FieldValue
{
    std::string              owner;
    std::string              name;
    std::string              desc;
    const_args::ValueLoad    value;
};

/// @brief 一个调用点站点（GETSTATIC 位）。
struct Site
{
    ca::u32   classIdx    = 0;
    ca::usize methodIndex = 0;
    ca::u32   srcPc       = 0;
    ca::u32   field       = 0;   ///< ScanResult::fields 下标
};

/// @brief 三段之间唯一的数据流。
struct ScanResult
{
    std::vector<FieldValue>                    fields;
    std::unordered_map<std::string, ca::u32>   fieldIndex;   ///< key = owner::name:desc
    std::vector<Site>                          sites;

    ca::usize poolClasses        = 0;   ///< 过形状门的伴生类数（字符串面）
    ca::usize classesNumeric     = 0;   ///< 数值面贡献过字段值的类数
    ca::usize fieldsNumeric      = 0;
    ca::usize fieldsString       = 0;
    ca::usize classesSkipped     = 0;
    ca::usize verifierDead      = 0;   ///< 验证器判死方法数（BadInstructions，普查豁免）
    bool      censusIncomplete   = false;
};

/// @brief 被改写的类 + 站点级统计。
struct DirtySet
{
    std::vector<std::string> uris;

    ca::usize sites              = 0;
    ca::usize attempted          = 0;
    ca::usize failed             = 0;
    ca::usize failedRelocate     = 0;   ///< srcPc 重定位找不到 GETSTATIC
    ca::usize failedMember       = 0;   ///< 成员复核不一致
    ca::usize failedMakeLoad     = 0;   ///< 常量装载指令构造失败
    ca::usize failedFrames       = 0;   ///< 帧重算失败（整方法回滚）
    ca::usize skippedByRoundTrip = 0;
};

/// @brief 验收结果。
struct ProofResult
{
    bool                     ok = true;
    std::vector<std::string> failed;
};

/// @brief SCAN：纯读。写普查不完整整轮放弃。
ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budget
                = iter_budget::ClassBudget{kScanClassBudget});

/// @brief REWRITE：纯写。只消费 ScanResult。
DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budget
                 = iter_budget::ClassBudget{kRewriteClassBudget});

/// @brief PROOF：纯判。类数不增 + 脏类可重新解析。
ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore);

}   // namespace grunt::const_pool_restore
