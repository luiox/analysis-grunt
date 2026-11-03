/// @file        number_fold_test.cpp
/// @brief       残留 `number_fold` 的三段判据测试（jasm 夹具现场汇编）。
#include "grunt/number_fold.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/const_args.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace grunt::number_fold {
namespace {

namespace model = mjbase::model;
using const_args::ValueLoad;

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

/// 单类单方法夹具：装进容器，返回 uri。
std::string addFixture(model::ResourceContainer& container, const std::string& name,
                       const std::string& source)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, asJava17(assemble(name, source))));
    return uri;
}

/// 改写后重新解析并 promote，遍历 DOM 取第一条常量装载的值面。
ValueLoad firstConstLoad(const std::vector<ca::u8>& bytes)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    if (promoted.is_err()) throw std::runtime_error("产物无法 promote");
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    for (const jbc::ir::InsnId id : cu->dom.order()) {
        const jbc::ir::Insn* in = cu->dom.peek(id);
        if (in == nullptr || in->op == jbc::core::Op::NOP) continue;
        ValueLoad v;
        if (const_args::decodeValueLoad(*session, *in, v)) return v;
        return ValueLoad{};   ///< 首条不是常量装载
    }
    return ValueLoad{};
}

/// DOM 里剩余的指令 op 计数（NOP 不计）。
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

bool contains(const std::vector<jbc::core::Op>& ops, jbc::core::Op op)
{
    for (const jbc::core::Op o : ops) {
        if (o == op) return true;
    }
    return false;
}

// ---- 正例夹具 --------------------------------------------------------------

/// int 跨度 1：ldc 100; i2l; ldc 23; i2l; lxor; l2i → 100 ^ 23 = 115。
const char* kIntViaLong =
    ".super java/lang/Object\n"
    ".class public super fixture/NfIntViaLong {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc 100\n"
    "      i2l\n"
    "      ldc 23\n"
    "      i2l\n"
    "      lxor\n"
    "      l2i\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// int 跨度 2：ldc2_w 4294967296L; l2i; ldc 5; ixor → (int)4294967296 ^ 5 = 5。
const char* kLongToInt =
    ".super java/lang/Object\n"
    ".class public super fixture/NfLongToInt {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc2_w 4294967296L\n"
    "      l2i\n"
    "      ldc 5\n"
    "      ixor\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// long 跨度："1e" 基 32 = 46；46 ^ 100 = 74。
const char* kBase32Long =
    ".super java/lang/Object\n"
    ".class public super fixture/NfBase32Long {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc \"1e\"\n"
    "      ldc 32\n"
    "      invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J\n"
    "      ldc2_w 100L\n"
    "      lxor\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// float 位形包装：0x3FC00000 = 1.5f。
const char* kFloatWrapper =
    ".super java/lang/Object\n"
    ".class public super fixture/NfFloatWrapper {\n"
    "  .method public static m ()F {\n"
    "    code: {\n"
    "      ldc 1069547520\n"
    "      i2l\n"
    "      iconst_0\n"
    "      i2l\n"
    "      lxor\n"
    "      l2i\n"
    "      invokestatic java/lang/Float.intBitsToFloat (I)F\n"
    "      freturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// double 位形包装："1e" ^ 0x400800000000002E = 0x4008000000000000 = 3.0。
const char* kDoubleWrapper =
    ".super java/lang/Object\n"
    ".class public super fixture/NfDoubleWrapper {\n"
    "  .method public static m ()D {\n"
    "    code: {\n"
    "      ldc \"1e\"\n"
    "      ldc 32\n"
    "      invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J\n"
    "      ldc2_w 4613937818241073198L\n"
    "      lxor\n"
    "      invokestatic java/lang/Double.longBitsToDouble (J)D\n"
    "      dreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 需落池的值：40000 超 bipush/sipush 域，折叠产物必须走 LDC。
const char* kPoolValue =
    ".super java/lang/Object\n"
    ".class public super fixture/NfPoolValue {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc 40000\n"
    "      i2l\n"
    "      iconst_0\n"
    "      i2l\n"
    "      lxor\n"
    "      l2i\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

// ---- 反例夹具 --------------------------------------------------------------

/// 实参非常量：iload_0 打断纯常量装载。
const char* kNonConstOperand =
    ".super java/lang/Object\n"
    ".class public super fixture/NfNonConst {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      i2l\n"
    "      ldc 23\n"
    "      i2l\n"
    "      lxor\n"
    "      l2i\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 形状断裂：跨度尾不是 IXOR（IADD 不构成跨度）。
const char* kShapeBreak =
    ".super java/lang/Object\n"
    ".class public super fixture/NfShapeBreak {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc2_w 100L\n"
    "      l2i\n"
    "      ldc 5\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// radix 恒 32：radix 16 不是家族生成面。
const char* kRadixNot32 =
    ".super java/lang/Object\n"
    ".class public super fixture/NfRadix16 {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc \"ff\"\n"
    "      ldc 16\n"
    "      invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J\n"
    "      ldc2_w 1L\n"
    "      lxor\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 键面非法：'z' 越出基 32 数字域。
const char* kBadBase32 =
    ".super java/lang/Object\n"
    ".class public super fixture/NfBadBase32 {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc \"zz\"\n"
    "      ldc 32\n"
    "      invokestatic java/lang/Long.parseUnsignedLong (Ljava/lang/String;I)J\n"
    "      ldc2_w 1L\n"
    "      lxor\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 被调不符：parseLong 不是 parseUnsignedLong（引用三元组精确判）。
const char* kWrongCallee =
    ".super java/lang/Object\n"
    ".class public super fixture/NfWrongCallee {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc \"ff\"\n"
    "      ldc 32\n"
    "      invokestatic java/lang/Long.parseLong (Ljava/lang/String;I)J\n"
    "      ldc2_w 1L\n"
    "      lxor\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 中段标签：分支路径与贯穿路径在跨度中段（ldc 23）汇合，栈形一致 [J]——
/// 汇编合法，但 ldc 23 带标签 = 合并点，scan 必须整站放弃。
const char* kMidSpanLabel =
    ".super java/lang/Object\n"
    ".class public super fixture/NfMidLabel {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      ldc 100\n"
    "      i2l\n"
    "      iload_0\n"
    "      ifeq Lmid\n"
    "      pop2\n"
    "      ldc 100\n"
    "      i2l\n"
    "    Lmid:\n"
    "      ldc 23\n"
    "      i2l\n"
    "      lxor\n"
    "      l2i\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

}   // namespace

// ==== SCAN ==================================================================

TEST(NumberFoldScan, FindsIntViaLongSpan)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(scanned.sites[0].value.i32(), 115);
}

TEST(NumberFoldScan, FindsLongToIntSpan)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfLongToInt", kLongToInt);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.i32(), 5);
}

TEST(NumberFoldScan, FindsBase32LongSpan)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfBase32Long", kBase32Long);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I64);
    EXPECT_EQ(scanned.sites[0].value.i64(), 74);
}

TEST(NumberFoldScan, FindsFloatBitsWrapper)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfFloatWrapper", kFloatWrapper);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::F32);
    EXPECT_EQ(scanned.sites[0].value.f32(), 1.5f);
}

TEST(NumberFoldScan, FindsDoubleBitsWrapper)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfDoubleWrapper", kDoubleWrapper);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::F64);
    EXPECT_EQ(jsim::doubleToBits(scanned.sites[0].value.f64()), 0x4008000000000000ULL);
}

TEST(NumberFoldScan, RejectsNonConstOperand)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfNonConst", kNonConstOperand);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsBrokenSpanShape)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfShapeBreak", kShapeBreak);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsNon32Radix)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfRadix16", kRadixNot32);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsIllegalBase32Key)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfBadBase32", kBadBase32);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsWrongCallee)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfWrongCallee", kWrongCallee);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsMidSpanMergeLabel)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfMidLabel", kMidSpanLabel);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u)
        << "跨度中段标签 = 合并点，必须整站放弃";
}

// ==== REWRITE ===============================================================

TEST(NumberFoldRewrite, FoldsIntSpanAndPersists)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfIntViaLong", kIntViaLong);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    // 跨度消失：I2L/LXOR/L2I 全清，只剩常量装载 + 返回。
    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_FALSE(contains(ops, jbc::core::Op::I2L)) << "跨度未擦除";
    EXPECT_FALSE(contains(ops, jbc::core::Op::LXOR)) << "跨度未擦除";
    EXPECT_FALSE(contains(ops, jbc::core::Op::L2I)) << "跨度未擦除";
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);

    // 值面：首条常量装载 == 115。
    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 115);
}

TEST(NumberFoldRewrite, FoldsBase32LongSpan)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfBase32Long", kBase32Long);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_FALSE(contains(ops, jbc::core::Op::INVOKESTATIC)) << "parseUnsignedLong 未清";
    EXPECT_FALSE(contains(ops, jbc::core::Op::LXOR)) << "跨度未擦除";

    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I64);
    EXPECT_EQ(v.i64(), 74);
}

TEST(NumberFoldRewrite, FoldsFloatBitsWrapper)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfFloatWrapper", kFloatWrapper);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_FALSE(contains(ops, jbc::core::Op::INVOKESTATIC)) << "intBitsToFloat 未清";
    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::F32);
    EXPECT_EQ(v.f32(), 1.5f);
}

TEST(NumberFoldRewrite, FoldsDoubleBitsWrapper)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfDoubleWrapper", kDoubleWrapper);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::F64);
    EXPECT_EQ(jsim::doubleToBits(v.f64()), 0x4008000000000000ULL);
}

TEST(NumberFoldRewrite, FallsBackToPoolForWideValues)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfPoolValue", kPoolValue);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);
    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 40000);
}

TEST(NumberFoldRewrite, RescanAfterRewriteFindsNothing)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);
    addFixture(container, "fixture/NfBase32Long", kBase32Long);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 2u);

    const ScanResult rescanned = scan(container);
    EXPECT_EQ(rescanned.sites.size(), 0u) << "折叠产物不得再成站点（收敛判据）";
}

// ==== PROOF =================================================================

TEST(NumberFoldProof, ClassesIntactAfterRewrite)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);
    addFixture(container, "fixture/NfLongToInt", kLongToInt);
    addFixture(container, "fixture/NfFloatWrapper", kFloatWrapper);

    const ca::usize before = countClasses(container);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 3u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 3u);

    const ProofResult proofed = proof(container, dirty, before);
    EXPECT_TRUE(proofed.ok) << "proof 失败：类数不增或脏类无法重新解析";
    EXPECT_EQ(countClasses(container), before);
}

TEST(NumberFoldProof, RejectsClassCountGrowth)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);

    const ca::usize before = countClasses(container);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    // 人为增类，类数门必须报失败。
    const std::string extra = "fixture/NfExtra.class";
    container.addResource(extra, std::make_unique<model::ClassResource>(
                                     extra, asJava17(assemble("fixture/NfExtra", kIntViaLong))));
    const ProofResult proofed = proof(container, dirty, before);
    EXPECT_FALSE(proofed.ok);
    ASSERT_EQ(proofed.failed.size(), 1u);
    EXPECT_EQ(proofed.failed[0], "class-count");
}

}   // namespace grunt::number_fold
