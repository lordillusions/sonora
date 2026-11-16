// DEV-15d: grouping hosted plug-ins with Cmd+G. Real VST3s (the Sonora ones built in this tree) hosted by AudioEngine,
// selected in the Device View with clicks, grouped with Cmd+G into one Audio Effect Rack, in chain order, keeping their
// running instances and state; the audio still flows.
#include "TestFramework.h"
#include "core/DeviceGroups.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "plugins/PluginDevice.h"
#include "plugins/PluginHost.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"
#include "ui/widgets/Knob.h"

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

juce::File sonoraBundle(const char* target, const char* product)
{
    const auto build = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getParentDirectory();
    for (const char* config : {"Debug", "Release", "RelWithDebInfo"})
    {
        const auto bundle = build.getChildFile("src").getChildFile(juce::String(target) + "_artefacts").getChildFile(config)
                                .getChildFile("VST3").getChildFile(juce::String(product) + ".vst3");
        if (bundle.exists()) return bundle;
    }
    return {};
}

ui::DetailView& detailOf(ui::MainLayout& root)
{
    std::vector<ui::DetailView*> details;
    collect(root, details);
    REQUIRE(details.size() == 1);
    return *details[0];
}

// Presses at (x, y) inside the n-th device box of the Device View.
void pressBox(ui::MainLayout& root, int box, float x, float y, bool shift = false, bool cmd = false)
{
    auto& detail = detailOf(root);
    REQUIRE((size_t) box < detail.children().size());
    const auto at = detail.children()[(size_t) box]->boundsInRoot();
    ui::MouseEvent e;
    e.x = at.x + x;
    e.y = at.y + y;
    e.shift = shift;
    e.cmd = cmd;
    root.dispatchMouseDown(e);
    root.dispatchMouseUp(e);
}

ui::KeyEvent cmdG()
{
    ui::KeyEvent k;
    k.keyCode = 'G';
    k.textChar = 'g';
    k.cmd = true;
    return k;
}
} // namespace

TEST_CASE("device groups UI: hosted plug-ins selected with clicks go into one Audio Effect Rack with Cmd+G (DEV-15d)")
{
#if JUCE_MAC
    juce::ScopedJuceInitialiser_GUI init;
    const auto phaseAlign = sonoraBundle("SonoraPhaseAlign", "Sonora Phase Align");
    const auto stutter = sonoraBundle("SonoraStutter", "Sonora Stutter");
    if (! phaseAlign.exists() || ! stutter.exists())
    {
        std::printf("    (skipped: build SonoraPhaseAlign_VST3 and SonoraStutter_VST3 first)\n");
        return;
    }
    plugins::PluginHost host;
    juce::VST3PluginFormat format;
    for (const auto& bundle : {phaseAlign, stutter})
    {
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile(types, bundle.getFullPathName());
        REQUIRE(types.size() == 1);
        host.knownPlugins().addType(*types[0]);
    }
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);

    model::Song song;
    song.tracks.resize(2);
    for (int i = 0; i < 2; ++i)
    {
        song.tracks[(size_t) i].id = 4 + i;
        song.tracks[(size_t) i].kind = model::TrackKind::Audio;
        song.tracks[(size_t) i].name = i == 0 ? "Drums" : "Bass";
    }
    const auto hosted = [&](const char* name) {
        model::Device d;
        d.kind = model::DeviceKind::PluginVst3;
        d.type = "PluginDevice";
        d.displayName = name;
        model::PluginInfo info;
        info.name = name;
        d.plugin = info;
        model::allocateDeviceIds(song, d);
        return d;
    };
    auto utility = devices::nativeDeviceModel("StereoGain");
    model::allocateDeviceIds(song, utility);
    auto eq = devices::nativeDeviceModel("Eq8");
    model::allocateDeviceIds(song, eq);
    song.tracks[1].devices = {utility, hosted("Sonora Phase Align"), hosted("Sonora Stutter"), hosted("Sonora Phase Align"), eq};
    eng.loadSong(song);
    REQUIRE(eng.latestGraph() != nullptr);
    std::vector<juce::AudioPluginInstance*> instances;
    for (size_t i = 1; i <= 3; ++i)
    {
        auto* running = dynamic_cast<plugins::PluginDevice*>(eng.latestGraph()->nodes[1].processorForModelDevice[i]);
        REQUIRE(running != nullptr);
        instances.push_back(&running->instance());
    }

    ui::UiContext context;
    context.song = &song;
    const auto bindGraph = [&] {
        context.graph = eng.latestGraph();
        context.controls = eng.latestGraph()->controls;
    };
    bindGraph();
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    context.selectedTrack = 1;
    model::EditHistory history;
    int trackGroups = 0;
    context.groupSelectedTracks = [&] { ++trackGroups; };
    context.groupSelectedDevices = [&] {
        const auto ids = context.selectedDeviceIds();
        const auto* list = model::deviceListAt(song.tracks[1], context.selectedDevicePath());
        int first = 0, count = 0;
        if (! list || ! model::groupableDevices(*list, ids, first, count)) return;
        const auto type = model::groupRackType(*list, first, count);
        if (type.empty()) return;
        auto rack = devices::nativeDeviceModel(type);
        rack.displayName = model::groupRackName(type);
        rack.numVisibleMacros = 8;
        rack.id = -1;
        model::allocateDeviceIds(song, rack);
        const int id = rack.id;
        history.perform(song, model::groupDevices(song, 1, ids, std::move(rack)));
        eng.updateTrackDevices(1, song);
        bindGraph();
        context.selectedRackId = id;
        context.selectedRackChain = 0;
        context.selectDevice(1, {}, id, false, false);
    };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1900, 900});
    root.layout();

    const auto& devices = song.tracks[1].devices;
    const int alignA = devices[1].id, stut = devices[2].id, alignB = devices[3].id;
    const auto ids = [&] { return context.selectedDeviceIds(); };
    // Each plug-in keeps its own state through the move: a parameter set on each, read back from the rack's chain.
    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto& params = instances[i]->getParameters();
        REQUIRE(! params.isEmpty());
        params[0]->setValue(0.2f + 0.3f * (float) i);
    }
    std::vector<float> values;
    for (auto* instance : instances) values.push_back(instance->getParameters()[0]->getValue());

    // The reported gesture: the first plug-in's title, then Shift / Cmd on the other plug-ins' bodies (the pad, the
    // Sidechain panel), which fill a plug-in's box. Before DEV-15d each of those presses selected that plug-in alone.
    pressBox(root, 1, 70, 8);
    CHECK(ids() == std::vector<int>{alignA});
    pressBox(root, 2, 75, 100, true); // Stutter's pad
    CHECK(ids() == (std::vector<int>{alignA, stut}));
    pressBox(root, 3, 8, 80, false, true); // the second Phase Align's Sidechain panel (its background)
    CHECK(ids() == (std::vector<int>{alignA, stut, alignB}));
    // Shift on a knob is a fine move: it leaves the selection alone.
    std::vector<ui::Knob*> knobs;
    collect(*detailOf(root).children()[3], knobs);
    REQUIRE(! knobs.empty());
    {
        const auto at = knobs.front()->boundsInRoot();
        ui::MouseEvent e;
        e.x = at.centreX();
        e.y = at.centreY();
        e.shift = true;
        root.dispatchMouseDown(e);
        root.dispatchMouseUp(e);
    }
    CHECK(ids() == (std::vector<int>{alignA, stut, alignB}));
    // A plain press on a body still selects that device alone; Shift on a title ranges from it, Cmd adds.
    pressBox(root, 2, 75, 100);
    CHECK(ids() == std::vector<int>{stut});
    pressBox(root, 1, 70, 8, true);
    pressBox(root, 3, 70, 8, false, true);
    CHECK(ids() == (std::vector<int>{alignA, stut, alignB}));
    CHECK(context.canGroupSelectedDevices());
    CHECK(context.deviceChainFocused && context.deviceChainFocused());

    // Cmd+G: one Audio Effect Rack with one chain holding the three plug-ins in chain order, between Utility and EQ Eight.
    root.dispatchKey(cmdG());
    CHECK(trackGroups == 0);
    REQUIRE(devices.size() == 3);
    CHECK(devices[0].id == utility.id);
    CHECK(devices[2].id == eq.id);
    const auto& rackModel = devices[1];
    CHECK(rackModel.type == "AudioEffectGroupDevice");
    REQUIRE(rackModel.chains.size() == 1);
    REQUIRE(rackModel.chains[0].devices.size() == 3);
    CHECK(rackModel.chains[0].devices[0].id == alignA);
    CHECK(rackModel.chains[0].devices[1].id == stut);
    CHECK(rackModel.chains[0].devices[2].id == alignB);
    for (const auto& d : rackModel.chains[0].devices) CHECK(d.plugin.has_value());

    // The engine moved the running instances into the rack's chain (no new instances), with their parameters.
    CHECK(eng.lastReusedPlugins() == 3);
    auto* rack = dynamic_cast<devices::RackProcessor*>(eng.latestGraph()->nodes[1].processorForModelDevice[1]);
    REQUIRE(rack != nullptr);
    REQUIRE(rack->chains().size() == 1);
    REQUIRE(rack->chains()[0].processorForModelDevice.size() == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        auto* inRack = dynamic_cast<plugins::PluginDevice*>(rack->chains()[0].processorForModelDevice[i]);
        REQUIRE(inRack != nullptr);
        CHECK(&inRack->instance() == instances[i]);
        CHECK(std::abs(inRack->instance().getParameters()[0]->getValue() - values[i]) < 1e-6f);
    }

    // The Device View shows the rack, its three plug-ins, then EQ Eight; the rack is the selection.
    root.layout();
    CHECK(detailOf(root).children().size() == 6);
    CHECK(ids() == std::vector<int>{rackModel.id});
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_plugin_group.png");
    CHECK(gfx::renderToPng(root, 1900, 900, 1.0f, png.getFullPathName().toStdString()));
    std::printf("    %s\n", png.getFullPathName().toRawUTF8());

    // Audio flows through the rack: a sine in comes out of the three plug-ins, finite and not silent.
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    double inEnergy = 0, outEnergy = 0;
    for (int block = 0; block < 40; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const float s = 0.25f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * (double) (block * 512 + i) / 48000.0);
            audio.setSample(0, i, s);
            audio.setSample(1, i, s);
            if (block >= 20) inEnergy += (double) s * s;
        }
        rack->process(audio, midi);
        if (block >= 20)
            for (int i = 0; i < 512; ++i)
            {
                REQUIRE(std::isfinite(audio.getSample(0, i)));
                outEnergy += (double) audio.getSample(0, i) * audio.getSample(0, i);
            }
    }
    std::printf("    rack out / in energy %.3f\n", outEnergy / inEnergy);
    CHECK(outEnergy > 0.1 * inEnergy);

    // Undo puts the three plug-ins back on the track, still the same instances.
    history.undo(song);
    eng.updateTrackDevices(1, song);
    REQUIRE(devices.size() == 5);
    for (size_t i = 0; i < 3; ++i)
    {
        auto* back = dynamic_cast<plugins::PluginDevice*>(eng.latestGraph()->nodes[1].processorForModelDevice[i + 1]);
        REQUIRE(back != nullptr);
        CHECK(&back->instance() == instances[i]);
    }
#endif
}
