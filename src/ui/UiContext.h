#pragma once
// Everything the views need to render and to issue commands, in one place.
// AppController fills it for the real app; tools/ui_snapshot fills only what it has.
// All pointers may be null (headless snapshot) — views must cope with that.

#include "core/Edit.h"
#include "core/Model.h"
#include "core/Scales.h"
#include "engine/PeakCache.h"
#include "devices/native/WavetableEffects.h"
#include "midi/ControlTarget.h"
#include "ui/GridModel.h"
#include "ui/Rect.h"
#include "ui/Theme.h"
#include "ui/BrowserDrag.h"
#include "ui/BrowserTags.h"

#include <algorithm>
#include <compare>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

namespace sonora::engine
{
class Transport;
class ControlBank;
class MasterMonitor;
struct EngineGraph;
} // namespace sonora::engine

namespace sonora::midi
{
class MidiManager;
}
namespace sonora::dsp { struct CurveGraph; }

namespace sonora::ui
{
struct KeyEvent;
class ShortcutMap;

enum class MainView
{
    Session,
    Arrangement
};

// What the bottom panel shows: the device chain of the selected track, or the selected clip's editor.
// Export dialog (File > Export Audio): what to render and in which file format.
struct ExportSettings
{
    int target = 0;      // 0 Main, 1 All Individual Tracks, 2 + n: the track with control index n
    int range = 0;       // 0 Entire Arrangement, 1 Loop Selection, 2 Custom
    double startBar = 1.0, endBar = 5.0; // Custom range, bar numbers (the end bar is not included)
    int tail = 0;        // 0 none, 1 1 s, 2 2 s, 3 4 s, 4 8 s, 5 until silent
    bool normalize = false, mono = false, dither = true;
    int format = 0;      // 0 WAV, 1 AIFF, 2 FLAC
    int bitDepth = 24;   // 16, 24, 32 (float, WAV only)
};

// Progress / outcome of the export that is running or finished (written by the app, read by the dialog).
struct ExportState
{
    enum class Phase { Idle, Running, Done, Failed } phase = Phase::Idle;
    float progress = 0.0f;
    std::string message;           // failure reason or short status
    std::vector<std::string> files;
    float integratedLufs = -200.0f, loudnessRange = 0.0f, truePeakDb = -200.0f, samplePeakDb = -200.0f;
    double seconds = 0.0;
};

// COL-5: a collab session as the views see it (filled by app/AppCollab.cpp; plain data, no collab types).
struct CollabView
{
    enum class State { None, Starting, Joining, Active, Reconnecting, Ended };
    enum class Role { Host, Editor, Listener };
    struct Participant
    {
        int id = 0;
        std::string name;
        Role role = Role::Editor;
        int color = 0;      // index into collabColor()
        bool self = false;
        std::string safetyCode; // four words to compare with this person ("" for yourself)
    };
    // Another participant's pointer and selection, in musical coordinates.
    struct Cursor
    {
        int id = 0;
        std::string name;
        int color = 0;
        int view = 0;          // 0 Arrangement, 1 Session, 2 clip editor
        int trackId = -1;
        double beat = -1.0;
        int pitch = -1;
        int clipUid = -1;      // the clip they have open
        int selectedClipUid = -1;
        int selectedTrackFrom = -1, selectedTrackTo = -1; // track ids
        double selectionStart = -1.0, selectionEnd = -1.0;
        float age = 0.0f;      // seconds since it last moved (fades out)
    };
    struct LanSession
    {
        std::string label, address;
    };
    State state = State::None;
    bool host = false, canEdit = true, followTransport = true, networkAvailable = false;
    std::string sessionName, status, invite, endedReason;
    float progress = -1.0f;
    std::vector<Participant> participants;
    std::vector<Cursor> cursors;
    std::vector<std::pair<std::string, std::string>> joinRequests; // peer, name
    std::vector<LanSession> lanSessions;
    std::vector<std::string> notices; // newest last
    std::string displayName, relayUrl;
    bool active() const { return state != State::None && state != State::Ended; }
};

// The participant palette (COL-5): distinct, readable on the light and dark themes. 0 is the host.
inline uint32_t collabColor(int index)
{
    static constexpr uint32_t colors[] = {0xff2f80edu, 0xffeb5757u, 0xff27ae60u, 0xfff2994au, 0xff9b51e0u, 0xff00a3a3u, 0xffd94f9bu, 0xff8d6e00u};
    return colors[((index % 8) + 8) % 8];
}

enum class DetailTab
{
    Devices,
    Clip,
    Monitor // loudness, spectrum, scope and stereo meters of the Main output
};

struct UiContext
{
    Theme theme;

    // Width of the Arrangement track headers: title + [I/O] + mixer + meters.
    float arrangementHeaderWidth() const { return theme.metrics.arrangementHeaderWidth - (ioVisible ? 0.0f : 81.0f); }

    // ---- data (not owned)
    const model::Song* song = nullptr;
    engine::Transport* transport = nullptr;
    std::shared_ptr<engine::ControlBank> controls;
    engine::EngineGraph* graph = nullptr; // latest graph: device processors for the detail view
    midi::MidiManager* midi = nullptr;

    std::string trackDisplayName(int index) const { return song ? model::displayTrackName(*song, index) : std::string{}; }

    // ---- UI state
    MainView mainView = MainView::Arrangement;
    int selectedTrack = 0;     // ControlBank index space: tracks..., returns..., main
    int selectedSessionScene = 0;
    bool sessionTrackSelection = false; // header/range selection, separate from the track containing the focused clip
    bool browserVisible = true;
    bool detailVisible = true;
    bool automationVisible = false; // arrangement automation lanes are on (UI-5)
    // UI-46: control indices whose automation lane shows while automationVisible; empty shows every track's lane.
    // A (Automation Mode) toggles only the selected track's lane.
    std::vector<int> automationTracks;
    static constexpr float automationLaneHeight = 70.0f;
    bool showsAutomation(int controlIndex) const
    {
        return automationVisible
               && (automationTracks.empty() || std::find(automationTracks.begin(), automationTracks.end(), controlIndex) != automationTracks.end());
    }
    float automationHeight(int controlIndex) const { return showsAutomation(controlIndex) ? automationLaneHeight : 0.0f; }
    // Shows or hides the automation lane of one track (the A key and View > Automation Mode).
    void toggleAutomationLane(int controlIndex)
    {
        if (! automationVisible) automationTracks.clear();
        if (showsAutomation(controlIndex))
        {
            if (automationTracks.empty()) automationVisible = false; // "every track" mode: A turns it off
            else automationTracks.erase(std::remove(automationTracks.begin(), automationTracks.end(), controlIndex), automationTracks.end());
            if (automationTracks.empty()) automationVisible = false;
        }
        else
        {
            automationTracks.push_back(controlIndex);
            automationVisible = true;
        }
        if (! automationVisible) selectedAutomationTrack = -1;
    }
    bool ioVisible = true;
    bool mixerVisible = true;
    bool sendsVisible = true;
    std::unordered_map<std::string, std::array<int, 2>> pluginPadAssignments; // per-document UI bindings
    int countInBars = 0; // persistent application preference: 0, 1, 2, 4
    std::function<void(int)> setCountInBars;
    std::function<int()> audioInputChannelCount;
    std::function<std::vector<std::pair<std::string, std::string>>()> midiInputDevices; // id, display name
    std::function<void(int track, bool armed)> armTrack;
    int selectedAutomationTrack = -1;
    int selectedDeviceId = -1;     // device (Device View) the user last clicked on the selected track; Delete removes it
    // Deletes the selected device through applyEdit. False when nothing is selected.
    bool deleteSelectedDevice()
    {
        if (song == nullptr || ! applyEdit || selectedDeviceId < 0) return false;
        const auto* track = trackAt(selectedTrack);
        if (track == nullptr) return false;
        const int id = selectedDeviceId;
        for (const auto& d : track->devices)
            if (d.id == id)
            {
                selectedDeviceId = -1;
                applyEdit(model::removeDevice(*song, selectedTrack, id));
                return true;
            }
        return false;
    }
    // Track multi-selection (Shift = range, Cmd = toggle on a track header): the extra tracks besides `selectedTrack`.
    std::set<int> trackSelection;
    int trackSelectionAnchor = -1;
    struct TrackDrag
    {
        const model::Song* song = nullptr;
        uint64_t structure = 0;
        std::vector<int> tracks;
    };
    std::optional<TrackDrag> trackDrag; // Root owns motion, target preview, cancellation and commit
    void beginTrackDrag()
    {
        if (song && ! recording && ! recordingRequested)
            trackDrag = TrackDrag{song, trackStructure(), selectedTrackIndices()};
    }
    bool isTrackSelected(int index) const { return index == selectedTrack || trackSelection.count(index) > 0; }
    void selectTrack(int index, bool shift, bool cmd)
    {
        if (shift)
        {
            const int anchor = trackSelectionAnchor >= 0 ? trackSelectionAnchor : selectedTrack;
            trackSelection.clear();
            for (int i = std::min(anchor, index); i <= std::max(anchor, index); ++i)
                trackSelection.insert(i);
            selectedTrack = index;
            trackSelectionAnchor = anchor;
        }
        else if (cmd)
        {
            if (trackSelection.empty())
                trackSelection.insert(selectedTrack);
            if (trackSelection.count(index) > 0 && trackSelection.size() > 1)
            {
                trackSelection.erase(index);
                selectedTrack = *trackSelection.begin();
            }
            else
            {
                trackSelection.insert(index);
                selectedTrack = index;
            }
            trackSelectionAnchor = selectedTrack;
        }
        else
        {
            trackSelection.clear();
            selectedTrack = trackSelectionAnchor = index;
        }
    }
    // The selected tracks (never returns or main), sorted.
    std::vector<int> selectedTrackIndices() const
    {
        std::set<int> all = trackSelection;
        all.insert(selectedTrack);
        std::vector<int> out;
        for (int i : all)
            if (i >= 0 && i < numTracks())
                out.push_back(i);
        return out;
    }
    std::function<void()> groupSelectedTracks, ungroupSelectedTracks; // Cmd+G / Shift+Cmd+G
    std::function<void()> duplicateSelectedTracks;
    std::function<void()> deleteSelectedTracks;
    std::vector<int> selectedChannelIndices() const
    {
        auto all = trackSelection; all.insert(selectedTrack);
        std::vector<int> result;
        for (int index : all) if (index >= 0 && index < numTracks() + numReturns()) result.push_back(index);
        return result;
    }
    GridSettings arrangementGrid; // Options > Narrow/Widen/Triplet/Snap/Fixed Grid of the Arrangement
    GridSettings clipGrid;        // the same for the MIDI note editor and clip envelopes
    bool clipEditorFocused = false; // the last editor clicked: true = note/envelope editor, false = Arrangement
    GridSettings& activeGrid() { return mainView == MainView::Session || clipEditorFocused ? clipGrid : arrangementGrid; }
    double pixelsPerBeat = 14.0; // arrangement horizontal zoom
    bool waveformZoomEnabled = true;
    float waveformZoom = 1.0f; // display amplitude only; never changes clip/audio gain
    double scrollBeats = 0.0;    // arrangement horizontal scroll
    struct TimeSelection { bool active = false; double start = 0, end = 0; int firstTrack = -1, lastTrack = -1; };
    TimeSelection arrangementSelection;
    // Bounce to New Track renders only this range when the Arrangement time selection (e.g. part of a clip) covers the
    // track; nullopt renders the whole track.
    std::optional<std::pair<double, double>> bounceSelection(int trackIndex) const
    {
        const auto& range = arrangementSelection;
        if (! range.active || mainView != MainView::Arrangement || trackIndex < range.firstTrack || trackIndex > range.lastTrack
            || range.end - range.start <= 1e-6)
            return std::nullopt;
        return std::make_pair(std::max(0.0, range.start), range.end);
    }
    double arrangementCursor = -1; // insertion position, independent of the moving playback cursor
    float scrollTracksY = 0.0f;  // vertical scroll (both views)
    std::string statusText = "Ready";
    std::vector<std::string> buildLog;
    // UI-51: work the user waits for (opening a set, starting plug-ins, scans). While a task is open the status bar shows
    // the newest one with an activity indicator, the launch splash mirrors it and, unless the task runs in the background
    // while the user keeps working (scans, stems), the canvas shows the busy cursor.
    struct BusyTask { int id = 0; std::string text; float progress = -1; /* 0..1, < 0 unknown */ bool background = false; };
    std::vector<BusyTask> busyTasks;
    int nextBusyId = 1;
    int beginBusy(std::string text, bool background = false)
    {
        busyTasks.push_back({nextBusyId, std::move(text), -1.0f, background});
        return nextBusyId++;
    }
    void updateBusy(int id, std::string text, float progress = -1.0f)
    {
        for (auto& task : busyTasks)
            if (task.id == id) { task.text = std::move(text); task.progress = progress; }
    }
    void endBusy(int id)
    {
        std::erase_if(busyTasks, [id](const BusyTask& task) { return task.id == id; });
    }
    const BusyTask* currentBusy() const { return busyTasks.empty() ? nullptr : &busyTasks.back(); }
    bool waitingForBusy() const
    {
        return std::any_of(busyTasks.begin(), busyTasks.end(), [](const BusyTask& task) { return ! task.background; });
    }
    double cpuUsage = 0.0;
    int midiActivityFrames = 0;
    std::vector<std::string> projectFiles; // .als found in the user's folders (browser "Places")
    std::vector<std::string> pluginNames;  // scanned plugins (browser "Plug-ins")
    struct BrowserPlugin { std::string label, identifier, vendor, format; bool instrument = false; std::string category{}; };
    std::vector<BrowserPlugin> browserPlugins;
    BrowserTags browserTags;
    std::function<void()> browserTagsChanged;
    std::optional<BrowserDrag> browserDrag;
    float browserDragX = 0, browserDragY = 0;
    // scene == -1: Arrangement at beat; scene >= 0: Session slot. Devices append to the track.
    std::function<void(const BrowserDrag&, int track, double beat, int scene)> dropBrowserItem;
    // Native sample/chain editing; message thread. chain == -1 addresses a top-level sampler.
    std::function<void(const BrowserDrag&, int track, int device, int chain, int zone, int child)> dropDeviceItem;
    std::function<void(int track, int device, std::function<void(model::Device&)>, std::string)> editDevice;
    std::function<void(int track, int device, int oscillator, const std::string& path, int chain, int child)> importWavetable;
    std::function<void(int track, int device, int oscillator, const std::string& path, int chain, int child)> importOscillatorMedia;
    std::function<void(int track, int device, const std::string& path, int chain, int child)> importWavetablePreset;
    std::function<void(int track, int device, int lfo, const dsp::CurveGraph&, int chain, int child)> editWavetableLfo;
    std::function<void(int track, int device, int slot, bool auxiliary, const dsp::CurveGraph&, bool enabled, int chain, int child)> editWavetableModulationCurve;
    struct WavetableModulationView
    {
        int page = 0, selectedSlot = 0;
        bool auxiliary = false, curveOpen = false;
    };
    std::unordered_map<int, WavetableModulationView> wavetableModulationViews;
    struct WavetableRoutingView { int page = 0; bool sources = true; };
    std::unordered_map<int, WavetableRoutingView> wavetableRoutingViews;
    std::function<void(int track, int device, const devices::WavetableEffects::Configuration&, int chain, int child)> setWavetableEffects;
    std::function<void(int track, int device, devices::WavetableEffects::ModuleId, const std::string& path, int chain, int child)> importWavetableImpulseResponse;
    struct WavetableFxView
    {
        int bus = 0, workflow = 0;
        uint32_t module = 0;
        int band = -1, chainPage = 0, controlsPage = 0;
        bool inspectorState = false, inspector = false;
        int section = 0, instance = 0, page = 0;
    };
    std::map<int, WavetableFxView> wavetableFxViews;
    int selectedRackId = -1, selectedRackChain = 0;
    int rackMacroToMap = 0;
    std::map<int, int> drumPadBanks;
    // Sonora Prism editor placement (UI-27): a floating window or docked in the device chain, chosen per instance.
    // Keyed by track id + device ids because imported device ids repeat across tracks. View state only: never saved.
    struct WavetableKey
    {
        int trackId = -1, deviceId = -1; // the track-level device (the rack, for a Wavetable inside a rack chain)
        int chain = -1, childId = -1;    // rack chain index and the Wavetable's own id, -1 at track level
        auto operator<=>(const WavetableKey&) const = default;
    };
    bool wavetableFloatingDefault = false;          // the app floats every editor not docked by hand; tests/snapshots dock
    std::map<WavetableKey, bool> wavetableFloating; // explicit choices: true floating, false docked
    uint64_t wavetablePlacementRevision = 0;        // the device chain rebuilds when a choice changes
    std::function<void(const WavetableKey&)> openWavetableWindow;  // shows or raises the floating editor
    std::function<void(const WavetableKey&)> closeWavetableWindow; // asynchronous: may be called from inside that window
    bool isWavetableFloating(const WavetableKey& key) const
    {
        const auto found = wavetableFloating.find(key);
        return found == wavetableFloating.end() ? wavetableFloatingDefault : found->second;
    }
    void setWavetableFloating(const WavetableKey& key, bool floating)
    {
        wavetableFloating[key] = floating;
        ++wavetablePlacementRevision;
    }
    // A track-level edit gives the device a fresh id; its placement (and nested ones) follow it.
    void renameWavetableDevice(int trackId, int oldId, int newId)
    {
        std::map<WavetableKey, bool> renamed;
        for (const auto& [key, floating] : wavetableFloating)
        {
            auto moved = key;
            if (key.trackId == trackId && key.deviceId == oldId) moved.deviceId = newId;
            renamed[moved] = floating;
        }
        wavetableFloating = std::move(renamed);
    }
    std::optional<WavetableKey> wavetableKey(int track, int device, int chain = -1, int child = -1) const
    {
        const auto* t = trackAt(track);
        if (t == nullptr || device < 0 || (size_t) device >= t->devices.size()) return std::nullopt;
        const auto& top = t->devices[(size_t) device];
        if (chain < 0) return WavetableKey{t->id, top.id, -1, -1};
        if ((size_t) chain >= top.chains.size() || child < 0 || (size_t) child >= top.chains[(size_t) chain].devices.size())
            return std::nullopt;
        return WavetableKey{t->id, top.id, chain, top.chains[(size_t) chain].devices[(size_t) child].id};
    }

    // ---- browser (UI-6)
    struct BrowserFile { std::string path; bool directory = false; };
    std::vector<BrowserFile> browserFiles;
    std::map<std::string, std::vector<BrowserFile>> browserFolderChildren;
    std::vector<std::string> browserPlaces;
    std::string browserFolder;
    bool browserLoading = false;
    std::function<void(const std::string&)> openBrowserFolder, expandBrowserFolder, previewSample;
    std::function<void()> addBrowserFolder;
    std::function<void()> insertMidiClip, toggleFullScreen;

    // ---- editing (UI-3 / UI-10): the selected Arrangement/Session clip and the document commands
    DetailTab detailTab = DetailTab::Devices;
    engine::MasterMonitor* monitor = nullptr; // analysis of the Main output (null in headless snapshots)
    int monitorView = 0;                      // Monitor tab: 0 spectrum bars, 1 spectrum curve, 2 oscilloscope
    int monitorScopeWindow = 2;               // index into the oscilloscope time windows
    bool monitorScopeTrigger = true;
    bool clipEnvelopesVisible = false;
    bool sampleFollow = false; // audio Clip view "Follow": the sample editor scrolls with the playhead
    int clipEnvelopeController = -1; // MIDI semantic index, independent of XML target IDs
    int selectedClipTrack = -1; // track index (== ControlBank index for tracks), -1 == none
    int selectedClipUid = -1;
    int selectedTakeLane = -1; // stable lane ID, independent of Session scene numbering
    std::vector<int> unfoldedTakeTracks; // local lane visibility, indexed by track
    std::vector<float> sessionColumnWidths; // per Session column (tracks, returns, main); 0 = default width
    std::vector<float> laneHeights;         // per Arrangement track; 0 = default height (UI only)
    std::vector<int> collapsedTracks;    // Arrangement lanes shrunk to a single row (UI only, not saved)
    bool showHiddenTracks = false;       // TRK-1: View > Show Hidden Tracks lists hidden tracks again (dimmed)
    int selectedClipScene = -1; // -1 Arrangement, otherwise exact Session slot
    bool scaleHighlight = false; // Scale Mode (view setting; starts from the set's InKey when a set is opened)
    // The set scale (UI-30b): Song::scaleRoot / scaleName, changed through an undoable edit.
    int scaleRoot() const { return song ? ((song->scaleRoot % 12) + 12) % 12 : 0; }
    int scaleName() const { return song ? song->scaleName : 0; }
    // The scale the note editors use: the selected clip's own (Live 12 clips carry one), else the set's.
    int editingScaleRoot() const { const auto* c = selectedClip(); return c ? ((c->scaleRoot % 12) + 12) % 12 : scaleRoot(); }
    int editingScaleName() const { const auto* c = selectedClip(); return c ? c->scaleName : scaleName(); }
    bool pitchInScale(int pitch) const { return model::inScale(pitch, editingScaleRoot(), editingScaleName()); }
    void setScale(int root, int name)
    {
        if (! song || ! applyEdit) return;
        if (auto command = model::setScale(*song, root, name); command.apply) applyEdit(std::move(command));
    }
    bool noteFold = false;
    bool clipMpeVisible = false;
    bool scaleFold = false; // show scale pitches plus any out-of-scale notes already in the clip
    bool pianoPreview = true;
    uint64_t clipZoomRequest = 0; // explicit clip open fits the editor, even when reopening the same clip
    bool recording = false;
    bool recordingIsSession = false; // the running/requested take records into Session slots (the circle button), not the Arrangement
    // ---- control-bar tools (Live's "+", automation arm, re-enable, capture, session record, draw mode, computer keyboard)
    bool arrangementOverdub = false; // MIDI Arrangement Overdub: Arrangement MIDI takes merge into the clips they overlap
    bool automationArm = true;       // moves of mixer / device controls are recorded as automation while recording to the Arrangement
    bool drawMode = false;           // Draw Mode (B): click-drag paints notes, envelope steps and automation
    bool computerKeyboard = false;   // the letter keys play the armed MIDI tracks (A W S E D F T G Y H U J K), Z/X octave, C/V velocity
    int computerKeyboardOctave = 3;  // octave of the key "A" (Live names MIDI note 60 "C3")
    int computerKeyboardVelocity = 100;
    // Maps a rectangle in root-widget coordinates to screen coordinates (the canvas sets it), so menus can drop down from the
    // control that opened them like Live's choosers. Unset (tests, ui_snapshot): menus open at the mouse.
    std::function<Rect(const Rect&)> rootToScreen;
    // Shows a list of choices (current ones ticked) and calls pick(index) for the chosen one. Unset: a native popup menu. Tests set it.
    std::function<void(const std::vector<std::string>& labels, const std::vector<bool>& ticked, std::function<void(int)> pick)> showChoices;
    std::function<void()> toggleSessionRecording;           // the circle button: Session recording regardless of the view
    std::function<void()> captureMidi;                       // retroactive MIDI recording of what was just played
    std::function<bool()> captureMidiAvailable;              // notes are waiting in the capture buffer
    std::function<void()> captureAudio;                      // TRK-4: retroactive audio recording of the armed audio inputs
    std::function<bool()> captureAudioAvailable;             // an armed (or selected) audio track has a kept input
    std::function<void()> reEnableAutomation;                // give overridden automated controls back to their lanes
    std::function<bool()> automationOverridden;              // some automated control was moved by hand
    std::function<void(int note, int velocity, bool on)> computerKeyboardNote; // routed to the armed tracks
    bool recordingRequested = false; // waiting for input permission/device setup
    std::function<void()> toggleRecording;
    std::function<void(int track, int scene)> recordSessionClip;
    std::function<int(int track)> recordingSessionScene;
    std::function<int(int)> sessionRecordingPhase; // track -1 queries the aggregate phase
    bool documentDirty = false; // edits since the last save
    std::function<void(model::EditCommand)> applyEdit; // performs, records for undo, refreshes engine + UI
    // Waveform overview of an audio clip's sample; nullptr while it is being computed (UI-4).
    std::function<std::shared_ptr<const engine::Peaks>(const model::Clip&)> peaks;
    std::function<void(const model::ClipLocation&)> reverseClip; // writes a reversed copy of the clip's sample and swaps it in
    std::function<void(const model::ClipLocation&)> toggleClipWarp; // first activation estimates source tempo on a worker
    std::function<void(const model::ClipLocation&)> separateStems; // splits the clip's sample into stems on new audio tracks; again while running = cancel
    std::function<void()> consolidateSelection;
    std::function<void(int track, bool freeze, bool toNewTrack)> renderTrack;
    std::function<void(const model::ClipLocation&, int mode)> convertAudioToMidi; // Melody, Harmony, Drums, Slices
    std::function<void(const model::ClipLocation&)> stripSilence; // TRK-3, Arrangement audio clips
    std::function<void(int direction)> jumpToTransient;         // TRK-3: insertion cursor to the next/previous transient
    std::function<void(const model::ClipLocation&)> resynthesizeClip;
    std::function<void(int track, int device, const std::string& path, int chain, int child)> importResynth;
    std::function<void(int track, int device, int chain, int child)> createResynthPhrase;
    bool stemsRunning = false;
    std::function<void(const model::ClipLocation&)> cropClip; // writes the loop region of the clip's sample to a new file and swaps it in
    std::function<void()> undo, redo, saveFile, saveFileAs, newProject, saveCopy, collectAllAndSave;
    std::function<void(bool midi)> insertTrack;
    std::function<void()> insertReturnTrack; // TRK-2: Option+Cmd+T
    std::function<void()> renameSelection;
    std::function<void(const std::string&)> setWindowTitle;
    std::function<void()> layoutChanged; // a panel size was changed by dragging a divider (persisted by the app)
    std::function<bool()> canUndo, canRedo;
    std::function<std::string()> undoLabel, redoLabel;

    // ---- COL-5 collab sessions (File > Share Session / Join Session, the top bar's people button)
    bool collabOpen = false; // the Collaborate panel
    CollabView collab;
    std::function<void(bool lan)> collabShare;            // host the open set on the local network or through the relay
    std::function<void()> collabJoinLink;                 // asks for an invite link, then joins
    std::function<void(const std::string& address)> collabJoinNearby; // a LAN session from the list: ask the host to let us in
    std::function<void()> collabLeave;                    // guests leave; the host ends the session
    std::function<void()> collabCopyInvite;
    std::function<void()> collabEditName, collabEditRelay;
    std::function<void(int id, CollabView::Role role)> collabSetRole;
    std::function<void(int id)> collabRemove;
    std::function<void(const std::string& peer, bool allow)> collabAnswerJoin;
    std::function<void(bool)> collabSetFollowTransport;
    // Views report where the pointer is (view 0 Arrangement, 1 Session, 2 clip editor; beat < 0 outside the timeline).
    std::function<void(int view, int trackIndex, double beat, int pitch)> collabPointer;

    // ---- MIDI map mode (Cmd+M): click a control, then move a knob / press a key on the controller
    bool midiMapMode = false;
    bool keyMapMode = false; // Cmd+K: assign unmodified letters/digits to Session clips
    // REQ-3a rebindable menu shortcuts: the app's map (nullptr in tools/tests without one), the hook that runs a rebound key
    // (true = handled), and the callback after an edit in Settings (persist + refresh menus).
    ShortcutMap* shortcuts = nullptr;
    std::function<bool(const KeyEvent&)> userShortcut;
    std::function<void()> shortcutsChanged;
    std::optional<midi::ControlTarget> mapSelection;
    std::function<void(const midi::ControlTarget&, const midi::ControlChange&)> mappedControl;

    // ---- preferences (PreferencesPanel). Rows are fetched on demand through the callbacks below.
    bool preferencesOpen = false;
    int settingsPage = 2;
    bool exportOpen = false;
    ExportSettings exportSettings;
    ExportState exportState;
    std::function<void(const ExportSettings&)> startExport; // asks for the destination, then renders on a worker
    std::function<void()> cancelExport;
    std::function<void(const std::string&)> revealInFinder;
    float uiZoom = 1.0f; // "Zoom Display": scales every metric (UI-11)
    enum class AudioSetting { Output, SampleRate, BufferSize };
    struct AudioSettings
    {
        std::string driver, input, output;
        std::vector<std::string> outputs;
        std::vector<double> sampleRates;
        std::vector<int> bufferSizes;
        double sampleRate = 0, inputLatencyMs = 0, outputLatencyMs = 0;
        int bufferSize = 0;
    };
    std::function<AudioSettings()> audioSettings;
    std::function<void(AudioSetting, const std::string&)> setAudioSetting;
    std::function<void()> loadTheme;
    std::function<void(bool)> useBuiltinTheme;
    std::function<void()> scanAudioUnits;
    std::string userLibraryPath, packsPath, temporaryProjectPath, settingsPath, pluginCachePath;
    std::function<void(bool packs)> chooseLibraryFolder;
    // COM-4 Settings > Updates, wired by MainWindow to app::Updater (empty in tools/tests: the page says so).
    struct UpdateSettings
    {
        bool available = false, automatic = false, canCheck = false, pending = false;
        std::string reason, version, backend;
        std::vector<std::string> channels; // display names; channel indexes into it
        int channel = 0;
    };
    std::function<UpdateSettings()> updateSettings;
    std::function<void()> checkForUpdates;
    std::function<void(bool)> setAutomaticUpdateChecks;
    std::function<void(int)> setUpdateChannel;
    bool browserPreviewEnabled = true;
    struct MidiInputRow
    {
        std::string id, name;
        bool track = false, remote = false;
    };
    struct MidiOutputRow
    {
        std::string id, name;
    };
    std::vector<std::string> pluginFolders;
    std::vector<std::string> failedPlugins; // scan hung / crashed / nothing loadable (blacklisted)
    bool scanning = false;
    std::string midiClockOutputId;
    bool midiClockEnabled = false;

    // ---- commands (wired by AppController; empty in headless tools)
    std::function<std::vector<MidiInputRow>()> midiInputRows;
    std::function<void(const std::string& id, bool track, bool remote)> setMidiInputRoles;
    std::function<std::vector<MidiOutputRow>()> midiOutputRows;
    std::function<void(const std::string& id, bool enabled)> setMidiClock;
    std::function<void()> clearFailedPlugins;
    std::function<void(float)> setUiZoom;
    std::function<void()> openFileDialog;
    std::function<void(const std::string& path)> openFile;
    std::function<void(int track, int device)> openPluginEditor;
    // Session launching (ENG-6): quantised by the set's global launch quantisation.
    std::function<void(int track, int scene)> launchClip;
    std::function<void(int track, int scene)> releaseClip; // mouse released on a Gate/Repeat launch button
    std::function<void(int track)> stopTrack;
    std::function<void(int scene)> launchScene;
    std::function<void()> stopAllClips;
    std::function<void()> backToArrangement;
    std::function<void()> scanPlugins;
    std::function<void()> showAudioSettings;
    std::function<void()> requestRepaint;
    // Must be called synchronously right after song/graph/controls change: views drop
    // pointers into the old graph before it can be deleted. Set by the window host.
    std::function<void()> modelChanged;

    // ---- helpers
    model::ClipLocation selectedClipLocation() const
    {
        if (selectedTakeLane >= 0) return model::ClipLocation::take(selectedClipTrack, selectedClipUid, selectedTakeLane);
        return selectedClipScene >= 0 ? model::ClipLocation::session(selectedClipTrack, selectedClipUid, selectedClipScene)
                                     : model::ClipLocation::arrangement(selectedClipTrack, selectedClipUid);
    }
    const model::Clip* selectedClip() const
    {
        return song ? model::findClip(*song, selectedClipLocation()) : nullptr;
    }
    // Changes whenever tracks are added, removed, reordered or regrouped, so views rebuild their per-track widgets.
    uint64_t trackStructure() const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        if (song == nullptr) return h;
        for (const auto& t : song->tracks) { mix((uint64_t) (int64_t) t.id); mix((uint64_t) t.kind); mix((uint64_t) (int64_t) t.groupId); }
        for (const auto& t : song->returns) mix((uint64_t) (int64_t) t.id);
        return h;
    }
    int numTracks() const { return song ? (int) song->tracks.size() : 0; }
    int numReturns() const { return song ? (int) song->returns.size() : 0; }
    int numControlTracks() const { return song ? numTracks() + numReturns() + 1 : 0; }
    const model::Track* trackAt(int controlIndex) const
    {
        if (song == nullptr || controlIndex < 0)
            return nullptr;
        if (controlIndex < numTracks())
            return &song->tracks[(size_t) controlIndex];
        if (controlIndex < numTracks() + numReturns())
            return &song->returns[(size_t) (controlIndex - numTracks())];
        if (controlIndex == numTracks() + numReturns())
            return &song->main;
        return nullptr;
    }
};

} // namespace sonora::ui
