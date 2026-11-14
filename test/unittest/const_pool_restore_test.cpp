/// @file        const_pool_restore_test.cpp
/// @brief       残留 `const_pool_restore` 的三段判据测试（jasm 夹具）。
///
/// 覆盖分工：
/// - 数值面：ConstantValue 注入后 num 字段定值回填，User 类两处 GETSTATIC
///   换成常量装载（数值走短形态，字符串 intern 进池走 LDC）；
/// - 字符串面：解密器模板钉形 + 逐码元 XOR 直解（key=32，"GRUNT" 直解回
///   "grunt"），写点三连形恰一次写；钉形覆盖不了的 junk 包裹变体走求值
///   通道（模拟执行真跑解密器，BSM 形方法在场不拒类）；
/// - 拒绝面：无数值 ConstantValue 不收、同字段双写不收、伴生类带实例字段
///   整类不收、解密器两通道都不收敛（null 返回）不收，四形 scan 零站点且
///   类字节逐位不动。
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

#include <cstdio>
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
        // 夹具失败现场落盘：源码比对用（rundir = 工程根；类名带 '/' 时拍平）。
        std::string flatName = name;
        for (char& ch : flatName) {
            if (ch == '/') ch = '_';
        }
        const std::string dump = "test/_jasm_fail_" + flatName + ".jasm";
        std::FILE*        f    = std::fopen(dump.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail + "（源码转储 " + dump
                                 + "）");
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

/// 解密器体核心：24 条指令逐字模板（key 是体内唯一自由变量，这里钉 32）。
/// 局部存取 jasm 汇成紧凑形（astore_1），钉形侧经归一后与生成源的一般形
/// 同一模板。loopOp 换 iand 即模板破坏变体。
std::string decCore(const char* loopOp)
{
    std::string src = "      new java/lang/StringBuilder\n"
                      "      dup\n"
                      "      invokespecial java/lang/StringBuilder.<init> ()V\n"
                      "      astore_1\n"
                      "      iconst_0\n"
                      "      istore_2\n"
                      "      goto C\n"
                      "    B:\n"
                      "      aload_1\n"
                      "      aload_0\n"
                      "      iload_2\n"
                      "      invokevirtual java/lang/String.charAt (I)C\n"
                      "      ldc 32\n";
    src += std::string("      ") + loopOp + "\n";
    src += "      i2c\n"
           "      invokevirtual java/lang/StringBuilder.append (C)Ljava/lang/StringBuilder;\n"
           "      pop\n"
           "      iinc 2 1\n"
           "    C:\n"
           "      iload_2\n"
           "      aload_0\n"
           "      invokevirtual java/lang/String.length ()I\n"
           "      if_icmplt B\n"
           "      aload_1\n"
           "      invokevirtual java/lang/StringBuilder.toString ()Ljava/lang/String;\n"
           "      areturn\n";
    return src;
}

std::string decBody(const std::string& self, const char* loopOp)
{
    std::string src = "  .method static dec (Ljava/lang/String;)Ljava/lang/String; {\n";
    src += "    code: {\n";
    src += decCore(loopOp);
    src += "    }\n";
    src += "  }\n";
    return src;
}

/// 求值通道变体：解密器体被 junk 包裹（恒真谓词 3<=7 + 死分支 ldc "junk"），
/// dom 序 29 条，钉形的 24 条整段钉不上（in.size() != 24 即拒）→ 键面空 →
/// 字段解码走求值通道真跑解密器（fork 变体 bI.JGzVjDfJzVW1ridB 形）。
std::string baitDecBody(const std::string& self)
{
    std::string src = "  .method static dec (Ljava/lang/String;)Ljava/lang/String; {\n";
    src += "    code: {\n";
    src += "      bipush 3\n";
    src += "      bipush 7\n";
    src += "      if_icmple Lreal\n";
    src += "      ldc \"junk\"\n";
    src += "      areturn\n";
    src += "    Lreal:\n";
    src += decCore("ixor");
    src += "    }\n";
    src += "  }\n";
    return src;
}

/// BSM 形方法（fork：StringEncrypt 的 BSM 落在伴生池类里）。签名形状自证，
/// 方法门经 isBootstrapDesc 放行不钉体；没有这道门，形状门外的方法会整类
/// 拒绝。体无关紧要（本 pass 不真跑它）。
std::string bsmStubBody(const std::string& self)
{
    (void)self;
    std::string src = "  .method static bsm (Ljava/lang/invoke/MethodHandles$Lookup;"
                      "Ljava/lang/String;Ljava/lang/invoke/MethodType;"
                      "Ljava/lang/String;)Ljava/lang/invoke/CallSite; {\n";
    src += "    parameters: { lk, nm, mt, key },\n";
    src += "    code: {\n";
    src += "      aconst_null\n";
    src += "      areturn\n";
    src += "    }\n";
    src += "  }\n";
    return src;
}

/// 伴生类源码：final super、super = Object、字段全 static（extraField 非空
/// 即加一个实例字段做形状钉失败变体），clinitTagWrites 给 tag 的写点序列
/// （写两遍即双写变体），decSrc 给解密器体（decBody / baitDecBody）。
/// key=32，"GRUNT" 直解回 "grunt"。
std::string poolSource(const std::string& name, const char* extraField,
                       const char* clinitTagWrites, const std::string& decSrc)
{
    const std::string self = "fixture/" + name;
    std::string       src  = ".super java/lang/Object\n";
    src += ".class public final super " + self + " {\n";
    src += "  .field public static num I\n";
    src += "  .field public static tag Ljava/lang/String;\n";
    src += extraField;
    src += "\n";
    src += decSrc;
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
                                            const std::string&        decSrc,
                                            bool                      injectCv)
{
    std::vector<ca::u8> poolBytes =
        assemble("fixture/" + poolName, poolSource(poolName, extraField, clinitTagWrites, decSrc));
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
        addPair(container, poolName, "", tagWrite(poolName).c_str(),
                decBody("fixture/" + poolName, "ixor"), true);
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
    addPair(container, poolName, "", tagWrite(poolName).c_str(),
            decBody("fixture/" + poolName, "ixor"), false);

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
        addPair(container, poolName, "", twice.c_str(),
                decBody("fixture/" + poolName, "ixor"), false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

TEST(ConstPoolRestoreRejectSurface, InstanceFieldLeavesBytesUntouched)
{
    model::ResourceContainer container;
    const std::string poolName = "PoolInstance";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "  .field public extra I\n", tagWrite(poolName).c_str(),
                decBody("fixture/" + poolName, "ixor"), false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

TEST(ConstPoolRestoreRejectSurface, UnconvergentDecryptorLeavesBytesUntouched)
{
    // 求值通道后的拒绝面口径：钉形失败只是快路径缺失（iand 变体这类确定性
    // 可求值的，求值通道照收），字段不收的条件是两通道都不收敛——null 返回
    // 的解密器求值不为 Str 载荷，字段放弃，scan 零站点，类字节逐位不动。
    model::ResourceContainer container;
    const std::string poolName = "PoolBroken";
    const std::string nullDec =
        "  .method static dec (Ljava/lang/String;)Ljava/lang/String; {\n"
        "    code: {\n"
        "      aconst_null\n"
        "      areturn\n"
        "    }\n"
        "  }\n";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", tagWrite(poolName).c_str(), nullDec, false);
    expectRejectionLeavesBytesUntouched(container, poolUri);
    expectRejectionLeavesBytesUntouched(container, userUri);
}

// ==== PROOF ===================================================================

TEST(ConstPoolRestoreProof, ClassesIntactAfterRewrite)
{
    model::ResourceContainer container;
    const std::string poolName = "Pool";
    addPair(container, poolName, "", tagWrite(poolName).c_str(),
            decBody("fixture/" + poolName, "ixor"), true);

    const ca::usize before = countClasses(container);
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 2u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);

    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
    EXPECT_EQ(countClasses(container), before);
}

// ==== 求值通道（钉形变体）=====================================================

TEST(ConstPoolRestoreEval, JunkWrappedDecryptorDecodesViaSimulation)
{
    // 解密器体 junk 包裹 → 钉形整段钉不上（dom 序 29 ≠ 24），类形状门不再
    // 因此拒类；BSM 形方法在场（fork：StringEncrypt 的 BSM 落在伴生池类），
    // 经签名判型放行。字段解码两通道：键面空 → 求值通道真跑解密器 → "grunt"。
    model::ResourceContainer container;
    const std::string poolName = "PoolEval";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", tagWrite(poolName).c_str(),
                baitDecBody("fixture/" + poolName) + bsmStubBody("fixture/" + poolName),
                false);

    const ScanResult scanned = scan(container);
    ASSERT_FALSE(scanned.censusIncomplete);
    EXPECT_EQ(scanned.poolClasses, 1u) << "钉形失败 + BSM 方法在场的类不该整类拒绝";
    ASSERT_EQ(scanned.fields.size(), 1u);
    EXPECT_EQ(scanned.fields.front().name, "tag");
    ASSERT_EQ(scanned.fields.front().value.kind, ValueLoad::Kind::Str);
    EXPECT_EQ(scanned.fields.front().value.str, "grunt") << "求值通道应真跑解密器解码";

    // 调用点照常登记 + 回填（User.tag() 的 GETSTATIC 换 LDC）。
    ASSERT_EQ(scanned.sites.size(), 1u);
    const DirtySet dirty = rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.uris.front(), userUri);
    const ValueLoad tagV = firstConstLoadOfMethod(classOf(container, userUri)->get(), "tag");
    EXPECT_EQ(tagV.kind, ValueLoad::Kind::Str);
    EXPECT_EQ(tagV.str, "grunt");
}

TEST(ConstPoolRestoreEval, NonXorLoopOpDecodesByRuntimeSemantics)
{
    // 契约变化锚点：loopOp 换 iand 的模板破坏变体，旧实现整类拒绝，求值
    // 通道按运行期语义照收——解密器运行期算什么，明文就是什么（确定性
    // 可求值不是拒绝理由）。key=32：大写字母 bit5 全 0（&32 → 0），小写
    // 全 1（&32 → 0x20），两密文的解码面直接钉死。
    model::ResourceContainer container;
    const std::string poolName = "PoolIand";
    const auto [poolUri, userUri] =
        addPair(container, poolName, "", tagWrite(poolName).c_str(),
                decBody("fixture/" + poolName, "iand"), false);

    const ScanResult scanned = scan(container);
    ASSERT_FALSE(scanned.censusIncomplete);
    EXPECT_EQ(scanned.poolClasses, 1u);
    ASSERT_EQ(scanned.fields.size(), 1u);
    ASSERT_EQ(scanned.fields.front().value.kind, ValueLoad::Kind::Str);
    EXPECT_EQ(scanned.fields.front().value.str, std::string("\0\0\0\0\0", 5))
        << "GRUNT 逐码元 & 32 应全为 0";

    model::ResourceContainer container2;
    const std::string poolName2 = "PoolIandLower";
    const std::string clinitLower =
        std::string("      ldc \"grunt\"\n"
                    "      invokestatic fixture/" + poolName2 +
                    ".dec (Ljava/lang/String;)Ljava/lang/String;\n"
                    "      putstatic fixture/" + poolName2 +
                    ".tag Ljava/lang/String;\n");
    addPair(container2, poolName2, "", clinitLower.c_str(),
            decBody("fixture/" + poolName2, "iand"), false);
    const ScanResult scanned2 = scan(container2);
    ASSERT_EQ(scanned2.fields.size(), 1u);
    EXPECT_EQ(scanned2.fields.front().value.str, std::string("\x20\x20\x20\x20\x20", 5))
        << "grunt 逐码元 & 32 应全为 0x20";
}

}   // namespace grunt::const_pool_restore
