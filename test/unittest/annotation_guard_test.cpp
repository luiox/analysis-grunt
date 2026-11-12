/// @file        annotation_guard_test.cpp
/// @brief       注解族深度守卫接线测试（mjbase #158 随升接线）。
///
/// libjbc attr_codec 注解族解码互递归无界，敌意深嵌注解（约 3000 层 '@'）
/// 会在 decodeAttr 内栈溢出（宿主进程不可恢复）。grunt 侧 decodeAttr 直调
/// 位已在读注解入口接 `mjbase::util::attrSafeToDecode` 预检。本测试以原始
/// 字节夹具（jasm 表达不了千层嵌套）钉住接线口径：
///   1. 深嵌敌意注解类过 BoogerAnnoStrip / DeadMember 两 pass 不炸，按
///      解不开原样保真跳过，类字节不动；
///   2. 敌意类在场不牵连同容器普通类的正常清理；
///   3. 合法浅嵌注解不受守卫影响（fail-open 回归）：@Booger 照常普查、
///      照常剥离。
#include "grunt/booger_anno_strip.hpp"
#include "grunt/container.hpp"
#include "grunt/dead_member.hpp"

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grunt::annotation_guard {
namespace {

namespace model  = mjbase::model;
namespace booger = grunt::booger_anno_strip;

void putU2(std::vector<ca::u8>& out, ca::u16 v)
{
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

void putU4(std::vector<ca::u8>& out, ca::u32 v)
{
    out.push_back(static_cast<ca::u8>(v >> 24));
    out.push_back(static_cast<ca::u8>(v >> 16));
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

/// @brief 常量池装配器：下标从 1 起，槽 0 无效。
struct CpBuilder
{
    std::vector<ca::u8> bytes;
    ca::u16             next = 1;

    ca::u16 utf8(std::string_view s)
    {
        const ca::u16 idx = next++;
        bytes.push_back(1);
        putU2(bytes, static_cast<ca::u16>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
        return idx;
    }

    ca::u16 clazz(ca::u16 nameIdx)
    {
        const ca::u16 idx = next++;
        bytes.push_back(7);
        putU2(bytes, nameIdx);
        return idx;
    }
};

/// @brief 最小 classfile（V1_8）：this=T、super=java/lang/Object、零成员，
/// 单个类级属性（attrName + 原始载荷）。extraCp 先于属性名落入池，载荷里
/// 的下标引用它。
std::vector<ca::u8> classWithAttr(std::string_view                           attrName,
                                  const std::function<void(CpBuilder&)>&     extraCp,
                                  const std::vector<ca::u8>&                 payload)
{
    CpBuilder    cp;
    const ca::u16 nameT      = cp.utf8("T");
    const ca::u16 classT     = cp.clazz(nameT);
    const ca::u16 nameObject = cp.utf8("java/lang/Object");
    const ca::u16 classObj   = cp.clazz(nameObject);
    if (extraCp) extraCp(cp);
    const ca::u16 attrNameIdx = cp.utf8(attrName);

    std::vector<ca::u8> out;
    putU4(out, 0xCAFEBABEu);
    putU2(out, 0);
    putU2(out, 0x34);
    putU2(out, cp.next);                               // cpCount（槽 0 无效）
    out.insert(out.end(), cp.bytes.begin(), cp.bytes.end());
    putU2(out, 0x0021);                                // public super
    putU2(out, classT);
    putU2(out, classObj);
    putU2(out, 0);                                     // interfaces
    putU2(out, 0);                                     // fields
    putU2(out, 0);                                     // methods
    putU2(out, 1);                                     // class attrs = 1
    putU2(out, attrNameIdx);
    putU4(out, static_cast<ca::u32>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// @brief Runtime{Visible,Invisible}Annotations 载荷：1 条注解，单 pair，
/// 元素值 '@' 连嵌 depth 层后以 's' 叶收尾。CP 下标一律落槽 idx（预检
/// 走查不校验 CP，守卫缺席时才会真解码）。
std::vector<ca::u8> nestedAnnoPayload(ca::u16 idx, ca::u32 depth)
{
    std::vector<ca::u8> p;
    putU2(p, 1);          // num_annotations
    putU2(p, idx);        // type_index
    putU2(p, 1);          // num_pairs
    for (ca::u32 i = 0; i < depth; ++i) {
        putU2(p, idx);    // element_name
        p.push_back('@'); // nested annotation
        putU2(p, idx);    // type_index
        putU2(p, 1);      // num_pairs
    }
    putU2(p, idx);        // element_name（叶）
    p.push_back('s');     // string 值
    putU2(p, idx);        // const_value_index
    return p;
}

/// @brief 浅层合法 @Booger 注解载荷：1 条注解，单 pair，'s' 叶。
std::vector<ca::u8> boogerAnnoPayload(ca::u16 annoDescIdx, ca::u16 nameIdx, ca::u16 valueIdx)
{
    std::vector<ca::u8> p;
    putU2(p, 1);              // num_annotations
    putU2(p, annoDescIdx);    // type_index = Ldev/realme/Booger;
    putU2(p, 1);              // num_pairs
    putU2(p, nameIdx);        // element_name
    p.push_back('s');
    putU2(p, valueIdx);
    return p;
}

model::ResourceContainer makeRawContainer(
    std::initializer_list<std::pair<std::string, std::vector<ca::u8>>> classes)
{
    model::ResourceContainer container;
    for (const auto& [name, bytes] : classes) {
        const std::string uri = name + ".class";
        container.addResource(uri, std::make_unique<model::ClassResource>(uri, bytes));
    }
    return container;
}

const std::vector<ca::u8>& classBytes(const model::ResourceContainer& container,
                                      const std::string&              uri)
{
    const model::IResource* raw = container.getResource(uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具缺类：" + uri);
    return cls->get();
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief jasm 现场汇编（夹具不落二进制，沿用各残留测试同一约定）。
std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto                         assembled = service.assemble(utf16(name + ".jasm").ref(),
                                                              utf16(source).ref());
    if (!assembled.isOk()) throw std::runtime_error("jasm 夹具汇编失败：" + name);
    return std::move(assembled.value->bytes);
}

std::string disassemble(const std::vector<ca::u8>& bytes)
{
    mjbase::jasm::JvmJasmService service;
    auto                         result = service.disassemble(bytes);
    if (!result.isOk()) throw std::runtime_error("产物反汇编失败");
    return result.value->source.to_utf8_string().to_std_string();
}

/// 普通类：私有 secret 无任何 CP 引用（DeadMember 删除候选形状），公开
/// 入口走 Math.max 不触碰 secret。
std::string plainSource()
{
    return ".super java/lang/Object\n"
           ".class public super Plain {\n"
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

}   // namespace

// ============================================================================
// ==== 敌意深嵌注解：守卫拒绝，pass 面不栈溢出、原样保真 =======================
// ============================================================================

TEST(AnnotationGuardHostile, DeepNestedAnnoClassSurvivesPassesUntouched)
{
    // 5000 层 '@' 嵌套（远超 kMaxAnnotationNesting=255，也远超无守卫时
    // decodeAttr 的栈预算）：守卫缺席时本测试进程直接栈溢出，在场时整槽
    // fail-open 跳过。CP 布局固定：属性名恰是第 5 槽，载荷下标一律 #5
    //（本测试不解码，下标无需有效）。
    const std::vector<ca::u8> hostile =
        classWithAttr("RuntimeInvisibleAnnotations", {}, nestedAnnoPayload(5, 5000));

    model::ResourceContainer container = makeRawContainer({{"Hostile", hostile}});
    const std::vector<ca::u8> original = classBytes(container, "Hostile.class");

    // BoogerAnnoStrip：scan 不计站点（解不开 → 不见注解），rewrite 不动字节。
    const booger::ScanResult scanned = booger::scan(container);
    EXPECT_TRUE(scanned.sites.empty()) << "敌意注解槽该被守卫拦下，不计剥离站点";
    EXPECT_FALSE(scanned.gateBlocked);
    const booger::DirtySet dirty = booger::rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());
    EXPECT_EQ(classBytes(container, "Hostile.class"), original) << "敌意类字节必须原样保真";

    // DeadMember：注解字符串普查同一入口，同样不得炸；敌意类零成员，其
    // ClassCand 若在场也不该带任何删除候选。
    const grunt::dead_member::ScanResult dmScanned = grunt::dead_member::scan(container);
    for (const auto& c : dmScanned.classes) {
        if (c.uri != "Hostile.class") continue;
        EXPECT_TRUE(c.methods.empty() && c.fields.empty())
            << "敌意类零成员，不该带任何删除候选";
    }
}

TEST(AnnotationGuardHostile, HostileClassDoesNotBlockPlainClassCleanup)
{
    const std::vector<ca::u8> hostile =
        classWithAttr("RuntimeVisibleAnnotations", {}, nestedAnnoPayload(5, 5000));

    // 同容器塞一个 jasm 现场汇编的普通类（私有 secret 无引用，照常删）。
    model::ResourceContainer container = makeRawContainer({{"Hostile", hostile}});
    container.addResource("Plain.class",
                          std::make_unique<model::ClassResource>(
                              "Plain.class", assemble("Plain", plainSource())));

    const grunt::dead_member::ScanResult scanned = grunt::dead_member::scan(container);
    bool secretCandidate = false;
    for (const auto& c : scanned.classes) {
        for (const auto& m : c.methods) secretCandidate |= (m.name == "secret");
    }
    EXPECT_TRUE(secretCandidate) << "同容器普通类的死方法照常成为删除候选";

    const grunt::dead_member::DirtySet dirty = grunt::dead_member::rewrite(container, scanned);
    const std::string after = disassemble(classBytes(container, "Plain.class"));
    EXPECT_EQ(after.find("secret"), std::string::npos) << after;
}

// ============================================================================
// ==== fail-open 回归：合法浅嵌注解不受守卫影响 ================================
// ============================================================================

TEST(AnnotationGuardFailOpen, ShallowBoogerAnnoStillScannedAndStripped)
{
    // CP 布局：#1..#4 由 classWithAttr 固定占用，extraCp 先于属性名入池，
    // 故 #5 = 注解描述符、#6 = 键名、#7 = 值串；载荷下标按此硬编码。
    const std::vector<ca::u8> booged = classWithAttr(
        "RuntimeInvisibleAnnotations",
        [](CpBuilder& cp) {
            cp.utf8("Ldev/realme/Booger;");   // #5
            cp.utf8("d2");                    // #6
            cp.utf8("x");                     // #7
        },
        boogerAnnoPayload(5, 6, 7));

    model::ResourceContainer container = makeRawContainer({{"Booged", booged}});

    const booger::ScanResult scanned = booger::scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u) << "守卫不得拦合法浅嵌注解";
    EXPECT_EQ(scanned.sites[0].annoCount, 1u);

    const booger::DirtySet dirty = booger::rewrite(container, scanned);
    ASSERT_EQ(dirty.uris.size(), 1u);
    EXPECT_EQ(dirty.annos, 1u);

    const booger::ScanResult rescanned = booger::scan(container);
    EXPECT_TRUE(rescanned.sites.empty()) << "@Booger 条目已剥除，再扫归零";
}

}   // namespace grunt::annotation_guard
