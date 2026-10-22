#include "TestFramework.h"

#include "engine/EngineGraph.h"
#include "engine/ClipLauncher.h"
#include "engine/PeakCache.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/Painter.h"
#include "ui/Theme.h"
#include "ui/views/MainLayout.h"
#include "ui/RootWidget.h"
#include "ui/views/ClipControls.h"
#include "ui/views/SampleView.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/SessionView.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Knob.h"

#include <juce_core/juce_core.h>

using namespace sonora;

namespace
{
ui::PianoRoll* findRoll(ui::Widget& w)
{
    if (auto* found = dynamic_cast<ui::PianoRoll*>(&w)) return found;
    for (const auto& child : w.children()) if (auto* found = findRoll(*child)) return found;
    return nullptr;
}
ui::ClipControls* findAudioControls(ui::Widget& w)
{
    if (auto* found = dynamic_cast<ui::ClipControls*>(&w)) return found;
    for (auto& child : w.children()) if (auto* found = findAudioControls(*child)) return found;
    return nullptr;
}
ui::SampleView* findSampleView(ui::Widget& w)
{
    if (auto* found = dynamic_cast<ui::SampleView*>(&w)) return found;
    for (auto& child : w.children()) if (auto* found = findSampleView(*child)) return found;
    return nullptr;
}
std::pair<float, float> audioControl(ui::RootWidget& root, ui::ClipControls::Id id)
{
    auto* panel = findAudioControls(root);
    REQUIRE(panel != nullptr);
    auto* rootWidget = &root;
    // The panel scrolls: bring the control into view with real wheel events, repainting so the layout follows.
    for (int i = 0; i < 40; ++i)
    {
        juce::TemporaryFile png(".png");
        REQUIRE(gfx::renderToPng(root, 1200, 700, 1.0f, png.getFile().getFullPathName().toStdString()));
        const auto r = panel->controlBounds(id);
        REQUIRE(! r.isEmpty());
        const bool above = r.y < 20.0f, below = r.bottom() > panel->height() - 4;
        if ((! above && ! below) || ! rootWidget)
            break;
        const auto origin = panel->boundsInRoot();
        ui::MouseEvent wheel;
        wheel.x = origin.x + 20; wheel.y = origin.y + 100; wheel.wheelDeltaY = above ? 0.1f : -0.1f; // about 50 px per notch
        rootWidget->dispatchMouseWheel(wheel);
    }
    const auto r = panel->controlBounds(id);
    const auto origin = panel->boundsInRoot();
    return {origin.x + r.centreX(), origin.y + r.centreY()};
}
} // namespace

TEST_CASE("fader mapping is monotonic and round-trips")
{
    CHECK(ui::Fader::gainToPosition(0.0f) == 0.0f);
    CHECK_NEAR(ui::Fader::gainToPosition(1.99526f), 1.0, 1e-4);
    float last = -1;
    for (float g : {0.001f, 0.01f, 0.1f, 0.5f, 1.0f, 1.5f})
    {
        const float p = ui::Fader::gainToPosition(g);
        CHECK(p > last);
        last = p;
        CHECK_NEAR(ui::Fader::positionToGain(p), g, 1e-4);
    }
}

TEST_CASE("theme: defaults and .ask import")
{
    ui::Theme t;
    CHECK(t.color("SurfaceArea") == SkColorSetRGB(0x57, 0x5b, 0x67));
    CHECK(t.color("NoSuchToken") == SK_ColorMAGENTA);

    const auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_test_theme.ask");
    tmp.replaceWithText(R"(<?xml version="1.0" encoding="UTF-8"?>
<Ableton><Theme><SurfaceArea Value="#102030" /><ShadowDark Value="#11223344" /></Theme></Ableton>)");
    REQUIRE(t.loadAsk(tmp.getFullPathName().toStdString()));
    CHECK(t.color("SurfaceArea") == SkColorSetRGB(0x10, 0x20, 0x30));
    CHECK(t.color("ShadowDark") == SkColorSetARGB(0x44, 0x11, 0x22, 0x33));
    tmp.deleteFile();
}

TEST_CASE("ui: full layout renders to PNG without a song")
{
    ui::UiContext ctx;
    ui::MainLayout root(ctx);
    const auto out = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_empty.png");
    REQUIRE(gfx::renderToPng(root, 1200, 800, 1.0f, out.getFullPathName().toStdString()));
    CHECK(out.getSize() > 1000);
    out.deleteFile();
}

TEST_CASE("text truncation removes whole UTF-8 code points")
{
    std::string s = "Caf\xC3\xA9 \xE2\x82\xAC\xF0\x9F\x8E\xB5"; // "Café €🎵" (1, 2, 3 and 4 byte characters)
    ui::paint::popLastCodePoint(s);
    CHECK(s == "Caf\xC3\xA9 \xE2\x82\xAC");
    ui::paint::popLastCodePoint(s);
    CHECK(s == "Caf\xC3\xA9 ");
    ui::paint::popLastCodePoint(s);
    ui::paint::popLastCodePoint(s);
    CHECK(s == "Caf");
    std::string empty;
    ui::paint::popLastCodePoint(empty); // must not underflow
    CHECK(empty.empty());
}

#include "ui/ClipEditing.h"

TEST_CASE("clip editing: grid follows zoom, drags snap, edges are recognised")
{
    CHECK_NEAR(ui::gridBeatsFor(200.0, 4.0), 0.125, 1e-9);
    CHECK_NEAR(ui::gridBeatsFor(50.0, 4.0), 0.5, 1e-9);
    CHECK_NEAR(ui::gridBeatsFor(14.0, 4.0), 1.0, 1e-9);
    CHECK_NEAR(ui::gridBeatsFor(2.0, 4.0), 4.0, 1e-9); // far out: whole bars
    CHECK_NEAR(ui::snapToGrid(5.4, 1.0), 5.0, 1e-9);
    CHECK_NEAR(ui::snapToGrid(5.6, 0.5), 5.5, 1e-9);
    CHECK_NEAR(ui::snapToGrid(5.4, 0.0), 5.4, 1e-9); // no grid

    CHECK(ui::hitZone(2.0f, 100.0f) == ui::DragMode::TrimStart);
    CHECK(ui::hitZone(97.0f, 100.0f) == ui::DragMode::ResizeEnd);
    CHECK(ui::hitZone(50.0f, 100.0f) == ui::DragMode::Move);
    CHECK(ui::hitZone(1.0f, 8.0f) == ui::DragMode::TrimStart); // tiny clip: edges shrink

    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.currentStart = 4.0;
    c.currentEnd = 8.0;
    c.loop = {0.0, 4.0, 0.0, true};
    auto moved = ui::dragClip(c, ui::DragMode::Move, 1.3, 1.0, true, 120.0);
    CHECK_NEAR(moved.currentStart, 5.0, 1e-9);
    CHECK_NEAR(moved.currentEnd, 9.0, 1e-9); // keeps its length
    auto free = ui::dragClip(c, ui::DragMode::Move, 1.3, 1.0, false, 120.0);
    CHECK_NEAR(free.currentStart, 5.3, 1e-9);
    auto cut = ui::dragClip(c, ui::DragMode::ResizeEnd, -0.9, 1.0, true, 120.0);
    CHECK_NEAR(cut.currentEnd, 7.0, 1e-9);
    auto trimmed = ui::dragClip(c, ui::DragMode::TrimStart, 1.2, 1.0, true, 120.0);
    CHECK_NEAR(trimmed.currentStart, 5.0, 1e-9);
    CHECK_NEAR(trimmed.loop.startRelative, 1.0, 1e-9);
    auto negative = ui::dragClip(c, ui::DragMode::Move, -10.0, 1.0, true, 120.0);
    CHECK_NEAR(negative.currentStart, 0.0, 1e-9); // never before the start of the timeline
}

#include "io/als/AlsReader.h"
#include "io/als/AlsNewSet.h"
#include "ui/RootWidget.h"

TEST_CASE("arrangement editing: select, move, resize, delete, duplicate and undo through real mouse and key events")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 14.0;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ctx.undo = [&] { history.undo(song); };

    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();

    const auto& clip0 = song.tracks[0].arrangementClips[0]; // beats 4..12 in the fixture
    const float topBar = ctx.theme.metrics.controlBarHeight;
    const float laneTop = topBar + ctx.theme.metrics.arrangementRulerHeight; // track 0's lane starts here
    auto mouse = [&](float x, float y) {
        ui::MouseEvent e;
        e.x = x;
        e.y = y;
        return e;
    };
    const float panelX = ctx.theme.metrics.outerMargin; // the main view starts after the window margin
    auto beatX = [&](double b) { return panelX + (float) (b * ctx.pixelsPerBeat); };
    const float y = laneTop + 30;

    // click selects the clip
    root.dispatchMouseDown(mouse(beatX(8.0), y));
    root.dispatchMouseUp(mouse(beatX(8.0), y));
    CHECK(ctx.selectedClipUid == clip0.uid);
    CHECK(history.canUndo() == false); // a plain click does not edit

    // Drag the title bar 2 beats to the right -> moved to 6..14
    root.dispatchMouseDown(mouse(beatX(8.0), laneTop + 6));
    root.dispatchMouseDrag(mouse(beatX(10.0), laneTop + 6));
    root.dispatchMouseUp(mouse(beatX(10.0), laneTop + 6));
    REQUIRE(history.canUndo());
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 6.0, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 14.0, 1e-9);

    // drag the right edge (in the title bar, UI-45) 2 beats left -> 6..12
    const float rightEdge = beatX(14.0) - 2;
    root.dispatchMouseDown(mouse(rightEdge, laneTop + 6));
    root.dispatchMouseDrag(mouse(rightEdge - (float) (2.0 * ctx.pixelsPerBeat), laneTop + 6));
    root.dispatchMouseUp(mouse(rightEdge - (float) (2.0 * ctx.pixelsPerBeat), laneTop + 6));
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 12.0, 1e-9);

    // undo twice restores the original
    ui::KeyEvent z;
    z.cmd = true;
    z.textChar = 'z';
    root.dispatchKey(z);
    root.dispatchKey(z);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 4.0, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 12.0, 1e-9);

    // Cmd+D duplicates after the clip; the copy is selected
    ui::KeyEvent d;
    d.cmd = true;
    d.textChar = 'd';
    root.dispatchMouseDown(mouse(beatX(8.0), y));
    root.dispatchMouseUp(mouse(beatX(8.0), y));
    root.dispatchKey(d);
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, 12.0, 1e-9);
    CHECK(ctx.selectedClipUid == song.tracks[0].arrangementClips[1].uid);

    // Delete removes the selected copy
    ui::KeyEvent del;
    del.keyCode = juce::KeyPress::deleteKey;
    root.dispatchKey(del);
    CHECK(song.tracks[0].arrangementClips.size() == 1);

    // Double-click on an empty MIDI lane inserts a clip (track 1 is an empty MIDI track)
    const float lane1 = laneTop + ctx.theme.metrics.arrangementLaneHeight + 20;
    ui::MouseEvent dbl = mouse(beatX(16.0), lane1);
    dbl.clicks = 2;
    root.dispatchMouseDown(dbl);
    root.dispatchMouseUp(dbl);
    REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, 16.0, 1e-9);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentEnd, 20.0, 1e-9);

    // Dragging a clip onto the other MIDI lane moves it to that track
    root.dispatchMouseDown(mouse(beatX(8.0), laneTop + 6));
    root.dispatchMouseDrag(mouse(beatX(8.0), lane1));
    root.dispatchMouseUp(mouse(beatX(8.0), lane1));
    CHECK(song.tracks[0].arrangementClips.empty());
    CHECK(song.tracks[1].arrangementClips.size() == 2);
}

TEST_CASE("piano roll: select, move, resize, add, velocity, transpose and delete notes through real input events")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ctx.detailTab = ui::DetailTab::Clip;
    ctx.selectedClipTrack = 0;
    ctx.selectedClipUid = song.tracks[0].arrangementClips[0].uid;

    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1.0f, png.getFile().getFullPathName().toStdString())); // first paint frames the clip
    auto* piano = findRoll(root); REQUIRE(piano); piano->view.pixelsPerBeat = 40; // explicit zoom for these gesture coordinates

    // Geometry (see PianoRoll.cpp): detail view at y = 700 - 22 - 220, content 22 px below; piano roll notes
    // area starts at x = 40, y = +16; 40 px per beat; row height 11; the top row is highest pitch + 4 (= 69).
    const auto& metrics = ctx.theme.metrics;
    const float roll = 700.0f - metrics.statusBarHeight - metrics.detailHeight + 22.0f;
    const float notesTop = roll + piano->noteAreaBounds().y;
    const float notesLeft = [&] { auto* r = findRoll(root); return r ? r->boundsInRoot().x + r->noteAreaBounds().x : 0.0f; }(); // after the clip panel (one or more box columns) and the keys
    auto at = [&](double beat, int pitch) { return std::pair<float, float>{notesLeft + (float) (beat * 40.0), notesTop + (float) (piano->view.topPitch - pitch) * 11.0f + 5.0f}; };
    auto mouse = [&](std::pair<float, float> p, int clicks = 1) {
        ui::MouseEvent e;
        e.x = p.first;
        e.y = p.second;
        e.clicks = clicks;
        return e;
    };
    auto notes = [&]() -> const std::vector<model::MidiNote>& { return song.tracks[0].arrangementClips[0].notes; };
    REQUIRE(notes().size() == 2);
    CHECK_NEAR(notes()[0].start, 0.75, 1e-9); // pitch 65
    CHECK(notes()[0].pitch == 65);

    // move the first note 1 beat to the right and one semitone up
    root.dispatchMouseDown(mouse(at(0.9, 65)));
    root.dispatchMouseDrag(mouse(at(1.9, 66)));
    root.dispatchMouseUp(mouse(at(1.9, 66)));
    REQUIRE(history.canUndo());
    CHECK_NEAR(notes()[0].start, 1.75, 1e-9);
    CHECK(notes()[0].pitch == 66);

    // resize it: grab the right edge (note is 0.5 beats = 20 px wide, starts at 1.75) and drag 1 beat wider
    const double end = 1.75 + 0.5;
    root.dispatchMouseDown(mouse(at(end - 0.03, 66)));
    root.dispatchMouseDrag(mouse(at(end + 0.97, 66)));
    root.dispatchMouseUp(mouse(at(end + 0.97, 66)));
    CHECK_NEAR(notes()[0].duration, 1.5, 1e-9);

    // Scroll the keyboard down a row so pitch 60 is visible below the toolbar.
    piano->view.topPitch = 68;
    // double-click an empty cell adds a note (grid 0.25 at this zoom)
    root.dispatchMouseDown(mouse(at(6.6, 60), 2));
    root.dispatchMouseUp(mouse(at(6.6, 60), 2));
    REQUIRE(notes().size() == 3);
    CHECK(notes().back().pitch == 60);
    CHECK_NEAR(notes().back().start, 6.5, 1e-9);
    CHECK_NEAR(notes().back().duration, 0.25, 1e-9);

    // velocity lane: click at the stem of the new note near the top sets a high velocity
    const float velTop = 700.0f - metrics.statusBarHeight - 44.0f; // lane starts 44 px above the panel bottom (status bar excluded)
    root.dispatchMouseDown(mouse({notesLeft + 6.5f * 40.0f + 1.5f, velTop + 6.0f}));
    root.dispatchMouseUp(mouse({notesLeft + 6.5f * 40.0f + 1.5f, velTop + 6.0f}));
    CHECK(notes().back().velocity > 110.0f);

    // Cmd+A selects everything, Shift+Up transposes an octave
    ui::KeyEvent selectAll;
    selectAll.cmd = true;
    selectAll.textChar = 'a';
    root.dispatchKey(selectAll);
    ui::KeyEvent up;
    up.keyCode = juce::KeyPress::upKey;
    up.shift = true;
    root.dispatchKey(up);
    CHECK(notes()[0].pitch == 78); // 66 + 12
    CHECK(notes().back().pitch == 72);

    // the transposed notes are still selected: Delete removes all of them
    ui::KeyEvent del;
    del.keyCode = juce::KeyPress::deleteKey;
    root.dispatchKey(del);
    REQUIRE(song.tracks[0].arrangementClips.size() == 1); // Delete belongs to the focused piano roll
    CHECK(notes().empty());

    // everything is undoable, step by step
    int steps = 0;
    while (history.canUndo() && steps < 20)
    {
        history.undo(song);
        ++steps;
    }
    CHECK(notes().size() == 2);
    CHECK_NEAR(notes()[0].start, 0.75, 1e-9);
}

TEST_CASE("automation UI: toggle, select, move, add, delete and undo through real input events")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = ctx.detailVisible = false;
    ctx.pixelsPerBeat = 40;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ctx.undo = [&] { history.undo(song); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    ui::KeyEvent a;
    a.textChar = 'a';
    REQUIRE(root.dispatchKey(a));
    CHECK(ctx.automationVisible);
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    const float y0 = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.arrangementRulerHeight + ctx.theme.metrics.arrangementLaneHeight;
    auto mouse = [&](float x, float y, int clicks = 1) {
        ui::MouseEvent e;
        e.x = x; e.y = y0 + y; e.clicks = clicks;
        return e;
    };
    // Volume range 0..1.995262: value 1 lands halfway down the lane.
    root.dispatchMouseDown(mouse(160, 35));
    root.dispatchMouseUp(mouse(160, 35));
    CHECK(! history.canUndo());
    root.dispatchMouseDown(mouse(160, 35));
    root.dispatchMouseDrag(mouse(200, 50));
    root.dispatchMouseUp(mouse(200, 50));
    REQUIRE(history.canUndo());
    CHECK_NEAR(song.tracks[0].automation[0].points[1].time, 4.75, 1e-9); // 192 px / 40 = 4.8, on the 1/16 adaptive grid
    CHECK(song.tracks[0].automation[0].points[1].value < 0.6);
    // Add a point on an empty part of the lane.
    root.dispatchMouseDown(mouse(480, 20, 2));
    root.dispatchMouseUp(mouse(480, 20, 2));
    CHECK(song.tracks[0].automation[0].points.size() == 4);
    // Select and Delete this point, then undo all edits.
    root.dispatchMouseDown(mouse(480, 20));
    root.dispatchMouseUp(mouse(480, 20));
    ui::KeyEvent del;
    del.keyCode = juce::KeyPress::deleteKey;
    root.dispatchKey(del);
    CHECK(song.tracks[0].automation[0].points.size() == 3);
    ui::KeyEvent z;
    z.cmd = true; z.textChar = 'z';
    root.dispatchKey(z); root.dispatchKey(z); root.dispatchKey(z);
    CHECK_NEAR(song.tracks[0].automation[0].points[1].time, 4, 1e-9);
    CHECK_NEAR(song.tracks[0].automation[0].points[1].value, 1, 1e-9);
    root.dispatchKey(a);
    CHECK(! ctx.automationVisible);
}

TEST_CASE("browser: filtering keeps file identities, folder navigation, preview and keyboard focus")
{
    ui::UiContext ctx;
    ctx.detailVisible = false;
    ctx.projectFiles = {"/tmp/Alpha.als", "/tmp/Beta.als"};
    ctx.browserPlaces = {"/tmp/Samples"};
    ctx.browserFiles = {{"/tmp/Samples/kick.wav", false}, {"/tmp/Samples/Subfolder", true}};
    std::string opened, previewed, folder;
    ctx.openFile = [&](const std::string& path) { opened = path; };
    ctx.previewSample = [&](const std::string& path) { previewed = path; };
    ctx.openBrowserFolder = [&](const std::string& path) { folder = ctx.browserFolder = path; };
    std::string expanded; ctx.expandBrowserFolder = [&](const std::string& path) { expanded = path; };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    ui::KeyEvent find;
    find.cmd = true; find.textChar = 'f';
    root.dispatchKey(find);
    for (auto ch : std::string("beta")) { ui::KeyEvent key; key.textChar = (uint32_t) ch; root.dispatchKey(key); }
    ui::BrowserPanel* browser = nullptr;
    for (const auto& child : root.children()) if (! browser) browser = dynamic_cast<ui::BrowserPanel*>(child.get());
    REQUIRE(browser);
    auto sidebarRow = [&](int index) { const auto r = browser->sidebarRowBounds(index); return std::pair<float, float>{browser->boundsInRoot().x + r.centreX(), browser->boundsInRoot().y + r.centreY()}; };
    auto listRow = [&](int index) { const auto r = browser->listRowBounds(index); return std::pair<float, float>{browser->boundsInRoot().x + r.centreX(), browser->boundsInRoot().y + r.centreY()}; };
    ui::MouseEvent first;
    first.x = listRow(0).first; first.y = listRow(0).second; first.clicks = 2;
    root.dispatchMouseDown(first);
    root.dispatchMouseUp(first);
    CHECK(opened == "/tmp/Beta.als"); // filtered row 0 must not open unfiltered Alpha
    // Clear the query, then select a custom Place and preview its sample.
    root.dispatchKey(find);
    ui::KeyEvent back;
    back.keyCode = juce::KeyPress::backspaceKey;
    for (int i = 0; i < 4; ++i) root.dispatchKey(back);
    ui::MouseEvent place;
    place.x = sidebarRow(13).first; place.y = sidebarRow(13).second; // the first custom Place
    root.dispatchMouseDown(place); root.dispatchMouseUp(place);
    CHECK(folder == "/tmp/Samples");
    first.clicks = 1;
    root.dispatchMouseDown(first); root.dispatchMouseUp(first);
    CHECK(previewed == "/tmp/Samples/kick.wav");
    first.y = listRow(1).second; first.clicks = 2;
    root.dispatchMouseDown(first); root.dispatchMouseUp(first);
    CHECK(folder == "/tmp/Samples"); // inline disclosure retains the browser root
    CHECK(expanded == "/tmp/Samples/Subfolder");
    // Clicking outside the browser ends search focus: 'a' toggles arrangement automation again.
    root.dispatchKey(find);
    ui::MouseEvent outside;
    outside.x = 500; outside.y = 140;
    root.dispatchMouseDown(outside); root.dispatchMouseUp(outside);
    ui::KeyEvent automation;
    automation.textChar = 'a';
    root.dispatchKey(automation);
    CHECK(ctx.automationVisible);
}

TEST_CASE("browser: real drag events route stable filtered items and Escape cancels")
{
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Audio;
    song.tracks = {track};
    song.scenes = {{"Scene", -1, {}}};
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.detailVisible = false;
    ctx.browserPlaces = {"/tmp/Samples"};
    ctx.browserFiles = {{"/tmp/Samples/kick.wav", false}, {"/tmp/Samples/snare.wav", false}};
    ctx.openBrowserFolder = [&](const std::string& path) { ctx.browserFolder = path; };
    int drops = 0, droppedTrack = -1, droppedScene = -2;
    double droppedBeat = -1;
    ui::BrowserDrag payload;
    ctx.dropBrowserItem = [&](const auto& item, int index, double beat, int scene) {
        ++drops; payload = item; droppedTrack = index; droppedBeat = beat; droppedScene = scene;
    };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    const auto image = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_drag_test.png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFullPathName().toStdString()));
    ui::MouseEvent place;
    ui::BrowserPanel* browser = nullptr;
    for (const auto& child : root.children()) if (! browser) browser = dynamic_cast<ui::BrowserPanel*>(child.get());
    REQUIRE(browser);
    auto sidebarRow = [&](int index) { const auto r = browser->sidebarRowBounds(index); return std::pair<float, float>{browser->boundsInRoot().x + r.centreX(), browser->boundsInRoot().y + r.centreY()}; };
    auto listRow = [&](int index) { const auto r = browser->listRowBounds(index); return std::pair<float, float>{browser->boundsInRoot().x + r.centreX(), browser->boundsInRoot().y + r.centreY()}; };
    place.x = sidebarRow(13).first; place.y = sidebarRow(13).second;
    root.dispatchMouseDown(place); root.dispatchMouseUp(place);
    ui::KeyEvent find;
    find.cmd = true; find.textChar = 'f'; root.dispatchKey(find);
    for (auto ch : std::string("snare")) { ui::KeyEvent key; key.textChar = (uint32_t) ch; root.dispatchKey(key); }
    ui::MouseEvent start;
    start.x = listRow(0).first; start.y = listRow(0).second;
    root.dispatchMouseDown(start);
    auto end = start;
    end.x = 600; end.y = 90;
    root.dispatchMouseDrag(end);
    REQUIRE(ctx.browserDrag.has_value());
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFullPathName().toStdString()));
    root.dispatchMouseUp(end);
    CHECK(drops == 1);
    CHECK(payload.kind == ui::BrowserDrag::Kind::Sample);
    CHECK(payload.value == "/tmp/Samples/snare.wav");
    CHECK(droppedTrack == 0);
    CHECK(droppedBeat >= 0);
    CHECK(droppedScene == -1);
    CHECK(! ctx.browserDrag);
    root.dispatchMouseDown(start); root.dispatchMouseDrag(end);
    ui::KeyEvent escape; escape.keyCode = juce::KeyPress::escapeKey;
    root.dispatchKey(escape); root.dispatchMouseUp(end);
    CHECK(drops == 1);
    // Native items can land on Session headers and the selected device panel.
    root.dispatchKey(find);
    ui::KeyEvent back; back.keyCode = juce::KeyPress::backspaceKey;
    for (int i = 0; i < 5; ++i) root.dispatchKey(back);
    auto category = place; category.x = sidebarRow(4).first; category.y = sidebarRow(4).second;
    root.dispatchMouseDown(category); root.dispatchMouseUp(category);
    auto group = start; group.x = listRow(2).first; group.y = listRow(2).second; // EQ & Filters
    root.dispatchMouseDown(group); root.dispatchMouseUp(group);
    start.x = listRow(4).first; start.y = listRow(4).second; // EQ Eight, after Auto Filter
    root.dispatchMouseDown(start); root.dispatchMouseDrag(end); root.dispatchMouseUp(end);
    CHECK(payload.kind == ui::BrowserDrag::Kind::NativeDevice);
    CHECK(payload.value == "Eq8");
    ctx.mainView = ui::MainView::Session;
    root.layout();
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFullPathName().toStdString()));
    end.x = 450; end.y = 90;
    root.dispatchMouseDown(start); root.dispatchMouseDrag(end); root.dispatchMouseUp(end);
    CHECK(droppedTrack == 0);
    CHECK(droppedScene == -1);
    // A sample goes to the exact Session scene under the pointer.
    start.x = listRow(0).first; start.y = listRow(0).second;
    root.dispatchMouseDown(place); root.dispatchMouseUp(place);
    root.dispatchMouseDown(start); root.dispatchMouseDrag(end); root.dispatchMouseUp(end);
    CHECK(payload.kind == ui::BrowserDrag::Kind::Sample);
    CHECK(droppedTrack == 0);
    CHECK(droppedScene == 1);
    image.deleteFile();
}

TEST_CASE("ui: Finder file drags show the drag label and drop audio files like browser samples")
{
    juce::ScopedJuceInitialiser_GUI gui;
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Audio;
    song.tracks = {track};
    song.scenes = {{"Scene", -1, {}}, {"Scene 2", -1, {}}};
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    int drops = 0, droppedTrack = -2, droppedScene = -2;
    ui::BrowserDrag payload;
    ctx.dropBrowserItem = [&](const auto& item, int index, double, int scene) { ++drops; payload = item; droppedTrack = index; droppedScene = scene; };
    std::string opened;
    ctx.openFile = [&](const std::string& path) { opened = path; };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    std::function<ui::Widget*(ui::Widget&, const std::function<bool(ui::Widget&)>&)> find = [&](ui::Widget& w, const auto& match) -> ui::Widget* {
        if (match(w)) return &w;
        for (const auto& child : w.children()) if (auto* hit = find(*child, match)) return hit;
        return nullptr;
    };
    auto* arrangement = dynamic_cast<ui::ArrangementView*>(find(root, [](ui::Widget& w) { return dynamic_cast<ui::ArrangementView*>(&w) != nullptr; }));
    REQUIRE(arrangement != nullptr);
    REQUIRE(! arrangement->lanes().empty());

#if JUCE_MAC // CoreAudio containers; other platforms decode WAV/AIFF/FLAC/Ogg/MP3 only (docs/PLATFORMS.md)
    const std::vector<std::string> files{"/tmp/Drop/One.m4a", "/tmp/Drop/notes.txt", "/tmp/Drop/Two.caf"};
    const juce::String upperCaseLoop = "/tmp/Drop/Loop.M4A";
#else
    const std::vector<std::string> files{"/tmp/Drop/One.wav", "/tmp/Drop/notes.txt", "/tmp/Drop/Two.flac"};
    const juce::String upperCaseLoop = "/tmp/Drop/Loop.WAV";
#endif
    CHECK(ui::RootWidget::acceptsFileDrag(files));
    CHECK(! ui::RootWidget::acceptsFileDrag({"/tmp/Drop/notes.txt"}));
    CHECK(ui::RootWidget::acceptsFileDrag({"/tmp/Drop/Song.als"}));
    gfx::SkiaCanvasComponent canvas(root);
    CHECK(canvas.isInterestedInFileDrag({upperCaseLoop}));
    CHECK(! canvas.isInterestedInFileDrag({"/tmp/Drop/readme.pdf"}));

    const auto origin = arrangement->boundsInRoot();
    const auto& lane = arrangement->lanes().front();
    ui::MouseEvent onLane;
    onLane.x = origin.x + 200; onLane.y = origin.y + lane.y + lane.h * 0.5f;
    CHECK(root.dispatchFileDragMove(onLane.x, onLane.y, files));
    REQUIRE(ctx.browserDrag.has_value());
    CHECK(ctx.browserDrag->label == "2 files");
    const auto image = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_file_drag_test.png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFullPathName().toStdString()));
    image.deleteFile();
    root.dispatchFileDragExit();
    CHECK(! ctx.browserDrag);

    // Plain drop: both audio files stacked on the lane's track; Cmd: one track per file.
    CHECK(root.dispatchFileDrop(onLane, files));
    CHECK(drops == 1);
    CHECK(droppedTrack == 0);
    CHECK(droppedScene == -1);
    CHECK(payload.kind == ui::BrowserDrag::Kind::Sample);
    CHECK(payload.value == files[0]);
    const std::vector<std::string> audioFiles{files[0], files[2]};
    CHECK(payload.files == audioFiles);
    CHECK(! payload.oneTrackPerFile);
    auto withCmd = onLane; withCmd.cmd = true;
    CHECK(root.dispatchFileDrop(withCmd, files));
    CHECK(payload.oneTrackPerFile);
    CHECK(! ctx.browserDrag);

    // The empty space below the last lane makes a new track.
    float lanesEnd = 0;
    for (const auto& l : arrangement->lanes()) lanesEnd = std::max(lanesEnd, l.y + l.h);
    auto below = onLane; below.y = origin.y + lanesEnd + 40;
    REQUIRE(below.y < origin.y + arrangement->height() - 40);
    CHECK(root.dispatchFileDrop(below, {files[0]}));
    CHECK(drops == 3);
    CHECK(droppedTrack == -1);
    CHECK(payload.files.size() == 1);

    // Nothing usable: no drop; a lone set opens.
    CHECK(! root.dispatchFileDrop(onLane, {"/tmp/Drop/notes.txt"}));
    CHECK(drops == 3);
    CHECK(root.dispatchFileDrop(onLane, {"/tmp/Drop/Song.als"}));
    CHECK(opened == "/tmp/Drop/Song.als");
    CHECK(drops == 3);

    // Session: the slot under the pointer, or a new track in the area right of the last column.
    ctx.mainView = ui::MainView::Session;
    root.layout();
    auto* session = dynamic_cast<ui::SessionView*>(find(root, [](ui::Widget& w) { return dynamic_cast<ui::SessionView*>(&w) != nullptr && w.visible; }));
    REQUIRE(session != nullptr);
    ui::MouseEvent slot;
    slot.x = 450; slot.y = 90;
    CHECK(root.dispatchFileDrop(slot, files));
    CHECK(droppedTrack == 0);
    CHECK(droppedScene == 1);
    auto empty = slot; empty.x = session->boundsInRoot().x + session->width() * 0.5f;
    CHECK(root.dispatchFileDrop(empty, files));
    CHECK(droppedTrack == -1);
    CHECK(droppedScene == 1);

    ctx.recording = true;
    CHECK(! root.dispatchFileDrop(slot, files));
    CHECK(drops == 5);
}

TEST_CASE("ui: macOS Command shortcuts work when JUCE provides no text character")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::UiContext ctx;
    int saves = 0, saveAs = 0, undos = 0;
    ctx.saveFile = [&] { ++saves; };
    ctx.saveFileAs = [&] { ++saveAs; };
    ctx.undo = [&] { ++undos; };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    gfx::SkiaCanvasComponent canvas(root);
    CHECK(canvas.keyPressed(juce::KeyPress('S', juce::ModifierKeys::commandModifier, 0)));
    CHECK(canvas.keyPressed(juce::KeyPress('S', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)));
    CHECK(canvas.keyPressed(juce::KeyPress('Z', juce::ModifierKeys::commandModifier, 0)));
    CHECK(saves == 1);
    CHECK(saveAs == 1);
    CHECK(undos == 1);
}

#include "io/als/AlsWriter.h"

TEST_CASE("session piano roll: selection does not launch, edits save undo redo and keep arrangement intact")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.mainView = ui::MainView::Session;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    int launches = 0;
    context.launchClip = [&](int track, int scene) { CHECK(track == 0); CHECK(scene == 1); ++launches; };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto mouse = [](float x, float y, int clicks = 1) { ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks; return e; };
    const auto& metrics = context.theme.metrics;
    const float row = metrics.controlBarHeight + metrics.sessionHeaderHeight + 1 + metrics.sessionSlotHeight * 1.5f;
    root.dispatchMouseDown(mouse(50, row, 2));
    root.dispatchMouseUp(mouse(50, row, 2));
    CHECK(launches == 0);
    CHECK(context.selectedClipScene == 1);
    CHECK(context.detailTab == ui::DetailTab::Clip);
    REQUIRE(context.selectedClip());
    const int uid = context.selectedClipUid;
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto* piano = findRoll(root); REQUIRE(piano); piano->view.pixelsPerBeat = 40; // exercise gestures at a manually selected zoom
    const float notesTop = 700 - metrics.statusBarHeight - metrics.detailHeight + 22 + 36;
    const float notesLeft = [&] { auto* r = findRoll(root); return r ? r->boundsInRoot().x + r->noteAreaBounds().x : 0.0f; }();
    auto note = [&](double beat, int pitch, int clicks = 1) { return mouse(notesLeft + (float) beat * 40, notesTop + (float) (64 - pitch) * 11 + 5, clicks); };
    root.dispatchMouseDown(note(0.2, 60));
    root.dispatchMouseDrag(note(1.2, 61));
    root.dispatchMouseUp(note(1.2, 61));
    CHECK(context.selectedClipUid == uid);
    CHECK_NEAR(context.selectedClip()->notes[0].start, 1, 1e-9);
    CHECK(context.selectedClip()->notes[0].pitch == 61);
    root.dispatchMouseDown(note(3.1, 62, 2));
    root.dispatchMouseUp(note(3.1, 62, 2));
    REQUIRE(context.selectedClip()->notes.size() == 2);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].notes[0].start, 0.75, 1e-9);
    juce::TemporaryFile file(".xml");
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    song.sourcePath = file.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    REQUIRE(history.undo(song));
    REQUIRE(history.undo(song));
    CHECK_NEAR(context.selectedClip()->notes[0].start, 0, 1e-9);
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    auto restored = io::loadAls(file.getFile());
    REQUIRE(restored);
    CHECK(restored.song->tracks[0].slots[1].clip->notes.size() == 1);
    CHECK_NEAR(restored.song->tracks[0].slots[1].clip->notes[0].start, 0, 1e-9);
    REQUIRE(history.redo(song));
    REQUIRE(history.redo(song));
    root.dispatchMouseDown(note(3.1, 62));
    root.dispatchMouseUp(note(3.1, 62));
    ui::KeyEvent all; all.cmd = true; all.textChar = 'a'; root.dispatchKey(all);
    ui::KeyEvent del; del.keyCode = juce::KeyPress::deleteKey; root.dispatchKey(del);
    REQUIRE(context.selectedClip());
    CHECK(context.selectedClip()->notes.empty());
    CHECK(song.tracks[0].arrangementClips.size() == 1);
    // Only the launch strip operates playback.
    root.dispatchMouseDown(mouse(metrics.outerMargin + 7, row));
    root.dispatchMouseUp(mouse(metrics.outerMargin + 7, row));
    CHECK(launches == 1);
    // An empty MIDI slot creates an editable Session clip using a retained template.
    root.dispatchMouseDown(mouse(50, row - metrics.sessionSlotHeight, 2));
    root.dispatchMouseUp(mouse(50, row - metrics.sessionSlotHeight, 2));
    CHECK(context.selectedClipScene == 0);
    REQUIRE(context.selectedClip());
    CHECK(context.selectedClip()->notes.empty());
    CHECK(context.selectedClip()->uid != uid);
}

TEST_CASE("clip controls: a MIDI clip's Notes, Signature and Launch sections edit the clip and save")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.mainView = ui::MainView::Session;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Clip;
    context.selectedClipTrack = 0;
    context.selectedClipScene = 1;
    context.selectedClipUid = song.tracks[0].slots[1].clip->uid;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 900});
    root.layout();
    using Id = ui::ClipControls::Id;
    auto click = [&](Id id) {
        const auto p = audioControl(root, id);
        ui::MouseEvent e; e.x = p.first; e.y = p.second;
        root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };
    auto drag = [&](Id id, float dy) {
        const auto p = audioControl(root, id);
        ui::MouseEvent e; e.x = p.first; e.y = p.second;
        root.dispatchMouseDown(e);
        e.y -= dy;
        root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    };
    auto current = [&]() -> const model::Clip& { return *context.selectedClip(); };

    const double lengthBefore = current().loop.loopEnd - current().loop.loopStart;
    const size_t notesBefore = current().notes.size();
    click(Id::NoteDuplicate);
    CHECK_NEAR(current().loop.loopEnd - current().loop.loopStart, 2 * lengthBefore, 1e-9);
    CHECK(current().notes.size() == 2 * notesBefore);
    history.undo(song);
    CHECK(current().notes.size() == notesBefore);
    click(Id::NoteDouble);
    CHECK_NEAR(current().loop.loopEnd - current().loop.loopStart, 2 * lengthBefore, 1e-9);
    click(Id::NoteHalf);
    CHECK_NEAR(current().loop.loopEnd - current().loop.loopStart, lengthBefore, 1e-9);

    click(Id::LaunchHeader);
    click(Id::LaunchLegato);
    CHECK(current().legato);
    drag(Id::LaunchVelocity, 40);
    CHECK_NEAR(current().velocityAmount, 0.2, 1e-9); // 40 px = 20 steps of 1 %
    drag(Id::SigNum, 12);
    CHECK(current().timeSigNumerator == 6);

    juce::TemporaryFile file(".als");
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    auto saved = io::loadAls(file.getFile());
    REQUIRE(saved);
    const auto& clip = *saved.song->tracks[0].slots[1].clip;
    CHECK(clip.legato);
    CHECK_NEAR(clip.velocityAmount, 0.2, 1e-9);
}

TEST_CASE("clip controls: Session start and loop gestures, scale and note probability are undoable where appropriate")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.mainView = ui::MainView::Session;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Clip;
    context.selectedClipTrack = 0;
    context.selectedClipScene = 1;
    context.selectedClipUid = song.tracks[0].slots[1].clip->uid;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto mouse = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    const float notesLeft = [&] { auto* r = findRoll(root); return r ? r->boundsInRoot().x + r->noteAreaBounds().x : 0.0f; }();
    using CId = ui::ClipControls::Id;
    auto dragControl = [&](CId id, float delta) {
        const auto at = audioControl(root, id);
        root.dispatchMouseDown(mouse(at.first, at.second));
        root.dispatchMouseDrag(mouse(at.first, at.second - delta));
        root.dispatchMouseUp(mouse(at.first, at.second - delta));
    };
    dragControl(CId::Start, 16); // start marker one beat forward
    CHECK_NEAR(context.selectedClip()->loop.startRelative, 1, 1e-9);
    dragControl(CId::Position, 8); // loop start moves half a beat; preserve absolute start marker
    CHECK_NEAR(context.selectedClip()->loop.loopStart, 0.5, 1e-9);
    CHECK_NEAR(context.selectedClip()->loop.startRelative, 0.5, 1e-9);
    dragControl(CId::Length, -16); // shorten loop by one beat
    CHECK_NEAR(context.selectedClip()->loop.loopEnd, 3, 1e-9);
    // Local scale display settings do not dirty the set.
    const int version = history.version();
    const auto highlight = audioControl(root, CId::ScaleIcon);
    root.dispatchMouseDown(mouse(highlight.first, highlight.second));
    root.dispatchMouseUp(mouse(highlight.first, highlight.second));
    CHECK(context.scaleHighlight);
    CHECK(history.version() == version);
    // Switch the note lane to probability, then drag its stem to 50%.
    const float velocityTop = 700 - context.theme.metrics.statusBarHeight - 44;
    root.dispatchMouseDown(mouse(notesLeft - 25, velocityTop + 8));
    root.dispatchMouseUp(mouse(notesLeft - 25, velocityTop + 8));
    root.dispatchMouseDown(mouse(notesLeft + 1.5f, velocityTop + 22));
    root.dispatchMouseUp(mouse(notesLeft + 1.5f, velocityTop + 22));
    CHECK_NEAR(context.selectedClip()->notes[0].probability, 0.5, 1e-6);
    CHECK_NEAR(context.selectedClip()->notes[0].velocity, 100, 1e-6);
    juce::TemporaryFile file(".als");
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    auto saved = io::loadAls(file.getFile());
    REQUIRE(saved);
    CHECK_NEAR(saved.song->tracks[0].slots[1].clip->notes[0].probability, 0.5, 1e-6);
    CHECK_NEAR(saved.song->tracks[0].slots[1].clip->loop.loopStart, 0.5, 1e-9);
    while (history.canUndo()) history.undo(song);
    CHECK_NEAR(context.selectedClip()->notes[0].probability, 1, 1e-6);
    CHECK_NEAR(context.selectedClip()->loop.loopStart, 0, 1e-9);
    CHECK_NEAR(context.selectedClip()->loop.loopEnd, 4, 1e-9);
}

TEST_CASE("audio clip editor: warp toggle converts units and marker add move delete save undo redo")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    REQUIRE(song.tracks.size() >= 2);
    auto& clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000;
    clip.lengthSamples = 96000;
    clip.warped = false;
    clip.loop = {0, 2, 0, false};
    clip.warpMarkers = {{0, 0}, {2, 4}};
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto mouse = [](float x, float y, int clicks = 1) { ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks; return e; };
    const auto warp = audioControl(root, ui::ClipControls::Id::Warp);
    root.dispatchMouseDown(mouse(warp.first, warp.second));
    root.dispatchMouseUp(mouse(warp.first, warp.second));
    CHECK(context.selectedClip()->warped);
    CHECK_NEAR(context.selectedClip()->loop.loopEnd, 4, 1e-9);
    // The 2 seconds fill the waveform's width: add a marker at one second, in the warp marker row.
    auto* view = findSampleView(root);
    REQUIRE(view != nullptr);
    const auto origin = view->boundsInRoot();
    const float waveWidth = view->waveArea().w;
    const float x = origin.x + view->waveArea().x + waveWidth / 2;
    const float warpY = origin.y + view->warpRowY() + ui::SampleView::kWarpRowHeight * 0.5f;
    const float dragPx = 0.2f * waveWidth / 2;
    root.dispatchMouseDown(mouse(x, warpY, 2));
    root.dispatchMouseUp(mouse(x, warpY, 2));
    REQUIRE(context.selectedClip()->warpMarkers.size() == 3);
    CHECK_NEAR(context.selectedClip()->warpMarkers[1].seconds, 1, 1e-6);
    CHECK_NEAR(context.selectedClip()->warpMarkers[1].beats, 2, 1e-6);
    root.dispatchMouseDown(mouse(x, warpY));
    root.dispatchMouseDrag(mouse(x + dragPx, warpY));
    root.dispatchMouseUp(mouse(x + dragPx, warpY));
    CHECK_NEAR(context.selectedClip()->warpMarkers[1].seconds, 1.2, 1e-5);
    auto shifted = mouse(x + dragPx, warpY - 20); shifted.shift = true;
    root.dispatchMouseDown(mouse(x + dragPx, warpY));
    root.dispatchMouseDrag(shifted); root.dispatchMouseUp(shifted);
    CHECK_NEAR(context.selectedClip()->warpMarkers[1].beats, 2.5, 1e-6);
    juce::TemporaryFile file(".als");
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    song.sourcePath = file.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    auto saved = io::loadAls(file.getFile()); REQUIRE(saved);
    CHECK_NEAR(saved.song->tracks[1].arrangementClips[0].warpMarkers[1].beats, 2.5, 1e-6);
    root.dispatchMouseDown(mouse(x + dragPx, warpY, 2)); root.dispatchMouseUp(mouse(x + dragPx, warpY, 2));
    CHECK(context.selectedClip()->warpMarkers.size() == 2);
    while (history.canUndo()) history.undo(song);
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    saved = io::loadAls(file.getFile()); REQUIRE(saved);
    CHECK(! saved.song->tracks[1].arrangementClips[0].warped);
    CHECK_NEAR(saved.song->tracks[1].arrangementClips[0].loop.loopEnd, 2, 1e-9);
    while (history.canRedo()) history.redo(song);
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    saved = io::loadAls(file.getFile()); REQUIRE(saved);
    CHECK(saved.song->tracks[1].arrangementClips[0].warped);
    CHECK(saved.song->tracks[1].arrangementClips[0].warpMarkers.size() == 2);
}

#include "ui/views/InputControls.h"

TEST_CASE("input UI: MIDI port channel and monitoring gestures undo, and I/O visibility toggles")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    ui::UiContext context; context.song = &song; context.browserVisible = false; context.detailVisible = false;
    context.mainView = ui::MainView::Session;
    context.midiInputDevices = [] { return std::vector<std::pair<std::string, std::string>>{{"port-a", "Controller A"}, {"port-b", "Controller B"}}; };
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context); root.setBounds({0, 0, 1200, 800}); root.layout();
    ui::InputControls* selected = nullptr;
    auto find = [&](const ui::Widget& widget, auto&& self) -> void {
        for (const auto& child : widget.children())
        {
            if (! selected) selected = dynamic_cast<ui::InputControls*>(child.get());
            self(*child, self);
        }
    };
    find(root, find); REQUIRE(selected);
    const auto area = selected->boundsInRoot();
    auto click = [&](float x, float y) { ui::MouseEvent e; e.x = area.x + x; e.y = area.y + y; root.dispatchMouseDown(e); root.dispatchMouseUp(e); };
    std::vector<std::string> shown;
    std::string want;
    context.showChoices = [&](const std::vector<std::string>& labels, const std::vector<bool>&, std::function<void(int)> pick) {
        shown = labels;
        for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == want) pick((int) i);
    };
    want = "Controller A";
    click(20, 22); // the MIDI From dropdown lists every input and picks one
    CHECK(shown == std::vector<std::string>({"All Ins", "Controller A", "Controller B", "No Input"}));
    CHECK(song.tracks[0].inputRouting == "MidiIn/External.port-a/-1");
    CHECK(song.tracks[0].inputUpperDisplay == "Controller A");
    want = "Ch. 1";
    click(20, 42);
    CHECK(shown.size() == 17); // All Channels + 16
    CHECK(song.tracks[0].inputRouting == "MidiIn/External.port-a/0");
    click(65, 62); CHECK(song.tracks[0].monitoring == 2); // the Off button of the Monitor row
    CHECK(! history.undoChangesInput()); // monitor-only changes do not alter fixed take membership
    REQUIRE(history.undo(song)); CHECK(song.tracks[0].monitoring == 1);
    REQUIRE(history.undo(song)); CHECK(song.tracks[0].inputRouting == "MidiIn/External.port-a/-1");
    ui::KeyEvent toggle; toggle.cmd = toggle.alt = true; toggle.textChar = 'i'; root.dispatchKey(toggle);
    CHECK(! context.ioVisible);
}

#include "ui/views/TopBar.h"
#include "engine/Transport.h"

TEST_CASE("recording controls: count-in preference, punch flags and loop toggle use real input and undo")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; engine::Transport transport;
    transport.setLoop(song.loopOn, song.loopStart, song.loopLength);
    model::EditHistory history; ui::UiContext context; context.song = &song; context.transport = &transport;
    context.browserVisible = context.detailVisible = false;
    int storedBars = -1;
    context.setCountInBars = [&](int bars) { storedBars = bars; };
    context.applyEdit = [&](model::EditCommand command) {
        history.perform(song, std::move(command)); transport.setLoop(song.loopOn, song.loopStart, song.loopLength);
    };
    ui::MainLayout root(context);
    juce::TemporaryFile image(".png"); REQUIRE(gfx::renderToPng(root, 1440, 900, 1, image.getFile().getFullPathName().toStdString()));
    auto* bar = dynamic_cast<ui::TopBar*>(root.children()[0].get()); REQUIRE(bar);
    auto click = [&](size_t index) {
        const auto bounds = bar->children()[index]->boundsInRoot();
        ui::MouseEvent event; event.x = bounds.centreX(); event.y = bounds.centreY();
        root.dispatchMouseDown(event); root.dispatchMouseUp(event);
    };
    for (int expected : {1, 2, 4, 0}) { click(5); CHECK(context.countInBars == expected); CHECK(storedBars == expected); }
    CHECK(! history.canUndo()); // count-in belongs to application preferences, not the set
    click(6); CHECK(song.punchIn); CHECK(! song.punchOut);
    click(7); CHECK(song.punchIn && song.punchOut);
    CHECK(history.undoChangesRecordingRange()); REQUIRE(history.undo(song)); CHECK(song.punchIn && ! song.punchOut);
    REQUIRE(history.undo(song)); CHECK(! song.punchIn && ! song.punchOut);
    const bool before = song.loopOn; click(3); CHECK(song.loopOn != before); CHECK(transport.loopEnabled() == song.loopOn);
    REQUIRE(history.undo(song)); CHECK(song.loopOn == before);
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(! again.song->punchIn && ! again.song->punchOut);
}

#include "ui/views/ArrangementView.h"
#include "ui/views/SessionView.h"

TEST_CASE("take UI: real gestures select ranges comp audition rename reorder and delete lanes")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; model::EditHistory history;
    auto source = song.tracks[0].arrangementClips[0]; source.takeId = 24; source.notes[0].pitch = 73;
    model::TakeLane lane; lane.name = "Pass A"; lane.clips = {source};
    history.perform(song, model::appendTakeLanes(song, 0, {lane, lane})); history.clear();
    const int id = song.tracks[0].takeLanes[0].id;
    ui::UiContext ctx; ctx.song = &song; ctx.ioVisible = ctx.browserVisible = ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement; ctx.pixelsPerBeat = 80;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ctx.undo = [&] { history.undo(song); };
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    auto render = [&] { juce::TemporaryFile png(".png"); REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString())); };
    render();
    ui::KeyEvent key; key.cmd = key.alt = true; key.textChar = 'u'; CHECK(root.dispatchKey(key)); root.layout();
    CHECK(ctx.unfoldedTakeTracks == std::vector<int>{0});
    auto mouse = [](float x, float y, int clicks = 1) { ui::MouseEvent e; e.x = x; e.y = y; e.clicks = clicks; return e; };
    const float takeY = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.arrangementRulerHeight + ctx.theme.metrics.arrangementLaneHeight;
    const float panelX = ctx.theme.metrics.outerMargin;
    root.dispatchMouseDown(mouse(panelX + 5 * 80, takeY + 30)); root.dispatchMouseDrag(mouse(panelX + 6 * 80, takeY + 30)); root.dispatchMouseUp(mouse(panelX + 6 * 80, takeY + 30));
    CHECK(ctx.selectedTakeLane == id); CHECK(ctx.selectedClip()->takeId == 24); CHECK(! history.canUndo());
    key = {}; key.keyCode = juce::KeyPress::returnKey; CHECK(root.dispatchKey(key));
    REQUIRE(history.undoLabel() == "Comp Take Range");
    bool found = false; for (const auto& clip : song.tracks[0].arrangementClips)
        if (std::fabs(clip.currentStart - 5) < 1e-9 && std::fabs(clip.currentEnd - 6) < 1e-9) { found = true; CHECK(clip.takeId == 24); }
    CHECK(found); history.undo(song);
    key = {}; key.textChar = 't'; CHECK(root.dispatchKey(key)); CHECK(model::findTakeLane(song, 0, id)->audition);
    CHECK(root.dispatchKey(key)); CHECK(! model::findTakeLane(song, 0, id)->audition);
    // Header selection and UTF-8 rename stay separate from clip edits.
    root.dispatchMouseDown(mouse(1040, takeY + 8)); root.dispatchMouseUp(mouse(1040, takeY + 8));
    CHECK(ctx.selectedClipUid < 0); key = {}; key.cmd = true; key.textChar = 'r'; CHECK(root.dispatchKey(key));
    key = {}; key.textChar = 'X'; CHECK(root.dispatchKey(key));
    key.textChar = 0x266c; CHECK(root.dispatchKey(key));
    key.textChar = 0; key.keyCode = juce::KeyPress::returnKey; CHECK(root.dispatchKey(key));
    CHECK(model::findTakeLane(song, 0, id)->name == "X♬");
    key = {}; key.cmd = true; key.keyCode = juce::KeyPress::downKey; CHECK(root.dispatchKey(key));
    CHECK(song.tracks[0].takeLanes[0].id == id); // reserved for comp-take cycling; never reorder lanes
    ui::ArrangementView* arrangement = nullptr;
    for (const auto& child : root.children()) if (auto* view = dynamic_cast<ui::ArrangementView*>(child.get())) arrangement = view;
    REQUIRE(arrangement);
    const float reorderX = arrangement->boundsInRoot().right() - 35;
    root.dispatchMouseDown(mouse(reorderX, takeY + 8));
    root.dispatchMouseUp(mouse(reorderX, takeY + 8));
    CHECK(song.tracks[0].takeLanes[1].id == id);
    key = {}; key.cmd = true; key.textChar = 'd'; CHECK(root.dispatchKey(key)); CHECK(song.tracks[0].takeLanes.size() == 3);
    key = {}; key.keyCode = juce::KeyPress::deleteKey; CHECK(root.dispatchKey(key)); CHECK(! model::findTakeLane(song, 0, id));
    history.undo(song); CHECK(model::findTakeLane(song, 0, id));
    key = {}; key.textChar = 'a'; CHECK(root.dispatchKey(key)); CHECK(ctx.automationVisible); root.layout();
    key = {}; key.cmd = key.alt = true; key.textChar = 'u'; CHECK(root.dispatchKey(key)); // hide current lanes
    CHECK(root.dispatchKey(key)); CHECK(! ctx.automationVisible); render();
}

TEST_CASE("take editors: MIDI and audio property gestures edit their selected lane independently")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; model::EditHistory history;
    model::TakeLane lane; lane.name = "MIDI pass"; lane.clips = {song.tracks[0].arrangementClips[0]};
    history.perform(song, model::appendTakeLanes(song, 0, {lane}));
    const int id = song.tracks[0].takeLanes[0].id;
    ui::UiContext ctx; ctx.song = &song; ctx.ioVisible = ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement; ctx.detailTab = ui::DetailTab::Clip;
    ctx.selectedClipTrack = ctx.selectedTrack = 0; ctx.selectedTakeLane = id; ctx.selectedClipUid = song.tracks[0].takeLanes[0].clips[0].uid;
    ctx.applyEdit = [&](model::EditCommand c) { CHECK(c.takeLanesOnly); history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    auto render = [&] { juce::TemporaryFile png(".png"); REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString())); };
    render();
    auto mouse = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    const auto& metrics = ctx.theme.metrics;
    const float notesTop = 700 - metrics.statusBarHeight - metrics.detailHeight + 22 + 36;
    const float notesLeft = [&] { auto* r = findRoll(root); return r ? r->boundsInRoot().x + r->noteAreaBounds().x : 0.0f; }();
    const float noteY = notesTop + 4 * 11 + 5;
    auto* piano = findRoll(root); REQUIRE(piano); piano->view.pixelsPerBeat = 40;
    root.dispatchMouseDown(mouse(notesLeft + 0.9f * 40, noteY));
    root.dispatchMouseDrag(mouse(notesLeft + 1.9f * 40, noteY - 11));
    root.dispatchMouseUp(mouse(notesLeft + 1.9f * 40, noteY - 11));
    CHECK(model::findTakeLane(song, 0, id)->clips[0].notes[0].pitch == 66);
    CHECK(song.tracks[0].arrangementClips[0].notes[0].pitch == 65);
    // Cmd+Opt+U must pass through a focused roll with selected notes.
    ui::KeyEvent toggle; toggle.cmd = toggle.alt = true; toggle.textChar = 'u'; CHECK(root.dispatchKey(toggle));
    CHECK(ctx.unfoldedTakeTracks == std::vector<int>{0});
    auto audio = song.tracks[0].takeLanes[0].clips[0]; audio.kind = model::Clip::Kind::Audio; audio.warped = true;
    lane.name = "Audio pass"; lane.clips = {audio}; history.perform(song, model::appendTakeLanes(song, 0, {lane}));
    ctx.selectedTakeLane = song.tracks[0].takeLanes.back().id; ctx.selectedClipUid = song.tracks[0].takeLanes.back().clips[0].uid;
    render();
    const auto pitch = audioControl(root, ui::ClipControls::Id::PitchSt);
    root.dispatchMouseDown(mouse(pitch.first, pitch.second)); root.dispatchMouseDrag(mouse(pitch.first, pitch.second - 12)); root.dispatchMouseUp(mouse(pitch.first, pitch.second - 12));
    CHECK(song.tracks[0].takeLanes.back().clips[0].pitchCoarse == 2);
    CHECK(song.tracks[0].arrangementClips[0].pitchCoarse == 0);
}

TEST_CASE("Session record buttons: exact empty slot and Shift overdub dispatch real gestures")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; ui::UiContext ctx; ctx.song = &song;
    ctx.browserVisible = ctx.detailVisible = ctx.ioVisible = false; ctx.mainView = ui::MainView::Session;
    ctx.controls = std::make_shared<engine::ControlBank>(2); (*ctx.controls)[0].armed.store(true);
    int recordedScene = -1, launches = 0, stops = 0;
    ctx.stopAllClips = [&] { ++stops; };
    ctx.recordSessionClip = [&](int track, int scene) { CHECK(track == 0); recordedScene = scene; };
    ctx.launchClip = [&](int, int) { ++launches; };
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    juce::TemporaryFile png(".png"); REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    const auto& m = ctx.theme.metrics;
    auto click = [&](int scene, bool shift) {
        ui::MouseEvent e; e.x = m.outerMargin + 8; e.y = m.controlBarHeight + m.sessionHeaderHeight + 1 + m.sessionSlotHeight * ((float) scene + 0.5f); e.shift = shift;
        root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };
    // Fixture scene 0 is empty and scene 1 has a MIDI clip.
    click(0, false); CHECK(recordedScene == 0); CHECK(launches == 0);
    click(1, false); CHECK(launches == 1);
    click(1, true); CHECK(recordedScene == 1); CHECK(launches == 1);
    ctx.recording = true; ctx.recordingSessionScene = [](int track) { return track == 0 ? 1 : -1; };
    std::vector<int> phaseTracks;
    ctx.sessionRecordingPhase = [&](int track) { phaseTracks.push_back(track); return track == 0 ? 3 : 2; };
    click(1, false); CHECK(recordedScene == 1); CHECK(launches == 1);
    // The Stop All Clips button is the stop square of the Main column.
    ui::SessionView* session = nullptr;
    for (const auto& child : root.children()) if (! session) session = dynamic_cast<ui::SessionView*>(child.get());
    REQUIRE(session);
    const auto stopRow = session->stopRowBounds(ctx.numTracks() + ctx.numReturns());
    ui::MouseEvent stop; stop.x = session->boundsInRoot().x + stopRow.x + 8;
    stop.y = session->boundsInRoot().y + stopRow.centreY();
    root.dispatchMouseDown(stop); root.dispatchMouseUp(stop); CHECK(stops == 1);
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    CHECK(std::find(phaseTracks.begin(), phaseTracks.end(), 0) != phaseTracks.end());
}

TEST_CASE("Session Repeat launch: captured mouse release outside the view ends holding without cancelling the launch")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    REQUIRE(song.tracks[0].slots[1].clip);
    auto& clip = *song.tracks[0].slots[1].clip;
    clip.launchMode = 3;
    engine::SampleCache samples;
    devices::BuildContext buildContext;
    auto built = engine::buildGraph(song, buildContext, samples);
    REQUIRE(built.graph);
    auto& graph = *built.graph;
    engine::Transport transport;
    ui::UiContext ctx;
    ctx.song = &song; ctx.controls = graph.controls;
    ctx.browserVisible = ctx.detailVisible = ctx.ioVisible = false;
    ctx.mainView = ui::MainView::Session;
    int launches = 0, releases = 0;
    ctx.launchClip = [&](int track, int scene) {
        CHECK(track == 0); CHECK(scene == 1); ++launches;
        engine::launchClipWithMode(graph, transport, clip, track, scene, 0.125);
    };
    ctx.releaseClip = [&](int track, int scene) {
        CHECK(track == 0); CHECK(scene == 1); ++releases;
        engine::releaseClip(graph, transport, clip, track, scene, 0.125);
    };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700}); root.layout();
    ui::MouseEvent press;
    const auto& m = ctx.theme.metrics;
    press.x = m.outerMargin + 8;
    press.y = m.controlBarHeight + m.sessionHeaderHeight + 1 + m.sessionSlotHeight * 1.5f;
    root.dispatchMouseDown(press);
    CHECK(launches == 1); CHECK(releases == 0);
    CHECK((*graph.controls)[0].repeatSlot.load() == 1);
    auto outside = press; outside.x = -40; outside.y = -40;
    root.dispatchMouseDrag(outside);
    root.dispatchMouseUp(outside);
    CHECK(releases == 1);
    CHECK((*graph.controls)[0].repeatSlot.load() == engine::TrackControls::kNoSlot);
    CHECK((*graph.controls)[0].queuedSlot.load() == 1);
    press.x += 40; // clip body selects without launching or holding
    root.dispatchMouseDown(press); root.dispatchMouseUp(press);
    CHECK(launches == 1); CHECK(releases == 1);
}

TEST_CASE("Session shortcuts: real focused Delete and Cmd D preserve controller envelopes through history")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; REQUIRE(song.tracks[0].slots[1].clip);
    model::AutomationEnvelope envelope; envelope.midiController = 66; envelope.points = {{0, 127}};
    song.tracks[0].slots[1].clip->envelopes.push_back(envelope);
    const int uid = song.tracks[0].slots[1].clip->uid;
    ui::UiContext ctx; ctx.song = &song; ctx.browserVisible = ctx.detailVisible = ctx.ioVisible = false;
    ctx.mainView = ui::MainView::Session; model::EditHistory history;
    ctx.applyEdit = [&](model::EditCommand edit) { history.perform(song, std::move(edit)); };
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    juce::TemporaryFile png(".png"); REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    ui::MouseEvent mouse; mouse.x = ctx.theme.metrics.outerMargin + 40;
    mouse.y = ctx.theme.metrics.controlBarHeight + ctx.theme.metrics.sessionHeaderHeight + 1 + ctx.theme.metrics.sessionSlotHeight * 1.5f;
    root.dispatchMouseDown(mouse); root.dispatchMouseUp(mouse); CHECK(ctx.selectedClipUid == uid);
    ui::KeyEvent key; key.keyCode = juce::KeyPress::deleteKey;
    CHECK(root.dispatchKey(key)); CHECK(! song.tracks[0].slots[1].clip);
    REQUIRE(history.undo(song)); REQUIRE(song.tracks[0].slots[1].clip); CHECK(song.tracks[0].slots[1].clip->envelopes.back().midiController == 66);
    key = {}; key.cmd = true; key.textChar = 'd'; CHECK(root.dispatchKey(key));
    REQUIRE(song.tracks[0].slots.size() >= 3); REQUIRE(song.tracks[0].slots[2].clip);
    CHECK(song.tracks[0].slots[2].clip->uid != uid); CHECK(song.tracks[0].slots[2].clip->envelopes.back().midiController == 66);
    REQUIRE(history.undo(song)); REQUIRE(history.redo(song)); CHECK(song.tracks[0].slots[2].clip->envelopes.back().points[0].value > 126);
}

#include "devices/DeviceFactory.h"
#include "ui/views/ReferenceFace.h"

// Regression: clicking the EQ Eight curve selects another band, which rebuilt the knobs and used to erase every
// hidden child of the device box (the "Edit A/B" button is hidden in Stereo mode) -> dangling pointer in layout().
// The UI-42 face retargets its knobs instead of rebuilding them; the gestures and checks stay.
TEST_CASE("device view: selecting EQ Eight bands and changing parameters keeps the device box alive")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    engine::SampleCache samples;
    devices::BuildContext build;
    auto built = engine::buildGraph(song, build, samples);
    REQUIRE(built.graph != nullptr);
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.graph = built.graph.get();
    ctx.controls = built.graph->controls;
    ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.detailTab = ui::DetailTab::Devices;
    ctx.selectedTrack = 0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1400, 800});
    root.layout();

    // The EQ Eight face (UI-42) paints its curve display itself.
    ui::ReferenceFace* face = nullptr;
    std::function<void(ui::Widget&)> find = [&](ui::Widget& w) {
        if (auto* f = dynamic_cast<ui::ReferenceFace*>(&w); f && w.visible) face = f;
        for (auto& child : w.children()) find(*child);
    };
    find(root);
    REQUIRE(face != nullptr);
    auto box = face->graphBounds();
    const auto origin = face->boundsInRoot();
    box.x += origin.x; box.y += origin.y;
    for (int i = 0; i < 12; ++i)
    {
        ui::MouseEvent e;
        e.x = box.x + 10.0f + (float) i * (box.w - 20.0f) / 11.0f;
        e.y = box.y + box.h * 0.5f + (float) (i % 3 - 1) * 12.0f;
        root.dispatchMouseDown(e);
        root.dispatchMouseDrag(e);
        root.dispatchMouseUp(e);
        root.layout();
        juce::TemporaryFile png(".png");
        REQUIRE(gfx::renderToPng(root, 1400, 800, 1.0f, png.getFile().getFullPathName().toStdString()));
    }
    // Every knob and button of the box can be operated afterwards (the band knobs were rebuilt several times).
    std::vector<ui::Widget*> controls;
    std::function<void(ui::Widget&)> collect = [&](ui::Widget& w) {
        if ((dynamic_cast<ui::Knob*>(&w) != nullptr || dynamic_cast<ui::TextButton*>(&w) != nullptr) && w.visible) controls.push_back(&w);
        for (auto& child : w.children()) collect(*child);
    };
    collect(root);
    CHECK(controls.size() > 8);
    for (auto* w : controls)
    {
        const auto b = w->boundsInRoot();
        ui::MouseEvent down; down.x = b.centreX(); down.y = b.centreY();
        auto drag = down; drag.y -= 12;
        root.dispatchMouseDown(down); root.dispatchMouseDrag(drag); root.dispatchMouseUp(drag);
        root.layout();
    }
    REQUIRE(gfx::renderToPng(root, 1400, 800, 1.0f, juce::TemporaryFile(".png").getFile().getFullPathName().toStdString()));
    CHECK(true); // reaching this point without a crash / sanitizer report is the assertion
}

TEST_CASE("audio clip panel: reverse, gain, pitch, BPM and loop gestures edit the selected clip through real input")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto& clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000; clip.lengthSamples = 96000; clip.warped = true; clip.loop = {0, 4, 0, false};
    clip.warpMarkers = {{0, 0}, {2, 4}};
    clip.gain = 1.0; clip.pitchCoarse = 0; clip.pitchFine = 0.0; // the fixture carries its own values
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    int reversed = 0;
    model::ClipLocation reversedLocation;
    context.reverseClip = [&](const model::ClipLocation& location) { ++reversed; reversedLocation = location; };
    int stems = 0;
    model::ClipLocation stemsLocation;
    context.separateStems = [&](const model::ClipLocation& location) { ++stems; stemsLocation = location; };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 900});
    root.layout();
    using Id = ui::ClipControls::Id;
    auto click = [&](Id id, int clicks = 1) {
        const auto p = audioControl(root, id);
        ui::MouseEvent e; e.x = p.first; e.y = p.second; e.clicks = clicks;
        root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };
    auto drag = [&](Id id, float dy) {
        const auto p = audioControl(root, id);
        ui::MouseEvent e; e.x = p.first; e.y = p.second;
        root.dispatchMouseDown(e);
        e.y -= dy;
        root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    };
    auto current = [&]() -> const model::Clip& { return *context.selectedClip(); };

    click(Id::Reverse);
    CHECK(reversed == 1);
    CHECK(reversedLocation.track == 1);
    CHECK(reversedLocation.uid == clip.uid);

    click(Id::Stems); // splits the sample into stems on new tracks (the app runs the separator)
    CHECK(stems == 1);
    CHECK(stemsLocation.uid == clip.uid);
    context.stemsRunning = true; // the same button now cancels
    click(Id::Stems);
    CHECK(stems == 2);
    context.stemsRunning = false;

    click(Id::Loop);
    REQUIRE(context.selectedClip() != nullptr);
    CHECK(current().loop.enabled);
    click(Id::Loop);
    CHECK(! current().loop.enabled);

    click(Id::Double);
    CHECK_NEAR(current().warpMarkers[1].beats, 8.0, 1e-9);
    click(Id::Half);
    CHECK_NEAR(current().warpMarkers[1].beats, 4.0, 1e-9);

    drag(Id::PitchSt, 12);
    CHECK(current().pitchCoarse == 2);
    drag(Id::PitchCt, -20);
    CHECK_NEAR(current().pitchFine, -10.0, 1e-9);
    click(Id::PitchSt, 2); // double-click resets
    CHECK(current().pitchCoarse == 0);

    // The gain fader follows Live's taper: 0 dB sits about 60 % down, and a drag moves along the fader's travel.
    CHECK_NEAR(ui::ClipControls::gainFaderPosition(24.0), 1.0, 1e-9);
    CHECK_NEAR(ui::ClipControls::gainFaderPosition(-70.0), 0.0, 1e-9);
    CHECK(ui::ClipControls::gainFaderPosition(0.0) > 0.35);
    CHECK(ui::ClipControls::gainFaderPosition(0.0) < 0.45);
    CHECK_NEAR(ui::ClipControls::gainFaderDb(ui::ClipControls::gainFaderPosition(-12.0)), -12.0, 1e-9);
    const float travel = findAudioControls(root)->controlBounds(Id::Gain).h - 8;
    drag(Id::Gain, -20);
    CHECK_NEAR(20.0 * std::log10(current().gain), ui::ClipControls::gainFaderDb(ui::ClipControls::gainFaderPosition(0.0) - 20.0 / travel), 1e-3);
    CHECK(current().gain < 1.0);
    click(Id::Gain, 2);
    CHECK_NEAR(current().gain, 1.0, 1e-9);

    drag(Id::Bpm, -20); // 120 - 1.0
    CHECK_NEAR(60.0 * current().warpMarkers[1].beats / current().warpMarkers[1].seconds, 119.0, 1e-6);

    click(Id::Warp);
    CHECK(! current().warped);
    while (history.canUndo()) history.undo(song);
    CHECK(current().warped);
    CHECK_NEAR(current().warpMarkers[1].beats, 4.0, 1e-9);
}

TEST_CASE("audio clip panel: boxes flow into columns in a short detail view; RAM / HiQ toggle, Fade only for Session clips")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto& clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000; clip.lengthSamples = 96000; clip.warped = true; clip.loop = {0, 4, 0, false};
    clip.warpMarkers = {{0, 0}, {2, 4}};
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto* panel = findAudioControls(root);
    REQUIRE(panel != nullptr);
    using Id = ui::ClipControls::Id;
    // Default (short) detail view: the Audio box sits beside the Clip box instead of below it.
    CHECK(panel->width() >= 2 * panel->columnWidth() - 1);
    const auto start = panel->controlBounds(Id::Start), warp = panel->controlBounds(Id::Warp);
    CHECK(warp.x >= panel->columnWidth() - 1);
    CHECK(warp.y < start.y + 40);
    CHECK(warp.bottom() <= panel->height());
    CHECK(start.bottom() <= panel->height());
    auto click = [&](Id id) {
        const auto p = audioControl(root, id); // scrolls the panel when the control is below its visible part
        ui::MouseEvent e; e.x = p.first; e.y = p.second;
        root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };
    click(Id::Ram);
    CHECK(context.selectedClip()->ram);
    CHECK(history.undoLabel() == "Toggle Clip RAM Mode");
    click(Id::HiQ);
    CHECK(! context.selectedClip()->hiQ);
    click(Id::Fade); // Arrangement clip: disabled, like Live
    CHECK(context.selectedClip()->clipFade);
    CHECK(history.undoLabel() == "Toggle Clip High Quality");
    click(Id::Follow);
    CHECK(context.sampleFollow);
    // A tall detail view stacks the boxes in one column again.
    context.theme.metrics.detailHeight = 520;
    root.layout();
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    CHECK(panel->width() < 1.5f * panel->columnWidth());
    CHECK(panel->controlBounds(Id::Warp).y > panel->controlBounds(Id::ScaleMode).bottom());
}

TEST_CASE("sample view: the clip start and end markers drag in source time and undo")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto& clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000; clip.lengthSamples = 96000; clip.warped = false; clip.loop = {0, 2, 0, false};
    clip.warpMarkers = {{0, 0}, {2, 4}};
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto* view = findSampleView(root);
    REQUIRE(view != nullptr);
    const auto origin = view->boundsInRoot();
    const float waveLeft = origin.x + view->waveArea().x, waveWidth = view->waveArea().w;
    const float startY = origin.y + view->startRowY() + ui::SampleView::kStartRowHeight * 0.5f;
    auto mouse = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    // End marker: starts at the right edge (2 s), drag it to 1 s.
    root.dispatchMouseDown(mouse(waveLeft + waveWidth - 2, startY));
    root.dispatchMouseDrag(mouse(waveLeft + waveWidth * 0.5f, startY));
    root.dispatchMouseUp(mouse(waveLeft + waveWidth * 0.5f, startY));
    CHECK_NEAR(context.selectedClip()->loop.loopEnd, 1.0, 0.01);
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    // Start marker: at 0 s, drag to a quarter of the sample (0.5 s).
    root.dispatchMouseDown(mouse(waveLeft + 1, startY));
    root.dispatchMouseDrag(mouse(waveLeft + waveWidth * 0.25f, startY));
    root.dispatchMouseUp(mouse(waveLeft + waveWidth * 0.25f, startY));
    CHECK_NEAR(context.selectedClip()->loop.loopStart + context.selectedClip()->loop.startRelative, 0.5, 0.01);
    while (history.canUndo()) history.undo(song);
    CHECK_NEAR(context.selectedClip()->loop.loopEnd, 2.0, 1e-9);
    CHECK_NEAR(context.selectedClip()->loop.startRelative, 0.0, 1e-9);
}

TEST_CASE("sample view: the loop row moves the position marker, the loop end and the whole loop (UI-17c)")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto& clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000; clip.lengthSamples = 96000; clip.warped = false; // 2 s, loop units are seconds
    clip.loop = {0.5, 1.5, 0, true};
    model::EditHistory history;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
    auto* view = findSampleView(root);
    REQUIRE(view != nullptr);
    const auto origin = view->boundsInRoot();
    const float left = origin.x + view->waveArea().x, width = view->waveArea().w; // the whole 2 s fit the width
    const float loopY = origin.y + view->loopRowY() + ui::SampleView::kLoopRowHeight * 0.5f;
    auto x = [&](double seconds) { return left + width * (float) (seconds / 2.0); };
    auto drag = [&](double from, double to) {
        ui::MouseEvent e; e.y = loopY;
        e.x = x(from); root.dispatchMouseDown(e);
        e.x = x(to); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    };
    const auto& loop = [&]() -> const model::LoopSettings& { return context.selectedClip()->loop; };
    drag(1.0, 1.25); // the brace body: the loop moves, its length and the clip start stay
    CHECK_NEAR(loop().loopStart, 0.75, 0.01);
    CHECK_NEAR(loop().loopEnd, 1.75, 0.01);
    CHECK_NEAR(loop().loopStart + loop().startRelative, 0.5, 1e-9);
    CHECK(history.undoLabel() == "Move Loop");
    drag(1.75, 1.9); // the brace end
    CHECK_NEAR(loop().loopEnd, 1.9, 0.01);
    CHECK(history.undoLabel() == "Change Loop Length");
    drag(0.75, 0.25); // the position marker
    CHECK_NEAR(loop().loopStart, 0.25, 0.01);
    CHECK_NEAR(loop().loopEnd, 1.9, 0.01);
    CHECK_NEAR(loop().loopStart + loop().startRelative, 0.5, 1e-9);
    CHECK(history.undoLabel() == "Move Loop Position");
    drag(1.0, 3.0); // dragged past the sample end: the loop stops at the end of the sample
    CHECK_NEAR(loop().loopEnd, 2.0, 0.01);
    while (history.canUndo()) history.undo(song);
    CHECK_NEAR(loop().loopStart, 0.5, 1e-9);
    CHECK_NEAR(loop().loopEnd, 1.5, 1e-9);
    CHECK_NEAR(loop().startRelative, 0.0, 1e-9);
}

TEST_CASE("sample view: a playing Session clip shows its playhead from the slot clock (UI-17c)")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tempo = 120;
    model::Clip clip = song.tracks[1].arrangementClips[0];
    clip.sampleRate = 48000; clip.lengthSamples = 96000; // 2 s
    clip.currentStart = 0; clip.currentEnd = 4;
    clip.warped = true; clip.warpMarkers = {{0, 0}, {2, 4}}; // 4 beats = 2 s at 120 BPM
    clip.loop = {0, 4, 0, true};
    song.tracks[1].slots.resize(2);
    song.tracks[1].slots[1].clip = clip;
    ui::UiContext context;
    context.song = &song;
    context.browserVisible = false;
    context.selectedTrack = context.selectedClipTrack = 1;
    context.selectedClipScene = 1;
    context.selectedClipUid = clip.uid;
    context.detailTab = ui::DetailTab::Clip;
    engine::Transport transport;
    context.transport = &transport;
    context.controls = std::make_shared<engine::ControlBank>(song.tracks.size() + song.returns.size() + 1);
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* view = findSampleView(root);
    REQUIRE(view != nullptr);
    REQUIRE(context.selectedClip() != nullptr);
    auto& controls = (*context.controls)[1];
    CHECK(view->playheadSeconds(*context.selectedClip()) < 0); // transport stopped
    transport.play();
    transport.beginBlock(512, 24000); // the request takes effect at the next audio block
    REQUIRE(transport.isPlaying());
    CHECK(view->playheadSeconds(*context.selectedClip()) < 0); // another slot (none) is playing
    controls.playingSlot.store(1);
    controls.sessionElapsed.store(1.0);
    CHECK_NEAR(view->playheadSeconds(*context.selectedClip()), 0.5, 1e-9);
    controls.sessionElapsed.store(5.0); // looping: wraps after 4 beats
    CHECK_NEAR(view->playheadSeconds(*context.selectedClip()), 0.5, 1e-9);
    song.tracks[1].slots[1].clip->loop.enabled = false;
    CHECK(view->playheadSeconds(*context.selectedClip()) < 0); // a one-shot has ended
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, image.getFile().getFullPathName().toStdString()));
}

TEST_CASE("resizing: dividers between panels, Session columns / fader section and Arrangement lanes / headers follow the mouse")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.mainView = ui::MainView::Session;
    int saved = 0;
    ctx.layoutChanged = [&] { ++saved; };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto& m = ctx.theme.metrics;
    auto mouse = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    auto drag = [&](float x0, float y0, float x1, float y1) {
        root.dispatchMouseDown(mouse(x0, y0));
        root.dispatchMouseDrag(mouse((x0 + x1) * 0.5f, (y0 + y1) * 0.5f));
        root.dispatchMouseDrag(mouse(x1, y1));
        root.dispatchMouseUp(mouse(x1, y1));
    };
    const float browserEdge = m.outerMargin + m.browserWidth + m.panelGap * 0.5f;
    drag(browserEdge, 300, browserEdge + 80, 300);
    CHECK_NEAR(m.browserWidth, 480.0, 1e-3);
    CHECK(saved == 1);
    drag(m.outerMargin + m.browserWidth + 3, 300, m.outerMargin + 100, 300); // clamps at the minimum
    CHECK_NEAR(m.browserWidth, 140.0, 1e-3);
    drag(m.outerMargin + m.browserWidth + 3, 300, m.outerMargin + 400 + 3, 300);
    CHECK_NEAR(m.browserWidth, 400.0, 1e-3);

    ctx.detailTab = ui::DetailTab::Clip;
    root.layout();
    const float previousDetailHeight = m.detailHeight;
    const float detailTop = 700 - m.statusBarHeight - m.detailHeight;
    drag(700, detailTop - m.panelGap * 0.5f, 700, detailTop - m.panelGap * 0.5f - 60);
    CHECK_NEAR(m.detailHeight, previousDetailHeight + 60, 1e-3);
    drag(700, 700 - m.statusBarHeight - m.detailHeight - 3, 700, 690); // clamps to leave room for the main view
    CHECK(m.detailHeight < 700 - m.controlBarHeight - m.statusBarHeight - 100);
    m.detailHeight = 300;
    root.layout();

    // Session: a track's right edge changes only that column; the boundary above the mixer resizes the fader section.
    ui::SessionView* session = nullptr;
    for (const auto& child : root.children()) if (! session) session = dynamic_cast<ui::SessionView*>(child.get());
    REQUIRE(session);
    const auto origin = session->boundsInRoot();
    const float edge = origin.x + m.sessionTrackWidth - 1;
    drag(edge, origin.y + 40, edge + 30, origin.y + 40);
    REQUIRE(ctx.sessionColumnWidths.size() >= 1);
    CHECK_NEAR(ctx.sessionColumnWidths[0], m.sessionTrackWidth + 30, 1e-3);
    if (ctx.sessionColumnWidths.size() > 1) CHECK(ctx.sessionColumnWidths[1] == 0.0f);
    const auto stop = session->stopRowBounds(0);
    const float mixerEdge = origin.y + stop.y - 1;
    const float before = m.sessionVolumeHeight;
    drag(origin.x + 200, mixerEdge, origin.x + 200, mixerEdge - 40);
    CHECK_NEAR(m.sessionVolumeHeight, before + 40, 1e-3);

    // Arrangement: lane bottom edge and the header's left edge.
    ctx.mainView = ui::MainView::Arrangement;
    ctx.ioVisible = true;
    root.layout();
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    ui::ArrangementView* arrangement = nullptr;
    for (const auto& child : root.children()) if (! arrangement) arrangement = dynamic_cast<ui::ArrangementView*>(child.get());
    REQUIRE(arrangement);
    const auto a = arrangement->boundsInRoot();
    const auto lane = arrangement->lanes().front();
    const float laneEdge = a.y + lane.y + lane.h - 1;
    drag(a.x + 200, laneEdge, a.x + 200, laneEdge + 30);
    REQUIRE(ctx.laneHeights.size() >= 1);
    CHECK_NEAR(ctx.laneHeights[0], lane.h + 30, 1e-3);
    const float headerEdge = a.x + a.w - ctx.arrangementHeaderWidth();
    const float headerBefore = ctx.arrangementHeaderWidth();
    drag(headerEdge, a.y + 100, headerEdge - 50, a.y + 100);
    CHECK_NEAR(ctx.arrangementHeaderWidth(), headerBefore + 50, 1e-3);
}

#include "devices/native/Delay.h"
#include "devices/native/Reverb.h"
#include "devices/native/Utility.h"
#include "ui/views/DetailView.h"
#include "ui/views/EffectPanels.h"
#include "ui/views/PluginPanel.h"
#include "ui/views/StatusBar.h"
#include "ui/views/SessionView.h"

TEST_CASE("device layout: browser ends above full-width detail and status mixer controls change layout")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    ui::UiContext context;
    context.song = loaded.song.get();
    context.mainView = ui::MainView::Session;
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1440, 900});
    ui::BrowserPanel* browser = nullptr;
    ui::DetailView* detail = nullptr;
    ui::StatusBar* status = nullptr;
    ui::SessionView* session = nullptr;
    for (auto& child : root.children())
    {
        if (auto* b = dynamic_cast<ui::BrowserPanel*>(child.get())) browser = b;
        if (auto* d = dynamic_cast<ui::DetailView*>(child.get())) detail = d;
        if (auto* s = dynamic_cast<ui::StatusBar*>(child.get())) status = s;
        if (auto* s = dynamic_cast<ui::SessionView*>(child.get())) session = s;
    }
    REQUIRE(browser && detail && status && session);
    CHECK_NEAR(browser->bounds().bottom() + context.theme.metrics.panelGap, detail->bounds().y, 0.01);
    CHECK_NEAR(browser->bounds().x, detail->bounds().x, 0.01);
    CHECK_NEAR(detail->bounds().right(), 1440 - context.theme.metrics.outerMargin, 0.01);
    const float stopBefore = session->stopRowBounds(0).y;
    ui::MouseEvent click;
    click.x = 1440 - context.theme.metrics.outerMargin - 29;
    click.y = status->bounds().centreY();
    root.dispatchMouseDown(click); root.dispatchMouseUp(click);
    CHECK(! context.mixerVisible);
    CHECK(session->stopRowBounds(0).y > stopBefore + 100);
    root.dispatchMouseDown(click); root.dispatchMouseUp(click);
    CHECK(context.mixerVisible);
    context.showChoices = [](const auto& labels, const auto&, auto pick) {
        REQUIRE(labels.size() == 3); pick(1);
    };
    click.x = 1440 - context.theme.metrics.outerMargin - 3;
    root.dispatchMouseDown(click); root.dispatchMouseUp(click);
    CHECK(! context.sendsVisible);
    CHECK(context.mixerVisible);
    CHECK(session->stopRowBounds(0).y > stopBefore + 40);
}

TEST_CASE("dedicated Delay panel: sync grid linked editing and graph gestures reach processor")
{
    devices::Delay delay;
    ui::UiContext context;
    ui::RootWidget root(context);
    root.setBounds({0, 0, 400, 166});
    auto* panel = root.add<ui::EffectPanel>(&delay, 0, 0, false);
    panel->setBounds({0, 0, 392, 166});
    ui::MouseEvent e;
    e.x = 106; e.y = 81; // right 16-sixteenth button
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    CHECK_NEAR(delay.getParameter(devices::Delay::SyncedR), 15, 0.001);
    e = {}; e.x = 225; e.y = 60;
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK_NEAR(delay.getParameter(devices::Delay::FilterFrequency), std::sqrt(20.0 * 20000.0), 1);
    CHECK(delay.getParameter(devices::Delay::FilterBandwidth) > 4);
    e = {}; e.x = 20; e.y = 20;
    root.dispatchMouseDown(e); root.dispatchMouseUp(e); root.tickTree();
    CHECK_NEAR(delay.getParameter(devices::Delay::SyncL), 0, 0.001);
    // All sixteen times stay available through free-time/sync controls; the compact grid is the eight reference choices.
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 400, 166, 1, png.getFile().getFullPathName().toStdString()));
}

TEST_CASE("dedicated Reverb panel: diffusion graph edits high and low independently")
{
    devices::Reverb reverb;
    ui::UiContext context;
    ui::RootWidget root(context);
    root.setBounds({0, 0, 500, 166});
    auto* panel = root.add<ui::EffectPanel>(&reverb, 0, 0, false);
    panel->setBounds({0, 0, 500, 166});
    const float originalLow = reverb.getParameter(devices::Reverb::ShelfLoFreq);
    ui::MouseEvent e; e.x = 300; e.y = 50;
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(reverb.getParameter(devices::Reverb::ShelfHiFreq) > 3000);
    CHECK_NEAR(reverb.getParameter(devices::Reverb::ShelfLoFreq), originalLow, 0.001);
    const float high = reverb.getParameter(devices::Reverb::ShelfHiFreq);
    e.x = 220; e.shift = true;
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(reverb.getParameter(devices::Reverb::ShelfLoFreq) < originalLow);
    CHECK_NEAR(reverb.getParameter(devices::Reverb::ShelfHiFreq), high, 0.001);
}

TEST_CASE("plugin XY pad: selected axes drive parameters and none leaves them untouched")
{
    devices::Utility processor; // a deterministic parameter source, no external plugin dependency
    ui::UiContext context;
    ui::RootWidget root(context);
    root.setBounds({0, 0, 150, 170});
    auto* panel = root.add<ui::PluginPanel>(&processor, 0, 12);
    panel->setBounds({0, 0, 150, 170});
    context.showChoices = [](const auto&, const auto&, auto pick) { pick(devices::Utility::Balance + 1); };
    ui::MouseEvent e; e.x = 20; e.y = 160;
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    CHECK(context.pluginPadAssignments.at("0:12")[0] == devices::Utility::Balance);
    e.x = 138; e.y = 20;
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK_NEAR(processor.getParameter(devices::Utility::Balance), 1, 0.01);
    context.showChoices = [](const auto&, const auto&, auto pick) { pick(0); };
    e.x = 20; e.y = 160;
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    e.x = 12; e.y = 80;
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK_NEAR(processor.getParameter(devices::Utility::Balance), 1, 0.01);
    model::Song song; model::Track track; track.id = 42;
    model::Device rack; rack.id = 100; track.devices.push_back(rack); song.tracks.push_back(track);
    context.song = &song;
    context.showChoices = [](const auto&, const auto&, auto pick) { pick(devices::Utility::Balance + 1); };
    auto* first = root.add<ui::PluginPanel>(&processor, 0, 12, 0, 0);
    auto* second = root.add<ui::PluginPanel>(&processor, 0, 12, 0, 1);
    first->setBounds({0, 0, 150, 170}); second->setBounds({0, 0, 150, 170});
    e.x = 20; e.y = 160; first->mouseDown(e); second->mouseDown(e);
    CHECK(context.pluginPadAssignments.contains("42:100/0/12"));
    CHECK(context.pluginPadAssignments.contains("42:100/1/12"));

}

TEST_CASE("device overview: drag navigates chain and wheel clamps to its ends")
{
    model::Song song;
    model::Track track;
    track.kind = model::TrackKind::Midi;
    for (int i = 0; i < 5; ++i)
    {
        auto device = devices::nativeDeviceModel(i % 2 == 0 ? "Delay" : "Reverb");
        device.id = i; track.devices.push_back(std::move(device));
    }
    song.tracks.push_back(std::move(track));
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    REQUIRE(graph.graph);
    ui::UiContext context;
    context.song = &song; context.graph = graph.graph.get(); context.controls = graph.graph->controls;
    ui::MainLayout root(context); root.setBounds({0, 0, 1200, 800});
    ui::DetailView* detail = nullptr;
    ui::StatusBar* status = nullptr;
    for (auto& child : root.children())
    {
        if (auto* d = dynamic_cast<ui::DetailView*>(child.get())) detail = d;
        if (auto* s = dynamic_cast<ui::StatusBar*>(child.get())) status = s;
    }
    REQUIRE(detail && status); REQUIRE(detail->children().size() == 5);
    const float start = detail->children()[0]->bounds().x;
    ui::MouseEvent e; e.x = 1138; e.y = status->bounds().centreY();
    root.dispatchMouseDown(e); root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(detail->children()[0]->bounds().x < start - 200);
    CHECK(context.detailTab == ui::DetailTab::Devices);
    e.wheelDeltaY = 20;
    root.dispatchMouseWheel(e);
    CHECK_NEAR(detail->children()[0]->bounds().x, start, 0.001);
}

TEST_CASE("empty instrument slot: follows MIDI effects and disappears for instruments or audio tracks")
{
    model::Song song;
    model::Track track; track.kind = model::TrackKind::Midi;
    model::Device midiEffect; midiEffect.type = "MidiPitcher"; midiEffect.displayName = "Pitch"; midiEffect.id = 1;
    auto delay = devices::nativeDeviceModel("Delay"); delay.id = 2;
    track.devices = {midiEffect, delay}; song.tracks.push_back(track);
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get();
    ui::MainLayout root(context); root.setBounds({0, 0, 1440, 800});
    ui::DetailView* detail = nullptr;
    for (auto& child : root.children()) if (auto* d = dynamic_cast<ui::DetailView*>(child.get())) detail = d;
    REQUIRE(detail); REQUIRE(detail->children().size() == 2);
    const auto first = detail->children()[0]->bounds(), second = detail->children()[1]->bounds();
    CHECK_NEAR(first.x, 8, 0.01);
    CHECK_NEAR(second.x - first.right(), 150, 0.01); // separator + empty instrument slot
    song.tracks[0].kind = model::TrackKind::Audio;
    root.layout();
    CHECK_NEAR(detail->children()[1]->bounds().x - detail->children()[0]->bounds().right(), 8, 0.01);
    song.tracks[0].kind = model::TrackKind::Midi;
    // Change an existing model device to a missing hosted instrument, retaining a safe prepared processor.
    song.tracks[0].devices[0].plugin = model::PluginInfo{};
    song.tracks[0].devices[0].plugin->isInstrument = true;
    root.layout();
    CHECK_NEAR(detail->children()[1]->bounds().x - detail->children()[0]->bounds().right(), 8, 0.01);
}

TEST_CASE("device title drag: reorders effects with undo, rejects instrument boundary, cancels outside and on Escape")
{
    model::Song song; model::Track track; track.id = 7; track.kind = model::TrackKind::Midi;
    model::Device instrument; instrument.id = 1; instrument.type = "PluginDevice"; instrument.displayName = "Instrument";
    instrument.plugin.emplace(); instrument.plugin->isInstrument = true;
    track.devices.push_back(instrument);
    for (const auto* type : {"Delay", "Reverb", "StereoGain"})
    {
        auto device = devices::nativeDeviceModel(type); device.id = (int) track.devices.size() + 1;
        track.devices.push_back(std::move(device));
    }
    song.tracks.push_back(track);
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.browserVisible = false;
    model::EditHistory history;
    ui::MainLayout root(context); root.setBounds({0, 0, 1600, 800});
    ui::DetailView* detail = nullptr;
    for (auto& child : root.children()) if (auto* d = dynamic_cast<ui::DetailView*>(child.get())) detail = d;
    REQUIRE(detail);
    context.applyEdit = [&](model::EditCommand cmd) {
        // Mirror the app's pointer invalidation during a structural edit. The captured DeviceBox dies here.
        context.song = nullptr; root.layout();
        history.perform(song, std::move(cmd));
        context.song = &song; root.layout();
    };
    auto event = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    auto drag = [&](ui::Rect source, ui::Rect destination) {
        root.dispatchMouseDown(event(source.x + 80, source.y + 9));
        root.dispatchMouseDrag(event(destination.x + 23, destination.y + 9));
        root.dispatchMouseUp(event(destination.x + 23, destination.y + 9));
    };
    auto box = [&](size_t index) { return detail->children()[index]->boundsInRoot(); };
    context.pluginPadAssignments["7:1"] = {1, 2};
    drag(box(2), box(1));
    CHECK(song.tracks[0].devices[1].id == 3); CHECK(song.tracks[0].devices[2].id == 2);
    CHECK(context.pluginPadAssignments.contains("7:1"));
    CHECK(context.selectedDeviceId == 3); CHECK(history.canUndo());
    CHECK(history.undoLabel() == "Move Reverb");
    drag(box(1), box(0)); // effects cannot precede the instrument
    CHECK(song.tracks[0].devices[0].id == 1); CHECK(song.tracks[0].devices[1].id == 3);
    drag(box(0), box(2)); // instrument titles cannot start an audio-effect move
    CHECK(song.tracks[0].devices[0].id == 1);
    const auto source = box(1);
    root.dispatchMouseDown(event(source.x + 80, source.y + 9));
    root.dispatchMouseDrag(event(source.x + 450, source.y - 50));
    root.dispatchMouseUp(event(source.x + 450, source.y - 50));
    CHECK(song.tracks[0].devices[1].id == 3);
    root.dispatchMouseDown(event(source.x + 80, source.y + 9));
    root.dispatchMouseDrag(event(box(2).right() - 10, source.y + 9));
    ui::KeyEvent escape; escape.keyCode = juce::KeyPress::escapeKey;
    CHECK(root.dispatchKey(escape));
    root.dispatchMouseUp(event(box(2).right() - 10, source.y + 9));
    CHECK(song.tracks[0].devices[1].id == 3);
    REQUIRE(history.undo(song)); root.layout();
    CHECK(song.tracks[0].devices[1].id == 2); CHECK(! history.canUndo());
    REQUIRE(history.redo(song));
    CHECK(song.tracks[0].devices[1].id == 3);
    model::Song otherSong; context.song = &otherSong; root.layout();
    CHECK(context.pluginPadAssignments.empty());
}

TEST_CASE("device title drag: edge scrolling moves viewport while recording and controls never start a reorder")
{
    model::Song song; model::Track track; track.kind = model::TrackKind::Audio;
    for (int i = 0; i < 5; ++i) { auto d = devices::nativeDeviceModel("Delay"); d.id = i; track.devices.push_back(d); }
    song.tracks.push_back(track);
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.browserVisible = false;
    int edits = 0; context.applyEdit = [&](auto) { ++edits; };
    ui::MainLayout root(context); root.setBounds({0, 0, 800, 700});
    ui::DetailView* detail = nullptr;
    for (auto& child : root.children()) if (auto* d = dynamic_cast<ui::DetailView*>(child.get())) detail = d;
    REQUIRE(detail);
    const auto source = detail->children()[0]->boundsInRoot();
    ui::MouseEvent e; e.x = source.x + 80; e.y = source.y + 9;
    root.dispatchMouseDown(e); e.x = detail->boundsInRoot().right() - 10;
    root.dispatchMouseDrag(e); root.tickTree(); root.tickTree();
    CHECK(detail->children()[0]->boundsInRoot().x < source.x);
    ui::KeyEvent escape; escape.keyCode = juce::KeyPress::escapeKey; root.dispatchKey(escape); root.dispatchMouseUp(e);
    CHECK(edits == 0);
    detail->navigateOverview(0); context.recording = true;
    e.x = source.x + 80; root.dispatchMouseDown(e); e.x += 450; root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(edits == 0); CHECK_NEAR(detail->children()[0]->boundsInRoot().x, source.x, 0.01);
}

#include "ui/views/AutomationEditor.h"
#include "core/DeviceCatalog.h"

TEST_CASE("automation choices: separate device and parameter menus disambiguate repeated devices and retain target identities")
{
    model::Song song; song.tracks.resize(1);
    auto& track = song.tracks[0]; track.kind = model::TrackKind::Audio;
    track.mixer.volumeTargetId = 100; track.mixer.panTargetId = 101;
    model::Device device; device.type = "StereoGain"; device.displayName = "Utility"; device.enabledTargetId = 200;
    model::Parameter parameter; parameter.path = "Gain"; parameter.value = 0.5; parameter.automationTargetId = 201;
    device.parameters.push_back(parameter); track.devices.push_back(device);
    device.enabledTargetId = 300; device.parameters[0].automationTargetId = 301; track.devices.push_back(device);
    ui::UiContext ctx; ctx.song = &song;
    model::EditHistory history; ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    std::vector<std::string> labels; std::vector<bool> ticks; std::function<void(int)> choose;
    ctx.showChoices = [&](const auto& names, const auto& selected, auto pick) { labels = names; ticks = selected; choose = std::move(pick); };
    ui::RootWidget root(ctx); root.setBounds({0, 0, 900, 70});
    auto* editor = root.add<ui::AutomationEditor>(0); editor->setBounds(root.localBounds());
    auto click = [&](float y) { ui::MouseEvent e; e.x = 900 - ctx.arrangementHeaderWidth() + 20; e.y = y; root.dispatchMouseDown(e); root.dispatchMouseUp(e); };
    click(10); REQUIRE(labels.size() == 3); CHECK(labels[0] == "Mixer"); CHECK(labels[1] == "1 Utility"); CHECK(labels[2] == "2 Utility");
    choose(2); click(30); REQUIRE(labels.size() == 2); CHECK(labels[0] == "On"); CHECK(labels[1] == "Gain"); choose(1);
    click(30); CHECK(ticks[1]); CHECK(! ticks[0]);
    // Creating a point now writes the second Utility's target, without cycling unrelated targets.
    ui::MouseEvent point; point.x = 160; point.y = 30; point.clicks = 2;
    root.dispatchMouseDown(point); root.dispatchMouseUp(point);
    REQUIRE(track.automation.size() == 1); CHECK(track.automation[0].targetId == 301);
    click(30); CHECK(labels[1] == "● Gain");
    CHECK(history.undoLabel() == "Edit Automation");
}

TEST_CASE("MIDI-only mixer: hides audio controls and restores them when an instrument is inserted")
{
    model::Song song; song.tracks.resize(1); song.scenes.resize(8);
    auto& track = song.tracks[0]; track.kind = model::TrackKind::Midi; track.id = 0;
    CHECK(model::isMidiOnly(track));
    model::Device effect; effect.type = "Delay"; track.devices.push_back(effect); CHECK(model::isMidiOnly(track));
    ui::UiContext ctx; ctx.song = &song; ctx.mainView = ui::MainView::Session;
    ctx.browserVisible = ctx.detailVisible = false;
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1200, 700}); root.layout();
    ui::Fader* fader = nullptr; ui::Knob* pan = nullptr;
    std::function<void(ui::Widget&)> find = [&](ui::Widget& widget) {
        if (auto* f = dynamic_cast<ui::Fader*>(&widget); f && ! fader) fader = f;
        if (auto* k = dynamic_cast<ui::Knob*>(&widget); k && ! pan) pan = k;
        for (const auto& child : widget.children()) find(*child);
    };
    find(root); REQUIRE(fader); REQUIRE(pan); CHECK(! fader->visible); CHECK(! pan->visible);
    model::Device instrument; instrument.type = "InstrumentGroupDevice"; track.devices.insert(track.devices.begin(), instrument);
    CHECK(! model::isMidiOnly(track)); root.layout();
    // Rebuilding may replace the widgets, so retrieve them again.
    fader = nullptr; pan = nullptr; find(root); REQUIRE(fader); REQUIRE(pan);
    CHECK(fader->visible); CHECK(pan->visible);
}

TEST_CASE("scale: the note editors follow the selected clip's scale, the control bar the set's (UI-30b)")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    song.scaleRoot = 2; song.scaleName = 2;                  // set: D Dorian
    auto& clip = song.tracks[0].arrangementClips[0];
    clip.scaleRoot = 9; clip.scaleName = 1;                  // clip: A Minor
    ui::UiContext context;
    context.song = &song;
    CHECK(context.editingScaleRoot() == 2);                  // nothing selected: the set scale
    CHECK(context.pitchInScale(62 + 11) == false);           // C# is not in D Dorian
    context.selectedTrack = context.selectedClipTrack = 0;
    context.selectedClipUid = clip.uid;
    REQUIRE(context.selectedClip() != nullptr);
    CHECK(context.scaleRoot() == 2);                         // control bar: still the set
    CHECK(context.editingScaleRoot() == 9);
    CHECK(context.editingScaleName() == 1);
    CHECK(context.pitchInScale(69 + 3));                     // C in A minor
    CHECK(! context.pitchInScale(69 + 4));                   // C# is not
}
