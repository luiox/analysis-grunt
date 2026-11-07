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
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.StringPoolFold",
            [] { return makeFailClosed<StringPoolFoldPass>("grunt.StringPoolFold"); },
            "Fold Grunt v2 StringEncrypt / v3 StringArrayedEncrypt string-pool"
            " read sites (getstatic String[] pool; const index; aaload) back into"
            " the decrypted ldc plaintext. The per-class pool chain (clinit"
            " allocation prelude, arrayInit store blocks, ([CJI)String decryptor"
            " with the class key burned in) is verified by structure before any"
            " rewrite; the supply chain itself stays intact (still referenced by"
            " the clinit prelude) for a dedicated pool-drain pass. No external"
            " evidence needed.");
        r.is_err()) {
        return Err(std::string("grunt.StringPoolFold"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.IndyRestore",
            [] { return makeFailClosed<IndyRestorePass>("grunt.IndyRestore"); },
            "Statically restore Grunt v2 InvokeDynamic / v3 ReferenceObfuscate"
            " invokedynamic call sites: resolve the per-class decryptor key via"
            " the three-route chain (same-name decryptor, BSM body-follow,"
            " name-family derivation for native BSM shells), decrypt the simple"
            " args (encOwner/encName/encDesc) arithmetically or scan the owner"
            " @Metadata arrays for the heavy magic pair, and swap the site in"
            " place to the real invokestatic/virtual/interface per isVirtual."
            " Feed a JDK root via --jdk so virtual sites on JDK interfaces"
            " dispatch to invokeinterface.");
        r.is_err()) {
        return Err(std::string("grunt.IndyRestore"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.DispatcherRestore",
            [] { return makeFailClosed<DispatcherRestorePass>("grunt.DispatcherRestore"); },
            "Statically restore Grunt v3 InvokeDispatcher call sites: recognize"
            " per-class tableswitch dispatcher methods by their head anchor"
            " (ILOAD key slot followed by a tableswitch whose default branches"
            " back to the method entry), rebuild the contiguous key-to-target"
            " table from each case's forwarding invoke, then swap each call"
            " site (padding nulls plus a constant access key followed by"
            " invokestatic of the dispatcher) in place back to the real"
            " invokestatic/virtual target. The dispatcher method and its table"
            " stay intact and become dead code for downstream DeadCode/"
            "DeadMember.");
        r.is_err()) {
        return Err(std::string("grunt.DispatcherRestore"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.CffUnflatten",
            [] { return makeFailClosed<CffUnflattenPass>("grunt.CffUnflatten"); },
            "Statically un-flatten Grunt v3 ControlflowFlattening (Flow IR"
            " variants): recognize per-method dispatcher islands ([ILOAD state;"
            " TABLESWITCH] blocks with contiguous shuffled key windows whose"
            " default targets the island's first real block), evaluate the"
            " constant-fed state spans on each incoming bridge (inline"
            " Add/Sub/Xor/Mul/Div/Math.max closed chains; processor-mode spans"
            " stay fail-closed since the key lives behind the key-processor"
            " trampoline), then rewrite each island: bridge predecessors jump"
            " straight to their real case targets (alias-face pruning sends"
            " leftover and out-of-table keys to the default target), stateless"
            " junk gates re-aim their branch arm via the unique constant-fed"
            " feeder, the entry bridge is erased and the original entry block"
            " moves back to the method head (trampoline fallback when it sits"
            " inside an exception range), exception handler bridges rebind to"
            " their bridge targets and end anchors on erased blocks shift"
            " forward. Entire islands stay untouched when any reference,"
            " shape, slot-confinement or exception edge fails to resolve.");
        r.is_err()) {
        return Err(std::string("grunt.CffUnflatten"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.MaterialKeyRestore",
            [] { return makeFailClosed<MaterialKeyRestorePass>("grunt.MaterialKeyRestore"); },
            "Statically fold the Grunt v3 RuntimeMaterial key back into the"
            " material decryptor window: evaluate the per-class material field"
            " init chain in clinit (parseUnsignedLong base-32 span or raw"
            " ldc2_w constant, poison constant corridor via a bounded int/long"
            " stack machine), verify the 9-insn materialKey() body and the"
            " perturbation writer census (suspicious-path windows must sit"
            " behind the anti-debug guard fingerprint), self-check the candidate"
            " key by decrypting every BSM-indy site routed to the decryptor,"
            " then swap the in-window INVOKESTATIC materialKey() call for an"
            " LDC of the folded key in place. The window becomes the plain"
            " folded-key shape IndyRestore already folds, so a second"
            " grunt.IndyRestore round closes the materialLane bucket;"
            " indy_restore itself stays byte-identical. Clean-run semantics"
            " only: anti-debug sabotage paths are out of deobf scope, and any"
            " unresolvable init chain, extra writer or guarded-shape miss keeps"
            " the whole class untouched.");
        r.is_err()) {
        return Err(std::string("grunt.MaterialKeyRestore"));
    }
    if (Result<void, mjbase::spi::RegistryError> r = registry.registerGlobalPass(
            "grunt.ExcBridgeRestore",
            [] { return makeFailClosed<ExcBridgeRestorePass>("grunt.ExcBridgeRestore"); },
            "Restore Grunt v3 ControlflowJump exception bridges (FlowExceptionBridge"
            " trampolines): pin the synthetic trap block verbatim (new"
            " java/lang/RuntimeException; dup; invokespecial <init>()V; athrow,"
            " protected range covering exactly those four insns, handler being"
            " pop; goto with no inbound field reference), cross-check the"
            " exception table (no other entry covers the trap head or handler,"
            " no other handler anchored on the bridge blocks, bridge target"
            " resolvable and outside every candidate victim set), then rewire"
            " every inbound edge straight to the real target (replaceAllUses plus"
            " a positional fall-through GOTO pin when the layout predecessor can"
            " fall into the trap head), delete the bridge exception entry, snap"
            " surviving entries' start/end anchors off the erased blocks, erase"
            " the six victim insns in one all-or-nothing batch and recompute"
            " frames Strictly. Chain before grunt.CffUnflatten: bridges sit on"
            " top of CFF islands, and removing them restores whole-form span"
            " gotos. Real catch (RuntimeException) regions never match the pin"
            " (user code shape, handler with exception-slot store) and any"
            " unresolved collision keeps the whole method untouched.");
        r.is_err()) {
        return Err(std::string("grunt.ExcBridgeRestore"));
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
