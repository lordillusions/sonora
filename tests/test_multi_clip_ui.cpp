// UI-93: multi-clip editing in the note editor through real mouse and key events: a time selection over MIDI clips on two
// tracks shows all their notes in Arrangement time, edits reach every clip as one undo step, a click on another clip's
// note focuses it, the velocity lane shows the focused clip only and Focus Mode (N) limits editing to that clip.
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PianoRoll.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

using namespace sonora;

namespace
{
ui::PianoRoll* findRoll(ui::Widget& w)
{
    if (auto* found = dynamic_cast<ui::PianoRoll*>(&w)) return found;
    for (const auto& child : w.children()) if (auto* found = findRoll(*child)) return found;
    return nullptr;
}

struct MultiRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    ui::PianoRoll* roll = nullptr;
    juce::TemporaryFile png{".png"};
    MultiRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        song = *loaded.song;
        auto& a = song.tracks[0].arrangementClips;
        auto& b = song.tracks[1].arrangementClips;
        a.resize(1);
        b = {a[0]}; // the fixture's clip as the format template, with its own uid
        b[0].uid = a[0].uid + 1000;
        // Track 0: bar 1-2 (content 0..8); track 1: bars 3-4 (content 0..8), different colours.
        auto shape = [](model::Clip& c, double start, std::vector<model::MidiNote> notes, int color) {
            c.currentStart = c.time = start; c.currentEnd = start + 8; c.loop = {0, 8, 0, false};
            c.notes = std::move(notes); c.colorIndex = color; model::assignNoteKeys(c.notes);
        };
        shape(a[0], 0, {{1.0, 1.0, 60, 100.0f}, {3.0, 1.0, 62, 100.0f}}, 3);
        shape(b[0], 8, {{2.0, 1.0, 67, 90.0f}}, 12);
        ctx.song = &song;
        ctx.ioVisible = ctx.browserVisible = false;
        ctx.detailVisible = true;
        ctx.detailTab = ui::DetailTab::Clip;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.undo = [this] { history.undo(song); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 760});
        root->layout();
        ctx.selectedTrack = ctx.selectedClipTrack = 0;
        ctx.selectedClipUid = a[0].uid;
        ctx.arrangementSelection = {true, 0.0, 16.0, 0, 1};
        render();
        roll = findRoll(*root);
        if (roll)
        {
            // The detail pane is short at this window size: small rows so pitches 58..72 are all on screen.
            roll->view.rowHeight = 6.0f;
            roll->view.topPitch = 72.0;
            render();
        }
    }
    void render() { REQUIRE(gfx::renderToPng(*root, 1200, 760, 1, png.getFile().getFullPathName().toStdString())); }
    // A point on a note (Arrangement beat, pitch) in root coordinates.
    std::pair<float, float> at(double beat, int pitch) const
    {
        const auto origin = roll->boundsInRoot(), area = roll->noteAreaBounds();
        const float y = area.y + (float) (roll->view.topPitch - pitch) * roll->view.rowHeight + roll->view.rowHeight * 0.5f;
        return {origin.x + roll->beatToXPublic(beat), origin.y + y};
    }
    void click(float x, float y, bool shift = false)
    {
        ui::MouseEvent e; e.x = x; e.y = y; e.shift = shift;
        root->dispatchMouseDown(e);
        root->dispatchMouseUp(e);
    }
    void key(uint32_t ch, int code = 0, bool cmd = false)
    {
        ui::KeyEvent k; k.textChar = ch; k.keyCode = code; k.cmd = cmd;
        root->dispatchKey(k);
    }
    const std::vector<model::MidiNote>& notes(int track) { return song.tracks[(size_t) track].arrangementClips[0].notes; }
};
} // namespace

TEST_CASE("multi-clip editing: a time selection over clips on two tracks shows all their notes in Arrangement time")
{
    MultiRig rig;
    REQUIRE(rig.roll);
    CHECK(rig.roll->multiClip());
    REQUIRE(rig.roll->multiClipMembers().size() == 2);
    const auto& shown = rig.roll->shownNotes();
    REQUIRE(shown.size() == 3);
    CHECK_NEAR(shown[2].start, 10.0, 1e-9); // track 1's note at content 2 of a clip at beat 8
    // The view fits the first clip's start to the last clip's end, and loop bars stack one row per track.
    CHECK_NEAR(rig.roll->view.scrollBeats, 0.0, 1e-9);
    CHECK_NEAR(rig.roll->view.pixelsPerBeat, (rig.roll->noteAreaBounds().w - 1) / 16.0, 1e-3);
    const auto bar0 = rig.roll->loopBarBounds(0), bar1 = rig.roll->loopBarBounds(1);
    CHECK(bar0.w > 0); CHECK(bar1.w > 0);
    CHECK(bar1.y > bar0.y);
    CHECK(bar1.x > bar0.x + bar0.w - 2);

    // Without the selection (or with the focused clip outside it) the editor shows just the focused clip again.
    rig.ctx.arrangementSelection = {};
    rig.render();
    CHECK(! rig.roll->multiClip());
    CHECK(rig.roll->shownNotes().size() == 2);
}

TEST_CASE("multi-clip editing: Cmd+A and transpose reach every clip as one undo step; a click focuses the note's clip")
{
    MultiRig rig;
    REQUIRE(rig.roll);
    // A click on track 1's note focuses its clip (the selected track stays) and selects the note.
    const auto [nx, ny] = rig.at(10.5, 67);
    rig.click(nx, ny);
    CHECK(rig.ctx.selectedClipTrack == 1);
    CHECK(rig.ctx.selectedClipUid == rig.song.tracks[1].arrangementClips[0].uid);
    CHECK(rig.ctx.selectedTrack == 0);
    CHECK(rig.roll->selectedNotes().size() == 1);
    rig.render();
    CHECK(rig.roll->multiClip());
    CHECK(rig.roll->focusedMember() == 1);

    rig.key('a', 'A', true);
    CHECK(rig.roll->selectedNotes().size() == 3);
    rig.key(0, juce::KeyPress::upKey);
    CHECK(rig.notes(0)[0].pitch == 61);
    CHECK(rig.notes(0)[1].pitch == 63);
    CHECK(rig.notes(1)[0].pitch == 68);
    CHECK(rig.roll->selectedNotes().size() == 3); // the transposed notes stay selected
    rig.ctx.undo();
    CHECK(rig.notes(0)[0].pitch == 60);
    CHECK(rig.notes(1)[0].pitch == 67);

    // Dragging a note of the first clip (all three still selected) moves the notes of both clips, each in its own content
    // time, as one undo step.
    rig.render();
    const auto [mx, my] = rig.at(1.5, 60);
    ui::MouseEvent down; down.x = mx; down.y = my;
    rig.root->dispatchMouseDown(down);
    auto drag = down; drag.x = mx + (float) (2.0 * rig.roll->view.pixelsPerBeat);
    rig.root->dispatchMouseDrag(drag);
    rig.root->dispatchMouseUp(drag);
    CHECK(rig.ctx.selectedClipTrack == 0); // focus followed the clicked note
    CHECK_NEAR(rig.notes(0)[0].start, 3.0, 1e-9);
    CHECK_NEAR(rig.notes(0)[1].start, 5.0, 1e-9);
    CHECK_NEAR(rig.notes(1)[0].start, 4.0, 1e-9);
    rig.ctx.undo();
    CHECK_NEAR(rig.notes(0)[0].start, 1.0, 1e-9);
    CHECK_NEAR(rig.notes(1)[0].start, 2.0, 1e-9);
}

TEST_CASE("multi-clip editing: Focus Mode (N) limits selection to the focused clip; a loop bar click focuses its clip")
{
    MultiRig rig;
    REQUIRE(rig.roll);
    // Give the editor the keyboard with a click on empty note space.
    const auto [ex, ey] = rig.at(6.5, 70);
    rig.click(ex, ey);
    rig.key('n');
    CHECK(rig.roll->focusMode());
    rig.key('a', 'A', true);
    CHECK(rig.roll->selectedNotes().size() == 2); // only the focused (track 0) clip's notes
    rig.key(0, juce::KeyPress::deleteKey);
    CHECK(rig.notes(0).empty());
    CHECK(rig.notes(1).size() == 1);

    // A click on the second loop bar focuses track 1's clip.
    rig.render();
    const auto origin = rig.roll->boundsInRoot();
    const auto bar = rig.roll->loopBarBounds(1);
    rig.click(origin.x + bar.x + bar.w * 0.5f, origin.y + bar.y + bar.h * 0.5f);
    CHECK(rig.ctx.selectedClipTrack == 1);
    rig.key('a', 'A', true);
    CHECK(rig.roll->selectedNotes().size() == 1);
}

TEST_CASE("multi-clip editing: a time selection dragged from empty space over two MIDI clips focuses the first")
{
    MultiRig rig;
    rig.ctx.arrangementSelection = {};
    rig.ctx.selectedClipTrack = rig.ctx.selectedClipUid = -1;
    rig.render();
    const float laneTop = rig.ctx.theme.metrics.controlBarHeight + ui::ArrangementView::rulerHeightFor(! rig.song.locators.empty());
    const float startX = rig.ctx.theme.metrics.outerMargin + (float) (20.0 * rig.ctx.pixelsPerBeat); // empty: after both clips
    ui::MouseEvent down; down.x = startX; down.y = laneTop + 30;
    rig.root->dispatchMouseDown(down);
    auto drag = down; drag.x = rig.ctx.theme.metrics.outerMargin + 2; drag.y = laneTop + 30 + rig.ctx.theme.metrics.arrangementLaneHeight;
    rig.root->dispatchMouseDrag(drag);
    rig.root->dispatchMouseUp(drag);
    CHECK(rig.ctx.arrangementSelection.active);
    CHECK(rig.ctx.arrangementSelection.firstTrack == 0);
    CHECK(rig.ctx.arrangementSelection.lastTrack == 1);
    CHECK(rig.ctx.selectedClipTrack == 0);
    CHECK(rig.ctx.selectedClipUid == rig.song.tracks[0].arrangementClips[0].uid);
    rig.render();
    CHECK(rig.roll->multiClip());
}
