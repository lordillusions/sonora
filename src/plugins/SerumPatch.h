#pragma once
// DEV-18d: explicit native import. Retains the source; reports unsupported DSP rather than claiming parity.
#include "core/Model.h"
#include "plugins/SerumState.h"

#include <string>
#include <vector>

namespace sonora::devices { class WavetableSynth; }
namespace sonora::plugins
{
struct SerumPatchResult
{
    model::Device device;
    std::vector<std::string> warnings;
    std::string error;
    bool completeFeatureSupport = false;
    bool audioVerified = false;
    explicit operator bool() const { return error.empty(); }
};
// Observed processor/preset containers only; file/controller identities are checked separately.
SerumPatchResult importSerumPatch(std::span<const uint8_t> bytes, const std::string& sourceDirectory = {},
                                  std::span<const uint8_t> controller = {});
void prepareSerumAssets(devices::WavetableSynth&, const model::NativeContent&, std::vector<std::string>& warnings);
} // namespace sonora::plugins
