/// @file        dispatcher_restore_test.cpp
/// @brief       残留 `dispatcher_restore` 的三段判据测试（真值往返 + 拒绝面
///              反证）。
///
/// 夹具是缩小版 Grunt v3 InvokeDispatcher 形态（jasm 现场汇编，不落二
/// 进制、不依赖语料目录）：每类带真实分发器方法（生成面逐字锚：头锚
/// ILOAD key 槽 + tableswitch default 回跳方法头 + case 转发体）+ 真实调用
/// 点（补位 + key 常量 + INVOKESTATIC 分发器）。
///
/// 真值往返的证据链：分发器键表（tableswitch 连续键 + case 转发目标）与调
/// 用点 key 常量是同一份静态事实的两半，scan 反查key → 真目标，rewrite
/// 回填后调用点直接 invoke 真目标；断言回填目标 == 夹具键表写下的目标（无
/// 循环论证：判据不读方法名，键表由结构重建）。
///
/// 覆盖分工：
/// - SCAN：static / virtual / N=0（无 Object 槽）三形态站点定案；原实参尾
///   带 null 字面量（null 跑长于补位数）的站点照常回填（只擦末段补位）；
///   形状不符分发器（default 不回头 / case 体多 invoke）、key 不在表、
///   key 非常量、补位核对不符、key 位合并点 → 拒绝面逐桶计数；
/// - REWRITE：调用指令原地换真目标 + 补位/key 擦除；**分发器本体与键表
///   不删**（改写后仍在场）；拒绝面上容器字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零（未跑 rewrite 时如实
///   报本可回填站点数）。
#include "grunt/dispatcher_restore.hpp"

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

namespace grunt::dispatcher_restore {
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
// ==== 夹具（v3 InvokeDispatcher 生成面缩样）=================================
// ============================================================================
//
// dispA：N=2（(Object;Object;I)V），键区间 [100,101]，c0 → static tgtA
// （1 参），c1 → virtual PrintStream.println(String)。
// dispB：N=0（(I)Ljava/lang/String;），键区间 [100,101]，c0 → tgtR0、
// c1 → tgtR1（均 static 无参返回 String）。

std::string dispHostSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method private static dispA (Ljava/lang/Object;Ljava/lang/Object;I)V {\n"
           "    code: {\n"
           "    Lhead:\n"                       // 判据锚点：default 回跳方法头
           "      iload 2\n"                     // key 参数槽 = Object 参数个数
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { Lc0, Lc1 },\n"
           "          default: Lhead\n"
           "      }\n"
           "    Lc0:\n"                          // case 转发体（生成面逐字）：
           "      aload 0\n"                     //   载实参 + CHECKCAST + 唯一
           "      checkcast java/lang/Object\n"  //   invoke + ret 型 return
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    Lc1:\n"
           "      aload 0\n"
           "      checkcast java/io/PrintStream\n"
           "      aload 1\n"
           "      checkcast java/lang/String\n"
           "      invokevirtual java/io/PrintStream.println (Ljava/lang/String;)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method private static dispB (I)Ljava/lang/String; {\n"
           "    code: {\n"
           "    Lhead2:\n"
           "      iload 0\n"                     // N=0：key 槽 = 0
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { Ld0, Ld1 },\n"
           "          default: Lhead2\n"
           "      }\n"
           "    Ld0:\n"
           "      invokestatic " + name + ".tgtR0 ()Ljava/lang/String;\n"
           "      areturn\n"
           "    Ld1:\n"
           "      invokestatic " + name + ".tgtR1 ()Ljava/lang/String;\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtA (Ljava/lang/Object;)V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtR0 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      ldc \"R0\"\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtR1 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      ldc \"R1\"\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static callA0 ()V {\n"
           "    code: {\n"
           "      ldc \"x\"\n"                  // 原调用实参（tgtA 1 参）
           "      aconst_null\n"                // 补位 addNulls = N - 1 = 1
           "      bipush 100\n"                 // key 常量（判据锚点：紧邻压栈）
           "      invokestatic " + name + ".dispA (Ljava/lang/Object;Ljava/lang/Object;I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static callA1 (Ljava/lang/String;)V {\n"
           "    code: {\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      aload 0\n"                    // 原调用实参（receiver + arg），
           "      bipush 101\n"                 //   无补位（addNulls = 2 - 2 = 0）
           "      invokestatic " + name + ".dispA (Ljava/lang/Object;Ljava/lang/Object;I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static callB0 ()Ljava/lang/String; {\n"
           "    code: {\n"
           "      bipush 100\n"                 // N=0：无补位，key 直邻调用
           "      invokestatic " + name + ".dispB (I)Ljava/lang/String;\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 正例夹具：原实参尾带 null 字面量（null 跑 2 = 实参 null + 补位 1
/// ，null 跑长于补位数仍回填，只擦末段补位，实参保留）。
std::string nullArgHostSource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method private static dispA (Ljava/lang/Object;Ljava/lang/Object;I)V {\n"
           "    code: {\n"
           "    Lhead:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { Lc0, Lc1 },\n"
           "          default: Lhead\n"
           "      }\n"
           "    Lc0:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    Lc1:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtA (Ljava/lang/Object;)V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static callNullArg ()V {\n"
           "    code: {\n"
           "      aconst_null\n"                // 原实参 null 字面量
           "      aconst_null\n"                // 补位（与实参 null 同形不可分辨）
           "      bipush 100\n"                 // null 跑 2 > addNulls 1
           "      invokestatic " + name + ".dispA (Ljava/lang/Object;Ljava/lang/Object;I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具基底：单分发器 + 单调用点（key = 99，补位全）。
std::string badKeySource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "\n"
           "  .method private static dispA (Ljava/lang/Object;Ljava/lang/Object;I)V {\n"
           "    code: {\n"
           "    Lhead:\n"
           "      iload 2\n"
           "      tableswitch {\n"
           "          min: 100,\n"
           "          max: 101,\n"
           "          cases: { Lc0, Lc1 },\n"
           "          default: Lhead\n"
           "      }\n"
           "    Lc0:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    Lc1:\n"
           "      aload 0\n"
           "      checkcast java/lang/Object\n"
           "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method static tgtA (Ljava/lang/Object;)V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static call ()V {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      aconst_null\n"
           "      bipush 99\n"                  // key = 99 < lo（不在 [100,101)）
           "      invokestatic " + name + ".dispA (Ljava/lang/Object;Ljava/lang/Object;I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 拒绝面夹具：key 位非常量（iload，NumberBasicEncrypt 藏形后的形）。
std::string nonConstKeySource(const std::string& name)
{
    std::string src = badKeySource(name);
    const std::string needle = "      bipush 99\n";
    const ca::usize   at      = src.find(needle);
    if (at == std::string::npos) return src;   // 夹具结构漂移即暴露
    src.replace(at, needle.size(), "      iload 0\n");
    // call ()V 无参，iload 0 槽位越界但 jasm 只汇编不校验（夹具不运行）。
    return src;
}

/// @brief 拒绝面夹具：分发器形状不符，default 落别名块（CFF alias face 同形
/// 窜入）而非回跳方法头。分发器整表拒绝，调用点静默跳过。
std::string aliasDefaultSource(const std::string& name)
{
    std::string src = badKeySource(name);
    const std::string needle = "          default: Lhead\n"
                               "      }\n";
    const ca::usize   at      = src.find(needle);
    if (at == std::string::npos) return src;
    src.replace(at, needle.size(),
                "          default: Lalias\n"     // default 落别名块 → 头锚缺席
                "      }\n"
                "    Lalias:\n"
                "      goto Lhead\n");
    return src;
}

/// @brief 拒绝面夹具：case 体转发目标不唯一（多候选，fail-closed 不猜）。
std::string ambiguousCaseSource(const std::string& name)
{
    std::string src = badKeySource(name);
    const std::string needle =
        "    Lc0:\n"
        "      aload 0\n"
        "      checkcast java/lang/Object\n"
        "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
        "      return\n";
    const ca::usize at = src.find(needle);
    if (at == std::string::npos) return src;
    src.replace(at, needle.size(),
                "    Lc0:\n"
                "      aload 0\n"
                "      checkcast java/lang/Object\n"
                "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
                "      aload 0\n"                // 第二个 invoke：转发目标不唯一
                "      invokestatic " + name + ".tgtA (Ljava/lang/Object;)V\n"
                "      return\n");
    return src;
}

/// @brief 拒绝面夹具：补位核对不符，调用点零补位（原实参非 null 值）但
/// addNulls = 1：null 跑 0 < 补位数，无 null 可擦（fail-closed）。
std::string padMismatchSource(const std::string& name)
{
    std::string src = badKeySource(name);
    const std::string needle =
        "      aconst_null\n"
        "      aconst_null\n"
        "      bipush 99\n";
    const ca::usize at = src.find(needle);
    if (at == std::string::npos) return src;
    src.replace(at, needle.size(),
                "      ldc \"x\"\n"              // 原实参非 null：零补位，
                "      ldc \"y\"\n"              //   null 跑 0 < addNulls 1
                "      bipush 100\n");
    return src;
}

/// @brief 拒绝面夹具：key 位带合并点标签（外部到达路径栈形不可知）。
std::string labeledKeySource(const std::string& name)
{
    std::string src = badKeySource(name);
    const std::string needle = "      bipush 99\n";
    const ca::usize   at      = src.find(needle);
    if (at == std::string::npos) return src;
    src.replace(at, needle.size(),
                "      goto Lmid\n"              // goto 使合并点标签在 dom 在场
                "    Lmid:\n"
                "      bipush 99\n");
    return src;
}

/// @brief 全部拒绝面夹具打包（每类独立：字节逐位不变的断言载体）。
struct NegativeFixture
{
    std::string name;
    std::string source;
};

std::vector<NegativeFixture> negativeFixtures()
{
    std::vector<NegativeFixture> out;
    out.push_back({"BadKeyHost", badKeySource("BadKeyHost")});
    out.push_back({"NonConstKeyHost", nonConstKeySource("NonConstKeyHost")});
    out.push_back({"AliasDefaultHost", aliasDefaultSource("AliasDefaultHost")});
    out.push_back({"AmbiguousCaseHost", ambiguousCaseSource("AmbiguousCaseHost")});
    out.push_back({"PadMismatchHost", padMismatchSource("PadMismatchHost")});
    out.push_back({"LabeledKeyHost", labeledKeySource("LabeledKeyHost")});
    return out;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(DispatcherScan, RestoresStaticVirtualAndZeroSlotSites)
{
    model::ResourceContainer container =
        makeContainer({{"GtDisp", dispHostSource("GtDisp")}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 3u);
    EXPECT_EQ(result.dispatchers, 2u);
    EXPECT_EQ(result.dispatcherRejected, 0u);
    EXPECT_EQ(result.rejects.size(), 0u);

    // callA0：key 100 → static tgtA；补位 1。
    const Site& s0 = result.sites[0];
    EXPECT_EQ(s0.uri, "GtDisp.class");
    EXPECT_EQ(s0.dispName, "dispA");
    EXPECT_EQ(s0.key, 100);
    EXPECT_EQ(s0.padCount, 1u);
    EXPECT_FALSE(s0.target.isVirtual);
    EXPECT_EQ(s0.target.owner, "GtDisp");
    EXPECT_EQ(s0.target.name, "tgtA");
    EXPECT_EQ(s0.target.desc, "(Ljava/lang/Object;)V");

    // callA1：key 101 → virtual PrintStream.println；无补位。
    const Site& s1 = result.sites[1];
    EXPECT_EQ(s1.key, 101);
    EXPECT_EQ(s1.padCount, 0u);
    EXPECT_TRUE(s1.target.isVirtual);
    EXPECT_EQ(s1.target.owner, "java/io/PrintStream");
    EXPECT_EQ(s1.target.name, "println");
    EXPECT_EQ(s1.target.desc, "(Ljava/lang/String;)V");

    // callB0：N=0 形（(I)String），key 100 → tgtR0。
    const Site& s2 = result.sites[2];
    EXPECT_EQ(s2.dispName, "dispB");
    EXPECT_EQ(s2.key, 100);
    EXPECT_EQ(s2.padCount, 0u);
    EXPECT_FALSE(s2.target.isVirtual);
    EXPECT_EQ(s2.target.name, "tgtR0");
}

TEST(DispatcherScan, RestoresSiteWithNullLiteralArg)
{
    // callNullArg：null 跑 2（实参 null + 补位 1），null 跑长于补位数仍
    // 回填（只擦末段补位），不能因跑长而误拒。
    model::ResourceContainer container =
        makeContainer({{"GtDispNull", nullArgHostSource("GtDispNull")}});

    const ScanResult result = scan(container, ScanInput{});
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().key, 100);
    EXPECT_EQ(result.sites.front().padCount, 1u);
    EXPECT_EQ(result.sites.front().target.name, "tgtA");
    EXPECT_EQ(result.padMismatch, 0u);
}

TEST(DispatcherScan, RejectsKeyOutOfTable)
{
    model::ResourceContainer container =
        makeContainer({{"BadKeyHost", badKeySource("BadKeyHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.keyNotInTable, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("不在键表"), std::string::npos);
}

TEST(DispatcherScan, RejectsNonConstantKey)
{
    model::ResourceContainer container =
        makeContainer({{"NonConstKeyHost", nonConstKeySource("NonConstKeyHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.keyNotConst, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("key 位非常量"), std::string::npos);
}

TEST(DispatcherScan, RejectsAliasDefaultDispatcher)
{
    // default 落别名块 → 头锚缺席 → 分发器整表拒绝；调用点引用不命中键表，
    // 静默跳过（非本家）。
    model::ResourceContainer container =
        makeContainer({{"AliasDefaultHost", aliasDefaultSource("AliasDefaultHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.dispatchers, 0u);
    EXPECT_EQ(result.dispatcherRejected, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("default 未回跳方法头"),
              std::string::npos);
}

TEST(DispatcherScan, RejectsAmbiguousCaseBody)
{
    model::ResourceContainer container =
        makeContainer({{"AmbiguousCaseHost", ambiguousCaseSource("AmbiguousCaseHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.dispatchers, 0u);
    EXPECT_EQ(result.dispatcherRejected, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("不唯一"), std::string::npos);
}

TEST(DispatcherScan, RejectsPadMismatch)
{
    model::ResourceContainer container =
        makeContainer({{"PadMismatchHost", padMismatchSource("PadMismatchHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    EXPECT_EQ(result.dispatchers, 1u);
    EXPECT_EQ(result.padMismatch, 1u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("核对不符"), std::string::npos);
}

TEST(DispatcherScan, RejectsLabeledKey)
{
    model::ResourceContainer container =
        makeContainer({{"LabeledKeyHost", labeledKeySource("LabeledKeyHost")}});

    const ScanResult result = scan(container, ScanInput{});
    EXPECT_EQ(result.sites.size(), 0u);
    ASSERT_EQ(result.rejects.size(), 1u);
    EXPECT_NE(result.rejects.front().why.find("合并点标签"), std::string::npos);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(DispatcherRewrite, RestoresSitesInPlaceAndKeepsDispatcher)
{
    model::ResourceContainer container =
        makeContainer({{"GtDisp", dispHostSource("GtDisp")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 3u);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    EXPECT_EQ(dirty.rewrittenStatic, 2u);
    EXPECT_EQ(dirty.rewrittenVirtual, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::string out = disassemble(classBytes(container, "GtDisp.class"));
    // 回填真目标（两 static + 一 virtual）。
    EXPECT_NE(out.find("invokestatic GtDisp.tgtA (Ljava/lang/Object;)V"),
              std::string::npos) << out;
    EXPECT_NE(out.find("invokevirtual java/io/PrintStream.println (Ljava/lang/String;)V"),
              std::string::npos) << out;
    EXPECT_NE(out.find("invokestatic GtDisp.tgtR0 ()Ljava/lang/String;"),
              std::string::npos) << out;
    EXPECT_NE(out.find("invokestatic GtDisp.tgtR1 ()Ljava/lang/String;"),
              std::string::npos) << out;
    // 分发器调用面归零（方法体内不再引用）。
    EXPECT_EQ(out.find("invokestatic GtDisp.dispA"), std::string::npos) << out;
    EXPECT_EQ(out.find("invokestatic GtDisp.dispB"), std::string::npos) << out;
    // key 常量与补位擦除（调用点 bipush 不在；实参 ldc "x" 保留）。
    EXPECT_EQ(out.find("bipush 100"), std::string::npos) << out;
    EXPECT_EQ(out.find("bipush 101"), std::string::npos) << out;
    EXPECT_NE(out.find("ldc \"x\""), std::string::npos) << out;
    // 分发器本体与键表不删（回收交死码/死成员/压池）。
    EXPECT_NE(out.find(".method private static dispA"), std::string::npos) << out;
    EXPECT_NE(out.find(".method private static dispB"), std::string::npos) << out;
    EXPECT_NE(out.find("tableswitch"), std::string::npos) << out;
}

TEST(DispatcherRewrite, KeepsNullLiteralArgWhenErasingTailPads)
{
    // null 实参站点：只擦末段补位，前段 null 实参保留，回填后调用点仍是
    // null 实参 + 真目标。
    model::ResourceContainer container =
        makeContainer({{"GtDispNull", nullArgHostSource("GtDispNull")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 1u);
    EXPECT_EQ(dirty.failed, 0u);

    const std::string out = disassemble(classBytes(container, "GtDispNull.class"));
    EXPECT_EQ(out.find("invokestatic GtDispNull.dispA"), std::string::npos) << out;
    EXPECT_NE(out.find("invokestatic GtDispNull.tgtA (Ljava/lang/Object;)V"),
              std::string::npos) << out;
    EXPECT_EQ(out.find("bipush 100"), std::string::npos) << out;
    // callNullArg 体里剩一个 aconst_null（实参），另一个（补位）被擦。
    const ca::usize callBody = out.find("callNullArg");
    ASSERT_NE(callBody, std::string::npos);
    const std::string tail = out.substr(callBody);
    EXPECT_EQ(tail.find("aconst_null"), tail.rfind("aconst_null")) << out;
}

TEST(DispatcherRewrite, KeepsBytesUntouchedOnRejections)
{
    // 拒绝面全开：单容器全部夹具拒绝 → rewrite 零动，字节逐位保持。
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
    EXPECT_EQ(scanned.keyNotInTable, 1u);      // BadKeyHost
    EXPECT_EQ(scanned.keyNotConst, 1u);        // NonConstKeyHost
    EXPECT_EQ(scanned.padMismatch, 1u);        // PadMismatchHost
    EXPECT_EQ(scanned.dispatcherRejected, 2u); // AliasDefault / AmbiguousCase
    ASSERT_EQ(scanned.rejects.size(), 6u);     // 每夹具恰好 1 条拒绝账

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

TEST(DispatcherProof, KeepsClassCountReparsesAndResidualZero)
{
    model::ResourceContainer container =
        makeContainer({{"GtDisp", dispHostSource("GtDisp")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.sites, 3u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(DispatcherProof, CountsResidualWhenSitesUntouched)
{
    // 分发器在场但没跑 rewrite：residual 如实报告本可回填的站点数。
    model::ResourceContainer container =
        makeContainer({{"GtDisp", dispHostSource("GtDisp")}});

    const ScanResult scanned = scan(container, ScanInput{});
    ASSERT_EQ(scanned.sites.size(), 3u);

    DirtySet dirty;   // 空：没改任何东西
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 3u);
}

}   // namespace
}   // namespace grunt::dispatcher_restore
