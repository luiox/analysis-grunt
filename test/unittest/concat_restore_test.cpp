/// @file        concat_restore_test.cpp
/// @brief       concat_restore 夹具与验收：纯 jasm 构造包装 BSM（包装方法 +
///              dummy 方法 indy 站点，indy 语法携带 BSM 句柄与静态参数），
///              逐字节形状还原 + recipe 回填 + fail-closed 反例（形状不符 /
///              常量数不齐）。
#include "grunt/concat_restore.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"

#include "jbc/core/writer.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/scan_toolbox.hpp"

namespace grunt::concat_restore {
namespace {

namespace model = mjbase::model;

constexpr const char* kConcatBsmDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/String;)"
    "Ljava/lang/invoke/CallSite;";

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    auto assembled = service.assemble(utf16(name + ".jasm").ref(), utf16(source).ref());
    if (!assembled.isOk()) {
        std::string detail;
        for (const auto& d : assembled.diagnostics) {
            detail += "\n  " + d.message.to_utf8_string().to_std_string();
        }
        throw std::runtime_error("jasm 夹具汇编失败：" + name + detail);
    }
    return std::move(assembled.value->bytes);
}

/// @brief 宿主：包装方法（转发体：aload×4 → iconst_N; anewarray → N×(dup;
///        iconst_i; ldc; aastore) → invokestatic makeConcatWithConstants 五参形
///        → areturn）+ dummy 方法 indy 站点（BSM 静态参数 = tag 串）。
///        malformed 在转发体 invokestatic 前注一条 pop 破坏形状（fail-closed 反例）。
std::vector<ca::u8> hostBytes(const std::string&              name,
                              const std::string&              tags,
                              const std::vector<std::string>& constants,
                              bool                            malformed = false)
{
    std::string arrInit;
    for (ca::u32 k = 0; k < constants.size(); ++k) {
        arrInit += std::string("      dup\n      iconst_") + std::to_string(k)
                   + "\n      ldc \"" + constants[k] + "\"\n      aastore\n";
    }
    std::string src;
    src += ".super java/lang/Object\n.class public super " + name + " {\n";
    src += "  .method private static wrapBsm (Ljava/lang/invoke/MethodHandles$Lookup;"
           "Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/String;)"
           "Ljava/lang/invoke/CallSite; {\n    code: {\n";
    src += "      aload 0\n      aload 1\n      aload 2\n      aload 3\n";
    src += "      iconst_" + std::to_string(constants.size())
           + "\n      anewarray java/lang/Object\n";
    src += arrInit;
    if (malformed) src += "      pop\n";
    src += "      invokestatic java/lang/invoke/StringConcatFactory.makeConcatWithConstants"
           " (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
           "Ljava/lang/invoke/MethodType;Ljava/lang/String;[Ljava/lang/Object;)"
           "Ljava/lang/invoke/CallSite;\n      areturn\n    }\n  }\n";
    src += "  .method public dummy ()Ljava/lang/String; {\n    code: {\n";
    src += "      invokedynamic makeConcatWithConstants ()Ljava/lang/String;"
           " { invokestatic, " + name + ".wrapBsm, " + kConcatBsmDesc
           + " } { \"" + tags + "\" }\n";
    src += "      areturn\n    }\n  }\n}\n";
    return assemble(name, src);
}

const std::vector<ca::u8> corruptWrapper(const std::vector<ca::u8>& base)
{
    auto loaded = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView(base));
    if (loaded.is_err()) throw std::runtime_error("corrupt 重解析失败");
    auto                  session = std::move(loaded).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    for (auto& m : cf.methods) {
        if (cf.utf8(m.nameCp) != "wrapBsm") continue;
        for (ca::usize si = 0; si < m.attrs.size(); ++si) {
            if (cf.utf8(m.attrs[si].nameCp) != "Code") continue;
            auto d = cf.decodeAttr(m.attrs[si]);
            if (d.is_err()) throw std::runtime_error("corrupt è§£ç å¤±è´¥");
            const auto* decoded = static_cast<const jbc::core::CodeAttr*>(d.unwrap());
            auto body           = std::make_unique<jbc::core::CodeAttr>();
            body->maxStack      = decoded->maxStack;
            body->maxLocals     = decoded->maxLocals;
            body->code          = decoded->code;
            // invokestatic（3 字节）之前注一条 pop，破坏转发体形状。
            const auto pos = body->code.size() - 4;
            body->code.insert(body->code.begin() + static_cast<ca::i64>(pos), 0x57);
            const ca::u32 nameCp = m.attrs[si].nameCp;
            m.attrs[si] = jbc::core::AttrSlot::makeNew(nameCp, std::move(body));
        }
    }
    auto encoded = jbc::core::writeClass(cf);
    if (encoded.is_err()) throw std::runtime_error("corrupt 回写失败");
    return std::move(encoded).unwrap();
}

void addHost(model::ResourceContainer& container, const std::string& name,
             const std::string& tags, const std::vector<std::string>& constants,
             bool malformed = false)
{
    const std::string uri = name + ".class";
    container.addResource(
        uri, std::make_unique<model::ClassResource>(uri, hostBytes(name, tags, constants, malformed)));
}

const std::vector<ca::u8>& bytesOf(const model::ResourceContainer& container,
                                   const std::string&              uri)
{
    const model::IResource* raw = container.getResource(uri);
    if (raw == nullptr) throw std::runtime_error("夹具缺条目：" + uri);
    const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(raw);
    if (cls == nullptr) throw std::runtime_error("夹具类型不符：" + uri);
    return cls->get();
}

}   // namespace

// ============================================================================
// ==== SCAN / REWRITE / PROOF =================================================
// ============================================================================

TEST(ConcatRestore, RestoresWrapperBsmToStandardForm)
{
    model::ResourceContainer container;
    addHost(container, "Host1", "\x01\x02\x01\x02\x01\x02",
            {". Status=", ", URL=[", "]"});

    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    EXPECT_EQ(scanned.sites.front().mergedRecipe, "\x01. Status=\x01, URL=[\x01]");
    EXPECT_EQ(scanned.wrapperShapeBad, 0u);
    EXPECT_EQ(scanned.recipeMismatch, 0u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_EQ(dirty.classes, 1u);
    EXPECT_EQ(dirty.entries, 1u);
    EXPECT_EQ(dirty.failed, 0u);

    // 产物面：BSM 条目句柄已指 javac 规范五参 varargs 形（CFR 折叠的匹配
    // 依据），参数表只剩回填 recipe。
    auto session = openSession(bytesOf(container, "Host1.class"));
    ASSERT_NE(session, nullptr);
    const jbc::core::ClassFile& cf = session->backing();
    const std::vector<jbc::core::BootstrapMethodsAttr::Entry>* bsms =
        scan_tools::bsmEntries(*session);
    ASSERT_NE(bsms, nullptr);
    ASSERT_EQ(bsms->size(), 1u);
    const jbc::core::ConstantPool::Entry& mh = cf.cp.at(bsms->front().methodRef);
    EXPECT_EQ(mh.tag, jbc::core::CpTag::MethodHandle);
    EXPECT_EQ(mh.a, 6u);
    const jbc::core::ConstantPool::Entry& ref = cf.cp.at(mh.b);
    EXPECT_EQ(ref.tag, jbc::core::CpTag::Methodref);
    const jbc::core::ConstantPool::Entry& nat = cf.cp.at(ref.b);
    ASSERT_EQ(nat.tag, jbc::core::CpTag::NameAndType);
    const jbc::core::ConstantPool::Entry& desc = cf.cp.at(nat.b);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(desc.utf8), desc.utf8Len),
              std::string("(Ljava/lang/invoke/MethodHandles$Lookup;"
                          "Ljava/lang/String;Ljava/lang/invoke/MethodType;"
                          "Ljava/lang/String;[Ljava/lang/Object;)"
                          "Ljava/lang/invoke/CallSite;"));
    ASSERT_EQ(bsms->front().args.size(), 1u);
    const jbc::core::ConstantPool::Entry& rec = cf.cp.at(cf.cp.at(bsms->front().args.front()).a);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(rec.utf8), rec.utf8Len),
              std::string("\x01. Status=\x01, URL=[\x01]"));
}

TEST(ConcatRestore, FailClosedOnMalformedShapeAndRecipeMismatch)
{
    model::ResourceContainer container;
    // 形状破坏（pop 注入）：汇编后程序化注 pop（jasm 自身校验栈形，拒收破坏体）。
    {
        const std::string uri = "BadShape.class";
        container.addResource(uri, std::make_unique<model::ClassResource>(
            uri, corruptWrapper(hostBytes("BadShape", "\x01\x02", {"c0"}, false))));
    }
    // 常量数不齐（recipe 2 个 \u0002，体里只有 1 个常量）。
    addHost(container, "BadCount", "\x01\x02\x02", {"c0"});

    const ScanResult scanned = scan(container);
    EXPECT_TRUE(scanned.sites.empty());
    EXPECT_EQ(scanned.wrapperShapeBad, 1u);
    EXPECT_EQ(scanned.recipeMismatch, 1u);

    const DirtySet dirty = rewrite(container, scanned);
    EXPECT_TRUE(dirty.uris.empty());

    const ProofResult verdict = proof(container, dirty, 2);
    EXPECT_TRUE(verdict.ok);
}

TEST(ConcatRestore, ProofResidualCountsUnrestoredWrappers)
{
    model::ResourceContainer container;
    addHost(container, "HostR", "\x01\x02", {"c0"});
    // 没跑 rewrite：residual 如实报告包装残留，两道检查不受牵连。
    const ScanResult scanned = scan(container);
    ASSERT_EQ(scanned.sites.size(), 1u);
    DirtySet dirty;
    const ProofResult verdict = proof(container, dirty, 1);
    EXPECT_TRUE(verdict.ok);
    EXPECT_EQ(verdict.residual, 1u);
}

}   // namespace grunt::concat_restore
