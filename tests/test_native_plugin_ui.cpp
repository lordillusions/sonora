#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/LfoShaper.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "ui/views/MainLayout.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/NativeEditors.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/Buttons.h"

using namespace sonora;
namespace
{
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}
ui::MouseEvent centre(ui::Widget& widget, ui::Rect bounds)
{
    ui::MouseEvent e; const auto parent = widget.boundsInRoot(); e.x = parent.x + bounds.centreX(); e.y = parent.y + bounds.centreY(); return e;
}
}

TEST_CASE("plugin browser: vendor folders include AU and VST3 with stable search and drag identities")
{
    ui::UiContext context; context.detailVisible = false;
    context.browserPlugins = {{"Synth (VST3)", "vst-synth", "Zeta", "VST3"}, {"EQ (AudioUnit)", "au-eq", "Acme", "AudioUnit"},
                              {"Compressor (VST3)", "vst-comp", "Acme", "VST3"}, {"Unknown (AudioUnit)", "au-other", "", "AudioUnit"}};
    ui::MainLayout root(context); root.setBounds({0, 0, 1200, 800}); root.layout();
    std::vector<ui::BrowserPanel*> browsers; collect(root, browsers); REQUIRE(browsers.size() == 1); auto& browser = *browsers[0];
    browser.showCategory(6);
    auto header = centre(browser, browser.listRowBounds(0)); root.dispatchMouseDown(header); root.dispatchMouseUp(header);
    auto au = centre(browser, browser.listRowBounds(2)); root.dispatchMouseDown(au);
    auto end = au; end.x += 20; root.dispatchMouseDrag(end);
    REQUIRE(context.browserDrag); CHECK(context.browserDrag->value == "au-eq");
    root.dispatchMouseUp(end);
    browser.focusSearch();
    for (auto c : std::string("acme")) { ui::KeyEvent k; k.textChar = (uint32_t) c; root.dispatchKey(k); }
    auto vst = centre(browser, browser.listRowBounds(1)); root.dispatchMouseDown(vst); end = vst; end.x += 20; root.dispatchMouseDrag(end);
    REQUIRE(context.browserDrag); CHECK(context.browserDrag->value == "vst-comp"); root.dispatchMouseUp(end);
    // Vendor folders do not become plugin drag payloads.
    header = centre(browser, browser.listRowBounds(0)); root.dispatchMouseDown(header); end = header; end.x += 20; root.dispatchMouseDrag(end);
    CHECK(! context.browserDrag); root.dispatchMouseUp(end);
}

TEST_CASE("native plugin UI: draw the LFO curve, edit zones, address empty drum pads and select rack chains")
{
    model::Song song; song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
    auto lfo = devices::nativeDeviceModel("SonoraLfo"), sampler = devices::nativeDeviceModel("MultiSampler");
    model::SamplePart zone; zone.name = "Sample"; sampler.sampleParts = {zone};
    auto rack = devices::nativeDeviceModel("DrumGroupDevice"); rack.id = 42;
    model::Device::Chain chain; chain.receivingNote = 36; chain.name = "Kick"; chain.devices = {devices::nativeDeviceModel("SonoraFm")}; rack.chains = {chain};
    song.tracks[0].devices = {lfo, sampler, rack};
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples); graph.graph->prepare(48000, 512);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.controls = graph.graph->controls;
    context.browserVisible = false; context.detailTab = ui::DetailTab::Devices; context.theme.metrics.detailHeight = 500;
    int edits = 0;
    context.editDevice = [&](int track, int device, const auto& operation, const std::string&) { CHECK(track == 0); ++edits; operation(song.tracks[0].devices[(size_t) device]); };
    int dropChain = -1, dropZone = -2;
    context.dropDeviceItem = [&](const auto&, int track, int device, int ci, int zi, int) { CHECK(track == 0); CHECK(device == 2); dropChain = ci; dropZone = zi; };
    ui::MainLayout root(context); root.setBounds({0, 0, 2200, 900}); root.layout();
    std::vector<ui::LfoCurveEditor*> curves; collect(root, curves); REQUIRE(curves.size() == 1);
    auto& curve = *curves[0];
    auto start = centre(curve, {0, curve.height() - 2, 1, 1}); root.dispatchMouseDown(start);
    auto end = start; end.x = curve.boundsInRoot().right() - 1; end.y = curve.boundsInRoot().y + 2;
    root.dispatchMouseDrag(end); root.dispatchMouseUp(end);
    auto* processor = dynamic_cast<devices::LfoShaper*>(graph.graph->nodes[0].processorForModelDevice[0]); REQUIRE(processor);
    CHECK_NEAR(processor->getParameter(devices::LfoShaper::Shape), 6, 0);
    CHECK(processor->getParameter(devices::LfoShaper::PointStart) < 0.05f);
    CHECK(processor->getParameter(devices::LfoShaper::PointStart + 16) > 0.95f);
    std::vector<ui::NativeDevicePanel*> panels; collect(root, panels); REQUIRE(panels.size() == 2);
    auto& zones = *panels[0];
    // The compact sampler face opens the existing zone table from its Zones tab.
    if (! zones.visible)
    {
        std::vector<ui::TextButton*> tabs; collect(*zones.parent(), tabs);
        auto tab = std::find_if(tabs.begin(), tabs.end(), [](auto* button) { return button->label() == "Zones"; });
        REQUIRE(tab != tabs.end()); const auto click = centre(**tab, (*tab)->localBounds());
        root.dispatchMouseDown(click); root.dispatchMouseUp(click); REQUIRE(zones.visible);
    }
    auto cell = centre(zones, zones.cellBounds(0, 0)); cell.wheelDeltaY = -1; root.dispatchMouseWheel(cell);
    CHECK(edits == 1); CHECK(song.tracks[0].devices[1].sampleParts[0].rootKey == 59);
    CHECK(song.tracks[0].devices[1].samplePartsEdited);
    auto& pads = *panels[1];
    ui::BrowserDrag payload{ui::BrowserDrag::Kind::Sample, "/tmp/snare.wav", "Snare"};
    ui::MouseEvent pad; pad.x = pads.padBounds(37).centreX(); pad.y = pads.padBounds(37).centreY();
    CHECK(pads.browserDrop(payload, pad)); CHECK(dropChain == 128 + 37); CHECK(dropZone == 0);
    auto select = centre(pads, pads.padBounds(36)); root.dispatchMouseDown(select); root.dispatchMouseUp(select);
    CHECK(context.selectedRackId == 42); CHECK(context.selectedRackChain == 0);
    root.layout();
    std::vector<ui::Knob*> knobs; collect(root, knobs);
    bool nested = false;
    for (auto* knob : knobs) if (knob->onRightClick)
    {
        context.rackMacroToMap = 2; knob->onRightClick(); nested = true; break;
    }
    CHECK(nested);
    bool mapped = false;
    for (const auto& p : song.tracks[0].devices[2].chains[0].devices[0].parameters) if (p.macroIndex == 2) mapped = true;
    CHECK(mapped);
    panels.clear(); collect(root, panels); REQUIRE(panels.size() == 2);
    ui::MouseEvent bank; bank.x = 30; bank.y = 100; bank.wheelDeltaY = 1; panels[1]->mouseWheel(bank);
    CHECK(context.drumPadBanks[42] == 52);
    context.selectedRackChain = 1; root.layout(); // rebuild the editor, retaining the displayed bank
    panels.clear(); collect(root, panels); REQUIRE(panels.size() == 2);
    pad.x = panels[1]->padBounds(53).centreX(); pad.y = panels[1]->padBounds(53).centreY();
    CHECK(panels[1]->browserDrop(payload, pad)); CHECK(dropChain == 128 + 53);
}
