/// @file        const_pool_restore_test.cpp
/// @brief       残留 `const_pool_restore` 的三段判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - 数值面：ConstantValue 注入后 num 字段定值回填，User 类两处 GETSTATIC
///   换成常量装载（数值走短形态，字符串 intern 进池走 LDC）；
/// - 字符串面：解密器模板钉形 + 逐码元 XOR 直解（key=32，"GRUNT" 直解回
///   "grunt"），写点三连形恰一次写；
/// - 拒绝面：无数值 ConstantValue 不收、同字段双写不收、伴生类带实例字段
///   整类不收、解密器模板破坏整类不收，四形 scan 零站点且类字节逐位不动。
#include "grunt/const_pool_restore.hpp"

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
#include "grunt/knowledge/const_args.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::const_pool_restore {
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

std::string addFixture(model::ResourceContainer& container, const std::string& name,
                       const std::vector<ca::u8>& bytes)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    return uri;
}

/// 绕过 jasm 给字段注入 ConstantValue（jasm 无法声明字段初值），抄
/// const_value_promote_test 的助手。
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

const model::ClassResource* classOf(const model::ResourceContainer& container,
                                    const std::string& uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls;
}

/// 按方法名取改写后首条常量装载的值面（User 的 num/tag 两方法分别断言）。
ValueLoad firstConstLoadOfMethod(const std::vector<ca::u8>& bytes, const std::string& method)
{
    auto session = openSession(bytes);
    if (!session) throw std::runtime_error("产物无法解析");
    const jbc::ir::ClassUnit& unit = session->unit();
    ca::usize                 mi   = static_cast<ca::usize>(-1);
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (unit.symbols().str(unit.methods[i].name) == method) {
            mi = i;
            break;
        }
    }
    if (mi == static_cast<ca::usize>(-1)) throw std::runtime_error("产物缺方法：" + method);
    auto promoted = jbc::ir::promoteMethod(*session, mi);
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

/// 拒绝面反证：scan 零站点 + rewrite 零改写后，类字节与夹具原字节逐位全等。
void expectRejectionLeavesBytesUntouched(model::ResourceContainer& container,
                                         const std::string& uri)
{
    const std::vector<ca::u8> original = classOf(container, uri)->get();

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 0u);
    EXPECT_FALSE(scanned.censusIncomplete);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 0u);

    const std::vector<ca::u8>& bytes = classOf(container, uri)->get();
    ASSERT_EQ(bytes.size(), original.size());
    EXPECT_EQ(std::memcmp(bytes.data(), original.data(), original.size()), 0)
        << "拒绝面类字节必须逐位不动";
}

// ---- 夹具 --------------------------------------------------------------------

/// 解密器体：24 条指令逐字模板（key 是体内唯一自由变量，这里钉 32）。
/// 局部存取 jasm 汇成紧凑形（astore_1），钉形侧经归一后与生成源的一般形
/// 同一模板。loopOp 换 iand 即模板破坏变体。
std::string decBody(const std::string& self, const char* loopOp)
{
    std::string src = "  .method static dec (Ljava/lang/String;)Ljava/lang/String; {\n";
    src += "    code: {\n";
    src += "      new java/lang/StringBuilder\n";
    src += "      dup\n";
    src += "      invokespecial java/lang/StringBuilder.<init> ()V\n";
    src += "      astore_1\n";
    src += "      iconst_0\n";
    src += "      istore_2\n";
    src += "      goto C\n";
    src += "    B:\n";
    src += "      aload_1\n";
    src += "      aload_0\n";
    src += "      iload_2\n";
    src += "      invokevirtual java/lang/String.charAt (I)C\n";
    src += "      ldc 32\n";
    src += std::string("      ") + loopOp + "\n";
    src += "      i2c\n";
    src += "      invokevirtual java/lang/StringBuilder.append (C)Ljava/lang/StringBuilder;\n";
    src += "      pop\n";
    src += "      iinc 2 1\n";
    src += "    C:\n";
    src += "      iload_2\n";
    src += "      aload_0\n";
    src += "      invokevirtual java/lang/String.length ()I\n";
    src += "      if_icmplt B\n";
    src += "      aload_1\n";
    src += "      invokevirtual java/lang/StringBuilder.toString ()Ljava/lang/String;\n";
    src += "      areturn\n";
    src += "    }\n";
    src += "  }\n";
    return src;
}

/// 伴生类源码：final super、super = Object、字段全 static（extraField 非空
/// 即加一个实例字段做形状钉失败变体），clinitTagWrites 给 tag 的写点序列
/// （写两遍即双写变体）。key=32，"GRUNT" 直解回 "grunt"。
std::string poolSource(const std::string& name, const char* extraField,
                       const char* clinitTagWrites, const char* loopOp)
{
    const std::string self = "fixture/" + name;
    std::string       src  = ".super java/lang/Object\n";
    src += ".class public final super " + self + " {\n";
    src += "  .field public static num I\n";
    src += "  .field public static tag Ljava/lang/String;\n";
    src += extraField;
    src += "\n";
    src += decBody(self, loopOp);
    src += "\n";
    src += "  .method static <clinit> ()V {\n";
    src += "    code: {\n";
    src += clinitTagWrites;
    src += "      return\n";
    src += "    }\n";
    src += "  }\n";
    src += "}\n";
    return src;
}

/// clinit 里对 tag 的一次写：LDC 密文 + INVOKESTATIC 自家解密器 + PUTSTATIC。
std::string tagWrite(const std::string& poolName)
{
    const std::string self = "fixture/" + poolName;
    return "      ldc \"GRUNT\"\n"
           "      invokestatic " + self + ".dec (Ljava/lang/String;)Ljava/lang/String;\n"
           "      putstatic " + self + ".tag Ljava/lang/String;\n";
}

/// 用户类源码：num/tag 两处 GETSTATIC 调用点，指向指定伴生类。
std::string userSource(const std::string& poolName)
{
    const std::string pool = "fixture/" + poolName;
    std::string       src  = ".super java/lang/Object\n";
    src += ".class public super fixture/User" + poolName + " {\n";
    src += "  .method public static num ()I {\n    code: {\n";
    src += "      getstatic " + pool + ".num I\n      ireturn\n    }\n  }\n";
    src += "\n";
    src += "  .method public static tag ()Ljava/lang/String; {\n    code: {\n";
    src += "      getstatic " + pool + ".tag Ljava/lang/String;\n      areturn\n    }\n  }\n";
    src += "}\n";
    return src;
}

/// 双类夹具装配：伴生类 + 指向它的用户类，返回 {伴生类 uri, 用户类 uri}。
std::pair<std::string, std::string> addPair(model::ResourceContainer& container,
                                            const std::string&        poolName,
                                            const char*               extraField,
                                            const char*               clinitTagWrites,
                                            const char*               loopOp,
                                            bool                      injectCv)
{
    std::vector<ca::u8> poolBytes =
        assemble("fixture/" + poolName, poolSource(poolName, extraField, clinitTagWrites, loopOp));
    if (injectCv) poolBytes = withConstantValue(poolBytes, "num", 42);
    const std::string userClassName = "fixture/User" + poolName;
    const std::string poolUri = addFixture(container, "fixture/" + poolName, poolBytes);
    const std::string userUri =
        addFixture(container, userClassName, assemble(userClassName, userSource(poolName)));
    return {poolUri, userUri};
}

}   // namespace

// ==== SCAN + REWRITE（数值面与字符串面正例）===================================

TEST(ConstPoolRestoreRewrite, BackfillsNumericAndStringCallSites)
{
    model::ResourceContainer container;
    const std::string poolName = "Pool";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", tagWrite(poolName).c_str(), "ixor", true);
    const std::vector<ca::u8> poolBefore = classOf(container, poolUri)->get();

    const ScanResult scanned = scan(container);
    ASSERT_FALSE(scanned.censusIncomplete);
    EXPECT_EQ(scanned.poolClasses, 1u);
    EXPECT_EQ(scanned.fieldsNumeric, 1u);
    EXPECT_EQ(scanned.fieldsString, 1u);
    ASSERT_EQ(scanned.fields.size(), 2u);

    // 两类字段各定值：num = 42（ConstantValue），tag = "grunt"（直解）。
    const FieldValue* num = nullptr;
    const FieldValue* tag = nullptr;
    for (const FieldValue& f : scanned.fields) {
        if (f.name == "num") num = &f;
        if (f.name == "tag") tag = &f;
    }
    ASSERT_NE(num, nullptr);
    EXPECT_EQ(num->desc, "I");
    ASSERT_EQ(num->value.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(num->value.i32(), 42);
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->desc, "Ljava/lang/String;");
    ASSERT_EQ(tag->value.kind, ValueLoad::Kind::Str);
    EXPECT_EQ(tag->value.str, "grunt");

    // 调用点：User 的 num/tag 两处 GETSTATIC 各成站点。
    ASSERT_EQ(scanned.sites.size(), 2u);
    ca::usize numSites = 0;
    ca::usize tagSites = 0;
    for (const Site& s : scanned.sites) {
        if (scanned.fields[s.field].name == "num") ++numSites;
        if (scanned.fields[s.field].name == "tag") ++tagSites;
    }
    EXPECT_EQ(numSites, 1u);
    EXPECT_EQ(tagSites, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), userUri) << "伴生类不进脏集";
    EXPECT_EQ(dirty.sites, 2u);

    // User 重解析：num() 首条常量装载 42，tag() 首条 ldc "grunt"。
    const ValueLoad numV = firstConstLoadOfMethod(classOf(container, userUri)->get(), "num");
    EXPECT_EQ(numV.kind, ValueLoad::Kind::I32);
    EXPECT_EQ(numV.i32(), 42);
    const ValueLoad tagV = firstConstLoadOfMethod(classOf(container, userUri)->get(), "tag");
    EXPECT_EQ(tagV.kind, ValueLoad::Kind::Str);
    EXPECT_EQ(tagV.str, "grunt");

    // 伴生类字节不动（回填只动调用点类）。
    const std::vector<ca::u8>& poolAfter = classOf(container, poolUri)->get();
    ASSERT_EQ(poolAfter.size(), poolBefore.size());
    EXPECT_EQ(std::memcmp(poolAfter.data(), poolBefore.data(), poolAfter.size()), 0)
        << "伴生类字节必须逐位不动";
}

// ==== 拒绝面 ==================================================================

TEST(ConstPoolRestoreScan, NumericFieldWithoutConstantValueNotCollected)
{
    model::ResourceContainer container;
    const std::string poolName = "Pool";
    addPair(container, poolName, "", tagWrite(poolName).c_str(), "ixor", false);

    const ScanResult scanned = scan(container);
    ASSERT_FALSE(scanned.censusIncomplete);
    ASSERT_EQ(scanned.fields.size(), 1u);
    EXPECT_EQ(scanned.fields.front().name, "tag") << "无数值 ConstantValue 不收";

    // num 站点为 0，tag 站点照常。
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.fields[scanned.sites.front().field].name, "tag");
}

TEST(ConstPoolRestoreRejectSurface, DoubleWriteLeavesBytesUntouched)
{
    model::ResourceContainer container;
    const std::string poolName = "PoolTwice";
    const std::string twice   = tagWrite(poolName) + tagWrite(poolName);
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", twice.c_str(), "ixor", false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

TEST(ConstPoolRestoreRejectSurface, InstanceFieldLeavesBytesUntouched)
{
    model::ResourceContainer container;
    const std::string poolName = "PoolInstance";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "  .field public extra I\n", tagWrite(poolName).c_str(),
                "ixor", false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

TEST(ConstPoolRestoreRejectSurface, BrokenDecryptTemplateLeavesBytesUntouched)
{
    model::ResourceContainer container;
    const std::string poolName = "PoolBroken";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", tagWrite(poolName).c_str(), "iand", false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

// ==== PROOF ===================================================================

TEST(ConstPoolRestoreProof, ClassesIntactAfterRewrite)
{
    model::ResourceContainer container;
    const std::string poolName = "Pool";
    addPair(container, poolName, "", tagWrite(poolName).c_str(), "ixor", true);

    const ca::usize before = countClasses(container);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_EQ(countClasses(container), before);
}

}   // namespace grunt::const_pool_restore
