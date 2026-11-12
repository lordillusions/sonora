// PLG-21 / PLG-23: the Sonora Phase Align face (ProFace kit) in the VST3 window (sidechain bus) and in the app's chain,
// where it names the kick's track and says what it does.
#include "ProFaceTestKit.h"
#include "devices/native/PhaseAlign.h"
#include "plugin/PhaseAlignPlugin.h"
#include "plugin/PhaseAlignPluginEditor.h"
#include "ui/views/PhaseAlignFace.h"

#include <cmath>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using P = devices::PhaseAlign;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

float kickAt(int s)
{
    const double t = (double) s / kRate, tk = (double) (s % 24000) / kRate;
    return (float) (0.8 * std::exp(-tk / 0.12) * std::sin(2 * kPi * 55.0 * t));
}
float bassAt(int s) { return (float) (0.4 * std::sin(2 * kPi * 55.0 * (double) s / kRate + kPi * 0.8)); }
} // namespace

TEST_CASE("phase align plugin: sidechain bus, the verdict and the last kick in the full face, modes by click, state")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::PhaseAlignPlugin p;
    CHECK(p.getParameters().size() == P::NumParams);
    CHECK(p.getBusCount(true) == 2);
    p.prepareToPlay(kRate, 512);
    CHECK(p.getLatencySamples() == 0);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::PhaseAlignPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::PhaseAlignFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    view.tickTree();
    CHECK(face.verdictTitle() == "No kick to listen to");

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
    CHECK(p.device().verdict() == P::Aligning);
    CHECK(face.verdictTitle().find("more low end on every kick") != std::string::npos);
    CHECK(face.shape().bins > 100);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_phase_align_plugin.png").getFullPathName().toStdString()));

    REQUIRE(click(view, face, "Manual"));
    CHECK(std::lround(p.device().getParameter(P::ModeParam)) == P::Manual);
    REQUIRE(click(view, face, "Kick + Bass"));
    CHECK(std::lround(p.device().getParameter(P::Listen)) == P::ListenSum);
    REQUIRE(drag(view, face, "CROSSOVER", -30));
    CHECK(p.device().getParameter(P::Crossover) > 150.0f);

    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::PhaseAlignPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK_NEAR(restored.device().getParameter(P::Crossover), p.device().getParameter(P::Crossover), 1e-3);
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("phase align: the compact face in the app's chain names the kick track; the window shows the full face")
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
    device.id = 30;
    device.sidechain.on = true;
    device.sidechain.sourceTrackId = 4;
    song.tracks[1].devices = {device};
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    graph.graph->prepare(kRate, 512);
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
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    auto* phase = dynamic_cast<P*>(graph.graph->nodes[1].processorForModelDevice[0]);
    REQUIRE(phase != nullptr);
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
    REQUIRE(face.sidechainName != nullptr);
    CHECK(face.sidechainName() == "Kick");
    CHECK(face.onChooseSidechain != nullptr);
    CHECK(phase->hitCount() >= 4u);
    CHECK(face.mappingTarget != nullptr);
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, testFile("sonora_phase_align_chain.png").getFullPathName().toStdString()));

    ui::ProFaceWindowView window(context, {5, 30, -1, -1});
    window.setBounds({0, 0, ui::PhaseAlignFace::kFull.w, ui::PhaseAlignFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);
    CHECK(window.title() == "Sonora Phase Align/Bass");
}
