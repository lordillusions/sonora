// UI-83: the Sonora Curve LFO and Sonora Prism fold to a compact live view in the device chain; the title arrow toggles.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveLfo.h"
#include "devices/native/WavetableSynth.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "midi/MidiManager.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/views/MainLayout.h"
#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetableMiniView.h"
#include "ui/widgets/Buttons.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;
namespace
{
using Key = ui::UiContext::WavetableKey;
using L = devices::CurveLfo;
using W = devices::WavetableSynth;

template <typename T> void collect(ui::Widget& widget, std::vector<T*>& result)
{
    if (auto* found = dynamic_cast<T*>(&widget)) result.push_back(found);
    for (const auto& child : widget.children()) collect(*child, result);
}
template <typename T> std::vector<T*> all(ui::Widget& root)
{
    std::vector<T*> result;
    collect(root, result);
    return result;
}
template <typename T> T* one(ui::Widget& root)
{
    const auto found = all<T>(root);
    return found.size() == 1 ? found[0] : nullptr;
}
// The title-bar icon of the device box holding `face` (the box is the face's parent).
ui::IconButton* titleIcon(ui::Widget* face, ui::IconButton::Icon kind)
{
    if (face == nullptr || face->parent() == nullptr) return nullptr;
    for (const auto& child : face->parent()->children())
        if (auto* b = dynamic_cast<ui::IconButton*>(child.get()); b && b->visible && b->icon() == kind) return b;
    return nullptr;
}
float boxWidth(ui::Widget* face) { return face && face->parent() ? face->parent()->width() : 0.0f; }
void click(ui::RootWidget& root, ui::Widget* widget)
{
    REQUIRE(widget != nullptr);
    const auto r = widget->boundsInRoot();
    ui::MouseEvent e; e.x = r.centreX(); e.y = r.centreY();
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
}

// One MIDI track: Sonora Prism, then Sonora Curve LFO. The app's defaults: Prism floats (its mini view in the chain).
struct MiniRig
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    std::vector<Key> opened;

    MiniRig()
    {
        song.tracks.resize(1);
        auto& track = song.tracks[0];
        track.kind = model::TrackKind::Midi; track.id = 10; track.name = "Lead";
        track.devices = {devices::nativeDeviceModel("SonoraWavetable"), devices::nativeDeviceModel("SonoraCurveLfo")};
        track.devices[0].id = 42; track.devices[1].id = 43;
        rebuildGraph();
        context.midi = &midi; context.browserVisible = false; context.detailTab = ui::DetailTab::Devices;
        context.wavetableFloatingDefault = true;
        context.openWavetableWindow = [this](const Key& key) { opened.push_back(key); };
        context.closeWavetableWindow = [](const Key&) {};
        context.setDeviceViewValue = [this](int t, int device, int, int, const std::string& key, double value) {
            song.tracks[(size_t) t].devices[(size_t) device].plain[key] = value;
        };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1600, 900}); root->layout();
    }
    void rebuildGraph()
    {
        context.graph = nullptr;
        devices::BuildContext build; graph = engine::buildGraph(song, build, samples).graph;
        REQUIRE(graph); graph->prepare(48000, 512);
        context.song = &song; context.graph = graph.get(); context.controls = graph->controls;
    }
    L* lfo() const { return dynamic_cast<L*>(graph->nodes[0].processorForModelDevice[1]); }
    W* prism() const { return dynamic_cast<W*>(graph->nodes[0].processorForModelDevice[0]); }
    void layout() { root->layout(); }
};
} // namespace

TEST_CASE("device mini: the Curve LFO's title arrow folds it to its graph and back; the choice survives rebuilds and saves")
{
    MiniRig rig;
    const Key key{10, 43, -1, -1};
    REQUIRE(rig.context.wavetableKey(0, 1) == key);
    auto* editor = one<ui::CurveLfoEditor>(*rig.root);
    REQUIRE(editor != nullptr);
    CHECK(all<ui::CurveLfoMini>(*rig.root).empty());
    const float fullWidth = boxWidth(editor);
    CHECK(fullWidth >= ui::CurveLfoEditor::kPreferredWidth);

    // Fold: a real click on the title arrow.
    click(*rig.root, titleIcon(editor, ui::IconButton::Icon::FoldOpen));
    rig.layout();
    CHECK(all<ui::CurveLfoEditor>(*rig.root).empty());
    auto* mini = one<ui::CurveLfoMini>(*rig.root);
    REQUIRE(mini != nullptr);
    CHECK_NEAR(boxWidth(mini), ui::CurveLfoMini::kWidth + 12, 0.01);
    CHECK(boxWidth(mini) < 260); CHECK(boxWidth(mini) < fullWidth * 0.25f);
    CHECK(rig.context.isDeviceMinimized(key));
    CHECK_NEAR(rig.song.tracks[0].devices[1].plain.at("IsExpanded"), 0, 0); // saved with the set
    CHECK(titleIcon(mini, ui::IconButton::Icon::FoldClosed) != nullptr);
    // The title name ends before the arrow.
    CHECK(titleIcon(mini, ui::IconButton::Icon::FoldClosed)->bounds().right() <= boxWidth(mini) - 50 + 0.01f);

    // A new engine graph (any device edit) rebuilds the chain: still folded.
    rig.rebuildGraph(); rig.layout();
    REQUIRE(one<ui::CurveLfoMini>(*rig.root) != nullptr);
    CHECK(all<ui::CurveLfoEditor>(*rig.root).empty());
    // An edit that gives the device a fresh id: the choice follows it.
    rig.song.tracks[0].devices[1].id = 77; rig.context.renameWavetableDevice(10, 43, 77);
    rig.rebuildGraph(); rig.layout();
    CHECK(rig.context.isDeviceMinimized(Key{10, 77, -1, -1}));
    REQUIRE(one<ui::CurveLfoMini>(*rig.root) != nullptr);
    // A loaded set has no session choices: the device's IsExpanded leaf keeps it folded.
    rig.context.deviceMinimized.clear(); ++rig.context.wavetablePlacementRevision; rig.layout();
    mini = one<ui::CurveLfoMini>(*rig.root);
    REQUIRE(mini != nullptr);

    // A click on the mini graph unfolds the whole face; the box grows back.
    click(*rig.root, mini);
    rig.layout();
    CHECK(all<ui::CurveLfoMini>(*rig.root).empty());
    editor = one<ui::CurveLfoEditor>(*rig.root);
    REQUIRE(editor != nullptr);
    CHECK_NEAR(boxWidth(editor), fullWidth, 0.01);
    CHECK(! rig.context.isDeviceMinimized(Key{10, 77, -1, -1}));
    CHECK_NEAR(rig.song.tracks[0].devices[1].plain.at("IsExpanded"), 1, 0);

    // The arrow folds again and unfolds again.
    click(*rig.root, titleIcon(editor, ui::IconButton::Icon::FoldOpen)); rig.layout();
    mini = one<ui::CurveLfoMini>(*rig.root);
    REQUIRE(mini != nullptr);
    click(*rig.root, titleIcon(mini, ui::IconButton::Icon::FoldClosed)); rig.layout();
    CHECK(one<ui::CurveLfoEditor>(*rig.root) != nullptr);
}

TEST_CASE("device mini: the Curve LFO mini graph's playhead and level follow the processor")
{
    MiniRig rig;
    rig.context.setDeviceMinimized(Key{10, 43, -1, -1}, true); rig.layout();
    auto* mini = one<ui::CurveLfoMini>(*rig.root);
    REQUIRE(mini != nullptr);
    auto* lfo = rig.lfo();
    REQUIRE(lfo != nullptr);
    lfo->setParameter(L::SelectedGraph, 2);
    const auto plot = mini->plot();
    CHECK(plot.w > 150); CHECK(plot.h > 80);
    CHECK(mini->levelBar().x >= plot.right());

    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midiBuffer;
    std::vector<float> heads;
    for (const double beat : {0.0, 0.7, 1.9, 3.1})
    {
        audio.clear();
        lfo->setHostInfo({120.0, beat, true, 48000.0});
        lfo->process(audio, midiBuffer);
        mini->tick();
        CHECK(lfo->displayRunning());
        const float position = lfo->displayPosition(2);
        CHECK_NEAR(mini->playheadX(), plot.x + position * plot.w, 0.01);
        CHECK_NEAR(mini->level(), std::clamp(lfo->graph(2).eval(position), 0.0f, 1.0f), 1e-5);
        heads.push_back(mini->playheadX());
    }
    CHECK(std::abs(heads[1] - heads[0]) > 1.0f); CHECK(std::abs(heads[2] - heads[1]) > 1.0f);
}

TEST_CASE("device mini: Prism's mini view shows the oscillators' positions and the output level; its arrow docks the dashboard")
{
    MiniRig rig;
    auto* mini = one<ui::WavetableMiniView>(*rig.root);
    REQUIRE(mini != nullptr);
    CHECK(all<ui::WavetableEditor>(*rig.root).empty());
    CHECK_NEAR(boxWidth(mini), ui::WavetableMiniView::kWidth + 12, 0.01);
    CHECK(boxWidth(mini) < 260);
    for (int o = 0; o < W::kOscillators; ++o)
    {
        CHECK(mini->oscillatorPlot(o).w > 60);
        CHECK(mini->oscillatorRow(o).bottom() <= mini->height() + 0.01f);
    }

    auto* prism = rig.prism();
    REQUIRE(prism != nullptr);
    REQUIRE(prism->tableFrameCount(0) > 1);
    prism->setParameter(W::oscParam(0, W::Position), 0); mini->tick();
    CHECK_NEAR(mini->markerPosition(0), 0, 1e-4);
    prism->setParameter(W::oscParam(0, W::Position), 1); mini->tick();
    CHECK_NEAR(mini->markerPosition(0), 1, 1e-4);

    // A note through the device lights the output meter, which then falls back to nothing.
    CHECK_NEAR(mini->meterLevel(0), 0, 0);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midiBuffer;
    REQUIRE(prism->queueUiMidi(0x90, 60, 110));
    for (int block = 0; block < 8; ++block) { audio.clear(); prism->process(audio, midiBuffer); }
    mini->tick();
    CHECK(mini->meterLevel(0) > 0.001f); CHECK(mini->meterLevel(1) > 0.001f);
    REQUIRE(prism->queueUiMidi(0x80, 60, 0));
    for (int block = 0; block < 400; ++block) { audio.clear(); prism->process(audio, midiBuffer); }
    for (int i = 0; i < 200; ++i) mini->tick();
    CHECK_NEAR(mini->meterLevel(0), 0, 0);

    // Its title arrow docks the full dashboard; the box grows to it.
    click(*rig.root, titleIcon(mini, ui::IconButton::Icon::FoldClosed));
    rig.layout();
    auto* editor = one<ui::WavetableEditor>(*rig.root);
    REQUIRE(editor != nullptr);
    CHECK(boxWidth(editor) >= ui::WavetableEditor::kPreferredWidth);
    CHECK(rig.opened.empty());
    CHECK(! rig.context.isWavetableFloating(Key{10, 42, -1, -1}));
}
