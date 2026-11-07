#pragma once
#include <map>
// Owns the long-lived services (plugin host, MIDI, audio engine), the current Song and
// the UiContext, and implements every user command. The window only hosts the UI.
//
// Lifetime/threading rules (see docs/ARCHITECTURE.md):
//  - .als parsing runs on a background thread; installing the song (graph build, plugin
//    instantiation) runs on the message thread.
//  - Before a new graph is installed, plugin windows are closed and views are told via
//    UiContext::modelChanged so nothing points into the old graph.

#include "collab/CollabSession.h"
#include "collab/Net.h"
#include "collab/Video.h"
#include "core/Edit.h"
#include "control/ControlService.h"
#include "control/LocalEndpoint.h"
#include "engine/AudioEngine.h"
#include "engine/Exporter.h"
#include "engine/StemSeparator.h"
#include "engine/AutomationCapture.h"
#include "engine/PeakCache.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiClockOut.h"
#include "midi/MidiCapture.h"
#include "midi/MidiManager.h"
#include "midi/MappingStore.h"
#include "plugins/PluginHost.h"
#include "plugins/NativeReplacement.h"
#include "packs/PackStore.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <set>

namespace sonora::app
{

class AppController : private juce::Timer
{
public:
    AppController();
    ~AppController() override;

    ui::UiContext& ui() { return ui_; }

    void openFile(const juce::File& file);
    void openFileDialog();
    void importAudioDialog();
    void renameSelection();
    void editLocatorText(bool infoText); // UI-3b: the selected locator's name or info text
    void openPluginEditor(int track, int device);
    // UI-27: floating Sonora Prism editors. Must run synchronously whenever the model changes (see UiContext::modelChanged).
    void syncWavetableWindows();
    std::function<bool(const juce::KeyPress&)> forwardKeyToMainWindow; // keys a floating editor leaves unused (Space...)
    void scanPlugins(bool includeAu);
    void showAudioSettings();
    void loadThemeDialog();
    void enableAllMidiInputs();

    // ---- editing (UI-3 / UI-10 / ALS-10)
    control::ApplyResult applyEdit(model::EditCommand cmd);
    control::ControlService& control() { return *control_; }
    control::ApplyResult enableLocalControl(bool enabled);
    bool localControlEnabled() const { return controlEndpoint_.enabled(); }
    control::LocalEndpoint::Activity localControlActivity() const { return controlEndpoint_.activity(); }
    std::function<void(const juce::String&, const juce::var&)> onControlActivity;
    void undo();
    void redo();
    void newProject();
    void saveFile();
    void saveFileAs();
    void saveCopy();
    void exportMidiClip(); // File > Export MIDI Clip: the selected MIDI clip as a .mid file (UI-25b)
    void reverseClipSample(const model::ClipLocation& location);
    void toggleClipWarp(const model::ClipLocation&);
    // Clip > Stems (AppStems.cpp): runs an installed separator (audio-separator / demucs) on the clip's sample and puts each stem
    // on a new audio track, clip settings (warp, loop, gain) copied. Calling it while a run is active cancels it.
    void separateClipStems(const model::ClipLocation& location);
    void cropClipSample(const model::ClipLocation& location);
    void consolidateSelection();
    void renderTrack(int track, bool freeze, bool toNewTrack = false);
    void convertAudioToMidi(const model::ClipLocation&, int mode);
    void stripSilence(const model::ClipLocation&);      // TRK-3
    void captureAudio();                                // TRK-4: retroactive audio recording of the armed audio tracks
    std::vector<int> audioCaptureTargets() const;
    void jumpToTransient(int direction);               // TRK-3: Option+Left/Right in the Arrangement
    std::map<int, std::pair<int, std::vector<double>>> transientBeats_; // clip uid -> (edit version, onsets in song beats)
    void resynthesizeClip(const model::ClipLocation&, double endBeat = -1); // endBeat: analyze only up to here (a cut the user accepted)
    void collectAllAndSave();
    // Asks to save unsaved changes first; quits afterwards unless the user cancels.
    void requestQuit(std::function<void()> quit);
    // COM-4: before an update restarts the app. Offers to save unsaved changes; done(true) means the app may quit now and
    // the quit that follows does not ask again; done(false) keeps it running (cancelled, save failed, recording, rendering).
    void prepareForRestart(std::function<void(bool)> done);
    bool isUntitled() const;
    // In a collab session undo is per person and runs through the session (AppCollab.cpp).
    bool canUndo() const;
    bool canRedo() const;
    juce::String undoLabel() const;
    juce::String redoLabel() const;

    // ---- COL-2..6 collab sessions (AppCollab.cpp)
    void shareSession(bool lan);
    void joinSessionDialog();
    void joinSession(const std::string& inviteLink);
    void joinNearbySession(const std::string& address);
    void leaveSession();
    bool inCollabSession() const { return collab_ && collab_->state() != collab::CollabSession::State::Ended; }
    bool collabGuest() const { return collab_ && ! collab_->isHost(); }
    // COL-9 chat: "" when the line went out, else why not. The window says when its chat dock opens or closes.
    std::string sendCollabChat(const std::string& text);
    void setCollabChatOpen(bool open);
    std::function<void()> onCollabChatChanged; // the session's lines, policy or availability changed

    // ---- Export Audio (AppExport.cpp): asks for the destination, then renders offline on a worker while the dialog shows progress.
    void startExport(const ui::ExportSettings& settings);
    void cancelExport();
    bool exporting() const { return exporting_; }
    // REQ-3a: one persisted application setting (the user settings file), e.g. "keyboardShortcuts".
    std::string setting(const std::string& key) { return settings().getValue(juce::String::fromUTF8(key.c_str())).toStdString(); }
    void storeSetting(const std::string& key, const std::string& value)
    {
        settings().setValue(juce::String::fromUTF8(key.c_str()), juce::String::fromUTF8(value.c_str()));
        settings().saveIfNeeded();
    }

    void saveSettings();
    void saveLayout();
    void loadLayout();
    const juce::RecentlyOpenedFilesList& recentSets() const { return recentSets_; }
    void clearRecentSets(bool unavailableOnly = false);

private:
    class CollabPort;
    friend class CollabPort;
    void initialiseCollab();
    void startCollabAutotest(const std::string& spec);
    void pollCollab();
    void applyCollabObjects(const std::vector<collab::ObjectState>&);
    void installCollabSong(std::unique_ptr<model::Song>);
    void collabAssetsArrived(const std::vector<std::string>& hashes);
    void pollLiveValues(bool plugins);
    void applyLiveValues(const std::vector<collab::LiveValue>&);
    void pollPluginStates();
    void applyCollabPluginState(const collab::PluginStateMsg&);
    void applyCollabTransport(const collab::TransportMsg&, int64_t localTimeMs);
    std::string performCollabDrop(collab::AuthorId author, const juce::File& file, const collab::DropRequestMsg&);
    void refreshCollabView();
    void collabNotice(const std::string& text);
    void collabEnded(const std::string& reason);
    void collabChatArrived(const collab::ChatEntry&);
    void collabVoiceArrived(collab::AuthorId speaker, const std::string& rtp);
    void setCollabMic(bool on);
    void pollVoice();
    void collabVideoArrived(collab::AuthorId speaker, const std::string& rtp);
    void setCollabCamera(bool on);
    void pollVideo();
    void refreshCollabVideo();
    void startCollab(std::unique_ptr<collab::CollabSession>);
    void endCollabDocument(); // a new document replaces the session's
    void saveCollabCopy();
    std::string collabName();
    int controlIndexOfTrack(int trackId) const;
    bool pluginEditorOpen(const juce::AudioPluginInstance*) const;
    std::unique_ptr<CollabPort> collabPort_;
    std::unique_ptr<collab::CollabSession> collab_;
    std::unique_ptr<collab::VoiceChannel> voice_; // COL-7: while the session has voice; reset before engine_
    bool collabMicOn_ = false, pushToTalkHeld_ = false;
    std::set<collab::AuthorId> mutedVoices_;      // people this machine does not hear
    std::unique_ptr<collab::VideoChannel> video_; // COL-8: while the session has video
    bool collabCameraOn_ = false;
    uint64_t videoSerial_ = 0; // sum of the shown frames' serials: repaint when it moves
    std::unique_ptr<collab::LanBrowser> lanBrowser_;
    struct CollabTimer : juce::Timer
    {
        explicit CollabTimer(AppController& owner) : owner_(owner) {}
        void timerCallback() override { owner_.pollCollab(); }
        AppController& owner_;
    } collabTimer_{*this};
    std::map<std::tuple<int, int, int>, float> liveSent_; // (track id, device id or -1, target) -> last value sent or received
    std::map<int, uint64_t> pluginStateHash_;             // device id -> hash of the last state sent or received
    int collabTicks_ = 0;
    bool copyInviteWhenReady_ = false; // internet share: put the invite on the clipboard once the relay answers
    collab::AuthorId collabAuthorForNextEdit_ = collab::hostAuthor; // host: a guest's dropped file
    juce::uint32 collabAttributionSetMs_ = 0;
    bool installingCollabSong_ = false;
    bool collabAutotest_ = false; // SONORA_COLLAB_AUTOTEST: chat lines are logged

    void initialiseControl();
    control::State controlState();
    std::unique_ptr<control::ControlService> control_;
    control::LocalEndpoint controlEndpoint_;
    std::string controlDocumentId_;
    uint64_t controlGrantGeneration_ = 0;
    juce::RecentlyOpenedFilesList recentSets_;
    juce::Component::SafePointer<juce::DialogWindow> audioSettingsWindow_;
    void rememberSet();
    // `offer`, when set with an error, replaces the failure panel: the job closes and offer() runs (e.g. a prompt
    // suggesting a shorter range that would succeed).
    struct MediaEditResult { model::EditCommand edit; std::string error; int selectTrack = -1; std::function<void()> offer; };
    void runMediaEdit(std::string label, bool offline,
                      std::function<MediaEditResult(const model::Song&)> worker);
    void beginExport(engine::ExportOptions options);
    void finishExport(const engine::ExportResult& result);
    void loadExportSettings();
    void saveExportSettings();
    void addStemTracks(const model::Clip& original, const std::vector<engine::StemFile>& stems, const juce::File& projectDirectory);
    bool stemsRunning_ = false;
    int stemsBusy_ = 0; // UiContext busy task while separating
    // UI-51: opening a set starts its plug-ins one per message-loop turn before installSong().
    void preloadPluginsThenInstall(std::shared_ptr<io::AlsLoadResult> result, std::shared_ptr<std::vector<const model::Device*>> plugins,
                                   size_t index, int generation, int busy, std::string name);
    int openGeneration_ = 0;
    std::atomic<bool> stemsCancel_{false};
    bool exporting_ = false;
    // COM-4: when prepareForRestart resolved unsaved changes for the update's quit (ms counter, 0 = never). Honoured for
    // one minute only, so a relaunch that never happens cannot skip the save prompt of a later, ordinary quit.
    juce::uint32 restartApprovedAt_ = 0;
    std::atomic<float> exportProgress_{0.0f};
    std::atomic<bool> exportCancel_{false};
    std::unique_ptr<juce::FileChooser> exportChooser_;
    struct ExportPoller : juce::Timer
    {
        explicit ExportPoller(AppController& owner) : owner_(owner) {}
        void timerCallback() override;
        AppController& owner_;
    } exportPoller_{*this};

    void timerCallback() override;
    void installSong(std::unique_ptr<model::Song> song);
    void closePluginWindows();
    void openWavetableWindow(const ui::UiContext::WavetableKey&);
    void closeWavetableWindow(const ui::UiContext::WavetableKey&);
    void closeWavetableWindows();
    void followWavetableIds(); // device edits give track-level devices fresh ids: placements and windows follow
    void openHushWindow(const ui::UiContext::WavetableKey&); // PLG-14g (AppHushWindow.cpp)
    void closeHushWindows();
    void syncHushWindows();
    void renameHushWindows(model::TrackId track, int oldId, int newId);
    void openInsertedWavetable(model::TrackId track, int deviceId, int childIdFrom);
    std::map<model::TrackId, std::vector<int>> deviceIdsBeforeEdit_; // ui::DeviceIdsByTrack
    void refreshPluginNames();
    midi::DeviceOrder deviceOrder_;
    void editDevice(int track, int device, const std::function<void(model::Device&)>& edit, const std::string& label);
    void editDeviceIf(int track, int device, const std::function<bool(model::Device&)>& edit, const std::string& label);
    void importWavetable(int track, int device, int oscillator, const std::string& path, int chain, int child);
    void importOscillatorMedia(int track, int device, int oscillator, const std::string& path, int chain, int child);
    void importWavetablePreset(int track, int device, const std::string& path, int chain, int child);
    void importResynth(int track, int device, const std::string& path, int chain, int child, double limitSeconds = 0);
    // LIB-1 (AppSamplePacks.cpp): the Browser's Packs list and downloads.
    void setUpSamplePacks();
    void publishSamplePacks();
    // PLG-16 (AppNativeSwap.cpp): after a load, offer Sonora Prism / Curve LFO for missing Serum 2 / LFOTool instances.
    void offerNativeReplacements();
    void showNativeReplacementOffer(std::shared_ptr<std::vector<plugins::NativeReplacement>> found, int generation, int attempt);
    void replaceWithNativeDevices(const std::vector<plugins::NativeReplacement>& replacements);
    void createResynthPhrase(int track, int device, int chain, int child);
    void editWavetableLfo(int track, int device, int lfo, const dsp::CurveGraph&, int chain, int child);
    void editWavetableModulationCurve(int track, int device, int slot, bool auxiliary, const dsp::CurveGraph&, bool enabled, int chain, int child);
    void setWavetableEffects(int track, int device, const devices::WavetableEffects::Configuration&, int chain, int child);
    void importWavetableImpulseResponse(int track, int device, devices::WavetableEffects::ModuleId,
                                       const std::string& path, int chain, int child);
    void dropDeviceItem(const ui::BrowserDrag& item, int track, int device, int chain, int zone, int child);
    void findProjectsInBackground();
    juce::PropertiesFile& settings();
    static juce::File midiSettingsFile();
    void loadMappingsForSong();
    void saveMappings();
    void wirePreferences();
    void afterEdit(const std::vector<int>& tracks, bool songLevel, bool devicesChanged = false, bool takesOnly = false,
                   bool structureChanged = false, const std::vector<int>& gainTracks = {});
    // Track groups: tracks were inserted/removed, so the engine graph is rebuilt and per-track UI state follows track ids.
    void captureMidi();
    // Automation Arm: controls moved while recording to the Arrangement are written into their lanes.
    void startAutomationRecording();
    void pollAutomation();
    void finishAutomationRecording();
    void beginAutomationOnlyRecording();
    engine::AutomationCapture automationCapture_;
    bool automationRecording_ = false, automationOnly_ = false; // automationOnly_: recording without any armed track
    double lastAutomationBeat_ = 0.0;
    struct AutomationPoller : juce::Timer
    {
        explicit AutomationPoller(AppController& owner) : owner_(owner) {}
        void timerCallback() override { owner_.pollAutomation(); }
        AppController& owner_;
    } automationPoller_{*this};
    void toggleSessionRecording();
    midi::MidiCapture capture_;
    bool forceSessionRecording_ = false; // the circle button: the next take records into Session slots even from the Arrangement view
    void groupSelectedTracks();
    void ungroupSelectedTracks();
    void rebuildEngine();
    void remapTrackUi(const std::vector<model::TrackId>& before);
    std::vector<model::TrackId> trackIds() const;
    std::vector<model::TrackId> idsBeforeStructureEdit_;
    std::vector<model::TrackId> returnIdsBeforeStructureEdit_;
    void snapshotTrackUi();
    void beginDeviceEdit();
    bool editingDevices_ = false;
    bool syncingModel_ = false;
    std::unique_ptr<model::Song> deferredSong_;
    void queueDeferredSong();
    void dropBrowserItem(const ui::BrowserDrag&, int track, double beat, int scene);
    void dropTrackDevice(const ui::BrowserDrag&, int track); // UI-7c: a device dragged to a track (Opt copies)
    void runDeviceDragAutotest(); // SONORA_DEVICE_DRAG_AUTOTEST (UI-7c verification without driving the UI)
    // LIB-2 (AppDevicePresets.cpp): the reference library's device presets in the Browser; loading one onto a track
    // (replaceIndex >= 0: replaces that top-level device in place when the preset is of its type).
    void scanDevicePresets();
    void loadDevicePreset(const ui::BrowserDrag&, int track, int replaceIndex);
    void runPresetAutotest(); // SONORA_PRESET_AUTOTEST (LIB-2 verification without driving the UI)
    void syncModelFromEngine();
    void openBrowserFolder(const std::string& path);
    void readBrowserFolder(const std::string& path, bool root);
    void indexBrowserFolder(const std::string& path);
    void addBrowserFolder();
    int browserGeneration_ = 0; // message thread, rejects results after navigating elsewhere
    // UI-6b: the running recursive search scan stops when this moves on (new scan or another folder).
    std::shared_ptr<std::atomic<int>> browserSearchGeneration_ = std::make_shared<std::atomic<int>>(0);
    int documentGeneration_ = 0; // rejects sample drops after another set was opened
    void toggleRecording();
    void beginRecording();
    void ensureAudioInputs(bool record = false);
    int requiredInputChannels(bool record) const;
    bool inputPermissionPending_ = false, inputPermissionDenied_ = false;
    void finishRecording();
    bool writeSet(const juce::File& destination, const io::AlsSaveOptions& options);
    void createNewProject();
    void confirmDiscardChanges(std::function<void()> proceed);
    void saveThen(std::function<void(bool)> done);
    void saveAsDialog(bool collect, std::function<void(bool)> done);
    void updateWindowTitle();
    void openFileNow(const juce::File& file);
    static juce::File untitledRoot();
    static juce::File projectsRoot();
    int requestedRecordingTrack_ = -1, requestedRecordingScene_ = -1;
    int recordingScene_ = -1; // legacy result destination; scheduled takes retain exact per-track scenes
    int lastRecordingPhase_ = -1;
    bool loadingMappings_ = false;

    plugins::PluginHost host_;
    midi::MidiManager midi_;
    midi::MidiClockOut midiClock_;
    engine::AudioEngine engine_;
    std::unique_ptr<model::Song> song_;
    ui::UiContext ui_;
    model::EditHistory history_;
    engine::PeakCache peaks_{engine_.sampleCache()};
    std::set<int> warpAnalysisPending_; // message-thread IDs; bounds/merges repeated Warp clicks
    juce::ThreadPool peakPool_{1}; // declared after peaks_: its jobs finish first

    std::unique_ptr<juce::FileChooser> chooser_;
    juce::OwnedArray<juce::DocumentWindow> pluginWindows_;
    juce::OwnedArray<juce::DocumentWindow> wavetableWindows_; // UI-27, survive device edits (unlike pluginWindows_)
    juce::OwnedArray<juce::DocumentWindow> hushWindows_;      // PLG-14g, like wavetableWindows_
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withNumberOfThreads(2)};
    std::unique_ptr<juce::PropertiesFile> settings_;
    std::atomic<bool> scanning_{false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::atomic<bool> samplePacksPublishPending_{false};
    bool samplePacksFetched_ = false, samplePacksPublished_ = false;
    std::set<std::string> samplePacksInstalled_, samplePacksFailed_;
    std::unique_ptr<packs::PackStore> packStore_; // last: its worker stops before anything above goes away
};

} // namespace sonora::app
