// PLG-21: the Sonora Phase Align face, in the VST3 window (kick on the sidechain bus) and in the app's device chain on a
// bass track whose sidechain listens to the kick track.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/PhaseAlign.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/PhaseAlignPlugin.h"
#include "plugin/PhaseAlignPluginEditor.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PhaseAlignFace.h"
#include "ui/widgets/Buttons.h"

#include <cmath>

using namespace sonora;
namespace
{
using P = devices::PhaseAlign;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}

ui::MouseEvent at(ui::Widget& widget, float x, float y)
{
    ui::MouseEvent e;
    const auto origin = widget.boundsInRoot();
    e.x = origin.x + x;
    e.y = origin.y + y;
    return e;
}

float kickAt(int s)
{
    const double t = (double) s / kRate, tk = (double) (s % 24000) / kRate;
    return (float) (0.8 * std::exp(-tk / 0.12) * std::sin(2 * kPi * 55.0 * t));
}
float bassAt(int s) { return (float) (0.4 * std::sin(2 * kPi * 55.0 * (double) s / kRate + kPi * 0.8)); }
} // namespace

TEST_CASE("phase align plugin: sidechain bus, host parameters, state, window")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::PhaseAlignPlugin p;
    CHECK(p.getParameters().size() == P::NumParams);
    CHECK(p.getBusCount(true) == 2);
    CHECK(p.getParameters()[P::ModeParam]->getCurrentValueAsText() == "Auto");
    p.prepareToPlay(kRate, 512);
    CHECK(p.getLatencySamples() == 0);

    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::PhaseAlignPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, plugin::PhaseAlignView::kWidth, plugin::PhaseAlignView::kHeight});
    view.layout();
    auto& face = view.face();

    // Main bus = bass (channels 0-1), sidechain bus = kick (channels 2-3), 6 hits.
    juce::AudioBuffer<float> block(4, 512);
    juce::MidiBuffer midi;
    for (int start = 0, i = 0; start < 6 * 24000; start += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
        {
            block.setSample(0, s, bassAt(start + s));
            block.setSample(1, s, bassAt(start + s));
            block.setSample(2, s, kickAt(start + s));
            block.setSample(3, s, kickAt(start + s));
        }
        p.processBlock(block, midi);
        if (i % 4 == 0) view.tickTree();
    }
    view.tickTree();
    CHECK(p.device().hitCount() >= 5u);
    CHECK(p.device().sidechainPresent());
    CHECK(p.device().lastCoherence() > 0.8f);
    CHECK(face.shape().bins > 100);
    CHECK(face.sidechainButton()->label() == "Kick: host sidechain"); // the VST3 shows no chooser: the host routes it

    // Mode buttons.
    std::vector<ui::TextButton*> buttons;
    collect(face, buttons);
    for (auto* b : buttons)
        if (b->label() == "Manual")
        {
            view.dispatchMouseDown(at(*b, 5, 5));
            view.dispatchMouseUp(at(*b, 5, 5));
        }
    CHECK(std::lround(p.device().getParameter(P::ModeParam)) == P::Manual);
    p.device().setParameter(P::ModeParam, (float) P::Auto);

    juce::MemoryBlock state;
    p.device().setParameter(P::Crossover, 110.0f);
    p.getStateInformation(state);
    plugin::PhaseAlignPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK_NEAR(restored.device().getParameter(P::Crossover), 110.0, 1e-3);
    p.device().setParameter(P::Crossover, 150.0f);

    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_phase_align_plugin.png");
    CHECK(gfx::renderToPng(view, (int) plugin::PhaseAlignView::kWidth, (int) plugin::PhaseAlignView::kHeight, 2.0f,
                           png.getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("phase align: the face in the app's chain names the kick track and its button opens the chooser")
{
    model::Song song;
    song.tracks.resize(2);
    song.tracks[0].id = 4;
    song.tracks[0].name = "Kick";
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[1].id = 5;
    song.tracks[1].name = "Bass";
    song.tracks[1].kind = model::TrackKind::Audio;
    auto device = devices::nativeDeviceModel("SonoraPhaseAlign");
    device.sidechain.on = true;
    device.sidechain.sourceTrackId = 4;
    song.tracks[1].devices = {device};
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    graph.graph->prepare(kRate, 512);
    REQUIRE(graph.graph->nodes[1].sidechains.size() == 1);
    ui::UiContext context;
    context.song = &song;
    context.graph = graph.graph.get();
    context.controls = graph.graph->controls;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    context.selectedTrack = 1;
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1400, 900});
    root.layout();
    std::vector<ui::PhaseAlignFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto* phase = dynamic_cast<P*>(graph.graph->nodes[1].processorForModelDevice[0]);
    REQUIRE(phase != nullptr);
    // Feed it through the device with the kick as its sidechain, as the engine does.
    juce::AudioBuffer<float> bass(2, 512), kick(2, 512);
    juce::MidiBuffer midi;
    for (int start = 0, i = 0; start < 5 * 24000; start += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
            for (int ch = 0; ch < 2; ++ch) { bass.setSample(ch, s, bassAt(start + s)); kick.setSample(ch, s, kickAt(start + s)); }
        phase->setSidechain(&kick, 1.0f);
        phase->process(bass, midi);
        if (i % 4 == 0) root.tickTree();
    }
    phase->setSidechain(nullptr, 1.0f);
    root.tickTree();
    CHECK(faces[0]->sidechainButton()->label() == "Kick: Kick");
    CHECK(faces[0]->onChooseSidechain != nullptr);
    CHECK(phase->hitCount() >= 4u);
    int mapped = 0;
    std::vector<ui::Widget*> all;
    collect(*faces[0], all);
    for (auto* w : all) mapped += w->midiTarget.has_value() ? 1 : 0;
    CHECK(mapped >= 9);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_phase_align_chain.png");
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, png.getFullPathName().toStdString()));
}
