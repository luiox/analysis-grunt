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
    // 折叠后：NOT-TAKEN 锚置 NOP，操作数区与分支本体消失，死块不可达但
    // 布局仍在（无 dead_code）。
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

/// 恒真双常量谓词：iconst_0; iconst_0; if_icmple Ldead，0<=0 恒成立，走
/// TAKEN 面。折叠必须落 [ICONST_0, IFEQ→Ldead] 恒真条件跳转，不得插无条件
/// goto（HotSpot 要求无条件跳转后的段首带帧，被擦延续段无帧即炸）。
const char* kPredTakenSource =
    ".super java/lang/Object\n"
    ".class public super fixture/PredTaken {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_0\n"
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

TEST(PredicateFoldRewrite, TakenFoldsToAlwaysTrueConditional)
{
    model::ResourceContainer container;
    const std::string        uri = "fixture/PredTaken.class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, asJava17(assemble("fixture/PredTaken", kPredTakenSource))));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    auto session = openSession(cls->get());
    ASSERT_TRUE(session != nullptr);
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    ASSERT_TRUE(promoted.is_ok());
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();

    ca::usize icmps    = 0;
    ca::usize ifeqs    = 0;
    ca::usize gotos    = 0;
    const jbc::ir::Insn* ifeqInsn = nullptr;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        if (in->op == jbc::core::Op::IF_ICMPLE) ++icmps;
        if (in->op == jbc::core::Op::IFEQ) { ++ifeqs; ifeqInsn = in; }
        if (in->op == jbc::core::Op::GOTO || in->op == jbc::core::Op::GOTO_W) ++gotos;
    }
    EXPECT_EQ(icmps, 0u) << "if_icmple 未折叠";
    EXPECT_EQ(ifeqs, 1u) << "TAKEN 未落恒真 IFEQ";
    EXPECT_EQ(gotos, 0u) << "禁止插无条件 goto";
    ASSERT_NE(ifeqInsn, nullptr);
    // IFEQ 目标 = 原分支目标 Ldead（死块头，bipush 之前的 iconst_0）。
    auto tgt = cu->dom.resolveLabel(ifeqInsn->target);
    ASSERT_TRUE(tgt.is_ok());
    const jbc::ir::Insn* tgtInsn = cu->dom.peek(tgt.unwrap().insn);
    ASSERT_NE(tgtInsn, nullptr);
    EXPECT_EQ(tgtInsn->op, jbc::core::Op::ICONST_0) << "IFEQ 未指向原分支目标";
}

/// 自身 TAKEN 形态的不动点：iconst_0; ifeq Ldead 恰是折叠产物形状，扫描
/// 必须排除（否则每轮原样重写、站点永不归零）。
const char* kPredSelfSource =
    ".super java/lang/Object\n"
    ".class public super fixture/PredSelf {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_0\n"
    "      ifeq Ldead\n"
    "      iconst_5\n"
    "      ireturn\n"
    "    Ldead:\n"
    "      iconst_0\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

TEST(PredicateFoldScan, SkipsOwnTakenFixedPoint)
{
    model::ResourceContainer container;
    const std::string        uri = "fixture/PredSelf.class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, asJava17(assemble("fixture/PredSelf", kPredSelfSource))));

    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u)
        << "[ICONST_0, IFEQ] 不动点被当成站点：收敛判据会失真";
}

}   // namespace
}   // namespace grunt::predicate_fold