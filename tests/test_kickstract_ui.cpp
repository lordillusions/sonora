// PLG-17: the Sonora Kickstract face, in the VST3 window and in the app's device chain, driven with real input.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Kickstract.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/KickstractPlugin.h"
#include "plugin/KickstractPluginEditor.h"
#include "ui/views/KickstractFace.h"
#include "ui/views/MainLayout.h"
#include "ui/widgets/Buttons.h"

#include <cmath>
#include <random>

using namespace sonora;
namespace
{
using K = devices::Kickstract;
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

// Kick (pitch drop, click) every 500 ms over hats and a pad, as in test_kickstract.cpp.
juce::AudioBuffer<float> mix(int kicks)
{
    const int period = (int) (0.5 * kRate), n = period * kicks;
    juce::AudioBuffer<float> a(2, n);
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    float hp = 0, prev = 0;
    for (int s = 0; s < n; ++s)
    {
        const float tk = (float) (s % period) / (float) kRate, t = (float) s / (float) kRate;
        const float phase = 6.2831853f * (50.0f * tk + 90.0f * 0.03f * (1.0f - std::exp(-tk / 0.03f)));
        const float kick = 0.8f * std::exp(-tk / 0.09f) * std::sin(phase) + 0.25f * std::exp(-tk / 0.002f) * noise(rng);
        const float white = noise(rng);
        hp = 0.7f * (hp + white - prev);
        prev = white;
        const float hat = 0.12f * std::exp(-(float) ((s + period / 8) % (period / 4)) / (float) kRate / 0.03f) * hp;
        const float v = kick + hat + 0.1f * std::sin(6.2831853f * 330.0f * t);
        a.setSample(0, s, v);
        a.setSample(1, s, v);
    }
    return a;
}

// Feeds `audio` to `process` in 512-sample blocks, ticking the UI between blocks so the display taps stay requested.
template <typename Process> void play(ui::Widget& root, const juce::AudioBuffer<float>& audio, Process&& process)
{
    juce::AudioBuffer<float> copy(audio);
    for (int start = 0, block = 0; start < copy.getNumSamples(); start += 512, ++block)
    {
        if (block % 4 == 0) root.tickTree();
        const int n = std::min(512, copy.getNumSamples() - start);
        juce::AudioBuffer<float> view(copy.getArrayOfWritePointers(), 2, start, n);
        process(view);
    }
    root.tickTree();
}
} // namespace

TEST_CASE("kickstract plugin: host parameters, latency, state, window with real input")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::KickstractPlugin p;
    CHECK(p.getParameters().size() == K::NumParams);
    auto* low = p.getParameters()[K::LowSplit];
    CHECK(low->getCurrentValueAsText().startsWith("150"));
    low->setValue(low->getValueForText("100"));
    CHECK_NEAR(p.device().getParameter(K::LowSplit), 100.0, 0.5);
    low->setValue(low->getValueForText("150"));
    CHECK(p.getParameters()[K::Listen]->getCurrentValueAsText() == "Kick");
    p.prepareToPlay(kRate, 512);
    CHECK(p.getLatencySamples() == (int) std::lround(K::kLookaheadSeconds * kRate));

    juce::MemoryBlock state;
    p.device().setParameter(K::bandParam(K::Mid, K::BandFade), 33.0f);
    p.getStateInformation(state);
    plugin::KickstractPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK_NEAR(restored.device().getParameter(K::bandParam(K::Mid, K::BandFade)), 33.0, 1e-4);

    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::KickstractPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, plugin::KickstractView::kWidth, plugin::KickstractView::kHeight});
    view.layout();
    auto& face = view.face();

    juce::MidiBuffer midi;
    play(view, mix(6), [&](juce::AudioBuffer<float>& block) { p.processBlock(block, midi); });
    CHECK(p.device().triggerCount() == 6u);
    CHECK(face.kick().bins > 0);

    // Drag the low split line from 150 Hz to about 100 Hz; the host hears about it.
    const float y = face.spectrumArea().centreY();
    REQUIRE(face.handleAt(face.xForHz(150), y) == 0);
    view.dispatchMouseDown(at(face, face.xForHz(150), y));
    view.dispatchMouseDrag(at(face, face.xForHz(100), y));
    view.dispatchMouseUp(at(face, face.xForHz(100), y));
    CHECK_NEAR(p.device().getParameter(K::LowSplit), 100.0, 3.0);
    p.reportEditorChanges();
    CHECK_NEAR(p.getParameters()[K::LowSplit]->getValue(), low->getValueForText("100"), 0.01);
    // Double-click resets it.
    const auto reset = at(face, face.xForHz(p.device().lowSplitHz()), y, 2);
    view.dispatchMouseDown(reset);
    view.dispatchMouseUp(reset);
    CHECK_NEAR(p.device().getParameter(K::LowSplit), 150.0, 1e-3);

    // Drag the low band's Fade handle on the kick display to the right: a longer fade.
    const float fadeEnd = (float) (K::kAttackSeconds * 1000.0) + p.device().getParameter(K::bandParam(K::Low, K::BandLength))
                          + p.device().getParameter(K::bandParam(K::Low, K::BandFade));
    const float cy = face.kickArea().centreY();
    REQUIRE(face.handleAt(face.xForMs(fadeEnd), cy) == 20 + K::Low);
    const float before = p.device().getParameter(K::bandParam(K::Low, K::BandFade));
    view.dispatchMouseDown(at(face, face.xForMs(fadeEnd), cy));
    view.dispatchMouseDrag(at(face, face.xForMs(fadeEnd) + 30, cy));
    view.dispatchMouseUp(at(face, face.xForMs(fadeEnd) + 30, cy));
    CHECK(p.device().getParameter(K::bandParam(K::Low, K::BandFade)) > before + 10);
    p.device().setParameter(K::bandParam(K::Low, K::BandFade), before);

    // Buttons: Listen "Rem", the Mid band's solo.
    std::vector<ui::TextButton*> buttons;
    collect(face, buttons);
    auto press = [&](const std::string& label, int nth = 0) {
        for (auto* b : buttons)
            if (b->label() == label && nth-- == 0)
            {
                const auto e = at(*b, b->width() / 2, b->height() / 2);
                view.dispatchMouseDown(e);
                view.dispatchMouseUp(e);
                return true;
            }
        return false;
    };
    REQUIRE(press("Rem"));
    CHECK(std::lround(p.device().getParameter(K::Listen)) == K::ListenRemoved);
    REQUIRE(press("Kick"));
    REQUIRE(press("S", 1));
    CHECK(p.device().getParameter(K::bandParam(K::Mid, K::BandSolo)) > 0.5f);
    REQUIRE(press("S", 1));

    play(view, mix(2), [&](juce::AudioBuffer<float>& block) { p.processBlock(block, midi); });
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_kickstract_plugin.png");
    CHECK(gfx::renderToPng(view, (int) plugin::KickstractView::kWidth, (int) plugin::KickstractView::kHeight, 2.0f,
                           png.getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("kickstract: the face in the app's device chain")
{
    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[0].devices = {devices::nativeDeviceModel("SonoraKickstract")};
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
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::KickstractFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto* kick = dynamic_cast<K*>(graph.graph->nodes[0].processorForModelDevice[0]);
    REQUIRE(kick != nullptr);
    CHECK(faces[0]->width() >= ui::KickstractFace::kPreferredWidth - 1);
    juce::MidiBuffer midi;
    play(root, mix(4), [&](juce::AudioBuffer<float>& block) { kick->process(block, midi); });
    CHECK(faces[0]->kick().bins > 0);
    // Knobs are MIDI-mappable device parameters.
    int mapped = 0;
    std::vector<ui::Widget*> all;
    collect(*faces[0], all);
    for (auto* w : all) mapped += w->midiTarget.has_value() ? 1 : 0;
    CHECK(mapped >= 20);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_kickstract_chain.png");
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, png.getFullPathName().toStdString()));
}
