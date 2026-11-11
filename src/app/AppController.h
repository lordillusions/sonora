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
#include "engine/SoundSimilarity.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/recovery/RecoveryStore.h"
#include "devices/DeviceFactory.h"
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
    void makeSweepFromClip(const model::ClipLocation&, bool up, double beats); // PLG-19 (AppMediaEdits.cpp)
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
    // PRJ-4 crash recovery (AppRecovery.cpp): once the first window is up, offers the newest copy a run that did not quit
    // cleanly left behind ("Sonora quit unexpectedly: recover <set>?"). Only the first call does anything.
    void offerRecovery();

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
    void openKickstractTimeline(const ui::UiContext::WavetableKey&); // PLG-17e (AppKickstractWindow.cpp)
    void closeKickstractTimelines();
    void syncKickstractTimelines();
    void renameKickstractTimelines(model::TrackId track, int oldId, int newId);
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
    // LIB-3/4/5 (AppSampleFiles.cpp): Show Similar Files, Replace Sample, similar-sample swaps.
    void setUpSampleFiles();
    void findSimilarSamples(const std::string& path, const model::ClipLocation& clip, int pendingSwap = 0);
    void swapSimilarSample(const model::ClipLocation& location, int direction);
    void replaceClipSample(const model::ClipLocation& location, const std::string& path, model::SampleScope scope);
    engine::SimilarSearch librarySearch() const; // LIB-4c: the indexed library (Places, Samples, User Library, packs, Core Library)
    void restartSoundIndex();                    // (re)builds the library sound index in the background
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
    void duplicateSelectedTracks(bool withAutomation);
    // UI-77 Automation Write mode: a capture runs whenever the transport plays; stopped, a touched control writes one point.
    void setAutomationWriteMode(bool on);
    void automationWriteTouch(const midi::ControlTarget& control, bool down);
    bool writeCapture_ = false; // the running capture belongs to Automation Write mode (not to a recording)
    // UI-77c: Automation Write mode with the transport stopped watches every control (plugin windows included) for moves:
    // the first move shows the control's lane, the lane draws the new breakpoint at once, and it is stored when the control
    // is let go (or settles, for a plugin's own window).
    engine::AutomationCapture writeWatch_;
    bool writeWatching_ = false, writeHeld_ = false;
    // A new graph, edit (or undo) or set starts the watch over.
    const engine::EngineGraph* writeWatchGraph_ = nullptr;
    const model::Song* writeWatchSong_ = nullptr;
    int writeWatchVersion_ = -1;
    struct PendingWrite
    {
        midi::ControlTarget control;
        double beat = 0;
        uint32_t changedAt = 0;
    };
    std::optional<PendingWrite> pendingWrite_;
    std::optional<std::pair<midi::ControlTarget, uint32_t>> lastWriteLane_; // the lane last shown for a move, and when
    std::optional<midi::ControlTarget> deferredWriteLane_; // Session view: the lane shows when the control is let go
    bool showWriteLane(const midi::ControlTarget&);
    std::optional<midi::ControlTarget> controlForMove(const engine::AutomationCapture::Moved&, const engine::EngineGraph&) const;
    void showMovedLanes(const std::vector<engine::AutomationCapture::Moved>&, const engine::EngineGraph&);
    void watchStoppedWrites(const engine::EngineGraph&);
    void commitPendingWrite();
    void publishCapturePreview(const engine::EngineGraph&);
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
    void runSampleDropAutotest(); // SONORA_SAMPLE_DROP_AUTOTEST (DEV-4c: a sample loaded into a Simpler, then saved)
    // LIB-2 (AppDevicePresets.cpp): the reference library's device presets in the Browser; loading one onto a track
    // (replaceIndex >= 0: replaces that top-level device in place when the preset is of its type).
    void scanDevicePresets();
    void loadDevicePreset(const ui::BrowserDrag&, int track, int replaceIndex);
    void runPresetAutotest(); // SONORA_PRESET_AUTOTEST (LIB-2 verification without driving the UI)
    // `autosave` (PRJ-4): the audio keeps running, and hosted plug-ins are asked for their state only when they reported a
    // change since the last autosave or their window is open. Save suspends the audio callback and asks every plug-in.
    void syncModelFromEngine(bool autosave = false);
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
    // UI-82: CPU meter details. Message thread.
    void updateCpuView();
    uint32_t lastOverruns_ = 0;
    double overloadUntilMs_ = 0;
    int xrunBase_ = 0;
    void finishRecording();
    bool writeSet(const juce::File& destination, const io::AlsSaveOptions& options);
    // ---- PRJ-4 crash recovery (AppRecovery.cpp)
    // Autosave: a copy of the model written to <data folder>/Recovery/<this run>/ on autosavePool_, never into the
    // user's project; polled from timerCallback, throttled by io::recovery::autosaveDue.
    void startRecovery();
    void autosaveTick();
    // Copies the model now (or writes `snapshot`, a copy a render already took) unless one is being written.
    bool autosaveNow(const char* reason, std::shared_ptr<const model::Song> snapshot = nullptr);
    bool autosavePending() const { return ui_.documentDirty && history_.version() != autosavedVersion_; }
    void recoveryDocumentInstalled(); // end of installSong: the previous document's copies go
    void recoveryDocumentSaved();     // writeSet succeeded: the document's copies go
    void openRecovered(io::recovery::Entry entry);
    void runPlayAutotest();           // SONORA_PLAY_AUTOTEST=<seconds>[,beat]: play, log engine load, quit
    bool playAutotestDone_ = false;
    void runAutosaveAutotest();       // SONORA_AUTOSAVE_AUTOTEST=<bpm>: a real edit once a set is open (verification)
    io::recovery::Store recovery_{juce::File()};
    std::vector<io::recovery::Entry> recoveryOffers_;
    std::unique_ptr<io::recovery::Entry> pendingRecovery_; // set while its copy is being opened
    bool recoveryOffered_ = false, autosaveInFlight_ = false, autosaveHasCopy_ = false, autosaveAutotestDone_ = false;
    uint64_t autosaveEpoch_ = 0;      // bumped when the document is saved or replaced: a copy still being written is dropped
    int autosavedVersion_ = 0, autosaveSeenVersion_ = 0; // EditHistory::version() in the last copy / last seen
    double autosaveLastChangeAt_ = 0, autosavePendingSince_ = 0, autosaveLastAt_ = -1e12, recoveryHeartbeatAt_ = 0;
    double autosaveLastWriteMs_ = 0;
    int autosavePluginsSeen_ = 0, autosavePluginsRead_ = 0; // hosted plug-ins in the last autosave / whose state it read
    // Hosted plug-in state for the autosave: read only when the plug-in reported a change since its last read (or its
    // window is open and the last read is 10 s old), and spread over the message loop, one plug-in per turn, while an
    // autosave is pending, so the autosave itself rarely has one left to read.
    struct PluginCapture { uint64_t serial = 0; double at = 0; };
    std::map<const plugins::PluginDevice*, PluginCapture> autosavePlugins_; // stateSerial() and time of each one's last read
    bool pluginNeedsCapture(plugins::PluginDevice& plugin, double now);
    void forEachHostedPlugin(const std::function<bool(model::Device&, plugins::PluginDevice&)>& fn); // false stops
    void refreshOnePluginState();
    double pluginRefreshAt_ = 0;
    int pluginsRefreshed_ = 0;      // since the last autosave
    double pluginRefreshMaxMs_ = 0; // the slowest of those reads
    void createNewProject();
    void confirmDiscardChanges(std::function<void()> proceed);
    void saveThen(std::function<void(bool)> done);
    void saveAsDialog(bool collect, std::function<void(bool)> done);
    void updateWindowTitle();
    void openFileNow(const juce::File& file);
    void openDocument(std::string name, std::function<io::AlsLoadResult()> load); // load runs on pool_
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
    juce::OwnedArray<juce::DocumentWindow> kickstractTimelines_; // PLG-17e, like hushWindows_
    // 16 MB stacks: offline renders (bounce, freeze, export) run whole devices on these threads; the default 512 KB overflowed.
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withNumberOfThreads(2).withThreadStackSizeBytes(16u * 1024 * 1024)};
    // PRJ-4: autosave copies are written here, at background priority, so they never wait behind an open or a render.
    juce::ThreadPool autosavePool_{juce::ThreadPoolOptions{}.withThreadName("Sonora autosave").withNumberOfThreads(1)
                                       .withThreadStackSizeBytes(4u * 1024 * 1024).withDesiredThreadPriority(juce::Thread::Priority::background)};
    std::unique_ptr<juce::PropertiesFile> settings_;
    std::atomic<bool> scanning_{false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::atomic<bool> samplePacksPublishPending_{false};
    bool samplePacksFetched_ = false, samplePacksPublished_ = false;
    std::set<std::string> samplePacksInstalled_, samplePacksFailed_;
    std::unique_ptr<packs::PackStore> packStore_; // its worker stops before anything above goes away
    // LIB-4: Show Similar Files decodes many files; its own thread keeps the browser's folder reads (pool_) responsive.
    std::shared_ptr<engine::SoundFeatureCache> soundFeatures_;
    std::shared_ptr<std::atomic<int>> similarGeneration_ = std::make_shared<std::atomic<int>>(0);
    juce::ThreadPool similarPool_{1}; // its job ends before the cache and generation above go away
    // LIB-4c: the library sound index, one low-priority thread that sleeps between files. Not `background`: macOS throttles
    // that class's disk reads so hard that a library took over half an hour (448 files in 113 s, measured).
    std::shared_ptr<std::atomic<int>> soundIndexGeneration_ = std::make_shared<std::atomic<int>>(0);
    juce::ThreadPool soundIndexPool_{juce::ThreadPoolOptions{}.withThreadName("Sonora sound index").withNumberOfThreads(1)
                                         .withDesiredThreadPriority(juce::Thread::Priority::low)}; // last: ends first
};

} // namespace sonora::app
