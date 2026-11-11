// PLG-19: the Sonora Sweep face, in the VST3 window and in the app's device chain.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Sweep.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/SweepPlugin.h"
#include "plugin/SweepPluginEditor.h"
#include "ui/views/MainLayout.h"
#include "ui/views/SweepFace.h"
#include "ui/widgets/Buttons.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

using namespace sonora;
namespace
{
using W = devices::Sweep;
constexpr double kRate = 48000.0;

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

juce::File writeCrash()
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_crash.wav");
    file.deleteFile();
    juce::AudioBuffer<float> crash(2, 48000);
    juce::Random random(3);
    for (int s = 0; s < 48000; ++s)
        for (int ch = 0; ch < 2; ++ch) crash.setSample(ch, s, (random.nextFloat() * 2 - 1) * 0.6f * std::exp(-(float) s / 15000.0f));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer(crash, 0, 48000);
    return file;
}
} // namespace

TEST_CASE("sweep plugin: an instrument that plays a sweep per note; Load Sample and its path in the state")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::SweepPlugin p;
    CHECK(p.getParameters().size() == W::NumParams);
    CHECK(p.acceptsMidi());
    CHECK(p.getBusCount(true) == 0);
    CHECK(p.getParameters()[W::LengthParam]->getCurrentValueAsText() == "4 Bars");
    p.prepareToPlay(kRate, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::SweepPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, plugin::SweepView::kWidth, plugin::SweepView::kHeight});
    view.layout();
    auto& face = view.face();
    CHECK(face.onLoadSample != nullptr);

    // A crash, loaded as the editor's button does after the file chooser.
    const auto crash = writeCrash();
    REQUIRE(p.loadSample(crash));
    CHECK(p.device().hasSample());
    CHECK(std::lround(p.device().getParameter(W::Source)) == W::Sample);
    p.reportEditorChanges();
    CHECK(p.getParameters()[W::Source]->getCurrentValueAsText() == "Sample");

    // Down / Up buttons with real clicks.
    std::vector<ui::TextButton*> buttons;
    collect(face, buttons);
    auto press = [&](const std::string& label) {
        for (auto* b : buttons)
            if (b->label() == label)
            {
                view.dispatchMouseDown(at(*b, 4, 4));
                view.dispatchMouseUp(at(*b, 4, 4));
                return true;
            }
        return false;
    };
    REQUIRE(press("Down"));
    CHECK(std::lround(p.device().getParameter(W::Direction)) == W::Down);
    REQUIRE(press("Up"));
    p.device().setParameter(W::LengthParam, 2.0f); // 1 bar

    juce::AudioBuffer<float> block(2, 512);
    float peak = 0;
    for (int i = 0; i < 200; ++i) // 2.1 s at the default 120 BPM
    {
        juce::MidiBuffer midi;
        if (i == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 60, 1.0f), 0);
        block.clear();
        p.processBlock(block, midi);
        peak = std::max(peak, block.getMagnitude(0, 512));
        if (i == 100) view.tickTree();
    }
    CHECK(peak > 0.05f);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_plugin.png");
    view.tickTree();
    CHECK(gfx::renderToPng(view, (int) plugin::SweepView::kWidth, (int) plugin::SweepView::kHeight, 2.0f, png.getFullPathName().toStdString()));

    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::SweepPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK(restored.device().hasSample());
    CHECK(restored.device().samplePath() == crash.getFullPathName().toStdString());
    CHECK(std::lround(restored.device().getParameter(W::LengthParam)) == 2);
    p.editorBeingDeleted(editor.get());
    editor.reset();
    crash.deleteFile();
}

TEST_CASE("sweep: the face in the app's chain on a MIDI track, Load Sample enabled")
{
    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].kind = model::TrackKind::Midi;
    song.tracks[0].devices = {devices::nativeDeviceModel("SonoraSweep")};
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
    context.editDevice = [](int, int, std::function<void(model::Device&)>, std::string) {};
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::SweepFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    CHECK(faces[0]->onLoadSample != nullptr);
    CHECK(faces[0]->width() >= ui::SweepFace::kPreferredWidth - 1);
    auto* sweep = dynamic_cast<W*>(graph.graph->nodes[0].processorForModelDevice[0]);
    REQUIRE(sweep != nullptr);
    int mapped = 0;
    std::vector<ui::Widget*> all;
    collect(*faces[0], all);
    for (auto* w : all) mapped += w->midiTarget.has_value() ? 1 : 0;
    CHECK(mapped >= 12);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sweep_chain.png");
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, png.getFullPathName().toStdString()));
}
