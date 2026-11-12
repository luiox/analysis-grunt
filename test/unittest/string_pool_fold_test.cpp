/// @file        string_pool_fold_test.cpp
/// @brief       残留 `string_pool_fold` 的三段判据测试（真算法往返 + 拒绝面
///              反证）。
///
/// 夹具是缩小版 Grunt v2/v3 字符串池形态（jasm 现场汇编，不落二进制、
/// 不依赖语料目录）：每类带池字段 + arrayInit + **真算法解密器**（体是
/// grunt2@36c0eef createDecryptMethod 的逐字生成面，非骨架桩），调用点各一。
///
/// 真算法往返的证据链（无循环论证）：
/// 1. 密文由 test/tools/GruntStringEncryptRef.java 产出，该文件的 encrypt()
///    从 Grunt 源码逐行转写（JVM int 语义原生），对明文跑真加密；
/// 2. 夹具把密文（literal ldc / arrayed CASTORE）、seed、key 嵌进 arrayInit，
///    classKey 嵌进解密器体首条 ldc；
/// 3. 本仓转写的解密算法（gruntCharXorStream，与 1 同源独立转写）在 scan 里
///    对密文解出明文，断言 == 1 步的输入明文。两份转写一正一逆、各自独立
///    成文，任何 JVM int 语义偏差都会在这一步爆。
///
/// 覆盖分工：
/// - SCAN：v2 literal / v3 arrayed 两形态站点定案；下标非常量（静默跳过）、
///   池字段形态不符（无前导，静默跳过）、解密器缺失 / 解密器体非生成面、
///   arrayed 码元写序列断链、seed 缺失 → 拒绝面逐桶计数；
/// - REWRITE：两形态锚点原地换 ldc + 三元组擦除；**池字段/arrayInit/解密器
///   不删**（改写后仍在场）；拒绝面上容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零（未跑 rewrite 时如实
///   报本可折叠站点数）。
#include "grunt/string_pool_fold.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::string_pool_fold {
namespace {

namespace model = mjbase::model;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        sourceName = utf16(name + ".jasm");
    auto                        text       = utf16(source);
    auto                        assembled  = service.assemble(sourceName.ref(), text.ref());
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
// ==== 夹具数据（GruntStringEncryptRef.java 产出的真密文）====================
// ============================================================================
//
// classKey v2 = -1272827931；classKey v3 = -1722001081（两代各自随机）。
// cipher 由参照加密器对明文加密得到（JVM 语义），fold 产物必须还原为明文。

/// @brief 解密器真体（grunt2@36c0eef createDecryptMethod 逐字生成面：
///        init 四连 + 循环头 + 52 条核 + NEW String 收尾）。classKey 烧在
///        首条 ldc；槽位 0=[C 1-2=J seed 3=I key 4=state 5=i 6=tmp（生成面
///        布局）。
std::string decryptorBody(const std::string& className, const std::string& classKeyLdc)
{
    return "  .method private static dec ([CJI)Ljava/lang/String; {\n"
           "    code: {\n"
           "      " + classKeyLdc + "\n"           // INT(classKey)，init 窗口头
           "      iload_3\n"                       // key 参数（desc ([CJI) 槽 3）
           "      ixor\n"
           "      istore 4\n"                      // state n2
           "      iconst_0\n"
           "      istore 5\n"                      // counter i
           "    Lloop:\n"
           "      iload 5\n"                       // 循环头窗口：bound = len
           "      aload_0\n"
           "      arraylength\n"
           "      if_icmpge Ldone\n"
           "      iload 4\n"                       // 核 1-9：state ^= (int)seed ^ ~i
           "      lload_1\n"
           "      l2i\n"
           "      ixor\n"
           "      iload 5\n"
           "      iconst_m1\n"
           "      ixor\n"
           "      ixor\n"
           "      istore 4\n"
           "      iload 4\n"                       // 核 10-18：state ^= key - i*len
           "      iload_3\n"
           "      iload 5\n"
           "      aload_0\n"
           "      arraylength\n"
           "      imul\n"
           "      isub\n"
           "      ixor\n"
           "      istore 4\n"
           "      iload 4\n"                       // 核 19-25：state = (-state*key) | i
           "      ineg\n"
           "      iload_3\n"
           "      imul\n"
           "      iload 5\n"
           "      ior\n"
           "      istore 4\n"
           "      aload_0\n"                       // 核 26-34：chars[i] ^= state
           "      iload 5\n"
           "      aload_0\n"
           "      iload 5\n"
           "      caload\n"
           "      iload 4\n"
           "      ixor\n"
           "      i2c\n"
           "      castore\n"
           "      iload 5\n"                       // 核 35-38：n3 = i & 0xFF
           "      sipush 255\n"
           "      iand\n"
           "      istore 6\n"
           "      iload_3\n"                       // 核 39-47：key rotl(n3)
           "      iload 6\n"
           "      ishl\n"
           "      iload_3\n"
           "      iload 6\n"
           "      ineg\n"
           "      iushr\n"
           "      ior\n"
           "      istore_3\n"
           "      lload_1\n"                       // 核 48-52：seed ^= n3
           "      iload 6\n"
           "      i2l\n"
           "      lxor\n"
           "      lstore_1\n"
           "      iinc 5 1\n"
           "      goto Lloop\n"
           "    Ldone:\n"
           "      new java/lang/String\n"
           "      dup\n"
           "      aload_0\n"
           "      invokespecial java/lang/String.<init> ([C)V\n"
           "      areturn\n"
           "    }\n"
           "  }\n";
}

/// @brief v2 literal 密文形态夹具（Arrayed=false 默认形；cipher 来自参照
///        加密器）。classKey = -1272827931。
std::string v2LiteralSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_2\n"                     // 池分配前导四连（判据锚点）
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      ldc \"\\ue12e\\u3e64\\u245e\\ubfff\\uf2cb\\u205aA\\ue3cd\\ue319\\u0847^\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      ldc2_w 889226191L\n"            // per-string seed
           "      ldc 17202\n"                    // per-string key
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_1\n"
           "      ldc \"\\u7085\\u3c91\\u895b\\ue0cd\\u3550\\u4440\\u8049\\ue3a9\\ubd90\\u7179\\ue045\\u2524\\u4420\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      ldc2_w 889226192L\n"
           "      ldc 17203\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n" + decryptorBody(name, "ldc -1272827931") +
           "\n"
           "  .method public static read0 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"                     // 判据锚点：三元组（GETSTATIC +
           "      aaload\n"                       //   紧邻常量下标 + AALOAD）
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static read1 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      bipush 1\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 正例夹具（直线蹦床形）：clinit 的 arrayInit 调用隔一层纯结构静态
///        蹦床（体恰为 INVOKESTATIC ai ()V + RETURN），上游 indy 还原后的
///        产物形（boar 语料验证：Grunteon 全链把该调用藏进自家 indy 机器，
///        上游还原后即此形）。池链判定应跟一跳到达真 arrayInit。
std::string v2TrampolineSource(const std::string& name)
{
    std::string src = v2LiteralSource(name);
    const std::string clinitTail = "      invokestatic " + name + ".ai ()V\n"
                                   "      return\n"
                                   "    }\n"
                                   "  }\n";
    const ca::usize at = src.find(clinitTail);
    if (at == std::string::npos) return src;   // 夹具结构漂移即暴露（钉形失效）
    src.replace(at, clinitTail.size(),
                "      invokestatic " + name + ".h ()V\n"
                "      return\n"
                "    }\n"
                "  }\n"
                "\n"
                "  .method private static h ()V {\n"
                "    code: {\n"
                "      invokestatic " + name + ".ai ()V\n"
                "      return\n"
                "    }\n"
                "  }\n");
    return src;
}

/// @brief v3 arrayed（carray）密文形态夹具（Carray=true 默认形；密文以
///        NEWARRAY T_CHAR + 逐码元 CASTORE 直存，码元序列来自参照加密器，
///        含代理区码元 56160）。classKey = -1722001081。
std::string v3ArrayedSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_2\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      bipush 13\n"                    // 密文长（判据锚点：NEWARRAY T_CHAR）
           "      newarray char\n"
           "      dup\n"
           "      iconst_0\n"
           "      ldc 7762\n"
           "      castore\n"
           "      dup\n"
           "      iconst_1\n"
           "      ldc 60135\n"
           "      castore\n"
           "      dup\n"
           "      iconst_2\n"
           "      ldc 59399\n"
           "      castore\n"
           "      dup\n"
           "      iconst_3\n"
           "      ldc 61381\n"
           "      castore\n"
           "      dup\n"
           "      iconst_4\n"
           "      ldc 10856\n"
           "      castore\n"
           "      dup\n"
           "      iconst_5\n"
           "      ldc 56160\n"                    // 代理区码元（DB60）：码元直存
           "      castore\n"
           "      dup\n"
           "      bipush 6\n"
           "      ldc 29049\n"
           "      castore\n"
           "      dup\n"
           "      bipush 7\n"
           "      ldc 18473\n"
           "      castore\n"
           "      dup\n"
           "      bipush 8\n"
           "      ldc 8855\n"
           "      castore\n"
           "      dup\n"
           "      bipush 9\n"
           "      ldc 60377\n"
           "      castore\n"
           "      dup\n"
           "      bipush 10\n"
           "      ldc 49968\n"
           "      castore\n"
           "      dup\n"
           "      bipush 11\n"
           "      ldc 54432\n"
           "      castore\n"
           "      dup\n"
           "      bipush 12\n"
           "      ldc 12032\n"
           "      castore\n"
           "      ldc2_w 4661L\n"
           "      ldc 125640279\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_1\n"
           "      iconst_5\n"
           "      newarray char\n"
           "      dup\n"
           "      iconst_0\n"
           "      ldc 6905\n"
           "      castore\n"
           "      dup\n"
           "      iconst_1\n"
           "      ldc 41648\n"
           "      castore\n"
           "      dup\n"
           "      iconst_2\n"
           "      ldc 19005\n"
           "      castore\n"
           "      dup\n"
           "      iconst_3\n"
           "      ldc 24078\n"
           "      castore\n"
           "      dup\n"
           "      iconst_4\n"
           "      ldc 61237\n"
           "      castore\n"
           "      ldc2_w 64756L\n"
           "      ldc 1156204009\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n" + decryptorBody(name, "ldc -1722001081") +
           "\n"
           "  .method public static read0 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static read1 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_1\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：读取点下标非常量（iload 0，非本家三元组，静默跳过）。
std::string badIdxSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      ldc \"x\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      ldc2_w 1L\n"
           "      ldc 2\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "\n" + decryptorBody(name, "ldc -1272827931") +
           "\n"
           "  .method public static read (I)Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iload 0\n"                      // 下标非常量 → 非本家三元组
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：池字段形态不符，String[] 字段 + 三元组读取，但无
///        Grunt 池分配前导（普通数组/Kasumi 缓存同形）。
std::string plainArraySource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field public static a [Ljava/lang/String;\n"
           "\n"
           "  .method public static read ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：解密器缺失，arrayInit 引用不在场的 ([CJI) 方法。
std::string missingDecSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      ldc \"x\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      ldc2_w 1L\n"
           "      ldc 2\n"
           "      invokestatic " + name + ".ghost ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：解密器体非生成面（骨架桩，核窗口缺失）。
std::string stubDecSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      ldc \"x\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      ldc2_w 1L\n"
           "      ldc 2\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static dec ([CJI)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"                  // 行为记录 §1.1 样例的骨架形：
           "      areturn\n"                      //   判据钉生成面 → 整类拒绝
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static read ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：arrayed 码元写序列断链（第二码元缺 DUP）。
std::string brokenArrayedSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      iconst_2\n"
           "      newarray char\n"
           "      dup\n"
           "      iconst_0\n"
           "      ldc 65\n"
           "      castore\n"
           "      dup\n"
           "      iconst_2\n"                     // 码元下标乱序（写序列断链，
           "      ldc 66\n"                       //   栈形合法可汇编）
           "      castore\n"
           "      dup\n"
           "      iconst_1\n"
           "      ldc 67\n"
           "      castore\n"
           "      ldc2_w 1L\n"
           "      ldc 2\n"
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static dec ([CJI)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static read ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：seed 缺失，block 的 long 位放了 int 装载。
std::string missingSeedSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field private static a [Ljava/lang/String;\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      putstatic " + name + ".a [Ljava/lang/String;\n"
           "      invokestatic " + name + ".ai ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static ai ()V {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      ldc \"x\"\n"
           "      invokevirtual java/lang/String.toCharArray ()[C\n"
           "      lload 0\n"                      // seed 位是局部装载（栈形合法，
           "      ldc 2\n"                        //   常量缺席 → seed 缺失）
           "      invokestatic " + name + ".dec ([CJI)Ljava/lang/String;\n"
           "      aastore\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static dec ([CJI)Ljava/lang/String; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static read ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      getstatic " + name + ".a [Ljava/lang/String;\n"
           "      iconst_0\n"
           "      aaload\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 全部拒绝面夹具打包（每类独立：字节逐位不变的断言载体）。
struct NegativeFixture
{
    std::string name;
    std::string source;
    bool        expectReject;   // true = 有计数拒绝；false = 静默跳过
};

std::vector<NegativeFixture> negativeFixtures()
{
    std::vector<NegativeFixture> out;
    out.push_back({"BadIdxHost", badIdxSource("BadIdxHost"), false});
    out.push_back({"PlainArrayHost", plainArraySource("PlainArrayHost"), false});
    out.push_back({"MissingDecHost", missingDecSource("MissingDecHost"), true});
    out.push_back({"StubDecHost", stubDecSource("StubDecHost"), true});
    out.push_back({"BrokenArrayedHost", brokenArrayedSource("BrokenArrayedHost"), true});
    out.push_back({"MissingSeedHost", missingSeedSource("MissingSeedHost"), true});
    return out;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(StringPoolScan, FoldsV2LiteralSites)
{
    model::ResourceContainer container =
        makeContainer({{"GtPool2", v2LiteralSource("GtPool2")}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(result.poolClasses, 1u);
    EXPECT_EQ(result.tripleHits, 2u);
    EXPECT_EQ(result.rejects.size(), 0u);
    EXPECT_EQ(result.idxMissing, 0u);

    // 真算法往返：fold 明文 == 参照加密器的输入明文（GruntStringEncryptRef
    // 产出密文时的原文）。
    const Site& s0 = result.sites[0];
    EXPECT_EQ(s0.uri, "GtPool2.class");
    EXPECT_EQ(s0.form, CipherForm::Literal);
    EXPECT_EQ(s0.fieldName, "a");
    EXPECT_EQ(s0.plaintext, "HELLO_GRUNT");
    const Site& s1 = result.sites[1];
    EXPECT_EQ(s1.form, CipherForm::Literal);
    EXPECT_EQ(s1.index, 1);
    EXPECT_EQ(s1.plaintext, "GRUNTEON_POOL");
}

TEST(StringPoolScan, FoldsV3ArrayedSites)
{
    model::ResourceContainer container =
        makeContainer({{"GtPool3", v3ArrayedSource("GtPool3")}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(result.poolClasses, 1u);
    EXPECT_EQ(result.tripleHits, 2u);

    // 真算法往返（arrayed 密文直存形）：明文含 CJK（MUTF-8 编码面）与代理区
    // 密文码元（CASTORE 直存面）。
    const Site& s0 = result.sites[0];
    EXPECT_EQ(s0.form, CipherForm::Arrayed);
    EXPECT_EQ(s0.plaintext, "GRUNTEON_POOL");
    const Site& s1 = result.sites[1];
    EXPECT_EQ(s1.form, CipherForm::Arrayed);
    EXPECT_EQ(s1.plaintext, "中文_AB");
}

TEST(StringPoolScan, FoldsThroughStaticTrampoline)
{
    // clinit → 蹦床 h（体恰两条件）→ ai：池链判定跟一跳，折叠面照常。
    model::ResourceContainer container =
        makeContainer({{"GtPoolTr", v2TrampolineSource("GtPoolTr")}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(result.poolClasses, 1u);
    EXPECT_EQ(result.tripleHits, 2u);
    EXPECT_EQ(result.rejects.size(), 0u);
    EXPECT_EQ(result.sites[0].plaintext, "HELLO_GRUNT");
    EXPECT_EQ(result.sites[1].plaintext, "GRUNTEON_POOL");
}

TEST(StringPoolScan, SkipsNonConstantIndexSilently)
{
    model::ResourceContainer container =
        makeContainer({{"BadIdxHost", badIdxSource("BadIdxHost")}});

    const ScanResult result = scan(container, ScanInput{});
    // 池链在场（三件套结构过），但读取点下标非常量 → 非本家三元组，静默跳过。
    EXPECT_EQ(result.poolClasses, 1u);
    EXPECT_EQ(result.tripleHits, 0u);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(StringPoolScan, SkipsPlainStringArraySilently)
{
    model::ResourceContainer container =
        makeContainer({{"PlainArrayHost", plainArraySource("PlainArrayHost")}});

    const ScanResult result = scan(container, ScanInput{});
    // String[] 字段 + 三元组同形，但无池分配前导（无 arrayInit/解密器链）：
    // 不是本家池，静默跳过（Kasumi 缓存字段同形同理）。
    EXPECT_EQ(result.poolClasses, 0u);
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(StringPoolScan, RejectsMissingDecryptor)
{
    model::ResourceContainer container =
        makeContainer({{"MissingDecHost", missingDecSource("MissingDecHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.decryptorBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("解密器缺失"), std::string::npos);
}

TEST(StringPoolScan, RejectsStubDecryptorBody)
{
    model::ResourceContainer container =
        makeContainer({{"StubDecHost", stubDecSource("StubDecHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.decryptorBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("核窗口"), std::string::npos);
}

TEST(StringPoolScan, RejectsBrokenArrayedStoreSequence)
{
    model::ResourceContainer container =
        makeContainer({{"BrokenArrayedHost", brokenArrayedSource("BrokenArrayedHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.arrayInitBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("乱序"), std::string::npos);
}

TEST(StringPoolScan, RejectsMissingSeed)
{
    model::ResourceContainer container =
        makeContainer({{"MissingSeedHost", missingSeedSource("MissingSeedHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.arrayInitBlocked, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("seed"), std::string::npos);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(StringPoolRewrite, FoldsBothFormsToLdcAndKeepsPoolStructure)
{
    model::ResourceContainer container = makeContainer(
        {{"GtPool2", v2LiteralSource("GtPool2")}, {"GtPool3", v3ArrayedSource("GtPool3")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 4u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 4u);
    EXPECT_EQ(dirty.classes, 2u);
    EXPECT_EQ(dirty.foldedLiteral, 2u);
    EXPECT_EQ(dirty.foldedArrayed, 2u);
    EXPECT_EQ(dirty.failed, 0u);
    ASSERT_EQ(dirty.uris.size(), 2u);

    // v2 类：读点折回明文，池三件套照旧在场（回收交下游）。
    const std::string v2 = disassemble(classBytes(container, "GtPool2.class"));
    EXPECT_NE(v2.find("ldc \"HELLO_GRUNT\""), std::string::npos) << v2;
    EXPECT_NE(v2.find("ldc \"GRUNTEON_POOL\""), std::string::npos) << v2;
    EXPECT_EQ(v2.find("aaload"), std::string::npos) << v2;
    EXPECT_NE(v2.find("invokestatic GtPool2.dec"), std::string::npos) << v2;
    EXPECT_NE(v2.find("anewarray java/lang/String"), std::string::npos) << v2;

    // v3 类：同折叠口径（arrayed 密文块的 CASTORE 序列不动，池面不在
    // 本残留改写范围）。
    const std::string v3 = disassemble(classBytes(container, "GtPool3.class"));
    EXPECT_NE(v3.find("ldc \"GRUNTEON_POOL\""), std::string::npos) << v3;
    EXPECT_NE(v3.find("ldc \"中文_AB\""), std::string::npos) << v3;
    EXPECT_EQ(v3.find("aaload"), std::string::npos) << v3;
    EXPECT_NE(v3.find("newarray char"), std::string::npos) << v3;
}

TEST(StringPoolRewrite, KeepsBytesUntouchedOnRejections)
{
    // 拒绝面全开：单容器全部夹具拒绝/跳过 → rewrite 零动，字节逐位保持。
    std::vector<std::pair<std::string, std::string>> fixtures;
    for (const NegativeFixture& f : negativeFixtures()) {
        fixtures.push_back({f.name, f.source});
    }

    std::vector<std::vector<ca::u8>> before;
    model::ResourceContainer         container;
    for (const auto& [name, source] : fixtures) {
        const std::string         uri   = name + ".class";
        const std::vector<ca::u8> bytes = assemble(name, source);
        before.push_back(bytes);
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    }

    const ScanResult scanned = scan(container, ScanInput{});
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.poolClasses, 1u);   // BadIdxHost 池链在场但三元组不钉
    EXPECT_EQ(scanned.decryptorBlocked, 2u);   // MissingDecHost / StubDecHost
    EXPECT_EQ(scanned.arrayInitBlocked, 2u);   // BrokenArrayedHost / MissingSeedHost
    ASSERT_EQ(scanned.rejects.size(), 4u);

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

TEST(StringPoolProof, KeepsClassCountReparsesAndResidualZero)
{
    model::ResourceContainer container = makeContainer(
        {{"GtPool2", v2LiteralSource("GtPool2")}, {"GtPool3", v3ArrayedSource("GtPool3")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 4u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 4u);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(StringPoolProof, CountsResidualWhenSitesUntouched)
{
    // 池表在场但没跑 rewrite：residual 如实报告本可折叠的站点数。
    model::ResourceContainer container =
        makeContainer({{"GtPool2", v2LiteralSource("GtPool2")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 2u);

    DirtySet dirty;   // 空：没改任何东西
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 2u);
}

}   // namespace
}   // namespace grunt::string_pool_fold
