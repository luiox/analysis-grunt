/// @file        junk_region_strip_test.cpp
/// @brief       残留 `junk_region_strip` 的三段判据测试（jasm 夹具 + 程序化
///              代码字节替换）。
///
/// jasm 表达不了非法操作码，死区字节由夹具助手程序化注入：先汇编正常方法，
/// 再 decodeAttr(Code) 整体替换 `code` 向量为手排字节（jasm 对直线小方法不
/// 产 StackMapTable，替换后无帧一致性面；真实 jar 的帧锚在可达指令上，
/// 长度保持改写不动布局）。
///
/// 覆盖分工：
/// - 尾部死区：`bipush 3; ireturn; ff ff` → 2 字节 NOP 化；
/// - 孤儿死区：`goto` 跨过 3 字节死区段（无任何入边）→ 3 字节 NOP 化；
/// - handler 入口：异常 handler 指向死区之后的真码，死区夹在中间 → 死区
///   NOP 化、handler 字节不动（handler 是 JVM 可达入口，必须展开走查）；
/// - 拒绝面：死区落在分支目标上（可达路径解不开）→ 方法整条不动；
/// - 反证面：净方法字节逐位不动；
/// - PROOF：类数不增 + 脏类可重解析。
#include "grunt/junk_region_strip.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/writer.hpp"
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

namespace grunt::junk_region_strip {
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
        const std::string dump = "test/_jasm_fail_" + name + ".jasm";
        std::FILE*        f    = std::fopen(dump.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail + "（转储 " + dump
                                 + "）");
    }
    return std::move(assembled.value->bytes);
}

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&              uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

/// @brief 宿主类：两个方法——`a ()I`（直线 bipush/ireturn）与 `b ()I`
///        （goto 形，供孤儿死区注入）。
std::string hostSource()
{
    return ".super java/lang/Object\n"
           ".class public super Host {\n"
           "  .method public static a ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public static b ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      goto T\n"
           "    T:\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

struct ExcSpec
{
    ca::u32 startPc   = 0;
    ca::u32 endPc     = 0;
    ca::u32 handlerPc = 0;
};

/// @brief 把 `method` 的 Code 替换为手排字节（+ 可选异常表；嵌套属性清空，
///        手排字节与 jasm 布局无关，fixture-only 简化）。
std::vector<ca::u8> withRawCode(const std::vector<ca::u8>&      bytes,
                                const std::string&              method,
                                const std::vector<ca::u8>&      code,
                                const std::vector<ExcSpec>&     exc = {})
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("夹具重解析失败");
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) != method) continue;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) throw std::runtime_error("夹具 Code 解码失败");
            auto* bodyPtr = slot.mutate();
            if (bodyPtr == nullptr) throw std::runtime_error("夹具槽置脏失败");
            auto& body = static_cast<jbc::core::CodeAttr&>(*bodyPtr);
            body.code  = code;
            body.excTable.clear();
            body.attrs.clear();
            for (const ExcSpec& e : exc) {
                body.excTable.push_back(
                    jbc::core::CodeAttr::ExcEntry{e.startPc, e.endPc, e.handlerPc, 0});
            }
            auto encoded = jbc::core::writeClass(cf);
            if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
            return std::move(encoded).unwrap();
        }
    }
    throw std::runtime_error("夹具缺方法：" + method);
}

model::ResourceContainer makeContainer(
    std::initializer_list<std::pair<std::string, std::vector<ca::u8>>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, bytes] : classes) {
        const std::string uri = name + ".class";
        container.addResource(uri,
                              std::make_unique<model::ClassResource>(uri, bytes));
    }
    return container;
}

}   // namespace

// ==== SCAN + REWRITE（死区正例）===============================================

TEST(JunkRegionStripRewrite, NopsTrailingDeadRegion)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // a ()I：`10 03 ac`（bipush 3; ireturn）+ 尾部死区 ff ff。
    const std::vector<ca::u8> withJunk =
        withRawCode(host, "a", std::vector<ca::u8>{0x10, 0x03, 0xac, 0xff, 0xff});
    model::ResourceContainer container = makeContainer({{"Host", withJunk}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.junkBytes, 2u);
    EXPECT_EQ(scanned.methodsFailClosed, 0u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.nopBytes, 2u);

    // 死区字节原位变 00，可达字节不动。
    const std::vector<ca::u8> expectA =
        withRawCode(host, "a", std::vector<ca::u8>{0x10, 0x03, 0xac, 0x00, 0x00});
    EXPECT_EQ(classBytes(container, "Host.class"), expectA);
}

TEST(JunkRegionStripRewrite, NopsOrphanDeadRegionBetweenBranchAndTarget)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // b ()I：`03 | a7 00 03 | 04 | ac | ff ff ff`——pc0 iconst_0(1B)，pc1
    // goto 偏移 3 → 目标 pc4（iconst_1），pc5 ireturn，pc6-8 死区（无入边）。
    const std::vector<ca::u8> orphanJunk =
        withRawCode(host, "b",
                    std::vector<ca::u8>{0x03, 0xa7, 0x00, 0x03, 0x04, 0xac,
                                        0xff, 0xff, 0xff});
    model::ResourceContainer container = makeContainer({{"Host", orphanJunk}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.junkBytes, 3u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.nopBytes, 3u);
    const std::vector<ca::u8> expectB =
        withRawCode(host, "b",
                    std::vector<ca::u8>{0x03, 0xa7, 0x00, 0x03, 0x04, 0xac, 0x00,
                                        0x00, 0x00});
    EXPECT_EQ(classBytes(container, "Host.class"), expectB);
}

TEST(JunkRegionStripScan, HandlerEntryIsWalkedMiddleJunkStripped)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // a ()I：`10 03 ac | ff ff | 57 ac`——死区夹在 ireturn 与 handler 体之间，
    // 异常 handler（pc5）是 JVM 可达入口：可达 = {0-2} ∪ {5-6}，死区 {3-4}
    // NOP 化，handler 体字节不动。
    const std::vector<ca::u8> handlerJunk =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0x10, 0x03, 0xac, 0xff, 0xff, 0x57, 0xac},
                    std::vector<ExcSpec>{ExcSpec{0, 3, 5}});
    model::ResourceContainer container = makeContainer({{"Host", handlerJunk}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().ranges.size(), 1u);
    EXPECT_EQ(scanned.sites.front().ranges.front().first, 3u);
    EXPECT_EQ(scanned.sites.front().ranges.front().second, 2u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    const std::vector<ca::u8> expectHandler =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0x10, 0x03, 0xac, 0x00, 0x00, 0x57, 0xac},
                    std::vector<ExcSpec>{ExcSpec{0, 3, 5}});
    EXPECT_EQ(classBytes(container, "Host.class"), expectHandler);
}

// ==== switch 布局锁定（JVMS 6.5：default 在对齐填充之后）=====================

TEST(JunkRegionStripRewrite, NopsTrailingDeadAfterTableswitch)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // a ()I：`aa | 00 00 00 | default=20 | lo=5 | hi=5 | case(5)=20 | 04 | ac |
    // ff ff ff`——switch 占 pc0-19（default/case 都指 pc20），pc20 iconst_1，
    // pc21 ireturn，pc22-24 死区。
    const std::vector<ca::u8> switchJunk =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0xaa, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x04, 0xac, 0xff, 0xff, 0xff});
    model::ResourceContainer container = makeContainer({{"Host", switchJunk}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.junkBytes, 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.nopBytes, 3u);
    const std::vector<ca::u8> expect =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0xaa, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x04, 0xac, 0x00, 0x00, 0x00});
    EXPECT_EQ(classBytes(container, "Host.class"), expect);
}

TEST(JunkRegionStripRewrite, NopsTrailingDeadAfterLookupswitch)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // a ()I：`ab | 00 00 00 | default=20 | npairs=1 | match=5 | off=20 | 04 | ac
    // | ff ff ff`——switch 占 pc0-19，pc20 iconst_1，pc21 ireturn，pc22-24 死区。
    const std::vector<ca::u8> switchJunk =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0xab, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x00, 0x00, 0x00, 0x01,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x04, 0xac, 0xff, 0xff, 0xff});
    model::ResourceContainer container = makeContainer({{"Host", switchJunk}});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.junkBytes, 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.nopBytes, 3u);
    const std::vector<ca::u8> expect =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0xab, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x00, 0x00, 0x00, 0x01,
                                        0x00, 0x00, 0x00, 0x05,
                                        0x00, 0x00, 0x00, 0x14,
                                        0x04, 0xac, 0x00, 0x00, 0x00});
    EXPECT_EQ(classBytes(container, "Host.class"), expect);
}

// ==== 拒绝面 ==================================================================

TEST(JunkRegionStripScan, ReachableJunkFailsClosed)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    // b ()I：`03 | a7 00 03 | ff | 04 | ac`——goto 目标 = 1+3 = 4，落在 0xff
    // 上：可达路径解不开 → 方法整条 fail-closed，字节逐位不动。
    const std::vector<ca::u8> patched =
        withRawCode(host, "b",
                    std::vector<ca::u8>{0x03, 0xa7, 0x00, 0x03, 0xff, 0x04, 0xac});

    model::ResourceContainer container = makeContainer({{"Host", std::move(patched)}});
    const std::vector<ca::u8> before = classBytes(container, "Host.class");

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.methodsFailClosed, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "Host.class"), before);
}

TEST(JunkRegionStripScan, CleanMethodsLeaveBytesUntouched)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    model::ResourceContainer container = makeContainer({{"Host", host}});
    const std::vector<ca::u8> before = classBytes(container, "Host.class");

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
    EXPECT_EQ(scanned.methodsFailClosed, 0u);
    EXPECT_EQ(scanned.junkBytes, 0u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "Host.class"), before);
}

// ==== PROOF ===================================================================

TEST(JunkRegionStripProof, ClassCountAndReparse)
{
    const std::vector<ca::u8> host = assemble("Host", hostSource());
    const std::vector<ca::u8> proofJunk =
        withRawCode(host, "a",
                    std::vector<ca::u8>{0x10, 0x03, 0xac, 0xff, 0xff, 0xff});
    model::ResourceContainer container = makeContainer({{"Host", proofJunk}});

    const ca::usize before = 1;
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

}   // namespace grunt::junk_region_strip
