// ENG-14e: the render schedule (dependency order for several threads), the worker pool, and that a parallel render of a
// mixed graph (groups, nested groups, sends, returns, a sidechain, delay compensation, loop wraps) is bit-identical to the
// serial one.
#include "TestFramework.h"

#include "engine/AudioEngine.h"
#include "engine/EngineWorkers.h"
#include "engine/RenderSchedule.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <algorithm>
#include <array>
#include <atomic>

using namespace sonora;

namespace
{
int jobOf(const engine::RenderSchedule& s, int node)
{
    for (size_t j = 0; j < s.jobs.size(); ++j)
        if (s.jobs[j].node == node) return (int) j;
    return -1;
}

// Is `before` a (transitive) predecessor of `after`?
bool precedes(const engine::RenderSchedule& s, int before, int after)
{
    std::vector<int> stack{before};
    std::vector<char> seen(s.jobs.size(), 0);
    while (! stack.empty())
    {
        const int j = stack.back();
        stack.pop_back();
        for (const int next : s.jobs[(size_t) j].successors)
        {
            if (next == after) return true;
            if (! seen[(size_t) next]) { seen[(size_t) next] = 1; stack.push_back(next); }
        }
    }
    return false;
}

// Deterministic per-track noise with state across blocks: any reordering or race changes the output.
class Noise final : public devices::DeviceProcessor
{
public:
    Noise(uint32_t seed, float gain) : state_(seed), gain_(gain) {}
    std::string name() const override { return "noise"; }
    bool isInstrument() const override { return true; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>& a, juce::MidiBuffer&) override
    {
        for (int i = 0; i < a.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                state_ = state_ * 1664525u + 1013904223u;
                a.addSample(ch, i, gain_ * ((float) (state_ >> 8) * (1.0f / 16777216.0f) - 0.5f));
            }
    }

private:
    uint32_t state_;
    float gain_;
};

model::Device nativeDevice(const std::string& type)
{
    model::Device d;
    d.kind = model::DeviceKind::Native;
    d.type = type;
    return d;
}

// Groups (one nested), top-level tracks, two returns every track sends to, a compressor keyed from another track.
model::Song mixedSong()
{
    model::Song song;
    int id = 100;
    auto track = [&](model::TrackKind kind, const std::string& name, int group) {
        model::Track t;
        t.id = id++;
        t.kind = kind;
        t.name = name;
        t.groupId = group;
        t.mixer.sends = {0.3, 0.15};
        t.mixer.volume = 0.8;
        song.tracks.push_back(t);
        return t.id;
    };
    const int g1 = track(model::TrackKind::Group, "G1", -1);
    const int kick = track(model::TrackKind::Audio, "kick", g1);
    track(model::TrackKind::Audio, "snare", g1);
    const int g2 = track(model::TrackKind::Group, "G2", g1);
    track(model::TrackKind::Audio, "hat", g2);
    track(model::TrackKind::Audio, "perc", g2);
    for (int i = 0; i < 6; ++i) track(model::TrackKind::Audio, "top" + std::to_string(i), -1);
    track(model::TrackKind::Audio, "bass", -1);
    song.tracks[2].devices.push_back(nativeDevice("Eq8"));
    song.tracks[4].devices.push_back(nativeDevice("AutoFilter2"));
    song.tracks[7].devices.push_back(nativeDevice("Delay"));
    song.tracks[8].trackDelayMs = 3.0; // delay compensation on every other path
    auto comp = nativeDevice("Compressor2");
    comp.sidechain.on = true;
    comp.sidechain.sourceTrackId = kick;
    comp.sidechain.tap = model::Sidechain::Tap::PostFx;
    comp.parameters.push_back({"Threshold", 0.1, -1, {}});
    comp.parameters.push_back({"Ratio", 8.0, -1, {}});
    song.tracks.back().devices.push_back(comp);
    for (const char* name : {"A-Reverb", "B-Delay"})
    {
        model::Track r;
        r.id = id++;
        r.kind = model::TrackKind::Return;
        r.name = name;
        r.devices.push_back(nativeDevice(name[0] == 'A' ? "Reverb" : "Delay"));
        song.returns.push_back(r);
    }
    song.main.id = id++;
    song.main.kind = model::TrackKind::Main;
    song.main.name = "Main";
    song.loopOn = true;
    song.loopStart = 0.0;
    song.loopLength = 3.0; // wraps inside blocks
    return song;
}

// Renders the mixed song on an engine using `threads` threads in all and returns the Main output.
std::vector<float> renderMixed(int threads, bool* usedParallel = nullptr, int* mostThreads = nullptr)
{
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    eng.setEngineThreads(threads);
    eng.setParallelThresholdMicros(0.0); // parallel for every block, however little work
    const auto song = mixedSong();
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto& g = *built.graph;
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind != model::TrackKind::Group)
            g.nodes[i].devices.insert(g.nodes[i].devices.begin(), std::make_unique<Noise>(17u * (uint32_t) i + 3u, 0.2f + 0.05f * (float) i));
    eng.installGraph(std::move(built.graph));
    eng.transport().setLoop(true, 0.0, 3.0);
    eng.transport().play();
    std::vector<float> l(256), r(256), mix;
    float* out[2] = {l.data(), r.data()};
    for (int b = 0; b < 300; ++b)
    {
        if (b == 150) (*eng.controls())[1].sends[0].store(0.0f); // a send switched off mid-render
        eng.renderBlock(out, 2, 256);
        mix.insert(mix.end(), l.begin(), l.end());
        mix.insert(mix.end(), r.begin(), r.end());
        const auto stats = eng.loadStats();
        if (usedParallel) *usedParallel = *usedParallel || stats.parallel;
        if (mostThreads) *mostThreads = std::max(*mostThreads, stats.threads);
    }
    return mix;
}
} // namespace

TEST_CASE("render schedule: children before groups, sidechain sources first, tracks before returns, Main last (ENG-14e)")
{
    const auto song = mixedSong();
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto& g = *built.graph;
    g.prepare(44100.0, 256);
    const auto& s = g.schedule;
    REQUIRE(s.jobs.size() == g.trackOrder.size() + g.returnNodes.size());
    CHECK(s.parallelSafe);
    // Serial order: trackOrder, then the returns.
    for (size_t j = 0; j < g.trackOrder.size(); ++j) CHECK(s.jobs[j].node == g.trackOrder[j]);
    CHECK(s.jobs[g.trackOrder.size()].isReturn);
    CHECK(s.jobs[g.trackOrder.size()].returnIndex == 0);
    CHECK(s.jobs.back().returnIndex == 1);

    const int g1 = 0, kick = 1, snare = 2, g2 = 3, hat = 4, perc = 5, bass = (int) song.tracks.size() - 1;
    CHECK(precedes(s, jobOf(s, kick), jobOf(s, g1)));
    CHECK(precedes(s, jobOf(s, hat), jobOf(s, g2)));
    CHECK(precedes(s, jobOf(s, g2), jobOf(s, g1)));
    CHECK(precedes(s, jobOf(s, kick), jobOf(s, bass))); // the compressor on "bass" listens to "kick"
    CHECK(! precedes(s, jobOf(s, snare), jobOf(s, bass)));
    CHECK(! precedes(s, jobOf(s, hat), jobOf(s, perc)));
    for (int t : g.trackOrder)
        for (int r : g.returnNodes) CHECK(precedes(s, jobOf(s, t), jobOf(s, r)));
    CHECK(! precedes(s, jobOf(s, g.returnNodes[0]), jobOf(s, g.returnNodes[1])));
    // Gathers in serial order: G1 sums kick, snare, G2 (as trackOrder has them); Main sums the top-level tracks.
    const auto& g1Sources = s.jobs[(size_t) jobOf(s, g1)].sources;
    CHECK(g1Sources.size() == 3);
    for (size_t i = 1; i < g1Sources.size(); ++i)
        CHECK(std::find(g.trackOrder.begin(), g.trackOrder.end(), g1Sources[i - 1])
              < std::find(g.trackOrder.begin(), g.trackOrder.end(), g1Sources[i]));
    CHECK(s.mainSources.size() == 8); // G1, top0..top5, bass
    CHECK(std::find(s.mainSources.begin(), s.mainSources.end(), kick) == s.mainSources.end());
    CHECK(s.jobs[(size_t) jobOf(s, g.returnNodes[0])].sources.size() == g.trackOrder.size());
    CHECK(s.width >= 6); // the leaves can all run at once
    for (const auto& job : s.jobs) // every edge points forward in the serial order
        for (const int next : job.successors) CHECK(next > jobOf(s, job.node));
}

TEST_CASE("engine workers: every job runs once, after all its predecessors, on several threads (ENG-14e)")
{
    const auto song = mixedSong();
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    auto& g = *built.graph;
    g.prepare(44100.0, 64);
    auto& s = g.schedule;
    engine::EngineWorkers workers;
    workers.start(4, 44100.0, 64);
    REQUIRE(workers.count() == 4);

    struct Probe
    {
        engine::RenderSchedule* schedule;
        std::array<std::atomic<int>, 64> runs{};
        std::array<std::atomic<int>, 64> done{};
        std::atomic<int> violations{0};
    } probe;
    probe.schedule = &s;
    auto fn = [](void* context, int job) {
        auto& p = *static_cast<Probe*>(context);
        for (size_t other = 0; other < p.schedule->jobs.size(); ++other)
            for (const int next : p.schedule->jobs[other].successors)
                if (next == job && p.done[other].load(std::memory_order_acquire) == 0) p.violations.fetch_add(1);
        p.runs[(size_t) job].fetch_add(1);
        volatile double spin = 0; // a little work so jobs overlap
        for (int i = 0; i < 2000; ++i) spin = spin + i;
        p.done[(size_t) job].store(1, std::memory_order_release);
    };
    int most = 1;
    for (int round = 0; round < 500; ++round)
    {
        for (auto& d : probe.done) d.store(0);
        most = std::max(most, workers.run(s, fn, &probe, 4));
    }
    CHECK(probe.violations.load() == 0);
    for (size_t j = 0; j < s.jobs.size(); ++j) CHECK(probe.runs[j].load() == 500);
    CHECK(most > 1);
    workers.stop();
    CHECK(workers.count() == 0);
    CHECK(workers.run(s, fn, &probe, 4) == 1); // stopped: the caller renders alone
}

TEST_CASE("engine threads: a parallel render is bit-identical to the serial render (ENG-14e)")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto serial = renderMixed(1);
    REQUIRE(serial.size() == 300u * 512u);
    float peak = 0;
    for (const float v : serial) peak = std::max(peak, std::abs(v));
    CHECK(peak > 0.05f);
    for (const int threads : {2, 4, 8})
    {
        bool parallel = false;
        int most = 1;
        const auto mix = renderMixed(threads, &parallel, &most);
        CHECK(parallel);
        CHECK(most > 1);
        REQUIRE(mix.size() == serial.size());
        size_t firstDifference = mix.size();
        for (size_t i = 0; i < mix.size() && firstDifference == mix.size(); ++i)
            if (std::memcmp(&mix[i], &serial[i], sizeof(float)) != 0) firstDifference = i;
        CHECK(firstDifference == mix.size());
    }
}

TEST_CASE("engine load: per-track cost, blocks over budget and reset (ENG-14e / UI-82)")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    eng.setEngineThreads(1);
    const auto song = mixedSong();
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    // A device that takes 3 ms: every 64-sample block (1.45 ms at 44.1 kHz) overruns.
    class Slow final : public devices::DeviceProcessor
    {
    public:
        std::string name() const override { return "slow"; }
        void prepare(double, int) override {}
        void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override { juce::Thread::sleep(3); }
    };
    built.graph->nodes[6].devices.push_back(std::make_unique<Slow>());
    eng.installGraph(std::move(built.graph));
    std::vector<float> l(64), r(64);
    float* out[2] = {l.data(), r.data()};
    for (int b = 0; b < 10; ++b) eng.renderBlock(out, 2, 64);
    auto stats = eng.loadStats();
    CHECK(stats.overruns == 10);
    CHECK(stats.blocks == 10);
    CHECK(stats.peak > 1.0f);
    CHECK(stats.threads == 1);
    CHECK(! stats.parallel);
    CHECK_NEAR(stats.bufferMicros, 1451.2f, 1.0f);
    const auto& controls = *eng.controls();
    CHECK(controls[6].cpuMicros.load() > 50.0f); // smoothed (~0.3 s) towards ~3000 us
    CHECK(controls[6].cpuMicros.load() > 10.0f * controls[7].cpuMicros.load());
    eng.resetLoadStats();
    eng.latestGraph()->nodes[6].devices.pop_back(); // fast again (headless: no audio thread owns the graph)
    for (int b = 0; b < 3; ++b) eng.renderBlock(out, 2, 64);
    stats = eng.loadStats();
    CHECK(stats.overruns == 0);
    CHECK(stats.blocks == 3);
    CHECK(stats.peak < 1.0f);
}
