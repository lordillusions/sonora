// UI-84: a device or plug-in dragged from the browser onto the device chain goes where it was let go (between the two
// devices under the pointer), not always at the end. Driven through the root's real drag / release events.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"
#include <juce_core/juce_core.h>

#include <functional>

using namespace sonora;

TEST_CASE("device chain drop (UI-84): browser devices land in the gap under the pointer, presets beside the device")
{
    model::Song song;
    for (int t = 0; t < 2; ++t) { model::Track track; track.id = 20 + t; track.kind = model::TrackKind::Midi; song.tracks.push_back(track); }
    model::Device instrument; instrument.id = 1; instrument.type = "PluginDevice"; instrument.displayName = "Instrument";
    instrument.plugin.emplace(); instrument.plugin->isInstrument = true;
    auto delay = devices::nativeDeviceModel("Delay"); delay.id = 2;
    auto reverb = devices::nativeDeviceModel("Reverb"); reverb.id = 3;
    song.tracks[0].devices = {instrument, delay, reverb};
    engine::SampleCache samples; devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.browserVisible = false;
    context.mainView = ui::MainView::Arrangement;
    int drops = 0, droppedTrack = -1;
    ui::BrowserDrag payload;
    context.dropBrowserItem = [&](const auto& item, int index, double, int) { ++drops; payload = item; droppedTrack = index; };
    int presetDrops = 0, presetDevice = -1;
    context.dropDeviceItem = [&](const auto& item, int, int device, int, int, int) { ++presetDrops; payload = item; presetDevice = device; };
    ui::MainLayout root(context); root.setBounds({0, 0, 1600, 800});
    ui::DetailView* detail = nullptr;
    ui::ArrangementView* arrangement = nullptr;
    std::function<void(ui::Widget&)> find = [&](ui::Widget& w) {
        if (auto* d = dynamic_cast<ui::DetailView*>(&w)) detail = d;
        if (auto* a = dynamic_cast<ui::ArrangementView*>(&w)) arrangement = a;
        for (const auto& child : w.children()) find(*child);
    };
    find(root);
    REQUIRE(detail); REQUIRE(arrangement); REQUIRE(arrangement->lanes().size() >= 2);
    auto event = [](float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; };
    // A point in device box `index` at `fraction` of its width, half way down its body.
    auto inBox = [&](size_t index, float fraction) {
        const auto b = detail->children()[index]->boundsInRoot();
        return event(b.x + b.w * fraction, b.y + b.h * 0.5f);
    };
    const auto pngDir = juce::SystemStats::getEnvironmentVariable("SONORA_CHAIN_DROP_PNG", {});
    auto dropAt = [&](ui::BrowserDrag drag, ui::MouseEvent at, const char* png = nullptr) {
        context.browserDrag = std::move(drag);
        root.dispatchMouseDrag(at);
        if (png && pngDir.isNotEmpty())
            CHECK(gfx::renderToPng(root, 1600, 800, 1, juce::File(pngDir).getChildFile(png).getFullPathName().toStdString()));
        root.dispatchMouseUp(at);
        CHECK(! context.browserDrag);
    };
    const ui::BrowserDrag chorus{ui::BrowserDrag::Kind::NativeDevice, "Chorus2", "Chorus-Ensemble"};

    // Left half of the Reverb: between Delay and Reverb.
    dropAt(chorus, inBox(2, 0.25f), "chain_drop_between.png");
    CHECK(drops == 1); CHECK(droppedTrack == 0); CHECK(payload.chainPosition == 2);
    // Right half of the Reverb, and the empty space after the chain: the end.
    dropAt(chorus, inBox(2, 0.75f));
    CHECK(drops == 2); CHECK(payload.chainPosition == 3);
    const auto last = detail->children()[2]->boundsInRoot();
    dropAt(chorus, event(last.right() + 60, last.centreY()), "chain_drop_end.png");
    CHECK(drops == 3); CHECK(payload.chainPosition == 3);
    // Left half of the Delay: right after the instrument. Before the instrument the gap is 0; the app moves an audio
    // effect behind the instrument (model::trackDeviceInsertIndex) and the marker shows that.
    dropAt(chorus, inBox(1, 0.2f));
    CHECK(drops == 4); CHECK(payload.chainPosition == 1);
    dropAt(chorus, inBox(0, 0.2f), "chain_drop_before_instrument.png");
    CHECK(drops == 5); CHECK(payload.chainPosition == 0);
    CHECK(model::trackDeviceInsertIndex(song.tracks[0], model::DeviceRole::AudioEffect, payload.chainPosition) == 1);

    // A plug-in drag carries the place too.
    dropAt({ui::BrowserDrag::Kind::Plugin, "VST3-Some FX-1234", "Some FX"}, inBox(1, 0.8f));
    CHECK(drops == 6); CHECK(payload.kind == ui::BrowserDrag::Kind::Plugin); CHECK(payload.chainPosition == 2);

    // A preset on a device: the app hot-swaps a preset of that device, any other goes beside it.
    dropAt({ui::BrowserDrag::Kind::DevicePreset, "/lib/Echo.adv", "Echo"}, inBox(1, 0.8f));
    CHECK(presetDrops == 1); CHECK(presetDevice == 1); CHECK(payload.chainPosition == 2);
    dropAt({ui::BrowserDrag::Kind::DevicePreset, "/lib/Echo.adv", "Echo"}, inBox(1, 0.2f));
    CHECK(presetDrops == 2); CHECK(payload.chainPosition == 1);

    // Dropped on a track in the Arrangement: no place in a chain, the insertion policy decides.
    const auto& lane = arrangement->lanes()[1];
    dropAt(chorus, event(arrangement->boundsInRoot().x + 300, arrangement->boundsInRoot().y + lane.y + lane.h * 0.5f));
    CHECK(drops == 7); CHECK(droppedTrack == 1); CHECK(payload.chainPosition == -1);
    CHECK(song.tracks[0].devices.size() == 3); // the rig applies nothing itself
}
