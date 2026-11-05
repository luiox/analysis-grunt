/// @file        booger_anno_strip.cpp
/// @brief       残留 `booger_anno_strip` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/booger_anno_strip.hpp"

#include <cstdio>
#include <set>
#include <string>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/util/annotation_depth.hpp"

#include "grunt/container.hpp"
#include "grunt/knowledge/booger_names.hpp"
#include "grunt/proof_toolbox.hpp"

namespace grunt::booger_anno_strip {

namespace {

namespace ir = jbc::ir;

/// @brief 一张属性表里的 @Booger 条目剔除。@return 剔除条数（0 = 没动）。
ca::usize stripTable(ir::ClassSession& s, std::vector<jbc::core::AttrSlot>& slots)
{
    ca::usize removed = 0;
    std::vector<ca::usize> dropSlots;
    for (ca::usize si = 0; si < slots.size(); ++si) {
        // 注解族槽先过嵌套深度预检（mjbase #158）：libjbc attr_codec 注解码码
        // 互递归无界，敌意深嵌注解会在 decodeAttr 内栈溢出；预检拒绝按
        // 「解不开原样保真」fail-open 跳过——少剥一条注解，进程不被打爆。
        if (!mjbase::util::attrSafeToDecode(s.backing(), slots[si])) continue;
        auto d = s.backing().decodeAttr(slots[si]);
        if (d.is_err()) continue;
        jbc::core::AttrBody* body = d.unwrap();
        if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) continue;
        auto*         aa    = static_cast<jbc::core::AnnotationsAttr*>(body);
        const ca::usize before = aa->annos.size();
        std::vector<jbc::core::AnnotationsAttr::Annotation> keep;
        for (auto& anno : aa->annos) {
            if (s.backing().utf8(anno.typeCp) == knowledge::kBoogerAnnoDesc) {
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

/// @brief Booger 引用门探针结果：是否在册 + 普查完整性（漏计即证据不足）。
struct RefProbe
{
    bool      found          = false;
    ca::usize classesSkipped = 0;   ///< 会话打不开、没能普查的类数
};

/// @brief 容器 CP 里是否有他类指向 Booger 的 Class 引用（自引不算）。
///
/// 引用门是删动词的前置判据，fail-closed 口径：会话打不开的类按「引用在册」
/// 记账（它可能正指着 Booger，普查看不见不等于引用不存在）——门整个关死，
/// 与 dead_member 引用普查的 census-blind 同一门。
RefProbe boogerReferenced(const mjbase::model::ResourceContainer& container)
{
    RefProbe probe;
    forEachClass(container, [&](const ClassEntry& entry) {
        if (probe.found) return;
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++probe.classesSkipped;
            return;
        }
        if (session->backing().thisName() == knowledge::kBoogerClassName) return;
        const jbc::core::ClassFile& cf = session->backing();
        for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
            if (!cf.cp.valid(i)) continue;
            const auto& e = cf.cp.at(i);
            if (e.tag != jbc::core::CpTag::Class) continue;
            if (cf.utf8(e.a) == knowledge::kBoogerClassName) {
                probe.found = true;
                return;
            }
        }
    });
    return probe;
}

/// @brief 一类里的注解条目普查（类/方法/字段三张面）。
ca::usize countAnnos(ir::ClassSession& s)
{
    ca::usize                    n  = 0;
    jbc::core::ClassFile&        cf = s.backing();
    const auto countIn = [&](std::vector<jbc::core::AttrSlot>& slots) {
        for (jbc::core::AttrSlot& slot : slots) {
            // 同 stripTable：注解族槽先过深度预检，敌意输入 fail-open 跳过。
            if (!mjbase::util::attrSafeToDecode(s.backing(), slot)) continue;
            auto d = s.backing().decodeAttr(slot);
            if (d.is_err()) continue;
            jbc::core::AttrBody* body = d.unwrap();
            if (body == nullptr || body->kind() != jbc::core::AttrKind::Annotations) {
                continue;
            }
            auto* aa = static_cast<jbc::core::AnnotationsAttr*>(body);
            for (const auto& anno : aa->annos) {
                if (s.backing().utf8(anno.typeCp) == knowledge::kBoogerAnnoDesc) ++n;
            }
        }
    };
    countIn(cf.attrs);
    for (jbc::core::MemberInfo& m : cf.methods) countIn(m.attrs);
    for (jbc::core::MemberInfo& f : cf.fields) countIn(f.attrs);
    return n;
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container)
{
    ScanResult result;
    const RefProbe probe = boogerReferenced(container);
    if (probe.found || probe.classesSkipped != 0) {
        result.gateBlocked = true;
        if (!probe.found) {
            std::fprintf(stderr,
                         "[grunt.BoogerAnnoStrip] 引用门普查不完整（%zu 类未普查）——按引用在册关死\n",
                         probe.classesSkipped);
        }
        return result;
    }
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        const ca::usize n = countAnnos(*session);
        if (n == 0) return;
        result.sites.push_back(Site{std::string(entry.uri), n});
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result)
{
    DirtySet dirty;
    if (result.gateBlocked) return dirty;

    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
        ca::usize           annos = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;
        jbc::core::ClassFile& cf = s.backing();

        ca::usize removed = 0;
        removed += stripTable(s, cf.attrs);
        for (jbc::core::MemberInfo& m : cf.methods) removed += stripTable(s, m.attrs);
        for (jbc::core::MemberInfo& f : cf.fields) removed += stripTable(s, f.attrs);
        if (removed == 0) return;

        auto encoded = jbc::core::writeClass(cf);
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), removed});
    });

    for (PendingWrite& item : pending) {
        // 压缩一次带走死 Utf8（注解描述符 / 参数名）。压缩前先过注解族深度
        // 预检（classAnnoDepthSafe）：cp_compact 会解全部属性槽，敌意深嵌
        // 注解在 libjbc 互递归解码里栈溢出；预检拒绝就跳过压缩，剥过注解
        // 的字节照常落盘（CP 不压缩只是少带走死条目）。
        auto session = openSession(item.bytes);
        if (session && classAnnoDepthSafe(session->backing())
            && ir::compactConstantPool(*session).is_ok()) {
            auto encoded2 = jbc::core::writeClass(session->backing());
            if (encoded2.is_ok()) item.bytes = std::move(encoded2).unwrap();
        }
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(item.uri);
        dirty.annos += item.annos;
        dirty.classes += 1;
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

}   // namespace grunt::booger_anno_strip
