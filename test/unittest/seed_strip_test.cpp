/// @file        seed_strip_test.cpp
/// @brief       残留 `seed_strip` 的三段判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - SCAN：NEWARRAY+PUTSTATIC 头 + GETSTATIC/IASTORE 组固定形状命中
///   （spanCount 计数）；NOP 落点容忍；组内除法族拒绝；组先于头拒绝；
///   Long 字面量组拒绝（组内只允许 Int/Float）；clinit 外有访问拒绝
///   （访问面普查检查）；
/// - REWRITE：擦除后首条 NOP 保锚、其余指令消失的字节码断言；中段标签
///   放弃该字段（容器字节逐位不动）；
/// - PROOF：类数不增 + 脏类可重解析。
#include "grunt/seed_strip.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::seed_strip {
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

/// 种子建表夹具骨架：单 static [I 字段 + clinit，body 为指令文本。
std::string seedHostSource(const std::string& name, const std::string& body)
{
    return ".super java/lang/Object\n"
           ".class public super " + name + " {\n"
           "  .field static bfAT [I\n"
           "\n"
           "  .method static <clinit> ()V {\n"
           "    code: {\n" + body +
           "    }\n"
           "  }\n"
           "}\n";
}

/// grunt 生成器正形：头（newarray+putstatic）+ 两个存储组 + return。
std::string canonicalSeedBody(const std::string& name)
{
    const std::string f = name + ".bfAT";
    return
        "      iconst_4\n"
        "      newarray int\n"
        "      putstatic " + f + " [I\n"
        "      getstatic " + f + " [I\n"
        "      iconst_0\n"
        "      bipush 42\n"
        "      iastore\n"
        "      getstatic " + f + " [I\n"
        "      iconst_1\n"
        "      bipush 7\n"
        "      iastore\n"
        "      return\n";
}

/// clinit 的 promote 视图：指令 op 按布局序。
struct ClinitDump
{
    std::vector<jbc::core::Op> ops;
};

ClinitDump dumpClinit(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    const jbc::ir::ClassUnit& unit = session->unit();
    ca::usize                 clinit = static_cast<ca::usize>(-1);
    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        if (unit.symbols().str(unit.methods[mi].name) == "<clinit>") {
            clinit = mi;
            break;
        }
    }
    if (clinit == static_cast<ca::usize>(-1)) throw std::runtime_error("夹具缺 clinit");
    auto promoted = jbc::ir::promoteMethod(*session, clinit);
    if (promoted.is_err() || promoted.unwrap().get() == nullptr) {
        throw std::runtime_error("产物 clinit 无法 promote");
    }
    ClinitDump dump;
    for (const jbc::ir::InsnId id : promoted.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = promoted.unwrap()->dom.peek(id);
        if (in == nullptr) continue;
        dump.ops.push_back(in->op);
    }
    return dump;
}

ca::usize countOp(const ClinitDump& dump, jbc::core::Op op)
{
    ca::usize n = 0;
    for (const jbc::core::Op o : dump.ops) {
        if (o == op) ++n;
    }
    return n;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(SeedStripScan, FindsCanonicalSeedTable)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost", seedHostSource("fixture/SeedHost", canonicalSeedBody("fixture/SeedHost"))},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    const Site& site = result.sites.front();
    EXPECT_EQ(site.uri, "fixture/SeedHost.class");
    EXPECT_EQ(site.fieldName, "bfAT");
    EXPECT_EQ(site.spanCount, 3u) << "头 + 两个存储组";
    EXPECT_EQ(result.classesSkipped, 0u);
    EXPECT_FALSE(result.censusIncomplete);
}

TEST(SeedStripScan, ToleratesNopLandingSpots)
{
    // squeeze 改写遗留的 NOP 落点散布在头与组内：跨度收集跳过 NOP，命中形状。
    const std::string f = "fixture/SeedHost.bfAT";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         seedHostSource("fixture/SeedHost",
                        "      iconst_4\n"
                        "      newarray int\n"
                        "      putstatic " + f + " [I\n"
                        "      nop\n"
                        "      getstatic " + f + " [I\n"
                        "      nop\n"
                        "      iconst_0\n"
                        "      bipush 42\n"
                        "      nop\n"
                        "      iastore\n"
                        "      return\n")},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().spanCount, 2u) << "头 + 一个存储组";
}

TEST(SeedStripScan, RejectsDivisionInValueChain)
{
    // 除法族可能抛算术异常，不在值链词汇表：整字段放弃。
    const std::string f = "fixture/SeedHost.bfAT";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         seedHostSource("fixture/SeedHost",
                        "      iconst_4\n"
                        "      newarray int\n"
                        "      putstatic " + f + " [I\n"
                        "      getstatic " + f + " [I\n"
                        "      iconst_0\n"
                        "      bipush 7\n"
                        "      bipush 2\n"
                        "      idiv\n"
                        "      iastore\n"
                        "      return\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty()) << "除法族进值链必须整字段放弃";
}

TEST(SeedStripScan, RejectsGroupBeforeHead)
{
    // 读无建表头（组出现在 NEWARRAY+PUTSTATIC 之前）：不是本工具改出的形态。
    const std::string f = "fixture/SeedHost.bfAT";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         seedHostSource("fixture/SeedHost",
                        "      getstatic " + f + " [I\n"
                        "      iconst_0\n"
                        "      bipush 1\n"
                        "      iastore\n"
                        "      iconst_2\n"
                        "      newarray int\n"
                        "      putstatic " + f + " [I\n"
                        "      return\n")},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

TEST(SeedStripScan, RejectsLongLiteralInGroup)
{
    // 组内只允许 Int/Float 字面量（Long/Double 不是 grunt 形态）。
    model::ResourceContainer container = makeContainer({
        {"fixture/LongHost",
         ".super java/lang/Object\n"
         ".class public super fixture/LongHost {\n"
         "  .field static seeds [J\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      iconst_2\n"
         "      newarray long\n"
         "      putstatic fixture/LongHost.seeds [J\n"
         "      getstatic fixture/LongHost.seeds [J\n"
         "      iconst_0\n"
         "      ldc2_w 889226191L\n"
         "      lastore\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty()) << "Long 字面量组必须放弃";
}

TEST(SeedStripScan, RejectsAccessOutsideClinit)
{
    // 访问面普查检查：类内其他方法读、类外写，任一越出 clinit 即放弃。
    const std::string f = "fixture/SeedHost.bfAT";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         seedHostSource("fixture/SeedHost", canonicalSeedBody("fixture/SeedHost")) +
             "\n"
             "  .method public static read ()I {\n"
             "    code: {\n"
             "      getstatic " + f + " [I\n"
             "      iconst_0\n"
             "      iaload\n"
             "      ireturn\n"
             "    }\n"
             "  }\n"},
        {"fixture/Writer",
         ".super java/lang/Object\n"
         ".class public super fixture/Writer {\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      iconst_2\n"
         "      newarray int\n"
         "      putstatic " + f + " [I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty()) << "clinit 外有任何访问即放弃";
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(SeedStripRewrite, StripsTableKeepingAnchorNops)
{
    const std::string uri = "fixture/SeedHost.class";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost", seedHostSource("fixture/SeedHost", canonicalSeedBody("fixture/SeedHost"))},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.fields, 1u);
    EXPECT_EQ(dirty.attempted, 1u);
    EXPECT_EQ(dirty.failed, 0u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), uri);

    // 字节码断言：头与两组全部拆除，唯余尺寸压栈、三枚锚 NOP（newarray 头 +
    // 两个 getstatic 组锚；首条 NOP 保锚保 srcPc 由 DOM 在擦除前计数，落盘
    // 重开后 srcPc 已按新布局重排，外部可观察面 = NOP 的数量与相对次序）与
    // return。
    const ClinitDump after = dumpClinit(classBytes(container, uri));
    ASSERT_EQ(after.ops.size(), 5u);
    EXPECT_EQ(after.ops[0], jbc::core::Op::ICONST_4);
    EXPECT_EQ(after.ops[1], jbc::core::Op::NOP) << "newarray 头锚置 NOP";
    EXPECT_EQ(after.ops[2], jbc::core::Op::NOP) << "组一 getstatic 锚置 NOP";
    EXPECT_EQ(after.ops[3], jbc::core::Op::NOP) << "组二 getstatic 锚置 NOP";
    EXPECT_EQ(after.ops[4], jbc::core::Op::RETURN);
    EXPECT_EQ(countOp(after, jbc::core::Op::NEWARRAY), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::PUTSTATIC), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::GETSTATIC), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::IASTORE), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::NOP), 3u);
}

TEST(SeedStripRewrite, ToleratedNopsStayPutWhenStripping)
{
    // NOP 落点夹具的擦除：组锚 NOP + 遗留 NOP 都在，表体指令消失。
    const std::string f = "fixture/SeedHost.bfAT";
    const std::string uri = "fixture/SeedHost.class";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         seedHostSource("fixture/SeedHost",
                        "      iconst_4\n"
                        "      newarray int\n"
                        "      putstatic " + f + " [I\n"
                        "      nop\n"
                        "      getstatic " + f + " [I\n"
                        "      nop\n"
                        "      iconst_0\n"
                        "      bipush 42\n"
                        "      nop\n"
                        "      iastore\n"
                        "      return\n")},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.fields, 1u);

    const ClinitDump after = dumpClinit(classBytes(container, uri));
    ASSERT_EQ(after.ops.size(), 7u);
    EXPECT_EQ(after.ops[0], jbc::core::Op::ICONST_4);
    EXPECT_EQ(countOp(after, jbc::core::Op::NOP), 5u) << "头锚 + 组锚 + 3 枚遗留 NOP";
    EXPECT_EQ(countOp(after, jbc::core::Op::GETSTATIC), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::IASTORE), 0u);
    EXPECT_EQ(countOp(after, jbc::core::Op::BIPUSH), 0u);
    EXPECT_EQ(after.ops.back(), jbc::core::Op::RETURN);
}

TEST(SeedStripRewrite, MidSpanLabelAbandonsFieldBytesUntouched)
{
    // 中段标签（合并点）→ 擦除放弃该字段；fail-closed 不落半截，容器字节
    // 逐位不动。scan 仍产出站点（标签检查在 rewrite 的擦除步执行）。
    // 注意：jasm 丢弃无引用标签，标签必须由异常表边界实例化成真标签。
    const std::string f = "fixture/SeedHost.bfAT";
    const std::string uri = "fixture/SeedHost.class";
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost",
         ".super java/lang/Object\n"
         ".class public super fixture/SeedHost {\n"
         "  .field static bfAT [I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    exceptions: { { Lmid, LgrpEnd, Lsafe, Ljava/lang/RuntimeException; } },\n"
         "    code: {\n"
         "      iconst_4\n"
         "      newarray int\n"
         "      putstatic " + f + " [I\n"
         "      getstatic " + f + " [I\n"
         "      iconst_0\n"
         "    Lmid:\n"
         "      bipush 42\n"
         "    LgrpEnd:\n"
         "      iastore\n"
         "      return\n"
         "    Lsafe:\n"
         "      pop\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const std::vector<ca::u8> before = classBytes(container, uri);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u) << "标签检查在 rewrite 擦除步，scan 仍产出站点";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.attempted, 1u);
    EXPECT_EQ(dirty.failed, 1u);
    EXPECT_EQ(dirty.fields, 0u);
    EXPECT_TRUE(dirty.uris.empty()) << "字段放弃 = 类不写回";

    EXPECT_EQ(classBytes(container, uri), before) << "放弃字段后容器字节必须逐位不动";
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(SeedStripProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/SeedHost", seedHostSource("fixture/SeedHost", canonicalSeedBody("fixture/SeedHost"))},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

}   // namespace
}   // namespace grunt::seed_strip
