#pragma once
// DEV-18d: observed Serum FX structure and measured scalar adapter, load/message thread only.
// Native algorithms remain independently authored. Retain NativeContent source bytes separately;
// this adapter never rewrites the source or treats successful parsing as audio equivalence.

#include "core/Model.h"
#include "devices/native/WavetableEffects.h"
#include "plugins/SerumState.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::plugins
{
struct SerumEffectAddress
{
    int rack = 0, flatIndex = 0, vendorAddress = 0;
    devices::WavetableEffects::ModuleId nativeId = 0;
    devices::WavetableEffects::Type type = devices::WavetableEffects::Type::Unknown;
};

struct SerumEffectsImportResult
{
    devices::WavetableEffects::Configuration configuration;
    std::vector<SerumEffectAddress> addresses;
    std::vector<std::string> warnings;
    bool hasRacks = false, completeFeatureSupport = false;
    // 100*rack+flatIndex is observed only for flatIndex<100. Ambiguous/unimported targets
    // intentionally return no mapping rather than selecting an unrelated native module.
    std::optional<devices::WavetableEffects::ModuleId> nativeModuleId(int vendorAddress) const;
};

// Three FXRackN/FX lists; Split3 counts delimit subsequent flat low/mid/high spans.
// Malformed/unknown entries and missing assets warn nonfatally, preserving ordered valid entries.
// Limits follow the native graph (128 modules, 16 roots/rack, 8 roots/band, depth3), with at most
// 128 inspected entries per rack and 64 MiB unique original IR PCM. No vendor defaults inferred.
SerumEffectsImportResult parseSerumEffects(const SerumValue& root, const std::string& sourceDirectory = {},
                                           double sampleRate = 44100);
// Asset lookup uses the immutable preserved source origin; source/controller bytes are unchanged.
inline SerumEffectsImportResult parseSerumEffects(const SerumValue& root, const model::NativeContent& origin,
                                                  double sampleRate = 44100)
{
    return parseSerumEffects(root, origin.sourceDirectory, sampleRate);
}

// Modulation-matrix FX targets (measured: the host adds amount x source to the control's normalized position, clamps it
// to 0..1 and applies the control's own normalized->plain law).
// The table samples that position at `points` evenly spaced values and records every native control of the module the
// adapter above derives from it, so the synth can follow the route without re-running the adapter on the audio thread.
struct SerumEffectModulation
{
    struct Curve { int parameter = -1; std::vector<float> values; };
    devices::WavetableEffects::ModuleId module = 0;
    float base = 0; // normalized position of the stored (or measured default) value
    std::vector<Curve> curves;
};
// True when the field's normalized law is measured for this module key ("FXDelay", ...).
bool serumEffectFieldModulatable(std::string_view moduleKey, std::string_view field);
// nullopt: unmeasured law, missing/mismatched entry, unknown stored default or no native control follows the field.
std::optional<SerumEffectModulation> serumEffectModulation(const SerumValue& root, int vendorAddress, std::string_view moduleKey,
                                                           std::string_view field, int points = 1025, double sampleRate = 44100);
} // namespace sonora::plugins
