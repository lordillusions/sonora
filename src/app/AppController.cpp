#include "app/AppController.h"
#include "app/CollabPort.h"
#include "app/PromptWindow.h"
#include "engine/TempoAnalysis.h"
#include "app/SaveChanges.h"

#include <set>
#include <cstring>

#include "engine/ClipLauncher.h"
#include "engine/ControlRouter.h"
#include "io/als/AlsReader.h"
#include "engine/SampleExport.h"
#include "engine/AudioFileFormats.h"
#include "io/als/AlsNewSet.h"
#include "ui/AudioClipEdits.h"
#include "io/als/AlsWriter.h"
#include "io/midi/MidiFileExport.h"
#include "io/als/AlsTemplates.h"
#include "io/als/SampleDrop.h"
#include "core/AppDataFolder.h"
#include "core/DeviceCatalog.h"
#include "plugins/Vst3PresetBuilder.h"
#include "midi/MappingStore.h"
#include "plugins/PluginDevice.h"
#include "devices/RackProcessor.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
#include "devices/native/OscillatorMedia.h"
#include "plugins/SerumPatch.h"
#include "plugins/SerumLibrary.h"
#include "plugins/PrismPreset.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/WavetableWindowView.h"
#include "util/Log.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace sonora::app
{

namespace
{
model::Device* wavetableTarget(model::Device& parent, int chain, int child)
{
    if (chain < 0) return parent.type == "SonoraWavetable" ? &parent : nullptr;
    if (static_cast<size_t>(chain) >= parent.chains.size() || child < 0
        || static_cast<size_t>(child) >= parent.chains[static_cast<size_t>(chain)].devices.size()) return nullptr;
    auto& target = parent.chains[static_cast<size_t>(chain)].devices[static_cast<size_t>(child)];
    return target.type == "SonoraWavetable" ? &target : nullptr;
}
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(juce::AudioPluginInstance& plugin, std::function<void(PluginWindow*)> onClose)
        : DocumentWindow(plugin.getName(), juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          plugin_(&plugin), onClose_(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor(plugin);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { onClose_(this); }
    const juce::AudioPluginInstance* plugin() const { return plugin_; }

private:
    const juce::AudioPluginInstance* plugin_;
    std::function<void(PluginWindow*)> onClose_;
};

// UI-27: the Sonora Prism dashboard in its own window, like PluginWindow: a second Skia canvas over the shared
// UiContext. Unlike plug-in windows it outlives device edits; the view re-resolves its instance after each graph change.
class WavetableWindow : public juce::DocumentWindow
{
public:
    WavetableWindow(ui::UiContext& ui, const ui::UiContext::WavetableKey& key, std::function<void(WavetableWindow*)> onClose,
                    std::function<bool(const juce::KeyPress&)> forwardKey)
        : DocumentWindow("Sonora Prism", juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          view_(ui, key), onClose_(std::move(onClose)), forwardKey_(std::move(forwardKey))
    {
        setUsingNativeTitleBar(true);
        view_.onTitleChanged = [this](const std::string& title) { setName(juce::String::fromUTF8(title.c_str())); };
        view_.sync();
        canvas_ = new gfx::SkiaCanvasComponent(view_);
        const float zoom = std::clamp(ui.uiZoom, 0.5f, 3.0f);
        canvas_->setSize(juce::roundToInt(ui::WavetableWindowView::kPreferredWidth * zoom),
                         juce::roundToInt(ui::WavetableWindowView::kPreferredHeight * zoom));
        setContentOwned(canvas_, true);
        setResizable(true, false);
        setResizeLimits(720, 320, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        canvas_->grabKeyboardFocus();
    }
    ~WavetableWindow() override { clearContentComponent(); } // the canvas references view_

    void closeButtonPressed() override { onClose_(this); }
    // Keys the editor did not use bubble up here: transport and view shortcuts still reach the main window.
    bool keyPressed(const juce::KeyPress& key) override { return forwardKey_ && forwardKey_(key); }
    const ui::UiContext::WavetableKey& key() const { return view_.key(); }
    void setKey(const ui::UiContext::WavetableKey& key) { view_.setKey(key); }
    void sync()
    {
        view_.sync();
        if (canvas_) canvas_->requestFrame();
    }

private:
    ui::WavetableWindowView view_;
    gfx::SkiaCanvasComponent* canvas_ = nullptr; // owned by the window (content component)
    std::function<void(WavetableWindow*)> onClose_;
    std::function<bool(const juce::KeyPress&)> forwardKey_;
};

// DEV-23: a copy of a replaced Max for Live patch for Live to load: the project's imported presets, then the User Library.
std::string locateMaxPatch(const model::Song& song, const std::string& userLibrary, const std::string& patch)
{
    const auto name = juce::String(patch) + ".amxd";
    std::vector<juce::File> candidates;
    if (! song.sourcePath.empty())
        candidates.push_back(juce::File(song.sourcePath).getParentDirectory().getChildFile("Presets/Audio Effects/Max Audio Effect/Imported").getChildFile(name));
    if (! userLibrary.empty())
        for (const char* folder : {"Presets/Audio Effects/Max Audio Effect/Imported", "Presets/Audio Effects/Max Audio Effect"})
            candidates.push_back(juce::File(userLibrary).getChildFile(folder).getChildFile(name));
    for (const auto& file : candidates)
        if (file.existsAsFile()) return file.getFullPathName().toStdString();
    return {};
}
} // namespace

AppController::AppController() : engine_(host_, midi_)
{
    ui_.transport = &engine_.transport();
    ui_.monitor = &engine_.monitor();
    ui_.midi = &midi_;
    ui_.openFileDialog = [this] { openFileDialog(); };
    ui_.openFile = [this](const std::string& p) { openFile(juce::File(juce::String::fromUTF8(p.c_str()))); };
    ui_.openPluginEditor = [this](int t, int d) { openPluginEditor(t, d); };
    ui_.wavetableFloatingDefault = true; // UI-27: the dashboard opens in its own window unless docked by hand
    ui_.openWavetableWindow = [this](const ui::UiContext::WavetableKey& key) { openWavetableWindow(key); };
    ui_.closeWavetableWindow = [this](const ui::UiContext::WavetableKey& key) { closeWavetableWindow(key); };
    auto launchQuant = [this] {
        return song_ ? engine::quantisationBeats(song_->globalQuantisation, song_->timeSigNumerator, song_->timeSigDenominator) : 0.0;
    };
    auto sessionClip = [this](int t, int s) -> const model::Clip* {
        if (! song_ || t < 0 || (size_t) t >= song_->tracks.size()) return nullptr;
        const auto& slots = song_->tracks[(size_t) t].slots;
        return s >= 0 && (size_t) s < slots.size() && slots[(size_t) s].clip ? &*slots[(size_t) s].clip : nullptr;
    };
    auto clipQuant = [this](const model::Clip& clip) {
        return song_ ? engine::clipQuantisationBeats(clip, song_->globalQuantisation, song_->timeSigNumerator, song_->timeSigDenominator) : 0.0;
    };
    ui_.launchClip = [this, launchQuant, sessionClip, clipQuant](int t, int s) {
        if (ui_.recording && engine_.recorder().sessionScene(t) >= 0)
        { ui_.statusText = "Finish Session recording before launching another clip on this track"; return; }
        auto* g = engine_.latestGraph();
        if (g == nullptr)
            return;
        if (const auto* clip = sessionClip(t, s))
            engine::launchClipWithMode(*g, engine_.transport(), *clip, t, s, clipQuant(*clip));
        else
            engine::launchClip(*g, engine_.transport(), t, s, launchQuant());
    };
    ui_.releaseClip = [this, sessionClip, clipQuant](int t, int s) {
        if (auto* g = engine_.latestGraph(); g != nullptr)
            if (const auto* clip = sessionClip(t, s))
                engine::releaseClip(*g, engine_.transport(), *clip, t, s, clipQuant(*clip));
    };
    ui_.stopTrack = [this, launchQuant](int t) {
        if (ui_.recording && engine_.recorder().sessionScene(t) >= 0)
        { engine_.recorder().requestSessionStop(t, false); }
        if (auto* g = engine_.latestGraph())
            engine::stopTrack(*g, engine_.transport(), t, launchQuant());
    };
    ui_.launchScene = [this, launchQuant](int s) {
        if (ui_.recording && engine_.recorder().sessionRecording())
        { ui_.statusText = "Finish Session recording before launching another scene"; return; }
        if (auto* g = engine_.latestGraph(); g != nullptr && song_)
            engine::launchScene(*g, engine_.transport(), *song_, s, launchQuant());
    };
    ui_.stopAllClips = [this, launchQuant] {
        if (ui_.recording && engine_.recorder().sessionRecording())
        { engine_.recorder().requestSessionStop(-1, false); }
        if (auto* g = engine_.latestGraph())
            engine::stopAllClips(*g, engine_.transport(), launchQuant());
    };
    ui_.backToArrangement = [this] {
        if (ui_.recording && engine_.recorder().sessionRecording())
        { ui_.statusText = "Finish Session recording before returning to Arrangement"; return; }
        if (auto* g = engine_.latestGraph())
            engine::backToArrangement(*g);
    };
    peaks_.setPool(&peakPool_);
    peaks_.onReady = [this, alive = std::weak_ptr<bool>(alive_)] {
        juce::MessageManager::callAsync([this, alive] {
            if (! alive.expired() && ui_.requestRepaint)
                ui_.requestRepaint();
        });
    };
    ui_.peaks = [this](const model::Clip& clip) { return song_ ? peaks_.request(*song_, clip) : nullptr; };
    ui_.openBrowserFolder = [this](const std::string& path) { openBrowserFolder(path); };
    ui_.expandBrowserFolder = [this](const std::string& path) { readBrowserFolder(path, false); };
    ui_.addBrowserFolder = [this] { addBrowserFolder(); };
    ui_.previewSample = [this](const std::string& path) { engine_.previewSample(path); };
    ui_.dropBrowserItem = [this](const ui::BrowserDrag& item, int track, double beat, int scene) {
        dropBrowserItem(item, track, beat, scene);
    };
    ui_.editDevice = [this](int track, int device, const auto& edit, const std::string& label) { editDevice(track, device, edit, label); };
    ui_.importWavetable = [this](int track, int device, int oscillator, const auto& path, int chain, int child) {
        importWavetable(track, device, oscillator, path, chain, child);
    };
    ui_.importOscillatorMedia = [this](int track, int device, int oscillator, const auto& path, int chain, int child) {
        importOscillatorMedia(track, device, oscillator, path, chain, child);
    };
    ui_.importWavetablePreset = [this](int track, int device, const auto& path, int chain, int child) {
        importWavetablePreset(track, device, path, chain, child);
    };
    ui_.editWavetableLfo = [this](int track, int device, int lfo, const auto& curve, int chain, int child) {
        editWavetableLfo(track, device, lfo, curve, chain, child);
    };
    ui_.editWavetableModulationCurve = [this](int track, int device, int slot, bool auxiliary, const auto& curve, bool enabled, int chain, int child) {
        editWavetableModulationCurve(track, device, slot, auxiliary, curve, enabled, chain, child);
    };
    ui_.setWavetableEffects = [this](int track, int device, const auto& configuration, int chain, int child) {
        setWavetableEffects(track, device, configuration, chain, child);
    };
    ui_.importWavetableImpulseResponse = [this](int track, int device, auto moduleId, const auto& path, int chain, int child) {
        importWavetableImpulseResponse(track, device, moduleId, path, chain, child);
    };
    ui_.dropDeviceItem = [this](const auto& item, int track, int device, int chain, int zone, int child) { dropDeviceItem(item, track, device, chain, zone, child); };
    for (const auto& place : juce::StringArray::fromTokens(settings().getValue("browserPlaces"), "\n", ""))
        ui_.browserPlaces.push_back(place.toStdString());
    ui_.toggleRecording = [this] { toggleRecording(); };
    ui_.recordSessionClip = [this](int track, int scene) {
        if (ui_.recording)
        {
            if (! engine_.recorder().sessionRecording())
            { ui_.statusText = "Finish Arrangement recording before recording a Session slot"; return; }
            if (engine_.recorder().sessionScene(track) >= 0)
            {
                if (engine_.recorder().sessionScene(track) == scene)
                { engine_.recorder().requestSessionStop(track); ui_.statusText = "Session track stop queued"; }
                else ui_.statusText = "Finish this track's recording before recording another slot on it";
                return;
            }
        }
        if (ui_.recordingRequested) return;
        requestedRecordingTrack_ = track; requestedRecordingScene_ = scene; ensureAudioInputs(true);
    };
    ui_.recordingSessionScene = [this](int track) { return engine_.recorder().sessionScene(track); };
    ui_.sessionRecordingPhase = [this](int track) { return engine_.recorder().sessionPhase(track); };
    ui_.audioInputChannelCount = [this] { return std::max(1, engine_.inputChannelCount() > 0 ? engine_.inputChannelCount() : 2); };
    ui_.midiInputDevices = [this] {
        std::vector<std::pair<std::string, std::string>> devices;
        for (const auto& input : midi_.availableInputs()) devices.emplace_back(input.identifier.toStdString(), input.name.toStdString());
        return devices;
    };
    ui_.armTrack = [this](int track, bool armed) {
        if (ui_.recording && (! engine_.recorder().sessionRecording() || engine_.recorder().capturesTrack(track)))
        { ui_.statusText = "Finish this track's recording before changing its arm state"; return; }
        inputPermissionDenied_ = false;
        if (const auto& controls = engine_.controls(); controls && track >= 0 && (size_t) track < controls->size())
        { (*controls)[(size_t) track].armed.store(armed); ensureAudioInputs(); }
    };
    ui_.applyEdit = [this](model::EditCommand c) { applyEdit(std::move(c)); };
    engine_.beforeReprepare = [this] { beginDeviceEdit(); };
    engine_.afterReprepare = [this] { afterEdit({}, false, true); };
    ui_.undo = [this] { undo(); };
    ui_.redo = [this] { redo(); };
    ui_.saveFile = [this] { saveFile(); };
    ui_.saveFileAs = [this] { saveFileAs(); };
    ui_.newProject = [this] { newProject(); };
    ui_.renameSelection = [this] { renameSelection(); };
    ui_.duplicateSelectedTracks = [this] {
        if (! song_ || ui_.recording || ui_.recordingRequested || editingDevices_ || syncingModel_ || exporting_) return;
        syncModelFromEngine();
        auto prepared = io::prepareTrackDuplication(*song_, ui_.selectedTrackIndices());
        if (! prepared.error.empty()) { ui_.statusText = prepared.error; return; }
        if (prepared.tracks.empty()) return;
        std::set<model::TrackId> ids;
        for (const auto& track : prepared.tracks) ids.insert(track.id);
        std::set<model::TrackId> tops;
        for (const auto& track : prepared.tracks) if (! ids.count(track.groupId)) tops.insert(track.id);
        auto result = applyEdit(model::insertTracks(std::move(prepared.tracks), prepared.pointeeEnd, "Duplicate Tracks", prepared.before));
        if (! result.ok) return;
        ui_.trackSelection.clear();
        for (int index = 0; index < ui_.numTracks(); ++index)
            if (tops.count(song_->tracks[(size_t) index].id)) ui_.trackSelection.insert(index);
        ui_.selectedTrack = ui_.trackSelection.empty() ? 0 : *ui_.trackSelection.begin();
        ui_.trackSelectionAnchor = ui_.selectedTrack;
        ui_.sessionTrackSelection = true;
        ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = ui_.selectedTakeLane = -1;
        ui_.detailTab = ui::DetailTab::Devices;
        if (ui_.modelChanged) ui_.modelChanged();
    };
    ui_.deleteSelectedTracks = [this] {
        if (! song_ || ui_.recording || ui_.recordingRequested || editingDevices_ || syncingModel_ || exporting_) return;
        syncModelFromEngine();
        auto command = model::deleteTracks(*song_, ui_.selectedChannelIndices());
        if (command.apply) applyEdit(std::move(command));
    };
    ui_.insertTrack = [this](bool midiTrack) {
        if (! song_ || ui_.recording || ui_.recordingRequested || editingDevices_ || syncingModel_ || exporting_) return;
        int end = 0;
        const int position = std::clamp(ui_.selectedTrack + 1, 0, ui_.numTracks());
        auto track = midiTrack ? io::createMidiTrack(*song_, "MIDI", 4, end)
                               : io::createAudioTrack(*song_, "Audio", 18, -1, -1, end);
        const bool unfold = ui_.selectedTrack < ui_.numTracks() && ui_.trackAt(ui_.selectedTrack)
                            && ui_.trackAt(ui_.selectedTrack)->kind == model::TrackKind::Group && ui_.trackAt(ui_.selectedTrack)->folded;
        if (const auto* parent = ui_.selectedTrack < ui_.numTracks() ? ui_.trackAt(ui_.selectedTrack) : nullptr)
        {
            track.groupId = parent->kind == model::TrackKind::Group ? parent->id : parent->groupId;
            if (track.groupId >= 0) { track.outputRouting = "AudioOut/GroupTrack"; track.outputRoutingEdited = true; }
        }
        const int id = track.id;
        auto command = model::insertTracks({std::move(track)}, end, midiTrack ? "Insert MIDI Track" : "Insert Audio Track", position);
        if (unfold)
            command = model::compose(command.label, {model::setTrackFolded(*song_, ui_.selectedTrack, false), command});
        applyEdit(std::move(command));
        for (int i = 0; i < ui_.numTracks(); ++i)
            if (song_->tracks[(size_t) i].id == id) { ui_.selectTrack(i, false, false); break; }
        ui_.sessionTrackSelection = true;
        ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = ui_.selectedTakeLane = -1;
        ui_.detailTab = ui::DetailTab::Devices;
        if (ui_.modelChanged) ui_.modelChanged();
    };
    ui_.insertReturnTrack = [this] {
        if (! song_ || ui_.recording || ui_.recordingRequested || editingDevices_ || syncingModel_ || exporting_) return;
        if (song_->returns.size() >= model::maxReturnTracks)
        { ui_.statusText = "A set can have at most 12 return tracks"; if (ui_.requestRepaint) ui_.requestRepaint(); return; }
        auto command = model::insertReturnTrack(*song_, io::createReturnTrack(*song_));
        if (! command.apply) return;
        applyEdit(std::move(command));
        ui_.selectTrack(ui_.numTracks() + ui_.numReturns() - 1, false, false);
        ui_.sessionTrackSelection = true;
        ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = ui_.selectedTakeLane = -1;
        ui_.detailTab = ui::DetailTab::Devices;
        if (ui_.modelChanged) ui_.modelChanged();
    };
    ui_.reverseClip = [this](const model::ClipLocation& location) { reverseClipSample(location); };
    ui_.toggleClipWarp = [this](const model::ClipLocation& location) { toggleClipWarp(location); };
    ui_.separateStems = [this](const model::ClipLocation& location) { separateClipStems(location); };
    ui_.consolidateSelection = [this] { consolidateSelection(); };
    ui_.renderTrack = [this](int track, bool freeze, bool toNew) { renderTrack(track, freeze, toNew); };
    ui_.convertAudioToMidi = [this](const auto& location, int mode) { convertAudioToMidi(location, mode); };
    ui_.stripSilence = [this](const model::ClipLocation& location) { stripSilence(location); };
    ui_.jumpToTransient = [this](int direction) { jumpToTransient(direction); };
    ui_.resynthesizeClip = [this](const auto& location) { resynthesizeClip(location); };
    ui_.importResynth = [this](int track, int device, const auto& path, int chain, int child) { importResynth(track, device, path, chain, child); };
    ui_.createResynthPhrase = [this](int track, int device, int chain, int child) { createResynthPhrase(track, device, chain, child); };
    ui_.cropClip = [this](const model::ClipLocation& location) { cropClipSample(location); };
    ui_.automationOverridden = [this] { auto* g = engine_.latestGraph(); return g != nullptr && engine::automationOverridden(*g); };
    ui_.reEnableAutomation = [this] { if (auto* g = engine_.latestGraph()) engine::reEnableAutomation(*g); };
    ui_.captureMidi = [this] { captureMidi(); };
    ui_.captureMidiAvailable = [this] { return capture_.available(); };
    ui_.captureAudio = [this] { captureAudio(); };
    ui_.captureAudioAvailable = [this] { return ! ui_.recording && ! audioCaptureTargets().empty(); };
    ui_.toggleSessionRecording = [this] { toggleSessionRecording(); };
    ui_.computerKeyboardNote = [this](int note, int velocity, bool on) {
        // The Computer MIDI Keyboard behaves like a controller on its own port: notes reach the armed tracks.
        auto message = on ? juce::MidiMessage::noteOn(1, note, (juce::uint8) std::clamp(velocity, 1, 127)) : juce::MidiMessage::noteOff(1, note);
        message.setTimeStamp(juce::Time::getMillisecondCounterHiRes() * 0.001);
        midi_.handleMessage(message, true, false, 0xC0DE);
    };
    midi_.onTrackMessage = [this](const juce::MidiMessage& message) {
        capture_.record(message, juce::Time::getMillisecondCounterHiRes() * 0.001, engine_.transport().positionBeats(), engine_.transport().isPlaying());
    };
    ui_.groupSelectedTracks = [this] { groupSelectedTracks(); };
    ui_.ungroupSelectedTracks = [this] { ungroupSelectedTracks(); };
    ui_.saveCopy = [this] { saveCopy(); };
    ui_.collectAllAndSave = [this] { collectAllAndSave(); };
    ui_.canUndo = [this] { return canUndo(); };
    ui_.canRedo = [this] { return canRedo(); };
    ui_.undoLabel = [this] { return undoLabel().toStdString(); };
    ui_.redoLabel = [this] { return redoLabel().toStdString(); };
    ui_.scanPlugins = [this] { scanPlugins(false); };
    ui_.showAudioSettings = [this] { showAudioSettings(); };

    // Restore settings
    std::unique_ptr<juce::XmlElement> audioState = settings().getXmlValue("audioDevice");
    const auto error = engine_.initialiseAudio(audioState.get());
    logf("audio: %s", error.isEmpty() ? engine_.deviceManager().getCurrentAudioDeviceType().toRawUTF8() : error.toRawUTF8());
    ui_.statusText = error.isEmpty() ? "Audio: " + engine_.deviceManager().getCurrentAudioDeviceType().toStdString()
                                     : "Audio error: " + error.toStdString();

    loadLayout();
    loadExportSettings();
    ui_.startExport = [this](const ui::ExportSettings& settings) { startExport(settings); };
    ui_.cancelExport = [this] { cancelExport(); };
    ui_.revealInFinder = [](const std::string& path) { juce::File(juce::String::fromUTF8(path.c_str())).revealToUser(); };
    ui_.layoutChanged = [this] { saveLayout(); };
    const auto themePath = settings().getValue("themePath");
    if (themePath.isNotEmpty())
        ui_.theme.loadAsk(themePath.toStdString());
    else
        ui_.theme.useBuiltin(settings().getBoolValue("darkTheme", false));

    host_.loadCache();
    initialiseControl();
    refreshPluginNames();
    if (host_.knownPlugins().getNumTypes() == 0)
        ui_.statusText += "  |  No plug-ins scanned yet: Options > Scan Plug-ins";

    midi_.onActivity = [this] { ui_.midiActivityFrames = 6; };
    ui_.mappedControl = [this](const midi::ControlTarget& t, const midi::ControlChange& change) {
        if (t.kind == midi::ControlTarget::Kind::SessionClip)
        {
            if (song_ && engine::applySessionClipControl(t, change, *song_, engine_.latestGraph(), engine_.transport(), &engine_.recorder()))
                if (ui_.requestRepaint) ui_.requestRepaint();
            return;
        }
        if (t.kind == midi::ControlTarget::Kind::TrackArm && ui_.recording
            && (! engine_.recorder().sessionRecording() || engine_.recorder().capturesTrack(t.track))) return;
        if (engine::applyControl(t, change, engine_.latestGraph(), engine_.transport()) && ui_.requestRepaint)
            ui_.requestRepaint();
        if (t.kind == midi::ControlTarget::Kind::TrackArm) { inputPermissionDenied_ = false; ensureAudioInputs(); }
    };
    midi_.onControl = ui_.mappedControl;
    midi_.onMappingsChanged = [this] {
        saveMappings();
        if (ui_.requestRepaint)
            ui_.requestRepaint();
    };
    if (midiSettingsFile().existsAsFile())
        midi_.loadSettings(midiSettingsFile());
    midi_.enableAllInputs(); // new devices get both roles, remembered roles are kept
    midi_.saveSettings(midiSettingsFile());

    engine_.setClockOut(&midiClock_);
    wirePreferences();

    recentSets_.setMaxNumberOfItems(30);
    recentSets_.restoreFromString(settings().getValue("recentSets"));

    findProjectsInBackground();
    initialiseCollab();
    startTimerHz(10);
}

AppController::~AppController()
{
    collabTimer_.stopTimer();
    if (collab_) collab_->leave(); // tells the others before the links close
    engine_.transport().requestHook = nullptr;
    collab_.reset();
    delete audioSettingsWindow_.getComponent();
    controlEndpoint_.disable();
    *alive_ = false;
    exportCancel_ = true;
    stemsCancel_ = true;
    exportPoller_.stopTimer();
    stopTimer();
    automationPoller_.stopTimer();
    saveSettings();
    midi_.saveSettings(midiSettingsFile());
    pool_.removeAllJobs(true, 5000);
    closePluginWindows();
    closeWavetableWindows();
}

juce::PropertiesFile& AppController::settings()
{
    if (settings_ == nullptr)
    {
        juce::PropertiesFile::Options o;
        // COM-4c: <data folder>/<data folder>.settings, so "Sonora/Sonora.settings" for stable as before.
        const auto folder = juce::String::fromUTF8(core::appDataFolderName().c_str());
        o.applicationName = folder;
        o.filenameSuffix = ".settings";
        o.osxLibrarySubFolder = "Application Support";
        o.folderName = folder;
        settings_ = std::make_unique<juce::PropertiesFile>(o);
    }
    return *settings_;
}

// Panel sizes the user dragged (browser, detail, clip panel, Session fader section, Arrangement header width).
void AppController::saveLayout()
{
    const auto& m = ui_.theme.metrics;
    auto& p = settings();
    p.setValue("layoutBrowserWidth", (double) m.browserWidth);
    p.setValue("layoutBrowserSidebar", (double) m.browserSidebarWidth);
    p.setValue("layoutDetailHeight", (double) m.detailHeight);
    p.setValue("layoutClipPanel", (double) m.clipPanelWidth);
    p.setValue("layoutVolumeHeight", (double) m.sessionVolumeHeight);
    p.setValue("layoutMixerVisible", ui_.mixerVisible);
    p.setValue("layoutSendsVisible", ui_.sendsVisible);
    p.setValue("browserPreviewEnabled", ui_.browserPreviewEnabled);
    p.setValue("layoutArrangementHeader", (double) m.arrangementHeaderWidth);
    p.setValue("layoutVelocityLane", (double) m.velocityLaneHeight);
    p.setValue("gridArrangement", ui::encodeGrid(ui_.arrangementGrid));
    p.setValue("gridClip", ui::encodeGrid(ui_.clipGrid));
    p.setValue("toolOverdub", ui_.arrangementOverdub);
    p.setValue("toolAutomationArm", ui_.automationArm);
    p.saveIfNeeded();
}

void AppController::loadLayout()
{
    auto& m = ui_.theme.metrics;
    auto& p = settings();
    auto load = [&](const char* key, float& value, float lo, float hi) {
        if (p.containsKey(key))
            value = std::clamp((float) p.getDoubleValue(key, value), lo, hi);
    };
    load("layoutBrowserWidth", m.browserWidth, 140, 1200);
    load("layoutBrowserSidebar", m.browserSidebarWidth, 90, 600);
    load("layoutDetailHeight", m.detailHeight, 60, 1000);
    load("layoutClipPanel", m.clipPanelWidth, 150, 700);
    load("layoutVolumeHeight", m.sessionVolumeHeight, 100, 520);
    ui_.mixerVisible = p.getBoolValue("layoutMixerVisible", true);
    ui_.sendsVisible = p.getBoolValue("layoutSendsVisible", true);
    ui_.browserPreviewEnabled = p.getBoolValue("browserPreviewEnabled", true);
    ui_.browserTags.loadJson(p.getValue("browserTags").toStdString());
    ui_.browserTagsChanged = [this] {
        settings().setValue("browserTags", juce::String(ui_.browserTags.toJson()));
        settings().saveIfNeeded();
    };
    load("layoutArrangementHeader", m.arrangementHeaderWidth, 182, 701);
    load("layoutVelocityLane", m.velocityLaneHeight, 24, 400);
    if (p.containsKey("gridArrangement")) ui_.arrangementGrid = ui::decodeGrid(p.getIntValue("gridArrangement"));
    if (p.containsKey("gridClip")) ui_.clipGrid = ui::decodeGrid(p.getIntValue("gridClip"));
    if (p.containsKey("toolOverdub")) ui_.arrangementOverdub = p.getBoolValue("toolOverdub");
    if (p.containsKey("toolAutomationArm")) ui_.automationArm = p.getBoolValue("toolAutomationArm");
}

void AppController::saveSettings()
{
    if (auto state = engine_.deviceManager().createStateXml())
        settings().setValue("audioDevice", state.get());
    if (song_ && ! isUntitled() && ! collabGuest())
        settings().setValue("lastFile", juce::String::fromUTF8(song_->sourcePath.c_str()));
    settings().saveIfNeeded();
}

void AppController::rememberSet()
{
    if (! song_ || isUntitled() || installingCollabSong_ || collabGuest()) return; // a session's set lives on the host
    recentSets_.addFile(juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())));
    settings().setValue("recentSets", recentSets_.toString());
    settings().saveIfNeeded();
}

void AppController::clearRecentSets(bool unavailableOnly)
{
    if (unavailableOnly) recentSets_.removeNonExistentFiles();
    else recentSets_.clear();
    settings().setValue("recentSets", recentSets_.toString());
    settings().saveIfNeeded();
}

void AppController::timerCallback()
{
    finishRecording();
    if (ui_.recordingRequested && ! inputPermissionPending_) ensureAudioInputs();
    ui_.cpuUsage = engine_.cpuUsage();
    if (auto controls = engine_.controls()) for (size_t i = 0; i < controls->size(); ++i)
        if ((*controls)[i].midiControllerOverflow.exchange(false, std::memory_order_relaxed))
            ui_.statusText = "MIDI preview/controller playback exceeded its buffer; voices, pedals and pitch bend were reset";
}

// ------------------------------------------------------------------------- files

void AppController::openFileDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>("Open Live Set", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                   "*.als");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                                  openFile(f);
                          });
}

void AppController::openFile(const juce::File& file)
{
    confirmDiscardChanges([this, file] { openFileNow(file); });
}

void AppController::renameSelection()
{
    if (! song_ || ui_.recording || ui_.recordingRequested || exporting_ || editingDevices_ || syncingModel_) return;
    const auto location = ui_.selectedClipLocation();
    const auto* clip = ui_.selectedClip();
    const auto* track = ui_.trackAt(ui_.selectedTrack);
    if (! clip && (! track || track->kind == model::TrackKind::Main)) return;
    const int generation = documentGeneration_, index = ui_.selectedTrack, id = track ? track->id : -1;
    auto* dialog = new juce::AlertWindow("Rename", clip ? "Clip name" : "Track name", juce::MessageBoxIconType::NoIcon);
    auto editableName = clip ? clip->name : track->name;
    if (! clip && track->nameIsAutomatic && index < ui_.numTracks())
    {
        const auto displayed = ui_.trackDisplayName(index);
        editableName = "#" + displayed.substr(std::to_string(index + 1).size());
    }
    dialog->addTextEditor("name", juce::String::fromUTF8(editableName.c_str()));
    dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<bool>(alive_), dialog, generation, location, index, id](int result) {
        const auto lifetime = alive.lock();
        if (! lifetime || ! *lifetime || result != 1 || generation != documentGeneration_ || ! song_) return;
        const auto name = dialog->getTextEditorContents("name").trim().toStdString();
        if (name.empty()) return;
        if (location.uid >= 0)
        {
            if (const auto* current = model::findClip(*song_, location))
            { auto changed = *current; changed.name = name; applyEdit(model::modifyClip(*song_, location, std::move(changed), "Rename Clip")); }
        }
        else if (const auto* current = ui_.trackAt(index); current && current->id == id)
            applyEdit(model::renameTrack(*song_, index, name));
    }), true);
}

void AppController::importAudioDialog()
{
    if (! song_ || ui_.recording || ui_.recordingRequested || exporting_) return;
    const int generation = documentGeneration_;
    chooser_ = std::make_unique<juce::FileChooser>("Import Audio File", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  engine::audioFileWildcard());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, generation](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (generation != documentGeneration_ || ! file.existsAsFile() || ui_.recording || ui_.recordingRequested) return;
            const auto* selected = ui_.trackAt(ui_.selectedTrack);
            if (! selected || selected->kind != model::TrackKind::Audio)
            {
                ui_.insertTrack(false);
                selected = ui_.trackAt(ui_.selectedTrack);
            }
            if (! selected || selected->kind != model::TrackKind::Audio) return;
            const ui::BrowserDrag payload{ui::BrowserDrag::Kind::Sample, file.getFullPathName().toStdString(), file.getFileNameWithoutExtension().toStdString()};
            dropBrowserItem(payload, ui_.selectedTrack, engine_.transport().positionBeats(), ui_.mainView == ui::MainView::Session ? ui_.selectedSessionScene : -1);
        });
}

void AppController::openFileNow(const juce::File& file)
{
    if (ui_.recording)
    {
        ui_.statusText = "Stop recording before opening another set";
        return;
    }
    const auto name = file.getFileName().toStdString();
    const int generation = ++openGeneration_; // a newer open abandons this one
    const int busy = ui_.beginBusy("Opening " + name + "...");
    ui_.statusText = "Loading " + name + "...";
    if (ui_.requestRepaint)
        ui_.requestRepaint();

    std::weak_ptr<bool> alive = alive_;
    pool_.addJob([this, file, alive, generation, busy, name] {
        auto result = io::loadAls(file);
        juce::MessageManager::callAsync([this, alive, r = std::make_shared<io::AlsLoadResult>(std::move(result)), file, generation, busy, name]() mutable {
            if (alive.expired())
                return;
            if (generation != openGeneration_ || ! *r)
            {
                ui_.endBusy(busy);
                if (generation == openGeneration_)
                {
                    ui_.statusText = "Could not open " + name + ": " + r->error;
                    logf("open failed: %s", r->error.c_str());
                }
                if (ui_.requestRepaint)
                    ui_.requestRepaint();
                return;
            }
            // Plug-ins are created on the message thread and some take a second each. Starting them one per message
            // loop turn (UI-51) keeps the window painting and the busy cursor moving, so macOS never shows its wait
            // cursor; installSong() then picks the started instances up.
            auto plugins = std::make_shared<std::vector<const model::Device*>>();
            const std::function<void(const std::vector<model::Device>&)> collect = [&](const std::vector<model::Device>& devices) {
                for (const auto& d : devices)
                {
                    if (d.plugin && (d.kind == model::DeviceKind::PluginVst3 || d.kind == model::DeviceKind::PluginAu
                                     || d.kind == model::DeviceKind::PluginVst2))
                        plugins->push_back(&d);
                    for (const auto& chain : d.chains) collect(chain.devices);
                }
            };
            for (const auto& t : r->song->tracks) collect(t.devices);
            for (const auto& t : r->song->returns) collect(t.devices);
            collect(r->song->main.devices);
            preloadPluginsThenInstall(r, plugins, 0, generation, busy, name);
        });
    });
}

void AppController::preloadPluginsThenInstall(std::shared_ptr<io::AlsLoadResult> result, std::shared_ptr<std::vector<const model::Device*>> plugins,
                                              size_t index, int generation, int busy, std::string name)
{
    if (generation != openGeneration_)
    {
        ui_.endBusy(busy);
        return;
    }
    const auto count = plugins->size();
    if (index < count)
    {
        const auto* device = (*plugins)[index];
        ui_.updateBusy(busy, "Starting plug-ins " + std::to_string(index + 1) + " of " + std::to_string(count) + ": " + device->plugin->name,
                       (float) index / (float) count);
    }
    else
        ui_.updateBusy(busy, "Building " + name + "...", count > 0 ? 1.0f : -1.0f);
    if (ui_.requestRepaint)
        ui_.requestRepaint();
    // Give the window a frame to show the new status before the message thread is busy again.
    std::weak_ptr<bool> alive = alive_;
    juce::Timer::callAfterDelay(20, [this, alive, result, plugins, index, generation, busy, name] {
        if (alive.expired())
            return;
        if (generation != openGeneration_)
        {
            ui_.endBusy(busy);
            return;
        }
        if (index < plugins->size())
        {
            (void) engine_.preloadPlugin(*(*plugins)[index]);
            preloadPluginsThenInstall(result, plugins, index + 1, generation, busy, name);
            return;
        }
        installSong(std::move(result->song));
        ui_.endBusy(busy);
        if (ui_.requestRepaint)
            ui_.requestRepaint();
    });
}

void AppController::installSong(std::unique_ptr<model::Song> song)
{
    if (editingDevices_ || syncingModel_ || ui_.recording) { deferredSong_ = std::move(song); return; }
    if (collab_ && ! installingCollabSong_)
    {
        // Another document replaces the shared one: leave (or end) the session first.
        if (inCollabSession()) collab_->leave();
        endCollabDocument();
    }
    ++documentGeneration_;
    controlEndpoint_.disable();
    controlDocumentId_ = juce::Uuid().toString().toStdString();
    closePluginWindows();
    closeWavetableWindows();
    ui_.wavetableFloating.clear(); // placements are per document (not saved): a loaded set starts floating again
    ++ui_.wavetablePlacementRevision;

    // 1) Point the UI at nothing so no view touches the old graph while we swap.
    ui_.graph = nullptr;
    ui_.song = nullptr;
    ui_.controls.reset();
    if (ui_.modelChanged)
        ui_.modelChanged();

    // 2) Build + install the graph (instantiates plugins: message thread).
    peaks_.clear();
    song_ = std::move(song);
    if (! song_->midiClipTemplate)
        song_->midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    history_.clear();
    history_.markSaved();
    ui_.documentDirty = false;
    ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = ui_.selectedTakeLane = -1;
    ui_.unfoldedTakeTracks.clear();
    ui_.drumPadBanks.clear();
    ui_.wavetableFxViews.clear();
    ui_.wavetableModulationViews.clear();
    ui_.wavetableRoutingViews.clear();
    ui_.selectedRackId = -1; ui_.selectedRackChain = 0;
    ui_.recordingRequested = false;
    inputPermissionDenied_ = false;
    const auto start = juce::Time::getMillisecondCounterHiRes();
    ui_.buildLog = engine_.loadSong(*song_);
    for (const auto& w : song_->warnings)
        ui_.buildLog.push_back(w);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;

    // 3) Point the UI at the new data.
    ui_.song = song_.get();
    ui_.scaleHighlight = song_->inKey; // Scale Mode as the set was saved (UI-30b)
    ui_.graph = engine_.latestGraph();
    ui_.controls = engine_.controls();
    ui_.selectedTrack = 0;
    ui_.trackSelection.clear();
    ui_.trackSelectionAnchor = -1;
    ui_.sessionTrackSelection = false;
    ui_.scrollBeats = 0;
    ui_.scrollTracksY = 0;
    deviceOrder_ = midi::deviceOrder(*song_);
    loadMappingsForSong();
    ui_.statusText = juce::File(song_->sourcePath).getFileName().toStdString() + " loaded (" + song_->creator + ", "
                     + std::to_string((int) ms) + " ms)";
    if (ui_.modelChanged)
        ui_.modelChanged();
    rememberSet();
    saveSettings();
    updateWindowTitle();
    logf("loaded %s: %zu tracks, %zu returns, %zu load notes, %d ms", song_->sourcePath.c_str(), song_->tracks.size(),
         song_->returns.size(), ui_.buildLog.size(), (int) ms);
    for (const auto& line : ui_.buildLog)
        logf("  note: %s", line.c_str());
    ensureAudioInputs();
}

// Hooks the preferences dialog up to the services it edits. Everything it changes is persisted.
void AppController::wirePreferences()
{
    ui_.countInBars = settings().getIntValue("countInBars", 0);
    if (ui_.countInBars != 1 && ui_.countInBars != 2 && ui_.countInBars != 4) ui_.countInBars = 0;
    ui_.setCountInBars = [this](int bars) { settings().setValue("countInBars", bars); settings().saveIfNeeded(); };
    ui_.uiZoom = (float) settings().getDoubleValue("uiZoom", 1.0);
    ui_.setUiZoom = [this](float z) {
        ui_.uiZoom = std::clamp(z, 0.8f, 2.0f);
        settings().setValue("uiZoom", (double) ui_.uiZoom);
        settings().saveIfNeeded();
    };
    ui_.loadTheme = [this] { loadThemeDialog(); };
    ui_.useBuiltinTheme = [this](bool dark) {
        ui_.theme.useBuiltin(dark);
        settings().removeValue("themePath");
        settings().setValue("darkTheme", dark);
        settings().saveIfNeeded();
        if (ui_.modelChanged) ui_.modelChanged();
    };
    ui_.scanAudioUnits = [this] { scanPlugins(true); };
    ui_.userLibraryPath = settings().getValue("userLibraryPath", juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                                                 .getChildFile("Ableton/User Library").getFullPathName()).toStdString();
    ui_.packsPath = settings().getValue("packsPath", "/Users/Shared/Ableton/Factory Packs").toStdString();
    ui_.temporaryProjectPath = untitledRoot().getFullPathName().toStdString();
    ui_.settingsPath = settings().getFile().getParentDirectory().getFullPathName().toStdString();
    ui_.pluginCachePath = plugins::PluginHost::cacheFile().getFullPathName().toStdString();
    ui_.chooseLibraryFolder = [this](bool packs) {
        chooser_ = std::make_unique<juce::FileChooser>(packs ? "Packs Folder" : "User Library Folder",
            juce::File(juce::String::fromUTF8((packs ? ui_.packsPath : ui_.userLibraryPath).c_str())));
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, packs](const juce::FileChooser& fc) {
                if (! fc.getResult().isDirectory()) return;
                auto& path = packs ? ui_.packsPath : ui_.userLibraryPath;
                path = fc.getResult().getFullPathName().toStdString();
                settings().setValue(packs ? "packsPath" : "userLibraryPath", fc.getResult().getFullPathName());
                settings().saveIfNeeded();
                if (ui_.requestRepaint) ui_.requestRepaint();
            });
    };
    ui_.audioSettings = [this] {
        ui::UiContext::AudioSettings state;
        auto& manager = engine_.deviceManager();
        const auto setup = manager.getAudioDeviceSetup();
        state.driver = manager.getCurrentAudioDeviceType().toStdString();
        state.input = setup.inputDeviceName.toStdString(); state.output = setup.outputDeviceName.toStdString();
        if (auto* type = manager.getCurrentDeviceTypeObject())
            for (const auto& name : type->getDeviceNames(false)) state.outputs.push_back(name.toStdString());
        if (auto* device = manager.getCurrentAudioDevice())
        {
            state.sampleRate = device->getCurrentSampleRate(); state.bufferSize = device->getCurrentBufferSizeSamples();
            for (double rate : device->getAvailableSampleRates()) state.sampleRates.push_back(rate);
            for (int size : device->getAvailableBufferSizes()) state.bufferSizes.push_back(size);
            if (state.sampleRate > 0)
            {
                state.inputLatencyMs = device->getInputLatencyInSamples() * 1000.0 / state.sampleRate;
                state.outputLatencyMs = device->getOutputLatencyInSamples() * 1000.0 / state.sampleRate;
            }
        }
        return state;
    };
    ui_.setAudioSetting = [this](ui::UiContext::AudioSetting field, const std::string& value) {
        if (ui_.recording || ui_.recordingRequested || exporting_ || editingDevices_ || syncingModel_)
        { ui_.statusText = "Finish recording, export or device changes before changing audio settings"; return; }
        auto setup = engine_.deviceManager().getAudioDeviceSetup();
        const auto text = juce::String::fromUTF8(value.c_str());
        if (field == ui::UiContext::AudioSetting::Output) setup.outputDeviceName = text;
        else if (field == ui::UiContext::AudioSetting::SampleRate) setup.sampleRate = text.getDoubleValue();
        else setup.bufferSize = text.getIntValue();
        const auto error = engine_.deviceManager().setAudioDeviceSetup(setup, true);
        ui_.statusText = error.isEmpty() ? "Audio settings updated" : "Audio settings: " + error.toStdString();
        if (error.isEmpty()) saveSettings();
        if (ui_.requestRepaint) ui_.requestRepaint();
    };

    ui_.pluginFolders = {"/Library/Audio/Plug-Ins/VST3", "~/Library/Audio/Plug-Ins/VST3"};
    ui_.failedPlugins.clear();
    for (const auto& f : host_.failedPlugins())
        ui_.failedPlugins.push_back(f.toStdString());
    ui_.clearFailedPlugins = [this] {
        host_.clearBlacklist();
        ui_.failedPlugins.clear();
    };

    ui_.midiInputRows = [this] {
        std::vector<ui::UiContext::MidiInputRow> rows;
        for (const auto& info : midi_.availableInputs())
        {
            const auto roles = midi_.inputRoles(info.identifier);
            rows.push_back({info.identifier.toStdString(), info.name.toStdString(), roles.track, roles.remote});
        }
        return rows;
    };
    ui_.setMidiInputRoles = [this](const std::string& id, bool track, bool remote) {
        juce::String name;
        for (const auto& info : midi_.availableInputs())
            if (info.identifier == juce::String(id))
                name = info.name;
        midi_.setInputRoles(juce::String(id), name, {track, remote});
        midi_.saveSettings(midiSettingsFile());
    };

    ui_.midiOutputRows = [this] {
        std::vector<ui::UiContext::MidiOutputRow> rows;
        for (const auto& info : midiClock_.availableOutputs())
            rows.push_back({info.identifier.toStdString(), info.name.toStdString()});
        return rows;
    };
    ui_.setMidiClock = [this](const std::string& id, bool enabled) {
        midiClock_.setOutput(enabled ? juce::String(id) : juce::String());
        midiClock_.setEnabled(enabled);
        ui_.midiClockEnabled = enabled;
        ui_.midiClockOutputId = enabled ? id : std::string();
        settings().setValue("midiClockOutput", enabled ? juce::String(id) : juce::String());
    };
    const auto clockOut = settings().getValue("midiClockOutput");
    if (clockOut.isNotEmpty())
        ui_.setMidiClock(clockOut.toStdString(), true);
}

// ------------------------------------------------------------------------- editing

control::ApplyResult AppController::applyEdit(model::EditCommand cmd)
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_)
        return control::ApplyResult::failure("busy", "No editable document, or a device/export operation is active");
    if (collabGuest() && inCollabSession() && ! collab_->canEdit())
    {
        ui_.statusText = collab_->state() == collab::CollabSession::State::Active ? "You are a listener in this session: the host can let you edit"
                                                                                  : "Wait until you are connected to the session";
        return control::ApplyResult::failure("collab_read_only", ui_.statusText);
    }
    if (! cmd.apply || ! cmd.revert) return control::ApplyResult::failure("invalid_edit", "Empty edit command");
    if (! cmd.structureChanged && cmd.gainTracks.empty())
        for (int track : cmd.tracks)
            if (const auto* current = model::trackAt(*song_, track); current && current->frozen)
            { ui_.statusText = "Unfreeze this track before editing its clips or devices"; return control::ApplyResult::failure("frozen_track", ui_.statusText); }
    if (cmd.devicesChanged && ! cmd.structureChanged)
        for (int track : cmd.tracks)
            if (const auto* current = model::trackAt(*song_, track); current && current->deactivated)
            { ui_.statusText = "Activate this track before editing its devices"; return control::ApplyResult::failure("deactivated_track", ui_.statusText); }
    const auto tracks = cmd.tracks;
    const auto gainTracks = cmd.gainTracks;
    const auto label = cmd.label;
    const bool songLevel = cmd.songLevel;
    const bool devicesChanged = cmd.devicesChanged;
    const bool takesOnly = cmd.takeLanesOnly;
    if (cmd.inputChanged && ui_.recording) { ui_.statusText = "Finish recording before changing inputs"; return control::ApplyResult::failure("recording_conflict", ui_.statusText); }
    if (cmd.recordingRangeChanged && ui_.recording) { ui_.statusText = "Finish recording before changing the punch/loop region"; return control::ApplyResult::failure("recording_conflict", ui_.statusText); }
    if (cmd.inputSettingsChanged) inputPermissionDenied_ = false;
    if (ui_.recording && engine_.recorder().sessionRecording() && ! cmd.inputSettingsChanged)
        for (int track : cmd.tracks) if (engine_.recorder().capturesTrack(track))
        { ui_.statusText = "Finish Session recording before editing its target track"; return control::ApplyResult::failure("recording_conflict", ui_.statusText); }
    if (devicesChanged && ui_.recording) { ui_.statusText = "Finish recording before inserting devices"; return control::ApplyResult::failure("recording_conflict", ui_.statusText); }
    if (cmd.structureChanged && ui_.recording) { ui_.statusText = "Finish recording before grouping tracks"; return control::ApplyResult::failure("recording_conflict", ui_.statusText); }
    const bool structure = cmd.structureChanged;
    if (structure) { syncModelFromEngine(); snapshotTrackUi(); } // before the track list changes
    if (devicesChanged) beginDeviceEdit();
    const auto linked = structure ? std::map<int, std::vector<model::MidiNote>>{} : model::snapshotLinkedNotes(*song_); // TRK-5
    history_.perform(*song_, std::move(cmd));
    auto changed = tracks;
    if (! linked.empty())
        if (auto sync = model::syncLinkedClips(*song_, linked); sync.apply)
        {
            for (int t : sync.tracks) if (std::find(changed.begin(), changed.end(), t) == changed.end()) changed.push_back(t);
            history_.amendLast(*song_, std::move(sync)); // one undo step with the edit that caused it
        }
    afterEdit(changed, songLevel, devicesChanged, takesOnly, structure, gainTracks);
    if (inCollabSession())
    {
        // COL-2: the session finds the objects this edit changed and shares them.
        const auto author = collab_->isHost() ? collabAuthorForNextEdit_ : collab_->self();
        collabAuthorForNextEdit_ = collab::hostAuthor;
        collab_->localEditBy(author, label);
    }
    return control::ApplyResult::success();
}

void AppController::undo()
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_)
        return;
    if (inCollabSession()) { collab_->undo(); return; } // per person, through the session
    if (ui_.recording && engine_.recorder().sessionRecording())
        if (const auto* command = history_.nextUndo(); command && ! command->inputSettingsChanged)
            for (int track : command->tracks) if (engine_.recorder().capturesTrack(track))
            { ui_.statusText = "Finish Session recording before changing its target track"; return; }
    if (ui_.recording && history_.undoChangesInput()) { ui_.statusText = "Finish recording before changing inputs"; return; }
    if (ui_.recording && history_.undoChangesRecordingRange()) { ui_.statusText = "Finish recording before changing the punch/loop region"; return; }
    if (ui_.recording && history_.undoChangesDevices()) { ui_.statusText = "Finish recording before changing devices"; return; }
    if (history_.undoChangesDevices()) { syncModelFromEngine(); beginDeviceEdit(); }
    const auto* next = history_.nextUndo();
    if (next && next->structureChanged)
    {
        if (ui_.recording) { ui_.statusText = "Finish recording before changing the track list"; return; }
        syncModelFromEngine();
        snapshotTrackUi();
    }
    if (const auto* c = history_.undo(*song_))
    {
        ui_.statusText = "Undo " + c->label;
        afterEdit(c->tracks, c->songLevel, c->devicesChanged, c->takeLanesOnly, c->structureChanged, c->gainTracks);
    }
}

void AppController::redo()
{
    if (! song_ || editingDevices_ || syncingModel_ || exporting_)
        return;
    if (inCollabSession()) { collab_->redo(); return; }
    if (ui_.recording && engine_.recorder().sessionRecording())
        if (const auto* command = history_.nextRedo(); command && ! command->inputSettingsChanged)
            for (int track : command->tracks) if (engine_.recorder().capturesTrack(track))
            { ui_.statusText = "Finish Session recording before changing its target track"; return; }
    if (ui_.recording && history_.redoChangesInput()) { ui_.statusText = "Finish recording before changing inputs"; return; }
    if (ui_.recording && history_.redoChangesRecordingRange()) { ui_.statusText = "Finish recording before changing the punch/loop region"; return; }
    if (ui_.recording && history_.redoChangesDevices()) { ui_.statusText = "Finish recording before changing devices"; return; }
    if (history_.redoChangesDevices()) beginDeviceEdit();
    const auto* next = history_.nextRedo();
    if (next && next->structureChanged)
    {
        if (ui_.recording) { ui_.statusText = "Finish recording before changing the track list"; return; }
        syncModelFromEngine();
        snapshotTrackUi();
    }
    if (const auto* c = history_.redo(*song_))
    {
        ui_.statusText = "Redo " + c->label;
        afterEdit(c->tracks, c->songLevel, c->devicesChanged, c->takeLanesOnly, c->structureChanged, c->gainTracks);
    }
}

void AppController::beginDeviceEdit()
{
    editingDevices_ = true;
    closePluginWindows();
    deviceIdsBeforeEdit_ = song_ ? ui::deviceIdsByTrack(*song_) : ui::DeviceIdsByTrack{};
    ui_.song = nullptr;
    // Drop pointers before model vectors move or processors die. Plugin instantiation may dispatch
    // message-thread events, including repaint, while the new chain is being constructed.
    if (ui_.modelChanged) ui_.modelChanged();
}

// Tells the engine which tracks / song settings changed and refreshes the UI.
void AppController::afterEdit(const std::vector<int>& tracks, bool songLevel, bool devicesChanged, bool takesOnly, bool structureChanged, const std::vector<int>& gainTracks)
{
    if (devicesChanged) closePluginWindows();
    if (structureChanged)
    {
        remapTrackUi(idsBeforeStructureEdit_);
        rebuildEngine();
    }
    else
    for (int t : tracks)
        if (devicesChanged)
        {
            const auto log = engine_.updateTrackDevices(t, *song_);
            ui_.buildLog.insert(ui_.buildLog.end(), log.begin(), log.end());
        }
        else engine_.updateTrackClips(t, *song_, takesOnly);
    if (songLevel)
    {
        engine_.transport().setTempo(song_->tempo);
        engine_.transport().setTimeSignature(song_->timeSigNumerator, song_->timeSigDenominator);
        engine_.transport().setLoop(song_->loopOn, song_->loopStart, song_->loopLength);
    }
    if (devicesChanged)
    {
        followWavetableIds(); // before any view sees the new song
        ui_.song = song_.get();
        editingDevices_ = false;
        auto mappings = midi_.mappings();
        auto order = midi::deviceOrder(*song_);
        midi::rebindDeviceMappings(deviceOrder_, order, mappings);
        deviceOrder_ = std::move(order);
        midi_.setMappings(std::move(mappings));
    }
    if (auto controls = engine_.controls())
        for (int index : gainTracks)
            if (index >= 0 && static_cast<size_t>(index) < controls->size())
                if (const auto* track = model::trackAt(*song_, index))
                    (*controls)[static_cast<size_t>(index)].volume.store(static_cast<float>(track->mixer.volume));
    // A selected clip that no longer exists is deselected.
    if (ui_.selectedClipUid >= 0 && ! model::findClip(*song_, ui_.selectedClipLocation()))
        ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = ui_.selectedTakeLane = -1;
    if (ui_.selectedTakeLane >= 0 && ! model::findTakeLane(*song_, ui_.selectedClipTrack, ui_.selectedTakeLane))
        ui_.selectedTakeLane = -1;
    ui_.documentDirty = history_.isDirty();
    updateWindowTitle();
    if (ui_.modelChanged)
        ui_.modelChanged();
    if (ui_.requestRepaint)
        ui_.requestRepaint();
    queueDeferredSong();
    ensureAudioInputs();
}

std::vector<model::TrackId> AppController::trackIds() const
{
    std::vector<model::TrackId> ids;
    if (song_)
    {
        for (const auto& t : song_->tracks) ids.push_back(t.id);
        for (const auto& t : song_->returns) ids.push_back(t.id);
        ids.push_back(song_->main.id);
    }
    return ids;
}

// Selection, lane heights, column widths... are stored per track index; after tracks were inserted or removed they follow
// the track ids (returns and the main track just shift).
void AppController::snapshotTrackUi()
{
    idsBeforeStructureEdit_ = trackIds(); returnIdsBeforeStructureEdit_.clear();
    if (song_) for (const auto& track : song_->returns) returnIdsBeforeStructureEdit_.push_back(track.id);
}

void AppController::remapTrackUi(const std::vector<model::TrackId>& before)
{
    ui_.arrangementSelection = {};
    ui_.mapSelection.reset(); midi_.cancelLearn();
    if (! song_)
        return;
    const auto after = trackIds();
    auto mapIndex = [&](int old) -> int {
        if (old < 0) return old;
        if ((size_t) old >= before.size()) return -1;
        const auto found = std::find(after.begin(), after.end(), before[(size_t) old]);
        return found == after.end() ? -1 : (int) (found - after.begin());
    };
    std::vector<model::TrackId> afterReturns;
    for (const auto& track : song_->returns) afterReturns.push_back(track.id);
    auto mappings = midi_.mappings();
    midi::rebindTrackMappings(before, after, returnIdsBeforeStructureEdit_, afterReturns, mappings);
    midi_.setMappings(std::move(mappings));
    midi::DeviceOrder aligned(after.size());
    for (size_t i = 0; i < before.size() && i < deviceOrder_.size(); ++i)
        if (const int j = mapIndex((int) i); j >= 0) aligned[(size_t) j] = deviceOrder_[i];
    deviceOrder_ = std::move(aligned);
    const int selected = mapIndex(ui_.selectedTrack);
    ui_.selectedTrack = selected >= 0 ? selected : std::clamp(ui_.selectedTrack, 0, (int) after.size() - 1);
    ui_.selectedClipTrack = mapIndex(ui_.selectedClipTrack);
    ui_.selectedAutomationTrack = mapIndex(ui_.selectedAutomationTrack);
    ui_.trackSelectionAnchor = mapIndex(ui_.trackSelectionAnchor);
    std::set<int> selection;
    for (int i : ui_.trackSelection) if (const int j = mapIndex(i); j >= 0) selection.insert(j);
    ui_.trackSelection = std::move(selection);
    auto remapList = [&](std::vector<int>& list) {
        std::vector<int> mapped;
        for (int i : list) if (const int j = mapIndex(i); j >= 0) mapped.push_back(j);
        list = std::move(mapped);
    };
    remapList(ui_.unfoldedTakeTracks);
    remapList(ui_.collapsedTracks);
    auto remapFloats = [&](std::vector<float>& values) {
        if (values.empty()) return;
        std::vector<float> mapped((size_t) song_->tracks.size() + song_->returns.size() + 1, 0.0f);
        for (size_t i = 0; i < values.size(); ++i)
            if (const int j = mapIndex((int) i); j >= 0 && (size_t) j < mapped.size()) mapped[(size_t) j] = values[i];
        values = std::move(mapped);
    };
    remapFloats(ui_.sessionColumnWidths);
    remapFloats(ui_.laneHeights);
}

// The track list changed: build and install a fresh graph from the model (keeping the play position).
void AppController::rebuildEngine()
{
    closePluginWindows();
    const double position = engine_.transport().positionBeats();
    ui_.graph = nullptr;
    ui_.controls.reset();
    if (ui_.modelChanged)
        ui_.modelChanged();
    ui_.buildLog = engine_.loadSong(*song_);
    for (const auto& w : song_->warnings)
        ui_.buildLog.push_back(w);
    engine_.transport().setPosition(position);
    ui_.song = song_.get();
    ui_.graph = engine_.latestGraph();
    ui_.controls = engine_.controls();
    editingDevices_ = false;
    auto mappings = midi_.mappings();
    auto order = midi::deviceOrder(*song_);
    midi::rebindDeviceMappings(deviceOrder_, order, mappings);
    deviceOrder_ = std::move(order);
    midi_.setMappings(std::move(mappings));
}

// Capture MIDI: the latest passage played on the armed MIDI tracks (selected MIDI track when none is armed) becomes a clip in
// the first empty Session slot of each, or in the Arrangement at the play position when the track has no empty slot.
void AppController::startAutomationRecording()
{
    automationCapture_.begin();
    automationRecording_ = true;
    lastAutomationBeat_ = engine_.transport().positionBeats();
    automationPoller_.startTimerHz(60);
}

void AppController::beginAutomationOnlyRecording()
{
    automationOnly_ = true;
    ui_.recording = true;
    ui_.recordingIsSession = false;
    startAutomationRecording();
    if (! engine_.transport().isPlaying() && ui_.countInBars > 0) engine_.transport().playWithCountIn(ui_.countInBars);
    else engine_.transport().play();
    ui_.statusText = "Recording automation: move a control, press Record again to finish";
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::pollAutomation()
{
    if (! automationRecording_)
        return;
    auto* graph = engine_.latestGraph();
    if (! song_ || graph == nullptr || editingDevices_ || syncingModel_)
        return;
    if (! ui_.recording || ui_.recordingIsSession || ! ui_.automationArm)
    {
        finishAutomationRecording();
        return;
    }
    if (engine_.transport().isPlaying())
    {
        lastAutomationBeat_ = engine_.transport().positionBeats();
        automationCapture_.poll(*song_, *graph, lastAutomationBeat_);
    }
}

// The touched controls get their lanes (existing points inside the take replaced) as one undo step; the new lanes take the
// controls back from their "hand override".
void AppController::finishAutomationRecording()
{
    automationPoller_.stopTimer();
    automationRecording_ = false;
    auto* graph = engine_.latestGraph();
    if (! song_ || graph == nullptr)
        return;
    auto touched = automationCapture_.finish(*song_, *graph, lastAutomationBeat_);
    if (touched.empty())
        return;
    std::vector<model::EditCommand> commands;
    for (const auto& t : touched)
        commands.push_back(model::setAutomation(*song_, t.track, t.envelope));
    applyEdit(model::compose("Record Automation", std::move(commands)));
    if (auto* current = engine_.latestGraph())
        for (const auto& t : touched)
            engine::reEnableAutomation(*current, t.track, engine::automationOverrideKey(t.kind, t.index, t.deviceSlot));
    ui_.statusText = "Recorded automation for " + std::to_string(touched.size()) + (touched.size() == 1 ? " control" : " controls") + "; Cmd+Z undoes it";
}

void AppController::captureMidi()
{
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording)
        return;
    std::vector<int> targets;
    if (auto controls = engine_.controls())
        for (int i = 0; i < (int) song_->tracks.size(); ++i)
            if (song_->tracks[(size_t) i].kind == model::TrackKind::Midi && (*controls)[(size_t) i].armed.load())
                targets.push_back(i);
    if (targets.empty() && ui_.selectedTrack >= 0 && ui_.selectedTrack < (int) song_->tracks.size()
        && song_->tracks[(size_t) ui_.selectedTrack].kind == model::TrackKind::Midi)
        targets.push_back(ui_.selectedTrack);
    if (targets.empty())
    {
        ui_.statusText = "Arm a MIDI track and play something, then press Capture MIDI";
        return;
    }
    const double beatsPerBar = song_->timeSigNumerator * 4.0 / std::max(1, song_->timeSigDenominator);
    const auto result = capture_.take(juce::Time::getMillisecondCounterHiRes() * 0.001, engine_.transport().tempo(), beatsPerBar);
    if (! result)
    {
        ui_.statusText = "Nothing to capture: play some notes on an armed MIDI track first";
        return;
    }
    int created = 0;
    std::string where;
    for (int track : targets)
    {
        auto& slots = song_->tracks[(size_t) track].slots;
        int scene = 0;
        while ((size_t) scene < slots.size() && slots[(size_t) scene].clip) ++scene;
        const bool inSession = (size_t) scene < slots.size();
        const double at = inSession ? 0.0 : (result->wasPlaying ? result->startBeats
                                                             : std::floor(engine_.transport().positionBeats() / beatsPerBar) * beatsPerBar);
        model::Clip clip;
        if (! model::makeEmptyMidiClip(*song_, at, result->length, clip))
        {
            ui_.statusText = "Cannot create a MIDI clip: this set has no MIDI clip to copy the format from";
            return;
        }
        clip.colorIndex = song_->tracks[(size_t) track].colorIndex;
        clip.notes = result->notes;
        if (inSession)
            applyEdit(model::setSessionClip(*song_, track, scene, std::move(clip)));
        else
            applyEdit(model::insertClip(track, std::move(clip), "Capture MIDI"));
        ++created;
        where = song_->tracks[(size_t) track].name + (inSession ? " (scene " + std::to_string(scene + 1) + ")" : " (Arrangement)");
        if (inSession && slots[(size_t) scene].clip)
        {
            ui_.selectedClipTrack = track;
            ui_.selectedClipScene = scene;
            ui_.selectedClipUid = slots[(size_t) scene].clip->uid;
        }
    }
    ui_.statusText = "Captured " + std::to_string(result->notes.size()) + " notes" + (created == 1 ? " on " + where : " on " + std::to_string(created) + " tracks");
    if (ui_.modelChanged) ui_.modelChanged();
}

// The circle button: record into Session slots whichever view is showing; pressed again it ends the take.
void AppController::toggleSessionRecording()
{
    if (! ui_.recording && ! ui_.recordingRequested)
        forceSessionRecording_ = true;
    toggleRecording();
    if (! ui_.recording && ! ui_.recordingRequested)
        forceSessionRecording_ = false; // the request did not start (nothing armed, permission pending is still "requested")
}

void AppController::groupSelectedTracks()
{
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested)
        return;
    std::vector<int> members;
    if (! model::groupableSelection(*song_, ui_.selectedTrackIndices(), members))
    {
        ui_.statusText = "Select tracks (Shift+click for a range, Cmd+click to add) that belong to the same group, then Cmd+G";
        return;
    }
    int pointeeEnd = 0;
    auto group = io::createGroupTrack(*song_, "Group", song_->tracks[(size_t) members.front()].colorIndex, pointeeEnd);
    const int groupId = group.id;
    applyEdit(model::groupTracks(*song_, members, std::move(group), pointeeEnd));
    ui_.trackSelection.clear();
    for (size_t i = 0; i < song_->tracks.size(); ++i)
        if (song_->tracks[i].id == groupId)
            ui_.selectedTrack = ui_.trackSelectionAnchor = (int) i; // the new group
    ui_.statusText = "Grouped " + std::to_string(members.size()) + (members.size() == 1 ? " track" : " tracks");
    if (ui_.modelChanged) ui_.modelChanged();
}

void AppController::ungroupSelectedTracks()
{
    if (! song_ || editingDevices_ || syncingModel_)
        return;
    const auto* group = ui_.trackAt(ui_.selectedTrack);
    if (group == nullptr || group->kind != model::TrackKind::Group || ui_.selectedTrack >= ui_.numTracks())
    {
        ui_.statusText = "Select a group track to ungroup it";
        return;
    }
    const int index = ui_.selectedTrack;
    applyEdit(model::ungroupTrack(*song_, index));
    ui_.trackSelection.clear();
    ui_.selectedTrack = ui_.trackSelectionAnchor = std::min(index, std::max(0, ui_.numTracks() - 1));
    ui_.statusText = "Ungrouped";
    if (ui_.modelChanged) ui_.modelChanged();
}

void AppController::queueDeferredSong()
{
    if (deferredSong_)
    {
        const auto pending = std::make_shared<std::unique_ptr<model::Song>>(std::move(deferredSong_));
        const auto alive = alive_;
        juce::MessageManager::callAsync([this, alive, pending] {
            if (*alive) installSong(std::move(*pending));
        });
    }
}

// The mixer and the devices are edited live (atomics in the engine); copy their current values into the model
// so that saving writes what the user hears.
void AppController::syncModelFromEngine()
{
    // JUCE serialises its device callbacks with this lock. Plugin getState may access processor
    // internals; keep it on the message thread while the callback is excluded.
    const juce::ScopedLock callbackLock(engine_.deviceManager().getAudioCallbackLock());
    auto* graph = engine_.latestGraph();
    if (! song_ || graph == nullptr)
        return;
    const juce::ScopedValueSetter<bool> syncing(syncingModel_, true);
    int index = 0;
    auto syncTrack = [&](model::Track& t) {
        if ((size_t) index >= graph->nodes.size())
            return;
        const auto& node = graph->nodes[(size_t) index];
        const auto& c = (*graph->controls)[(size_t) node.controlIndex];
        t.mixer.volume = c.volume.load();
        t.mixer.pan = c.pan.load();
        t.mixer.speakerOn = c.active.load();
        t.mixer.solo = c.solo.load();
        for (size_t s = 0; s < t.mixer.sends.size() && s < c.sends.size(); ++s)
            t.mixer.sends[s] = c.sends[s].load();
        for (size_t di = 0; di < t.devices.size() && di < node.processorForModelDevice.size(); ++di)
            devices::captureDeviceState(t.devices[di], node.processorForModelDevice[di]);
        ++index;
    };
    for (auto& t : song_->tracks)
        syncTrack(t);
    for (auto& t : song_->returns)
        syncTrack(t);
    syncTrack(song_->main);
    queueDeferredSong();
}

void AppController::openBrowserFolder(const std::string& path)
{
    ++browserGeneration_;
    ui_.browserFolder = path;
    ui_.browserFiles.clear();
    ui_.browserFolderChildren.clear();
    ui_.browserLoading = true;
    readBrowserFolder(path, true);
}

void AppController::readBrowserFolder(const std::string& path, bool root)
{
    const juce::File folder(juce::String::fromUTF8(path.c_str()));
    const int generation = browserGeneration_;
    pool_.addJob([this, folder, path, root, generation, alive = std::weak_ptr<bool>(alive_)] {
        std::vector<ui::UiContext::BrowserFile> files;
        for (const auto& file : folder.findChildFiles(juce::File::findFilesAndDirectories, false))
        {
            if (file.isHidden()) continue;
            const bool directory = file.isDirectory();
            if (directory || file.hasFileExtension("als") || engine::isAudioFile(file))
                files.push_back({file.getFullPathName().toStdString(), directory});
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            if (a.directory != b.directory) return a.directory;
            return juce::File(a.path).getFileName().compareNatural(juce::File(b.path).getFileName()) < 0;
        });
        juce::MessageManager::callAsync([this, alive, generation, path, root, entries = std::move(files)]() mutable {
            if (alive.expired() || generation != browserGeneration_) return;
            if (root) { ui_.browserFiles = std::move(entries); ui_.browserLoading = false; }
            else ui_.browserFolderChildren[path] = std::move(entries);
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::addBrowserFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>("Add a folder to Places", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& chooser) {
            const auto folder = chooser.getResult();
            if (! folder.isDirectory()) return;
            const auto path = folder.getFullPathName().toStdString();
            if (std::find(ui_.browserPlaces.begin(), ui_.browserPlaces.end(), path) == ui_.browserPlaces.end())
                ui_.browserPlaces.push_back(path);
            juce::StringArray places;
            for (const auto& place : ui_.browserPlaces) places.add(juce::String::fromUTF8(place.c_str()));
            settings().setValue("browserPlaces", places.joinIntoString("\n"));
            openBrowserFolder(path);
        });
}

int AppController::requiredInputChannels(bool record) const
{
    int required = 0;
    if (! song_ || ! engine_.controls()) return required;
    for (size_t i = 0; i < song_->tracks.size(); ++i)
    {
        const auto& track = song_->tracks[i];
        if (track.kind != model::TrackKind::Audio) continue;
        const auto& control = (*engine_.controls())[i];
        const auto route = control.inputRoute.load();
        const auto kind = model::inputKind(route);
        if (kind != model::InputKind::AudioMono && kind != model::InputKind::AudioStereo) continue;
        const bool armed = control.armed.load();
        const int monitor = control.monitoring.load();
        if ((record && armed) || monitor == 0 || (monitor == 1 && armed))
            required = std::max(required, model::inputChannel(route) + (kind == model::InputKind::AudioMono ? 1 : 2));
    }
    return required;
}

void AppController::ensureAudioInputs(bool record)
{
    if (record) ui_.recordingRequested = true;
    if (inputPermissionPending_ || editingDevices_ || syncingModel_ || ! song_) return;
    const int required = requiredInputChannels(ui_.recordingRequested);
    bool configured = required == 0;
    if (auto* device = engine_.deviceManager().getCurrentAudioDevice())
    {
        const auto active = device->getActiveInputChannels();
        configured |= active.countNumberOfSetBits() >= required && active.getHighestBit() + 1 == active.countNumberOfSetBits();
    }
    if (ui_.recording && (! configured || ! engine_.graphPreparedForDevice()))
    {
        ui_.recordingRequested = false;
        ui_.statusText = "Finish recording before enabling additional hardware inputs or changing the audio device";
        return;
    }
    if (configured)
    {
        if (ui_.recordingRequested)
        {
            if (! engine_.graphPreparedForDevice()) engine_.reprepareGraph();
            beginRecording();
        }
        return;
    }
    if (inputPermissionDenied_ && ! record) return;
    inputPermissionPending_ = true;
    const int generation = documentGeneration_;
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
        [this, generation, alive = std::weak_ptr<bool>(alive_)](bool granted) {
            const auto lifetime = alive.lock();
            if (! lifetime || ! *lifetime) return;
            inputPermissionPending_ = false;
            if (generation != documentGeneration_) { ensureAudioInputs(); return; }
            inputPermissionDenied_ = ! granted;
            if (! granted)
            {
                ui_.recordingRequested = false;
                ui_.statusText = "Microphone permission was denied";
                return;
            }
            const int channels = requiredInputChannels(ui_.recordingRequested);
            if (channels > 0)
            {
                const auto error = engine_.enableRecordingInputs(channels);
                if (error.isNotEmpty())
                {
                    ui_.recordingRequested = false;
                    ui_.statusText = "Audio input: " + error.toStdString();
                    return;
                }
            }
            // Reprepare now so a queued device-size change cannot abort a just-created take.
            if (! engine_.graphPreparedForDevice()) engine_.reprepareGraph();
            if (ui_.recordingRequested) beginRecording();
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
}

void AppController::toggleRecording()
{
    if (ui_.recordingRequested)
    {
        ui_.recordingRequested = false;
        ui_.statusText = "Recording request cancelled";
        return;
    }
    if (ui_.recording)
    {
        if (automationOnly_)
        {
            // recording that only captures automation ends right away; playback continues
            automationOnly_ = false;
            ui_.recording = false;
            engine_.transport().cancelCountIn();
            ui_.statusText = "Automation recording finished; Cmd+Z undoes it";
            return;
        }
        if (engine_.recorder().sessionRecording() && engine_.transport().isPlaying()) engine_.recorder().requestSessionStop();
        else engine_.recorder().requestStop();
        engine_.transport().cancelCountIn();
        return;
    }
    if (! song_ || ! engine_.controls() || editingDevices_ || syncingModel_) return;
    requestedRecordingTrack_ = requestedRecordingScene_ = -1;
    ui_.recordingIsSession = forceSessionRecording_; // the Session circle requests slots; the main record button always records Arrangement
    ensureAudioInputs(true);
}

void AppController::beginRecording()
{
    if (! song_ || (ui_.recording && (! engine_.recorder().sessionRecording() || requestedRecordingTrack_ < 0)) || editingDevices_ || syncingModel_) return;
    ui_.recordingRequested = false;
    const juce::File source(juce::String::fromUTF8(song_->sourcePath.c_str()));
    const auto folder = source.getParentDirectory().getChildFile("Samples/Recorded");
    const bool arrangement = ! ui_.recordingIsSession && requestedRecordingTrack_ < 0 && ! forceSessionRecording_;
    ui_.recordingIsSession = ! arrangement;
    forceSessionRecording_ = false;
    engine::Recorder::SessionPlan plan;
    if (! arrangement)
    {
        plan.quant = engine::quantisationBeats(song_->globalQuantisation, song_->timeSigNumerator, song_->timeSigDenominator);
        plan.immediateStart = ! engine_.transport().isPlaying();
        plan.looping = engine_.transport().loopEnabled(); plan.loopStart = engine_.transport().loopStart();
        plan.loopEnd = plan.loopStart + engine_.transport().loopLength();
        for (int track = 0; track < (int) song_->tracks.size(); ++track)
        {
            auto& controls = (*engine_.controls())[(size_t) track];
            if (! controls.armed.load() || (requestedRecordingTrack_ >= 0 && requestedRecordingTrack_ != track)) continue;
            int scene = requestedRecordingScene_ >= 0 ? requestedRecordingScene_ : ui_.selectedSessionScene;
            const auto& sourceTrack = song_->tracks[(size_t) track];
            const int playing = controls.playingSlot.load();
            if (requestedRecordingScene_ < 0 && sourceTrack.kind == model::TrackKind::Midi && playing >= 0) scene = playing;
            if (scene < 0) scene = 0;
            const model::Clip* existing = (size_t) scene < sourceTrack.slots.size() && sourceTrack.slots[(size_t) scene].clip ? &*sourceTrack.slots[(size_t) scene].clip : nullptr;
            if (existing && existing->kind == model::Clip::Kind::Audio)
            {
                if (requestedRecordingScene_ >= 0) { ui_.statusText = "Record audio into an empty Session slot"; return; }
                scene = 0;
                while ((size_t) scene < sourceTrack.slots.size() && sourceTrack.slots[(size_t) scene].clip) ++scene;
                existing = nullptr;
            }
            plan.targets.push_back({track, scene, existing ? existing->uid : -1});
        }
    }
    std::string error;
    if (! engine_.recorder().start(*song_, *engine_.controls(), folder, engine_.sampleRate(), error, arrangement, arrangement ? nullptr : &plan))
    {
        if (arrangement && ui_.automationArm && error.rfind("Arm an audio or MIDI track", 0) == 0)
        {
            beginAutomationOnlyRecording(); // Live records automation even when no track is armed
            return;
        }
        ui_.statusText = error;
        return;
    }
    if (arrangement && ui_.automationArm)
        startAutomationRecording();
    for (const auto& target : plan.targets)
        if (target.uid >= 0 && (*engine_.controls())[(size_t) target.track].playingSlot.load() != target.scene)
        {
            auto& controls = (*engine_.controls())[(size_t) target.track];
            controls.queuedQuant.store(plan.immediateStart ? 0 : plan.quant, std::memory_order_relaxed);
            controls.queuedSlot.store(target.scene, std::memory_order_release);
        }
    recordingScene_ = arrangement ? -1 : (plan.targets.empty() ? 0 : plan.targets[0].scene);
    ui_.recording = true;
    if (! engine_.transport().isPlaying() && ui_.countInBars > 0) engine_.transport().playWithCountIn(ui_.countInBars);
    else engine_.transport().play();
    ui_.statusText = arrangement ? "Recording armed tracks to Arrangement" : "Session recording queued; Record stops at the next quantisation boundary";
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::finishRecording()
{
    if (editingDevices_ || syncingModel_) return;
    if (! engine_.recorder().finished())
    {
        const int phase = engine_.recorder().sessionPhase();
        if (ui_.recording && engine_.recorder().sessionRecording() && phase != lastRecordingPhase_)
        {
            lastRecordingPhase_ = phase;
            ui_.statusText = phase == 1 ? "Session recording queued" : phase == 2 ? "Recording Session clips" :
                             phase == 3 ? "Session recording stop queued" : "Saving Session recording; other tracks continue";
        }
        return;
    }
    auto result = engine_.recorder().takeResult();
    ui_.recording = engine_.recorder().busy();
    lastRecordingPhase_ = -1;
    if (song_ && (! result.clips.empty() || ! result.session.empty()))
    {
        std::vector<model::EditCommand> commands;
        std::vector<std::pair<int, int>> launches;
        for (size_t track = 0; track < song_->tracks.size(); ++track)
        {
            std::vector<model::Clip*> clips;
            for (auto& take : result.session) if (take.track == (int) track) clips.push_back(&take.clip);
            for (auto& [index, passes] : result.passes) if (index == (int) track)
                for (auto& clip : passes) clips.push_back(&clip);
            for (auto& [index, clip] : result.clips) if (index == (int) track) clips.push_back(&clip);
            auto targets = model::assignRecordedMidiTargets(*song_, (int) track, clips);
            if (targets.apply) commands.push_back(std::move(targets));
        }
        for (auto& take : result.session)
        {
            if (take.uid >= 0)
            {
                const auto location = model::ClipLocation::session(take.track, take.uid, take.scene);
                if (! model::findClip(*song_, location)) { result.warning = "Recorded overdub target changed; original clip was left intact"; continue; }
                commands.push_back(model::modifyClip(*song_, location, std::move(take.clip), "Overdub MIDI Clip"));
            }
            else
            {
                const auto& slots = song_->tracks[(size_t) take.track].slots;
                if ((size_t) take.scene < slots.size() && slots[(size_t) take.scene].clip)
                { result.warning = "Recorded Session slot changed; existing clip was left intact"; continue; }
                commands.push_back(model::setSessionClip(*song_, take.track, take.scene, std::move(take.clip)));
                if (take.launchAfterRecording) launches.emplace_back(take.track, take.scene);
            }
        }
        for (auto& [track, passes] : result.passes)
            commands.push_back(model::recordArrangementPasses(*song_, track, std::move(passes)));
        for (auto& [track, clip] : result.clips)
        {
            if (std::any_of(result.passes.begin(), result.passes.end(), [track](const auto& p) { return p.first == track; })) continue;
            if (recordingScene_ >= 0)
                commands.push_back(model::setSessionClip(*song_, track, recordingScene_, std::move(clip)));
            else if (ui_.arrangementOverdub && clip.kind == model::Clip::Kind::Midi)
                commands.push_back(model::recordArrangementOverdub(*song_, track, std::move(clip)));
            else
                commands.push_back(model::recordArrangementTake(*song_, track, std::move(clip)));
        }
        if (! commands.empty()) applyEdit(model::compose("Record Take", std::move(commands)));
        if (engine_.transport().isPlaying()) for (const auto& [track, scene] : launches)
            engine::launchClip(*engine_.latestGraph(), engine_.transport(), track, scene, 0);
    }
    ui_.statusText = result.warning.empty() ? (result.clips.empty() && result.session.empty() ? "Recording cancelled; no material captured" : "Recording finished; Cmd+Z undoes the take") : result.warning;
    queueDeferredSong();
    if (ui_.requestRepaint) ui_.requestRepaint();
}

namespace
{
// Calls fn for every sample reference the model holds (clips of every container, device sample zones).
void forEachSampleRef(model::Song& song, const std::function<void(model::FileRef&)>& fn)
{
    std::function<void(std::vector<model::Device>&)> devices = [&](std::vector<model::Device>& list) {
        for (auto& device : list)
        {
            for (auto& part : device.sampleParts) fn(part.sample);
            for (auto& chain : device.chains) devices(chain.devices);
        }
    };
    auto track = [&](model::Track& t) {
        for (auto& clip : t.arrangementClips) fn(clip.sample);
        for (auto& clip : t.frozenArrangement) fn(clip.sample);
        for (auto& slot : t.frozenSlots) if (slot.clip) fn(slot.clip->sample);
        for (auto& slot : t.slots) if (slot.clip) fn(slot.clip->sample);
        for (auto& lane : t.takeLanes) for (auto& clip : lane.clips) fn(clip.sample);
        devices(t.devices);
    };
    for (auto& t : song.tracks) track(t);
    for (auto& t : song.returns) track(t);
    track(song.main);
}

// After a save into `project`: refs whose files moved there point at the copies, and project-relative
// refs resolve against the new project folder. Must run before song.sourcePath changes.
void rebaseSampleRefs(model::Song& song, const juce::File& project, const io::AlsSaveResult& result)
{
    forEachSampleRef(song, [&](model::FileRef& ref) {
        const auto resolved = io::resolveSamplePath(song, ref);
        if (const auto moved = result.movedSamples.find(resolved); moved != result.movedSamples.end())
        {
            const juce::File target(juce::String::fromUTF8(moved->second.c_str()));
            ref.absolutePath = moved->second;
            ref.relativePath = target.getRelativePathFrom(project).replaceCharacter('\\', '/').toStdString();
            ref.relativePathType = 3;
            ref.sourceDirectory = project.getFullPathName().toStdString();
        }
        else if (ref.relativePathType == 3 && ! ref.relativePath.empty()
                 && project.getChildFile(juce::String::fromUTF8(ref.relativePath.c_str())).existsAsFile())
            ref.sourceDirectory = project.getFullPathName().toStdString();
    });
}
} // namespace

juce::File AppController::untitledRoot()
{
    return core::appDataDirectory().getChildFile("Untitled Projects");
}

juce::File AppController::projectsRoot()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora Projects");
}

bool AppController::isUntitled() const
{
    return song_ && juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).isAChildOf(untitledRoot());
}

void AppController::updateWindowTitle()
{
    if (! ui_.setWindowTitle)
        return;
    if (! song_)
    {
        ui_.setWindowTitle("Sonora");
        return;
    }
    const auto name = isUntitled() ? juce::String("Untitled") : juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getFileNameWithoutExtension();
    if (collabGuest())
    {
        ui_.setWindowTitle((name + (inCollabSession() ? " (shared session)" : " (session copy)")).toStdString());
        return;
    }
    ui_.setWindowTitle((name + (ui_.documentDirty ? " *" : "")).toStdString());
}

bool AppController::writeSet(const juce::File& destination, const io::AlsSaveOptions& options)
{
    if (editingDevices_ || syncingModel_) { ui_.statusText = "Wait until the device operation has finished"; return false; }
    syncModelFromEngine();
    song_->inKey = ui_.scaleHighlight; // Scale Mode is a view toggle; the set stores it as <InKey> (UI-30b)
    const auto result = io::saveAls(*song_, destination, options);
    bool ok = result.ok;
    if (! result.ok)
    {
        ui_.statusText = "Could not save: " + result.error;
        logf("save failed: %s", result.error.c_str());
    }
    else
    {
        const auto project = destination.getParentDirectory();
        rebaseSampleRefs(*song_, project, result);
        song_->sourcePath = destination.getFullPathName().toStdString();
        io::refreshClipIdentities(*song_);
        history_.markSaved();
        ui_.documentDirty = false;
        std::string text = "Saved " + destination.getFileName().toStdString() + " ("
                           + std::to_string(result.clipsAdded) + " clips added, " + std::to_string(result.clipsRemoved) + " removed)";
        if (result.samplesCopied > 0)
            text += "; " + std::to_string(result.samplesCopied) + " samples copied ("
                    + juce::File::descriptionOfSizeInBytes((juce::int64) result.bytesCopied).toStdString() + ")";
        if (options.collectAll && result.samplesMissing > 0)
            text += "; " + std::to_string(result.samplesMissing) + " samples could not be found";
        ui_.statusText = text;
        saveMappings();
        rememberSet();
        saveSettings();
        updateWindowTitle();
    }
    if (ui_.requestRepaint)
        ui_.requestRepaint();
    return ok;
}

namespace
{
// UI-44: Save (Return), Don't Save (Cmd+D), Cancel (Escape) in Sonora's prompt; indices as finishSaveChanges expects.
void askToSave(std::string title, std::string message, std::function<void(int)> done)
{
    PromptOptions options;
    options.state.title = std::move(title);
    options.state.message = std::move(message);
    options.state.buttons = {{"Save", true, false}, {"Don\u2019t Save", false, true}, {"Cancel", false, false}};
    options.defaultButton = 0;
    options.cancelButton = 2;
    options.shortcuts = {0, 'd', 0};
    PromptWindow::show(std::move(options), std::move(done));
}
} // namespace

void AppController::confirmDiscardChanges(std::function<void()> proceed)
{
    if (exporting_)
    { ui_.statusText = "Finish or cancel audio processing before changing projects"; return; }
    if (! song_ || ! ui_.documentDirty || ui_.recording)
    {
        proceed();
        return;
    }
    const auto name = isUntitled() ? juce::String("Untitled") : juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getFileNameWithoutExtension();
    std::weak_ptr<bool> alive = alive_;
    askToSave("Save changes to \u201C" + name.toStdString() + "\u201D?", "Your changes will be lost if you don\u2019t save them.",
              [this, alive, next = std::move(proceed)](int choice) {
        if (alive.expired())
            return;
        finishSaveChanges(choice, [this](std::function<void(bool)> done) { saveThen(std::move(done)); }, next);
    });
}

void AppController::requestQuit(std::function<void()> quit)
{
    if (exporting_)
    {
        cancelExport(); // quit again once the render has stopped
        ui_.statusText = "Stopping the export before quitting...";
        return;
    }
    if (restartApprovedAt_ != 0 && juce::Time::getMillisecondCounter() - restartApprovedAt_ < 60000)
    {
        quit();
        return;
    }
    confirmDiscardChanges(std::move(quit));
}

void AppController::prepareForRestart(std::function<void(bool)> done)
{
    if (exporting_ || ui_.recording || ui_.recordingRequested)
    {
        done(false);
        return;
    }
    auto approve = [this, done] { restartApprovedAt_ = std::max<juce::uint32>(1, juce::Time::getMillisecondCounter()); done(true); };
    if (! song_ || ! ui_.documentDirty)
    {
        approve();
        return;
    }
    const auto name = isUntitled() ? juce::String("Untitled") : juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getFileNameWithoutExtension();
    std::weak_ptr<bool> alive = alive_;
    askToSave("Save changes before updating?",
              "Sonora restarts to install the update. Your changes to \u201C" + name.toStdString() + "\u201D will be lost if you don\u2019t save them.",
              [this, alive, approve, done](int choice) {
        if (alive.expired())
            return;
        if (choice == 0)
            saveThen([approve, done](bool saved) { if (saved) approve(); else done(false); });
        else if (choice == 1)
            approve();
        else
            done(false);
    });
}

void AppController::newProject()
{
    confirmDiscardChanges([this] { createNewProject(); });
}

// Builds an empty Live Set, writes it into a scratch project folder (the writer patches an existing
// file) and opens it. The first Save asks where the project should live.
void AppController::createNewProject()
{
    if (ui_.recording)
    {
        ui_.statusText = "Stop recording before creating a new project";
        return;
    }
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    const auto folder = untitledRoot().getNonexistentChildFile("Untitled " + stamp + " Project", "", false);
    const auto file = folder.getChildFile("Untitled.als");
    std::string error;
    if (! folder.createDirectory() || ! io::writeAlsDom(*io::createNewSetDom(), file, error))
    {
        ui_.statusText = "Could not create a new project: " + (error.empty() ? std::string("cannot create folder") : error);
        return;
    }
    auto result = io::loadAls(file);
    if (! result)
    {
        ui_.statusText = "Could not create a new project: " + result.error;
        return;
    }
    installSong(std::move(result.song));
    ui_.mainView = ui::MainView::Session;
    ui_.statusText = "New project. Save (Cmd+S) to choose where it lives";
    if (ui_.modelChanged)
        ui_.modelChanged();
    updateWindowTitle();
}

// Clip > Reverse: writes a reversed copy of the clip's sample into <project>/Samples/Processed/Reverse and points the
// clip at it (one undo step restores the original file). Markers and loop are mirrored so the same region plays.
void AppController::reverseClipSample(const model::ClipLocation& location)
{
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording)
        return;
    const auto* clip = model::findClip(*song_, location);
    if (clip == nullptr || clip->kind != model::Clip::Kind::Audio)
        return;
    const auto path = io::resolveSamplePath(*song_, clip->sample);
    if (path.empty())
    {
        ui_.statusText = "Cannot reverse: the sample file was not found";
        return;
    }
    const juce::File projectDirectory = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory();
    const auto folder = projectDirectory.getChildFile("Samples/Processed/Reverse");
    const auto stem = juce::File(juce::String::fromUTF8(path.c_str())).getFileNameWithoutExtension();
    const int generation = documentGeneration_, uid = clip->uid;
    const std::weak_ptr<bool> alive = alive_;
    ui_.statusText = "Reversing " + stem.toStdString() + "...";
    pool_.addJob([this, alive, generation, uid, location, path, folder, stem, projectDirectory] {
        juce::File target;
        double sampleRate = 0.0;
        int frames = 0;
        std::string error;
        if (const auto data = engine_.sampleCache().load(path))
        {
            sampleRate = data->sampleRate;
            frames = data->audio.getNumSamples();
            target = folder.getNonexistentChildFile(stem + " [reversed]", ".wav", false);
            error = engine::writeReversedWav(data->audio, sampleRate, target);
        }
        else
            error = "cannot read the sample";
        juce::MessageManager::callAsync([this, alive, generation, uid, location, target, sampleRate, frames, error, projectDirectory] {
            if (alive.expired() || generation != documentGeneration_ || ! song_)
                return;
            const auto* current = model::findClip(*song_, location);
            if (! error.empty())
            {
                ui_.statusText = "Could not reverse the sample: " + error;
                return;
            }
            if (current == nullptr || current->uid != uid)
            {
                ui_.statusText = "The clip changed while reversing; nothing was replaced";
                return;
            }
            const double seconds = sampleRate > 0 ? (double) frames / sampleRate : 0.0;
            auto changed = ui::audioedit::mirrorForReverse(*current, seconds);
            changed.sample.absolutePath = target.getFullPathName().toStdString();
            changed.sample.relativePath = target.getRelativePathFrom(projectDirectory).replaceCharacter('\\', '/').toStdString();
            changed.sample.relativePathType = 3;
            changed.sample.sourceDirectory = projectDirectory.getFullPathName().toStdString();
            changed.sampleRate = sampleRate;
            changed.lengthSamples = frames;
            applyEdit(model::modifyClip(*song_, location, std::move(changed), "Reverse Clip"));
            ui_.statusText = "Reversed " + target.getFileName().toStdString();
        });
    });
}

// Clip > Crop Sample: writes the part of the sample the clip uses (its loop region) into <project>/Samples/Processed/Crop
// and points the clip at it, so unused audio is dropped; one undo step restores the original file.
void AppController::cropClipSample(const model::ClipLocation& location)
{
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording)
        return;
    const auto* clip = model::findClip(*song_, location);
    if (clip == nullptr || clip->kind != model::Clip::Kind::Audio)
        return;
    const auto path = io::resolveSamplePath(*song_, clip->sample);
    if (path.empty())
    {
        ui_.statusText = "Cannot crop: the sample file was not found";
        return;
    }
    const auto range = ui::audioedit::cropRange(*clip, song_->tempo);
    const juce::File projectDirectory = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory();
    const auto folder = projectDirectory.getChildFile("Samples/Processed/Crop");
    const auto stem = juce::File(juce::String::fromUTF8(path.c_str())).getFileNameWithoutExtension();
    const int generation = documentGeneration_, uid = clip->uid;
    const std::weak_ptr<bool> alive = alive_;
    ui_.statusText = "Cropping " + stem.toStdString() + "...";
    pool_.addJob([this, alive, generation, uid, location, path, folder, stem, projectDirectory, range] {
        juce::File target;
        double sampleRate = 0.0, startSeconds = 0.0;
        int frames = 0;
        std::string error;
        if (const auto data = engine_.sampleCache().load(path))
        {
            sampleRate = data->sampleRate;
            const int total = data->audio.getNumSamples();
            const int first = std::clamp((int) std::floor(range.start * sampleRate), 0, std::max(0, total - 1));
            const int last = std::clamp((int) std::ceil(range.end * sampleRate), first + 1, total);
            frames = last - first;
            startSeconds = (double) first / sampleRate;
            target = folder.getNonexistentChildFile(stem + " [cropped]", ".wav", false);
            error = engine::writeCroppedWav(data->audio, sampleRate, first, frames, target);
        }
        else
            error = "cannot read the sample";
        juce::MessageManager::callAsync([this, alive, generation, uid, location, target, sampleRate, frames, startSeconds, error, projectDirectory] {
            if (alive.expired() || generation != documentGeneration_ || ! song_)
                return;
            const auto* current = model::findClip(*song_, location);
            if (! error.empty())
            {
                ui_.statusText = "Could not crop the sample: " + error;
                return;
            }
            if (current == nullptr || current->uid != uid)
            {
                ui_.statusText = "The clip changed while cropping; nothing was replaced";
                return;
            }
            auto changed = ui::audioedit::cropped(*current, startSeconds);
            changed.sample.absolutePath = target.getFullPathName().toStdString();
            changed.sample.relativePath = target.getRelativePathFrom(projectDirectory).replaceCharacter('\\', '/').toStdString();
            changed.sample.relativePathType = 3;
            changed.sample.sourceDirectory = projectDirectory.getFullPathName().toStdString();
            changed.sampleRate = sampleRate;
            changed.lengthSamples = frames;
            applyEdit(model::modifyClip(*song_, location, std::move(changed), "Crop Clip"));
            ui_.statusText = "Cropped to " + target.getFileName().toStdString();
        });
    });
}

void AppController::saveFile()
{
    saveThen([](bool) {});
}

void AppController::saveThen(std::function<void(bool)> done)
{
    if (! song_)
    {
        done(false);
        return;
    }
    if (collabGuest())
    {
        // The host keeps the copy of record; a guest saves a copy of its own.
        saveCollabCopy();
        done(false);
        return;
    }
    const juce::File file(juce::String::fromUTF8(song_->sourcePath.c_str()));
    if (isUntitled() || file.getFileExtension() != ".als")
    {
        saveAsDialog(false, std::move(done));
        return;
    }
    io::AlsSaveOptions options;
    options.portableReferences = true;
    done(writeSet(file, options)); // a copy of the previous version goes to the Backup folder first
}

void AppController::saveFileAs()
{
    if (collabGuest()) { saveCollabCopy(); return; }
    saveAsDialog(false, [](bool) {});
}

void AppController::collectAllAndSave()
{
    if (! song_)
        return;
    if (collabGuest()) { saveCollabCopy(); return; }
    const juce::File file(juce::String::fromUTF8(song_->sourcePath.c_str()));
    if (isUntitled() || file.getFileExtension() != ".als")
    {
        saveAsDialog(true, [](bool) {});
        return;
    }
    io::AlsSaveOptions options;
    options.portableReferences = true;
    options.collectAll = true;
    writeSet(file, options);
}

// Save As / first save. A never-saved project gets its own "<Name> Project" folder (Live's convention)
// and brings its recordings along; any other set is written exactly where the user chose.
void AppController::saveAsDialog(bool collect, std::function<void(bool)> done)
{
    if (! song_)
    {
        done(false);
        return;
    }
    const bool untitled = isUntitled();
    juce::File initial = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str()));
    if (untitled)
    {
        projectsRoot().createDirectory();
        initial = projectsRoot().getChildFile("My Project.als");
    }
    const int generation = documentGeneration_;
    chooser_ = std::make_unique<juce::FileChooser>(collect ? "Collect All and Save As" : (untitled ? "Save New Project" : "Save Live Set As"),
                                                   initial, "*.als");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, generation, untitled, collect, finish = std::move(done)](const juce::FileChooser& fc) {
                              if (! song_ || generation != documentGeneration_)
                              {
                                  ui_.statusText = "Save As cancelled: a different set is now open";
                                  finish(false);
                                  return;
                              }
                              auto f = fc.getResult();
                              if (f == juce::File())
                              {
                                  finish(false);
                                  return;
                              }
                              if (! f.hasFileExtension("als"))
                                  f = f.withFileExtension("als");
                              if (untitled && ! f.getParentDirectory().getFileName().endsWith(" Project"))
                              {
                                  const auto name = f.getFileNameWithoutExtension();
                                  const auto folder = f.getParentDirectory().getChildFile(name + " Project");
                                  if (! folder.createDirectory())
                                  {
                                      ui_.statusText = "Could not create the project folder " + folder.getFullPathName().toStdString();
                                      finish(false);
                                      return;
                                  }
                                  f = folder.getChildFile(name + ".als");
                              }
                              io::AlsSaveOptions options;
                              options.portableReferences = true;
                              options.copyProjectSamples = true;
                              options.collectAll = collect;
                              finish(writeSet(f, options));
                          });
}

// Writes a copy elsewhere; the open document and its dirty state are left untouched.
void AppController::exportMidiClip()
{
    const auto* selected = song_ ? ui_.selectedClip() : nullptr;
    if (selected == nullptr || selected->kind != model::Clip::Kind::Midi) return;
    const int generation = documentGeneration_;
    const auto location = ui_.selectedClipLocation();
    const auto folder = isUntitled() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                     : juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory();
    const auto name = juce::File::createLegalFileName(juce::String::fromUTF8((selected->name.empty() ? std::string("MIDI Clip") : selected->name).c_str()));
    chooser_ = std::make_unique<juce::FileChooser>("Export MIDI Clip", folder.getChildFile(name).withFileExtension("mid"), "*.mid");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, generation, location](const juce::FileChooser& fc) {
                              if (! song_ || generation != documentGeneration_) return;
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (! f.hasFileExtension("mid;midi")) f = f.withFileExtension("mid");
                              const auto* clip = model::findClip(*song_, location); // the clip as it is now
                              if (clip == nullptr) { ui_.statusText = "The clip no longer exists"; return; }
                              std::string error;
                              const bool ok = io::exportMidiClip(*clip, *song_, f.getFullPathName().toStdString(), error);
                              ui_.statusText = ok ? "Exported " + f.getFileName().toStdString() : error;
                              if (ui_.requestRepaint) ui_.requestRepaint();
                          });
}

void AppController::saveCopy()
{
    if (! song_)
        return;
    if (collabGuest()) { saveCollabCopy(); return; }
    const int generation = documentGeneration_;
    chooser_ = std::make_unique<juce::FileChooser>("Save a Copy", juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory(), "*.als");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, generation](const juce::FileChooser& fc) {
                              if (! song_ || generation != documentGeneration_) return;
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (! f.hasFileExtension("als")) f = f.withFileExtension("als");
                              if (editingDevices_ || syncingModel_) { ui_.statusText = "Wait until the device operation has finished"; return; }
                              syncModelFromEngine();
                              song_->inKey = ui_.scaleHighlight;
                              io::AlsSaveOptions options;
                              options.portableReferences = true;
                              options.copyProjectSamples = true;
                              const auto result = io::saveAls(*song_, f, options);
                              ui_.statusText = result.ok ? "Saved a copy as " + f.getFileName().toStdString() : "Could not save a copy: " + result.error;
                              if (ui_.requestRepaint) ui_.requestRepaint();
                          });
}

juce::File AppController::midiSettingsFile()
{
    return core::appDataDirectory().getChildFile("MidiSettings.json");
}

// Mappings: the set's sidecar (*.sonora.json) wins; without one the mappings stored by Live in
// the .als are used. Transport/tempo mappings live in the global file and apply to every set.
void AppController::loadMappingsForSong()
{
    std::vector<midi::Mapping> mappings;
    if (! midi::loadMappings(midi::sidecarFor(juce::File(song_->sourcePath)), mappings))
        mappings = midi::importLiveMappings(*song_);
    std::vector<midi::Mapping> global;
    midi::loadMappings(midi::globalMappingsFile(), global);
    mappings.insert(mappings.end(), global.begin(), global.end());
    midi::bindMappingsToSong(*song_, mappings);

    loadingMappings_ = true;
    midi_.setMappings(std::move(mappings));
    loadingMappings_ = false;
}

void AppController::saveMappings()
{
    if (loadingMappings_ || ! song_)
        return;
    std::vector<midi::Mapping> perSet, global;
    for (const auto& m : midi_.mappings())
    {
        const auto k = m.target.kind;
        const bool isGlobal = k == midi::ControlTarget::Kind::TransportPlay || k == midi::ControlTarget::Kind::TransportStop
                              || k == midi::ControlTarget::Kind::TransportToggle || k == midi::ControlTarget::Kind::Tempo;
        (isGlobal ? global : perSet).push_back(m);
    }
    midi::saveMappings(midi::sidecarFor(juce::File(song_->sourcePath)), midi::mappingsForSave(*song_, std::move(perSet)));
    midi::saveMappings(midi::globalMappingsFile(), global);
}

void AppController::findProjectsInBackground()
{
    std::weak_ptr<bool> alive = alive_;
    pool_.addJob([this, alive] {
        std::vector<std::string> found;
        for (auto loc : {juce::File::userMusicDirectory, juce::File::userDocumentsDirectory, juce::File::userDesktopDirectory})
        {
            const auto dir = juce::File::getSpecialLocation(loc);
            // The walk visits every file under the folder (sample libraries too), so it takes every file and checks
            // for quit on each one (a "*.als" wildcard would only return between matches): removeAllJobs in
            // ~AppController signals the job before alive_ expires.
            auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
            for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles))
            {
                if (alive.expired() || (job != nullptr && job->shouldExit()))
                    return;
                if (! entry.getFile().hasFileExtension("als"))
                    continue;
                const auto path = entry.getFile().getFullPathName();
                if (! path.contains("/Backup/") && ! path.contains("/Library/"))
                    found.push_back(path.toStdString());
                if (found.size() > 2000)
                    break;
            }
        }
        std::sort(found.begin(), found.end());
        juce::MessageManager::callAsync([this, alive, files = std::move(found)]() mutable {
            if (alive.expired())
                return;
            ui_.projectFiles = std::move(files);
            if (ui_.requestRepaint)
                ui_.requestRepaint();
        });
    });
}

// ------------------------------------------------------------------------- plugins

void AppController::refreshPluginNames()
{
    ui_.pluginNames.clear();
    ui_.browserPlugins.clear();
    for (const auto& d : host_.knownPlugins().getTypes())
    {
        ui_.pluginNames.push_back((d.name + "  (" + d.pluginFormatName + ")").toStdString());
        ui_.browserPlugins.push_back({d.name.toStdString(), d.createIdentifierString().toStdString(),
                                      d.manufacturerName.trim().toStdString(), d.pluginFormatName.toStdString(), d.isInstrument, d.category.toStdString()});
    }
    std::sort(ui_.pluginNames.begin(), ui_.pluginNames.end());
    std::sort(ui_.browserPlugins.begin(), ui_.browserPlugins.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
}

void AppController::editDevice(int trackIndex, int index, const std::function<void(model::Device&)>& edit, const std::string& label)
{
    editDeviceIf(trackIndex, index, [&](auto& device) { edit(device); return true; }, label);
}

void AppController::editDeviceIf(int trackIndex, int index, const std::function<bool(model::Device&)>& edit, const std::string& label)
{
    if (! song_ || editingDevices_ || syncingModel_) return;
    if (ui_.recording) { ui_.statusText = "Finish recording before editing device structure"; return; }
    const int generation = documentGeneration_;
    syncModelFromEngine();
    if (generation != documentGeneration_) return;
    const auto* track = model::trackAt(*song_, trackIndex);
    if (! track || index < 0 || (size_t) index >= track->devices.size()) return;
    auto device = track->devices[(size_t) index];
    const int oldId = device.id;
    const auto oldChains = device.chains.size();
    if (! edit(device)) return;
    device.id = -1; // a structural edit needs a fresh processor, preserving the rest of the track
    model::allocateDeviceIds(*song_, device);
    if (const auto bank = ui_.drumPadBanks.find(oldId); bank != ui_.drumPadBanks.end()) ui_.drumPadBanks[device.id] = bank->second;
    if (const auto view = ui_.wavetableFxViews.find(oldId); view != ui_.wavetableFxViews.end())
        ui_.wavetableFxViews[device.id] = view->second;
    if (const auto view = ui_.wavetableModulationViews.find(oldId); view != ui_.wavetableModulationViews.end())
        ui_.wavetableModulationViews[device.id] = view->second;
    if (const auto view = ui_.wavetableRoutingViews.find(oldId); view != ui_.wavetableRoutingViews.end())
        ui_.wavetableRoutingViews[device.id] = view->second;
    if (ui_.selectedRackId == oldId) ui_.selectedRackId = device.id;
    if (device.kind == model::DeviceKind::Rack)
    {
        ui_.selectedRackId = device.id;
        if (device.chains.size() > oldChains) ui_.selectedRackChain = (int) device.chains.size() - 1;
        else ui_.selectedRackChain = std::clamp(ui_.selectedRackChain, 0, std::max(0, (int) device.chains.size() - 1));
    }
    applyEdit(model::replaceDevice(*song_, trackIndex, index, std::move(device), label));
}

void AppController::importWavetable(int trackIndex, int index, int oscillator, const std::string& path, int chain, int child)
{
    // A Serum table (its `clm ` chunk) keeps the reference frame law; any other WAV uses Sonora's continuous layout.
    const auto loaded = plugins::loadSerumTable(juce::File(juce::String(path)));
    if (! loaded) { ui_.statusText = "Could not import wavetable: " + loaded.error; return; }
    bool changed = false;
    std::string error;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
            changed = devices::storeWavetableTable(*target, oscillator, *loaded.data, error);
        else error = "The selected wavetable device is no longer available";
        return changed;
    }, "Import Wavetable");
    if (changed) ui_.statusText = "Imported " + loaded.data->name();
    else if (! error.empty()) ui_.statusText = "Could not import wavetable: " + error;
}

void AppController::importOscillatorMedia(int trackIndex, int index, int oscillator, const std::string& path, int chain, int child)
{
    const juce::File file{juce::String(path)};
    const auto loaded = file.hasFileExtension("sfz") ? devices::OscillatorMedia::fromSfz(file, true)
                                                    : devices::OscillatorMedia::fromFile(file, true);
    if (! loaded) { ui_.statusText = "Could not import sample bank: " + loaded.error; return; }
    bool changed = false;
    std::string error;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
            changed = devices::storeOscillatorMedia(*target, oscillator, *loaded.data, error);
        else error = "The selected wavetable device is no longer available";
        return changed;
    }, "Import Oscillator Media");
    if (changed)
    {
        ui_.buildLog.insert(ui_.buildLog.end(), loaded.warnings.begin(), loaded.warnings.end());
        ui_.statusText = "Imported " + loaded.data->name();
        if (! loaded.warnings.empty()) ui_.statusText += " — " + std::to_string(loaded.warnings.size()) + " sample bank notes";
    }
    else if (! error.empty()) ui_.statusText = "Could not import sample bank: " + error;
}

void AppController::editWavetableLfo(int trackIndex, int index, int lfo, const dsp::CurveGraph& curve, int chain, int child)
{
    std::string error;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
            return devices::storeWavetableLfo(*target, lfo, curve, error);
        error = "The selected wavetable device is no longer available";
        return false;
    }, "Edit LFO Shape");
    if (! error.empty()) ui_.statusText = "Could not edit LFO: " + error;
}

void AppController::editWavetableModulationCurve(int trackIndex, int index, int slot, bool auxiliary,
                                               const dsp::CurveGraph& curve, bool enabled, int chain, int child)
{
    std::string error;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
            return devices::storeWavetableModulationCurve(*target, slot, auxiliary, curve, enabled, error);
        error = "The selected wavetable device is no longer available";
        return false;
    }, "Edit Modulation Curve");
    if (! error.empty()) ui_.statusText = "Could not edit modulation curve: " + error;
}

void AppController::setWavetableEffects(int trackIndex, int index,
                                       const devices::WavetableEffects::Configuration& configuration, int chain, int child)
{
    std::string error;
    {
        devices::WavetableEffects prepared;
        if (! prepared.prepare(configuration, engine_.sampleRate() > 0 ? engine_.sampleRate() : 44100, error))
        { ui_.statusText = "Could not edit effects: " + error; return; }
    }
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
            return devices::storeWavetableEffects(*target, configuration, error);
        error = "The selected wavetable device is no longer available";
        return false;
    }, "Edit Instrument Effects");
    if (! error.empty()) ui_.statusText = "Could not edit effects: " + error;
}

void AppController::importWavetableImpulseResponse(int trackIndex, int index, devices::WavetableEffects::ModuleId moduleId,
                                                  const std::string& path, int chain, int child)
{
    const auto loaded = devices::ConvolutionIR::fromFile(juce::File(juce::String(path)));
    if (! loaded) { ui_.statusText = "Could not import impulse response: " + loaded.error; return; }
    bool changed = false; std::string error;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        auto* target = wavetableTarget(parent, chain, child);
        if (! target) { error = "The selected wavetable device is no longer available"; return false; }
        devices::WavetableEffects::Configuration configuration;
        if (! devices::readWavetableEffects(*target, configuration, error)) return false;
        const auto replace = [&](const auto& self, std::vector<devices::WavetableEffects::Module>& modules) -> bool {
            for (auto& module : modules)
            {
                if (module.id == moduleId)
                {
                    if (module.type != devices::WavetableEffects::Type::Convolution)
                    { error = "Select a convolution module for this impulse response"; return false; }
                    module.impulse = loaded.data; return true;
                }
                for (auto& band : module.bands) if (self(self, band)) return true;
            }
            return false;
        };
        bool found = false;
        for (auto& bus : configuration.buses) if (replace(replace, bus)) { found = true; break; }
        if (! found) { if (error.empty()) error = "The selected convolution module is no longer available"; return false; }
        devices::WavetableEffects prepared;
        if (! prepared.prepare(configuration, engine_.sampleRate() > 0 ? engine_.sampleRate() : 44100, error)) return false;
        changed = devices::storeWavetableEffects(*target, configuration, error);
        return changed;
    }, "Import Convolution Impulse Response");
    if (changed) ui_.statusText = "Imported " + loaded.data->name();
    else if (! error.empty()) ui_.statusText = "Could not import impulse response: " + error;
}

void AppController::importWavetablePreset(int trackIndex, int index, const std::string& path, int chain, int child)
{
    const juce::File file{juce::String(path)};
    auto imported = plugins::loadPrismPreset(file); // .prism, or another instrument preset the importer reads
    if (! imported) { ui_.statusText = "Could not open preset: " + imported.error; return; }
    bool changed = false;
    editDeviceIf(trackIndex, index, [&](auto& parent) {
        if (auto* target = wavetableTarget(parent, chain, child))
        {
            for (auto& p : imported.device.parameters)
                for (const auto& old : target->parameters)
                    if (old.path == p.path)
                    {
                        p.automationTargetId = old.automationTargetId;
                        p.midi = old.midi;
                        p.macroIndex = old.macroIndex;
                        p.rangeMin = old.rangeMin; p.rangeMax = old.rangeMax;
                        break;
                    }
            imported.device.id = target->id;
            imported.device.enabledTargetId = target->enabledTargetId;
            *target = std::move(imported.device);
            changed = true;
        }
        return changed;
    }, "Load Preset");
    if (changed)
    {
        ui_.buildLog.insert(ui_.buildLog.end(), imported.warnings.begin(), imported.warnings.end());
        ui_.statusText = "Loaded " + file.getFileNameWithoutExtension().toStdString();
        if (! imported.warnings.empty()) ui_.statusText += " — " + std::to_string(imported.warnings.size()) + " compatibility notes";
    }
}

void AppController::dropDeviceItem(const ui::BrowserDrag& item, int trackIndex, int index, int chainIndex, int zoneIndex, int childIndex)
{
    if (! song_ || editingDevices_ || syncingModel_) return;
    auto* track = model::trackAt(*song_, trackIndex);
    if (! track || index < 0 || (size_t) index >= track->devices.size()) return;
    const auto& target = track->devices[(size_t) index];
    if (target.kind != model::DeviceKind::Rack && target.type != "OriginalSimpler" && target.type != "MultiSampler" && target.type != "SonoraImpulse") return;
    if (target.type == "AudioEffectGroupDevice" && item.kind == ui::BrowserDrag::Kind::Sample)
    { ui_.statusText = "Drop samples on an instrument or drum rack"; return; }
    if (item.kind == ui::BrowserDrag::Kind::Plugin)
    { ui_.statusText = "Append hosted plugins to the track; rack insertion currently accepts native devices"; return; }
    const auto maxPatchPath = item.value.starts_with("max:") ? locateMaxPatch(*song_, ui_.userLibraryPath, item.value.substr(4)) : std::string();
    auto insert = [chainIndex, zoneIndex, childIndex, item, maxPatchPath](model::Device& root, const std::optional<model::SamplePart>& part) {
        auto* device = &root;
        if (root.kind == model::DeviceKind::Rack)
        {
            int ci = chainIndex;
            if (ci >= 128) // a drum-pad address (128 + MIDI note), including empty pads
            {
                const int note = ci - 128;
                ci = -1;
                for (size_t i = 0; i < root.chains.size(); ++i) if (root.chains[i].receivingNote == note) ci = (int) i;
                if (ci < 0)
                {
                    model::Device::Chain chain;
                    chain.name = item.label; chain.receivingNote = note; chain.sendingNote = 60;
                    root.chains.push_back(std::move(chain)); ci = (int) root.chains.size() - 1;
                }
            }
            if (ci < 0 || (size_t) ci >= root.chains.size())
            { model::Device::Chain chain; chain.name = item.label; root.chains.push_back(std::move(chain)); ci = (int) root.chains.size() - 1; }
            auto& chain = root.chains[(size_t) ci];
            root.chainsEdited = true;
            if (! part)
            {
                auto child = devices::nativeDeviceModel(item.value);
                child.maxPatchPath = maxPatchPath;
                const auto* entry = model::findCatalogEntry(item.value);
                if (entry && entry->role == model::DeviceRole::MidiEffect) chain.devices.insert(chain.devices.begin(), std::move(child));
                else chain.devices.push_back(std::move(child));
                return;
            }
            auto found = std::find_if(chain.devices.begin(), chain.devices.end(), [](const auto& child) {
                return child.type == "OriginalSimpler" || child.type == "MultiSampler" || child.type == "SonoraImpulse";
            });
            if (childIndex >= 0 && (size_t) childIndex < chain.devices.size()) found = chain.devices.begin() + childIndex;
            if (found == chain.devices.end())
            {
                chain.devices.push_back(devices::nativeDeviceModel("OriginalSimpler"));
                found = chain.devices.end() - 1;
                // Drum voices ignore note length, so a short keyboard tap still plays the hit.
                if (root.type == "DrumGroupDevice") found->plain["Globals/PlaybackMode"] = 1;
            }
            device = &*found;
        }
        if (part)
        {
            if (device->type == "SonoraImpulse")
            {
                static constexpr int notes[]{60, 62, 64, 65, 67, 69, 71, 72};
                if (zoneIndex < 0 || zoneIndex >= 8) return;
                auto replacement = *part;
                replacement.rootKey = replacement.keyMin = replacement.keyMax = notes[zoneIndex];
                replacement.loopMode = 0;
                auto found = std::find_if(device->sampleParts.begin(), device->sampleParts.end(), [&](const auto& z) { return z.rootKey == notes[zoneIndex]; });
                if (found == device->sampleParts.end()) device->sampleParts.push_back(std::move(replacement));
                else *found = std::move(replacement);
                device->samplePartsEdited = true;
                return;
            }
            device->samplePartsEdited = true;
            if (zoneIndex >= 0 && (size_t) zoneIndex < device->sampleParts.size())
            {
                auto replacement = *part;
                const auto& old = device->sampleParts[(size_t) zoneIndex];
                replacement.rootKey = old.rootKey; replacement.keyMin = old.keyMin; replacement.keyMax = old.keyMax;
                replacement.velMin = old.velMin; replacement.velMax = old.velMax;
                device->sampleParts[(size_t) zoneIndex] = std::move(replacement);
            }
            else device->sampleParts.push_back(*part);
        }
    };
    if (item.kind == ui::BrowserDrag::Kind::NativeDevice)
    {
        if (target.kind != model::DeviceKind::Rack) return;
        const auto* entry = model::findCatalogEntry(item.value);
        if (! entry || ! entry->implemented || model::isRackTag(item.value)) return;
        if (target.type == "AudioEffectGroupDevice" && entry->role != model::DeviceRole::AudioEffect)
        { ui_.statusText = "Drop audio effects on an audio effect rack"; return; }
        const int firstNewId = song_->nextDeviceId;
        const auto trackId = track->id;
        editDevice(trackIndex, index, [insert](auto& d) { insert(d, {}); }, "Add " + item.label + " to Rack");
        if (item.value == "SonoraWavetable") openInsertedWavetable(trackId, -1, firstNewId);
        return;
    }
    const int generation = documentGeneration_, deviceId = target.id;
    const auto alive = alive_;
    ui_.statusText = "Reading " + item.label;
    pool_.addJob([this, alive, generation, deviceId, trackIndex, index, insert, item] {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(item.value)));
        model::SamplePart part;
        part.name = juce::File(item.value).getFileNameWithoutExtension().toStdString();
        part.sample.absolutePath = item.value;
        if (reader) { part.sampleRate = reader->sampleRate; part.lengthSamples = part.sampleEnd = reader->lengthInSamples; }
        juce::MessageManager::callAsync([this, alive, generation, deviceId, trackIndex, index, insert, part] {
            if (! *alive || generation != documentGeneration_ || ! song_) return;
            const auto* current = model::trackAt(*song_, trackIndex);
            if (! current || index < 0 || (size_t) index >= current->devices.size() || current->devices[(size_t) index].id != deviceId) return;
            if (part.sampleRate <= 0 || part.lengthSamples <= 0) { ui_.statusText = "Could not read sample"; return; }
            editDevice(trackIndex, index, [insert, part](auto& d) { insert(d, part); }, "Load Sample");
        });
    });
}

void AppController::dropBrowserItem(const ui::BrowserDrag& item, int trackIndex, double beat, int scene)
{
    if (! song_ || editingDevices_ || syncingModel_) return;
    const int dropGeneration = documentGeneration_;
    if (ui_.recording) { ui_.statusText = "Finish recording before dropping browser items"; return; }
    if (item.kind == ui::BrowserDrag::Kind::Sample && collabGuest() && inCollabSession())
    {
        // COL-4: the file goes to the host, which drops it for everyone (its copy lands in the host's project).
        if (trackIndex >= (int) song_->tracks.size()) { ui_.statusText = "Drop samples on an audio track"; return; }
        const auto* track = trackIndex >= 0 ? ui_.trackAt(trackIndex) : nullptr;
        if (! collab_->uploadAndDrop(juce::File(juce::String::fromUTF8(item.value.c_str())), track ? track->id : -1, beat, scene))
            ui_.statusText = collab_->canEdit() ? "Could not send that file to the host" : "You are a listener in this session";
        return;
    }
    if (item.kind == ui::BrowserDrag::Kind::Sample)
    {
        // trackIndex -1 (empty space below the tracks / Session drop area) creates a new audio track, and so does a MIDI or
        // group track (the new one goes right after it, io::placeSamples).
        if (trackIndex >= (int) song_->tracks.size())
        {
            ui_.statusText = "Drop samples on an audio track";
            return;
        }
        if (exporting_) return;
        auto paths = item.files.empty() ? std::vector<std::string>{item.value} : item.files;
        const io::SampleDropTarget target{trackIndex, beat, scene, item.oneTrackPerFile};
        const auto alive = alive_;
        const double tempoHint = song_->tempo; // estimateSampleTempo breaks octave ties towards the set's tempo
        ui_.statusText = "Reading " + item.label;
        pool_.addJob([this, alive, dropGeneration, item, paths, target, tempoHint] {
            std::vector<io::DroppedSample> samples;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (const auto& path : paths)
            {
                const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String::fromUTF8(path.c_str()))));
                samples.push_back({path, reader ? reader->sampleRate : 0.0, reader ? (int64_t) reader->lengthInSamples : 0});
                // UI-47: the clip starts warped at the sample's own tempo, like Live, so detect it now (the sample cache
                // loads the file for playback anyway). Loops find a tempo; one-shots keep the set's (and the status says so).
                if (reader && reader->lengthInSamples > 0 && *alive)
                    if (const auto loaded = engine_.sampleCache().load(path))
                    {
                        const auto estimate = engine::estimateSampleTempo(loaded->audio, loaded->sampleRate, tempoHint);
                        samples.back().detectedBpm = estimate.bpm;
                        samples.back().tempoConfidence = estimate.confidence;
                    }
            }
            io::shareStemTempo(samples); // UI-47b: stems of one song keep one grid
            juce::MessageManager::callAsync([this, alive, dropGeneration, item, target, samples] {
                if (! *alive || dropGeneration != documentGeneration_ || ! song_ || editingDevices_ || syncingModel_ || exporting_) return;
                if (ui_.recording || ui_.recordingRequested) { ui_.statusText = "Finish recording before dropping samples"; return; }
                auto plan = io::placeSamples(*song_, samples, target);
                if (! plan.command.apply)
                {
                    ui_.statusText = samples.size() == 1 && plan.error.rfind("Could not read", 0) == 0 ? "Could not read " + item.label : plan.error;
                    return;
                }
                const bool structural = plan.command.structureChanged;
                if (! applyEdit(std::move(plan.command)).ok) return;
                if (structural && plan.firstTrack >= 0 && plan.firstTrack < ui_.numTracks())
                {
                    ui_.selectTrack(plan.firstTrack, false, false);
                    if (ui_.modelChanged) ui_.modelChanged();
                }
                const int skipped = (int) samples.size() - plan.placed;
                int noTempo = 0;
                double tempo = 0;
                for (const auto& sample : samples)
                    if (sample.sampleRate > 0 && sample.frames > 0) { noTempo += sample.detectedBpm <= 0; tempo = sample.detectedBpm; }
                ui_.statusText = (plan.placed == 1 ? "Inserted " + juce::File(juce::String::fromUTF8(item.value.c_str())).getFileName().toStdString()
                                                   : "Inserted " + std::to_string(plan.placed) + " samples")
                               + (plan.newTrackForKind ? " on a new audio track" : "")
                               + (skipped > 0 ? " (" + std::to_string(skipped) + " could not be read)" : "");
                // UI-47b: never fall back to the set's tempo silently.
                if (noTempo == 0 && plan.placed == 1)
                    ui_.statusText += " at " + juce::String(tempo, 2).toStdString() + " BPM";
                else if (noTempo > 0)
                    ui_.statusText += (plan.placed == 1 ? ". No tempo detected; the clip uses"
                                                        : ". " + std::to_string(noTempo) + " without a detected tempo use")
                                    + std::string(" the set tempo (edit the clip BPM in Warp)");
            });
        });
        return;
    }
    auto* track = model::trackAt(*song_, trackIndex);
    if (! track) return;
    model::Device device;
    device.type = item.kind == ui::BrowserDrag::Kind::NativeDevice ? item.value : "PluginDevice";
    device.displayName = item.label;
    bool instrument = false;
    if (item.kind == ui::BrowserDrag::Kind::NativeDevice)
    {
        const auto* entry = model::findCatalogEntry(device.type);
        if (! entry || ! entry->implemented) return;
        if (entry->role == model::DeviceRole::MidiEffect && track->kind != model::TrackKind::Midi)
        { ui_.statusText = "Drop MIDI effects on a MIDI track"; return; }
        instrument = entry->role == model::DeviceRole::Instrument;
        device = devices::nativeDeviceModel(item.value);
        if (device.kind == model::DeviceKind::MaxForLive) device.maxPatchPath = locateMaxPatch(*song_, ui_.userLibraryPath, device.maxPatch);
    }
    else
    {
        const auto types = host_.knownPlugins().getTypes();
        const auto found = std::find_if(types.begin(), types.end(), [&](const auto& desc) { return desc.createIdentifierString().toStdString() == item.value; });
        if (found == types.end()) { ui_.statusText = "Plugin is no longer in the scan cache"; return; }
        if (found->pluginFormatName != "VST3") { ui_.statusText = "Browser insertion currently supports VST3 plugins"; return; }
        instrument = found->isInstrument;
        if (instrument && track->kind != model::TrackKind::Midi) { ui_.statusText = "Drop instruments on a MIDI track"; return; }
        juce::String error;
        auto instance = host_.formats().createPluginInstance(*found, engine_.sampleRate(), engine_.blockSize(), error);
        if (! instance) { ui_.statusText = "Could not load plugin: " + error.toStdString(); return; }
        const auto* client = instance->getVST3Client();
        const auto preset = client ? client->getPreset() : juce::MemoryBlock{};
        model::PluginInfo info;
        info.name = found->name.toStdString();
        info.vendor = found->manufacturerName.toStdString();
        info.isInstrument = instrument;
        const auto* bytes = static_cast<const uint8_t*>(preset.getData());
        if (preset.isEmpty() || ! plugins::readVstPreset({bytes, bytes + preset.getSize()}, info))
        { ui_.statusText = "Plugin did not provide a saveable VST3 preset"; return; }
        device.kind = model::DeviceKind::PluginVst3;
        device.plugin = std::move(info);
        device.displayName = found->name.toStdString();
    }
    if (dropGeneration != documentGeneration_) return; // a plugin may dispatch an Open Set completion
    if (instrument && track->kind != model::TrackKind::Midi) { ui_.statusText = "Drop instruments on a MIDI track"; return; }
    syncModelFromEngine();
    if (dropGeneration != documentGeneration_) return;
    model::allocateDeviceIds(*song_, device);
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    const bool replacing = instrument && model::instrumentIndex(*track) >= 0;
    const bool wavetable = device.type == "SonoraWavetable";
    const int insertedId = device.id;
    const auto trackId = track->id;
    const bool inserted = applyEdit(model::insertTrackDevice(*song_, trackIndex, std::move(device))).ok;
    if (inserted && wavetable) openInsertedWavetable(trackId, insertedId, -1);
    ui_.selectedTrack = trackIndex;
    ui_.detailTab = ui::DetailTab::Devices;
    ui_.detailVisible = true;
    ui_.statusText = (replacing ? "Replaced instrument with " : "Inserted ") + item.label;
}

void AppController::scanPlugins(bool includeAu)
{
    if (scanning_.exchange(true))
        return;
    ui_.statusText = "Scanning plug-ins...";
    ui_.scanning = true;
    const int busy = ui_.beginBusy("Scanning plug-ins...", true);
    std::weak_ptr<bool> alive = alive_;
    pool_.addJob([this, includeAu, alive, busy] {
        host_.scan(includeAu, [this, alive, busy](float progress, const juce::String& name) {
            juce::MessageManager::callAsync([this, alive, progress, name, busy] {
                if (alive.expired())
                    return;
                ui_.statusText = "Scanning " + std::to_string((int) (progress * 100)) + "%  " + name.toStdString();
                ui_.updateBusy(busy, "Scanning plug-ins: " + name.toStdString(), progress);
                if (ui_.requestRepaint)
                    ui_.requestRepaint();
            });
        });
        juce::MessageManager::callAsync([this, alive, busy] {
            if (alive.expired())
                return;
            scanning_ = false;
            ui_.scanning = false;
            ui_.endBusy(busy);
            ui_.failedPlugins.clear();
            for (const auto& f : host_.failedPlugins())
                ui_.failedPlugins.push_back(f.toStdString());
            refreshPluginNames();
            ui_.statusText = "Scan finished: " + std::to_string(host_.knownPlugins().getNumTypes())
                             + " plug-ins. Re-open the set to load them.";
            if (ui_.requestRepaint)
                ui_.requestRepaint();
        });
    });
}

void AppController::closePluginWindows()
{
    pluginWindows_.clear();
}

// ------------------------------------------------------------------------- floating Wavetable editors (UI-27)

void AppController::openWavetableWindow(const ui::UiContext::WavetableKey& key)
{
    for (auto* window : wavetableWindows_)
        if (auto* w = static_cast<WavetableWindow*>(window); w->key() == key && w->isVisible())
        {
            w->toFront(true);
            return;
        }
    if (! song_ || ! ui_.song || ! ui::findWavetable(ui_, key)) return;
    wavetableWindows_.add(new WavetableWindow(ui_, key, [this](WavetableWindow* w) { wavetableWindows_.removeObject(w); },
                                              [this](const juce::KeyPress& k) { return forwardKeyToMainWindow && forwardKeyToMainWindow(k); }));
}

void AppController::closeWavetableWindow(const ui::UiContext::WavetableKey& key)
{
    // Can run inside that window's own event (Dock button, device deleted while it repaints): hide now, delete later.
    for (auto* window : wavetableWindows_)
    {
        if (static_cast<WavetableWindow*>(window)->key() != key || ! window->isVisible()) continue;
        window->setVisible(false);
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<bool>(alive_), safe = juce::Component::SafePointer<juce::DocumentWindow>(window)] {
            const auto lifetime = alive.lock();
            if (lifetime && *lifetime && safe != nullptr) wavetableWindows_.removeObject(safe.getComponent());
        });
    }
}

void AppController::closeWavetableWindows()
{
    wavetableWindows_.clear();
}

void AppController::syncWavetableWindows()
{
    for (auto* window : wavetableWindows_)
        if (window->isVisible()) static_cast<WavetableWindow*>(window)->sync();
}

void AppController::followWavetableIds()
{
    if (! song_) return;
    // Keyed instances keep their placement and open window when an edit, undo or redo swaps their device id.
    for (const auto& replaced : ui::replacedDevices(deviceIdsBeforeEdit_, *song_))
    {
        ui_.renameWavetableDevice(replaced.track, replaced.oldId, replaced.newId);
        for (auto* window : wavetableWindows_)
            if (auto* w = static_cast<WavetableWindow*>(window); w->key().trackId == replaced.track && w->key().deviceId == replaced.oldId)
            {
                auto key = w->key();
                key.deviceId = replaced.newId;
                w->setKey(key);
            }
    }
    deviceIdsBeforeEdit_.clear();
}

// A Wavetable inserted from the browser opens floating, like an inserted plug-in (set loads open no windows). childIdFrom >= 0: the new instance is
// a rack child, the first id allocated by the insertion (the rack itself took a fresh id too).
void AppController::openInsertedWavetable(model::TrackId trackId, int deviceId, int childIdFrom)
{
    if (! song_) return;
    for (int i = 0; i < (int) (song_->tracks.size() + song_->returns.size()) + 1; ++i)
    {
        const auto* track = model::trackAt(*song_, i);
        if (track == nullptr || track->id != trackId) continue;
        for (const auto& device : track->devices)
        {
            std::optional<ui::UiContext::WavetableKey> key;
            if (childIdFrom < 0 && device.id == deviceId && device.type == "SonoraWavetable")
                key = ui::UiContext::WavetableKey{trackId, deviceId, -1, -1};
            for (size_t c = 0; childIdFrom >= 0 && ! key && c < device.chains.size(); ++c)
                for (const auto& child : device.chains[c].devices)
                    if (child.type == "SonoraWavetable" && child.id >= childIdFrom)
                        key = ui::UiContext::WavetableKey{trackId, device.id, (int) c, child.id};
            if (key)
            {
                // A fresh instance always starts floating, even where it replaced an instrument whose id it took over.
                if (ui_.wavetableFloating.erase(*key) > 0) ++ui_.wavetablePlacementRevision;
                if (ui_.isWavetableFloating(*key)) openWavetableWindow(*key);
                return;
            }
        }
        return;
    }
}

bool AppController::pluginEditorOpen(const juce::AudioPluginInstance* instance) const
{
    for (auto* window : pluginWindows_)
        if (static_cast<PluginWindow*>(window)->plugin() == instance) return true;
    return false;
}

void AppController::openPluginEditor(int track, int device)
{
    auto* graph = engine_.latestGraph();
    if (graph == nullptr || (size_t) track >= graph->nodes.size())
        return;
    const auto& node = graph->nodes[(size_t) track];
    if ((size_t) device >= node.processorForModelDevice.size())
        return;
    auto* dev = dynamic_cast<plugins::PluginDevice*>(node.processorForModelDevice[(size_t) device]);
    if (dev == nullptr)
        return;
    // PLG-10: a plugin has one editor. Opening it again must bring that window forward: asking for a second editor fails
    // (JUCE asserts and returns none), which left a generic slider page instead of the plugin's own interface.
    for (auto* window : pluginWindows_)
        if (auto* existing = static_cast<PluginWindow*>(window); existing->plugin() == &dev->instance())
        {
            existing->setVisible(true);
            existing->toFront(true);
            return;
        }
    pluginWindows_.add(new PluginWindow(dev->instance(), [this](PluginWindow* w) { pluginWindows_.removeObject(w); }));
}

// ------------------------------------------------------------------------- settings dialogs

void AppController::showAudioSettings()
{
    if (ui_.recording || ui_.recordingRequested || exporting_ || editingDevices_ || syncingModel_) return;
    if (audioSettingsWindow_) { audioSettingsWindow_->toFront(true); return; }
    auto show = [this, alive = std::weak_ptr<bool>(alive_)](bool inputAllowed) {
        const auto lifetime = alive.lock();
        if (! lifetime || ! *lifetime || ui_.recording || ui_.recordingRequested || exporting_ || editingDevices_ || syncingModel_) return;
        if (audioSettingsWindow_) { audioSettingsWindow_->toFront(true); return; }
        auto* selector = new juce::AudioDeviceSelectorComponent(engine_.deviceManager(), 0, inputAllowed ? 64 : 0, 0, 64, false, false, true, false);
        selector->setSize(560, 480);
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned(selector);
        options.dialogTitle = "Audio Input / Output Configuration";
        options.useNativeTitleBar = true;
        options.resizable = false;
        audioSettingsWindow_ = options.launchAsync();
    };
    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio, std::move(show));
}

void AppController::loadThemeDialog()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Load Live Theme (.ask)", juce::File("/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Themes"),
        "*.ask");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& fc) {
                              const auto f = fc.getResult();
                              if (f.existsAsFile() && ui_.theme.loadAsk(f.getFullPathName().toStdString()))
                              {
                                  settings().setValue("themePath", f.getFullPathName());
                                  ui_.statusText = "Theme: " + ui_.theme.sourceName;
                                  if (ui_.modelChanged)
                                      ui_.modelChanged();
                              }
                          });
}

void AppController::enableAllMidiInputs()
{
    midi_.enableAllInputs();
    settings().setValue("midiAllInputs", true);
    ui_.statusText = "MIDI inputs enabled: " + std::to_string(midi_.availableInputs().size());
}

} // namespace sonora::app
