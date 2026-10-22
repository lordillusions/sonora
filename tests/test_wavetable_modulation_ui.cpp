#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/ExponentialCurveGraph.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/RootWidget.h"
#include "ui/views/WavetableModulationEditor.h"
#include "ui/views/WavetableEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <set>

using namespace sonora;
namespace
{
using Editor = ui::WavetableModulationEditor;
using Field = Editor::Field;
constexpr int fieldCount = static_cast<int>(Field::Count);
struct Row
{
    std::array<float, fieldCount> values{1, 0, 0, 0.5f, 0, 1};
    std::array<Editor::CurveSnapshot, 2> curves;
    std::string destination = "Osc.0/Position";
};
struct Bank
{
    std::array<Row, 64> rows;
    std::vector<uint8_t> retainedSource{0x51, 0x31, 0, 0xa5, 0x72}; // synthetic opaque source bytes
};
struct MatrixUi
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    ui::UiContext context;
    std::unique_ptr<ui::RootWidget> root;
    Editor* editor = nullptr;
    std::shared_ptr<const Bank> state = std::make_shared<Bank>();
    std::vector<std::shared_ptr<const Bank>> history;
    Editor::ViewState view;
    std::vector<Editor::Target> catalog{{"", "Off", ""}, {"Osc.0/Position", "Osc A · Position", "Oscillators"},
        {"Fx.7/Feedback", "Delay #7 · Feedback", "Main FX"}, {"Fx.40/Cutoff", "Filter #40 · Cutoff", "Aux 1 FX"}};
    int curveEdits = 0, targetEdits = 0, contexts = 0, contextIndex = -1;
    std::string chosenTarget = "Fx.40/Cutoff";
    MatrixUi(float width = 1080, float height = 300)
    {
        context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, width, height}); rebuild();
    }
    void rebuild()
    {
        Editor::Binding binding;
        binding.parameter = [](int slot, Field field) {
            const std::array<const char*, fieldCount> ids{"Source", "AuxSource", "Destination", "Amount", "Bipolar", "Out"};
            Editor::ParameterBinding parameter; parameter.index = slot * fieldCount + static_cast<int>(field);
            parameter.info = devices::makeParam("Mod." + std::to_string(slot) + "/" + ids[static_cast<size_t>(field)], ids[static_cast<size_t>(field)],
                                                field == Field::Amount ? -1.0f : 0.0f, 1, field == Field::Out ? 1.0f : 0.0f);
            if (field == Field::Source || field == Field::Via)
            { parameter.info.choices = {"Off", "LFO 1", "Env 2", "Velocity", "Macro 8"}; parameter.info.maxValue = 4; }
            parameter.info.isToggle = field == Field::Bipolar;
            parameter.target = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, 0, 0, 0, parameter.info.id}; return parameter;
        };
        binding.getParameter = [this](int index) { return state->rows[static_cast<size_t>(index / fieldCount)].values[static_cast<size_t>(index % fieldCount)]; };
        binding.setParameter = [this](int index, float value) {
            auto next = std::make_shared<Bank>(*state); next->rows[static_cast<size_t>(index / fieldCount)].values[static_cast<size_t>(index % fieldCount)] = value; state = next;
        };
        binding.curve = [this](int slot, bool auxiliary) { return state->rows[static_cast<size_t>(slot)].curves[auxiliary ? 1u : 0u]; };
        binding.destination = [this](int slot) { return state->rows[static_cast<size_t>(slot)].destination; };
        binding.targets = [this] { return catalog; };
        binding.onViewChange = [this](const auto& next) { view = next; };
        binding.parameterContext = [this](int index) { ++contexts; contextIndex = index; };
        binding.commitCurve = [this](int slot, bool auxiliary, const dsp::CurveGraph& graph, bool enabled) {
            auto next = std::make_shared<Bank>(*state); auto& edited = next->rows[static_cast<size_t>(slot)].curves[auxiliary ? 1u : 0u];
            edited.graph = graph; edited.enabled = enabled; history.push_back(state); state = next; ++curveEdits; rebuild();
        };
        binding.commitDestination = [this](int slot, const std::string& id) {
            auto next = std::make_shared<Bank>(*state); next->rows[static_cast<size_t>(slot)].destination = id;
            history.push_back(state); state = next; ++targetEdits; rebuild();
        };
        root->removeAllChildren(); editor = root->add<Editor>(std::move(binding)); editor->setBounds(root->localBounds()); editor->layout(); editor->restoreViewState(view);
        editor->onChooseDestination = [this](const auto& targets, const auto&, auto choose) {
            CHECK(targets.size() == catalog.size()); choose(chosenTarget);
        };
    }
    ui::MouseEvent at(ui::Rect cell, float x = 0.5f, float y = 0.5f) const
    { ui::MouseEvent event; event.x = cell.x + cell.w * x; event.y = cell.y + cell.h * y; return event; }
    void click(ui::Rect cell, float x = 0.5f)
    { REQUIRE(! cell.isEmpty()); const auto event = at(cell, x); root->dispatchMouseDown(event); root->dispatchMouseUp(event); }
    void open(int slot, bool auxiliary = false) { click(editor->curveCell(slot, auxiliary)); CHECK(editor->viewState().curveOpen); }
    void snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_modulation_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1,
                                 directory.getChildFile(name).getFullPathName().toStdString()));
    }
};

struct NativeMatrixUi
{
    using W = devices::WavetableSynth;
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    model::EditHistory history;
    ui::UiContext context;
    std::unique_ptr<ui::RootWidget> root;
    std::unique_ptr<devices::DeviceProcessor> processor;
    W* synth = nullptr;
    ui::WavetableEditor* editor = nullptr;
    int chain = -1, child = -1, commits = 0;
    explicit NativeMatrixUi(bool nested = false, float height = 700, bool imported = false)
    {
        auto target = devices::nativeDeviceModel("SonoraWavetable"); target.id = nested ? 77 : 42;
        auto curve = dsp::CurveGraph::preset(3); curve.curve[0] = -0.5f;
        std::string error; REQUIRE(devices::storeWavetableModulationCurve(target, 63, false, curve, true, error));
        REQUIRE(devices::storeWavetableModulationCurve(target, 63, true, dsp::CurveGraph::preset(3), false, error));
        auto content = std::make_shared<model::NativeContent>(*target.nativeContent); content->controllerData = {0x51, 0, 0x73, 0xa5};
        auto json = juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(content->data.data()), static_cast<int>(content->data.size())));
        REQUIRE(json.isObject()); json.getDynamicObject()->setProperty("ownUntouchedMetadata", "retain-ui-proof");
        if (imported)
        {
            auto* entries = json.getProperty("modulationCurves", {}).getArray(); REQUIRE(entries);
            for (auto& entry : *entries) entry.getDynamicObject()->setProperty("law", "exponential");
        }
        const auto serialized = juce::JSON::toString(json, true); const auto* begin = reinterpret_cast<const uint8_t*>(serialized.toRawUTF8());
        content->data.assign(begin, begin + serialized.getNumBytesAsUTF8()); target.nativeContent = content;
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        if (nested)
        {
            auto rack = devices::nativeDeviceModel("InstrumentGroupDevice"); rack.id = 42; rack.chains.resize(1); rack.chains[0].devices.resize(3);
            rack.chains[0].devices[0] = devices::nativeDeviceModel("SonoraWavetable"); rack.chains[0].devices[0].id = 88;
            rack.chains[0].devices[2] = std::move(target); song.tracks[0].devices = {std::move(rack)}; chain = 0; child = 2;
        }
        else song.tracks[0].devices = {std::move(target)};
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        context.editWavetableModulationCurve = [this](int track, int device, int slot, bool auxiliary, const dsp::CurveGraph& graph, bool enabled, int c, int ch) {
            CHECK(track == 0); CHECK(device == 0); CHECK(slot == 63); CHECK(c == chain); CHECK(ch == child);
            auto replacement = song.tracks[0].devices[0]; auto& edited = targetIn(replacement);
            devices::captureDeviceState(edited, processor.get()); std::string reason;
            REQUIRE(devices::storeWavetableModulationCurve(edited, slot, auxiliary, graph, enabled, reason));
            history.perform(song, model::replaceDevice(song, 0, 0, std::move(replacement), "Edit modulation curve")); ++commits; refresh();
        };
        refresh();
    }
    model::Device& targetIn(model::Device& device) const
    { return chain < 0 ? device : device.chains[static_cast<size_t>(chain)].devices[static_cast<size_t>(child)]; }
    model::Device& target() { return targetIn(song.tracks[0].devices[0]); }
    void refresh()
    {
        std::vector<std::string> warnings; auto next = devices::createNative(target(), &warnings); REQUIRE(next); CHECK(warnings.empty()); next->prepare(48000, 512);
        auto old = std::move(processor); processor = std::move(next); synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        root->removeAllChildren(); editor = root->add<ui::WavetableEditor>(synth, 0, 0, chain, child); editor->setBounds(root->localBounds()); editor->layout();
    }
    ui::MouseEvent at(ui::Rect area, float x = 0.5f, float y = 0.5f) const
    { ui::MouseEvent event; event.x = area.x + x * area.w; event.y = area.y + y * area.h; return event; }
    void click(ui::Rect area, float x = 0.5f)
    { REQUIRE(! area.isEmpty()); const auto event = at(area, x); root->dispatchMouseDown(event); root->dispatchMouseUp(event); }
    ui::Rect inMatrix(ui::Rect area) const
    { const auto bounds = editor->modulationEditor()->bounds(); return area.translated(bounds.x, bounds.y); }
    void matrix()
    {
        click(editor->workflowCell(ui::WavetableEditor::Page::Matrix)); REQUIRE(editor->modulationEditor()->visible);
        for (int page = 0; page < editor->modulationEditor()->pageCount() && editor->parameterCell(W::outParam(63)).isEmpty(); ++page)
            click(editor->matrixBankCell(1));
        REQUIRE(! editor->parameterCell(W::outParam(63)).isEmpty());
    }
    void snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_modulation_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1,
                                 directory.getChildFile(name).getFullPathName().toStdString()));
    }
};
} // namespace

TEST_CASE("wavetable modulation UI: compact paging exposes all64 rows and plain Source Via Amount Bipolar Out controls")
{
    MatrixUi rig(620, 154); std::set<int> visited;
    for (int page = 0; page < rig.editor->pageCount(); ++page)
    {
        for (int slot : rig.editor->visibleSlots())
        {
            visited.insert(slot);
            for (int field = 0; field < fieldCount; ++field)
            {
                const auto area = rig.editor->parameterCell(slot, static_cast<Field>(field)); CHECK(! area.isEmpty());
                CHECK(area.right() <= rig.editor->width()); CHECK(area.bottom() <= rig.editor->height() - 23); CHECK(area.w >= 35);
            }
            CHECK(! rig.editor->curveCell(slot, false).isEmpty()); CHECK(! rig.editor->curveCell(slot, true).isEmpty());
        }
        rig.click(rig.editor->pageCell(1));
    }
    CHECK(visited.size() == 64); CHECK(rig.editor->viewState().page == 0);
    rig.click(rig.editor->parameterCell(0, Field::Source), 0.98f); CHECK_NEAR(rig.state->rows[0].values[0], 2, 0);
    auto via = rig.at(rig.editor->parameterCell(0, Field::Via)); via.wheelDeltaY = 1; rig.root->dispatchMouseWheel(via);
    CHECK_NEAR(rig.state->rows[0].values[1], 1, 0);
    int choices = 0;
    rig.context.showChoices = [&](const auto& labels, const auto& ticked, auto pick) { CHECK(labels.size() == 5); CHECK(ticked[2]); ++choices; pick(4); };
    rig.click(rig.editor->parameterCell(0, Field::Source)); CHECK(choices == 1); CHECK_NEAR(rig.state->rows[0].values[0], 4, 0);
    rig.click(rig.editor->parameterCell(0, Field::Bipolar)); CHECK_NEAR(rig.state->rows[0].values[4], 1, 0);
    auto down = rig.at(rig.editor->parameterCell(0, Field::Amount)); rig.root->dispatchMouseDown(down);
    auto move = down; move.y -= 20; rig.root->dispatchMouseDrag(move); rig.root->dispatchMouseUp(move); CHECK_NEAR(rig.state->rows[0].values[3], 0.7, 0.001);
    auto out = rig.at(rig.editor->parameterCell(0, Field::Out)); out.shift = true; out.wheelDeltaY = -1; rig.root->dispatchMouseWheel(out);
    CHECK_NEAR(rig.state->rows[0].values[5], 0.995, 0.00001); CHECK_NEAR(rig.state->rows[0].values[3], 0.7, 0.001);
    out.clicks = 2; rig.root->dispatchMouseDown(out); rig.root->dispatchMouseUp(out); CHECK_NEAR(rig.state->rows[0].values[5], 1, 0);
    rig.snapshot("Compact Rows.png");
}

TEST_CASE("wavetable modulation UI: curve gestures commit once through immutable snapshots and survive synchronous view rebuild undo")
{
    MatrixUi rig; const auto original = rig.state; rig.open(0); rig.click(rig.editor->curveEnableCell(false));
    CHECK(rig.curveEdits == 1); CHECK(rig.state->rows[0].curves[0].enabled); CHECK(! original->rows[0].curves[0].enabled);
    auto insert = rig.at(rig.editor->plot(), 0.25f, 0.2f); insert.clicks = 2; rig.root->dispatchMouseDown(insert); rig.root->dispatchMouseUp(insert);
    CHECK(rig.curveEdits == 2); CHECK(rig.state->rows[0].curves[0].graph.count == 3); CHECK(rig.editor->viewState().curveOpen);
    const auto beforeDrag = rig.state; auto point = rig.at(rig.editor->plot(), 0.25f, 0.2f); rig.root->dispatchMouseDown(point);
    auto drag = rig.at(rig.editor->plot(), 0.62f, 0.67f); drag.alt = true; rig.root->dispatchMouseDrag(drag);
    CHECK(rig.state == beforeDrag); CHECK(rig.curveEdits == 2); rig.root->dispatchMouseUp(drag); CHECK(rig.curveEdits == 3);
    CHECK_NEAR(rig.state->rows[0].curves[0].graph.x[1], 0.625, 0.001); CHECK_NEAR(rig.state->rows[0].curves[0].graph.y[1], 0.375, 0.001);
    CHECK(rig.state->retainedSource == original->retainedSource); rig.snapshot("Source Curve.png");
    rig.state = rig.history.back(); rig.history.pop_back(); rig.rebuild(); CHECK_NEAR(rig.state->rows[0].curves[0].graph.x[1], 0.25, 0.001);
    rig.click(rig.editor->curveSelectorCell(true)); CHECK(rig.editor->viewState().auxiliary); rig.click(rig.editor->curveEnableCell(true));
    CHECK(rig.state->rows[0].curves[1].enabled); CHECK(rig.state->rows[0].curves[0].graph.count == 3);
    rig.click(rig.editor->resetCell()); CHECK(rig.state->rows[0].curves[1].graph.count == 2); CHECK(rig.state->rows[0].curves[0].graph.count == 3);
    rig.click(rig.editor->slotCell(-1)); CHECK(rig.editor->viewState().selectedSlot == 63); rig.click(rig.editor->closeCell());
    CHECK(rig.editor->visibleSlots().back() == 63); rig.snapshot("Last Routes.png");
}

TEST_CASE("wavetable modulation UI: imported primary and auxiliary handles follow the prepared exponential law independently")
{
    MatrixUi rig; auto imported = std::make_shared<Bank>(*rig.state);
    for (int auxiliary = 0; auxiliary < 2; ++auxiliary)
    {
        auto& curve = imported->rows[0].curves[static_cast<size_t>(auxiliary)]; curve.enabled = true; curve.law = Editor::CurveLaw::Imported;
        curve.graph.curve[0] = auxiliary == 0 ? -0.5f : 0.5f;
    }
    rig.state = imported; rig.rebuild(); rig.open(0);
    for (int auxiliary = 0; auxiliary < 2; ++auxiliary)
    {
        if (auxiliary > 0) rig.click(rig.editor->curveSelectorCell(true));
        const auto before = rig.state; dsp::ExponentialCurveGraph curve; curve.prepare(before->rows[0].curves[static_cast<size_t>(auxiliary)].graph);
        CHECK_NEAR(curve.eval(0.5f), auxiliary == 0 ? 0.9375 : 0.0625, 0.0001);
        auto handle = rig.at(rig.editor->plot(), 0.5f, 1 - curve.eval(0.5f)); rig.root->dispatchMouseDown(handle);
        auto drag = rig.at(rig.editor->plot(), 0.5f, 0.28f); rig.root->dispatchMouseDrag(drag); CHECK(rig.state == before);
        rig.root->dispatchMouseUp(drag); CHECK(rig.curveEdits == auxiliary + 1);
        curve.prepare(rig.state->rows[0].curves[static_cast<size_t>(auxiliary)].graph); CHECK_NEAR(curve.eval(0.5f), 0.72, 0.001);
        CHECK_NEAR(before->rows[0].curves[static_cast<size_t>(auxiliary)].graph.curve[0], auxiliary == 0 ? -0.5f : 0.5f, 0);
    }
    rig.snapshot("Imported Via Curve.png");
}

TEST_CASE("wavetable modulation UI: stable FX destinations survive catalog reorder and missing targets and Out exposes exact MIDI mapping")
{
    MatrixUi rig; rig.click(rig.editor->parameterCell(0, Field::Destination)); CHECK(rig.targetEdits == 1);
    CHECK(rig.state->rows[0].destination == "Fx.40/Cutoff");
    std::reverse(rig.catalog.begin(), rig.catalog.end()); rig.catalog.back().label = "Off renamed"; rig.rebuild();
    CHECK(rig.state->rows[0].destination == "Fx.40/Cutoff");
    rig.catalog.erase(std::remove_if(rig.catalog.begin(), rig.catalog.end(), [](const auto& target) { return target.id == "Fx.40/Cutoff"; }), rig.catalog.end());
    rig.rebuild(); rig.snapshot("Missing Stable Target.png"); CHECK(rig.state->rows[0].destination == "Fx.40/Cutoff");
    const int edits = rig.targetEdits; rig.chosenTarget = "Fx.999/Unknown"; rig.click(rig.editor->parameterCell(0, Field::Destination)); CHECK(rig.targetEdits == edits);
    while (rig.editor->visibleSlots().back() != 63) rig.click(rig.editor->pageCell(1));
    rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(63, Field::Out));
    REQUIRE(rig.context.mapSelection); CHECK(rig.context.mapSelection->param == "Mod.63/Out"); rig.midi.cancelLearn(); rig.context.midiMapMode = false;
    auto context = rig.at(rig.editor->parameterCell(63, Field::Out)); context.right = true;
    rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context); CHECK(rig.contexts == 1); CHECK(rig.contextIndex == 63 * fieldCount + static_cast<int>(Field::Out));
}

TEST_CASE("wavetable modulation UI: cancelling a curve drag leaves immutable source unchanged and minimum pane fits")
{
    MatrixUi rig(620, 154); rig.open(0); const auto original = rig.state;
    auto handle = rig.at(rig.editor->plot()); rig.root->dispatchMouseDown(handle);
    auto drag = rig.at(rig.editor->plot(), 0.5f, 0.2f); rig.root->dispatchMouseDrag(drag); rig.root->setKeyboardFocus(nullptr); rig.root->dispatchMouseUp(drag);
    CHECK(rig.curveEdits == 0); CHECK(rig.state == original);
    CHECK(rig.editor->plot().bottom() < rig.editor->height() - 23); CHECK(rig.editor->curveEnableCell(true).right() <= rig.editor->width());
    rig.snapshot("Minimum Curve.png");
    ui::KeyEvent escape; escape.keyCode = juce::KeyPress::escapeKey; CHECK(rig.root->dispatchKey(escape)); CHECK(! rig.editor->viewState().curveOpen);
}

TEST_CASE("wavetable modulation UI: native dashboard binds real Out destination and Anchor controls and publishes immutable curve undo")
{
    using W = NativeMatrixUi::W;
    NativeMatrixUi rig; rig.matrix();
    const int source = W::modParam(63, W::ModSource), amount = W::modParam(63, W::ModAmount), out = W::outParam(63);
    rig.click(rig.editor->parameterCell(source), 0.98f); CHECK_NEAR(rig.synth->getParameter(source), W::Env1, 0);
    rig.synth->setParameter(amount, 0.6f); auto wheel = rig.at(rig.editor->parameterCell(out)); wheel.wheelDeltaY = -1; rig.root->dispatchMouseWheel(wheel);
    CHECK_NEAR(rig.synth->getParameter(out), 0.95, 0.0001); CHECK_NEAR(rig.synth->getParameter(amount), 0.6, 0.0001);
    rig.editor->modulationEditor()->onChooseDestination = [](const auto& targets, const auto&, auto pick) {
        CHECK(targets.size() == W::NumDestinations);
        CHECK(std::none_of(targets.begin(), targets.end(), [](const auto& target) { return target.id.starts_with("fx/"); }));
        pick("native/" + std::to_string(W::Osc3GrainDensity));
    };
    rig.click(rig.editor->parameterCell(W::modParam(63, W::ModDestination)));
    CHECK_NEAR(rig.synth->getParameter(W::modParam(63, W::ModDestination)), W::Osc3GrainDensity, 0);
    const int anchor = W::lfoAnchorParam(0); const float beforeAnchor = rig.synth->getParameter(anchor);
    REQUIRE(! rig.editor->parameterCell(anchor).isEmpty()); rig.click(rig.editor->parameterCell(anchor));
    CHECK_NEAR(rig.synth->getParameter(anchor), beforeAnchor > 0.5f ? 0 : 1, 0); rig.snapshot("Native Matrix Rows.png");
    const auto before = rig.target().nativeContent;
    rig.click(rig.inMatrix(rig.editor->modulationEditor()->curveCell(63, false)));
    auto insert = rig.at(rig.inMatrix(rig.editor->modulationEditor()->plot()), 0.25f, 0.2f); insert.clicks = 2;
    rig.root->dispatchMouseDown(insert); rig.root->dispatchMouseUp(insert); CHECK(rig.commits == 1);
    CHECK(rig.synth->modulationCurve(63, false).graph.count == 3); CHECK(rig.target().nativeContent != before);
    CHECK(rig.target().nativeContent->controllerData == before->controllerData); CHECK(rig.editor->modulationEditor()->viewState().curveOpen);
    CHECK(rig.editor->modulationEditor()->viewState().selectedSlot == 63); CHECK(rig.editor->workflow() == ui::WavetableEditor::Page::Matrix);
    CHECK_NEAR(rig.synth->getParameter(out), 0.95, 0.0001); CHECK_NEAR(rig.synth->getParameter(amount), 0.6, 0.0001);
    rig.snapshot("Native Source Curve.png"); REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(rig.synth->modulationCurve(63, false).graph.count == 2);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->modulationCurve(63, false).graph.count == 3);
}

TEST_CASE("wavetable modulation UI: compact native rack curve publication retains imported law exact destination and independent auxiliary enable")
{
    using W = NativeMatrixUi::W;
    NativeMatrixUi rig(true, 210, true); rig.matrix(); rig.snapshot("Native Compact Matrix.png");
    const auto before = rig.target().nativeContent; rig.click(rig.inMatrix(rig.editor->modulationEditor()->curveCell(63, false)));
    CHECK(rig.synth->modulationCurve(63, false).law == W::CurveLaw::ImportedExponential);
    auto handle = rig.at(rig.inMatrix(rig.editor->modulationEditor()->plot()), 0.5f, 0.0625f); rig.root->dispatchMouseDown(handle);
    auto drag = rig.at(rig.inMatrix(rig.editor->modulationEditor()->plot()), 0.5f, 0.28f); rig.root->dispatchMouseDrag(drag); rig.root->dispatchMouseUp(drag);
    CHECK(rig.commits == 1); CHECK_NEAR(rig.synth->modulationCurveAt(63, false, 0.5f), 0.72, 0.001);
    CHECK(rig.synth->modulationCurve(63, false).law == W::CurveLaw::ImportedExponential); CHECK(before->controllerData == rig.target().nativeContent->controllerData);
    rig.click(rig.inMatrix(rig.editor->modulationEditor()->curveEnableCell(true)));
    CHECK(rig.commits == 2); CHECK(rig.synth->modulationCurve(63, true).enabled); CHECK(rig.synth->modulationCurve(63, false).enabled);
    CHECK(! rig.song.tracks[0].devices[0].chains[0].devices[0].nativeContent);
    CHECK(rig.context.wavetableModulationViews.contains(77)); CHECK(rig.context.wavetableModulationViews.at(77).auxiliary);
    rig.snapshot("Native Compact Via Curve.png");
    rig.click(rig.inMatrix(rig.editor->modulationEditor()->closeCell())); REQUIRE(! rig.editor->parameterCell(W::outParam(63)).isEmpty());
}

TEST_CASE("wavetable modulation UI: native LFO10x and mono legato portamento controls fit the minimum dashboard and Global page")
{
    using W = NativeMatrixUi::W;
    NativeMatrixUi rig(false, 500); rig.click(rig.editor->lfoCell(9));
    const int anchor = W::lfoAnchorParam(9), rate10x = W::lfoRateScaleParam(9);
    REQUIRE(! rig.editor->parameterCell(anchor).isEmpty()); REQUIRE(! rig.editor->parameterCell(rate10x).isEmpty());
    rig.click(rig.editor->parameterCell(rate10x)); CHECK_NEAR(rig.synth->getParameter(rate10x), 1, 0);
    CHECK_NEAR(rig.synth->getParameter(W::lfoRateScaleParam(0)), 0, 0);
    for (int parameter = 0; parameter < W::VoiceModeParams; ++parameter)
    {
        const auto area = rig.editor->parameterCell(W::voiceModeParam(static_cast<W::VoiceModeParam>(parameter)));
        CHECK(! area.isEmpty()); CHECK(area.right() <= rig.editor->width()); CHECK(area.bottom() < rig.editor->height() - 42);
    }
    CHECK(rig.editor->parameterCell(W::voiceModeParam(W::MonoOn)).bottom() < rig.editor->parameterCell(W::voiceModeParam(W::PortaTime)).y);
    CHECK(rig.editor->parameterCell(W::voiceModeParam(W::PortaTime)).bottom() < rig.editor->parameterCell(W::voiceModeParam(W::PortaAlways)).y);
    CHECK(rig.editor->parameterCell(W::voiceModeParam(W::PortaAlways)).bottom() < rig.editor->parameterCell(W::voiceModeParam(W::NotePriority)).y);
    rig.click(rig.editor->parameterCell(W::voiceModeParam(W::MonoOn))); rig.click(rig.editor->parameterCell(W::voiceModeParam(W::LegatoOn)));
    CHECK_NEAR(rig.synth->getParameter(W::voiceModeParam(W::MonoOn)), 1, 0); CHECK_NEAR(rig.synth->getParameter(W::voiceModeParam(W::LegatoOn)), 1, 0);
    auto porta = rig.at(rig.editor->parameterCell(W::voiceModeParam(W::PortaTime))); rig.root->dispatchMouseDown(porta);
    auto drag = porta; drag.y -= 48; rig.root->dispatchMouseDrag(drag); rig.root->dispatchMouseUp(drag);
    CHECK(rig.synth->getParameter(W::voiceModeParam(W::PortaTime)) > 0); CHECK(rig.synth->getParameter(W::voiceModeParam(W::PortaTime)) < 0.05f);
    rig.snapshot("Native Voice Minimum.png"); rig.click(rig.editor->workflowCell(ui::WavetableEditor::Page::Global));
    for (int parameter = 0; parameter < W::VoiceModeParams; ++parameter)
    {
        const auto area = rig.editor->parameterCell(W::voiceModeParam(static_cast<W::VoiceModeParam>(parameter)));
        CHECK(! area.isEmpty()); CHECK(area.bottom() <= rig.editor->envelopeCell(0).y);
    }
    rig.snapshot("Native Global Voice.png");
}
