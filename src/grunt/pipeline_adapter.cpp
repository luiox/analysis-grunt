/// @file        pipeline_adapter.cpp
/// @brief       `grunt.ConstantCall` 的管线入口：三段编排 + trace。
#include "grunt/pipeline_adapter.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "grunt/booger_anno_strip.hpp"
#include "grunt/const_value_promote.hpp"
#include "grunt/constant_call.hpp"
#include "grunt/const_method.hpp"
#include "grunt/container.hpp"
#include "grunt/dead_code.hpp"
#include "grunt/dead_member.hpp"
#include "grunt/cff_unflatten.hpp"
#include "grunt/dispatcher_restore.hpp"
#include "grunt/exc_bridge_restore.hpp"
#include "grunt/field_access_proxy.hpp"
#include "grunt/flags_strip.hpp"
#include "grunt/gate_processor_fold.hpp"
#include "grunt/indy_decode.hpp"
#include "grunt/indy_fold.hpp"
#include "grunt/indy_restore.hpp"
#include "grunt/knowledge/kasumi_names.hpp"
#include "grunt/material_key_restore.hpp"
#include "grunt/metadata_strip.hpp"
#include "grunt/number_fold.hpp"
#include "grunt/predicate_fold.hpp"
#include "grunt/proof_toolbox.hpp"
#include "grunt/proxy_inline.hpp"
#include "grunt/seed_strip.hpp"
#include "grunt/string_cache_fold.hpp"
#include "grunt/string_pool_fold.hpp"
#include "grunt/trampoline_squeeze.hpp"

#include "mjbase/jar/jar_reader.hpp"

namespace grunt {

const char* gActivePass = nullptr;
std::string gKasumiJdkPath;
std::string gKasumiTsvPath;

namespace {

bool traceEnabled()
{
    static const bool enabled = std::getenv("GRUNT_TRACE") != nullptr;
    return enabled;
}

constexpr ca::u16 kAccInterface = 0x0200;   ///< class access_flags 的 ACC_INTERFACE

/// @brief 跳过常量池取 access_flags：类文件结构 magic(4)+minor(2)+major(2) 后
/// 是 cp_count(u2)+cp[]+access_flags(u2)，逐条按 tag 步进；未知 tag 放弃该类。
bool classAccessSkipCp(const std::vector<ca::u8>& data, ca::u16& access)
{
    if (data.size() < 10) return false;
    const ca::u8*   p       = data.data();
    ca::usize       off     = 8;   // magic + minor + major
    const ca::usize cpCount = (static_cast<ca::usize>(p[off]) << 8) | p[off + 1];
    off += 2;
    for (ca::usize i = 1; i < cpCount && off < data.size(); ++i) {
        const ca::u8 tag = p[off++];
        switch (tag) {
        case 1:   // Utf8
            if (off + 2 > data.size()) return false;
            off += 2 + ((static_cast<ca::usize>(p[off]) << 8) | p[off + 1]);
            break;
        case 3:   // Integer
        case 4:   // Float
            off += 4;
            break;
        case 5:   // Long（占 2 槽）
        case 6:   // Double
            off += 8;
            ++i;
            break;
        case 7:   // Class
        case 8:   // String
        case 16:  // MethodType
        case 19:  // Module
        case 20:  // Package
            off += 2;
            break;
        case 9:    // Fieldref
        case 10:   // Methodref
        case 11:   // InterfaceMethodref
        case 12:   // NameAndType
        case 17:   // Dynamic
        case 18:   // InvokeDynamic
            off += 4;
            break;
        case 15:  // MethodHandle
            off += 3;
            break;
        default: return false;   // 未知 tag，放弃此 class
        }
    }
    if (off + 2 > data.size()) return false;
    access = static_cast<ca::u16>((static_cast<ca::u16>(p[off]) << 8) | p[off + 1]);
    return true;
}

}   // namespace

std::unordered_set<std::string> scanJdkInterfaces(const std::string& jdkPath)
{
    std::unordered_set<std::string> interfaces;
    if (jdkPath.empty()) return interfaces;

    namespace fs = std::filesystem;
    const fs::path homeDir(jdkPath);
    // JDK 9+ 用 jmods/，JDK 8 用 lib/（rt.jar 之类）。
    std::vector<fs::path> archives;
    const fs::path        jmods = homeDir / "jmods";
    const fs::path        lib   = homeDir / "lib";
    const auto collect = [&](const fs::path& dir) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) return;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".jmod" || ext == ".jar" || ext == ".zip") archives.push_back(it->path());
        }
    };
    collect(jmods);
    if (archives.empty()) collect(lib);
    if (archives.empty()) return interfaces;

    for (const fs::path& archive : archives) {
        try {
            mjbase::jar::JvmJarReader reader(
                ca::str::Utf8StringRef::from_cstr(archive.string().c_str()));
            (void)reader.read([&](const ca::str::Utf8String& name,
                                  const std::vector<ca::u8>& data) {
                std::string internal = name.to_std_string();
                // jmod 条目名带 "classes/" 前缀，去掉得到标准内部名。
                constexpr std::string_view kClasses = "classes/";
                if (internal.size() >= kClasses.size()
                    && internal.compare(0, kClasses.size(), kClasses) == 0) {
                    internal.erase(0, kClasses.size());
                }
                if (internal.size() <= 6
                    || internal.compare(internal.size() - 6, 6, ".class") != 0) {
                    return;
                }
                if (internal.find("module-info") != std::string::npos) return;
                ca::u16 access = 0;
                if (!classAccessSkipCp(data, access)) return;
                if ((access & kAccInterface) != 0) {
                    interfaces.insert(internal.substr(0, internal.size() - 6));
                }
            });
        } catch (const std::exception&) {
            // 跳过无法读取的归档。
        }
    }
    return interfaces;
}

bool readTextFile(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

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
        if (scanned.censusIncomplete) {
            std::fprintf(stderr, "[grunt.ConstValuePromote] census-incomplete：整轮放弃\n");
        }
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
        if (scanned.censusIncomplete) {
            std::fprintf(stderr, "[grunt.SeedStrip] census-incomplete：整轮放弃\n");
        }
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
                     " clinits=%zu compacted=%zu shells=%zu ref-rejected=%zu"
                     " census-raw=%zu census-blind=%zu proof=%s\n",
                     dirty.uris.size(), dirty.methods, dirty.fields, dirty.clinits,
                     dirty.compacted, dirty.classes, dirty.rejectedByRef,
                     dirty.censusRawSources, dirty.censusBlind,
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
            std::fprintf(stderr,
                         "[grunt.BoogerAnnoStrip] gate: Booger 仍有 CP 引用或引用普查不完整，跳过\n");
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

mjbase::spi::PreservedAnalyses IndyDecodePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // Era 探测（adapter 调一次，结果注入 scan）+ JDK 接口旁证（Era=Kasumi 才
    // 激活文件系统扫描；残留本体不做遍历）。
    indy_decode::ScanInput input;
    input.era = knowledge::probeEra(container);
    if (input.era == knowledge::Era::Kasumi && !gKasumiJdkPath.empty()) {
        input.jdkInterfaces = scanJdkInterfaces(gKasumiJdkPath);
    }

    const indy_decode::ScanResult scanned = indy_decode::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.IndyDecode] scan: era=%s jdk-ifaces=%zu total=%zu sites=%zu"
                     " rejects=%zu (era=%zu key=%zu malformed=%zu decode=%zu desc=%zu"
                     " iface=%zu) skip-classes=%zu\n",
                     input.era == knowledge::Era::Kasumi     ? "Kasumi"
                     : input.era == knowledge::Era::Grunt2   ? "Grunt2"
                     : input.era == knowledge::Era::Grunt3   ? "Grunt3"
                                                             : "Unknown",
                     input.jdkInterfaces.size(), scanned.totalIndy, scanned.sites.size(),
                     scanned.rejects.size(), scanned.eraBlocked, scanned.keyMissing,
                     scanned.malformedArguments, scanned.decodeFailed, scanned.descMismatch,
                     scanned.interfaceUnattested, scanned.classesSkipped);
        if (std::getenv("GRUNT_INDY_DECODE_TRACE") != nullptr) {
            for (const indy_decode::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.IndyDecode]   %s#%zu pc=%u form=%d → %s.%s%s%s\n",
                             site.uri.c_str(), site.methodIndex, site.indySrcPc,
                             static_cast<int>(site.form), site.targetOwner.c_str(),
                             site.targetName.c_str(),
                             site.ownerIsInterface ? " (iface)" : "",
                             site.targetDesc.c_str());
            }
            for (const indy_decode::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.IndyDecode]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.indySrcPc, rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const indy_decode::DirtySet dirty = indy_decode::rewrite(container, scanned);
    const indy_decode::ProofResult verdict = indy_decode::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.IndyDecode] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu (static=%zu virtual=%zu getfield=%zu putfield=%zu"
                     " getstatic=%zu putstatic=%zu new=%zu) proof=%s residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     dirty.rewrittenStatic, dirty.rewrittenVirtual, dirty.rewrittenGetField,
                     dirty.rewrittenPutField, dirty.rewrittenGetStatic, dirty.rewrittenPutStatic,
                     dirty.rewrittenNew, verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.IndyDecode] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses StringCacheFoldPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // Era 探测（adapter 调一次，结果注入 scan）+ TSV 明文旁证（Era=Kasumi 才
    // 激活文件读取；残留本体零文件系统副作用，解析走 parseTsv 纯函数面）。
    string_cache_fold::ScanInput input;
    input.era = knowledge::probeEra(container);
    if (input.era == knowledge::Era::Kasumi && !gKasumiTsvPath.empty()) {
        std::string content;
        if (readTextFile(gKasumiTsvPath, content)) {
            (void)string_cache_fold::parseTsv(content, input.plaintexts);
            input.tsvState = string_cache_fold::TsvState::Loaded;
        } else {
            input.tsvState = string_cache_fold::TsvState::Unreadable;
        }
    }

    const string_cache_fold::ScanResult scanned =
        string_cache_fold::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.StringCacheFold] scan: era=%s tsv=%s total-shape=%zu sites=%zu"
                     " rejects=%zu (era=%zu tsv=%zu no-plain=%zu) skip-classes=%zu\n",
                     input.era == knowledge::Era::Kasumi ? "Kasumi" : "Unknown",
                     input.tsvState == string_cache_fold::TsvState::Loaded ? "loaded"
                     : input.tsvState == string_cache_fold::TsvState::Unreadable
                         ? "unreadable"
                         : "absent",
                     scanned.totalShapeHits, scanned.sites.size(), scanned.rejects.size(),
                     scanned.eraBlocked, scanned.tsvBlocked, scanned.noPlaintext,
                     scanned.classesSkipped);
        if (std::getenv("GRUNT_STRING_CACHE_FOLD_TRACE") != nullptr) {
            for (const string_cache_fold::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.StringCacheFold]   %s#%zu pc=%u form=%d %s.%s[%d]"
                             " → \"%s\"\n",
                             site.uri.c_str(), site.methodIndex, site.anchorSrcPc,
                             static_cast<int>(site.form), site.owner.c_str(),
                             site.fieldKey.c_str(), site.index,
                             site.plaintext.c_str());
            }
            for (const string_cache_fold::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.StringCacheFold]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.anchorSrcPc,
                             rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const string_cache_fold::DirtySet dirty =
        string_cache_fold::rewrite(container, scanned);
    const string_cache_fold::ProofResult verdict =
        string_cache_fold::proof(container, dirty, before, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.StringCacheFold] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu (cache-read=%zu direct-decrypt=%zu) proof=%s"
                     " residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     dirty.rewrittenCacheReads, dirty.rewrittenDirectDecrypt,
                     verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.StringCacheFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses StringPoolFoldPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（池链结构 + 解密器窗口），Era 不
    // 消费——v2/v3 共用全部形态常量，形态内无版本敏感判据（缺口 #15 不在此）。
    const string_pool_fold::ScanInput input;
    const string_pool_fold::ScanResult scanned = string_pool_fold::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.StringPoolFold] scan: pool-classes=%zu triple-hits=%zu"
                     " sites=%zu rejects=%zu (arrayinit=%zu decryptor=%zu idx-miss=%zu)"
                     " skip-classes=%zu\n",
                     scanned.poolClasses, scanned.tripleHits, scanned.sites.size(),
                     scanned.rejects.size(), scanned.arrayInitBlocked,
                     scanned.decryptorBlocked, scanned.idxMissing, scanned.classesSkipped);
        if (std::getenv("GRUNT_STRING_POOL_FOLD_TRACE") != nullptr) {
            for (const string_pool_fold::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.StringPoolFold]   %s#%zu pc=%u form=%d %s.%s[%d]"
                             " → \"%s\"\n",
                             site.uri.c_str(), site.methodIndex, site.anchorSrcPc,
                             static_cast<int>(site.form), site.owner.c_str(),
                             site.fieldName.c_str(), site.index, site.plaintext.c_str());
            }
            for (const string_pool_fold::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.StringPoolFold]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.anchorSrcPc,
                             rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const string_pool_fold::DirtySet dirty = string_pool_fold::rewrite(container, scanned);
    const string_pool_fold::ProofResult verdict =
        string_pool_fold::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.StringPoolFold] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu (literal=%zu arrayed=%zu) proof=%s residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     dirty.foldedLiteral, dirty.foldedArrayed, verdict.ok ? "ok" : "FAILED",
                     verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.StringPoolFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses IndyRestorePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // Era 探测（adapter 调一次，结果注入 scan）+ JDK 接口旁证（Era 非
    // Unknown 才激活文件系统扫描——simple 面自证不挂 Era，但旁证只在家族
    // 判定后注入；残留本体不做遍历）。
    indy_restore::ScanInput input;
    input.era = knowledge::probeEra(container);
    if (input.era != knowledge::Era::Unknown && !gKasumiJdkPath.empty()) {
        input.jdkInterfaces = scanJdkInterfaces(gKasumiJdkPath);
    }

    const indy_restore::ScanResult scanned = indy_restore::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.IndyRestore] scan: era=%s jdk-ifaces=%zu total=%zu sites=%zu"
                     " rejects=%zu (key=%zu malformed=%zu decode=%zu desc=%zu magic=%zu"
                     " metadata=%zu material=%zu iface=%zu) skip-classes=%zu\n",
                     input.era == knowledge::Era::Kasumi     ? "Kasumi"
                     : input.era == knowledge::Era::Grunt2   ? "Grunt2"
                     : input.era == knowledge::Era::Grunt3   ? "Grunt3"
                                                             : "Unknown",
                     input.jdkInterfaces.size(), scanned.totalIndy, scanned.sites.size(),
                     scanned.rejects.size(), scanned.keyMissing, scanned.malformedArguments,
                     scanned.decodeFailed, scanned.descMismatch, scanned.magicMismatch,
                     scanned.metadataMissing, scanned.materialLane,
                     scanned.interfaceUnattested, scanned.classesSkipped);
        if (std::getenv("GRUNT_INDY_RESTORE_TRACE") != nullptr) {
            for (const indy_restore::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.IndyRestore]   %s#%zu pc=%u form=%d → %s.%s%s%s\n",
                             site.uri.c_str(), site.methodIndex, site.indySrcPc,
                             static_cast<int>(site.form), site.targetOwner.c_str(),
                             site.targetName.c_str(),
                             site.ownerIsInterface ? " (iface)" : "",
                             site.targetDesc.c_str());
            }
            for (const indy_restore::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.IndyRestore]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.indySrcPc, rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const indy_restore::DirtySet dirty = indy_restore::rewrite(container, scanned);
    const indy_restore::ProofResult verdict = indy_restore::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.IndyRestore] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu (static=%zu virtual=%zu) proof=%s residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     dirty.rewrittenStatic, dirty.rewrittenVirtual,
                     verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.IndyRestore] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses MetadataStripPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize classesBefore   = countClasses(container);
    const ca::usize resourcesBefore = proof_tools::countResources(container);

    // Era 探测（adapter 调一次，结果注入 scan）：三面判据全是 Kasumi 版本
    // 知识，Era 未判整面关闭（fail-closed）。
    metadata_strip::ScanInput input;
    input.era = knowledge::probeEra(container);

    const metadata_strip::ScanResult scanned = metadata_strip::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.MetadataStrip] scan: era=%s classes=%zu annos=%zu fields=%zu"
                     " resources=%zu (candidates=%zu anno-blocked=%zu skip-classes=%zu)\n",
                     input.era == knowledge::Era::Kasumi ? "Kasumi" : "Unknown",
                     scanned.sites.size(), scanned.totalAnnos, scanned.totalFields,
                     scanned.resources.size(), scanned.resourceCandidates,
                     scanned.annoBlocked, scanned.classesSkipped);
        if (std::getenv("GRUNT_METADATA_STRIP_TRACE") != nullptr) {
            for (const metadata_strip::Site& site : scanned.sites) {
                std::fprintf(stderr, "[grunt.MetadataStrip]   %s annos=%zu fields=%zu\n",
                             site.uri.c_str(), site.annos, site.fields);
            }
            for (const std::string& res : scanned.resources) {
                std::fprintf(stderr, "[grunt.MetadataStrip]   resource %s\n", res.c_str());
            }
        }
    }
    if (scanned.sites.empty() && scanned.resources.empty()) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    const metadata_strip::DirtySet dirty = metadata_strip::rewrite(container, scanned);
    const metadata_strip::ProofResult verdict =
        metadata_strip::proof(container, dirty, classesBefore, resourcesBefore);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.MetadataStrip] rewrite: classes=%zu annos=%zu fields=%zu"
                     " resources=%zu failed=%zu anno-blocked=%zu candidates=%zu proof=%s"
                     " residual(a/f/r)=%zu/%zu/%zu\n",
                     dirty.classes, dirty.annos, dirty.fields, dirty.removedResources.size(),
                     dirty.failed, dirty.annoBlocked, dirty.resourceCandidates,
                     verdict.ok ? "ok" : "FAILED", verdict.residualAnnos,
                     verdict.residualFields, verdict.residualResources);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.MetadataStrip] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty() && dirty.removedResources.empty()) {
        return mjbase::spi::PreservedAnalyses::all();
    }
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses DispatcherRestorePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（键表结构 + 调用点形状），Era 不
    // 消费——v3-only 形态（grunt2 无此 transformer），形态锚自证家族。
    const dispatcher_restore::ScanInput input;
    const dispatcher_restore::ScanResult scanned =
        dispatcher_restore::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.DispatcherRestore] scan: dispatchers=%zu dispatcher-rejected=%zu"
                     " sites=%zu rejects=%zu (key-not-const=%zu key-not-in-table=%zu"
                     " pad-mismatch=%zu dispatcher-blocked=%zu) skip-classes=%zu\n",
                     scanned.dispatchers, scanned.dispatcherRejected, scanned.sites.size(),
                     scanned.rejects.size(), scanned.keyNotConst, scanned.keyNotInTable,
                     scanned.padMismatch, scanned.dispatcherBlocked, scanned.classesSkipped);
        if (std::getenv("GRUNT_DISPATCHER_RESTORE_TRACE") != nullptr) {
            for (const dispatcher_restore::Site& site : scanned.sites) {
                std::fprintf(stderr,
                     "[grunt.DispatcherRestore]   %s#%zu pc=%u key=%d pads=%u"
                     " → %s %s.%s%s\n",
                     site.uri.c_str(), site.methodIndex, site.callSrcPc, site.key,
                     site.padCount, site.target.isVirtual ? "virtual" : "static",
                             site.target.owner.c_str(), site.target.name.c_str(),
                             site.target.desc.c_str());
            }
            for (const dispatcher_restore::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.DispatcherRestore]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.anchorSrcPc,
                             rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const dispatcher_restore::DirtySet dirty =
        dispatcher_restore::rewrite(container, scanned);
    const dispatcher_restore::ProofResult verdict =
        dispatcher_restore::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.DispatcherRestore] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu (static=%zu virtual=%zu) proof=%s residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     dirty.rewrittenStatic, dirty.rewrittenVirtual,
                     verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.DispatcherRestore] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses CffUnflattenPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（state 槽 confinement + 岛形 +
    // 桥块形状），Era 不消费——v3-only 形态（v2 Controlflow 无 state 槽摊平），
    // 岛形自证家族；SSA 变体不同构（LookupSwitch + per-case stub + passArgs
    // 编组）挂账，processor 桥键静态不可解整岛保留（解锁路径记 doc）。
    const cff_unflatten::ScanInput input;
    const cff_unflatten::ScanResult scanned =
        cff_unflatten::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.CffUnflatten] scan: dispatchers=%zu islands-solvable=%zu"
                     " rejects=%zu (key-processor=%zu span-unsolvable=%zu foreign-ref=%zu"
                     " state-polluted=%zu shape-mismatch=%zu blocks-shared=%zu"
                     " exc-blocked=%zu) skip-classes=%zu\n",
                     scanned.dispatchers, scanned.islandsSolvable, scanned.rejects.size(),
                     scanned.keyProcessor, scanned.spanUnsolvable, scanned.foreignRef,
                     scanned.statePolluted, scanned.shapeMismatch, scanned.blocksShared,
                     scanned.excBlocked, scanned.classesSkipped);
        if (std::getenv("GRUNT_CFF_UNFLATTEN_TRACE") != nullptr) {
            for (const cff_unflatten::IslandPlan& plan : scanned.islands) {
                std::fprintf(stderr, "[grunt.CffUnflatten]   island %s#%zu pc=%u\n",
                             plan.uri.c_str(), plan.methodIndex, plan.headSrcPc);
            }
            for (const cff_unflatten::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.CffUnflatten]   reject %s#%zu pc=%u: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.anchorSrcPc,
                             rej.why.c_str());
            }
        }
    }
    if (scanned.islands.empty()) return mjbase::spi::PreservedAnalyses::all();

    const cff_unflatten::DirtySet dirty =
        cff_unflatten::rewrite(container, scanned);
    const cff_unflatten::ProofResult verdict =
        cff_unflatten::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.CffUnflatten] rewrite: classes=%zu islands=%zu scrambles=%zu"
                     " entry-restored=%zu entry-trampoline=%zu failed=%zu"
                     " roundtrip-skip=%zu proof=%s residual=%zu\n",
                     dirty.classes, dirty.islands, dirty.scrambles, dirty.entryRestored,
                     dirty.entryTrampoline, dirty.failed, dirty.skippedByRoundTrip,
                     verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.CffUnflatten] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses MaterialKeyRestorePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（材料初值链 + materialKey 体形 +
    // 写者普查 + S1 路由互证），Era 不消费（同 string_pool_fold 口径——形态
    // 内无版本敏感判据；家族锚只钉 simple BSM desc）。还原口径 = 清洁运行
    // 环境语义（doc/material-key-restore.md §边界）。
    const material_key_restore::ScanInput input;
    const material_key_restore::ScanResult scanned =
        material_key_restore::scan(container, input);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.MaterialKeyRestore] scan: material-classes=%zu sites=%zu"
                     " rejects=%zu (key-body=%zu init-blocked=%zu init-conflict=%zu"
                     " writer-shape=%zu writer-unguarded=%zu key-refuted=%zu"
                     " decryptor-ambiguous=%zu) skip-classes=%zu\n",
                     scanned.materialClasses, scanned.sites.size(), scanned.rejects.size(),
                     scanned.keyBodyMismatch, scanned.initBlocked, scanned.initConflict,
                     scanned.writerShape, scanned.writerUnguarded, scanned.keyRefuted,
                     scanned.decryptorAmbiguous, scanned.classesSkipped);
        if (std::getenv("GRUNT_MATERIAL_KEY_RESTORE_TRACE") != nullptr) {
            for (const material_key_restore::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.MaterialKeyRestore]   %s#%zu pc=%u raw=%d"
                             " material-key=%d verified=%zu\n",
                             site.uri.c_str(), site.decryptorMethodIndex, site.invokeSrcPc,
                             site.decryptorRawKey, site.materialKey, site.sitesVerified);
            }
            for (const material_key_restore::Reject& rej : scanned.rejects) {
                std::fprintf(stderr, "[grunt.MaterialKeyRestore]   reject %s#%zu: %s\n",
                             rej.uri.c_str(), rej.methodIndex, rej.why.c_str());
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const material_key_restore::DirtySet dirty =
        material_key_restore::rewrite(container, scanned);
    const material_key_restore::ProofResult verdict =
        material_key_restore::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.MaterialKeyRestore] rewrite: classes=%zu sites=%zu failed=%zu"
                     " roundtrip-skip=%zu proof=%s residual=%zu\n",
                     dirty.classes, dirty.sites, dirty.failed, dirty.skippedByRoundTrip,
                     verdict.ok ? "ok" : "FAILED", verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.MaterialKeyRestore] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses ExcBridgeRestorePass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（trap/handler 逐字形状钉 + 异常
    // 表互证门），Era 不消费——v3-only 形态（v2 Controlflow 无异常桥面）。
    const exc_bridge_restore::ScanResult scanned = exc_bridge_restore::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ExcBridgeRestore] scan: sites=%zu classes=%zu"
                     " rejects=%zu (shape=%zu covered=%zu handler-collide=%zu"
                     " x-invalid=%zu rebind=%zu) skip-classes=%zu\n",
                     scanned.sites.size(), scanned.bridgeClasses, scanned.rejects.size(),
                     scanned.shapeMiss, scanned.covered, scanned.handlerCollide,
                     scanned.xInvalid, scanned.rebindBlocked, scanned.classesSkipped);
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const exc_bridge_restore::DirtySet dirty =
        exc_bridge_restore::rewrite(container, scanned);
    const exc_bridge_restore::ProofResult verdict =
        exc_bridge_restore::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.ExcBridgeRestore] rewrite: classes=%zu methods=%zu"
                     " bridges=%zu pins=%zu insns=%zu failed=%zu proof=%s residual=%zu\n",
                     dirty.uris.size(), dirty.methods, dirty.bridges, dirty.pins,
                     dirty.insns, dirty.failed, verdict.ok ? "ok" : "FAILED",
                     verdict.residual);
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.ExcBridgeRestore] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

mjbase::spi::PreservedAnalyses GateProcessorFoldPass::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;

    const ca::usize before = countClasses(container);

    // 无旁证注入面：判据全为容器自证事实（(III)I ActionDesc 指纹 + 前窗逐
    // 字形 + 恒真裁决），Era 不消费（v3-only 形态，v2 Controlflow 无门点
    // processor 面）。
    const gate_processor_fold::ScanResult scanned = gate_processor_fold::scan(container);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.GateProcessorFold] scan: shape=%zu sites=%zu rejects=%zu"
                     " (target-outside=%zu not-static=%zu sync=%zu no-code=%zu"
                     " args=%zu forward=%zu tlr=%zu clinit=%zu eval-bad=%zu step-limit=%zu"
                     " value-kind=%zu not-true=%zu) skip-classes=%zu\n",
                     scanned.totalShape, scanned.sites.size(), scanned.rejects.size(),
                     scanned.targetOutside, scanned.targetNotStatic, scanned.targetSynchronized,
                     scanned.targetNoCode, scanned.argsNotConstant, scanned.forwardShape,
                     scanned.tlrWindow, scanned.classInitPresent, scanned.evalBadState,
                     scanned.evalStepLimit, scanned.valueKindUnsupported, scanned.evalNotTrue,
                     scanned.classesSkipped);
        if (std::getenv("GRUNT_GATE_PROCESSOR_FOLD_TRACE") != nullptr) {
            for (const gate_processor_fold::Site& site : scanned.sites) {
                std::fprintf(stderr,
                             "[grunt.GateProcessorFold]   %s#%zu pc=%u %s.%s%s"
                             " args=%d,%d,%d cmp=%d -> %d\n",
                             site.uri.c_str(), site.methodIndex, site.callSrcPc,
                             site.targetOwner.c_str(), site.targetName.c_str(),
                             site.targetDesc.c_str(),
                             site.args.size() > 0 ? static_cast<ca::i32>(site.args[0].intBits) : 0,
                             site.args.size() > 1 ? static_cast<ca::i32>(site.args[1].intBits) : 0,
                             site.args.size() > 2 ? static_cast<ca::i32>(site.args[2].intBits) : 0,
                             site.compareValue, site.result);
            }
            for (const gate_processor_fold::Reject& rej : scanned.rejects) {
                static const char* kWhy[] = {
                    "target-outside", "not-static", "synchronized", "no-code",
                    "args-not-const", "forward-shape", "tlr-window", "clinit-present",
                    "eval-bad", "step-limit", "value-kind", "not-true"};
                const ca::usize idx = static_cast<ca::usize>(rej.reason);
                std::fprintf(stderr, "[grunt.GateProcessorFold]   reject %s.%s pc=%u: %s\n",
                             rej.callerOwner.c_str(), rej.callerMethod.c_str(), rej.callSrcPc,
                             idx < sizeof(kWhy) / sizeof(kWhy[0]) ? kWhy[idx] : "?");
            }
        }
    }
    if (scanned.sites.empty()) return mjbase::spi::PreservedAnalyses::all();

    const gate_processor_fold::DirtySet dirty =
        gate_processor_fold::rewrite(container, scanned);
    const gate_processor_fold::ProofResult verdict =
        gate_processor_fold::proof(container, dirty, before);
    if (traceEnabled()) {
        std::fprintf(stderr,
                     "[grunt.GateProcessorFold] rewrite: classes=%zu sites=%zu/%zu failed=%zu"
                     " roundtrip-skip=%zu proof=%s residual=%zu\n",
                     dirty.uris.size(), dirty.sites, dirty.attempted, dirty.failed,
                     dirty.skippedByRoundTrip, verdict.ok ? "ok" : "FAILED", verdict.residual);
        static const char* kRwReason[] = {"ok", "branch-not-found", "region-mismatch",
                                          "verdict-changed", "label-blocked", "erase-fail"};
        for (ca::usize i = 0; i < dirty.failedByReason.size(); ++i) {
            if (dirty.failedByReason[i] == 0) continue;
            const char* name = i < sizeof(kRwReason) / sizeof(kRwReason[0]) ? kRwReason[i] : "?";
            std::fprintf(stderr, "[grunt.GateProcessorFold]   %-16s %zu\n", name,
                         dirty.failedByReason[i]);
        }
        for (const std::string& f : verdict.failed) {
            std::fprintf(stderr, "[grunt.GateProcessorFold] proof failed: %s\n", f.c_str());
        }
    }
    if (dirty.uris.empty()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace grunt
