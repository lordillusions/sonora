// PLG-22: sidechain like the reference: a hosted plug-in's sidechain input gets the routed track (Gain, Mix), and the
// device chain's Sidechain panel (source, tap, Gain, Mix, Mute, the arrow) edits it.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/HushSession.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugins/PluginDevice.h"
#include "ui/views/MainLayout.h"
#include "ui/views/SidechainPanel.h"
#include "ui/widgets/Buttons.h"
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

// The Sonora Phase Align VST3 built next to the tests: a real VST3 with a sidechain bus, hosted like any third-party one.
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
} // namespace

TEST_CASE("sidechain: a hosted VST3's sidechain bus gets the routed track only when routed, scaled by Gain and Mix")
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
    const auto make = [&](bool routed) {
        juce::String error;
        auto instance = format.createInstanceFromDescription(*types[0], 48000.0, 512, error);
        auto device = std::make_unique<plugins::PluginDevice>(std::move(instance));
        device->setSidechainRouted(routed);
        device->prepare(48000.0, 512);
        for (int i = 0; i < (int) device->parameters().size(); ++i) // Listen = "Sum": the output carries the sidechain
            if (device->parameters()[(size_t) i].name == "Listen") device->setParameter(i, 0.5f);
        return device;
    };
    // Silence on the main input, a 60 Hz tone on the routed track: whatever comes out came through the sidechain bus.
    juce::AudioBuffer<float> key(2, 512);
    const auto run = [&](plugins::PluginDevice& device, float gain) {
        float peak = 0;
        for (int block = 0; block < 40; ++block)
        {
            for (int s = 0; s < 512; ++s)
                for (int ch = 0; ch < 2; ++ch) key.setSample(ch, s, 0.5f * std::sin(6.2831853f * 60.0f * (float) (block * 512 + s) / 48000.0f));
            juce::AudioBuffer<float> audio(2, 512);
            audio.clear();
            juce::MidiBuffer midi;
            device.setSidechain(&key, gain);
            device.process(audio, midi);
            if (block > 4) peak = std::max(peak, audio.getMagnitude(0, 512));
        }
        return peak;
    };
    auto routed = make(true);
    CHECK(routed->takesSidechain());
    CHECK(routed->sidechainBusActive());
    CHECK_NEAR(run(*routed, 1.0f), 0.5, 0.05);
    CHECK_NEAR(run(*routed, 0.5f), 0.25, 0.03); // Gain
    routed->setSidechainLevels(1.0f, 0.0f);    // Mix 0: the key is the device's own (silent) input
    CHECK(run(*routed, 1.0f) < 1e-4f);
    auto unrouted = make(false);
    CHECK(! unrouted->sidechainBusActive());   // not routed: the bus stays off (plug-ins notice an active bus)
    CHECK(run(*unrouted, 1.0f) < 1e-4f);
#endif
}

TEST_CASE("sidechain panel: on Hush in the chain, pick the kick track, tap, Mute, Gain and Mix; the arrow folds it")
{
    model::Song song;
    song.tracks.resize(2);
    song.tracks[0].id = 4;
    song.tracks[0].name = "Kick";
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[1].id = 5;
    song.tracks[1].name = "Bass";
    song.tracks[1].kind = model::TrackKind::Audio;
    auto hushModel = devices::nativeDeviceModel("SonoraHush");
    hushModel.id = 40;
    song.tracks[1].devices = {hushModel};
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
    std::vector<std::string> labels;
    context.editDevice = [&](int track, int device, std::function<void(model::Device&)> edit, std::string label) {
        edit(song.tracks[(size_t) track].devices[(size_t) device]);
        labels.push_back(label);
    };
    float savedGain = -1, savedMix = -1;
    context.setDeviceSidechainLevels = [&](int, int, float gain, float mix) { savedGain = gain; savedMix = mix; };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1700, 900});
    root.layout();
    std::vector<ui::SidechainPanel*> panels;
    collect(root, panels);
    REQUIRE(panels.size() == 1);
    auto& panel = *panels[0];
    CHECK(panel.visible);
    CHECK(panel.sourceLabel() == "No Input");
    CHECK(panel.tapChoices().empty());

    auto sources = panel.sourceChoices();
    REQUIRE(sources.size() == 2); // No Input, Kick (not Bass itself)
    CHECK(sources[0].ticked);
    sources[1].run();
    const auto& sc = song.tracks[1].devices[0].sidechain;
    CHECK(sc.routed());
    CHECK(sc.sourceTrackId == 4);
    CHECK_NEAR(song.tracks[1].devices[0].param("Sidechain_on", -1), 1.0, 1e-9);
    CHECK(labels.back() == "Sidechain from Kick");
    CHECK(panel.sourceLabel() == "Kick");
    CHECK(panel.tapLabel() == "Post FX");
    auto taps = panel.tapChoices();
    REQUIRE(taps.size() == 3);
    taps[0].run();
    CHECK(sc.tap == model::Sidechain::Tap::PreFx);
    panel.toggleMute();
    CHECK(! sc.on);
    CHECK(! sc.routed());
    CHECK(sc.sourceTrackId == 4); // muted keeps the source
    panel.toggleMute();
    CHECK(sc.routed());

    // Gain and Mix knobs: live on the device, into the model.
    std::vector<ui::Knob*> knobs;
    collect(panel, knobs);
    REQUIRE(knobs.size() == 2);
    auto* hush = graph.graph->nodes[1].processorForModelDevice[0];
    REQUIRE(hush != nullptr);
    knobs[0]->source().set(-6.0f);
    CHECK_NEAR(hush->sidechainGainLevel(), std::pow(10.0f, -6.0f / 20.0f), 1e-4);
    CHECK_NEAR(savedGain, std::pow(10.0f, -6.0f / 20.0f), 1e-4);
    knobs[1]->source().set(0.5f);
    CHECK_NEAR(hush->sidechainMixLevel(), 0.5, 1e-6);
    CHECK_NEAR(savedMix, 0.5, 1e-6);

    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sidechain_panel.png");
    CHECK(gfx::renderToPng(root, 1700, 900, 1.0f, png.getFullPathName().toStdString()));

    // The arrow folds the panel away (and back).
    std::vector<ui::TextButton*> buttons;
    collect(root, buttons);
    ui::TextButton* arrow = nullptr;
    for (auto* b : buttons) if (b->tooltip == "Show or hide the Sidechain section") arrow = b;
    REQUIRE(arrow != nullptr);
    ui::MouseEvent click;
    const auto at = arrow->boundsInRoot();
    click.x = at.x + 4;
    click.y = at.y + 4;
    root.dispatchMouseDown(click);
    root.dispatchMouseUp(click);
    CHECK(context.sidechainPanelsClosed.count(40) == 1);
    CHECK(! panel.visible);
}

#include "plugins/PluginHost.h"
#include "engine/AudioEngine.h"
#include "midi/MidiManager.h"

TEST_CASE("engine rebuild: a running plug-in instance is handed to the new graph instead of being created again")
{
    // Duplicate Tracks rebuilt the engine and created every plug-in again; SoundID Reference blocked forever in its
    // initialise doing that. The running instance is now donated to the host and claimed by the next build.
#if JUCE_MAC
    juce::ScopedJuceInitialiser_GUI init;
    const auto bundle = phaseAlignBundle();
    if (! bundle.exists()) return;
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, bundle.getFullPathName());
    REQUIRE(types.size() == 1);
    juce::String error;
    auto device = std::make_unique<plugins::PluginDevice>(format.createInstanceFromDescription(*types[0], 48000.0, 512, error));
    device->prepare(48000.0, 512);
    auto released = device->releaseInstance();
    REQUIRE(released != nullptr);
    auto* running = released.get();
    device.reset(); // an emptied device is destroyed without touching the instance

    plugins::PluginHost host;
    model::PluginInfo info;
    info.name = "Sonora Phase Align";
    host.donate(*types[0], info, model::DeviceKind::PluginVst3, 48000.0, 512, std::move(released));
    CHECK(host.preloadedCount() == 1);
    auto claimed = host.instantiate(*types[0], info, model::DeviceKind::PluginVst3, 48000.0, 512, error);
    CHECK(claimed.get() == running); // the same instance, not a new one
    CHECK(host.preloadedCount() == 0);
    // A different state (another device of the same plug-in) does not take it.
    info.state = {1, 2, 3};
    host.donate(*types[0], {}, model::DeviceKind::PluginVst3, 48000.0, 512, std::move(claimed));
    auto other = host.instantiate(*types[0], info, model::DeviceKind::PluginVst3, 48000.0, 512, error);
    CHECK(other.get() != running);
    host.clearPreloaded();
#endif
}

TEST_CASE("engine rebuild: Duplicate Tracks keeps the running plug-ins of the set (no new instances)")
{
#if JUCE_MAC
    juce::ScopedJuceInitialiser_GUI init;
    const auto bundle = phaseAlignBundle();
    if (! bundle.exists()) return;
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
    model::Device plugin;
    plugin.kind = model::DeviceKind::PluginVst3;
    plugin.type = "PluginDevice";
    plugin.id = 11;
    plugin.displayName = "Sonora Phase Align";
    model::PluginInfo info;
    info.name = "Sonora Phase Align";
    plugin.plugin = info;
    bass.devices.push_back(plugin);
    song.tracks.push_back(bass);
    song.main.devices.push_back(plugin); // a plug-in on Main too (where SoundID sat)
    song.main.devices.back().id = 12;
    eng.loadSong(song);
    auto* graph = eng.latestGraph();
    REQUIRE(graph != nullptr);
    auto* onBass = dynamic_cast<plugins::PluginDevice*>(graph->nodes[0].processorForModelDevice[0]);
    auto* onMain = dynamic_cast<plugins::PluginDevice*>(graph->nodes[(size_t) graph->mainNode].processorForModelDevice[0]);
    REQUIRE(onBass != nullptr);
    REQUIRE(onMain != nullptr);
    auto* bassInstance = &onBass->instance();
    auto* mainInstance = &onMain->instance();

    // Duplicate the bass track: the copy is a new device id (a new instance); the original and Main keep theirs.
    auto duplicated = song;
    auto copy = bass;
    copy.id = 4;
    copy.name = "Bass 2";
    copy.devices[0].id = 13;
    duplicated.tracks.push_back(copy);
    eng.reloadSongKeepingPlugins(duplicated);
    CHECK(eng.lastReusedPlugins() == 2);
    graph = eng.latestGraph();
    REQUIRE(graph != nullptr);
    auto* bassAfter = dynamic_cast<plugins::PluginDevice*>(graph->nodes[0].processorForModelDevice[0]);
    auto* copyAfter = dynamic_cast<plugins::PluginDevice*>(graph->nodes[1].processorForModelDevice[0]);
    auto* mainAfter = dynamic_cast<plugins::PluginDevice*>(graph->nodes[(size_t) graph->mainNode].processorForModelDevice[0]);
    REQUIRE(bassAfter != nullptr);
    REQUIRE(copyAfter != nullptr);
    REQUIRE(mainAfter != nullptr);
    CHECK(&bassAfter->instance() == bassInstance);
    CHECK(&mainAfter->instance() == mainInstance);
    CHECK(&copyAfter->instance() != bassInstance);
    // Audio still runs through them.
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.renderBlock(out, 2, 512);
    CHECK(std::isfinite(l[100]));
#endif
}
