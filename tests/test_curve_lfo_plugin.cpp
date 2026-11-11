// PLG-12: Sonora Curve LFO as a VST3 — the AudioProcessor and its editor, outside any host.
#include "TestFramework.h"
#include "devices/native/CurveLfo.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/CurveLfoPlugin.h"
#include "plugin/CurveLfoPluginEditor.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/widgets/Buttons.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <set>

using namespace sonora;
namespace
{
using L = devices::CurveLfo;

juce::AudioProcessorParameterWithID* hostParameter(plugin::CurveLfoPlugin& p, const char* id)
{
    for (auto* parameter : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter); withId && withId->paramID == id) return withId;
    return nullptr;
}

// A playhead that is always playing at `bpm` from beat 0.
struct Playing final : juce::AudioPlayHead
{
    double bpm = 120, beats = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm(bpm);
        info.setPpqPosition(beats);
        info.setIsPlaying(true);
        return info;
    }
};

void fill(juce::AudioBuffer<float>& audio, float value)
{
    for (int c = 0; c < audio.getNumChannels(); ++c)
        for (int i = 0; i < audio.getNumSamples(); ++i) audio.setSample(c, i, value);
}

struct Recorder final : juce::AudioProcessorListener
{
    std::vector<int> changed;
    int gestures = 0, stateChanges = 0;
    void audioProcessorParameterChanged(juce::AudioProcessor*, int index, float) override { changed.push_back(index); }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged) ++stateChanges;
    }
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++gestures; }
};
} // namespace

TEST_CASE("curve lfo plugin: host parameters are the controls before the graphs, unique ids, discrete where the face is")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin p;
    CHECK(p.getParameters().size() == plugin::CurveLfoPlugin::kHostParameters);
    std::set<juce::String> ids;
    for (auto* parameter : p.getParameters())
        if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameter)) ids.insert(withId->paramID);
    CHECK(ids.size() == (size_t) plugin::CurveLfoPlugin::kHostParameters);
    CHECK(hostParameter(p, "SelectedGraph") == nullptr);
    CHECK(hostParameter(p, "G1.Count") == nullptr);

    auto* cutoff = hostParameter(p, "Filter.Cutoff");
    REQUIRE(cutoff != nullptr);
    cutoff->setValue(0.25f);
    CHECK_NEAR(p.lfo().getParameter(L::Cutoff), 0.25f, 1e-6);

    auto* cc = hostParameter(p, "Midi.CcOut");
    REQUIRE(cc != nullptr);
    CHECK(cc->isDiscrete());
    CHECK(cc->getNumSteps() == 129);
    cc->setValue(cc->getValueForText("75"));
    CHECK_NEAR(p.lfo().getParameter(L::CcOut), 75.0f, 1e-6);
    CHECK(cc->getText(cc->getValue(), 0) == "75");

    auto* volGraph = hostParameter(p, "Vol.Source");
    REQUIRE(volGraph != nullptr);
    CHECK(volGraph->getName(64) == "Vol Graph");
    CHECK(volGraph->getNumSteps() == L::kGraphs);
}

TEST_CASE("curve lfo plugin: state restores controls and graphs exactly; damaged data keeps the current curves")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin source;
    source.setParameterValue(L::Rate, 9);
    source.setParameterValue(L::Swing, 0.123456789f);
    source.setParameterValue(L::FilterType, 3);
    source.lfo().setParameter(L::SelectedGraph, 4);
    source.lfo().setGraph(4, dsp::CurveGraph::preset(2));
    source.lfo().setParameter(L::warpParam(4), 5);
    auto custom = dsp::CurveGraph::preset(0);
    custom.y[0] = 0.3f;
    custom.curve[0] = -0.7f;
    source.lfo().setGraph(11, custom);

    juce::MemoryBlock state;
    source.getStateInformation(state);
    REQUIRE(state.getSize() > 0);

    plugin::CurveLfoPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    const int count = (int) source.lfo().parameters().size();
    int differing = 0;
    for (int i = 0; i < count; ++i)
        if (std::abs(restored.lfo().getParameter(i) - source.lfo().getParameter(i)) > 0.0f) ++differing; // exact, not near
    CHECK(differing == 0);
    CHECK(restored.lfo().graph(11).count == custom.count);
    CHECK_NEAR(restored.lfo().graph(11).curve[0], -0.7f, 0);

    const char garbage[] = "not a state";
    restored.setStateInformation(garbage, (int) sizeof(garbage));
    CHECK(! restored.buildLog.empty());
    CHECK_NEAR(restored.lfo().getParameter(L::Rate), 9.0f, 0);
    CHECK_NEAR(restored.lfo().graph(11).curve[0], -0.7f, 0);

    // A state written by a later version is refused as a whole, not half-applied.
    juce::XmlElement future("SonoraCurveLfo");
    future.setAttribute("version", 99);
    auto* rate = future.createNewChildElement("Param");
    rate->setAttribute("id", "Rate");
    rate->setAttribute("value", 2.0);
    juce::MemoryBlock futureState;
    juce::AudioProcessor::copyXmlToBinary(future, futureState);
    std::string error;
    CHECK(! plugin::CurveLfoPlugin::decodeState(futureState.getData(), futureState.getSize(), restored.lfo(), error));
    CHECK_NEAR(restored.lfo().getParameter(L::Rate), 9.0f, 0);

    // Unknown ids are skipped; missing ones take their defaults.
    juce::XmlElement partial("SonoraCurveLfo");
    partial.setAttribute("version", 1);
    auto* depth = partial.createNewChildElement("Param");
    depth->setAttribute("id", "Depth");
    depth->setAttribute("value", 0.5);
    partial.createNewChildElement("Param")->setAttribute("id", "Something.New");
    juce::MemoryBlock partialState;
    juce::AudioProcessor::copyXmlToBinary(partial, partialState);
    REQUIRE(plugin::CurveLfoPlugin::decodeState(partialState.getData(), partialState.getSize(), restored.lfo(), error));
    CHECK_NEAR(restored.lfo().getParameter(L::Depth), 0.5f, 0);
    CHECK_NEAR(restored.lfo().getParameter(L::Rate), restored.lfo().parameters()[(size_t) L::Rate].defaultValue, 0);
    CHECK(restored.lfo().graph(11).count == dsp::CurveGraph::preset(0).count);
}

TEST_CASE("curve lfo plugin: modulates the input in host blocks larger than prepared, and sends only its CC")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin p;
    Playing head;
    p.setPlayHead(&head);
    p.prepareToPlay(48000, 256);
    p.setParameterValue(L::Rate, 15);     // 1/4 at 120 BPM: 24000 samples per cycle
    p.setParameterValue(L::Smooth, 0);
    p.setParameterValue(L::CcOut, 75);    // CC 74
    p.setParameterValue(L::CcDepth, 127);
    p.setParameterValue(L::amountParam(L::Cut), 1);

    juce::AudioBuffer<float> audio(2, 24000);
    fill(audio, 0.5f);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 74, 3), 10); // the host's own CC is not echoed
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 20);
    p.processBlock(audio, midi);

    float lowest = 1, highest = 0;
    bool finite = true;
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float v = audio.getSample(0, i);
        finite = finite && std::isfinite(v);
        lowest = std::min(lowest, v);
        highest = std::max(highest, v);
    }
    CHECK(finite);
    CHECK(highest > 0.45f);  // the default pump graph opens the volume ...
    CHECK(lowest < 0.1f);    // ... and ducks it within one cycle

    int ccEvents = 0, others = 0, lastPosition = -1;
    bool ordered = true, sawLowCc = false;
    for (const auto event : midi)
    {
        const auto message = event.getMessage();
        if (message.isControllerOfType(74))
        {
            ++ccEvents;
            sawLowCc = sawLowCc || (message.getControllerValue() == 3 && event.samplePosition == 10);
        }
        else ++others;
        ordered = ordered && event.samplePosition >= lastPosition;
        lastPosition = event.samplePosition;
    }
    CHECK(ccEvents > 10);
    CHECK(others == 0);
    CHECK(! sawLowCc);
    CHECK(ordered);
    CHECK(lastPosition > 256); // CC from later chunks lands at their place in the host block
    p.setPlayHead(nullptr);
}

TEST_CASE("curve lfo plugin: mono and stereo effect layouts only")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin p;
    juce::AudioProcessor::BusesLayout stereo, mono, mismatched, instrument;
    stereo.inputBuses.add(juce::AudioChannelSet::stereo());
    stereo.outputBuses.add(juce::AudioChannelSet::stereo());
    mono.inputBuses.add(juce::AudioChannelSet::mono());
    mono.outputBuses.add(juce::AudioChannelSet::mono());
    mismatched.inputBuses.add(juce::AudioChannelSet::mono());
    mismatched.outputBuses.add(juce::AudioChannelSet::stereo());
    instrument.inputBuses.add(juce::AudioChannelSet::disabled());
    instrument.outputBuses.add(juce::AudioChannelSet::stereo());
    CHECK(p.checkBusesLayoutSupported(stereo));
    CHECK(p.checkBusesLayoutSupported(mono));
    CHECK(! p.checkBusesLayoutSupported(mismatched));
    CHECK(! p.checkBusesLayoutSupported(instrument));
}

TEST_CASE("curve lfo plugin: editor edits are reported once, graph edits mark the state changed, host writes are not echoed")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin p;
    Recorder recorder;
    p.addListener(&recorder);
    p.reportEditorChanges();
    CHECK(recorder.changed.empty());
    CHECK(recorder.stateChanges == 0);

    p.lfo().setParameter(L::Depth, 0.4f); // what the face's knobs do
    p.reportEditorChanges();
    REQUIRE(recorder.changed.size() == 1);
    CHECK(recorder.changed.front() == L::Depth);
    CHECK(recorder.gestures == 1);
    p.reportEditorChanges();
    CHECK(recorder.changed.size() == 1);

    p.lfo().setGraph(2, dsp::CurveGraph::preset(3)); // a drawn curve
    p.reportEditorChanges();
    CHECK(recorder.changed.size() == 1);
    CHECK(recorder.stateChanges == 1);
    p.reportEditorChanges();
    CHECK(recorder.stateChanges == 1);

    p.getParameters()[(int) L::Phase]->setValue(0.75f); // host automation
    p.reportEditorChanges();
    CHECK(recorder.changed.size() == 1);
    p.removeListener(&recorder);
}

TEST_CASE("curve lfo plugin: the editor shows the device face over the plug-in's instance")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CurveLfoPlugin p;
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditorAndMakeActive());
    auto* window = dynamic_cast<plugin::CurveLfoPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    CHECK(window->getWidth() >= (int) ui::CurveLfoEditor::kPreferredWidth);
    auto& view = window->view();
    REQUIRE(view.editor() != nullptr);

    view.setBounds({0, 0, (float) window->getWidth(), (float) window->getHeight()});
    view.layout();
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_curve_lfo_plugin.png");
    CHECK(gfx::renderToPng(view, window->getWidth(), window->getHeight(), 2.0f, png.getFullPathName().toStdString()));

    // UI-83: the header arrow folds the window to the graph and back (a real click on it).
    const auto clickOn = [&](ui::Widget* widget) {
        REQUIRE(widget != nullptr);
        const auto r = widget->boundsInRoot();
        ui::MouseEvent e; e.x = r.centreX(); e.y = r.centreY();
        view.dispatchMouseDown(e); view.dispatchMouseUp(e);
        view.setBounds({0, 0, (float) window->getWidth(), (float) window->getHeight()});
        view.layout();
    };
    const int fullWidth = window->getWidth();
    clickOn(view.foldButton());
    CHECK(view.showsMini()); CHECK(p.editorMini);
    CHECK(! view.editor()->visible); CHECK(view.mini()->visible);
    CHECK(window->getWidth() == plugin::CurveLfoPluginEditor::kMiniWidth);
    CHECK(window->getHeight() == plugin::CurveLfoPluginEditor::kMiniHeight);
    CHECK(view.foldButton()->icon() == ui::IconButton::Icon::FoldClosed);
    const auto miniPng = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_curve_lfo_plugin_mini.png");
    CHECK(gfx::renderToPng(view, window->getWidth(), window->getHeight(), 2.0f, miniPng.getFullPathName().toStdString()));
    clickOn(view.foldButton());
    CHECK(! view.showsMini()); CHECK(! p.editorMini);
    CHECK(window->getWidth() == fullWidth);
    clickOn(view.foldButton());
    clickOn(view.mini()); // a click on the graph unfolds too
    CHECK(! view.showsMini()); CHECK(window->getWidth() == fullWidth);
    p.editorBeingDeleted(editor.get()); // what a host's wrapper does
    editor.reset();
}
