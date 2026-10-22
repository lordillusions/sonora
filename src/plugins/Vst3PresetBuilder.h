#pragma once
// Builds an in-memory .vstpreset from the raw component/controller state stored in .als
// (<Vst3Preset><ProcessorState>/<ControllerState>). JUCE can load a .vstpreset into a
// hosted VST3 via AudioPluginInstance::getVST3Client()->setPreset().
//
// .vstpreset layout (Steinberg VST3 SDK, public/vstpresetfile.h), all little-endian:
//   header:  "VST3" | int32 version(=1) | char[32] classID (ASCII hex) | int64 chunkListOffset
//   chunks:  raw "Comp" bytes, raw "Cont" bytes
//   list:    "List" | int32 count | { char[4] id | int64 offset | int64 size } * count

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "core/Model.h"

namespace sonora::plugins
{

// Live stores the 16-byte class id as four int32 fields; the ASCII form is "%08X%08X%08X%08X".
std::string vst3ClassIdString(const std::array<uint32_t, 4>& uid);

std::vector<uint8_t> buildVstPreset(const std::array<uint32_t, 4>& uid,
                                    const std::vector<uint8_t>& componentState,
                                    const std::vector<uint8_t>& controllerState);
// Bounded decoder for the preset returned by JUCE's VST3 client. On failure leaves info unchanged.
bool readVstPreset(const std::vector<uint8_t>& preset, model::PluginInfo& info);

} // namespace sonora::plugins
