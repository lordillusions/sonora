#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/RootWidget.h"
#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetableRoutingEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <set>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using E = devices::WavetableEffects;
using R = ui::WavetableRoutingEditor;
using Editor = ui::WavetableEditor;
E::Module effect(uint32_t id, E::Type type)
{
    E::Module result; result.id = id; result.type = type;
    for (const auto& descriptor : E::parameterDescriptors(type)) result.values.push_back(descriptor.initial);
    return result;
}
struct RoutingUi
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    model::EditHistory history;
    ui::UiContext context;
    std::unique_ptr<ui::RootWidget> root;
    std::unique_ptr<devices::DeviceProcessor> processor;
    W* synth = nullptr;
    Editor* editor = nullptr;
    int chain = -1, child = -1, edits = 0, selection = 0;
    explicit RoutingUi(float height = 700, bool nested = false)
    {
        auto target = devices::nativeDeviceModel("SonoraWavetable"); target.id = nested ? 77 : 42;
        E::Configuration configuration; auto convolution = effect(1, E::Type::Convolution);
        const auto ir = devices::ConvolutionIR::fromSamples(std::vector<float>{1}, 1, 48000, "Own routing impulse"); REQUIRE(ir);
        convolution.impulse = ir.data; configuration.buses[0] = {convolution}; configuration.buses[1] = {effect(2, E::Type::FrequencyShift)};
        std::string error; REQUIRE(devices::storeWavetableEffects(target, configuration, error));
        auto content = std::make_shared<model::NativeContent>(*target.nativeContent); content->controllerData = {0x51, 0, 0x32}; target.nativeContent = content;
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        if (nested)
        {
            auto rack = devices::nativeDeviceModel("InstrumentGroupDevice"); rack.id = 42; rack.chains.resize(1); rack.chains[0].devices.resize(3);
            rack.chains[0].devices[0] = devices::nativeDeviceModel("SonoraWavetable"); rack.chains[0].devices[0].id = 88;
            rack.chains[0].devices[2] = std::move(target); song.tracks[0].devices = {std::move(rack)}; chain = 0; child = 2;
        }
        else song.tracks[0].devices = {std::move(target)};
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        context.showChoices = [this](const auto&, const auto&, auto pick) { pick(selection); };
        context.setWavetableEffects = [this](int track, int device, const E::Configuration& configurationCopy, int c, int ch) {
            CHECK(track == 0); CHECK(device == 0); CHECK(c == chain); CHECK(ch == child);
            auto replacement = song.tracks[0].devices[0]; auto& edited = targetIn(replacement); devices::captureDeviceState(edited, processor.get());
            std::string reason; REQUIRE(devices::storeWavetableEffects(edited, configurationCopy, reason));
            history.perform(song, model::replaceDevice(song, 0, 0, std::move(replacement), "Change bus return")); ++edits; refresh();
        };
        refresh();
    }
    model::Device& targetIn(model::Device& value) const { return chain < 0 ? value : value.chains[static_cast<size_t>(chain)].devices[static_cast<size_t>(child)]; }
    model::Device& target() { return targetIn(song.tracks[0].devices[0]); }
    void refresh()
    {
        std::vector<std::string> warnings; auto next = devices::createNative(target(), &warnings); REQUIRE(next); CHECK(warnings.empty()); next->prepare(48000, 512);
        auto old = std::move(processor); processor = std::move(next); synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        root->removeAllChildren(); editor = root->add<Editor>(synth, 0, 0, chain, child); editor->setBounds(root->localBounds()); editor->layout();
    }
    ui::MouseEvent at(ui::Rect area, float x = 0.5f, float y = 0.5f) const { ui::MouseEvent event; event.x = area.x + x * area.w; event.y = area.y + y * area.h; return event; }
    void click(ui::Rect area) { REQUIRE(! area.isEmpty()); const auto event = at(area); root->dispatchMouseDown(event); root->dispatchMouseUp(event); }
    void drag(int parameter, float pixels, bool fine = false)
    {
        const auto area = editor->parameterCell(parameter); REQUIRE(! area.isEmpty()); auto down = at(area); down.shift = fine; root->dispatchMouseDown(down);
        auto moved = down; moved.y -= pixels; root->dispatchMouseDrag(moved); root->dispatchMouseUp(moved);
    }
    void wheel(int parameter, float delta, bool fine = false)
    {
        const auto area = editor->parameterCell(parameter); REQUIRE(! area.isEmpty()); auto event = at(area); event.wheelDeltaY = delta; event.shift = fine; root->dispatchMouseWheel(event);
    }
    void reset(int parameter)
    {
        const auto area = editor->parameterCell(parameter); REQUIRE(! area.isEmpty()); auto event = at(area); event.clicks = 2; root->dispatchMouseDown(event); root->dispatchMouseUp(event);
    }
    void inspect(Editor::Section section, int instance, int parameter)
    {
        click(editor->sectionCell(section));
        for (int count = 0; count < editor->instanceCount() && editor->selectedInstance() != instance; ++count) click(editor->selectorCell(1));
        REQUIRE(editor->selectedInstance() == instance);
        for (int page = 0; page < editor->pageCount() && editor->parameterCell(parameter).isEmpty(); ++page) click(editor->pageCell(1));
        REQUIRE(! editor->parameterCell(parameter).isEmpty());
    }
    ui::Rect inRouting(ui::Rect area) const { const auto bounds = editor->routingEditor()->bounds(); return area.translated(bounds.x, bounds.y); }
    void mix() { click(editor->workflowCell(Editor::Page::Mix)); REQUIRE(editor->routingEditor()->visible); }
    void reveal(int source) { for (int page = 0; page < editor->routingEditor()->pageCount() && editor->routingEditor()->parameterCell(source, R::Field::Destination).isEmpty(); ++page) click(inRouting(editor->routingEditor()->pageCell(1))); }
    void snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_routing_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1, directory.getChildFile(name).getFullPathName().toStdString()));
    }
};
} // namespace

TEST_CASE("wavetable routing UI: seven native sources expose exact destination sends envelope balance output and Direct gain mappings")
{
    RoutingUi rig; rig.mix(); CHECK(rig.editor->routingEditor()->visibleSources().size() == 7);
    rig.click(rig.editor->parameterCell(W::SourceRoutingOn)); CHECK_NEAR(rig.synth->getParameter(W::SourceRoutingOn), 1, 0);
    rig.selection = W::RouteToDirect; rig.click(rig.editor->parameterCell(W::sourceRoutingParam(0, W::SourceDestination)));
    CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(0, W::SourceDestination)), W::RouteToDirect, 0);
    const int send = W::sourceRoutingParam(0, W::SourceBus1); auto down = rig.at(rig.editor->parameterCell(send)); rig.root->dispatchMouseDown(down);
    auto drag = down; drag.y -= 100; rig.root->dispatchMouseDrag(drag); rig.root->dispatchMouseUp(drag); CHECK_NEAR(rig.synth->getParameter(send), 0.5, 0.0001);
    const int envelope = W::sourceRoutingParam(4, W::SourceEnvelope); const float before = rig.synth->getParameter(envelope); rig.click(rig.editor->parameterCell(envelope));
    CHECK_NEAR(rig.synth->getParameter(envelope), before > 0.5f ? 0 : 1, 0);
    auto direct = rig.at(rig.editor->parameterCell(W::DirectGain)); direct.wheelDeltaY = 1; rig.root->dispatchMouseWheel(direct);
    CHECK_NEAR(rig.synth->getParameter(W::DirectGain), 1.21, 0.0001); CHECK_NEAR(rig.synth->effects()->getRoutingParameter(5), 1.21, 0.0001);
    REQUIRE(! rig.editor->parameterCell(W::filterOutputParam(1)).isEmpty());
    CHECK(rig.editor->parameterCell(W::sourceRoutingParam(5, W::SourceFilterBalance)).isEmpty());
    CHECK(rig.editor->parameterCell(W::sourceRoutingParam(6, W::SourceFilterBalance)).isEmpty());
    rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(send)); REQUIRE(rig.context.mapSelection);
    CHECK(rig.context.mapSelection->param == rig.synth->parameters()[static_cast<size_t>(send)].id); rig.midi.cancelLearn(); rig.context.midiMapMode = false;
    rig.snapshot("Full Sources.png"); rig.click(rig.editor->mixPageCell(false)); CHECK(! rig.editor->routingEditor()->visible);
    for (int route = 0; route < W::FxRoutingParams; ++route) CHECK(! rig.editor->parameterCell(rig.synth->fxRoutingParameterIndex(static_cast<W::FxRoutingParam>(route))).isEmpty());
    rig.snapshot("Bus Mixer.png"); rig.click(rig.editor->mixPageCell(true)); CHECK(rig.editor->routingEditor()->visible);
}

TEST_CASE("wavetable routing UI: Other Filter cycle choices are rejected in routing rows and the generic inspector")
{
    RoutingUi rig; rig.mix(); rig.selection = W::RouteToFilters;
    rig.click(rig.editor->parameterCell(W::sourceRoutingParam(5, W::SourceDestination))); CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(5, W::SourceDestination)), 0, 0);
    rig.click(rig.editor->parameterCell(W::sourceRoutingParam(6, W::SourceDestination))); CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(6, W::SourceDestination)), W::RouteToMain, 0);
    auto wheel = rig.at(rig.editor->parameterCell(W::sourceRoutingParam(6, W::SourceDestination))); wheel.wheelDeltaY = -1; rig.root->dispatchMouseWheel(wheel);
    CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(6, W::SourceDestination)), W::RouteToMain, 0);
    rig.click(rig.editor->inspectorCell()); rig.click(rig.editor->sectionCell(Editor::Section::Routing));
    while (rig.editor->selectedInstance() != 6) rig.click(rig.editor->selectorCell(1));
    wheel = rig.at(rig.editor->parameterCell(W::sourceRoutingParam(6, W::SourceDestination))); wheel.wheelDeltaY = -1; rig.root->dispatchMouseWheel(wheel);
    CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(6, W::SourceDestination)), W::RouteToMain, 0);
    rig.snapshot("Routing Inspector.png");
}

TEST_CASE("wavetable routing UI: compact nested return edits preserve page live scalar state source bytes undo and serial latency")
{
    RoutingUi rig(210, true); rig.mix(); rig.reveal(6); rig.snapshot("Compact Sources.png");
    const int page = rig.editor->routingEditor()->page(); const auto before = rig.target().nativeContent;
    rig.synth->setParameter(W::sourceRoutingParam(6, W::SourceBus2), 0.37f); rig.synth->setParameter(W::DirectGain, 0.25f);
    CHECK(rig.synth->latencySamples() == 128); rig.selection = 0; rig.click(rig.inRouting(rig.editor->routingEditor()->returnCell(0)));
    CHECK(rig.edits == 1); CHECK(rig.synth->effectsConfiguration().returnBeforeMain[0]); CHECK(rig.synth->latencySamples() == 159);
    CHECK(rig.editor->workflow() == Editor::Page::Mix); CHECK(rig.editor->routingEditor()->page() == page);
    CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(6, W::SourceBus2)), 0.37, 0.0001); CHECK_NEAR(rig.synth->getParameter(W::DirectGain), 0.25, 0.0001);
    CHECK(rig.target().nativeContent->controllerData == before->controllerData); CHECK(! rig.song.tracks[0].devices[0].chains[0].devices[0].nativeContent);
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(! rig.synth->effectsConfiguration().returnBeforeMain[0]); CHECK(rig.synth->latencySamples() == 128);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->effectsConfiguration().returnBeforeMain[0]); rig.snapshot("Compact Pre Return.png");
}

TEST_CASE("wavetable routing UI: minimum source pages remain bounded and generic inspector reaches all new base controls")
{
    RoutingUi rig(500); rig.mix(); std::set<int> sources;
    for (int page = 0; page < rig.editor->routingEditor()->pageCount(); ++page)
    {
        for (int source : rig.editor->routingEditor()->visibleSources())
        {
            sources.insert(source);
            for (int field = 0; field < static_cast<int>(R::Field::Count); ++field)
            {
                const auto cell = rig.editor->routingEditor()->parameterCell(source, static_cast<R::Field>(field));
                if (! cell.isEmpty()) { CHECK(cell.bottom() < rig.editor->routingEditor()->pageCell(1).y); CHECK(cell.right() <= rig.editor->routingEditor()->width()); }
            }
        }
        if (rig.editor->routingEditor()->pageCount() > 1) rig.click(rig.inRouting(rig.editor->routingEditor()->pageCell(1)));
    }
    CHECK(sources.size() == 7); rig.reveal(6); rig.snapshot("Minimum Sources.png");
    rig.root->setBounds({0, 0, 1080, 210}); rig.editor->setBounds(rig.root->localBounds()); rig.editor->layout();
    rig.click(rig.editor->workflowCell(Editor::Page::Osc)); rig.click(rig.editor->sectionCell(Editor::Section::Routing));
    std::set<int> reached;
    for (int instance = 0; instance < rig.editor->instanceCount(); ++instance)
    {
        while (rig.editor->selectedInstance() != instance) rig.click(rig.editor->selectorCell(1));
        for (int page = 0; page < rig.editor->pageCount(); ++page)
        {
            for (int parameter : rig.editor->visibleParameters()) reached.insert(parameter);
            if (rig.editor->pageCount() > 1) rig.click(rig.editor->pageCell(1));
        }
    }
    CHECK(reached.size() == W::kRoutingSources * W::SourceRoutingParams + 2); CHECK(reached.contains(W::DirectGain)); CHECK(reached.contains(W::SourceRoutingOn));
    rig.click(rig.editor->sectionCell(Editor::Section::Filters)); while (rig.editor->selectedInstance() != 1) rig.click(rig.editor->selectorCell(1));
    for (int page = 0; page < rig.editor->pageCount() && rig.editor->parameterCell(W::filterOutputParam(1)).isEmpty(); ++page) rig.click(rig.editor->pageCell(1));
    REQUIRE(! rig.editor->parameterCell(W::filterOutputParam(1)).isEmpty()); rig.snapshot("Filter Output Inspector.png");
}

TEST_CASE("wavetable routing UI: quadratic Direct and filter output gestures preserve plain gain endpoints fine edits defaults and MIDI targets")
{
    RoutingUi rig; rig.mix();
    for (int parameter : {W::DirectGain, W::filterOutputParam(0), W::filterOutputParam(1)})
    {
        const auto& info = rig.synth->parameters()[static_cast<size_t>(parameter)];
        CHECK_NEAR(info.minValue, 0, 0); CHECK_NEAR(info.maxValue, 4, 0); CHECK_NEAR(info.defaultValue, 1, 0);
        rig.synth->setParameter(parameter, 1); rig.drag(parameter, 20); CHECK_NEAR(rig.synth->getParameter(parameter), 1.44, 0.000001);
        rig.synth->setParameter(parameter, 1); rig.drag(parameter, 10, true); CHECK_NEAR(rig.synth->getParameter(parameter), 1.0404, 0.000001);
        rig.synth->setParameter(parameter, 0); rig.wheel(parameter, 1); CHECK_NEAR(rig.synth->getParameter(parameter), 0.01, 0.000001);
        rig.synth->setParameter(parameter, 0); rig.wheel(parameter, 1, true); CHECK_NEAR(rig.synth->getParameter(parameter), 0.0001, 0.0000001);
        rig.synth->setParameter(parameter, 1); rig.drag(parameter, 100); CHECK_NEAR(rig.synth->getParameter(parameter), 4, 0);
        rig.drag(parameter, -200); CHECK_NEAR(rig.synth->getParameter(parameter), 0, 0);
        rig.reset(parameter); CHECK_NEAR(rig.synth->getParameter(parameter), 1, 0);
        rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(parameter)); REQUIRE(rig.context.mapSelection);
        CHECK(rig.context.mapSelection->param == info.id); rig.midi.cancelLearn(); rig.context.midiMapMode = false;
    }
    const int send = W::sourceRoutingParam(0, W::SourceBus1); rig.drag(send, 100); CHECK_NEAR(rig.synth->getParameter(send), 0.5, 0.000001);
    rig.synth->setParameter(W::DirectGain, 1); rig.drag(W::DirectGain, 20);
    CHECK_NEAR(rig.synth->effects()->getRoutingParameter(5), 1.44, 0.000001);
    rig.synth->setParameter(W::filterOutputParam(0), 0.0001f); rig.synth->setParameter(W::filterOutputParam(1), 4);
    rig.snapshot("Quadratic Sources.png");
}

TEST_CASE("wavetable routing UI: return and generic gain knobs retain plain captured factors and leave send and Main gain gestures linear")
{
    RoutingUi rig; rig.mix(); rig.click(rig.editor->mixPageCell(false));
    for (auto route : {W::Bus1ToMain, W::Bus2ToMain})
    {
        const int parameter = rig.synth->fxRoutingParameterIndex(route);
        rig.wheel(parameter, 1); CHECK_NEAR(rig.synth->getParameter(parameter), 1.21, 0.000001);
        rig.synth->setParameter(parameter, 0); rig.wheel(parameter, 1, true); CHECK_NEAR(rig.synth->getParameter(parameter), 0.0001, 0.0000001);
        rig.reset(parameter); CHECK_NEAR(rig.synth->getParameter(parameter), 1, 0);
        rig.drag(parameter, 20); CHECK_NEAR(rig.synth->getParameter(parameter), 1.44, 0.000001);
    }
    const int main = rig.synth->fxRoutingParameterIndex(W::MainGain); rig.drag(main, 20); CHECK_NEAR(rig.synth->getParameter(main), 1.2, 0.000001);
    const int send = rig.synth->fxRoutingParameterIndex(W::MainToBus1); rig.drag(send, 20); CHECK_NEAR(rig.synth->getParameter(send), 0.2, 0.000001);
    rig.snapshot("Quadratic Bus Mixer.png"); rig.click(rig.editor->inspectorCell());
    rig.inspect(Editor::Section::Routing, 7, W::DirectGain); rig.drag(W::DirectGain, 20); CHECK_NEAR(rig.synth->getParameter(W::DirectGain), 1.44, 0.000001);
    for (int filter = 0; filter < W::kFilters; ++filter)
    {
        const int parameter = W::filterOutputParam(filter); rig.inspect(Editor::Section::Filters, filter, parameter);
        rig.drag(parameter, 20); CHECK_NEAR(rig.synth->getParameter(parameter), 1.44, 0.000001);
        rig.synth->setParameter(parameter, 0); rig.wheel(parameter, 1, true); CHECK_NEAR(rig.synth->getParameter(parameter), 0.0001, 0.0000001);
        rig.reset(parameter); rig.drag(parameter, -50); CHECK_NEAR(rig.synth->getParameter(parameter), 0.25, 0.000001);
    }
    for (auto route : {W::Bus1ToMain, W::Bus2ToMain})
    {
        const int parameter = rig.synth->fxRoutingParameterIndex(route); rig.inspect(Editor::Section::Effects, 0, parameter);
        rig.reset(parameter); rig.drag(parameter, 20); CHECK_NEAR(rig.synth->getParameter(parameter), 1.44, 0.000001);
    }
    rig.snapshot("Quadratic Routing Inspector.png");
    auto captured = rig.target(); devices::captureDeviceState(captured, rig.processor.get());
    CHECK_NEAR(captured.param("FxRouting/DirectGain", -1), 1.44, 0.000001);
    CHECK_NEAR(captured.param("FxRouting/Bus1ToMain", -1), 1.44, 0.000001); CHECK_NEAR(captured.param("FxRouting/Bus2ToMain", -1), 1.44, 0.000001);
    CHECK_NEAR(captured.param("Filter.0/OutputLevel", -1), 0.25, 0.000001); CHECK_NEAR(captured.param("Filter.1/OutputLevel", -1), 0.25, 0.000001);
    std::vector<std::string> warnings; auto restored = devices::createNative(captured, &warnings); REQUIRE(restored); CHECK(warnings.empty());
    for (int parameter : {W::DirectGain, W::filterOutputParam(0), W::filterOutputParam(1), rig.synth->fxRoutingParameterIndex(W::Bus1ToMain), rig.synth->fxRoutingParameterIndex(W::Bus2ToMain)})
        CHECK_NEAR(restored->getParameter(parameter), rig.synth->getParameter(parameter), 0.000001);
}
