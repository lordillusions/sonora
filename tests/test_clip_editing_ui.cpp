// UI-71: clip editing like the reference app, through real mouse and key events (docs/CLIP_EDITING.md): Arrangement moves
// and edge drags snap to the grid and to other clips, overwrite what they land on and keep unlooped end markers; the clip
// editor opens on the clip's region with Live's clip-relative numbering, and its start / end / loop markers drag.
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "core/ClipGeometry.h"
#include "ui/GridModel.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PianoRoll.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
ui::PianoRoll* findPianoRoll(ui::Widget& w)
{
    if (auto* found = dynamic_cast<ui::PianoRoll*>(&w)) return found;
    for (const auto& child : w.children()) if (auto* found = findPianoRoll(*child)) return found;
    return nullptr;
}

model::Clip midiClip(const model::Clip& templateClip, double start, double end, model::LoopSettings loop)
{
    auto c = templateClip; // keeps the fixture's XML template so the set still saves
    c.currentStart = c.time = start;
    c.currentEnd = end;
    c.loop = loop;
    c.notes = {{0.25, 0.5, 60, 100.0f}};
    return c;
}

struct ArrangementRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    float laneTop = 0;
    explicit ArrangementRig(bool detail = false)
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        song = *loaded.song;
        ctx.song = &song;
        ctx.ioVisible = ctx.browserVisible = false;
        ctx.detailVisible = detail;
        ctx.detailTab = ui::DetailTab::Clip;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.pixelsPerBeat = 14.0; // adaptive grid (Medium): 1 beat
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.undo = [this] { history.undo(song); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 700});
        root->layout();
        laneTop = ctx.theme.metrics.controlBarHeight + ui::ArrangementView::rulerHeightFor(! song.locators.empty());
    }
    float beatX(double beat) const { return ctx.theme.metrics.outerMargin + (float) (beat * ctx.pixelsPerBeat); }
    static ui::MouseEvent mouse(float x, float y, bool cmd = false, bool alt = false)
    {
        ui::MouseEvent e; e.x = x; e.y = y; e.cmd = cmd; e.alt = alt; return e;
    }
    void drag(float x0, float x1, float y, bool cmd = false, bool alt = false)
    {
        root->dispatchMouseDown(mouse(x0, y, cmd, alt));
        root->dispatchMouseDrag(mouse(x1, y, cmd, alt));
        root->dispatchMouseUp(mouse(x1, y, cmd, alt));
    }
    std::vector<model::Clip>& clips(int track = 0) { return song.tracks[(size_t) track].arrangementClips; }
};
} // namespace

TEST_CASE("clip editing: moving snaps the start to the grid, Cmd moves freely, and Cmd snaps while the grid is off")
{
    ArrangementRig rig;
    auto& clips = rig.clips();
    clips[0] = midiClip(clips[0], 4.3, 8.3, {0, 4, 0, true}); // off the grid
    const float title = rig.laneTop + 6;
    // grabbed at 6.0, dragged 2.4 beats: the raw start 6.7 lands on the grid line 7 (Live), not at 6.7.
    rig.drag(rig.beatX(6.0), rig.beatX(8.4), title);
    CHECK_NEAR(clips[0].currentStart, 7.0, 1e-9);
    CHECK_NEAR(clips[0].currentEnd, 11.0, 1e-9);
    // Cmd: no snapping.
    rig.drag(rig.beatX(9.0), rig.beatX(9.0 + 1.3), title, true);
    CHECK_NEAR(clips[0].currentStart, 8.3, 1e-6);
    // Snap to Grid off: free, unless Cmd is held, which snaps temporarily.
    rig.ctx.arrangementGrid.snap = false;
    rig.drag(rig.beatX(10.0), rig.beatX(10.5), title);
    CHECK_NEAR(clips[0].currentStart, 8.8, 1e-6);
    rig.drag(rig.beatX(10.0), rig.beatX(10.5), title, true);
    CHECK_NEAR(clips[0].currentStart, 9.0, 1e-9);
}

TEST_CASE("clip editing: a moved clip docks to its neighbour's edge and overwrites what it lands on")
{
    ArrangementRig rig;
    auto& clips = rig.clips();
    const auto base = clips[0];
    clips = {midiClip(base, 0, 4, {0, 4, 0, false}), midiClip(base, 10.4, 18.4, {0, 8, 0, false})};
    clips[1].uid = model::allocateClipUid();
    const int first = clips[0].uid, second = clips[1].uid;
    const float title = rig.laneTop + 6;
    // Dragged to a raw start of 6.5: the grid says 6 or 7, but the end (10.5) is 0.1 from the next clip's start (10.4).
    rig.drag(rig.beatX(2.0), rig.beatX(8.5), title);
    REQUIRE(clips.size() == 2);
    CHECK(clips[0].uid == first);
    CHECK_NEAR(clips[0].currentStart, 6.4, 1e-9);
    CHECK_NEAR(clips[0].currentEnd, 10.4, 1e-9);
    // Dropped 2 beats into the other clip: that clip loses the covered part, its content stays in place.
    rig.drag(rig.beatX(8.4), rig.beatX(10.4), title, true);
    REQUIRE(clips.size() == 2);
    CHECK_NEAR(clips[0].currentEnd, 12.4, 1e-6);
    CHECK(clips[1].uid == second);
    CHECK_NEAR(clips[1].currentStart, 12.4, 1e-6);
    CHECK_NEAR(clips[1].currentEnd, 18.4, 1e-6);
    CHECK_NEAR(clips[1].loop.loopStart, 2.0, 1e-6);
    CHECK(rig.history.undoLabel() == "Move Clip");
    REQUIRE(rig.history.undo(rig.song));
    CHECK_NEAR(clips[1].currentStart, 10.4, 1e-9);
}

TEST_CASE("clip editing: edge drags snap, keep one grid step, and move an unlooped clip's end marker like Live")
{
    ArrangementRig rig;
    auto& clips = rig.clips();
    clips[0] = midiClip(clips[0], 4, 12, {0, 8, 0, false});
    const float title = rig.laneTop + 6;
    // Right edge: 12 -> 15.3 snaps to 15; the end marker follows (content 0 .. 11 is now heard).
    rig.drag(rig.beatX(12.0) - 2, rig.beatX(15.3) - 2, title);
    CHECK_NEAR(clips[0].currentEnd, 15.0, 1e-9);
    CHECK_NEAR(clips[0].loop.loopEnd, 11.0, 1e-9);
    CHECK_NEAR(clips[0].loop.startRelative, 0.0, 1e-9);
    // Dragging the right edge past the left one stops one grid step after the start.
    rig.drag(rig.beatX(15.0) - 2, rig.beatX(1.0), title);
    CHECK_NEAR(clips[0].currentEnd, 5.0, 1e-9);
    REQUIRE(rig.history.undo(rig.song));
    // Left edge: 4 -> 6.2 snaps to 6; the content stays where it was (the start marker moves to 2).
    rig.drag(rig.beatX(4.0) + 2, rig.beatX(6.2) + 2, title);
    CHECK_NEAR(clips[0].currentStart, 6.0, 1e-9);
    CHECK_NEAR(clips[0].loop.loopStart, 2.0, 1e-9);
    CHECK_NEAR(clips[0].loop.loopEnd, 11.0, 1e-9);
    // Looped clip: the right edge repeats the loop, the loop itself stays.
    clips[0] = midiClip(clips[0], 4, 8, {0, 4, 0, true});
    rig.drag(rig.beatX(8.0) - 2, rig.beatX(16.0) - 2, title);
    CHECK_NEAR(clips[0].currentEnd, 16.0, 1e-9);
    CHECK_NEAR(clips[0].loop.loopEnd, 4.0, 1e-9);
}

TEST_CASE("clip editing: Cmd+Shift+M inserts a MIDI clip over the time selection")
{
    ArrangementRig rig;
    rig.clips().clear();
    rig.clips(1).clear();
    // the fixture's clips are the format template; keep one on the other track
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    rig.clips(1) = {midiClip(loaded.song->tracks[0].arrangementClips[0], 40, 44, {0, 4, 0, true})};
    // a click in the empty lane gives the Arrangement the keyboard; then the time selection 8 .. 14 on track 0
    rig.root->dispatchMouseDown(ArrangementRig::mouse(rig.beatX(30.0), rig.laneTop + 30));
    rig.root->dispatchMouseUp(ArrangementRig::mouse(rig.beatX(30.0), rig.laneTop + 30));
    rig.ctx.selectedTrack = 0;
    rig.ctx.arrangementSelection = {true, 8.0, 14.0, 0, 0};
    ui::KeyEvent m; m.cmd = m.shift = true; m.textChar = 'M';
    rig.root->dispatchKey(m);
    REQUIRE(rig.clips().size() == 1);
    CHECK_NEAR(rig.clips()[0].currentStart, 8.0, 1e-9);
    CHECK_NEAR(rig.clips()[0].currentEnd, 14.0, 1e-9);
    CHECK_NEAR(rig.clips()[0].loop.loopEnd - rig.clips()[0].loop.loopStart, 6.0, 1e-9);
}

TEST_CASE("clip editor: opens on the clip's region with clip-relative bar numbers, before 1.1.1 too")
{
    // Ruler numbers: content beat 0 is "1"; before it come 0.4 and 0 (Live's bar 0), then -1.
    CHECK(ui::rulerText(-1.0, 4.0, 1.0) == "0.4");
    CHECK(ui::rulerText(-4.0, 4.0, 1.0) == "0");
    CHECK(ui::rulerText(-8.0, 4.0, 1.0) == "-1");
    const auto marks = ui::rulerMarks(-8.0, 8.0, 10.0, 4.0, 1.0, 5.5f, 9.0f, true);
    auto markAt = [](const std::vector<ui::RulerMark>& list, double beat) {
        const auto found = std::find_if(list.begin(), list.end(), [beat](const auto& mark) { return std::fabs(mark.beat - beat) < 1e-9; });
        return found == list.end() ? ui::RulerMark{-1e9, 0, false, "missing"} : *found;
    };
    CHECK(markAt(marks, -8.0).text == "-1");
    CHECK(markAt(marks, -4.0).text == "0");
    CHECK(markAt(marks, 0.0).text == "1");
    CHECK(ui::rulerMarks(-8.0, 8.0, 10.0, 4.0, 1.0).front().beat >= 0.0); // the Arrangement ruler starts at 1

    ArrangementRig rig(true);
    auto& clips = rig.clips();
    // Bar 5 of the song, 16 bars long, one-bar loop: the editor shows that one bar, numbered from 1.
    clips[0] = midiClip(clips[0], 16, 80, {0, 4, 0, true});
    rig.ctx.selectedTrack = rig.ctx.selectedClipTrack = 0;
    rig.ctx.selectedClipUid = clips[0].uid;
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    auto* roll = findPianoRoll(*rig.root);
    REQUIRE(roll);
    const float width = roll->noteAreaBounds().w;
    CHECK_NEAR(roll->view.scrollBeats, 0.0, 1e-9);
    CHECK_NEAR(roll->view.pixelsPerBeat, (width - 1) / 4.0, 1e-3);
    // The first labelled ruler mark at the clip's left edge reads "1".
    const auto visible = ui::rulerMarks(roll->view.scrollBeats, roll->view.scrollBeats + width / roll->view.pixelsPerBeat,
                                        roll->view.pixelsPerBeat, 4.0, 1.0, 5.5f, 9.0f, true);
    CHECK(markAt(visible, 0.0).labelled);
    CHECK(markAt(visible, 0.0).text == "1");
    CHECK(markAt(visible, 1.0).text == "1.2");

    // A split fragment that plays content 92..94 (bar 24.1 .. 24.3 of its clip): opens there.
    clips[0] = midiClip(clips[0], 574, 576, {92, 94, 0, false});
    ++rig.ctx.clipZoomRequest;
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    CHECK_NEAR(roll->view.scrollBeats, 92.0, 1e-9);
    CHECK_NEAR(roll->view.pixelsPerBeat, (width - 1) / 2.0, 1e-3);
    CHECK(ui::rulerText(92.0, 4.0, 1.0) == "24");
    // Loop on with an offset start marker (StartRelative 2 in a 4..8 loop): the loop from its start to its end.
    clips[0] = midiClip(clips[0], 16, 48, {4, 8, 2, true});
    ++rig.ctx.clipZoomRequest;
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    CHECK_NEAR(roll->view.scrollBeats, 4.0, 1e-9);
    CHECK_NEAR(roll->view.pixelsPerBeat, (width - 1) / 4.0, 1e-3);
    const auto m = roll->markers();
    CHECK_NEAR(m.start, 6.0, 1e-9);
    CHECK(m.looping);
}

TEST_CASE("clip editor: dragging the end and start flags edits the markers; an unlooped clip's length follows")
{
    ArrangementRig rig(true);
    auto& clips = rig.clips();
    clips[0] = midiClip(clips[0], 16, 24, {0, 8, 0, false});
    rig.ctx.selectedTrack = rig.ctx.selectedClipTrack = 0;
    rig.ctx.selectedClipUid = clips[0].uid;
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    auto* roll = findPianoRoll(*rig.root);
    REQUIRE(roll);
    roll->view.pixelsPerBeat = 40; // grid: 1/8 (Medium density)
    roll->view.scrollBeats = 0;
    const auto origin = roll->boundsInRoot();
    const auto strip = roll->markerStripBounds();
    const float y = origin.y + strip.y + strip.h * 0.5f;
    auto x = [&](double beat) { return origin.x + roll->beatToXPublic(beat); };
    // End flag (points left from beat 8) dragged to 10.1 -> 10 on the clip grid: the clip now ends 2 beats later.
    rig.drag(x(8.0) - 3, x(10.1) - 3, y);
    CHECK_NEAR(clips[0].loop.loopEnd, 10.0, 1e-9);
    CHECK_NEAR(clips[0].currentStart, 16.0, 1e-9);
    CHECK_NEAR(clips[0].currentEnd, 26.0, 1e-9);
    // Start flag (points right from beat 0) dragged to 1: the clip stays at 16 and plays from content beat 1.
    rig.drag(x(0.0) + 3, x(1.0) + 3, y);
    CHECK_NEAR(clips[0].loop.loopStart, 1.0, 1e-9);
    CHECK_NEAR(clips[0].currentStart, 16.0, 1e-9);
    CHECK_NEAR(clips[0].currentEnd, 25.0, 1e-9);
    CHECK(rig.history.undoLabel() == "Change Clip Markers");

    // Looped: dragging the brace's end changes the loop, not the Arrangement length.
    clips[0] = midiClip(clips[0], 16, 48, {0, 4, 0, true});
    rig.drag(x(4.0), x(6.0), y);
    CHECK_NEAR(clips[0].loop.loopEnd, 6.0, 1e-9);
    CHECK_NEAR(clips[0].currentEnd, 48.0, 1e-9);
    // Dragging the brace's body moves the whole loop.
    rig.drag(x(3.0), x(4.0), y);
    CHECK_NEAR(clips[0].loop.loopStart, 1.0, 1e-9);
    CHECK_NEAR(clips[0].loop.loopEnd, 7.0, 1e-9);
}

TEST_CASE("clip editor: a moved note keeps its offset from the grid unless it is dragged onto a grid line")
{
    CHECK_NEAR(ui::snapKeepingOffset(1.35, 0.1, 0.25), 1.35, 1e-9); // 0.1 + 5 steps: nearer than the grid line 1.25
    CHECK_NEAR(ui::snapKeepingOffset(1.27, 0.1, 0.25), 1.25, 1e-9); // the grid line is nearer
    CHECK_NEAR(ui::snapKeepingOffset(1.27, 0.1, 0.0), 1.27, 1e-9);  // free

    ArrangementRig rig(true);
    auto& clips = rig.clips();
    clips[0] = midiClip(clips[0], 0, 8, {0, 8, 0, true});
    clips[0].notes = {{1.1, 0.5, 64, 100.0f}};
    rig.ctx.selectedTrack = rig.ctx.selectedClipTrack = 0;
    rig.ctx.selectedClipUid = clips[0].uid;
    juce::TemporaryFile png(".png");
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    auto* roll = findPianoRoll(*rig.root);
    REQUIRE(roll);
    roll->view.pixelsPerBeat = 40; // grid 1/8 (0.5 beats)
    roll->view.scrollBeats = 0;
    roll->view.topPitch = 68;
    const auto origin = roll->boundsInRoot();
    const auto area = roll->noteAreaBounds();
    const float y = origin.y + area.y + (float) (roll->view.topPitch - 64) * roll->view.rowHeight + roll->view.rowHeight * 0.5f;
    auto x = [&](double beat) { return origin.x + roll->beatToXPublic(beat); };
    // Dragged one beat right: 2.1 (offset kept), not 2.0.
    rig.drag(x(1.3), x(2.3), y);
    CHECK_NEAR(clips[0].notes[0].start, 2.1, 1e-9);
    // Cmd: free.
    rig.drag(x(2.3), x(2.53), y, true);
    CHECK_NEAR(clips[0].notes[0].start, 2.33, 1e-6);
}
