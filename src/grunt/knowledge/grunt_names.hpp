/// @file        grunt_names.hpp
/// @brief       Grunt 家族知识：StringEncrypt / StringArrayedEncrypt 字符串池
///              形态常量（v2/v3 共用面）+ ReferenceObfuscate / InvokeDynamic
///              的 indy 形态常量（v2/v3 共用面）。
///
/// CONVENTION §2：家族知识唯一落点。Grunt v2（grunt2 分支 36c0eef 的
/// `StringEncryptTransformer.kt`）与 v3（grunt3 分支 e41e852 的
/// `StringArrayedEncrypt.kt`）给每类生成同一套字符串池三件套：`String[]`
/// 池字段 + `arrayInit` 填池方法 + `([CJI)Ljava/lang/String;` 解密器；
/// `<clinit>` 头部插池分配前导，调用点原地换 `GETSTATIC pool; idx; AALOAD`。
/// 解密算法两代逐字相同（char 级 XOR 流，classKey ^ key ^ seed 驱动、内部
/// 状态随 `i & 0xFF` 旋转），差别只在产物配置：v2 默认 `Arrayed=false`
/// （密文走 `ldc` 串 + `toCharArray`），v3 默认 `Carray=true`（密文走
/// `NEWARRAY T_CHAR` + 逐码元 `CASTORE`）——密文形态是配置选择不是版本键，
/// v2/v3 判别（缺口 #15）不挂本表，era.hpp 不消费本文件的池段。
///
/// indy 段（v2 `InvokeDynamicTransformer.kt` / v3 `ReferenceObfuscate.kt`）：
/// 两代的 bootstrap 描述符、`encrypt`（逐码元 `code ^ xor` 自逆 XOR）、
/// heavy 的 `@Metadata` 注解（mv/d1/d2 成员 + `data1 % 59557 == magic2`
/// 互证）逐字同源，也落本表；`@Metadata` 注解类默认名两代不同（v2
/// `net/spartanb312/grunt/GruntMetadata`、v3 `net/spartanb312/grunteon/
/// internal/Metadata`，均可配置），名字不进判据，era 指纹见 era.hpp。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace grunt::knowledge {

// ---- 池三件套形态（StringEncryptTransformer.kt / StringArrayedEncrypt.kt
//      的 field(...)/method(...)/createDecryptMethod(...) 生成面）------------

/// @brief 池字段描述符（`"[Ljava/lang/String;"` 逐字取自生成面 field 调用）。
inline constexpr std::string_view kGruntPoolFieldDesc = "[Ljava/lang/String;";

/// @brief 解密器描述符（两代 createDecryptMethod 逐字 `"([CJI)Ljava/lang/String;"`：
///        char[] 密文 + long seed + int key）。
inline constexpr std::string_view kGruntDecryptorDesc = "([CJI)Ljava/lang/String;";

/// @brief `<clinit>` 池分配前导里 ANEWARRAY 的类操作数（`ANEWARRAY("java/lang/String")`）。
inline constexpr std::string_view kGruntPoolElemType = "java/lang/String";

/// @brief literal 密文形态的 toCharArray 调用（`INVOKEVIRTUAL
///        "java/lang/String", "toCharArray", "()[C"`）。
inline constexpr std::string_view kGruntToCharArrayOwner = "java/lang/String";
inline constexpr std::string_view kGruntToCharArrayName  = "toCharArray";
inline constexpr std::string_view kGruntToCharArrayDesc  = "()[C";

/// @brief arrayed 密文形态的 NEWARRAY atype（`NEWARRAY(Opcodes.T_CHAR)`，JVM
///        规范 T_CHAR = 5）。
inline constexpr ca::i32 kGruntNewarrayTChar = 5;

// ---- 解密器参数槽（desc ([CJI) 决定的静态方法槽位事实）--------------------

/// @brief 解密器 int key 参数槽（static 方法：0=[C，1-2=J seed，3=I key）。
inline constexpr ca::u32 kGruntDecryptorKeySlot = 3;

/// @brief 解密器 long seed 参数槽（低位槽；lload/lstore 家族按此钉）。
inline constexpr ca::u32 kGruntDecryptorSeedSlot = 1;

/// @brief 解密器循环的状态旋转掩码（体内 `SIPUSH(255); IAND` 逐字面）。
inline constexpr ca::i32 kGruntRotateMask = 255;

/// @brief per-string seed 生成上界（`Random.nextLong(100000L)`）——记录性
///        常量（语料普查对数用），不作判据。
inline constexpr ca::i64 kGruntSeedBound = 100000;

// ---- indy 形态（v2 InvokeDynamicTransformer.kt / v3 ReferenceObfuscate.kt
//      的 createBootstrap / createHeavyBootstrap / encrypt / metadata 生成面，
//      两代逐字同源）----------------------------------------------------------

/// @brief simple bootstrap 描述符（7 参：Lookup, String, MethodType,
///        String, String, String, Integer；静态参数 4 个）。两代 BSM access
///        均 `PUBLIC+STATIC+SYNTHETIC+BRIDGE`（名字不钉）。
inline constexpr std::string_view kGruntSimpleBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/Integer;)"
    "Ljava/lang/invoke/CallSite;";

/// @brief heavy bootstrap 描述符（7 参：Lookup, String, MethodType,
///        String, Integer, Integer, Integer；静态参数 4 个）。两代 BSM access
///        均 `PRIVATE+STATIC+SYNTHETIC+BRIDGE`。
inline constexpr std::string_view kGruntHeavyBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/String;Ljava/lang/Integer;Ljava/lang/Integer;Ljava/lang/Integer;)"
    "Ljava/lang/invoke/CallSite;";

/// @brief simple 解密器描述符（`createDecryptMethod`，逐码元 `^ key`）。
inline constexpr std::string_view kGruntSimpleDecryptorDesc =
    "(Ljava/lang/String;)Ljava/lang/String;";

/// @brief heavy 解密器描述符（`createHeavyDecryptMethod`，逐码元 `^ key`）。
inline constexpr std::string_view kGruntHeavyDecryptorDesc =
    "(Ljava/lang/String;I)Ljava/lang/String;";

/// @brief heavy 元数据自证模数：`data1 = 59557 * q + magic2`（两代 metadata
///        生成面逐字 `59557`；BSM 体 `LDC 59557; IREM` 消费同一常量）。
inline constexpr ca::i32 kGruntMetadataModulus = 59557;

/// @brief heavy BSM 的 mv 版本门（体 `BIPUSH 100; IF_ICMPGE`，生成面注解
///        `mv = 100`）。
inline constexpr ca::i32 kGruntMetadataMv = 100;

/// @brief v3 默认 `@Metadata` 注解类内部名（`config.metadataClass` 默认值，
///        可配置——只作 era 指纹与文档锚点，不作还原判据）。
inline constexpr std::string_view kGrunt3MetadataAnnoInternalName =
    "net/spartanb312/grunteon/internal/Metadata";

/// @brief v3 默认 `@Metadata` 注解描述符（era 指纹半边）。
inline constexpr std::string_view kGrunt3MetadataAnnoDesc =
    "Lnet/spartanb312/grunteon/internal/Metadata;";

/// @brief v3 混淆器生成类命名空间前缀（`grunteon` 字样 v2 不产；era 指纹
///        另一半边）。
inline constexpr std::string_view kGrunt3NamespacePrefix = "net/spartanb312/grunteon/";

/// @brief heavy `d2` 载荷分隔符（生成面 `name + "<>" + desc`，BSM 体
///        `LDC "<>"; split` 消费同一串）。
inline constexpr std::string_view kGruntMetadataPairSep = "<>";

/// @brief v3 RuntimeMaterial 联动的解密器材料通道形状锚点：`materialKey()`
///        体内 `GETSTATIC keyField(J); DUP2; INT(32); LUSHR; LXOR; L2I;
///        GETSTATIC poisonField(I); IXOR; IRETURN`（源锚
///        `createMaterialKeyMethod`，grunt3 ReferenceObfuscate.kt L1187-1213
///        ——本常量即其中 `INT(32); LUSHR` 的位移），`createDecryptMethod` 的
///        `materialKeyName != null` 分支解密器体内 `LDC key; INVOKESTATIC
///        <self> ()I; IXOR; IXOR`——有效 key 依赖运行期材料字段
///        （canonicalKey/poison，boar 实证其初值由解密后的字符串池经
///        parseLong 转出），纯静态不可解。判据在 indy_restore（fail-closed，
///        只按窗口形状自证，不消费本位移值；锚点留给缺口 #15 联动面）。
inline constexpr ca::u32 kGruntMaterialKeySlotShift = 32;

// ---- InvokeDispatcher 分发器形态（v3 专属；grunt3 InvokeDispatcher.kt
//      生成面。grunt2 分支 redirect/ 下无此 transformer——v3-only，见
//      doc/dispatcher-restore.md）--------------------------------------------

/// @brief 分发器逐槽参数类型（生成面 `repeat(maxLoadOp) { desc +=
///        "Ljava/lang/Object;" }` 逐字面）。
inline constexpr std::string_view kGruntDispatcherParamType = "Ljava/lang/Object;";

/// @brief 分发器 key 参数类型（desc 尾参 `...I)$returnDesc` 逐字面）。
inline constexpr std::string_view kGruntDispatcherKeyType = "I";

/// @brief 分发器生成名前缀（`dispatcher_<10 随机字符>`；记录性常量——改名
///        后不存活，不作判据）。
inline constexpr std::string_view kGruntDispatcherNamePrefix = "dispatcher_";

/// @brief 分发器单组转发目标上限（`MaxHandles` 默认值；记录性常量——可配置，
///        不作判据）。
inline constexpr ca::i32 kGruntDispatcherMaxHandles = 10;

/// @brief 分发器同返回类型分组的最小调用点数（生成面 `group.filter { 
///        it.value.size > 1 }`——键表 case 数 ≥ 2 是生成面事实，作形状门）。
inline constexpr ca::i32 kGruntDispatcherMinCases = 2;

}   // namespace grunt::knowledge
