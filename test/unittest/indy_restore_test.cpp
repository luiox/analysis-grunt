/// @file        indy_restore_test.cpp
/// @brief       残留 `indy_restore` 的三段判据测试（真值往返 + 键链三路 +
///              heavy @Metadata 互证 + 拒绝面反证）。
///
/// 夹具是「缩小版 Grunt v2/v3 indy 形态」（jasm 现场汇编，不落二进制、不
/// 依赖语料目录）：每类自带 7 参 BSM（desc 逐字取 knowledge 的
/// kGruntSimpleBsmDesc / kGruntHeavyBsmDesc）+ `(String;)String` XOR 解密器
/// （单层 `charAt; LDC K; IXOR; i2c` 窗口）；密文串以 \uXXXX 转义落 jasm 源
/// （encrypt 自逆：夹具侧用同一 XOR 造密文，C++ 侧解出真值 owner/name/desc）。
///
/// 键链三路各一夹具：BSM 体跟随（v2/v3 常态）、同名直连（v3 plainDecryptName
/// == plainBsmName）、名字族推导（BSM 名 B 的解密器名 = B[2, len-1)——grunt3
/// e41e852 ReferenceObfuscate.kt L216 生成面）+ c2 材料捐赠（plain 解密器壳
/// 死、material 解密器 LDC 同一原始 key——源锚 createDecryptMethod 两分支同
/// 一 decryptKey）。heavy 夹具：owner 类挂 @Metadata（mv=100 + d1/d2 数组，
/// d1 = encrypt("59557*q+magic2", magic2)、d2 = encrypt("name<>desc", magic1)），
/// BSM 体 LDC generalKey。
///
/// 覆盖分工：
/// - SCAN：键链三路 + 解密 + desc 互证 → 站点成立（static/virtual 两态）；
///   参数畸形 / kind 位越界 / desc 互证不符 / Era 未判 / magic 互证失配 /
///   owner 无 @Metadata / 接口无旁证 → 拒绝面逐桶记账；
/// - REWRITE：static 原地换 invokestatic；virtual 原地换 invokevirtual /
///   invokeinterface（纯原地：receiver 槽就是 indy 首参）；拒绝面上容器字节
///   逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零。
#include "grunt/indy_restore.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::indy_restore {
namespace {

namespace model     = mjbase::model;
namespace knowledge = grunt::knowledge;

/// simple 夹具解密 key（任意 16 位面值；encrypt 自逆）。
constexpr ca::i32 kKey = 0x2A17;
/// heavy 夹具三键：generalKey（BSM 体 LDC）、magic1（d2 解密）、magic2（d1
/// 解密 + data1 自证模数）。
constexpr ca::i32 kGeneralKey = 777;
constexpr ca::i32 kMagic1     = 4242;
constexpr ca::i32 kMagic2     = 2345;
/// data1 = 59557 * q + magic2（q=7）——d1 解出后 `data1 % 59557 == magic2`
/// 必须自证命中。
constexpr ca::i64 kData1 = 59557LL * 7 + kMagic2;

const std::string kSimpleBsmDesc(knowledge::kGruntSimpleBsmDesc);
const std::string kHeavyBsmDesc(knowledge::kGruntHeavyBsmDesc);

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief 明文 → 密文的 jasm 字面量：每个 ASCII 码元 XOR key 低 16 位后落
///        `\uXXXX` 转义（夹具明文全 ASCII，异或产物不进增补平面/代理区）。
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

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        assembled  = service.assemble(utf16(name + ".jasm").ref(),
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

/// @brief (String;)String 解密器方法体。shape：
///        plain    = 单层窗口 `charAt; LDC K; IXOR; I2C`；
///        material = v3 材料通道 `charAt; LDC K; INVOKESTATIC mk ()I; IXOR;
///                   IXOR; I2C`（K 是异或前的原始 decryptKey）；
///        dead     = 无窗口体（family 推导的「壳死家族证据」）；
///        none     = 方法不在场。
std::string decMethod(const std::string& hostName, const std::string& decName,
                      const std::string& shape)
{
    if (shape == "none") return "";
    std::string body;
    if (shape == "dead") {
        body = "      aconst_null\n      areturn\n";
    } else {
        body = "      aload 0\n"
               "      iconst_0\n"
               "      invokevirtual java/lang/String.charAt (I)C\n"
               "      ldc " + std::to_string(kKey) + "\n";
        if (shape == "material") {
            body += "      invokestatic " + hostName + ".mk ()I\n";
        }
        body += "      ixor\n";
        if (shape == "material") {
            body += "      ixor\n";
        }
        body += "      i2c\n"
                "      pop\n"
                "      aconst_null\n"
                "      areturn\n";
    }
    return "  .method private static " + decName +
           " (Ljava/lang/String;)Ljava/lang/String; {\n"
           "    code: {\n" + body + "    }\n"
           "  }\n";
}

/// @brief v3 材料键方法（`LUSHR 32; LXOR; IXOR` 的运行期材料面——静态不可解，
///        夹具只供形状归属）。
std::string mkMethod()
{
    return "  .method private static mk ()I {\n"
           "    code: {\n"
           "      bipush 9\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief simple BSM。`callsDec` = 体跟随路（体内首条 INVOKESTATIC 本类
///        (String;)String）；false = 体不暴露解密器（族推导路）。
std::string simpleBsmMethod(const std::string& hostName, const std::string& bsmName,
                            bool callsDec, const std::string& decName)
{
    std::string body;
    if (callsDec) {
        body += "      aload 3\n"
                "      invokestatic " + hostName + "." + decName +
                " (Ljava/lang/String;)Ljava/lang/String;\n"
                "      pop\n";
    }
    body += "      aconst_null\n      areturn\n";
    return "  .method public static " + bsmName + " " + kSimpleBsmDesc + " {\n"
           "    code: {\n" + body + "    }\n"
           "  }\n";
}

/// @brief indy 调用点方法。indy desc / 实参由调用方给（static 形 `(I)I` 配
///        iload 0，virtual 形 `(Ljava/lang/Object;)X` 配 aload 0）。
std::string callerMethod(const std::string& hostName, const std::string& bsmName,
                         const std::string& callerDesc, const std::string& load,
                         const std::string& indyDesc, const std::string& indyArgs)
{
    return "  .method public static caller " + callerDesc + " {\n"
           "    code: {\n"
           "      " + load + "\n"
           "      invokedynamic " + bsmName + " " + indyDesc + " { invokestatic, " + hostName +
           "." + bsmName + ", " + kSimpleBsmDesc + " } { " + indyArgs + " }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief simple 形夹具（单类）。static 站点实参默认解出 java.lang.Math.abs
///        (I)I；virtual 站点实参默认解出 java.util.Iterator.hasNext ()Z。
struct SimpleFixture
{
    std::string name    = "GruntHost";
    std::string bsmName = "bsm";
    std::string decName = "dec";
    std::string decShape = "plain";          ///< plain / material / dead / none
    std::string deadDecName;                 ///< 壳死 (String;)String（c2 家族证据）
    bool        bsmCallsDec = true;          ///< 路由 2（体跟随）
    bool        withMk = false;              ///< material 解密器的 ()I 伴生方法
    std::string staticArgs;
    std::string virtualArgs;
    std::string virtualIndyDesc = "(Ljava/lang/Object;)Z";   ///< 须与目标 ret 同型

    std::string source() const
    {
        std::string src;
        if (decShape == "material") src += mkMethod();
        if (!deadDecName.empty()) src += decMethod(name, deadDecName, "dead");
        src += decMethod(name, decName, decShape);
        src += simpleBsmMethod(name, bsmName, bsmCallsDec, decName);
        if (!staticArgs.empty()) {
            src += callerMethod(name, bsmName, "(I)I", "iload 0", "(I)I", staticArgs);
        }
        if (!virtualArgs.empty()) {
            src += callerMethod(name, bsmName, virtualIndyDesc, "aload 0",
                                virtualIndyDesc, virtualArgs);
        }
        src += "}\n";
        return ".super java/lang/Object\n.class public super " + name + " {\n" + src;
    }
};

/// @brief static 站点实参（真值：owner=java.lang.Math name=abs desc=(I)I）。
std::string staticArgsOk(ca::i32 key = kKey)
{
    return "\"" + xorEscape("java.lang.Math", key) + "\", \"" + xorEscape("abs", key)
           + "\", \"" + xorEscape("(I)I", key) + "\", 0";
}

/// @brief virtual 站点实参（真值：owner=java.util.Iterator name=hasNext
///        desc=()Z；JDK 接口走注入面旁证）。
std::string virtualArgsOk(ca::i32 key = kKey)
{
    return "\"" + xorEscape("java.util.Iterator", key) + "\", \"" + xorEscape("hasNext", key)
           + "\", \"" + xorEscape("()Z", key) + "\", 1";
}

/// @brief heavy BSM + heavy 解密器（体面不参与判据——generalKey 在 BSM 体
///        `LDC K; INVOKESTATIC (String;I)String` 窗口里）。
std::string heavyMethods(const std::string& hostName, const std::string& bsmName)
{
    return "  .method public static " + bsmName + " " + kHeavyBsmDesc + " {\n"
           "    code: {\n"
           "      aload 3\n"
           "      ldc " + std::to_string(kGeneralKey) + "\n"
           "      invokestatic " + hostName +
           ".hdec (Ljava/lang/String;I)Ljava/lang/String;\n"
           "      pop\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method private static hdec (Ljava/lang/String;I)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n";
}

/// @brief heavy indy 调用点（static 形）。
std::string heavyCaller(const std::string& hostName, const std::string& bsmName,
                        const std::string& args)
{
    return "  .method public static heavyCaller (I)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      invokedynamic " + bsmName + " (I)I { invokestatic, " + hostName + "."
           + bsmName + ", " + kHeavyBsmDesc + " } { " + args + " }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n";
}

/// @brief heavy 实参（真值：owner=MetaOwner 经 generalKey 一层；magic 对由
///        kData1/kMagic1/kMagic2 互证）。
std::string heavyArgsOk(ca::i32 generalKey = kGeneralKey)
{
    return "\"" + xorEscape("MetaOwner", generalKey) + "\", " + std::to_string(kMagic1)
           + ", " + std::to_string(kMagic2) + ", 0";
}

/// @brief heavy 目标类：类级 @Metadata（mv=100 + d1/d2 各一项）+ abs 方法。
///        d1[0] = encrypt(data1 十进制串, magic2)、d2[0] = encrypt("abs<>(I)I",
///        magic1)。
std::string metaOwnerSource(ca::i64 data1 = kData1, ca::i32 magic1 = kMagic1,
                            ca::i32 magic2 = kMagic2)
{
    const std::string d1 = xorEscape(std::to_string(data1), magic2);
    const std::string d2 = xorEscape("abs<>(I)I", magic1);
    return ".visible-annotation net/spartanb312/grunteon/internal/Metadata {\n"
           "    mv: 100,\n"
           "    d1: { \"" + d1 + "\" },\n"
           "    d2: { \"" + d2 + "\" }\n"
           "}\n"
           ".super java/lang/Object\n"
           ".class public super MetaOwner {\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static abs (I)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief @Metadata 注解类（mv/d1/d2 抽象方法三件套）。判据只看方法形状
///        （名字 + 描述符），ACC_ANNOTATION 位不在 gate 内——夹具用普通类
///        承载（jasm 面等价：方法表逐字同源）。
std::string metadataAnnoClassSource()
{
    return ".super java/lang/Object\n"
           ".class public super net/spartanb312/grunteon/internal/Metadata {\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public mv ()I {\n"
           "    code: {\n"
           "      bipush 100\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public d1 ()[Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public d2 ()[Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 容器内 virtual 目标类（invokevirtual 分派反证：非接口类）。
std::string friendSource()
{
    return ".super java/lang/Object\n"
           ".class public super Friend {\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public foo ()I {\n"
           "    code: {\n"
           "      bipush 1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

ScanInput gruntInput(knowledge::Era era = knowledge::Era::Grunt3,
                     std::unordered_set<std::string> jdkInterfaces
                     = std::unordered_set<std::string>{})
{
    ScanInput input;
    input.era            = era;
    input.jdkInterfaces  = std::move(jdkInterfaces);
    return input;
}

/// @brief 容器内 virtual 实参（真值：owner=Friend name=foo desc=()I）。
std::string virtualArgsFriend(ca::i32 key = kKey)
{
    return "\"" + xorEscape("Friend", key) + "\", \"" + xorEscape("foo", key) + "\", \""
           + xorEscape("()I", key) + "\", 1";
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(IndyRestoreScan, SimpleSiteRestoredThroughBsmBodyFollow)
{
    // 路由 2（v2/v3 常态）：BSM 体首条 INVOKESTATIC 即解密器。
    SimpleFixture fx;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container =
        makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.totalIndy, 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "GruntHost.class");
    EXPECT_EQ(site.form, Form::Static);
    EXPECT_EQ(site.targetOwner, "java/lang/Math");
    EXPECT_EQ(site.targetName, "abs");
    EXPECT_EQ(site.targetDesc, "(I)I");
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyRestoreScan, SimpleSiteRestoredThroughSameNameDecryptor)
{
    // 路由 1（v3 material 配对类）：plainDecryptName == plainBsmName——BSM 与
    // 解密器同名，体不跟随也成立。
    SimpleFixture fx;
    fx.bsmName    = "dec";
    fx.decName    = "dec";
    fx.bsmCallsDec = false;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().targetName, "abs");
}

TEST(IndyRestoreScan, SimpleSiteRestoredThroughNameFamily)
{
    // 路由 3 c1（NativePipeline 壳现场）：BSM 体不暴露解密器，族推导
    // decryptName = BSM 名 B[2, len-1)——BSM "zzQAbcd" ↔ 解密器 "QAbc"。
    SimpleFixture fx;
    fx.bsmName    = "zzQAbcd";
    fx.decName    = "QAbc";
    fx.bsmCallsDec = false;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().targetName, "abs");
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyRestoreScan, PlainSiteRestoredThroughMaterialDonor)
{
    // 路由 3 c2（plain 解密器壳死 + material 解密器在场）：BSM 名 N="qbaBc"
    // 与壳死 (String;)String 同名，material 解密器 M="baBcXY" 满足
    // len(M)==len(N)+1 且 M[0,len(N)-1)==N[1,)——M 的 LDC 即原始 decryptKey
    // （源锚 createDecryptMethod 两分支同一 decryptKey）。
    SimpleFixture fx;
    fx.bsmName    = "qbaBc";
    fx.decName    = "baBcXY";
    fx.decShape   = "material";
    fx.deadDecName = "qbaBc";
    fx.bsmCallsDec = false;
    fx.withMk     = true;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().targetName, "abs");
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyRestoreScan, MaterialLaneSiteFailsClosed)
{
    // v3 材料通道（BSM 名 B="qqAbcd" 的族解密器 D=B[2,len-1)="Abc"）：有效
    // key 依赖运行期材料字段——fail-closed 记 materialLane，不猜。
    SimpleFixture fx;
    fx.bsmName    = "qqAbcd";
    fx.decName    = "Abc";
    fx.decShape   = "material";
    fx.bsmCallsDec = false;
    fx.withMk     = true;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.totalIndy, 1u);
    EXPECT_EQ(result.materialLane, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
}

TEST(IndyRestoreScan, VirtualSiteDispatchesInvokevirtualInJar)
{
    // 容器普查旁证（权威面）：目标类在 jar 内且非接口 → invokevirtual。
    SimpleFixture fx;
    fx.staticArgs.clear();
    fx.virtualArgs     = virtualArgsFriend();
    fx.virtualIndyDesc = "(Ljava/lang/Object;)I";   // 目标 ()I：ret 必须同型
    model::ResourceContainer container =
        makeContainer({{fx.name, fx.source()}, {"Friend", friendSource()}});

    const ScanResult result = scan(container, gruntInput());
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.form, Form::Virtual);
    EXPECT_EQ(site.targetOwner, "Friend");
    EXPECT_EQ(site.targetName, "foo");
    EXPECT_FALSE(site.ownerIsInterface);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyRestoreScan, VirtualSiteDispatchesInvokeinterfaceViaJdkSet)
{
    // JDK 注入面旁证：目标在穷举接口集 → invokeinterface（成员与否双向作证）。
    SimpleFixture fx;
    fx.staticArgs.clear();
    fx.virtualArgs = virtualArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result =
        scan(container, gruntInput(knowledge::Era::Grunt3,
                                   std::unordered_set<std::string>{"java/util/Iterator"}));
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_TRUE(result.sites.front().ownerIsInterface);
    EXPECT_EQ(result.sites.front().targetOwner, "java/util/Iterator");
}

TEST(IndyRestoreScan, HeavySiteRestoredThroughMetadataInterlock)
{
    // heavy 面：generalKey 解 owner → owner 类 @Metadata（mv=100）→
    // data1 % 59557 == magic2 自证命中 → d2 解出 name<>desc → desc 互证。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});

    const ScanResult result = scan(container, gruntInput(knowledge::Era::Grunt3));
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.totalIndy, 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "GruntHeavyHost.class");
    EXPECT_EQ(site.form, Form::Static);
    EXPECT_EQ(site.targetOwner, "MetaOwner");
    EXPECT_EQ(site.targetName, "abs");
    EXPECT_EQ(site.targetDesc, "(I)I");
    EXPECT_EQ(result.magicMismatch, 0u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(IndyRestoreScan, RejectsSiteWhenArgsMalformed)
{
    // 静态参数第 4 位给 String（应为 Integer kind 位）：种类不符。
    SimpleFixture fx;
    fx.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                    + xorEscape("abs", kKey) + "\", \"" + xorEscape("(I)I", kKey)
                    + "\", \"x\"";
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.malformedArguments, 1u);
}

TEST(IndyRestoreScan, RejectsSiteWhenKindBitIllegal)
{
    // kind 位越界（生成面只发 0/1，给 2）：fail-closed 拒绝。
    SimpleFixture fx;
    fx.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                    + xorEscape("abs", kKey) + "\", \"" + xorEscape("(I)I", kKey) + "\", 2";
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.decodeFailed, 1u);
}

TEST(IndyRestoreScan, RejectsSiteWhenDescCrossCheckFails)
{
    // 解密 desc 与 indy desc 互证不符（(II)V vs (I)I）：S1 拒绝。
    SimpleFixture fx;
    fx.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                    + xorEscape("abs", kKey) + "\", \"" + xorEscape("(II)V", kKey) + "\", 0";
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.descMismatch, 1u);
}

TEST(IndyRestoreScan, RejectsVirtualSiteWithoutInterfaceEvidence)
{
    // 接口集空 + 目标不在容器 = 无旁证：fail-closed（invokevirtual 误调接口
    // 方法运行时 IncompatibleClassChangeError）。
    SimpleFixture fx;
    fx.staticArgs.clear();
    fx.virtualArgs = virtualArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});

    const ScanResult result = scan(container, gruntInput());
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.interfaceUnattested, 1u);
}

TEST(IndyRestoreScan, RejectsHeavySiteWhenEraNotGrunt3)
{
    // heavy 面挂 Era 门：Era 未判 Grunt3（此处 Kasumi）整面关闭。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});

    const ScanResult result = scan(container, gruntInput(knowledge::Era::Kasumi));
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.totalIndy, 1u);
    EXPECT_EQ(result.metadataMissing, 1u);
}

TEST(IndyRestoreScan, RejectsHeavySiteWhenMagicCrossCheckFails)
{
    // d1 解出 100000：100000 % 59557 == 40443 != magic2——完整性自证不命中。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource(100000)},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});

    const ScanResult result = scan(container, gruntInput(knowledge::Era::Grunt3));
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.magicMismatch, 1u);
}

TEST(IndyRestoreScan, RejectsHeavySiteWhenOwnerMetadataMissing)
{
    // owner 类不在容器（无 @Metadata 可读）：heavy 面无解释材料，fail-closed。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});

    const ScanResult result = scan(container, gruntInput(knowledge::Era::Grunt3));
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.metadataMissing, 1u);
}

TEST(IndyRestoreScan, RejectsHeavySiteWhenGeneralKeyMissing)
{
    // heavy BSM 壳死（NativePipeline 摘体）：generalKey 不在场，keyMissing。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});
    // 把 heavy BSM 体换成无 key 窗口的形状（重开容器直接改字节面太绕：这里
    // 用第二类承载「BSM 体不暴露 generalKey」的形状）。
    model::ResourceContainer shell = makeContainer(
        {{"GruntHeavyShell",
          ".super java/lang/Object\n.class public super GruntHeavyShell {\n"
          "  .method public static hbsm " + kHeavyBsmDesc + " {\n"
          "    code: {\n"
          "      aconst_null\n"
          "      areturn\n"
          "    }\n"
          "  }\n"
          "  .method private static hdec (Ljava/lang/String;I)Ljava/lang/String; {\n"
          "    code: {\n"
          "      aconst_null\n"
          "      areturn\n"
          "    }\n"
          "  }\n"
              + heavyCaller("GruntHeavyShell", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});

    const ScanResult result = scan(shell, gruntInput(knowledge::Era::Grunt3));
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.keyMissing, 1u);
    (void)container;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(IndyRestoreRewrite, SwapsStaticIndyForInvokestatic)
{
    SimpleFixture fx;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});
    const ScanResult scanned = scan(container, gruntInput());
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.rewrittenStatic, 1u);
    EXPECT_EQ(dirty.rewrittenVirtual, 0u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string after = disassemble(classBytes(container, "GruntHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokestatic java/lang/Math.abs (I)I"), std::string::npos) << after;
}

TEST(IndyRestoreRewrite, SwapsVirtualIndyForInvokevirtualInPlace)
{
    // 纯原地改写（v2/v3 virtual 的 receiver 槽就是 indy 首参，不插 spill）：
    // 目标类在容器内 → invokevirtual。
    SimpleFixture fx;
    fx.staticArgs.clear();
    fx.virtualArgs     = virtualArgsFriend();
    fx.virtualIndyDesc = "(Ljava/lang/Object;)I";   // 目标 ()I：ret 必须同型
    model::ResourceContainer container =
        makeContainer({{fx.name, fx.source()}, {"Friend", friendSource()}});
    const ScanResult scanned = scan(container, gruntInput());
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.rewrittenVirtual, 1u);
    EXPECT_EQ(dirty.rewrittenStatic, 0u);

    const std::string after = disassemble(classBytes(container, "GruntHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokevirtual Friend.foo ()I"), std::string::npos) << after;
    // 原地换指令不插指令：aload 0 之后直接是 invoke（无 spill/checkcast）。
    EXPECT_EQ(after.find("checkcast"), std::string::npos) << after;
}

TEST(IndyRestoreRewrite, SwapsVirtualIndyForInvokeinterfaceViaJdkSet)
{
    SimpleFixture fx;
    fx.staticArgs.clear();
    fx.virtualArgs = virtualArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});
    const ScanResult scanned =
        scan(container, gruntInput(knowledge::Era::Grunt3,
                                   std::unordered_set<std::string>{"java/util/Iterator"}));
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_TRUE(scanned.sites.front().ownerIsInterface);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.rewrittenVirtual, 1u);

    const std::string after = disassemble(classBytes(container, "GruntHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokeinterface java/util/Iterator.hasNext ()Z"), std::string::npos)
        << after;
}

TEST(IndyRestoreRewrite, SwapsHeavyIndyForInvokestatic)
{
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});
    const ScanResult scanned = scan(container, gruntInput(knowledge::Era::Grunt3));
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.rewrittenStatic, 1u);

    const std::string after = disassemble(classBytes(container, "GruntHeavyHost.class"));
    EXPECT_EQ(after.find("invokedynamic"), std::string::npos) << after;
    EXPECT_NE(after.find("invokestatic MetaOwner.abs (I)I"), std::string::npos) << after;
}

TEST(IndyRestoreRewrite, LeavesContainerUntouchedOnRejections)
{
    // 拒绝面四态（参数畸形 / kind 位越界 / desc 互证不符 / 接口无旁证）：
    // 字节逐位不动。
    const SimpleFixture bad = [] {
        SimpleFixture f;
        f.name       = "BadArgsHost";
        f.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                       + xorEscape("abs", kKey) + "\", \"" + xorEscape("(I)I", kKey)
                       + "\", \"x\"";
        return f;
    }();
    const SimpleFixture kindBit = [] {
        SimpleFixture f;
        f.name       = "KindBitHost";
        f.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                       + xorEscape("abs", kKey) + "\", \"" + xorEscape("(I)I", kKey) + "\", 2";
        return f;
    }();
    const SimpleFixture badDesc = [] {
        SimpleFixture f;
        f.name       = "BadDescHost";
        f.staticArgs = "\"" + xorEscape("java.lang.Math", kKey) + "\", \""
                       + xorEscape("abs", kKey) + "\", \"" + xorEscape("(II)V", kKey)
                       + "\", 0";
        return f;
    }();
    const SimpleFixture noIface = [] {
        SimpleFixture f;
        f.name        = "NoIfaceHost";
        f.staticArgs.clear();
        f.virtualArgs = virtualArgsOk();
        return f;
    }();

    std::vector<std::pair<std::string, std::string>> fixtures;
    fixtures.push_back({bad.name, bad.source()});
    fixtures.push_back({kindBit.name, kindBit.source()});
    fixtures.push_back({badDesc.name, badDesc.source()});
    fixtures.push_back({noIface.name, noIface.source()});

    std::vector<std::vector<ca::u8>> before;
    model::ResourceContainer         container;
    for (const auto& [name, source] : fixtures) {
        const std::string         uri   = name + ".class";
        const std::vector<ca::u8> bytes = assemble(name, source);
        before.push_back(bytes);
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    }

    const ScanResult scanned = scan(container, gruntInput());
    ASSERT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.malformedArguments, 1u);
    EXPECT_EQ(scanned.decodeFailed, 1u);
    EXPECT_EQ(scanned.descMismatch, 1u);
    EXPECT_EQ(scanned.interfaceUnattested, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 0u);
    EXPECT_TRUE(dirty.uris.empty());

    for (ca::usize i = 0; i < fixtures.size(); ++i) {
        EXPECT_EQ(classBytes(container, fixtures[i].first + ".class"), before[i])
            << fixtures[i].first << " 被改写";
    }
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(IndyRestoreProof, KeepsClassCountReparsesAndResidualZero)
{
    // static + virtual + heavy 三态全还原：类数不增、脏类可重解析、residual 0
    // （Grunt 指纹 BSM 已无 indy 引用——BSM 条目本身留在罐里交下游清理面）。
    model::ResourceContainer container = makeContainer(
        {{"GruntHost",
          [&] {
              SimpleFixture fx;
              fx.staticArgs       = staticArgsOk();
              fx.virtualArgs      = virtualArgsFriend();
              fx.virtualIndyDesc  = "(Ljava/lang/Object;)I";
              return fx.source();
          }()},
         {"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"Friend", friendSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});
    const ca::usize before = 5;
    const ScanResult scanned = scan(container, gruntInput());
    ASSERT_EQ(scanned.sites.size(), 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 3u);
    EXPECT_EQ(dirty.classes, 2u);
    EXPECT_EQ(dirty.rewrittenStatic, 2u);
    EXPECT_EQ(dirty.rewrittenVirtual, 1u);

    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

// ============================================================================
// ==== Era（grunt3 判据随本批落 era.hpp，顺手在此钉双指纹面）==================
// ============================================================================

TEST(IndyRestoreEra, MetadataPlusBsmDescHitsGrunt3)
{
    // Grunt3 双指纹：BSM desc + 类级 @Metadata（注解类命名空间也命中）。
    model::ResourceContainer container = makeContainer(
        {{"GruntHeavyHost",
          ".super java/lang/Object\n.class public super GruntHeavyHost {\n"
              + heavyMethods("GruntHeavyHost", "hbsm")
              + heavyCaller("GruntHeavyHost", "hbsm", heavyArgsOk()) + "}\n"},
         {"MetaOwner", metaOwnerSource()},
         {"net/spartanb312/grunteon/internal/Metadata", metadataAnnoClassSource()}});
    EXPECT_EQ(knowledge::probeEra(container), knowledge::Era::Grunt3);
}

TEST(IndyRestoreEra, BsmDescWithoutAnnoOrNamespaceStaysUnknown)
{
    // 只有 BSM desc 指纹（无 @Metadata、无 grunteon 命名空间）：单一半边不判。
    SimpleFixture fx;
    fx.staticArgs = staticArgsOk();
    model::ResourceContainer container = makeContainer({{fx.name, fx.source()}});
    EXPECT_EQ(knowledge::probeEra(container), knowledge::Era::Unknown);
}

}   // namespace
}   // namespace grunt::indy_restore
