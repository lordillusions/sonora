#pragma once
// Mapping persistence (MIDI-4) and import of the mappings stored in a Live set (MIDI-3).
//
//  - JSON: [{"channel","isNote","number","mode","keyCode"?,"target":{"kind","track","device","index","param",...}}]
//  - Per-set sidecar next to the .als: "<set>.sonora.json" {"midiMappings":[...]}
//  - Global file: <data folder>/Mappings.json (core/AppDataFolder.h)

#include "core/Model.h"
#include "core/Edit.h"
#include "midi/ControlTarget.h"

#include <juce_core/juce_core.h>

#include <vector>
#include <optional>

namespace sonora::midi
{

juce::var mappingsToVar(const std::vector<Mapping>& mappings);
std::vector<Mapping> mappingsFromVar(const juce::var& v);

bool saveMappings(const juce::File& file, const std::vector<Mapping>& mappings);
// Returns false when the file is missing or not valid; `out` is then left empty.
bool loadMappings(const juce::File& file, std::vector<Mapping>& out);

// Message-thread helpers. Runtime mappings follow UIDs through moves/history. On disk they address
// the saved file's track ID/scene, not unsaved positions; new/deleted clips remain unbound until saved.
std::optional<ControlTarget> sessionClipTarget(const model::Song&, int track, int scene);
std::optional<model::ClipLocation> sessionClipLocation(const model::Song&, const ControlTarget&);
std::vector<Mapping> mappingsForSave(const model::Song&, std::vector<Mapping>);
void bindMappingsToSong(const model::Song&, std::vector<Mapping>&);
using DeviceOrder = std::vector<std::vector<std::pair<int, std::string>>>; // ID/type, indexed like ControlBank
DeviceOrder deviceOrder(const model::Song&);
void rebindDeviceMappings(const DeviceOrder& before, const DeviceOrder& after, std::vector<Mapping>&);
void rebindTrackMappings(const std::vector<model::TrackId>& before, const std::vector<model::TrackId>& after,
                         const std::vector<model::TrackId>& beforeReturns, const std::vector<model::TrackId>& afterReturns,
                         std::vector<Mapping>&);

juce::File sidecarFor(const juce::File& setFile);
juce::File globalMappingsFile();

// Mappings stored in the set: mixer volume/pan/activator/sends and the parameters of the
// top-level devices of every track. Control index space = tracks, returns, main.
// Devices nested in racks (macros) are skipped until racks have macros (DEV-5).
std::vector<Mapping> importLiveMappings(const model::Song& song);

} // namespace sonora::midi
