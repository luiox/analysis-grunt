/// @file        jsim_bridge_test.cpp
/// @brief       官方桥（jsim::jbcbridge）迁移后的两颗雷钉形测试。
///
/// 背景：自建 jsim_bridge 对 NEWARRAY / MULTIANEWARRAY 的搬运坐标错位——
/// libjbc 把 newarray 的 atype 与 multianewarray 的维数放在 `a` 槽，jsim 引擎
/// 却读 `b` 槽（engine.hpp executeArray）。错位后果两样：NEWARRAY 的 b 恒空
/// → 引擎「atype 非 primitive」恒 Bail（意外 fail-closed）；MULTIANEWARRAY
/// 不剥类名的 `[` 前缀且维数错位 → **静默求值错误**（全仓唯一非 fail-closed
/// 分歧）。迁官方桥后两者走官方搬运，本文件把行为钉成显式正形。
///
/// 覆盖分工：
/// - 桥形面：NEWARRAY 的 atype 落 b、MULTIANEWARRAY 剥 dims 落 b + 元素类型
///   进 allocTypes、CHECKCAST/INSTANCEOF 不占 allocTypes（官方 no-op 口径）；
/// - 拒绝面：INVOKEDYNAMIC / ATHROW 整方法不桥（UnsupportedOp，此前自建桥
///   是占位/原样搬运、运行期才拒——官方口径把拒绝提前到桥面）；
/// - 引擎求值面：桥出的数组方法经 SimExecutor 正确求值（长度、元素往返、
///   多维分配）+ 未知/负长度显式拒（NotConcrete / BadOperand）。
///
/// 夹具一律 jasm 现场汇编（同 constant_call_test 口径），汇编失败直接抛。
#include "grunt/container.hpp"
#include "grunt/jsim_executor.hpp"

#include "jbc/ir/units.hpp"

#include "jsim/engine.hpp"
#include "jsim/jbc.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::jsim_bridge_test {
namespace {

namespace model = mjbase::model;

namespace jbcbridge = jsim::jbcbridge;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源码 → class 字节。汇编失败直接抛（夹具写错要立刻炸，不能静默空跑）。
std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                        sourceName = utf16(name + ".jasm");
    auto                        text       = utf16(source);
    auto                        assembled  = service.assemble(sourceName.ref(), text.ref());
    if (!assembled.isOk()) throw std::runtime_error("jasm 夹具汇编失败：" + name);
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

/// 数组夹具：NEWARRAY / MULTIANEWARRAY 的正形求值与显式拒绝各占一法。
///
/// 槽位注记：静态方法的参数从 slot 0 起；无参方法的 slot 0 空闲可当暂存。
/// `len`/`probe` 的数组不落地也能走：dup 后栈上直接操作。
std::string arrSource()
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Arr {\n"
           // 正形：分配 int[len]，长度原样回来（atype 走 b 槽的证据）。
           "  .method public static len (I)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      newarray int\n"
           "      arraylength\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           // 元素往返：newarray → iastore → iaload（atype 错了到不了这一步）。
           "  .method public static probe (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      newarray int\n"
           "      astore 2\n"
           "      aload 2\n"
           "      iconst_1\n"
           "      iload 1\n"
           "      iastore\n"
           "      aload 2\n"
           "      iconst_1\n"
           "      iaload\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           // 拒绝面：负长度是确定要发生的 NegativeArraySizeException，显式 Bail。
           "  .method public static neg ()I {\n"
           "    code: {\n"
           "      iconst_m1\n"
           "      newarray int\n"
           "      arraylength\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           // 多维正形：[[I 两维全分配，外层长度 = 先压的 2。
           "  .method public static multi ()I {\n"
           "    code: {\n"
           "      iconst_2\n"
           "      iconst_3\n"
           "      multianewarray [[I 2\n"
           "      arraylength\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           // 多维内层：aaload 取内层 int[3]，长度 3 回来（元素类型剥 `[` 的证据）。
           "  .method public static multiInner ()I {\n"
           "    code: {\n"
           "      iconst_2\n"
           "      iconst_3\n"
           "      multianewarray [[I 2\n"
           "      astore 0\n"
           "      aload 0\n"
           "      iconst_1\n"
           "      aaload\n"
           "      arraylength\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// indy 拒绝面夹具：调用点含 invokedynamic（BSM 返回 null 也无所谓——
/// 拒绝发生在桥面，跑不到 BSM）。
std::string indySource()
{
    return ".super java/lang/Object\n"
           ".class public super IndyHost {\n"
           "  .method public static t (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public static a (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;"
           ")Ljava/lang/invoke/CallSite; {\n"
           "    code: {\n"
           "      aconst_null\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      bipush 3\n"
           "      bipush 4\n"
           "      invokedynamic t (II)I { invokestatic, IndyHost.a, "
           "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } "
           "{ \"t\", \"IndyHost\", (II)I }\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// athrow 拒绝面夹具：异常表派发 v1 不做，官方桥整方法不桥。
std::string athrowSource()
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Thrower {\n"
           "  .method public static boom ()V {\n"
           "    code: {\n"
           "      new java/lang/RuntimeException\n"
           "      dup\n"
           "      invokespecial java/lang/RuntimeException.<init> ()V\n"
           "      athrow\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// allocTypes 占位口径夹具：NEW 的类进表，CHECKCAST/INSTANCEOF 不进（官方
/// 语义 checkcast 恒等 / instanceof 压 Unknown，操作数无需落位——自建桥曾
/// 把它们也推进表，属无害但多余的占位，官方口径统一为 no-op）。
std::string allocSource()
{
    return ".super java/lang/Object\n"
           ".class public super fixture/Alloc {\n"
           "  .method public static misc ()I {\n"
           "    code: {\n"
           "      new java/lang/Object\n"
           "      dup\n"
           "      invokespecial java/lang/Object.<init> ()V\n"
           "      checkcast java/lang/String\n"
           "      aconst_null\n"
           "      instanceof java/lang/String\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 按方法名取下标（测试夹具的方法名唯一，不带描述符歧义）。
ca::usize methodIndexByName(jbc::ir::ClassSession& s, const std::string& name)
{
    const jbc::ir::ClassUnit& unit = s.unit();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) == name) return i;
    }
    throw std::runtime_error("夹具缺方法：" + name);
}

/// 按下标桥一个方法，失败带 stage 名炸出来（夹具拒绝面断言用不到正形路）。
jsim::Method bridgeMethodOrThrow(jbc::ir::ClassSession& s, ca::usize index)
{
    auto bridged = jsim::jbcbridge::toSimMethod(s, index);
    if (!bridged.isOk()) throw std::runtime_error("桥失败：" + bridged.error().detail);
    return std::move(bridged.value());
}

/// 在桥产物里按 opcode 找第一条指令（夹具方法短，线性扫即可）。
const jsim::Insn* insnOf(const jsim::Method& m, ca::u8 opcode)
{
    for (const jsim::Insn& ji : m.code) {
        if (ji.opcode == opcode) return &ji;
    }
    return nullptr;
}

}   // namespace

// ============================================================================
// ==== 桥形面（两颗雷的搬运坐标钉形）==========================================
// ============================================================================

TEST(JsimBridgeShape, NewarrayCarriesAtypeInSlotB)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    const std::vector<ca::u8>* bytes   = findClassBytes(container, "fixture/Arr.class");
    ASSERT_NE(bytes, nullptr);
    std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);
    ASSERT_NE(s, nullptr);

    const jsim::Method m = bridgeMethodOrThrow(*s, methodIndexByName(*s, "len"));
    const jsim::Insn*   na = insnOf(m, static_cast<ca::u8>(jbc::core::Op::NEWARRAY));
    ASSERT_NE(na, nullptr);
    // JVMS 6.5：newarray atype=10 是 int[]。libjbc 把 atype 放 a，jsim 引擎读
    // b（primitiveArrayClass(b)）——官方桥在桥内显式搬运（a 归零、b = atype）。
    EXPECT_EQ(na->a, 0);
    EXPECT_EQ(na->b, 10);
    // 原始类型数组的 atype 不进 allocTypes（那是引用类常量的表）。
    EXPECT_TRUE(m.allocTypes.empty());
}

TEST(JsimBridgeShape, MultianewarrayStripsDimsAndCarriesDimCount)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    const std::vector<ca::u8>* bytes   = findClassBytes(container, "fixture/Arr.class");
    ASSERT_NE(bytes, nullptr);
    std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);
    ASSERT_NE(s, nullptr);

    const jsim::Method m   = bridgeMethodOrThrow(*s, methodIndexByName(*s, "multi"));
    const jsim::Insn*  man = insnOf(m, static_cast<ca::u8>(jbc::core::Op::MULTIANEWARRAY));
    ASSERT_NE(man, nullptr);
    // 类名剥掉 dims 个 '['：[[[? 不存在——[[I 的元素类型是 I；维数 2 落 b
    //（引擎按 b 弹两份 count）。自建桥的错位（类名不剥 + b 恒空）在此钉死。
    ASSERT_EQ(m.allocTypes.size(), 1u);
    EXPECT_EQ(m.allocTypes.front(), "I");
    EXPECT_EQ(man->a, 0);
    EXPECT_EQ(man->b, 2);
}

TEST(JsimBridgeShape, OnlyNewOccupiesAllocTypes_CheckcastInstanceofNoop)
{
    model::ResourceContainer container = makeContainer({{"fixture/Alloc", allocSource()}});
    const std::vector<ca::u8>* bytes   = findClassBytes(container, "fixture/Alloc.class");
    ASSERT_NE(bytes, nullptr);
    std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);

    const jsim::Method m = bridgeMethodOrThrow(*s, methodIndexByName(*s, "misc"));
    // misc 里有 new / checkcast / instanceof 三类类操作数：只有 new 的类占
    // allocTypes（官方口径 checkcast 恒等、instanceof 压 Unknown，不落位）。
    ASSERT_EQ(m.allocTypes.size(), 1u);
    EXPECT_EQ(m.allocTypes.front(), "java/lang/Object");
}

// ============================================================================
// ==== 拒绝面（UnsupportedOp：拒绝从运行期提前到桥面）=========================
// ============================================================================

TEST(JsimBridgeReject, InvokedynamicIsWholeMethodUnsupportedOp)
{
    model::ResourceContainer container = makeContainer({{"IndyHost", indySource()}});
    const std::vector<ca::u8>* bytes   = findClassBytes(container, "IndyHost.class");
    ASSERT_NE(bytes, nullptr);
    std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);

    const auto bridged = jsim::jbcbridge::toSimMethod(*s, methodIndexByName(*s, "call"));
    ASSERT_FALSE(bridged.isOk());
    EXPECT_EQ(bridged.error().stage, jsim::jbcbridge::BridgeError::Stage::UnsupportedOp);
    // 同类的 BSM 方法体不含 indy，桥照常成功——拒绝是方法级，不是类级。
    const auto bsm = jsim::jbcbridge::toSimMethod(*s, methodIndexByName(*s, "a"));
    EXPECT_TRUE(bsm.isOk());
}

TEST(JsimBridgeReject, AthrowIsWholeMethodUnsupportedOp)
{
    model::ResourceContainer container = makeContainer({{"fixture/Thrower", athrowSource()}});
    const std::vector<ca::u8>* bytes   = findClassBytes(container, "fixture/Thrower.class");
    ASSERT_NE(bytes, nullptr);
    std::shared_ptr<jbc::ir::ClassSession> s = openSession(*bytes);

    const auto bridged = jsim::jbcbridge::toSimMethod(*s, methodIndexByName(*s, "boom"));
    ASSERT_FALSE(bridged.isOk());
    EXPECT_EQ(bridged.error().stage, jsim::jbcbridge::BridgeError::Stage::UnsupportedOp);
}

// ============================================================================
// ==== 引擎求值面（桥出方法经 SimExecutor 的正形与显式拒）=====================
// ============================================================================

/// 数组夹具的标准执行器（分层红线：核心裸会话即可，数组是指令集语义不装包）。
SimExecutor arrExecutor(const model::ResourceContainer& container)
{
    return SimExecutor(container);
}

TEST(JsimEngineEval, NewarrayAllocatesAndReportsLength)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    const SimOutcome out =
        exec.invoke("fixture/Arr", "len", {jsim::SimValue::knownI32(7)});
    ASSERT_TRUE(out.ok()) << out.detail;
    EXPECT_EQ(out.kind, SimOutcome::Kind::Returned);
    EXPECT_EQ(out.value.asU32(), 7u);
}

TEST(JsimEngineEval, NewarrayElementRoundtrip)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    const SimOutcome out =
        exec.invoke("fixture/Arr", "probe", {jsim::SimValue::knownI32(4),
                                             jsim::SimValue::knownI32(42)});
    ASSERT_TRUE(out.ok()) << out.detail;
    EXPECT_EQ(out.value.asU32(), 42u);
}

TEST(JsimEngineEval, MultianewarrayAllocatesAllDims)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    // 外层长度 = 先压的 2：引擎按 b=2 弹两份 count（错位时只弹一份，取错值）。
    const SimOutcome out = exec.invoke("fixture/Arr", "multi", {});
    ASSERT_TRUE(out.ok()) << out.detail;
    EXPECT_EQ(out.value.asU32(), 2u);
}

TEST(JsimEngineEval, MultianewarrayInnerLengthVisibleThroughAaload)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    // 内层 int[3] 可经 aaload 取到：元素类型剥 `[`（allocTypes["I"]）建出的
    // 嵌套形状是真的，不是带 `[` 前缀的静默错形。
    const SimOutcome out = exec.invoke("fixture/Arr", "multiInner", {});
    ASSERT_TRUE(out.ok()) << out.detail;
    EXPECT_EQ(out.value.asU32(), 3u);
}

TEST(JsimEngineEval, NewarrayRejectsUnknownLengthExplicitly)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    const SimOutcome out = exec.invoke(
        "fixture/Arr", "len", {jsim::SimValue::unknownOf(jsim::ValType::I32)});
    EXPECT_EQ(out.kind, SimOutcome::Kind::Bailed);
    EXPECT_NE(out.detail.find("newarray 长度不具体"), std::string::npos) << out.detail;
}

TEST(JsimEngineEval, NewarrayRejectsNegativeLengthExplicitly)
{
    model::ResourceContainer container = makeContainer({{"fixture/Arr", arrSource()}});
    SimExecutor              exec      = arrExecutor(container);

    const SimOutcome out = exec.invoke("fixture/Arr", "neg", {});
    EXPECT_EQ(out.kind, SimOutcome::Kind::Bailed);
    EXPECT_NE(out.detail.find("newarray 长度越界"), std::string::npos) << out.detail;
}

TEST(JsimEngineEval, IndyMethodFailsClosedAtBridge)
{
    model::ResourceContainer container = makeContainer({{"IndyHost", indySource()}});
    SimExecutor              exec      = arrExecutor(container);

    // 官方口径：含 indy 的方法整方法不桥，invoke 显式报「桥转换失败」——
    // 自建桥时代是占位 CallRef 运行期才 Bail，两路都拒但原因可读性不同。
    const SimOutcome out = exec.invoke("IndyHost", "call", {});
    EXPECT_EQ(out.kind, SimOutcome::Kind::Bailed);
    EXPECT_NE(out.detail.find("桥转换失败"), std::string::npos) << out.detail;
}

}   // namespace grunt::jsim_bridge_test
