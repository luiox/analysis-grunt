/// @file        arith_fold_test.cpp
/// @brief       残留 `arith_fold` 的三段判据测试（jasm 夹具现场汇编）。
///
/// 覆盖分工：
/// - 替换形状：八式里的五式代表面（INEG 两形 / IAND / IOR / IXOR 形 0），
///   锚点折回单条目标指令，窗口其余指令全擦；
/// - 常量窗口：int/long/TRIP1/MutateJumps 直线形/i2f 各值面，整窗归一
///   后折成单条常量装载；
/// - 拒绝面：除零、留死值、栈内类型不吻合、窗口中段标签四形，scan 零站点
///   且类字节逐位不动；
/// - 两轮收敛：形状还原暴露的新常量窗口由第二轮吃掉。
#include "grunt/arith_fold.hpp"

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

namespace grunt::arith_fold {
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
    if (!assembled.isOk()) {
        std::string detail;
        for (const auto& d : assembled.diagnostics) {
            detail += "\n  " + d.message.to_utf8_string().to_std_string();
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail);
    }
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

const model::ClassResource* classOf(const model::ResourceContainer& container,
                                    const std::string& uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls;
}

/// 拒绝面反证：scan 零站点 + rewrite 零改写后，类字节与夹具原字节逐位全等。
void expectRejectionLeavesBytesUntouched(model::ResourceContainer& container,
                                         const std::string& uri)
{
    const model::ClassResource* cls = classOf(container, uri);
    const std::vector<ca::u8> original = cls->get();

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 0u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 0u);

    const model::ClassResource* after = classOf(container, uri);
    const std::vector<ca::u8>& bytes = after->get();
    ASSERT_EQ(bytes.size(), original.size());
    EXPECT_EQ(std::memcmp(bytes.data(), original.data(), original.size()), 0)
        << "拒绝面类字节必须逐位不动";
}

// ---- 替换形状夹具 ------------------------------------------------------------

/// INEG 形 0：ICONST_M1 IXOR ICONST_1 IADD，前置被求反的栈值。
const char* kShapeIneg0 =
    ".super java/lang/Object\n"
    ".class public super fixture/AfIneg0 {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      iconst_1\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// INEG 形 1：ICONST_1 ISUB ICONST_M1 IXOR。
const char* kShapeIneg1 =
    ".super java/lang/Object\n"
    ".class public super fixture/AfIneg1 {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iconst_1\n"
    "      isub\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// IAND 形：SWAP DUP_X1 ICONST_M1 IXOR IOR SWAP ICONST_M1 IXOR ISUB。
const char* kShapeIand =
    ".super java/lang/Object\n"
    ".class public super fixture/AfIand {\n"
    "  .method public static m (II)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iload_1\n"
    "      swap\n"
    "      dup_x1\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      ior\n"
    "      swap\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      isub\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// IOR 形：DUP_X1 ICONST_M1 IXOR IAND IADD。
const char* kShapeIor =
    ".super java/lang/Object\n"
    ".class public super fixture/AfIor {\n"
    "  .method public static m (II)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iload_1\n"
    "      dup_x1\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      iand\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// IXOR 形 0：DUP2 IOR DUP_X2 POP IAND ISUB。
const char* kShapeIxor0 =
    ".super java/lang/Object\n"
    ".class public super fixture/AfIxor0 {\n"
    "  .method public static m (II)I {\n"
    "    code: {\n"
    "      iload_0\n"
    "      iload_1\n"
    "      dup2\n"
    "      ior\n"
    "      dup_x2\n"
    "      pop\n"
    "      iand\n"
    "      isub\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

// ---- 常量窗口夹具 ------------------------------------------------------------

/// int 常量对：0x1234 ^ 0x5678 = 0x444C = 17484，两条 LDC 都是池值。
const char* kConstIntXor =
    ".super java/lang/Object\n"
    ".class public super fixture/AfConstIntXor {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      ldc 4660\n"
    "      ldc 22136\n"
    "      ixor\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// TRIP1 形：12 ^ -1 = -13，-13 & 10 = 2（bipush/iconst 短形态喂给）。
const char* kConstTrip1 =
    ".super java/lang/Object\n"
    ".class public super fixture/AfConstTrip1 {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      bipush 12\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      bipush 10\n"
    "      iand\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// long 常量对：883318107 & 7 = 3，I64 位形双槽。
const char* kConstLongAnd =
    ".super java/lang/Object\n"
    ".class public super fixture/AfConstLongAnd {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc2_w 883318107L\n"
    "      ldc2_w 7L\n"
    "      land\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// MutateJumps 直线形：100 + 27 = 127（sipush/bipush 域）。
const char* kConstMutateJumps =
    ".super java/lang/Object\n"
    ".class public super fixture/AfConstMj {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      sipush 100\n"
    "      bipush 27\n"
    "      iadd\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// i2f 转换面：2 → 2.0f，F32 位形。
const char* kConstI2f =
    ".super java/lang/Object\n"
    ".class public super fixture/AfConstI2f {\n"
    "  .method public static m ()F {\n"
    "    code: {\n"
    "      ldc 2\n"
    "      i2f\n"
    "      freturn\n"
    "    }\n"
    "  }\n"
    "}\n";

// ---- 常量窗口拒绝面夹具 ------------------------------------------------------

/// 除零：1 / 0 原地保留 ArithmeticException 语义，整站放弃。
const char* kConstDivZero =
    ".super java/lang/Object\n"
    ".class public super fixture/AfDivZero {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      iconst_0\n"
    "      idiv\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 留死值：3 计算完后压 5 又 pop，净栈效不是一个值，整窗不折。
const char* kConstDeadValue =
    ".super java/lang/Object\n"
    ".class public super fixture/AfDeadValue {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      iconst_1\n"
    "      iconst_2\n"
    "      iadd\n"
    "      iconst_5\n"
    "      pop\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 栈内类型不吻合的基础形：两条 LDC2_W 喂 LAND，汇编合法。测试里再把
/// LAND 字节改成 IADD（I64 槽喂 int 运算），模拟 jasm 汇不出的错型窗口。
const char* kConstMixedWidth =
    ".super java/lang/Object\n"
    ".class public super fixture/AfMixedWidth {\n"
    "  .method public static m ()J {\n"
    "    code: {\n"
    "      ldc2_w 5L\n"
    "      ldc2_w 7L\n"
    "      land\n"
    "      lreturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 窗口中段标签：ldc 5 ldc 6 ixor 本可整窗归一，但分支在 ixor 汇合，
/// 合并点栈形耦合，整站放弃（两条路径各自保持 [I,I] 栈形，汇编合法）。
const char* kConstMidLabel =
    ".super java/lang/Object\n"
    ".class public super fixture/AfMidLabel {\n"
    "  .method public static m (I)I {\n"
    "    code: {\n"
    "      ldc 77\n"
    "      ldc 88\n"
    "      iload_0\n"
    "      ifeq Lmid\n"
    "      pop2\n"
    "      ldc 5\n"
    "      ldc 6\n"
    "    Lmid:\n"
    "      ixor\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

// ---- 两轮收敛夹具 ------------------------------------------------------------

/// 两轮收敛：IAND 形窗口内喂常量 60 与 24，第一轮还原出 IAND，
/// 第二轮吃掉留下的常量对 iand → 60 & 24 = 24（jasm 把 bipush 汇成 ldc，
/// 值面不变）。
const char* kTwoRound =
    ".super java/lang/Object\n"
    ".class public super fixture/AfTwoRound {\n"
    "  .method public static m ()I {\n"
    "    code: {\n"
    "      bipush 60\n"
    "      bipush 24\n"
    "      swap\n"
    "      dup_x1\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      ior\n"
    "      swap\n"
    "      iconst_m1\n"
    "      ixor\n"
    "      isub\n"
    "      ireturn\n"
    "    }\n"
    "  }\n"
    "}\n";

/// 把类字节里唯一一处 `LDC2_W ? ? LDC2_W ? ? LAND LRETURN` 序列的 LAND
/// （0x7F）改成 IADD（0x60）：两个 I64 槽喂 int 运算，求值器必须因类型
/// 不吻合停窗。jasm 的栈分析不许直接汇编这种错型，所以走字节改。
std::vector<ca::u8> mixedWidthBytes(const std::string& name)
{
    std::vector<ca::u8> bytes = asJava17(assemble(name, kConstMixedWidth));
    ca::usize           hit   = static_cast<ca::usize>(-1);
    for (ca::usize i = 0; i + 7 < bytes.size(); ++i) {
        if (bytes[i] != 0x14 || bytes[i + 3] != 0x14) continue;
        if (bytes[i + 6] != 0x7F || bytes[i + 7] != 0xAD) continue;
        if (hit != static_cast<ca::usize>(-1)) throw std::runtime_error("模式不唯一");
        hit = i + 6;
    }
    if (hit == static_cast<ca::usize>(-1)) throw std::runtime_error("找不到 LAND 序列");
    bytes[hit] = 0x60;
    return bytes;
}

}   // namespace

// ==== 替换形状 ================================================================

TEST(ArithFoldShape, RestoresInegForm0)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfIneg0", kShapeIneg0);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.shapeSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 0u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::ICONST_M1);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::INEG);
    EXPECT_EQ(ops[2], jbc::core::Op::IRETURN);
}

TEST(ArithFoldShape, RestoresInegForm1)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfIneg1", kShapeIneg1);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.shapeSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 1u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::ICONST_1);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::INEG);
    EXPECT_EQ(ops[2], jbc::core::Op::IRETURN);
}

TEST(ArithFoldShape, RestoresIand)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfIand", kShapeIand);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.shapeSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 2u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::SWAP);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::ILOAD_1);
    EXPECT_EQ(ops[2], jbc::core::Op::IAND);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);
}

TEST(ArithFoldShape, RestoresIor)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfIor", kShapeIor);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.shapeSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 3u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::DUP_X1);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::ILOAD_1);
    EXPECT_EQ(ops[2], jbc::core::Op::IOR);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);
}

TEST(ArithFoldShape, RestoresIxorForm0)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfIxor0", kShapeIxor0);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.shapeSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 4u);
    EXPECT_EQ(scanned.sites[0].anchorOp, jbc::core::Op::DUP2);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::ILOAD_1);
    EXPECT_EQ(ops[2], jbc::core::Op::IXOR);
    EXPECT_EQ(ops[3], jbc::core::Op::IRETURN);
}

// ==== 常量窗口 ================================================================

TEST(ArithFoldConst, FoldsIntXorPair)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfConstIntXor", kConstIntXor);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.constSites, 1u);
    EXPECT_EQ(scanned.sites[0].shapeIdx, 255u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I32);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.sites, 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);

    const ValueLoad v = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 17484);
}

TEST(ArithFoldConst, FoldsTrip1Form)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/AfConstTrip1", kConstTrip1);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.constSites, 1u);
    EXPECT_EQ(scanned.sites[0].value.i32(), 2);
}

TEST(ArithFoldConst, FoldsLongAndPair)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfConstLongAnd", kConstLongAnd);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::I64);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ValueLoad v = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I64);
    EXPECT_EQ(v.i64(), 3);
}

TEST(ArithFoldConst, FoldsMutateJumpsStraightLine)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfConstMj", kConstMutateJumps);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ValueLoad v = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 127);
}

TEST(ArithFoldConst, FoldsI2fToFloat)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfConstI2f", kConstI2f);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites[0].value.kind, ValueLoad::Kind::F32);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ValueLoad v = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::F32);
    EXPECT_EQ(v.f32(), 2.0f);
}

// ==== 拒绝面反证（memcmp 全等）================================================

TEST(ArithFoldRejectSurface, DivZeroLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/AfDivZero", kConstDivZero);
    expectRejectionLeavesBytesUntouched(container, "fixture/AfDivZero.class");
}

TEST(ArithFoldRejectSurface, DeadValueLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/AfDeadValue", kConstDeadValue);
    expectRejectionLeavesBytesUntouched(container, "fixture/AfDeadValue.class");
}

TEST(ArithFoldRejectSurface, MixedWidthLeavesBytesUntouched)
{
    model::ResourceContainer container;
    container.addResource("fixture/AfMixedWidth.class",
                          std::make_unique<model::ClassResource>(
                              "fixture/AfMixedWidth.class",
                              mixedWidthBytes("fixture/AfMixedWidth")));
    expectRejectionLeavesBytesUntouched(container, "fixture/AfMixedWidth.class");
}

TEST(ArithFoldRejectSurface, MidWindowLabelLeavesBytesUntouched)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/AfMidLabel", kConstMidLabel);
    expectRejectionLeavesBytesUntouched(container, "fixture/AfMidLabel.class");
}

// ==== 两轮收敛 ================================================================

TEST(ArithFoldRewrite, ShapeThenConstConvergesInTwoRounds)
{
    model::ResourceContainer container;
    const std::string uri = addFixture(container, "fixture/AfTwoRound", kTwoRound);

    // 第一轮：IAND 形状还原，窗口剩两个常量装载 + iand + ireturn
    // （jasm 把 bipush 汇成 ldc，不钉具体装载形态，只钉条数与形状）。
    const ScanResult first = scan(container);
    ASSERT_EQ(first.sites.size(), 1u);
    EXPECT_EQ(first.shapeSites, 1u);
    const DirtySet dirtyFirst = rewrite(container, first);
    ASSERT_EQ(dirtyFirst.uris.size(), 1u);

    const std::vector<jbc::core::Op> opsAfterFirst = realOps(classOf(container, uri)->get());
    ASSERT_EQ(opsAfterFirst.size(), 4u);
    EXPECT_EQ(opsAfterFirst[2], jbc::core::Op::IAND);
    EXPECT_EQ(opsAfterFirst[3], jbc::core::Op::IRETURN);
    const ValueLoad vAfterFirst = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(vAfterFirst.i32(), 60);

    // 第二轮：常量窗口吃掉还原出的 IAND，60 & 24 = 24。
    const ScanResult second = scan(container);
    ASSERT_EQ(second.sites.size(), 1u);
    EXPECT_EQ(second.constSites, 1u);
    const DirtySet dirtySecond = rewrite(container, second);
    ASSERT_EQ(dirtySecond.uris.size(), 1u);

    const std::vector<jbc::core::Op> ops = realOps(classOf(container, uri)->get());
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);
    const ValueLoad v = firstConstLoad(classOf(container, uri)->get());
    EXPECT_EQ(v.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(v.i32(), 24);
}

// ==== 收敛自证 ================================================================

TEST(ArithFoldRewrite, RescanAfterRewriteFindsNothing)
{
    model::ResourceContainer container;
    addFixture(container, "fixture/AfConstIntXor", kConstIntXor);
    addFixture(container, "fixture/AfConstMj", kConstMutateJumps);

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 2u);

    const ScanResult rescanned = scan(container);
    EXPECT_EQ(rescanned.sites.size(), 0u) << "折叠产物不得再成站点（收敛判据）";
}

}   // namespace grunt::arith_fold
