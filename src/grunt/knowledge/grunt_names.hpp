/// @file        grunt_names.hpp
/// @brief       Grunt 家族知识：StringEncrypt / StringArrayedEncrypt 字符串池
///              形态常量（v2/v3 共用面）。
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
/// v2/v3 判别（缺口 #15）不挂本表，era.hpp 不消费本文件。
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

}   // namespace grunt::knowledge
