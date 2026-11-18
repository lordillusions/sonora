#include "control/ControlInternal.h"
#include <algorithm>

// AGT-7e: live actions. They change the running session (mixer, transport, launching, recording, save, plug-in scan,
// device on/off), have no undo and no revision guard, and take absolute values so a repeat is harmless. This file only
// validates and normalises arguments against the model; the effect is the backend's (src/app/AppControl.cpp).
namespace sonora::control::detail
{
namespace
{
constexpr int maxId = 1000000000;

juce::var copyOf(const juce::var& args)
{
    return args.getDynamicObject() ? juce::var(args.getDynamicObject()->clone().release()) : object({});
}
void setProperty(juce::var& target, const char* name, juce::var value) { target.getDynamicObject()->setProperty(name, std::move(value)); }

const model::Track& trackFor(const State& state, const juce::var& args, int& controlIndex)
{
    const int id = integer(args, "trackId", 0, maxId);
    controlIndex = controlIndexOf(*state.song, id, true);
    return *findAnyTrack(*state.song, id);
}
void requireNoAutomation(const model::Track& track, int target, const char* what)
{
    if (hasAutomation(track, target))
        throw model::OpError("automation_conflict", std::string(what) + " has authored automation; edit its envelope instead");
}

juce::var mixerSet(const juce::var& args, const State& state)
{
    fields(args, {"trackId", "gainDb", "pan", "muted", "solo", "armed", "sends"});
    int index = 0;
    const auto& track = trackFor(state, args, index);
    if (! has(args, "gainDb") && ! has(args, "pan") && ! has(args, "muted") && ! has(args, "solo") && ! has(args, "armed") && ! has(args, "sends"))
        throw std::runtime_error("Specify at least one of gainDb, pan, muted, solo, armed, sends");
    auto out = copyOf(args);
    const bool main = track.kind == model::TrackKind::Main;
    if (has(args, "gainDb")) { number(args, "gainDb", -70, 6); requireNoAutomation(track, track.mixer.volumeTargetId, "Volume"); }
    if (has(args, "pan")) { number(args, "pan", -1, 1); requireNoAutomation(track, track.mixer.panTargetId, "Pan"); }
    if (has(args, "muted"))
    {
        boolean(args, "muted");
        if (main) throw std::runtime_error("The main track cannot be muted");
        requireNoAutomation(track, track.mixer.speakerTargetId, "The track activator");
    }
    if (has(args, "solo") && (boolean(args, "solo"), main)) throw std::runtime_error("The main track cannot be soloed");
    if (has(args, "armed"))
    {
        boolean(args, "armed");
        if (track.kind != model::TrackKind::Audio && track.kind != model::TrackKind::Midi) throw std::runtime_error("Only audio and MIDI tracks can be armed");
    }
    if (has(args, "sends"))
    {
        const auto* sends = args["sends"].getArray();
        if (! sends || sends->isEmpty() || sends->size() > 12) throw std::runtime_error("Expected 1–12 send entries");
        std::vector<int> seen;
        for (const auto& send : *sends)
        {
            fields(send, {"sendIndex", "gainDb"});
            const int sendIndex = integer(send, "sendIndex", 0, 11);
            number(send, "gainDb", -70, 0);
            if (static_cast<size_t>(sendIndex) >= track.mixer.sends.size() || static_cast<size_t>(sendIndex) >= state.song->returns.size())
                throw model::OpError("invalid_request", "No such send; sendIndex follows returnTracks[]");
            if (std::find(seen.begin(), seen.end(), sendIndex) != seen.end()) throw std::runtime_error("Duplicate sendIndex");
            seen.push_back(sendIndex);
            if (static_cast<size_t>(sendIndex) < track.mixer.sendTargetIds.size())
                requireNoAutomation(track, track.mixer.sendTargetIds[static_cast<size_t>(sendIndex)], "That send");
        }
    }
    setProperty(out, "trackIndex", index);
    setProperty(out, "trackKind", kindName(track.kind));
    return out;
}
juce::var transportSet(const juce::var& args)
{
    fields(args, {"action", "positionBeats", "metronome"});
    if (! has(args, "action") && ! has(args, "positionBeats") && ! has(args, "metronome")) throw std::runtime_error("Specify action, positionBeats or metronome");
    if (has(args, "action"))
    {
        const auto action = string(args, "action", 32);
        if (action != "play" && action != "stop" && action != "continue" && action != "return_to_start")
            throw std::runtime_error("action must be play, stop, continue or return_to_start");
    }
    if (has(args, "positionBeats")) number(args, "positionBeats", 0, 1000000);
    if (has(args, "metronome")) boolean(args, "metronome");
    return copyOf(args);
}
juce::var sessionLaunch(const juce::var& args, const State& state)
{
    fields(args, {"action", "trackId", "scene"});
    const auto action = string(args, "action", 32);
    auto out = copyOf(args);
    if (action == "stop_all" || action == "back_to_arrangement")
    {
        if (has(args, "trackId") || has(args, "scene")) throw std::runtime_error(action.toStdString() + " takes no trackId or scene");
        return out;
    }
    if (action != "clip" && action != "scene" && action != "stop_track") throw std::runtime_error("action must be clip, scene, stop_track, stop_all or back_to_arrangement");
    if (action != "stop_track") integer(args, "scene", 0, 999);
    else if (has(args, "scene")) throw std::runtime_error("stop_track takes no scene");
    if (action == "scene" && has(args, "trackId")) throw std::runtime_error("scene takes no trackId");
    if (action != "scene")
    {
        const int index = regularTrackIndex(*state.song, integer(args, "trackId", 0, maxId));
        setProperty(out, "trackIndex", index);
        const auto& track = state.song->tracks[static_cast<size_t>(index)];
        if (action == "clip")
        {
            const int scene = integer(args, "scene", 0, 999);
            if (track.kind == model::TrackKind::Group) throw std::runtime_error("Group tracks hold no clips");
            if (static_cast<size_t>(scene) >= track.slots.size() || ! track.slots[static_cast<size_t>(scene)].clip)
                throw model::OpError("target_replaced", "There is no Session clip in that slot");
        }
    }
    return out;
}
juce::var recordingSet(const juce::var& args)
{
    fields(args, {"mode", "recording"});
    const auto mode = string(args, "mode", 16);
    if (mode != "arrangement" && mode != "session") throw std::runtime_error("mode must be arrangement or session");
    boolean(args, "recording");
    return copyOf(args);
}
juce::var pluginsScan(const juce::var& args)
{
    fields(args, {"includeAudioUnits"});
    if (has(args, "includeAudioUnits")) boolean(args, "includeAudioUnits");
    return copyOf(args);
}
juce::var deviceEnabled(const juce::var& args, const State& state)
{
    fields(args, {"trackId", "deviceId", "enabled"});
    int index = 0;
    const auto& track = trackFor(state, args, index);
    const int deviceId = integer(args, "deviceId", 0, maxId);
    boolean(args, "enabled");
    const auto found = std::find_if(track.devices.begin(), track.devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
    if (found == track.devices.end()) throw model::OpError("target_replaced", "Device no longer exists on this track (top-level devices only)");
    requireNoAutomation(track, found->enabledTargetId, "The device switch");
    auto out = copyOf(args);
    setProperty(out, "trackIndex", index);
    for (size_t i = 0; i < track.devices.size(); ++i)
        if (track.devices[i].id == deviceId) setProperty(out, "deviceIndex", static_cast<int>(i));
    return out;
}
// ---- AGT-7e round 2: export, copies, view, editors and media jobs
juce::File newPath(const juce::var& args, const char* name, bool allowExisting = false)
{
    const auto text = string(args, name, 4096);
    if (! juce::File::isAbsolutePath(text)) throw std::runtime_error(std::string(name) + " must be an absolute path");
    const juce::File file(text);
    if (! file.getParentDirectory().isDirectory()) throw std::runtime_error("The parent folder does not exist: " + file.getParentDirectory().getFullPathName().toStdString());
    if (! allowExisting && file.exists()) throw model::OpError("destination_exists", "Refusing to overwrite: " + file.getFullPathName().toStdString());
    return file;
}
juce::var projectSaveCopy(const juce::var& args)
{
    fields(args, {"path"});
    const auto file = newPath(args, "path");
    if (! file.hasFileExtension("als")) throw std::runtime_error("path must end in .als");
    return copyOf(args);
}
juce::var exportAudio(const juce::var& args, const State& state)
{
    fields(args, {"destination", "target", "trackId", "startBeat", "endBeat", "format", "bitDepth", "normalize", "mono", "dither", "tailSeconds", "autoTail"});
    const auto target = string(args, "target", 8), format = string(args, "format", 8);
    if (target != "main" && target != "track" && target != "stems") throw std::runtime_error("target must be main, track or stems");
    if (format != "wav" && format != "aiff" && format != "flac") throw std::runtime_error("format must be wav, aiff or flac");
    const int depth = integer(args, "bitDepth", 16, 32);
    if (depth != 16 && depth != 24 && depth != 32) throw std::runtime_error("bitDepth must be 16, 24 or 32");
    if (depth == 32 && format != "wav") throw std::runtime_error("32-bit float export is WAV only");
    const double start = number(args, "startBeat", 0, 1000000), end = number(args, "endBeat", 0, 1000000);
    if (end <= start) throw std::runtime_error("endBeat must be after startBeat");
    for (const char* flag : {"normalize", "mono", "dither", "autoTail"}) if (has(args, flag)) boolean(args, flag);
    if (has(args, "tailSeconds"))
    {
        number(args, "tailSeconds", 0, 60);
        if (has(args, "autoTail") && static_cast<bool>(args["autoTail"])) throw std::runtime_error("Choose tailSeconds OR autoTail");
    }
    const auto destination = newPath(args, "destination");
    if (target != "stems" && ! destination.hasFileExtension(format)) throw std::runtime_error("destination must end in ." + format.toStdString() + " for this format");
    auto out = copyOf(args);
    if (target == "track")
    {
        if (! has(args, "trackId")) throw std::runtime_error("target track needs trackId");
        const int id = integer(args, "trackId", 0, maxId);
        if (id == model::mainTrackId(*state.song)) throw std::runtime_error("Use target main for the main track");
        setProperty(out, "trackIndex", controlIndexOf(*state.song, id, false));
    }
    else if (has(args, "trackId")) throw std::runtime_error("trackId applies to target track only");
    return out;
}
juce::var viewSet(const juce::var& args, const State& state)
{
    fields(args, {"mainView", "detail", "selectTrackId", "selectClip", "browserVisible", "mixerVisible", "followPlayhead"});
    bool any = false;
    for (const char* key : {"mainView", "detail", "selectTrackId", "selectClip", "browserVisible", "mixerVisible", "followPlayhead"}) any |= has(args, key);
    if (! any) throw std::runtime_error("Specify at least one view setting");
    auto out = copyOf(args);
    if (has(args, "mainView")) { const auto v = string(args, "mainView", 16); if (v != "session" && v != "arrangement") throw std::runtime_error("mainView must be session or arrangement"); }
    if (has(args, "detail")) { const auto v = string(args, "detail", 16); if (v != "clip" && v != "devices" && v != "hidden") throw std::runtime_error("detail must be clip, devices or hidden"); }
    for (const char* flag : {"browserVisible", "mixerVisible", "followPlayhead"}) if (has(args, flag)) boolean(args, flag);
    if (has(args, "selectTrackId"))
    {
        const int id = integer(args, "selectTrackId", 0, maxId);
        if (id == model::mainTrackId(*state.song)) throw std::runtime_error("The main track cannot be selected through the view");
        setProperty(out, "selectTrackIndex", controlIndexOf(*state.song, id, false));
    }
    if (has(args, "selectClip"))
    {
        const auto clip = args["selectClip"];
        fields(clip, {"trackId", "clipId", "scene"});
        const int index = regularTrackIndex(*state.song, integer(clip, "trackId", 0, maxId));
        const int uid = integer(clip, "clipId", 0, maxId), scene = has(clip, "scene") ? integer(clip, "scene", 0, 1000000) : -1;
        if (! model::findClip(*state.song, scene >= 0 ? model::ClipLocation::session(index, uid, scene) : model::ClipLocation::arrangement(index, uid)))
            throw model::OpError("target_replaced", "Clip no longer exists at the specified location");
        setProperty(out, "selectClipTrackIndex", index);
    }
    return out;
}
juce::var openEditor(const juce::var& args, const State& state)
{
    fields(args, {"trackId", "deviceId"});
    int index = 0;
    const auto& track = trackFor(state, args, index);
    const int deviceId = integer(args, "deviceId", 0, maxId);
    for (size_t i = 0; i < track.devices.size(); ++i)
        if (track.devices[i].id == deviceId)
        {
            if (! track.devices[i].plugin) throw model::OpError("unsupported_device", "Only hosted plug-ins (VST3, VST2, Audio Unit) have an editor window; native devices show in the device view");
            auto out = copyOf(args);
            setProperty(out, "trackIndex", index);
            setProperty(out, "deviceIndex", static_cast<int>(i));
            return out;
        }
    throw model::OpError("target_replaced", "Device no longer exists on this track (top-level devices only)");
}
juce::var mediaRun(const juce::var& args, const State& state)
{
    fields(args, {"action", "trackId", "clipId", "scene", "mode"});
    const auto action = string(args, "action", 24);
    static const char* trackActions[] = {"freeze", "unfreeze", "bounce_in_place", "bounce_new_track"};
    static const char* clipActions[] = {"reverse", "crop", "toggle_warp", "audio_to_midi", "strip_silence"};
    const bool onTrack = std::any_of(std::begin(trackActions), std::end(trackActions), [&](const char* a) { return action == a; });
    const bool onClip = std::any_of(std::begin(clipActions), std::end(clipActions), [&](const char* a) { return action == a; });
    if (! onTrack && ! onClip) throw std::runtime_error("action must be freeze, unfreeze, bounce_in_place, bounce_new_track, reverse, crop, toggle_warp, audio_to_midi or strip_silence");
    const int index = regularTrackIndex(*state.song, integer(args, "trackId", 0, maxId));
    const auto& track = state.song->tracks[static_cast<size_t>(index)];
    auto out = copyOf(args);
    setProperty(out, "trackIndex", index);
    if (onTrack)
    {
        if (has(args, "clipId") || has(args, "scene") || has(args, "mode")) throw std::runtime_error(action.toStdString() + " takes only trackId");
        if (track.kind == model::TrackKind::Group) throw std::runtime_error("Group tracks cannot be frozen or bounced");
        if (action == "freeze" && track.frozen) throw model::OpError("invalid_request", "The track is already frozen");
        if (action == "unfreeze" && ! track.frozen) throw model::OpError("invalid_request", "The track is not frozen");
        if (track.frozen && action != "unfreeze") throw model::OpError("track_frozen", "Unfreeze the track first");
        return out;
    }
    const int uid = integer(args, "clipId", 0, maxId), scene = has(args, "scene") ? integer(args, "scene", 0, 1000000) : -1;
    if (action == "audio_to_midi") { const auto mode = string(args, "mode", 8); if (mode != "melody" && mode != "harmony" && mode != "drums" && mode != "slices") throw std::runtime_error("mode must be melody, harmony, drums or slices"); }
    else if (has(args, "mode")) throw std::runtime_error("mode applies to audio_to_midi only");
    if (action == "strip_silence" && scene >= 0) throw std::runtime_error("strip_silence works on Arrangement clips");
    const auto* clip = model::findClip(*state.song, scene >= 0 ? model::ClipLocation::session(index, uid, scene) : model::ClipLocation::arrangement(index, uid));
    if (! clip) throw model::OpError("target_replaced", "Clip no longer exists at the specified location");
    if (clip->kind != model::Clip::Kind::Audio) throw model::OpError("unsupported_clip", action.toStdString() + " needs an audio clip");
    if (track.frozen) throw model::OpError("track_frozen", "Unfreeze the track first");
    return out;
}
} // namespace

const std::vector<std::string>& liveOperationNames()
{
    static const std::vector<std::string> names = {"mixer.set", "transport.set", "session.launch", "recording.set", "project.save",
                                                   "plugins.scan", "devices.set_enabled", "project.save_copy", "project.export_audio", "project.export_cancel", "view.set",
                                                   "devices.open_editor", "media.run"};
    return names;
}
bool isLiveOperation(const juce::String& name)
{
    const auto text = name.toStdString();
    return std::find(liveOperationNames().begin(), liveOperationNames().end(), text) != liveOperationNames().end();
}
juce::var normaliseLive(const juce::String& operation, const juce::var& args, const State& state)
{
    if (! state.song) throw std::runtime_error("No document is open");
    if (operation == "mixer.set") return mixerSet(args, state);
    if (operation == "transport.set") return transportSet(args);
    if (operation == "session.launch") return sessionLaunch(args, state);
    if (operation == "recording.set") return recordingSet(args);
    if (operation == "project.save") { fields(args, {}); return copyOf(args); }
    if (operation == "plugins.scan") return pluginsScan(args);
    if (operation == "devices.set_enabled") return deviceEnabled(args, state);
    if (operation == "project.save_copy") return projectSaveCopy(args);
    if (operation == "project.export_audio") return exportAudio(args, state);
    if (operation == "project.export_cancel") { fields(args, {}); return copyOf(args); }
    if (operation == "view.set") return viewSet(args, state);
    if (operation == "devices.open_editor") return openEditor(args, state);
    if (operation == "media.run") return mediaRun(args, state);
    throw model::OpError("unsupported_operation", "Unsupported operation: " + operation.toStdString());
}
} // namespace sonora::control::detail
