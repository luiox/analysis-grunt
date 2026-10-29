/// @file        pipeline_adapter.cpp
/// @brief       `grunt.ConstantCall` 的管线入口：三段编排 + trace。
#include "grunt/pipeline_adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "grunt/constant_call.hpp"
#include "grunt/const_method.hpp"
#include "grunt/container.hpp"
#include "grunt/flags_strip.hpp"

namespace grunt {

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
        std::fprintf(stderr, "[grunt.ConstMethod] scan: sites=%zu rejects=%zu\n",
                     scanned.sites.size(), scanned.rejects.size());
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const const_method::DirtySet dirty = const_method::rewrite(container, scanned);
    const const_method::ProofResult proof =
        const_method::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ConstMethod] rewrite: classes=%zu applied=%zu/%zu skipped=%zu proof=%s\n",
                     dirty.uris.size(), dirty.applied, dirty.attempted,
                     dirty.skippedByRoundTrip, proof.ok ? "ok" : "FAILED");
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

}   // namespace grunt
