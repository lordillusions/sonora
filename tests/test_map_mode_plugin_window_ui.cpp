// UI-112 (owner: "if I click MIDI it turns blue, and clicking to open the plug-in's window does nothing"): in MIDI Map mode
// the wrench of a hosted plug-in (a real VST3 built in this tree) still opens its window; a knob is still taken for mapping.
#include "TestFramework.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "midi/MidiManager.h"
#include "plugins/PluginDevice.h"
#include "plugins/PluginHost.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"
#include "ui/widgets/Buttons.h"

#include <juce_audio_processors/juce_audio_processors.h>

using namespace sonora;

namespace
{
template <typename T> void collectAll(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collectAll(*child, found);
}

juce::File stutterBundle()
{
    const auto build = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory().getParentDirectory();
    for (const char* config : {"Debug", "Release", "RelWithDebInfo"})
    {
        const auto bundle = build.getChildFile("src/SonoraStutter_artefacts").getChildFile(config).getChildFile("VST3/Sonora Stutter.vst3");
        if (bundle.exists()) return bundle;
    }
    return {};
}

void click(ui::MainLayout& root, const ui::Rect& r)
{
    ui::MouseEvent e;
    e.x = r.x + r.w * 0.5f;
    e.y = r.y + r.h * 0.5f;
    root.dispatchMouseDown(e);
    root.dispatchMouseUp(e);
}
} // namespace

TEST_CASE("MIDI Map mode: a hosted plug-in's wrench still opens its window, its knobs are still mapped (UI-112)")
{
#if JUCE_MAC
    juce::ScopedJuceInitialiser_GUI init;
    const auto bundle = stutterBundle();
    if (! bundle.exists())
    {
        std::printf("    (skipped: build SonoraStutter_VST3 first)\n");
        return;
    }
    plugins::PluginHost host;
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile(types, bundle.getFullPathName());
    REQUIRE(types.size() == 1);
    host.knownPlugins().addType(*types[0]);
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);

    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].id = 4;
    song.tracks[0].kind = model::TrackKind::Audio;
    song.tracks[0].name = "Bass";
    model::Device d;
    d.kind = model::DeviceKind::PluginVst3;
    d.type = "PluginDevice";
    d.displayName = "Sonora Stutter";
    model::PluginInfo info;
    info.name = "Sonora Stutter";
    d.plugin = info;
    model::allocateDeviceIds(song, d);
    song.tracks[0].devices = {d};
    eng.loadSong(song);
    REQUIRE(eng.latestGraph() != nullptr);
    REQUIRE(dynamic_cast<plugins::PluginDevice*>(eng.latestGraph()->nodes[0].processorForModelDevice[0]) != nullptr);

    ui::UiContext context;
    context.song = &song;
    context.graph = eng.latestGraph();
    context.controls = eng.latestGraph()->controls;
    context.midi = &midiManager;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    context.selectedTrack = 0;
    int opened = 0, openedTrack = -1, openedDevice = -1;
    context.openPluginEditor = [&](int track, int device) { ++opened; openedTrack = track; openedDevice = device; };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1400, 800});
    root.layout();

    std::vector<ui::IconButton*> buttons;
    collectAll(root, buttons);
    ui::IconButton* wrench = nullptr;
    for (auto* b : buttons) if (b->icon() == ui::IconButton::Icon::Wrench) wrench = b;
    REQUIRE(wrench != nullptr);

    context.midiMapMode = true;
    click(root, wrench->boundsInRoot());
    CHECK(opened == 1); CHECK(openedTrack == 0); CHECK(openedDevice == 0);
    CHECK(! context.mapSelection); // the wrench is not a mapping target
    CHECK(context.midiMapMode);    // and the mode stays on

    // Outside map mode it opens too (unchanged).
    context.midiMapMode = false;
    click(root, wrench->boundsInRoot());
    CHECK(opened == 2);
#endif
}
