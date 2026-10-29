/// @file        trampoline_squeeze.cpp
/// @brief       残留 `trampoline_squeeze` 的三段实现：SCAN / REWRITE / PROOF。
#include "grunt/trampoline_squeeze.hpp"

#include <set>
#include <string>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"

namespace grunt::trampoline_squeeze {

namespace {

namespace ir = jbc::ir;

/// @brief 单方法 SCAN：收集「目标即顺序后继」的 goto/goto_w。
void scanMethod(ir::ClassSession& s, ca::usize methodIndex, const std::string& uri,
                ScanResult& result)
{
    if (!ir::hasCode(s, methodIndex)) return;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr) continue;
        if (in->op != jbc::core::Op::GOTO && in->op != jbc::core::Op::GOTO_W) continue;

        auto resolved = cu->dom.resolveLabel(in->target);
        if (resolved.is_err()) continue;
        const ir::LabelTarget t = resolved.unwrap();
        if (t.isEnd()) continue;
        // 目标必须是布局顺序后继（句柄相等判据，与字节布局无关）。
        if (i + 1 >= order.size() || !(t.insn == order[i + 1])) continue;

        result.sites.push_back(
            Site{uri, methodIndex, in->srcPc, in->op == jbc::core::Op::GOTO_W});
    }
}

/// @brief 单方法 REWRITE：按 srcPc 重定位蹦床，原地置 NOP。
/// @return 命中数。
ca::usize rewriteMethod(ir::ClassSession& s, ca::usize methodIndex,
                        const std::vector<const Site*>& sites)
{
    if (!ir::hasCode(s, methodIndex)) return 0;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return 0;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return 0;

    ca::usize hit = 0;
    for (const Site* site : sites) {
        const std::vector<ir::InsnId>& order = cu->dom.order();
        for (const ir::InsnId id : order) {
            const ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            const bool isTrampoline = in->op == jbc::core::Op::GOTO
                                      || in->op == jbc::core::Op::GOTO_W;
            if (!isTrampoline || in->srcPc != site->srcPc) continue;
            // 复核判据（字节可能已被同方法其他站点改写挤压，不盲改）。
            auto resolved = cu->dom.resolveLabel(in->target);
            if (resolved.is_err()) break;
            const ir::LabelTarget t = resolved.unwrap();
            const ca::usize         pos = [&] {
                for (ca::usize i = 0; i < order.size(); ++i) {
                    if (order[i] == id) return i;
                }
                return order.size();
            }();
            if (t.isEnd() || pos + 1 >= order.size() || !(t.insn == order[pos + 1])) break;

            auto slot = cu->dom.get(id);
            if (slot.is_err()) break;
            ir::Insn*      m     = slot.unwrap();
            const ca::u32  keepPc = m->srcPc;
            *m                    = ir::Insn{};
            m->op                 = jbc::core::Op::NOP;
            m->srcPc              = keepPc;
            ++hit;
            break;
        }
    }
    return hit;
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
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            scanMethod(s, mi, std::string(entry.uri), result);
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

    // 按 uri 聚合站点（容器遍历序 = scan 序，逐类一次性处理）。
    std::set<std::string> uris;
    for (const Site& site : result.sites) uris.insert(site.uri);

    struct PendingWrite
    {
        std::string           uri;
        std::vector<ca::u8>   bytes;
        ca::usize             hit = 0;
    };
    std::vector<PendingWrite> pending;

    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        if (uris.count(uri) == 0) return;

        std::vector<const Site*> sitesOf;
        for (const Site& site : result.sites) {
            if (site.uri == uri) sitesOf.push_back(&site);
        }

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        // 方法下标聚合（同方法多站点一次 promote）。
        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        ca::usize hit = 0;
        for (const ca::usize mi : methodIdxs) {
            dirty.attempted += 1;
            hit += rewriteMethod(s, mi, sitesOf);
        }
        if (hit == 0) return;

        for (const ca::usize mi : methodIdxs) {
            auto lowered = ir::lowerMethod(s, mi);
            if (lowered.is_err()) return;   // 整类放弃（fail-closed）
        }
        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        pending.push_back(PendingWrite{uri, std::move(encoded).unwrap(), hit});
    });

    for (PendingWrite& item : pending) {
        replaceClass(container, item.uri, std::move(item.bytes));
        dirty.uris.push_back(std::move(item.uri));
        dirty.squeezed += item.hit;
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
    result.ok    = intact.ok;
    result.failed = intact.failed;
    return result;
}

}   // namespace grunt::trampoline_squeeze
