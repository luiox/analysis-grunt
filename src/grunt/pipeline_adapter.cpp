/// @file        pipeline_adapter.cpp
/// @brief       `grunt.ConstantCall` 的管线入口：三段编排 + trace。
#include "grunt/pipeline_adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "grunt/booger_anno_strip.hpp"
#include "grunt/const_value_promote.hpp"
#include "grunt/constant_call.hpp"
#include "grunt/const_method.hpp"
#include "grunt/container.hpp"
#include "grunt/dead_code.hpp"
#include "grunt/dead_member.hpp"
#include "grunt/field_access_proxy.hpp"
#include "grunt/flags_strip.hpp"
#include "grunt/indy_fold.hpp"
#include "grunt/number_fold.hpp"
#include "grunt/predicate_fold.hpp"
#include "grunt/proxy_inline.hpp"
#include "grunt/seed_strip.hpp"
#include "grunt/trampoline_squeeze.hpp"

namespace grunt {

const char* gActivePass = nullptr;

namespace {

bool traceEnabled()
{
    static const bool enabled = std::getenv("GRUNT_TRACE") != nullptr;
    return enabled;
}

}   // namespace

mjbase::spi::PreservedAnalyses ConstantCallPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 本残留自包含三段，不消费注册分析

    const ca::usize before = countClasses(container);
    const constant_call::ScanResult scanned = constant_call::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.ConstantCall] scan: sites=%zu rejects=%zu\n",
                     scanned.sites.size(), scanned.rejects.size());
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const constant_call::DirtySet dirty = constant_call::rewrite(container, scanned);
    const constant_call::ProofResult proof =
        constant_call::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ConstantCall] rewrite: classes=%zu applied=%zu/%zu skipped=%zu proof=%s\n",
                     dirty.uris.size(), dirty.applied, dirty.attempted,
                     dirty.skippedByRoundTrip, proof.ok ? "ok" : "FAILED");
        static const char* kRwReason[] = {"ok",     "call-not-found", "member-mismatch",
                                          "args-mismatch", "load-fail", "insert-fail",
                                          "label-blocked", "erase-fail"};
        for (ca::usize i = 0; i < dirty.failedByReason.size(); ++i) {
            if (dirty.failedByReason[i] == 0) continue;
            const char* name = i < sizeof(kRwReason) / sizeof(kRwReason[0]) ? kRwReason[i] : "?";
            std::fprintf(stderr, "[grunt.ConstantCall]   %-16s %zu\n", name,
                         dirty.failedByReason[i]);
        }
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.ConstantCall] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses FlagsStripPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 本残留自包含三段，不消费注册分析

    const ca::usize before = countClasses(container);

    const flags_strip::ScanResult scanned = flags_strip::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.FlagsStrip] scan: sites=%zu skipped-classes=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const flags_strip::DirtySet dirty = flags_strip::rewrite(container, scanned);
    const flags_strip::ProofResult proof =
        flags_strip::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.FlagsStrip] rewrite: classes=%zu members=%zu out-of-range=%zu proof=%s\n",
                     dirty.uris.size(), dirty.membersCleared, dirty.sitesOutOfRange,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.FlagsStrip] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses ConstMethodPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 本残留自包含三段，不消费注册分析

    const ca::usize before = countClasses(container);

    const const_method::ScanResult scanned = const_method::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.ConstMethod] scan: sites=%zu rejects=%zu budget-skip=%zu\n",
                     scanned.sites.size(), scanned.rejects.size(),
                     scanned.classesSkippedByBudget);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const const_method::DirtySet dirty = const_method::rewrite(container, scanned);
    const const_method::ProofResult proof =
        const_method::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ConstMethod] rewrite: classes=%zu applied=%zu/%zu skipped=%zu"
                     " budget-skip=%zu proof=%s\n",
                     dirty.uris.size(), dirty.applied, dirty.attempted,
                     dirty.skippedByRoundTrip, dirty.skippedByBudget,
                     proof.ok ? "ok" : "FAILED");
        static const char* kRwReason[] = {"ok", "call-not-found", "member-mismatch",
                                          "load-fail", "slot-fail"};
        for (ca::usize i = 0; i < dirty.failedByReason.size(); ++i) {
            if (dirty.failedByReason[i] == 0) continue;
            const char* name = i < sizeof(kRwReason) / sizeof(kRwReason[0]) ? kRwReason[i] : "?";
            std::fprintf(stderr, "[grunt.ConstMethod]   %-16s %zu\n", name,
                         dirty.failedByReason[i]);
        }
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.ConstMethod] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses TrampolineSqueezePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const trampoline_squeeze::ScanResult scanned = trampoline_squeeze::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.TrampolineSqueeze] scan: sites=%zu skipped=%zu budget-skip=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped,
                     scanned.classesSkippedByBudget);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const trampoline_squeeze::DirtySet dirty =
        trampoline_squeeze::rewrite(container, scanned);
    const trampoline_squeeze::ProofResult proof =
        trampoline_squeeze::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.TrampolineSqueeze] rewrite: classes=%zu squeezed=%zu"
                     " budget-skip=%zu proof=%s\n",
                     dirty.uris.size(), dirty.squeezed, dirty.skippedByBudget,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.TrampolineSqueeze] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses PredicateFoldPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const predicate_fold::ScanResult scanned = predicate_fold::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.PredicateFold] scan: sites=%zu skipped=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const predicate_fold::DirtySet dirty = predicate_fold::rewrite(container, scanned);
    const predicate_fold::ProofResult proof =
        predicate_fold::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.PredicateFold] rewrite: classes=%zu sites=%zu/%zu failed=%zu"
                     " roundtrip-skip=%zu proof=%s\n",
                     dirty.uris.size(), dirty.sites, dirty.attempted, dirty.failed,
                     dirty.skippedByRoundTrip, proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.PredicateFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses DeadCodePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const dead_code::ScanResult scanned = dead_code::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.DeadCode] scan: methods=%zu skipped=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const dead_code::DirtySet dirty = dead_code::rewrite(container, scanned);
    const dead_code::ProofResult proof = dead_code::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.DeadCode] rewrite: classes=%zu methods=%zu insns=%zu proof=%s\n",
                     dirty.uris.size(), dirty.methods, dirty.insns,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.DeadCode] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses ConstValuePromotePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const const_value_promote::ScanResult scanned =
        const_value_promote::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.ConstValuePromote] scan: fields=%zu skipped=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const const_value_promote::DirtySet dirty =
        const_value_promote::rewrite(container, scanned);
    const const_value_promote::ProofResult proof =
        const_value_promote::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ConstValuePromote] rewrite: classes=%zu promoted=%zu/%zu"
                     " failed=%zu proof=%s\n",
                     dirty.uris.size(), dirty.promoted, dirty.attempted, dirty.failed,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.ConstValuePromote] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses SeedStripPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const seed_strip::ScanResult scanned = seed_strip::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.SeedStrip] scan: fields=%zu skipped=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const seed_strip::DirtySet dirty = seed_strip::rewrite(container, scanned);
    const seed_strip::ProofResult proof = seed_strip::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.SeedStrip] rewrite: classes=%zu fields=%zu/%zu failed=%zu"
                     " proof=%s\n",
                     dirty.uris.size(), dirty.fields, dirty.attempted, dirty.failed,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.SeedStrip] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses DeadMemberPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const dead_member::ScanResult scanned = dead_member::scan(container);
    if (traceEnabled()) {
        ca::usize m = 0;
        ca::usize f = 0;
        for (const dead_member::ClassCand& c : scanned.classes) {
            m += c.methods.size();
            f += c.fields.size();
        }
        std::fprintf(stderr, "[grunt.DeadMember] scan: classes=%zu methods=%zu fields=%zu\n",
                     scanned.classes.size(), m, f);
    }
    if (scanned.classes.empty()) return mjbase::spi::PreservedAnalyses::all();

    const dead_member::DirtySet dirty = dead_member::rewrite(container, scanned);
    const dead_member::ProofResult proof = dead_member::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.DeadMember] rewrite: classes=%zu methods=%zu fields=%zu"
                     " clinits=%zu compacted=%zu shells=%zu ref-rejected=%zu proof=%s\n",
                     dirty.uris.size(), dirty.methods, dirty.fields, dirty.clinits,
                     dirty.compacted, dirty.classes, dirty.rejectedByRef,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.DeadMember] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses BoogerAnnoStripPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const booger_anno_strip::ScanResult scanned = booger_anno_strip::scan(container);
    if (traceEnabled()) {
        if (scanned.gateBlocked) {
            std::fprintf(stderr, "[grunt.BoogerAnnoStrip] gate: Booger 仍有 CP 引用，跳过\n");
        } else {
            std::fprintf(stderr, "[grunt.BoogerAnnoStrip] scan: classes=%zu\n",
                         scanned.sites.size());
        }
    }
    if (scanned.gateBlocked || scanned.sites.empty()) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    const booger_anno_strip::DirtySet dirty =
        booger_anno_strip::rewrite(container, scanned);
    const booger_anno_strip::ProofResult proof =
        booger_anno_strip::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.BoogerAnnoStrip] rewrite: classes=%zu annos=%zu proof=%s\n",
                     dirty.classes, dirty.annos, proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.BoogerAnnoStrip] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses IndyFoldPass::run(mjbase::model::ResourceContainer& container,
                                                mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const indy_fold::ScanResult scanned = indy_fold::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.IndyFold] scan: classes=%zu sites=%zu foldable=%zu bsmFailed=%zu"
                     " budget-skip=%zu\n",
                     scanned.classes, scanned.sites.size(), scanned.foldable, scanned.bsmFailed,
                     scanned.classesSkippedByBudget);
        if (std::getenv("GRUNT_INDY_FOLD_TRACE") != nullptr) {
            for (const indy_fold::Site& site : scanned.sites) {
                std::string verdict;
                if (site.foldable) {
                    verdict = site.target.owner + "." + site.target.name + site.target.desc;
                } else {
                    verdict = "skip: " + site.why;
                }
                std::fprintf(stderr, "[grunt.IndyFold]   %s#%zu pc=%zu %s%s → %s\n",
                             site.uri.c_str(), site.methodIndex, site.insnPc,
                             site.indyName.c_str(), site.indyDesc.c_str(), verdict.c_str());
            }
        }
    }
    if (scanned.foldable == 0) return mjbase::spi::PreservedAnalyses::all();

    const indy_fold::DirtySet dirty = indy_fold::rewrite(container, scanned);
    const indy_fold::ProofResult proof = indy_fold::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.IndyFold] rewrite: classes=%zu sites=%zu proof=%s\n",
                     dirty.classes, dirty.sites, proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.IndyFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses NumberFoldPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const number_fold::ScanResult scanned = number_fold::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.NumberFold] scan: sites=%zu skipped=%zu budget-skip=%zu\n",
                     scanned.sites.size(), scanned.classesSkipped,
                     scanned.classesSkippedByBudget);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const number_fold::DirtySet dirty = number_fold::rewrite(container, scanned);
    const number_fold::ProofResult proof =
        number_fold::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.NumberFold] rewrite: classes=%zu sites=%zu/%zu failed=%zu"
                     " roundtrip-skip=%zu budget-skip=%zu proof=%s\n",
                     dirty.uris.size(), dirty.sites, dirty.attempted, dirty.failed,
                     dirty.skippedByRoundTrip, dirty.skippedByBudget,
                     proof.ok ? "ok" : "FAILED");
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.NumberFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses ProxyInlinePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const proxy_inline::ScanResult scanned = proxy_inline::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr, "[grunt.ProxyInline] scan: proxies=%zu sites=%zu rejects=%zu\n",
                     scanned.proxiesFound, scanned.sites.size(), scanned.rejects.size());
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const proxy_inline::DirtySet dirty = proxy_inline::rewrite(container, scanned);
    const proxy_inline::ProofResult proof =
        proxy_inline::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ProxyInline] rewrite: classes=%zu sites=%zu/%zu proxies=%zu"
                     " skipped=%zu proof=%s\n",
                     dirty.uris.size(), dirty.applied, dirty.attempted, dirty.proxies,
                     dirty.skippedByRoundTrip, proof.ok ? "ok" : "FAILED");
        static const char* kRwReason[] = {"ok", "call-not-found", "member-mismatch", "slot-fail"};
        for (ca::usize i = 0; i < dirty.failedByReason.size(); ++i) {
            if (dirty.failedByReason[i] == 0) continue;
            const char* name = i < sizeof(kRwReason) / sizeof(kRwReason[0]) ? kRwReason[i] : "?";
            std::fprintf(stderr, "[grunt.ProxyInline]   %-16s %zu\n", name,
                         dirty.failedByReason[i]);
        }
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.ProxyInline] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}


mjbase::spi::PreservedAnalyses FieldAccessProxyPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);
    const field_access_proxy::ScanResult scanned = field_access_proxy::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.FieldAccessProxy] scan: proxies=%zu sites=%zu rejects=%zu\n",
                     scanned.proxiesFound, scanned.sites.size(), scanned.rejects.size());
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const field_access_proxy::DirtySet dirty = field_access_proxy::rewrite(container, scanned);
    const field_access_proxy::ProofResult proof =
        field_access_proxy::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.FieldAccessProxy] rewrite: classes=%zu sites=%zu/%zu proxies=%zu"
                     " skipped=%zu proof=%s\n",
                     dirty.uris.size(), dirty.applied, dirty.attempted, dirty.proxies,
                     dirty.skippedByRoundTrip, proof.ok ? "ok" : "FAILED");
        static const char* kRwReason[] = {"ok", "call-not-found", "member-mismatch", "slot-fail"};
        for (ca::usize i = 0; i < dirty.failedByReason.size(); ++i) {
            if (dirty.failedByReason[i] == 0) continue;
            const char* name = i < sizeof(kRwReason) / sizeof(kRwReason[0]) ? kRwReason[i] : "?";
            std::fprintf(stderr, "[grunt.FieldAccessProxy]   %-16s %zu\n", name,
                         dirty.failedByReason[i]);
        }
        for (const std::string& f : proof.failed) {
            std::fprintf(stderr, "[grunt.FieldAccessProxy] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace grunt
