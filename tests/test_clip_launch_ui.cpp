#include "TestFramework.h"
#include "engine/ClipLauncher.h"
#include "engine/ControlRouter.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "midi/MappingStore.h"
#include "midi/MidiManager.h"
#include "ui/views/MainLayout.h"
#include "ui/views/SessionView.h"

using namespace sonora;
namespace
{
struct ClipMapUi
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    engine::Transport transport;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    std::vector<midi::ControlChange> changes;
    ClipMapUi()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
        song = std::move(*loaded.song); song.globalQuantisation = 0;
        song.tracks[0].slots[1].clip->launchMode = 1;
        devices::BuildContext context; graph = engine::buildGraph(song, context, samples).graph;
        ctx.song = &song; ctx.controls = graph->controls; ctx.graph = graph.get(); ctx.transport = &transport; ctx.midi = &midi;
        ctx.browserVisible = ctx.detailVisible = ctx.ioVisible = false; ctx.mainView = ui::MainView::Session;
        ctx.mappedControl = [&](const auto& target, const auto& change) {
            changes.push_back(change);
            if (ctx.song) engine::applySessionClipControl(target, change, *ctx.song, graph.get(), transport);
        };
        midi.onControl = ctx.mappedControl;
        midi.postToMessageThread = [](std::function<void()> f) { f(); };
        root = std::make_unique<ui::MainLayout>(ctx); root->setBounds({0, 0, 1200, 700}); root->layout();
    }
    void clickClip(bool right = false)
    {
        ui::MouseEvent e; const auto& m = ctx.theme.metrics;
        ui::SessionView* session = nullptr;
        for (const auto& child : root->children())
            if (auto* view = dynamic_cast<ui::SessionView*>(child.get())) session = view;
        REQUIRE(session);
        const auto origin = session->boundsInRoot();
        e.x = origin.x + 8;
        e.y = origin.y + m.sessionHeaderHeight + 1 + m.sessionSlotHeight * 1.5f; e.right = right;
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
    bool key(int code, bool command = false)
    {
        ui::KeyEvent event; event.keyCode = code; event.textChar = (uint32_t) code; event.cmd = command;
        return root->dispatchKey(event);
    }
    void assignKey()
    {
        CHECK(key('K', true)); CHECK(ctx.keyMapMode); clickClip();
        REQUIRE(ctx.mapSelection); CHECK(key('J')); CHECK(key('K', true)); CHECK(! ctx.keyMapMode);
    }
};
}

TEST_CASE("clip mapping UI: real MIDI map click learns a painted Session slot without launching it")
{
    ClipMapUi rig;
    CHECK(rig.key('M', true)); CHECK(rig.ctx.midiMapMode);
    rig.clickClip(); REQUIRE(rig.ctx.mapSelection);
    CHECK(rig.ctx.mapSelection->kind == midi::ControlTarget::Kind::SessionClip);
    CHECK(rig.midi.isLearning()); CHECK(rig.changes.empty());
    rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 64), false, true);
    rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), false, true);
    CHECK(rig.changes.empty()); REQUIRE(rig.midi.mappings().size() == 1);
    CHECK(rig.key('M', true));
    rig.midi.handleMessage(juce::MidiMessage::noteOn(2, 40, (juce::uint8) 64), true, true);
    REQUIRE(rig.changes.size() == 1); CHECK_NEAR(rig.changes[0].value, 64.0 / 127, 1e-7);
    CHECK((*rig.graph->controls)[0].queuedSlot.load() == 1);
    rig.midi.handleMessage(juce::MidiMessage::noteOff(2, 40), true, true);
    REQUIRE(rig.changes.size() == 2); CHECK(rig.changes[1].released);
    CHECK((*rig.graph->controls)[0].queuedSlot.load() == engine::TrackControls::kQueuedStop);
}

TEST_CASE("clip mapping UI: computer key learn, auto-repeat, release and removal coexist with MIDI mappings")
{
    ClipMapUi rig;
    midi::Mapping mapping; mapping.isNote = true; mapping.number = 40;
    mapping.target = *midi::sessionClipTarget(rig.song, 0, 1); rig.midi.addMapping(mapping);
    rig.assignKey(); REQUIRE(rig.midi.mappings().size() == 2); CHECK(rig.changes.empty());
    CHECK(rig.key('J')); CHECK(rig.key('J')); REQUIRE(rig.changes.size() == 1);
    CHECK(rig.changes[0].trigger); CHECK_NEAR(rig.changes[0].value, 1, 0);
    CHECK(rig.root->dispatchKeyUp('J')); REQUIRE(rig.changes.size() == 2); CHECK(rig.changes.back().released);
    CHECK(! rig.root->dispatchKeyUp('J'));
    CHECK(rig.key('K', true)); rig.clickClip(true);
    REQUIRE(rig.midi.mappings().size() == 1); CHECK(rig.midi.mappings()[0].keyCode == 0);
    CHECK(rig.key('K', true));
    CHECK(! rig.key('J')); CHECK(rig.changes.size() == 2);
}

TEST_CASE("clip mapping UI: typing and modifiers take priority, focus/view/document changes release held keys")
{
    ClipMapUi rig;
    rig.ctx.browserVisible = true; rig.root->layout();
    CHECK(rig.key('F', true));
    rig.assignKey(); // clicking a mapping target also leaves the prior search focus
    CHECK(rig.key('J')); REQUIRE(rig.changes.size() == 1);
    CHECK(rig.root->dispatchKeyUp('J')); REQUIRE(rig.changes.size() == 2);
    rig.changes.clear();
    CHECK(rig.key('F', true)); CHECK(rig.key('J')); CHECK(rig.changes.empty()); // browser search text
    CHECK(rig.key(juce::KeyPress::escapeKey));
    ui::KeyEvent modified; modified.keyCode = 'J'; modified.textChar = 'J'; modified.ctrl = true;
    CHECK(! rig.root->dispatchKey(modified)); CHECK(rig.changes.empty());
    CHECK(rig.key('J')); REQUIRE(rig.changes.size() == 1);
    gfx::SkiaCanvasComponent canvas(*rig.root);
    canvas.focusLost(juce::Component::focusChangedDirectly);
    REQUIRE(rig.changes.size() == 2); CHECK(rig.changes.back().released);
    CHECK(rig.key('J')); CHECK(rig.key(juce::KeyPress::tabKey));
    CHECK(rig.ctx.mainView == ui::MainView::Arrangement);
    REQUIRE(rig.changes.size() == 4); CHECK(rig.changes.back().released);
    CHECK(! rig.key('J')); CHECK(rig.changes.size() == 4);
    CHECK(rig.key(juce::KeyPress::tabKey)); CHECK(rig.key('J'));
    rig.ctx.song = nullptr; rig.root->layout();
    REQUIRE(rig.changes.size() == 6); CHECK(rig.changes.back().released);
    CHECK(rig.root->heldLaunchKeys().empty()); CHECK(! rig.ctx.mapSelection);
}
