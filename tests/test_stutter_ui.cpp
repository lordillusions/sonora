// PLG-18: the Sonora Stutter face, in the VST3 window and in the app's device chain, driven with real input.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Stutter.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/StutterPlugin.h"
#include "plugin/StutterPluginEditor.h"
#include "ui/views/MainLayout.h"
#include "ui/views/StutterFace.h"
#include "ui/widgets/Buttons.h"

#include <cmath>

using namespace sonora;
namespace
{
using S = devices::Stutter;
constexpr double kRate = 48000.0;

template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}

ui::MouseEvent at(ui::Widget& widget, float x, float y, int clicks = 1)
{
    ui::MouseEvent e;
    const auto origin = widget.boundsInRoot();
    e.x = origin.x + x;
    e.y = origin.y + y;
    e.clicks = clicks;
    return e;
}

// A "voice": a 220 Hz tone with a syllable envelope every 250 ms.
juce::AudioBuffer<float> voice(int samples)
{
    juce::AudioBuffer<float> a(2, samples);
    for (int s = 0; s < samples; ++s)
    {
        const float t = (float) s / (float) kRate, syllable = std::fmod(t, 0.25f);
        const float v = 0.5f * std::sin(6.2831853f * 220.0f * t) * std::min(1.0f, syllable * 40.0f) * std::exp(-syllable * 6.0f);
        a.setSample(0, s, v);
        a.setSample(1, s, v);
    }
    return a;
}

template <typename Process> void play(ui::Widget& root, int samples, Process&& process)
{
    auto audio = voice(samples);
    for (int start = 0, block = 0; start < audio.getNumSamples(); start += 512, ++block)
    {
        if (block % 4 == 0) root.tickTree();
        const int n = std::min(512, audio.getNumSamples() - start);
        juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, start, n);
        process(view);
    }
    root.tickTree();
}
} // namespace

TEST_CASE("stutter plugin: host parameters, state, MIDI, pads and pattern with real input")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::StutterPlugin p;
    CHECK(p.getParameters().size() == S::NumParams);
    CHECK(p.acceptsMidi());
    CHECK(p.getParameters()[S::Rate]->getCurrentValueAsText() == "1/16");
    CHECK(p.getParameters()[S::ModeParam]->getCurrentValueAsText() == "Repeat");
    auto* pitch = p.getParameters()[S::Pitch];
    pitch->setValue(pitch->getValueForText("7"));
    CHECK_NEAR(p.device().getParameter(S::Pitch), 7.0, 1e-4);
    pitch->setValue(pitch->getValueForText("0"));
    p.prepareToPlay(kRate, 512);
    CHECK(p.getLatencySamples() == 0);

    juce::MemoryBlock state;
    p.device().setParameter(S::Shrink, 0.4f);
    p.device().setParameter(S::stepParam(2), 3.0f);
    p.getStateInformation(state);
    plugin::StutterPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK_NEAR(restored.device().getParameter(S::Shrink), 0.4, 1e-4);
    CHECK_NEAR(restored.device().getParameter(S::stepParam(2)), 3.0, 1e-4);
    p.device().setParameter(S::stepParam(2), 0.0f);

    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::StutterPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, plugin::StutterView::kWidth, plugin::StutterView::kHeight});
    view.layout();
    auto& face = view.face();

    // A MIDI note (E1 = Freeze) engages through processBlock and the notes do not pass on.
    {
        juce::AudioBuffer<float> block(2, 512);
        block.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 10);
        p.processBlock(block, midi);
        CHECK(midi.isEmpty());
        view.tickTree();
        CHECK(face.view().engaged);
        CHECK(face.view().mode == S::Freeze);
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::noteOff(1, 40), 0);
        juce::AudioBuffer<float> rest(2, 4096);
        rest.clear();
        p.processBlock(rest, off);
    }

    // Hold the Reverse pad with the mouse: it stutters while held and selects Reverse; releasing stops it.
    juce::MidiBuffer midi;
    play(view, 24000, [&](juce::AudioBuffer<float>& block) { p.processBlock(block, midi); });
    const auto pad = face.padArea(S::Reverse);
    view.dispatchMouseDown(at(face, pad.centreX(), pad.centreY()));
    play(view, 12000, [&](juce::AudioBuffer<float>& block) { p.processBlock(block, midi); });
    CHECK(face.view().engaged);
    CHECK(face.view().mode == S::Reverse);
    CHECK(face.view().bins > 0);
    CHECK(std::lround(p.device().getParameter(S::ModeParam)) == S::Reverse);
    p.reportEditorChanges();
    CHECK(p.getParameters()[S::ModeParam]->getCurrentValueAsText() == "Reverse");
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_stutter_plugin.png");
    CHECK(gfx::renderToPng(view, (int) plugin::StutterView::kWidth, (int) plugin::StutterView::kHeight, 2.0f,
                           png.getFullPathName().toStdString()));
    view.dispatchMouseUp(at(face, pad.centreX(), pad.centreY()));
    play(view, 4800, [&](juce::AudioBuffer<float>& block) { p.processBlock(block, midi); });
    CHECK(! face.view().engaged);

    // Latch = the Hold control.
    const auto latch = face.padArea(S::kModes);
    view.dispatchMouseDown(at(face, latch.centreX(), latch.centreY()));
    view.dispatchMouseUp(at(face, latch.centreX(), latch.centreY()));
    CHECK(p.device().getParameter(S::Hold) > 0.5f);
    view.dispatchMouseDown(at(face, latch.centreX(), latch.centreY()));
    view.dispatchMouseUp(at(face, latch.centreX(), latch.centreY()));
    CHECK(p.device().getParameter(S::Hold) < 0.5f);

    // Pattern cells: a click sets the selected mode (Reverse), dragging paints, a second click clears, Alt clears.
    const auto c0 = face.stepArea(0), c3 = face.stepArea(3);
    view.dispatchMouseDown(at(face, c0.centreX(), c0.centreY()));
    view.dispatchMouseDrag(at(face, c3.centreX(), c3.centreY()));
    view.dispatchMouseUp(at(face, c3.centreX(), c3.centreY()));
    for (int s = 0; s <= 3; ++s) CHECK(std::lround(p.device().getParameter(S::stepParam(s))) == 1 + S::Reverse);
    view.dispatchMouseDown(at(face, c0.centreX(), c0.centreY()));
    view.dispatchMouseUp(at(face, c0.centreX(), c0.centreY()));
    CHECK(std::lround(p.device().getParameter(S::stepParam(0))) == 0);
    auto alt = at(face, c3.centreX(), c3.centreY());
    alt.alt = true;
    view.dispatchMouseDown(alt);
    view.dispatchMouseUp(alt);
    CHECK(std::lround(p.device().getParameter(S::stepParam(3))) == 0);

    // The Pattern and Sync buttons; Sync off swaps Rate for Length.
    std::vector<ui::TextButton*> buttons;
    collect(face, buttons);
    auto press = [&](const std::string& label) {
        for (auto* b : buttons)
            if (b->label() == label)
            {
                const auto e = at(*b, b->width() / 2, b->height() / 2);
                view.dispatchMouseDown(e);
                view.dispatchMouseUp(e);
                return true;
            }
        return false;
    };
    REQUIRE(press("Pattern"));
    CHECK(p.device().getParameter(S::PatternOn) > 0.5f);
    REQUIRE(press("Sync"));
    CHECK(p.device().getParameter(S::Sync) < 0.5f);
    view.tickTree();
    REQUIRE(press("Sync"));
    REQUIRE(press("Pattern"));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("stutter: the face in the app's device chain")
{
    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[0].devices = {devices::nativeDeviceModel("SonoraStutter")};
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
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1400, 900});
    root.layout();
    std::vector<ui::StutterFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto* stutter = dynamic_cast<S*>(graph.graph->nodes[0].processorForModelDevice[0]);
    REQUIRE(stutter != nullptr);
    CHECK(faces[0]->width() >= ui::StutterFace::kPreferredWidth - 1);
    stutter->setParameter(S::Hold, 1.0f);
    stutter->setParameter(S::Shrink, 0.5f);
    juce::MidiBuffer midi;
    play(root, 20000, [&](juce::AudioBuffer<float>& block) { stutter->process(block, midi); });
    CHECK(faces[0]->view().engaged);
    CHECK(faces[0]->view().repeat >= 2);
    int mapped = 0;
    std::vector<ui::Widget*> all;
    collect(*faces[0], all);
    for (auto* w : all) mapped += w->midiTarget.has_value() ? 1 : 0;
    CHECK(mapped >= 14);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_stutter_chain.png");
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, png.getFullPathName().toStdString()));
}
