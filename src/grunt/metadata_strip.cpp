/// @file        metadata_strip.cpp
/// @brief       残留 `metadata_strip` 三段实现：SCAN（三面盘查 + Era 门）→
///              REWRITE（资源先删 + 注解三面摘除 + 水印字段删除 + 压池写回）
///              → PROOF（双门 + 三面 residual）。
///
/// 判据分级与资源动词见头文件；骨架对齐 booger_anno_strip（注解槽 decodeAttr
/// 直调位 + 深度预检 + 压池收尾）与 K2 string_cache_fold（Era 门在 scan、
/// proof 挂起全量）。
///
/// ## SCAN 三面与判据谓词
///
/// 判据谓词三段共用单源（树版逐字对齐）。S3 Era 门在 scan 开头：
/// era != Kasumi 整面关闭（eraBlocked，fail-closed）——未判时一个字节都不
/// 看内容判据（非 Kasumi 罐挂同前缀资源时 scan 空手而归，产物资源逐字节
/// 原样）。注解面走 decodeAttr 直调位（booger_anno_strip 同一通路）普查
/// 类/字段/方法三张挂载面的 @Metadata 条目；深度预检（attrSafeToDecode）
/// 拦下的槽计 annoBlocked——解不开的 fail-closed 不摘、不猜条目数。水印
/// 字段面：字段带 @Metadata 或 ConstantValue 为水印串都计入；ConstantValue
/// 双形态都收——jbc 原生 ConstantValueAttr 解码产物与 RawAttr 2 字节载荷
/// （const_value_promote 新建形态）。资源面 visitResources 全量走一遍，
/// 只看 UnknownResource 载荷（树版同口径），URI 前缀 + `.md` + 内容探针
/// 三证齐才进删除清单；形状命中而证据不全 → resourceCandidates 盘点不删
/// （前缀面可能有合法文档，删的是内容坐实的注入物）。
///
/// ## REWRITE
///
/// 资源先删（树版序）：只消费 scan 清单，删除时刻复核同判据（scan 之后字
/// 节可能已变，不盲删）；复核不齐计 resourceCandidates。类清理与
/// booger_anno_strip 同骨架：类面/方法面只摘条目（摘空属性槽整槽移除）；
/// 字段面摘条目后按「摘到过条目 ∨ ConstantValue 水印」整字段删除（注解
/// 字段与水印字段都是混淆器署名位，树版直接删，不做引用普查——病态引用
/// 形态由类加载校验兜底）。方法体不碰。压池收尾：classAnnoDepthSafe 预检
/// + compactConstantPool 带走死条目；预检拒绝或压缩失败就跳过压缩，清理
/// 过的字节照常落盘——CP 不压缩只是少带走死条目。条目数在类写回成功后归
/// 账；编码失败计 failed。
///
/// ## PROOF
///
/// pass_clock::Suspend 挂起全量，三道门：类数门（classesIntact）；资源对
/// 账门（resourcesShrunk：资源数只减 + 删除清单逐条不在场，虚报删减即
/// FAILED）；三面 residual（与 scan 同判据重钉，语料单罐门要求全 0）。深
/// 度预检拦下的敌意槽不在 residual 视野内——不可解面不是清理缺陷，
/// annoBlocked 已记账。
///
/// ## 已知边界
///
/// 水印串清理只到字段面：方法体内的水印常量（`PROTECTED BY Kasumi` 字面
/// 量与 `kasumi_` 前缀会话 key）是 indy 链路运行时数据，树版不触方法体，
/// 本仓对齐；全链收口后由 DeadMember 压池/死码面自然带走解引用的死串。
/// 换代面（注解描述符 / 水印三判据 / 资源前缀与五探针都钉自当前 Kasumi
/// 版本）由 Era 门整面拒绝兜底。

#include "grunt/metadata_strip.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/kasumi_names.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::metadata_strip {

namespace {

namespace ir = jbc::ir;

// ============================================================================
// ==== 判据谓词（scan / rewrite / proof 三段共用，单源）=======================
// ============================================================================

/// @brief 单条注解是否 @Metadata（S2 形状钉面：描述符全等，树版
///        isKasumiMetadataAnnotation 对齐）。
bool isMetadataAnno(const jbc::core::ClassFile&                   cf,
                    const jbc::core::AnnotationsAttr::Annotation& anno)
{
    return cf.utf8(anno.typeCp) == knowledge::kKasumiMetadataAnnoDesc;
}

/// @brief 水印串判据（树版 isKasumiWatermarkString 三条逐字对齐）：含
///        `PROTECTED BY Kasumi` / 含 `Gruntpocalypse` / 前缀 `kasumi_`。
bool isWatermarkText(std::string_view text)
{
    if (text.find(knowledge::kKasumiWatermarkText) != std::string_view::npos) return true;
    if (text.find(knowledge::kKasumiWatermarkAltText) != std::string_view::npos) return true;
    return text.rfind(knowledge::kKasumiWatermarkKeyPrefix, 0) == 0;
}

/// @brief 反 LLM 注入资源判据（树版 isKasumiAntiLlmResource 三面对齐）：
///        URI 前缀 `META-INF/Kasumi/` + `.md` 后缀 + 内容探针任一子串在场。
bool isAntiLlmResource(std::string_view uri, const std::vector<ca::u8>& bytes)
{
    if (uri.rfind(knowledge::kKasumiResourcePrefix, 0) != 0) return false;
    const std::string_view suffix = knowledge::kKasumiResourceSuffix;
    if (uri.size() < suffix.size()
        || uri.compare(uri.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    const std::string text(bytes.begin(), bytes.end());
    for (const std::string_view probe : knowledge::kKasumiResourceProbes) {
        if (text.find(probe) != std::string::npos) return true;
    }
    return false;
}

/// @brief 资源 URI 是否命中前缀形状（resourceCandidates 盘点口径）。
bool resourcePrefixHit(std::string_view uri)
{
    return uri.rfind(knowledge::kKasumiResourcePrefix, 0) == 0;
}

// ============================================================================
// ==== 注解槽普查 / 摘除（booger_anno_strip 同一 decodeAttr 直调位）===========
// ============================================================================

/// @brief 一张属性表里的 @Metadata 条目普查。深度预检拦下的槽计 blocked
///        （fail-closed：解不开的不算「没有」，如实记账但不产出条目）。
ca::usize countMetadataTable(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& slots,
                             ca::usize& blocked)
{
    ca::usize n = 0;
    for (jbc::core::AttrSlot& slot : slots) {
        // 注解族解码互递归无界（mjbase #158）：预检拒绝 = 解不开，fail-closed
        // 记账跳过——条目数不猜（树版无此敌意面；本仓守卫优先于树版静默）。
        if (!mjbase::util::attrSafeToDecode(cf, slot)) {
            ++blocked;
            continue;
        }
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) continue;
        jbc::core::AttrBody* body = d.unwrap();
        if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) continue;
        const auto* aa = static_cast<const jbc::core::AnnotationsAttr*>(body);
        for (const auto& anno : aa->annos) {
            if (isMetadataAnno(cf, anno)) ++n;
        }
    }
    return n;
}

/// @brief 一张属性表里的 @Metadata 条目摘除（其余注解保留；摘空的属性槽
///        整槽移除）。@return 摘除条数。
ca::usize stripMetadataTable(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& slots,
                             ca::usize& blocked)
{
    ca::usize              removed   = 0;
    std::vector<ca::usize> dropSlots;
    for (ca::usize si = 0; si < slots.size(); ++si) {
        if (!mjbase::util::attrSafeToDecode(cf, slots[si])) {
            ++blocked;   // fail-closed：该槽不摘，原样保真
            continue;
        }
        auto d = cf.decodeAttr(slots[si]);
        if (d.is_err()) continue;
        jbc::core::AttrBody* body = d.unwrap();
        if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) continue;
        auto*         aa    = static_cast<jbc::core::AnnotationsAttr*>(body);
        const ca::usize before = aa->annos.size();
        std::vector<jbc::core::AnnotationsAttr::Annotation> keep;
        for (auto& anno : aa->annos) {
            if (isMetadataAnno(cf, anno)) {
                ++removed;
            } else {
                keep.push_back(std::move(anno));
            }
        }
        if (keep.size() == before) continue;
        if (keep.empty()) {
            dropSlots.push_back(si);
        } else {
            auto* m = slots[si].mutate();
            static_cast<jbc::core::AnnotationsAttr*>(m)->annos = std::move(keep);
        }
    }
    for (auto it = dropSlots.rbegin(); it != dropSlots.rend(); ++it) {
        slots.erase(slots.begin() + static_cast<ca::i64>(*it));
    }
    return removed;
}

/// @brief 字段的 ConstantValue → String 常量内容（树版 field->value 的
///        String 分支）。非 String / 解不开返回 false（fail-closed：不猜）。
///
/// 两个落地形态都收：解码产物 ConstantValueAttr（jbc 原生解码）与 RawAttr
/// （const_value_promote 新建的 2 字节 Raw 载荷，写回前不重解码）。
bool fieldConstantString(jbc::core::ClassFile& cf, jbc::core::MemberInfo& fld,
                         std::string_view& out)
{
    for (jbc::core::AttrSlot& slot : fld.attrs) {
        if (cf.utf8(slot.nameCp) != "ConstantValue") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return false;
        const jbc::core::AttrBody* body = d.unwrap();
        if (body == nullptr) return false;
        ca::u32 cpIdx = 0;
        if (body->kind() == jbc::core::AttrKind::ConstantValue) {
            cpIdx = static_cast<const jbc::core::ConstantValueAttr*>(body)->constantValueCp;
        } else if (body->kind() == jbc::core::AttrKind::Raw) {
            const std::vector<ca::u8>& raw = static_cast<const jbc::core::RawAttr*>(body)->bytes();
            if (raw.size() < 2) return false;
            cpIdx = (static_cast<ca::u32>(raw[0]) << 8) | static_cast<ca::u32>(raw[1]);
        } else {
            return false;
        }
        if (!cf.cp.valid(cpIdx)) return false;
        const jbc::core::ConstantPool::Entry& e = cf.cp.at(cpIdx);
        if (e.tag != jbc::core::CpTag::String || !cf.cp.valid(e.a)) return false;
        out = cf.utf8(e.a);
        return true;
    }
    return false;
}

/// @brief 单类三面盘查（SCAN）。@return 是否产出站点（条目或字段命中）。
bool scanClass(ir::ClassSession& s, const std::string& uri, Site& out, ScanResult& result)
{
    jbc::core::ClassFile& cf = s.backing();
    ca::usize             blocked = 0;

    ca::usize annos = countMetadataTable(cf, cf.attrs, blocked);
    for (jbc::core::MemberInfo& m : cf.methods) {
        annos += countMetadataTable(cf, m.attrs, blocked);
    }
    ca::usize fields = 0;
    for (jbc::core::MemberInfo& f : cf.fields) {
        const ca::usize fieldAnnos = countMetadataTable(cf, f.attrs, blocked);
        annos += fieldAnnos;
        std::string_view cv;
        if (fieldAnnos > 0) {
            ++fields;   // 带注解字段：整字段删除（树版对齐）
            continue;
        }
        if (fieldConstantString(cf, f, cv) && isWatermarkText(cv)) ++fields;
    }
    result.annoBlocked += blocked;
    if (annos == 0 && fields == 0) return false;
    out.uri    = uri;
    out.annos  = annos;
    out.fields = fields;
    return true;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container, const ScanInput& input)
{
    ScanResult result;

    // ---- S3 Era 门：三面判据全是 Kasumi 版本知识，Era 分支唯一合法落点。
    //      未判即整面关闭（fail-closed）：资源一个不看内容，类一个不进站。
    if (input.era != knowledge::Era::Kasumi) {
        result.eraBlocked = true;
        return result;
    }

    // ---- 资源清单（树版序：资源先于类）。UnknownResource 条目逐个三证
    //      判定；形状命中而证据不全的如实盘点（树版保留，本仓记账）。
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource& res) {
        const auto* unknown =
            ca::core::tag_cast::dyn_cast<const mjbase::model::UnknownResource>(&res);
        if (unknown == nullptr) return;   // 树版只看 Unknown 载荷
        if (isAntiLlmResource(uri, unknown->get())) {
            result.resources.emplace_back(uri);
            return;
        }
        if (resourcePrefixHit(uri)) ++result.resourceCandidates;
    });

    // ---- 类三面盘查。
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        auto         session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        Site site;
        if (scanClass(*session, uri, site, result)) {
            result.totalAnnos  += site.annos;
            result.totalFields += site.fields;
            result.sites.push_back(std::move(site));
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;

    // ---- 资源先删（树版序）。只消费 scan 清单；删除时刻复核同判据
    //      （scan 之后字节可能已变，不盲删——K2 rewriteSite 复核同纪律）。
    for (const std::string& uri : result.resources) {
        const mjbase::model::IResource* raw = container.getResource(uri);
        const auto* unknown = ca::core::tag_cast::dyn_cast<const mjbase::model::UnknownResource>(raw);
        if (unknown == nullptr || !isAntiLlmResource(uri, unknown->get())) {
            ++dirty.resourceCandidates;   // 复核不齐：不删，如实记账
            continue;
        }
        if (!rewrite_tools::removeResource(container, uri)) {
            ++dirty.resourceCandidates;   // 条目已不在：对账面由 proof 判
            continue;
        }
        dirty.removedResources.push_back(uri);
    }

    // ---- 类按 uri 清理（booger_anno_strip 同骨架：条目数在类写回成功后归账）。
    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           annos  = 0;
        ca::usize           fields = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession&    s  = *session;
        jbc::core::ClassFile& cf = s.backing();

        ca::usize blocked = 0;
        ca::usize annos   = 0;
        ca::usize fields  = 0;

        // 类面：只摘条目。
        annos += stripMetadataTable(cf, cf.attrs, blocked);

        // 方法面：只摘条目（方法体不动，树版对齐）。
        for (jbc::core::MemberInfo& m : cf.methods) {
            annos += stripMetadataTable(cf, m.attrs, blocked);
        }

        // 字段面：摘条目 + 删字段（带 @Metadata 或 ConstantValue 水印的
        // 整字段删除，树版 removeKasumiWatermarkFields 对齐；倒序遍历保
        // erase 稳定）。
        for (ca::usize fi = cf.fields.size(); fi-- > 0;) {
            jbc::core::MemberInfo& f = cf.fields[fi];
            const ca::usize stripped = stripMetadataTable(cf, f.attrs, blocked);
            std::string_view cv;
            const bool watermark = fieldConstantString(cf, f, cv) && isWatermarkText(cv);
            annos += stripped;
            if (stripped > 0 || watermark) {
                cf.fields.erase(cf.fields.begin() + static_cast<ca::i64>(fi));
                ++fields;
            }
        }
        dirty.annoBlocked += blocked;

        if (annos == 0 && fields == 0) return;   // 复核零命中：不写回（字节不动）
        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) {
            dirty.failed += annos + fields;   // 未归账如实记账
            return;
        }
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), annos, fields});
    });

    for (PendingWrite& item : pending) {
        // 压缩一次带走死条目（注解描述符 / 水印串 / 字段引用）。压缩前先过
        // 注解族深度预检（classAnnoDepthSafe）：cp_compact 会解全部属性槽，
        // 敌意深嵌注解在 libjbc 互递归解码里栈溢出；预检拒绝就跳过压缩，
        // 清理过的字节照常落盘（CP 不压缩只是少带走死条目）。
        auto session = openSession(item.bytes);
        if (session && classAnnoDepthSafe(session->backing())
            && ir::compactConstantPool(*session).is_ok()) {
            auto encoded2 = jbc::core::writeClass(session->backing());
            if (encoded2.is_ok()) item.bytes = std::move(encoded2).unwrap();
        }
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.annos  += item.annos;
        dirty.fields += item.fields;
        dirty.classes += 1;
    }
    return dirty;
}

// ============================================================================
// ==== PROOF =================================================================
// ============================================================================

namespace {

/// @brief 容器里仍可判定的三面残留（residual 口径，与 scan 同判据重钉）。
///
/// 深度预检拦下的敌意槽不在视野内（解不开 = 不可判，不是清理缺陷；
/// scan 的 annoBlocked 已记账）。
void countResidual(const mjbase::model::ResourceContainer& container, ProofResult& result)
{
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) return;
        jbc::core::ClassFile& cf    = session->backing();
        ca::usize             dummy = 0;
        result.residualAnnos += countMetadataTable(cf, cf.attrs, dummy);
        for (jbc::core::MemberInfo& m : cf.methods) {
            result.residualAnnos += countMetadataTable(cf, m.attrs, dummy);
        }
        for (jbc::core::MemberInfo& f : cf.fields) {
            const ca::usize fieldAnnos = countMetadataTable(cf, f.attrs, dummy);
            result.residualAnnos += fieldAnnos;
            std::string_view cv;
            if (fieldAnnos > 0) {
                ++result.residualFields;
                continue;
            }
            if (fieldConstantString(cf, f, cv) && isWatermarkText(cv)) ++result.residualFields;
        }
    });
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource& res) {
        const auto* unknown =
            ca::core::tag_cast::dyn_cast<const mjbase::model::UnknownResource>(&res);
        if (unknown == nullptr) return;
        if (isAntiLlmResource(uri, unknown->get())) ++result.residualResources;
    });
}

}   // namespace

ProofResult proof(const mjbase::model::ResourceContainer& container, const DirtySet& dirty,
                  ca::usize classesBefore, ca::usize resourcesBefore)
{
    // proof 是效力门（不变量复核），不许被 pass 级时间片截短——挂起期间
    // forEachClass 全量放行（residual 统计同样要看到全量类）。
    pass_clock::Suspend suspend;

    // ---- 类门（与全部残留同门）：类数不增 + 脏类可重解析。
    const proof_tools::ClassIntactResult intact =
        proof_tools::classesIntact(container, dirty.uris, classesBefore);

    // ---- 资源对账门（资源动词验收面）：资源数不增（只减）+ 删除清单逐条
    //      不在场。
    const proof_tools::ResourceShrunkResult shrunk =
        proof_tools::resourcesShrunk(container, dirty.removedResources, resourcesBefore);

    ProofResult result;
    result.ok     = intact.ok && shrunk.ok;
    result.failed = intact.failed;
    for (const std::string& f : shrunk.failed) result.failed.push_back(f);
    countResidual(container, result);
    return result;
}

}   // namespace grunt::metadata_strip
