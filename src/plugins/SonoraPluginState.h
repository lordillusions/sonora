#pragma once
// COL-4c: Sonora's own VST3s (PLG-11 Prism, PLG-12 Curve LFO, PLG-14 Hush) inside a set. Their class ids follow JUCE's
// scheme {0xABCDEF01, 0x9182FAEB, 'Snra', plug-in code}; their saved state is the plug-in's own bytes followed by JUCE's
// private-data trailer. A computer without the VST3 (a collab guest, a set made elsewhere) plays Prism and Curve LFO with
// the built-in devices from that state (DeviceFactory); Hush since COL-4e (its state body is HushSession's bytes).
#include "core/Model.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sonora::devices { class CurveLfo; class Kickstract; }
namespace sonora::plugins
{
enum class SonoraPlugin { None, Prism, CurveLfo, Hush, Kickstract };

// Which Sonora VST3 this plug-in reference is, by class id only.
SonoraPlugin sonoraPluginFor(const model::PluginInfo& info, model::DeviceKind kind);

// The plug-in's own bytes inside a JUCE VST3 state (the JUCEPrivateData trailer removed); `state` when it has none.
std::span<const uint8_t> juceVst3StateBody(std::span<const uint8_t> state);

// Curve LFO state: every control by its id (unknown ids are skipped, missing ones keep their defaults). decode applies
// to `lfo` only when the whole state could be read. Shared by the VST3 and the built-in stand-in.
std::vector<uint8_t> encodeCurveLfoState(const devices::CurveLfo& lfo);
bool decodeCurveLfoState(const void* data, size_t size, devices::CurveLfo& lfo, std::string& error);

// PLG-17 Kickstract state: every parameter by its id, like Curve LFO's. Shared by the VST3, the .als compatibility
// wrapper and the built-in stand-in for a missing VST3.
std::vector<uint8_t> encodeKickstractState(const devices::Kickstract& device);
bool decodeKickstractState(const void* data, size_t size, devices::Kickstract& device, std::string& error);
} // namespace sonora::plugins
