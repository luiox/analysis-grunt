/// @file        const_value_promote_test.cpp
/// @brief       残留 `const_value_promote` 的三段判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - SCAN：int/long/String 三通道单写正例（kind/slots/desc 互证）；同字段
///   多次写、clinit 外写、宿主类外写拒绝；ACONST_NULL 值拒绝（ConstantValue
///   无 null 形态）；已带 ConstantValue 的字段跳过；类外读不拒（契约：
///   读的次数不限，回填后读者仍见同值）；
/// - REWRITE：值指令置 NOP + putstatic 擦除的字节码断言；ACC_FINAL 与
///   ConstantValue 属性载荷（CP 索引 → Integer/Long/String 条目）；
/// - PROOF：类数不增 + 脏类可重解析。
#include "grunt/const_value_promote.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
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
#include <utility>
#include <vector>

namespace grunt::const_value_promote {
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

/// 正形夹具：int/long/String 三字段各恰一次 clinit 常量写。
const char* kTripleSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Consts {\n"
    "  .field public static answer I\n"
    "  .field public static big J\n"
    "  .field public static tag Ljava/lang/String;\n"
    "\n"
    "  .method static <clinit> ()V {\n"
    "    code: {\n"
    "      bipush 42\n"
    "      putstatic fixture/Consts.answer I\n"
    "      ldc2_w 883318107L\n"
    "      putstatic fixture/Consts.big J\n"
    "      ldc \"grunt\"\n"
    "      putstatic fixture/Consts.tag Ljava/lang/String;\n"
    "      return\n"
    "    }\n"
    "  }\n"
    "}\n";

/// promote 夹具 <clinit>，返回按布局序的 op 列表。
std::vector<jbc::core::Op> clinitOps(const std::vector<ca::u8>& bytes)
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
    std::vector<jbc::core::Op> ops;
    for (const jbc::ir::InsnId id : promoted.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = promoted.unwrap()->dom.peek(id);
        if (in == nullptr) continue;
        ops.push_back(in->op);
    }
    return ops;
}

const jbc::core::MemberInfo& fieldOf(const jbc::core::ClassFile& cf, const std::string& name)
{
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if (cf.utf8(f.nameCp) == name) return f;
    }
    throw std::runtime_error("夹具缺字段：" + name);
}

/// ConstantValue 属性的 2 字节 CP 载荷（RawAttr 形态，直读源字节）；无则 0。
ca::u32 constantValueCp(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& fld)
{
    const jbc::core::ByteView src = cf.source();
    for (const jbc::core::AttrSlot& slot : fld.attrs) {
        if (cf.utf8(slot.nameCp) != "ConstantValue") continue;
        EXPECT_EQ(slot.data.length, 2u) << "ConstantValue 载荷须为 u2 CP 索引";
        return (static_cast<ca::u32>(src[slot.data.offset]) << 8)
             | static_cast<ca::u32>(src[slot.data.offset + 1]);
    }
    return 0;
}

/// 绕过 jasm 给字段注入 ConstantValue（jasm 无法声明字段初值），验证
/// 「已带 ConstantValue 跳过」门。
std::vector<ca::u8> withConstantValue(const std::vector<ca::u8>& bytes,
                                      const std::string& fieldName, ca::i32 v)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("夹具无法解析");
    jbc::core::ClassFile& cf = session->backing();
    for (jbc::core::MemberInfo& f : cf.fields) {
        if (cf.utf8(f.nameCp) != fieldName) continue;
        const ca::u32 cp = session->internInteger(v);
        if (cp == 0) throw std::runtime_error("intern 失败");
        std::vector<ca::u8> payload{static_cast<ca::u8>(cp >> 8),
                                    static_cast<ca::u8>(cp & 0xFF)};
        f.attrs.push_back(jbc::core::AttrSlot::makeNew(
            cf.cp.addUtf8("ConstantValue"),
            std::make_unique<jbc::core::RawAttr>(std::move(payload))));
        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) throw std::runtime_error("夹具回写失败");
        return std::move(encoded).unwrap();
    }
    throw std::runtime_error("夹具缺字段：" + fieldName);
}

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

TEST(ConstValuePromoteScan, PromotesIntLongStringSingleWrites)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Consts", kTripleSource},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 3u);
    EXPECT_FALSE(result.censusIncomplete);

    const Site* i32  = nullptr;
    const Site* i64  = nullptr;
    const Site* str  = nullptr;
    for (const Site& s : result.sites) {
        if (s.fieldName == "answer") i32 = &s;
        if (s.fieldName == "big") i64 = &s;
        if (s.fieldName == "tag") str = &s;
    }
    ASSERT_NE(i32, nullptr);
    EXPECT_EQ(i32->fieldDesc, "I");
    ASSERT_EQ(i32->value.kind, const_args::ValueLoad::Kind::I32);
    EXPECT_EQ(i32->value.i32(), 42);
    ASSERT_NE(i64, nullptr);
    EXPECT_EQ(i64->fieldDesc, "J");
    ASSERT_EQ(i64->value.kind, const_args::ValueLoad::Kind::I64);
    EXPECT_EQ(i64->value.i64(), 883318107);
    ASSERT_NE(str, nullptr);
    EXPECT_EQ(str->fieldDesc, "Ljava/lang/String;");
    ASSERT_EQ(str->value.kind, const_args::ValueLoad::Kind::Str);
    EXPECT_EQ(str->value.str, "grunt");
}

TEST(ConstValuePromoteScan, RejectsMultipleWrites)
{
    // 同字段在 clinit 里写两次：唯一写门失败。
    model::ResourceContainer container = makeContainer({
        {"fixture/Twice",
         ".super java/lang/Object\n"
         ".class public super fixture/Twice {\n"
         "  .field public static f I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      putstatic fixture/Twice.f I\n"
         "      bipush 43\n"
         "      putstatic fixture/Twice.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

TEST(ConstValuePromoteScan, RejectsWriteOutsideClinit)
{
    // 唯一写必须落在本类 clinit：同方法体的第二处写也越门。
    model::ResourceContainer container = makeContainer({
        {"fixture/Late",
         ".super java/lang/Object\n"
         ".class public super fixture/Late {\n"
         "  .field public static f I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      putstatic fixture/Late.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "\n"
         "  .method public static set ()V {\n"
         "    code: {\n"
         "      bipush 44\n"
         "      putstatic fixture/Late.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

TEST(ConstValuePromoteScan, RejectsCrossClassWrite)
{
    // 宿主类外另有写者：全容器唯一写门失败。
    model::ResourceContainer container = makeContainer({
        {"fixture/Host",
         ".super java/lang/Object\n"
         ".class public super fixture/Host {\n"
         "  .field public static f I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      putstatic fixture/Host.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Writer",
         ".super java/lang/Object\n"
         ".class public super fixture/Writer {\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 43\n"
         "      putstatic fixture/Host.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

TEST(ConstValuePromoteScan, RejectsNullValueLoad)
{
    // ACONST_NULL 不是合法 ConstantValue 形态（栈形合法，值面拒绝）。
    model::ResourceContainer container = makeContainer({
        {"fixture/Nulls",
         ".super java/lang/Object\n"
         ".class public super fixture/Nulls {\n"
         "  .field public static tag Ljava/lang/String;\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      aconst_null\n"
         "      putstatic fixture/Nulls.tag Ljava/lang/String;\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    EXPECT_TRUE(result.sites.empty());
}

TEST(ConstValuePromoteScan, SkipsFieldAlreadyWithConstantValue)
{
    // 已带 ConstantValue 的字段跳过，其余字段照常产出站点。
    const char* preSource =
        ".super java/lang/Object\n"
        ".class public super fixture/Pre {\n"
        "  .field public static answer I\n"
        "  .field public static other I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 42\n"
        "      putstatic fixture/Pre.answer I\n"
        "      bipush 7\n"
        "      putstatic fixture/Pre.other I\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
    auto injected = withConstantValue(assemble("fixture/Pre", preSource), "answer", 42);
    model::ResourceContainer container;
    container.addResource("fixture/Pre.class",
                          std::make_unique<model::ClassResource>("fixture/Pre.class",
                                                                 std::move(injected)));

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().fieldName, "other") << "已带 ConstantValue 的字段必须跳过";
}

TEST(ConstValuePromoteScan, AllowsCrossClassReads)
{
    // 契约（hpp 判据 2）：读的次数不限——回填后读者仍见同值，类外读不拒。
    model::ResourceContainer container = makeContainer({
        {"fixture/Host",
         ".super java/lang/Object\n"
         ".class public super fixture/Host {\n"
         "  .field public static f I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      putstatic fixture/Host.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Reader",
         ".super java/lang/Object\n"
         ".class public super fixture/Reader {\n"
         "  .method public static get ()I {\n"
         "    code: {\n"
         "      getstatic fixture/Host.f I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const ScanResult result = scan(container);
    ASSERT_EQ(result.sites.size(), 1u);
    EXPECT_EQ(result.sites.front().fieldName, "f");
    EXPECT_EQ(result.sites.front().uri, "fixture/Host.class");
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

TEST(ConstValuePromoteRewrite, BackfillsConstantValueAndFinal)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Consts", kTripleSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 3u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.promoted, 3u);
    EXPECT_EQ(dirty.failed, 0u);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), "fixture/Consts.class");

    // 字节码断言：clinit 只余三枚值锚 NOP + return，putstatic 全部擦除。
    const std::vector<jbc::core::Op> ops = clinitOps(classBytes(container, "fixture/Consts.class"));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::NOP) << "bipush 值锚置 NOP";
    EXPECT_EQ(ops[1], jbc::core::Op::NOP) << "ldc2_w 值锚置 NOP";
    EXPECT_EQ(ops[2], jbc::core::Op::NOP) << "ldc 值锚置 NOP";
    EXPECT_EQ(ops[3], jbc::core::Op::RETURN);

    // 字段断言：ACC_FINAL + ConstantValue 载荷指向对的池条目。
    auto session = openSession(classBytes(container, "fixture/Consts.class"));
    ASSERT_TRUE(session != nullptr);
    const jbc::core::ClassFile& cf = session->backing();

    const jbc::core::MemberInfo& answer = fieldOf(cf, "answer");
    EXPECT_NE(answer.access & 0x0010, 0u) << "回填须补 ACC_FINAL";
    const ca::u32 answerCp = constantValueCp(cf, answer);
    ASSERT_NE(answerCp, 0u);
    const jbc::core::ConstantPool::Entry& answerEntry = cf.cp.at(answerCp);
    EXPECT_EQ(answerEntry.tag, jbc::core::CpTag::Integer);
    EXPECT_EQ(answerEntry.ival, 42);

    const jbc::core::MemberInfo& big = fieldOf(cf, "big");
    EXPECT_NE(big.access & 0x0010, 0u);
    const ca::u32 bigCp = constantValueCp(cf, big);
    ASSERT_NE(bigCp, 0u);
    const jbc::core::ConstantPool::Entry& bigEntry = cf.cp.at(bigCp);
    EXPECT_EQ(bigEntry.tag, jbc::core::CpTag::Long);
    EXPECT_EQ(bigEntry.lval, 883318107);

    const jbc::core::MemberInfo& tag = fieldOf(cf, "tag");
    EXPECT_NE(tag.access & 0x0010, 0u);
    const ca::u32 tagCp = constantValueCp(cf, tag);
    ASSERT_NE(tagCp, 0u);
    const jbc::core::ConstantPool::Entry& tagEntry = cf.cp.at(tagCp);
    EXPECT_EQ(tagEntry.tag, jbc::core::CpTag::String);
    EXPECT_EQ(cf.utf8(tagEntry.a), "grunt");
}

TEST(ConstValuePromoteRewrite, CrossClassReadUntouched)
{
    // 类外读者类不进脏集：回填只动宿主类。
    model::ResourceContainer container = makeContainer({
        {"fixture/Host",
         ".super java/lang/Object\n"
         ".class public super fixture/Host {\n"
         "  .field public static f I\n"
         "\n"
         "  .method static <clinit> ()V {\n"
         "    code: {\n"
         "      bipush 42\n"
         "      putstatic fixture/Host.f I\n"
         "      return\n"
         "    }\n"
         "  }\n"
         "}\n"},
        {"fixture/Reader",
         ".super java/lang/Object\n"
         ".class public super fixture/Reader {\n"
         "  .method public static get ()I {\n"
         "    code: {\n"
         "      getstatic fixture/Host.f I\n"
         "      ireturn\n"
         "    }\n"
         "  }\n"
         "}\n"},
    });

    const std::vector<ca::u8> readerBefore = classBytes(container, "fixture/Reader.class");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), "fixture/Host.class");
    EXPECT_EQ(dirty.promoted, 1u);

    EXPECT_EQ(classBytes(container, "fixture/Reader.class"), readerBefore) << "读者类字节不动";
    auto host = openSession(classBytes(container, "fixture/Host.class"));
    ASSERT_TRUE(host != nullptr);
    EXPECT_NE(constantValueCp(host->backing(), fieldOf(host->backing(), "f")), 0u)
        << "宿主字段须已带 ConstantValue";
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

TEST(ConstValuePromoteProof, KeepsClassCountAndReparsesDirtyClasses)
{
    model::ResourceContainer container = makeContainer({
        {"fixture/Consts", kTripleSource},
    });

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 3u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

}   // namespace
}   // namespace grunt::const_value_promote
