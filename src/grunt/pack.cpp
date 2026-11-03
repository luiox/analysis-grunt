/// @file        pack.cpp
/// @brief       Grunt 家族 pack：manifest + pass 注册。
#include "grunt/pack.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "libca/core/result.hpp"

#include "grunt/pipeline_adapter.hpp"

namespace grunt {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

Result<void, std::string> registerAll(mjbase::spi::PassNameRegistry& registry)
{
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ConstantCall",
            [] { return std::make_unique<ConstantCallPass>(); },
            "Fold invokestatic sites whose arguments are all constants and whose target"
            " is a pure numeric method into a single constant load.");
        r.is_err()) {
        return Err(std::string("grunt.ConstantCall"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.FlagsStrip",
            [] { return std::make_unique<FlagsStripPass>(); },
            "Clear bogus ACC_SYNTHETIC/ACC_BRIDGE access flags on static members"
            " (Java 11+ non-enum classes only).");
        r.is_err()) {
        return Err(std::string("grunt.FlagsStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ConstMethod",
            [] { return std::make_unique<ConstMethodPass>(); },
            "Inline invokestatic calls whose target body is a single constant producer"
            " (primitive TYPE getters, int constants, null) into a constant load.");
        r.is_err()) {
        return Err(std::string("grunt.ConstMethod"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.TrampolineSqueeze",
            [] { return std::make_unique<TrampolineSqueezePass>(); },
            "NOP out gotos that jump to their sequential successor (obfuscator"
            " trampolines), keeping srcPc so downstream argument collection sees"
            " through.");
        r.is_err()) {
        return Err(std::string("grunt.TrampolineSqueeze"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.PredicateFold",
            [] { return std::make_unique<PredicateFoldPass>(); },
            "Statically decide branches and switches whose operands are all"
            " constant loads (decoy blocks become dead code).");
        r.is_err()) {
        return Err(std::string("grunt.PredicateFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.DeadCode",
            [] { return std::make_unique<DeadCodePass>(); },
            "Remove unreachable instruction sequences found by exception-table-aware"
            " control-flow reachability; dead exception ranges are dropped and"
            " endpoint anchors snapped forward to a live instruction.");
        r.is_err()) {
        return Err(std::string("grunt.DeadCode"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ConstValuePromote",
            [] { return std::make_unique<ConstValuePromotePass>(); },
            "Back-fill clinit's single constant putstatic into the field's"
            " ConstantValue attribute (plus ACC_FINAL).");
        r.is_err()) {
        return Err(std::string("grunt.ConstValuePromote"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.SeedStrip",
            [] { return std::make_unique<SeedStripPass>(); },
            "Erase int[] seed-table construction spans in <clinit> (NEWARRAY +"
            " PUTSTATIC head, GETSTATIC/IASTORE groups), pruning orphan SMT"
            " frames before lowering.");
        r.is_err()) {
        return Err(std::string("grunt.SeedStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.DeadMember",
            [] { return std::make_unique<DeadMemberPass>(); },
            "Delete zero-reference methods (pure-arith / synthetic+bridge /"
            " private gates), zero-reference private and orphan-array fields,"
            " and empty <clinit>s; compacts the constant pool first so the"
            " reference census is the live closure.");
        r.is_err()) {
        return Err(std::string("grunt.DeadMember"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.BoogerAnnoStrip",
            [] { return std::make_unique<BoogerAnnoStripPass>(); },
            "Strip @Booger runtime annotation entries once no class in the"
            " container references the Booger class anymore (decryption bodies"
            " folded away).");
        r.is_err()) {
        return Err(std::string("grunt.BoogerAnnoStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.IndyFold",
            [] { return std::make_unique<IndyFoldPass>(); },
            "Fold invokedynamic call sites whose bootstrap method resolves to a"
            " plain static/virtual target: run the BSM under libjsim, read the"
            " CallSite handle, and rewrite the site into a real invoke.");
        r.is_err()) {
        return Err(std::string("grunt.IndyFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.NumberFold",
            [] { return std::make_unique<NumberFoldPass>(); },
            "Restore hidden numeric constants: XOR-through-long int spans,"
            " base-32 Long.parseUnsignedLong long spans, and their"
            " intBitsToFloat/longBitsToDouble wrappers fold into single"
            " constant loads.");
        r.is_err()) {
        return Err(std::string("grunt.NumberFold"));
    }
    return Ok();
}

/// 家族 pack：manifest 声明构建戳与命名空间，注册委托 registerAll。
class GruntPack final : public mjbase::plugin::IPassPack
{
public:
    const mjbase::plugin::PackManifest& manifest() const override { return manifest_; }

    Result<void, mjbase::plugin::PackError> registerPasses(
        mjbase::spi::PassNameRegistry& registry, std::string_view config) override
    {
        static_cast<void>(config);
        if (Result<void, std::string> r = registerAll(registry); r.is_err()) {
            return Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, r.unwrap_err()));
        }
        return Ok();
    }

private:
    mjbase::plugin::PackManifest manifest_{
        "grunt", mjbase::plugin::buildCommit(),
        "Grunt obfuscation family deobfuscation (constant-call folding, flags strip,"
        " const-method inlining)."};
};

GruntPack& packInstance()
{
    static GruntPack pack;
    return pack;
}

}   // namespace

mjbase::plugin::IPassPack& grunt_pack()
{
    return packInstance();
}

}   // namespace grunt
