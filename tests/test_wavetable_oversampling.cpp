#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"

#include <algorithm>
#include <array>
#include <numbers>
#include <span>

using namespace sonora;

namespace
{
using W = devices::WavetableSynth;
using E = devices::WavetableEffects;
constexpr double rate = 48000;
constexpr std::array<int, 1> regular{512}, whole{131072};
constexpr std::array<int, 7> uneven{1, 31, 97, 511, 9, 2017, 43};

void isolated(W& synth, int factor = 2, int capacity = 512)
{
    if (factor != 0) REQUIRE(synth.setVoiceRateFactor(factor));
    synth.prepare(rate, capacity);
    REQUIRE(synth.voiceRatePreparationError().empty());
    synth.setParameter(W::Volume, -12);
    synth.setParameter(W::SubOn, 0);
    synth.setParameter(W::NoiseOn, 0);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        synth.setParameter(W::oscParam(oscillator, W::On), oscillator == 0 ? 1.0f : 0.0f);
        synth.setParameter(W::oscParam(oscillator, W::Table), W::Basic);
        synth.setParameter(W::oscParam(oscillator, W::Position), 0);
        synth.setParameter(W::oscParam(oscillator, W::Level), 0.8f);
        synth.setParameter(W::oscParam(oscillator, W::Random), 0);
        synth.setParameter(W::oscParam(oscillator, W::Phase), 0.25f);
        synth.setParameter(W::oscParam(oscillator, W::Unison), 1);
    }
    for (int filter = 0; filter < W::kFilters; ++filter)
        synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    for (auto parameter : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release})
        synth.setParameter(W::envParam(0, parameter), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
}

void configure(W& synth, const E::Configuration& configuration)
{
    std::string error;
    REQUIRE(synth.setEffectsConfiguration(configuration, error));
    REQUIRE(error.empty());
}

E::Module effect(E::ModuleId id, E::Type type)
{
    E::Module module;
    module.id = id;
    module.type = type;
    for (const auto& descriptor : E::parameterDescriptors(type))
        module.values.push_back(descriptor.initial);
    return module;
}

void set(E::Module& module, std::string_view name, float value)
{
    const int index = E::parameterIndex(module.type, name);
    REQUIRE(index >= 0);
    module.values[static_cast<size_t>(index)] = value;
}

juce::MidiBuffer note(int pitch = 60, int start = 17, int end = -1)
{
    juce::MidiBuffer midi;
    midi.ensureSize(256);
    midi.addEvent(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(100)), start);
    if (end >= 0) midi.addEvent(juce::MidiMessage::noteOff(1, pitch), end);
    return midi;
}

juce::AudioBuffer<float> render(W& synth, const juce::MidiBuffer& input, int samples = 8192,
                              std::span<const int> blocks = regular, float foreign = 0, double tempo = 120,
                              double initialBeat = 0)
{
    REQUIRE(! blocks.empty());
    juce::AudioBuffer<float> result(2, samples);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < samples; ++sample)
            result.setSample(channel, sample, foreign * (channel == 0 ? 1.0f : -0.7f));
    juce::MidiBuffer midi;
    midi.ensureSize(65536);
    size_t blockIndex = 0;
    for (int start = 0; start < samples;)
    {
        const int count = std::min(blocks[blockIndex++ % blocks.size()], samples - start);
        REQUIRE(count > 0);
        juce::AudioBuffer<float> block(result.getArrayOfWritePointers(), 2, start, count);
        midi.clear();
        midi.addEvents(input, start, count, -start);
        const int eventCount = midi.getNumEvents();
        synth.setHostInfo({tempo, initialBeat + static_cast<double>(start) * tempo / (60 * rate), true, rate});
        synth.process(block, midi);
        CHECK(midi.getNumEvents() == eventCount);
        start += count;
    }
    return result;
}

double maxDifference(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b,
                     int start = 0, int end = -1)
{
    REQUIRE(a.getNumSamples() == b.getNumSamples());
    if (end < 0) end = a.getNumSamples();
    double result = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = start; sample < end; ++sample)
            result = std::max(result, std::abs(static_cast<double>(a.getSample(channel, sample)) - b.getSample(channel, sample)));
    return result;
}

double peak(const juce::AudioBuffer<float>& audio, int start = 0, int end = -1)
{
    if (end < 0) end = audio.getNumSamples();
    double result = 0;
    bool finite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = start; sample < end; ++sample)
        {
            const auto value = static_cast<double>(audio.getSample(channel, sample));
            finite = finite && std::isfinite(value);
            result = std::max(result, std::abs(value));
        }
    CHECK(finite);
    return result;
}

int firstAudio(const juce::AudioBuffer<float>& audio, double threshold = 1e-6)
{
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        if (std::abs(audio.getSample(0, sample)) > threshold || std::abs(audio.getSample(1, sample)) > threshold)
            return sample;
    return -1;
}

void route(W& synth, W::Source source, W::Destination destination, float amount)
{
    synth.setParameter(W::modParam(0, W::ModSource), static_cast<float>(source));
    synth.setParameter(W::modParam(0, W::ModDestination), static_cast<float>(destination));
    synth.setParameter(W::modParam(0, W::ModAmount), amount);
}

void complex(W& synth)
{
    synth.setParameter(W::oscParam(0, W::Unison), 3);
    synth.setParameter(W::oscParam(0, W::Detune), 23);
    synth.setParameter(W::oscParam(0, W::Width), 0.8f);
    synth.setParameter(W::oscParam(0, W::Position), 0.6f);
    synth.setParameter(W::oscParam(1, W::On), 1);
    synth.setParameter(W::oscParam(1, W::Octave), -1);
    synth.setParameter(W::oscParam(1, W::Level), 0.2f);
    synth.setParameter(W::NoiseOn, 1);
    synth.setParameter(W::NoiseLevel, 0.08f);
    for (int filter = 0; filter < W::kFilters; ++filter)
    {
        synth.setParameter(W::filterParam(filter, W::FilterOn), 1);
        synth.setParameter(W::filterParam(filter, W::Cutoff), filter == 0 ? 1800.0f : 3700.0f);
        synth.setParameter(W::filterParam(filter, W::Resonance), 0.3f);
    }
    synth.setParameter(W::envParam(0, W::Attack), 0.004f);
    synth.setParameter(W::envParam(0, W::Release), 0.03f);
    synth.setParameter(W::lfoParam(0, W::Mode), W::Retrigger);
    synth.setParameter(W::lfoParam(0, W::Rate), 3.7f);
    route(synth, W::Lfo1, W::Osc1Position, 0.2f);
    synth.setParameter(W::modParam(1, W::ModSource), W::ModWheel);
    synth.setParameter(W::modParam(1, W::ModDestination), W::Filter1Cutoff);
    synth.setParameter(W::modParam(1, W::ModAmount), 0.3f);
}

juce::MidiBuffer performance()
{
    auto input = note(60, 17, 6200);
    input.addEvent(juce::MidiMessage::noteOn(2, 67, static_cast<juce::uint8>(71)), 513);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 1300);
    input.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90)), 1403);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 85), 2037);
    input.addEvent(juce::MidiMessage::pitchWheel(2, 12100), 2875);
    input.addEvent(juce::MidiMessage::channelPressureChange(2, 91), 3127);
    input.addEvent(juce::MidiMessage::noteOff(1, 60), 4033);
    input.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 4701);
    input.addEvent(juce::MidiMessage::noteOff(2, 67), 7003);
    const std::array<juce::uint8, 17> payload{};
    input.addEvent(juce::MidiMessage::createSysExMessage(payload.data(), static_cast<int>(payload.size())), 900);
    return input;
}

void constantTable(W& synth)
{
    const std::array<float, 2048> pcm = [] { std::array<float, 2048> result{}; result.fill(0.5f); return result; }();
    const auto table = devices::WavetableData::fromSamples(pcm, 1, 2048, rate, "Authored constant");
    REQUIRE(table);
    REQUIRE(synth.setCustomTable(0, table.data));
    synth.setParameter(W::oscParam(0, W::Table), W::Custom);
}

int peakPosition(const juce::AudioBuffer<float>& audio, int start, int end)
{
    int result = start;
    for (int sample = start + 1; sample < end; ++sample)
        if (audio.getSample(0, sample) > audio.getSample(0, result)) result = sample;
    return result;
}

double frequency(const juce::AudioBuffer<float>& audio, int start, int end)
{
    double first = 0, last = 0;
    int crossings = 0;
    for (int sample = start + 1; sample < end; ++sample)
    {
        const double previous = audio.getSample(0, sample - 1), current = audio.getSample(0, sample);
        if (previous <= 0 && current > 0)
        {
            const double position = sample - 1 - previous / (current - previous);
            if (crossings == 0) first = position;
            last = position;
            ++crossings;
        }
    }
    REQUIRE(crossings >= 3);
    return static_cast<double>(crossings - 1) * rate / (last - first);
}

std::shared_ptr<const devices::OscillatorMedia> pulseMedia()
{
    std::array<float, 128> pcm{};
    std::fill(pcm.begin() + 32, pcm.begin() + 96, 0.5f);
    auto sample = devices::OscillatorSample::fromSamples(pcm, 1, 24000, "Authored pulse");
    REQUIRE(sample);
    devices::OscillatorZone zone;
    zone.sample = sample.data;
    zone.rootKey = 60;
    zone.releaseSeconds = 0;
    const auto media = devices::OscillatorMedia::fromZones({zone}, "Authored pulse bank");
    REQUIRE(media);
    return media.data;
}

int pulseWidth(const juce::AudioBuffer<float>& audio)
{
    const double threshold = peak(audio) * 0.5;
    REQUIRE(threshold > 0.001);
    int first = -1, last = -1;
    for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        if (audio.getSample(0, sample) > threshold)
        {
            if (first < 0) first = sample;
            last = sample;
        }
    return last - first + 1;
}
}

TEST_CASE("wavetable oversampling: prepared rate selection is transactional and default audio remains bit identical")
{
    W implicit, explicitRate;
    isolated(implicit, 0);
    isolated(explicitRate, 1);
    CHECK(implicit.requestedVoiceRateFactor() == 1);
    CHECK(implicit.voiceRateFactor() == 1);
    CHECK(implicit.latencySamples() == 0);
    CHECK(! implicit.setVoiceRateFactor(0));
    CHECK(! implicit.setVoiceRateFactor(4));
    CHECK(implicit.requestedVoiceRateFactor() == 1);
    complex(implicit);
    complex(explicitRate);
    const auto input = performance();
    CHECK_NEAR(maxDifference(render(implicit, input), render(explicitRate, input)), 0, 0);
    REQUIRE(implicit.setVoiceRateFactor(2));
    CHECK(implicit.requestedVoiceRateFactor() == 2);
    CHECK(implicit.voiceRateFactor() == 1);
    CHECK(implicit.latencySamples() == 0);
    CHECK(! implicit.setVoiceRateFactor(-1));
    CHECK(implicit.requestedVoiceRateFactor() == 2);
    implicit.prepare(rate, 37);
    CHECK(implicit.voiceRateFactor() == 2);
    CHECK(implicit.latencySamples() == 2);
    CHECK(implicit.voiceRatePreparationError().empty());
    const auto audio = render(implicit, note(), 2048, whole);
    CHECK(peak(audio) > 0.01);
}

TEST_CASE("wavetable oversampling: reported queue latency combines with effects and changes only on preparation")
{
    E::Configuration graph;
    graph.buses[0].push_back(effect(41, E::Type::FrequencyShift));
    W synth;
    configure(synth, graph);
    isolated(synth, 1);
    REQUIRE(synth.effects() != nullptr);
    const int effectLatency = synth.effects()->latencySamples();
    REQUIRE(effectLatency > 0);
    CHECK(synth.latencySamples() == effectLatency);
    REQUIRE(synth.setVoiceRateFactor(2));
    CHECK(synth.latencySamples() == effectLatency);
    CHECK(! synth.setVoiceRateFactor(3));
    CHECK(synth.latencySamples() == effectLatency);
    synth.prepare(rate, 61);
    CHECK(synth.latencySamples() == effectLatency + 2);
    REQUIRE(synth.setVoiceRateFactor(1));
    CHECK(synth.latencySamples() == effectLatency + 2);
    synth.prepare(rate, 61);
    CHECK(synth.latencySamples() == effectLatency);
}

TEST_CASE("wavetable oversampling: polyphony controllers filters and unison are invariant across uneven and oversized blocks")
{
    W a, b;
    isolated(a, 2, 4096);
    isolated(b, 2, 47);
    complex(a);
    complex(b);
    const auto input = performance();
    const auto continuous = render(a, input, 16384, whole);
    const auto partitioned = render(b, input, 16384, uneven);
    CHECK(peak(continuous) > 0.01);
    CHECK(peak(partitioned) < 4);
    CHECK(maxDifference(continuous, partitioned) < 2e-5);
    CHECK(a.activeVoiceCount() == 0);
    CHECK(b.activeVoiceCount() == 0);
}

TEST_CASE("wavetable oversampling: note offsets shift actual audio exactly and preserve caller buffers and SysEx")
{
    W a, b;
    isolated(a, 2, 53);
    isolated(b, 2, 53);
    const auto first = render(a, note(60, 17), 4096, whole);
    const auto shifted = render(b, note(60, 30), 4096, uneven);
    CHECK(firstAudio(first) >= 19);
    CHECK(firstAudio(first) < 25);
    CHECK(firstAudio(shifted) == firstAudio(first) + 13);
    double error = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 30; sample < 4096; ++sample)
            error = std::max(error, std::abs(static_cast<double>(shifted.getSample(channel, sample)) - first.getSample(channel, sample - 13)));
    CHECK(error < 1e-6);

    W owned;
    isolated(owned);
    juce::AudioBuffer<float> audio(2, 512);
    audio.clear();
    auto input = note(60, 37);
    const std::array<juce::uint8, 32> payload{};
    input.addEvent(juce::MidiMessage::createSysExMessage(payload.data(), static_cast<int>(payload.size())), 101);
    input.ensureSize(65536);
    const auto* eventPointer = (*input.begin()).data;
    const auto* leftPointer = audio.getReadPointer(0);
    owned.process(audio, input);
    CHECK(audio.getReadPointer(0) == leftPointer);
    CHECK((*input.begin()).data == eventPointer);
    CHECK(input.getNumEvents() == 2);
    auto last = input.begin();
    ++last;
    CHECK((*last).samplePosition == 101);
    CHECK((*last).numBytes == static_cast<int>(payload.size()) + 2);
    CHECK(firstAudio(audio) >= 39);
}

TEST_CASE("wavetable oversampling: all sound off bypass and reset clear converter state without a stuck tail")
{
    W synth, fresh;
    isolated(synth);
    isolated(fresh);
    auto input = note();
    input.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 99);
    const auto killed = render(synth, input, 512, uneven);
    CHECK(peak(killed, 20, 99) > 0.01);
    CHECK_NEAR(peak(killed, 99), 0, 0);
    CHECK(synth.activeVoiceCount() == 0);
    render(synth, note(), 512);
    synth.setBypassed(true);
    const auto bypassed = render(synth, note(), 512, uneven, 0.23f);
    double error = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 512; ++sample)
            error = std::max(error, std::abs(static_cast<double>(bypassed.getSample(channel, sample)) - 0.23f * (channel == 0 ? 1.0f : -0.7f)));
    CHECK_NEAR(error, 0, 0);
    synth.setBypassed(false);
    CHECK_NEAR(peak(render(synth, {}, 512)), 0, 0);
    synth.reset();
    CHECK_NEAR(maxDifference(render(synth, note()), render(fresh, note())), 0, 0);
}

TEST_CASE("wavetable oversampling: main auxiliary and direct stereo lanes have equal timing and independent routing")
{
    W reference;
    configure(reference, {});
    isolated(reference, 2, 29);
    reference.setParameter(W::SourceRoutingOn, 1);
    reference.setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToMain);
    reference.setParameter(W::oscParam(0, W::OscPan), 0.4f);
    const auto expected = render(reference, note(), 4096, uneven);
    CHECK(peak(expected) > 0.01);
    for (int path = 0; path < 3; ++path)
    {
        W routed;
        configure(routed, {});
        isolated(routed, 2, 61);
        routed.setParameter(W::SourceRoutingOn, 1);
        routed.setParameter(W::oscParam(0, W::OscPan), 0.4f);
        routed.setParameter(W::sourceRoutingParam(0, W::SourceDestination), path == 2 ? W::RouteToDirect : W::RouteToNone);
        if (path < 2)
            routed.setParameter(W::sourceRoutingParam(0, path == 0 ? W::SourceBus1 : W::SourceBus2), 1);
        const auto actual = render(routed, note(), 4096, whole);
        CHECK(firstAudio(actual) == firstAudio(expected));
        CHECK(maxDifference(actual, expected) < 2e-6);
        CHECK(std::abs(actual.getSample(0, 300) - actual.getSample(1, 300)) > 0.001f);
    }
}

TEST_CASE("wavetable oversampling: nonlinear effects act on own lanes and master gain remains after effects")
{
    E::Configuration graph;
    auto distortion = effect(19, E::Type::Distortion);
    set(distortion, "Drive", 24);
    set(distortion, "Tone", 20000);
    set(distortion, "Bias", 0.4f);
    graph.buses[0].push_back(distortion);
    W a, b;
    configure(a, graph);
    configure(b, graph);
    isolated(a, 2, 31);
    isolated(b, 2, 127);
    const auto input = note();
    const auto own = render(a, input, 4096, whole);
    const auto foreign = render(b, input, 4096, uneven, 0.3f);
    double error = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 4096; ++sample)
            error = std::max(error, std::abs(static_cast<double>(foreign.getSample(channel, sample)) - own.getSample(channel, sample) - 0.3f * (channel == 0 ? 1.0f : -0.7f)));
    CHECK(error < 2e-6);
    a.reset();
    b.reset();
    a.setParameter(W::Volume, 0);
    const auto loud = render(a, input, 4096, whole);
    const auto quiet = render(b, input, 4096, uneven);
    const double gain = std::pow(10.0, -12.0 / 20);
    error = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 4096; ++sample)
            error = std::max(error, std::abs(quiet.getSample(channel, sample) - loud.getSample(channel, sample) * gain));
    CHECK(error < 2e-6);
    b.reset();
    const auto untouched = render(b, {}, 2048, uneven, 0.23f);
    CHECK_NEAR(untouched.getSample(0, 1000), 0.23f, 0);
    CHECK_NEAR(untouched.getSample(1, 1000), -0.23f * 0.7f, 0);
}

TEST_CASE("wavetable oversampling: effects delay time stays at host rate after voice conversion")
{
    E::Configuration graph;
    auto delay = effect(7, E::Type::Delay);
    set(delay, "Mix", 1);
    set(delay, "Time", 0.04f);
    set(delay, "RightTime", 0.04f);
    set(delay, "Feedback", 0);
    set(delay, "LowCut", 20);
    set(delay, "HighCut", 20000);
    graph.buses[0].push_back(delay);
    W dry, wet;
    configure(dry, {});
    configure(wet, graph);
    isolated(dry, 2, 37);
    isolated(wet, 2, 37);
    const auto input = note(60, 17, 33);
    const auto original = render(dry, input, 8192, uneven);
    const auto delayed = render(wet, input, 8192, whole);
    CHECK(peak(delayed) > 0.001);
    CHECK_NEAR(firstAudio(delayed) - firstAudio(original), 0.04 * rate, 2);
    CHECK_NEAR(peak(delayed, 0, 1900), 0, 1e-6);
}

TEST_CASE("wavetable oversampling: trigger and host synced LFO periods remain in host seconds")
{
    for (bool synced : {false, true})
    {
        std::array<int, 2> peakDistance{};
        for (int factor = 1; factor <= 2; ++factor)
        {
            W synth;
            constantTable(synth);
            isolated(synth, factor, 53);
            synth.setParameter(W::oscParam(0, W::Table), W::Custom);
            REQUIRE(synth.setLfoCurve(0, dsp::CurveGraph::preset(2)));
            synth.setParameter(W::lfoParam(0, W::Mode), synced ? W::Global : W::Retrigger);
            synth.setParameter(W::lfoParam(0, W::Rate), 2.5f);
            synth.setParameter(W::lfoParam(0, W::SyncOn), synced ? 1.0f : 0.0f);
            int quarter = 0;
            for (int division = 0; division < W::kLfoDivisions; ++division)
                if (std::abs(W::divisionBeats(division) - 1) < 1e-12) quarter = division;
            synth.setParameter(W::lfoParam(0, W::Division), static_cast<float>(quarter));
            route(synth, W::Lfo1, W::Amplitude, 0.3f);
            const int period = synced ? 24000 : 19200;
            const auto audio = render(synth, note(), period * 3, uneven);
            const int first = peakPosition(audio, period / 2 - 1000, period / 2 + 1000);
            const int second = peakPosition(audio, period * 3 / 2 - 1000, period * 3 / 2 + 1000);
            peakDistance[static_cast<size_t>(factor - 1)] = second - first;
            CHECK_NEAR(second - first, period, 24);
            CHECK(peak(audio) > 0.01);
        }
        CHECK_NEAR(peakDistance[0], peakDistance[1], 24);
    }
}

TEST_CASE("wavetable oversampling: imported Free LFO seeds and progresses at the host block clock")
{
    W normal, oversampled;
    for (auto* synth : {&normal, &oversampled})
    {
        constantTable(*synth);
        synth->setImportedLfoProfile(true);
        isolated(*synth, synth == &normal ? 1 : 2, 73);
        synth->setParameter(W::oscParam(0, W::Table), W::Custom);
        REQUIRE(synth->setLfoCurve(0, dsp::CurveGraph::preset(2)));
        synth->setParameter(W::lfoParam(0, W::Mode), W::Global);
        synth->setParameter(W::lfoParam(0, W::Rate), 2.5f);
        synth->setParameter(W::lfoParam(0, W::SyncOn), 0);
        synth->setParameter(W::lfoAnchorParam(0), 0);
        route(*synth, W::Lfo1, W::Amplitude, 0.3f);
    }
    // The imported Free seed intentionally depends on actual host block length. Both runs use
    // the same host blocks; the internal converter capacity differs from that public block clock.
    const auto a = render(normal, note(), 64000, regular);
    const auto b = render(oversampled, note(), 64000, regular);
    const int peakA = peakPosition(a, 26000, 30000), peakB = peakPosition(b, 26000, 30000);
    CHECK_NEAR(peakA, peakB, 32);
    CHECK_NEAR(peakPosition(a, 45200, 49200) - peakA, 19200, 24);
    CHECK_NEAR(peakPosition(b, 45200, 49200) - peakB, 19200, 24);
    CHECK(maxDifference(a, b, 1000) < 0.004);
}

TEST_CASE("wavetable oversampling: mono glide time and fallback remain in host seconds")
{
    W synth;
    isolated(synth, 2, 41);
    synth.setParameter(W::voiceModeParam(W::MonoOn), 1);
    synth.setParameter(W::voiceModeParam(W::LegatoOn), 1);
    synth.setParameter(W::voiceModeParam(W::PortaTime), 0.1f);
    synth.setParameter(W::voiceModeParam(W::PortaCurve), 0);
    auto input = note(60, 17);
    input.addEvent(juce::MidiMessage::noteOn(1, 72, static_cast<juce::uint8>(100)), 2400);
    input.addEvent(juce::MidiMessage::noteOff(1, 72), 10000);
    const auto audio = render(synth, input, 18000, uneven);
    CHECK(synth.activeVoiceCount() == 1);
    const double middle = frequency(audio, 4290, 5310);
    CHECK(std::abs(12 * std::log2(middle / 369.9944227)) < 0.5);
    CHECK_NEAR(frequency(audio, 7800, 9500), 523.2511306, 0.2);
    CHECK_NEAR(frequency(audio, 16000, 17800), 261.6255653, 0.2);
}

TEST_CASE("wavetable oversampling: sample playback preserves source rate and octave timing across block sizes")
{
    const auto media = pulseMedia();
    for (int pitch : {48, 60, 72})
    {
        W wholeSynth, splitSynth;
        for (auto* synth : {&wholeSynth, &splitSynth})
        {
            REQUIRE(synth->setOscillatorMedia(0, media));
            isolated(*synth, 2, synth == &wholeSynth ? 4096 : 19);
            synth->setParameter(W::mediaParam(0, W::MediaMode), W::SampleMode);
            synth->setParameter(W::mediaParam(0, W::MediaLoopMode), 1);
            synth->setParameter(W::mediaParam(0, W::MediaOneShot), 1);
            synth->setParameter(W::mediaParam(0, W::MediaScanRate), 1);
        }
        const auto a = render(wholeSynth, note(pitch), 4096, whole);
        const auto b = render(splitSynth, note(pitch), 4096, uneven);
        CHECK(maxDifference(a, b) < 1e-6);
        CHECK_NEAR(pulseWidth(a), pitch == 48 ? 256 : pitch == 60 ? 128 : 64, 8);
        CHECK_NEAR(peak(a, 1200), 0, 1e-6);
    }
}
