/// @file        string_equals_restore_test.cpp
/// @brief       残留 `string_equals_restore` 的判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - equals 窗口（6 条）：hashCode 比较链换回 String.equals，操作数两 aload
///   保留，窗口其余指令全擦；
/// - equalsIgnoreCase 窗口（8 条）：两端各前置 toUpperCase，换回
///   String.equalsIgnoreCase；
/// - 拒绝面：首条成员错位（String.hashCode 占 Object.hashCode 位）、窗口
///   截断（5 条后 return）两形 scan 零站点且类字节逐位不动；
/// - NOP 夹层：窗口中间两条 nop 不影响还原。
#include "grunt/string_equals_restore.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace grunt::string_equals_restore {
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

/// 单类单方法夹具：装进容器，返回 uri。
std::string addFixture(model::ResourceContainer& container, const std::string& name,
                       const std::string& source)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, assemble(name, source)));
    return uri;
}

const model::ClassResource* classOf(const model::ResourceContainer& container,
                                    const std::string& uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls;
}

/// DOM 里剩余的指令 op 列表（NOP 不计）。
std::vector<jbc::core::Op> realOps(const std::vector<ca::u8>& bytes)
{
    std::vector<jbc::core::Op> ops;
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    if (promoted.is_err()) throw std::runtime_error("产物无法 promote");
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr || in->op == jbc::core::Op::NOP) continue;
        ops.push_back(in->op);
    }
    return ops;
}

/// 第 nth 条（0 起，NOP 不计）指令的成员引用三联（owner/name/desc）。
struct MemberRef
{
    std::string owner;
    std::string name;
    std::string desc;
};

MemberRef memberOfNth(const std::vector<ca::u8>& bytes, ca::usize nth)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    if (promoted.is_err()) throw std::runtime_error("产物无法 promote");
    jbc::ir::CodeUnit* cu    = promoted.unwrap().get();
    ca::usize          seen  = 0;
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr || in->op == jbc::core::Op::NOP) continue;
        if (seen == nth) {
            jbc::ir::ClassSession::MemberSem sem;
            if (!session->memberOf(*in, sem)) throw std::runtime_error("产物成员无法解析");
            return MemberRef{std::string(sem.owner), std::string(sem.name),
                             std::string(sem.desc)};
        }
        ++seen;
    }
    throw std::runtime_error("产物指令数不足");
}

/// 拒绝面反证：scan 零站点 + rewrite 零改写后，类字节与夹具原字节逐位全等。
void expectRejectionLeavesBytesUntouched(model::ResourceContainer& container,
                                         const std::string& uri)
{
    const std::vector<ca::u8> original = classOf(container, uri)->get();

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 0u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 0u);

    const std::vector<ca::u8>& bytes = classOf(container, uri)->get();
    ASSERT_EQ(bytes.size(), original.size());
    EXPECT_EQ(std::memcmp(bytes.data(), original.data(), original.size()), 0)
        << "拒绝面类字节必须逐位不动";
}

/// equals 窗口正形：两 aload 压参后接 6 条 hashCode 比较链。
/// 栈形：[a0,a1] → hash(a1)装箱 → swap → hash(a0)装箱 → Integer.equals。
const char* kEqualsSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SerEqHost {\n"
    "  .method public static eq (Ljava/lang/String;Ljava/lang/String;)Z {\n"
    "    code: {\n"
    "      aload_0\n"
    "      aload_1\n"
    "      invokevirtual java/lang/Object.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      swap\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      invokevirtual java/lang/Integer.equals (Ljava/lang/Object;)Z\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// equalsIgnoreCase 窗口正形：窗口 8 条（两端各前置一次 toUpperCase），
/// 第二个操作数在窗口前压栈。
const char* kIgnoreSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SerIgHost {\n"
    "  .method public static eq (Ljava/lang/String;Ljava/lang/String;)Z {\n"
    "    code: {\n"
    "      aload_1\n"
    "      aload_0\n"
    "      invokevirtual java/lang/String.toUpperCase ()Ljava/lang/String;\n"
    "      invokevirtual java/lang/Object.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      swap\n"
    "      invokevirtual java/lang/String.toUpperCase ()Ljava/lang/String;\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      invokevirtual java/lang/Integer.equals (Ljava/lang/Object;)Z\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 成员错位：原 Object.hashCode 位换成 String.hashCode，锚点成员钉死
/// 必须拒绝（两个窗口的锚点成员都不再吻合）。
const char* kMisplacedSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SerMisHost {\n"
    "  .method public static eq (Ljava/lang/String;Ljava/lang/String;)Z {\n"
    "    code: {\n"
    "      aload_0\n"
    "      aload_1\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      swap\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      invokevirtual java/lang/Integer.equals (Ljava/lang/Object;)Z\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// NOP 夹层：窗口中间（valueOf 与 swap 之间）插两条 nop，仍须还原。
const char* kNopSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SerNopHost {\n"
    "  .method public static eq (Ljava/lang/String;Ljava/lang/String;)Z {\n"
    "    code: {\n"
    "      aload_0\n"
    "      aload_1\n"
    "      invokevirtual java/lang/Object.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      nop\n"
    "      nop\n"
    "      swap\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      invokevirtual java/lang/Integer.equals (Ljava/lang/Object;)Z\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 截断形：窗口前 5 条后直接 ireturn（缺第二条 valueOf 与 Integer.equals），
/// 窗口不完整必须拒绝。栈形错位只为占位，jasm 不做返回类型核对。
const char* kTruncatedSource =
    ".super java/lang/Object\n"
    ".class public super fixture/SerTruncHost {\n"
    "  .method public static eq (Ljava/lang/String;Ljava/lang/String;)Z {\n"
    "    code: {\n"
    "      aload_0\n"
    "      aload_1\n"
    "      invokevirtual java/lang/Object.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      swap\n"
    "      invokevirtual java/lang/String.hashCode ()I\n"
    "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

}   // namespace

// ==== REWRITE ================================================================

TEST(StringEqualsRewrite, RestoresEqualsWindow)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/SerEqHost", kEqualsSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].variant, Variant::Equals);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ALOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::ALOAD_1);
    EXPECT_EQ(ops[2], jbc::core::Op::INVOKEVIRTUAL);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);

    const MemberRef target = memberOfNth(classOf(container, uri)->get(), 2);
    EXPECT_EQ(target.owner, "java/lang/String");
    EXPECT_EQ(target.name, "equals");
    EXPECT_EQ(target.desc, "(Ljava/lang/Object;)Z");
}

TEST(StringEqualsRewrite, RestoresEqualsIgnoreCaseWindow)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/SerIgHost", kIgnoreSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].variant, Variant::EqualsIgnore);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ALOAD_1);
    EXPECT_EQ(ops[1], jbc::core::Op::ALOAD_0);
    EXPECT_EQ(ops[2], jbc::core::Op::INVOKEVIRTUAL);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);

    const MemberRef target = memberOfNth(classOf(container, uri)->get(), 2);
    EXPECT_EQ(target.owner, "java/lang/String");
    EXPECT_EQ(target.name, "equalsIgnoreCase");
    EXPECT_EQ(target.desc, "(Ljava/lang/String;)Z");
}

TEST(StringEqualsRewrite, ToleratesNopInterleave)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/SerNopHost", kNopSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].variant, Variant::Equals);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[2], jbc::core::Op::INVOKEVIRTUAL);

    const MemberRef target = memberOfNth(classOf(container, uri)->get(), 2);
    EXPECT_EQ(target.owner, "java/lang/String");
    EXPECT_EQ(target.name, "equals");
    EXPECT_EQ(target.desc, "(Ljava/lang/Object;)Z");
}

// ==== 拒绝面反证（memcmp 全等）================================================

TEST(StringEqualsRejectSurface, MisplacedMemberLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/SerMisHost", kMisplacedSource);
    expectRejectionLeavesBytesUntouched(container, "fixture/SerMisHost.class");
}

TEST(StringEqualsRejectSurface, TruncatedWindowLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/SerTruncHost", kTruncatedSource);
    expectRejectionLeavesBytesUntouched(container, "fixture/SerTruncHost.class");
}

// ==== 收敛自证 ================================================================

TEST(StringEqualsRewrite, RescanAfterRewriteFindsNothing)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/SerEqHost", kEqualsSource);
    addFixture(container, "fixture/SerIgHost", kIgnoreSource);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 2u);

    const ScanResult rescanned = scan(container);
    EXPECT_EQ(rescanned.sites.size(), 0u) << "还原产物不得再成站点（收敛判据）";
}

}   // namespace grunt::string_equals_restore
