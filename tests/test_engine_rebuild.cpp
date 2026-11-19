// ENG-17: structural edits (Insert / Delete / Duplicate / Group Track) hand the running devices of the tracks they leave
// alone to the new graph instead of creating them again. Creating every device again took seconds on a set with plug-ins
// and hung the app when SoundID Reference was created a second time (Insert Audio Track, 0.1.52).
#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "engine/SampleCache.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "io/als/AlsReader.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <juce_events/juce_events.h>

#include <cmath>

using namespace sonora;

namespace
{
model::Device native(const std::string& type, int id)
{
    auto d = devices::nativeDeviceModel(type);
    d.id = id;
    return d;
}

model::Track track(model::TrackId id, model::TrackKind kind, const std::string& name)
{
    model::Track t;
    t.id = id;
    t.kind = kind;
    t.name = name;
    return t;
}

// Two tracks with native devices and a device on Main: what the tests rebuild.
model::Song twoTracks()
{
    model::Song song;
    song.tempo = 120.0;
    auto lead = track(30, model::TrackKind::Midi, "Lead");
    lead.devices.push_back(native("Operator", 1));
    lead.devices.push_back(native("StereoGain", 2));
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi;
    clip.currentStart = 0.0;
    clip.currentEnd = 8.0;
    clip.loop = {0.0, 8.0, 0.0, true};
    for (double s = 0.0; s < 8.0; s += 0.5) clip.notes.push_back({s, 0.25, 60 + (int) s, 100.0f});
    lead.arrangementClips.push_back(clip);
    song.tracks.push_back(lead);
    auto pad = track(31, model::TrackKind::Audio, "Pad");
    pad.devices.push_back(native("Reverb", 3));
    song.tracks.push_back(pad);
    song.main.devices.push_back(native("Eq8", 4));
    return song;
}

model::Song withInsertedAudioTrack(const model::Song& song, int before)
{
    auto edited = song;
    int end = 0;
    auto audio = io::createAudioTrack(edited, "Audio", 18, -1, -1, end);
    auto insert = model::insertTracks({std::move(audio)}, end, "Insert Audio Track", before);
    insert.apply(edited);
    return edited;
}

const devices::DeviceProcessor* processorOf(engine::AudioEngine& eng, int node, size_t device)
{
    auto* graph = eng.latestGraph();
    if (graph == nullptr || node < 0 || (size_t) node >= graph->nodes.size()) return nullptr;
    const auto& n = graph->nodes[(size_t) node];
    return device < n.processorForModelDevice.size() ? n.processorForModelDevice[device] : nullptr;
}

std::vector<float> render(engine::AudioEngine& eng, int blocks)
{
    std::vector<float> all, l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    for (int b = 0; b < blocks; ++b)
    {
        eng.renderBlock(out, 2, 512);
        all.insert(all.end(), l.begin(), l.end());
        all.insert(all.end(), r.begin(), r.end());
    }
    return all;
}
} // namespace

TEST_CASE("engine rebuild (ENG-17): Insert and Delete Track keep the running devices of the other tracks")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);
    const auto song = twoTracks();
    eng.loadSong(song);
    const auto* operatorBefore = processorOf(eng, 0, 0);
    const auto* utilityBefore = processorOf(eng, 0, 1);
    const auto* reverbBefore = processorOf(eng, 1, 0);
    const auto* eqBefore = processorOf(eng, eng.latestGraph()->mainNode, 0);
    REQUIRE(operatorBefore != nullptr);
    REQUIRE(utilityBefore != nullptr);
    REQUIRE(reverbBefore != nullptr);
    REQUIRE(eqBefore != nullptr);

    // A new audio track between them: Lead stays node 0, Pad moves to node 2, Main keeps its Eq8.
    const auto inserted = withInsertedAudioTrack(song, 1);
    REQUIRE(inserted.tracks.size() == 3);
    eng.reloadSongKeepingPlugins(inserted);
    CHECK(eng.lastReusedDevices() == 4);
    CHECK(processorOf(eng, 0, 0) == operatorBefore);
    CHECK(processorOf(eng, 0, 1) == utilityBefore);
    CHECK(processorOf(eng, 2, 0) == reverbBefore);
    CHECK(processorOf(eng, eng.latestGraph()->mainNode, 0) == eqBefore);

    // Deleting it again keeps them too.
    auto removed = inserted;
    auto del = model::deleteTracks(removed, {1});
    REQUIRE(del.apply != nullptr);
    del.apply(removed);
    REQUIRE(removed.tracks.size() == 2);
    eng.reloadSongKeepingPlugins(removed);
    CHECK(eng.lastReusedDevices() == 4);
    CHECK(processorOf(eng, 0, 0) == operatorBefore);
    CHECK(processorOf(eng, 1, 0) == reverbBefore);
    CHECK(processorOf(eng, eng.latestGraph()->mainNode, 0) == eqBefore);

    eng.transport().play();
    const auto audio = render(eng, 20);
    float peak = 0.0f;
    bool finite = true;
    for (float v : audio) { finite = finite && std::isfinite(v); peak = std::max(peak, std::abs(v)); }
    CHECK(finite);
    CHECK(peak > 1e-4f); // the carried-over Operator still plays its clip
}

TEST_CASE("engine rebuild (ENG-17): a changed device, a track the edit names and plug-in-only keeping build new devices")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);
    const auto song = twoTracks();
    eng.loadSong(song);
    const auto* utilityBefore = processorOf(eng, 0, 1);
    const auto* reverbBefore = processorOf(eng, 1, 0);

    // Same id, another device: built again; the rest carries over.
    auto changed = withInsertedAudioTrack(song, 2);
    changed.tracks[1].devices[0] = native("Delay", 3);
    eng.reloadSongKeepingPlugins(changed);
    CHECK(eng.lastReusedDevices() == 3);
    CHECK(processorOf(eng, 1, 0) != reverbBefore);
    CHECK(processorOf(eng, 1, 0) != nullptr);
    CHECK(processorOf(eng, 0, 1) == utilityBefore);

    // The track the edit replaced (by id) is built again.
    const auto* delayNow = processorOf(eng, 1, 0);
    engine::AudioEngine::KeepRunning keep;
    keep.rebuild = {30};
    eng.reloadSongKeepingPlugins(changed, keep);
    CHECK(eng.lastReusedDevices() == 2); // Pad's Delay and Main's Eq8
    CHECK(processorOf(eng, 0, 1) != utilityBefore);
    CHECK(processorOf(eng, 1, 0) == delayNow);

    // Plug-in-only keeping (an edit that also changed devices, a failed edit): every native device is built from the model.
    const auto* utilityNow = processorOf(eng, 0, 1);
    engine::AudioEngine::KeepRunning pluginsOnly;
    pluginsOnly.wholeDevices = false;
    eng.reloadSongKeepingPlugins(changed, pluginsOnly);
    CHECK(eng.lastReusedDevices() == 0);
    CHECK(processorOf(eng, 0, 1) != utilityNow);

    // A frozen track builds its frozen audio, not its devices: nothing of it carries over.
    auto frozen = changed;
    frozen.tracks[0].frozen = true;
    eng.reloadSongKeepingPlugins(frozen);
    CHECK(eng.lastReusedDevices() == 2);
}

TEST_CASE("engine rebuild (ENG-17): a carried-over device renders exactly what a new one does")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midiManager;
    const auto song = twoTracks();

    engine::AudioEngine fresh(host, midiManager);
    fresh.setEngineThreads(1);
    fresh.loadSong(song);
    fresh.transport().play();
    const auto expected = render(fresh, 40);

    engine::AudioEngine rebuilt(host, midiManager);
    rebuilt.setEngineThreads(1);
    rebuilt.loadSong(song);
    const auto inserted = withInsertedAudioTrack(song, 0);
    rebuilt.reloadSongKeepingPlugins(inserted);
    auto removed = inserted;
    auto del = model::deleteTracks(removed, {0});
    del.apply(removed);
    rebuilt.reloadSongKeepingPlugins(removed);
    REQUIRE(rebuilt.lastReusedDevices() == 4);
    rebuilt.transport().play();
    const auto got = render(rebuilt, 40);
    REQUIRE(got.size() == expected.size());
    size_t differing = 0;
    for (size_t i = 0; i < got.size(); ++i)
        if (std::abs(got[i] - expected[i]) > 0.0f) ++differing;
    CHECK(differing == 0);
}

namespace
{
// Remembers the sidechain it was last pointed at, like Compressor, Phase Align, Hush and hosted plug-ins do.
class SidechainSpy final : public devices::DeviceProcessor
{
public:
    std::string name() const override { return "spy"; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    bool takesSidechain() const override { return true; }
    void setSidechain(const juce::AudioBuffer<float>* audio, float) override { last = audio; ++calls; }
    const juce::AudioBuffer<float>* last = nullptr;
    int calls = 0;
};
} // namespace

TEST_CASE("engine rebuild (ENG-17): deleting a sidechain source leaves the carried-over listener without the old tap")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugins::PluginHost host;
    midi::MidiManager midiManager;
    engine::AudioEngine eng(host, midiManager);
    model::Song song;
    song.tracks.push_back(track(30, model::TrackKind::Audio, "Kick"));
    auto bass = track(31, model::TrackKind::Audio, "Bass");
    auto meter = native("SonoraCorrelation", 5);
    meter.sidechain.on = true;
    meter.sidechain.sourceTrackId = 30;
    bass.devices.push_back(meter);
    song.tracks.push_back(bass);
    eng.loadSong(song); // the engine's model of what runs
    // The same graph with the listener played by the spy.
    engine::SampleCache samples;
    auto built = engine::buildGraph(song, {}, samples);
    auto& node = built.graph->nodes[1];
    REQUIRE(node.sidechains.size() == 1);
    auto spy = std::make_unique<SidechainSpy>();
    auto* listener = spy.get();
    node.devices[0] = std::move(spy);
    node.processorForModelDevice[0] = listener;
    node.sidechains[0].device = listener;
    eng.installGraph(std::move(built.graph));
    eng.transport().play();
    render(eng, 2);
    REQUIRE(listener->calls > 0);
    CHECK(listener->last != nullptr); // the kick's tap

    // The kick goes; the device (and its routing in the model) stays: the new graph finds no source and does not link it.
    auto removed = song;
    removed.tracks.erase(removed.tracks.begin());
    eng.reloadSongKeepingPlugins(removed);
    CHECK(eng.lastReusedDevices() == 1);
    REQUIRE(processorOf(eng, 0, 0) == listener);
    CHECK(eng.latestGraph()->nodes[0].sidechains.empty());
    CHECK(listener->last == nullptr); // no pointer into the old graph's kick tap, which is gone
    render(eng, 2);
}
