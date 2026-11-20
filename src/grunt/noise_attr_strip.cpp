/// @file        noise_attr_strip.cpp
/// @brief       残留 `noise_attr_strip` 的三段实现：SCAN / REWRITE / PROOF。
///
/// Signature 的合法性判定是 **槽位形分派** 的严格文法校验（JVMS 4.7.9.1）：
/// 同一个串在类槽按 ClassSignature 判、方法槽按 MethodSignature 判、字段槽
/// 按 FieldSignature 判，判不过才摘。合法签名（jsoup 真泛型）逐字保留。
/// 属性体是裸 u2 CP 下标（未建模属性 span 直读），不走 decodeAttr。
///
/// 摘除后按 booger_anno_strip 同款收尾：writeClass → classAnnoDepthSafe
/// 预检 → cp_compact（带走 Crasher 死 Utf8，动辄数百 KB）→ 回写容器。
#include "grunt/noise_attr_strip.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/units.hpp"

#include "jbc/type/signature.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"

namespace grunt::noise_attr_strip {

namespace {

namespace ir = jbc::ir;

constexpr const char* kSignatureName = "Signature";
constexpr const char* kSdeName       = "SourceDebugExtension";

/// @brief 类属性表成员下标哨兵（方法/字段下标从 0 起，取 max 作哨兵不撞）。
constexpr ca::usize kClassAttrs = static_cast<ca::usize>(-1);

// ============================================================================
// ==== JVMS 4.7.9.1 签名文法校验（jbc/type 官方解析器）=====================
// ============================================================================

/// @brief 槽位形分派的 Signature 合法性判定：判不过 = Crasher 串。
///
/// jbc/type 的三个官方解析器（parseClassSignature / parseMethodSignature /
/// parseFieldSignature）是全仓单一文法源——严格递归下降 + 深度上限，空串/
/// 空白块/缺 ``;`` 变体分别落 Empty/Truncated/BadSignatureShape 等错误码。
bool signatureValid(std::string_view s, bool isMethod, bool isField)
{
    if (isMethod) return jbc::type::parseMethodSignature(s).is_ok();
    if (isField) return jbc::type::parseFieldSignature(s).is_ok();
    return jbc::type::parseClassSignature(s).is_ok();
}

// ============================================================================
// ==== 槽位普查 / 摘除 =======================================================
// ============================================================================

/// @brief 一张属性表上的噪声站点普查。只按名字选槽解码（Signature 建模体
///        带 signatureCp；SDE 落 RawAttr 原样字节），其余属性不碰不解码。
void collectIn(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& slots,
               const std::string& uri, bool isMethod, ca::usize memberIndex,
               ScanResult& result)
{
    for (ca::usize si = 0; si < slots.size(); ++si) {
        jbc::core::AttrSlot& slot = slots[si];
        const std::string    name(cf.utf8(slot.nameCp));
        const bool           isSig = name == kSignatureName;
        if (!isSig && name != kSdeName) continue;
        if (isSig) {
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) continue;
            jbc::core::AttrBody* body = d.unwrap();
            if (body == nullptr || body->kind() != jbc::core::AttrKind::Signature) {
                continue;
            }
            const ca::u32 ref =
                static_cast<jbc::core::SignatureAttr*>(body)->signatureCp;
            if (!cf.cp.valid(ref)) continue;
            const bool isFieldSlot = !isMethod && memberIndex != kClassAttrs;
            if (signatureValid(cf.utf8(ref), isMethod, isFieldSlot)) continue;
            result.sites.push_back(AttrSite{uri, isMethod, memberIndex, si, true});
            ++result.sigInvalid;
            continue;
        }
        result.sites.push_back(AttrSite{uri, isMethod, memberIndex, si, false});
        ++result.sdeCount;
    }
}

/// @brief 一类上的站点普查（scan 专用，纯读）。
void collectSites(jbc::core::ClassFile& cf, const std::string& uri, ScanResult& result)
{
    collectIn(cf, cf.attrs, uri, false, kClassAttrs, result);
    for (ca::usize mi = 0; mi < cf.methods.size(); ++mi) {
        collectIn(cf, cf.methods[mi].attrs, uri, true, mi, result);
    }
    for (ca::usize fi = 0; fi < cf.fields.size(); ++fi) {
        collectIn(cf, cf.fields[fi].attrs, uri, false, fi, result);
    }
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        collectSites(session->backing(), std::string(entry.uri), result);
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    std::set<std::string> uris;
    for (const AttrSite& site : result.sites) uris.insert(site.uri);

    for (const std::string& uri : uris) {
        const std::vector<ca::u8>* bytes = findClassBytes(container, uri);
        if (bytes == nullptr) continue;
        auto session = openSession(*bytes);
        if (!session) continue;
        jbc::core::ClassFile& cf = session->backing();

        // 倒序摘除：类表 → 成员表各倒序遍历，erase 防下标漂移。Signature 与
        // scan 同判据复核（同字节同判，防漂移），SDE 无条件摘。
        ca::usize removed = 0;
        const auto drop = [&](std::vector<jbc::core::AttrSlot>& slots, bool isMethod,
                              bool isField) {
            for (ca::usize si = slots.size(); si-- > 0;) {
                jbc::core::AttrSlot& slot = slots[si];
                const std::string    name(cf.utf8(slot.nameCp));
                const bool           isSig = name == kSignatureName;
                if (!isSig && name != kSdeName) continue;
                if (isSig) {
                    auto d = cf.decodeAttr(slot);
                    if (d.is_err()) continue;
                    jbc::core::AttrBody* body = d.unwrap();
                    if (body == nullptr
                        || body->kind() != jbc::core::AttrKind::Signature) {
                        continue;
                    }
                    const ca::u32 ref =
                        static_cast<jbc::core::SignatureAttr*>(body)->signatureCp;
                    if (!cf.cp.valid(ref)) continue;
                    if (signatureValid(cf.utf8(ref), isMethod, isField)) continue;
                }
                slots.erase(slots.begin() + static_cast<ca::i64>(si));
                ++removed;
            }
        };
        drop(cf.attrs, false, false);
        for (jbc::core::MemberInfo& m : cf.methods) drop(m.attrs, true, false);
        for (jbc::core::MemberInfo& f : cf.fields) drop(f.attrs, false, true);
        if (removed == 0) continue;

        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) continue;
        std::vector<ca::u8> out = std::move(encoded).unwrap();
        // 压缩一次带走死 Utf8（Crasher 串动辄数百 KB）；预检同 booger 口径。
        auto after = openSession(out);
        if (after && classAnnoDepthSafe(after->backing())
            && ir::compactConstantPool(*after).is_ok()) {
            auto encoded2 = jbc::core::writeClass(after->backing());
            if (encoded2.is_ok()) out = std::move(encoded2).unwrap();
        }
        replaceClass(container, uri, std::move(out));
        dirty.uris.push_back(uri);
        dirty.attrsRemoved += removed;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore)
{
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);
    ProofResult result;
    result.ok     = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::noise_attr_strip
