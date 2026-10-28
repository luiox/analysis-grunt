/// @file        pipeline_adapter.cpp
/// @brief       `grunt.ConstantCall` 的管线入口：三段编排 + trace。
#include "grunt/pipeline_adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "grunt/constant_call.hpp"
#include "grunt/container.hpp"

namespace grunt {

namespace {

bool traceEnabled()
{
    static const bool enabled = std::getenv("GRUNT_TRACE") != nullptr;
    return enabled;
}

ca::usize countClasses(const mjbase::model::ResourceContainer& container)
{
    ca::usize n = 0;
    forEachClass(container, [&](const ClassEntry&) { ++n; });
    return n;
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

}   // namespace grunt
