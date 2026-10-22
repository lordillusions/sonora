#include "TestFramework.h"
#include "core/Edit.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/MeterBallistics.h"
#include "ui/MidiClipEdits.h"
#include "ui/views/MainLayout.h"
#include "ui/views/ClipControls.h"
#include "ui/views/DetailView.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/SessionView.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/MpeEditor.h"
#include "ui/widgets/Fader.h"
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <cmath>

using namespace sonora;
namespace
{
template <typename T> T* widget(ui::Widget& parent)
{
    if (auto* result = dynamic_cast<T*>(&parent)) return result;
    for (const auto& child : parent.children()) if (auto* result = widget<T>(*child)) return result;
    return nullptr;
}
struct EditorRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    EditorRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded); song = *loaded.song;
        ctx.song = &song; ctx.browserVisible = false; ctx.detailTab = ui::DetailTab::Clip; ctx.theme.metrics.detailHeight = 500;
        ctx.selectedClipTrack = 0; ctx.selectedClipUid = song.tracks[0].arrangementClips[0].uid;
        ctx.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); };
        root = std::make_unique<ui::MainLayout>(ctx); root->setBounds({0, 0, 1200, 800}); root->layout(); render();
    }
    void render(const char* destination = nullptr)
    {
        juce::TemporaryFile file(".png");
        REQUIRE(gfx::renderToPng(*root, 1200, 800, 1, destination ? destination : file.getFile().getFullPathName().toStdString()));
    }
    void click(ui::Widget& owner, const ui::Rect& rect)
    {
        const auto origin = owner.boundsInRoot(); ui::MouseEvent e; e.x = origin.x + rect.centreX(); e.y = origin.y + rect.centreY();
        root->dispatchMouseDown(e); root->dispatchMouseUp(e); render();
    }
};
}

TEST_CASE("editor meter: stereo bars decay to exact silence and their final painted pixels clear")
{
    float value = 1;
    for (int tick = 0; tick < 150; ++tick) value = ui::meterDecay(0, value);
    CHECK_NEAR(value, 0, 0); CHECK_NEAR(ui::meterDecay(NAN, 0), 0, 0);
    ui::UiContext context; ui::RootWidget root(context); root.setBounds({0, 0, 90, 200});
    auto* fader = root.add<ui::Fader>(); fader->setBounds({0, 0, 90, 200}); float peak = 1;
    fader->getPeakL = fader->getPeakR = [&] { return peak; }; fader->tick(); peak = 0;
    for (int tick = 0; tick < 150; ++tick) fader->tick();
    juce::TemporaryFile image(".png"); REQUIRE(gfx::renderToPng(root, 90, 200, 2, image.getFile().getFullPathName().toStdString()));
    auto png = juce::ImageFileFormat::loadFrom(image.getFile()); const auto meter = fader->meterRect();
    CHECK(png.getPixelAt((int) (meter.x * 2 + 2), 397).getARGB() == context.theme.color("MeterBackground"));
}

TEST_CASE("editor MIDI tools: generators obey scale and range while transforms preserve unselected material")
{
    model::Clip clip; clip.loop.loopEnd = 4;
    model::MidiNote note; note.start = 0.18; note.duration = 1; note.pitch = 61; note.noteId = 33;
    auto outside = note; outside.start = 8; outside.pitch = 100; clip.notes = {note, outside};
    ui::midiedit::ToolSettings settings; settings.rate = 0.25;
    auto result = ui::midiedit::applyTool(clip, settings, {0}, 0, 4); CHECK_NEAR(result.notes[0].start, 0.25, 1e-9); CHECK_NEAR(result.notes[1].start, 8, 1e-9);
    settings.tool = ui::midiedit::Tool::Chop; settings.parts = 4;
    result = ui::midiedit::applyTool(clip, settings, {0}, 0, 4); CHECK(result.notes.size() == 5); CHECK(result.notes.back().pitch == 100);
    for (const auto& n : result.notes) if (n.start < 4) { CHECK(n.noteId == -1); CHECK(n.duration > 0); }
    for (auto tool : {ui::midiedit::Tool::Rhythm, ui::midiedit::Tool::Seed, ui::midiedit::Tool::Shape, ui::midiedit::Tool::Stacks})
    {
        settings.tool = tool; settings.scale = true; settings.scaleName = 1; settings.root = 3; // Minor settings.lowPitch = 60; settings.highPitch = 84; settings.density = 1;
        result = ui::midiedit::applyTool(clip, settings, {}, 0, 4); REQUIRE(result.notes.size() > 1);
        CHECK(result.notes.back().pitch == 100);
        for (const auto& n : result.notes) if (n.start < 4)
        { const int degree = (n.pitch - 3 + 12) % 12; CHECK(degree == 0 || degree == 2 || degree == 3 || degree == 5 || degree == 7 || degree == 8 || degree == 10); CHECK(n.start >= 0); CHECK(n.start < 4); CHECK(n.pitch >= 60); CHECK(n.pitch <= 84); CHECK(n.noteId == -1); }
    }
}

TEST_CASE("editor piano toolbar: Fold Scale and Highlight are independent and empty Fold retains an editable keyboard")
{
    EditorRig rig; auto* roll = widget<ui::PianoRoll>(*rig.root); REQUIRE(roll);
    rig.click(*roll, roll->headerControlBounds(ui::PianoRoll::HeaderControl::Fold)); CHECK(rig.ctx.noteFold); CHECK(! rig.ctx.scaleFold);
    rig.click(*roll, roll->headerControlBounds(ui::PianoRoll::HeaderControl::Scale)); CHECK(rig.ctx.scaleFold); CHECK(! rig.ctx.noteFold);
    rig.click(*roll, roll->headerControlBounds(ui::PianoRoll::HeaderControl::Highlight)); CHECK(! rig.ctx.scaleHighlight); CHECK(rig.ctx.scaleFold);
    rig.song.tracks[0].arrangementClips[0].notes.clear(); rig.ctx.noteFold = true; rig.ctx.scaleFold = false; rig.render();
    CHECK(std::isfinite(roll->view.topPitch)); CHECK(roll->noteAreaBounds().h > 0);
}

TEST_CASE("editor clip panels: Transform Apply edits the note selection once and Generate replaces the time scope")
{
    EditorRig rig; auto* panel = widget<ui::ClipControls>(*rig.root); auto* roll = widget<ui::PianoRoll>(*rig.root); REQUIRE(panel); REQUIRE(roll);
    auto& notes = rig.song.tracks[0].arrangementClips[0].notes; notes.resize(1); notes[0].start = 0.18;
    rig.render(); rig.click(*panel, panel->controlBounds(ui::ClipControls::Id::TransformHeader));
    const auto apply = panel->controlBounds(ui::ClipControls::Id::ToolApply); REQUIRE(! apply.isEmpty());
    rig.click(*panel, apply); CHECK_NEAR(notes[0].start, 0.25, 1e-9); CHECK(rig.history.undoLabel() == "Quantize");
    REQUIRE(rig.history.undo(rig.song)); CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].notes[0].start, 0.18, 1e-9);
    rig.render(); rig.click(*panel, panel->controlBounds(ui::ClipControls::Id::GenerateHeader));
    rig.click(*panel, panel->controlBounds(ui::ClipControls::Id::ToolApply)); CHECK(rig.history.undoLabel() == "Rhythm");
    CHECK(rig.song.tracks[0].arrangementClips[0].notes.size() > 1);
}

TEST_CASE("editor MPE: a curve gesture is previewed then saved as one undo and switching dimensions edits release velocity")
{
    EditorRig rig; auto* detail = widget<ui::DetailView>(*rig.root); REQUIRE(detail);
    rig.click(*detail, detail->noteTabBounds(2)); CHECK(rig.ctx.clipMpeVisible);
    auto* mpe = widget<ui::MpeEditor>(*rig.root); REQUIRE(mpe); REQUIRE(mpe->visible);
    const auto bounds = mpe->boundsInRoot(), area = mpe->plot();
    ui::MouseEvent e; e.x = bounds.x + area.x + area.w * 0.5f; e.y = bounds.y + area.y + area.h * 0.25f;
    rig.root->dispatchMouseDown(e); e.x += 20; rig.root->dispatchMouseDrag(e);
    CHECK(rig.song.tracks[0].arrangementClips[0].noteExpressions.empty());
    rig.root->dispatchMouseUp(e); REQUIRE(rig.history.canUndo());
    const auto& clip = rig.song.tracks[0].arrangementClips[0]; REQUIRE(clip.noteExpressions.size() == 1); CHECK(clip.noteExpressions[0].cc == -2); CHECK(clip.notes[0].noteId >= 0);
    REQUIRE(rig.history.undo(rig.song)); CHECK(rig.song.tracks[0].arrangementClips[0].noteExpressions.empty());
    rig.click(*mpe, mpe->dimensionBounds(3)); rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    CHECK_NEAR(rig.song.tracks[0].arrangementClips[0].notes[0].releaseVelocity, 95, 1);
    rig.render("/tmp/sonora-editor-mpe-ui.png");
    rig.click(*mpe, mpe->dimensionBounds(0));
    rig.root->dispatchMouseDown(e); rig.root->dispatchMouseDrag(e);
    ++rig.song.tracks[0].arrangementClips[0].notes[0].pitch;
    const int observedPitch = rig.song.tracks[0].arrangementClips[0].notes[0].pitch;
    rig.root->dispatchMouseUp(e);
    CHECK(rig.song.tracks[0].arrangementClips[0].notes[0].pitch == observedPitch);
    CHECK(rig.song.tracks[0].arrangementClips[0].noteExpressions.empty());
}

TEST_CASE("editor fidelity: Device View stays compact and clip editors retain their independent resized height")
{
    EditorRig rig; auto* detail = widget<ui::DetailView>(*rig.root); REQUIRE(detail);
    CHECK_NEAR(detail->height(), 500, 0);
    rig.ctx.detailTab = ui::DetailTab::Devices; rig.root->layout();
    CHECK_NEAR(detail->height(), 200, 0);
    const auto bounds = detail->boundsInRoot(); ui::MouseEvent e; e.x = 800; e.y = bounds.y - 3;
    rig.root->dispatchMouseDown(e); e.y -= 100; rig.root->dispatchMouseDrag(e); rig.root->dispatchMouseUp(e);
    CHECK_NEAR(rig.ctx.theme.metrics.detailHeight, 500, 0); CHECK_NEAR(detail->height(), 200, 0);
    rig.ctx.detailTab = ui::DetailTab::Clip; rig.root->layout(); CHECK_NEAR(detail->height(), 500, 0);
}

TEST_CASE("editor fidelity: Arrangement clip bodies and piano notes retain their title colour at every velocity")
{
    EditorRig rig;
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.colorIndex = rig.song.tracks[0].colorIndex = 3;
    clip.currentStart = 0; clip.currentEnd = clip.loop.loopEnd = 4;
    clip.loop.loopStart = clip.loop.startRelative = 0;
    clip.notes = {{0.5, 0.5, 60, 25}, {1.5, 0.5, 62, 120}};
    rig.ctx.scaleFold = rig.ctx.noteFold = false; rig.ctx.pixelsPerBeat = 100;
    auto* roll = widget<ui::PianoRoll>(*rig.root); REQUIRE(roll);
    roll->view.topPitch = 68; roll->view.rowHeight = 12; roll->view.scrollBeats = 0; roll->view.pixelsPerBeat = 100;
    juce::TemporaryFile file(".png"); rig.render(file.getFile().getFullPathName().toRawUTF8());
    const auto image = juce::ImageFileFormat::loadFrom(file.getFile()); REQUIRE(image.isValid());
    const auto origin = roll->boundsInRoot(), area = roll->noteAreaBounds();
    const auto expected = juce::Colour(ui::Theme::paletteColor(clip.colorIndex));
    for (const auto& note : clip.notes)
    {
        const int x = (int) (origin.x + area.x + (float) (note.start * roll->view.pixelsPerBeat) + 10);
        const int y = (int) (origin.y + area.y + (float) (roll->view.topPitch - note.pitch) * roll->view.rowHeight + 5);
        CHECK(image.getPixelAt(x, y) == expected);
    }
    auto* arrangement = widget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement); REQUIRE(! arrangement->lanes().empty());
    const auto arrangementOrigin = arrangement->boundsInRoot();
    const int x = (int) arrangementOrigin.x + 300;
    const int y = (int) (arrangementOrigin.y + arrangement->lanes()[0].y);
    CHECK(image.getPixelAt(x, y + 5) == expected);
    CHECK(image.getPixelAt(x, y + 40) == expected);
}

TEST_CASE("editor fidelity: new Arrangement and Session MIDI clips inherit their destination channel colour")
{
    EditorRig rig; rig.ctx.detailVisible = false;
    auto& target = rig.song.tracks[1]; target.kind = model::TrackKind::Midi; target.colorIndex = 8;
    target.arrangementClips.clear(); target.slots.resize(1); target.slots[0].clip.reset();
    rig.root->layout();
    auto* arrangement = widget<ui::ArrangementView>(*rig.root); REQUIRE(arrangement); REQUIRE(arrangement->lanes().size() >= 2);
    const auto origin = arrangement->boundsInRoot(); ui::MouseEvent e;
    e.x = origin.x + 100; e.y = origin.y + arrangement->lanes()[1].y + 25; e.clicks = 2;
    rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    REQUIRE(target.arrangementClips.size() == 1); CHECK(target.arrangementClips[0].colorIndex == target.colorIndex);
    rig.ctx.mainView = ui::MainView::Session; rig.root->layout();
    auto* session = widget<ui::SessionView>(*rig.root); REQUIRE(session);
    const auto sessionOrigin = session->boundsInRoot(), column = session->stopRowBounds(1);
    e.x = sessionOrigin.x + column.x + 30; e.y = sessionOrigin.y + rig.ctx.theme.metrics.sessionHeaderHeight + 10;
    rig.root->dispatchMouseDown(e); rig.root->dispatchMouseUp(e);
    REQUIRE(target.slots[0].clip); CHECK(target.slots[0].clip->colorIndex == target.colorIndex);
}

TEST_CASE("editor piano playhead: follows the Arrangement inside the clip, wraps its loop and shows a playing Session slot")
{
    EditorRig rig; auto* roll = widget<ui::PianoRoll>(*rig.root); REQUIRE(roll);
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    clip.loop.loopStart = 0; clip.loop.loopEnd = 4; clip.loop.startRelative = 0; clip.loop.enabled = true;
    clip.currentEnd = clip.currentStart + 16;
    engine::Transport transport; rig.ctx.transport = &transport;
    auto playAt = [&](double beat) { transport.setPosition(beat); transport.playNow(); transport.beginBlock(); };
    transport.setPosition(clip.currentStart + 1.5); transport.beginBlock();
    CHECK(roll->playheadBeat() < 0); // stopped: no playhead
    playAt(clip.currentStart + 1.5); CHECK_NEAR(roll->playheadBeat(), 1.5, 1e-9);
    playAt(clip.currentStart + 9.25); CHECK_NEAR(roll->playheadBeat(), 1.25, 1e-9); // third pass of the 4-beat loop
    playAt(clip.currentEnd + 1); CHECK(roll->playheadBeat() < 0);                   // past the clip
    playAt(clip.currentStart + 2); roll->tick(); rig.render();                      // paints without tripping anything

    // A Session clip plays on its own clock: the slot's elapsed beats, only while that slot is the one playing.
    model::ClipSlot slot; slot.clip = clip; slot.clip->uid = 9001; rig.song.tracks[0].slots.push_back(slot);
    rig.ctx.selectedClipScene = (int) rig.song.tracks[0].slots.size() - 1; rig.ctx.selectedClipUid = 9001;
    rig.ctx.controls = std::make_shared<engine::ControlBank>(4);
    auto& controls = (*rig.ctx.controls)[0];
    controls.sessionElapsed.store(6.5);
    CHECK(roll->playheadBeat() < 0); // another slot (none) is playing
    controls.playingSlot.store(rig.ctx.selectedClipScene); CHECK_NEAR(roll->playheadBeat(), 2.5, 1e-9);
}

TEST_CASE("editor piano note names: shown centred inside notes only once rows are zoomed in")
{
    EditorRig rig; auto* roll = widget<ui::PianoRoll>(*rig.root); REQUIRE(roll);
    auto& clip = rig.song.tracks[0].arrangementClips[0];
    model::MidiNote note; note.start = 0; note.duration = 2; note.pitch = 60; note.velocity = 100; clip.notes = {note};
    clip.colorIndex = 0;
    auto inkInside = [&](float rows) {
        roll->view.rowHeight = rows; roll->view.topPitch = 62;
        juce::TemporaryFile file(".png"); REQUIRE(gfx::renderToPng(*rig.root, 1200, 800, 1, file.getFile().getFullPathName().toStdString()));
        auto png = juce::ImageFileFormat::loadFrom(file.getFile());
        const auto origin = roll->boundsInRoot(); const auto area = roll->noteAreaBounds();
        const float x0 = origin.x + area.x + (float) ((note.start - roll->view.scrollBeats) * roll->view.pixelsPerBeat);
        const float x1 = x0 + (float) (note.duration * roll->view.pixelsPerBeat);
        const float y = origin.y + area.y + (float) (roll->view.topPitch - 60) * rows + rows / 2;
        // Pixels that differ from the note's fill in its middle third: the text.
        const auto fill = png.getPixelAt((int) x0 + 3, (int) y);
        int ink = 0;
        for (int x = (int) (x0 + (x1 - x0) / 3); x < (int) (x1 - (x1 - x0) / 3); ++x)
            if (png.getPixelAt(x, (int) y) != fill) ++ink;
        return ink;
    };
    CHECK(inkInside(11) == 0); // default zoom: plain notes
    CHECK(inkInside(22) > 0);  // zoomed: "C3" in the middle
}
