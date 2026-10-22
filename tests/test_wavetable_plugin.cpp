// PLG-11: Sonora Prism as a VST3 — the AudioProcessor and its editor, outside any host.
#include "TestFramework.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/WavetablePlugin.h"
#include "plugin/WavetablePluginEditor.h"
#include "ui/views/WavetableEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <set>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;

std::shared_ptr<const devices::WavetableData> ownTable()
{
    std::vector<float> samples(256 * 2);
    for (int frame = 0; frame < 2; ++frame)
        for (int i = 0; i < 256; ++i)
            samples[(size_t) (frame * 256 + i)] = (float) std::sin(6.283185307179586 * i * (frame + 1) / 256);
    auto result = devices::WavetableData::fromSamples(samples, 1, 256, 48000, "Plug-in test table");
    REQUIRE(result);
    return result.data;
}

float peak(const juce::AudioBuffer<float>& audio, int start, int end)
{
    float result = 0;
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = start; i < end; ++i)
        {
            const float v = audio.getSample(c, i);
            if (! std::isfinite(v)) return 1e9f;
            result = std::max(result, std::abs(v));
        }
    return result;
}

juce::AudioProcessorParameterWithID* hostParameter(plugin::WavetablePlugin& p, const char* id)
{
    for (auto* parameter : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter); withId && withId->paramID == id) return withId;
    return nullptr;
}

struct Recorder final : juce::AudioProcessorListener
{
    std::vector<int> changed;
    int gestures = 0;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override { changed.push_back(index); }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++gestures; }
};
} // namespace

TEST_CASE("wavetable plugin: host parameters are the fixed controls with unique ids and plain-value round trips")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::WavetablePlugin p;
    CHECK(p.getParameters().size() == W::ParameterCount);
    std::set<juce::String> ids;
    for (auto* parameter : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter)) ids.insert(withId->paramID);
    CHECK(ids.size() == (size_t) W::ParameterCount);

    auto* volume = hostParameter(p, "Volume");
    REQUIRE(volume != nullptr);
    CHECK(volume->getName(64) == "Volume");
    volume->setValue(0.5f); // -60..12 dB
    CHECK_NEAR(p.synth()->getParameter(0), -24.0f, 1e-4);
    CHECK_NEAR(volume->getValue(), 0.5f, 1e-6);

    const auto& info = p.synth()->parameters()[(size_t) W::oscParam(1, W::Unison)];
    auto* unison = hostParameter(p, info.id.c_str());
    REQUIRE(unison != nullptr);
    CHECK(unison->isDiscrete());
    CHECK(unison->getName(64).contains("2")); // the id's oscillator number: host lists tell the oscillators apart
    unison->setValue(1.0f);
    CHECK_NEAR(p.synth()->getParameter(W::oscParam(1, W::Unison)), (float) W::kUnison, 0);

    const auto& routing = p.synth()->parameters()[5];
    REQUIRE(routing.choices.size() == 2);
    auto* route = hostParameter(p, routing.id.c_str());
    REQUIRE(route != nullptr);
    CHECK(route->getText(1.0f, 0) == "Parallel");
    CHECK_NEAR(route->getValueForText("Parallel"), 1.0f, 1e-6);
}

TEST_CASE("wavetable plugin: state restores controls and native content into a fresh instance")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::WavetablePlugin source;
    source.prepareToPlay(48000, 256);
    source.setParameterValue(W::oscParam(0, W::Level), 0.3f);
    source.setParameterValue(W::filterParam(0, W::Cutoff), source.synth()->parameters()[(size_t) W::filterParam(0, W::Cutoff)].minValue);
    const auto table = ownTable();
    std::string error;
    REQUIRE(source.editDevice([&](model::Device& device) { return devices::storeWavetableTable(device, 0, *table, error); }));
    CHECK(source.synth()->customTableName(0) == "Plug-in test table");
    CHECK_NEAR(source.parameterValue(W::oscParam(0, W::Level)), 0.3f, 1e-6); // the edit kept the live controls

    juce::MemoryBlock state;
    source.getStateInformation(state);
    REQUIRE(state.getSize() > 0);

    plugin::WavetablePlugin restored;
    restored.prepareToPlay(48000, 256);
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK_NEAR(restored.parameterValue(W::oscParam(0, W::Level)), 0.3f, 1e-6);
    CHECK_NEAR(restored.parameterValue(W::filterParam(0, W::Cutoff)), source.parameterValue(W::filterParam(0, W::Cutoff)), 1e-6);
    CHECK(restored.synth()->customTableName(0) == "Plug-in test table");
    REQUIRE(restored.device().nativeContent != nullptr);
    CHECK(restored.device().nativeContent->data == source.device().nativeContent->data);
    for (int i = 0; i < W::ParameterCount; ++i)
        CHECK_NEAR(restored.parameterValue(i), source.parameterValue(i), 1e-6);

    // Damaged or foreign data keeps the current sound and leaves a note.
    const char garbage[] = "not a state";
    restored.setStateInformation(garbage, (int) sizeof(garbage));
    CHECK(restored.synth()->customTableName(0) == "Plug-in test table");
    CHECK(! restored.buildLog.empty());
    model::Device decoded;
    CHECK(! plugin::WavetablePlugin::decodeState(garbage, sizeof(garbage), decoded, error));
}

TEST_CASE("wavetable plugin: a note sounds in host blocks larger than prepared and across an instance swap")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::WavetablePlugin p;
    p.prepareToPlay(48000, 256);
    juce::AudioBuffer<float> audio(2, 1024);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 300);
    p.processBlock(audio, midi);
    CHECK(peak(audio, 0, 300) < 1e-6f);
    CHECK(peak(audio, 300, 1024) > 1e-3f);
    CHECK(peak(audio, 0, 1024) < 8.0f);
    CHECK(midi.isEmpty());

    std::string error;
    const auto table = ownTable();
    REQUIRE(p.editDevice([&](model::Device& device) { return devices::storeWavetableTable(device, 0, *table, error); }));
    juce::MidiBuffer next;
    next.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0);
    for (int block = 0; block < 4; ++block)
    {
        audio.clear();
        p.processBlock(audio, next);
    }
    CHECK(peak(audio, 0, 1024) > 1e-3f);
    p.releaseRetired();
}

TEST_CASE("wavetable plugin: editor edits are reported to the host once, host writes are not echoed")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::WavetablePlugin p;
    Recorder recorder;
    p.addListener(&recorder);
    p.reportEditorChanges();
    CHECK(recorder.changed.empty());

    p.synth()->setParameter(W::oscParam(2, W::Level), 0.25f); // what the dashboard's knobs do
    p.reportEditorChanges();
    REQUIRE(recorder.changed.size() == 1);
    CHECK(recorder.changed.front() == W::oscParam(2, W::Level));
    CHECK(recorder.gestures == 1);
    p.reportEditorChanges();
    CHECK(recorder.changed.size() == 1);

    p.getParameters()[(int) W::oscParam(2, W::Level)]->setValue(0.75f); // host automation
    p.reportEditorChanges();
    CHECK(recorder.changed.size() == 1);
    p.removeListener(&recorder);
}

TEST_CASE("wavetable plugin: the editor shows the dashboard and follows instance swaps")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::WavetablePlugin p;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditorAndMakeActive());
    auto* window = dynamic_cast<plugin::WavetablePluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    CHECK(window->getWidth() >= 1080);
    auto& view = window->view();
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->synth() == p.synth());

    std::string error;
    const auto table = ownTable();
    CHECK(window->context().importWavetable != nullptr); // the dashboard's callbacks are connected
    CHECK(window->context().importWavetablePreset != nullptr);
    CHECK(window->context().editDevice != nullptr);
    REQUIRE(p.editDevice([&](model::Device& device) { return devices::storeWavetableTable(device, 0, *table, error); }));
    CHECK(view.editor()->synth() != p.synth()); // the rebuild runs after the gesture, the old instance stays alive till then
    window->rebuildNowIfPending();
    REQUIRE(view.editor() != nullptr);
    CHECK(view.editor()->synth() == p.synth());

    view.setBounds({0, 0, (float) window->getWidth(), (float) window->getHeight()});
    view.layout();
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_wavetable_plugin.png");
    CHECK(gfx::renderToPng(view, window->getWidth(), window->getHeight(), 1.0f, png.getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get()); // what a host's wrapper does
    editor.reset();
}
