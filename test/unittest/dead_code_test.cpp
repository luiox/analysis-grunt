/// @file        dead_code_test.cpp
/// @brief       残留 `dead_code` 的三段判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - SCAN：return 后死尾 / goto 越过的死块产出站点（victimCount 计数），
///   全活方法零站点；可达性含异常边，handler 无显式跳转入口也存活；
/// - REWRITE：死尾擦除的字节码断言；保护区中段死块擦除且 handler 存活
///   （端点全在存活指令上的正形，回归锚）；端点锚在死指令 / 全死区间
///   两类形状按文档行为断言（吸附路径已修复，2026-11-12 修复批）；
/// - PROOF：类数不增 + 脏类可重解析。
#include "grunt/dead_code.hpp"

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

namespace grunt::dead_code {
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

/// 死尾夹具：ireturn 之后的诱饵段无控制流入口。
const char* kDeadTailSource =
    ".super java/lang/Object\n"
    ".class public super fixture/DeadTail {\n"
    "  .method public static messy ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      ireturn\n"
    "      iconst_5\n"
    "      iconst_0\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 异常边正形夹具：保护区 [Ltry, Lmid) 端点全在存活指令上，死块在区间中段
/// （goto 越过），handler 无显式跳转入口（只能靠异常边进）。jasm 的区间末
/// 标签是排他语义，且未定义标签会落到方法末哨兵，第二标签必须指向已定义
/// 的 Lmid。
const char* kMidRegionExcSource =
    ".super java/lang/Object\n"
    ".class public super fixture/MidExc {\n"
    "  .method public static m (I)I {\n"
    "    exceptions: { { Ltry, Lmid, Lh, Ljava/lang/RuntimeException; } },\n"
    "    code: {\n"
    "    Ltry:\n"
    "      iload 0\n"
    "      goto Lmid\n"
    "    Ldead:\n"
    "      iconst_5\n"
    "    Lmid:\n"
    "      iconst_0\n"
    "      idiv\n"
    "      ireturn\n"
    "    Lh:\n"
    "      pop\n"
    "      bipush 7\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 异常边端点锚死指令夹具：区间 [Ltry, Lend) 内 idiv 可抛（handler 靠异常边
/// 存活），但 Lend 锚在死指令 iconst_5 上。
const char* kEndAnchorDeadSource =
    ".super java/lang/Object\n"
    ".class public super fixture/ExcHost {\n"
    "  .method public static m (I)I {\n"
    "    exceptions: { { Ltry, Lend, Lh, Ljava/lang/RuntimeException; } },\n"
    "    code: {\n"
    "    Ltry:\n"
    "      iload 0\n"
    "      iconst_0\n"
    "      idiv\n"
    "      ireturn\n"
    "    Lend:\n"
    "      iconst_5\n"
    "      bipush 3\n"
    "      iadd\n"
    "      ireturn\n"
    "    Lh:\n"
    "      pop\n"
    "      bipush 7\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 全死区间夹具：保护区 [Ldead, LdeadEnd) 整段不可达，handler 唯一根是该
/// 区间。
const char* kFullyDeadRegionSource =
    ".super java/lang/Object\n"
    ".class public super fixture/DeadExc {\n"
    "  .method public static m ()I {\n"
    "    exceptions: { { Ldead, LdeadEnd, Lh, Ljava/lang/RuntimeException; } },\n"
    "    code: {\n"
    "      bipush 9\n"
    "      ireturn\n"
    "    Ldead:\n"
    "      iconst_1\n"
    "      iconst_2\n"
    "      iadd\n"
    "    LdeadEnd:\n"
    "      ireturn\n"
    "    Lh:\n"
    "      pop\n"
    "      bipush 5\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// start 锚死指令夹具：区间 [Ltry(iconst_4), Lend(iconst_5)) 内 iload..goto
/// 存活，start/end 两枚锚都落在死指令上。
const char* kStartAnchorDeadSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SnapHost {\n"
    "  .method public static m (I)I {\n"
    "    exceptions: { { Ltry, Lend, Lh, Ljava/lang/RuntimeException; } },\n"
    "    code: {\n"
    "      iconst_1\n"
    "      goto Lmid\n"
    "    Ltry:\n"
    "      iconst_4\n"
    "    Lmid:\n"
    "      iload 0\n"
    "      iconst_0\n"
    "      idiv\n"
    "      goto Lret\n"
    "    Lend:\n"
    "      iconst_5\n"
    "    Lret:\n"
    "      bipush 3\n"
    "      ireturn\n"
    "    Lh:\n"
    "      pop\n"
    "      bipush 7\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// promote 指定方法，返回按布局序的指令 op 列表 + 方法内异常表端点解析。
/// end 锚允许是方法末哨兵（LabelTarget::End，合法形态），此时 endOp 不置。
struct MethodDump
{
    std::vector<jbc::core::Op> ops;
    struct ExcDump
    {
        jbc::core::Op startOp;
        jbc::core::Op endOp;
        jbc::core::Op handlerOp;
        bool          endIsEnd = false;   ///< end 锚在方法末哨兵上
    };
    std::vector<ExcDump> excs;
};

MethodDump dumpMethod(const std::vector<ca::u8>& bytes, ca::usize methodIndex)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, methodIndex);
    if (promoted.is_err() || promoted.unwrap().get() == nullptr) {
        throw std::runtime_error("产物方法无法 promote");
    }
    const jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    MethodDump               dump;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr) continue;
        dump.ops.push_back(in->op);
    }
    const auto opAt = [&](const jbc::ir::LabelTarget& t) {
        if (t.isEnd()) return jbc::core::Op::NOP;
        const jbc::ir::Insn* in = cu->dom.peek(t.insn);
        if (in == nullptr) throw std::runtime_error("产物异常表端点悬空");
        return in->op;
    };
    for (const jbc::ir::CodeUnit::Exc& e : cu->exc) {
        auto startT   = cu->dom.resolveLabel(e.start);
        auto endT     = cu->dom.resolveLabel(e.end);
        auto handlerT = cu->dom.resolveLabel(e.handler);
        if (startT.is_err() || endT.is_err() || handlerT.is_err()) {
            throw std::runtime_error("产物异常表端点无法解析");
        }
        dump.excs.push_back({opAt(startT.unwrap()), opAt(endT.unwrap()),
                             opAt(handlerT.unwrap()), endT.unwrap().isEnd()});
    }
    return dump;
}

ca::usize countOp(const MethodDump& dump, jbc::core::Op op)
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

TEST(DeadCodeScan, FindsDeadTailsAndSkipsLiveMethods)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/DeadTail", kDeadTailSource},
        {"fixture/DeadBlock",
         ".super java/lang/Object\n"
         ".class public super fixture/DeadBlock {\n"
         "  .method public static m ()I {\n"
         "    code: {\n"
         "      goto Llive\n"
         "    Ldead:\n"
         "      iconst_5\n"
         "    Llive:\n"
         "      bipush 3\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Clean",
         ".super java/lang/Object\n"
         ".class public super fixture/Clean {\n"
         "  .method public static m ()I {\n"
         "    code: {\n"
         "      bipush 3\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 2u) << "Clean 类零站点";

    const Site* tail  = nullptr;
    const Site* block = nullptr;
    for (const Site& s : result.sites) {
        if (s.uri == "fixture/DeadTail.class") tail = &s;
        if (s.uri == "fixture/DeadBlock.class") block = &s;
    }
    ASSERT_NE(tail, nullptr);
    EXPECT_EQ(tail->methodIndex, 0u);
    EXPECT_EQ(tail->victimCount, 3u) << "ireturn 后的 iconst_5/iconst_0/ireturn";
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->victimCount, 1u) << "goto 越过的 iconst_5";
}

TEST(DeadCodeScan, HandlerAliveViaExceptionEdge)
{
    // Lh 无任何显式跳转入口：区间 [Ltry, Lend) 与可达集相交（idiv 可抛），
    // handler 以异常边为根纳入 BFS → 存活；死块只有 Lend 段四条。
    model::ResourceContainer container = makeContainer({
        {"fixture/ExcHost", kEndAnchorDeadSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().victimCount, 4u);
}

TEST(DeadCodeScan, FullyDeadRegionLeavesHandlerUnrooted)
{
    // 区间 [Ldead, LdeadEnd) 整段不可达：handler 唯一根是本区间 → 无根传导，
    // handler 三条也进受害者（这段代码抛不出异常，handler 也进不来）。
    model::ResourceContainer container = makeContainer({
        {"fixture/DeadExc", kFullyDeadRegionSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().victimCount, 7u)
        << "死区间三条 + 区间外 ireturn + handler 三条";
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(DeadCodeRewrite, ErasesUnreachableTail)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/DeadTail", kDeadTailSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.insns, 3u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), "fixture/DeadTail.class");

    const MethodDump after = dumpMethod(classBytes(container, "fixture/DeadTail.class"), 0);
    ASSERT_EQ(after.ops.size(), 2u);
    EXPECT_EQ(after.ops[0], jbc::core::Op::ICONST_1);
    EXPECT_EQ(after.ops[1], jbc::core::Op::IRETURN);
    EXPECT_EQ(countOp(after, jbc::core::Op::ICONST_5), 0u) << "死尾必须物理消失";
}

TEST(DeadCodeRewrite, HandlerSurvivesExcEdgeInMidRegionDeadBlock)
{
    // 端点全在存活指令上的正形：区间中段死块（goto 越过）擦除，handler 靠
    // 异常边存活，异常表原样保留（端点无需吸附）。
    model::ResourceContainer container = makeContainer({
        {"fixture/MidExc", kMidRegionExcSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().victimCount, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.insns, 1u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const MethodDump after = dumpMethod(classBytes(container, "fixture/MidExc.class"), 0);
    ASSERT_EQ(after.ops.size(), 8u)
        << "iload/goto/iconst_0/idiv/ireturn + pop/bipush 7/ireturn";
    EXPECT_EQ(countOp(after, jbc::core::Op::ICONST_5), 0u) << "死块物理消失";
    EXPECT_EQ(countOp(after, jbc::core::Op::POP), 1u) << "handler 存活";

    ASSERT_EQ(after.excs.size(), 1u);
    EXPECT_EQ(after.excs[0].startOp, jbc::core::Op::ILOAD_0) << "lower 归一 iload 0 → iload_0";
    EXPECT_FALSE(after.excs[0].endIsEnd);
    EXPECT_EQ(after.excs[0].endOp, jbc::core::Op::ICONST_0);
    EXPECT_EQ(after.excs[0].handlerOp, jbc::core::Op::POP);
}

/// 端点吸附/删表路径测试（2026-11-12 修复批翻转为文档行为断言）：修复前
/// `rewriteMethod` 用 `createLabel` 造新 Boundary 标签替换 `cu->exc` 端点
/// 但从不销毁旧标签，旧标签仍锚在待擦指令上，eraseBatch 按批内成员被
/// 存活 Boundary 标签锚定 → Err 且整批不删契约拒绝，整方法 fail-closed
/// 空转。修法 = 端点吸附改 `bindLabel` 搬移旧标签（句柄身份不变，表项引
/// 用不动）+ 删表项的孤儿端点/handler 标签销毁（实现契约见
/// src/grunt/dead_code.cpp 文件头注释）。

TEST(DeadCodeRewrite, EndAnchorOnDeadInsnSnapsForward)
{
    // Lend 锚在死指令 iconst_5 上：表项保留，end 向前吸附到其后第一条存活
    // 指令（handler 头 pop）；死尾四条物理消失，handler 靠异常边存活。
    model::ResourceContainer container = makeContainer({
        {"fixture/ExcHost", kEndAnchorDeadSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().victimCount, 4u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.insns, 4u) << "iconst_5/bipush 3/iadd/ireturn";
    ASSERT_EQ(dirty.uris.size(), 1u);

    const MethodDump after = dumpMethod(classBytes(container, "fixture/ExcHost.class"), 0);
    ASSERT_EQ(after.ops.size(), 7u)
        << "iload_0/iconst_0/idiv/ireturn + pop/bipush 7/ireturn";
    EXPECT_EQ(countOp(after, jbc::core::Op::ICONST_5), 0u) << "死尾物理消失";

    ASSERT_EQ(after.excs.size(), 1u) << "表项保留（区间吞并被删段）";
    EXPECT_EQ(after.excs[0].startOp, jbc::core::Op::ILOAD_0);
    EXPECT_FALSE(after.excs[0].endIsEnd);
    EXPECT_EQ(after.excs[0].endOp, jbc::core::Op::POP) << "end 吸附到 handler 头";
    EXPECT_EQ(after.excs[0].handlerOp, jbc::core::Op::POP) << "handler 存活";
}

TEST(DeadCodeRewrite, FullyDeadRegionDropsEntryAndHandler)
{
    // 区间全死：表项删除，handler 唯一根是本区间 → handler 三条随区间一并
    // 擦除（scan 已判 7 条受害者），产物只剩入口 bipush 9/ireturn 两条。
    model::ResourceContainer container = makeContainer({
        {"fixture/DeadExc", kFullyDeadRegionSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().victimCount, 7u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.insns, 7u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const MethodDump after = dumpMethod(classBytes(container, "fixture/DeadExc.class"), 0);
    ASSERT_EQ(after.ops.size(), 2u) << "bipush 9/ireturn";
    EXPECT_EQ(countOp(after, jbc::core::Op::POP), 0u) << "handler 物理消失";
    EXPECT_TRUE(after.excs.empty()) << "全死区间表项删除";
}

TEST(DeadCodeRewrite, StartAnchorOnDeadInsnSnapsForward)
{
    // start 锚在死指令 iconst_4 上（区间内 iload..goto 存活）、end 锚在死指
    // 令 iconst_5 上：start 吸附到区间首条存活指令（iload 0）、end 吸附到
    // 其后第一条存活指令（bipush 3），两条锚死指令擦除，handler 存活。
    model::ResourceContainer container = makeContainer({
        {"fixture/SnapHost", kStartAnchorDeadSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    ASSERT_EQ(scanned.sites.front().victimCount, 2u) << "Ltry 锚与 Lend 锚两条死指令";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.insns, 2u);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const MethodDump after = dumpMethod(classBytes(container, "fixture/SnapHost.class"), 0);
    ASSERT_EQ(after.ops.size(), 11u)
        << "iconst_1/goto/iload_0/iconst_0/idiv/goto/ldc(3)/ireturn "
        << "+ pop/ldc(7)/ireturn";
    EXPECT_EQ(countOp(after, jbc::core::Op::ICONST_4), 0u) << "start 锚死指令物理消失";
    EXPECT_EQ(countOp(after, jbc::core::Op::ICONST_5), 0u) << "end 锚死指令物理消失";
    EXPECT_EQ(countOp(after, jbc::core::Op::POP), 1u) << "handler 存活";

    ASSERT_EQ(after.excs.size(), 1u) << "表项保留（两端点吸附）";
    EXPECT_EQ(after.excs[0].startOp, jbc::core::Op::ILOAD_0)
        << "start 吸附到区间首条存活指令";
    EXPECT_FALSE(after.excs[0].endIsEnd);
    // 夹具文本 `bipush 3` 经 libjhasm constantLoad 规范化落 ldc（bipush/
    // sipush/ldc 族文本并入同一语义常量不再保形），吸附靶指令 op 为 LDC。
    EXPECT_EQ(after.excs[0].endOp, jbc::core::Op::LDC)
        << "end 吸附到 Lret 头（bipush 3 的落码形态 ldc）";
    EXPECT_EQ(after.excs[0].handlerOp, jbc::core::Op::POP) << "handler 存活";
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(DeadCodeProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/DeadTail", kDeadTailSource},
        {"fixture/MidExc", kMidRegionExcSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 2u);

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

}   // namespace
}   // namespace grunt::dead_code
