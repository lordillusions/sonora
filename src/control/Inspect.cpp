#include "control/ControlInternal.h"
#include "core/DeviceCatalog.h"
#include <algorithm>

// AGT-7e: project.inspect, automation.get and status.get. Everything here reads the model (and the live mixer values
// the backend supplies); nothing mutates. Per-container caps keep the result bounded.
namespace sonora::control::detail
{
namespace
{
constexpr int maxClipsPerContainer = 512, maxDeviceDepth = 4, maxChainsPerDevice = 64, maxDevicesPerList = 64, maxScenes = 1024;

const char* deviceKindName(model::DeviceKind kind)
{
    switch (kind)
    {
        case model::DeviceKind::Native: return "native";
        case model::DeviceKind::Rack: return "rack";
        case model::DeviceKind::PluginVst3: return "vst3";
        case model::DeviceKind::PluginVst2: return "vst2";
        case model::DeviceKind::PluginAu: return "au";
        case model::DeviceKind::MaxForLive: return "max";
    }
    return "other";
}
const char* roleName(model::DeviceRole role)
{
    if (role == model::DeviceRole::Instrument) return "instrument";
    if (role == model::DeviceRole::MidiEffect) return "midiEffect";
    return "audioEffect";
}
const char* monitoringName(int mode) { return mode == 0 ? "in" : mode == 2 ? "off" : "auto"; }

juce::var deviceEntry(const model::Device& device, int depth)
{
    auto entry = object({{"deviceId", device.id}, {"name", device.displayName}, {"type", device.type},
                         {"hosted", device.plugin.has_value()}, {"enabled", device.enabled},
                         {"kind", deviceKindName(device.kind)}, {"role", roleName(model::deviceRole(device))}});
    if (device.chains.empty()) return entry;
    auto* data = entry.getDynamicObject();
    if (depth >= maxDeviceDepth) { data->setProperty("chainsTruncated", true); return entry; }
    juce::Array<juce::var> chains;
    for (size_t c = 0; c < device.chains.size() && c < static_cast<size_t>(maxChainsPerDevice); ++c)
    {
        juce::Array<juce::var> children;
        for (size_t d = 0; d < device.chains[c].devices.size() && d < static_cast<size_t>(maxDevicesPerList); ++d)
            children.add(deviceEntry(device.chains[c].devices[d], depth + 1));
        chains.add(object({{"index", static_cast<int>(c)}, {"name", device.chains[c].name}, {"devices", children}}));
    }
    data->setProperty("chains", chains);
    if (device.chains.size() > static_cast<size_t>(maxChainsPerDevice)) data->setProperty("chainsTruncated", true);
    return entry;
}
juce::var deviceList(const model::Track& track)
{
    juce::Array<juce::var> devices;
    for (const auto& device : track.devices) devices.add(deviceEntry(device, 0));
    return juce::var(devices);
}

juce::var mixerEntry(const model::Song& song, const model::Track& track, size_t controlIndex, const State& state)
{
    const auto& mixer = track.mixer;
    const LiveMixer* live = controlIndex < state.liveMixer.size() ? &state.liveMixer[controlIndex] : nullptr;
    const double gain = live ? live->volume : controlIndex < state.liveGains.size() ? state.liveGains[controlIndex] : mixer.volume;
    const std::vector<double>& sendValues = live && ! live->sends.empty() ? live->sends : mixer.sends;
    juce::Array<juce::var> sends;
    for (size_t i = 0; i < sendValues.size() && i < song.returns.size(); ++i)
        sends.add(object({{"sendIndex", static_cast<int>(i)}, {"returnTrackId", song.returns[i].id}, {"gainLinear", sendValues[i]}}));
    return object({{"gainLinear", gain}, {"authoredGainLinear", mixer.volume}, {"pan", live ? live->pan : mixer.pan},
                   {"muted", ! (live ? live->active : mixer.speakerOn)}, {"solo", live ? live->solo : mixer.solo},
                   {"armed", live ? live->armed : false}, {"sends", sends}});
}

juce::String sampleName(const model::Clip& clip)
{
    const auto& path = clip.sample.absolutePath.empty() ? clip.sample.relativePath : clip.sample.absolutePath;
    return juce::String::fromUTF8(path.c_str()).replaceCharacter('\\', '/').fromLastOccurrenceOf("/", false, false);
}
void describeClip(juce::DynamicObject& out, const model::Clip& clip)
{
    out.setProperty("kind", clip.kind == model::Clip::Kind::Audio ? "audio" : "midi");
    out.setProperty("muted", clip.disabled);
    out.setProperty("color", clip.colorIndex);
    out.setProperty("looping", clip.loop.enabled);
    if (clip.kind == model::Clip::Kind::Audio) out.setProperty("sampleFile", sampleName(clip));
}
} // namespace

juce::var automationLanes(const model::Track& track)
{
    juce::Array<juce::var> lanes;
    for (const auto& envelope : track.automation)
    {
        const auto info = model::describeAutomationTarget(track, envelope.targetId);
        juce::Array<juce::var> points;
        for (const auto& point : envelope.points)
            if (point.time > -1e6 && points.size() < 64) points.add(object({{"beat", point.time}, {"value", point.value}}));
        int count = 0;
        for (const auto& point : envelope.points) count += point.time > -1e6 ? 1 : 0;
        auto lane = object({{"targetId", envelope.targetId}, {"target", juce::String(info.kind.empty() ? "unknown" : info.kind)},
                            {"pointCount", count}, {"points", points}, {"stepped", envelope.stepped}});
        if (info.kind == "send") lane.getDynamicObject()->setProperty("sendIndex", info.sendIndex);
        if (info.kind == "device")
        {
            lane.getDynamicObject()->setProperty("deviceId", info.deviceId);
            lane.getDynamicObject()->setProperty("parameterId", juce::String::fromUTF8(info.parameter.c_str()));
        }
        lanes.add(lane);
    }
    return juce::var(lanes);
}

juce::var automationDetail(const model::Song&, const model::Track& track, int targetId, int offset, int limit)
{
    const auto info = model::describeAutomationTarget(track, targetId);
    const auto found = std::find_if(track.automation.begin(), track.automation.end(), [targetId](const auto& e) { return e.targetId == targetId; });
    juce::Array<juce::var> points;
    int total = 0;
    juce::var initial;
    if (found != track.automation.end())
    {
        for (const auto& point : found->points)
        {
            if (point.time <= -1e6) { initial = point.value; continue; } // the pre-roll value before the first breakpoint
            if (total >= offset && points.size() < limit)
                points.add(object({{"beat", point.time}, {"value", point.value}, {"curved", point.curved()}}));
            ++total;
        }
    }
    auto out = object({{"trackId", -1}, {"targetId", targetId}, {"target", juce::String(info.kind.empty() ? "unknown" : info.kind)},
                       {"exists", found != track.automation.end()}, {"stepped", found != track.automation.end() ? found->stepped : info.stepped},
                       {"pointCount", total}, {"points", points}, {"offset", offset}, {"hasMore", offset + points.size() < total}});
    auto* data = out.getDynamicObject();
    if (! initial.isVoid()) data->setProperty("initialValue", initial);
    if (info.kind == "send") data->setProperty("sendIndex", info.sendIndex);
    if (info.kind == "device")
    {
        data->setProperty("deviceId", info.deviceId);
        data->setProperty("parameterId", juce::String::fromUTF8(info.parameter.c_str()));
    }
    return out;
}

juce::var inspectSnapshot(const State& state)
{
    juce::Array<juce::var> tracks, returns, scenes;
    const auto* song = state.song;
    if (song)
    {
        for (size_t index = 0; index < song->tracks.size(); ++index)
        {
            const auto& track = song->tracks[index];
            juce::Array<juce::var> clips, sessionClips, takeClips, takeLanes;
            for (const auto& clip : track.arrangementClips)
                if (clips.size() < maxClipsPerContainer)
                {
                    auto entry = object({{"clipId", clip.uid}, {"name", clip.name}, {"startBeat", clip.currentStart},
                                         {"endBeat", clip.currentEnd}, {"noteCount", static_cast<int>(clip.notes.size())}});
                    describeClip(*entry.getDynamicObject(), clip);
                    clips.add(entry);
                }
            for (size_t scene = 0; scene < track.slots.size(); ++scene)
                if (const auto& slot = track.slots[scene]; slot.clip && sessionClips.size() < maxClipsPerContainer)
                {
                    auto entry = object({{"clipId", slot.clip->uid}, {"scene", static_cast<int>(scene)}, {"name", slot.clip->name}});
                    describeClip(*entry.getDynamicObject(), *slot.clip);
                    sessionClips.add(entry);
                }
            for (const auto& lane : track.takeLanes)
            {
                for (const auto& clip : lane.clips)
                    if (takeClips.size() < maxClipsPerContainer)
                    {
                        auto entry = object({{"clipId", clip.uid}, {"laneId", lane.id}, {"name", clip.name}});
                        describeClip(*entry.getDynamicObject(), clip);
                        takeClips.add(entry);
                    }
                takeLanes.add(object({{"laneId", lane.id}, {"name", lane.name}, {"audition", lane.audition},
                                      {"clipCount", static_cast<int>(lane.clips.size())}}));
            }
            const double gain = index < state.liveGains.size() ? state.liveGains[index] : track.mixer.volume;
            auto item = object({{"trackId", track.id}, {"name", track.name}, {"kind", kindName(track.kind)}, {"groupId", track.groupId},
                                {"gainLinear", gain}, {"authoredGainLinear", track.mixer.volume},
                                {"clips", clips}, {"sessionClips", sessionClips}, {"takeClips", takeClips}, {"devices", deviceList(track)},
                                {"automation", automationLanes(track)}, {"color", track.colorIndex},
                                {"mixer", mixerEntry(*song, track, index, state)},
                                {"monitoring", monitoringName(index < state.liveMixer.size() ? state.liveMixer[index].monitoring : track.monitoring)},
                                {"frozen", track.frozen}, {"deactivated", track.deactivated}, {"takeLanes", takeLanes}});
            if (track.hidden) item.getDynamicObject()->setProperty("hidden", true);
            if (track.kind == model::TrackKind::Group)
            {
                // AGT-7d: a group holds no clips itself; its content is on the tracks listed here.
                juce::Array<juce::var> children;
                for (const auto& other : song->tracks) if (other.groupId == track.id) children.add(other.id);
                item.getDynamicObject()->setProperty("folded", track.folded);
                item.getDynamicObject()->setProperty("childTrackIds", children);
            }
            tracks.add(item);
        }
        for (size_t i = 0; i < song->returns.size(); ++i)
        {
            const auto& track = song->returns[i];
            returns.add(object({{"trackId", track.id}, {"name", track.name}, {"sendIndex", static_cast<int>(i)},
                                {"automation", automationLanes(track)}, {"color", track.colorIndex}, {"devices", deviceList(track)},
                                {"mixer", mixerEntry(*song, track, song->tracks.size() + i, state)}}));
        }
        size_t sceneCount = song->scenes.size();
        for (const auto& track : song->tracks) sceneCount = std::max(sceneCount, track.slots.size());
        for (size_t scene = 0; scene < sceneCount && scene < static_cast<size_t>(maxScenes); ++scene)
        {
            auto entry = object({{"scene", static_cast<int>(scene)}, {"name", scene < song->scenes.size() ? song->scenes[scene].name : std::string()}});
            if (scene < song->scenes.size() && song->scenes[scene].tempo) entry.getDynamicObject()->setProperty("tempo", *song->scenes[scene].tempo);
            scenes.add(entry);
        }
    }
    juce::Array<juce::var> locators;
    if (song)
        for (const auto& locator : song->locators)
            locators.add(object({{"locatorId", locator.id}, {"name", juce::String::fromUTF8(locator.name.c_str())}, {"beat", locator.time},
                                 {"color", locator.colorIndex}, {"songStart", locator.isSongStart},
                                 {"infoText", juce::String::fromUTF8(locator.annotation.c_str())}}));
    auto out = object({{"documentId", state.documentId}, {"revision", state.revision}, {"busy", state.busy},
                       {"tempo", state.liveTempo > 0 ? state.liveTempo : (song ? song->tempo : 0.0)}, {"tracks", tracks},
                       {"returnTracks", returns}, {"locators", locators}});
    auto* data = out.getDynamicObject();
    data->setProperty("path", juce::String::fromUTF8(state.path.c_str()));
    data->setProperty("untitled", state.untitled);
    data->setProperty("dirty", state.dirty);
    data->setProperty("history", object({{"canUndo", state.history.canUndo}, {"undoLabel", state.history.undoLabel},
                                         {"canRedo", state.history.canRedo}, {"redoLabel", state.history.redoLabel}}));
    if (song)
    {
        data->setProperty("timeSignature", object({{"numerator", song->timeSigNumerator}, {"denominator", song->timeSigDenominator}}));
        data->setProperty("loop", object({{"enabled", song->loopOn}, {"startBeat", song->loopStart}, {"lengthBeats", song->loopLength}}));
        data->setProperty("punch", object({{"in", song->punchIn}, {"out", song->punchOut}}));
        data->setProperty("scale", object({{"root", song->scaleRoot}, {"name", song->scaleName}, {"inKey", song->inKey}}));
        data->setProperty("quantization", song->globalQuantisation);
        data->setProperty("scenes", scenes);
        const auto& main = song->main;
        data->setProperty("mainTrack", object({{"trackId", model::mainTrackId(*song)}, {"name", main.name}, {"color", main.colorIndex},
                                               {"mixer", mixerEntry(*song, main, song->tracks.size() + song->returns.size(), state)},
                                               {"devices", deviceList(main)}, {"automation", automationLanes(main)}}));
    }
    return out;
}

juce::var statusSnapshot(const State& state, const Backend& backend)
{
    auto out = object({{"busy", state.busy}, {"dirty", state.dirty}, {"path", juce::String::fromUTF8(state.path.c_str())},
                       {"untitled", state.untitled},
                       {"history", object({{"canUndo", state.history.canUndo}, {"undoLabel", state.history.undoLabel},
                                           {"canRedo", state.history.canRedo}, {"redoLabel", state.history.redoLabel}})},
                       {"tempo", state.liveTempo > 0 ? state.liveTempo : (state.song ? state.song->tempo : 0.0)}});
    bool live = false;
    if (backend.status)
        if (const auto extra = backend.status(); extra.getDynamicObject())
        {
            live = true;
            for (const auto& property : extra.getDynamicObject()->getProperties()) out.getDynamicObject()->setProperty(property.name, property.value);
        }
    out.getDynamicObject()->setProperty("liveStatus", live);
    return out;
}
} // namespace sonora::control::detail
