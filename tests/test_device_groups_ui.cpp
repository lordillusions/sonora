// DEV-15c: device multi-selection in the Device View, Cmd+G routed to device or track grouping, the grouped rack drawn, and
// hosted plug-ins keeping their running instance when they move into a rack and back out.
#include "TestFramework.h"
#include "core/DeviceGroups.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "plugins/PluginDevice.h"
#include "plugins/PluginHost.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>

using namespace sonora;

namespace
{
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}

juce::File phaseAlignBundle()
{
    const auto build = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getParentDirectory();
    for (const char* config : {"Debug", "Release", "RelWithDebInfo"})
    {
        const auto bundle = build.getChildFile("src/SonoraPhaseAlign_artefacts").getChildFile(config).getChildFile("VST3/Sonora Phase Align.vst3");
        if (bundle.exists()) return bundle;
    }
    return {};
}

model::Device groupRack(model::Song& song, const std::string& type)
{
    auto rack = devices::nativeDeviceModel(type);
    rack.displayName = model::groupRackName(type);
    rack.numVisibleMacros = 8;
    rack.id = -1;
    model::allocateDeviceIds(song, rack);
    return rack;
}

// Clicks the title band of the n-th device box of the Device View (its name, away from the switch and the icons).
void clickTitle(ui::MainLayout& root, int box, bool shift = false, bool cmd = false)
{
    std::vector<ui::DetailView*> details;
    collect(root, details);
    REQUIRE(details.size() == 1);
    REQUIRE((size_t) box < details[0]->children().size());
    const auto at = details[0]->children()[(size_t) box]->boundsInRoot();
    ui::MouseEvent e;
    e.x = at.x + 40;
    e.y = at.y + 6;
    e.shift = shift;
    e.cmd = cmd;
    root.dispatchMouseDown(e);
    root.dispatchMouseUp(e);
}

ui::KeyEvent cmdG(bool shift)
{
    ui::KeyEvent k;
    k.keyCode = 'G';
    k.textChar = shift ? 'G' : 'g';
    k.cmd = true;
    k.shift = shift;
    return k;
}
} // namespace

TEST_CASE("device groups UI: Shift/Cmd+click select devices, Cmd+G groups them in the Device View, tracks elsewhere (DEV-15c)")
{
    model::Song song;
    song.tracks.resize(2);
    for (int i = 0; i < 2; ++i)
    {
        song.tracks[(size_t) i].id = 4 + i;
        song.tracks[(size_t) i].kind = model::TrackKind::Audio;
        song.tracks[(size_t) i].name = i == 0 ? "Drums" : "Bass";
    }
    for (const char* type : {"StereoGain", "Eq8", "Reverb"})
    {
        auto d = devices::nativeDeviceModel(type);
        model::allocateDeviceIds(song, d);
        song.tracks[1].devices.push_back(d);
    }
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    graph.graph->prepare(48000, 512);
    ui::UiContext context;
    context.song = &song;
    context.graph = graph.graph.get();
    context.controls = graph.graph->controls;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    context.selectedTrack = 1;
    model::EditHistory history;
    int trackGroups = 0;
    context.groupSelectedTracks = [&] { ++trackGroups; };
    // What the app does (AppDeviceGroups.cpp), minus syncing the engine state.
    context.groupSelectedDevices = [&] {
        const auto* list = model::deviceListAt(song.tracks[1], context.selectedDevicePath());
        int first = 0, count = 0;
        if (! list || ! model::groupableDevices(*list, context.selectedDeviceIds(), first, count)) return;
        auto rack = groupRack(song, model::groupRackType(*list, first, count));
        const int id = rack.id;
        history.perform(song, model::groupDevices(song, 1, context.selectedDeviceIds(), std::move(rack)));
        graph = engine::buildGraph(song, build, samples);
        graph.graph->prepare(48000, 512);
        context.graph = graph.graph.get();
        context.controls = graph.graph->controls;
        context.selectedRackId = id;
        context.selectedRackChain = 0;
        context.selectDevice(1, {}, id, false, false);
    };
    context.ungroupSelectedDevice = [&] {
        history.perform(song, model::ungroupDevice(song, 1, context.selectedDeviceIds().front()));
        graph = engine::buildGraph(song, build, samples);
        graph.graph->prepare(48000, 512);
        context.graph = graph.graph.get();
        context.controls = graph.graph->controls;
        context.selectedRackId = -1;
    };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1700, 900});
    root.layout();

    const auto ids = [&] { return context.selectedDeviceIds(); };
    const int utility = song.tracks[1].devices[0].id, eq = song.tracks[1].devices[1].id, reverb = song.tracks[1].devices[2].id;
    clickTitle(root, 1);
    CHECK(ids() == std::vector<int>{eq});
    CHECK(context.selectedDeviceId == eq);
    clickTitle(root, 2, true); // Shift: a range
    CHECK(ids() == (std::vector<int>{eq, reverb}));
    clickTitle(root, 0, false, true); // Cmd: add
    CHECK(ids() == (std::vector<int>{utility, eq, reverb}));
    clickTitle(root, 2, false, true); // Cmd again: remove
    CHECK(ids() == (std::vector<int>{utility, eq}));
    CHECK(context.canGroupSelectedDevices());
    CHECK(! context.canUngroupSelectedDevice());
    CHECK(context.deviceChainFocused && context.deviceChainFocused());

    // Cmd+G with the Device View in focus groups the devices, not the tracks.
    root.dispatchKey(cmdG(false));
    CHECK(trackGroups == 0);
    REQUIRE(song.tracks[1].devices.size() == 2);
    CHECK(song.tracks[1].devices[0].type == "AudioEffectGroupDevice");
    CHECK(song.tracks[1].devices[0].chains[0].devices.size() == 2);
    root.layout();
    std::vector<ui::DetailView*> details;
    collect(root, details);
    REQUIRE(details.size() == 1);
    CHECK(details[0]->children().size() == 4); // the rack, its two devices, the Reverb
    CHECK(context.canUngroupSelectedDevice());
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_device_group.png");
    CHECK(gfx::renderToPng(root, 1700, 900, 1.0f, png.getFullPathName().toStdString()));

    // Shift+Cmd+G ungroups the selected rack.
    root.dispatchKey(cmdG(true));
    REQUIRE(song.tracks[1].devices.size() == 3);
    CHECK(song.tracks[1].devices[0].id == utility);
    CHECK(song.tracks[1].devices[1].id == eq);

    // Focus elsewhere (the Arrangement): Cmd+G groups tracks.
    root.setKeyboardFocus(&root);
    CHECK(! context.deviceChainFocused());
    root.dispatchKey(cmdG(false));
    CHECK(trackGroups == 1);
    CHECK(song.tracks[1].devices.size() == 3);
}

TEST_CASE("device groups engine: a hosted plug-in keeps its running instance going into a rack and back out (DEV-15c)")
{
#if JUCE_MAC
    juce::ScopedJuceInitialiser_GUI init;
    const auto bundle = phaseAlignBundle();
    if (! bundle.exists())
    {
        std::printf("    (skipped: build SonoraPhaseAlign_VST3 first)\n");
        return;
    }
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, bundle.getFullPathName());
    REQUIRE(types.size() == 1);
    plugins::PluginHost host;
    host.knownPlugins().addType(*types[0]);
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);

    model::Song song;
    model::Track bass;
    bass.id = 3;
    bass.name = "Bass";
    bass.kind = model::TrackKind::Audio;
    auto utility = devices::nativeDeviceModel("StereoGain");
    model::allocateDeviceIds(song, utility);
    model::Device plugin;
    plugin.kind = model::DeviceKind::PluginVst3;
    plugin.type = "PluginDevice";
    plugin.displayName = "Sonora Phase Align";
    model::PluginInfo info;
    info.name = "Sonora Phase Align";
    plugin.plugin = info;
    model::allocateDeviceIds(song, plugin);
    auto eq = devices::nativeDeviceModel("Eq8");
    model::allocateDeviceIds(song, eq);
    bass.devices = {utility, plugin, eq};
    song.tracks.push_back(bass);
    eng.loadSong(song);
    auto* graph = eng.latestGraph();
    REQUIRE(graph != nullptr);
    auto* running = dynamic_cast<plugins::PluginDevice*>(graph->nodes[0].processorForModelDevice[1]);
    REQUIRE(running != nullptr);
    auto* instance = &running->instance();

    // Group the plug-in and the EQ: the new rack's chain runs the same instance.
    model::EditHistory history;
    history.perform(song, model::groupDevices(song, 0, {plugin.id, eq.id}, groupRack(song, "AudioEffectGroupDevice")));
    eng.updateTrackDevices(0, song);
    CHECK(eng.lastReusedPlugins() == 1);
    graph = eng.latestGraph();
    auto* rack = dynamic_cast<devices::RackProcessor*>(graph->nodes[0].processorForModelDevice[1]);
    REQUIRE(rack != nullptr);
    REQUIRE(rack->chains().size() == 1);
    auto* inRack = dynamic_cast<plugins::PluginDevice*>(rack->chains()[0].processorForModelDevice[0]);
    REQUIRE(inRack != nullptr);
    CHECK(&inRack->instance() == instance);
    CHECK(graph->nodes[0].processorForModelDevice[0] != nullptr); // the Utility before the rack is left alone
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.renderBlock(out, 2, 512);
    CHECK(std::isfinite(l[100]));

    // Ungroup: back at the track level, still the same instance.
    history.perform(song, model::ungroupDevice(song, 0, song.tracks[0].devices[1].id));
    eng.updateTrackDevices(0, song);
    CHECK(eng.lastReusedPlugins() == 1);
    graph = eng.latestGraph();
    auto* back = dynamic_cast<plugins::PluginDevice*>(graph->nodes[0].processorForModelDevice[1]);
    REQUIRE(back != nullptr);
    CHECK(&back->instance() == instance);
    // Undo the ungroup and the group: the instance follows every step.
    history.undo(song);
    eng.updateTrackDevices(0, song);
    graph = eng.latestGraph();
    rack = dynamic_cast<devices::RackProcessor*>(graph->nodes[0].processorForModelDevice[1]);
    REQUIRE(rack != nullptr);
    CHECK(&dynamic_cast<plugins::PluginDevice*>(rack->chains()[0].processorForModelDevice[0])->instance() == instance);
    history.undo(song);
    eng.updateTrackDevices(0, song);
    graph = eng.latestGraph();
    back = dynamic_cast<plugins::PluginDevice*>(graph->nodes[0].processorForModelDevice[1]);
    REQUIRE(back != nullptr);
    CHECK(&back->instance() == instance);
    eng.renderBlock(out, 2, 512);
    CHECK(std::isfinite(r[100]));
#endif
}
