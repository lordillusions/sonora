// ui_snapshot — renders the full Sonora UI off-screen (Skia raster) to a PNG.
// The quickest way for a human or an LLM to check visual changes without launching the app.
//
//   ui_snapshot out.png [set.als] [--session] [--track N] [--size 1440x900] [--scale 2] [--bench N [--gpu]]
//                       [--theme "/path/Live Theme.ask"] [--no-browser] [--no-detail]
//                       [--prefs plugins|midi|display|audio|keys|updates] [--zoom 1.25] [--midimap|--keymap] [--clip TRACK] [--scene N] [--count-in N]
//                       [--floating-wavetables] [--wavetable-window] [--waveform-zoom FACTOR]
//                       [--wavetable-browser] [--wavetable-preset NAME] [--wavetable-search TEXT]
//                       [--collab share|host|guest|presence] [--play-at BEAT] [--piano-rows PX] [--automation-track N]
//
// --play-at plays the transport from that song beat, or with --clip/--scene from that many beats into the Session clip,
// so playheads (Arrangement, piano roll, sample editor) show.
//
// --collab (COL-5) fills a synthetic session: "share" opens the Collaborate panel before a session (with two nearby
// sessions), "host"/"guest" open it during one (three people, an invite, a join request), "presence" closes it and draws
// two collaborators' pointers and selections over the Arrangement (or Session with --session).
//
// Sonora Prism editors render docked by default here; --floating-wavetables shows the app's default (a stub in the
// chain) and --wavetable-window renders the floating editor window of the selected track's first Wavetable (UI-27).
// --wavetable-browser opens the first Wavetable editor's preset browser (UI-28i); --wavetable-preset marks an installed
// Serum 2 preset as the loaded one (navigation state only: no importer runs here) and --wavetable-search types TEXT into
// the search field through RootWidget::dispatchKey.
//
// Without a .als it renders tests/fixtures/minimal_set.xml.

#include "core/AppDataFolder.h"
#include "ui/views/PianoRoll.h"
#include "devices/DeviceFactory.h"
#include "engine/EngineGraph.h"
#include "engine/MasterMonitor.h"
#include "engine/AudioFileFormats.h"
#include "engine/SampleCache.h"
#include "engine/PeakCache.h"
#include "engine/Transport.h"
#include "gfx/MetalRenderer.h"
#include "gfx/SkiaCanvasComponent.h"
#include "include/core/SkCanvas.h"
#include "gfx/Fonts.h"
#include "include/core/SkString.h"
#include "io/als/AlsReader.h"
#include "midi/MappingStore.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include "ui/Shortcuts.h"
#include "ui/views/MainLayout.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/ClipControls.h"
#include "ui/views/DetailView.h"
#include "ui/views/DeviceFace.h"
#include "ui/views/ResynthEditor.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/MonitorPanel.h"
#include "ui/views/PreferencesPanel.h"
#include "ui/views/WavetableWindowView.h"
#include "ui/views/WavetableEditor.h"
#include "plugins/SerumLibrary.h"
#include "ui/TrackMenu.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace sonora;

namespace
{
ui::WavetableEditor* firstWavetableEditor(ui::Widget& widget)
{
    if (auto* editor = dynamic_cast<ui::WavetableEditor*>(&widget)) return editor;
    for (const auto& child : widget.children())
        if (auto* found = firstWavetableEditor(*child)) return found;
    return nullptr;
}
// UI-28i: the preset browser state for a snapshot. False when no editor is shown.
bool openWavetableBrowser(ui::RootWidget& root, ui::UiContext& ctx, const std::string& preset, const std::string& search)
{
    auto* editor = firstWavetableEditor(root);
    if (editor == nullptr) return false;
    if (! preset.empty())
    {
        ctx.importWavetablePreset = [](int, int, const std::string&, int, int) {};
        for (const auto& item : plugins::serumLibrary("Presets"))
            if (item.name == preset) { editor->loadLibraryPreset(item.path); break; }
    }
    editor->openPresetBrowser();
    for (const char c : search)
    {
        ui::KeyEvent key; key.keyCode = (unsigned char) c; key.textChar = (unsigned char) c;
        root.dispatchKey(key);
    }
    root.layout();
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    int benchFrames = 0, monitorView = -1;
    bool benchGpu = false, benchSweep = true;
    bool wavetableWindow = false, wavetableBrowser = false;
    std::string wavetablePreset, wavetableSearch;
    bool trackMenu = false, arrangementMenu = false;
    double deviceDemoBeats = 0;
    std::string collabMode;
    float fileDragX = -1, fileDragY = -1;
    std::vector<std::string> fileDragPaths;
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: ui_snapshot out.png [set.als] [--session] [--track N] [--size WxH] [--scale S]"
                             " [--theme file.ask] [--no-browser] [--no-detail] [--envelopes] [--controller INDEX] [--recording TRACK:SCENE:PHASE]\n");
        return 2;
    }

    if (std::string(argv[1]) == "--font-report")
    {
        const float size = ui::Theme{}.metrics.fontSize;
        for (bool bold : {false, true})
        {
            const auto font = gfx::uiFont(size, bold);
            SkString family; font.getTypeface()->getFamilyName(&family);
            SkFontMetrics metrics; font.getMetrics(&metrics);
            std::printf("%s: %s weight=%d cap=%.2f x-height=%.2f Group=%.2f\n", bold ? "Emphasis" : "Regular",
                        family.c_str(), font.getTypeface()->fontStyle().weight(), (double) metrics.fCapHeight,
                        (double) metrics.fXHeight, (double) gfx::measureText("Group", size, bold));
        }
        return 0;
    }

    std::string out = argv[1];
    std::string als = SONORA_DEFAULT_FIXTURE;
    int width = 1440, height = 900;
    float scale = 2.0f, deviceScroll = -1;
    int devicePage = -1, deviceSection = -1;
    ui::UiContext ctx;
    midi::MidiManager mappingInput; // no hardware ports opened by the snapshot tool
    ctx.midi = &mappingInput;
    int prefsTab = 0;
    int midiTool = -1;
    int clipTrack = -1;
    int clipScene = -1, takeLane = -1;
    double playAt = -1;
    float pianoRows = 0; // piano roll row height (vertical zoom), 0 = default
    std::string browserFolder;
    std::string regionDrag;
    int browserCategory = -1, browserSelection = -1;
    std::string browserQuery;
    ui::BrowserTags::Tags browserFilters;
    std::vector<int> selectedTracks;
    struct RecordingState { int track = -1, scene = -1, phase = 0; };
    std::vector<RecordingState> recordingStates;

    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--track-menu") trackMenu = true;
        else if (a == "--arrangement-menu") arrangementMenu = true;
        else if (a == "--fold-scale") { ctx.scaleFold = ctx.scaleHighlight = true; }
        else if (a == "--session")
            ctx.mainView = ui::MainView::Session;
        else if (a == "--mpe") ctx.clipMpeVisible = true;
        else if (a == "--midi-tool") midiTool = std::stoi(next());
        else if (a == "--fold-notes") ctx.noteFold = true;
        else if (a == "--highlight-scale") ctx.scaleHighlight = true;
        else if (a == "--envelopes")
            ctx.clipEnvelopesVisible = true;
        else if (a == "--controller")
            ctx.clipEnvelopeController = std::stoi(next());
        else if (a == "--recording")
        {
            RecordingState state;
            if (std::sscanf(next().c_str(), "%d:%d:%d", &state.track, &state.scene, &state.phase) != 3
                || state.track < 0 || state.scene < 0 || state.phase < 1 || state.phase > 4)
            { std::fprintf(stderr, "--recording needs TRACK:SCENE:PHASE (phase 1..4)\n"); return 2; }
            recordingStates.push_back(state);
        }
        else if (a == "--automation")
            ctx.automationVisible = true;
        else if (a == "--automation-track") // one track's automation lane, as the A key shows it (UI-46)
        {
            ctx.automationVisible = true;
            ctx.automationTracks.push_back(std::stoi(next()));
        }
        else if (a == "--takes")
            ctx.unfoldedTakeTracks.push_back(std::stoi(next()));
        else if (a == "--take-lane")
            takeLane = std::stoi(next());
        else if (a == "--count-in")
            ctx.countInBars = std::stoi(next());
        else if (a == "--track")
            ctx.selectedTrack = std::stoi(next());
        else if (a == "--select-tracks")
            for (const auto& index : juce::StringArray::fromTokens(juce::String(next()), ",", ""))
                selectedTracks.push_back(index.getIntValue());
        else if (a == "--size")
            std::sscanf(next().c_str(), "%dx%d", &width, &height);
        else if (a == "--scale")
            scale = std::stof(next());
        else if (a == "--theme")
        {
            if (! ctx.theme.loadAsk(next()))
                std::fprintf(stderr, "warning: could not load theme\n");
        }
        else if (a == "--file-drag") // X:Y:PATH[,PATH...] Finder drag hovering at root coordinates (not dropped)
        {
            const auto value = juce::String(next());
            fileDragX = value.upToFirstOccurrenceOf(":", false, false).getFloatValue();
            const auto rest = value.fromFirstOccurrenceOf(":", false, false);
            fileDragY = rest.upToFirstOccurrenceOf(":", false, false).getFloatValue();
            for (const auto& path : juce::StringArray::fromTokens(rest.fromFirstOccurrenceOf(":", false, false), ",", ""))
                fileDragPaths.push_back(path.toStdString());
        }
        else if (a == "--browser-folder")
            browserFolder = next();
        else if (a == "--browser-width")
            ctx.theme.metrics.browserWidth = std::stof(next());
        else if (a == "--browser-filter")
        {
            const auto value = next(); const auto separator = value.find(':');
            if (separator == std::string::npos) { std::fprintf(stderr, "--browser-filter needs Group:Value\n"); return 2; }
            browserFilters.push_back({value.substr(0, separator), value.substr(separator + 1)});
        }
        else if (a == "--browser-category")
            browserCategory = std::stoi(next());
        else if (a == "--browser-select")
            browserSelection = std::stoi(next());
        else if (a == "--browser-query")
            browserQuery = next();
        else if (a == "--rack-chain")
            ctx.selectedRackChain = std::stoi(next());
        else if (a == "--no-browser")
            ctx.browserVisible = false;
        else if (a == "--no-mixer")
            ctx.mixerVisible = false;
        else if (a == "--no-sends")
            ctx.sendsVisible = false;
        else if (a == "--device-scroll")
            deviceScroll = std::stof(next());
        else if (a == "--device-page") // PAGE[:SECTION]: page (and section tab) of compact device faces, e.g. Simpler Controls = 1
        {
            const auto value = next();
            devicePage = std::stoi(value);
            if (const auto colon = value.find(':'); colon != std::string::npos) deviceSection = std::stoi(value.substr(colon + 1));
        }
        else if (a == "--no-detail")
            ctx.detailVisible = false;
        else if (a == "--lane-height")
            ctx.theme.metrics.arrangementLaneHeight = std::stof(next());
        else if (a == "--waveform-zoom")
        {
            ctx.waveformZoom = std::clamp(std::stof(next()), 0.01f, 100.0f);
            ctx.waveformZoomEnabled = true;
        }
        else if (a == "--detail-height") // taller detail view, to review panels that normally scroll
            ctx.theme.metrics.detailHeight = std::stof(next());
        else if (a == "--prefs")
        {
            const std::string tab = next();
            prefsTab = tab == "midi" ? 1 : tab == "display" ? 2 : tab == "audio" ? 3 : tab == "theme" ? 4
                       : tab == "files" ? 5 : tab == "library" ? 6 : tab == "record" ? 7 : tab == "keys" ? 8 : tab == "updates" ? 9 : 0;
            ctx.preferencesOpen = true;
        }
        else if (a == "--clip") // show the piano roll for the first clip of that track
        {
            clipTrack = std::stoi(next());
            ctx.detailTab = ui::DetailTab::Clip;
        }
        else if (a == "--scene")
            clipScene = std::stoi(next());
        else if (a == "--play-at")
            playAt = std::stod(next());
        else if (a == "--piano-rows")
            pianoRows = std::stof(next());
        else if (a == "--midimap")
            ctx.midiMapMode = true;
        else if (a == "--floating-wavetables")
            ctx.wavetableFloatingDefault = true;
        else if (a == "--wavetable-window")
            wavetableWindow = ctx.wavetableFloatingDefault = true;
        else if (a == "--wavetable-browser")
            wavetableBrowser = true;
        else if (a == "--wavetable-preset")
            wavetablePreset = next();
        else if (a == "--wavetable-search")
            wavetableSearch = next();
        else if (a == "--keymap")
            ctx.keyMapMode = true;
        else if (a == "--scroll") // arrangement scroll position in beats
            ctx.scrollBeats = std::stod(next());
        else if (a == "--region-drag") // start:end:firstTrack:lastTrack:beatDelta:trackOffset (uncommitted preview)
            regionDrag = next();
        else if (a == "--ppb") // arrangement zoom in pixels per beat
            ctx.pixelsPerBeat = std::stod(next());
        else if (a == "--zoom")
            ctx.uiZoom = std::stof(next());
        else if (a == "--bench") // time N frames through a horizontal zoom sweep instead of writing a picture
            benchFrames = std::stoi(next());
        else if (a == "--gpu") // with --bench: render through Skia/Metal off-screen like the app, not the CPU raster
            benchGpu = true;
        else if (a == "--no-sweep") // with --bench: keep the starting zoom/scroll (a repaint during playback)
            benchSweep = false;
        else if (a == "--collab")
            collabMode = next();
        else if (a == "--export") // Export dialog: idle | running | done | failed
        {
            const auto phase = next();
            ctx.exportOpen = true;
            ctx.exportState.phase = phase == "running" ? ui::ExportState::Phase::Running : phase == "done" ? ui::ExportState::Phase::Done
                                    : phase == "failed" ? ui::ExportState::Phase::Failed : ui::ExportState::Phase::Idle;
            ctx.exportState.progress = 0.42f;
            ctx.exportState.message = "Cannot write /Volumes/Backup/mix.wav (disk full?)";
            ctx.exportState.files = {"/Users/me/Music/Song.wav"};
            ctx.exportState.integratedLufs = -9.8f; ctx.exportState.loudnessRange = 6.4f;
            ctx.exportState.truePeakDb = -0.4f; ctx.exportState.samplePeakDb = -0.9f; ctx.exportState.seconds = 186.5;
            ctx.startExport = [](const ui::ExportSettings&) {};
        }
        else if (a == "--device-demo") // play BEATS of a demo pad through every native device first (scopes, meters, playheads)
            deviceDemoBeats = std::stod(next());
        else if (a == "--monitor") // Monitor tab with demo audio: bars | curve | scope
        {
            const auto view = next();
            monitorView = view == "scope" ? 2 : view == "curve" ? 1 : 0;
        }
        else
            als = a;
    }

    auto loaded = io::loadAls(juce::File(juce::String::fromUTF8(als.c_str())));
    if (! loaded)
    {
        std::fprintf(stderr, "error: %s\n", loaded.error.c_str());
        return 1;
    }
    for (int track : selectedTracks)
        if (track >= 0 && (size_t) track < loaded.song->tracks.size())
        {
            if (ctx.trackSelection.empty()) ctx.selectedTrack = track;
            ctx.trackSelection.insert(track);
            ctx.sessionTrackSelection = true;
        }

    // Headless graph: native devices are real (knobs show values), plugins are skipped.
    engine::SampleCache samples;
    devices::BuildContext bctx;
    auto graph = engine::buildGraph(*loaded.song, bctx, samples);
    graph.graph->prepare(48000.0, 512);
    if (deviceDemoBeats > 0)
    {
        const double tempo = loaded.song->tempo > 0 ? loaded.song->tempo : 120.0, start = loaded.song->currentTime;
        const int total = (int) (deviceDemoBeats * 60.0 / tempo * 48000.0);
        juce::AudioBuffer<float> block(2, 512);
        juce::MidiBuffer midi;
        for (auto& node : graph.graph->nodes)
            for (auto* device : node.processorForModelDevice)
            {
                if (device == nullptr) continue;
                for (int done = 0; done < total; done += 512)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const double t = (done + i) / 48000.0;
                        const double pulse = 0.75 + 0.25 * std::cos(2 * 3.14159265 * t * tempo / 60.0 * 2);
                        const float v = (float) (0.3 * pulse * (std::sin(2 * 3.14159265 * 110 * t) + 0.6 * std::sin(2 * 3.14159265 * 164.8 * t)
                                                                + 0.4 * std::sin(2 * 3.14159265 * 220.5 * t)));
                        block.setSample(0, i, v); block.setSample(1, i, v);
                    }
                    midi.clear();
                    device->setHostInfo({tempo, start + done / 48000.0 * tempo / 60.0, true, 48000.0});
                    device->process(block, midi);
                }
            }
    }

    engine::Transport transport;
    transport.setTempo(loaded.song->tempo);
    transport.setTimeSignature(loaded.song->timeSigNumerator, loaded.song->timeSigDenominator);
    transport.setLoop(loaded.song->loopOn, loaded.song->loopStart, loaded.song->loopLength);
    transport.setPosition(loaded.song->currentTime);
    transport.beginBlock(); // apply the seek

    engine::PeakCache peaks(samples); // synchronous: the snapshot must show finished waveforms
    ctx.peaks = [&](const model::Clip& clip) { return peaks.request(*loaded.song, clip); };
    ctx.song = loaded.song.get();
    ctx.transport = &transport;
    ctx.controls = graph.graph->controls;
    ctx.graph = graph.graph.get();
    ctx.buildLog = graph.log;
    if (trackMenu)
    {
        ctx.applyEdit = [](model::EditCommand) {};
        ctx.duplicateSelectedTracks = [] {};
        ctx.deleteSelectedTracks = [] {};
        ctx.renderTrack = [](int, bool, bool) {};
        ctx.consolidateSelection = [] {};
        ctx.convertAudioToMidi = [](const auto&, int) {};
        ctx.insertTrack = [](bool) {};
        ctx.renameSelection = [] {};
        ctx.groupSelectedTracks = [] {};
        auto image = ui::renderTrackMenuPreview(ctx, scale);
        juce::FileOutputStream stream{juce::File(juce::String(out))};
        stream.setPosition(0); stream.truncate();
        if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream(image, stream)) return 1;
        std::printf("wrote track menu preview %s\n", out.c_str());
        return 0;
    }
    ctx.statusText = "Snapshot of " + juce::File(juce::String::fromUTF8(als.c_str())).getFileName().toStdString();
    ctx.projectFiles = {als};
    if (browserCategory == 6)
    {
        plugins::PluginHost host; host.loadCache();
        for (const auto& p : host.knownPlugins().getTypes())
            ctx.browserPlugins.push_back({p.name.toStdString(), p.createIdentifierString().toStdString(),
                                           p.manufacturerName.toStdString(), p.pluginFormatName.toStdString(), p.isInstrument, p.category.toStdString()});
    }
    if (const auto* track = ctx.trackAt(ctx.selectedTrack))
        for (const auto& d : track->devices) if (d.kind == model::DeviceKind::Rack) { ctx.selectedRackId = d.id; break; }
    if (! collabMode.empty())
    {
        // COL-5: a synthetic session for checking the panel, the top-bar badge and remote cursors.
        auto& v = ctx.collab;
        v.networkAvailable = true;
        v.displayName = "Hana Okafor";
        v.relayUrl = "https://relay.sonora.example";
        ctx.collabOpen = collabMode != "presence";
        if (collabMode == "share")
        {
            v.lanSessions = {{"Ben's MacBook Pro: Night Drive", "bens-macbook-pro.local:47100"}, {"Studio B: Mixdown v3", "studio-b.local:47100"}};
            v.endedReason = "The host ended the session.";
        }
        else
        {
            const bool host = collabMode != "guest";
            v.state = ui::CollabView::State::Active;
            v.host = host;
            v.sessionName = "Night Drive";
            v.invite = "sonora://join/s_x7Qk2LpA9mZ0?relay=https%3A%2F%2Frelay.sonora.example&ticket=Jd1e...#d0Fz...";
            v.participants = {{0, host ? "Hana Okafor" : "Ben Laurent", ui::CollabView::Role::Host, 0, host, host ? "" : "amber-tiger-river-nine"},
                              {1, host ? "Ben Laurent" : "Hana Okafor", ui::CollabView::Role::Editor, 1, ! host, host ? "amber-tiger-river-nine" : ""},
                              {2, "Chiara Rossi", ui::CollabView::Role::Listener, 2, false, host ? "pearl-orbit-maple-zinc" : ""}};
            if (host) v.joinRequests = {{"lan-4", "Dmitri"}};
            v.notices = {"Ben Laurent joined the session", "Chiara Rossi joined the session",
                         host ? "Your edit was undone: Ben Laurent changed the same thing first" : "Downloading samples (12 of 48 MB)..."};
            if (! host) { v.status = "Downloading samples (12 of 48 MB)..."; v.progress = 0.25f; }
            const auto& tracks = loaded.song->tracks;
            if (! tracks.empty())
            {
                ui::CollabView::Cursor ben;
                ben.id = 1; ben.name = "Ben"; ben.color = 1; ben.view = ctx.mainView == ui::MainView::Session ? 1 : 0;
                ben.trackId = tracks.front().id; ben.beat = 6.5;
                if (! tracks.front().arrangementClips.empty()) ben.selectedClipUid = tracks.front().arrangementClips.front().uid;
                ui::CollabView::Cursor chiara;
                chiara.id = 2; chiara.name = "Chiara"; chiara.color = 2; chiara.view = ben.view;
                chiara.trackId = tracks.back().id; chiara.beat = 13.0; chiara.age = 12.0f;
                chiara.selectedTrackFrom = tracks.front().id; chiara.selectedTrackTo = tracks.back().id;
                chiara.selectionStart = 10.0; chiara.selectionEnd = 14.0;
                v.cursors = {ben, chiara};
            }
        }
    }

    if (! recordingStates.empty())
    {
        ctx.recording = true;
        ctx.recordingSessionScene = [&](int track) { for (const auto& state : recordingStates) if (state.track == track) return state.scene; return -1; };
        ctx.sessionRecordingPhase = [&](int track) {
            int phase = 0; for (const auto& state : recordingStates) if (track < 0 || state.track == track) phase = std::max(phase, state.phase); return phase;
        };
        ctx.statusText = "Recording phase preview: 1 queued, 2 recording, 3 stopping, 4 saving";
    }

    static ui::ShortcutMap demoShortcuts; // REQ-3a: a representative slice of the menu-bar registry, one rebound and one cleared
    if (ctx.preferencesOpen)
    {
        demoShortcuts.setCommands({
            {1, "Edit", "Undo", ui::KeyCombo::fromGlyphs("⌘Z")}, {2, "Edit", "Duplicate", ui::KeyCombo::fromGlyphs("⌘D")},
            {3, "Edit", "Freeze Track", ui::KeyCombo::fromGlyphs("⌥⇧⌘F")}, {4, "Edit", "Strip Silence", std::nullopt},
            {5, "Create", "Insert Audio Track", ui::KeyCombo::fromGlyphs("⌘T")}, {6, "Create", "Insert MIDI Track", ui::KeyCombo::fromGlyphs("⇧⌘T")},
            {7, "Create", "Insert Return Track", ui::KeyCombo::fromGlyphs("⌥⌘T")}, {8, "Create", "Capture Audio", std::nullopt},
            {9, "View", "Automation Mode", ui::KeyCombo::fromGlyphs("A")}, {10, "View", "Show Hidden Tracks", std::nullopt},
        });
        demoShortcuts.assign(4, ui::KeyCombo::fromText("Opt+Cmd+S"));
        demoShortcuts.assign(9, std::nullopt);
        ctx.shortcuts = &demoShortcuts;
        ctx.showAudioSettings = [] {};
        ctx.scanPlugins = ctx.scanAudioUnits = [] {};
        ctx.loadTheme = [] {};
        ctx.revealInFinder = [](const std::string&) {};
        ctx.chooseLibraryFolder = [](bool) {};
        ctx.updateSettings = [] { // COM-4: a configured Sparkle build on the stable channel
            ui::UiContext::UpdateSettings state;
            state.available = state.automatic = state.canCheck = true;
            state.version = "0.2.0"; state.backend = "Sparkle 2.9.6"; state.channels = {"Stable", "Beta"};
            return state;
        };
        ctx.checkForUpdates = [] {};
        ctx.setAutomaticUpdateChecks = [](bool) {};
        ctx.setUpdateChannel = [](int) {};
        ctx.audioSettings = [] {
            ui::UiContext::AudioSettings state;
            state.driver = "CoreAudio"; state.output = "Built-in Output";
            state.outputs = {"Built-in Output"}; state.sampleRate = 44100; state.bufferSize = 512;
            state.sampleRates = {44100, 48000, 96000}; state.bufferSizes = {64, 128, 256, 512, 1024};
            state.outputLatencyMs = 14.8; return state;
        };
        ctx.userLibraryPath = "~/Music/Sonora/User Library"; ctx.packsPath = "~/Music/Sonora/Packs";
        ctx.settingsPath = "~/Library/Application Support/" + core::appDataFolderName();
        ctx.temporaryProjectPath = ctx.settingsPath + "/Untitled Projects"; ctx.pluginCachePath = ctx.settingsPath + "/PluginCache.xml";
        // Demo rows so the layout can be reviewed without hardware.
        ctx.pluginFolders = {"/Library/Audio/Plug-Ins/VST3", "~/Library/Audio/Plug-Ins/VST3"};
        ctx.failedPlugins = {"/Library/Audio/Plug-Ins/VST3/Ozone 11 Imager.vst3", "/Library/Audio/Plug-Ins/VST3/ValhallaDelay.vst3"};
        ctx.pluginNames = {"Vital", "Serum 2", "Diva"};
        ctx.midiInputRows = [] {
            return std::vector<ui::UiContext::MidiInputRow>{{"a", "Launchkey MK3 49 MIDI", true, true}, {"b", "IAC Driver Bus 1", true, false}};
        };
        ctx.midiOutputRows = [] { return std::vector<ui::UiContext::MidiOutputRow>{{"o", "IAC Driver Bus 1"}}; };
    }
    if (clipTrack >= 0 && (size_t) clipTrack < ctx.song->tracks.size())
    {
        const auto& track = ctx.song->tracks[(size_t) clipTrack];
        const model::Clip* clip = nullptr;
        if (takeLane >= 0)
        {
            if (const auto* lane = model::findTakeLane(*ctx.song, clipTrack, takeLane))
                if (! lane->clips.empty()) clip = &lane->clips[0];
            ctx.unfoldedTakeTracks.push_back(clipTrack); ctx.selectedTakeLane = takeLane;
        }
        else if (clipScene >= 0)
        {
            if ((size_t) clipScene < track.slots.size() && track.slots[(size_t) clipScene].clip)
                clip = &*track.slots[(size_t) clipScene].clip;
        }
        else if (! track.arrangementClips.empty()) clip = &track.arrangementClips[0];
        if (clip)
        {
            ctx.selectedTrack = ctx.selectedClipTrack = clipTrack;
            ctx.selectedClipScene = clipScene;
            ctx.selectedClipUid = clip->uid;
            if (clipScene >= 0) ctx.selectedSessionScene = clipScene;
        }
    }
    if (playAt >= 0)
    {
        transport.setPosition(clipScene >= 0 ? 0.0 : playAt);
        transport.playNow();
        transport.beginBlock();
        if (clipScene >= 0 && ctx.controls && clipTrack >= 0 && (size_t) clipTrack < ctx.controls->size())
        {
            (*ctx.controls)[(size_t) clipTrack].playingSlot.store(clipScene);
            (*ctx.controls)[(size_t) clipTrack].sessionElapsed.store(playAt);
        }
    }
    if (ctx.keyMapMode || ctx.midiMapMode)
        if (const auto target = midi::sessionClipTarget(*ctx.song, ctx.selectedClipTrack, ctx.selectedClipScene))
        {
            midi::Mapping mapping; mapping.target = *target;
            if (ctx.keyMapMode) mapping.keyCode = 'A';
            else { mapping.channel = 2; mapping.isNote = true; mapping.number = 40; }
            mappingInput.addMapping(mapping);
            ctx.mapSelection = *target;
        }
    if (! browserFolder.empty())
    {
        ctx.openBrowserFolder = [&](const std::string& path) {
            ctx.browserFolder = path;
            ctx.browserFiles.clear();
            for (const auto& file : juce::File(path).findChildFiles(juce::File::findFilesAndDirectories, false))
                if (file.isDirectory() || file.hasFileExtension("als") || sonora::engine::isAudioFile(file))
                    ctx.browserFiles.push_back({file.getFullPathName().toStdString(), file.isDirectory()});
        };
    }
    engine::MasterMonitor monitor;
    if (monitorView >= 0)
    {
        // Demo programme: a bass line, a detuned chord and some noise, slightly different left and right.
        ctx.detailTab = ui::DetailTab::Monitor;
        ctx.monitorView = monitorView;
        ctx.monitor = &monitor;
        monitor.prepare(48000.0);
        monitor.setEnabled(true);
        std::vector<float> l(48000 * 6), r(l.size());
        unsigned seed = 12345;
        for (size_t i = 0; i < l.size(); ++i)
        {
            const double t = (double) i / 48000.0;
            seed = seed * 1664525u + 1013904223u;
            const double noise = ((double) (seed >> 8) / 8388608.0 - 1.0) * 0.02;
            const double bass = 0.32 * std::sin(6.2831853 * 55.0 * t) + 0.12 * std::sin(6.2831853 * 110.0 * t);
            const double chord = 0.07 * (std::sin(6.2831853 * 261.6 * t) + std::sin(6.2831853 * 329.6 * t) + std::sin(6.2831853 * 392.0 * t));
            const double lead = 0.05 * std::sin(6.2831853 * 1760.0 * t + 3.0 * std::sin(6.2831853 * 5.0 * t));
            l[i] = (float) (bass + chord + lead + noise);
            r[i] = (float) (bass + 0.8 * chord + 1.2 * lead - noise);
        }
        for (size_t i = 0; i < l.size(); i += 512)
        {
            monitor.push(l.data() + i, r.data() + i, (int) std::min<size_t>(512, l.size() - i));
            if ((i / 512) % 32 == 31)
                monitor.drainNow(); // keep up like the analysis thread does in the app
        }
        monitor.drainNow();
        monitor.setScope(ui::MonitorPanel::scopeWindowMs(ctx.monitorScopeWindow), ctx.monitorScopeTrigger);
    }
    if (wavetableWindow)
    {
        std::optional<ui::UiContext::WavetableKey> key;
        if (const auto* track = ctx.trackAt(ctx.selectedTrack))
            for (size_t d = 0; ! key && d < track->devices.size(); ++d)
            {
                if (track->devices[d].type == "SonoraWavetable") key = ctx.wavetableKey(ctx.selectedTrack, (int) d);
                for (size_t c = 0; ! key && c < track->devices[d].chains.size(); ++c)
                    for (size_t i = 0; ! key && i < track->devices[d].chains[c].devices.size(); ++i)
                        if (track->devices[d].chains[c].devices[i].type == "SonoraWavetable")
                            key = ctx.wavetableKey(ctx.selectedTrack, (int) d, (int) c, (int) i);
            }
        if (! key)
        {
            std::fprintf(stderr, "error: --wavetable-window needs a Sonora Prism on the selected track\n");
            return 1;
        }
        ui::WavetableWindowView window(ctx, *key);
        window.setBounds({0, 0, (float) width / ctx.uiZoom, (float) height / ctx.uiZoom});
        window.sync();
        if (wavetableBrowser && ! openWavetableBrowser(window, ctx, wavetablePreset, wavetableSearch))
        { std::fprintf(stderr, "error: --wavetable-browser found no Sonora Prism editor\n"); return 1; }
        if (! gfx::renderToPng(window, width, height, scale, out))
        {
            std::fprintf(stderr, "error: rendering failed\n");
            return 1;
        }
        std::printf("wrote %s (%dx%d @%.1fx)\n", out.c_str(), width, height, scale);
        return 0;
    }
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, (float) width / ctx.uiZoom, (float) height / ctx.uiZoom});
    root.layout();
    if (wavetableBrowser && ! openWavetableBrowser(root, ctx, wavetablePreset, wavetableSearch))
    { std::fprintf(stderr, "error: --wavetable-browser found no Sonora Prism editor\n"); return 1; }
    if (! regionDrag.empty())
    {
        double start = 0, end = 0, delta = 0;
        int first = 0, last = 0, offset = 0;
        if (std::sscanf(regionDrag.c_str(), "%lf:%lf:%d:%d:%lf:%d", &start, &end, &first, &last, &delta, &offset) != 6
            || ! model::canTransferArrangementRegion(*ctx.song, first, last, start, end, start + delta, offset))
        { std::fprintf(stderr, "error: invalid --region-drag destination\n"); return 1; }
        for (auto& child : root.children())
            if (auto* arrangement = dynamic_cast<ui::ArrangementView*>(child.get()))
            {
                const ui::ArrangementView::Lane* from = nullptr; const ui::ArrangementView::Lane* to = nullptr;
                for (const auto& lane : arrangement->lanes()) if (lane.takeLaneId < 0)
                {
                    if (lane.controlIndex == first) from = &lane;
                    if (lane.controlIndex == first + offset) to = &lane;
                }
                if (! from || ! to || ! arrangement->visible) { std::fprintf(stderr, "error: region lanes are not visible\n"); return 1; }
                const auto origin = arrangement->boundsInRoot();
                ctx.arrangementSelection = {true, start, end, first, last};
                ctx.applyEdit = [](model::EditCommand) {}; // no release/commit: preview only
                const double grab = start + std::min(1.0, (end - start) * 0.5);
                ui::MouseEvent down; down.x = origin.x + (float) ((grab - ctx.scrollBeats) * ctx.pixelsPerBeat); down.y = origin.y + from->y + 30;
                root.dispatchMouseDown(down);
                auto drag = down; drag.x += (float) (delta * ctx.pixelsPerBeat); drag.y = origin.y + to->y + 30;
                root.dispatchMouseDrag(drag);
            }
    }
    if (arrangementMenu)
        for (auto& child : root.children())
            if (auto* arrangement = dynamic_cast<ui::ArrangementView*>(child.get()))
            {
                ctx.applyEdit = [](model::EditCommand) {};
                ctx.arrangementSelection = {true, 4, 12, ctx.selectedTrack, ctx.selectedTrack};
                arrangement->zoomTimeSelection(false);
                float laneY = ctx.theme.metrics.arrangementRulerHeight + 20;
                for (const auto& lane : arrangement->lanes()) if (lane.controlIndex == ctx.selectedTrack) { laneY = lane.y + 20; break; }
                ui::MouseEvent point; point.x = 40; point.y = laneY;
                auto image = ui::renderGridMenuPreview(ctx.arrangementGrid, 4.0 * loaded.song->timeSigNumerator / loaded.song->timeSigDenominator, ctx.theme, arrangement->gridContextActions(point), scale);
                juce::FileOutputStream stream{juce::File(juce::String(out))}; stream.setPosition(0); stream.truncate();
                if (! stream.openedOk() || ! juce::PNGImageFormat().writeImageToStream(image, stream)) return 1;
                std::printf("wrote arrangement menu preview %s\n", out.c_str()); return 0;
            }
    if (browserCategory >= 0)
        for (auto& child : root.children())
            if (auto* browser = dynamic_cast<ui::BrowserPanel*>(child.get()))
            {
                browser->showCategory(std::clamp(browserCategory, 2, 6));
                if (! browserQuery.empty())
                {
                    browser->focusSearch();
                    for (const auto ch : browserQuery) { ui::KeyEvent key; key.textChar = (uint32_t) (unsigned char) ch; browser->keyPressed(key); }
                }
            }
    if (! browserFolder.empty())
        for (auto& child : root.children())
            if (auto* browser = dynamic_cast<ui::BrowserPanel*>(child.get())) browser->showFolder(browserFolder);
    for (auto& child : root.children())
        if (auto* browser = dynamic_cast<ui::BrowserPanel*>(child.get()))
            for (const auto& filter : browserFilters) browser->toggleTagFilter(filter);
    if (ctx.preferencesOpen)
        for (auto& child : root.children())
            if (auto* p = dynamic_cast<ui::PreferencesPanel*>(child.get()))
                p->setTab(static_cast<ui::PreferencesPanel::Tab>(prefsTab));
    if (browserSelection >= 0)
        for (auto& child : root.children())
            if (auto* browser = dynamic_cast<ui::BrowserPanel*>(child.get()))
            {
                const auto row = browser->listRowBounds(browserSelection), origin = browser->boundsInRoot();
                ui::MouseEvent event; event.x = origin.x + row.centreX(); event.y = origin.y + row.centreY();
                root.dispatchMouseDown(event); root.dispatchMouseUp(event);
            }
    if (! fileDragPaths.empty() && ! root.dispatchFileDragMove(fileDragX, fileDragY, fileDragPaths))
        std::fprintf(stderr, "warning: --file-drag paths hold no audio file\n");
    if (deviceScroll >= 0)
        for (auto& child : root.children())
            if (auto* detail = dynamic_cast<ui::DetailView*>(child.get())) detail->navigateOverview(deviceScroll);
    if (devicePage >= 0)
    {
        std::function<void(ui::Widget&)> select = [&](ui::Widget& widget) {
            if (auto* face = dynamic_cast<ui::DeviceFace*>(&widget)) { if (deviceSection >= 0) face->selectSection(deviceSection); face->selectPage(devicePage); }
            if (auto* resynth = dynamic_cast<ui::ResynthEditor*>(&widget)) resynth->selectPage(devicePage);
            if (auto* lfo = dynamic_cast<ui::CurveLfoEditor*>(&widget)) lfo->selectPage(devicePage);
            for (const auto& child : widget.children()) select(*child);
        };
        select(root);
    }
    if (midiTool >= 0)
    {
        std::function<void(ui::Widget&)> show = [&](ui::Widget& widget) {
            if (auto* panel = dynamic_cast<ui::ClipControls*>(&widget)) panel->showTool((ui::midiedit::Tool) std::clamp(midiTool, 0, 17));
            for (const auto& child : widget.children()) show(*child);
        };
        show(root);
    }
    if (pianoRows > 0)
    {
        std::function<void(ui::Widget&)> zoom = [&](ui::Widget& widget) {
            if (auto* roll = dynamic_cast<ui::PianoRoll*>(&widget)) roll->view.rowHeight = pianoRows;
            for (const auto& child : widget.children()) zoom(*child);
        };
        zoom(root);
    }
    if (benchFrames > 0)
    {
        const double basePpb = ctx.pixelsPerBeat, baseScroll = ctx.scrollBeats;
        auto sweep = [&](int i) {
            if (! benchSweep)
                return;
            // zoom in and out between 0.5x and 8x of the starting zoom, scrolling a little each frame
            const double phase = std::sin((double) i * 0.35);
            ctx.pixelsPerBeat = basePpb * std::pow(4.0, phase * 0.75 + 0.25);
            ctx.scrollBeats = baseScroll + (double) i * 0.05;
        };
        if (benchGpu)
        {
            const float zoom = std::clamp(ctx.uiZoom, 0.5f, 3.0f);
            const ui::Rect logical{0, 0, (float) width / zoom, (float) height / zoom};
            const auto gpu = gfx::benchmarkMetalOffscreen((int) std::lround((float) width * scale), (int) std::lround((float) height * scale),
                                                          benchFrames, [&](SkCanvas& canvas, int i) {
                                                              sweep(i);
                                                              root.setBounds(logical);
                                                              root.tickTree();
                                                              root.setBounds(logical);
                                                              canvas.scale(scale * zoom, scale * zoom);
                                                              root.paintTree(canvas, root.context().theme);
                                                          });
            if (! gpu.ok)
            {
                std::fprintf(stderr, "error: no Metal device\n");
                return 1;
            }
            const double n = std::max(1, gpu.frames);
            std::printf("bench %d frames (Metal): %.2f ms/frame on the CPU (worst %.2f), GPU wait %.2f ms/frame\n", gpu.frames,
                        gpu.cpuMs / n, gpu.worstCpuMs, gpu.gpuWaitMs / n);
            return 0;
        }
        const auto times = gfx::benchmarkFrames(root, width, height, scale, benchFrames, sweep);
        const double n = std::max(1, times.frames);
        std::printf("bench %d frames: %.2f ms/frame (tick %.2f, layout %.2f, paint %.2f), worst %.2f ms\n", times.frames,
                    (times.tickMs + times.layoutMs + times.paintMs) / n, times.tickMs / n, times.layoutMs / n, times.paintMs / n, times.worstFrameMs);
        return 0;
    }
    if (! gfx::renderToPng(root, width, height, scale, out))
    {
        std::fprintf(stderr, "error: rendering failed\n");
        return 1;
    }
    std::printf("wrote %s (%dx%d @%.1fx)\n", out.c_str(), width, height, scale);
    return 0;
}
