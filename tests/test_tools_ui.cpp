#include "TestFramework.h"

#include "engine/Transport.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "ui/RootWidget.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/TopBar.h"
#include "ui/views/SessionView.h"
#include "ui/views/ArrangementView.h"
#include "ui/TrackMenu.h"

#include <juce_core/juce_core.h>

using namespace sonora;

namespace
{
ui::MouseEvent mouseAt(float x, float y)
{
    ui::MouseEvent e;
    e.x = x;
    e.y = y;
    return e;
}

template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* hit = dynamic_cast<T*>(&w)) return hit;
    for (auto& child : w.children())
        if (auto* hit = findWidget<T>(*child)) return hit;
    return nullptr;
}

// TopBar children in creation order (see TopBar's constructor).
enum BarChild { Play, Stop, Record, Loop, Metronome, CountIn, PunchIn, PunchOut, Browser,
                Overdub, AutomationArm, ReEnable, Capture, SessionRecord, DrawMode, Keyboard, KeyMap };

struct BarRig
{
    model::Song song;
    engine::Transport transport;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    ui::TopBar* bar = nullptr;
    model::EditHistory history;

    BarRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
        REQUIRE(loaded);
        song = *loaded.song;
        transport.setLoop(song.loopOn, song.loopStart, song.loopLength);
        ctx.song = &song;
        ctx.transport = &transport;
        ctx.browserVisible = ctx.detailVisible = false;
        ctx.applyEdit = [this](model::EditCommand c) {
            history.perform(song, std::move(c));
            transport.setLoop(song.loopOn, song.loopStart, song.loopLength);
        };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1440, 900});
        root->layout();
        juce::TemporaryFile image(".png");
        REQUIRE(gfx::renderToPng(*root, 1440, 900, 1.0f, image.getFile().getFullPathName().toStdString()));
        bar = findWidget<ui::TopBar>(*root);
        REQUIRE(bar != nullptr);
    }
    void click(size_t child)
    {
        const auto b = bar->children()[child]->boundsInRoot();
        root->dispatchMouseDown(mouseAt(b.centreX(), b.centreY()));
        root->dispatchMouseUp(mouseAt(b.centreX(), b.centreY()));
    }
};
} // namespace

TEST_CASE("arrangement workflow: drag time selection, Cmd+L, Return then Space starts at the loop beginning")
{
    BarRig rig;
    auto* arrangement = findWidget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement);
    const auto bounds = arrangement->boundsInRoot();
    const float y = bounds.y + rig.ctx.theme.metrics.arrangementRulerHeight + 20;
    auto at = [&](double beat) { return mouseAt(bounds.x + (float) ((beat - rig.ctx.scrollBeats) * rig.ctx.pixelsPerBeat), y); };
    rig.root->dispatchMouseDown(at(20)); rig.root->dispatchMouseDrag(at(28)); rig.root->dispatchMouseUp(at(28));
    CHECK(rig.ctx.arrangementSelection.active);
    REQUIRE(rig.root->dispatchKey({.textChar = 'l', .cmd = true}));
    CHECK(rig.song.loopOn); CHECK_NEAR(rig.song.loopStart, 20, 1e-9); CHECK_NEAR(rig.song.loopLength, 8, 1e-9);
    rig.transport.setPosition(26); rig.transport.beginBlock();
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
    auto reset = rig.transport.beginBlock(); CHECK(! reset.playing); CHECK_NEAR(reset.position, 20, 1e-9);
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::spaceKey}));
    auto start = rig.transport.beginBlock(); CHECK(start.playing); CHECK_NEAR(start.position, 20, 1e-9);

    // Loop switch off with a different time selection: Return still goes to the loop brace's start.
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::spaceKey})); rig.transport.beginBlock();
    rig.transport.setLoopEnabled(false);
    rig.root->dispatchMouseDown(at(24)); rig.root->dispatchMouseDrag(at(30)); rig.root->dispatchMouseUp(at(30));
    CHECK(rig.ctx.arrangementSelection.active); CHECK(rig.ctx.arrangementSelection.start > 20.5);
    rig.transport.setPosition(30); rig.transport.beginBlock();
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::spaceKey}));
    auto again = rig.transport.beginBlock(); CHECK(again.playing); CHECK_NEAR(again.position, 20, 1e-9);
}

TEST_CASE("view selectors: right-side icon buttons switch and carry hover tooltips; track Cmd+D routes separately")
{
    BarRig rig;
    for (const auto& child : rig.bar->children())
        if (const auto* text = dynamic_cast<ui::TextButton*>(child.get()))
            CHECK(text->label() != "Back to Arr.");
    ui::IconButton* session = nullptr; ui::IconButton* arrangement = nullptr;
    for (const auto& child : rig.root->children())
        if (auto* icon = dynamic_cast<ui::IconButton*>(child.get()))
        {
            if (icon->icon() == ui::IconButton::Icon::SessionView) session = icon;
            if (icon->icon() == ui::IconButton::Icon::ArrangementView) arrangement = icon;
        }
    REQUIRE(session); REQUIRE(arrangement);
    CHECK(session->tooltip == "Session View"); CHECK(arrangement->tooltip == "Arrangement View");
    CHECK(session->bounds().x > rig.root->width() - 60);
    CHECK_NEAR(session->bounds().y, arrangement->bounds().y, 1e-6);
    CHECK(session->bounds().y < rig.ctx.theme.metrics.controlBarHeight);
    CHECK(session->bounds().right() < arrangement->bounds().x);
    const auto b = session->boundsInRoot();
    rig.root->dispatchMouseDown(mouseAt(b.centreX(), b.centreY())); rig.root->dispatchMouseUp(mouseAt(b.centreX(), b.centreY()));
    CHECK(rig.ctx.mainView == ui::MainView::Session);
    int duplicates = 0; rig.ctx.duplicateSelectedTracks = [&] { ++duplicates; };
    rig.ctx.sessionTrackSelection = true; rig.ctx.selectedClipUid = -1;
    REQUIRE(rig.root->dispatchKey({.textChar = 'd', .cmd = true})); CHECK(duplicates == 1);
    rig.ctx.sessionTrackSelection = false;
    rig.root->dispatchKey({.textChar = 'd', .cmd = true}); CHECK(duplicates == 1);
}

TEST_CASE("control bar tools: overdub, automation arm, draw mode, computer keyboard, session record, capture and re-enable")
{
    BarRig rig;
    auto& x = rig.ctx;
    CHECK(! x.arrangementOverdub);
    rig.click(Overdub);
    CHECK(x.arrangementOverdub);
    rig.click(Overdub);
    CHECK(! x.arrangementOverdub);

    CHECK(x.automationArm); // Live starts with automation arm on
    rig.click(AutomationArm);
    CHECK(! x.automationArm);
    rig.click(AutomationArm);
    CHECK(x.automationArm);

    CHECK(! x.drawMode);
    rig.click(DrawMode);
    CHECK(x.drawMode);
    rig.click(DrawMode);
    CHECK(! x.drawMode);

    // the computer keyboard and Session clip key mapping exclude each other
    x.keyMapMode = true;
    rig.click(Keyboard);
    CHECK(x.computerKeyboard);
    CHECK(! x.keyMapMode);
    rig.click(Keyboard);
    CHECK(! x.computerKeyboard);

    int sessionRecords = 0, captures = 0, reEnables = 0;
    x.toggleSessionRecording = [&] { ++sessionRecords; x.recording = true; x.recordingIsSession = true; };
    rig.click(SessionRecord);
    CHECK(sessionRecords == 1);

    // Capture MIDI and Re-Enable are inert until there is something to capture / re-enable
    x.captureMidi = [&] { ++captures; };
    rig.click(Capture);
    CHECK(captures == 0);
    bool available = true;
    x.captureMidiAvailable = [&] { return available; };
    rig.click(Capture);
    CHECK(captures == 1);
    available = false;
    rig.click(Capture);
    CHECK(captures == 1);

    bool overridden = false;
    x.reEnableAutomation = [&] { ++reEnables; overridden = false; };
    x.automationOverridden = [&] { return overridden; };
    rig.click(ReEnable);
    CHECK(reEnables == 0);
    overridden = true;
    rig.click(ReEnable);
    CHECK(reEnables == 1);
    CHECK(! overridden);
}

TEST_CASE("control bar tools: the Key and MIDI buttons enter the mapping modes, punch icons and loop fields edit the loop")
{
    BarRig rig;
    auto& x = rig.ctx;
    rig.click(KeyMap);
    CHECK(x.keyMapMode);
    rig.click(KeyMap);
    CHECK(! x.keyMapMode);
    const auto midiX = 1440.0f - 8.0f - 54.0f - 20.0f, barY = 15.0f; // MIDI box before the view buttons
    rig.root->dispatchMouseDown(mouseAt(midiX, barY));
    rig.root->dispatchMouseUp(mouseAt(midiX, barY));
    CHECK(x.midiMapMode);
    rig.root->dispatchMouseDown(mouseAt(midiX, barY));
    rig.root->dispatchMouseUp(mouseAt(midiX, barY));
    CHECK(! x.midiMapMode);

    rig.click(PunchIn);
    CHECK(rig.song.punchIn);
    rig.click(PunchOut);
    CHECK(rig.song.punchOut);

    // Loop start / length: drag a field up to raise it by a step (left third = bars). The edit is one undo step.
    const double length = rig.transport.loopLength(), start = rig.transport.loopStart();
    const auto lengthBox = rig.bar->children()[PunchOut]->boundsInRoot();
    const float fx = lengthBox.right() + 2 + 8.0f, fy = lengthBox.centreY(); // left third of the length box
    rig.root->dispatchMouseDown(mouseAt(fx, fy));
    rig.root->dispatchMouseDrag(mouseAt(fx, fy - 12.0f)); // two 6 px steps up
    rig.root->dispatchMouseUp(mouseAt(fx, fy - 12.0f));
    CHECK_NEAR(rig.transport.loopLength(), length + 2 * 4.0, 1e-9);
    CHECK_NEAR(rig.song.loopLength, length + 8.0, 1e-9);
    CHECK_NEAR(rig.transport.loopStart(), start, 1e-9);
    rig.history.undo(rig.song);
    CHECK_NEAR(rig.song.loopLength, length, 1e-9);

    const auto startBox = rig.bar->children()[PunchIn]->boundsInRoot();
    const float sx = startBox.x - 2 - 8.0f; // right third of the start box = sixteenths
    rig.root->dispatchMouseDown(mouseAt(sx, fy));
    rig.root->dispatchMouseDrag(mouseAt(sx, fy - 6.0f));
    rig.root->dispatchMouseUp(mouseAt(sx, fy - 6.0f));
    CHECK_NEAR(rig.transport.loopStart(), start + 0.25, 1e-9);
}

TEST_CASE("computer MIDI keyboard: letters play notes, held keys do not repeat, Z/X/C/V change octave and velocity")
{
    BarRig rig;
    auto& x = rig.ctx;
    struct Played { int note, velocity; bool on; };
    std::vector<Played> played;
    x.computerKeyboardNote = [&](int note, int velocity, bool on) { played.push_back({note, velocity, on}); };
    ui::KeyEvent a;
    a.keyCode = 'A';
    a.textChar = 'a';
    rig.root->dispatchKey(a);
    CHECK(played.empty()); // off by default: 'a' is the automation shortcut

    x.computerKeyboard = true;
    CHECK(rig.root->dispatchKey(a));
    REQUIRE(played.size() == 1);
    CHECK(played[0].note == 60); // C3 in Live's naming
    CHECK(played[0].velocity == 100);
    CHECK(played[0].on);
    rig.root->dispatchKey(a); // OS key repeat
    CHECK(played.size() == 1);
    ui::KeyEvent w; w.keyCode = 'W'; w.textChar = 'w';
    rig.root->dispatchKey(w);
    REQUIRE(played.size() == 2);
    CHECK(played[1].note == 61);
    CHECK(rig.root->dispatchKeyUp('A'));
    REQUIRE(played.size() == 3);
    CHECK(played[2].note == 60);
    CHECK(! played[2].on);
    CHECK(rig.root->heldLaunchKeys().size() == 1); // W is still down

    ui::KeyEvent z; z.keyCode = 'Z'; z.textChar = 'z';
    rig.root->dispatchKey(z);
    CHECK(x.computerKeyboardOctave == 2);
    ui::KeyEvent v; v.keyCode = 'V'; v.textChar = 'v';
    rig.root->dispatchKey(v);
    CHECK(x.computerKeyboardVelocity == 120); // +20, clamped at 127
    rig.root->dispatchKey(v);
    CHECK(x.computerKeyboardVelocity == 127);
    ui::KeyEvent k; k.keyCode = 'K'; k.textChar = 'k';
    played.clear();
    rig.root->dispatchKey(k);
    REQUIRE(played.size() == 1);
    CHECK(played[0].note == 2 * 12 + 24 + 12); // octave 2: C0 base 48... K is the C an octave above A
    CHECK(played[0].velocity == 127);
    rig.root->releaseMappedKeys(); // focus loss releases everything that sounds
    CHECK(rig.root->heldLaunchKeys().empty());
    CHECK(! played.back().on);

    // Cmd+key combinations stay shortcuts
    ui::KeyEvent cmdA = a; cmdA.cmd = true;
    played.clear();
    rig.root->dispatchKey(cmdA);
    CHECK(played.empty());
}

TEST_CASE("draw mode: dragging in the note editor paints a note per grid cell, clicking a note removes it, B toggles the mode")
{
    BarRig rig;
    auto& x = rig.ctx;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    rig.song = *loaded.song;
    x.detailVisible = true;
    x.detailTab = ui::DetailTab::Clip;
    x.mainView = ui::MainView::Arrangement;
    x.selectedClipTrack = 0;
    x.selectedClipUid = rig.song.tracks[0].arrangementClips[0].uid;
    rig.root->layout();
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1440, 900, 1.0f, image.getFile().getFullPathName().toStdString()));
    auto* roll = findWidget<ui::PianoRoll>(*rig.root);
    REQUIRE(roll != nullptr);
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    const size_t before = clip.notes.size();
    const auto area = roll->boundsInRoot();
    const double ppb = roll->view.pixelsPerBeat;
    auto xAt = [&](double beat) { return area.x + 40.0f + (float) ((beat - roll->view.scrollBeats) * ppb); };
    auto yAt = [&](int pitch) { return area.y + roll->noteAreaBounds().y + (float) ((roll->view.topPitch - pitch) * roll->view.rowHeight) + 5.0f; };

    ui::KeyEvent b; b.keyCode = 'B'; b.textChar = 'b';
    rig.root->dispatchMouseDown(mouseAt(xAt(3.1), yAt(60) + 60)); // focus the editor on empty space first
    rig.root->dispatchMouseUp(mouseAt(xAt(3.1), yAt(60) + 60));
    rig.root->dispatchKey(b);
    CHECK(x.drawMode);

    const double cell = ui::gridBeats(x.clipGrid, ppb, 4.0); // the adaptive grid follows the newly fitted clip zoom
    rig.root->dispatchMouseDown(mouseAt(xAt(5.0) + 1, yAt(62)));
    rig.root->dispatchMouseDrag(mouseAt(xAt(5.0 + 3 * cell) + 1, yAt(62))); // across four cells, fast
    rig.root->dispatchMouseUp(mouseAt(xAt(5.0 + 3 * cell) + 1, yAt(62)));
    REQUIRE(clip.notes.size() == before + 4);
    int found = 0;
    for (const auto& n : clip.notes)
        if (n.pitch == 62 && n.start >= 5.0 - 1e-9 && n.start < 6.0 - 1e-9) { ++found; CHECK_NEAR(n.duration, cell, 1e-9); }
    CHECK(found == 4);

    // clicking one of the painted notes removes it
    rig.root->dispatchMouseDown(mouseAt(xAt(5.0 + cell) + 3, yAt(62)));
    rig.root->dispatchMouseUp(mouseAt(xAt(5.0 + cell) + 3, yAt(62)));
    CHECK(clip.notes.size() == before + 3);
    rig.history.undo(rig.song);
    rig.history.undo(rig.song);
    CHECK(clip.notes.size() == before);

    rig.root->dispatchKey(b);
    CHECK(! x.drawMode);
}

#include "ui/views/BrowserPanel.h"

TEST_CASE("browser: Up/Down walk the samples and audition them, the content-column headphone button turns preview off and on")
{
    ui::UiContext ctx;
    ctx.detailVisible = false;
    ctx.browserPlaces = {"/tmp/Samples"};
    ctx.browserFiles = {{"/tmp/Samples/a.wav", false}, {"/tmp/Samples/b.wav", false}, {"/tmp/Samples/c.wav", false}};
    std::string previewed = "(none)";
    ctx.previewSample = [&](const std::string& path) { previewed = path; };
    ctx.openBrowserFolder = [&](const std::string& path) { ctx.browserFolder = path; };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    ui::BrowserPanel* browser = findWidget<ui::BrowserPanel>(root);
    REQUIRE(browser != nullptr);
    const auto origin = browser->boundsInRoot();
    auto click = [&](float x, float y) {
        root.dispatchMouseDown(mouseAt(origin.x + x, origin.y + y));
        root.dispatchMouseUp(mouseAt(origin.x + x, origin.y + y));
    };
    const auto place = browser->sidebarRowBounds(13); // the custom Place
    click(place.centreX(), place.centreY());
    const auto first = browser->listRowBounds(0);
    click(first.centreX(), first.centreY());
    CHECK(previewed == "/tmp/Samples/a.wav");

    ui::KeyEvent down; down.keyCode = juce::KeyPress::downKey;
    ui::KeyEvent up; up.keyCode = juce::KeyPress::upKey;
    root.dispatchKey(down);
    CHECK(previewed == "/tmp/Samples/b.wav");
    root.dispatchKey(down);
    CHECK(previewed == "/tmp/Samples/c.wav");
    root.dispatchKey(down); // stays on the last row
    CHECK(previewed == "/tmp/Samples/c.wav");
    root.dispatchKey(up);
    CHECK(previewed == "/tmp/Samples/b.wav");
    CHECK(ctx.selectedTrack == 0); // the arrows belong to the browser, they did not select another track

    // The headphone button belongs to the list column and moves with the browser splitter.
    const auto button = browser->previewButton();
    const auto footer = browser->previewFooter();
    CHECK_NEAR(footer.x, ctx.theme.metrics.browserSidebarWidth, 0.01);
    CHECK(button.x > footer.x);
    CHECK(button.right() < footer.right());
    CHECK(footer.w < browser->width());
    click(button.centreX(), button.centreY());
    CHECK(previewed.empty());
    root.dispatchKey(up);
    CHECK(previewed.empty()); // walking the list is silent now
    click(button.centreX(), button.centreY());
    CHECK(previewed == "/tmp/Samples/a.wav"); // switching it on auditions the selected sample again
}

TEST_CASE("tempo field: double-click and type a value, Enter commits, Escape cancels, the wheel nudges it, dragging still works")
{
    BarRig rig;
    auto& x = rig.ctx;
    const float cx = 8 + 26 + 10 + 36 + 2 + 27, cy = 15.0f; // the tempo box right of the tap box
    auto typeKey = [&](char c) { ui::KeyEvent k; k.textChar = (uint32_t) c; rig.root->dispatchKey(k); };
    auto special = [&](int code) { ui::KeyEvent k; k.keyCode = code; rig.root->dispatchKey(k); };
    ui::MouseEvent dbl = mouseAt(cx, cy);
    dbl.clicks = 2;
    rig.root->dispatchMouseDown(dbl);
    rig.root->dispatchMouseUp(dbl);
    for (char c : std::string("98.5")) typeKey(c);
    special(juce::KeyPress::returnKey);
    CHECK_NEAR(rig.transport.tempo(), 98.5, 1e-9);
    CHECK_NEAR(rig.song.tempo, 98.5, 1e-9); // one undoable edit
    rig.history.undo(rig.song);
    CHECK_NEAR(rig.song.tempo, 128.0, 1e-9);

    // typing replaces, letters are ignored while typing, Escape leaves the value alone
    rig.transport.setTempo(120.0);
    rig.root->dispatchMouseDown(dbl);
    rig.root->dispatchMouseUp(dbl);
    for (char c : std::string("1a4b0")) typeKey(c);
    special(juce::KeyPress::escapeKey);
    CHECK_NEAR(rig.transport.tempo(), 120.0, 1e-9);
    // out-of-range entries clamp
    rig.root->dispatchMouseDown(dbl);
    rig.root->dispatchMouseUp(dbl);
    for (char c : std::string("5")) typeKey(c);
    special(juce::KeyPress::returnKey);
    CHECK_NEAR(rig.transport.tempo(), 20.0, 1e-9);

    // mouse wheel over the field: up raises, Shift is fine
    rig.transport.setTempo(100.0);
    ui::MouseEvent wheel = mouseAt(cx, cy);
    wheel.wheelDeltaY = 0.1f;
    rig.root->dispatchMouseWheel(wheel);
    CHECK(rig.transport.tempo() > 100.5);
    const double before = rig.transport.tempo();
    wheel.shift = true;
    rig.root->dispatchMouseWheel(wheel);
    CHECK(rig.transport.tempo() > before);
    CHECK(rig.transport.tempo() < before + 0.2);

    // while typing, letters are not shortcuts (the computer keyboard / automation key stay quiet)
    x.automationVisible = false;
    rig.root->dispatchMouseDown(dbl);
    rig.root->dispatchMouseUp(dbl);
    typeKey('a');
    CHECK(! x.automationVisible);
    special(juce::KeyPress::escapeKey);
}

#include "ui/views/PreferencesPanel.h"
#include "ui/views/ArrangementView.h"

TEST_CASE("Settings: category sidebar routes real controls and modal clipping prevents hidden actions")
{
    ui::UiContext ctx;
    ctx.preferencesOpen = true;
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1000, 700});
    auto* settings = findWidget<ui::PreferencesPanel>(root); REQUIRE(settings);
    juce::TemporaryFile file(".png");
    auto draw = [&] { REQUIRE(gfx::renderToPng(root, 1000, 700, 1, file.getFile().getFullPathName().toStdString())); };
    auto click = [&](const std::string& label) {
        draw(); const auto r = settings->controlBounds(label); REQUIRE(! r.isEmpty());
        root.dispatchMouseDown(mouseAt(r.centreX(), r.centreY())); root.dispatchMouseUp(mouseAt(r.centreX(), r.centreY()));
    };
    draw();
    const auto frame = findWidget<ui::ArrangementView>(root)->boundsInRoot();
    const auto settingsImage = juce::ImageFileFormat::loadFrom(file.getFile());
    CHECK(settingsImage.getPixelAt((int) frame.x + 1, (int) settings->panelRect().bottom() - 10).getARGB() == ctx.theme.color("SurfaceBackground"));
    int scans = 0, auScans = 0, retries = 0, folder = -1, config = 0;
    ctx.setUiZoom = [&](float zoom) { ctx.uiZoom = zoom; };
    click("Zoom In"); CHECK_NEAR(ctx.uiZoom, 1.1, 1e-6);
    click("Reset Zoom"); CHECK_NEAR(ctx.uiZoom, 1, 1e-6);
    click("Computer MIDI Keyboard"); CHECK(ctx.computerKeyboard);
    ctx.useBuiltinTheme = [&](bool dark) { ctx.theme.useBuiltin(dark); };
    click("Theme & Colors"); CHECK(ctx.settingsPage == (int) ui::PreferencesPanel::Tab::Theme);
    click("Dark"); CHECK(ctx.theme.sourceName == "Built-in Dark");
    click("Light"); CHECK(ctx.theme.sourceName == "Built-in Light");
    ctx.showAudioSettings = [&] { ++config; };
    click("Audio"); click("Channel Configuration"); CHECK(config == 1);
    ctx.midiInputRows = [] { return std::vector<ui::UiContext::MidiInputRow>{{"a", "Controller", true, false}}; };
    ctx.setMidiInputRoles = [&](const std::string& id, bool track, bool remote) { CHECK(id == "a"); CHECK(track); CHECK(remote); ++config; };
    click("Tempo & MIDI"); click("Remote a"); CHECK(config == 2);
    ctx.chooseLibraryFolder = [&](bool packs) { folder = packs ? 1 : 0; };
    click("Library"); click("Location of User Library"); CHECK(folder == 0);
    click("Folder for Packs"); CHECK(folder == 1);
    ctx.scanPlugins = [&] { ++scans; }; ctx.scanAudioUnits = [&] { ++auScans; }; ctx.clearFailedPlugins = [&] { ++retries; };
    click("Plug-Ins"); click("Rescan Plug-Ins"); CHECK(scans == 1);
    click("Scan Audio Units"); CHECK(auScans == 1);
    click("Retry Failed Plug-Ins"); CHECK(retries == 1); CHECK(auScans == 2);
    ctx.scanning = true; draw(); CHECK(settings->controlBounds("Rescan Plug-Ins").isEmpty());
    ctx.scanning = false;
    ctx.failedPlugins.assign(100, "/tmp/failed.vst3"); draw();
    const auto panel = settings->panelRect();
    ui::MouseEvent wheel = mouseAt(panel.right() - 80, panel.centreY()); wheel.wheelDeltaY = -100;
    root.dispatchMouseWheel(wheel); draw(); CHECK(settings->controlBounds("Retry Failed Plug-Ins").isEmpty());
    // A category switch resets the content scroll without losing the sidebar or executing off-screen buttons.
    click("Display & Input"); draw(); CHECK(! settings->controlBounds("Zoom In").isEmpty()); CHECK(retries == 1);
    root.dispatchMouseDown(mouseAt(2, 2)); root.dispatchMouseUp(mouseAt(2, 2)); CHECK(ctx.preferencesOpen);
    ui::KeyEvent escape; escape.keyCode = juce::KeyPress::escapeKey; root.dispatchKey(escape); CHECK(! ctx.preferencesOpen);
}

TEST_CASE("Settings: built-in themes preserve dragged layout metrics and restore light colors")
{
    ui::Theme theme;
    const auto light = theme.color("SurfaceBackground");
    theme.metrics.detailHeight = 317;
    theme.useBuiltin(true); CHECK(theme.color("SurfaceBackground") != light); CHECK_NEAR(theme.metrics.detailHeight, 317, 1e-6);
    theme.useBuiltin(false); CHECK(theme.color("SurfaceBackground") == light); CHECK_NEAR(theme.metrics.detailHeight, 317, 1e-6);
}

TEST_CASE("browser disclosure: folder arrows expand inline, Right is idempotent, Left selects the parent and folds")
{
    ui::UiContext ctx;
    ctx.browserPlaces = {"/tmp/Tree"};
    ctx.browserFiles = {{"/tmp/Tree/Folder", true}, {"/tmp/Tree/last.wav", false}};
    ctx.browserFolderChildren["/tmp/Tree/Folder"] = {{"/tmp/Tree/Folder/child.wav", false}};
    int requests = 0, drops = 0;
    std::string preview;
    ctx.openBrowserFolder = [&](const auto& path) { ctx.browserFolder = path; };
    ctx.expandBrowserFolder = [&](const auto&) { ++requests; };
    ctx.previewSample = [&](const auto& path) { preview = path; };
    ctx.dropBrowserItem = [&](const auto&, int, double, int) { ++drops; };
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    auto* browser = findWidget<ui::BrowserPanel>(root); REQUIRE(browser);
    auto click = [&](ui::Rect r, bool arrow = false) {
        const auto origin = browser->boundsInRoot();
        auto e = mouseAt(origin.x + (arrow ? r.x + 12 : r.centreX()), origin.y + r.centreY());
        root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };
    auto key = [&](int code) { root.dispatchKey({.keyCode = code}); };
    click(browser->sidebarRowBounds(13));
    click(browser->listRowBounds(0), true);
    CHECK(ctx.browserFolder == "/tmp/Tree"); CHECK(requests == 0); // cached children
    key(juce::KeyPress::rightKey); key(juce::KeyPress::rightKey);
    REQUIRE(gfx::renderToPng(root, 1200, 700, 2, "/tmp/sonora-browser-disclosure.png"));
    key(juce::KeyPress::downKey);
    CHECK(preview == "/tmp/Tree/Folder/child.wav");
    key(juce::KeyPress::leftKey); // parent selection
    key(juce::KeyPress::leftKey); // collapse
    key(juce::KeyPress::downKey);
    CHECK(preview == "/tmp/Tree/last.wav");
    CHECK(drops == 0);
    browser->showCategory(4); // device group: Right must never load a device
    click(browser->listRowBounds(0), true);
    key(juce::KeyPress::rightKey); key(juce::KeyPress::downKey); key(juce::KeyPress::rightKey);
    CHECK(drops == 0);
}

TEST_CASE("shortcuts: view toggles, focus navigation, global zoom and quantization match their menu actions")
{
    BarRig rig;
    auto& x = rig.ctx;
    auto key = [&](uint32_t ch, bool cmd = false, bool alt = false, bool shift = false, int code = 0) {
        return rig.root->dispatchKey({.keyCode = code, .textChar = ch, .shift = shift, .cmd = cmd, .alt = alt});
    };
    x.mainView = ui::MainView::Session; rig.root->layout();
    auto* session = findWidget<ui::SessionView>(*rig.root); REQUIRE(session);
    rig.root->setKeyboardFocus(session);
    key(0, false, false, true, juce::KeyPress::tabKey);
    CHECK(x.mainView == ui::MainView::Session); CHECK(x.detailVisible); CHECK(x.detailTab == ui::DetailTab::Clip);
    key(0, false, false, false, juce::KeyPress::F12Key);
    CHECK(x.detailTab == ui::DetailTab::Devices);
    key('4', true, true); CHECK(! x.detailVisible);
    key('4', true, true); CHECK(x.detailVisible);
    key('m', true, true); CHECK(! x.mixerVisible);
    key('s', true, true); CHECK(! x.sendsVisible);
    key('5', true, true); CHECK(x.browserVisible);
    key('2', false, true); CHECK(x.mainView == ui::MainView::Arrangement);
    CHECK(rig.root->keyboardFocus() == findWidget<ui::ArrangementView>(*rig.root));
    key('1', false, true); CHECK(x.mainView == ui::MainView::Session); CHECK(rig.root->keyboardFocus() == session);
    key('+', true); CHECK_NEAR(x.uiZoom, 1.1, 0.0001);
    key('-', true); CHECK_NEAR(x.uiZoom, 1.0, 0.0001);
    for (auto [ch, expected] : {std::pair{'6', 11}, {'7', 9}, {'8', 7}, {'9', 4}, {'0', 0}})
    { key((uint32_t) ch, true); CHECK(rig.song.globalQuantisation == expected); }
    int inserted = 0, arrangementRecord = 0, sessionRecord = 0;
    x.insertMidiClip = [&] { ++inserted; }; x.toggleRecording = [&] { ++arrangementRecord; }; x.toggleSessionRecording = [&] { ++sessionRecord; };
    key('M', true, false, true); CHECK(inserted == 1);
    key(0, false, false, false, juce::KeyPress::F9Key);
    key(0, true, false, true, juce::KeyPress::F9Key);
    CHECK(arrangementRecord == 1); CHECK(sessionRecord == 1);
    key('m'); CHECK(x.computerKeyboard);
    key('b', false, false, true); CHECK(x.drawMode); // Shift makes single-letter shortcuts available during keyboard input
    key('m'); CHECK(! x.computerKeyboard);
}

TEST_CASE("context commands: track menus match selection and clip actions keep their original identity")
{
    BarRig rig;
    auto& ctx = rig.ctx;
    ctx.selectedTrack = 0;
    ctx.groupSelectedTracks = [] {};
    auto actions = ui::trackContextActions(ctx);
    REQUIRE(actions.size() > 10);
    CHECK(actions[0].label == "Cut"); CHECK(! actions[0].enabled);
    CHECK(actions[1].label == "Copy"); CHECK(actions[4].label.empty()); CHECK(actions[5].label == "Rename");
    int renames = 0;
    ctx.renameSelection = [&] { ++renames; };
    actions = ui::trackContextActions(ctx);
    const auto rename = actions[5].run;
    rename(); CHECK(renames == 1);
    const auto originalId = rig.song.tracks[0].id;
    rig.song.tracks[0].id += 1000; rename(); CHECK(renames == 1);
    rig.song.tracks[0].id = originalId;
    model::Clip clip; clip.uid = model::allocateClipUid(); clip.currentEnd = 4;
    rig.song.tracks[0].arrangementClips.push_back(clip);
    const auto location = model::ClipLocation::arrangement(0, clip.uid);
    auto commands = ui::clipContextActions(ctx, location);
    REQUIRE(commands.size() > 8);
    CHECK(commands[3].label == "Duplicate");
    auto duplicate = commands[3].run;
    const auto before = rig.song.tracks[0].arrangementClips.size();
    duplicate(); CHECK(rig.song.tracks[0].arrangementClips.size() == before + 1);
    rig.history.undo(rig.song);
    CHECK(rig.song.tracks[0].arrangementClips.size() == before);
    ctx.recording = true;
    commands = ui::clipContextActions(ctx, location);
    CHECK(! commands[3].enabled); CHECK(! commands[4].enabled);
    ctx.recording = false;
    std::erase_if(rig.song.tracks[0].arrangementClips, [&](const auto& current) { return current.uid == clip.uid; });
    duplicate(); // deleted identity cannot edit a different clip
    CHECK(rig.song.tracks[0].arrangementClips.size() == before - 1);
}

TEST_CASE("track menu: supported insert duplicate content and take actions are distinct from unavailable rendering workflows")
{
    BarRig rig;
    rig.ctx.duplicateSelectedTracks = [] {};
    rig.ctx.insertTrack = [](bool) {};
    auto actions = ui::trackContextActions(rig.ctx);
    auto action = [&](const char* label) -> ui::MenuAction {
        for (const auto& item : actions) if (item.label == label) return item;
        REQUIRE(false); return {};
    };
    CHECK(action("Duplicate").enabled); CHECK(action("Insert Audio Track").enabled); CHECK(action("Insert MIDI Track").enabled);
    CHECK(! action("Freeze Track").enabled); CHECK(! action("Bounce Track in Place").enabled); CHECK(! action("Insert Return Track").enabled);
    auto show = action("Show Take Lanes"); REQUIRE(show.enabled); show.run();
    CHECK(rig.ctx.unfoldedTakeTracks == std::vector<int>{0}); show.run(); CHECK(rig.ctx.unfoldedTakeTracks.empty());
    auto insert = action("Insert Take Lane"); REQUIRE(insert.enabled); insert.run();
    REQUIRE(rig.song.tracks[0].takeLanes.size() == 1); CHECK(rig.song.tracks[0].takeLanes[0].clips.empty());
    CHECK(rig.history.undoLabel() == "Insert Take Lane");
    juce::TemporaryFile file(".als"); REQUIRE(io::saveAls(rig.song, file.getFile()).ok);
    auto saved = io::loadAls(file.getFile()); REQUIRE(saved); CHECK(saved.song->tracks[0].takeLanes.size() == 1);
    REQUIRE(rig.history.undo(rig.song)); CHECK(rig.song.tracks[0].takeLanes.empty());
    auto content = action("Select Track Content"); REQUIRE(content.enabled); content.run();
    CHECK(rig.ctx.arrangementSelection.active);
    CHECK_NEAR(rig.ctx.arrangementSelection.start, rig.song.tracks[0].arrangementClips[0].currentStart, 1e-9);
    auto menu = ui::createTrackContextMenu(rig.ctx);
    juce::PopupMenu::MenuItemIterator items(menu);
    bool palette = false;
    while (items.next()) if (items.getItem().customComponent) { int w = 0, h = 0; items.getItem().customComponent->getIdealSize(w, h); CHECK(w == 250); CHECK(h == 91); palette = true; }
    CHECK(palette);
    rig.ctx.recording = true;
    for (const auto& item : ui::trackContextActions(rig.ctx))
        if (item.label == "Duplicate" || item.label == "Insert Audio Track" || item.label == "Insert MIDI Track") CHECK(! item.enabled);
}

TEST_CASE("shortcut focus: duplicate notes and velocity edits stay in the note editor, grouping never groups tracks")
{
    BarRig rig;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    rig.song = *loaded.song;
    auto& ctx = rig.ctx;
    ctx.detailVisible = true; ctx.detailTab = ui::DetailTab::Clip;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    ctx.selectedClipTrack = 0; ctx.selectedClipUid = clip.uid;
    rig.root->layout();
    auto* roll = findWidget<ui::PianoRoll>(*rig.root); REQUIRE(roll);
    rig.root->setKeyboardFocus(roll);
    auto key = [&](uint32_t ch, bool cmd = false, bool shift = false, int code = 0) {
        return rig.root->dispatchKey({.keyCode = code, .textChar = ch, .shift = shift, .cmd = cmd});
    };
    key('A', true);
    const auto count = clip.notes.size(); REQUIRE(count > 0);
    const int pitch = clip.notes.front().pitch;
    const float velocity = clip.notes.front().velocity;
    key(0, true, false, juce::KeyPress::upKey);
    CHECK(clip.notes.front().pitch == pitch);
    CHECK_NEAR(clip.notes.front().velocity, std::min(127.0f, velocity + 1), 0.001);
    key('D', true); CHECK(clip.notes.size() == count * 2);
    CHECK(rig.history.undoLabel() == "Duplicate Notes");
    key('D', true); CHECK(clip.notes.size() == count * 3); // only the selected copies duplicate again
    int grouped = 0; ctx.groupSelectedTracks = [&] { ++grouped; };
    key('G', true); CHECK(grouped == 0);
    CHECK(ctx.statusText == "Note grouping is not implemented");
    const auto clips = rig.song.tracks[0].arrangementClips.size();
    roll->keyPressed({.keyCode = juce::KeyPress::deleteKey});
    CHECK(rig.song.tracks[0].arrangementClips.size() == clips);
    const float headerX = rig.root->width() - ctx.theme.metrics.outerMargin - ctx.arrangementHeaderWidth() + 30;
    const float headerY = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.arrangementRulerHeight + 8;
    rig.root->dispatchMouseDown(mouseAt(headerX, headerY)); rig.root->dispatchMouseUp(mouseAt(headerX, headerY));
    CHECK(ctx.selectedClipUid == -1);
    const auto remainingId = rig.song.tracks[1].id;
    const auto remainingClips = rig.song.tracks[1].arrangementClips.size();
    key(0, false, false, juce::KeyPress::deleteKey);
    REQUIRE(rig.song.tracks.size() == 1);
    CHECK(rig.song.tracks[0].id == remainingId);
    CHECK(rig.song.tracks[0].arrangementClips.size() == remainingClips);
}

#include "ui/widgets/Knob.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Buttons.h"

TEST_CASE("control keyboard focus: Delete resets a parameter, arrows adjust it, right drags and button releases never edit")
{
    ui::UiContext ctx;
    ui::RootWidget root(ctx);
    float value = 0.7f, gain = 0.2f;
    auto* knob = root.add<ui::Knob>("Mix", ui::Knob::Source{[&] { return value; }, [&](float v) { value = v; }, 0, 1, 0.5f, false, {}});
    root.setKeyboardFocus(knob);
    root.dispatchKey({.keyCode = juce::KeyPress::deleteKey}); CHECK_NEAR(value, 0.5, 0.001);
    root.dispatchKey({.keyCode = juce::KeyPress::upKey}); CHECK(value > 0.5f);
    auto event = mouseAt(10, -100); event.right = true;
    const auto before = value; knob->mouseDrag(event); CHECK_NEAR(value, before, 0.001);
    auto* fader = root.add<ui::Fader>(); fader->getGain = [&] { return gain; }; fader->setGain = [&](float v) { gain = v; };
    root.setKeyboardFocus(fader);
    root.dispatchKey({.keyCode = juce::KeyPress::deleteKey}); CHECK_NEAR(gain, 1, 0.001);
    fader->mouseDrag(event); CHECK_NEAR(gain, 1, 0.001);
    int toggles = 0;
    auto* button = root.add<ui::TextButton>("Arm"); button->setBounds({0, 0, 20, 20}); button->onClick = [&] { ++toggles; };
    event.x = event.y = 10; button->mouseDown(event); button->mouseUp(event); CHECK(toggles == 0);
    root.setBounds({0, 0, 200, 200});
    root.dispatchMouseDown(event); event.right = false; root.dispatchMouseUp(event); CHECK(toggles == 0);
}
