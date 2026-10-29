/// @file        predicate_fold_test.cpp
/// @brief       残留 `predicate_fold` 的三段判据测试 + promote/erase 语义哨兵。
#include "grunt/predicate_fold.hpp"

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

namespace grunt::predicate_fold {
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

std::vector<ca::u8> asJava17(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("夹具无法解析");
    session->backing().major = 61;
    auto encoded = jbc::core::writeClass(session->backing());
    if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
    return std::move(encoded).unwrap();
}

/// 恒假双常量谓词 + 死块：iconst_1; iconst_0; if_icmple Ldead; iconst_5;
/// ireturn; Ldead: iconst_0; ireturn
const char* kPredSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Pred {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      iconst_0\n"
    "      if_icmple Ldead\n"
    "      iconst_5\n"
    "      ireturn\n"
    "    Ldead:\n"
    "      iconst_0\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

TEST(PredicateFoldScan, FindsConstantPredicate)
{
    model::ResourceContainer container;
    const std::string        uri = "fixture/Pred.class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, asJava17(assemble("fixture/Pred", kPredSource))));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].uri, uri);
}

TEST(PredicateFoldRewrite, FoldsAndPersists)
{
    model::ResourceContainer container;
    const std::string        uri = "fixture/Pred.class";
    auto bytes = asJava17(assemble("fixture/Pred", kPredSource));
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    // ===== promote/erase 语义哨兵：改写必须真的改变方法体 =====
    auto session = openSession(cls->get());
    ASSERT_TRUE(session != nullptr);
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    ASSERT_TRUE(promoted.is_ok());
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    // 折叠后：操作数区被压成一条 goto_w，死块不可达但布局仍在（无 dead_code）。
    ca::usize branches = 0;
    ca::usize nops     = 0;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (in->op == jbc::core::Op::IF_ICMPLE) ++branches;
        if (in->op == jbc::core::Op::NOP) ++nops;
    }
    EXPECT_EQ(branches, 0u) << "改写未落盘：if_icmple 仍在";
    EXPECT_EQ(nops, 1u) << "改写未落盘：not-taken 锚未置 NOP";

    // 确定性哨兵：同一段字节连开三个 session，逐个数分支。
    for (int round = 0; round < 3; ++round) {
        auto rs = openSession(cls->get());
        ASSERT_TRUE(rs != nullptr);
        auto rp = jbc::ir::promoteMethod(*rs, 0);
        ASSERT_TRUE(rp.is_ok());
        ca::usize rb = 0;
        std::string ops;
        for (const jbc::ir::InsnId id : rp.unwrap()->dom.order()) {
            const jbc::ir::Insn* in = rp.unwrap()->dom.peek(id);
            if (in->op == jbc::core::Op::IF_ICMPLE) ++rb;
            ops += std::to_string(static_cast<int>(in->op)) + " ";
        }
        EXPECT_EQ(rb, 0u) << "round " << round << " ops: " << ops;
    }

    // 再哨兵：session 重开后字节里真的没有 if_icmple 了。
    auto session2 = openSession(cls->get());
    ASSERT_TRUE(session2 != nullptr);
    auto promoted2 = jbc::ir::promoteMethod(*session2, 0);
    ASSERT_TRUE(promoted2.is_ok());
    for (const jbc::ir::InsnId id : promoted2.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = promoted2.unwrap()->dom.peek(id);
        ASSERT_TRUE(in != nullptr);
        ASSERT_NE(in->op, jbc::core::Op::IF_ICMPLE)
            << "session 重开后仍见 if_icmple：容器字节未更新";
    }
}

}   // namespace
}   // namespace grunt::predicate_fold
