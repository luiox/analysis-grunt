/// @file        junk_member_strip_test.cpp
/// @brief       残留 `grunt.JunkMemberStrip` 的扫描/改写/验收测试。
///
/// 五个规定面 + 三个补充面：
/// 1. 混合类全收：数字开头 junk 字段 + `$g` 形字段/方法 + clinit junk 键段
///    （ldc2_w/nop 对 + 状态机混算 + dup/ifeq 条件分发）+ 真实池初始化并存
///    ——区域 NOP 化 + 成员全删 + 真实初始化保真；
/// 2. fail-closed 三面：真实方法调用探针（纯调用点不构成区域，G3）、真实
///    代码读 junk 字段（读值流出区域，G2 入口起区域判）、白名单外锚截断
///    馈送链（改写后保留 return 槽深非零，判 11）——整方法放弃、字节不动；
/// 3. 模拟不出面：栈效应不可模拟的 clinit（字节级注入不平衡体，jasm 汇编器
///    拒绝不平衡源码）——整方法放弃；
/// 4. 干净类零搅动：无 junk 成员的类不进 dirty、字节零变化；
/// 5. 夹层真实块 + SMT 帧：区域中段的真实初始化块保留、锚在其上的 SMT 帧
///    保留并跨被摘帧重排 delta（same_frame_extended 升格形断言）；
/// 6. 真实样本回归：dec_jdk.jar 全量 655 类进容器（jar 缺位则跳过），钉死
///    普查/删除/residual 数字，App 抽查 junk 成员删净；
/// 7. 联动面：删净成员后伴生类（`Comp.helper` 空壳）成为既有
///    `grunt.DeadMember` 的删除候选，整类摘除接力成立。
///
/// 夹具改名口径：jasm 不接受 `$` 前缀/数字开头的裸标识符，夹具用等长占位名
/// 汇编，产物字节按名替换（utf8 长度前缀不变、池计数不变，全部引用共享同一
/// utf8 条目，一处换字节处处生效）。
#include "grunt/junk_member_strip.hpp"

#include "grunt/container.hpp"
#include "grunt/dead_member.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace grunt::junk_member_strip {
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
        // 夹具失败现场落盘：源码比对用（rundir = 工程根）。
        const std::string dump = "test/_jasm_fail_" + name + ".jasm";
        std::FILE* f = std::fopen(dump.c_str(), "wb");
        if (f != nullptr) {
            std::fwrite(source.data(), 1, source.size(), f);
            std::fclose(f);
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail + "（源码转储 " + dump
                                 + "）");
    }
    return std::move(assembled.value->bytes);
}

/// @brief 汇编 + 字节级改名（占位名 → 真实 junk 形名，等长换字节）。
std::vector<ca::u8> assembleJunkShaped(
    const std::string& name, const std::string& source,
    std::initializer_list<std::pair<std::string, std::string>> renames)
{
    std::vector<ca::u8> bytes = assemble(name, source);
    for (const auto& [from, to] : renames) {
        if (from.size() != to.size()) throw std::runtime_error("改名表不等长：" + from);
        ca::usize hits = 0;
        for (ca::usize i = 0; i + from.size() <= bytes.size(); ++i) {
            const bool hit = std::equal(from.begin(), from.end(),
                                        bytes.begin() + static_cast<ca::i64>(i));
            if (!hit) continue;
            std::copy(to.begin(), to.end(), bytes.begin() + static_cast<ca::i64>(i));
            i += from.size() - 1;
            ++hits;
        }
        if (hits == 0) throw std::runtime_error("占位名不在产物里：" + from);
    }
    return bytes;
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                        result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

void addClassBytes(model::ResourceContainer& container, const std::string& name,
                   const std::vector<ca::u8>& bytes)
{
    const std::string uri = name + ".class";
    container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
}

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&             uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto*             cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

bool containsUri(const DirtySet& dirty, const std::string& uri)
{
    return std::find(dirty.uris.begin(), dirty.uris.end(), uri) != dirty.uris.end();
}

/// @brief jasm 的 nop 串（夹具馈送链拉长用，压真实块首指令 pc 越过 64）。
std::string nops(int count)
{
    std::string out;
    for (int i = 0; i < count; ++i) out += "      nop\n";
    return out;
}

// ============================================================================
// ==== 夹具源码 ===============================================================
// ============================================================================

/// 测试①：真实池初始化前缀 + junk 键键段（含 dup/ifeq 条件分发）+ 探针方法
/// + 真实入口并存。占位名（qflda/qfldb/qfldi/qfldp/qmeth1）汇编后改 junk 形。
std::string mixedHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host {\n"
        "  .field private static qflda J\n"
        "  .field private static qfldb J\n"
        "  .field private static qfldi I\n"
        "  .field private static qfldp [Ljava/lang/String;\n"
        "  .field private static realPool [Ljava/lang/String;\n"
        "  .method static realInit ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method static qmeth1 ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 4\n"
        "      anewarray java/lang/String\n"
        "      putstatic Host.realPool [Ljava/lang/String;\n"
        "      invokestatic Host.realInit ()V\n"
        "      ldc2_w 111L\n"
        "      nop\n"
        "      nop\n"
        "      putstatic Host.qflda J\n"
        "      getstatic Host.qflda J\n"
        "      ldc2_w 222L\n"
        "      lsub\n"
        "      putstatic Host.qflda J\n"
        "      iconst_0\n"
        "      putstatic Host.qfldi I\n"
        "      invokestatic Host.qmeth1 ()I\n"
        "      dup\n"
        "      ifeq L1\n"
        "      getstatic Host.qfldi I\n"
        "      swap\n"
        "      ldc 77\n"
        "      ior\n"
        "      ior\n"
        "      putstatic Host.qfldi I\n"
        "      getstatic Host.qflda J\n"
        "      ldc2_w 333L\n"
        "      lxor\n"
        "      putstatic Host.qflda J\n"
        "      aconst_null\n"
        "      putstatic Host.qfldp [Ljava/lang/String;\n"
        "      goto Lexit\n"
        "    L1:\n"
        "      pop\n"
        "      goto Lexit\n"
        "    Lexit:\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      bipush 4\n"
        "      invokestatic java/lang/Math.max (II)I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试②：探针方法一个被真实方法调用（活引用）、一个零引用（死码对照）。
std::string callerHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host2 {\n"
        "  .method static qmeth1 ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method static qmeth2 ()I {\n"
        "    code: {\n"
        "      bipush 9\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      invokestatic Host2.qmeth1 ()I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试③：junk 字段被真实代码读（读值经 lreturn 流出）。
std::string readerHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host3 {\n"
        "  .field private static qflda J\n"
        "  .method public static read ()J {\n"
        "    code: {\n"
        "      getstatic Host3.qflda J\n"
        "      lreturn\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试④：junk 键段里混白名单外锚（new+pop 自棄分配）——锚截断馈送回拉链，
/// 闭包缺 ldc2_w 馈送前缀，改写后保留 return 槽深非零 → 整方法放弃。
std::string anchoredHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host4 {\n"
        "  .field private static qflda J\n"
        "  .field private static realPool [Ljava/lang/String;\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 4\n"
        "      anewarray java/lang/String\n"
        "      putstatic Host4.realPool [Ljava/lang/String;\n"
        "      ldc2_w 111L\n"
        "      nop\n"
        "      nop\n"
        "      new java/lang/Object\n"
        "      pop\n"
        "      putstatic Host4.qflda J\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试④b：clinit 源码保持平凡（jasm 汇编器拒绝不平衡体），Code 字节程序化
/// 换成 bipush/pop/pop/return（第二次 pop 槽位下溢，栈模拟放弃）。
std::string simFailHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host5 {\n"
        "  .field private static qflda J\n"
        "  .method public static read ()J {\n"
        "    code: {\n"
        "      getstatic Host5.qflda J\n"
        "      lreturn\n"
        "    }\n"
        "  }\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试⑤：干净类（无 junk 形成员）。
std::string cleanHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host {\n"
        "  .method private static secret (II)I {\n"
        "    code: {\n"
        "      iload 0\n"
        "      iload 1\n"
        "      iadd\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      bipush 4\n"
        "      invokestatic java/lang/Math.max (II)I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试⑦：junk 键段中段夹真实初始化块（new/dup/invokespecial/pop）+ nop 串
/// 把真实块首指令 pc 压过 64（SMT same_frame delta 超 63 → 升格 extended）。
std::string midRegionHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host8 {\n"
        "  .field private static qflda J\n"
        "  .field private static qfldi I\n"
        "  .field private static qfldp [Ljava/lang/String;\n"
        "  .field private static realPool [Ljava/lang/String;\n"
        "  .method static realInit ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method static qmeth1 ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 4\n"
        "      anewarray java/lang/String\n"
        "      putstatic Host8.realPool [Ljava/lang/String;\n"
        "      invokestatic Host8.realInit ()V\n"
        "      ldc2_w 111L\n"
        + nops(24) +
        "      putstatic Host8.qflda J\n"
        "      getstatic Host8.qflda J\n"
        "      ldc2_w 222L\n"
        "      lsub\n"
        "      putstatic Host8.qflda J\n"
        "      iconst_0\n"
        "      putstatic Host8.qfldi I\n"
        "      invokestatic Host8.qmeth1 ()I\n"
        "      dup\n"
        "      ifeq L1\n"
        "      getstatic Host8.qfldi I\n"
        "      swap\n"
        "      ldc 77\n"
        "      ior\n"
        "      ior\n"
        "      putstatic Host8.qfldi I\n"
        "      aconst_null\n"
        "      putstatic Host8.qfldp [Ljava/lang/String;\n"
        "      new java/lang/Object\n"
        "      dup\n"
        "      invokespecial java/lang/Object.<init> ()V\n"
        "      pop\n"
        "      getstatic Host8.qflda J\n"
        "      ldc2_w 444L\n"
        "      ladd\n"
        "      putstatic Host8.qflda J\n"
        "      goto Lexit\n"
        "    L1:\n"
        "      pop\n"
        "      goto Lexit\n"
        "    Lexit:\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

/// 测试⑨：宿主 junk 键段调探针、探针体调伴生类 `Comp.helper`——宿主删净
/// 成员后 Comp 失去引用，DeadMember 空壳回收接力（联动验收面）。
std::string handoffHostSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Host9 {\n"
        "  .field private static qflda J\n"
        "  .field private static realPool [Ljava/lang/String;\n"
        "  .method static qmeth1 ()I {\n"
        "    code: {\n"
        "      invokestatic Comp.helper ()I\n"
        "      pop\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 4\n"
        "      anewarray java/lang/String\n"
        "      putstatic Host9.realPool [Ljava/lang/String;\n"
        "      ldc2_w 111L\n"
        "      nop\n"
        "      nop\n"
        "      putstatic Host9.qflda J\n"
        "      invokestatic Host9.qmeth1 ()I\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static call ()I {\n"
        "    code: {\n"
        "      bipush 3\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

std::string companionSource()
{
    return
        ".super java/lang/Object\n"
        ".class public super Comp {\n"
        "  .method private static helper ()I {\n"
        "    code: {\n"
        "      bipush 5\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
}

// ============================================================================
// ==== SMT 夹具工具 ===========================================================
// ============================================================================

/// @brief 程序化给 clinit 挂合成 StackMapTable（替换已有帧）并回写字节。
///
/// jasm 对分支体可能自产帧、帧位不确定；测试要确定性的锚位（区域内 / 夹层
/// 真实块首指令），程序化补挂（junk_region_strip / metadata_strip 同形）。
std::vector<ca::u8> attachSyntheticClinitSmt(
    const std::vector<ca::u8>& bytes,
    const std::vector<jbc::core::StackMapTableAttr::Frame>& frames)
{
    using jbc::core::AttrSlot;
    using jbc::core::StackMapTableAttr;
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("SMT 夹具重解析失败");
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    bool done = false;
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) != "<clinit>") continue;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) throw std::runtime_error("SMT 夹具 Code 解不开");
            if (!ca::core::tag_cast::isa<jbc::core::CodeAttr>(d.unwrap())) {
                throw std::runtime_error("SMT 夹具 Code 形不符");
            }
            auto* body = ca::core::tag_cast::cast<jbc::core::CodeAttr>(d.unwrap());
            if (body == nullptr) throw std::runtime_error("SMT 夹具 Code 空指针");
            // 替换已有 SMT 槽（jasm 自产帧位不确定，测试要确定性锚位）
            body->attrs.erase(
                std::remove_if(body->attrs.begin(), body->attrs.end(),
                               [&](const AttrSlot& s) {
                                   return cf.utf8(s.nameCp) == "StackMapTable";
                               }),
                body->attrs.end());
            auto attr = std::make_unique<StackMapTableAttr>();
            attr->frames = frames;
            body->attrs.push_back(
                AttrSlot::makeNew(cf.cp.addUtf8("StackMapTable"), std::move(attr)));
            done = true;
        }
    }
    if (!done) throw std::runtime_error("SMT 夹具缺 clinit Code");
    auto encoded = jbc::core::writeClass(cf);
    if (encoded.is_err()) throw std::runtime_error("SMT 夹具回写失败");
    return std::move(encoded).unwrap();
}

/// @brief clinit SMT 读回视图（帧绝对 pc + 原始 type/offset，重排断言用）。
struct SmtView
{
    std::vector<ca::u32> pcs;
    std::vector<ca::u8>  types;
    std::vector<ca::u32> offsets;
};

SmtView readClinitSmt(const std::vector<ca::u8>& bytes)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (loaded.is_err()) throw std::runtime_error("SMT 读回重解析失败");
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    SmtView out;
    for (jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) != "<clinit>") continue;
        for (jbc::core::AttrSlot& slot : m.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) throw std::runtime_error("SMT 读回 Code 解不开");
            if (!ca::core::tag_cast::isa<jbc::core::CodeAttr>(d.unwrap())) {
                throw std::runtime_error("SMT 读回 Code 形不符");
            }
            auto* body = ca::core::tag_cast::cast<jbc::core::CodeAttr>(d.unwrap());
            if (body == nullptr) throw std::runtime_error("SMT 读回 Code 空指针");
            for (jbc::core::AttrSlot& s2 : body->attrs) {
                if (cf.utf8(s2.nameCp) != "StackMapTable") continue;
                auto d2 = cf.decodeAttr(s2);
                if (d2.is_err()) throw std::runtime_error("SMT 读回解不开");
                if (!ca::core::tag_cast::isa<jbc::core::StackMapTableAttr>(d2.unwrap())) {
                    throw std::runtime_error("SMT 读回形不符");
                }
                auto* smt = ca::core::tag_cast::cast<jbc::core::StackMapTableAttr>(d2.unwrap());
                if (smt == nullptr) throw std::runtime_error("SMT 读回空指针");
                ca::i64 prev = -1;
                for (const auto& f : smt->frames) {
                    ca::u32 delta = 0;
                    if (f.type <= jbc::core::StackMapTableAttr::Frame::kSameMax) {
                        delta = f.type;
                    } else if (f.type >= jbc::core::StackMapTableAttr::Frame::kSame1Base
                               && f.type <= jbc::core::StackMapTableAttr::Frame::kSame1Max) {
                        delta = f.type - jbc::core::StackMapTableAttr::Frame::kSame1Base;
                    } else {
                        delta = f.offset;
                    }
                    const ca::u32 pc = (prev < 0)
                                           ? delta
                                           : static_cast<ca::u32>(prev) + delta + 1;
                    out.pcs.push_back(pc);
                    out.types.push_back(f.type);
                    out.offsets.push_back(f.offset);
                    prev = pc;
                }
            }
        }
    }
    return out;
}

/// same_frame 帧构造（type 即 delta，≤63）。
jbc::core::StackMapTableAttr::Frame sameFrame(ca::u32 delta)
{
    jbc::core::StackMapTableAttr::Frame f;
    f.type = static_cast<ca::u8>(delta);
    return f;
}

/// same_frame_extended 帧构造（type 251，offset 即 delta）。
jbc::core::StackMapTableAttr::Frame sameExtFrame(ca::u32 delta)
{
    jbc::core::StackMapTableAttr::Frame f;
    f.type   = jbc::core::StackMapTableAttr::Frame::kSameExt;
    f.offset = delta;
    return f;
}

// ============================================================================
// ==== 测试①：混合类全收 ======================================================
// ============================================================================

TEST(JunkMemberStripScan, MixedJunkAndRealInitFullyCollected)
{
    model::ResourceContainer container;
    addClassBytes(container, "Host",
                  assembleJunkShaped("Host", mixedHostSource(),
                                     {{"qflda", "$ga11"},
                                      {"qfldb", "$gb22"},
                                      {"qfldi", "$gp33"},
                                      {"qfldp", "4abcD"},
                                      {"qmeth1", "$gg111"}}));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    EXPECT_EQ(scanned.classes[0].owner, "Host");
    ASSERT_EQ(scanned.classes[0].regions.size(), 1u)
        << "clinit junk 键段应收进一个区域";
    EXPECT_EQ(scanned.classes[0].methodIdxs.size(), 1u);
    EXPECT_EQ(scanned.classes[0].fieldIdxs.size(), 4u);
    EXPECT_EQ(scanned.methodsSimFail, 0u);
    EXPECT_EQ(scanned.methodsFailClosed, 0u);
    ASSERT_FALSE(scanned.classes[0].regions[0].spans.empty());
    // 区域从真实前缀之后起（G2），一直铺到保留尾 return 前
    EXPECT_GT(scanned.classes[0].regions[0].spans.front().first, 0u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u) << "探针方法删";
    EXPECT_EQ(dirty.fields, 4u) << "三个 $g 形字段 + 数字开头池字段删";
    EXPECT_EQ(dirty.rejectedByRef, 0u);
    EXPECT_GE(dirty.nopBytes, 1u);
    EXPECT_EQ(dirty.residualMethods, 0u);
    EXPECT_EQ(dirty.residualFields, 0u);

    const std::string after = disassemble(classBytes(container, "Host.class"));
    EXPECT_EQ(after.find("$ga11"), std::string::npos) << after;
    EXPECT_EQ(after.find("$gb22"), std::string::npos) << after;
    EXPECT_EQ(after.find("$gp33"), std::string::npos) << after;
    EXPECT_EQ(after.find("4abcD"), std::string::npos) << after;
    EXPECT_EQ(after.find("$gg11"), std::string::npos) << after;
    // junk 键算术随 NOP 化消失（全类只有键段用 lsub/lxor）
    EXPECT_EQ(after.find("lsub"), std::string::npos) << after;
    EXPECT_EQ(after.find("lxor"), std::string::npos) << after;
    // 真实初始化保真：真实池字段、真实初始化调用、公开入口全在
    EXPECT_NE(after.find("realPool"), std::string::npos) << after;
    EXPECT_NE(after.find("realInit"), std::string::npos) << after;
    EXPECT_NE(after.find("java/lang/Math.max"), std::string::npos) << after;

    const ProofResult verdict = proof(container, dirty, 1u);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

// ============================================================================
// ==== 测试②③：活引用 fail-closed ============================================
// ============================================================================

TEST(JunkMemberStripFailClosed, RealCallerKeepsJunkMethod)
{
    // 真实方法整段白名单 + 纯探针调用点（无 junk 字段种子，G3）：不构成区域
    // ——调用者方法体保真，被调探针靠引用普查宁留；零引用探针照删（对照）。
    model::ResourceContainer container;
    addClassBytes(container, "Host2",
                  assembleJunkShaped("Host2", callerHostSource(),
                                     {{"qmeth1", "$gg111"}, {"qmeth2", "$gg222"}}));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    EXPECT_TRUE(scanned.classes[0].regions.empty()) << "纯调用点不构成区域";
    EXPECT_EQ(scanned.classes[0].methodIdxs.size(), 2u) << "两个探针都是名字形候选";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u) << "零引用探针删，活引用探针宁留";
    EXPECT_EQ(dirty.rejectedByRef, 1u);
    EXPECT_EQ(dirty.fields, 0u);

    const std::string after = disassemble(classBytes(container, "Host2.class"));
    EXPECT_NE(after.find("$gg11"), std::string::npos) << after;
    EXPECT_EQ(after.find("$gg22"), std::string::npos) << after;
    // 调用点保真：call 体里的 invokestatic 还在
    EXPECT_NE(after.find("invokestatic"), std::string::npos) << after;
}

TEST(JunkMemberStripFailClosed, RealReaderKeepsJunkField)
{
    // 真实代码读 junk 字段：读值经 lreturn 流出（lreturn 白名单外锚），闭包
    // 缩到读点自身且从方法入口起（G2）→ 整方法放弃，字段靠引用普查宁留。
    model::ResourceContainer container;
    addClassBytes(container, "Host3",
                  assembleJunkShaped("Host3", readerHostSource(), {{"qflda", "$ga11"}}));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    EXPECT_TRUE(scanned.classes[0].regions.empty());
    EXPECT_GE(scanned.methodsFailClosed, 1u) << "读值流出区域 → 整方法放弃";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.fields, 0u);
    EXPECT_EQ(dirty.rejectedByRef, 1u);

    const std::string after = disassemble(classBytes(container, "Host3.class"));
    EXPECT_NE(after.find("$ga11"), std::string::npos) << after;
    EXPECT_NE(after.find("lreturn"), std::string::npos) << after << "真实读者方法体保真";
}

// ============================================================================
// ==== 测试④：白名单外锚 / 模拟不出 → 整方法放弃 =============================
// ============================================================================

TEST(JunkMemberStripFailClosed, WhitelistAnchorAbandonsMethod)
{
    // 锚（new+pop 自弃分配）截断馈送回拉链：闭包缺 ldc2_w 馈送前缀，改写后
    // 保留 return 槽深非零（判 11）→ 整方法放弃、类不落盘、字节零变化。
    model::ResourceContainer container;
    addClassBytes(container, "Host4",
                  assembleJunkShaped("Host4", anchoredHostSource(), {{"qflda", "$ga11"}}));
    const std::vector<ca::u8> before = classBytes(container, "Host4.class");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    EXPECT_TRUE(scanned.classes[0].regions.empty());
    EXPECT_GE(scanned.methodsFailClosed, 1u) << "改写后 return 槽深非零 → 整方法放弃";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.regions, 0u);
    EXPECT_EQ(dirty.methods, 0u);
    EXPECT_EQ(dirty.fields, 0u);
    EXPECT_EQ(dirty.rejectedByRef, 1u) << "junk 字段引用还活着（clinit 未 NOP），宁留";
    EXPECT_FALSE(containsUri(dirty, "Host4.class")) << "整方法放弃的类不落盘";
    EXPECT_TRUE(before == classBytes(container, "Host4.class")) << "字节零变化";

    const std::string after = disassemble(classBytes(container, "Host4.class"));
    EXPECT_NE(after.find("$ga11"), std::string::npos) << after << "clinit 键段字节原样";
}

TEST(JunkMemberStripFailClosed, UnsimulatableClinitAbandonsMethod)
{
    // 栈效应模拟不出（字节级注入不平衡体：第二次 pop 槽位下溢；jasm 汇编器
    // 拒绝不平衡源码，只能程序化注入）：整方法放弃、类不落盘、字节零变化。
    std::vector<ca::u8> bytes
        = assembleJunkShaped("Host5", simFailHostSource(), {{"qflda", "$ga11"}});
    {
        auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
        ASSERT_FALSE(loaded.is_err());
        auto                  session = std::move(loaded).unwrap();
        jbc::core::ClassFile& cf      = session->backing();
        bool done = false;
        for (jbc::core::MemberInfo& m : cf.methods) {
            if (cf.utf8(m.nameCp) != "<clinit>") continue;
            for (jbc::core::AttrSlot& slot : m.attrs) {
                if (cf.utf8(slot.nameCp) != "Code") continue;
            auto d = cf.decodeAttr(slot);
            ASSERT_FALSE(d.is_err());
            ASSERT_TRUE(ca::core::tag_cast::isa<jbc::core::CodeAttr>(slot.mutate()));
            auto* body = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            ASSERT_NE(body, nullptr);
                body->code.assign({0x10, 0x04, 0x57, 0x57, 0xb1});   // bipush 4; pop; pop; return
                done = true;
            }
        }
        ASSERT_TRUE(done);
        auto encoded = jbc::core::writeClass(cf);
        ASSERT_FALSE(encoded.is_err());
        bytes = std::move(encoded).unwrap();
    }

    model::ResourceContainer container;
    addClassBytes(container, "Host5", bytes);
    const std::vector<ca::u8> before = classBytes(container, "Host5.class");

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    EXPECT_GE(scanned.methodsSimFail, 1u) << "栈模拟不出的 clinit 整方法放弃";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.regions, 0u);
    EXPECT_EQ(dirty.fields, 0u);
    EXPECT_EQ(dirty.rejectedByRef, 1u) << "junk 字段有真实读者（read），宁留";
    EXPECT_FALSE(containsUri(dirty, "Host5.class")) << "放弃方法不该带动类落盘";
    EXPECT_TRUE(before == classBytes(container, "Host5.class")) << "字节零变化";
}

// ============================================================================
// ==== 测试⑤：干净类零搅动 ====================================================
// ============================================================================

TEST(JunkMemberStripClean, CleanClassUntouched)
{
    model::ResourceContainer container;
    addClassBytes(container, "Host", assemble("Host", cleanHostSource()));
    const std::vector<ca::u8> before = classBytes(container, "Host.class");

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.classes.empty()) << "无 junk 形成员的类不进候选";

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty()) << "干净类不进 dirty";
    EXPECT_EQ(dirty.methods, 0u);
    EXPECT_EQ(dirty.fields, 0u);
    EXPECT_EQ(dirty.regions, 0u);
    EXPECT_EQ(dirty.attempted, 0u);
    EXPECT_EQ(dirty.residualMethods, 0u);
    EXPECT_EQ(dirty.residualFields, 0u);
    EXPECT_TRUE(before == classBytes(container, "Host.class")) << "字节零变化";

    const ProofResult verdict = proof(container, dirty, 1u);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

// ============================================================================
// ==== 测试⑦：夹层真实块 + SMT 帧摘除/重排 ====================================
// ============================================================================

TEST(JunkMemberStripScan, MidRegionKeepsRealBlockAndRearrangesSmt)
{
    // 夹层真实块形（枚举初始化同构）：区域分两段、真实块保留并照常执行；
    // 合成 SMT 帧：pc 0（区域外，保留）、区域内首指令（摘）、真实块首指令
    // （保留 + 跨被摘帧重排 delta，超 63 升格 same_frame_extended）、保留尾
    // return（闭包内，摘）。
    model::ResourceContainer container;
    addClassBytes(container, "Host8",
                  assembleJunkShaped("Host8", midRegionHostSource(),
                                     {{"qflda", "$ga11"},
                                      {"qfldi", "$gp33"},
                                      {"qfldp", "4abcD"},
                                      {"qmeth1", "$gg111"}}));

    // 第一轮 scan 拿区域段位（纯读），按段位锚合成帧后重挂重扫。
    const ScanResult probed = scan(container);
    ASSERT_EQ(probed.classes.size(), 1u);
    ASSERT_EQ(probed.classes[0].regions.size(), 1u);
    const auto& spans = probed.classes[0].regions[0].spans;
    ASSERT_EQ(spans.size(), 2u) << "夹层真实块把区域分成两段";
    const ca::u32 aPc = spans[0].first;      // 区域首指令（段内，帧该摘）
    const ca::u32 bPc = spans[0].second;     // 夹层真实块首指令（帧该留）
    const ca::u32 rPc = spans[1].second;     // 保留尾 return（段外，帧该留）
    EXPECT_GT(bPc, 64u) << "nop 串该把真实块首指令 pc 压过 64，升格形才有分辨力";

    std::vector<jbc::core::StackMapTableAttr::Frame> frames;
    frames.push_back(sameFrame(0));                                  // pc 0
    frames.push_back(sameFrame(aPc - 1));                            // pc aPc
    frames.push_back(sameExtFrame(bPc - aPc - 1));                   // pc bPc
    frames.push_back(sameFrame(rPc - bPc - 1));                      // pc rPc
    const std::vector<ca::u8> patched = attachSyntheticClinitSmt(
        classBytes(container, "Host8.class"), frames);
    container.replaceResource(
        std::string("Host8.class"),
        std::make_unique<model::ClassResource>(std::string("Host8.class"), patched));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u);
    ASSERT_EQ(scanned.classes[0].regions.size(), 1u);
    ASSERT_EQ(scanned.classes[0].regions[0].droppedFramePcs.size(), 1u)
        << "只有段内帧该摘；真实块首与保留尾 return 的帧原样保留";
    EXPECT_EQ(scanned.classes[0].regions[0].droppedFramePcs[0], aPc);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u);
    EXPECT_EQ(dirty.fields, 3u);
    EXPECT_EQ(dirty.rejectedByRef, 0u);

    const SmtView smt = readClinitSmt(classBytes(container, "Host8.class"));
    ASSERT_EQ(smt.pcs.size(), 3u) << "段内一帧摘、段外三帧留";
    EXPECT_EQ(smt.pcs[0], 0u);
    EXPECT_EQ(smt.types[0], 0u);
    EXPECT_EQ(smt.pcs[1], bPc);
    EXPECT_EQ(smt.types[1], jbc::core::StackMapTableAttr::Frame::kSameExt)
        << "pc 0 之后帧 delta = bPc - 1 > 63 → 升格 same_frame_extended";
    EXPECT_EQ(smt.offsets[1], bPc - 1u);
    EXPECT_EQ(smt.pcs[2], rPc);
    EXPECT_EQ(smt.types[2], static_cast<ca::u8>(rPc - bPc - 1u))
        << "跨被摘帧重排后 delta = rPc - bPc - 1 ≤ 63，same_frame 原形重编码";

    // 夹层真实块照常执行（指令保留），junk 算术随 NOP 化消失
    const std::string after = disassemble(classBytes(container, "Host8.class"));
    EXPECT_NE(after.find("java/lang/Object.<init>"), std::string::npos) << after;
    EXPECT_EQ(after.find("lsub"), std::string::npos) << after;
    EXPECT_EQ(after.find("ladd"), std::string::npos) << after;
    EXPECT_EQ(after.find("$ga11"), std::string::npos) << after;

    const ProofResult verdict = proof(container, dirty, 1u);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());
}

// ============================================================================
// ==== 测试⑨：DeadMember 空壳回收接力 =========================================
// ============================================================================

TEST(JunkMemberStripPipeline, HandsEmptyCompanionToDeadMember)
{
    // 宿主 junk 键段调探针、探针体调伴生类 `Comp.helper`：本 pass 删净宿主
    // junk 成员（探针删除带走伴生类的最后引用），Comp 以「成员全零引用」
    // 状态成为既有 grunt.DeadMember 的空壳回收候选，整类摘除接力成立。
    model::ResourceContainer container;
    addClassBytes(container, "Host9",
                  assembleJunkShaped("Host9", handoffHostSource(),
                                     {{"qflda", "$ga11"}, {"qmeth1", "$gg111"}}));
    addClassBytes(container, "Comp", assemble("Comp", companionSource()));

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.classes.size(), 1u) << "Comp 无 junk 形成员，不进本 pass 候选";
    ASSERT_EQ(scanned.classes[0].regions.size(), 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.methods, 1u) << "探针删除（调用点在键段内，NOP 化后零引用）";
    EXPECT_EQ(dirty.fields, 1u);
    // 普查先于删除：Comp.helper 在探针删除前还活着，本 pass 不摘它
    EXPECT_TRUE(container.hasResource("Comp.class"));

    const std::string after = disassemble(classBytes(container, "Host9.class"));
    EXPECT_EQ(after.find("$gg11"), std::string::npos) << after;
    EXPECT_EQ(after.find("$ga11"), std::string::npos) << after;
    EXPECT_NE(after.find("realPool"), std::string::npos) << after;

    // 接力：DeadMember 收 Comp.helper 为候选，空壳整类摘除；宿主保全。
    const grunt::dead_member::ScanResult dmScan = grunt::dead_member::scan(container);
    bool helperCandidate = false;
    for (const grunt::dead_member::ClassCand& c : dmScan.classes) {
        if (c.owner != "Comp") continue;
        for (const grunt::dead_member::MethodCand& m : c.methods) {
            helperCandidate |= (m.name == "helper");
        }
    }
    EXPECT_TRUE(helperCandidate) << "删净成员后 Comp.helper 应成为 DeadMember 候选";

    const grunt::dead_member::DirtySet dmDirty
        = grunt::dead_member::rewrite(container, dmScan);
    EXPECT_GE(dmDirty.classes, 1u);
    EXPECT_FALSE(container.hasResource("Comp.class")) << "伴生空壳整类摘除";
    EXPECT_TRUE(container.hasResource("Host9.class")) << "宿主类保全";
}

// ============================================================================
// ==== 测试⑥：真实样本回归（dec_jdk.jar 全量）=================================
// ============================================================================

TEST(JunkMemberStripReal, DecJdkFullContainerRegression)
{
    const char*    env     = std::getenv("GRUNT_JMS_REAL_JAR");
    const std::string jarPath = (env != nullptr)
                                    ? env
                                    : "C:/sw/grunt-eval/target-jsoup/_bcx/verify_fix/dec_jdk.jar";
    {
        std::FILE* probe = std::fopen(jarPath.c_str(), "rb");
        if (probe == nullptr) GTEST_SKIP() << "真实样本 jar 不在位：" << jarPath;
        std::fclose(probe);
    }

    model::ResourceContainer container;
    mjbase::jar::JarReader   reader(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(jarPath.data()), jarPath.size()));
    ca::usize loaded = 0;
    auto read = reader.read(
        [&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
            const std::string n = name.to_std_string();
            if (n.size() < 6 || n.compare(n.size() - 6, 6, ".class") != 0) return;
            if (container.hasResource(n)) return;
            container.addResource(n, std::make_unique<model::ClassResource>(n, data));
            ++loaded;
        });
    ASSERT_TRUE(read.is_ok()) << "jar 读取失败";
    EXPECT_EQ(loaded, 655u);

    const ca::usize before = 655u;
    const ScanResult scanned = scan(container);
    EXPECT_EQ(scanned.classesSkipped, 0u) << "真实类全量可解析";
    EXPECT_EQ(scanned.classes.size(), 260u) << "junk 承载类全数进候选";
    // 普查口径（2026-11-22 实证）：260 个 $g 探针方法 + 1081 junk 字段
    // （1040 $g 形 + 41 数字开头池字段），全部进候选。
    ca::usize candMethods = 0;
    ca::usize candFields  = 0;
    for (const ClassCand& c : scanned.classes) {
        candMethods += c.methodIdxs.size();
        candFields += c.fieldIdxs.size();
    }
    EXPECT_EQ(candMethods, 260u);
    EXPECT_EQ(candFields, 1081u);

    const DirtySet dirty = rewrite(container, scanned);
    std::printf("[jms-real] regions=%zu nopBytes=%zu athrows=%zu methods=%zu fields=%zu "
                "rejected=%zu residual=%zu/%zu compacted=%zu attempted=%zu\n",
                dirty.regions, dirty.nopBytes, dirty.athrows, dirty.methods, dirty.fields,
                dirty.rejectedByRef, dirty.residualMethods, dirty.residualFields,
                dirty.compacted, dirty.attempted);
    // 实测口径（本实现，JVM 全量加载 + 14 语料对拍验收通过）：8/260 探针进
    // 删除面、32/1081 字段进删除面。与四步原型（278 区域 / 53 探针 / 212 字
    // 段）的差额全部落在保守面：本实现多四道门（G1 junk 命名方法不做区域宿
    // 主 / G2 区域不得从入口起 / G3 区域须有 junk 字段种子 / G4 馈送链净效
    // 应闸）、保留尾 return 及其陷阱尾原样保留（locals 基线不被摘帧破坏，
    // JVM "Instruction type does not match stack map" / "falls through code
    // end" 实证）、帧面五道核对（F1-F4）——删除集是原型的子集，宁留方向。
    EXPECT_EQ(dirty.methods, 8u);
    EXPECT_EQ(dirty.fields, 32u);
    EXPECT_EQ(dirty.attempted, 1341u);
    EXPECT_EQ(dirty.rejectedByRef, 1341u - 8u - 32u);
    EXPECT_EQ(dirty.residualMethods, 252u);
    EXPECT_EQ(dirty.residualFields, 1049u);
    EXPECT_GE(dirty.regions, 1u);
    EXPECT_GE(dirty.nopBytes, 1u);

    const ProofResult verdict = proof(container, dirty, before);
    EXPECT_TRUE(verdict.ok) << (verdict.failed.empty() ? "" : verdict.failed.front());

    // 代表类抽查：org/demo/App（后缀形键段，init+clinit 双区域全判定）junk
    // 成员删净。
    const std::string app = disassemble(classBytes(container, "org/demo/App.class"));
    EXPECT_EQ(app.find("$g"), std::string::npos) << "App 的 junk 形成员应删净\n" << app;

    // 联动抽查：删净成员后伴生类（$PredicateProcessor$/$KeyProcessor$ 形）
    // 成为既有 grunt.DeadMember 的空壳回收候选。
    const grunt::dead_member::ScanResult dmScan = grunt::dead_member::scan(container);
    ca::usize companionCandidates = 0;
    for (const grunt::dead_member::ClassCand& c : dmScan.classes) {
        if (c.owner.find("$PredicateProcessor$") != std::string::npos
            || c.owner.find("$KeyProcessor$") != std::string::npos) {
            ++companionCandidates;
        }
    }
    EXPECT_GE(companionCandidates, 1u) << "伴生空壳应进 DeadMember 删除面";

    // GRUNT_JMS_OUT_JAR：把改写后容器写成 jar，供 JVM 侧全量加载 + 对拍
    // 验收（验收脚本在仓外 grunt-eval；缺省不落盘）。
    if (const char* outJar = std::getenv("GRUNT_JMS_OUT_JAR")) {
        mjbase::jar::JarWriter writer(ca::str::Utf8StringRef::from_data(
            reinterpret_cast<const ca::u8*>(outJar), std::strlen(outJar)));
        auto wres = writer.write([&](mjbase::jar::IEntryWriter& ew) {
            grunt::forEachClass(container, [&](const grunt::ClassEntry& entry) {
                ew.writeEntry(
                    ca::str::Utf8StringRef::from_data(
                        reinterpret_cast<const ca::u8*>(entry.uri.data()), entry.uri.size()),
                    *entry.bytes);
            });
        });
        ASSERT_TRUE(wres.is_ok()) << "改写 jar 写出失败";
    }
}

}   // namespace
}   // namespace grunt::junk_member_strip
