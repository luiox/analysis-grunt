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
