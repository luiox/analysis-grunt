/// @file        metadata_strip_test.cpp
/// @brief       残留 `metadata_strip` 的三段判据测试（树版三面迁移 + 拒绝面
///              反证）。
///
/// 夹具一律现场构建（jasm 现场汇编 + RawAttr 程序化补挂 ConstantValue，不
/// 落二进制、不依赖语料目录）：
/// - @Metadata 注解夹具：类/字段/方法三张挂载面各一条
///   `@dev/kasumi/internal/Metadata`（mv/d1/d2），另挂一条异名注解与一个
///   无注解 keeper 方法验证「只摘本家条目、其余原样」；
/// - 水印字段夹具：jasm 表达不了 ConstantValue，汇编后程序化给 static
///   String 字段补挂 RawAttr 形态的 ConstantValue（const_value_promote 同形，
///   值 = 水印串 / 非水印串各一）；
/// - 资源夹具：UnknownResource 直挂容器（命中 / 前缀非 .md / 前缀无探针 /
///   前缀不符四形态）；
/// - 敌意注解夹具：原始字节千层嵌套注解（jasm 表达不了，annotation_guard
///   同形）钉深度预检 fail-closed 面。
///
/// 覆盖分工：
/// - SCAN：三面普查 + 水印字段普查 + 资源清单/盘点；Era 未判整面关闭；
///   敌意槽 blocked 记账不产出站点；
/// - REWRITE：三面摘注解 + 水印字段删除 + 资源删除；拒绝面容器逐位不动；
/// - PROOF：类数门 + 资源对账门（数不增 + 清单逐条不在场）+ 三面 residual。
#include "grunt/metadata_strip.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::metadata_strip {
namespace {

namespace model = mjbase::model;
namespace knowledge_ns = grunt::knowledge;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        assembled =
        service.assemble(utf16(name + ".jasm").ref(), utf16(source).ref());
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
    auto                         result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

void addClass(model::ResourceContainer& container, const std::string& name,
              const std::string& source)
{
    const std::string uri = name + ".class";
    container.addResource(uri,
                          std::make_unique<model::ClassResource>(uri, assemble(name, source)));
}

void addRawClass(model::ResourceContainer& container, const std::string& name,
                 const std::vector<ca::u8>& bytes)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
}

void addResource(model::ResourceContainer& container, const std::string& uri,
                 const std::string& content)
{
    std::vector<ca::u8> bytes(content.begin(), content.end());
    container.addResource(uri, std::make_unique<model::UnknownResource>(uri, std::move(bytes)));
}

const std::vector<ca::u8>& bytesOf(const model::ResourceContainer& container,
                                   const std::string&              uri)
{
    const model::IResource* raw = container.getResource(uri);
    if (raw == nullptr) throw std::runtime_error("夹具缺条目：" + uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls != nullptr) return cls->get();
    const auto* unk = ca::core::tag_cast::dyn_cast<const model::UnknownResource>(raw);
    if (unk == nullptr) throw std::runtime_error("夹具条目类型不符：" + uri);
    return unk->get();
}

std::string textOf(const model::ResourceContainer& container, const std::string& uri)
{
    const std::vector<ca::u8>& bytes = bytesOf(container, uri);
    return std::string(bytes.begin(), bytes.end());
}

// ============================================================================
// ==== 夹具 ==================================================================
// ============================================================================

/// @brief @Metadata 三面夹具：类面 + 字段面 + 方法面各一条本家注解，类面另
///        挂一条异名注解（保留面反证），方法面另有无注解 keeper（其余成员
///        原样反证）。带注解的字段按树版口径整字段删除。
std::string annoHostSource()
{
    return ".invisible-annotation dev/kasumi/internal/Metadata {\n"
           "    mv: 1,\n"
           "    d1: \"class-d1\",\n"
           "    d2: \"class-d2\"\n"
           "}\n"
           ".invisible-annotation dev/other/Keep {\n"
           "    tag: \"keep-me\"\n"
           "}\n"
           ".super java/lang/Object\n"
           ".class public super AnnoHost {\n"
           "\n"
           "  .invisible-annotation dev/kasumi/internal/Metadata {\n"
           "      mv: 2,\n"
           "      d1: \"field-d1\",\n"
           "      d2: \"field-d2\"\n"
           "  }\n"
           "  .field public static mark Ljava/lang/String;\n"
           "\n"
           "  .field public static plain Ljava/lang/String;\n"
           "\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "\n"
           "  .invisible-annotation dev/kasumi/internal/Metadata {\n"
           "      mv: 3,\n"
           "      d1: \"method-d1\",\n"
           "      d2: \"method-d2\"\n"
           "  }\n"
           "  .method public static keeper ()I {\n"
           "    code: {\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 水印字段夹具：static String 字段两个（wmField / okField），
///        ConstantValue 由 withConstantValue 程序化补挂。
std::string watermarkHostSource()
{
    return ".super java/lang/Object\n"
           ".class public super WmHost {\n"
           "  .field public static wmField Ljava/lang/String;\n"
           "  .field public static okField Ljava/lang/String;\n"
           "\n"
           "  .method public <init> ()V {\n"
           "    code: {\n"
           "      aload 0\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// @brief 给汇编产物的若干 static String 字段程序化补挂 ConstantValue
///        （RawAttr 2 字节 CP 索引，const_value_promote 新建同形——jasm
///        表达不了 ConstantValue 属性）。@return 补挂后的类字节。
std::vector<ca::u8> attachConstantValues(
    const std::string& name, const std::string& source,
    const std::vector<std::pair<std::string, std::string>>& fieldValues)
{
    const std::vector<ca::u8> bytes = assemble(name, source);
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("夹具重解析失败：" + name);
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    for (const auto& [fieldName, value] : fieldValues) {
        jbc::core::MemberInfo* target = nullptr;
        for (jbc::core::MemberInfo& f : cf.fields) {
            if (cf.utf8(f.nameCp) == fieldName) target = &f;
        }
        if (target == nullptr) throw std::runtime_error("夹具缺字段：" + fieldName);
        const ca::u32 strIdx = cf.cp.addString(cf.cp.addUtf8(value));
        if (strIdx == 0 || strIdx > 0xFFFF) throw std::runtime_error("夹具池装配失败");
        std::vector<ca::u8> payload{static_cast<ca::u8>(strIdx >> 8),
                                    static_cast<ca::u8>(strIdx & 0xFF)};
        target->attrs.push_back(jbc::core::AttrSlot::makeNew(
            cf.cp.addUtf8("ConstantValue"),
            std::make_unique<jbc::core::RawAttr>(std::move(payload))));
    }
    auto encoded = jbc::core::writeClass(cf);
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败：" + name);
    return std::move(encoded).unwrap();
}

// ---- 原始字节敌意夹具（annotation_guard 同形：jasm 表达不了千层嵌套）------

void putU2(std::vector<ca::u8>& out, ca::u16 v)
{
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

void putU4(std::vector<ca::u8>& out, ca::u32 v)
{
    out.push_back(static_cast<ca::u8>(v >> 24));
    out.push_back(static_cast<ca::u8>(v >> 16));
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

/// @brief 最小 classfile（V1_8）：this=T、super=java/lang/Object、零成员，
///        单个类级属性（RuntimeInvisibleAnnotations）。
std::vector<ca::u8> hostileClassBytes()
{
    std::vector<ca::u8> cp;
    ca::u16             next = 1;
    const auto utf8 = [&](std::string_view s) {
        const ca::u16 idx = next++;
        cp.push_back(1);
        putU2(cp, static_cast<ca::u16>(s.size()));
        cp.insert(cp.end(), s.begin(), s.end());
        return idx;
    };
    const auto clazz = [&](ca::u16 nameIdx) {
        const ca::u16 idx = next++;
        cp.push_back(7);
        putU2(cp, nameIdx);
        return idx;
    };
    const ca::u16 nameT      = utf8("T");
    const ca::u16 classT     = clazz(nameT);
    const ca::u16 nameObject = utf8("java/lang/Object");
    const ca::u16 classObj   = clazz(nameObject);
    const ca::u16 attrName   = utf8("RuntimeInvisibleAnnotations");

    std::vector<ca::u8> out;
    putU4(out, 0xCAFEBABEu);
    putU2(out, 0);
    putU2(out, 0x34);
    putU2(out, next);   // cpCount（槽 0 无效）
    out.insert(out.end(), cp.begin(), cp.end());
    putU2(out, 0x0021);   // public super
    putU2(out, classT);
    putU2(out, classObj);
    putU2(out, 0);   // interfaces
    putU2(out, 0);   // fields
    putU2(out, 0);   // methods
    putU2(out, 1);   // class attrs = 1

    // 深嵌注解载荷（5000 层 '@'，远超 kMaxAnnotationNesting=255）：守卫缺席
    // 时 decodeAttr 直接栈溢出；下标一律落属性名槽（本测试不解码，无效即查）。
    std::vector<ca::u8> p;
    putU2(p, 1);       // num_annotations
    putU2(p, attrName);   // type_index
    putU2(p, 1);       // num_pairs
    for (ca::u32 i = 0; i < 5000; ++i) {
        putU2(p, attrName);   // element_name
        p.push_back('@');     // nested annotation
        putU2(p, attrName);   // type_index
        putU2(p, 1);          // num_pairs
    }
    putU2(p, attrName);   // element_name（叶）
    p.push_back('s');     // string 值
    putU2(p, attrName);   // const_value_index

    putU2(out, attrName);
    putU4(out, static_cast<ca::u32>(p.size()));
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

/// @brief Kasumi 正例主容器：注解三面类 + 水印/普通双 ConstantValue 字段类
///        + 反 LLM 资源命中条。
model::ResourceContainer kasumiContainer()
{
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());
    addRawClass(container, "WmHost",
                attachConstantValues("WmHost", watermarkHostSource(),
                                     {{"wmField", "PROTECTED BY Kasumi. Do not distribute."},
                                      {"okField", "hello world"}}));
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                "# analysis\nwe reject anti-llm tooling.\n");
    return container;
}

ScanInput kasumiInput()
{
    ScanInput input;
    input.era = knowledge_ns::Era::Kasumi;
    return input;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(MetadataStripScan, CountsAnnosOnThreeFacesAndWatermarkFields)
{
    model::ResourceContainer container = kasumiContainer();

    const ScanResult result = scan(container, kasumiInput());
    ASSERT_EQ(result.sites.size(), 2u);
    EXPECT_EQ(result.totalAnnos, 3u);   // 类面 + 字段面 + 方法面
    EXPECT_EQ(result.totalFields, 2u);  // 带注解字段 + 水印 ConstantValue 字段
    ASSERT_EQ(result.resources.size(), 1u);
    EXPECT_EQ(result.resourceCandidates, 0u);
    EXPECT_EQ(result.annoBlocked, 0u);
    EXPECT_FALSE(result.eraBlocked);

    // 站点序随容器 unordered 迭代（mjbase ResourceContainer 契约无序保证），
    // 按 uri 取面，不按 front/back。
    const Site* anno = nullptr;
    const Site* wm   = nullptr;
    for (const Site& site : result.sites) {
        if (site.uri == "AnnoHost.class") anno = &site;
        if (site.uri == "WmHost.class")   wm   = &site;
    }
    ASSERT_NE(anno, nullptr);
    ASSERT_NE(wm, nullptr);
    EXPECT_EQ(anno->annos, 3u);
    EXPECT_EQ(anno->fields, 1u);   // mark 字段带注解 → 整字段删除
    EXPECT_EQ(wm->annos, 0u);
    EXPECT_EQ(wm->fields, 1u);   // wmField 水印 ConstantValue（okField 不命中）
}

TEST(MetadataStripScan, KeepsPlainConstantValueField)
{
    // 非水印 ConstantValue 字段不进站点（树版只删水印判据命中面）。
    model::ResourceContainer container;
    addRawClass(container, "PlainWm",
                attachConstantValues("PlainWm", watermarkHostSource(), {{"okField", "hello world"}}));

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_TRUE(result.sites.empty());
    EXPECT_EQ(result.totalFields, 0u);
}

TEST(MetadataStripScan, InventoriesResourcesByShapeAndProbes)
{
    model::ResourceContainer container;
    // 命中：形状 + 探针双证齐。
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                "# analysis\nwe reject anti-llm tooling.\n");
    // 前缀命中但非 .md：候选盘点，不删。
    addResource(container, "META-INF/Kasumi/README.txt", "jailbreak guard notes\n");
    // 前缀 + .md 但探针缺席：候选盘点，不删（树版返回 false 同口径）。
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-9-plain.md",
                "# harmless release notes\n");
    // 前缀不符：完全不在视野。
    addResource(container, "assets/data.bin", "PROTECTED BY Kasumi\n");

    const ScanResult result = scan(container, kasumiInput());
    ASSERT_EQ(result.resources.size(), 1u);
    EXPECT_EQ(result.resources.front(), "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md");
    EXPECT_EQ(result.resourceCandidates, 2u);
}

TEST(MetadataStripScan, BlocksEverythingWhenEraUnknown)
{
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                "anti-llm\n");

    ScanInput input;
    input.era = knowledge_ns::Era::Unknown;
    const ScanResult result = scan(container, input);
    EXPECT_TRUE(result.eraBlocked);
    EXPECT_TRUE(result.sites.empty());
    EXPECT_TRUE(result.resources.empty());
    EXPECT_EQ(result.totalAnnos, 0u);
    EXPECT_EQ(result.totalFields, 0u);
    EXPECT_EQ(result.resourceCandidates, 0u);
}

TEST(MetadataStripScan, HostileAnnoSlotBlockedNotASite)
{
    // 深嵌敌意注解槽：深度预检拦下（fail-closed 记账），不产出站点。
    model::ResourceContainer container;
    addRawClass(container, "Hostile", hostileClassBytes());

    const ScanResult result = scan(container, kasumiInput());
    EXPECT_TRUE(result.sites.empty());
    EXPECT_EQ(result.annoBlocked, 1u);
    EXPECT_EQ(result.totalAnnos, 0u);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(MetadataStripRewrite, StripsAnnosOnThreeFacesKeepsOthers)
{
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.annos, 3u);
    EXPECT_EQ(dirty.fields, 1u);   // mark 字段随注解整字段删除
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_TRUE(dirty.removedResources.empty());

    const std::string after = disassemble(bytesOf(container, "AnnoHost.class"));
    EXPECT_EQ(after.find("dev/kasumi/internal/Metadata"), std::string::npos) << after;
    EXPECT_NE(after.find("dev/other/Keep"), std::string::npos) << after;   // 异名注解保留
    EXPECT_NE(after.find("keeper"), std::string::npos) << after;           // keeper 方法原样
    EXPECT_EQ(after.find("mark"), std::string::npos) << after;             // 注解字段已删
    EXPECT_NE(after.find("plain"), std::string::npos) << after;            // 无注解字段原样
}

TEST(MetadataStripRewrite, DeletesWatermarkFieldKeepsPlainField)
{
    // wmField=水印 / okField=普通：合成版两字段都带 ConstantValue，只删水印
    // 判据命中面；死串（kasumi_ 前缀）随压池带走。
    model::ResourceContainer container;
    addRawClass(container, "WmHost",
                attachConstantValues("WmHost", watermarkHostSource(),
                                     {{"wmField", "kasumi_19rzezp3uywn6"},
                                      {"okField", "hello world"}}));

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().fields, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.fields, 1u);
    EXPECT_EQ(dirty.annos, 0u);

    const std::string after = disassemble(bytesOf(container, "WmHost.class"));
    EXPECT_EQ(after.find("wmField"), std::string::npos) << after;
    EXPECT_EQ(after.find("kasumi_"), std::string::npos) << after;      // 死串随压池带走
    EXPECT_NE(after.find("okField"), std::string::npos) << after;      // 普通字段原样
}

TEST(MetadataStripRewrite, DeletesInventoryResourcesKeepsCandidates)
{
    model::ResourceContainer container;
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                "anti-llm\n");
    addResource(container, "META-INF/Kasumi/README.txt", "jailbreak notes\n");
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-9-plain.md",
                "# harmless\n");
    addResource(container, "assets/data.bin", "PROTECTED BY Kasumi\n");
    addResource(container, "META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n");

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.resources.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.removedResources.size(), 1u);
    EXPECT_EQ(dirty.removedResources.front(),
              "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md");
    EXPECT_EQ(dirty.resourceCandidates, 0u);
    EXPECT_FALSE(container.hasResource("META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md"));
    // 未命中面逐位不动。
    EXPECT_EQ(textOf(container, "META-INF/Kasumi/README.txt"), "jailbreak notes\n");
    EXPECT_EQ(textOf(container, "META-INF/Kasumi/cybersecurity/analysis-9-plain.md"),
              "# harmless\n");
    EXPECT_EQ(textOf(container, "assets/data.bin"), "PROTECTED BY Kasumi\n");
    EXPECT_EQ(textOf(container, "META-INF/MANIFEST.MF"), "Manifest-Version: 1.0\n");
}

TEST(MetadataStripRewrite, KeepsEveryByteOnEraUnknownAndHostile)
{
    // 拒绝面全景：Era 未判（整面关闭）——类与资源逐位保持；敌意注解类
    // 同罐在场不产出站点、字节原样。
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());
    addRawClass(container, "Hostile", hostileClassBytes());
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md", "anti-llm\n");
    addResource(container, "assets/data.bin", "keep\n");

    std::vector<std::vector<ca::u8>> before;
    for (const char* uri : {"AnnoHost.class", "Hostile.class",
                            "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                            "assets/data.bin"}) {
        before.push_back(bytesOf(container, uri));
    }

    ScanInput input;
    input.era = knowledge_ns::Era::Unknown;
    const ScanResult scanned = scan(container, input);
    EXPECT_TRUE(scanned.eraBlocked);
    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_TRUE(dirty.removedResources.empty());

    const std::vector<const char*> uris = {"AnnoHost.class", "Hostile.class",
                                           "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md",
                                           "assets/data.bin"};
    for (ca::usize i = 0; i < uris.size(); ++i) {
        EXPECT_EQ(bytesOf(container, uris[i]), before[i]) << uris[i] << " 被改写";
    }

    // 敌意类独占面：Kasumi Era 下也不产出站点、字节原样（fail-closed 不摘）。
    model::ResourceContainer hostileOnly;
    addRawClass(hostileOnly, "Hostile", hostileClassBytes());
    const std::vector<ca::u8> hostileBefore = bytesOf(hostileOnly, "Hostile.class");
    const ScanResult hs = scan(hostileOnly, kasumiInput());
    EXPECT_TRUE(hs.sites.empty());
    EXPECT_EQ(hs.annoBlocked, 1u);
    const DirtySet hd = rewrite(hostileOnly, hs);
    EXPECT_TRUE(hd.uris.empty());
    EXPECT_EQ(bytesOf(hostileOnly, "Hostile.class"), hostileBefore);
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(MetadataStripProof, DoubleGateAndResidualZero)
{
    model::ResourceContainer container = kasumiContainer();

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 2u);
    ASSERT_EQ(scanned.resources.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.classes, 2u);
    EXPECT_EQ(dirty.removedResources.size(), 1u);

    const ProofResult verdict = proof(container, dirty, 2, 3);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_TRUE(verdict.failed.empty());
    EXPECT_EQ(verdict.residualAnnos, 0u);
    EXPECT_EQ(verdict.residualFields, 0u);
    EXPECT_EQ(verdict.residualResources, 0u);
}

TEST(MetadataStripProof, ResidualCountsWhenNothingRewritten)
{
    // 没跑 rewrite：residual 如实报告三面残留（双门不受牵连：类数不变 +
    // 空清单对账通过）。
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());
    addResource(container, "META-INF/Kasumi/cybersecurity/analysis-0-deadbeef.md", "anti-llm\n");

    const ScanResult scanned = scan(container, kasumiInput());
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.resources.size(), 1u);

    DirtySet dirty;   // 空：没改任何东西
    const ProofResult verdict = proof(container, dirty, 1, 2);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residualAnnos, 3u);
    EXPECT_EQ(verdict.residualFields, 1u);
    EXPECT_EQ(verdict.residualResources, 1u);
}

// ============================================================================
// ==== Era 指纹（@Metadata 注解探针，knowledge_era_test 未覆盖的第三指纹）====
// ============================================================================

TEST(MetadataStripProbeEra, MetadataAnnoFingerprintHitsKasumi)
{
    model::ResourceContainer container;
    addClass(container, "AnnoHost", annoHostSource());
    EXPECT_EQ(knowledge_ns::probeEra(container), knowledge_ns::Era::Kasumi);
}

}   // namespace grunt::metadata_strip
