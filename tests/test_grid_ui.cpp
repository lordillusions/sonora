#include "TestFramework.h"

#include "engine/Transport.h"
#include "engine/EngineGraph.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "ui/RootWidget.h"
#include "ui/views/ArrangementView.h"
#include "engine/MasterMonitor.h"
#include "ui/views/MainLayout.h"
#include "ui/views/MonitorPanel.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/SessionView.h"

#include <juce_core/juce_core.h>

#include <functional>

using namespace sonora;

namespace
{
template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* hit = dynamic_cast<T*>(&w)) return hit;
    for (auto& child : w.children())
        if (auto* hit = findWidget<T>(*child)) return hit;
    return nullptr;
}

ui::MouseEvent mouseAt(float x, float y, int clicks = 1)
{
    ui::MouseEvent e;
    e.x = x;
    e.y = y;
    e.clicks = clicks;
    return e;
}

struct RollRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    ui::PianoRoll* roll = nullptr;

    RollRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        REQUIRE(loaded);
        song = *loaded.song;
        ctx.song = &song;
        ctx.ioVisible = false;
        ctx.browserVisible = false;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.detailTab = ui::DetailTab::Clip;
        ctx.selectedClipTrack = 0;
        ctx.selectedClipUid = song.tracks[0].arrangementClips[0].uid;
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 700});
        root->layout();
        render();
        roll = findWidget<ui::PianoRoll>(*root);
        REQUIRE(roll != nullptr);
    }
    void render()
    {
        juce::TemporaryFile png(".png");
        REQUIRE(gfx::renderToPng(*root, 1200, 700, 1.0f, png.getFile().getFullPathName().toStdString()));
    }
    std::pair<float, float> centre() const
    {
        const auto b = roll->boundsInRoot();
        return {b.x + b.w * 0.5f, b.y + b.h * 0.4f};
    }
    void wheel(float dx, float dy, bool cmd = false, bool shift = false, bool alt = false)
    {
        const auto p = centre();
        ui::MouseEvent e = mouseAt(p.first, p.second);
        e.wheelDeltaX = dx;
        e.wheelDeltaY = dy;
        e.cmd = cmd;
        e.shift = shift;
        e.alt = alt;
        root->dispatchMouseWheel(e);
    }
    double visibleRows() const { return roll->noteAreaBounds().h / roll->view.rowHeight; }
};
} // namespace

TEST_CASE("piano roll: the wheel scrolls smoothly through all 128 pitches, small trackpad deltas included")
{
    RollRig rig;
    const double start = rig.roll->view.topPitch;
    for (int i = 0; i < 40; ++i)
        rig.wheel(0.0f, -0.02f); // tiny deltas used to round to zero rows and never moved the view
    CHECK(rig.roll->view.topPitch < start - 2.0);

    for (int i = 0; i < 400; ++i)
        rig.wheel(0.0f, -0.5f);
    // pitch 0 sits at the bottom edge, never further
    CHECK_NEAR(rig.roll->view.topPitch - rig.visibleRows() + 1.0, 0.0, 1e-6);
    for (int i = 0; i < 400; ++i)
        rig.wheel(0.0f, 0.5f);
    CHECK_NEAR(rig.roll->view.topPitch, 127.0, 1e-9);
}

TEST_CASE("piano roll: opening a one-bar clip fits its content and preserves manual zoom until explicitly reopened")
{
    RollRig rig;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.currentStart = 32; clip.currentEnd = 36; clip.loop = {4, 8, 0, true};
    ++rig.ctx.clipZoomRequest; rig.render();
    CHECK_NEAR(rig.roll->view.scrollBeats, 4, 1e-9);
    CHECK_NEAR(rig.roll->view.pixelsPerBeat * 4, rig.roll->width() - 41, 1e-3);
    rig.roll->view.pixelsPerBeat = 99;
    rig.render(); CHECK_NEAR(rig.roll->view.pixelsPerBeat, 99, 1e-9);
    ++rig.ctx.clipZoomRequest; rig.render();
    CHECK_NEAR(rig.roll->view.pixelsPerBeat * 4, rig.roll->width() - 41, 1e-3);
}

TEST_CASE("piano roll: keys audition and glide without editing notes, release outside and on focus or document change")
{
    RollRig rig;
    rig.ctx.controls = std::make_shared<engine::ControlBank>(4);
    const auto keys = rig.roll->boundsInRoot();
    const auto count = rig.song.tracks[0].arrangementClips[0].notes.size();
    const auto key = mouseAt(keys.x + 20, keys.y + rig.roll->noteAreaBounds().y + 16);
    rig.root->dispatchMouseDown(key);
    const int pitch = (*rig.ctx.controls)[0].previewPitch.load(); CHECK(pitch >= 0);
    auto next = key; next.y += rig.roll->view.rowHeight;
    rig.root->dispatchMouseDrag(next); CHECK((*rig.ctx.controls)[0].previewPitch.load() == pitch - 1);
    next.x += 200; rig.root->dispatchMouseUp(next);
    CHECK((*rig.ctx.controls)[0].previewPitch.load() == -1);
    CHECK(rig.song.tracks[0].arrangementClips[0].notes.size() == count);
    rig.root->dispatchMouseDown(key); rig.root->setKeyboardFocus(nullptr);
    CHECK((*rig.ctx.controls)[0].previewPitch.load() == -1);
    rig.root->dispatchMouseDown(key); auto old = rig.ctx.controls;
    rig.ctx.controls = std::make_shared<engine::ControlBank>(4); rig.roll->tick();
    CHECK((*old)[0].previewPitch.load() == -1);
}

TEST_CASE("piano scale: Scale button folds to the selected key while retaining out-of-scale notes")
{
    RollRig rig;
    rig.ctx.controls = std::make_shared<engine::ControlBank>(4);
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    auto outside = clip.notes[0]; outside.pitch = 66; clip.notes.push_back(outside);
    const auto bounds = rig.roll->boundsInRoot();
    const auto scaleButton = rig.roll->headerControlBounds(ui::PianoRoll::HeaderControl::Scale);
    auto toggle = mouseAt(bounds.x + scaleButton.centreX(), bounds.y + scaleButton.centreY());
    rig.root->dispatchMouseDown(toggle); rig.root->dispatchMouseUp(toggle);
    CHECK(rig.ctx.scaleFold); CHECK(rig.ctx.scaleHighlight);
    rig.render(); rig.roll->view.topPitch = 43; rig.render(); // C4, after retaining the F# already in the clip
    const std::vector<int> expected{72, 71, 69, 67, 66, 65};
    for (size_t i = 0; i < expected.size(); ++i)
    {
        auto key = mouseAt(bounds.x + 20, bounds.y + rig.roll->noteAreaBounds().y + 5 + (float) i * rig.roll->view.rowHeight);
        rig.root->dispatchMouseDown(key);
        CHECK((*rig.ctx.controls)[0].previewPitch.load() == expected[i]);
        rig.root->dispatchMouseUp(key);
    }
    CHECK(clip.notes.back().pitch == 66); // folding never changes the authored notes
    rig.root->dispatchMouseDown(toggle); rig.root->dispatchMouseUp(toggle); CHECK(! rig.ctx.scaleFold);
    REQUIRE(rig.root->dispatchKey({.textChar = 'k'})); CHECK(! rig.ctx.scaleHighlight);
}

TEST_CASE("piano roll: transposed notes stay in view and a drag near the bottom edge scrolls down")
{
    RollRig rig;
    ui::KeyEvent selectAll;
    selectAll.cmd = true;
    selectAll.textChar = 'a';
    rig.root->dispatchKey(selectAll); // the piano roll needs focus first
    const auto p = rig.centre();
    rig.root->dispatchMouseDown(mouseAt(p.first, p.second));
    rig.root->dispatchMouseUp(mouseAt(p.first, p.second));
    rig.root->dispatchKey(selectAll);

    ui::KeyEvent down;
    down.keyCode = juce::KeyPress::downKey;
    down.shift = true; // an octave per press
    for (int i = 0; i < 4; ++i)
        rig.root->dispatchKey(down);
    const auto& notes = rig.song.tracks[0].arrangementClips[0].notes;
    int lowest = 127;
    for (const auto& n : notes) lowest = std::min(lowest, n.pitch);
    CHECK(lowest < 30); // the notes really moved far below the initial view
    // the lowest selected note must be inside the visible rows
    CHECK(lowest >= rig.roll->view.topPitch - rig.visibleRows() + 1.0 - 1e-6);
    CHECK(lowest <= rig.roll->view.topPitch + 1e-6);
}

TEST_CASE("piano roll: Cmd+wheel and pinch zoom around the pointer, grid shortcuts and the right-click menu target the clip grid")
{
    RollRig rig;
    rig.roll->view.scrollBeats = 30.0; // far enough from the start that zooming never hits the left limit
    const auto p = rig.centre();
    const auto area = rig.roll->boundsInRoot();
    auto beatAt = [&](float x) { return rig.roll->view.scrollBeats + (x - area.x - 40.0f) / rig.roll->view.pixelsPerBeat; };
    const double before = beatAt(p.first);
    const double ppb = rig.roll->view.pixelsPerBeat;
    rig.wheel(0.0f, 0.1f, true);
    CHECK(rig.roll->view.pixelsPerBeat > ppb); // scroll up zooms in (about +25 % per mouse notch)
    CHECK_NEAR(beatAt(p.first), before, 1e-6);  // the beat under the pointer did not move
    rig.wheel(0.0f, -0.25f, true);
    CHECK(rig.roll->view.pixelsPerBeat < ppb);
    CHECK_NEAR(beatAt(p.first), before, 1e-6);

    const double beforePinch = rig.roll->view.pixelsPerBeat;
    rig.root->dispatchMouseMagnify(mouseAt(p.first, p.second), 1.25f);
    CHECK_NEAR(rig.roll->view.pixelsPerBeat, beforePinch * 1.25, 1e-6);
    CHECK_NEAR(beatAt(p.first), before, 1e-6);

    // Cmd+3 triplet, Cmd+4 snap, Cmd+5 fixed, Cmd+1 narrower, once the roll has focus
    rig.root->dispatchMouseDown(mouseAt(p.first, p.second));
    rig.root->dispatchMouseUp(mouseAt(p.first, p.second));
    auto cmdKey = [&](char c) { ui::KeyEvent k; k.cmd = true; k.textChar = (uint32_t) c; rig.root->dispatchKey(k); };
    CHECK(! rig.ctx.clipGrid.triplet);
    cmdKey('3');
    CHECK(rig.ctx.clipGrid.triplet);
    cmdKey('4');
    CHECK(! rig.ctx.clipGrid.snap);
    cmdKey('5');
    CHECK(rig.ctx.clipGrid.fixed);
    const int index = rig.ctx.clipGrid.fixedIndex;
    cmdKey('1');
    CHECK(rig.ctx.clipGrid.fixedIndex == index + 1);
    cmdKey('2');
    cmdKey('2');
    CHECK(rig.ctx.clipGrid.fixedIndex == index - 1);
    CHECK(rig.ctx.activeGrid().fixed); // the clip editor was the last one used
}

TEST_CASE("piano roll: Cmd while dragging, or Snap to Grid off, moves notes freely; a fixed grid keeps its size when zooming")
{
    RollRig rig;
    rig.ctx.clipGrid.fixed = true;
    rig.ctx.clipGrid.fixedIndex = 6; // 1/8 = half a beat
    const auto& notes = rig.song.tracks[0].arrangementClips[0].notes;
    const auto area = rig.roll->boundsInRoot();
    const double ppb = rig.roll->view.pixelsPerBeat;
    const float rowY = area.y + rig.roll->noteAreaBounds().y + (float) (rig.roll->view.topPitch - notes[0].pitch) * rig.roll->view.rowHeight + 5.0f;
    auto xAt = [&](double beat) { return area.x + 40.0f + (float) ((beat - rig.roll->view.scrollBeats) * ppb); };
    const double original = notes[0].start;

    // plain drag by ~0.37 beat snaps to the fixed 1/8 grid
    rig.root->dispatchMouseDown(mouseAt(xAt(original + 0.1), rowY));
    rig.root->dispatchMouseDrag(mouseAt(xAt(original + 0.47), rowY));
    rig.root->dispatchMouseUp(mouseAt(xAt(original + 0.47), rowY));
    CHECK_NEAR(std::fmod(rig.song.tracks[0].arrangementClips[0].notes[0].start, 0.5), 0.0, 1e-9);

    // with Cmd held the same drag is free
    rig.history.undo(rig.song);
    ui::MouseEvent down = mouseAt(xAt(original + 0.1), rowY), drag = mouseAt(xAt(original + 0.47), rowY);
    drag.cmd = true;
    rig.root->dispatchMouseDown(down);
    rig.root->dispatchMouseDrag(drag);
    rig.root->dispatchMouseUp(drag);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].notes[0].start, original + 0.37, 0.03);
    CHECK(std::fabs(std::fmod(rig.song.tracks[0].arrangementClips[0].notes[0].start, 0.5)) > 1e-3);

    // zooming does not change a fixed grid
    const double g = ui::gridBeats(rig.ctx.clipGrid, rig.roll->view.pixelsPerBeat, 4.0);
    rig.wheel(0.0f, 1.0f, true);
    CHECK_NEAR(ui::gridBeats(rig.ctx.clipGrid, rig.roll->view.pixelsPerBeat, 4.0), g, 1e-12);
}

TEST_CASE("arrangement: clicking the ruler plays from that position (snapped), lane clicks set the insert marker, Cmd+1..5 edit the grid")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    engine::Transport transport;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.transport = &transport;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 40.0; // adaptive grid: 1/16
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    const auto& m = ctx.theme.metrics;
    auto xAt = [&](double beat) { return origin.x + (float) ((beat - ctx.scrollBeats) * ctx.pixelsPerBeat); };

    // the scrub area is the number strip of the ruler
    const float scrubY = origin.y + 22.0f;
    root.dispatchMouseDown(mouseAt(xAt(8.1), scrubY)); // snaps to the 1/16 grid: 8.0
    root.dispatchMouseUp(mouseAt(xAt(8.1), scrubY));
    auto block = transport.beginBlock();
    CHECK(block.playing);
    CHECK_NEAR(block.position, 8.0, 1e-9);

    // dragging scrubs; Cmd skips the snap
    root.dispatchMouseDown(mouseAt(xAt(2.0), scrubY));
    ui::MouseEvent drag = mouseAt(xAt(5.1), scrubY);
    drag.cmd = true;
    root.dispatchMouseDrag(drag);
    root.dispatchMouseUp(drag);
    block = transport.beginBlock();
    CHECK_NEAR(block.position, 5.1, 0.03);

    // stop, then click empty lane space: the insert marker moves, playback stays stopped
    transport.stop();
    transport.beginBlock();
    const float laneY = origin.y + m.arrangementRulerHeight + m.arrangementLaneHeight + 20.0f; // the empty MIDI lane
    root.dispatchMouseDown(mouseAt(xAt(20.2), laneY));
    root.dispatchMouseUp(mouseAt(xAt(20.2), laneY));
    block = transport.beginBlock();
    CHECK(! block.playing);
    CHECK_NEAR(block.position, 20.25, 1e-9);

    // grid shortcuts apply to the Arrangement grid once it has been clicked
    auto cmdKey = [&](char c) { ui::KeyEvent k; k.cmd = true; k.textChar = (uint32_t) c; root.dispatchKey(k); };
    cmdKey('3');
    CHECK(ctx.arrangementGrid.triplet);
    cmdKey('4');
    CHECK(! ctx.arrangementGrid.snap);
    CHECK(! ctx.clipGrid.triplet);
    CHECK(! ctx.activeGrid().snap);
}

TEST_CASE("arrangement: + and - and Cmd+wheel zoom around the pointer, Shift+wheel scrolls in time")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 20.0;
    ctx.scrollBeats = 10.0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    const float px = origin.x + 300.0f, py = origin.y + 150.0f;
    const double beatUnder = ctx.scrollBeats + 300.0 / ctx.pixelsPerBeat;

    ui::MouseEvent wheel = mouseAt(px, py);
    wheel.cmd = true;
    wheel.wheelDeltaY = 0.5f;
    root.dispatchMouseWheel(wheel);
    CHECK(ctx.pixelsPerBeat > 20.0);
    CHECK_NEAR(ctx.scrollBeats + 300.0 / ctx.pixelsPerBeat, beatUnder, 1e-9);
    wheel.wheelDeltaY = -0.5f;
    root.dispatchMouseWheel(wheel);
    CHECK_NEAR(ctx.pixelsPerBeat, 20.0, 1e-6);

    root.dispatchMouseMagnify(mouseAt(px, py), 2.0f);
    CHECK_NEAR(ctx.pixelsPerBeat, 40.0, 1e-6);
    CHECK_NEAR(ctx.scrollBeats + 300.0 / ctx.pixelsPerBeat, beatUnder, 1e-9);

    ui::MouseEvent shiftWheel = mouseAt(px, py);
    shiftWheel.shift = true;
    shiftWheel.wheelDeltaY = -1.0f; // scrolling "down" moves forward in time
    const double scroll = ctx.scrollBeats;
    root.dispatchMouseWheel(shiftWheel);
    CHECK(ctx.scrollBeats > scroll);
}

TEST_CASE("arrangement: double-click on an empty MIDI lane creates a one-bar clip at the grid position, never over the next clip")
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
    ctx.pixelsPerBeat = 40.0;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    const auto& m = ctx.theme.metrics;
    const float laneY = origin.y + m.arrangementRulerHeight + m.arrangementLaneHeight + 20.0f;
    auto xAt = [&](double beat) { return origin.x + (float) (beat * ctx.pixelsPerBeat); };

    root.dispatchMouseDown(mouseAt(xAt(9.9), laneY));
    root.dispatchMouseUp(mouseAt(xAt(9.9), laneY));
    root.dispatchMouseDown(mouseAt(xAt(9.9), laneY, 2));
    root.dispatchMouseUp(mouseAt(xAt(9.9), laneY, 2));
    REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, 9.75, 1e-9); // 1/16 grid at 40 px per beat
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentEnd, 13.75, 1e-9);  // one bar long

    // a second clip placed 2 beats before the first is cut short by it
    root.dispatchMouseDown(mouseAt(xAt(7.2), laneY, 2));
    root.dispatchMouseUp(mouseAt(xAt(7.2), laneY, 2));
    REQUIRE(song.tracks[1].arrangementClips.size() == 2);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, 7.0, 1e-9);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentEnd, 9.75, 1e-9);
}

#include "io/als/AlsNewSet.h"

TEST_CASE("track groups: Shift/Cmd+click track names select several tracks and Cmd+G / Shift+Cmd+G group and ungroup them")
{
    for (auto mainView : {ui::MainView::Arrangement, ui::MainView::Session})
    {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(mainView == ui::MainView::Session ? "sonora_group_session_ui" : "sonora_group_ui");
    dir.deleteRecursively();
    dir.createDirectory();
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), dir.getChildFile("Set.als"), error));
    auto loaded = io::loadAls(dir.getChildFile("Set.als"));
    REQUIRE((bool) loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = mainView;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ctx.groupSelectedTracks = [&] {
        std::vector<int> members;
        if (! model::groupableSelection(song, ctx.selectedTrackIndices(), members)) return;
        int end = 0;
        auto group = io::createGroupTrack(song, "Group", song.tracks[(size_t) members.front()].colorIndex, end);
        ctx.applyEdit(model::groupTracks(song, members, std::move(group), end));
        ctx.trackSelection.clear();
        ctx.selectedTrack = members.front();
    };
    ctx.ungroupSelectedTracks = [&] {
        if (ctx.selectedTrack < ctx.numTracks() && song.tracks[(size_t) ctx.selectedTrack].kind == model::TrackKind::Group)
            ctx.applyEdit(model::ungroupTrack(song, ctx.selectedTrack));
    };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    auto* session = findWidget<ui::SessionView>(root);
    REQUIRE(session);
    auto headerPoint = [&](int track) {
        if (mainView == ui::MainView::Session)
        {
            const auto bounds = session->boundsInRoot();
            return std::pair<float, float>{bounds.x + session->stopRowBounds(track).x + 25, bounds.y + 12};
        }
        for (const auto& lane : arrangement->lanes())
            if (lane.controlIndex == track && lane.takeLaneId < 0)
                return std::pair<float, float>{origin.x + arrangement->width() - ctx.arrangementHeaderWidth() + 25.0f, origin.y + lane.y + 30.0f};
        return std::pair<float, float>{0, 0};
    };
    auto click = [&](int track, bool shift, bool cmd) {
        const auto p = headerPoint(track);
        ui::MouseEvent e = mouseAt(p.first, p.second);
        e.shift = shift;
        e.cmd = cmd;
        root.dispatchMouseDown(e);
        root.dispatchMouseUp(e);
    };
    auto key = [&](char c, bool shift) { ui::KeyEvent k; k.cmd = true; k.shift = shift; k.textChar = (uint32_t) c; root.dispatchKey(k); };

    click(1, false, false);
    click(2, true, false); // Shift+click: range 1..2
    CHECK(ctx.selectedTrackIndices() == std::vector<int>({1, 2}));
    click(3, false, true); // Cmd+click adds
    CHECK(ctx.selectedTrackIndices() == std::vector<int>({1, 2, 3}));
    click(2, false, true); // Cmd+click removes
    CHECK(ctx.selectedTrackIndices() == std::vector<int>({1, 3}));
    if (mainView == ui::MainView::Session)
    {
        juce::TemporaryFile file(".png");
        REQUIRE(gfx::renderToPng(root, 1200, 700, 1, file.getFile().getFullPathName().toStdString()));
        auto png = juce::ImageFileFormat::loadFrom(file.getFile()); REQUIRE(png.isValid());
        for (int index : {1, 3})
        {
            const auto point = headerPoint(index);
            CHECK(png.getPixelAt((int) point.first, (int) session->boundsInRoot().y + 2).getARGB() == ctx.theme.color("SelectionBackground"));
        }
        CHECK(ctx.selectedClipUid == -1); CHECK(ctx.detailTab == ui::DetailTab::Devices);
    }
    // non-adjacent tracks group too: the group takes the place of the first one and the selected tracks follow it
    const auto idBefore = std::vector<int>{song.tracks[0].id, song.tracks[1].id, song.tracks[2].id, song.tracks[3].id};
    key('g', false);
    REQUIRE(song.tracks.size() == 5);
    CHECK(song.tracks[0].id == idBefore[0]);
    CHECK(song.tracks[1].kind == model::TrackKind::Group);
    CHECK(song.tracks[2].id == idBefore[1]);
    CHECK(song.tracks[3].id == idBefore[3]);
    CHECK(song.tracks[4].id == idBefore[2]); // the track in between that was not selected now comes after the group
    CHECK(song.tracks[2].groupId == song.tracks[1].id);
    CHECK(song.tracks[3].groupId == song.tracks[1].id);
    CHECK(song.tracks[4].groupId == -1);
    history.undo(song);
    REQUIRE(song.tracks.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(song.tracks[i].id == idBefore[i]);
    root.layout(); // the app's modelChanged hook does this after every edit

    click(1, false, false);
    click(2, true, false);
    key('g', false);
    REQUIRE(song.tracks.size() == 5);
    CHECK(song.tracks[1].kind == model::TrackKind::Group);
    CHECK(song.tracks[2].groupId == song.tracks[1].id);
    CHECK(song.tracks[3].groupId == song.tracks[1].id);

    key('g', true); // Shift+Cmd+G with the group selected
    CHECK(song.tracks.size() == 4);
    CHECK(song.tracks[1].groupId == -1);
    history.undo(song); // ungroup undone: the group is back
    CHECK(song.tracks.size() == 5);
    }
}

#include "ui/views/DetailView.h"
#include "ui/views/StatusBar.h"

TEST_CASE("piano roll: dragging on empty space draws a selection rectangle that selects the notes inside it")
{
    RollRig rig;
    const auto& notes = rig.song.tracks[0].arrangementClips[0].notes;
    REQUIRE(notes.size() == 2);
    const auto area = rig.roll->boundsInRoot();
    const double ppb = rig.roll->view.pixelsPerBeat;
    auto xAt = [&](double beat) { return area.x + 40.0f + (float) ((beat - rig.roll->view.scrollBeats) * ppb); };
    auto yAt = [&](double pitchRow) { return area.y + rig.roll->noteAreaBounds().y + (float) ((rig.roll->view.topPitch - pitchRow) * rig.roll->view.rowHeight); };
    // from above-left of the first note (0.75, pitch 65) to just past its end: the second note (2.75) stays outside
    const auto from = std::pair<float, float>{xAt(0.4), yAt(67.0)};
    const auto to = std::pair<float, float>{xAt(1.4), yAt(64.4)};
    rig.root->dispatchMouseDown(mouseAt(from.first, from.second));
    rig.root->dispatchMouseDrag(mouseAt(to.first, to.second));
    rig.render(); // paints the rectangle without a crash
    rig.root->dispatchMouseUp(mouseAt(to.first, to.second));
    CHECK(! rig.history.canUndo()); // selecting is not an edit

    ui::KeyEvent del;
    del.keyCode = juce::KeyPress::deleteKey;
    rig.root->dispatchKey(del);
    REQUIRE(rig.song.tracks[0].arrangementClips[0].notes.size() == 1);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].notes[0].start, 2.75, 1e-9); // only the enclosed note went

    // Shift adds a second rectangle to the selection; a plain click on empty space deselects
    rig.history.undo(rig.song);
    rig.root->dispatchMouseDown(mouseAt(from.first, from.second));
    rig.root->dispatchMouseDrag(mouseAt(to.first, to.second));
    rig.root->dispatchMouseUp(mouseAt(to.first, to.second));
    ui::MouseEvent shiftDown = mouseAt(xAt(2.4), yAt(67.0));
    shiftDown.shift = true;
    rig.root->dispatchMouseDown(shiftDown);
    rig.root->dispatchMouseDrag(mouseAt(xAt(3.6), yAt(64.4)));
    rig.root->dispatchMouseUp(mouseAt(xAt(3.6), yAt(64.4)));
    rig.root->dispatchKey(del);
    CHECK(rig.song.tracks[0].arrangementClips[0].notes.empty());
    rig.history.undo(rig.song);
    rig.root->dispatchMouseDown(mouseAt(from.first, from.second));
    rig.root->dispatchMouseDrag(mouseAt(to.first, to.second));
    rig.root->dispatchMouseUp(mouseAt(to.first, to.second));
    rig.root->dispatchMouseDown(mouseAt(xAt(5.0), yAt(64.0))); // plain click on empty space
    rig.root->dispatchMouseUp(mouseAt(xAt(5.0), yAt(64.0)));
    rig.root->dispatchKey(del);
    REQUIRE(rig.song.tracks[0].arrangementClips.size() == 1); // Delete with no selected note does not delete the clip
    CHECK(rig.song.tracks[0].arrangementClips[0].notes.size() == 2);
}

TEST_CASE("loop selection: Cmd+L loops the selected notes, the selected clip, or the take range")
{
    RollRig rig;
    ui::KeyEvent selectAll; selectAll.cmd = true; selectAll.textChar = 'a';
    const auto p = rig.centre();
    rig.root->dispatchMouseDown(mouseAt(p.first, p.second));
    rig.root->dispatchMouseUp(mouseAt(p.first, p.second));
    rig.root->dispatchKey(selectAll);
    ui::KeyEvent loop; loop.cmd = true; loop.textChar = 'l';
    rig.root->dispatchKey(loop);
    const auto& clip = rig.song.tracks[0].arrangementClips[0];
    CHECK_NEAR(clip.loop.loopStart, 0.75, 1e-9);   // first note start
    CHECK_NEAR(clip.loop.loopEnd, 3.25, 1e-9);     // last note end
    CHECK(clip.loop.enabled);
    rig.history.undo(rig.song);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].loop.loopStart, 0.0, 1e-9);

    // with no notes selected the key falls through to the clip: the loop brace becomes the clip's range and switches on
    rig.song.loopOn = false;
    rig.ctx.selectedClipTrack = 0;
    rig.ctx.selectedClipUid = rig.song.tracks[0].arrangementClips[0].uid;
    rig.root->dispatchMouseDown(mouseAt(p.first, p.second)); // empty space: deselects notes
    rig.root->dispatchMouseUp(mouseAt(p.first, p.second));
    rig.root->dispatchKey(loop);
    CHECK(rig.song.loopOn);
    CHECK_NEAR(rig.song.loopStart, rig.song.tracks[0].arrangementClips[0].currentStart, 1e-9);
    CHECK_NEAR(rig.song.loopLength, rig.song.tracks[0].arrangementClips[0].currentEnd - rig.song.tracks[0].arrangementClips[0].currentStart, 1e-9);
}

TEST_CASE("devices: clicking a device selects it and Delete (or the context menu action) removes it; undo puts it back in place")
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
    ctx.detailTab = ui::DetailTab::Devices;
    ctx.selectedTrack = 0;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* detail = findWidget<ui::DetailView>(root);
    REQUIRE(detail != nullptr);
    REQUIRE(song.tracks[0].devices.size() >= 2);
    const int firstId = song.tracks[0].devices[0].id, secondId = song.tracks[0].devices[1].id;
    const size_t count = song.tracks[0].devices.size();

    // click the title of the second device box
    REQUIRE(detail->children().size() >= 2);
    const auto box = detail->children()[1]->boundsInRoot();
    root.dispatchMouseDown(mouseAt(box.x + 40, box.y + 8));
    root.dispatchMouseUp(mouseAt(box.x + 40, box.y + 8));
    CHECK(ctx.selectedDeviceId == secondId);

    ui::KeyEvent del; del.keyCode = juce::KeyPress::deleteKey;
    root.dispatchKey(del);
    REQUIRE(song.tracks[0].devices.size() == count - 1);
    CHECK(song.tracks[0].devices[0].id == firstId);
    for (const auto& d : song.tracks[0].devices) CHECK(d.id != secondId);
    CHECK(ctx.selectedDeviceId == -1);
    CHECK(history.undoLabel().rfind("Delete ", 0) == 0);

    history.undo(song);
    REQUIRE(song.tracks[0].devices.size() == count);
    CHECK(song.tracks[0].devices[1].id == secondId);
    history.redo(song);
    CHECK(song.tracks[0].devices.size() == count - 1);
    history.undo(song);

    // the menu path
    ctx.selectedDeviceId = firstId;
    CHECK(ctx.deleteSelectedDevice());
    CHECK(song.tracks[0].devices.size() == count - 1);
    CHECK(song.tracks[0].devices[0].id == secondId);
    CHECK(! ctx.deleteSelectedDevice()); // nothing selected any more
}

TEST_CASE("option+drag copies: a clip to another position or track, notes inside the editor; a plain Option click copies nothing")
{
    // --- Arrangement clip
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
    ctx.pixelsPerBeat = 20.0;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    const auto& m = ctx.theme.metrics;
    auto xAt = [&](double beat) { return origin.x + (float) (beat * ctx.pixelsPerBeat); };
    const float lane0 = origin.y + m.arrangementRulerHeight + 30.0f;
    const float lane1 = origin.y + m.arrangementRulerHeight + m.arrangementLaneHeight + 20.0f;
    const auto original = song.tracks[0].arrangementClips[0]; // 4..12
    const int uid = original.uid;

    ui::MouseEvent down = mouseAt(xAt(8.0), lane0), plain = down;
    down.alt = true;
    root.dispatchMouseDown(down);
    ui::MouseEvent drag = mouseAt(xAt(8.0), lane0);
    drag.alt = true;
    root.dispatchMouseUp(drag); // Option-click without a drag copies nothing
    CHECK(song.tracks[0].arrangementClips.size() == 1);

    // drag a copy 12 beats to the right on the same track: the original stays
    root.dispatchMouseDown(down);
    drag = mouseAt(xAt(20.0), lane0); drag.alt = true;
    root.dispatchMouseDrag(drag);
    root.dispatchMouseUp(drag);
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    CHECK(song.tracks[0].arrangementClips[0].uid == uid);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, original.currentStart, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, original.currentStart + 12.0, 1e-9);
    CHECK(song.tracks[0].arrangementClips[1].uid != uid);
    CHECK(ctx.selectedClipUid == song.tracks[0].arrangementClips[1].uid);
    CHECK(song.tracks[0].arrangementClips[1].notes.size() == original.notes.size());

    // a copy dropped on the other MIDI track
    root.layout();
    down = mouseAt(xAt(8.0), lane0); down.alt = true;
    root.dispatchMouseDown(down);
    drag = mouseAt(xAt(8.0), lane1); drag.alt = true;
    root.dispatchMouseDrag(drag);
    root.dispatchMouseUp(drag);
    CHECK(song.tracks[0].arrangementClips.size() == 2); // the original is still there
    REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, original.currentStart, 1e-9);
    history.undo(song);
    history.undo(song);
    CHECK(song.tracks[0].arrangementClips.size() == 1);
    CHECK(song.tracks[1].arrangementClips.empty());

    // A plain title-bar drag still moves.
    root.layout();
    plain.y = origin.y + m.arrangementRulerHeight + 6;
    root.dispatchMouseDown(plain);
    root.dispatchMouseDrag(mouseAt(xAt(10.0), lane0));
    root.dispatchMouseUp(mouseAt(xAt(10.0), lane0));
    REQUIRE(song.tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, original.currentStart + 2.0, 1e-9);
    history.undo(song);

    // --- notes in the editor
    RollRig rig;
    auto& notes = rig.song.tracks[0].arrangementClips[0].notes;
    REQUIRE(notes.size() == 2);
    const auto area = rig.roll->boundsInRoot();
    const double ppb = rig.roll->view.pixelsPerBeat;
    auto nx = [&](double beat) { return area.x + 40.0f + (float) ((beat - rig.roll->view.scrollBeats) * ppb); };
    auto ny = [&](int pitch) { return area.y + rig.roll->noteAreaBounds().y + (float) ((rig.roll->view.topPitch - pitch) * rig.roll->view.rowHeight) + 5.0f; };
    const double start0 = notes[0].start;
    ui::MouseEvent noteDown = mouseAt(nx(start0 + 0.1), ny(65)), noteUp = mouseAt(nx(start0 + 0.1), ny(65));
    noteDown.alt = noteUp.alt = true;
    rig.root->dispatchMouseDown(noteDown);
    rig.root->dispatchMouseUp(noteUp);
    CHECK(notes.size() == 2); // click only
    ui::MouseEvent moved = mouseAt(nx(start0 + 1.1), ny(67));
    moved.alt = true;
    rig.root->dispatchMouseDown(noteDown);
    rig.root->dispatchMouseDrag(moved);
    rig.root->dispatchMouseUp(moved);
    REQUIRE(notes.size() == 3);
    bool kept = false, copied = false;
    for (const auto& n : notes)
    {
        if (n.pitch == 65 && std::fabs(n.start - start0) < 1e-9) kept = true;
        if (n.pitch == 67 && std::fabs(n.start - (start0 + 1.0)) < 1e-9) copied = true;
    }
    CHECK(kept);
    CHECK(copied);
    CHECK(rig.history.undoLabel() == "Copy Notes");
}

TEST_CASE("clip fades: dragging the corner handles of an audio clip sets the fade-in / fade-out and one undo restores them")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 20.0;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    REQUIRE(song.tracks.size() >= 2);
    const auto clip = song.tracks[1].arrangementClips[0];
    REQUIRE(clip.kind == model::Clip::Kind::Audio);
    float laneY = -1;
    for (const auto& lane : arrangement->lanes())
        if (lane.controlIndex == 1 && lane.takeLaneId < 0) laneY = lane.y;
    REQUIRE(laneY >= 0);
    auto xAt = [&](double beat) { return origin.x + (float) (beat * ctx.pixelsPerBeat); };
    const float handleY = origin.y + laneY + 1 + 12 + 4; // the top of the waveform, below the title strip

    // pointer cursor shows a horizontal resize over the fade-in handle (clip left edge, fade 0)
    ctx.selectedClipTrack = 1;
    ctx.selectedClipUid = clip.uid;
    root.dispatchMouseMove(mouseAt(xAt(clip.currentStart) + 1, handleY));
    CHECK(root.cursor() == ui::Cursor::ResizeHorizontal);

    // drag the fade-in handle 1.5 beats to the right (the fixture clip is 4 beats long)
    root.dispatchMouseDown(mouseAt(xAt(clip.currentStart) + 1, handleY));
    root.dispatchMouseDrag(mouseAt(xAt(clip.currentStart + 1.5) + 1, handleY));
    root.dispatchMouseUp(mouseAt(xAt(clip.currentStart + 1.5) + 1, handleY));
    const auto& after = song.tracks[1].arrangementClips[0];
    CHECK_NEAR(after.currentStart, clip.currentStart, 1e-9); // the clip itself did not move or trim
    CHECK_NEAR(after.currentEnd, clip.currentEnd, 1e-9);
    CHECK(after.fadeIn > 1.0);
    CHECK(after.fadeIn < 2.0);
    CHECK(! after.fadeInDefault);
    CHECK(history.undoLabel() == "Change Fade");

    // and the fade-out handle (clip right edge) 1.5 beats to the left
    root.layout();
    const double fadeIn = after.fadeIn;
    root.dispatchMouseDown(mouseAt(xAt(clip.currentEnd) - 1, handleY));
    root.dispatchMouseDrag(mouseAt(xAt(clip.currentEnd - 1.5) - 1, handleY));
    root.dispatchMouseUp(mouseAt(xAt(clip.currentEnd - 1.5) - 1, handleY));
    const auto& both = song.tracks[1].arrangementClips[0];
    CHECK(both.fadeOut > 1.0);
    CHECK(both.fadeOut < 2.0);
    CHECK_NEAR(both.fadeIn, fadeIn, 1e-9);
    CHECK(! both.fadeOutDefault);

    // fades cannot overlap: dragging the fade-in past the fade-out stops where the fade-out begins
    root.layout();
    const float inHandle = xAt(clip.currentStart + fadeIn);
    root.dispatchMouseDown(mouseAt(inHandle, handleY));
    root.dispatchMouseDrag(mouseAt(xAt(clip.currentEnd + 5.0), handleY));
    root.dispatchMouseUp(mouseAt(xAt(clip.currentEnd + 5.0), handleY));
    const auto& clamped = song.tracks[1].arrangementClips[0];
    CHECK_NEAR(clamped.fadeIn + clamped.fadeOut, clip.currentEnd - clip.currentStart, 1e-6);

    history.undo(song);
    history.undo(song);
    history.undo(song);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].fadeIn, 0.0, 1e-12);
    CHECK(song.tracks[1].arrangementClips[0].fadeInDefault);
}

TEST_CASE("arrangement: double-clicking the ruler plays from that position as well")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    engine::Transport transport;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.transport = &transport;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.pixelsPerBeat = 40.0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    const auto origin = arrangement->boundsInRoot();
    const float y = origin.y + 22.0f;
    const float x = origin.x + (float) (12.0 * ctx.pixelsPerBeat);
    root.dispatchMouseDown(mouseAt(x, y, 1));
    root.dispatchMouseUp(mouseAt(x, y, 1));
    auto block = transport.beginBlock();
    CHECK(block.playing);
    CHECK_NEAR(block.position, 12.0, 1e-9);
    // the second click of a double click lands while the transport already plays: it must keep playing from there
    root.dispatchMouseDown(mouseAt(x + 80, y, 2));
    root.dispatchMouseUp(mouseAt(x + 80, y, 2));
    block = transport.beginBlock();
    CHECK(block.playing);
    CHECK_NEAR(block.position, 14.0, 1e-9);
}

TEST_CASE("groups from existing sets: a closed group can be opened with its ring, shows what is inside it when closed, and the state is saved")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_group_fold");
    dir.deleteRecursively();
    dir.createDirectory();
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), dir.getChildFile("Set.als"), error));
    auto loaded = io::loadAls(dir.getChildFile("Set.als"));
    REQUIRE((bool) loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    int end = 0;
    history.perform(song, model::groupTracks(song, 0, 2, io::createGroupTrack(song, "Drums", 4, end), end));
    model::Clip clip;
    song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    REQUIRE(model::makeEmptyMidiClip(song, 4.0, 4.0, clip));
    history.perform(song, model::insertClip(1, clip, "Insert MIDI Clip"));
    history.perform(song, model::setTrackFolded(song, 0, true)); // a group saved closed by Live
    REQUIRE(song.tracks[0].folded);

    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.detailVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.applyEdit = [&](model::EditCommand c) { history.perform(song, std::move(c)); };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* arrangement = findWidget<ui::ArrangementView>(root);
    REQUIRE(arrangement != nullptr);
    auto laneCount = [&] { int n = 0; for (const auto& l : arrangement->lanes()) if (l.takeLaneId < 0 && l.controlIndex < ctx.numTracks()) ++n; return n; };
    const int closedLanes = laneCount();
    CHECK(closedLanes == 3); // the group and the two plain tracks; its two children are hidden

    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1.0f, png.getFile().getFullPathName().toStdString())); // paints the summary of the closed group

    // click the group's ring: the children appear
    const auto origin = arrangement->boundsInRoot();
    float laneY = 0;
    for (const auto& l : arrangement->lanes()) if (l.controlIndex == 0 && l.takeLaneId < 0) laneY = l.y;
    const float ringX = origin.x + arrangement->width() - ctx.arrangementHeaderWidth() + 11, ringY = origin.y + laneY + 10;
    root.dispatchMouseDown(mouseAt(ringX, ringY));
    root.dispatchMouseUp(mouseAt(ringX, ringY));
    root.layout();
    CHECK(! song.tracks[0].folded);
    CHECK(laneCount() == 5);
    CHECK(history.undoLabel() == "Unfold Group");

    // saved as <TrackUnfolded>, both ways
    juce::TemporaryFile open(".als");
    REQUIRE(io::saveAls(song, open.getFile()).ok);
    auto reopened = io::loadAls(open.getFile());
    REQUIRE((bool) reopened);
    CHECK(! reopened.song->tracks[0].folded);
    history.undo(song);
    juce::TemporaryFile closed(".als");
    REQUIRE(io::saveAls(song, closed.getFile()).ok);
    auto again = io::loadAls(closed.getFile());
    REQUIRE((bool) again);
    CHECK(again.song->tracks[0].folded);

    // Session uses the same persisted fold state and undo command as Arrangement.
    ctx.mainView = ui::MainView::Session;
    root.layout();
    auto* session = findWidget<ui::SessionView>(root);
    REQUIRE(session);
    CHECK(session->stopRowBounds(1).isEmpty());
    const auto sessionOrigin = session->boundsInRoot();
    const auto icon = session->groupToggleBounds(0);
    root.dispatchMouseDown(mouseAt(sessionOrigin.x + icon.centreX(), sessionOrigin.y + icon.centreY()));
    root.dispatchMouseUp(mouseAt(sessionOrigin.x + icon.centreX(), sessionOrigin.y + icon.centreY()));
    CHECK(! song.tracks[0].folded);
    CHECK(! session->stopRowBounds(1).isEmpty());
    REQUIRE(io::saveAls(song, open.getFile()).ok);
    reopened = io::loadAls(open.getFile()); REQUIRE(reopened);
    CHECK(! reopened.song->tracks[0].folded);
    REQUIRE(history.undo(song)); root.layout();
    CHECK(session->stopRowBounds(1).isEmpty());
    REQUIRE(io::saveAls(song, closed.getFile()).ok);
    again = io::loadAls(closed.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[0].folded);
}

#include "ui/views/DetailView.h"
#include "ui/views/StatusBar.h"

TEST_CASE("device chain: a long chain scrolls with sideways swipes (also over knobs), a scrollbar shows it, and a new device scrolls into view")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    REQUIRE(! song.tracks[0].devices.empty());
    const auto proto = song.tracks[0].devices[0];
    while (song.tracks[0].devices.size() < 9)
    {
        auto copy = proto;
        copy.id = 100 + (int) song.tracks[0].devices.size();
        song.tracks[0].devices.push_back(copy);
    }
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.detailTab = ui::DetailTab::Devices;
    ctx.selectedTrack = 0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 700});
    root.layout();
    auto* detail = findWidget<ui::DetailView>(root);
    REQUIRE(detail != nullptr);
    const auto bounds = detail->boundsInRoot();
    REQUIRE(detail->children().size() >= 9);
    CHECK(detail->children().back()->boundsInRoot().right() > bounds.right()); // the last device is off to the right

    // a sideways swipe over the first device scrolls the chain
    const auto first = detail->children().front()->boundsInRoot();
    ui::MouseEvent swipe = mouseAt(first.centreX(), first.centreY());
    swipe.wheelDeltaX = -0.5f;
    root.dispatchMouseWheel(swipe);
    CHECK(detail->children().front()->boundsInRoot().x < first.x - 20.0f);

    // the scrollbar thumb drags the chain all the way to the end
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1200, 700, 1.0f, png.getFile().getFullPathName().toStdString()));
    const float barY = bounds.y + detail->height() - 6.0f;
    root.dispatchMouseDown(mouseAt(bounds.x + 450.0f, barY)); // somewhere on the thumb (it is wide: most of the chain is visible)
    root.dispatchMouseDrag(mouseAt(bounds.x + 450.0f + 1500.0f, barY));
    root.dispatchMouseUp(mouseAt(bounds.x + 450.0f + 1500.0f, barY));
    CHECK(detail->children().back()->boundsInRoot().right() <= bounds.right() + 1.0f);

    // adding another device (a new song object, as the app does while rebuilding) scrolls it into view instead of hiding it
    model::Song more = song;
    auto extra = proto;
    extra.id = 999;
    more.tracks[0].devices.push_back(extra);
    ctx.song = &more;
    root.layout();
    REQUIRE(detail->children().size() >= 10);
    CHECK(detail->children().back()->boundsInRoot().right() <= bounds.right() + 1.0f);
    CHECK(detail->children().back()->boundsInRoot().x >= bounds.x);
}

TEST_CASE("monitor tab: opens from the status navigation, runs the analysis only while shown, switches views and resets loudness")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    engine::MasterMonitor monitor;
    monitor.prepare(48000.0);
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.monitor = &monitor;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ctx.selectedTrack = 0;
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1300, 760});
    root.layout();
    auto* detail = findWidget<ui::DetailView>(root);
    REQUIRE(detail != nullptr);
    root.tickTree();
    CHECK(! monitor.enabled()); // nobody is watching

    auto* status = findWidget<ui::StatusBar>(root);
    REQUIRE(status != nullptr);
    ctx.showChoices = [](const auto& labels, const auto&, auto pick) {
        REQUIRE(labels.size() == 3); pick(2);
    };
    const float navigationX = 1300 - ctx.theme.metrics.outerMargin - 43 - 300 + 45;
    root.dispatchMouseDown(mouseAt(navigationX, status->boundsInRoot().centreY()));
    root.dispatchMouseUp(mouseAt(navigationX, status->boundsInRoot().centreY()));
    CHECK(ctx.detailTab == ui::DetailTab::Monitor);
    root.layout();
    root.tickTree();
    CHECK(monitor.enabled());
    auto* panel = findWidget<ui::MonitorPanel>(root);
    REQUIRE(panel != nullptr);

    // 4 s of a stereo -20 dBFS sine reaches the readouts
    std::vector<float> block(512);
    for (int i = 0; i < 400; ++i)
    {
        for (int n = 0; n < 512; ++n)
            block[(size_t) n] = (float) (0.1 * std::sin(6.2831853 * 1000.0 * (double) (i * 512 + n) / 48000.0));
        monitor.push(block.data(), block.data(), 512);
        if (i % 16 == 15)
            monitor.drainNow();
    }
    monitor.drainNow();
    CHECK_NEAR(monitor.snapshot().integratedLufs, -20.0f, 0.3f);
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(root, 1300, 760, 1.0f, png.getFile().getFullPathName().toStdString()));

    // view buttons: Curve, then Scope (shows the time window and trigger buttons)
    const auto p = panel->boundsInRoot();
    root.dispatchMouseDown(mouseAt(p.x + 8 + 206 + 10 + 62.0f + 20.0f, p.y + 15.0f));
    CHECK(ctx.monitorView == 1);
    root.dispatchMouseDown(mouseAt(p.x + 8 + 206 + 10 + 124.0f + 20.0f, p.y + 15.0f));
    CHECK(ctx.monitorView == 2);
    const int window = ctx.monitorScopeWindow;
    root.dispatchMouseDown(mouseAt(p.x + 8 + 206 + 10 + 186.0f + 6.0f + 20.0f, p.y + 15.0f));
    CHECK(ctx.monitorScopeWindow == (window + 1) % ui::MonitorPanel::kScopeWindows);
    root.dispatchMouseDown(mouseAt(p.x + 8 + 206 + 10 + 186.0f + 6.0f + 62.0f + 4.0f + 20.0f, p.y + 15.0f));
    CHECK(! ctx.monitorScopeTrigger);
    CHECK(gfx::renderToPng(root, 1300, 760, 1.0f, png.getFile().getFullPathName().toStdString()));

    // Reset restarts the integrated loudness
    root.dispatchMouseDown(mouseAt(p.x + 8 + 30.0f, p.y + p.h - 6 - 9.0f));
    for (int wait = 0; wait < 100 && monitor.snapshot().integratedLufs > -150.0f; ++wait)
    {
        monitor.drainNow();
        juce::Thread::sleep(10);
    }
    CHECK(monitor.snapshot().integratedLufs <= -150.0f);

    // leaving the tab stops the analysis again
    ctx.detailTab = ui::DetailTab::Devices;
    root.layout();
    root.tickTree();
    CHECK(! monitor.enabled());
}

TEST_CASE("export dialog: opens with Shift+Cmd+R, edits range / options through real clicks, starts, shows progress and closes")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    ui::UiContext ctx;
    ctx.song = &song;
    ctx.ioVisible = false;
    ctx.browserVisible = false;
    ctx.mainView = ui::MainView::Arrangement;
    ui::ExportSettings started;
    int starts = 0, cancels = 0;
    ctx.startExport = [&](const ui::ExportSettings& s) { started = s; ++starts; };
    ctx.cancelExport = [&] { ++cancels; };
    std::vector<std::string> shownLabels;
    ctx.showChoices = [&](const std::vector<std::string>& labels, const std::vector<bool>&, std::function<void(int)> pick) {
        shownLabels = labels;
        pick(2); // always the third entry
    };
    ui::MainLayout root(ctx);
    root.setBounds({0, 0, 1200, 760});
    root.layout();
    juce::TemporaryFile png(".png");
    auto draw = [&] { REQUIRE(gfx::renderToPng(root, 1200, 760, 1.0f, png.getFile().getFullPathName().toStdString())); };
    auto click = [&](float x, float y) { root.dispatchMouseDown(mouseAt(x, y)); root.dispatchMouseUp(mouseAt(x, y)); };
    auto key = [&](uint32_t ch, int code = 0, bool cmd = false, bool shift = false) {
        ui::KeyEvent k; k.textChar = ch; k.keyCode = code; k.cmd = cmd; k.shift = shift; root.dispatchKey(k);
    };

    key('R', 'R', true, true);
    CHECK(ctx.exportOpen);
    root.layout();
    draw();
    const float panelX = (1200 - 560) / 2.0f, panelY = (760 - 370) / 2.0f, rowY = panelY + 34.0f, field = panelX + 14 + 150 + 10;

    // Render Range -> third entry (Custom); the dialog offers the three modes
    click(field, rowY + 28 + 14);
    CHECK(shownLabels.size() == 3);
    CHECK(ctx.exportSettings.range == 2);
    draw();
    // Start bar 3, End bar 9 typed into the number boxes
    click(field, rowY + 56 + 14);
    key('3');
    key(0, juce::KeyPress::returnKey);
    click(field + 80, rowY + 56 + 14);
    key('9');
    key(0, juce::KeyPress::returnKey);
    CHECK_NEAR(ctx.exportSettings.startBar, 3.0, 1e-9);
    CHECK_NEAR(ctx.exportSettings.endBar, 9.0, 1e-9);
    draw();
    click(field, rowY + 4 * 28 + 14); // Normalize
    CHECK(ctx.exportSettings.normalize);
    draw();

    // typing letters or shortcuts while the dialog is open must not leak to the app
    key('g', 0, true);
    CHECK(! ctx.exportSettings.mono);

    click(panelX + 560 - 114 + 20, panelY + 370 - 36 + 12); // Export...
    CHECK(starts == 1);
    CHECK(started.range == 2);
    CHECK_NEAR(started.startBar, 3.0, 1e-9);
    CHECK_NEAR(started.endBar, 9.0, 1e-9);
    CHECK(started.normalize);

    ctx.exportState.phase = ui::ExportState::Phase::Running;
    ctx.exportState.progress = 0.3f;
    root.layout();
    draw();
    click(panelX + 560 - 104 + 20, (760 - 170) / 2.0f + 170 - 36 + 12); // Cancel while rendering
    CHECK(cancels == 1);
    CHECK(ctx.exportOpen); // stays open until the render has stopped
    key(0, juce::KeyPress::escapeKey);
    CHECK(cancels == 2);

    ctx.exportState.phase = ui::ExportState::Phase::Done;
    ctx.exportState.files = {"/tmp/Song.wav"};
    ctx.exportState.integratedLufs = -14.0f;
    ctx.exportState.truePeakDb = -1.0f;
    root.layout();
    draw();
    click(panelX + 560 - 104 + 20, (760 - 300) / 2.0f + 300 - 36 + 12); // Close
    CHECK(! ctx.exportOpen);
    CHECK(ctx.exportState.phase == ui::ExportState::Phase::Idle);

    key('R', 'R', true, true);
    CHECK(ctx.exportOpen);
    key(0, juce::KeyPress::escapeKey); // Escape closes an idle dialog
    CHECK(! ctx.exportOpen);
}

namespace
{
struct SessionGroupRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    ui::SessionView* session = nullptr;
    SessionGroupRig()
    {
        song.scenes.resize(8);
        auto track = [&](int id, model::TrackKind kind, int parent, const char* name) {
            model::Track t; t.id = id; t.kind = kind; t.groupId = parent; t.name = name; t.colorIndex = id;
            song.tracks.push_back(std::move(t));
        };
        track(10, model::TrackKind::Group, -1, "Group");
        track(11, model::TrackKind::Audio, 10, "First");
        track(12, model::TrackKind::Group, 10, "Nested");
        track(13, model::TrackKind::Midi, 12, "Inner");
        track(14, model::TrackKind::Audio, 10, "Second");
        track(15, model::TrackKind::Audio, -1, "Outside");
        model::Track ret; ret.id = 20; ret.kind = model::TrackKind::Return; ret.name = "A Return"; song.returns.push_back(ret);
        context.song = &song; context.mainView = ui::MainView::Session;
        context.browserVisible = context.detailVisible = context.ioVisible = context.mixerVisible = false;
        context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1200, 700});
        session = findWidget<ui::SessionView>(*root); REQUIRE(session);
    }
    void click(float x, float y)
    {
        const auto origin = session->boundsInRoot();
        root->dispatchMouseDown(mouseAt(origin.x + x, origin.y + y));
        root->dispatchMouseUp(mouseAt(origin.x + x, origin.y + y));
    }
    void fold(int track)
    {
        const auto icon = session->groupToggleBounds(track); REQUIRE(! icon.isEmpty());
        click(icon.centreX(), icon.centreY());
    }
};
}

TEST_CASE("Session groups: fold icon hides nested columns and mixer without changing control indices or return anchors")
{
    SessionGroupRig rig;
    const auto returnX = rig.session->stopRowBounds(6).x;
    const auto outsideBefore = rig.session->stopRowBounds(5).x;
    const auto icon = rig.session->groupToggleBounds(0);
    CHECK(icon.x > 70); CHECK(icon.y > 0);
    rig.fold(0);
    CHECK(rig.song.tracks[0].folded); CHECK(rig.history.undoLabel() == "Fold Group");
    CHECK(rig.session->groupToggleBounds(2).isEmpty());
    for (int i = 1; i <= 4; ++i)
    {
        CHECK(rig.session->stopRowBounds(i).isEmpty());
        CHECK(! rig.session->children()[(size_t) i]->visible);
    }
    CHECK_NEAR(rig.session->stopRowBounds(5).x, rig.context.theme.metrics.sessionTrackWidth, 0.01);
    CHECK(rig.session->stopRowBounds(5).x < outsideBefore);
    CHECK_NEAR(rig.session->stopRowBounds(6).x, returnX, 0.01);
    rig.click(rig.session->stopRowBounds(5).x + 25, 12);
    CHECK(rig.context.selectedTrack == 5); // hidden members do not take clicks intended for Outside
    rig.fold(0); CHECK(! rig.song.tracks[0].folded);
    rig.fold(2); CHECK(rig.song.tracks[2].folded); CHECK(rig.session->stopRowBounds(3).isEmpty());
    rig.fold(0); rig.fold(0); // reopening the outer group retains the nested group's fold state
    CHECK(rig.session->stopRowBounds(3).isEmpty());
    REQUIRE(rig.history.undo(rig.song)); rig.root->layout(); CHECK(rig.song.tracks[0].folded);
    REQUIRE(rig.history.redo(rig.song)); rig.root->layout(); CHECK(! rig.song.tracks[0].folded);
    CHECK(rig.session->stopRowBounds(3).isEmpty());
}

TEST_CASE("Session groups: slot and stop-row squares stop all descendants including folded members and no outsiders")
{
    SessionGroupRig rig; std::vector<int> stops;
    rig.context.stopTrack = [&](int track) { stops.push_back(track); };
    const float row = rig.context.theme.metrics.sessionHeaderHeight + 1 + rig.context.theme.metrics.sessionSlotHeight * 0.5f;
    rig.click(8, row); CHECK(stops == std::vector<int>({1, 3, 4}));
    CHECK(rig.context.selectedSessionScene == 0); CHECK(rig.context.selectedClipUid == -1);
    stops.clear(); rig.click(40, row); CHECK(stops.empty()); // group body selects without stopping
    CHECK(rig.context.detailTab == ui::DetailTab::Devices);
    rig.fold(0); rig.click(8, row); CHECK(stops == std::vector<int>({1, 3, 4}));
    stops.clear();
    const auto stop = rig.session->stopRowBounds(0); rig.click(stop.x + 8, stop.centreY());
    CHECK(stops == std::vector<int>({1, 3, 4}));
    rig.fold(0); stops.clear();
    const auto nested = rig.session->stopRowBounds(2); rig.click(nested.x + 8, row);
    CHECK(stops == std::vector<int>({3}));
}

TEST_CASE("Session groups: rendered empty-group square and slot rules use full theme colors and scrolling preserves header")
{
    SessionGroupRig rig;
    juce::TemporaryFile file(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, file.getFile().getFullPathName().toStdString()));
    auto png = juce::ImageFileFormat::loadFrom(file.getFile()); REQUIRE(png.isValid());
    const auto origin = rig.session->boundsInRoot(); const auto& m = rig.context.theme.metrics;
    const auto pixel = [&](int x, int y) { return png.getPixelAt((int) origin.x + x, (int) origin.y + y).getARGB(); };
    CHECK(pixel(8, (int) (m.sessionHeaderHeight + 1 + m.sessionSlotHeight * 0.5f)) == rig.context.theme.color("ClipSlotButton"));
    CHECK(pixel(40, (int) (m.sessionHeaderHeight + m.sessionSlotHeight)) == rig.context.theme.color("SurfaceArea"));
    CHECK(pixel((int) m.sessionTrackWidth + 1, 2) == ui::Theme::paletteColor(rig.song.tracks[0].colorIndex));
    const auto icon = rig.session->groupToggleBounds(0);
    CHECK(pixel((int) icon.x + 5, (int) icon.y + 4) != ui::Theme::paletteColor(rig.song.tracks[0].colorIndex)); // three-line glyph survives 1x rasterization
    // Lots of scenes allow vertical scrolling; slot paint must remain below the title/icon.
    rig.song.scenes.resize(100);
    ui::MouseEvent scroll; scroll.x = origin.x + 40; scroll.y = origin.y + 100; scroll.wheelDeltaY = -0.13f;
    rig.root->dispatchMouseWheel(scroll);
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, file.getFile().getFullPathName().toStdString()));
    png = juce::ImageFileFormat::loadFrom(file.getFile());
    CHECK(pixel(40, 2) == ui::Theme::paletteColor(rig.song.tracks[0].colorIndex));
    rig.fold(0); CHECK(rig.song.tracks[0].folded);
}

TEST_CASE("Session Main: left divider spans header slots and mixer with or without returns and sends")
{
    SessionGroupRig rig;
    rig.song.tracks.resize(1); // leave a gap before the pinned columns
    rig.context.mixerVisible = true;
    juce::TemporaryFile file(".png");
    for (const bool returns : {true, false})
    {
        if (! returns) rig.song.returns.clear();
        for (const bool sends : {true, false})
        {
            rig.context.sendsVisible = sends;
            REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, file.getFile().getFullPathName().toStdString()));
            const auto png = juce::ImageFileFormat::loadFrom(file.getFile()); REQUIRE(png.isValid());
            const auto origin = rig.session->boundsInRoot();
            const float mainX = rig.session->stopRowBounds(rig.context.numTracks() + rig.context.numReturns()).x;
            for (const int y : {2, 70, 300, (int) rig.session->height() - 10})
                CHECK(png.getPixelAt((int) (origin.x + mainX) - 1, (int) origin.y + y).getARGB()
                      == rig.context.theme.color("SurfaceArea"));
        }
    }
}

TEST_CASE("Session navigation: arrows select slots, Enter launches, Cmd Enter stops and folded members are skipped")
{
    SessionGroupRig rig;
    auto& ctx = rig.context;
    model::Clip clip; clip.uid = model::allocateClipUid(); clip.kind = model::Clip::Kind::Audio;
    rig.song.tracks[1].slots.resize(8); rig.song.tracks[1].slots[1].clip = clip;
    ctx.selectedTrack = 1; ctx.selectedSessionScene = 0; ctx.sessionTrackSelection = false;
    rig.root->setKeyboardFocus(rig.session);
    int launched = -1, scene = -1, stopped = -1;
    ctx.launchClip = [&](int t, int s) { launched = t; scene = s; };
    ctx.stopTrack = [&](int t) { stopped = t; };
    auto key = [&](int code, bool cmd = false) { return rig.root->dispatchKey({.keyCode = code, .cmd = cmd}); };
    CHECK(key(juce::KeyPress::downKey));
    CHECK(ctx.selectedTrack == 1); CHECK(ctx.selectedSessionScene == 1); CHECK(ctx.selectedClipUid == clip.uid);
    key(juce::KeyPress::returnKey); CHECK(launched == 1); CHECK(scene == 1);
    key(juce::KeyPress::returnKey, true); CHECK(stopped == 1);
    rig.song.tracks[2].folded = true; rig.root->layout();
    key(juce::KeyPress::rightKey); CHECK(ctx.selectedTrack == 2);
    key(juce::KeyPress::rightKey); CHECK(ctx.selectedTrack == 4); // nested child is hidden
    key(juce::KeyPress::homeKey); CHECK(ctx.selectedTrack == 0);
    key(juce::KeyPress::endKey); CHECK(ctx.selectedTrack == ctx.numControlTracks() - 1);
    key(juce::KeyPress::escapeKey); CHECK(ctx.sessionTrackSelection); CHECK(ctx.selectedClipUid == -1);
}

TEST_CASE("Session group selection: selected parent frames its members in cyan without replacing their colors")
{
    SessionGroupRig rig;
    rig.context.selectedTrack = 0; rig.context.sessionTrackSelection = true;
    juce::TemporaryFile image(".png");
    auto render = [&] {
        REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 2, image.getFile().getFullPathName().toStdString()));
        auto png = juce::ImageFileFormat::loadFrom(image.getFile()); REQUIRE(png.isValid()); return png;
    };
    auto png = render();
    const auto origin = rig.session->boundsInRoot(); const auto& m = rig.context.theme.metrics;
    auto pixel = [&](float x, float y) { return png.getPixelAt((int) ((origin.x + x) * 2), (int) ((origin.y + y) * 2)).getARGB(); };
    CHECK(pixel(40, 2) == rig.context.theme.color("SelectionBackground"));
    CHECK(pixel(m.sessionTrackWidth + 1, 2) == rig.context.theme.color("SelectionBackground"));
    CHECK(pixel(m.sessionTrackWidth + 40, 2) == rig.context.theme.color("SelectionBackground"));
    CHECK(pixel(m.sessionTrackWidth + 40, 7) == ui::Theme::paletteColor(rig.song.tracks[1].colorIndex));
    CHECK(pixel(m.sessionTrackWidth + 2, 5) == rig.context.theme.color("SurfaceArea"));
    CHECK(pixel(m.sessionTrackWidth, 10) == rig.context.theme.color("SurfaceArea"));
    rig.context.selectTrack(5, false, false); png = render();
    CHECK(pixel(m.sessionTrackWidth + 40, 2) == ui::Theme::paletteColor(rig.song.tracks[0].colorIndex));
    rig.context.selectedTrack = 0; rig.context.sessionTrackSelection = false; png = render();
    CHECK(pixel(m.sessionTrackWidth + 40, 2) == ui::Theme::paletteColor(rig.song.tracks[0].colorIndex));
}

TEST_CASE("Arrangement group selection: light group lane and cyan header continue into member rails")
{
    SessionGroupRig rig;
    rig.context.mainView = ui::MainView::Arrangement;
    rig.context.selectedTrack = 0; rig.context.sessionTrackSelection = true;
    rig.root->layout();
    auto* arrangement = findWidget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement);
    juce::TemporaryFile image(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 2, image.getFile().getFullPathName().toStdString()));
    auto png = juce::ImageFileFormat::loadFrom(image.getFile()); REQUIRE(png.isValid());
    const auto origin = arrangement->boundsInRoot();
    const float header = arrangement->width() - rig.context.arrangementHeaderWidth();
    const auto& lanes = arrangement->lanes(); REQUIRE(lanes.size() > 1);
    auto pixel = [&](float x, float y) { return png.getPixelAt((int) ((origin.x + x) * 2), (int) ((origin.y + y) * 2)).getARGB(); };
    CHECK(pixel(header + 30, lanes[0].y + 30) == rig.context.theme.color("SelectionBackground"));
    CHECK(pixel(header + 4, lanes[1].y + 30) == rig.context.theme.color("SelectionBackground"));
    CHECK(pixel(header + 30, lanes[1].y + 30) == ui::Theme::paletteColor(rig.song.tracks[1].colorIndex));
    CHECK(pixel(10, lanes[0].y + 30) == rig.context.theme.color("SurfaceHighlight"));
}

TEST_CASE("region UI: plain clip-body range duplicates immediately after the selected beats and repeats")
{
    RollRig rig;
    rig.ctx.detailVisible = false; rig.ctx.pixelsPerBeat = 40; rig.root->layout();
    auto* arrangement = findWidget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement);
    const auto b = arrangement->boundsInRoot();
    const float y = b.y + rig.ctx.theme.metrics.arrangementRulerHeight + 30;
    rig.root->dispatchMouseDown(mouseAt(b.x + 6 * 40, y));
    rig.root->dispatchMouseDrag(mouseAt(b.x + 8 * 40, y));
    rig.root->dispatchMouseUp(mouseAt(b.x + 8 * 40, y));
    CHECK(rig.ctx.arrangementSelection.active);
    REQUIRE(rig.root->dispatchKey({.textChar = 'd', .cmd = true}));
    CHECK(rig.history.undoLabel() == "Duplicate Region");
    REQUIRE(rig.song.tracks[0].arrangementClips.size() == 3);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[1].currentStart, 8, 1e-9);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[1].currentEnd, 10, 1e-9);
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 8, 1e-9);
    CHECK_NEAR(rig.ctx.arrangementSelection.end, 10, 1e-9);
    REQUIRE(rig.root->dispatchKey({.textChar = 'd', .cmd = true}));
    CHECK_NEAR(rig.ctx.arrangementSelection.end, 12, 1e-9);
    REQUIRE(rig.history.undo(rig.song)); REQUIRE(rig.history.undo(rig.song));
    CHECK(rig.song.tracks[0].arrangementClips.size() == 1);
    CHECK(rig.song.tracks[1].arrangementClips.empty());
}

TEST_CASE("region piano: ruler time selection preserves leading and trailing silence across repeated duplication")
{
    RollRig rig;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.notes = {{0.5, 0.25, 60, 90}, {1.5, 0.25, 64, 90}};
    rig.roll->view.pixelsPerBeat = 80; rig.roll->view.scrollBeats = 0;
    const auto b = rig.roll->boundsInRoot();
    rig.root->dispatchMouseDown(mouseAt(b.x + 40, b.y + rig.roll->rulerBounds().centreY()));
    rig.root->dispatchMouseDrag(mouseAt(b.x + 40 + 4 * 80, b.y + rig.roll->rulerBounds().centreY()));
    rig.root->dispatchMouseUp(mouseAt(b.x + 40 + 4 * 80, b.y + rig.roll->rulerBounds().centreY()));
    REQUIRE(rig.root->dispatchKey({.textChar = 'd', .cmd = true}));
    REQUIRE(clip.notes.size() == 4);
    CHECK_NEAR(clip.notes[2].start, 4.5, 1e-9); CHECK_NEAR(clip.notes[3].start, 5.5, 1e-9);
    REQUIRE(rig.root->dispatchKey({.textChar = 'd', .cmd = true}));
    REQUIRE(clip.notes.size() == 6);
    CHECK_NEAR(clip.notes[4].start, 8.5, 1e-9); CHECK_NEAR(clip.notes[5].start, 9.5, 1e-9);
    REQUIRE(rig.history.undo(rig.song)); CHECK(clip.notes.size() == 4);
    REQUIRE(rig.history.undo(rig.song)); CHECK(clip.notes.size() == 2);
}

TEST_CASE("velocity line: fast forward and reverse drags interpolate every crossed stem including chords with one undo")
{
    RollRig rig;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.notes = {{0.5, 0.25, 60, 90}, {1, 0.25, 62, 90}, {2, 0.25, 64, 90},
                  {2, 0.25, 67, 90}, {3, 0.25, 65, 90}, {5, 0.25, 69, 90}};
    rig.roll->view.pixelsPerBeat = 80; rig.roll->view.scrollBeats = 0;
    const auto b = rig.roll->boundsInRoot();
    const float vh = rig.ctx.theme.metrics.velocityLaneHeight;
    auto point = [&](double beat, float velocity) {
        return mouseAt(b.x + 40 + (float) beat * 80 + 1.5f, b.bottom() - vh + 3 + (1 - velocity / 127) * (vh - 6));
    };
    rig.root->dispatchMouseDown(point(0, 20)); // empty space starts line mode
    rig.root->dispatchMouseDrag(point(4, 100)); // one event crosses five notes
    rig.root->dispatchMouseUp(point(4, 100));
    REQUIRE(rig.history.canUndo()); CHECK(rig.history.undoLabel() == "Change Velocity");
    CHECK_NEAR(clip.notes[0].velocity, 30, 0.1); CHECK_NEAR(clip.notes[1].velocity, 40, 0.1);
    CHECK_NEAR(clip.notes[2].velocity, 60, 0.1); CHECK_NEAR(clip.notes[3].velocity, 60, 0.1);
    CHECK_NEAR(clip.notes[4].velocity, 80, 0.1); CHECK_NEAR(clip.notes[5].velocity, 90, 0.1);
    REQUIRE(rig.history.undo(rig.song)); CHECK(! rig.history.canUndo());
    for (const auto& note : clip.notes) CHECK_NEAR(note.velocity, 90, 0.1);
    rig.root->dispatchMouseDown(point(4, 20)); rig.root->dispatchMouseDrag(point(0, 100)); rig.root->dispatchMouseUp(point(0, 100));
    CHECK_NEAR(clip.notes[0].velocity, 90, 0.1); CHECK_NEAR(clip.notes[4].velocity, 40, 0.1);
    CHECK_NEAR(clip.notes[5].velocity, 90, 0.1);
}

TEST_CASE("Arrangement display controls: H W restore previous geometry and waveform zoom edits no audio gain")
{
    RollRig rig;
    rig.ctx.detailVisible = false; rig.root->layout();
    auto* arrangement = findWidget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement);
    rig.root->setKeyboardFocus(arrangement);
    rig.ctx.pixelsPerBeat = 60; rig.ctx.scrollBeats = 2; rig.ctx.laneHeights = {100, 150};
    const auto before = rig.ctx.laneHeights;
    rig.root->layout();
    REQUIRE(rig.root->dispatchKey({.textChar = 'w'}));
    CHECK_NEAR(rig.ctx.scrollBeats, 0, 1e-9); CHECK(std::abs(rig.ctx.pixelsPerBeat - 60) > 1e-6);
    REQUIRE(rig.root->dispatchKey({.textChar = 'w'}));
    CHECK_NEAR(rig.ctx.pixelsPerBeat, 60, 1e-9); CHECK_NEAR(rig.ctx.scrollBeats, 2, 1e-9);
    REQUIRE(rig.root->dispatchKey({.textChar = 'h'})); CHECK(rig.ctx.laneHeights != before);
    REQUIRE(rig.root->dispatchKey({.textChar = 'h'})); CHECK(rig.ctx.laneHeights == before);
    const auto b = arrangement->boundsInRoot();
    const auto zoom = mouseAt(b.right() - 90, b.bottom() - 8);
    auto scroll = zoom; scroll.wheelDeltaY = 0.5f;
    rig.root->dispatchMouseWheel(scroll); CHECK(rig.ctx.waveformZoom > 1);
    CHECK(! rig.history.canUndo());
    auto reset = zoom; reset.cmd = true; rig.root->dispatchMouseDown(reset); rig.root->dispatchMouseUp(reset);
    CHECK_NEAR(rig.ctx.waveformZoom, 1, 1e-9);
    auto type = zoom; type.clicks = 2; rig.root->dispatchMouseDown(type); rig.root->dispatchMouseUp(type);
    REQUIRE(rig.root->dispatchKey({.textChar = '2'}));
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
    CHECK_NEAR(rig.ctx.waveformZoom, 2, 1e-9);
    std::function<void(int)> pick;
    rig.ctx.showChoices = [&](const auto&, const auto&, auto choose) { pick = std::move(choose); };
    auto menu = zoom; menu.right = true; rig.root->dispatchMouseDown(menu); rig.root->dispatchMouseUp(menu);
    REQUIRE(pick); pick(1); // display/enter dB
    rig.root->dispatchMouseDown(type); rig.root->dispatchMouseUp(type);
    REQUIRE(rig.root->dispatchKey({.textChar = '0'}));
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
    CHECK_NEAR(rig.ctx.waveformZoom, 1, 1e-9); CHECK(! rig.history.canUndo());
}

TEST_CASE("velocity line: starting on a selected stem leaves uncrossed selected notes unchanged")
{
    RollRig rig;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.notes = {{0.5, 0.25, 60, 90}, {2, 0.25, 64, 90}, {5, 0.25, 69, 90}};
    rig.roll->view.pixelsPerBeat = 80; rig.roll->view.scrollBeats = 0;
    const auto b = rig.roll->boundsInRoot();
    rig.root->setKeyboardFocus(rig.roll); REQUIRE(rig.root->dispatchKey({.textChar = 'a', .cmd = true}));
    const float vh = rig.ctx.theme.metrics.velocityLaneHeight;
    auto point = [&](double beat, float velocity) {
        return mouseAt(b.x + 40 + (float) beat * 80 + 1.5f, b.bottom() - vh + 3 + (1 - velocity / 127) * (vh - 6));
    };
    rig.root->dispatchMouseDown(point(0.5, 20));
    rig.root->dispatchMouseDrag(point(3, 100)); rig.root->dispatchMouseUp(point(3, 100));
    CHECK_NEAR(clip.notes[0].velocity, 20, 0.1); CHECK_NEAR(clip.notes[1].velocity, 68, 0.1);
    CHECK_NEAR(clip.notes[2].velocity, 90, 0.1);
}

namespace
{
void prepareRegionDrag(RollRig& rig)
{
    rig.ctx.detailVisible = false; rig.ctx.pixelsPerBeat = 40; rig.ctx.scrollBeats = 0;
    const auto original = rig.song.tracks[0].arrangementClips[0];
    for (int track = 0; track < 2; ++track)
    {
        auto& channel = rig.song.tracks[(size_t) track]; channel.automation.clear(); channel.arrangementClips.clear();
        for (int index = 0; index < 2; ++index)
        {
            auto clip = original; clip.uid = model::allocateClipUid(); clip.currentStart = clip.time = 1 + track + index * 4;
            clip.currentEnd = clip.currentStart + 2; channel.arrangementClips.push_back(std::move(clip));
        }
    }
    rig.root->layout();
}

ui::MouseEvent regionPoint(RollRig& rig, double beat, int track, bool title = false)
{
    auto* arrangement = findWidget<ui::ArrangementView>(*rig.root);
    const auto bounds = arrangement->boundsInRoot();
    for (const auto& lane : arrangement->lanes()) if (lane.controlIndex == track && lane.takeLaneId < 0)
        return mouseAt(bounds.x + (float) ((beat - rig.ctx.scrollBeats) * rig.ctx.pixelsPerBeat), bounds.y + lane.y + (title ? 6.0f : 32.0f));
    return {};
}

void selectRegion(RollRig& rig)
{
    rig.root->dispatchMouseDown(regionPoint(rig, 0, 0));
    rig.root->dispatchMouseDrag(regionPoint(rig, 8, 1));
    rig.root->dispatchMouseUp(regionPoint(rig, 8, 1));
    REQUIRE(rig.ctx.arrangementSelection.active); CHECK(rig.ctx.arrangementSelection.firstTrack == 0);
    CHECK(rig.ctx.arrangementSelection.lastTrack == 1);
}
}

TEST_CASE("region drag UI: clip title or body moves all selected clips across channels after a non-destructive preview")
{
    RollRig rig; prepareRegionDrag(rig);
    rig.root->dispatchMouseDown(regionPoint(rig, 0, 0));
    auto overshoot = regionPoint(rig, 8, 1); overshoot.y = rig.root->height() - 80;
    rig.root->dispatchMouseDrag(overshoot); rig.root->dispatchMouseUp(overshoot);
    REQUIRE(rig.ctx.arrangementSelection.active); CHECK(rig.ctx.arrangementSelection.firstTrack == 0);
    CHECK(rig.ctx.arrangementSelection.lastTrack == 1);
    const int first = rig.song.tracks[0].arrangementClips[0].uid;
    const int second = rig.song.tracks[1].arrangementClips[1].uid;
    const auto grab = regionPoint(rig, 1.5, 0, true), drop = regionPoint(rig, 9.5, 0, true);
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(drop);
    CHECK(! rig.history.canUndo()); CHECK_NEAR(model::findArrangementClip(rig.song, 0, first)->currentStart, 1, 1e-9);
    rig.render(); // paint the actual multi-clip preview while the model is still unchanged
    rig.root->dispatchMouseUp(drop);
    CHECK(rig.history.undoLabel() == "Move Region");
    CHECK_NEAR(model::findArrangementClip(rig.song, 0, first)->currentStart, 9, 1e-9);
    CHECK_NEAR(model::findArrangementClip(rig.song, 1, second)->currentStart, 14, 1e-9);
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 8, 1e-9); CHECK_NEAR(rig.ctx.arrangementSelection.end, 16, 1e-9);
    REQUIRE(rig.history.undo(rig.song)); CHECK(! rig.history.canUndo());
    CHECK_NEAR(model::findArrangementClip(rig.song, 0, first)->currentStart, 1, 1e-9);
    // A body drag within the retained selection also moves the entire region.
    rig.ctx.arrangementSelection = {true, 0, 8, 0, 1};
    rig.root->dispatchMouseDown(regionPoint(rig, 1.5, 0));
    rig.root->dispatchMouseDrag(regionPoint(rig, 5.5, 0)); rig.root->dispatchMouseUp(regionPoint(rig, 5.5, 0));
    CHECK_NEAR(model::findArrangementClip(rig.song, 0, first)->currentStart, 5, 1e-9);
    CHECK_NEAR(model::findArrangementClip(rig.song, 1, second)->currentStart, 10, 1e-9);
}

TEST_CASE("region drag UI: compatible vertical block moves retain relative channel spacing and incompatible drops cancel")
{
    RollRig rig; prepareRegionDrag(rig);
    auto third = rig.song.tracks[1]; third.id = 999; third.arrangementClips.clear(); rig.song.tracks.push_back(third); rig.root->layout();
    selectRegion(rig);
    const int first = rig.song.tracks[0].arrangementClips[0].uid, second = rig.song.tracks[1].arrangementClips[0].uid;
    rig.root->dispatchMouseDown(regionPoint(rig, 1.5, 0));
    rig.root->dispatchMouseDrag(regionPoint(rig, 5.5, 1)); rig.root->dispatchMouseUp(regionPoint(rig, 5.5, 1));
    CHECK(rig.song.tracks[0].arrangementClips.empty());
    REQUIRE(model::findArrangementClip(rig.song, 1, first)); REQUIRE(model::findArrangementClip(rig.song, 2, second));
    CHECK_NEAR(model::findArrangementClip(rig.song, 1, first)->currentStart, 5, 1e-9);
    CHECK_NEAR(model::findArrangementClip(rig.song, 2, second)->currentStart, 6, 1e-9);
    CHECK(rig.ctx.arrangementSelection.firstTrack == 1); CHECK(rig.ctx.arrangementSelection.lastTrack == 2);
    REQUIRE(rig.history.undo(rig.song));
    rig.song.tracks[2].kind = model::TrackKind::Audio; rig.root->layout(); selectRegion(rig);
    rig.root->dispatchMouseDown(regionPoint(rig, 1.5, 0));
    rig.root->dispatchMouseDrag(regionPoint(rig, 5.5, 1)); rig.root->dispatchMouseUp(regionPoint(rig, 5.5, 1));
    CHECK(! rig.history.canUndo()); CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9);
    REQUIRE(model::findArrangementClip(rig.song, 0, first)); REQUIRE(model::findArrangementClip(rig.song, 1, second));
}

TEST_CASE("region drag UI: click Escape and no-op drag make no edits; Option copies the whole block and Cmd bypasses snap")
{
    RollRig rig; prepareRegionDrag(rig); selectRegion(rig);
    const auto grab = regionPoint(rig, 1.5, 0);
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseUp(grab); CHECK(! rig.history.canUndo());
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(regionPoint(rig, 5.5, 0));
    REQUIRE(rig.root->dispatchKey({.keyCode = juce::KeyPress::escapeKey}));
    rig.root->dispatchMouseUp(regionPoint(rig, 5.5, 0)); CHECK(! rig.history.canUndo());
    auto copyGrab = grab; copyGrab.alt = true;
    auto freeDrop = regionPoint(rig, 9.87, 0); freeDrop.alt = true; freeDrop.cmd = true;
    rig.root->dispatchMouseDown(copyGrab); rig.root->dispatchMouseDrag(freeDrop); rig.root->dispatchMouseUp(freeDrop);
    CHECK(rig.history.undoLabel() == "Copy Region");
    REQUIRE(rig.song.tracks[0].arrangementClips.size() == 4); REQUIRE(rig.song.tracks[1].arrangementClips.size() == 4);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].currentStart, 1, 1e-9);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[2].currentStart, 9.37, 1e-5);
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 8.37, 1e-5);
}

TEST_CASE("region drag UI: release outside cancels, recording blocks commit and the whole region clamps to beat zero")
{
    RollRig rig; prepareRegionDrag(rig); selectRegion(rig);
    const auto grab = regionPoint(rig, 1.5, 0), drop = regionPoint(rig, 9.5, 0);
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(drop);
    auto outside = drop; outside.x = rig.root->width() - 20;
    rig.root->dispatchMouseUp(outside); CHECK(! rig.history.canUndo());
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9);
    rig.root->dispatchMouseDown(grab); rig.root->dispatchMouseDrag(drop);
    rig.ctx.recording = true; rig.root->dispatchMouseUp(drop); CHECK(! rig.history.canUndo()); rig.ctx.recording = false;
    rig.ctx.arrangementSelection = {true, 1, 8, 0, 1};
    const int first = rig.song.tracks[0].arrangementClips[0].uid, second = rig.song.tracks[1].arrangementClips[0].uid;
    rig.root->dispatchMouseDown(regionPoint(rig, 2.5, 0));
    rig.root->dispatchMouseDrag(regionPoint(rig, 0.5, 0)); rig.root->dispatchMouseUp(regionPoint(rig, 0.5, 0));
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9); CHECK_NEAR(rig.ctx.arrangementSelection.end, 7, 1e-9);
    REQUIRE(model::findArrangementClip(rig.song, 0, first)); REQUIRE(model::findArrangementClip(rig.song, 1, second));
    CHECK_NEAR(model::findArrangementClip(rig.song, 0, first)->currentStart, 0, 1e-9);
    CHECK_NEAR(model::findArrangementClip(rig.song, 1, second)->currentStart, 1, 1e-9);
}

TEST_CASE("shift clip range: includes both endpoints intervening clips and tracks and keeps the first anchor")
{
    RollRig rig; prepareRegionDrag(rig); rig.ctx.pixelsPerBeat = 20;
    auto& clips = rig.song.tracks[0].arrangementClips;
    const auto prototype = clips.front(); clips.clear();
    for (int i = 0; i < 7; ++i) { auto clip = prototype; clip.uid = model::allocateClipUid(); clip.currentStart = i * 4; clip.currentEnd = i * 4 + 2; clips.push_back(clip); }
    rig.root->layout();
    auto click = [&](double beat, int track, bool shift) {
        auto e = regionPoint(rig, beat, track, true); e.shift = shift;
        rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    };
    click(1, 0, false); click(21, 0, true);
    REQUIRE(rig.ctx.arrangementSelection.active); CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9); CHECK_NEAR(rig.ctx.arrangementSelection.end, 22, 1e-9);
    CHECK(rig.ctx.selectedClipUid == clips[5].uid);
    click(25, 0, true); CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9); CHECK_NEAR(rig.ctx.arrangementSelection.end, 26, 1e-9);
    click(3, 1, true); CHECK(rig.ctx.arrangementSelection.firstTrack == 0); CHECK(rig.ctx.arrangementSelection.lastTrack == 1);
    CHECK_NEAR(rig.ctx.arrangementSelection.start, 0, 1e-9); CHECK_NEAR(rig.ctx.arrangementSelection.end, 4, 1e-9);
    CHECK(! rig.history.canUndo());
}

TEST_CASE("track drag UI: both views move a channel into a nested group and Escape or recording cancels")
{
    for (auto view : {ui::MainView::Session, ui::MainView::Arrangement})
    {
        SessionGroupRig rig; rig.context.mainView = view; rig.root->layout();
        auto point = [&](int track, float fraction) {
            if (view == ui::MainView::Session)
            {
                const auto origin = rig.session->boundsInRoot(); const auto slot = rig.session->stopRowBounds(track);
                return mouseAt(origin.x + slot.x + slot.w * fraction, origin.y + 10);
            }
            auto* arrangement = findWidget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement);
            const auto origin = arrangement->boundsInRoot();
            for (const auto& lane : arrangement->lanes()) if (lane.controlIndex == track)
                return mouseAt(origin.x + arrangement->width() - rig.context.arrangementHeaderWidth() + 70, origin.y + lane.y + lane.h * fraction);
            return mouseAt(-1, -1);
        };
        auto from = point(5, 0.5f), to = point(2, 0.5f);
        rig.root->dispatchMouseDown(from); rig.root->dispatchMouseDrag(to);
        CHECK(rig.song.tracks[5].id == 15); // model stays untouched during preview
        rig.root->dispatchKey({.keyCode = juce::KeyPress::escapeKey}); rig.root->dispatchMouseUp(to); CHECK(! rig.history.canUndo());
        rig.root->dispatchMouseDown(from); rig.root->dispatchMouseDrag(to); rig.root->dispatchMouseUp(to);
        REQUIRE(rig.history.canUndo()); CHECK(rig.song.tracks[3].id == 15); CHECK(rig.song.tracks[3].groupId == 12);
        REQUIRE(rig.history.undo(rig.song)); rig.root->layout();
        rig.context.recording = true;
        rig.root->dispatchMouseDown(from); rig.root->dispatchMouseDrag(to); rig.root->dispatchMouseUp(to);
        CHECK(rig.song.tracks[5].id == 15); CHECK(! rig.history.canUndo());
    }
}
