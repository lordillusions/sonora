#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/ExponentialCurveGraph.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/RootWidget.h"
#include "ui/views/WavetableEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <set>

using namespace sonora;
namespace
{
using E = devices::WavetableEffects;
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;

E::Module effect(E::ModuleId id, E::Type type)
{
    E::Module module; module.id = id; module.type = type;
    for (const auto& descriptor : E::parameterDescriptors(type)) module.values.push_back(descriptor.initial);
    return module;
}
std::shared_ptr<const devices::ConvolutionIR> ownImpulse()
{
    std::array<float, 64> samples{}; samples[0] = 0.8f; samples[9] = 0.1f; samples[23] = -0.05f;
    const auto loaded = devices::ConvolutionIR::fromSamples(samples, 1, 48000, "Own UI impulse"); REQUIRE(loaded); return loaded.data;
}
E::Configuration initialEffects()
{
    E::Configuration configuration;
    auto split = effect(5, E::Type::Split3); split.bands[0] = {effect(40, E::Type::Utility)};
    configuration.buses[0] = {effect(9, E::Type::Delay), std::move(split)};
    configuration.buses[1] = {effect(4, E::Type::Reverb)};
    auto convolution = effect(7, E::Type::Convolution); convolution.impulse = ownImpulse();
    configuration.buses[2] = {std::move(convolution)};
    return configuration;
}
E::Module* findModule(std::vector<E::Module>& modules, E::ModuleId id)
{
    for (auto& module : modules)
    {
        if (module.id == id) return &module;
        for (auto& band : module.bands) if (auto* found = findModule(band, id)) return found;
    }
    return nullptr;
}

struct FxUi
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
    E::Type nextType = E::Type::Chorus;
    int edits = 0, imports = 0, chain = -1, child = -1;
    bool importedProfile = false;

    explicit FxUi(E::Configuration configuration = initialEffects(), bool nested = false, float height = 700, bool imported = false)
    {
        importedProfile = imported;
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        auto target = devices::nativeDeviceModel("SonoraWavetable"); target.id = nested ? 77 : 42;
        std::string error; REQUIRE(devices::storeWavetableEffects(target, configuration, error));
        if (nested)
        {
            auto rack = devices::nativeDeviceModel("InstrumentGroupDevice"); rack.id = 42; rack.chains.resize(2); rack.chains[1].devices.resize(4);
            rack.chains[1].devices[3] = std::move(target); rack.chains[1].devices[0] = devices::nativeDeviceModel("SonoraWavetable");
            rack.chains[1].devices[0].id = 78; song.tracks[0].devices = {std::move(rack)}; chain = 1; child = 3;
        }
        else song.tracks[0].devices = {std::move(target)};
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        context.setWavetableEffects = [this](int track, int device, const E::Configuration& next, int c, int ch) {
            CHECK(track == 0); CHECK(device == 0); CHECK(c == chain); CHECK(ch == child);
            auto replacement = song.tracks[0].devices[0]; auto& edited = targetIn(replacement);
            devices::captureDeviceState(edited, processor.get());
            std::string reason; REQUIRE(devices::storeWavetableEffects(edited, next, reason));
            history.perform(song, model::replaceDevice(song, 0, 0, std::move(replacement), "Edit effects")); ++edits; refresh();
        };
        context.importWavetableImpulseResponse = [this](int track, int device, E::ModuleId id, const std::string& path, int c, int ch) {
            CHECK(track == 0); CHECK(device == 0); CHECK(c == chain); CHECK(ch == child); CHECK(path == "/tmp/own-ui-impulse.wav");
            auto configurationCopy = synth->effectsConfiguration(); E::Module* found = nullptr;
            for (auto& bus : configurationCopy.buses) if (auto* module = findModule(bus, id)) found = module;
            REQUIRE(found); CHECK(found->type == E::Type::Convolution); found->impulse = ownImpulse(); ++imports;
            context.setWavetableEffects(0, 0, configurationCopy, chain, child);
        };
        refresh();
    }
    model::Device& targetIn(model::Device& value) const { return chain < 0 ? value : value.chains[(size_t) chain].devices[(size_t) child]; }
    model::Device& target() { return targetIn(song.tracks[0].devices[0]); }
    void refresh()
    {
        std::vector<std::string> warnings;
        auto next = devices::createNative(target(), &warnings); REQUIRE(next); CHECK(warnings.empty());
        if (importedProfile)
        {
            auto* wavetable = dynamic_cast<W*>(next.get()); REQUIRE(wavetable);
            wavetable->setImportedEnvelopeProfile(true); wavetable->setImportedLfoProfile(true);
            dsp::CurveGraph curve; curve.x[0] = 0; curve.x[1] = 1; curve.y[0] = 0.05f; curve.y[1] = 0.95f; curve.curve[0] = 0.6f;
            REQUIRE(wavetable->setLfoCurve(0, curve));
        }
        next->prepare(48000, 512);
        auto old = std::move(processor); processor = std::move(next); synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        root->removeAllChildren(); editor = root->add<Editor>(synth, 0, 0, chain, child); editor->setBounds(root->localBounds()); editor->layout();
        editor->onChooseEffect = [this](auto choose) { choose(nextType); };
    }
    ui::MouseEvent at(ui::Rect cell, float x = 0.5f, float y = 0.5f) const
    { ui::MouseEvent event; event.x = cell.x + cell.w * x; event.y = cell.y + cell.h * y; return event; }
    void click(ui::Rect cell, float x = 0.5f)
    { REQUIRE(! cell.isEmpty()); const auto event = at(cell, x); root->dispatchMouseDown(event); root->dispatchMouseUp(event); }
    void fx() { click(editor->workflowCell(Editor::Page::Fx)); CHECK(editor->workflow() == Editor::Page::Fx); }
    void snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_fx_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        REQUIRE(gfx::renderToPng(*root, (int) root->width(), (int) root->height(), 1, directory.getChildFile(name).getFullPathName().toStdString()));
    }
};
} // namespace

TEST_CASE("wavetable FX UI: native rack input selects bypasses inserts reorders removes and undoes modules across rebuilds")
{
    FxUi rig; rig.fx(); rig.click(rig.editor->fxModuleCell(9)); CHECK(rig.editor->selectedFxModule() == 9);
    // Serum-style rack: every visible module has its own panel; clicking a panel's name column selects that module.
    REQUIRE(! rig.editor->fxPanelCell(9).isEmpty());
    const auto rack = rig.synth->effectsConfiguration();
    for (const auto& module : rack.buses[0])
        if (module.id != 9 && ! rig.editor->fxPanelCell(module.id).isEmpty())
        {
            const auto panel = rig.editor->fxPanelCell(module.id);
            rig.click({panel.x + 40, panel.y + panel.h - 12, 1, 1}); CHECK(rig.editor->selectedFxModule() == module.id);
            rig.click(rig.editor->fxModuleCell(9)); CHECK(rig.editor->selectedFxModule() == 9);
            break;
        }
    const int feedbackDescriptor = E::parameterIndex(E::Type::Delay, "Feedback");
    const int feedback = rig.synth->fxParameterIndex(9, feedbackDescriptor); REQUIRE(feedback >= W::ParameterCount);
    const float before = rig.synth->getParameter(feedback);
    auto wheel = rig.at(rig.editor->parameterCell(feedback)); wheel.wheelDeltaY = 1; rig.root->dispatchMouseWheel(wheel);
    CHECK(rig.synth->getParameter(feedback) > before);
    const int enabled = rig.synth->fxEnabledParameterIndex(9); rig.click(rig.editor->parameterCell(enabled));
    CHECK_NEAR(rig.synth->getParameter(enabled), 0, 0);
    rig.click(rig.editor->fxAddCell()); CHECK(rig.edits == 1); CHECK(rig.editor->workflow() == Editor::Page::Fx);
    const auto created = rig.editor->selectedFxModule(); CHECK(created == 1);
    auto graph = rig.synth->effectsConfiguration(); REQUIRE(graph.buses[0].size() == 3);
    CHECK(graph.buses[0][1].id == created); CHECK(graph.buses[0][1].type == E::Type::Chorus);
    CHECK(! graph.buses[0][0].enabled); CHECK(graph.buses[0][0].values[(size_t) feedbackDescriptor] > before);
    rig.click(rig.editor->fxMoveCell(1)); graph = rig.synth->effectsConfiguration(); CHECK(graph.buses[0][2].id == created);
    rig.click(rig.editor->fxMoveCell(-1)); graph = rig.synth->effectsConfiguration(); CHECK(graph.buses[0][1].id == created);
    rig.snapshot("Effects.png");
    rig.click(rig.editor->fxRemoveCell()); graph = rig.synth->effectsConfiguration(); CHECK(graph.buses[0].size() == 2);
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(rig.synth->effectsConfiguration().buses[0].size() == 3);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->effectsConfiguration().buses[0].size() == 2);
    rig.click(rig.editor->workflowCell(Editor::Page::Mix));
    for (int route = 0; route < W::FxRoutingParams; ++route) CHECK(! rig.editor->parameterCell(rig.synth->fxRoutingParameterIndex((W::FxRoutingParam) route)).isEmpty());
    const int send = rig.synth->fxRoutingParameterIndex(W::MainToBus1);
    auto down = rig.at(rig.editor->parameterCell(send)); rig.root->dispatchMouseDown(down);
    auto drag = down; drag.y -= 24; rig.root->dispatchMouseDrag(drag); rig.root->dispatchMouseUp(drag);
    CHECK(rig.synth->getParameter(send) > 0); rig.snapshot("Mixer.png");
    rig.click(rig.editor->fxBusCell(1)); CHECK(rig.editor->workflow() == Editor::Page::Fx); CHECK(rig.editor->fxBus() == 1);
    CHECK(rig.editor->selectedFxModule() == 4);
    rig.context.midiMapMode = true;
    const int decay = rig.synth->fxParameterIndex(4, E::parameterIndex(E::Type::Reverb, "Decay")); rig.click(rig.editor->parameterCell(decay));
    REQUIRE(rig.context.mapSelection); CHECK(rig.context.mapSelection->param == "Fx.4/Decay"); rig.midi.cancelLearn();
}

TEST_CASE("wavetable FX UI: split-band insertion and impulse drops retain exact stable IDs and nested rack destination")
{
    FxUi rig(initialEffects(), true); rig.fx(); rig.click(rig.editor->fxModuleCell(5));
    rig.nextType = E::Type::Distortion; rig.click(rig.editor->fxBandCell(1));
    const auto inserted = rig.editor->selectedFxModule(); auto configuration = rig.synth->effectsConfiguration();
    auto* split = findModule(configuration.buses[0], 5); REQUIRE(split); REQUIRE(split->bands[1].size() == 1);
    CHECK(split->bands[1][0].id == inserted); CHECK(split->bands[1][0].type == E::Type::Distortion);
    CHECK(split->bands[0][0].id == 40); CHECK(rig.editor->workflow() == Editor::Page::Fx);
    CHECK(! rig.song.tracks[0].devices[0].chains[1].devices[0].nativeContent);
    rig.click(rig.editor->fxModuleCell(5)); rig.snapshot("Split Bands.png");
    rig.click(rig.editor->fxBusCell(2)); CHECK(rig.editor->selectedFxModule() == 7);
    rig.context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-ui-impulse.wav", "Own impulse"};
    rig.root->dispatchMouseUp(rig.at(rig.editor->fxImpulseCell())); CHECK(rig.imports == 1);
    CHECK(rig.editor->fxBus() == 2); CHECK(rig.editor->selectedFxModule() == 7);
    configuration = rig.synth->effectsConfiguration(); REQUIRE(configuration.buses[2][0].impulse);
    CHECK(configuration.buses[2][0].impulse->name() == "Own UI impulse"); rig.snapshot("Convolution.png");
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(rig.editor->fxBus() == 2);
}

TEST_CASE("wavetable FX UI: compact inspector reaches every dynamic module and routing control and offers structural editing")
{
    E::Configuration configuration;
    for (int i = 0; i < E::maxBusModules; ++i) configuration.buses[0].push_back(effect((E::ModuleId) (i + 1), E::Type::Eq));
    configuration.buses[1] = {effect(100, E::Type::Convolution)};
    FxUi rig(configuration, false, 210); rig.fx(); CHECK(rig.editor->inspectorOpen());
    std::set<int> reached;
    for (int instance = 0; instance < rig.editor->instanceCount(); ++instance)
    {
        while (rig.editor->selectedInstance() != instance) rig.click(rig.editor->selectorCell(1));
        const int pages = rig.editor->pageCount();
        for (int page = 0; page < pages; ++page)
        {
            for (int parameter : rig.editor->visibleParameters())
            {
                reached.insert(parameter); const auto cell = rig.editor->parameterCell(parameter);
                CHECK(cell.w >= 80); CHECK(cell.bottom() <= rig.editor->height()); CHECK(cell.right() <= rig.editor->width());
            }
            if (pages > 1) rig.click(rig.editor->pageCell(1));
        }
    }
    CHECK(reached.size() == rig.synth->parameters().size() - (size_t) W::ParameterCount);
    CHECK(! rig.editor->fxAddCell().isEmpty()); CHECK(! rig.editor->fxImpulseCell().isEmpty());
    rig.context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-ui-impulse.wav", "Own impulse"};
    rig.root->dispatchMouseUp(rig.at(rig.editor->fxImpulseCell())); CHECK(rig.imports == 1);
    rig.snapshot("Compact Effects.png");
    rig.click(rig.editor->fxBusCell(0)); const int edits = rig.edits; rig.click(rig.editor->fxAddCell());
    CHECK(rig.edits == edits); CHECK(rig.context.statusText == "This effects chain is full");
    rig.root->setBounds({0, 0, 1080, 700}); rig.editor->setBounds(rig.root->localBounds()); rig.editor->layout();
    rig.click(rig.editor->workflowCell(Editor::Page::Fx)); CHECK(! rig.editor->fxChainPageCell(1).isEmpty());
    while (rig.editor->fxModuleCell(16).isEmpty()) rig.click(rig.editor->fxChainPageCell(1));
    rig.click(rig.editor->fxModuleCell(16)); CHECK(rig.editor->selectedFxModule() == 16); rig.snapshot("Paged Effects.png");
}

TEST_CASE("wavetable FX UI: added envelope endpoints and imported curve handles remain visible and use the prepared plot law")
{
    FxUi rig(initialEffects(), false, 700, true); rig.click(rig.editor->envelopeCell(3));
    CHECK(! rig.editor->parameterCell(W::envLevelParam(3, false)).isEmpty()); CHECK(! rig.editor->parameterCell(W::envLevelParam(3, true)).isEmpty());
    const auto plot = rig.editor->envelopePlot();
    CHECK(plot.y >= rig.editor->parameterCell(W::envLevelParam(3, false)).bottom());
    rig.click(rig.editor->lfoCell(0));
    const auto original = rig.synth->lfoCurve(0);
    dsp::ExponentialCurveGraph prepared; prepared.prepare(original);
    int edits = 0; dsp::CurveGraph edited;
    rig.context.editWavetableLfo = [&](int track, int device, int lfo, const dsp::CurveGraph& curve, int chain, int child) {
        CHECK(track == 0); CHECK(device == 0); CHECK(lfo == 0); CHECK(chain == -1); CHECK(child == -1); edited = curve; ++edits;
    };
    auto handle = rig.at(rig.editor->lfoPlot(), 0.5f, 1 - prepared.eval(0.5f)); rig.root->dispatchMouseDown(handle);
    auto drag = rig.at(rig.editor->lfoPlot(), 0.5f, 0.28f); rig.root->dispatchMouseDrag(drag); rig.root->dispatchMouseUp(drag);
    REQUIRE(edits == 1); CHECK(edited.count == 2); prepared.prepare(edited); CHECK_NEAR(prepared.eval(0.5f), 0.72f, 0.001);
    CHECK_NEAR(rig.synth->lfoCurve(0).curve[0], original.curve[0], 0);
    rig.snapshot("Envelope Endpoints.png");
}

TEST_CASE("wavetable FX UI: minimum dashboard and header preserve separate input regions and bus controls across rebuilds")
{
    FxUi rig(initialEffects(), false, 500);
    CHECK(rig.editor->workflowCell(Editor::Page::Global).right() < rig.editor->presetCell(-1).x);
    rig.click(rig.editor->workflowCell(Editor::Page::Mix));
    const auto send = rig.editor->parameterCell(rig.synth->fxRoutingParameterIndex(W::MainToBus1));
    CHECK(send.y > rig.editor->fxBusCell(0).bottom() + 20); CHECK(send.bottom() < rig.editor->envelopeCell(0).y);
    rig.snapshot("Minimum Mixer.png");
    rig.refresh(); CHECK(rig.editor->workflow() == Editor::Page::Mix); CHECK(rig.editor->section() == Editor::Section::Main);
    rig.fx(); rig.click(rig.editor->fxModuleCell(5)); rig.snapshot("Minimum Split.png");
    // Compact rack panel (UI-28h): Mix and the main controls on the dashboard, the rest one click away on •••.
    for (const char* id : {"Mix", "LowSplit", "HighSplit", "LowGain", "MidGain", "HighGain"})
    {
        const int index = rig.synth->fxParameterIndex(5, E::parameterIndex(E::Type::Split3, id));
        const auto cell = rig.editor->parameterCell(index); CHECK(! cell.isEmpty()); CHECK(cell.bottom() < rig.editor->envelopeCell(0).y);
    }
    REQUIRE(! rig.editor->fxPanelMoreCell(5).isEmpty());
    rig.click(rig.editor->fxPanelMoreCell(5));
    CHECK(rig.editor->inspectorOpen()); CHECK(rig.editor->section() == Editor::Section::Effects); CHECK(rig.editor->selectedFxModule() == 5);
    std::set<int> reached;
    for (int page = 0; page < rig.editor->pageCount(); ++page)
    {
        for (int parameter : rig.editor->visibleParameters()) reached.insert(parameter);
        if (rig.editor->pageCount() > 1) rig.click(rig.editor->pageCell(1));
    }
    for (const auto& descriptor : E::parameterDescriptors(E::Type::Split3))
        CHECK(reached.contains(rig.synth->fxParameterIndex(5, E::parameterIndex(E::Type::Split3, descriptor.id))));
}

TEST_CASE("wavetable FX UI: Serum-style rack rows hide and delete modules; panels carry each effect's graph")
{
    E::Configuration configuration;
    configuration.buses[0] = {effect(1, E::Type::Delay), effect(2, E::Type::Reverb), effect(3, E::Type::Distortion), effect(4, E::Type::Eq),
                              effect(5, E::Type::Filter), effect(6, E::Type::Compressor)};
    FxUi rig(configuration, false, 900); rig.fx();
    rig.click(rig.editor->fxModuleCell(1)); CHECK(rig.editor->selectedFxModule() == 1);
    REQUIRE(! rig.editor->fxPanelCell(1).isEmpty()); REQUIRE(! rig.editor->fxPanelCell(2).isEmpty());
    rig.snapshot("Serum Rack.png");
    // Hide: the row's eye collapses the panel to a header strip; again restores it. The panel gutter does the same.
    REQUIRE(! rig.editor->fxRowHideCell(2).isEmpty());
    const float open = rig.editor->fxPanelCell(2).h;
    rig.click(rig.editor->fxRowHideCell(2)); CHECK(rig.editor->fxCollapsed(2)); CHECK(rig.editor->fxPanelCell(2).h < 30); CHECK(open > 60);
    rig.click(rig.editor->fxRowHideCell(2)); CHECK(! rig.editor->fxCollapsed(2));
    const auto panel = rig.editor->fxPanelCell(1);
    rig.click({panel.x + 4, panel.y + 30, 10, 10}); CHECK(rig.editor->fxCollapsed(1));
    rig.click({panel.x + 4, panel.y + 6, 10, 10}); CHECK(! rig.editor->fxCollapsed(1));
    // Turn off: the row's power switch is the module's On parameter.
    const int enabled = rig.synth->fxEnabledParameterIndex(3);
    const float before = rig.synth->getParameter(enabled);
    rig.click(rig.editor->parameterCell(enabled)); CHECK_NEAR(rig.synth->getParameter(enabled), before > 0.5f ? 0 : 1, 0);
    // Delete: the row's ✕ removes that module (one undoable edit).
    const int edits = rig.edits;
    REQUIRE(! rig.editor->fxRowRemoveCell(4).isEmpty());
    rig.click(rig.editor->fxRowRemoveCell(4));
    CHECK(rig.edits == edits + 1);
    const auto graph = rig.synth->effectsConfiguration();
    CHECK(std::none_of(graph.buses[0].begin(), graph.buses[0].end(), [](const auto& module) { return module.id == 4; }));
    CHECK(graph.buses[0].size() == 5);
}
