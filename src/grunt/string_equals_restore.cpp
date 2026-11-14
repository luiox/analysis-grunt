/// @file        string_equals_restore.cpp
/// @brief       残留 `string_equals_restore` 的三段实现：SCAN / REWRITE / PROOF。
///
/// ## 数据流
///
/// `Site{classIdx, methodIndex, anchorSrcPc, variant}` →
/// `DirtySet{uris, sites, attempted, failed, skippedByRoundTrip}`。
#include "grunt/string_equals_restore.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/rewrite_toolbox.hpp"

namespace grunt::string_equals_restore {

namespace {

namespace ir = jbc::ir;
using jbc::core::Op;
using proof_tools::roundTripSafe;

/// @brief 窗口步：一个 op + 可选成员引用（owner/name/desc 精确吻合）。
struct WinStep
{
    Op                 op;
    const char*        owner  = nullptr;
    const char*        name   = nullptr;
    const char*        desc   = nullptr;
};

/// equals 窗口（6 条），锚点 = Object.hashCode。
const WinStep* equalsWindow()
{
    static const WinStep w[6] = {
        {Op::INVOKEVIRTUAL, "java/lang/Object", "hashCode", "()I"},
        {Op::INVOKESTATIC, "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"},
        {Op::SWAP},
        {Op::INVOKEVIRTUAL, "java/lang/String", "hashCode", "()I"},
        {Op::INVOKESTATIC, "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"},
        {Op::INVOKEVIRTUAL, "java/lang/Integer", "equals", "(Ljava/lang/Object;)Z"},
    };
    return w;
}

constexpr ca::usize kEqualsLen = 6;

/// equalsIgnoreCase 窗口（8 条），锚点 = 第一条 String.toUpperCase。
const WinStep* ignoreWindow()
{
    static const WinStep w[8] = {
        {Op::INVOKEVIRTUAL, "java/lang/String", "toUpperCase", "()Ljava/lang/String;"},
        {Op::INVOKEVIRTUAL, "java/lang/Object", "hashCode", "()I"},
        {Op::INVOKESTATIC, "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"},
        {Op::SWAP},
        {Op::INVOKEVIRTUAL, "java/lang/String", "toUpperCase", "()Ljava/lang/String;"},
        {Op::INVOKEVIRTUAL, "java/lang/String", "hashCode", "()I"},
        {Op::INVOKESTATIC, "java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"},
        {Op::INVOKEVIRTUAL, "java/lang/Integer", "equals", "(Ljava/lang/Object;)Z"},
    };
    return w;
}

constexpr ca::usize kIgnoreLen = 8;

/// @brief 窗口逐条对齐（NOP 夹层跳过；成员引用逐字钉）。
///
/// @param[out] lastPos  窗口末条在 order 里的位置。
bool matchWindow(ir::ClassSession& s, const ir::CodeDom& dom,
                 const std::vector<ir::InsnId>& order, ca::usize anchorPos,
                 const WinStep* win, ca::usize winLen, ca::usize& lastPos)
{
    ca::usize got = 0;
    for (ca::usize p = anchorPos; p < order.size() && got < winLen; ++p) {
        const ir::Insn* cur = dom.peek(order[p]);
        if (cur == nullptr || cur->op == Op::NOP) continue;
        if (cur->op != win[got].op) return false;
        if (win[got].owner != nullptr) {
            ir::ClassSession::MemberSem sem;
            if (!s.memberOf(*cur, sem)
                || sem.kind != ir::ClassSession::MemberSem::Kind::Method
                || sem.owner != win[got].owner || sem.name != win[got].name
                || sem.desc != win[got].desc) {
                return false;
            }
        }
        if (got > 0 && !dom.labelsAt(order[p]).empty()) return false;   ///< 合并点
        ++got;
        if (got == winLen) {
            lastPos = p;
            return true;
        }
    }
    return false;
}

/// @brief 目标成员（还原后的真调用）。
struct Target
{
    Op         op;
    const char* name;
    const char* desc;
};

Target targetOf(Variant v)
{
    if (v == Variant::EqualsIgnore) {
        return Target{Op::INVOKEVIRTUAL, "equalsIgnoreCase", "(Ljava/lang/String;)Z"};
    }
    return Target{Op::INVOKEVIRTUAL, "equals", "(Ljava/lang/Object;)Z"};
}

}   // namespace

// ============================================================================
// ==== SCAN ==================================================================
// ============================================================================

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& /*budgetTemplate*/)
{
    ScanResult result;
    ca::u32    classIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = classIdx++;
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        ir::ClassSession&    s    = *session;
        const ir::ClassUnit& unit = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!ir::hasCode(s, mi)) continue;
            auto promoted = ir::promoteMethod(s, mi);
            if (promoted.is_err()) continue;
            ir::CodeUnit* cu = promoted.unwrap().get();
            if (cu == nullptr) continue;
            const std::vector<ir::InsnId>& order = cu->dom.order();
            ca::usize                      pos   = 0;
            while (pos < order.size()) {
                const ir::Insn* in = cu->dom.peek(order[pos]);
                if (in == nullptr || in->op == Op::NOP) {
                    ++pos;
                    continue;
                }
                Site      site;
                ca::usize last = 0;
                // 锚点分派：Object.hashCode 先试 equals 窗口；String.toUpperCase
                // 试 ignoreCase 窗口（两窗口锚点不交）。
                bool hit = false;
                if (in->op == Op::INVOKEVIRTUAL) {
                    ir::ClassSession::MemberSem sem;
                    if (s.memberOf(*in, sem) && sem.name == "hashCode"
                        && sem.owner == "java/lang/Object" && sem.desc == "()I"
                        && matchWindow(s, cu->dom, order, pos, equalsWindow(), kEqualsLen,
                                       last)) {
                        site.variant = Variant::Equals;
                        hit          = true;
                    } else if (s.memberOf(*in, sem) && sem.name == "toUpperCase"
                               && sem.owner == "java/lang/String"
                               && sem.desc == "()Ljava/lang/String;"
                               && matchWindow(s, cu->dom, order, pos, ignoreWindow(),
                                              kIgnoreLen, last)) {
                        site.variant = Variant::EqualsIgnore;
                        hit          = true;
                    }
                }
                if (hit) {
                    site.classIdx    = idx;
                    site.methodIndex = mi;
                    site.anchorSrcPc = in->srcPc;
                    result.sites.push_back(site);
                    pos = last + 1;
                    continue;
                }
                ++pos;
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& /*budgetTemplate*/)
{
    DirtySet dirty;
    if (result.sites.empty()) return dirty;

    rewrite_tools::FrameContext frames(container);

    std::unordered_map<ca::u32, std::vector<const Site*>> byIdx;
    for (const Site& site : result.sites) byIdx[site.classIdx].push_back(&site);

    ca::u32 curIdx = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const ca::u32 idx = curIdx++;
        const auto    it  = byIdx.find(idx);
        if (it == byIdx.end()) return;
        const std::vector<const Site*> sitesOf = it->second;
        byIdx.erase(it);

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        const std::unordered_set<ca::usize> safe =
            proof_tools::roundTripSafe(*entry.bytes,
                                       std::vector<ca::usize>(methodIdxs.begin(),
                                                              methodIdxs.end()));

        ca::usize hit    = 0;
        ca::usize failed = 0;
        for (const ca::usize mi : methodIdxs) {
            std::vector<const Site*> sitesOfMethod;
            for (const Site* site : sitesOf) {
                if (site->methodIndex == mi) sitesOfMethod.push_back(site);
            }
            dirty.attempted += sitesOfMethod.size();
            if (safe.count(mi) == 0) {
                dirty.skippedByRoundTrip += sitesOfMethod.size();
                continue;
            }
            ca::usize methodHit = 0;
            const bool ok = rewrite_tools::mutateMethodWithFrames(
                s, mi, frames.resolver(),
                [&](ir::CodeUnit& cu) {
                    const std::vector<ir::InsnId>& order = cu.dom.order();
                    for (const Site* site : sitesOfMethod) {
                        ca::usize anchorPos = static_cast<ca::usize>(-1);
                        for (ca::usize p = 0; p < order.size(); ++p) {
                            const ir::Insn* in = cu.dom.peek(order[p]);
                            if (in != nullptr && in->srcPc == site->anchorSrcPc
                                && in->op == Op::INVOKEVIRTUAL) {
                                anchorPos = p;
                                break;
                            }
                        }
                        if (anchorPos == static_cast<ca::usize>(-1)) {
                            ++failed;
                            continue;
                        }
                        // 全量复核：窗口重钉，变体一致才动。
                        ca::usize last = 0;
                        bool      rehit = false;
                        if (site->variant == Variant::Equals) {
                            rehit = matchWindow(s, cu.dom, order, anchorPos, equalsWindow(),
                                                kEqualsLen, last);
                        } else {
                            rehit = matchWindow(s, cu.dom, order, anchorPos, ignoreWindow(),
                                                kIgnoreLen, last);
                        }
                        if (!rehit) {
                            ++failed;
                            continue;
                        }

                        auto slotR = cu.dom.get(order[anchorPos]);
                        if (slotR.is_err()) {
                            ++failed;
                            continue;
                        }
                        ir::Insn*     insn     = slotR.unwrap();
                        const ir::Insn original = *insn;
                        const Target  target   = targetOf(site->variant);
                        insn->op    = target.op;
                        if (!s.setMember(*insn, ir::ClassSession::MemberSem::Kind::Method,
                                         "java/lang/String", target.name, target.desc)) {
                            *insn = original;
                            ++failed;
                            continue;
                        }

                        std::vector<ir::InsnId> victims;
                        for (ca::usize p = anchorPos + 1; p <= last && p < order.size(); ++p) {
                            const ir::Insn* victim = cu.dom.peek(order[p]);
                            if (victim == nullptr || victim->op == Op::NOP) continue;
                            victims.push_back(order[p]);
                        }
                        if (!victims.empty() && cu.dom.eraseBatch(victims).is_err()) {
                            *insn = original;
                            ++failed;
                            continue;
                        }
                        ++methodHit;
                    }
                    return methodHit > 0;
                });
            if (!ok) {
                failed += methodHit;
                continue;
            }
            hit += methodHit;
        }
        dirty.failed += failed;
        if (hit == 0) return;

        auto encoded = jbc::core::writeClass(s.backing());
        if (encoded.is_err()) return;
        mjbase::model::IResource* raw = container.getResource(entry.uri);
        if (!ca::core::tag_cast::isa<mjbase::model::ClassResource>(raw)) return;
        auto* clsRes = ca::core::tag_cast::cast<mjbase::model::ClassResource>(raw);
        if (clsRes == nullptr) return;
        clsRes->set(std::move(encoded).unwrap());
        dirty.uris.emplace_back(entry.uri);
        dirty.sites += hit;
    });
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

}   // namespace grunt::string_equals_restore
