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
            [] { return makeFailClosed<ConstantCallPass>("grunt.ConstantCall"); },
            "Fold invokestatic sites whose arguments are all constants and whose target"
            " is a pure numeric method into a single constant load.");
        r.is_err()) {
        return Err(std::string("grunt.ConstantCall"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.FlagsStrip",
            [] { return makeFailClosed<FlagsStripPass>("grunt.FlagsStrip"); },
            "Clear bogus ACC_SYNTHETIC/ACC_BRIDGE access flags on static members"
            " (Java 11+ non-enum classes only).");
        r.is_err()) {
        return Err(std::string("grunt.FlagsStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ConstMethod",
            [] { return makeFailClosed<ConstMethodPass>("grunt.ConstMethod"); },
            "Inline invokestatic calls whose target body is a single constant producer"
            " (primitive TYPE getters, int constants, null) into a constant load.");
        r.is_err()) {
        return Err(std::string("grunt.ConstMethod"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.TrampolineSqueeze",
            [] { return makeFailClosed<TrampolineSqueezePass>("grunt.TrampolineSqueeze"); },
            "NOP out gotos that jump to their sequential successor (obfuscator"
            " trampolines), keeping srcPc so downstream argument collection sees"
            " through.");
        r.is_err()) {
        return Err(std::string("grunt.TrampolineSqueeze"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.PredicateFold",
            [] { return makeFailClosed<PredicateFoldPass>("grunt.PredicateFold"); },
            "Statically decide branches and switches whose operands are all"
            " constant loads (decoy blocks become dead code).");
        r.is_err()) {
        return Err(std::string("grunt.PredicateFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.DeadCode",
            [] { return makeFailClosed<DeadCodePass>("grunt.DeadCode"); },
            "Remove unreachable instruction sequences found by exception-table-aware"
            " control-flow reachability; dead exception ranges are dropped and"
            " endpoint anchors snapped forward to a live instruction.");
        r.is_err()) {
        return Err(std::string("grunt.DeadCode"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ConstValuePromote",
            [] { return makeFailClosed<ConstValuePromotePass>("grunt.ConstValuePromote"); },
            "Back-fill clinit's single constant putstatic into the field's"
            " ConstantValue attribute (plus ACC_FINAL).");
        r.is_err()) {
        return Err(std::string("grunt.ConstValuePromote"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.SeedStrip",
            [] { return makeFailClosed<SeedStripPass>("grunt.SeedStrip"); },
            "Erase int[] seed-table construction spans in <clinit> (NEWARRAY +"
            " PUTSTATIC head, GETSTATIC/IASTORE groups), pruning orphan SMT"
            " frames before lowering.");
        r.is_err()) {
        return Err(std::string("grunt.SeedStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.DeadMember",
            [] { return makeFailClosed<DeadMemberPass>("grunt.DeadMember"); },
            "Delete zero-reference methods (pure-arith / synthetic+bridge /"
            " private gates), zero-reference private and orphan-array fields,"
            " and empty <clinit>s; compacts the constant pool first so the"
            " reference census is the live closure.");
        r.is_err()) {
        return Err(std::string("grunt.DeadMember"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.BoogerAnnoStrip",
            [] { return makeFailClosed<BoogerAnnoStripPass>("grunt.BoogerAnnoStrip"); },
            "Strip @Booger runtime annotation entries once no class in the"
            " container references the Booger class anymore (decryption bodies"
            " folded away).");
        r.is_err()) {
        return Err(std::string("grunt.BoogerAnnoStrip"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.IndyFold",
            [] { return makeFailClosed<IndyFoldPass>("grunt.IndyFold"); },
            "Fold invokedynamic call sites whose bootstrap method resolves to a"
            " plain static/virtual target: run the BSM under libjsim, read the"
            " CallSite handle, and rewrite the site into a real invoke.");
        r.is_err()) {
        return Err(std::string("grunt.IndyFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.NumberFold",
            [] { return makeFailClosed<NumberFoldPass>("grunt.NumberFold"); },
            "Restore hidden numeric constants: XOR-through-long int spans,"
            " base-32 Long.parseUnsignedLong long spans, and their"
            " intBitsToFloat/longBitsToDouble wrappers fold into single"
            " constant loads.");
        r.is_err()) {
        return Err(std::string("grunt.NumberFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.FieldAccessProxy",
            [] { return makeFailClosed<FieldAccessProxyPass>("grunt.FieldAccessProxy"); },
            "Rewrite call sites of static field-access proxy methods (body is a plain"
            " argument walk plus a single field access) back to the real get/put field"
            " in place; inlined proxy bodies become dead code for downstream"
            " DeadMember/DeadCode.");
        r.is_err()) {
        return Err(std::string("grunt.FieldAccessProxy"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ProxyInline",
            [] { return makeFailClosed<ProxyInlinePass>("grunt.ProxyInline"); },
            "Retarget call sites of static proxy methods (body is a plain"
            " argument walk plus a single static/virtual invoke) back to the"
            " real target in place; inlined proxy bodies become dead code for"
            " downstream DeadMember/DeadCode.");
        r.is_err()) {
        return Err(std::string("grunt.ProxyInline"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.IndyDecode",
            [] { return makeFailClosed<IndyDecodePass>("grunt.IndyDecode"); },
            "Statically restore Kasumi obfuscator invokedynamic call sites:"
            " extract the per-class XOR decryptor key, decrypt the bootstrap"
            " static args (encOwner/encName/encDesc), and rewrite the site by"
            " the mode/extra table into invokestatic/invokevirtual/"
            "invokeinterface/getfield/putfield/getstatic/putstatic/new."
            " Feed a JDK root via --jdk so virtual sites on JDK interfaces"
            " dispatch to invokeinterface.");
        r.is_err()) {
        return Err(std::string("grunt.IndyDecode"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.StringCacheFold",
            [] { return makeFailClosed<StringCacheFoldPass>("grunt.StringCacheFold"); },
            "Fold Kasumi string-cache sites into constant loads: cache reads"
            " (getstatic String[]; idx; aaload) and direct-decrypt calls"
            " (getstatic byte[][]; idx; aaload; dup; arraylength; off;"
            " invokestatic ([BII)String) are replaced by the plaintext"
            " constant from an external TSV dump. Feed the dump via --tsv;"
            " family detection gates the evidence (fail-closed without it).");
        r.is_err()) {
        return Err(std::string("grunt.StringCacheFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.MetadataStrip",
            [] { return makeFailClosed<MetadataStripPass>("grunt.MetadataStrip"); },
            "Strip Kasumi obfuscator metadata residue: @dev/kasumi/internal/Metadata"
            " annotations on classes/fields/methods, watermark constant fields"
            " (PROTECTED BY Kasumi / Gruntpocalypse / kasumi_ prefix), and"
            " META-INF/Kasumi/*.md anti-LLM injection resources (content probes"
            " attested). Family detection gates everything (fail-closed); fields"
            " are deleted wholesale, method bodies are untouched.");
        r.is_err()) {
        return Err(std::string("grunt.MetadataStrip"));
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
