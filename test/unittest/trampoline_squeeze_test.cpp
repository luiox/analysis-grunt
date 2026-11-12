/// @file        trampoline_squeeze_test.cpp
/// @brief       残留 `trampoline_squeeze` 的判据测试与迭代预算行为（jasm 夹具）。
#include "grunt/trampoline_squeeze.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace grunt::trampoline_squeeze {
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

/// 单蹦床夹具：iconst_1; goto L1; L1: ireturn（目标即顺序后继）。
const char* kTrampolineSource =
    ".super java/lang/Object\n"
    ".class public super fixture/TsHost {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      goto L1\n"
    "    L1:\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

std::string addFixture(model::ResourceContainer& container, const std::string& name,
                       const std::string& source)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, assemble(name, source)));
    return uri;
}

ca::usize countGotos(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    if (promoted.is_err()) throw std::runtime_error("产物无法 promote");
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    ca::usize          n  = 0;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (in->op == jbc::core::Op::GOTO || in->op == jbc::core::Op::GOTO_W) ++n;
    }
    return n;
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
// ==== SCAN ==================================================================
// ============================================================================

TEST(TrampolineScan, FindsSequentialGoto)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/TsHost", kTrampolineSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.classesSkippedByBudget, 0u);
    EXPECT_FALSE(scanned.sites[0].wide);
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(TrampolineRewrite, SqueezesTrampolineToNop)
{
    model::ResourceContainer container;
    const std::string        uri = addFixture(container, "fixture/TsHost", kTrampolineSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.squeezed, 1u);

    EXPECT_EQ(countGotos(classBytes(container, uri)), 0u) << "蹦床未消解";
}

/// rewrite 预算超限：站点复核指令数超限的站点保守放弃，产物保持原样。
TEST(TrampolineRewrite, OverBudgetLeavesTrampolineUntouched)
{
    model::ResourceContainer container;
    const std::string        uri = addFixture(container, "fixture/TsHost", kTrampolineSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    // 复核计数单位是方法指令数（本夹具 3 条）；预算 1 必在首站点即超限。
    const DirtySet dirty = rewrite(container, scanned, iter_budget::ClassBudget{1});
    EXPECT_EQ(dirty.uris.size(), 0u) << "超限类不许被改写";
    EXPECT_EQ(dirty.skippedByBudget, 1u);
    EXPECT_EQ(countGotos(classBytes(container, uri)), 1u) << "预算超限后蹦床不应被改";
}

// ============================================================================
// ==== 迭代预算（scan）=======================================================
// ============================================================================

TEST(TrampolineScan, OverBudgetSkipsClassConservatively)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/TsHost", kTrampolineSource);

    // 预算 1（方法 3 个位置）：首位置耗尽，站点不产出，类记预算跳过。
    const ScanResult starved = scan(container, iter_budget::ClassBudget{1});
    EXPECT_EQ(starved.sites.size(), 0u);
    EXPECT_EQ(starved.classesSkippedByBudget, 1u);
}

}   // namespace
}   // namespace grunt::trampoline_squeeze
