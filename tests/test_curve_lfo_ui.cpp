#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveLfo.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/views/MainLayout.h"
#include "ui/widgets/Knob.h"

using namespace sonora;
namespace
{
using L = devices::CurveLfo;
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}
ui::MouseEvent at(ui::Widget& widget, float x, float y, int clicks = 1)
{
    ui::MouseEvent e; const auto origin = widget.boundsInRoot(); e.x = origin.x + x; e.y = origin.y + y; e.clicks = clicks; return e;
}
}

TEST_CASE("curve LFO editor: select, add, drag, bend and remove points, shapes, warp, copy and paste with real input")
{
    model::Song song; song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
    song.tracks[0].devices = {devices::nativeDeviceModel("SonoraCurveLfo")};
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples); graph.graph->prepare(48000, 512);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.controls = graph.graph->controls;
    context.browserVisible = false; context.detailTab = ui::DetailTab::Devices; context.theme.metrics.detailHeight = 400;
    ui::MainLayout root(context); root.setBounds({0, 0, 2200, 900}); root.layout();
    std::vector<ui::CurveLfoEditor*> editors; collect(root, editors); REQUIRE(editors.size() == 1);
    auto& editor = *editors[0];
    auto* lfo = dynamic_cast<L*>(graph.graph->nodes[0].processorForModelDevice[0]); REQUIRE(lfo);
    auto click = [&](ui::Rect cell, int clicks = 1, bool shift = false) {
        auto e = at(editor, cell.centreX(), cell.centreY(), clicks); e.shift = shift; root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    };

    // Default graph: two points with an ease-out segment, and a hollow handle that bends it.
    CHECK(lfo->graph(0).count == 2);
    const auto start = lfo->graph(0);
    auto handle = at(editor, editor.pointX(0.5f), editor.pointY(start.eval(0.5f)));
    root.dispatchMouseDown(handle); auto target = at(editor, editor.pointX(0.5f), editor.pointY(0.5f)); root.dispatchMouseDrag(target); root.dispatchMouseUp(target);
    CHECK_NEAR(lfo->graph(0).curve[0], 0, 0.05);

    // Double-click adds a point; dragging moves it; double-click on it removes it.
    auto add = at(editor, editor.pointX(0.5f), editor.pointY(0.9f), 2); root.dispatchMouseDown(add); root.dispatchMouseUp(add);
    REQUIRE(lfo->graph(0).count == 3); CHECK_NEAR(lfo->graph(0).x[1], 0.5, 0.01); CHECK_NEAR(lfo->graph(0).y[1], 0.9, 0.03);
    auto grab = at(editor, editor.pointX(lfo->graph(0).x[1]), editor.pointY(lfo->graph(0).y[1]));
    root.dispatchMouseDown(grab); auto drag = at(editor, editor.pointX(0.7f), editor.pointY(0.2f)); root.dispatchMouseDrag(drag); root.dispatchMouseUp(drag);
    CHECK_NEAR(lfo->graph(0).x[1], 0.7, 0.01); CHECK_NEAR(lfo->graph(0).y[1], 0.2, 0.03);
    drag = at(editor, editor.pointX(1.5f), editor.pointY(0.5f)); root.dispatchMouseDown(at(editor, editor.pointX(0.7f), editor.pointY(0.2f)));
    root.dispatchMouseDrag(drag); root.dispatchMouseUp(drag);                          // cannot cross the end point
    CHECK(lfo->graph(0).x[1] <= 1.0f); CHECK_NEAR(lfo->graph(0).x[2], 1, 0);
    auto remove = at(editor, editor.pointX(lfo->graph(0).x[1]), editor.pointY(lfo->graph(0).y[1]), 2); root.dispatchMouseDown(remove); root.dispatchMouseUp(remove);
    CHECK(lfo->graph(0).count == 2);
    // The end points never move horizontally.
    auto first = at(editor, editor.pointX(0), editor.pointY(0)); root.dispatchMouseDown(first);
    auto away = at(editor, editor.pointX(0.4f), editor.pointY(0.3f)); root.dispatchMouseDrag(away); root.dispatchMouseUp(away);
    CHECK_NEAR(lfo->graph(0).x[0], 0, 0); CHECK_NEAR(lfo->graph(0).y[0], 0.3, 0.03);

    // Shift-click makes a flat step at the grid size.
    click({editor.plot().x + 4, editor.plot().y + 4, 1, 1}, 1, true);
    CHECK(lfo->graph(0).count > 2);

    // Graph selector, shape menu, warp, copy and paste act on the selected graph only.
    int pickChoice = 1; size_t menuSize = 0;
    context.showChoices = [&](const auto& labels, const auto&, auto pick) { menuSize = labels.size(); pick(pickChoice); };
    click(editor.graphCell(4)); CHECK_NEAR(lfo->getParameter(L::SelectedGraph), 4, 0);
    click(editor.shapeCell()); CHECK(menuSize == 8); CHECK(lfo->graph(4).count == 17); CHECK(lfo->graph(0).count > 2);
    pickChoice = 4; click(editor.warpCell()); CHECK(menuSize == 7);
    CHECK_NEAR(lfo->getParameter(L::warpParam(4)), 4, 0); CHECK_NEAR(lfo->getParameter(L::warpParam(0)), 3, 0);
    ui::MouseEvent wheel; wheel.x = editor.warpCell().centreX(); wheel.y = editor.warpCell().centreY(); wheel.wheelDeltaY = 1;
    CHECK(editor.mouseWheel(wheel));
    CHECK_NEAR(lfo->getParameter(L::warpParam(4)), 5, 0);
    lfo->setParameter(L::warpParam(4), 4);
    click(editor.copyCell()); click(editor.graphCell(5)); click(editor.pasteCell());
    CHECK(lfo->graph(5).count == 17); CHECK_NEAR(lfo->graph(5).y[4], lfo->graph(4).y[4], 1e-6);

    // Routing column: the graph picker and amount bar edit the route; a click on the picker opens that graph.
    lfo->setParameter(L::sourceParam(L::Pan), 7); lfo->setParameter(L::amountParam(L::Pan), 0.5f);
    const auto panRow = editor.routeRow(L::Pan);
    click({panRow.x + 60, panRow.y + 2, 4, 4}); CHECK_NEAR(lfo->getParameter(L::SelectedGraph), 7, 0);
    auto bar = at(editor, panRow.x + 150, panRow.centreY()); root.dispatchMouseDown(bar);
    bar.y -= 40; root.dispatchMouseDrag(bar); root.dispatchMouseUp(bar);
    CHECK(lfo->getParameter(L::amountParam(L::Pan)) > 0.55f);
    // Pages: the filter controls only show on the Filter page; Sync swaps the synced rate for the free rate.
    CHECK(editor.page() == ui::CurveLfoEditor::LfoPage);
    std::vector<ui::Knob*> knobs; collect(editor, knobs);
    auto visibleKnob = [&](const std::string& tooltipStart) {
        for (auto* k : knobs) if (k->visible && k->tooltip.rfind(tooltipStart, 0) == 0) return true;
        return false;
    };
    CHECK(visibleKnob("Rate:")); CHECK(! visibleKnob("Free Rate:")); CHECK(! visibleKnob("Cutoff:"));
    lfo->setParameter(L::Sync, 0); editor.tick(); CHECK(! visibleKnob("Rate:")); CHECK(visibleKnob("Free Rate:"));
    lfo->setParameter(L::Sync, 1); editor.tick();
    editor.selectPage(ui::CurveLfoEditor::FilterPage); CHECK(visibleKnob("Cutoff:")); CHECK(! visibleKnob("Swing:"));
    editor.selectPage(ui::CurveLfoEditor::LfoPage);

    // Playhead follows the audio thread.
    lfo->setHostInfo({120, 0.5, true, 48000}); juce::AudioBuffer<float> audio(2, 64); audio.clear(); juce::MidiBuffer midi; lfo->process(audio, midi);
    CHECK_NEAR(lfo->displayPhase(), 0.5, 0.01);
}
