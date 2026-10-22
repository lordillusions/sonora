// PLG-10: hosted plugins see the host transport through an AudioPlayHead (Ozone's Master Assistant, tempo-synced effects).
#include "TestFramework.h"
#include "plugins/PluginDevice.h"

#include <juce_audio_processors/juce_audio_processors.h>

using namespace sonora;

namespace
{
// A minimal effect that records what its play head reports inside processBlock, like a real plugin would.
class TransportProbe final : public juce::AudioPluginInstance
{
public:
    TransportProbe() : AudioPluginInstance(BusesProperties().withInput("In", juce::AudioChannelSet::stereo()).withOutput("Out", juce::AudioChannelSet::stereo())) {}
    bool sawPlayHead = false, preparedWithPlayHead = false;
    juce::AudioPlayHead::PositionInfo last;
    int blocks = 0, playingBlocks = 0;
    double maxPlayingPpq = -1;

    void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = "TransportProbe"; d.isInstrument = false; }
    const juce::String getName() const override { return "TransportProbe"; }
    void prepareToPlay(double, int) override { preparedWithPlayHead = getPlayHead() != nullptr; }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
        ++blocks;
        if (auto* head = getPlayHead())
            if (auto position = head->getPosition())
            {
                sawPlayHead = true; last = *position;
                if (position->getIsPlaying()) { ++playingBlocks; maxPlayingPpq = std::max(maxPlayingPpq, position->getPpqPosition().orFallback(-1.0)); }
            }
    }
    double getTailLengthSeconds() const override { return 0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};
} // namespace

TEST_CASE("plugin play head: a hosted plugin sees tempo, position, bar, loop and play state")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto owned = std::make_unique<TransportProbe>();
    auto* probe = owned.get();
    plugins::PluginDevice device(std::move(owned));
    device.prepare(48000, 512);
    CHECK(probe->preparedWithPlayHead);

    devices::HostInfo host;
    host.tempo = 96; host.beats = 13.5; host.playing = true; host.sampleRate = 48000;
    host.timeSigNumerator = 3; host.timeSigDenominator = 4;
    host.looping = true; host.loopStart = 8; host.loopEnd = 16;
    juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer midi;
    device.setHostInfo(host);
    device.process(audio, midi);
    REQUIRE(probe->sawPlayHead);
    const auto& p = probe->last;
    CHECK(p.getIsPlaying());
    CHECK_NEAR(*p.getBpm(), 96, 1e-9);
    CHECK_NEAR(*p.getPpqPosition(), 13.5, 1e-9);
    CHECK_NEAR(*p.getPpqPositionOfLastBarStart(), 12, 1e-9); // 3/4: bars every 3 beats
    CHECK(*p.getBarCount() == 4);
    CHECK(p.getTimeSignature()->numerator == 3); CHECK(p.getTimeSignature()->denominator == 4);
    CHECK_NEAR(*p.getTimeInSeconds(), 13.5 * 60 / 96, 1e-9);
    CHECK(*p.getTimeInSamples() == (juce::int64) std::llround(13.5 * 60 / 96 * 48000));
    CHECK(p.getIsLooping());
    CHECK_NEAR(p.getLoopPoints()->ppqStart, 8, 1e-9); CHECK_NEAR(p.getLoopPoints()->ppqEnd, 16, 1e-9);

    host.playing = false; host.looping = false;
    device.setHostInfo(host);
    device.process(audio, midi);
    CHECK(! probe->last.getIsPlaying()); CHECK(! probe->last.getIsLooping());
    CHECK(probe->blocks == 2);
}

#include "engine/AudioEngine.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

TEST_CASE("plugin play head: a plugin on the Main track hears the playing transport during a render")
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::TemporaryFile file(".als"); std::string error;
    auto dom = io::createNewSetDom(); REQUIRE(io::writeAlsDom(*dom, file.getFile(), error));
    auto loaded = io::loadAls(file.getFile()); REQUIRE(loaded);
    loaded.song->tempo = 100; loaded.song->timeSigNumerator = 4; loaded.song->timeSigDenominator = 4;
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi);
    engine.loadSong(*loaded.song);
    engine.transport().setTempo(100);
    auto* graph = engine.latestGraph(); REQUIRE(graph);
    auto owned = std::make_unique<TransportProbe>();
    auto* probe = owned.get();
    auto device = std::make_unique<plugins::PluginDevice>(std::move(owned));
    device->prepare(48000, 512);
    graph->nodes[(size_t) graph->mainNode].devices.push_back(std::move(device)); // as if inserted on Main (mastering chain)
    REQUIRE(engine.beginOffline());
    int rendered = 0;
    engine.renderOffline(8.0, {}, [&](double, double, int maxBlock) { return rendered < 48000 ? std::min(512, maxBlock) : 0; },
                         [&](const engine::AudioEngine::OfflineBlock& block) { rendered += block.numFrames; });
    engine.endOffline();
    REQUIRE(probe->sawPlayHead);
    // Every rendered block reports a playing transport (the seek block before and the stop after do not count).
    CHECK(probe->playingBlocks >= 93);
    CHECK(probe->maxPlayingPpq > 9.5); // advanced from beat 8 by ~1 s at 100 BPM
    CHECK(probe->maxPlayingPpq < 10.0);
    CHECK_NEAR(*probe->last.getBpm(), 100, 1e-6);
}
