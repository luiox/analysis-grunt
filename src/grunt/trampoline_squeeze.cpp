/// @file        trampoline_squeeze.cpp
/// @brief       残留 `trampoline_squeeze` 的三段实现：SCAN / REWRITE / PROOF。
///
/// `Site{uri, methodIndex, srcPc, wide}` → `DirtySet{uris, squeezed,
/// attempted}`。SCAN：逐方法 promote 后按 order 遍历，goto 目标即顺序
/// 后继即收站点。REWRITE：按 uri 聚合 → 逐方法一次 promote，按 `srcPc`
/// 重定位站点，**复核判据**（同方法其他站点改写后布局可能变，句柄判据
/// 重推）→ 清指令后置 `NOP` + 保 `srcPc` → 逐方法 `lowerMethod`（失败整类
/// 放弃）→ `writeClass`。PROOF：`proof_tools::classesIntact`。
///
/// ## 为什么 NOP 而不是擦除
///
/// NOP 化零布局扰动：帧/行号锚不动，不触发 lower 的属性重定位失败路径。
/// 缩布局的收益留给 dead_code（非 indy 擦除）统一拿。目标在方法末尾之外
/// （`t.isEnd()`）不是蹦床，跳过。
#include "grunt/trampoline_squeeze.hpp"

#include <set>
#include <string>
#include <unordered_map>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "grunt/container.hpp"
#include "grunt/iter_budget.hpp"
#include "grunt/rewrite_toolbox.hpp"
#include "grunt/proof_toolbox.hpp"

namespace grunt::trampoline_squeeze {

namespace {

namespace ir = jbc::ir;

/// @brief 单方法 SCAN：收集目标即顺序后继的 goto/goto_w。
/// @return false = 类级迭代预算超限（调用方放弃该类剩余方法）。
bool scanMethod(ir::ClassSession& s, ca::usize methodIndex, const std::string& uri,
                ScanResult& result, iter_budget::ClassBudget& budget)
{
    if (!ir::hasCode(s, methodIndex)) return true;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return true;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return true;

    const std::vector<ir::InsnId>& order = cu->dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (!budget.tick()) return false;   ///< 逐位置计数：超限即停，类保守跳过
        const ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr) continue;
        if (in->op != jbc::core::Op::GOTO && in->op != jbc::core::Op::GOTO_W) continue;

        // 目标必须是布局顺序后继（工具箱单源 isFallthroughGoto：backing
        // 布局真值判据，order 相邻判据在真 jar 上漏站点——order 不保证
        // pc 升序，b.main 族蹦床全漏实证，2026-11-16 修）。
        if (!rewrite_tools::isFallthroughGoto(*cu, *in)) continue;

        result.sites.push_back(
            Site{uri, methodIndex, in->srcPc, in->op == jbc::core::Op::GOTO_W});
    }
    return true;
}

/// @brief 单方法 REWRITE：按 srcPc 重定位蹦床，原地置 NOP。
/// @return 命中数。
///
/// budget 逐站点按方法指令数计数（重定位是线性扫）；超限时剩余站点保守放弃。
ca::usize rewriteMethod(ir::ClassSession& s, ca::usize methodIndex,
                        const std::vector<const Site*>& sites, ca::usize& skippedByBudget,
                        iter_budget::ClassBudget& budget)
{
    if (!ir::hasCode(s, methodIndex)) return 0;
    auto promoted = ir::promoteMethod(s, methodIndex);
    if (promoted.is_err()) return 0;
    ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr) return 0;

    ca::usize hit   = 0;
    ca::usize tried = 0;   ///< 本方法已尝试的站点数（含复核失败的）
    for (const Site* site : sites) {
        // 逐站点按方法指令数计数：重定位 + 定位都是对 order 的线性扫。
        const std::vector<ir::InsnId>& order = cu->dom.order();
        if (!budget.tick(order.size())) {
            skippedByBudget += sites.size() - tried;   ///< 余下站点保守放弃
            return hit;
        }
        ++tried;
        for (const ir::InsnId id : order) {
            const ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            const bool isTrampoline = in->op == jbc::core::Op::GOTO
                                      || in->op == jbc::core::Op::GOTO_W;
            if (!isTrampoline || in->srcPc != site->srcPc) continue;
            // 复核判据（字节可能已被同方法其他站点改写挤压，不盲改）；
            // 布局真值单源 isFallthroughGoto（同 scan 修）。
            if (!rewrite_tools::isFallthroughGoto(*cu, *in)) break;

            // 目标标签残渣处置（2026-11-16，与 DeadCode 穿线同根因）：标签
            // 锚在落点（存活指令）上，NOP 化后蹦床不再引用它——再无引用者
            // 时它成为死标签，落在常量窗口中段会把下游折叠判据挡死。引用
            // 者清点 = 存活分支（此刻蹦床尚未 NOP，排除自身）+ 异常表。
            const ir::LabelId targetLbl = in->target;
            const ir::InsnId  landing   = [&] {
                auto resolved = cu->dom.resolveLabel(targetLbl);
                return resolved.is_err() ? ir::kNoInsn : resolved.unwrap().insn;
            }();

            auto slot = cu->dom.get(id);
            if (slot.is_err()) break;
            ir::Insn*      m     = slot.unwrap();
            const ca::u32  keepPc = m->srcPc;
            *m                    = ir::Insn{};
            m->op                 = jbc::core::Op::NOP;
            m->srcPc              = keepPc;

            if (landing == ir::kNoInsn || !cu->dom.validLabel(targetLbl)) {
                ++hit;
                break;   // 解析失败 / 标签已被其他站点处置：NOP 本身已等价
            }
            bool referenced = false;
            for (const ir::InsnId oid : order) {
                const ir::Insn* o = cu->dom.peek(oid);
                if (o != nullptr && o->target == targetLbl) {
                    referenced = true;
                    break;
                }
            }
            if (!referenced) {
                for (const ir::CodeUnit::Exc& e : cu->exc) {
                    if (e.start == targetLbl || e.end == targetLbl
                        || e.handler == targetLbl) {
                        referenced = true;
                        break;
                    }
                }
            }
            if (!referenced && cu->dom.destroyLabel(targetLbl).is_err()) break;
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

ScanResult scan(const mjbase::model::ResourceContainer& container,
                const iter_budget::ClassBudget& budgetTemplate)
{
    ScanResult result;
    ca::usize  named = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        auto session = openSession(*entry.bytes);
        if (!session) {
            ++result.classesSkipped;
            return;
        }
        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置计数
        ir::ClassSession&        s      = *session;
        const ir::ClassUnit&     unit   = s.unit();
        for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
            if (!scanMethod(s, mi, std::string(entry.uri), result, budget)) {
                ++result.classesSkippedByBudget;
                iter_budget::reportExhausted("grunt.TrampolineSqueeze", "scan",
                                             std::string(entry.uri), budget, named);
                return;   ///< 该类剩余方法不再扫（保守跳过）
            }
        }
    });
    return result;
}

// ============================================================================
// ==== REWRITE ===============================================================
// ============================================================================

DirtySet rewrite(mjbase::model::ResourceContainer& container, const ScanResult& result,
                 const iter_budget::ClassBudget& budgetTemplate)
{
    DirtySet dirty;

    // 站点按 uri 聚类（一遍建索引；逐类线性筛全站点的做法在万类大 jar 上是
    // classes × sites 的平方账）。
    std::unordered_map<std::string, std::vector<const Site*>> byUri;
    for (const Site& site : result.sites) byUri[site.uri].push_back(&site);

    struct PendingWrite
    {
        std::string           uri;
        std::vector<ca::u8>   bytes;
        ca::usize             hit = 0;
    };
    std::vector<PendingWrite> pending;

    ca::usize named = 0;
    forEachClass(container, [&](const ClassEntry& entry) {
        const std::string uri(entry.uri);
        const auto        found = byUri.find(uri);
        if (found == byUri.end()) return;
        const std::vector<const Site*>& sitesOf = found->second;

        auto session = openSession(*entry.bytes);
        if (!session) return;
        ir::ClassSession& s = *session;

        // 方法下标聚合（同方法多站点一次 promote）。
        std::set<ca::usize> methodIdxs;
        for (const Site* site : sitesOf) methodIdxs.insert(site->methodIndex);

        iter_budget::ClassBudget budget = budgetTemplate;   ///< 每类重置计数

        ca::usize hit             = 0;
        ca::usize skippedByBudget = 0;
        for (const ca::usize mi : methodIdxs) {
            dirty.attempted += 1;
            hit += rewriteMethod(s, mi, sitesOf, skippedByBudget, budget);
        }
        dirty.skippedByBudget += skippedByBudget;
        if (skippedByBudget > 0) {
            iter_budget::reportExhausted("grunt.TrampolineSqueeze", "rewrite", uri,
                                         budget, named);
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
