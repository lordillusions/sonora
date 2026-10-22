#include "TestFramework.h"
#include "core/Edit.h"
#include "core/TimeUtils.h"
#include "engine/Transport.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "ui/AudioClipEdits.h"
#include "ui/RootWidget.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/ClipControls.h"
#include "ui/views/MainLayout.h"
#include "ui/views/TopBar.h"
using namespace sonora;
namespace
{
template<class T> T* find(ui::Widget& root)
{
    if (auto* value = dynamic_cast<T*>(&root)) return value;
    for (const auto& child : root.children()) if (auto* value = find<T>(*child)) return value;
    return nullptr;
}
struct WarpRig
{
    juce::TemporaryFile file{".als"};
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    engine::Transport transport;
    ui::UiContext ctx;
    WarpRig()
    {
        std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(), file.getFile(), error));
        auto result = io::loadAls(file.getFile()); REQUIRE(result); song = std::move(result.song);
        ctx.song = song.get(); ctx.transport = &transport; ctx.browserVisible = ctx.detailVisible = false;
        ctx.applyEdit = [&](model::EditCommand edit) {
            history.perform(*song, std::move(edit)); transport.setTimeSignature(song->timeSigNumerator, song->timeSigDenominator);
        };
    }
};
void click(ui::RootWidget& root, ui::Rect box, int count = 1)
{
    ui::MouseEvent e; e.x = box.centreX(); e.y = box.centreY(); e.clicks = count;
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
}
void type(ui::RootWidget& root, const std::string& text)
{
    for (char ch : text) REQUIRE(root.dispatchKey({.textChar = (uint32_t) ch}));
    REQUIRE(root.dispatchKey({.keyCode = juce::KeyPress::returnKey}));
}
}
TEST_CASE("transport editing: signature typing drag wheel save and undo update the meter")
{
    WarpRig rig; ui::MainLayout root(rig.ctx); root.setBounds({0, 0, 1300, 800}); root.layout();
    auto* top = find<ui::TopBar>(root); REQUIRE(top); const auto origin = top->boundsInRoot();
    auto box = top->timeSignatureBounds().translated(origin.x, origin.y);
    click(root, {box.x, box.y, box.w * 0.45f, box.h}, 2); type(root, "7/8");
    CHECK(rig.song->timeSigNumerator == 7); CHECK(rig.song->timeSigDenominator == 8); CHECK(rig.transport.timeSigNumerator() == 7);
    const auto save = rig.file.getFile().getSiblingFile("signature-" + juce::Uuid().toString() + ".als");
    REQUIRE(io::saveAls(*rig.song, save).ok); auto reopened = io::loadAls(save); REQUIRE(reopened);
    CHECK(reopened.song->timeSigNumerator == 7); CHECK(reopened.song->timeSigDenominator == 8); save.deleteFile();
    REQUIRE(rig.history.undo(*rig.song)); CHECK(rig.song->timeSigNumerator == 4); CHECK(rig.song->timeSigDenominator == 4);
    ui::MouseEvent e; e.x = box.right() - 6; e.y = box.centreY(); e.wheelDeltaY = 0.05f;
    root.dispatchMouseWheel(e); CHECK(rig.song->timeSigDenominator == 8);
    e.x = box.x + 6; e.wheelDeltaY = 0; root.dispatchMouseDown(e); e.y -= 18; root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(rig.song->timeSigNumerator == 7); REQUIRE(rig.history.undo(*rig.song)); CHECK(rig.song->timeSigNumerator == 4);
    click(root, {box.x, box.y, box.w * 0.45f, box.h}, 2); type(root, "4/3"); CHECK(rig.song->timeSigDenominator == 8);
}
TEST_CASE("arrangement zoom: Option scroll and keys resize selected lanes while waveform zoom re-enables")
{
    WarpRig rig; ui::MainLayout root(rig.ctx); root.setBounds({0, 0, 1300, 800}); root.layout();
    auto* view = find<ui::ArrangementView>(root); REQUIRE(view); const auto origin = view->boundsInRoot();
    const auto lane = view->lanes()[0];
    ui::MouseEvent e; e.x = origin.x + 100; e.y = origin.y + lane.y + 25; e.alt = true; e.wheelDeltaY = 0.1f;
    root.dispatchMouseWheel(e); CHECK(rig.ctx.laneHeights[0] > lane.h); CHECK_NEAR(rig.ctx.scrollBeats, 0, 0);
    rig.ctx.arrangementSelection = {true, 0, 4, 0, 1}; const float first = rig.ctx.laneHeights[0];
    e.wheelDeltaY = 0.1f; root.dispatchMouseWheel(e); CHECK(rig.ctx.laneHeights[0] > first); CHECK(rig.ctx.laneHeights[1] > 87);
    rig.ctx.arrangementSelection = {}; root.setKeyboardFocus(view);
    const float before = rig.ctx.laneHeights[0]; REQUIRE(root.dispatchKey({.keyCode = '+', .textChar = '+', .alt = true})); CHECK(rig.ctx.laneHeights[0] > before);
    rig.ctx.waveformZoomEnabled = false; rig.ctx.waveformZoom = 1;
    e.x = origin.right() - 90; e.y = origin.bottom() - 8; e.alt = false; e.wheelDeltaY = 0.1f;
    root.dispatchMouseWheel(e); CHECK(rig.ctx.waveformZoomEnabled); CHECK(rig.ctx.waveformZoom > 1);
}
TEST_CASE("warp BPM editing: text and wheel use tenths and save the detected tempo across toggles")
{
    WarpRig rig; model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.currentStart = 8; clip.currentEnd = 12;
    clip.sampleRate = 48000; clip.lengthSamples = 96000; clip.loop = {0.25, 1.75, 0.125, false};
    clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
    auto warped = ui::audioedit::enableWarpWithTempo(clip, 120, 100); CHECK(warped.tempoAnalyzed); CHECK(warped.warped);
    CHECK_NEAR(ui::audioedit::clipBpm(warped, 120), 100, 1e-8);
    CHECK_NEAR(warped.loop.loopStart, 0.25 * 100 / 60, 1e-8); CHECK_NEAR(warped.currentStart, 8, 0);
    model::insertClip(2, warped, "Audio").apply(*rig.song);
    rig.ctx.selectedTrack = rig.ctx.selectedClipTrack = 2; rig.ctx.selectedClipUid = rig.song->tracks[2].arrangementClips[0].uid;
    ui::RootWidget root(rig.ctx); auto* panel = root.add<ui::ClipControls>(); root.setBounds({0, 0, 400, 850}); panel->setBounds({0, 0, 340, 850});
    panel->columnsFor(850); const auto bpmBox = panel->controlBounds(ui::ClipControls::Id::Bpm); REQUIRE(! bpmBox.isEmpty());
    int requests = 0; rig.ctx.toggleClipWarp = [&](const model::ClipLocation& location) { ++requests; CHECK(location.uid == rig.ctx.selectedClipUid); };
    click(root, panel->controlBounds(ui::ClipControls::Id::Warp)); CHECK(requests == 1); CHECK(rig.ctx.selectedClip()->warped);
    rig.ctx.toggleClipWarp = {};
    click(root, bpmBox, 2); type(root, "127.34"); CHECK_NEAR(ui::audioedit::clipBpm(*rig.ctx.selectedClip(), 120), 127.3, 1e-8);
    ui::MouseEvent e; e.x = bpmBox.centreX(); e.y = bpmBox.centreY(); e.wheelDeltaY = 0.002f;
    root.dispatchMouseWheel(e); CHECK_NEAR(ui::audioedit::clipBpm(*rig.ctx.selectedClip(), 120), 127.4, 1e-8);
    e.wheelDeltaY = -0.08f; root.dispatchMouseWheel(e); CHECK_NEAR(ui::audioedit::clipBpm(*rig.ctx.selectedClip(), 120), 127.3, 1e-8);
    const auto beforeCancel = rig.history.version(); click(root, bpmBox, 2); REQUIRE(root.dispatchKey({.textChar = '1'}));
    REQUIRE(root.dispatchKey({.keyCode = juce::KeyPress::escapeKey})); CHECK(rig.history.version() == beforeCancel);
    auto off = ui::audioedit::toggleWarp(*rig.ctx.selectedClip(), 120); CHECK(off.tempoAnalyzed);
    rig.ctx.applyEdit(model::modifyClip(*rig.song, rig.ctx.selectedClipLocation(), off, "Warp Off"));
    REQUIRE(io::saveAls(*rig.song, rig.file.getFile()).ok); auto saved = io::loadAls(rig.file.getFile()); REQUIRE(saved);
    const auto& stored = saved.song->tracks[2].arrangementClips[0]; CHECK(stored.tempoAnalyzed); CHECK(! stored.warped);
    const auto on = ui::audioedit::enableWarpWithTempo(stored, 120, 90); CHECK_NEAR(ui::audioedit::clipBpm(on, 120), 127.3, 1e-7);
}

TEST_CASE("transport editing: static signature automation follows manual changes and undo exactly")
{
    model::Song song; song.main.mixer.timeSignatureTargetId = 55;
    model::AutomationEnvelope base; base.targetId = 55; base.points = {{-63072000, 201}};
    auto dynamic = base; dynamic.targetId = 56; dynamic.points.push_back({8, 102});
    song.main.automation = {base, dynamic}; model::EditHistory history;
    history.perform(song, model::setTimeSignature(song, 7, 8));
    CHECK_NEAR(song.main.automation[0].points[0].value, 303, 0); CHECK_NEAR(song.main.automation[1].points[0].value, 201, 0);
    REQUIRE(history.undo(song)); CHECK_NEAR(song.main.automation[0].points[0].value, 201, 0);
    CHECK(! model::setTimeSignature(song, 4, 3).apply); CHECK(! model::setTimeSignature(song, 4, 4).apply);
}
