#pragma once
// PLG-16: Sonora alternatives for missing plug-ins. A set that uses Serum 2 or LFOTool on a machine that does not have
// them offers Sonora Prism / Sonora Curve LFO instead (the prompt the app shows after loading). Identity is the plug-in
// class (VST3 uid, VST2 id, AU type/subtype/manufacturer) with the name as a guard, never the name alone.
//
// Serum 2 converts its saved state with importSerumPatch (DEV-18d) and passes the importer's warnings on. LFOTool's
// chunk goes through plugins/LfoToolState (PLG-16b: graphs, rate, routing, filter, MIDI options); when the set holds
// no chunk (or an all-zero one), a default Curve LFO gets what Live stored as named parameters with a clear meaning.
// Plug-in automation carries over where the Sonora parameter has the same normalized 0..1 range (the lane plays as is);
// other lanes stay in the set, unused, and the notes say so.
#include "core/Edit.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sonora::plugins
{
enum class NativeAlternative { None, Prism, CurveLfo };

// The Sonora device that can stand in for this hosted plug-in, or None.
NativeAlternative nativeAlternativeFor(const model::Device& device);
const char* nativeAlternativeName(NativeAlternative alternative); // "Sonora Prism"

struct NativeReplacement
{
    int track = -1;        // model::trackAt index: tracks, then returns, then main
    std::vector<int> path; // top-level device index, then (chain, device) index pairs into racks
    std::string trackName, pluginName;
    NativeAlternative alternative = NativeAlternative::None;
    model::Device device;  // the converted Sonora device; the edit gives it fresh ids
    std::vector<std::string> notes; // what did not carry over, honestly
};

// Instances of supported plug-ins that `installed` reports missing, in track order (racks included). Frozen and
// deactivated tracks are skipped: their devices cannot be edited until they are unfrozen / activated.
std::vector<NativeReplacement> findNativeReplacements(const model::Song& song,
                                                      const std::function<bool(const model::Device&)>& installed);

// PLG-16f: the same conversion for instances the user swaps by hand (the device menu), installed or not.
// The instance at `path` on track `track` (model::trackAt index), or nullopt when it is not Serum 2 / LFOTool or its
// track cannot be edited (frozen, deactivated).
std::optional<NativeReplacement> nativeReplacementAt(const model::Song& song, int track, const std::vector<int>& path);
// Every Serum 2 / LFOTool instance on editable tracks (`alternative` None = both kinds).
std::vector<NativeReplacement> allNativeReplacements(const model::Song& song, NativeAlternative alternative = NativeAlternative::None);
// How many instances of that kind the set has on editable tracks (cheap: no conversion).
int countNativeCandidates(const model::Song& song, NativeAlternative alternative);

// One undoable edit that puts each converted device where its plug-in was. The rest of every track (clips, other
// devices, automation lanes) is untouched; undo brings the plug-in devices back as they were.
model::EditCommand replaceWithNative(const model::Song& song, const std::vector<NativeReplacement>& replacements);

// Base64 <data> of one key of an XML plist dictionary (the AU ClassInfo Live stores); empty when absent.
std::vector<uint8_t> plistData(const std::vector<uint8_t>& plist, const std::string& key);

// Prompt text: the instances found and how many compatibility notes each conversion has.
struct NativeReplacementOffer
{
    std::string title, message;
};
NativeReplacementOffer describeNativeReplacements(const std::vector<NativeReplacement>& replacements);
} // namespace sonora::plugins
