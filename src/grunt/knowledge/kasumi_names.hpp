/// @file        kasumi_names.hpp
/// @brief       Grunt 家族知识：Kasumi Obfuscator 的名字形态与 indy 形态表。
///
/// CONVENTION §2：家族知识唯一落点。Kasumi 把调用点/字段访问/构造器全部改写成
/// `invokedynamic`，由每类自带的 8 参 bootstrap 在运行时解密 bsmArgs、按
/// mode/extra 选 `findStatic/findVirtual/findGetter/…/findConstructor` 出句柄。
/// 本表钉的是「8 参 bootstrap 描述符、解密器描述符、mode×extra 7 形态、
/// `@Metadata` 注解、水印串、反 LLM 资源前缀」这批版本常量。
///
/// 常量逐字取自参考实现（kasumi_indy_resolve_pass / kasumi_metadata_cleanup_pass
/// / kasumi_string_decrypt_pass / KasumiStringDumper.java，源在仓外），
/// SnakeGame-obf.jar（582,974 B，验收数字见 doc/kasumi-verify.md）实证。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace grunt::knowledge {

// ---- indy 引导方法（kasumi_indy_resolve_pass.cpp）--------------------------

/// @brief Kasumi bootstrap 方法描述符（8 参，返回 CallSite）。
///
/// 参数：Lookup, String(name), MethodType, Integer mode, String encOwner,
/// String encName, String encDesc, Integer extra；bsmArgs 5 个 = 后五项。
inline constexpr std::string_view kKasumiBootstrapDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/Integer;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
    "Ljava/lang/Integer;)Ljava/lang/invoke/CallSite;";

/// @brief 每类一个的 XOR 解密器描述符（key 提取与 BSM→解密器定位共用）。
inline constexpr std::string_view kKasumiDecryptorDesc =
    "(Ljava/lang/String;)Ljava/lang/String;";

/// @brief bootstrap 静态参数个数：[mode, encOwner, encName, encDesc, extra]。
inline constexpr ca::usize kKasumiBsmArgCount = 5;

// ---- 字符串缓存解密面（kasumi_string_decrypt_pass.cpp 逐字取参考实现）---------

/// @brief String[] 缓存字段描述符（形态 1：读缓存站点的 getstatic 钉面）。
///
/// Kasumi 把 Blowfish 解密后的明文缓存到 `String[]` 静态字段，调用点取
/// `getstatic owner.f:[Ljava/lang/String;; ldc idx; aaload`。
inline constexpr std::string_view kKasumiCacheFieldDesc = "[Ljava/lang/String;";

/// @brief byte[][] 密文字段描述符（形态 2：直接解密站点的 getstatic 钉面）。
///
/// 密文以 `byte[][]` 静态字段在场，调用点
/// `getstatic owner.f:[[*B; ldc idx; aaload; dup; arraylength; ldc off;
/// invokestatic owner.a([BII)String`。
inline constexpr std::string_view kKasumiCryptFieldDesc = "[[B";

/// @brief 直接解密方法描述符（形态 2 的 invokestatic 钉面；方法名样本相关，
///        不钉名）。语义：`a(bytes, length, offset) → String`。
inline constexpr std::string_view kKasumiByteDecryptDesc =
    "([BII)Ljava/lang/String;";

/// @brief TSV 旁证里 byte[][] 字段的 key 前缀（KasumiStringDumper 用
///        `"byte:" + 字段名` 与 String[] 缓存字段区分）。
inline constexpr std::string_view kKasumiByteKeyPrefix = "byte:";

// ---- mode × extra 7 形态表（S3 级判据的数据；越界组合一律拒绝）------------

/// @brief mode 分量（实现选择，换代需复核——Era 分支唯一合法落点的数据面）。
inline constexpr ca::u32 kKasumiModeMethod = 0;   ///< 方法调用（extra 分 static/virtual）
inline constexpr ca::u32 kKasumiModeField  = 1;   ///< 字段访问（extra 分 4 路）
inline constexpr ca::u32 kKasumiModeCtor   = 2;   ///< 构造器（extra 恒 0，语料未见他用）

/// @brief 7 形态：还原落点指令的投影。数值来自 bootstrap 体 tableswitch 分派
///        与 627 站点语料印证（notes §2.4：0/0=247, 0/1=37, 1/0=4, 1/1=4,
///        1/2=22, 1/3=23, 2/0=2——旧样本口径，SnakeGame-obf 实测见语料账本）。
enum class KasumiIndyForm : ca::u8 {
    Static,       ///< mode 0 extra 0 → invokestatic
    Virtual,      ///< mode 0 extra 1 → invokevirtual / invokeinterface（receiver 走 indy 首参）
    GetField,     ///< mode 1 extra 0 → getfield
    PutField,     ///< mode 1 extra 1 → putfield
    GetStatic,    ///< mode 1 extra 2 → getstatic
    PutStatic,    ///< mode 1 extra 3 → putstatic
    Constructor,  ///< mode 2（extra 任意，语料恒 0）→ NEW;DUP + INVOKESPECIAL `<init>`
};

/// @brief mode+extra → 形态；不在表内的组合返回 false（fail-closed，不猜）。
inline bool kasumiIndyForm(ca::u32 mode, ca::u32 extra, KasumiIndyForm& out) noexcept
{
    switch (mode) {
    case kKasumiModeMethod:
        if (extra == 0) {
            out = KasumiIndyForm::Static;
            return true;
        }
        if (extra == 1) {
            out = KasumiIndyForm::Virtual;
            return true;
        }
        return false;
    case kKasumiModeField:
        switch (extra) {
        case 0: out = KasumiIndyForm::GetField; return true;
        case 1: out = KasumiIndyForm::PutField; return true;
        case 2: out = KasumiIndyForm::GetStatic; return true;
        case 3: out = KasumiIndyForm::PutStatic; return true;
        default: return false;
        }
    case kKasumiModeCtor:
        out = KasumiIndyForm::Constructor;
        return true;
    default: return false;
    }
}

// ---- 命名空间与清理面常量（kasumi_metadata_cleanup_pass.cpp + notes）-------

/// @brief 混淆器自身的命名空间（Main-Class `dev.kasumi.obfuscator.w` 的包）。
inline constexpr std::string_view kKasumiNamespace = "dev/kasumi/obfuscator";

/// @brief `@Ldev/kasumi/internal/Metadata;` 注解描述符（字段 mv/d1/d2）。
inline constexpr std::string_view kKasumiMetadataAnnoDesc = "Ldev/kasumi/internal/Metadata;";

/// @brief 水印串：`PROTECTED BY Kasumi` 子串命中。
inline constexpr std::string_view kKasumiWatermarkText = "PROTECTED BY Kasumi";

/// @brief 水印串：`Gruntpocalypse` 子串命中（参考实现 isKasumiWatermarkString 的
///        第三条判据，逐字取自 kasumi_metadata_cleanup_pass.cpp——参考实现水印
///        口径把姊妹家族的署名串一并当水印清）。
inline constexpr std::string_view kKasumiWatermarkAltText = "Gruntpocalypse";

/// @brief 水印串：`kasumi_` 前缀命中。
inline constexpr std::string_view kKasumiWatermarkKeyPrefix = "kasumi_";

/// @brief 反 LLM 注入资源 URI 前缀（`META-INF/Kasumi/cybersecurity/analysis-*.md`）。
inline constexpr std::string_view kKasumiResourcePrefix = "META-INF/Kasumi/";

/// @brief 反 LLM 注入资源 URI 后缀（参考实现 endsWith(uri, ".md") 逐字口径）。
inline constexpr std::string_view kKasumiResourceSuffix = ".md";

/// @brief 反 LLM 注入资源内容探针（参考实现 isKasumiAntiLlmResource 的五条逐字
///        取自 kasumi_metadata_cleanup_pass.cpp）：URI 形状命中之外，字节里
///        任一探针子串在场才算注入内容——S1 互证面，删的是内容坐实的资源。
inline constexpr std::string_view kKasumiResourceProbes[] = {
    "anti-llm",
    "developer-message",
    "jailbreak",
    "prompt injection",
    "PROTECTED BY Kasumi",
};

}   // namespace grunt::knowledge
