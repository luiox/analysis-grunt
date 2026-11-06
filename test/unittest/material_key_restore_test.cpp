/// @file        material_key_restore_test.cpp
/// @brief       残留 `material_key_restore` 的三段判据测试（真值往返 +
///              S1 互证咬合 + 拒绝面反证）。
///
/// 夹具是「缩小版 Grunt v3 RuntimeMaterial 形态」（jasm 现场汇编，不落
/// 二进制、不依赖语料目录）：材料字段 `kf J` / `po I`（private static）+
/// `mk ()I` 逐字 9 条体（createMaterialKeyMethod 生成面）+ 材料解密器
/// `Dec (String;)String`（窗口 `charAt; LDC K; INVOKESTATIC mk ()I; IXOR;
/// IXOR; I2C`）+ simple BSM（名字族配对：解密器名 = BSM 名 B[2, len-1)，
/// grunt3 e41e852 ReferenceObfuscate 生成面，boar zA 实证同形）。
///
/// 初值链与 boar 逐形同源（number-reobf + SPF/IndyRestore 供给链产物）：
/// - keyField：`ldc <基32串>; bipush 32; invokestatic <蹦床>(String;I)J;
///   ldc2_w C; lxor; putstatic kf`（Form B）；蹦体恰为
///   `aload_0; iload_1; invokestatic Long.parseUnsignedLong; lreturn`；
/// - poison：`ldc X; i2l; ldc X; i2l; lxor; l2i; putstatic po`（X^X ≡ 0
///   走廊，boar 实证形）；
/// - 扰动面：clinit 里守卫支路（`INVOKESTATIC gd()I; dup; ifeq clean`）+
///   可疑窗口（keyField LXOR 尾 / poison IOR 尾）——清洁口径下不参与
///   effective key（源锚 RuntimeMaterial.kt perturbation：可疑路径才写
///   keyField/poison；清洁 delta 只碰 shareA/shareB，不进 materialKey）。
///
/// 覆盖分工：
/// - SCAN（真值）：链求值 → 候选 key = fold(keyInit) ^ poisonInit → S1 互证
///   （BSM 族路由的全部 indy 站点按候选 key 解密 + desc 互证）→ 站点成立；
/// - SCAN（S1 咬合反证）：poison 求值面被人为抬高（候选 ≠ 真钥）→ 互证解出
///   垃圾 → keyRefuted 桶；
/// - SCAN（拒绝面）：初值链断（池读未折回）/ 多写冲突 / 字段外写 / 可疑写
///   缺守卫指纹 / mk 体 native 化（boar 70 类同形）→ 逐桶记账；
/// - REWRITE：解密器窗口 INVOKESTATIC 原地换 LDC（srcPc 保留）；拒绝面上
///   容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零。
#include "grunt/material_key_restore.hpp"

#include "grunt/knowledge/grunt_names.hpp"

#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::material_key_restore {
namespace {

namespace model = mjbase::model;

/// 解密器窗口 LDC K（异或前原始 decryptKey；任意值）。
constexpr ca::i32 kRawKey = 0x2A17;
/// keyField 初值 = parseUnsignedLong(基32串, 32) ^ C——夹具侧随意选 M/C，
/// 真值链在测试里 C++ 直解（parseUnsignedLong 语义固定，与 pass 内
/// parseRadixDigits 同一 JVM 语义）。
constexpr ca::u64 kParseMag = 0x3EAD1C0FFEE12345ULL;
constexpr ca::i64 kXorConst = static_cast<ca::i64>(0x2C4A6B8D0E1F3577ULL);
/// poison 扰动窗口的常数（清洁口径不进 key）。
constexpr ca::i64 kBadDelta = 123456789321LL;

const std::string kSimpleBsmDesc(grunt::knowledge::kGruntSimpleBsmDesc);

/// @brief keyField 初值（i64 位型）：parseUnsignedLong(M, 32) ^ C = M ^ C。
constexpr ca::i64 keyInitValue()
{
    return static_cast<ca::i64>(kParseMag ^ static_cast<ca::u64>(kXorConst));
}

/// @brief `(seed ^ (seed ushr 32)).toInt()`（Long.foldToInt，与 pass 内
///        materialFold 同式独立实现——真值面）。
ca::i32 foldToInt(ca::i64 seed)
{
    const ca::u64 x = static_cast<ca::u64>(seed);
    return static_cast<ca::i32>(static_cast<ca::u32>(x ^ (x >> 32)));
}

/// @brief u64 → 基 32 数字串（MSB 在前，13 位；字母表与
///        Long.parseUnsignedLong(s, 32) 一致）。
std::string base32(ca::u64 v)
{
    const char* digits = "0123456789abcdefghijklmnopqrstuv";
    std::string out;
    for (int i = 12; i >= 0; --i) {
        out += digits[(v >> (5 * i)) & 0x1F];
    }
    return out;
}

/// @brief 明文 → 密文的 jasm 字面量（ASCII 码元 XOR key 低 16 位，\uXXXX）。
std::string xorEscape(const std::string& plain, ca::i32 key)
{
    const ca::u16 lo = static_cast<ca::u16>(key);
    std::string out;
    char buf[8];
    for (const char c : plain) {
        const ca::u16 enc = static_cast<ca::u16>(static_cast<ca::u16>(c) ^ lo);
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(enc));
        out += buf;
    }
    return out;
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        assembled = service.assemble(utf16(name + ".jasm").ref(),
                                                             utf16(source).ref());
    if (!assembled.isOk()) {
        std::string detail;
        for (const auto& d : assembled.diagnostics) {
            detail += "\n  " + d.message.to_utf8_string().to_std_string();
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail);
    }
    return std::move(assembled.value->bytes);
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

model::ResourceContainer makeContainer(
    std::initializer_list<std::pair<std::string, std::string>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, source] : classes) {
        const std::string uri = name + ".class";
        container.addResource(
            uri, std::make_unique<model::ClassResource>(uri, assemble(name, source)));
    }
    return container;
}

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

// ============================================================================
// ==== 夹具构造 ==============================================================
// ============================================================================

/// @brief mk() 体：逐字 9 条（createMaterialKeyMethod 生成面）。
std::string mkMethod(const std::string& host, const std::string& kf, const std::string& po)
{
    return "  .method private static mk ()I {\n"
           "    code: {\n"
           "      getstatic " + host + "." + kf + " J\n"
           "      dup2\n"
           "      bipush 32\n"
           "      lushr\n"
           "      lxor\n"
           "      l2i\n"
           "      getstatic " + host + "." + po + " I\n"
           "      ixor\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief mk() 的 native 化变体（boar 70 类同形：native 桥体不在 jar）。
std::string mkMethodNative()
{
    return "  .method private static native mk ()I {\n"
           "    }\n";
}

/// @brief 材料解密器（窗口 `charAt; LDC K; INVOKESTATIC mk ()I; IXOR; IXOR;
///        I2C`）。name 即解密器名。
std::string decMethod(const std::string& host, const std::string& name)
{
    return "  .method private static " + name +
           " (Ljava/lang/String;)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aload 0\n"
           "      iconst_0\n"
           "      invokevirtual java/lang/String.charAt (I)C\n"
           "      ldc " + std::to_string(kRawKey) + "\n"
           "      invokestatic " + host + ".mk ()I\n"
           "      ixor\n"
           "      ixor\n"
           "      i2c\n"
           "      pop\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n";
}

/// @brief parse 蹦床（IndyRestore#1 解直后的产物形）。
std::string trampMethod(const std::string& host)
{
    return "  .method private static tramp (Ljava/lang/String;I)J {\n"
           "    code: {\n"
           "      aload 0\n"
           "      iload 1\n"
           "      invokestatic java/lang/Long.parseUnsignedLong"
           " (Ljava/lang/String;I)J\n"
           "      lreturn\n"
           "    }\n"
           "  }\n";
}

/// @brief 守卫方法（体随意；指纹只钉调用点的 INVOKESTATIC; DUP; IFEQ）。
std::string guardMethod()
{
    return "  .method private static gd ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief simple BSM。名字族配对面：解密器名 = BSM 名 B[2, len-1)。
///        callsDec = 体跟随路（体内 INVOKESTATIC 解密器）。
std::string bsmMethod(const std::string& host, const std::string& bsmName,
                      const std::string& decName, bool callsDec)
{
    std::string body;
    if (callsDec) {
        body += "      aload 3\n"
                "      invokestatic " + host + "." + decName +
                " (Ljava/lang/String;)Ljava/lang/String;\n"
                "      pop\n";
    }
    body += "      aconst_null\n      areturn\n";
    return "  .method public static " + bsmName + " " + kSimpleBsmDesc + " {\n"
           "    code: {\n" + body + "    }\n"
           "  }\n";
}

/// @brief indy 调用点（static 形，实参由调用方给）。
std::string callerMethod(const std::string& host, const std::string& bsmName,
                         const std::string& args)
{
    return "  .method public static caller (I)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      invokedynamic " + bsmName + " (I)I { invokestatic, " + host + "."
           + bsmName + ", " + kSimpleBsmDesc + " } { " + args + " }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief keyField 初值写（Form B：parse 蹦床 + C 异或）。
std::string keyInitSpan(const std::string& host, const std::string& kf)
{
    return "      ldc \"" + base32(kParseMag) + "\"\n"
           "      bipush 32\n"
           "      invokestatic " + host + ".tramp (Ljava/lang/String;I)J\n"
           "      ldc2_w " + std::to_string(kXorConst) + "L\n"
           "      lxor\n"
           "      putstatic " + host + "." + kf + " J\n";
}

/// @brief poison 初值写（X^X ≡ 0 走廊，boar 实证形）。
std::string poisonInitSpan(const std::string& host, const std::string& po)
{
    return "      ldc 305419896\n"
           "      i2l\n"
           "      ldc 305419896\n"
           "      i2l\n"
           "      lxor\n"
           "      l2i\n"
           "      putstatic " + host + "." + po + " I\n";
}

/// @brief clinit 尾段：守卫支路 + 可疑窗口（keyField LXOR 尾 / poison IOR 尾）
///        + clean 收口（RuntimeMaterial.kt perturbation 同形）。guarded =
///        true 时守卫 DUP 的余值由 poison 窗口 SWAP 形吞并（boar 实证形）；
///        false 时省 DUP、poison 窗口用无余值形（守卫指纹缺席 →
///        writerUnguarded 桶）。
std::string perturbationTail(const std::string& host, const std::string& kf,
                             const std::string& po, bool guarded)
{
    std::string out = "      invokestatic " + host + ".gd ()I\n";
    out += guarded ? "      dup\n      ifeq Lclean\n" : "      ifeq Lclean\n";
    out += "      getstatic " + host + "." + kf + " J\n"
           "      ldc2_w " + std::to_string(kBadDelta) + "L\n"
           "      lxor\n"
           "      putstatic " + host + "." + kf + " J\n";
    out += guarded
        ? "      getstatic " + host + "." + po + " I\n"
          "      swap\n"
          "      iconst_5\n"
          "      ior\n"
          "      ior\n"
          "      putstatic " + host + "." + po + " I\n"
        : "      getstatic " + host + "." + po + " I\n"
          "      iconst_5\n"
          "      ior\n"
          "      putstatic " + host + "." + po + " I\n";
    out += "      goto Lend\n"
           "    Lclean:\n";
    out += guarded ? "      pop\n" : "";
    out += "    Lend:\n"
           "      return\n";
    return out;
}

/// simple 夹具（单类）。字段名 kf/po；BSM 名族配对 B="zzDecX" ↔ 解密器
/// "Dec"（B[2, len-1)）。形态开关供拒绝面裁剪。
struct MatFixture
{
    std::string host    = "MatHost";
    std::string kf      = "kf";
    std::string po      = "po";
    std::string decName = "Dec";
    std::string bsmName = "zzDecX";
    bool        mkNative   = false;   ///< mk() native 化（keyBodyMismatch 桶）
    std::string keyInitOverride;        ///< 非空时替换 keyField 初值跨度
    bool        doubleKeyInit = false;  ///< keyField 双初值写（initConflict 桶）
    bool        externalWrite = false;  ///< 非 clinit 方法补一条裸写（writerShape 桶）
    bool        guarded       = true;   ///< 守卫指纹（false → writerUnguarded 桶）
    /// S1 咬合反证：poison 求值面抬高（候选 = fold^poisonEval，真钥 = fold）。
    bool poisonRaised = false;

    std::string source() const
    {
        // 站点实参按「真钥」加密：真钥 = K ^ fold(keyInit)（清洁口径，
        // poison ≡ 0）；候选错时互证必翻车。
        const ca::i32 realKey     = kRawKey ^ foldToInt(keyInitValue());
        const std::string args = "\"" + xorEscape("java.lang.Math", realKey) + "\", \""
                               + xorEscape("abs", realKey) + "\", \""
                               + xorEscape("(I)I", realKey) + "\", 0";

        std::string src;
        src += mkNative ? mkMethodNative() : mkMethod(host, kf, po);
        src += decMethod(host, decName);
        src += trampMethod(host);
        src += guardMethod();
        src += bsmMethod(host, bsmName, decName, true);

        // clinit：初值链 + 扰动面。
        std::string clinit;
        clinit += keyInitOverride.empty() ? keyInitSpan(host, kf) : keyInitOverride;
        if (doubleKeyInit) {
            clinit += "      ldc2_w 7L\n      putstatic " + host + "." + kf + " J\n";
        }
        clinit += poisonRaised
            ? "      iconst_5\n      putstatic " + host + "." + po + " I\n"
            : poisonInitSpan(host, po);
        clinit += perturbationTail(host, kf, po, guarded);
        src += "  .method public static <clinit> ()V {\n    code: {\n" + clinit
             + "    }\n  }\n";

        if (externalWrite) {
            src += "  .method public static extra (I)I {\n"
                   "    code: {\n"
                   "      ldc2_w 7L\n"
                   "      putstatic " + host + "." + kf + " J\n"
                   "      iload 0\n"
                   "      ireturn\n"
                   "    }\n"
                   "  }\n";
        }
        src += callerMethod(host, bsmName, args);
        src += "}\n";
        return ".super java/lang/Object\n.class public super " + host + " {\n"
               "  .field private static " + kf + " J\n"
               "  .field private static " + po + " I\n"
               "\n" + src;
    }
};

/// @brief 断链初值跨度（链序缺 StringPoolFold 的 face：池读未折回）。
std::string poolFedKeyInit(const std::string& host, const std::string& kf)
{
    return "      getstatic " + host + ".pool [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      bipush 32\n"
           "      invokestatic " + host + ".tramp (Ljava/lang/String;I)J\n"
           "      ldc2_w " + std::to_string(kXorConst) + "L\n"
           "      lxor\n"
           "      putstatic " + host + "." + kf + " J\n";
}

/// @brief 拒绝面共用收尾：rewrite 无脏类 + 容器字节逐位不动。
void expectRewriteNoop(model::ResourceContainer& container, const ScanResult& scanned,
                       const std::vector<ca::u8>& before)
{
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(dirty.sites, 0u);
    const std::vector<ca::u8>& after = classBytes(container, "MatHost.class");
    ASSERT_EQ(after.size(), before.size());
    EXPECT_EQ(0, std::memcmp(after.data(), before.data(), before.size()));
}

}   // namespace

// ============================================================================
// ==== SCAN：真值往返 ========================================================
// ============================================================================

TEST(MaterialKeyRestoreScan, TrueValueRoundtripFoldsMaterialKey)
{
    MatFixture fx;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.materialClasses, 1u);
    EXPECT_EQ(result.rejects.size(), 0u);

    // 真值：候选 key = fold(parseUnsignedLong(M, 32) ^ C) ^ poison(0)。
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "MatHost.class");
    EXPECT_EQ(site.decryptorRawKey, kRawKey);
    EXPECT_EQ(site.materialKey, foldToInt(keyInitValue()));
    // S1 互证：族路由 BSM 的 indy 站点按候选 key 解出真值并过 desc 互证。
    EXPECT_EQ(site.sitesVerified, 1u);
}

TEST(MaterialKeyRestoreScan, S1RefutesWrongCandidateKey)
{
    // poison 求值面抬高（iconst_5 走廊）：候选 = fold ^ 5 ≠ 真钥（实参按
    // fold 加密）→ 全站点解出垃圾 → keyRefuted（S1 网真咬合的反证）。
    MatFixture fx;
    fx.poisonRaised = true;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.keyRefuted, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    expectRewriteNoop(container, result, before);
}

// ============================================================================
// ==== SCAN：拒绝面（逐桶 fail-closed）=======================================
// ============================================================================

TEST(MaterialKeyRestoreScan, PoolFedInitChainBlocked)
{
    // 初值跨度仍含池读（链序缺 grunt.StringPoolFold）→ initBlocked。
    MatFixture fx;
    fx.keyInitOverride = poolFedKeyInit(fx.host, fx.kf);
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.initBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("StringPoolFold"), std::string::npos);
    expectRewriteNoop(container, result, before);
}

TEST(MaterialKeyRestoreScan, DoubleKeyInitConflicts)
{
    // keyField 双初值写 → initConflict（多写不猜）。
    MatFixture fx;
    fx.doubleKeyInit = true;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.initConflict, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    expectRewriteNoop(container, result, before);
}

TEST(MaterialKeyRestoreScan, ExternalWriteRejected)
{
    // 非 clinit 方法的裸写（非扰动窗口形）→ writerShape（字段被外写）。
    MatFixture fx;
    fx.externalWrite = true;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.writerShape, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    expectRewriteNoop(container, result, before);
}

TEST(MaterialKeyRestoreScan, SuspiciousWriteWithoutGuardRejected)
{
    // 可疑写在场但守卫指纹缺席（DUP 被摘）→ writerUnguarded。
    MatFixture fx;
    fx.guarded = false;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.writerUnguarded, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    expectRewriteNoop(container, result, before);
}

TEST(MaterialKeyRestoreScan, NativeMaterialKeyBodyMismatch)
{
    // mk() native 化（boar 70 类同形：键不在字节码里）→ keyBodyMismatch。
    MatFixture fx;
    fx.mkNative = true;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});
    const std::vector<ca::u8>& before = classBytes(container, "MatHost.class");

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.keyBodyMismatch, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    expectRewriteNoop(container, result, before);
}

// ============================================================================
// ==== REWRITE + PROOF =======================================================
// ============================================================================

TEST(MaterialKeyRestoreRewrite, SwapsInvokestaticForLdcInPlace)
{
    MatFixture fx;
    model::ResourceContainer container = makeContainer({{fx.host, fx.source()}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.skippedByRoundTrip, 0u);

    // 原地面：解密器窗口里 mk() 调用换成常量装载（mk 零调用点，交下游死码
    // 回收）；窗口随即变成 indy_restore 的 reobf 折叠键形。
    const std::string jasm = disassemble(classBytes(container, "MatHost.class"));
    EXPECT_EQ(jasm.find("invokestatic MatHost.mk ()I"), std::string::npos);
    EXPECT_NE(jasm.find(std::to_string(scanned.sites.front().materialKey)),
              std::string::npos);

    // PROOF：类数不增 + 脏类可重解析 + residual（仍带窗口形的类）归零。
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

// ============================================================================

}   // namespace grunt::material_key_restore
