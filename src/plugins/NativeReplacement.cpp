#include "plugins/NativeReplacement.h"

#include "devices/DeviceFactory.h"
#include "devices/DeviceProcessor.h"
#include "plugins/LfoToolState.h"
#include "plugins/SerumPatch.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <span>

namespace sonora::plugins
{
namespace
{
constexpr uint32_t fourCC(const char (&c)[5])
{
    return (uint32_t) (uint8_t) c[0] << 24 | (uint32_t) (uint8_t) c[1] << 16 | (uint32_t) (uint8_t) c[2] << 8 | (uint32_t) (uint8_t) c[3];
}

// Identities read from the installed plug-ins (Info.plist AudioComponents, the VST3 class, the VST2 header) and the
// reference sets that use them.
constexpr std::array<uint32_t, 4> serum2Vst3{0x56534558, 0x66735073, 0x6572756D, 0x20320000}; // "VSTXfsPserum 2"
constexpr uint32_t xferAu = fourCC("XFER");
constexpr uint32_t serum2AuType = fourCC("aumu"), serum2AuSubtype = fourCC("Xf2X");
constexpr int64_t lfoToolVst2 = fourCC("XffO");
constexpr uint32_t lfoToolAuType = fourCC("aumf"), lfoToolAuSubtype = fourCC("XffO"); // not the aumi MIDI-effect build

// Live writes this value for a plug-in parameter it never read from the plug-in.
constexpr double liveUnreadValue = 0.1234567687;

std::string squashed(std::string text)
{
    std::erase_if(text, [](char c) { return c == ' ' || c == '_' || c == '-'; });
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return (char) std::tolower((unsigned char) c); });
    return text;
}

struct ParameterLink
{
    const char* pluginName;
    const char* nativePath;
};
// Plug-in parameter -> Sonora parameter where the meaning is the same and both are 0..1 knob positions, so the value
// and any automation lane carry over unchanged. Serum 2's macros are 0..100 normalized to 0..1, Prism's are 0..1
// (importSerumPatch divides the saved value by 100). LFOTool's names come from its VST2 parameter list.
constexpr ParameterLink prismLinks[]{
    {"Macro 1", "Macro.0"}, {"Macro 2", "Macro.1"}, {"Macro 3", "Macro.2"}, {"Macro 4", "Macro.3"},
    {"Macro 5", "Macro.4"}, {"Macro 6", "Macro.5"}, {"Macro 7", "Macro.6"}, {"Macro 8", "Macro.7"},
};
// LFOTool's VST2 parameter names with the same 0..1 meaning on both sides (phase 0..360 degrees, depth / mix / var /
// resonance 0..100 %), plus the names an earlier table guessed.
constexpr ParameterLink curveLfoLinks[]{
    {"Phase", "Phase"}, {"LFO Depth", "Depth"}, {"VF Mix", "Filter.Mix"}, {"VF Var", "Filter.Var"},
    {"F.Reso", "Filter.Resonance"}, {"Cutoff", "Filter.Cutoff"}, {"Reso", "Filter.Resonance"}, {"Vel->PWM", "Midi.VelPwm"},
};

int countLanes(const model::Track& track, const std::set<int>& targets)
{
    int lanes = 0;
    const auto count = [&](const std::vector<model::AutomationEnvelope>& envelopes) {
        for (const auto& e : envelopes) lanes += e.midiController < 0 && targets.count(e.targetId) ? 1 : 0;
    };
    count(track.automation);
    for (const auto& clip : track.arrangementClips) count(clip.envelopes);
    for (const auto& slot : track.slots)
        if (slot.clip) count(slot.clip->envelopes);
    return lanes;
}

NativeReplacement convert(const model::Song& song, const model::Track& track, const model::Device& plugin, NativeAlternative alternative)
{
    NativeReplacement result;
    result.alternative = alternative;
    result.pluginName = plugin.plugin->name;
    const std::string target = nativeAlternativeName(alternative);
    auto& notes = result.notes;
    if (alternative == NativeAlternative::Prism)
    {
        auto state = plugin.plugin->state, controller = plugin.plugin->controllerState;
        if (plugin.kind == model::DeviceKind::PluginAu) // the VST3 AU wrapper keeps both VST3 states in its ClassInfo
        {
            controller = plistData(state, "Controller State");
            state = plistData(state, "Processor State");
        }
        const auto folder = juce::File(juce::String::fromUTF8(song.sourcePath.c_str())).getParentDirectory();
        auto imported = importSerumPatch(state, song.sourcePath.empty() ? std::string() : folder.getFullPathName().toStdString(), controller);
        if (imported)
        {
            result.device = std::move(imported.device);
            notes = std::move(imported.warnings);
        }
        else
        {
            result.device = devices::nativeDeviceModel("SonoraWavetable");
            notes.push_back("The saved Serum 2 patch could not be read (" + imported.error + "): a default Prism patch is used");
        }
    }
    bool fromChunk = false;
    if (alternative == NativeAlternative::CurveLfo)
    {
        // PLG-16b: LFOTool's own chunk carries the graphs and every setting (plugins/LfoToolState.h).
        result.device = devices::nativeDeviceModel("SonoraCurveLfo");
        const auto decoded = decodeLfoToolChunk(lfoToolChunk(*plugin.plugin, plugin.kind));
        if (decoded.ok)
        {
            applyLfoToolState(decoded.state, result.device, notes);
            fromChunk = true;
        }
        else if (decoded.empty)
            notes.push_back("LFOTool saved an empty state in this set (all zeros: the plug-in gave the host no settings when "
                            "the set was saved), so there are no graphs or settings to carry: the Curve LFO starts from its defaults");
        else
            notes.push_back("LFOTool's saved state could not be read (" + decoded.error + "): the Curve LFO starts from its defaults");
    }
    result.device.id = -1;
    result.device.enabled = plugin.enabled;
    result.device.enabledTargetId = plugin.enabledTargetId; // on/off automation keeps playing

    const auto probe = devices::createNative(result.device);
    static const std::vector<devices::ParameterInfo> noInfos;
    const auto& infos = probe ? probe->parameters() : noInfos;
    std::set<int> carried;
    int restored = 0;
    const auto links = alternative == NativeAlternative::Prism ? std::span<const ParameterLink>(prismLinks)
                                                               : std::span<const ParameterLink>(curveLfoLinks);
    for (const auto& link : links)
    {
        const auto source = std::find_if(plugin.pluginParams.begin(), plugin.pluginParams.end(),
                                         [&](const model::PluginParam& p) { return p.name == link.pluginName; });
        if (source == plugin.pluginParams.end()) continue;
        auto destination = std::find_if(result.device.parameters.begin(), result.device.parameters.end(),
                                        [&](const model::Parameter& p) { return p.path == link.nativePath; });
        const auto info = std::find_if(infos.begin(), infos.end(), [&](const devices::ParameterInfo& i) { return i.id == link.nativePath; });
        if (destination == result.device.parameters.end() || info == infos.end() || info->minValue != 0.0f || info->maxValue != 1.0f)
            continue; // the table promises 0..1 on both sides; a changed Sonora range must not reinterpret old values
        if (source->automationTargetId >= 0)
        {
            destination->automationTargetId = source->automationTargetId;
            carried.insert(source->automationTargetId);
        }
        // The saved state is authoritative; Live's named values only stand in when LFOTool's chunk had nothing.
        if (alternative == NativeAlternative::CurveLfo && ! fromChunk && std::abs(source->value - liveUnreadValue) > 1e-6)
        {
            destination->value = info->isToggle ? (source->value >= 0.5 ? 1.0 : 0.0) : std::clamp(source->value, 0.0, 1.0);
            ++restored;
        }
    }
    if (alternative == NativeAlternative::CurveLfo && ! fromChunk)
        notes.push_back(restored > 0 ? "Restored " + std::to_string(restored) + " LFOTool settings by name (same knob positions, "
                                           "Sonora's own response)"
                                     : "No LFOTool settings were stored in the set: rate, shape and filter use Curve LFO defaults");
    std::set<int> dropped;
    for (const auto& p : plugin.pluginParams)
        if (p.automationTargetId >= 0 && ! carried.count(p.automationTargetId)) dropped.insert(p.automationTargetId);
    if (const int lanes = countLanes(track, dropped); lanes > 0)
        notes.push_back(std::to_string(lanes) + " automation lane" + (lanes == 1 ? "" : "s") + " of " + result.pluginName
                        + " parameters ha" + (lanes == 1 ? "s" : "ve") + " no " + target + " equivalent: kept in the set, not played");
    if (const int lanes = countLanes(track, carried); lanes > 0)
        notes.push_back(std::to_string(lanes) + " automation lane" + (lanes == 1 ? "" : "s") + " now drive" + (lanes == 1 ? "s " : " ") + target);
    std::set<std::string> seen; // the importer reports some findings once per oscillator
    std::erase_if(notes, [&](const std::string& note) { return ! seen.insert(note).second; });
    return result;
}

void collect(const model::Song& song, const model::Track& track, int trackIndex, const std::vector<model::Device>& devices,
             std::vector<int>& path, const std::function<bool(const model::Device&)>& installed, std::vector<NativeReplacement>& out)
{
    for (size_t i = 0; i < devices.size(); ++i)
    {
        const auto& device = devices[i];
        path.push_back((int) i);
        if (const auto alternative = nativeAlternativeFor(device); alternative != NativeAlternative::None && ! installed(device))
        {
            auto replacement = convert(song, track, device, alternative);
            replacement.track = trackIndex;
            replacement.path = path;
            replacement.trackName = track.name;
            out.push_back(std::move(replacement));
        }
        for (size_t c = 0; c < device.chains.size(); ++c)
        {
            path.push_back((int) c);
            collect(song, track, trackIndex, device.chains[c].devices, path, installed, out);
            path.pop_back();
        }
        path.pop_back();
    }
}

// The device at `path` below `top` (path[0] is top's own index), marking every rack on the way as edited.
model::Device* nestedDevice(model::Device& top, const std::vector<int>& path)
{
    auto* device = &top;
    for (size_t i = 1; i + 1 < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= device->chains.size()) return nullptr;
        auto& chain = device->chains[(size_t) path[i]];
        if (path[i + 1] < 0 || (size_t) path[i + 1] >= chain.devices.size()) return nullptr;
        device->chainsEdited = true;
        device = &chain.devices[(size_t) path[i + 1]];
    }
    return device;
}
} // namespace

std::vector<uint8_t> plistData(const std::vector<uint8_t>& plist, const std::string& key)
{
    const auto xml = juce::XmlDocument::parse(juce::String::fromUTF8((const char*) plist.data(), (int) plist.size()));
    const auto* dict = xml != nullptr ? xml->getChildByName("dict") : nullptr;
    for (auto* item = dict != nullptr ? dict->getFirstChildElement() : nullptr; item != nullptr; item = item->getNextElement())
    {
        if (! item->hasTagName("key") || item->getAllSubText() != juce::String::fromUTF8(key.c_str())) continue;
        const auto* value = item->getNextElement();
        if (value == nullptr || ! value->hasTagName("data")) return {};
        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64(decoded, value->getAllSubText().removeCharacters(" \t\r\n"))) return {};
        const auto* first = static_cast<const uint8_t*>(decoded.getData());
        return {first, first + decoded.getDataSize()};
    }
    return {};
}

NativeAlternative nativeAlternativeFor(const model::Device& device)
{
    if (! device.plugin) return NativeAlternative::None;
    const auto& info = *device.plugin;
    const auto name = squashed(info.name);
    switch (device.kind)
    {
    case model::DeviceKind::PluginVst3:
        return info.vst3Uid == serum2Vst3 && name.find("serum2") != std::string::npos ? NativeAlternative::Prism : NativeAlternative::None;
    case model::DeviceKind::PluginVst2:
        return info.vst2UniqueId == lfoToolVst2 && name.find("lfotool") != std::string::npos ? NativeAlternative::CurveLfo
                                                                                             : NativeAlternative::None;
    case model::DeviceKind::PluginAu:
        if (info.auManufacturer != xferAu) return NativeAlternative::None;
        if (info.auType == serum2AuType && info.auSubType == serum2AuSubtype && name.find("serum2") != std::string::npos)
            return NativeAlternative::Prism;
        if (info.auType == lfoToolAuType && info.auSubType == lfoToolAuSubtype && name.find("lfotool") != std::string::npos)
            return NativeAlternative::CurveLfo;
        return NativeAlternative::None;
    case model::DeviceKind::Native:
    case model::DeviceKind::Rack:
    case model::DeviceKind::MaxForLive:
        break;
    }
    return NativeAlternative::None;
}

const char* nativeAlternativeName(NativeAlternative alternative)
{
    switch (alternative)
    {
    case NativeAlternative::Prism: return "Sonora Prism";
    case NativeAlternative::CurveLfo: return "Sonora Curve LFO";
    case NativeAlternative::None: break;
    }
    return "";
}

std::vector<NativeReplacement> findNativeReplacements(const model::Song& song, const std::function<bool(const model::Device&)>& installed)
{
    std::vector<NativeReplacement> out;
    const int count = (int) (song.tracks.size() + song.returns.size()) + 1;
    for (int index = 0; index < count; ++index)
    {
        const auto* track = model::trackAt(song, index);
        if (track == nullptr || track->frozen || track->deactivated) continue;
        std::vector<int> path;
        collect(song, *track, index, track->devices, path, installed, out);
    }
    return out;
}

std::optional<NativeReplacement> nativeReplacementAt(const model::Song& song, int trackIndex, const std::vector<int>& path)
{
    const auto* track = model::trackAt(song, trackIndex);
    if (track == nullptr || track->frozen || track->deactivated || path.empty() || path.size() % 2 == 0) return std::nullopt;
    const std::vector<model::Device>* devices = &track->devices;
    const model::Device* device = nullptr;
    for (size_t i = 0; i < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= devices->size()) return std::nullopt;
        device = &(*devices)[(size_t) path[i]];
        if (i + 1 < path.size())
        {
            if (path[i + 1] < 0 || (size_t) path[i + 1] >= device->chains.size()) return std::nullopt;
            devices = &device->chains[(size_t) path[i + 1]].devices;
        }
    }
    const auto alternative = nativeAlternativeFor(*device);
    if (alternative == NativeAlternative::None) return std::nullopt;
    auto replacement = convert(song, *track, *device, alternative);
    replacement.track = trackIndex;
    replacement.path = path;
    replacement.trackName = track->name;
    return replacement;
}

std::vector<NativeReplacement> allNativeReplacements(const model::Song& song, NativeAlternative alternative)
{
    auto found = findNativeReplacements(song, [alternative](const model::Device& d) {
        return alternative != NativeAlternative::None && nativeAlternativeFor(d) != alternative; // "installed" = skipped
    });
    return found;
}

int countNativeCandidates(const model::Song& song, NativeAlternative alternative)
{
    int count = 0;
    const std::function<void(const std::vector<model::Device>&)> walk = [&](const std::vector<model::Device>& devices) {
        for (const auto& d : devices)
        {
            const auto kind = nativeAlternativeFor(d);
            count += kind != NativeAlternative::None && (alternative == NativeAlternative::None || kind == alternative) ? 1 : 0;
            for (const auto& chain : d.chains) walk(chain.devices);
        }
    };
    const int tracks = (int) (song.tracks.size() + song.returns.size()) + 1;
    for (int index = 0; index < tracks; ++index)
        if (const auto* track = model::trackAt(song, index); track != nullptr && ! track->frozen && ! track->deactivated) walk(track->devices);
    return count;
}

model::EditCommand replaceWithNative(const model::Song& song, const std::vector<NativeReplacement>& replacements)
{
    // Instances inside one top-level rack become one replacement of that rack.
    std::map<std::pair<int, int>, model::Device> tops;
    for (const auto& r : replacements)
    {
        const auto* track = model::trackAt(song, r.track);
        if (track == nullptr || r.path.empty() || r.path[0] < 0 || (size_t) r.path[0] >= track->devices.size()) continue;
        const auto key = std::make_pair(r.track, r.path[0]);
        auto it = tops.find(key);
        if (it == tops.end()) it = tops.emplace(key, track->devices[(size_t) r.path[0]]).first;
        auto* target = nestedDevice(it->second, r.path);
        if (target == nullptr || nativeAlternativeFor(*target) != r.alternative) continue; // the set changed since the offer
        *target = r.device;
    }
    if (tops.empty()) return {};
    // A structural edit needs fresh processors: new ids for every replaced top-level device and the Sonora devices.
    auto allocator = song;
    std::vector<model::EditCommand> parts;
    for (auto& [key, device] : tops)
    {
        device.id = -1;
        model::allocateDeviceIds(allocator, device);
        parts.push_back(model::replaceDevice(song, key.first, key.second, std::move(device), {}));
    }
    auto command = model::compose(replacements.size() == 1 ? "Replace with " + std::string(nativeAlternativeName(replacements[0].alternative))
                                                           : "Replace with Sonora Devices",
                                  std::move(parts));
    const auto apply = command.apply;
    command.apply = [apply, deviceEnd = allocator.nextDeviceId, pointeeEnd = allocator.nextPointeeId](model::Song& destination) {
        destination.nextDeviceId = std::max(destination.nextDeviceId, deviceEnd);
        destination.nextPointeeId = std::max(destination.nextPointeeId, pointeeEnd);
        destination.pointeeIdsAllocated = true;
        apply(destination);
    };
    return command;
}

NativeReplacementOffer describeNativeReplacements(const std::vector<NativeReplacement>& replacements)
{
    NativeReplacementOffer offer;
    // Grouped by plug-in, in the order the set uses them: the swap, how many, and the tracks.
    struct Group
    {
        std::string plugin;
        NativeAlternative alternative = NativeAlternative::None;
        std::vector<std::string> tracks;
        size_t count = 0;
    };
    std::vector<Group> groups;
    size_t notes = 0;
    for (const auto& r : replacements)
    {
        auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.plugin == r.pluginName; });
        if (group == groups.end()) group = groups.insert(groups.end(), Group{r.pluginName, r.alternative, {}, 0});
        ++group->count;
        if (std::find(group->tracks.begin(), group->tracks.end(), r.trackName) == group->tracks.end()) group->tracks.push_back(r.trackName);
        notes += r.notes.size();
    }
    std::string names;
    for (size_t i = 0; i < groups.size(); ++i)
        names += (i == 0 ? "" : i + 1 == groups.size() ? " and " : ", ") + groups[i].plugin;
    offer.title = names + (groups.size() == 1 ? " is" : " are") + " not installed";
    std::string& m = offer.message;
    m = replacements.size() == 1 ? std::string("This set uses it once.") : "This set uses " + std::string(groups.size() == 1 ? "it " : "them ")
                                                                               + std::to_string(replacements.size()) + " times.";
    m += " Sonora can put a compatible device in each place:\n";
    constexpr size_t listed = 6;
    for (const auto& g : groups)
    {
        m += "\n\u2022 " + g.plugin + " \u2192 " + nativeAlternativeName(g.alternative) + " (" + std::to_string(g.count) + "): ";
        for (size_t i = 0; i < g.tracks.size() && i < listed; ++i) m += (i == 0 ? "" : ", ") + g.tracks[i];
        if (g.tracks.size() > listed) m += " and " + std::to_string(g.tracks.size() - listed) + " more";
    }
    m += "\n";
    for (const auto& g : groups)
        m += g.alternative == NativeAlternative::Prism
                 ? "\nPrism rebuilds each saved patch with its own engine: similar, not identical."
                 : "\nCurve LFO rebuilds each saved LFOTool state (graphs, rate, routing, filter) with its own engine: similar, not identical.";
    if (notes > 0) m += " " + std::to_string(notes) + " compatibility note" + (notes == 1 ? " goes" : "s go") + " to the load notes.";
    m += " Keep leaves placeholders; Undo brings the plug-ins back after a replace.";
    return offer;
}
} // namespace sonora::plugins
