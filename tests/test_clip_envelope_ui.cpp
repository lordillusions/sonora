#include "TestFramework.h"
#include "engine/Automation.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/views/ClipEnvelopeEditor.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"
#include <juce_gui_basics/juce_gui_basics.h>
using namespace sonora;
namespace
{
ui::ClipEnvelopeEditor* editor(ui::Widget& root)
{
    if (auto* found = dynamic_cast<ui::ClipEnvelopeEditor*>(&root)) return found;
    for (auto& child : root.children()) if (auto* found = editor(*child)) return found;
    return nullptr;
}
struct EnvelopeUi
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext context;
    ui::MainLayout root{context};
    EnvelopeUi()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml")); REQUIRE(loaded); song = *loaded.song;
        context.song = &song; context.browserVisible = false; context.ioVisible = false;
        context.detailTab = ui::DetailTab::Clip;
        context.selectedClipTrack = context.selectedTrack = 0; context.selectedClipUid = song.tracks[0].arrangementClips[0].uid;
        context.applyEdit = [&](model::EditCommand command) { history.perform(song, std::move(command)); root.layout(); };
        context.undo = [&] { history.undo(song); root.layout(); };
        context.redo = [&] { history.redo(song); root.layout(); };
        root.setBounds({0, 0, 1200, 700}); root.layout();
        // Actual header gesture enters envelope view, then a first paint frames the clip.
        ui::DetailView* detail = nullptr;
        for (const auto& child : root.children()) if (auto* found = dynamic_cast<ui::DetailView*>(child.get())) detail = found;
        REQUIRE(detail); const auto bounds = detail->boundsInRoot(), tab = detail->noteTabBounds(1);
        ui::MouseEvent event; event.x = bounds.x + tab.centreX(); event.y = bounds.y + tab.centreY(); root.dispatchMouseDown(event); root.dispatchMouseUp(event);
        REQUIRE(context.clipEnvelopesVisible); paint(); REQUIRE(editor(root)); REQUIRE(editor(root)->visible);
    }
    void paint()
    {
        juce::TemporaryFile png(".png"); REQUIRE(gfx::renderToPng(root, 1200, 700, 1, png.getFile().getFullPathName().toStdString()));
    }
    ui::MouseEvent at(double beat, double value, int clicks = 1)
    {
        auto* view = editor(root); const auto origin = view->boundsInRoot(), plot = view->plot();
        const double min = context.clipEnvelopeController == 0 ? -8192 : 0, max = context.clipEnvelopeController == 0 ? 8191 : 127;
        ui::MouseEvent event; event.x = origin.x + plot.x + (float) ((beat - view->scrollBeats) * view->pixelsPerBeat);
        event.y = origin.y + plot.bottom() - 2 - (float) ((value - min) / (max - min)) * (plot.h - 4);
        event.clicks = clicks; return event;
    }
    void click(ui::MouseEvent event) { root.dispatchMouseDown(event); root.dispatchMouseUp(event); }
    void choose(const std::string& text)
    {
        auto* view = editor(root); const auto origin = view->boundsInRoot(), box = view->chooser();
        ui::MouseEvent event; event.x = origin.x + box.x + 10; event.y = origin.y + box.y + 10; click(event);
        for (char c : text) { ui::KeyEvent key; key.textChar = (uint32_t) c; REQUIRE(root.dispatchKey(key)); }
        ui::KeyEvent enter; enter.keyCode = juce::KeyPress::returnKey; REQUIRE(root.dispatchKey(enter)); paint();
    }
    const model::AutomationEnvelope& lane(int index)
    {
        for (const auto& env : context.selectedClip()->envelopes) if (env.midiController == index) return env;
        throw test::RequireFailed{};
    }
};
}

TEST_CASE("clip envelope UI: chooser add drag delete and undo use real input without changing notes")
{
    EnvelopeUi ui; const auto notes = ui.context.selectedClip()->notes;
    ui.choose("modulation"); CHECK(ui.context.clipEnvelopeController == 3);
    ui.click(ui.at(1, 50, 2)); REQUIRE(ui.lane(3).points.size() == 3);
    CHECK_NEAR(ui.lane(3).points.back().value, 50, 1e-9);
    ui.root.dispatchMouseDown(ui.at(1, 50)); ui.root.dispatchMouseDrag(ui.at(2, 100)); ui.root.dispatchMouseUp(ui.at(2, 100));
    CHECK_NEAR(ui.lane(3).points.back().time, 2, 1e-9); CHECK_NEAR(ui.lane(3).points.back().value, 100, 1e-9);
    ui::KeyEvent remove; remove.keyCode = juce::KeyPress::deleteKey; REQUIRE(ui.root.dispatchKey(remove));
    CHECK(ui.context.selectedClip()->notes.size() == notes.size()); CHECK(ui.lane(3).points.size() == 2);
    REQUIRE(ui.history.undo(ui.song)); CHECK_NEAR(ui.lane(3).points.back().value, 100, 1e-9);
    ui.choose("pitch"); ui.click(ui.at(1, -8192, 2));
    CHECK_NEAR(ui.lane(0).points.back().value, -8192, 1e-9);
    ui.choose("pressure"); ui.click(ui.at(1, 127, 2)); CHECK_NEAR(ui.lane(1).points.back().value, 127, 1e-9);
}

TEST_CASE("clip envelope UI: draw steps preserves original left limits and values outside the painted cells")
{
    EnvelopeUi ui; ui.choose("sustain");
    model::AutomationEnvelope env; env.midiController = 66;
    env.points = {{-63072000, 0}, {1, 0}, {1, 30}, {2, 30}, {2, 60}, {3, 60}, {3, 90}};
    ui.history.perform(ui.song, model::setClipMidiEnvelope(ui.song, ui.context.selectedClipLocation(), env)); ui.paint();
    ui.click(ui.at(1.5, 30)); ui::KeyEvent draw; draw.textChar = 'b'; REQUIRE(ui.root.dispatchKey(draw));
    // A coalesced gesture paints multiple cells in one undo step, including a right-edge jump.
    const int version = ui.history.version();
    ui.root.dispatchMouseDown(ui.at(1, 127)); ui.root.dispatchMouseDrag(ui.at(1.99, 127)); ui.root.dispatchMouseUp(ui.at(1.99, 127));
    CHECK(ui.history.version() == version + 1);
    const engine::AutomationLane result(ui.lane(66));
    CHECK_NEAR(result.valueAt(0.999), 0, 1e-9); CHECK_NEAR(result.valueAt(1), 127, 1e-9);
    CHECK_NEAR(result.valueAt(1.99), 127, 1e-9); CHECK_NEAR(result.valueAt(2), 60, 1e-9); CHECK_NEAR(result.valueAt(3), 90, 1e-9);
    REQUIRE(ui.history.undo(ui.song)); CHECK_NEAR(engine::AutomationLane(ui.lane(66)).valueAt(1.5), 30, 1e-9);
    REQUIRE(ui.history.redo(ui.song)); CHECK_NEAR(engine::AutomationLane(ui.lane(66)).valueAt(1.5), 127, 1e-9);
}

TEST_CASE("clip envelope UI: interrupted gesture and losing focus do not overwrite external edits or claim clip Delete")
{
    EnvelopeUi ui; ui.choose("volume"); ui.click(ui.at(1, 100, 2));
    ui.root.dispatchMouseDown(ui.at(1, 100)); ui.root.dispatchMouseDrag(ui.at(2, 20));
    REQUIRE(ui.history.undo(ui.song)); const int version = ui.history.version();
    ui.root.dispatchMouseUp(ui.at(2, 20)); CHECK(ui.history.version() == version);
    CHECK(ui.context.selectedClip()->envelopes.empty());
    auto* view = editor(ui.root); view->focusLost(); ui::KeyEvent remove; remove.keyCode = juce::KeyPress::deleteKey;
    CHECK(! view->keyPressed(remove));
}

TEST_CASE("clip envelope UI: the shared editor targets Session and take clips independently through history")
{
    for (bool take : {false, true})
    {
        EnvelopeUi ui;
        auto copy = ui.song.tracks[0].arrangementClips[0];
        if (take)
        {
            model::TakeLane source; source.id = 9; source.name = "controller take";
            copy.uid = model::allocateClipUid(); source.clips = {copy}; ui.song.tracks[0].takeLanes = {source};
            ui.context.selectedClipUid = copy.uid; ui.context.selectedTakeLane = 9;
        }
        else
        {
            model::setSessionClip(ui.song, 0, 1, copy).apply(ui.song);
            ui.context.selectedClipScene = 1; ui.context.selectedClipUid = ui.song.tracks[0].slots[1].clip->uid;
        }
        ui.root.layout(); ui.paint(); ui.choose("expression");
        ui.click(ui.at(2, 65, 2)); CHECK_NEAR(ui.lane(13).points.back().value, 65, 1e-9);
        CHECK(ui.song.tracks[0].arrangementClips[0].envelopes.empty());
        const int target = ui.lane(13).targetId;
        REQUIRE(ui.history.undo(ui.song)); CHECK(ui.context.selectedClip()->envelopes.empty());
        REQUIRE(ui.history.redo(ui.song)); CHECK(ui.lane(13).targetId == target);
        CHECK(ui.context.selectedClip()->notes.size() == copy.notes.size());
        ui.choose("127"); CHECK(ui.context.clipEnvelopeController == 129);
        ui.click(ui.at(1, 127, 2)); CHECK_NEAR(ui.lane(129).points.back().value, 127, 1e-9);
    }
}
