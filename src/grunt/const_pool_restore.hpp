/// @file        const_pool_restore.hpp
/// @brief       残留 `const_pool_restore`：v2 ConstPoolEncrypt 伴生池常量的
///              调用点回填（缺口 #5）。
///
/// ## 形态（artifact 侧）
///
/// ConstPoolEncrypt 把类内全部字面常量（iconst 族 / ldc 数值 / ldc 字符串）
/// 改写成对伴生类 `Orig$ConstantPool` 静态字段的读取：
///
///     调用点：  GETSTATIC Companion.const_xxxxx : T     （原 ldc 常量位）
///     伴生类：  PUBLIC FINAL，super = java/lang/Object，全部字段 static
///               数值/字符串形，clinit 逐字段解密回填：
///               数值   NumberEncryptorClassic 走廊（NumberFold 折回 ldc）
///                      + ConstValuePromote 回填 ConstantValue 并摘写点
///               字符串 LDC 密文 + INVOKESTATIC 自家解密器
///                      (Ljava/lang/String;)Ljava/lang/String; + PUTSTATIC
///
/// 伴生类与解密器体在 heavy 配置下会被二次加密（HeavyEncrypt，默认关），
/// 本 pass 只消费解密链已折回的形态：数值面靠 ConstantValue 属性（链上
/// ConstValuePromote 的产物，静态 final + 零写 = JVM 权威值），字符串面
/// 靠解密器模板钉形 + 逐码元自逆 XOR 直解。
///
/// ## 判据（scan 的细找）
///
///   1. 伴生类形状钉：非 interface/abstract/enum/annotation、ACC_FINAL、
///      super = java/lang/Object、字段全部 static 且描述符 ∈ {I,J,F,D,
///      Ljava/lang/String;}、方法面 ⊆ {<clinit>} ∪ {static 解密器
///      (Ljava/lang/String;)Ljava/lang/String; 且体逐字钉模板}；
///   2. 数值字段：ConstantValue 属性在、池条目 tag 与描述符相容、容器级
///      写普查零写（static + ConstantValue + 零写 + 非 volatile，GETSTATIC
///      与 ldc 逐位等值，S1）；
///   3. 字符串字段：容器级写普查恰一次写且在伴生类 clinit、写点形状 =
///      [LDC 密文, INVOKESTATIC 自家解密器, PUTSTATIC 本字段]、解密器体
///      24 条指令逐字钉（StringBuilder 逐字符 charAt ^ key 链，key 是体内
///      唯一自由变量）、密文逐码元 XOR key 直解（S1 算术自证）；
///   4. 调用点：GETSTATIC 解析成员命中已登记字段。
///
/// 写普查走 field_census（promote 后指令面），普查不完整整轮放弃（宁误留
/// 不误删，与 const_value_promote 同一口径）。
///
/// ## 改写（rewrite）
///
/// 按 `srcPc` 重定位 GETSTATIC（op 复核）后成员复核（owner/name/desc 与
/// 站点一致才动），单条指令原地换常量装载（保 srcPc；数值走
/// makeConstLoad 短形态优先，字符串 intern 进池走 LDC/LDC_W）。无擦除、
/// 栈形逐位等（long/double 双槽两读同形），帧链走 mutateMethodWithFrames
/// 正统写回，前置 roundTripSafe。
///
/// ## 链序
///
/// 数值面依赖 `grunt.NumberFold` + `grunt.ConstValuePromote` 先行（走廊
/// 折回 + ConstantValue 回填 + 写点摘除）；字符串面只依赖解密器体未被
/// 二次加密。本 pass 之后 `grunt.ArithFold` 吃回填出的常量对。
///
/// ## 已知边界
///
/// - HeavyEncrypt 配置（默认关）下伴生 clinit 与解密器体被 NumberEncrypt/
///   StringEncrypt 二次加密，判据 3 的模板钉形失效，逐类 fail-closed。
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
