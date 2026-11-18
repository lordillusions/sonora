#include "app/AppController.h"
#include "control/SerumLead.h"
#include "control/DeviceControl.h"
#include "engine/TempoAnalysis.h"
#include "io/als/SampleDrop.h"
#include "plugins/PluginDevice.h"
#include "ui/Locators.h"
#include <bit>
#include <set>

namespace sonora::app
{
namespace
{
double gainDbOf(float linear) { return linear <= 0.000316f ? -70.0 : 20.0 * std::log10(static_cast<double>(linear)); }
float linearOf(double db) { return db <= -70.0 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0)); }
control::ApplyResult busy() { return control::ApplyResult::failure("busy", "The application is busy with another operation"); }
} // namespace

control::State AppController::controlState()
{
    control::State state;
    state.song = song_.get(); state.documentId = controlDocumentId_;
    if (exporting_) state.busy = "Export owns the audio graph";
    else if (ui_.recording) state.busy = "Finish recording and draining before agent edits";
    else if (editingDevices_ || syncingModel_) state.busy = "Device operation in progress";
    if (song_) { state.path = song_->sourcePath; state.untitled = isUntitled(); }
    state.dirty = ui_.documentDirty;
    state.history = {canUndo(), canRedo(), undoLabel().toStdString(), redoLabel().toStdString()};
    if (editingDevices_ || syncingModel_)
    { state.song = nullptr; state.revision = std::to_string(history_.version()); return state; }
    uint64_t fingerprint = 1469598103934665603ULL;
    const auto hash = [&fingerprint](uint64_t value) { fingerprint = (fingerprint ^ value) * 1099511628211ULL; };
    hash(controlGrantGeneration_);
    if (auto controls = engine_.controls())
        for (size_t index = 0; index < controls->size(); ++index)
        {
            const auto& track = (*controls)[index];
            const float gain = track.volume.load();
            state.liveGains.push_back(gain);
            control::LiveMixer mixer;
            mixer.volume = gain; mixer.pan = track.pan.load(); mixer.active = track.active.load(); mixer.solo = track.solo.load();
            mixer.armed = track.armed.load(); mixer.monitoring = track.monitoring.load();
            const size_t returns = song_ ? song_->returns.size() : 0;
            for (size_t send = 0; send < returns && send < track.sends.size(); ++send) mixer.sends.push_back(track.sends[send].load());
            state.liveMixer.push_back(std::move(mixer));
            hash(std::bit_cast<uint32_t>(gain)); hash(std::bit_cast<uint32_t>(track.pan.load()));
            hash(track.active.load()); hash(track.solo.load()); hash(track.armed.load());
            for (const auto& send : track.sends) hash(std::bit_cast<uint32_t>(send.load()));
        }
    state.liveTempo = engine_.transport().tempo(); hash(std::bit_cast<uint64_t>(state.liveTempo));
    // Conservative conflict detection includes hosted editor/MIDI writes without serializing plugin state.
    if (auto* graph = engine_.latestGraph())
        for (const auto& node : graph->nodes)
            for (const auto* device : node.processorForModelDevice)
                if (device)
                    for (size_t p = 0; p < device->parameters().size(); ++p)
                        hash(std::bit_cast<uint32_t>(device->getParameter(static_cast<int>(p))));
    state.revision = std::to_string(history_.version()) + ":" + std::to_string(fingerprint);
    return state;
}

// In-place save for the control API: the non-dialog branch of saveThen, reporting instead of asking.
control::ApplyResult AppController::controlSaveInPlace(juce::var& receipt)
{
    if (! song_) return control::ApplyResult::failure("no_document", "No project is open");
    if (collabGuest()) return control::ApplyResult::failure("untitled_document", "A collaboration guest saves a copy through the Save dialog");
    if (exporting_ || ui_.recording || ui_.recordingRequested || editingDevices_ || syncingModel_) return busy();
    const juce::File file(juce::String::fromUTF8(song_->sourcePath.c_str()));
    if (isUntitled() || file.getFileExtension() != ".als")
        return control::ApplyResult::failure("untitled_document", "This project has no file yet; use Save As in the application (it needs a dialog)");
    io::AlsSaveOptions options;
    options.portableReferences = true;
    if (! writeSet(file, options)) return control::ApplyResult::failure("save_failed", ui_.statusText.empty() ? "The project could not be saved" : ui_.statusText);
    receipt = control::object({{"saved", true}, {"path", juce::String::fromUTF8(song_->sourcePath.c_str())}});
    return control::ApplyResult::success();
}

void AppController::initialiseControl()
{
    control::Backend backend;
    backend.state = [this] { return controlState(); };
    backend.apply = [this](model::EditCommand command) { return applyEdit(std::move(command)); };
    backend.undo = [this] {
        const int version = history_.version(); undo();
        return version != history_.version() ? control::ApplyResult::success()
            : control::ApplyResult::failure("undo_conflict", "History could not be undone");
    };
    backend.historyStep = [this](bool undoStep, std::string& label) {
        label = (undoStep ? undoLabel() : redoLabel()).toStdString();
        const int version = history_.version();
        if (undoStep) undo(); else redo();
        return version != history_.version() ? control::ApplyResult::success()
            : control::ApplyResult::failure("undo_conflict", "The history step could not be applied right now");
    };
    backend.syncModel = [this] { if (song_ && ! syncingModel_ && ! editingDevices_ && ! exporting_) syncModelFromEngine(); };
    backend.prepareSerum = [this](model::Device& device, juce::var& report) {
        return control::prepareSerumLead(host_, engine_.sampleRate(), engine_.blockSize(), device, report);
    };
    control::configureDeviceBackend(backend, host_, engine_, [this] { return song_.get(); });
    // Plugin methods may run nested message loops. Keep document/device edits excluded while capturing/preparing.
    const auto capture = backend.captureDevice;
    backend.captureDevice = [this, capture](int track, int device, model::Device& out) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return capture(track, device, out);
    };
    const auto prepare = backend.prepareParameters;
    backend.prepareParameters = [this, prepare](int track, int device, const auto& writes, auto& before, auto& after, auto& report) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return prepare(track, device, writes, before, after, report);
    };
    const auto insert = backend.prepareDevice;
    backend.prepareDevice = [this, insert](const auto& id, auto& device, auto& report) {
        const juce::ScopedValueSetter<bool> guard(syncingModel_, true); return insert(id, device, report);
    };

    // AGT-7e: status.get (transport, recording, export, plug-in scan).
    backend.status = [this] {
        auto& transport = engine_.transport();
        return control::object({
            {"transport", control::object({{"playing", transport.isPlaying()}, {"countingIn", transport.isCountingIn()},
                {"positionBeats", transport.positionBeats()}, {"tempo", transport.tempo()}, {"metronome", transport.metronomeOn()},
                {"loop", control::object({{"enabled", transport.loopEnabled()}, {"startBeat", transport.loopStart()}, {"lengthBeats", transport.loopLength()}})}})},
            {"recording", control::object({{"recording", ui_.recording}, {"requested", ui_.recordingRequested},
                {"mode", ui_.recordingIsSession ? "session" : "arrangement"}})},
            {"export", [this] {
                juce::Array<juce::var> files;
                for (const auto& file : controlExport_.files) files.add(juce::String::fromUTF8(file.c_str()));
                return control::object({{"phase", controlExport_.phase}, {"running", controlExport_.phase == "running"},
                    {"progress", controlExport_.phase == "done" ? 1.0 : static_cast<double>(exportProgress_.load())},
                    {"files", files}, {"error", juce::String::fromUTF8(controlExport_.error.c_str())}});
            }()},
            {"mediaJob", controlMediaStatus()},
            {"pluginScan", control::object({{"running", scanning_.load()}})}});
    };

    // AGT-7e: audio import mirrors the sample-drop path, synchronously (decoding and tempo analysis run on the message thread).
    backend.importAudio = [this](const std::vector<std::string>& paths, int trackId, double startBeat, int scene, juce::var& receipt) {
        if (! song_ || editingDevices_ || syncingModel_ || exporting_) return busy();
        if (ui_.recording || ui_.recordingRequested) return control::ApplyResult::failure("recording_conflict", "Finish recording before importing audio");
        if (collabGuest() && inCollabSession()) return control::ApplyResult::failure("collab_guest", "A guest cannot import audio; ask the host");
        int trackIndex = -1;
        if (trackId >= 0)
            for (size_t i = 0; i < song_->tracks.size(); ++i) if (song_->tracks[i].id == trackId) trackIndex = static_cast<int>(i);
        if (trackId >= 0 && trackIndex < 0) return control::ApplyResult::failure("target_replaced", "Track ID no longer exists");
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::vector<io::DroppedSample> samples;
        for (const auto& path : paths)
        {
            const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String::fromUTF8(path.c_str()))));
            samples.push_back({path, reader ? reader->sampleRate : 0.0, reader ? static_cast<int64_t>(reader->lengthInSamples) : 0});
            if (reader && reader->lengthInSamples > 0)
                if (const auto loaded = engine_.sampleCache().load(path))
                {
                    const auto estimate = engine::estimateSampleTempo(loaded->audio, loaded->sampleRate, song_->tempo);
                    samples.back().detectedBpm = estimate.bpm; samples.back().tempoConfidence = estimate.confidence;
                }
        }
        io::shareStemTempo(samples);
        syncModelFromEngine();
        std::set<int> knownTracks;
        for (const auto& track : song_->tracks) knownTracks.insert(track.id);
        auto plan = io::placeSamples(*song_, samples, io::SampleDropTarget{trackIndex, startBeat, scene, false});
        if (! plan.command.apply) return control::ApplyResult::failure("invalid_request", plan.error.empty() ? "Nothing could be placed" : plan.error);
        std::set<int> knownClips;
        const auto collect = [&](const model::Track& track, std::set<int>& into) {
            for (const auto& clip : track.arrangementClips) into.insert(clip.uid);
            for (const auto& slot : track.slots) if (slot.clip) into.insert(slot.clip->uid);
        };
        for (const auto& track : song_->tracks) collect(track, knownClips);
        const int placed = plan.placed;
        const auto applied = applyEdit(std::move(plan.command));
        if (! applied.ok) return applied;
        juce::Array<juce::var> tracks, clips;
        for (const auto& track : song_->tracks)
        {
            if (! knownTracks.count(track.id)) tracks.add(control::object({{"trackId", track.id}, {"kind", model::toString(track.kind)}, {"name", track.name}}));
            for (const auto& clip : track.arrangementClips)
                if (! knownClips.count(clip.uid)) clips.add(control::object({{"trackId", track.id}, {"clipId", clip.uid}, {"startBeat", clip.currentStart}, {"endBeat", clip.currentEnd}}));
            for (size_t slot = 0; slot < track.slots.size(); ++slot)
                if (track.slots[slot].clip && ! knownClips.count(track.slots[slot].clip->uid))
                    clips.add(control::object({{"trackId", track.id}, {"clipId", track.slots[slot].clip->uid}, {"scene", static_cast<int>(slot)}}));
        }
        receipt = control::object({{"placed", placed}, {"skipped", static_cast<int>(samples.size()) - placed}, {"tracks", tracks}, {"clips", clips}});
        return control::ApplyResult::success();
    };

    // AGT-7e live actions: no undo, absolute values. The service already validated and resolved the track.
    backend.live["mixer.set"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_ || exporting_ || editingDevices_ || syncingModel_) return busy();
        const auto controls = engine_.controls();
        const int index = args["trackIndex"];
        if (! controls || index < 0 || static_cast<size_t>(index) >= controls->size())
            return control::ApplyResult::failure("target_replaced", "The mixer of that track is not built yet");
        auto& c = (*controls)[static_cast<size_t>(index)];
        const auto snapshot = [&c, returns = song_->returns.size()] {
            juce::Array<juce::var> sends;
            for (size_t i = 0; i < returns && i < c.sends.size(); ++i) sends.add(control::object({{"sendIndex", static_cast<int>(i)}, {"gainDb", gainDbOf(c.sends[i].load())}}));
            return control::object({{"gainDb", gainDbOf(c.volume.load())}, {"pan", static_cast<double>(c.pan.load())}, {"muted", ! c.active.load()},
                                    {"solo", c.solo.load()}, {"armed", c.armed.load()}, {"sends", sends}});
        };
        const auto previous = snapshot();
        if (args.hasProperty("armed") && static_cast<bool>(args["armed"]) != c.armed.load())
        {
            ui_.armTrack(index, static_cast<bool>(args["armed"])); // the UI path: refuses while that track records
            if (c.armed.load() != static_cast<bool>(args["armed"]))
                return control::ApplyResult::failure("recording_conflict", ui_.statusText.empty() ? "The track could not be armed right now" : ui_.statusText);
        }
        if (args.hasProperty("gainDb")) c.volume.store(linearOf(args["gainDb"]));
        if (args.hasProperty("pan")) c.pan.store(static_cast<float>(static_cast<double>(args["pan"])));
        if (args.hasProperty("muted")) c.active.store(! static_cast<bool>(args["muted"]));
        if (args.hasProperty("solo")) c.solo.store(static_cast<bool>(args["solo"]));
        if (const auto* sends = args["sends"].getArray())
            for (const auto& send : *sends)
            {
                const auto slot = static_cast<size_t>(static_cast<int>(send["sendIndex"]));
                if (slot < c.sends.size()) c.sends[slot].store(linearOf(send["gainDb"]));
            }
        // The hand-edit detector would notice these values within a quarter of a second; marking now advances the history
        // version before the receipt's revision is read, so the next guarded request can use that revision.
        if (args.hasProperty("gainDb") || args.hasProperty("pan") || args.hasProperty("muted") || args.hasProperty("solo") || args.hasProperty("sends")) markHandEdited();
        receipt = control::object({{"trackId", args["trackId"]}, {"previous", previous}, {"current", snapshot()}, {"undoable", false},
                                   {"persistence", "Live mixer values; the set takes them when it is saved"}});
        return control::ApplyResult::success();
    };
    backend.live["transport.set"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_ || exporting_) return busy();
        auto& transport = engine_.transport();
        const auto previous = control::object({{"playing", transport.isPlaying()}, {"positionBeats", transport.positionBeats()}, {"metronome", transport.metronomeOn()}});
        const auto action = args["action"].toString();
        const bool positioned = args.hasProperty("positionBeats");
        if (action == "return_to_start") transport.setPosition(0);
        if (positioned) transport.setPosition(static_cast<double>(args["positionBeats"]));
        if (action == "stop") transport.stop();
        else if (action == "continue" || (action == "play" && positioned)) transport.play();
        else if (action == "play") ui::startPlayback(ui_); // like the Play menu item: honours a Song Start locator
        if (args.hasProperty("metronome")) transport.setMetronome(static_cast<bool>(args["metronome"]));
        receipt = control::object({{"previous", previous}, {"requested", args}, {"undoable", false},
                                   {"note", "Transport changes are asynchronous; read status.get for the position and playing state"}});
        return control::ApplyResult::success();
    };
    backend.live["session.launch"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_ || exporting_ || editingDevices_ || syncingModel_) return busy();
        const auto action = args["action"].toString();
        const int track = args.hasProperty("trackIndex") ? static_cast<int>(args["trackIndex"]) : -1;
        const int scene = args.hasProperty("scene") ? static_cast<int>(args["scene"]) : -1;
        if (action == "clip") { if (! ui_.launchClip) return busy(); ui_.launchClip(track, scene); }
        else if (action == "scene") { if (! ui_.launchScene) return busy(); ui_.launchScene(scene); }
        else if (action == "stop_track") { if (! ui_.stopTrack) return busy(); ui_.stopTrack(track); }
        else if (action == "stop_all") { if (! ui_.stopAllClips) return busy(); ui_.stopAllClips(); }
        else if (! ui_.backToArrangement) return busy();
        else ui_.backToArrangement();
        receipt = control::object({{"requested", args}, {"undoable", false}, {"note", "Launches start at the clip's launch quantisation"}});
        return control::ApplyResult::success();
    };
    backend.live["recording.set"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_ || exporting_ || editingDevices_ || syncingModel_) return busy();
        const bool session = args["mode"].toString() == "session", want = static_cast<bool>(args["recording"]);
        const bool active = ui_.recording || ui_.recordingRequested;
        if (want && active) return control::ApplyResult::failure("recording_conflict", "A recording is already running or requested");
        if (! want && ! active)
        { receipt = control::object({{"changed", false}, {"undoable", false}}); return control::ApplyResult::success(); }
        if (! want && ui_.recordingIsSession != session) return control::ApplyResult::failure("recording_conflict", "The running recording is of the other mode");
        if (session) toggleSessionRecording(); else toggleRecording();
        receipt = control::object({{"changed", true}, {"requested", want}, {"undoable", false},
                                   {"note", "Recording starts after the input permission and device checks; poll status.get recording"}});
        return control::ApplyResult::success();
    };
    backend.live["project.save"] = [this](const juce::var&, juce::var& receipt) { return controlSaveInPlace(receipt); };
    backend.live["plugins.scan"] = [this](const juce::var& args, juce::var& receipt) {
        if (scanning_.load()) return control::ApplyResult::failure("busy", "A plug-in scan is already running");
        if (exporting_ || ui_.recording) return busy();
        scanPlugins(args.hasProperty("includeAudioUnits") && static_cast<bool>(args["includeAudioUnits"]));
        receipt = control::object({{"started", true}, {"undoable", false}, {"note", "Poll status.get pluginScan.running, then devices.catalog"}});
        return control::ApplyResult::success();
    };
    backend.live["devices.set_enabled"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_ || exporting_ || editingDevices_ || syncingModel_) return busy();
        auto* graph = engine_.latestGraph();
        const int trackIndex = args["trackIndex"], deviceIndex = args["deviceIndex"];
        if (! graph || trackIndex < 0 || static_cast<size_t>(trackIndex) >= graph->nodes.size()
            || deviceIndex < 0 || static_cast<size_t>(deviceIndex) >= graph->nodes[static_cast<size_t>(trackIndex)].processorForModelDevice.size())
            return control::ApplyResult::failure("target_replaced", "The device is not part of the running graph");
        auto* processor = graph->nodes[static_cast<size_t>(trackIndex)].processorForModelDevice[static_cast<size_t>(deviceIndex)];
        if (! processor) return control::ApplyResult::failure("unsupported_device", "That device has no running processor (unsupported or deactivated)");
        const bool previous = ! processor->isBypassed(), enabled = static_cast<bool>(args["enabled"]);
        processor->setBypassed(! enabled); // the device view's power button does exactly this
        if (auto* track = model::trackAt(*song_, trackIndex))
            if (static_cast<size_t>(deviceIndex) < track->devices.size()) track->devices[static_cast<size_t>(deviceIndex)].enabled = enabled;
        markHandEdited();
        receipt = control::object({{"previousEnabled", previous}, {"enabled", enabled}, {"undoable", false},
                                   {"persistence", "Live device switch; the set takes it when it is saved"}});
        return control::ApplyResult::success();
    };
    installControlActions(backend);
    control_ = std::make_unique<control::ControlService>(std::move(backend));
}

// What status.get says of the last media job started through media.run. The jobs run through runMediaEdit (one history step when
// they succeed) or their own worker; completion is seen as the history moving, a failure panel, or the job ending without a change.
juce::var AppController::controlMediaStatus()
{
    if (! controlMedia_.started) return control::object({{"state", "idle"}});
    const bool working = (exporting_ && controlExport_.phase != "running") || stemsRunning_ || ! warpAnalysisPending_.empty();
    const auto elapsed = juce::Time::getMillisecondCounter() - controlMedia_.startedMs;
    juce::String state = "running";
    if (! working)
    {
        if (history_.version() != controlMedia_.version) state = "done";
        else if (ui_.exportState.phase == ui::ExportState::Phase::Failed && ! ui_.exportState.message.empty()) state = "failed";
        else if (elapsed > 2500) state = "finished_without_change";
    }
    return control::object({{"state", state}, {"action", controlMedia_.action}, {"message", ui_.statusText},
        {"undoLabel", undoLabel()}, {"seconds", static_cast<double>(elapsed) / 1000.0}});
}

void AppController::installControlActions(control::Backend& backend)
{
    const auto ready = [this] { return song_ && ! exporting_ && ! editingDevices_ && ! syncingModel_; };
    backend.live["project.save_copy"] = [this, ready](const juce::var& args, juce::var& receipt) {
        if (! ready()) return busy();
        if (collabGuest()) return control::ApplyResult::failure("untitled_document", "A collaboration guest saves a copy through the Save dialog");
        const juce::File file(args["path"].toString());
        const auto result = writeCopy(file);
        if (! result.ok) return control::ApplyResult::failure("save_failed", result.error);
        receipt = control::object({{"saved", false}, {"copyPath", file.getFullPathName()}, {"undoable", false},
                                   {"note", "A copy was written; the open project keeps its own file and unsaved state"}});
        return control::ApplyResult::success();
    };
    backend.live["project.export_audio"] = [this, ready](const juce::var& args, juce::var& receipt) {
        if (! ready()) return busy();
        if (ui_.recording || ui_.recordingRequested) return control::ApplyResult::failure("recording_conflict", "Stop recording before exporting");
        engine::ExportOptions o;
        const auto target = args["target"].toString(), format = args["format"].toString();
        o.target = target == "main" ? engine::ExportTarget::Main : target == "stems" ? engine::ExportTarget::AllTracks : engine::ExportTarget::SingleTrack;
        o.trackIndex = target == "track" ? static_cast<int>(args["trackIndex"]) : -1;
        o.startBeat = static_cast<double>(args["startBeat"]); o.endBeat = static_cast<double>(args["endBeat"]);
        o.format = format == "aiff" ? engine::ExportFormat::Aiff : format == "flac" ? engine::ExportFormat::Flac : engine::ExportFormat::Wav;
        o.bitDepth = static_cast<int>(args["bitDepth"]);
        o.normalize = args.hasProperty("normalize") && static_cast<bool>(args["normalize"]);
        o.mono = args.hasProperty("mono") && static_cast<bool>(args["mono"]);
        o.dither = ! args.hasProperty("dither") || static_cast<bool>(args["dither"]);
        o.autoTail = args.hasProperty("autoTail") && static_cast<bool>(args["autoTail"]);
        o.tailSeconds = args.hasProperty("tailSeconds") ? static_cast<double>(args["tailSeconds"]) : 0.0;
        o.destination = juce::File(args["destination"].toString());
        o.baseName = isUntitled() ? std::string("Untitled") : juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getFileNameWithoutExtension().toStdString();
        if (o.target == engine::ExportTarget::AllTracks && ! o.destination.createDirectory())
            return control::ApplyResult::failure("export_failed", "Could not create the stems folder");
        beginExport(o);
        if (! exporting_) return control::ApplyResult::failure("export_failed", controlExport_.error.empty() ? "The export did not start" : controlExport_.error);
        receipt = control::object({{"started", true}, {"destination", o.destination.getFullPathName()}, {"undoable", false},
                                   {"note", "Asynchronous: poll status.get export (phase, progress, files, error)"}});
        return control::ApplyResult::success();
    };
    backend.live["project.export_cancel"] = [this](const juce::var&, juce::var& receipt) {
        if (controlExport_.phase != "running" || ! exporting_) return control::ApplyResult::failure("invalid_request", "No export is running");
        cancelExport();
        receipt = control::object({{"requested", true}, {"undoable", false}, {"note", "Poll status.get export until the phase is cancelled"}});
        return control::ApplyResult::success();
    };
    backend.live["view.set"] = [this](const juce::var& args, juce::var& receipt) {
        if (! song_) return busy();
        const auto previous = control::object({{"mainView", ui_.mainView == ui::MainView::Session ? "session" : "arrangement"},
            {"detailVisible", ui_.detailVisible}, {"browserVisible", ui_.browserVisible}, {"mixerVisible", ui_.mixerVisible}, {"followPlayhead", ui_.followPlayhead}});
        if (args.hasProperty("mainView")) ui_.mainView = args["mainView"].toString() == "session" ? ui::MainView::Session : ui::MainView::Arrangement;
        if (args.hasProperty("detail"))
        {
            const auto detail = args["detail"].toString();
            ui_.detailVisible = detail != "hidden";
            if (detail == "clip") ui_.detailTab = ui::DetailTab::Clip;
            if (detail == "devices") ui_.detailTab = ui::DetailTab::Devices;
        }
        if (args.hasProperty("selectTrackIndex")) ui_.selectTrack(static_cast<int>(args["selectTrackIndex"]), false, false);
        if (args.hasProperty("selectClip"))
        {
            ui_.selectedClipTrack = static_cast<int>(args["selectClipTrackIndex"]);
            ui_.selectedClipUid = static_cast<int>(args["selectClip"]["clipId"]);
            ui_.selectedClipScene = args["selectClip"].hasProperty("scene") ? static_cast<int>(args["selectClip"]["scene"]) : -1;
        }
        if (args.hasProperty("browserVisible")) ui_.browserVisible = static_cast<bool>(args["browserVisible"]);
        if (args.hasProperty("mixerVisible")) { ui_.mixerVisible = static_cast<bool>(args["mixerVisible"]); if (ui_.layoutChanged) ui_.layoutChanged(); }
        if (args.hasProperty("followPlayhead")) { ui_.followPlayhead = static_cast<bool>(args["followPlayhead"]); ui_.followPaused = false; }
        if (ui_.requestRepaint) ui_.requestRepaint();
        receipt = control::object({{"previous", previous}, {"undoable", false}});
        return control::ApplyResult::success();
    };
    backend.live["devices.open_editor"] = [this, ready](const juce::var& args, juce::var& receipt) {
        if (! ready()) return busy();
        openPluginEditor(static_cast<int>(args["trackIndex"]), static_cast<int>(args["deviceIndex"]));
        receipt = control::object({{"opened", true}, {"undoable", false}});
        return control::ApplyResult::success();
    };
    backend.live["media.run"] = [this, ready](const juce::var& args, juce::var& receipt) {
        if (! ready() || stemsRunning_) return busy();
        if (ui_.recording || ui_.recordingRequested) return control::ApplyResult::failure("recording_conflict", "Stop recording first");
        const auto action = args["action"].toString();
        const int track = static_cast<int>(args["trackIndex"]);
        const int uid = args.hasProperty("clipId") ? static_cast<int>(args["clipId"]) : -1, scene = args.hasProperty("scene") ? static_cast<int>(args["scene"]) : -1;
        const auto where = scene >= 0 ? model::ClipLocation::session(track, uid, scene) : model::ClipLocation::arrangement(track, uid);
        controlMedia_ = {true, action.toStdString(), history_.version(), juce::Time::getMillisecondCounter()};
        ui_.exportState = {};
        if (action == "freeze" || action == "unfreeze") renderTrack(track, true, false);
        else if (action == "bounce_in_place") renderTrack(track, false, false);
        else if (action == "bounce_new_track") renderTrack(track, false, true);
        else if (action == "reverse") reverseClipSample(where);
        else if (action == "crop") cropClipSample(where);
        else if (action == "toggle_warp") toggleClipWarp(where);
        else if (action == "strip_silence") stripSilence(where);
        else if (action == "audio_to_midi")
        {
            const auto mode = args["mode"].toString();
            convertAudioToMidi(where, mode == "melody" ? 0 : mode == "harmony" ? 1 : mode == "drums" ? 2 : 3);
        }
        receipt = control::object({{"started", true}, {"action", action}, {"undoable", false},
                                   {"note", "Asynchronous: poll status.get mediaJob; a successful job is one history step (see history.undoLabel)"}});
        return control::ApplyResult::success();
    };
}
control::ApplyResult AppController::enableLocalControl(bool enabled)
{
    ++controlGrantGeneration_;
    if (! enabled) { controlEndpoint_.disable(); return control::ApplyResult::success(); }
    if (! song_) return control::ApplyResult::failure("no_document", "Open a project before enabling local automation");
    const auto document = controlDocumentId_;
    return controlEndpoint_.enable(control_->instanceId(), document, [this, document](const juce::var& request) {
        if (controlDocumentId_ != document)
            return control::object({{"ok", false}, {"error", control::object({{"code", "stale_document"}, {"message", "Document changed; enable automation again"}})}});
        auto result = control_->execute(request);
        if (onControlActivity) onControlActivity(request["operation"].toString(), result);
        return result;
    });
}
} // namespace sonora::app
