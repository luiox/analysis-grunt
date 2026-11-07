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

#include <cstring>
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

/// 拒绝面反证：scan 零站点 + rewrite 零改写后，类字节与夹具原字节逐位全等。
void expectRejectionLeavesBytesUntouched(model::ResourceContainer& container,
                                         const std::string& uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);
    const std::vector<ca::u8> original = cls->get();

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 0u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 0u);

    const model::IResource* after = container.getResource(uri);
    const auto* clsAfter = ca::core::tag_cast::dyn_cast<const model::ClassResource>(after);
    ASSERT_NE(clsAfter, nullptr);
    const std::vector<ca::u8>& bytes = clsAfter->get();
    ASSERT_EQ(bytes.size(), original.size());
    EXPECT_EQ(std::memcmp(bytes.data(), original.data(), original.size()), 0)
        << "拒绝面类字节必须逐位不动";
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

// ---- v2 位拼正例夹具 -------------------------------------------------------

/// int 位拼：305419896 & -252645136 = 271601776（0x12345678 & 0xF0F0F0F0 =
/// 0x10305070，生成侧 first & second = value 的构造形，两操作数都是 LDC）。
const char* kIntSplice =
    ".super java/lang/Object\n"
    ".class public super fixture/NfIntSplice {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc 305419896\n"
    "      ldc -252645136\n"
    "      iand\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// int 位拼短形态：bipush/iconst 域操作数 12 & 5 = 4。
const char* kIntSpliceShort =
    ".super java/lang/Object\n"
    ".class public super fixture/NfIntSpliceShort {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      bipush 12\n"
    "      iconst_5\n"
    "      iand\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// long 位拼：-2401053086426760323 & -8645941142645738499 = -8646574489311087747。
const char* kLongSplice =
    ".super java/lang/Object\n"
    ".class public super fixture/NfLongSplice {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc2_w -2401053086426760323L\n"
    "      ldc2_w -8645941142645738499L\n"
    "      land\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 位拼 + 位形包装：v2 encrypt(Float) 走 int 加密第二式。
/// 1069547520 & -1 = 1069547520 = 0x3FC00000 = 1.5f。
const char* kIntSpliceFloatWrapper =
    ".super java/lang/Object\n"
    ".class public super fixture/NfIntSpliceFloat {\n"
    "  .method public static m ()F {\n"
    "    code: {\n"
    "      ldc 1069547520\n"
    "      ldc -1\n"
    "      iand\n"
    "      invokestatic java/lang/Float.intBitsToFloat (I)F\n"
    "      freturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 位拼 + 位形包装（double）：0x4008000000000000 & -1 = 0x4008000000000000
/// = 3.0。
const char* kLongSpliceDoubleWrapper =
    ".super java/lang/Object\n"
    ".class public super fixture/NfLongSpliceDouble {\n"
    "  .method public static m ()D {\n"
    "    code: {\n"
    "      ldc2_w 4613937818241073152L\n"
    "      ldc2_w -1L\n"
    "      land\n"
    "      invokestatic java/lang/Double.longBitsToDouble (J)D\n"
    "      dreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

// ---- 位拼反例夹具 ----------------------------------------------------------

/// 位拼第二操作数非常量：iload_0 打断（真实掩码计算形，不是生成面跨度）。
const char* kSpliceNonConstOperand =
    ".super java/lang/Object\n"
    ".class public super fixture/NfSpliceNonConst {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      ldc 255\n"
    "      iload_0\n"
    "      iand\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 位拼栈形破裂：cat1/cat2 混喂（iconst_3 喂 LAND，第二操作数类型门拒绝）。
const char* kSpliceMixedWidth =
    ".super java/lang/Object\n"
    ".class public super fixture/NfSpliceMixed {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc2_w -2401053086426760323L\n"
    "      iconst_3\n"
    "      iand\n"
    "      i2l\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 位拼 op 断裂：尾条不是 IAND（ISUB 不构成位拼跨度）。
const char* kSpliceShapeBreak =
    ".super java/lang/Object\n"
    ".class public super fixture/NfSpliceShapeBreak {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc 100\n"
    "      ldc 5\n"
    "      isub\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 位拼中段标签：分支在第二操作数（ldc 5）汇合 = 合并点，整站放弃。
const char* kSpliceMidLabel =
    ".super java/lang/Object\n"
    ".class public super fixture/NfSpliceMidLabel {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      ldc 100\n"
    "      iload_0\n"
    "      ifeq Lmid\n"
    "      pop\n"
    "      ldc 100\n"
    "    Lmid:\n"
    "      ldc 5\n"
    "      iand\n"
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

// ==== SCAN（v2 位拼）========================================================

TEST(NumberFoldScan, FindsIntSplice)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntSplice", kIntSplice);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::LDC);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(scanned.sites[0].value.i32(), 271601776);
}

TEST(NumberFoldScan, FindsIntSpliceShortForms)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntSpliceShort", kIntSpliceShort);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.i32(), 4);
}

TEST(NumberFoldScan, FindsLongSplice)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfLongSplice", kLongSplice);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I64);
    EXPECT_EQ(scanned.sites[0].value.i64(), -8646574489311087747LL);
    EXPECT_EQ(scanned.sites[0].value.slots, 2u);
}

TEST(NumberFoldScan, FindsIntSpliceFloatWrapper)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntSpliceFloat", kIntSpliceFloatWrapper);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::F32);
    EXPECT_EQ(scanned.sites[0].value.f32(), 1.5f);
}

TEST(NumberFoldScan, FindsLongSpliceDoubleWrapper)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfLongSpliceDouble", kLongSpliceDoubleWrapper);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::F64);
    EXPECT_EQ(jsim::doubleToBits(scanned.sites[0].value.f64()), 0x4008000000000000ULL);
}

TEST(NumberFoldScan, RejectsSpliceNonConstOperand)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceNonConst", kSpliceNonConstOperand);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsSpliceMixedWidth)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceMixed", kSpliceMixedWidth);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u)
        << "cat1/cat2 混喂 = 栈形破裂，位拼第二操作数类型门必须拒绝";
}

TEST(NumberFoldScan, RejectsSpliceShapeBreak)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceShapeBreak", kSpliceShapeBreak);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u);
}

TEST(NumberFoldScan, RejectsSpliceMidSpanMergeLabel)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceMidLabel", kSpliceMidLabel);
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.sites.size(), 0u)
        << "位拼跨度中段标签 = 合并点，必须整站放弃";
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

// ==== REWRITE（v2 位拼）=====================================================

TEST(NumberFoldRewrite, FoldsIntSpliceAndPersists)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfIntSplice", kIntSplice);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    // 跨度消失：第二操作数与 IAND 全清，只剩常量装载 + 返回。
    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_FALSE(contains(ops, jbc::core::Op::IAND)) << "位拼跨度未擦除";
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);

    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 271601776);
}

TEST(NumberFoldRewrite, FoldsLongSpliceAndPersists)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfLongSplice", kLongSplice);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);

    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_FALSE(contains(ops, jbc::core::Op::LAND)) << "位拼跨度未擦除";
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], jbc::core::Op::LRETURN);

    const ValueLoad v = firstConstLoad(cls->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I64);
    EXPECT_EQ(v.i64(), -8646574489311087747LL);
}

TEST(NumberFoldRewrite, FoldsSpliceWrappersToBits)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntSpliceFloat", kIntSpliceFloatWrapper);
    addFixture(container, "fixture/NfLongSpliceDouble", kLongSpliceDoubleWrapper);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 2u);

    const model::IResource* raw = container.getResource("fixture/NfIntSpliceFloat.class");
    const auto* clsF = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(clsF, nullptr);
    const ValueLoad vf = firstConstLoad(clsF->get());
    EXPECT_EQ(vf.kind, ValueLoad::Kind::F32);
    EXPECT_EQ(vf.f32(), 1.5f);

    const model::IResource* rawD = container.getResource("fixture/NfLongSpliceDouble.class");
    const auto* clsD = ca::core::tag_cast::dyn_cast<const model::ClassResource>(rawD);
    ASSERT_NE(clsD, nullptr);
    const ValueLoad vd = firstConstLoad(clsD->get());
    EXPECT_EQ(vd.kind, ValueLoad::Kind::F64);
    EXPECT_EQ(jsim::doubleToBits(vd.f64()), 0x4008000000000000ULL);
}

/// 位拼与旧式共存：同一容器两类站点全折、重扫归零（新式不蚕食旧式站点）。
TEST(NumberFoldRewrite, FoldsMixedOldAndSpliceForms)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);
    addFixture(container, "fixture/NfIntSplice", kIntSplice);
    addFixture(container, "fixture/NfLongSplice", kLongSplice);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 3u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 3u);
    EXPECT_EQ(dirty.sites, 3u);

    const ScanResult rescanned = scan(container);
    EXPECT_EQ(rescanned.sites.size(), 0u) << "折叠产物不得再成站点（收敛判据）";
}

// ==== 拒绝面反证（memcmp 全等）==============================================

TEST(NumberFoldRejectSurface, SpliceRejectionsLeaveBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceNonConst", kSpliceNonConstOperand);
    addFixture(container, "fixture/NfSpliceShapeBreak", kSpliceShapeBreak);
    expectRejectionLeavesBytesUntouched(container, "fixture/NfSpliceNonConst.class");
}

TEST(NumberFoldRejectSurface, SpliceLabelRejectionLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfSpliceMidLabel", kSpliceMidLabel);
    expectRejectionLeavesBytesUntouched(container, "fixture/NfSpliceMidLabel.class");
}

TEST(NumberFoldRejectSurface, OldFormRejectionsStillLeaveBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfNonConst", kNonConstOperand);
    addFixture(container, "fixture/NfRadix16", kRadixNot32);
    expectRejectionLeavesBytesUntouched(container, "fixture/NfNonConst.class");
    expectRejectionLeavesBytesUntouched(container, "fixture/NfRadix16.class");
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

// ============================================================================
// ==== 迭代预算 ==============================================================
// ============================================================================

/// scan 预算超限：位置数超限的类保守跳过（计数进 ScanResult），默认预算不触限。
TEST(NumberFoldBudget, ScanOverBudgetSkipsClassConservatively)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/NfIntViaLong", kIntViaLong);

    // 极小预算（1 个位置）：首位置的跨度产出保留，其后位置不再扫，
    // 类记预算跳过（预算语义：超限前的产出保留，超限后保守放弃）。
    const ScanResult starved = scan(container, iter_budget::ClassBudget{1});
    EXPECT_EQ(starved.sites.size(), 1u);
    EXPECT_EQ(starved.classesSkippedByBudget, 1u);

    // 默认预算：正常夹具绝不触限（预算不改变正常罐行为）。
    const ScanResult normal = scan(container);
    ASSERT_EQ(normal.sites.size(), 1u);
    EXPECT_EQ(normal.classesSkippedByBudget, 0u);
}

/// rewrite 预算超限：站点复核指令数超限的站点保守放弃，产物保持原样。
TEST(NumberFoldBudget, RewriteOverBudgetLeavesClassUntouched)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/NfIntViaLong", kIntViaLong);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    // 复核记账单位是方法指令数（本夹具 7 条）；预算 1 必在首站点即超限。
    const DirtySet dirty = rewrite(container, scanned, iter_budget::ClassBudget{1});
    EXPECT_EQ(dirty.uris.size(), 0u) << "超限类不许被改写";
    EXPECT_EQ(dirty.skippedByBudget, 1u);

    // 产物面：类保持原样（跨度还在）。
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    ASSERT_NE(cls, nullptr);
    const std::vector<jbc::core::Op> ops = realOps(cls->get());
    EXPECT_TRUE(contains(ops, jbc::core::Op::LXOR)) << "预算超限后跨度不应被改";
}

}   // namespace grunt::number_fold
