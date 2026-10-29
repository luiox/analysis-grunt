/// @file        flags_strip_test.cpp
/// @brief       残留 `flags_strip` 的三段判据测试。
///
/// 夹具用 jasm 源码现场汇编（synthetic/bridge 是合法访问关键字），覆盖分工：
/// - SCAN：static 的伪 BRIDGE/SYNTHETIC 进站点；实例成员、无标志成员、
///   枚举类、旧版本类文件全部不进；
/// - REWRITE：按站点掩码清位，非站点成员原样；
/// - PROOF：类数不增 + 脏类可重新解析。
#include "grunt/flags_strip.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
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

namespace grunt::flags_strip {
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
    if (!assembled.isOk()) throw std::runtime_error("jasm 夹具汇编失败：" + name);
    return std::move(assembled.value->bytes);
}

/// 汇编器默认产出较旧的 class 版本；判据门要求 major >= 55，这里统一抬到 61。
std::vector<ca::u8> asJava17(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("夹具无法解析");
    session->backing().major = 61;
    auto encoded = jbc::core::writeClass(session->backing());
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
    return std::move(encoded).unwrap();
}

model::ResourceContainer makeContainer(
    std::initializer_list<std::pair<std::string, std::string>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, source] : classes) {
        const std::string uri = name + ".class";
        container.addResource(
            uri, std::make_unique<model::ClassResource>(uri, asJava17(assemble(name, source))));
    }
    return container;
}

/// 夹具类：static/实例 × 有/无伪标志 × 方法/字段 全组合。
const char* kFlaggedSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Flagged {\n"
    "  .field public static synthetic sfield I\n"
    "  .field public static nfield I\n"
    "  .field public synthetic ifield I\n"
    "  .method public static synthetic bridge sm ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "  .method public synthetic bridge im ()I {\n"
    "    code: {\n"
    "      iconst_2\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "  .method public static ok ()I {\n"
    "    code: {\n"
    "      iconst_3\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

std::size_t countMemberSites(const ScanResult& result, bool isMethod)
{
    std::size_t n = 0;
    for (const FlagSite& s : result.sites) {
        if (s.isMethod == isMethod) ++n;
    }
    return n;
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(FlagsStripScan, MarksOnlyStaticBogusFlags)
{
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});
    const ScanResult         result    = scan(container);

    // sfield（static synthetic）+ sm（static bridge synthetic）各一条。
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(countMemberSites(result, true), 1u);
    EXPECT_EQ(countMemberSites(result, false), 1u);
    EXPECT_EQ(result.sites.front().clearMask,
              static_cast<ca::u16>(0x0040 | 0x1000));
}

TEST(FlagsStripScan, PreservesDollarNamedMembers)
{
    // 编译器合成物一律带 $（access$000、$assertionsDisabled）：$ 名字成员
    // 即使 static synthetic/bridge 也不清。
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});

    auto session = openSession(classBytes(container, "fixture/Flagged.class"));
    ASSERT_TRUE(session != nullptr);
    jbc::core::ClassFile& cf = session->backing();
    // sm 的名字改成 $access$000（名字形状门生效的前提：标志保持伪造态）
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) == "sm") m.nameCp = cf.cp.addUtf8("$access$000");
    }
    // 新增一个 $assertionsDisabled 风格的 static synthetic 字段
    jbc::core::MemberInfo f;
    f.access = 0x1009;
    f.nameCp = cf.cp.addUtf8("$assertionsDisabled");
    f.descCp = cf.cp.addUtf8("Z");
    cf.fields.push_back(std::move(f));
    auto encoded = jbc::core::writeClass(cf);
    ASSERT_FALSE(encoded.is_err());
    container.replaceResource(std::string("fixture/Flagged.class"),
                              std::make_unique<model::ClassResource>(
                                  std::string("fixture/Flagged.class"),
                                  std::move(encoded).unwrap()));

    const ScanResult result = scan(container);
    // sfield（static synthetic，普通名）仍是站点；两个 $ 名字成员都不是。
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_FALSE(result.sites.front().isMethod);
}

TEST(FlagsStripScan, ClearsOldClassFileVersionToo)
{
    // $ 名字门与版本无关：major 52 的伪标志 static synthetic 照样收。
    model::ResourceContainer container;
    const std::string        uri = "fixture/Flagged.class";
    auto session = openSession(assemble("fixture/Flagged", kFlaggedSource));
    ASSERT_TRUE(session != nullptr);
    session->backing().major = 52;
    auto encoded = jbc::core::writeClass(session->backing());
    ASSERT_FALSE(encoded.is_err());
    container.addResource(
        uri, std::make_unique<model::ClassResource>(uri, std::move(encoded).unwrap()));

    const ScanResult result = scan(container);
    EXPECT_EQ(result.sites.size(), 2u);
}

TEST(FlagsStripScan, SkipsEnumClasses)
{
    // 枚举的 $VALUES 等机制字段是真实的 static synthetic：整类跳过。
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});

    auto session = openSession(classBytes(container, "fixture/Flagged.class"));
    ASSERT_TRUE(session != nullptr);
    session->backing().access |= 0x4000;   // ACC_ENUM
    auto encoded = jbc::core::writeClass(session->backing());
    ASSERT_FALSE(encoded.is_err());
    container.replaceResource(std::string("fixture/Flagged.class"),
                              std::make_unique<model::ClassResource>(
                                  std::string("fixture/Flagged.class"),
                                  std::move(encoded).unwrap()));

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(FlagsStripRewrite, ClearsExactlyTheBogusBits)
{
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});
    const ScanResult         scanned   = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.membersCleared, 2u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    auto session = openSession(classBytes(container, "fixture/Flagged.class"));
    ASSERT_TRUE(session != nullptr);
    const jbc::core::ClassFile& cf = session->backing();

    // 静态成员：两个标志都清了。
    bool sawStaticMethod = false, sawStaticField = false;
    for (const jbc::core::MemberInfo& m : cf.methods) {
        if ((m.access & 0x0008) != 0 && cf.utf8(m.nameCp) == "sm") {
            sawStaticMethod = true;
            EXPECT_EQ(m.access & (0x0040 | 0x1000), 0u);
        }
    }
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if ((f.access & 0x0008) != 0 && cf.utf8(f.nameCp) == "sfield") {
            sawStaticField = true;
            EXPECT_EQ(f.access & 0x1000, 0u);
        }
    }
    ASSERT_TRUE(sawStaticMethod);
    ASSERT_TRUE(sawStaticField);

    // 实例成员与干净成员：原样。
    for (const jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) == "im") {
            EXPECT_NE(m.access & (0x0040 | 0x1000), 0u);
        }
        if (cf.utf8(m.nameCp) == "ok") {
            EXPECT_EQ(m.access & (0x0040 | 0x1000), 0u);
        }
    }
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if (cf.utf8(f.nameCp) == "ifield") {
            EXPECT_NE(f.access & 0x1000, 0u);
        }
    }
}

TEST(FlagsStripRewrite, LeavesContainerUntouchedWhenNoSites)
{
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});

    auto session = openSession(classBytes(container, "fixture/Flagged.class"));
    ASSERT_TRUE(session != nullptr);
    session->backing().access |= 0x4000;
    auto encoded = jbc::core::writeClass(session->backing());
    ASSERT_FALSE(encoded.is_err());
    container.replaceResource(std::string("fixture/Flagged.class"),
                              std::make_unique<model::ClassResource>(
                                  std::string("fixture/Flagged.class"),
                                  std::move(encoded).unwrap()));
    const std::vector<ca::u8> before = classBytes(container, "fixture/Flagged.class");

    const ScanResult scanned = scan(container);
    const DirtySet   dirty   = rewrite(container, scanned);

    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "fixture/Flagged.class"), before);
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(FlagsStripProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});
    const ScanResult         scanned   = scan(container);
    const DirtySet           dirty     = rewrite(container, scanned);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

TEST(FlagsStripProof, FailsWhenClassCountChanges)
{
    model::ResourceContainer container = makeContainer({{"fixture/Flagged", kFlaggedSource}});
    const ScanResult         scanned   = scan(container);
    const DirtySet           dirty     = rewrite(container, scanned);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_FALSE(verdict.ok);
    ASSERT_FALSE(verdict.failed.empty());
    EXPECT_EQ(verdict.failed.front(), std::string("class-count"));
}

}   // namespace
}   // namespace grunt::flags_strip
