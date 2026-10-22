#pragma once
// Sonora Prism presets (UI-50): the instrument's own `.prism` file, the same bytes as the VST3's saved state. A preset is
// the device model of a SonoraWavetable device: its controls, plain values and native content (tables, media, curves,
// effects), so it restores exactly what the instrument plays. Message/load thread only.
//
// File layout: "PRSM", a little-endian uint32 version, then a juce::ValueTree stream.

#include "core/Model.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sonora::plugins
{
inline constexpr const char* prismPresetExtension = ".prism";
// Open dialogs: the own format first; the other instrument presets loadPrismPreset() understands are accepted too.
inline constexpr const char* prismPresetPatterns = "*.prism;*.SerumPreset;*.vstpreset";

std::vector<uint8_t> encodePrismPreset(const model::Device&);
// Restores onto a fresh SonoraWavetable model (controls the file lacks keep their defaults). `device.id` is kept.
bool decodePrismPreset(std::span<const uint8_t> bytes, model::Device& device, std::string& error);
bool isPrismPreset(std::span<const uint8_t> bytes);

struct PrismPresetLoad
{
    model::Device device;
    std::vector<std::string> warnings;
    std::string error;
    explicit operator bool() const { return error.empty(); }
};
// A `.prism` file, or another instrument preset the importer understands. The device is named after the file.
PrismPresetLoad loadPrismPreset(const juce::File&);
bool savePrismPreset(const model::Device&, const juce::File&, std::string& error);

// ~/Music/Sonora/Prism, with Presets/ (banks and groups as folders) and Tables/. Created on first save.
juce::File prismLibraryRoot();
} // namespace sonora::plugins
