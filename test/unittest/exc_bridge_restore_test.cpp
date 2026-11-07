/// @file        exc_bridge_restore_test.cpp
/// @brief       残留 `exc_bridge_restore` 的三段判据测试（真值往返 + 反误伤
///              互证 + 拒绝面字节恒等 + JVM -Xverify:all 实跑）。
///
/// 夹具是「缩小版 Grunt v3 ControlflowJump 异常桥产物形态」（jasm 现场汇编，
/// 源算法锚 grunt3@e41e852 FlowExceptionBridgeInserter.kt，罐 A 13461 站点
/// 实证同构）：合成 trap 块 `NEW java/lang/RuntimeException; DUP;
/// INVOKESPECIAL <init>()V; ATHROW`（布局尾追加、相邻成排）、handler 块
/// `POP; GOTO`（随机落位）、异常表项 catchType=RuntimeException 且保护区
/// 恰罩 trap 四条；位置落入变体 = 生成面 goto 省略形（前驱 istore 落入
/// trap 头）。
///
/// 反误伤互证的证据链：真实 `catch (RuntimeException)` 的保护区是用户代码
/// 段（非四条 trap 形）或 handler 带异常槽存储（ASTORE 非 POP）——形状钉
/// 天然拒绝；桥间互证 = X 落点不得进任何候选桥受害者集。
///
/// 覆盖分工：
/// - SCAN：跳转边桥 / 位置落入桥（needsPin）/ 相邻双 trap 多站点 / 桥与
///   真实 try 区共存四形态定案；真实 RTE 区（区间长 + ASTORE handler）、
///   外层保护区罩 trap 头（covered）、双桥互指 X（xInvalid）拒绝面逐桶记账；
/// - REWRITE：replaceAllUses 直连 + 位置落入 GOTO 钉 + 表项剔除 + 六指令
///   单批擦除；真实表项 end 锚吸附（snap forward）；拒绝面上容器字节逐位
///   不动；
/// - PROOF：类数不增 + 脏类可重解析 + residual 归零；
/// - JVM：-Xverify:all 下实跑还原前后产物（含异常路径），stdout 对照真值。
#include "grunt/exc_bridge_restore.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::exc_bridge_restore {
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
// ==== 夹具（v3 FlowExceptionBridgeInserter 产物缩样）========================
// ============================================================================

constexpr const char* kRte = "java/lang/RuntimeException";

/// jump 边桥：`goto Ltrap`（原边本直达 Lx）。pick(0)=7 走落空臂；pick(1)=42
/// 经桥（trap 抛 RTE → handler POP → Lx）。还原后 goto 直达 Lx。
std::string jumpEdgeSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static pick (I)I {\n"
           "    exceptions: { { Ltrap, Lafter, Lh, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifeq Lzero\n"
           "      goto Ltrap\n"                 // 桥：原边被改指 trap
           "    Lzero:\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    Lx:\n"
           "      bipush 42\n"
           "      ireturn\n"
           "    Ltrap:\n"                       // trap 块（生成面布局尾追加）
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lafter:\n"
           "    Lh:\n"                          // bridge handler（POP; GOTO）
           "      pop\n"
           "      goto Lx\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 位置落入桥：istore 块的边被生成面 goto 省略（nextBlock==target），istore
/// 直接落入 trap 头。pick2(0)=7；pick2(x!=0)=x+1 经桥。还原后 istore 后补
/// GOTO Lx 钉（cff_unflatten fallPins 同形）。
std::string fallthroughSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static pick2 (I)I {\n"
           "    exceptions: { { Ltrap, Lafter, Lh, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifeq Lzero\n"
           "      iload 0\n"
           "      iconst_1\n"
           "      iadd\n"
           "      istore 0\n"                 // 位置落入下一块（= trap 头）
           "    Ltrap:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lafter:\n"
           "    Lh:\n"
           "      pop\n"
           "      goto Lx\n"
           "    Lzero:\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    Lx:\n"
           "      iload 0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 真实 RTE catch 双形（反误伤面）：realRte 的保护区是用户码段（6 条，非四
/// 条 trap 形）；exact4 恰四条 trap 形但 handler 是 ASTORE（异常槽存储）。
/// 两者都必须被形状钉拒绝、字节原样保留。realRte(0)=3；realRte(x!=0)=9
/// （真实 throw 被 catch）；exact4(0)=2；exact4(x!=0)=4。
std::string realThrowSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static realRte (I)I {\n"
           "    exceptions: { { Ltry, Lend, Lcatch, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifeq Lkeep\n"
           "    Ltry:\n"
           "      iconst_1\n"
           "      pop\n"                      // 用户码前缀：保护区 > 4 条
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend:\n"
           "    Lcatch:\n"
           "      astore 1\n"                 // 真实 handler：异常槽存储
           "      bipush 9\n"
           "      ireturn\n"
           "    Lkeep:\n"
           "      bipush 3\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .method public static exact4 (I)I {\n"
           "    exceptions: { { Ltrap, Lend, Lh, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifeq Lskip\n"
           "    Ltrap:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend:\n"
           "    Lh:\n"
           "      astore 1\n"                 // 恰四条但 handler 非 POP
           "      bipush 4\n"
           "      ireturn\n"
           "    Lskip:\n"
           "      bipush 2\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 桥与真实 try 区共存 + 真实表项 end 锚吸附面：真实 RTE 区（6 条用户形）
/// 紧邻桥 trap（真实表项 end 锚 = 桥 trap 头，exclusive 不罩）；x>0 经桥到
/// Lx2（x+100），x<0 走真实 throw（Lrh catch → 5），x==0 → 7。还原只吃桥，
/// 真实表项 end 锚前移吸附、真实 throw 不动。
std::string mixedSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static mixed (I)I {\n"
           "    exceptions: { { LrStart, Ltrap, Lrh, Ljava/lang/RuntimeException; },"
           " { Ltrap, Lend, Lh, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifgt Ltrap\n"               // 桥：原边（x>0 → Lx2）被改指
           "      iload 0\n"
           "      ifeq Lzero\n"
           "    LrStart:\n"                   // 真实 throw 区（用户形）
           "      iconst_1\n"
           "      pop\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Ltrap:\n"                     // 桥 trap（紧随真实区尾）
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend:\n"
           "    Lrh:\n"
           "      astore 1\n"
           "      bipush 5\n"
           "      ireturn\n"
           "    Lh:\n"
           "      pop\n"
           "      goto Lx2\n"
           "    Lzero:\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    Lx2:\n"
           "      iload 0\n"
           "      bipush 100\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 相邻双 trap 多站点（生成面布局尾追加成排形）：trap1 的 end 锚 = trap2 头
/// （共享边界），两桥各有 handler。mixed2(0)=0；mixed2(x>0)=11；mixed2(x<0)=22。
std::string adjacentSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static mixed2 (I)I {\n"
           "    exceptions: { { Ltrap1, Ltrap2, Lh1, Ljava/lang/RuntimeException; },"
           " { Ltrap2, Lend2, Lh2, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifgt Ltrap1\n"              // 桥 1（原边 → Lx1）
           "      iload 0\n"
           "      iflt Ltrap2\n"              // 桥 2（原边 → Lx2）
           "      bipush 0\n"
           "      ireturn\n"
           "    Lx1:\n"
           "      bipush 11\n"
           "      ireturn\n"
           "    Lx2:\n"
           "      bipush 22\n"
           "      ireturn\n"
           "    Ltrap1:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Ltrap2:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend2:\n"
           "    Lh1:\n"
           "      pop\n"
           "      goto Lx1\n"
           "    Lh2:\n"
           "      pop\n"
           "      goto Lx2\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 覆盖门拒绝面：外层 Throwable 保护区罩住桥 trap 头（真实 try 区延伸到
/// 追加 trap 上的变形）——派发歧义，桥整条拒。covered(0)=8；covered(x!=0)=6
/// （外层 handler 先接住桥 trap 抛出的 RTE）。
std::string coveredSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static covered (I)I {\n"
           "    exceptions: { { Louter, Lend, Lrh, Ljava/lang/Throwable; },"
           " { Ltrap, Lend, Lh, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifeq Lx\n"
           "      goto Ltrap\n"
           "    Louter:\n"
           "      nop\n"
           "    Ltrap:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend:\n"
           "    Lrh:\n"
           "      astore 1\n"
           "      bipush 6\n"
           "      ireturn\n"
           "    Lh:\n"
           "      pop\n"
           "      goto Lx\n"
           "    Lx:\n"
           "      bipush 8\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// X 落点门拒绝面：双桥 handler 互指对方 trap 头（X 落进候选桥受害者集），
/// 两桥全拒、字节原样。不可达面（夹具只做判据数据）。
std::string mutualXSource(const std::string& name)
{
    return std::string(".super java/lang/Object\n") +
           ".class public super " + name + " {\n"
           "\n"
           "  .method public static mutual (I)I {\n"
           "    exceptions: { { Ltrap1, Ltrap2, Lh1, Ljava/lang/RuntimeException; },"
           " { Ltrap2, Lend2, Lh2, Ljava/lang/RuntimeException; } },\n"
           "    code: {\n"
           "      iload 0\n"
           "      ifgt Ltrap1\n"
           "      iload 0\n"
           "      iflt Ltrap2\n"
           "      bipush 0\n"
           "      ireturn\n"
           "    Lx1:\n"
           "      bipush 11\n"
           "      ireturn\n"
           "    Lx2:\n"
           "      bipush 22\n"
           "      ireturn\n"
           "    Ltrap1:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Ltrap2:\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    Lend2:\n"
           "    Lh1:\n"
           "      pop\n"
           "      goto Ltrap2\n"              // X 落进桥 2 受害者集
           "    Lh2:\n"
           "      pop\n"
           "      goto Ltrap1\n"              // X 落进桥 1 受害者集
           "    }\n"
           "  }\n"
           "}\n";
}

/// JVM 实跑夹具：mixed + main（-Xverify:all 下跑还原前后，stdout 对照真值
/// "105/7/5"——三条路径各一条：桥 / 真实 throw / 直落）。
std::string jvmMixedSource(const std::string& name)
{
    std::string src = mixedSource(name);
    while (!src.empty() && (src.back() == '\n' || src.back() == '}')) {
        if (src.back() == '}') {
            src.pop_back();
            break;
        }
        src.pop_back();
    }
    src += "\n"
           "  .method public static main ([Ljava/lang/String;)V {\n"
           "    code: {\n"
           "      bipush 5\n"
           "      invokestatic " + name + ".mixed (I)I\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      swap\n"
           "      invokevirtual java/io/PrintStream.println (I)V\n"
           "      bipush 0\n"
           "      invokestatic " + name + ".mixed (I)I\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      swap\n"
           "      invokevirtual java/io/PrintStream.println (I)V\n"
           "      bipush -3\n"
           "      invokestatic " + name + ".mixed (I)I\n"
           "      getstatic java/lang/System.out Ljava/io/PrintStream;\n"
           "      swap\n"
           "      invokevirtual java/io/PrintStream.println (I)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
    return src;
}

// ============================================================================
// ==== JVM 实跑辅助（-Xverify:all，cff_unflatten_test 同形）==================
// ============================================================================

std::string locateJava()
{
    if (const char* home = std::getenv("JAVA_HOME"); home != nullptr && *home != '\0') {
        const std::filesystem::path p =
            std::filesystem::path(home) / "bin" / "java.exe";
        if (std::filesystem::exists(p)) return p.string();
        const std::filesystem::path ux = std::filesystem::path(home) / "bin" / "java";
        if (std::filesystem::exists(ux)) return ux.string();
    }
    return "java";
}

/// @brief 产物落临时目录 + `java -Xverify:all -cp <dir> <main>` 实跑。
/// @return (退出码, 合并输出)；java 不可用返回 (-1, "no java")。
std::pair<int, std::string> runJavaVerify(const std::vector<ca::u8>& bytes,
                                          const std::string&         mainClass)
{
    const std::string java = locateJava();
    if (java == "java" && std::system("java -version >NUL 2>&1") != 0) {
        return {-1, "no java"};
    }

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("exc_bridge_restore_test_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    const std::string clsUri = mainClass + ".class";
    {
        std::ofstream out(dir / clsUri, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    const std::filesystem::path outPath = dir / "out.txt";
    const std::string dirStr  = dir.string();
    const std::string javaBin = (java.find(' ') == std::string::npos
                                     && dirStr.find(' ') == std::string::npos)
                                    ? java
                                    : std::string();
    if (javaBin.empty()) return {-1, "no java"};

    const std::string cmd = javaBin + " -Xverify:all -cp " + dirStr + " "
                            + mainClass + " > " + outPath.string() + " 2>&1";
    const int rc = std::system(cmd.c_str());
    std::string captured;
    {
        std::ifstream in(outPath, std::ios::binary);
        std::string   line;
        while (std::getline(in, line)) {
            captured += line;
            captured += '\n';
        }
        if (captured.empty()) captured = "<空输出>";
    }
    if (rc != 0) captured += "[cmd] " + cmd + "\n";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return {rc, captured};
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(ExcScan, FindsBridgeOnJumpEdge)
{
    model::ResourceContainer container =
        makeContainer({{"ExcJump", jumpEdgeSource("ExcJump")}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().uri, "ExcJump.class");
    EXPECT_EQ(result.sites.front().methodIndex, 0u);
    EXPECT_EQ(result.sites.front().users, 1u);     // goto Ltrap
    EXPECT_FALSE(result.sites.front().needsPin);   // 前驱 goto = 终结符
    EXPECT_EQ(result.rejects.size(), 0u);
    EXPECT_EQ(result.bridgeClasses, 1u);
    EXPECT_EQ(result.shapeMiss, 0u);
    EXPECT_EQ(result.covered, 0u);
    EXPECT_EQ(result.handlerCollide, 0u);
    EXPECT_EQ(result.xInvalid, 0u);
    EXPECT_EQ(result.rebindBlocked, 0u);
}

TEST(ExcScan, FindsFallthroughBridgeNeedingPin)
{
    model::ResourceContainer container =
        makeContainer({{"ExcFall", fallthroughSource("ExcFall")}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_TRUE(result.sites.front().needsPin);    // istore 位置落入 trap 头
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(ExcScan, RealThrowRegionsNotEaten)
{
    model::ResourceContainer container =
        makeContainer({{"ExcReal", realThrowSource("ExcReal")}});

    const ScanResult result = scan(container);
    EXPECT_EQ(result.sites.size(), 0u);            // 真实 throw 区一条不吃
    EXPECT_EQ(result.shapeMiss, 2u);               // 区间长形 + handler ASTORE 形
    EXPECT_EQ(result.covered, 0u);
    EXPECT_EQ(result.xInvalid, 0u);
}

TEST(ExcScan, MixedRealRegionAndBridge)
{
    model::ResourceContainer container =
        makeContainer({{"ExcMixed", mixedSource("ExcMixed")}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);            // 只吃桥
    EXPECT_EQ(result.sites.front().users, 1u);     // ifgt Ltrap
    EXPECT_FALSE(result.sites.front().needsPin);   // 桥前驱 = 真实区尾 athrow
    EXPECT_EQ(result.shapeMiss, 1u);               // 真实表项（区间长）如实计
    EXPECT_EQ(result.covered, 0u);
    EXPECT_EQ(result.handlerCollide, 0u);
}

TEST(ExcScan, AdjacentTrapsBothSites)
{
    model::ResourceContainer container =
        makeContainer({{"ExcAdj", adjacentSource("ExcAdj")}});

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 2u);            // 相邻 trap 共享边界互不阻塞
    EXPECT_EQ(result.sites.front().users, 1u);
    EXPECT_EQ(result.sites.back().users, 1u);
    EXPECT_EQ(result.rejects.size(), 0u);
}

TEST(ExcScan, CoveredTrapRejected)
{
    model::ResourceContainer container =
        makeContainer({{"ExcCov", coveredSource("ExcCov")}});

    const ScanResult result = scan(container);
    EXPECT_EQ(result.sites.size(), 0u);            // 外层保护区罩 trap 头：整条拒
    EXPECT_EQ(result.covered, 1u);
    EXPECT_EQ(result.shapeMiss, 1u);               // 外层表项（用户形）如实计
}

TEST(ExcScan, MutualXCollisionRejected)
{
    model::ResourceContainer container =
        makeContainer({{"ExcMut", mutualXSource("ExcMut")}});

    const ScanResult result = scan(container);
    EXPECT_EQ(result.sites.size(), 0u);            // X 落进候选桥受害者集：全拒
    EXPECT_EQ(result.xInvalid, 2u);
    EXPECT_EQ(result.shapeMiss, 0u);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(ExcRewrite, RestoresJumpEdgeBridge)
{
    model::ResourceContainer container =
        makeContainer({{"ExcJump", jumpEdgeSource("ExcJump")}});
    const std::vector<ca::u8> before = classBytes(container, "ExcJump.class");
    EXPECT_NE(disassemble(before).find("athrow"), std::string::npos);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.bridges, 1u);
    EXPECT_EQ(dirty.pins, 0u);
    EXPECT_EQ(dirty.insns, 6u);                    // trap 4 + handler 2
    EXPECT_EQ(dirty.failed, 0u);

    const std::string after = disassemble(classBytes(container, "ExcJump.class"));
    EXPECT_EQ(after.find("athrow"), std::string::npos) << after;
    EXPECT_EQ(after.find("java/lang/RuntimeException"), std::string::npos) << after;

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(ExcRewrite, PinsFallthroughEdge)
{
    model::ResourceContainer container =
        makeContainer({{"ExcFall", fallthroughSource("ExcFall")}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.bridges, 1u);
    EXPECT_EQ(dirty.pins, 1u);                     // istore 后补 GOTO Lx
    EXPECT_EQ(dirty.failed, 0u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(ExcRewrite, MixedKeepsRealRegionAndSnapsAnchor)
{
    model::ResourceContainer container =
        makeContainer({{"ExcMixed", mixedSource("ExcMixed")}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.bridges, 1u);
    EXPECT_EQ(dirty.failed, 0u);

    const std::string after = disassemble(classBytes(container, "ExcMixed.class"));
    // 桥面全清：双 athrow / 双 new / 双 pop 只剩真实 throw 区一份（真实区
    // 尾 athrow、new、iconst_1; pop 都保留，桥的 trap/handler 全擦）。
    ca::usize athrows = 0, newRte = 0, pops = 0;
    for (ca::usize pos = after.find("athrow"); pos != std::string::npos;
         pos = after.find("athrow", pos + 1)) {
        ++athrows;
    }
    for (ca::usize pos = after.find("new java/lang/RuntimeException");
         pos != std::string::npos;
         pos = after.find("new java/lang/RuntimeException", pos + 1)) {
        ++newRte;
    }
    for (ca::usize pos = after.find(" pop"); pos != std::string::npos;
         pos = after.find(" pop", pos + 1)) {
        ++pops;
    }
    EXPECT_EQ(athrows, 1u) << after;
    EXPECT_EQ(newRte, 1u) << after;
    EXPECT_EQ(pops, 1u) << after;
    // 真实 throw 区保持：真实表项（catch → astore handler）仍在场。
    EXPECT_NE(after.find("java/lang/RuntimeException"), std::string::npos) << after;
    EXPECT_NE(after.find("astore"), std::string::npos) << after;

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(ExcRewrite, AdjacentTrapsBothRestored)
{
    model::ResourceContainer container =
        makeContainer({{"ExcAdj", adjacentSource("ExcAdj")}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.bridges, 2u);
    EXPECT_EQ(dirty.insns, 12u);                   // 双桥六指令各一批
    EXPECT_EQ(dirty.failed, 0u);

    const std::string after = disassemble(classBytes(container, "ExcAdj.class"));
    EXPECT_EQ(after.find("athrow"), std::string::npos) << after;
    EXPECT_EQ(after.find("java/lang/RuntimeException"), std::string::npos) << after;

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 0u);
}

TEST(ExcRewrite, RejectedContainersByteIdentical)
{
    for (const auto& [name, source] :
         std::vector<std::pair<std::string, std::string>>{
             {"ExcReal", realThrowSource("ExcReal")},
             {"ExcCov", coveredSource("ExcCov")},
             {"ExcMut", mutualXSource("ExcMut")}}) {
        model::ResourceContainer container = makeContainer({{name, source}});
        const std::vector<ca::u8> before = classBytes(container, name + ".class");

        const ScanResult scanned = scan(container);
        ASSERT_EQ(scanned.sites.size(), 0u) << name;
        const DirtySet dirty = rewrite(container, scanned);
        EXPECT_EQ(dirty.uris.size(), 0u) << name;

        const std::vector<ca::u8>& after = classBytes(container, name + ".class");
        EXPECT_EQ(after, before) << name;          // 拒绝面：字节逐位不动
    }
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(ExcProof, ProofCoversCountAndResidual)
{
    model::ResourceContainer container =
        makeContainer({{"ExcJump", jumpEdgeSource("ExcJump")},
                       {"ExcReal", realThrowSource("ExcReal")}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);           // 真实 throw 区不在站内
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok);                       // 类数不增 + 脏类可重解析
    EXPECT_EQ(verdict.failed.size(), 0u);
    EXPECT_EQ(verdict.residual, 0u);
}

// ============================================================================
// ==== JVM 实跑（-Xverify:all）===============================================
// ============================================================================

TEST(ExcJvm, RestoredBridgesRunAndVerify)
{
    model::ResourceContainer container =
        makeContainer({{"ExcJvmMixed", jvmMixedSource("ExcJvmMixed")}});

    // 还原前夹具自证：桥形态在 -Xverify:all 下实跑即真值（105/7/5——桥路 /
    // 直落路 / 真实 throw 路）。
    const auto [rc0, out0] = runJavaVerify(classBytes(container, "ExcJvmMixed.class"),
                                           "ExcJvmMixed");
    if (rc0 == -1) GTEST_SKIP() << "no java";
    ASSERT_EQ(rc0, 0) << out0;
    EXPECT_NE(out0.find("105"), std::string::npos) << out0;
    EXPECT_NE(out0.find("7"), std::string::npos) << out0;
    EXPECT_NE(out0.find("5"), std::string::npos) << out0;

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.failed, 0u);

    // 还原后同值同验：原边直连真实目标，异常路径语义不变。
    const auto [rc1, out1] = runJavaVerify(classBytes(container, "ExcJvmMixed.class"),
                                           "ExcJvmMixed");
    ASSERT_EQ(rc1, 0) << out1;
    EXPECT_EQ(out1, out0) << out1;
}

}   // namespace
}   // namespace grunt::exc_bridge_restore
