#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/views/MainLayout.h"
#include "ui/views/WavetableEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <set>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
using Section = Editor::Section;

template <typename T> void collect(ui::Widget& widget, std::vector<T*>& result)
{
    if (auto* found = dynamic_cast<T*>(&widget)) result.push_back(found);
    for (const auto& child : widget.children()) collect(*child, result);
}

struct WavetableUi
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    Editor* editor = nullptr;
    W* processor = nullptr;

    explicit WavetableUi(float detailHeight = 700)
    {
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        song.tracks[0].devices = {devices::nativeDeviceModel("SonoraWavetable")};
        song.tracks[0].devices[0].id = 42;
        devices::BuildContext build; graph = engine::buildGraph(song, build, samples).graph;
        REQUIRE(graph); graph->prepare(48000, 512);
        processor = dynamic_cast<W*>(graph->nodes[0].processorForModelDevice[0]); REQUIRE(processor);
        context.song = &song; context.graph = graph.get(); context.controls = graph->controls; context.midi = &midi;
        context.browserVisible = false; context.detailTab = ui::DetailTab::Devices;
        context.theme.metrics.detailHeight = detailHeight;
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1400, 1000}); root->layout();
        std::vector<Editor*> editors; collect(*root, editors); REQUIRE(editors.size() == 1); editor = editors[0];
    }
    ui::MouseEvent at(ui::Rect cell, float fractionX = 0.5f, float fractionY = 0.5f) const
    {
        const auto origin = editor->boundsInRoot();
        ui::MouseEvent event; event.x = origin.x + cell.x + cell.w * fractionX; event.y = origin.y + cell.y + cell.h * fractionY;
        return event;
    }
    void click(ui::Rect cell, int clicks = 1, float fractionX = 0.5f)
    {
        REQUIRE(! cell.isEmpty());
        auto event = at(cell, fractionX); event.clicks = clicks;
        root->dispatchMouseDown(event); root->dispatchMouseUp(event);
    }
    void section(Section value)
    {
        if (! editor->inspectorOpen()) click(editor->inspectorCell());
        click(editor->sectionCell(value)); CHECK(editor->section() == value);
    }
    void select(int instance)
    {
        while (editor->selectedInstance() != instance) click(editor->selectorCell(1));
    }
    void reveal(int parameter)
    {
        const int pages = editor->pageCount();
        for (int i = 0; i < pages && editor->parameterCell(parameter).isEmpty(); ++i) click(editor->pageCell(1));
        REQUIRE(! editor->parameterCell(parameter).isEmpty());
    }
};
} // namespace

TEST_CASE("wavetable UI: native controls select oscillators, change table, drag/reset knobs and edit later modulation")
{
    WavetableUi rig;
    CHECK(rig.editor->section() == Section::Oscillators);
    CHECK_NEAR(rig.editor->width(), Editor::kPreferredWidth, 0.1);
    REQUIRE(! rig.editor->oscillatorPlot(2).isEmpty());
    rig.click(rig.editor->oscillatorPlot(2)); CHECK(rig.editor->selectedInstance() == 2);
    CHECK(rig.context.selectedDeviceId == 42);
    const int table = W::oscParam(2, W::Table), position = W::oscParam(2, W::Position), enabled = W::oscParam(2, W::On);
    rig.reveal(table); const float tableBefore = rig.processor->getParameter(table);
    rig.click(rig.editor->parameterCell(table), 1, 0.98f);
    CHECK_NEAR(rig.processor->getParameter(table), tableBefore + 1, 0);
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(0, W::Table)), rig.processor->parameters()[(size_t) W::oscParam(0, W::Table)].defaultValue, 0);

    rig.reveal(position);
    const auto cell = rig.editor->parameterCell(position);
    auto down = rig.at(cell); rig.root->dispatchMouseDown(down);
    auto up = down; up.y -= 40; rig.root->dispatchMouseDrag(up); rig.root->dispatchMouseUp(up);
    CHECK(rig.processor->getParameter(position) > rig.processor->parameters()[(size_t) position].defaultValue);
    rig.click(rig.editor->parameterCell(position), 2);
    CHECK_NEAR(rig.processor->getParameter(position), rig.processor->parameters()[(size_t) position].defaultValue, 0);
    rig.reveal(enabled); const float onBefore = rig.processor->getParameter(enabled);
    rig.click(rig.editor->parameterCell(enabled)); CHECK_NEAR(rig.processor->getParameter(enabled), onBefore > 0.5f ? 0 : 1, 0);

    rig.section(Section::Lfos); rig.select(9);
    const int rate = W::lfoParam(9, W::Rate); rig.reveal(rate);
    const float rateBefore = rig.processor->getParameter(rate);
    auto wheel = rig.at(rig.editor->parameterCell(rate)); wheel.wheelDeltaY = 1; rig.root->dispatchMouseWheel(wheel);
    CHECK(rig.processor->getParameter(rate) > rateBefore);
    rig.section(Section::Envelopes); rig.select(3); rig.reveal(W::envParam(3, W::Release));
    const int attack = W::envParam(3, W::Attack); rig.reveal(attack);
    auto attackDown = rig.at(rig.editor->parameterCell(attack)); rig.root->dispatchMouseDown(attackDown);
    auto attackUp = attackDown; attackUp.y -= 10; rig.root->dispatchMouseDrag(attackUp); rig.root->dispatchMouseUp(attackUp);
    CHECK(rig.processor->getParameter(attack) > rig.processor->parameters()[(size_t) attack].defaultValue);
    CHECK(rig.processor->getParameter(attack) < 0.05f); // millisecond edits remain practical over the 32-second range
    rig.click(rig.editor->parameterCell(attack), 2);
    CHECK_NEAR(rig.processor->getParameter(attack), rig.processor->parameters()[(size_t) attack].defaultValue, 0);
    rig.section(Section::Filters); rig.select(1); rig.reveal(W::filterParam(1, W::Cutoff));
    rig.section(Section::Macros); rig.reveal(W::macroParam(7));
    rig.section(Section::Main); rig.reveal(W::NoiseColor);

    rig.section(Section::Matrix); rig.select(63);
    const int destination = W::modParam(63, W::ModDestination); rig.reveal(destination);
    rig.click(rig.editor->parameterCell(destination), 1, 0.98f);
    CHECK_NEAR(rig.processor->getParameter(destination), 1, 0);
    ui::KeyEvent right; right.keyCode = juce::KeyPress::rightKey; CHECK(rig.root->dispatchKey(right));
    CHECK_NEAR(rig.processor->getParameter(destination), 2, 0);
    // Navigation remains operable in map mode and the last matrix row exposes its exact stable parameter.
    rig.context.midiMapMode = true; rig.section(Section::Lfos); rig.section(Section::Matrix);
    const int amount = W::modParam(63, W::ModAmount); rig.reveal(amount);
    const float amountBefore = rig.processor->getParameter(amount);
    rig.click(rig.editor->parameterCell(amount)); REQUIRE(rig.context.mapSelection);
    CHECK(rig.context.mapSelection->kind == midi::ControlTarget::Kind::DeviceParameter);
    CHECK(rig.context.mapSelection->param == rig.processor->parameters()[(size_t) amount].id);
    CHECK(rig.context.mapSelection->track == 0); CHECK(rig.context.mapSelection->device == 0); CHECK(rig.midi.isLearning());
    CHECK_NEAR(rig.processor->getParameter(amount), amountBefore, 0);
    rig.context.midiMapMode = false; rig.midi.cancelLearn();
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_ui_tests");
    REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1200, 800, 1,
                            directory.getChildFile("Matrix.png").getFullPathName().toStdString()));
}

TEST_CASE("wavetable UI: every parameter is reachable and unclipped at compact detail height through real section and page clicks")
{
    WavetableUi rig(210);
    std::set<int> reached;
    bool paged = false;
    for (int section = 0; section < (int) Section::Count; ++section)
    {
        rig.section((Section) section);
        for (int instance = 0; instance < rig.editor->instanceCount(); ++instance)
        {
            rig.select(instance);
            const int pages = rig.editor->pageCount(); paged |= pages > 1;
            for (int page = 0; page < pages; ++page)
            {
                for (int parameter : rig.editor->visibleParameters())
                {
                    reached.insert(parameter);
                    const auto cell = rig.editor->parameterCell(parameter);
                    CHECK(cell.w >= 80); CHECK(cell.h >= 20); CHECK(cell.x >= 0); CHECK(cell.y >= 0);
                    CHECK(cell.right() <= rig.editor->width()); CHECK(cell.bottom() <= rig.editor->height());
                    CHECK(rig.editor->boundsInRoot().translated(cell.x, cell.y).x >= 0);
                }
                if (pages > 1) rig.click(rig.editor->pageCell(1));
            }
        }
    }
    for (size_t parameter = 0; parameter < rig.processor->parameters().size(); ++parameter)
        if (! reached.contains(static_cast<int>(parameter)))
            std::printf("    Unreachable parameter: %s\n", rig.processor->parameters()[parameter].id.c_str());
    CHECK(paged); CHECK(reached.size() == rig.processor->parameters().size());
    CHECK(reached.contains(W::oscParam(2, W::RouteFilter2)));
    CHECK(reached.contains(W::envParam(3, W::ReleaseCurve)));
    CHECK(reached.contains(W::lfoParam(9, W::Fade)));
    CHECK(reached.contains(W::modParam(63, W::ModBipolar)));
    CHECK(reached.contains(W::macroParam(7)));
    CHECK(reached.contains(W::mediaParam(2, W::MediaVelocityTracking)));
    // Every parameter control, including choice/toggle fields, retains MIDI learn metadata.
    std::set<std::string> mapped;
    for (const auto& child : rig.editor->children())
        if (child->midiTarget) mapped.insert(child->midiTarget->param);
    CHECK(mapped.size() == rig.processor->parameters().size());
    rig.click(rig.editor->inspectorCell()); CHECK(rig.editor->dashboardVisible());
    CHECK(rig.context.theme.metrics.detailHeight > 600);
    CHECK(! rig.editor->oscillatorPlot(2).isEmpty());
}

TEST_CASE("wavetable UI: horizontal gestures over choice fields and knobs scroll a narrow device view without editing values")
{
    WavetableUi rig(210); rig.root->setBounds({0, 0, 900, 800}); rig.root->layout();
    rig.section(Section::Matrix);
    const int source = W::modParam(0, W::ModSource); rig.reveal(source);
    const float sourceBefore = rig.processor->getParameter(source), originBefore = rig.editor->boundsInRoot().x;
    auto scroll = rig.at(rig.editor->parameterCell(source)); scroll.wheelDeltaX = -1; rig.root->dispatchMouseWheel(scroll);
    CHECK(rig.editor->boundsInRoot().x < originBefore); CHECK_NEAR(rig.processor->getParameter(source), sourceBefore, 0);
    scroll = rig.at(rig.editor->parameterCell(source), 0.8f); scroll.wheelDeltaX = 1; rig.root->dispatchMouseWheel(scroll);
    CHECK_NEAR(rig.editor->boundsInRoot().x, originBefore, 0.1);
    rig.section(Section::Oscillators);
    const int position = W::oscParam(0, W::Position); rig.reveal(position);
    const float positionBefore = rig.processor->getParameter(position);
    scroll = rig.at(rig.editor->parameterCell(position)); scroll.wheelDeltaX = -1; rig.root->dispatchMouseWheel(scroll);
    CHECK(rig.editor->boundsInRoot().x < originBefore); CHECK_NEAR(rig.processor->getParameter(position), positionBefore, 0);
}

TEST_CASE("wavetable UI: immersive dashboard edits simultaneous modules, navigates modulation and matrix banks, imports and auditions safely")
{
    WavetableUi rig;
    REQUIRE(rig.editor->dashboardVisible());
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        CHECK(! rig.editor->oscillatorPlot(oscillator).isEmpty());
        CHECK(! rig.editor->parameterCell(W::oscParam(oscillator, W::Position)).isEmpty());
        CHECK(! rig.editor->parameterCell(W::oscParam(oscillator, W::Unison)).isEmpty());
        CHECK(! rig.editor->parameterCell(W::oscParam(oscillator, W::Warp2Amount)).isEmpty());
        CHECK(! rig.editor->parameterCell(W::mediaParam(oscillator, W::MediaMode)).isEmpty());
    }
    CHECK(! rig.editor->parameterCell(W::SubLevel).isEmpty());
    CHECK(! rig.editor->parameterCell(W::NoiseColor).isEmpty());
    CHECK(! rig.editor->parameterCell(W::filterParam(1, W::Cutoff)).isEmpty());
    rig.click(rig.editor->envelopeCell(3)); CHECK(! rig.editor->parameterCell(W::envParam(3, W::Attack)).isEmpty());
    rig.click(rig.editor->lfoCell(9)); CHECK(! rig.editor->parameterCell(W::lfoParam(9, W::Shape)).isEmpty());
    CHECK(! rig.editor->envelopePlot().isEmpty()); CHECK(! rig.editor->lfoPlot().isEmpty());

    auto positionDown = rig.at(rig.editor->oscillatorPlot(1)); rig.root->dispatchMouseDown(positionDown);
    auto positionUp = rig.at(rig.editor->oscillatorPlot(1), 0.82f); rig.root->dispatchMouseDrag(positionUp); rig.root->dispatchMouseUp(positionUp);
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(1, W::Position)), 0.82, 0.01);

    int importedOscillator = -1; std::string importedPath;
    rig.context.importWavetable = [&](int track, int device, int oscillator, const std::string& path, int chain, int child) {
        CHECK(track == 0); CHECK(device == 0); CHECK(chain == -1); CHECK(child == -1); importedOscillator = oscillator; importedPath = path;
    };
    rig.context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-table.wav", "Own table"};
    rig.root->dispatchMouseUp(rig.at(rig.editor->oscillatorPlot(2)));
    CHECK(importedOscillator == 2); CHECK(importedPath == "/tmp/own-table.wav");
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(2, W::Table)), W::Basic, 0); // publication belongs to the controller
    const int engine = W::mediaParam(2, W::MediaMode);
    const float previousWavetablePosition = rig.processor->getParameter(W::oscParam(2, W::Position));
    for (int i = 0; i < 3; ++i) rig.click(rig.editor->parameterCell(engine), 1, 0.98f);
    CHECK_NEAR(rig.processor->getParameter(engine), W::GranularMode, 0);
    CHECK(! rig.editor->parameterCell(W::mediaParam(2, W::MediaGrainSize)).isEmpty());
    CHECK(! rig.editor->parameterCell(W::mediaParam(2, W::MediaDensity)).isEmpty());
    CHECK(rig.editor->parameterCell(W::oscParam(2, W::Table)).isEmpty());
    int mediaImports = 0;
    rig.context.importOscillatorMedia = [&](int track, int device, int oscillator, const std::string& path, int chain, int child) {
        CHECK(track == 0); CHECK(device == 0); CHECK(oscillator == 2); CHECK(chain == -1); CHECK(child == -1);
        CHECK(path == "/tmp/own-media.wav"); ++mediaImports;
    };
    rig.context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-media.wav", "Own media"};
    rig.root->dispatchMouseUp(rig.at(rig.editor->oscillatorPlot(2))); CHECK(mediaImports == 1);
    CHECK(rig.processor->oscillatorMedia(2) == nullptr);
    auto mediaStart = rig.at(rig.editor->oscillatorPlot(2), 0.1f); rig.root->dispatchMouseDown(mediaStart);
    auto mediaMoved = rig.at(rig.editor->oscillatorPlot(2), 0.6f); rig.root->dispatchMouseDrag(mediaMoved); rig.root->dispatchMouseUp(mediaMoved);
    CHECK_NEAR(rig.processor->getParameter(W::mediaParam(2, W::MediaStartPosition)), 0.6, 0.01);
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(2, W::Position)), previousWavetablePosition, 0);
    rig.click(rig.editor->parameterCell(engine), 2);
    CHECK_NEAR(rig.processor->getParameter(engine), W::WavetableMode, 0);
    CHECK(! rig.editor->parameterCell(W::oscParam(2, W::Table)).isEmpty());

    int curveEdits = 0, editedLfo = -1; dsp::CurveGraph edited;
    rig.context.editWavetableLfo = [&](int track, int device, int lfo, const dsp::CurveGraph& graph, int chain, int child) {
        CHECK(track == 0); CHECK(device == 0); CHECK(chain == -1); CHECK(child == -1); ++curveEdits; editedLfo = lfo; edited = graph;
    };
    rig.processor->setParameter(W::lfoParam(9, W::Shape), 2);
    auto insert = rig.at(rig.editor->lfoPlot(), 0.25f, 0.2f); insert.clicks = 2;
    rig.root->dispatchMouseDown(insert); rig.root->dispatchMouseUp(insert);
    CHECK(curveEdits == 1); CHECK(editedLfo == 9); CHECK(edited.count == 4);
    CHECK_NEAR(edited.x[1], 0.25, 0.01); CHECK_NEAR(edited.y[1], 0.8, 0.01);
    CHECK(rig.processor->lfoCurve(9).count == 3); // immutable DSP curve was not changed by the widget
    auto handle = rig.at(rig.editor->lfoPlot(), 0.25f, 0.5f); rig.root->dispatchMouseDown(handle);
    auto bend = rig.at(rig.editor->lfoPlot(), 0.25f, 0.8f); rig.root->dispatchMouseDrag(bend); rig.root->dispatchMouseUp(bend);
    CHECK(curveEdits == 2); CHECK(edited.curve[0] > 0.4f); CHECK(edited.count == 3);
    auto remove = rig.at(rig.editor->lfoPlot(), 0.5f, 0); remove.right = true;
    rig.root->dispatchMouseDown(remove); rig.root->dispatchMouseUp(remove);
    CHECK(curveEdits == 3); CHECK(edited.count == 2); CHECK_NEAR(edited.x[0], 0, 0); CHECK_NEAR(edited.x[1], 1, 0);
    auto point = rig.at(rig.editor->lfoPlot(), 0.5f, 0); rig.root->dispatchMouseDown(point);
    auto snap = rig.at(rig.editor->lfoPlot(), 0.62f, 0.67f); snap.alt = true;
    rig.root->dispatchMouseDrag(snap); rig.root->dispatchMouseUp(snap);
    CHECK(curveEdits == 4); CHECK_NEAR(edited.x[1], 0.625, 0.01); CHECK_NEAR(edited.y[1], 0.375, 0.01);

    // The on-screen keys play this device directly (no armed track, no computer-keyboard routing).
    int routed = 0; rig.context.computerKeyboardNote = [&](int, int, bool) { ++routed; };
    const auto pump = [&] { juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi; rig.processor->process(audio, midi); };
    auto key = rig.at(rig.editor->keyboardCell(60), 0.5f, 0.85f); rig.root->dispatchMouseDown(key); pump();
    CHECK(rig.processor->noteLit(60));
    auto otherKey = rig.at(rig.editor->keyboardCell(62), 0.5f, 0.85f); rig.root->dispatchMouseDrag(otherKey); pump();
    CHECK(rig.processor->noteLit(62)); CHECK(! rig.processor->noteLit(60));
    rig.root->dispatchMouseUp(otherKey); pump();
    CHECK(! rig.processor->noteLit(62)); CHECK(routed == 0);

    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("Dashboard.png").getFullPathName().toStdString()));
    rig.click(rig.editor->workflowCell(Editor::Page::Matrix));
    for (int bank = 0; bank < 7; ++bank) rig.click(rig.editor->matrixBankCell(1));
    CHECK(! rig.editor->parameterCell(W::modParam(63, W::ModDestination)).isEmpty());
    CHECK(! rig.editor->parameterCell(W::modParam(56, W::ModAmount)).isEmpty());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("Matrix Dashboard.png").getFullPathName().toStdString()));
    rig.click(rig.editor->workflowCell(Editor::Page::Global)); CHECK(! rig.editor->parameterCell(W::NoiseOn).isEmpty());
    rig.click(rig.editor->workflowCell(Editor::Page::Mix)); CHECK(! rig.editor->parameterCell(W::filterParam(0, W::Drive)).isEmpty());
    rig.click(rig.editor->workflowCell(Editor::Page::Osc)); REQUIRE(rig.editor->dashboardVisible());
    rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(W::oscParam(1, W::Level)));
    REQUIRE(rig.context.mapSelection); CHECK(rig.context.mapSelection->param == rig.processor->parameters()[(size_t) W::oscParam(1, W::Level)].id);
}

TEST_CASE("wavetable UI: preset navigation table imports and curve commits support undo across synchronized editor rebuilds")
{
    WavetableUi rig;
    model::EditHistory history;
    auto refresh = [&] {
        devices::BuildContext build;
        auto next = engine::buildGraph(rig.song, build, rig.samples).graph; REQUIRE(next); next->prepare(48000, 512);
        rig.context.graph = next.get(); rig.context.controls = next->controls;
        auto old = std::move(rig.graph); rig.graph = std::move(next); rig.root->layout();
        std::vector<Editor*> editors; collect(*rig.root, editors); REQUIRE(editors.size() == 1); rig.editor = editors[0];
        rig.processor = dynamic_cast<W*>(rig.graph->nodes[0].processorForModelDevice[0]); REQUIRE(rig.processor);
    };
    rig.context.editDevice = [&](int track, int device, const auto& operation, const std::string& label) {
        auto replacement = rig.song.tracks[(size_t) track].devices[(size_t) device];
        devices::captureDeviceState(replacement, rig.processor); operation(replacement);
        history.perform(rig.song, model::replaceDevice(rig.song, track, device, std::move(replacement), label)); refresh();
    };
    rig.click(rig.editor->presetCell(1));
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(0, W::Unison)), 7, 0);
    CHECK_NEAR(rig.song.tracks[0].devices[0].param("Osc.0/Unison", -1), 7, 0);
    CHECK(rig.song.tracks[0].devices[0].displayName == "Sonora Prism / Pulse Stack");
    REQUIRE(history.undo(rig.song)); refresh();
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(0, W::Unison)), 1, 0);
    REQUIRE(history.redo(rig.song)); refresh();
    CHECK_NEAR(rig.processor->getParameter(W::oscParam(0, W::Unison)), 7, 0);
    rig.click(rig.editor->presetCell(-1)); CHECK_NEAR(rig.processor->getParameter(W::oscParam(0, W::Unison)), 1, 0);
    std::array<float, 64> samples{};
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * 6.283185307179586 / 64));
    const auto table = devices::WavetableData::fromSamples(samples, 1, 64, 48000, "Own UI import"); REQUIRE(table);
    rig.context.importWavetable = [&](int track, int device, int oscillator, const std::string&, int chain, int child) {
        CHECK(chain == -1); CHECK(child == -1);
        rig.context.editDevice(track, device, [oscillator, &table](auto& target) {
            std::string error; REQUIRE(devices::storeWavetableTable(target, oscillator, *table.data, error));
        }, "Import own UI table");
    };
    rig.context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-ui-import.wav", "Own UI import"};
    rig.root->dispatchMouseUp(rig.at(rig.editor->oscillatorPlot(1)));
    REQUIRE(rig.processor->customTable(1)); CHECK(rig.processor->customTableName(1) == "Own UI import");
    const auto tableContent = rig.song.tracks[0].devices[0].nativeContent; REQUIRE(tableContent);
    REQUIRE(history.undo(rig.song)); refresh(); CHECK(rig.processor->customTable(1) == nullptr);
    REQUIRE(history.redo(rig.song)); refresh(); CHECK(rig.song.tracks[0].devices[0].nativeContent == tableContent);
    rig.context.editWavetableLfo = [&](int track, int device, int lfo, const dsp::CurveGraph& curve, int chain, int child) {
        CHECK(chain == -1); CHECK(child == -1);
        rig.context.editDevice(track, device, [lfo, curve](auto& target) {
            std::string error; REQUIRE(devices::storeWavetableLfo(target, lfo, curve, error));
        }, "Edit own UI LFO");
    };
    rig.click(rig.editor->parameterCell(W::lfoParam(0, W::Shape)), 1, 0.98f);
    CHECK_NEAR(rig.processor->getParameter(W::lfoParam(0, W::Shape)), 2, 0);
    auto insert = rig.at(rig.editor->lfoPlot(), 0.25f, 0.2f); insert.clicks = 2;
    rig.root->dispatchMouseDown(insert); rig.root->dispatchMouseUp(insert);
    CHECK(rig.processor->hasCustomLfoCurve(0)); CHECK(rig.processor->lfoCurve(0).count == 4);
    REQUIRE(history.undo(rig.song)); refresh(); CHECK(! rig.processor->hasCustomLfoCurve(0));
    REQUIRE(history.redo(rig.song)); refresh(); CHECK(rig.processor->hasCustomLfoCurve(0)); CHECK(rig.processor->lfoCurve(0).count == 4);
}

TEST_CASE("wavetable UI: imported tables curves and preset edits retain the exact nested rack destination")
{
    juce::ScopedJuceInitialiser_GUI init;
    W processor; processor.prepare(48000, 512);
    model::Song song; song.tracks.resize(3); song.tracks[2].devices.resize(5);
    auto& rack = song.tracks[2].devices[4]; rack = devices::nativeDeviceModel("InstrumentGroupDevice");
    const auto rackName = rack.displayName;
    rack.chains.resize(2); rack.chains[1].devices.resize(4);
    rack.chains[1].devices[3] = devices::nativeDeviceModel("SonoraWavetable");
    rack.chains[1].devices[0] = devices::nativeDeviceModel("SonoraWavetable");
    ui::UiContext context; context.song = &song;
    ui::RootWidget root(context); root.setBounds({0, 0, 1080, 700});
    auto* editor = root.add<Editor>(&processor, 2, 4, 1, 3); editor->setBounds(root.localBounds()); editor->layout();
    auto at = [](ui::Rect cell, float x = 0.5f, float y = 0.5f) {
        ui::MouseEvent event; event.x = cell.x + cell.w * x; event.y = cell.y + cell.h * y; return event;
    };
    int imports = 0, edits = 0;
    context.importWavetable = [&](int track, int device, int oscillator, const std::string& path, int chain, int child) {
        CHECK(track == 2); CHECK(device == 4); CHECK(chain == 1); CHECK(child == 3); CHECK(oscillator == 1);
        CHECK(path == "/tmp/own-nested-table.wav"); ++imports;
    };
    context.editWavetableLfo = [&](int track, int device, int lfo, const dsp::CurveGraph& curve, int chain, int child) {
        CHECK(track == 2); CHECK(device == 4); CHECK(chain == 1); CHECK(child == 3); CHECK(lfo == 0); CHECK(curve.count == 4); ++edits;
    };
    context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-nested-table.wav", "Own nested table"};
    root.dispatchMouseUp(at(editor->oscillatorPlot(1))); CHECK(imports == 1);
    context.importOscillatorMedia = [&](int track, int device, int oscillator, const std::string& path, int chain, int child) {
        CHECK(track == 2); CHECK(device == 4); CHECK(chain == 1); CHECK(child == 3); CHECK(oscillator == 1);
        CHECK(path == "/tmp/own-nested-media.wav"); ++imports;
    };
    auto mode = at(editor->parameterCell(W::mediaParam(1, W::MediaMode)), 0.98f);
    root.dispatchMouseDown(mode); root.dispatchMouseUp(mode);
    context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, "/tmp/own-nested-media.wav", "Own nested media"};
    root.dispatchMouseUp(at(editor->oscillatorPlot(1))); CHECK(imports == 2);
    processor.setParameter(W::lfoParam(0, W::Shape), 2);
    auto insert = at(editor->lfoPlot(), 0.25f, 0.2f); insert.clicks = 2;
    root.dispatchMouseDown(insert); root.dispatchMouseUp(insert); CHECK(edits == 1);
    CHECK(processor.lfoCurve(0).count == 3);
    context.editDevice = [&](int track, int device, const auto& operation, const std::string&) {
        CHECK(track == 2); CHECK(device == 4); operation(song.tracks[(size_t) track].devices[(size_t) device]);
    };
    auto preset = at(editor->presetCell(1)); root.dispatchMouseDown(preset); root.dispatchMouseUp(preset);
    CHECK(rack.chainsEdited); CHECK_NEAR(rack.chains[1].devices[3].param("Osc.0/Unison", -1), 7, 0);
    CHECK_NEAR(rack.chains[1].devices[0].param("Osc.0/Unison", -1), 1, 0);
    CHECK(rack.displayName == rackName);
}
