#include "app/MainWindow.h"
#include "util/Log.h"
#include "ui/TrackMenu.h"
#include "app/WindowChrome.h"
#include "app/AboutWindow.h"

#include "ui/GridMenu.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/ClipControls.h"
#include "ui/views/TopBar.h"
#include "ui/views/SessionView.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/DetailView.h"
#include "engine/ClipLauncher.h"

#include <cctype>
#include <cstdlib>

namespace sonora::app
{

namespace
{
enum MenuIds
{
    NewProject = 1,
    OpenFile,
    SaveFile,
    SaveFileAs,
    SaveCopy,
    CollectAllAndSave,
    ExportAudio,
    ExportMidiClip,
    Undo,
    Redo,
    DuplicateClip, SplitClip,
    DeleteClip,
    Preferences,
    AudioSettings,
    ScanVst3,
    ScanVst3AndAu,
    LoadTheme,
    EnableMidi,
    ShowSession,
    ShowArrangement,
    ToggleBrowser,
    ToggleDetail,
    ToggleTakeLanes,
    Play,
    Stop,
    Record,
    StopAllClips,
    MidiClipMap,
    KeyClipMap,
    DeleteDevice,
    LoopSelection,
    GroupTracks,
    UngroupTracks,
    InsertMidiClip,
    NarrowGrid,
    WidenGrid,
    TripletGrid,
    SnapToGrid,
    FixedGrid,
    ZoomInTime,
    ZoomOutTime,
    ToggleAssistant,
    CloseWindow, ClearRecent, RemoveUnavailable, AddFolder, ShowBackups, RevealBackups,
    SelectAll, ToggleClipActive, SoloTrack, ArmTrack, Quantize, QuantizeSettings, ApplyMidiTool,
    InsertAudioTrack, InsertMidiTrack, InsertReturnTrack, CaptureMidi, CaptureAudio, SeparateStems,
    ContinuePlayback, ReturnToStart, SessionRecord, Metronome, BackToArrangement,
    ToggleViews, ShowClip, ShowDevices, ToggleClipDevice, SearchBrowser,
    ToggleMixer, ToggleIo, ToggleSends, ToggleAutomation, HideGroupedTracks,
    ZoomUiIn, ZoomUiOut, ZoomUiReset, FullScreen, ComputerKeyboard, BrowserPreview, DrawMode, Overdub,
    SmallerQuant, LargerQuant, ToggleQuant,
    FocusControlBar, FocusBrowser, FocusSession, FocusArrangement, FocusClip, FocusDevices,
    ReadManual, ImportAudio, Rename, AddLocator, ContentEditor, EnvelopeEditor, ExpressionEditor, ShowGroupedTrack, TripletLaunchQuant, InvertNotes,
    FoldNotes, FoldScale, HighlightScales, PianoPreview,
    InsertTakeLane, FreezeTrack, BounceTrack, BounceNew, ConsolidateClips, ConvertMelody, ConvertHarmony, ConvertDrums, SliceMidi, ResynthesizeClip,
    DeactivateTrack, HideTrack, ShowHiddenTracks, StripSilence, NextTransient, PreviousTransient,
    CheckForUpdates, AboutSonora,
    ShareSessionLan, ShareSessionRelay, JoinSession, CollaboratePanel, LeaveSession,
    RecentBase = 1000,
};
class Workspace final : public juce::Component
{
public:
    Workspace(juce::Component* canvas, juce::Component* assistant) : canvas_(canvas), assistant_(assistant)
    { addAndMakeVisible(canvas_); addChildComponent(assistant_); }
    ~Workspace() override { deleteAllChildren(); }
    void resized() override
    {
        auto area = getLocalBounds();
        if (assistant_->isVisible()) assistant_->setBounds(area.removeFromRight(std::min(420, getWidth() / 2)));
        canvas_->setBounds(area);
    }
private:
    juce::Component* canvas_;
    juce::Component* assistant_;
};
// First widget of type T in the tree (depth first). A plain function template: MSVC 14.51 crashed (C1001) on the
// templated lambda holding a recursive std::function that this replaced.
template <typename T>
T* findWidget(ui::Widget& widget)
{
    if (auto* result = dynamic_cast<T*>(&widget)) return result;
    for (const auto& child : widget.children())
        if (auto* result = findWidget<T>(*child)) return result;
    return nullptr;
}
} // namespace

MainWindow::MainWindow(AppController& app)
    : DocumentWindow("Sonora", juce::Colour(0xff2a2a2a), DocumentWindow::allButtons), app_(app)
{
    setUsingNativeTitleBar(true);
    juce::LookAndFeel::setDefaultLookAndFeel(&menuLook_);
    layout_ = std::make_unique<ui::MainLayout>(app_.ui());

    canvas_ = new gfx::SkiaCanvasComponent(*layout_);
    app_.ui().requestRepaint = [this] { updateChrome(); if (canvas_) canvas_->requestFrame(); };
    app_.ui().modelChanged = [this] {
        if (assistant_) assistant_->refreshAutomation(app_.localControlEnabled());
        layout_->layout(); // rebuild views synchronously (drops pointers into the old graph)
        layout_->markDirty();
        app_.syncWavetableWindows(); // floating Wavetable editors too (UI-27)
        menuItemsChanged(); // native menus were initially built before a song was loaded
    };

    app_.ui().setWindowTitle = [this](const std::string& title) { setName(juce::String::fromUTF8(title.c_str())); };
    app_.forwardKeyToMainWindow = [this](const juce::KeyPress& key) { return canvas_ != nullptr && canvas_->keyPressed(key); };
    app_.ui().insertMidiClip = [this] { menuItemSelected(InsertMidiClip, 2); };
    app_.ui().toggleFullScreen = [this] { setFullScreen(! isFullScreen()); };
    assistant_ = new AssistantDock();
    assistant_->setControlService(app_.control());
    assistant_->onActivity = [this] { return app_.localControlActivity(); };
    assistant_->onFocus = [this] { layout_->releaseMappedKeys(); };
    assistant_->onAutomation = [this](bool enabled) {
        const auto result = app_.enableLocalControl(enabled);
        assistant_->refreshAutomation(app_.localControlEnabled());
        return result.ok ? juce::String(enabled ? "Local MCP control enabled for this project." : "Local MCP control disabled.")
                         : juce::String(result.message);
    };
    app_.onControlActivity = [this](const juce::String& operation, const juce::var& result) {
        if (assistant_) assistant_->showResult("MCP", operation, result);
    };
    setContentOwned(new Workspace(canvas_, assistant_), false);
    setResizable(true, false);
    setResizeLimits(900, 560, 10000, 10000);
    centreWithSize(1440, 900);
    setVisible(true);
    updateChrome();
    canvas_->grabKeyboardFocus();

    // The registry reads every menu, so it is built once the window (assistant dock, layout) exists.
    buildShortcutRegistry();
    shortcuts_.fromJson(app_.setting("keyboardShortcuts"));
    shortcutsReady_ = true;
    app_.ui().shortcuts = &shortcuts_;
    app_.ui().userShortcut = [this](const ui::KeyEvent& k) {
        const int id = shortcuts_.resolve(ui::KeyCombo::fromEvent(k));
        if (id == 0) return false;
        if (id > 0 && commandEnabled(id)) menuItemSelected(id, commandMenu_[id]);
        return true; // a released default key, or a disabled command, does nothing
    };
    app_.ui().shortcutsChanged = [this] {
        app_.storeSetting("keyboardShortcuts", shortcuts_.toJson());
        menuItemsChanged();
    };
    createUpdater();
#if JUCE_MAC
    juce::PopupMenu applicationMenu;
    applicationMenu.addItem(AboutSonora, "About Sonora");
    applicationMenu.addSeparator();
    applicationMenu.addItem(CheckForUpdates, "Check for Updates...", updater_->canCheckNow() || updater_->installPending());
    applicationMenu.addSeparator();
    applicationMenu.addItem(Preferences, "Settings...    (Cmd+,)");
    applicationMenu.addSeparator();
    juce::MenuBarModel::setMacMainMenu(this, &applicationMenu);
#else
    setMenuBar(this); // COM-7: Windows/Linux show the menus inside the window
#endif
    app_.ui().statusText += canvas_->usingGpu() ? "  |  Skia/Metal" : "  |  Skia/raster";
}

MainWindow::~MainWindow()
{
    app_.ui().updateSettings = nullptr;
    app_.ui().checkForUpdates = nullptr;
    app_.ui().setAutomaticUpdateChecks = nullptr;
    app_.ui().setUpdateChannel = nullptr;
    updater_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    app_.onControlActivity = nullptr;
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    app_.ui().requestRepaint = nullptr;
    app_.ui().setWindowTitle = nullptr;
    app_.forwardKeyToMainWindow = nullptr;
    app_.ui().insertMidiClip = nullptr;
    app_.ui().toggleFullScreen = nullptr;
    app_.ui().shortcuts = nullptr;
    app_.ui().userShortcut = nullptr;
    app_.ui().shortcutsChanged = nullptr;
    app_.ui().modelChanged = nullptr;
    clearContentComponent(); // canvas references layout_, destroy it first
    canvas_ = nullptr;
    assistant_ = nullptr;
}

// COM-4: the platform updater (Sparkle on macOS) asks the session before it restarts; Settings > Updates drives it.
void MainWindow::createUpdater()
{
    Updater::Host host;
    host.session = [this] {
        auto& ui = app_.ui();
        UpdateSessionState state;
        state.recording = ui.recording || ui.recordingRequested;
        state.exporting = app_.exporting();
        state.playing = ui.transport != nullptr && (ui.transport->isPlaying() || ui.transport->isCountingIn());
        state.dirty = ui.documentDirty;
        return state;
    };
    host.prepareForRestart = [this](std::function<void(bool)> done) { app_.prepareForRestart(std::move(done)); };
    host.status = [this](const std::string& text) {
        app_.ui().statusText = text;
        if (app_.ui().requestRepaint) app_.ui().requestRepaint();
    };
    host.changed = [this] {
        menuItemsChanged();
        if (app_.ui().requestRepaint) app_.ui().requestRepaint();
    };
    host.loadSetting = [this](const std::string& key) { return app_.setting(key); };
    host.storeSetting = [this](const std::string& key, const std::string& value) { app_.storeSetting(key, value); };
    updater_ = Updater::create(std::move(host));

    auto& ui = app_.ui();
    ui.updateSettings = [this] {
        ui::UiContext::UpdateSettings state;
        state.available = updater_->available();
        state.reason = updater_->unavailableReason();
        state.version = updater_->currentVersion();
        state.backend = updater_->backendName();
        state.automatic = updater_->automaticChecks();
        state.canCheck = updater_->canCheckNow();
        state.pending = updater_->installPending();
        const auto channels = updater_->channels();
        for (size_t i = 0; i < channels.size(); ++i)
        {
            const std::string name = channelName(channels[i]);
            state.channels.push_back((char) std::toupper((unsigned char) name[0]) + name.substr(1));
            if (channels[i] == updater_->channel()) state.channel = (int) i;
        }
        return state;
    };
#if SONORA_UPDATE_LOCAL_TEST
    // docs/UPDATES.md local test builds only: SONORA_UPDATE_AUTOTEST=busy plays for 20 s, so the update has to wait.
    if (const char* mode = std::getenv("SONORA_UPDATE_AUTOTEST"); mode != nullptr && std::string(mode) == "busy")
        juce::Timer::callAfterDelay(2000, [this] {
            if (app_.ui().transport == nullptr) return;
            app_.ui().transport->play();
            logf("updates: autotest, playing for 20 s");
            juce::Timer::callAfterDelay(20000, [this] {
                if (app_.ui().transport != nullptr) app_.ui().transport->stop();
                logf("updates: autotest, stopped");
            });
        });
#endif
    ui.checkForUpdates = [this] { updater_->checkNow(); };
    ui.setAutomaticUpdateChecks = [this](bool enabled) { updater_->setAutomaticChecks(enabled); };
    ui.setUpdateChannel = [this](int index) {
        const auto channels = updater_->channels();
        if (index >= 0 && (size_t) index < channels.size()) updater_->setChannel(channels[(size_t) index]);
    };
}

void MainWindow::updateChrome()
{
    if (! getPeer()) return;
    const auto colour = juce::Colour(app_.ui().theme.color("Desktop"));
    menuLook_.configure(app_.ui().theme, app_.ui().uiZoom);
    if (colour == chromeColour_) return;
    chromeColour_ = colour;
    blendWindowTitleBar(*this, colour);
}

void MainWindow::toggleAssistant()
{
    assistant_->setVisible(! assistant_->isVisible());
    getContentComponent()->resized();
    if (assistant_->isVisible()) assistant_->focusEditor(); else canvas_->grabKeyboardFocus();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

juce::StringArray MainWindow::getMenuBarNames()
{
    return {"File", "Edit", "Create", "Playback", "View", "Navigate", "Options", "Help"};
}

juce::PopupMenu MainWindow::getMenuForIndex(int index, const juce::String&)
{
    juce::PopupMenu m;
    auto& ui = app_.ui();
    const bool loaded = ui.song != nullptr;
    const bool hasClip = ui.selectedClip() != nullptr;
    const bool noteEditor = dynamic_cast<ui::PianoRoll*>(layout_->keyboardFocus()) != nullptr;
    std::function<bool(const ui::Widget&)> activeTool = [&](const ui::Widget& widget) {
        if (! widget.visible) return false;
        if (auto* panel = dynamic_cast<const ui::ClipControls*>(&widget)) return panel->toolPanelOpen();
        for (const auto& child : widget.children()) if (activeTool(*child)) return true;
        return false;
    };
    const bool toolReady = ui.detailVisible && ui.detailTab == ui::DetailTab::Clip && activeTool(*layout_);
    bool trackContext = true;
    for (auto* focus = layout_->keyboardFocus(); focus; focus = focus->parent())
        if (dynamic_cast<ui::DetailView*>(focus) || dynamic_cast<ui::BrowserPanel*>(focus)) trackContext = false;
    auto unavailable = [](juce::PopupMenu& menu, std::initializer_list<const char*> labels) {
        for (const auto* label : labels) menu.addItem(juce::PopupMenu::Item(label).setEnabled(false));
    };
    if (index == 0)
    {
        m.addItem(NewProject, "New Set    (Cmd+N)");
        m.addItem(OpenFile, "Open Set...    (Cmd+O)");
        juce::PopupMenu recent;
        for (int i = 0; i < app_.recentSets().getNumFiles(); ++i)
        {
            const auto file = app_.recentSets().getFile(i);
            auto found = std::find(recentMenuFiles_.begin(), recentMenuFiles_.end(), file);
            if (found == recentMenuFiles_.end()) { recentMenuFiles_.push_back(file); found = recentMenuFiles_.end() - 1; }
            const int menuId = RecentBase + (int) (found - recentMenuFiles_.begin());
            recent.addItem(menuId, file.getFileName() + "  [" + file.getParentDirectory().getFileName() + "]", file.existsAsFile());
        }
        if (app_.recentSets().getNumFiles() == 0) unavailable(recent, {"No Recent Sets"});
        recent.addSeparator();
        recent.addItem(RemoveUnavailable, "Remove Unavailable Sets", app_.recentSets().getNumFiles() > 0);
        recent.addItem(ClearRecent, "Clear List", app_.recentSets().getNumFiles() > 0);
        m.addSubMenu("Open Recent Set", recent);
        m.addItem(CloseWindow, "Close Window    (Cmd+W)");
        m.addSeparator();
        unavailable(m, {"Install Pack..."});
        m.addItem(AddFolder, "Add Folder to Browser...");
        unavailable(m, {"Manage Files"});
        const auto backup = loaded ? juce::File(ui.song->sourcePath).getParentDirectory().getChildFile("Backup") : juce::File();
        juce::PopupMenu backups;
        backups.addItem(ShowBackups, "Show Backups in Browser", backup.isDirectory());
        backups.addItem(RevealBackups, "Reveal Backups in Finder", backup.isDirectory());
        m.addSubMenu("Open Set Backups", backups);
        m.addSeparator();
        m.addItem(SaveFile, "Save Set    (Cmd+S)", loaded);
        m.addItem(SaveFileAs, "Save Set As...    (Shift+Cmd+S)", loaded);
        m.addItem(SaveCopy, "Save a Copy...", loaded);
        m.addItem(CollectAllAndSave, "Collect All and Save", loaded);
        unavailable(m, {"Save Set As Template...", "Save Set As Default Set..."});
        m.addSeparator();
        m.addItem(ExportAudio, "Export Audio...    (Shift+Cmd+R)", loaded && ! app_.exporting());
        const auto* clip = ui.selectedClip();
        m.addItem(ExportMidiClip, "Export MIDI Clip...", loaded && clip != nullptr && clip->kind == model::Clip::Kind::Midi);
        // COL-5: collab sessions.
        m.addSeparator();
        const bool inSession = app_.inCollabSession();
        m.addItem(ShareSessionLan, "Share Session on Local Network", loaded && ! inSession);
        m.addItem(ShareSessionRelay, "Share Session through Relay", loaded && ! inSession);
        m.addItem(JoinSession, "Join Session...", ! inSession);
        m.addItem(CollaboratePanel, "Collaborate...", true, ui.collabOpen);
        m.addItem(LeaveSession, app_.collabGuest() ? "Leave Session" : "End Session", inSession);
    }
    else if (index == 1)
    {
        const bool canUndo = ui.canUndo && ui.canUndo(), canRedo = ui.canRedo && ui.canRedo();
        m.addItem(Undo, canUndo ? "Undo " + juce::String(ui.undoLabel()) + "    (Cmd+Z)" : juce::String("Nothing to Undo"), canUndo);
        m.addItem(Redo, canRedo ? "Redo " + juce::String(ui.redoLabel()) + "    (Shift+Cmd+Z)" : juce::String("Nothing to Redo"), canRedo);
        m.addSeparator();
        unavailable(m, {"Cut", "Copy", "Paste"});
        m.addItem(DuplicateClip, "Duplicate    (Cmd+D)", hasClip || (loaded && ui.mainView == ui::MainView::Arrangement && ui.arrangementSelection.active)
                  || (loaded && trackContext && ui.selectedTrack < ui.numTracks()));
        m.addItem(DeleteClip, "Delete    (Delete)", hasClip || ui.selectedDeviceId >= 0
                  || (loaded && trackContext && ui.sessionTrackSelection && ui.selectedTrack >= 0
                      && ui.selectedTrack < ui.numTracks() + ui.numReturns()
                      && ! ui.recording && ! ui.recordingRequested));
        m.addItem(SplitClip, "Split Clip    (Cmd+E)", loaded && ui.mainView == ui::MainView::Arrangement && ! ui.clipEditorFocused && ! ui.recording && ! ui.recordingRequested);
        m.addItem(ToggleClipActive, "Activate/Deactivate Clip(s)    (0)", hasClip);
        m.addItem(SelectAll, "Select All    (Cmd+A)", loaded);
        m.addItem(InvertNotes, "Invert Selection    (Shift+Cmd+A)", noteEditor);
        unavailable(m, {"Select Loop"});
        m.addItem(LoopSelection, "Loop Selection    (Cmd+L)", hasClip || (loaded && ui.arrangementSelection.active));
        juce::PopupMenu markers;
        unavailable(markers, {"Set Start Marker", "Set Loop Start", "Set Loop End", "Set End Marker"});
        m.addSubMenu("Clip Markers", markers);
        m.addSeparator();
        std::vector<int> members;
        m.addItem(GroupTracks, "Group Tracks    (Cmd+G)", loaded && trackContext && ! ui.recording && ! ui.recordingRequested && model::groupableSelection(*ui.song, ui.selectedTrackIndices(), members));
        const auto* track = ui.trackAt(ui.selectedTrack);
        m.addItem(UngroupTracks, "Ungroup Tracks    (Shift+Cmd+G)", trackContext && track && track->kind == model::TrackKind::Group && ui.selectedTrack < ui.numTracks());
        unavailable(m, {"Compare: Switch to B", "Compare: Copy A to B", "Cut Time", "Copy Time", "Paste Time", "Duplicate Time", "Delete Time"});
        m.addItem(ApplyMidiTool, "Apply Transformation/Generator    (Cmd+Enter)", hasClip && toolReady);
        m.addItem(Rename, "Rename    (Cmd+R)", hasClip || (track && track->kind != model::TrackKind::Main));
        unavailable(m, {"Edit Info Text"});
        m.addItem(SoloTrack, "Solo Track", ui.controls && track && ui.selectedTrack < ui.numTracks() + ui.numReturns());
        m.addItem(ArmTrack, "Arm Track", track && (track->kind == model::TrackKind::Midi || track->kind == model::TrackKind::Audio));
        const bool renderable = loaded && track && ui.selectedTrack >= 0 && ui.selectedTrack < ui.numTracks() && ! ui.recording && ! ui.recordingRequested;
        m.addItem(FreezeTrack, track && track->frozen ? "Unfreeze Track" : "Freeze Track    (Opt+Shift+Cmd+F)", renderable && track->kind != model::TrackKind::Group);
        m.addItem(BounceTrack, track && track->kind == model::TrackKind::Group ? "Bounce Group in Place" : "Bounce Track in Place", renderable);
        const bool flaggable = loaded && track && track->kind != model::TrackKind::Main && ! ui.recording && ! ui.recordingRequested;
        m.addItem(DeactivateTrack, track && track->deactivated ? "Activate Track" : "Deactivate Track", flaggable);
        m.addItem(HideTrack, track && track->hidden ? "Show Track" : "Hide Track", flaggable);
        m.addItem(BounceNew, "Bounce to New Track    (Cmd+B)", renderable);
        unavailable(m, {"Paste Bounced Audio", "Remove Stop Button"});
        m.addItem(ConsolidateClips, "Consolidate    (Cmd+J)", loaded && ui.mainView == ui::MainView::Arrangement && ! ui.clipEditorFocused && (hasClip || ui.arrangementSelection.active));
        const auto* clipForStrip = hasClip ? ui.selectedClip() : nullptr;
        m.addItem(StripSilence, "Strip Silence", loaded && clipForStrip && clipForStrip->kind == model::Clip::Kind::Audio
                  && ui.selectedClipScene < 0 && ui.selectedTakeLane < 0 && ! ui.recording && ! ui.recordingRequested);
        m.addItem(NextTransient, "Next Transient    (Opt+Right)", loaded && ui.mainView == ui::MainView::Arrangement);
        m.addItem(PreviousTransient, "Previous Transient    (Opt+Left)", loaded && ui.mainView == ui::MainView::Arrangement);
        unavailable(m, {"Fit to Time Range"});
        m.addItem(Quantize, "Quantize    (Cmd+U)", noteEditor && hasClip);
        m.addItem(QuantizeSettings, "Quantize Settings...    (Shift+Cmd+U)", noteEditor && hasClip);
        unavailable(m, {"Extract Groove(s)", "Record Quantization", "Simplify Envelope"});
    }
    else if (index == 2)
    {
        m.addItem(InsertAudioTrack, "Insert Audio Track    (Cmd+T)", loaded && ! ui.recording);
        m.addItem(InsertMidiTrack, "Insert MIDI Track    (Shift+Cmd+T)", loaded && ! ui.recording);
        m.addItem(InsertReturnTrack, "Insert Return Track    (Opt+Cmd+T)", loaded && ! ui.recording && ! ui.recordingRequested
                  && ui.song->returns.size() < model::maxReturnTracks);
        m.addItem(InsertTakeLane, "Insert Take Lane    (Opt+Shift+T)", loaded && ui.selectedTrack >= 0 && ui.selectedTrack < ui.numTracks()
                  && ui.trackAt(ui.selectedTrack)->kind != model::TrackKind::Group && ! ui.recording && ! ui.recordingRequested);
        unavailable(m, {"Insert Scene", "Capture and Insert Scene", "Consolidate Time to New Scene"});
        const auto* track = ui.trackAt(ui.selectedTrack);
        const bool emptySlot = ui.mainView != ui::MainView::Session || (track && ui.selectedSessionScene >= 0
            && (size_t) ui.selectedSessionScene < ui.song->scenes.size()
            && ((size_t) ui.selectedSessionScene >= track->slots.size() || ! track->slots[(size_t) ui.selectedSessionScene].clip));
        m.addItem(InsertMidiClip, "Insert Empty MIDI Clip    (Shift+Cmd+M)", track && track->kind == model::TrackKind::Midi && emptySlot && ! ui.recording);
        m.addItem(CaptureMidi, "Capture MIDI", ui.captureMidiAvailable && ui.captureMidiAvailable());
        m.addItem(CaptureAudio, "Capture Audio", ui.captureAudioAvailable && ui.captureAudioAvailable());
        unavailable(m, {"Create Fade"});
        const bool audio = hasClip && ui.selectedClip()->kind == model::Clip::Kind::Audio && ! ui.recording && ! ui.recordingRequested;
        m.addItem(SliceMidi, "Slice to New MIDI Track", audio);
        m.addItem(ConvertHarmony, "Convert Harmony to New MIDI Track", audio);
        m.addItem(ConvertMelody, "Convert Melody to New MIDI Track", audio);
        m.addItem(ResynthesizeClip, "Resynthesize to New MIDI Track", audio);
        m.addItem(ConvertDrums, "Convert Drums to New MIDI Track", audio);
        m.addItem(SeparateStems, "Separate Stems to New Audio Tracks", hasClip && ui.selectedClip()->kind == model::Clip::Kind::Audio);
        m.addItem(AddLocator, "Add Locator", loaded);
        unavailable(m, {"Add Time Signature Change"});
        m.addItem(ImportAudio, "Import Audio File...", loaded && ! ui.recording);
    }
    else if (index == 3)
    {
        m.addItem(Play, "Play", loaded);
        m.addItem(Stop, "Stop", loaded);
        m.addItem(ContinuePlayback, "Continue Playback", loaded);
        unavailable(m, {"Play From Insert Marker"});
        m.addItem(ReturnToStart, "Return Play Position to 1.1.1", loaded);
        unavailable(m, {"Move Insert Marker To Playhead", "Follow", "Start Transport With Record", "Start Transport With Tap Tempo", "Link Start Stop Sync"});
        m.addSeparator();
        m.addItem(Record, "Record to Arrangement    (F9)", loaded);
        unavailable(m, {"Arm Recording to Arrangement"});
        m.addItem(SessionRecord, "Record to Session", loaded);
        m.addItem(Metronome, "Metronome", loaded, ui.transport && ui.transport->metronomeOn());
        unavailable(m, {"Hear Metronome Only While Recording"});
        m.addItem(BackToArrangement, "Back to Arrangement", ui.graph && engine::arrangementOverridden(*ui.graph));
        m.addItem(StopAllClips, "Stop All Clips", loaded);
    }
    else if (index == 4)
    {
        m.addItem(FullScreen, "Full Screen", true, isFullScreen());
        unavailable(m, {"Second Window"});
        juce::PopupMenu zoom;
        zoom.addItem(ZoomUiIn, "Zoom In", ui.uiZoom < 2); zoom.addItem(ZoomUiOut, "Zoom Out", ui.uiZoom > 0.8f);
        zoom.addItem(ZoomUiReset, "Reset Zoom to 100%"); m.addSubMenu("Zoom", zoom);
        unavailable(m, {"Info", "Learn View", "Pack Overview", "File Manager", "Undo History"});
        m.addSeparator();
        m.addItem(ToggleViews, "Toggle Arrangement/Session View    (Tab)");
        m.addItem(ShowSession, "Session View", true, ui.mainView == ui::MainView::Session);
        m.addItem(ShowArrangement, "Arrangement View", true, ui.mainView == ui::MainView::Arrangement);
        m.addItem(ShowClip, "Clip View", true, ui.detailVisible && ui.detailTab == ui::DetailTab::Clip);
        m.addItem(ShowDevices, "Device View", true, ui.detailVisible && ui.detailTab == ui::DetailTab::Devices);
        m.addItem(ToggleClipDevice, "Toggle Clip/Device View");
        m.addItem(ToggleDetail, "Detail View", true, ui.detailVisible);
        unavailable(m, {"Expand Clip View", "Arrange Clip View Panels Automatically", "Arrange Clip View Panels Horizontally", "Arrange Clip View Panels Vertically"});
        m.addItem(ContentEditor, "Content Editor", hasClip, ! ui.clipEnvelopesVisible && ! ui.clipMpeVisible);
        m.addItem(EnvelopeEditor, "Envelope Editor", hasClip && ui.selectedClip()->kind == model::Clip::Kind::Midi, ui.clipEnvelopesVisible && ! ui.clipMpeVisible);
        m.addItem(ExpressionEditor, "Expression Editor", hasClip && ui.selectedClip()->kind == model::Clip::Kind::Midi, ui.clipMpeVisible);
        m.addItem(ToggleBrowser, "Browser    (Cmd+Opt+B)", true, ui.browserVisible);
        m.addItem(SearchBrowser, "Search in Browser    (Cmd+F)");
        unavailable(m, {"Groove Pool", "Tuning", "Full-Height Browser", "Show Similar Files", "Show Filter View", "Plug-In Windows", "Video Window", "Show the Max for Live Window"});
        m.addItem(ToggleMixer, "Mixer", true, ui.mixerVisible);
        juce::PopupMenu controls;
        controls.addItem(ToggleIo, "In/Out    (Cmd+Opt+I)", true, ui.ioVisible);
        controls.addItem(ToggleSends, "Sends", true, ui.sendsVisible);
        controls.addItem(ToggleMixer, "Volume", true, ui.mixerVisible);
        unavailable(controls, {"Track Options", "Crossfader", "Performance Impact", "Return Tracks"});
        m.addSubMenu("Mixer Controls", controls);
        unavailable(m, {"Arrangement Track Controls", "Overview", "Scene Tempo and Time Signature"});
        m.addItem(ToggleAutomation, "Automation Mode    (A)", loaded, ui.showsAutomation(ui.selectedTrack));
        m.addItem(HideGroupedTracks, "Hide Grouped Tracks", loaded);
        int hiddenCount = 0;
        if (loaded) { for (const auto& t : ui.song->tracks) hiddenCount += t.hidden ? 1 : 0; for (const auto& t : ui.song->returns) hiddenCount += t.hidden ? 1 : 0; }
        m.addItem(ShowHiddenTracks, hiddenCount > 0 ? "Show Hidden Tracks (" + juce::String(hiddenCount) + ")" : juce::String("Show Hidden Tracks"),
                  loaded && (hiddenCount > 0 || ui.showHiddenTracks), ui.showHiddenTracks);
        m.addItem(ShowGroupedTrack, "Show Selected Grouped Track", loaded && ui.selectedTrack < ui.numTracks());
        m.addItem(FoldNotes, "Fold to Notes    (F)", hasClip && ui.selectedClip()->kind == model::Clip::Kind::Midi, ui.noteFold);
        m.addItem(FoldScale, "Fold to Scale", hasClip && ui.selectedClip()->kind == model::Clip::Kind::Midi, ui.scaleFold);
        unavailable(m, {"Show Chains", "Hide Chains", "Zoom to Time Selection", "Zoom Back from Time Selection"});
        m.addItem(ToggleTakeLanes, "Take Lanes    (Cmd+Opt+U)", loaded && ui.selectedTrack < ui.numTracks(),
                  std::find(ui.unfoldedTakeTracks.begin(), ui.unfoldedTakeTracks.end(), ui.selectedTrack) != ui.unfoldedTakeTracks.end());
        m.addItem(ZoomInTime, "Zoom In Time    (+)", loaded); m.addItem(ZoomOutTime, "Zoom Out Time    (-)", loaded);
        m.addSeparator(); m.addItem(ToggleAssistant, "Agent Control    (Cmd+Shift+P)", true, assistant_->isVisible());
    }
    else if (index == 5)
    {
        m.addItem(FocusControlBar, "Control Bar"); m.addItem(FocusSession, "Session View"); m.addItem(FocusArrangement, "Arrangement View");
        m.addItem(FocusClip, "Clip View"); m.addItem(FocusDevices, "Device View"); m.addItem(FocusBrowser, "Browser");
        unavailable(m, {"Groove Pool", "Learn View", "Tuning", "Mixer", "Clip Panels", "Previous", "Next", "Previous Neighbor", "Next Neighbor", "Browser History Back", "Browser History Forward", "Use Tab Key to Move Focus", "Wrap Keyboard Navigation"});
    }
    else if (index == 6)
    {
        unavailable(m, {"Accessibility"});
        m.addItem(MidiClipMap, "Edit MIDI Map    (Cmd+M)", loaded, ui.midiMapMode);
        m.addItem(KeyClipMap, "Edit Key Map    (Cmd+K)", loaded, ui.keyMapMode);
        m.addItem(ComputerKeyboard, "Computer MIDI Keyboard    (M)", true, ui.computerKeyboard);
        m.addItem(PianoPreview, "MIDI Note Editor Preview", true, ui.pianoPreview);
        m.addItem(BrowserPreview, "Browser File Preview", true, ui.browserPreviewEnabled);
        unavailable(m, {"External Sync", "Delay Compensation", "Reduced Latency When Monitoring", "Time Ruler Format"});
        m.addItem(SmallerQuant, "Smaller Launch Quantization", loaded);
        m.addItem(LargerQuant, "Larger Launch Quantization", loaded);
        m.addItem(TripletLaunchQuant, "Triplet Launch Quantization", loaded && ui.song->globalQuantisation >= 5 && ui.song->globalQuantisation <= 12,
                  loaded && ui.song->globalQuantisation >= 5 && ui.song->globalQuantisation <= 12 && ui.song->globalQuantisation % 2 == 0);
        m.addItem(ToggleQuant, "Launch Quantization Enabled", loaded, loaded && ui.song->globalQuantisation != 0);
        unavailable(m, {"Snap Automation to Grid"});
        const auto& grid = ui.activeGrid();
        m.addItem(NarrowGrid, "Narrow Grid    (Cmd+1)", loaded); m.addItem(WidenGrid, "Widen Grid    (Cmd+2)", loaded);
        m.addItem(TripletGrid, "Triplet Grid    (Cmd+3)", loaded, grid.triplet);
        m.addItem(SnapToGrid, "Snap to Grid    (Cmd+4)", loaded, grid.snap);
        m.addItem(FixedGrid, "Fixed Grid    (Cmd+5)", loaded, grid.fixed);
        m.addItem(DrawMode, "Draw Mode    (B)", true, ui.drawMode);
        unavailable(m, {"Step Input Mode"});
        m.addItem(HighlightScales, "Highlight Scales    (K)", true, ui.scaleHighlight);
        unavailable(m, {"Chase MIDI Notes", "MIDI Envelope Auto-Reset", "Retune Set On Loading Tuning Systems", "Solo Switches", "Cue Switches", "Solo in Place", "Lock Envelopes"});
        m.addItem(Overdub, "MIDI Arrangement Overdub", true, ui.arrangementOverdub);
        unavailable(m, {"Max for Live Developer Mode", "Audio Engine On"});
        m.addSeparator(); m.addItem(Preferences, "Settings...    (Cmd+,)"); m.addItem(AudioSettings, "Audio Settings...");
        m.addItem(EnableMidi, "Enable All MIDI Inputs");
        m.addItem(ScanVst3, "Scan Plug-ins (VST3)"); m.addItem(ScanVst3AndAu, "Scan Plug-ins (VST3 + Audio Units)");
        m.addItem(LoadTheme, "Load Theme (.ask)...");
    }
    else if (index == 7)
    {
        m.addItem(ReadManual, "Read the Sonora Guide...");
        unavailable(m, {"Learn View", "Pack Overview", "Load Demo Set", "Get Support"});
        m.addItem(CheckForUpdates, "Check for Updates...", updater_ != nullptr && (updater_->canCheckNow() || updater_->installPending()));
        m.addSeparator();
        m.addItem(AboutSonora, "About Sonora");
    }
    juce::PopupMenu::MenuItemIterator iterator(m, true);
    while (iterator.next())
    {
        auto& item = iterator.getItem();
        const int hint = item.text.indexOf("    (");
        if (hint >= 0)
        {
            item.shortcutKeyDescription = item.text.substring(hint + 5).upToLastOccurrenceOf(")", false, false)
                .replace("Shift+", "⇧").replace("Cmd+", "⌘").replace("Opt+", "⌥");
            item.text = item.text.substring(0, hint);
        }
        switch (item.itemID)
        {
            case ToggleClipDevice: item.shortcutKeyDescription = juce::String::fromUTF8("⇧⇥"); break;
            case ShowClip: item.shortcutKeyDescription = juce::String::fromUTF8("⌥⌘3"); break;
            case ShowDevices: item.shortcutKeyDescription = juce::String::fromUTF8("⌥⌘4"); break;
            case ToggleMixer: item.shortcutKeyDescription = juce::String::fromUTF8("⌥⌘M"); break;
            case ToggleSends: item.shortcutKeyDescription = juce::String::fromUTF8("⌥⌘S"); break;
            case ComputerKeyboard: item.shortcutKeyDescription = "M"; break;
            case SoloTrack: item.shortcutKeyDescription = "S"; break;
            case SplitClip: item.shortcutKeyDescription = juce::String::fromUTF8("⌘E"); break;
            case ArmTrack: item.shortcutKeyDescription = "C"; break;
            case SessionRecord: item.shortcutKeyDescription = juce::String::fromUTF8("⇧⌘F9"); break;
            case FullScreen: item.shortcutKeyDescription = juce::String::fromUTF8("⌃⌘F"); break;
            case FocusControlBar: item.shortcutKeyDescription = juce::String::fromUTF8("⌥0"); break;
            case FocusSession: item.shortcutKeyDescription = juce::String::fromUTF8("⌥1"); break;
            case FocusArrangement: item.shortcutKeyDescription = juce::String::fromUTF8("⌥2"); break;
            case FocusClip: item.shortcutKeyDescription = juce::String::fromUTF8("⌥3"); break;
            case FocusDevices: item.shortcutKeyDescription = juce::String::fromUTF8("⌥4"); break;
            case FocusBrowser: item.shortcutKeyDescription = juce::String::fromUTF8("⌥5"); break;
            default: break;
        }
        if (shortcutsReady_ && shortcuts_.isCustom(item.itemID))
        {
            const auto key = shortcuts_.keyFor(item.itemID);
            item.shortcutKeyDescription = key ? juce::String::fromUTF8(key->glyphs().c_str()) : juce::String();
        }
    }
    return m;
}

void MainWindow::buildShortcutRegistry()
{
    std::vector<ui::ShortcutMap::Command> commands;
    const auto names = getMenuBarNames();
    for (int index = 0; index < names.size(); ++index)
    {
        auto menu = getMenuForIndex(index, names[index]);
        juce::PopupMenu::MenuItemIterator iterator(menu, true);
        while (iterator.next())
        {
            const auto& item = iterator.getItem();
            if (item.itemID <= 0 || item.itemID >= RecentBase || item.subMenu != nullptr || item.text.isEmpty() || commandMenu_.count(item.itemID))
                continue;
            ui::ShortcutMap::Command command;
            command.id = item.itemID;
            command.menu = names[index].toStdString();
            command.name = item.text.toStdString();
            command.defaultKey = ui::KeyCombo::fromGlyphs(item.shortcutKeyDescription.toStdString());
            commandMenu_[item.itemID] = index;
            commands.push_back(std::move(command));
        }
    }
    int withKeys = 0;
    for (const auto& command : commands) withKeys += command.defaultKey ? 1 : 0;
    logf("shortcuts: %d menu commands rebindable, %d with a default key", (int) commands.size(), withKeys);
    shortcuts_.setCommands(std::move(commands));
}

bool MainWindow::commandEnabled(int id)
{
    const auto found = commandMenu_.find(id);
    if (found == commandMenu_.end()) return false;
    auto menu = getMenuForIndex(found->second, getMenuBarNames()[found->second]);
    juce::PopupMenu::MenuItemIterator iterator(menu, true);
    while (iterator.next())
        if (iterator.getItem().itemID == id) return iterator.getItem().isEnabled;
    return false;
}

void MainWindow::menuItemSelected(int itemId, int)
{
    auto& ui = app_.ui();
    if (app_.exporting())
        return; // the export dialog is modal: nothing may edit or play while it renders
    if (ui.preferencesOpen && itemId != Preferences && itemId != AudioSettings && itemId != CheckForUpdates && itemId != AboutSonora) return;
    if (itemId >= RecentBase && (size_t) (itemId - RecentBase) < recentMenuFiles_.size())
    {
        const auto file = recentMenuFiles_[(size_t) (itemId - RecentBase)];
        if (file.existsAsFile()) app_.openFile(file);
        return;
    }
    auto key = [this](int ch, bool cmd = false, bool shift = false) {
        return layout_->dispatchKey({.keyCode = ch, .textChar = (uint32_t) ch, .shift = shift, .cmd = cmd});
    };
    switch (itemId)
    {
        case FreezeTrack: app_.renderTrack(ui.selectedTrack, true); break;
        case BounceTrack: app_.renderTrack(ui.selectedTrack, false); break;
        case DeactivateTrack: ui::toggleSelectedTracksDeactivated(ui); break;
        case HideTrack: ui::toggleSelectedTracksHidden(ui); break;
        case StripSilence: if (ui.stripSilence) ui.stripSilence(ui.selectedClipLocation()); break;
        case NextTransient: if (ui.jumpToTransient) ui.jumpToTransient(1); break;
        case PreviousTransient: if (ui.jumpToTransient) ui.jumpToTransient(-1); break;
        case ShowHiddenTracks:
            ui.showHiddenTracks = ! ui.showHiddenTracks;
            if (ui.modelChanged) ui.modelChanged();
            if (ui.requestRepaint) ui.requestRepaint();
            break;
        case BounceNew: app_.renderTrack(ui.selectedTrack, false, true); break;
        case SplitClip: ui::splitArrangementSelection(ui); break;
        case ConsolidateClips: app_.consolidateSelection(); break;
        case ConvertMelody: app_.convertAudioToMidi(ui.selectedClipLocation(), 0); break;
        case ResynthesizeClip: app_.resynthesizeClip(ui.selectedClipLocation()); break;
        case ConvertHarmony: app_.convertAudioToMidi(ui.selectedClipLocation(), 1); break;
        case ConvertDrums: app_.convertAudioToMidi(ui.selectedClipLocation(), 2); break;
        case SliceMidi: app_.convertAudioToMidi(ui.selectedClipLocation(), 3); break;
        case NewProject: app_.newProject(); break;
        case OpenFile: app_.openFileDialog(); break;
        case SaveFile: app_.saveFile(); break;
        case SaveFileAs: app_.saveFileAs(); break;
        case SaveCopy: app_.saveCopy(); break;
        case ExportMidiClip: app_.exportMidiClip(); break;
        case CollectAllAndSave: app_.collectAllAndSave(); break;
        case ExportAudio: if (ui.song != nullptr) ui.exportOpen = true; break;
        case ShareSessionLan: app_.shareSession(true); break;
        case ShareSessionRelay: app_.shareSession(false); break;
        case JoinSession: app_.joinSessionDialog(); break;
        case CollaboratePanel: ui.collabOpen = ! ui.collabOpen; if (ui.requestRepaint) ui.requestRepaint(); break;
        case LeaveSession: app_.leaveSession(); break;
        case Undo: app_.undo(); break;
        case Redo: app_.redo(); break;
        case DuplicateClip:
            if (ui.sessionTrackSelection && ui.selectedClipUid < 0 && ui.duplicateSelectedTracks) { ui.duplicateSelectedTracks(); break; }
            if (key('D', true)) break;
            if (ui.selectedClipUid >= 0 && ui.song && ui.applyEdit) ui.applyEdit(model::duplicateClip(*ui.song, ui.selectedClipLocation()));
            break;
        case DeleteClip:
            if (! key(juce::KeyPress::deleteKey)) ui.deleteSelectedDevice();
            break;
        case Play: if (ui.transport && ui.song) ui.transport->play(); break;
        case Stop: if (ui.transport) ui.transport->stop(); break;
        case Record: if (ui.toggleRecording) ui.toggleRecording(); break;
        case StopAllClips: if (ui.stopAllClips) ui.stopAllClips(); break;
        case Preferences: ui.preferencesOpen = ! ui.preferencesOpen; break;
        case CheckForUpdates: if (updater_) updater_->checkNow(); break;
        case AboutSonora: AboutWindow::show(); break;
        case AudioSettings: ui.preferencesOpen = true; ui.settingsPage = 3; break;
        case ScanVst3: app_.scanPlugins(false); break;
        case ScanVst3AndAu: app_.scanPlugins(true); break;
        case LoadTheme: app_.loadThemeDialog(); break;
        case EnableMidi: app_.enableAllMidiInputs(); break;
        case MidiClipMap: layout_->keyPressed({.keyCode = 'M', .textChar = 'M', .cmd = true}); break;
        case KeyClipMap: layout_->keyPressed({.keyCode = 'K', .textChar = 'K', .cmd = true}); break;
        case ToggleTakeLanes:
            if (ui.song && ui.selectedTrack >= 0 && ui.selectedTrack < ui.numTracks())
            {
                ui.mainView = ui::MainView::Arrangement;
                const auto found = std::find(ui.unfoldedTakeTracks.begin(), ui.unfoldedTakeTracks.end(), ui.selectedTrack);
                if (found == ui.unfoldedTakeTracks.end()) { ui.unfoldedTakeTracks.push_back(ui.selectedTrack); ui.automationVisible = false; }
                else ui.unfoldedTakeTracks.erase(found);
            }
            break;
        case ShowSession: ui.mainView = ui::MainView::Session; break;
        case ShowArrangement: ui.mainView = ui::MainView::Arrangement; break;
        case InsertMidiClip:
            if (ui.song != nullptr && ui.applyEdit && ui.selectedTrack >= 0 && ui.selectedTrack < ui.numTracks()
                && ui.song->tracks[(size_t) ui.selectedTrack].kind == model::TrackKind::Midi)
            {
                // a one-bar clip at the bar of the play position, like Live's Create > Insert Empty MIDI Clip
                const double bar = ui.song->timeSigNumerator * 4.0 / ui.song->timeSigDenominator;
                const bool session = ui.mainView == ui::MainView::Session;
                const double at = ! session && ui.arrangementSelection.active ? ui.arrangementSelection.start
                                : ! session && ui.transport ? std::floor(ui.transport->positionBeats() / bar) * bar : 0.0;
                const double length = ! session && ui.arrangementSelection.active ? ui.arrangementSelection.end - at : bar;
                model::Clip fresh;
                if (model::makeEmptyMidiClip(*ui.song, at, length, fresh))
                {
                    const int targetTrack = ui.selectedTrack;
                    fresh.colorIndex = ui.trackAt(targetTrack)->colorIndex;
                    std::vector<int> existing;
                    for (const auto& clip : ui.trackAt(targetTrack)->arrangementClips) existing.push_back(clip.uid);
                    if (session)
                    {
                        if (ui.selectedSessionScene < 0 || (size_t) ui.selectedSessionScene >= ui.song->scenes.size()) break;
                        const auto* track = ui.trackAt(ui.selectedTrack);
                        if ((size_t) ui.selectedSessionScene < track->slots.size() && track->slots[(size_t) ui.selectedSessionScene].clip) break;
                        ui.applyEdit(model::setSessionClip(*ui.song, ui.selectedTrack, ui.selectedSessionScene, std::move(fresh)));
                    }
                    else ui.applyEdit(model::insertClip(ui.selectedTrack, fresh, "Insert MIDI Clip"));
                    const auto* track = ui.trackAt(targetTrack);
                    const model::Clip* created = session && (size_t) ui.selectedSessionScene < track->slots.size()
                        && track->slots[(size_t) ui.selectedSessionScene].clip ? &*track->slots[(size_t) ui.selectedSessionScene].clip : nullptr;
                    if (! session)
                        for (const auto& clip : track->arrangementClips)
                            if (std::find(existing.begin(), existing.end(), clip.uid) == existing.end()) { created = &clip; break; }
                    if (created)
                    {
                        ui.selectedClipTrack = targetTrack; ui.selectedClipUid = created->uid;
                        ui.selectedClipScene = session ? ui.selectedSessionScene : -1; ui.selectedTakeLane = -1;
                        ui.detailTab = ui::DetailTab::Clip; ui.detailVisible = true; ui.sessionTrackSelection = false;
                        ui.arrangementSelection = {}; ++ui.clipZoomRequest;
                    }
                }
                else
                    ui.statusText = "Cannot create a MIDI clip: this set has no MIDI clip to copy the format from";
            }
            break;
        case DeleteDevice: ui.deleteSelectedDevice(); break;
        case LoopSelection: layout_->dispatchKey({.keyCode = 'L', .textChar = 'L', .cmd = true}); break;
        case FoldNotes: ui.noteFold = ! ui.noteFold; if (ui.noteFold) ui.scaleFold = false; break;
        case FoldScale: ui.scaleFold = ! ui.scaleFold; if (ui.scaleFold) { ui.scaleHighlight = true; ui.noteFold = false; } break;
        case HighlightScales: ui.scaleHighlight = ! ui.scaleHighlight; break;
        case PianoPreview: ui.pianoPreview = ! ui.pianoPreview; if (! ui.pianoPreview) layout_->releaseMappedKeys(); break;
        case InsertTakeLane: ui::insertEmptyTakeLane(ui); break;
        case GroupTracks: if (ui.groupSelectedTracks) ui.groupSelectedTracks(); break;
        case UngroupTracks: if (ui.ungroupSelectedTracks) ui.ungroupSelectedTracks(); break;
        case NarrowGrid: ui::handleGridShortcut(ui.activeGrid(), '1'); if (ui.layoutChanged) ui.layoutChanged(); break;
        case WidenGrid: ui::handleGridShortcut(ui.activeGrid(), '2'); if (ui.layoutChanged) ui.layoutChanged(); break;
        case TripletGrid: ui::handleGridShortcut(ui.activeGrid(), '3'); if (ui.layoutChanged) ui.layoutChanged(); break;
        case SnapToGrid: ui::handleGridShortcut(ui.activeGrid(), '4'); if (ui.layoutChanged) ui.layoutChanged(); break;
        case FixedGrid: ui::handleGridShortcut(ui.activeGrid(), '5'); if (ui.layoutChanged) ui.layoutChanged(); break;
        case ZoomInTime: layout_->dispatchKey({.keyCode = '+', .textChar = '+'}); break;
        case ZoomOutTime: layout_->dispatchKey({.keyCode = '-', .textChar = '-'}); break;
        case ToggleBrowser: ui.browserVisible = ! ui.browserVisible; break;
        case ToggleDetail: ui.detailVisible = ! ui.detailVisible; break;
        case CloseWindow: closeButtonPressed(); break;
        case ClearRecent: app_.clearRecentSets(); break;
        case RemoveUnavailable: app_.clearRecentSets(true); break;
        case AddFolder: if (ui.addBrowserFolder) ui.addBrowserFolder(); break;
        case ImportAudio: app_.importAudioDialog(); break;
        case Rename: app_.renameSelection(); break;
        case AddLocator:
            if (ui.song && ui.applyEdit && ui.transport)
                ui.applyEdit(model::addLocator(*ui.song, ui.transport->positionBeats(), std::to_string(ui.song->locators.size() + 1)));
            break;
        case ShowBackups:
        case RevealBackups:
            if (ui.song)
            {
                const auto path = juce::File(ui.song->sourcePath).getParentDirectory().getChildFile("Backup");
                if (path.isDirectory())
                {
                    if (itemId == RevealBackups) path.revealToUser();
                    else { ui.browserVisible = true; layout_->layout(); findWidget<ui::BrowserPanel>(*layout_)->showFolder(path.getFullPathName().toStdString()); }
                }
            }
            break;
        case Quantize: key('U', true); break;
        case QuantizeSettings: key('U', true, true); break;
        case ApplyMidiTool: if (auto* panel = findWidget<ui::ClipControls>(*layout_)) panel->keyPressed({.keyCode = juce::KeyPress::returnKey, .cmd = true}); break;
        case InvertNotes: key('A', true, true); break;
        case SelectAll:
            if (dynamic_cast<ui::PianoRoll*>(layout_->keyboardFocus())) key('A', true);
            else if (ui.numTracks() > 0) { ui.sessionTrackSelection = true; ui.selectTrack(0, false, false); ui.selectTrack(ui.numTracks() - 1, true, false); }
            break;
        case ToggleClipActive:
            if (dynamic_cast<ui::PianoRoll*>(layout_->keyboardFocus())) { key('0'); break; }
            if (const auto* clip = ui.selectedClip(); clip && ui.applyEdit)
            { auto changed = *clip; changed.disabled = ! changed.disabled; ui.applyEdit(model::modifyClip(*ui.song, ui.selectedClipLocation(), std::move(changed), "Activate/Deactivate Clip")); }
            break;
        case SoloTrack:
            if (ui.controls && ui.selectedTrack >= 0 && (size_t) ui.selectedTrack < ui.controls->size())
            { ui::toggleTrackSolo(ui, ui.selectedTrack); }
            break;
        case ArmTrack:
            if (ui.armTrack && ui.controls && ui.selectedTrack >= 0 && (size_t) ui.selectedTrack < ui.controls->size())
                ui.armTrack(ui.selectedTrack, ! (*ui.controls)[(size_t) ui.selectedTrack].armed.load());
            break;
        case InsertAudioTrack: if (ui.insertTrack) ui.insertTrack(false); break;
        case InsertMidiTrack: if (ui.insertTrack) ui.insertTrack(true); break;
        case InsertReturnTrack: if (ui.insertReturnTrack) ui.insertReturnTrack(); break;
        case CaptureMidi: if (ui.captureMidi) ui.captureMidi(); break;
        case CaptureAudio: if (ui.captureAudio) ui.captureAudio(); break;
        case SeparateStems: if (ui.separateStems && ui.selectedClip()) ui.separateStems(ui.selectedClipLocation()); break;
        case ContinuePlayback: if (ui.transport) ui.transport->play(); break;
        case ReturnToStart: if (ui.transport) ui.transport->setPosition(0); break;
        case SessionRecord: if (ui.toggleSessionRecording) ui.toggleSessionRecording(); break;
        case Metronome: if (ui.transport) ui.transport->setMetronome(! ui.transport->metronomeOn()); break;
        case BackToArrangement: if (ui.backToArrangement) ui.backToArrangement(); break;
        case ToggleViews: ui.mainView = ui.mainView == ui::MainView::Session ? ui::MainView::Arrangement : ui::MainView::Session; break;
        case ToggleClipDevice: ui.detailTab = ui.detailTab == ui::DetailTab::Devices ? ui::DetailTab::Clip : ui::DetailTab::Devices; ui.detailVisible = true; break;
        case ShowClip: layout_->keyPressed({.textChar = '3', .cmd = true, .alt = true}); break;
        case ShowDevices: layout_->keyPressed({.textChar = '4', .cmd = true, .alt = true}); break;
        case FocusClip: layout_->keyPressed({.textChar = '3', .alt = true}); break;
        case FocusDevices: layout_->keyPressed({.textChar = '4', .alt = true}); break;
        case ExpressionEditor: ui.clipMpeVisible = true; ui.clipEnvelopesVisible = false; ui.detailTab = ui::DetailTab::Clip; ui.detailVisible = true; break;
        case ContentEditor: case EnvelopeEditor:
            ui.clipMpeVisible = false; ui.clipEnvelopesVisible = itemId == EnvelopeEditor; ui.detailTab = ui::DetailTab::Clip; ui.detailVisible = true; break;
        case SearchBrowser: case FocusBrowser:
            ui.browserVisible = true; layout_->layout(); findWidget<ui::BrowserPanel>(*layout_)->focusSearch(); break;
        case FocusControlBar: layout_->setKeyboardFocus(findWidget<ui::TopBar>(*layout_)); break;
        case FocusSession: ui.mainView = ui::MainView::Session; layout_->layout(); layout_->setKeyboardFocus(findWidget<ui::SessionView>(*layout_)); break;
        case FocusArrangement: ui.mainView = ui::MainView::Arrangement; layout_->layout(); layout_->setKeyboardFocus(findWidget<ui::ArrangementView>(*layout_)); break;
        case ToggleMixer: ui.mixerVisible = ! ui.mixerVisible; if (ui.layoutChanged) ui.layoutChanged(); break;
        case ToggleIo: ui.ioVisible = ! ui.ioVisible; break;
        case ToggleSends: ui.sendsVisible = ! ui.sendsVisible; if (ui.layoutChanged) ui.layoutChanged(); break;
        case ToggleAutomation: ui.toggleAutomationLane(ui.selectedTrack); ui.mainView = ui::MainView::Arrangement; break;
        case HideGroupedTracks:
            if (ui.song && ui.applyEdit)
            {
                std::vector<model::EditCommand> commands;
                for (int i = 0; i < ui.numTracks(); ++i)
                    if (ui.song->tracks[(size_t) i].kind == model::TrackKind::Group && ! ui.song->tracks[(size_t) i].folded)
                        commands.push_back(model::setTrackFolded(*ui.song, i, true));
                if (! commands.empty()) ui.applyEdit(model::compose("Hide Grouped Tracks", std::move(commands)));
            }
            break;
        case ShowGroupedTrack:
            if (ui.song && ui.applyEdit && ui.selectedTrack >= 0 && ui.selectedTrack < ui.numTracks())
            {
                const auto* selected = ui.trackAt(ui.selectedTrack);
                int id = selected->kind == model::TrackKind::Group ? selected->id : selected->groupId;
                std::vector<model::EditCommand> commands;
                for (int depth = 0; id >= 0 && depth < ui.numTracks(); ++depth)
                {
                    const auto found = std::find_if(ui.song->tracks.begin(), ui.song->tracks.end(), [id](const auto& track) { return track.id == id; });
                    if (found == ui.song->tracks.end()) break;
                    if (found->folded) commands.push_back(model::setTrackFolded(*ui.song, (int) (found - ui.song->tracks.begin()), false));
                    id = found->groupId;
                }
                if (! commands.empty()) ui.applyEdit(model::compose("Show Grouped Track", std::move(commands)));
            }
            break;
        case ZoomUiIn: if (ui.setUiZoom) ui.setUiZoom(ui.uiZoom + 0.1f); break;
        case ZoomUiOut: if (ui.setUiZoom) ui.setUiZoom(ui.uiZoom - 0.1f); break;
        case ZoomUiReset: if (ui.setUiZoom) ui.setUiZoom(1); break;
        case FullScreen: setFullScreen(! isFullScreen()); break;
        case ComputerKeyboard: layout_->releaseMappedKeys(); ui.computerKeyboard = ! ui.computerKeyboard; break;
        case BrowserPreview:
            ui.browserPreviewEnabled = ! ui.browserPreviewEnabled;
            if (! ui.browserPreviewEnabled && ui.previewSample) ui.previewSample("");
            if (ui.layoutChanged) ui.layoutChanged();
            break;
        case DrawMode: ui.drawMode = ! ui.drawMode; break;
        case Overdub: ui.arrangementOverdub = ! ui.arrangementOverdub; if (ui.layoutChanged) ui.layoutChanged(); break;
        case TripletLaunchQuant:
            if (ui.song && ui.applyEdit && ui.song->globalQuantisation >= 5 && ui.song->globalQuantisation <= 12)
                ui.applyEdit(model::setGlobalQuantisation(*ui.song, ui.song->globalQuantisation + (ui.song->globalQuantisation % 2 == 0 ? -1 : 1)));
            break;
        case SmallerQuant: case LargerQuant: case ToggleQuant:
            if (ui.song && ui.applyEdit)
            {
                const int current = ui.song->globalQuantisation;
                const int next = itemId == ToggleQuant ? (current == 0 ? 4 : 0)
                                 : current == 0 ? (itemId == SmallerQuant ? 13 : 1)
                                 : std::clamp(current + (itemId == SmallerQuant ? 1 : -1), 1, 13);
                ui.applyEdit(model::setGlobalQuantisation(*ui.song, next));
            }
            break;
        case ReadManual:
        {
            auto guide = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getChildFile("Contents/Resources/UI_GUIDE.md");
            if (! guide.existsAsFile()) // Windows/Linux: Resources/ next to the executable (COM-7)
                guide = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("Resources/UI_GUIDE.md");
            if (! guide.existsAsFile()) guide = juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("docs/UI_GUIDE.md");
            if (guide.existsAsFile()) guide.startAsProcess();
            break;
        }
        case ToggleAssistant: toggleAssistant(); break;
        default: break;
    }
    if (itemId == FocusClip || itemId == FocusDevices)
    {
        layout_->layout();
        auto* roll = findWidget<ui::PianoRoll>(*layout_);
        layout_->setKeyboardFocus(itemId == FocusClip && roll ? static_cast<ui::Widget*>(roll) : findWidget<ui::DetailView>(*layout_));
    }
    if (ui.modelChanged)
        ui.modelChanged();
}

} // namespace sonora::app
