#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <algorithm>
#include <array>
#include <complex>
#include <random>
#include <limits>
#include <set>

using namespace sonora;

namespace
{
using W = devices::WavetableSynth;
constexpr double sampleRate = 48000.0;

void isolated(W& synth, int blockSize = 512)
{
    synth.prepare(sampleRate, blockSize);
    synth.setParameter(W::Volume, -12.0f);
    synth.setParameter(W::SubOn, 0);
    synth.setParameter(W::NoiseOn, 0);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        synth.setParameter(W::oscParam(oscillator, W::On), oscillator == 0 ? 1.0f : 0.0f);
        synth.setParameter(W::oscParam(oscillator, W::Table), W::Basic);
        synth.setParameter(W::oscParam(oscillator, W::Position), 0.0f);
        synth.setParameter(W::oscParam(oscillator, W::Level), 0.8f);
        synth.setParameter(W::oscParam(oscillator, W::Random), 0.0f);
        synth.setParameter(W::oscParam(oscillator, W::Phase),
                           synth.parameters()[static_cast<size_t>(W::oscParam(oscillator, W::Phase))].maxValue * 0.25f);
        synth.setParameter(W::oscParam(oscillator, W::Unison), 1.0f);
    }
    for (int filter = 0; filter < W::kFilters; ++filter)
        synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    synth.setParameter(W::envParam(0, W::Delay), 0);
    synth.setParameter(W::envParam(0, W::Attack), 0);
    synth.setParameter(W::envParam(0, W::Hold), 0);
    synth.setParameter(W::envParam(0, W::Decay), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
    synth.setParameter(W::envParam(0, W::Release), 0);
}

juce::MidiBuffer note(int channel = 1, int pitch = 69, int start = 0, int end = -1)
{
    juce::MidiBuffer midi;
    midi.ensureSize(256);
    midi.addEvent(juce::MidiMessage::noteOn(channel, pitch, static_cast<juce::uint8>(110)), start);
    if (end >= 0)
        midi.addEvent(juce::MidiMessage::noteOff(channel, pitch), end);
    return midi;
}

juce::AudioBuffer<float> render(W& synth, const juce::MidiBuffer& events, int samples = 2048,
                              int blockSize = 512, double tempo = 120, double startBeat = 0)
{
    juce::AudioBuffer<float> result(2, samples);
    result.clear();
    juce::MidiBuffer midi;
    midi.ensureSize(65536);
    for (int start = 0; start < samples; start += blockSize)
    {
        const int count = std::min(blockSize, samples - start);
        juce::AudioBuffer<float> block(result.getArrayOfWritePointers(), 2, start, count);
        midi.clear();
        midi.addEvents(events, start, count, -start);
        synth.setHostInfo({tempo, startBeat + static_cast<double>(start) * tempo / (60 * sampleRate), true, sampleRate});
        synth.process(block, midi);
    }
    return result;
}

template <typename Configure>
juce::AudioBuffer<float> tone(Configure configure, int samples = 2048)
{
    W synth;
    isolated(synth);
    configure(synth);
    return render(synth, note(), samples);
}

void finiteAndBounded(const juce::AudioBuffer<float>& audio, double bound = 128)
{
    bool allFinite = true;
    double peak = 0;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            const auto value = static_cast<double>(audio.getSample(channel, sample));
            if (! std::isfinite(value))
                allFinite = false;
            else
                peak = std::max(peak, std::abs(value));
        }
    CHECK(allFinite);
    CHECK(peak <= bound);
}

double difference(const juce::AudioBuffer<float>& first, const juce::AudioBuffer<float>& second,
                  int start = 0, int count = -1)
{
    REQUIRE(first.getNumSamples() == second.getNumSamples());
    if (count < 0)
        count = first.getNumSamples() - start;
    double total = 0;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = start; sample < start + count; ++sample)
            total += std::abs(static_cast<double>(first.getSample(channel, sample)) - second.getSample(channel, sample));
    return total / static_cast<double>(2 * count);
}

void route(W& synth, int slot, W::Source source, W::Destination destination, float amount)
{
    synth.setParameter(W::modParam(slot, W::ModSource), static_cast<float>(source));
    synth.setParameter(W::modParam(slot, W::ModDestination), static_cast<float>(destination));
    synth.setParameter(W::modParam(slot, W::ModAmount), amount);
}
}

TEST_CASE("wavetable: scalar helpers agree with measured host and serialized plain values")
{
    constexpr std::array<float, 3> normalized{0.25f, 0.5f, 0.75f};
    constexpr std::array<float, 3> level{0.0625f, 0.25f, 0.5625f};
    constexpr std::array<float, 3> seconds{0.03125f, 1.0f, 7.59375f};
    constexpr std::array<float, 3> hertz{0.390625f, 6.25f, 31.640625f};
    constexpr std::array<float, 3> table{64.75f, 128.5f, 192.25f};
    for (size_t index = 0; index < normalized.size(); ++index)
    {
        CHECK_NEAR(W::observedLevel(normalized[index]), level[index], 1e-6);
        CHECK_NEAR(W::observedEnvelopeSeconds(normalized[index]), seconds[index], 1e-6);
        CHECK_NEAR(W::observedLfoHertz(normalized[index]), hertz[index], 1e-6);
        CHECK_NEAR(W::observedTableIndex(normalized[index]), table[index], 1e-6);
    }
    CHECK_NEAR(W::observedLevel(0), 0, 0);
    CHECK_NEAR(W::observedLevel(1), 1, 0);
    CHECK_NEAR(W::observedEnvelopeSeconds(1), 32, 0);
    CHECK_NEAR(W::observedLfoHertz(1), 100, 0);
    CHECK_NEAR(W::observedTableIndex(0), 1, 0);
    CHECK_NEAR(W::observedTableIndex(1), 256, 0);
}

TEST_CASE("wavetable: stable parameter identities, finite generated previews and invalid writes")
{
    W synth;
    isolated(synth);
    CHECK(synth.isInstrument());
    CHECK(! synth.isHostedPlugin());
    CHECK(synth.parameters().size() == static_cast<size_t>(W::ParameterCount));
    CHECK(W::LegacyParameterCount == 540);
    CHECK(synth.parameters()[539].id == "Macro.7");
    CHECK(synth.parameters()[W::mediaParam(0, W::MediaMode)].id == "Media.0/Mode");
    std::set<std::string> identities;
    for (const auto& parameter : synth.parameters())
    {
        CHECK(! parameter.id.empty());
        CHECK(identities.insert(parameter.id).second);
        CHECK(parameter.minValue <= parameter.defaultValue);
        CHECK(parameter.defaultValue <= parameter.maxValue);
    }
    const auto level = synth.getParameter(W::oscParam(0, W::Level));
    synth.setParameter(W::oscParam(0, W::Level), std::numeric_limits<float>::quiet_NaN());
    CHECK_NEAR(synth.getParameter(W::oscParam(0, W::Level)), level, 0);
    synth.setParameter(W::Volume, std::numeric_limits<float>::infinity());
    CHECK(std::isfinite(synth.getParameter(W::Volume)));
    auto previous = synth.wavePreview(0);
    for (int family = 0; family < W::kTableFamilies; ++family)
    {
        synth.setParameter(W::oscParam(0, W::Table), static_cast<float>(family));
        synth.setParameter(W::oscParam(0, W::Position), 0.63f);
        const auto current = synth.wavePreview(0);
        double delta = 0;
        for (size_t sample = 0; sample < current.size(); ++sample)
        {
            CHECK(std::isfinite(current[sample]));
            CHECK(std::abs(current[sample]) <= 2);
            delta += std::abs(current[sample] - previous[sample]);
        }
        CHECK(delta > 0.1);
        previous = current;
    }
}

TEST_CASE("wavetable: filter display mapping respects sample rate and finite inputs")
{
    CHECK_NEAR(W::observedFilterHertz(0), 8.175799, 1e-5);
    CHECK_NEAR(W::observedFilterHertz(0.25f), 58.947540, 1e-4);
    CHECK_NEAR(W::observedFilterHertz(0.5f), 425.011985, 1e-3);
    CHECK_NEAR(W::observedFilterHertz(0.75f), 3064.337983, 1e-2);
    CHECK_NEAR(W::observedFilterHertz(1, 44100), 22050, 1e-3);
    CHECK_NEAR(W::observedFilterHertz(1, 32000), 16000, 1e-3);
    CHECK_NEAR(W::observedFilterHertz(1, 96000), 22093.888197, 0.05);
    for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()})
    {
        CHECK(std::isfinite(W::observedFilterHertz(value)));
        CHECK(W::observedFilterHertz(value) >= 0);
        CHECK(W::observedFilterHertz(value) <= 22050);
    }
    CHECK(std::isfinite(W::observedFilterHertz(0.5f, std::numeric_limits<double>::quiet_NaN())));
}

TEST_CASE("wavetable: notes and immediate releases occur at their exact samples and preserve owned buffers")
{
    W synth;
    isolated(synth, 512);
    juce::AudioBuffer<float> audio(2, 512);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 512; ++sample)
            audio.setSample(channel, sample, 0.125f);
    auto midi = note(3, 69, 37, 293);
    const std::array<juce::uint8, 32> payload{};
    midi.addEvent(juce::MidiMessage::createSysExMessage(payload.data(), static_cast<int>(payload.size())), 200);
    midi.ensureSize(65536);
    const auto* firstEvent = (*midi.begin()).data;
    const auto* firstChannel = audio.getReadPointer(0);
    const auto* secondChannel = audio.getReadPointer(1);
    synth.process(audio, midi);
    CHECK(audio.getReadPointer(0) == firstChannel);
    CHECK(audio.getReadPointer(1) == secondChannel);
    CHECK((*midi.begin()).data == firstEvent);
    CHECK(midi.getNumEvents() == 3);
    auto event = midi.begin();
    CHECK((*event).samplePosition == 37);
    CHECK((*event).getMessage().isNoteOn());
    ++event;
    CHECK((*event).samplePosition == 200);
    CHECK((*event).numBytes == static_cast<int>(payload.size()) + 2);
    ++event;
    CHECK((*event).samplePosition == 293);
    CHECK((*event).getMessage().isNoteOff());
    for (int sample = 0; sample < 37; ++sample)
        CHECK_NEAR(audio.getSample(0, sample), 0.125, 0);
    CHECK(std::abs(audio.getSample(0, 37) - 0.125f) > 0.001f);
    for (int sample = 293; sample < 512; ++sample)
        CHECK_NEAR(audio.getSample(0, sample), 0.125, 1e-7);
    CHECK(synth.activeVoiceCount() == 0);
    finiteAndBounded(audio);
}

TEST_CASE("wavetable: repeated pitches pair releases in arrival order and remain channel specific")
{
    W synth;
    isolated(synth);
    auto midi = note(1, 60);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(110)), 17);
    render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 2);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 19);
    auto audio = render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 1);
    CHECK(audio.getRMSLevel(0, 40, 180) > 0.01f);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 11);
    audio = render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 0);
    CHECK_NEAR(audio.getMagnitude(11, 245), 0, 1e-7);

    synth.reset();
    midi = note(1, 64);
    midi.addEvent(juce::MidiMessage::noteOn(2, 64, static_cast<juce::uint8>(110)), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 64), 73);
    render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 1);
    midi.clear();
    midi.addEvent(juce::MidiMessage::allSoundOff(2), 31);
    audio = render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 0);
    CHECK_NEAR(audio.getMagnitude(31, 225), 0, 1e-7);
}

TEST_CASE("wavetable: channel sustain and controller reset release held notes without releasing other channels")
{
    W synth;
    isolated(synth);
    auto midi = note(1, 60, 0, 51);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::noteOn(2, 67, static_cast<juce::uint8>(110)), 0);
    midi.addEvent(juce::MidiMessage::noteOff(2, 67), 51);
    auto audio = render(synth, midi, 512);
    CHECK(synth.activeVoiceCount() == 1);
    CHECK(audio.getRMSLevel(0, 100, 300) > 0.01f);
    midi.clear();
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 0);
    render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 1);
    midi.clear();
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 121, 0), 23);
    audio = render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 0);
    CHECK_NEAR(audio.getMagnitude(23, 233), 0, 1e-7);

    synth.reset();
    midi = note(1, 60, 0, 51);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    render(synth, midi, 256);
    midi.clear();
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 37);
    audio = render(synth, midi, 256);
    CHECK_NEAR(audio.getMagnitude(37, 219), 0, 1e-7);
}

TEST_CASE("wavetable: pitch bend affects its own channel and reset restores controller state")
{
    W baseline, otherChannel, ownChannel, cleared;
    for (auto* synth : {&baseline, &otherChannel, &ownChannel, &cleared})
        isolated(*synth);
    const auto a = render(baseline, note(2));
    auto midi = note(2);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    const auto b = render(otherChannel, midi);
    midi = note(2);
    midi.addEvent(juce::MidiMessage::pitchWheel(2, 16383), 0);
    const auto c = render(ownChannel, midi);
    CHECK_NEAR(difference(a, b), 0, 1e-7);
    CHECK(difference(a, c) > 0.01);
    midi.clear();
    midi.addEvent(juce::MidiMessage::pitchWheel(2, 16383), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 0);
    render(cleared, midi, 32);
    cleared.reset();
    const auto d = render(cleared, note(2));
    CHECK_NEAR(difference(a, d), 0, 1e-7);
}

TEST_CASE("wavetable: signed bend amounts invert pitch direction and negative unison width reverses stereo placement")
{
    W up, inverted, down;
    for (auto* synth : {&up, &inverted, &down}) isolated(*synth);
    inverted.setParameter(W::BendUp, -2);
    auto bendUp = note();
    bendUp.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0);
    auto bendDown = note();
    bendDown.addEvent(juce::MidiMessage::pitchWheel(1, 0), 0);
    const auto positive = render(up, bendUp), negative = render(inverted, bendUp), standardDown = render(down, bendDown);
    CHECK(difference(positive, negative) > 0.01);
    CHECK_NEAR(difference(negative, standardDown), 0, 2e-6);
    const auto wide = tone([](W& synth) {
        synth.setParameter(W::oscParam(0, W::Unison), 5);
        synth.setParameter(W::oscParam(0, W::Width), 0.8f);
    });
    const auto reversed = tone([](W& synth) {
        synth.setParameter(W::oscParam(0, W::Unison), 5);
        synth.setParameter(W::oscParam(0, W::Width), -0.8f);
    });
    for (int sample = 0; sample < wide.getNumSamples(); ++sample)
    {
        CHECK_NEAR(wide.getSample(0, sample), reversed.getSample(1, sample), 2e-6);
        CHECK_NEAR(wide.getSample(1, sample), reversed.getSample(0, sample), 2e-6);
    }
}

TEST_CASE("wavetable: unrouted note velocity leaves output identical and explicit velocity modulation changes amplitude")
{
    juce::MidiBuffer quiet, loud;
    quiet.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(1)), 0);
    loud.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(127)), 0);
    W quietDefault, loudDefault;
    isolated(quietDefault); isolated(loudDefault);
    const auto plainQuiet = render(quietDefault, quiet), plainLoud = render(loudDefault, loud);
    CHECK_NEAR(difference(plainQuiet, plainLoud), 0, 0);
    W quietRouted, loudRouted;
    for (auto* synth : {&quietRouted, &loudRouted})
    {
        isolated(*synth);
        route(*synth, 0, W::Velocity, W::Amplitude, 0.5f);
    }
    const auto modQuiet = render(quietRouted, quiet), modLoud = render(loudRouted, loud);
    CHECK(modLoud.getRMSLevel(0, 100, 1800) > modQuiet.getRMSLevel(0, 100, 1800) * 2);
    CHECK(difference(modQuiet, modLoud) > 0.01);
}

TEST_CASE("wavetable: amp sustain retains its stored linear value and produces the measured squared amplitude")
{
    const auto full = tone([](W&) {});
    for (const float sustain : {0.25f, 0.5f, 0.75f})
    {
        W synth;
        isolated(synth);
        synth.setParameter(W::envParam(0, W::Sustain), sustain);
        CHECK_NEAR(synth.getParameter(W::envParam(0, W::Sustain)), sustain, 0);
        const auto partial = render(synth, note());
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < full.getNumSamples(); ++sample)
                CHECK_NEAR(partial.getSample(channel, sample), full.getSample(channel, sample) * sustain * sustain, 2e-7);
    }
}

TEST_CASE("wavetable: all sound off and bypass clear voices while instrument output preserves its input")
{
    W synth;
    isolated(synth);
    synth.setParameter(W::envParam(0, W::Release), 8);
    auto midi = note();
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    render(synth, midi, 256);
    CHECK(synth.activeVoiceCount() == 1);
    midi.clear();
    midi.addEvent(juce::MidiMessage::allSoundOff(1), 83);
    auto audio = render(synth, midi, 256);
    CHECK_NEAR(audio.getMagnitude(83, 173), 0, 1e-7);
    CHECK(synth.activeVoiceCount() == 0);
    render(synth, note(), 256);
    synth.setBypassed(true);
    audio.setSize(2, 256);
    audio.clear();
    midi = note(1, 72, 12, 100);
    synth.process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(0, 256), 0, 0);
    CHECK(synth.activeVoiceCount() == 0);
    synth.setBypassed(false);
    midi.clear();
    audio = render(synth, midi, 256);
    CHECK_NEAR(audio.getMagnitude(0, 256), 0, 0);
    CHECK(synth.activeVoiceCount() == 0);
}

TEST_CASE("wavetable: unison dual filters and modulation render identically across uneven host blocks")
{
    W whole, split;
    for (auto* synth : {&whole, &split})
    {
        isolated(*synth, 4096);
        synth->setParameter(W::oscParam(0, W::Unison), 7);
        synth->setParameter(W::oscParam(0, W::Detune), 31);
        synth->setParameter(W::oscParam(0, W::Width), 0.8f);
        synth->setParameter(W::oscParam(0, W::Position), 0.6f);
        synth->setParameter(W::oscParam(1, W::On), 1);
        synth->setParameter(W::oscParam(1, W::Table), W::Harmonic);
        synth->setParameter(W::oscParam(1, W::Level), 0.3f);
        synth->setParameter(W::oscParam(1, W::Semi), 7);
        synth->setParameter(W::NoiseOn, 1);
        synth->setParameter(W::NoiseLevel, 0.04f);
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), W::Low24);
        synth->setParameter(W::filterParam(0, W::Cutoff), 1600);
        synth->setParameter(W::filterParam(0, W::Resonance), 0.4f);
        synth->setParameter(W::filterParam(1, W::FilterOn), 1);
        synth->setParameter(W::filterParam(1, W::FilterMode), W::Notch);
        synth->setParameter(W::filterParam(1, W::Cutoff), 800);
        synth->setParameter(W::oscParam(0, W::RouteFilter2), 1);
        synth->setParameter(W::envParam(0, W::Release), 0.03f);
        synth->setParameter(W::lfoParam(0, W::Shape), 2);
        synth->setParameter(W::lfoParam(0, W::SyncOn), 0);
        synth->setParameter(W::lfoParam(0, W::Rate), 7.3f);
        route(*synth, 0, W::Lfo1, W::Osc1Position, 0.25f);
        route(*synth, 63, W::ModWheel, W::Filter1Cutoff, 0.2f);
    }
    auto midi = note(3, 69, 31, 1703);
    midi.addEvent(juce::MidiMessage::noteOn(2, 64, static_cast<juce::uint8>(90)), 779);
    midi.addEvent(juce::MidiMessage::pitchWheel(3, 10000), 555);
    midi.addEvent(juce::MidiMessage::controllerEvent(3, 1, 110), 999);
    midi.addEvent(juce::MidiMessage::noteOff(2, 64), 2741);
    const auto a = render(whole, midi, 4096, 4096);
    const auto b = render(split, midi, 4096, 97);
    finiteAndBounded(a);
    finiteAndBounded(b);
    CHECK_NEAR(difference(a, b), 0, 2e-6);
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 4096; ++sample)
            CHECK_NEAR(a.getSample(channel, sample), b.getSample(channel, sample), 2e-5);
}

TEST_CASE("wavetable: each oscillator sub and generated noise can sound independently")
{
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        const auto audio = tone([oscillator](W& synth) {
            for (int index = 0; index < W::kOscillators; ++index)
                synth.setParameter(W::oscParam(index, W::On), index == oscillator ? 1.0f : 0.0f);
        });
        CHECK(audio.getRMSLevel(0, 100, 1800) > 0.01f);
        finiteAndBounded(audio);
    }
    for (bool noise : {false, true})
    {
        const auto audio = tone([noise](W& synth) {
            synth.setParameter(W::oscParam(0, W::On), 0);
            synth.setParameter(noise ? W::NoiseOn : W::SubOn, 1);
            synth.setParameter(noise ? W::NoiseLevel : W::SubLevel, 0.5f);
        });
        CHECK(audio.getRMSLevel(0, 100, 1800) > 0.005f);
        finiteAndBounded(audio);
    }
    const auto silent = tone([](W& synth) { synth.setParameter(W::oscParam(0, W::On), 0); });
    CHECK_NEAR(silent.getMagnitude(0, silent.getNumSamples()), 0, 0);
}

TEST_CASE("wavetable: every own warp type changes audio and either warp stage can be used")
{
    auto setup = [](W& synth) {
        synth.setParameter(W::oscParam(0, W::Position), 0.63f);
        synth.setParameter(W::oscParam(1, W::On), 1);
        synth.setParameter(W::oscParam(1, W::Table), W::Harmonic);
        synth.setParameter(W::oscParam(1, W::Level), 0.3f);
        synth.setParameter(W::oscParam(1, W::Semi), 7);
        synth.setParameter(W::oscParam(2, W::On), 1);
        synth.setParameter(W::oscParam(2, W::Table), W::Harmonic);
        synth.setParameter(W::oscParam(2, W::Level), 0.3f);
        synth.setParameter(W::oscParam(2, W::Semi), 12);
        synth.setParameter(W::SubOn, 1); // sourced warps modulate from the other oscillators and the (silent) Sub
        synth.setParameter(W::SubLevel, 0.0f);
    };
    const auto baseline = tone(setup);
    for (int warp = 1; warp < W::NumWarps; ++warp)
        for (int stage = 0; stage < 2; ++stage)
        {
            const auto audio = tone([&](W& synth) {
                setup(synth);
                synth.setParameter(W::oscParam(0, stage == 0 ? W::Warp1Mode : W::Warp2Mode), static_cast<float>(warp));
                synth.setParameter(W::oscParam(0, stage == 0 ? W::Warp1Amount : W::Warp2Amount), 0.65f);
            });
            finiteAndBounded(audio);
            CHECK(difference(baseline, audio) > 1e-4);
        }
}

TEST_CASE("wavetable: sourced warps are bypassed when their source is disabled")
{
    const auto baseline = tone([](W& synth) { synth.setParameter(W::oscParam(0, W::Position), 0.63f); });
    // Osc 1 reads Osc 2 for the "Osc1" warps (source 0) and Osc 3 for the "Osc2" ones; Sub warps need the Sub on.
    for (int warp : {W::FmOsc1, W::PdOsc1, W::AmOsc1, W::RmOsc1, W::FmOsc2, W::PdOsc2, W::AmOsc2, W::RmOsc2, W::PdSub, W::AmSub, W::RmSub, W::FmSub})
    {
        const auto audio = tone([warp](W& synth) {
            synth.setParameter(W::oscParam(0, W::Position), 0.63f);
            synth.setParameter(W::oscParam(0, W::Warp1Mode), static_cast<float>(warp));
            synth.setParameter(W::oscParam(0, W::Warp1Amount), 0.8f);
        });
        CHECK_NEAR(difference(baseline, audio), 0, 1e-9);
    }
}

TEST_CASE("wavetable: low filtering attenuates tone and every filter type stays finite at boundaries")
{
    auto filtered = [](float cutoff) {
        return tone([cutoff](W& synth) {
            synth.setParameter(W::filterParam(0, W::FilterOn), 1);
            synth.setParameter(W::filterParam(0, W::FilterMode), W::Low24);
            synth.setParameter(W::filterParam(0, W::Cutoff), cutoff);
            synth.setParameter(W::filterParam(0, W::Resonance), 0.05f);
        }, 4096);
    };
    const auto low = filtered(80), high = filtered(12000);
    CHECK(low.getRMSLevel(0, 2000, 2000) < high.getRMSLevel(0, 2000, 2000) * 0.2f);
    for (int type = 0; type < W::NumFilterTypes; ++type)
        for (float cutoff : {20.0f, 20000.0f})
        {
            const auto audio = tone([type, cutoff](W& synth) {
                synth.setParameter(W::oscParam(0, W::Position), 0.8f);
                synth.setParameter(W::filterParam(0, W::FilterOn), 1);
                synth.setParameter(W::filterParam(0, W::FilterMode), static_cast<float>(type));
                synth.setParameter(W::filterParam(0, W::Cutoff), cutoff);
                const auto& infos = synth.parameters();
                synth.setParameter(W::filterParam(0, W::Resonance), infos[W::filterParam(0, W::Resonance)].maxValue);
                synth.setParameter(W::filterParam(0, W::Drive), infos[W::filterParam(0, W::Drive)].maxValue);
            });
            finiteAndBounded(audio);
        }
}

TEST_CASE("wavetable: macros auxiliary gating and the last matrix slot change intended destinations")
{
    auto mapped = [](float macro, float auxiliary, bool useAuxiliary) {
        return tone([=](W& synth) {
            synth.setParameter(W::oscParam(0, W::Level), 0.2f);
            synth.setParameter(W::macroParam(0), macro);
            synth.setParameter(W::macroParam(1), auxiliary);
            route(synth, 63, W::Macro1, W::Osc1Level, 0.5f);
            if (useAuxiliary)
                synth.setParameter(W::modParam(63, W::AuxSource), W::Macro2);
        });
    };
    const auto dry = mapped(0, 1, false), wet = mapped(1, 1, false);
    CHECK(wet.getRMSLevel(0, 100, 1800) > dry.getRMSLevel(0, 100, 1800) * 2);
    const auto blocked = mapped(1, 0, true), passed = mapped(1, 1, true);
    CHECK_NEAR(difference(blocked, dry), 0, 1e-6);
    CHECK_NEAR(difference(passed, wet), 0, 1e-6);

    const auto baseline = tone([](W&) {});
    for (int source = W::Env1; source < W::NumSources; ++source)
    {
        // Alternate 2 and Discrete are zero on the first note by design (covered by the note-source test below).
        if (source == W::NoteAlternate2 || source == W::NoteRandomDiscrete) continue;
        W synth;
        isolated(synth);
        for (int envelope = 1; envelope < W::kEnvelopes; ++envelope)
        {
            synth.setParameter(W::envParam(envelope, W::Attack), 0);
            synth.setParameter(W::envParam(envelope, W::Decay), 0);
            synth.setParameter(W::envParam(envelope, W::Sustain), 1);
        }
        for (int macro = 0; macro < W::kMacros; ++macro)
            synth.setParameter(W::macroParam(macro), 0.8f);
        for (int lfo = 0; lfo < W::kLfos; ++lfo)
        {
            synth.setParameter(W::lfoParam(lfo, W::Shape), 4);
            synth.setParameter(W::lfoParam(lfo, W::SyncOn), 0);
            synth.setParameter(W::lfoParam(lfo, W::Rate), 2);
        }
        // LFO n Y is the attractor's second output: zero on a normal LFO, so give those sources a chaos LFO.
        if (source >= W::Lfo1Y && source <= W::Lfo10Y) synth.setParameter(W::lfoTypeParam(source - W::Lfo1Y), W::LfoLorenz);
        route(synth, 0, static_cast<W::Source>(source), W::Pitch, 0.3f);
        auto midi = note();
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 127), 0);
        midi.addEvent(juce::MidiMessage::channelPressureChange(1, 127), 0);
        const auto audio = render(synth, midi);
        finiteAndBounded(audio);
        CHECK(difference(baseline, audio) > 1e-4);
    }
}

TEST_CASE("wavetable: every modulation destination changes its enabled audio path")
{
    auto configure = [](W& synth) {
        for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        {
            synth.setParameter(W::oscParam(oscillator, W::On), 1);
            synth.setParameter(W::oscParam(oscillator, W::Level), 0.3f);
            synth.setParameter(W::oscParam(oscillator, W::Position), 0.25f + static_cast<float>(oscillator) * 0.2f);
            synth.setParameter(W::oscParam(oscillator, W::Semi), static_cast<float>(oscillator * 3));
            synth.setParameter(W::oscParam(oscillator, W::Warp1Mode), W::Bend);
            synth.setParameter(W::oscParam(oscillator, W::Warp1Amount), 0.1f);
            synth.setParameter(W::oscParam(oscillator, W::Warp2Mode), W::Pulse);
            synth.setParameter(W::oscParam(oscillator, W::Warp2Amount), 0.1f);
            synth.setParameter(W::oscParam(oscillator, W::RouteFilter1), 1);
            synth.setParameter(W::oscParam(oscillator, W::RouteFilter2), 1);
        }
        for (int filter = 0; filter < W::kFilters; ++filter)
        {
            synth.setParameter(W::filterParam(filter, W::FilterOn), 1);
            synth.setParameter(W::filterParam(filter, W::Cutoff), filter == 0 ? 900.0f : 1600.0f);
            synth.setParameter(W::filterParam(filter, W::Resonance), 0.25f);
        }
        synth.setParameter(W::SubOn, 1);
        synth.setParameter(W::SubLevel, 0.15f);
        synth.setParameter(W::NoiseOn, 1);
        synth.setParameter(W::NoiseLevel, 0.1f);
        synth.setParameter(W::macroParam(0), 1);
    };
    const auto baseline = tone(configure);
    for (int destination = W::Pitch; destination <= W::NoiseAmplitude; ++destination)
    {
        const auto audio = tone([&](W& synth) {
            configure(synth);
            route(synth, 0, W::Macro1, static_cast<W::Destination>(destination), 0.3f);
        });
        finiteAndBounded(audio);
        CHECK(difference(baseline, audio) > 1e-5);
    }
}

namespace
{
std::shared_ptr<const devices::OscillatorMedia> ownOscillatorMedia(bool spectral = true)
{
    std::vector<float> pcm(32768 * 2);
    for (int frame = 0; frame < 32768; ++frame)
    {
        const double time = static_cast<double>(frame) / sampleRate;
        pcm[static_cast<size_t>(frame) * 2] = static_cast<float>(0.4 * std::sin(2 * dsp::kPi * 375 * time) + 0.13 * std::sin(2 * dsp::kPi * 1130 * time));
        pcm[static_cast<size_t>(frame) * 2 + 1] = static_cast<float>(0.3 * std::cos(2 * dsp::kPi * 563 * time));
    }
    auto result = devices::OscillatorMedia::fromSamples(pcm, 2, sampleRate, "Own stereo media", spectral);
    REQUIRE(result);
    return result.data;
}
}

TEST_CASE("wavetable: each oscillator plays prepared sample granular and spectral media with immutable callback ownership")
{
    const auto media = ownOscillatorMedia();
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        for (int mode = W::SampleMode; mode < W::NumOscillatorModes; ++mode)
        {
            W synth;
            isolated(synth);
            for (int o = 0; o < W::kOscillators; ++o) synth.setParameter(W::oscParam(o, W::On), o == oscillator ? 1.0f : 0.0f);
            REQUIRE(synth.setOscillatorMedia(oscillator, media));
            CHECK(synth.oscillatorMedia(oscillator) == media.get());
            CHECK(synth.oscillatorMediaName(oscillator) == "Own stereo media");
            CHECK_NEAR(synth.getParameter(W::mediaParam(oscillator, W::MediaMode)), W::SampleMode, 0);
            synth.setParameter(W::mediaParam(oscillator, W::MediaMode), static_cast<float>(mode));
            synth.setParameter(W::mediaParam(oscillator, W::MediaStartPosition), 0.25f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaLoopMode), 2);
            synth.setParameter(W::mediaParam(oscillator, W::MediaScanRate), mode >= W::GranularMode ? 0.0f : 1.0f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaGrainSize), 0.5f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaDensityMode), 0);
            synth.setParameter(W::mediaParam(oscillator, W::MediaDensity), 0.1f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaWindow), 2);
            CHECK_NEAR(synth.waveformAt(oscillator, 0.25f, 0), media->zones()[0].sample->sampleLinear(32767 * 0.25, 0), 2e-6);
            const auto references = media.use_count();
            const auto audio = render(synth, note(1, 60), 4096);
            CHECK(media.use_count() == references);
            finiteAndBounded(audio);
            CHECK(audio.getRMSLevel(0, 100, 3000) > 0.01f);
            CHECK(audio.getRMSLevel(1, 100, 3000) > 0.01f);
            synth.reset(); CHECK(synth.oscillatorMedia(oscillator) == media.get());
            REQUIRE(synth.setOscillatorMedia(oscillator, {}));
            CHECK(synth.oscillatorMedia(oscillator) == nullptr);
            CHECK_NEAR(synth.getParameter(W::mediaParam(oscillator, W::MediaMode)), W::WavetableMode, 0);
        }
    W invalid;
    CHECK(! invalid.setOscillatorMedia(-1, media)); CHECK(! invalid.setOscillatorMedia(3, media));
}

TEST_CASE("wavetable: media engines retain MIDI sample timing channel pedal bypass and uneven block determinism")
{
    const auto media = ownOscillatorMedia();
    for (int mode = W::SampleMode; mode < W::NumOscillatorModes; ++mode)
    {
        W whole, split;
        for (auto* synth : {&whole, &split})
        {
            isolated(*synth, 4096);
            REQUIRE(synth->setOscillatorMedia(0, media));
            synth->setParameter(W::mediaParam(0, W::MediaMode), static_cast<float>(mode));
            synth->setParameter(W::mediaParam(0, W::MediaStartPosition), 0.2f);
            synth->setParameter(W::mediaParam(0, W::MediaLoopMode), 2);
            synth->setParameter(W::mediaParam(0, W::MediaGrainSize), 0.02f);
            synth->setParameter(W::mediaParam(0, W::MediaPositionSpread), 0.01f);
            synth->setParameter(W::mediaParam(0, W::MediaPitchSpread), 2);
            synth->setParameter(W::mediaParam(0, W::MediaJumpStart), 1);
            synth->setParameter(W::envParam(0, W::Release), 0.02f);
            route(*synth, 63, W::ModWheel, W::Osc1MediaScan, 0.1f);
        }
        auto events = note(2, 60, 17, 3200);
        events.addEvent(juce::MidiMessage::pitchWheel(2, 12000), 517);
        events.addEvent(juce::MidiMessage::controllerEvent(2, 1, 95), 811);
        events.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 2500);
        events.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 3300);
        const auto a = render(whole, events, 4096, 4096), b = render(split, events, 4096, 97);
        CHECK_NEAR(difference(a, b), 0, 2e-6); finiteAndBounded(a);
        CHECK_NEAR(a.getMagnitude(0, 17), 0, 0);
        CHECK(whole.activeVoiceCount() == 1);
        split.setBypassed(true);
        const auto stopped = render(split, {}, 128, 128);
        CHECK_NEAR(stopped.getMagnitude(0, 128), 0, 0); CHECK(split.activeVoiceCount() == 0);
    }
}

TEST_CASE("wavetable: multisample zones receive exact note velocity and enable velocity gain only through asset tracking")
{
    const auto soft = devices::OscillatorSample::fromSamples(std::vector<float>(8192, 0.2f), 1, sampleRate);
    const auto loud = devices::OscillatorSample::fromSamples(std::vector<float>(8192, 0.6f), 1, sampleRate);
    REQUIRE(soft); REQUIRE(loud);
    devices::OscillatorZone low, high, tracked;
    low.sample = soft.data; low.lowKey = 60; low.highKey = 62; low.lowVelocity = 1; low.highVelocity = 63;
    high = low; high.sample = loud.data; high.lowVelocity = 64; high.highVelocity = 127;
    tracked = low; tracked.lowKey = 64; tracked.highKey = 64; tracked.highVelocity = 127; tracked.velocityTracking = 1;
    const auto media = devices::OscillatorMedia::fromZones({low, high, tracked}, "Own velocity map"); REQUIRE(media);
    auto mapped = [&](int pitch, int velocity, bool track = true) {
        W synth;
        isolated(synth);
        REQUIRE(synth.setOscillatorMedia(0, media.data));
        CHECK_NEAR(synth.getParameter(W::mediaParam(0, W::MediaMode)), W::MultisampleMode, 0);
        synth.setParameter(W::mediaParam(0, W::MediaVelocityTracking), track ? 1.0f : 0.0f);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(velocity)), 0);
        return render(synth, midi, 128, 128).getSample(0, 64);
    };
    const float lowValue = mapped(62, 1), highValue = mapped(62, 127);
    CHECK(lowValue > 0.01f); CHECK_NEAR(highValue, lowValue * 3, 2e-6);
    CHECK_NEAR(mapped(63, 127), 0, 0);
    CHECK_NEAR(mapped(64, 64), mapped(64, 127) * (64.0f / 127) * (64.0f / 127), 2e-6);
    CHECK_NEAR(mapped(64, 1, false), mapped(64, 127), 2e-6);
}

TEST_CASE("wavetable: suspended media replacement releases old ownership and refreshes held voice cursors safely")
{
    W synth;
    isolated(synth);
    std::weak_ptr<const devices::OscillatorMedia> oldOwner;
    {
        auto old = devices::OscillatorMedia::fromSamples(std::vector<float>(8192, 0.25f), 1, sampleRate);
        REQUIRE(old); oldOwner = old.data;
        REQUIRE(synth.setOscillatorMedia(0, old.data));
    }
    const auto previous = render(synth, note(1, 60), 5, 5);
    CHECK(! oldOwner.expired());
    const auto replacement = devices::OscillatorMedia::fromSamples(std::vector<float>(8192, 0.75f), 1, sampleRate);
    REQUIRE(replacement); REQUIRE(synth.setOscillatorMedia(0, replacement.data));
    CHECK(oldOwner.expired());
    const auto resumed = render(synth, {}, 64, 64);
    CHECK_NEAR(resumed.getSample(0, 0), previous.getSample(0, 4) * 3, 2e-6);
    finiteAndBounded(resumed);
}

TEST_CASE("wavetable: appended scan position grain size and density matrix destinations change every oscillator media path")
{
    const auto media = ownOscillatorMedia(false);
    for (int destination = W::Osc1MediaScan; destination <= W::Osc3GrainDensity; ++destination)
    {
        const int oscillator = (destination - W::Osc1MediaScan) % W::kOscillators;
        auto configure = [&](W& synth) {
            for (int o = 0; o < W::kOscillators; ++o) synth.setParameter(W::oscParam(o, W::On), o == oscillator ? 1.0f : 0.0f);
            REQUIRE(synth.setOscillatorMedia(oscillator, media));
            synth.setParameter(W::mediaParam(oscillator, W::MediaMode), W::GranularMode);
            synth.setParameter(W::mediaParam(oscillator, W::MediaStartPosition), 0.2f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaScanRate), 0.5f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaLoopMode), 2);
            synth.setParameter(W::mediaParam(oscillator, W::MediaGrainSize), 0.02f);
            synth.setParameter(W::mediaParam(oscillator, W::MediaDensity), 3);
            synth.setParameter(W::mediaParam(oscillator, W::MediaJumpStart), 1);
            synth.setParameter(W::macroParam(0), 1);
        };
        const auto baseline = tone(configure, 4096);
        const auto routed = tone([&](W& synth) { configure(synth); route(synth, 0, W::Macro1, static_cast<W::Destination>(destination), 0.25f); }, 4096);
        CHECK(difference(baseline, routed) > 1e-5);
        finiteAndBounded(routed);
    }
}

TEST_CASE("wavetable: envelope delay and tempo anchored versus note retriggered LFO have audible timing")
{
    W delayed;
    isolated(delayed);
    delayed.setParameter(W::envParam(0, W::Delay), 0.01f);
    const auto audio = render(delayed, note(), 2048);
    CHECK_NEAR(audio.getMagnitude(0, 470), 0, 0);
    CHECK(audio.getRMSLevel(0, 600, 1000) > 0.01f);

    auto lfoRender = [](W::LfoMode mode, double tempo, double beat) {
        W synth;
        isolated(synth);
        synth.setParameter(W::lfoParam(0, W::Shape), 3);
        synth.setParameter(W::lfoParam(0, W::SyncOn), 1);
        synth.setParameter(W::lfoParam(0, W::Mode), static_cast<float>(mode));
        const auto& info = synth.parameters()[W::lfoParam(0, W::Division)];
        int division = static_cast<int>(info.minValue);
        for (int candidate = static_cast<int>(info.minValue); candidate <= static_cast<int>(info.maxValue); ++candidate)
            if (std::abs(W::divisionBeats(candidate) - 1) < 1e-9)
                division = candidate;
        synth.setParameter(W::lfoParam(0, W::Division), static_cast<float>(division));
        route(synth, 0, W::Lfo1, W::Amplitude, 0.7f);
        return render(synth, note(), 4096, 512, tempo, beat);
    };
    const auto global = lfoRender(W::Global, 120, 0), shifted = lfoRender(W::Global, 120, 0.5);
    CHECK(difference(global, shifted) > 0.005);
    const auto faster = lfoRender(W::Global, 240, 0);
    CHECK(difference(global, faster) > 1e-4);
    const auto retrigger = lfoRender(W::Retrigger, 120, 0), retriggerShifted = lfoRender(W::Retrigger, 120, 0.5);
    CHECK_NEAR(difference(retrigger, retriggerShifted), 0, 1e-6);
    const auto oneShot = lfoRender(W::OneShot, 120, 0);
    finiteAndBounded(oneShot);
}

TEST_CASE("wavetable: voice stealing maximum unison and all parameter boundaries stay finite and release")
{
    W synth;
    isolated(synth, 512);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        synth.setParameter(W::oscParam(oscillator, W::On), 1);
        synth.setParameter(W::oscParam(oscillator, W::Unison), W::kUnison);
        synth.setParameter(W::oscParam(oscillator, W::Detune), 100);
        synth.setParameter(W::oscParam(oscillator, W::Random), 1);
        synth.setParameter(W::oscParam(oscillator, W::Position), 0.8f);
    }
    juce::MidiBuffer midi;
    midi.ensureSize(65536);
    for (int pitch = 24; pitch < 88; ++pitch)
        midi.addEvent(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(127)), pitch - 24);
    auto audio = render(synth, midi, 512);
    CHECK(synth.activeVoiceCount() == W::kVoices);
    finiteAndBounded(audio);
    midi.clear();
    for (int pitch = 24; pitch < 88; ++pitch)
        midi.addEvent(juce::MidiMessage::noteOff(1, pitch), 5);
    audio = render(synth, midi, 512);
    CHECK(synth.activeVoiceCount() == 0);
    CHECK_NEAR(audio.getMagnitude(5, 507), 0, 1e-6);
    for (bool maximum : {false, true})
    {
        W boundary;
        isolated(boundary, 256);
        for (int index = 0; index < W::ParameterCount; ++index)
        {
            const auto& parameter = boundary.parameters()[static_cast<size_t>(index)];
            boundary.setParameter(index, maximum ? parameter.maxValue : parameter.minValue);
            CHECK(std::isfinite(boundary.getParameter(index)));
        }
        // Keep one signal path audible rather than hiding extreme oscillator/filter values behind a 32 s delay.
        boundary.setParameter(W::oscParam(0, W::On), 1);
        boundary.setParameter(W::oscParam(0, W::Level), 0.8f);
        boundary.setParameter(W::envParam(0, W::Delay), 0);
        boundary.setParameter(W::envParam(0, W::Attack), 0);
        boundary.setParameter(W::envParam(0, W::Hold), 0);
        boundary.setParameter(W::envParam(0, W::Decay), 0);
        boundary.setParameter(W::envParam(0, W::Sustain), 1);
        audio = render(boundary, note(16, maximum ? 127 : 0), 512, 256);
        finiteAndBounded(audio, 512);
        boundary.reset();
        CHECK(boundary.activeVoiceCount() == 0);
    }
}

TEST_CASE("wavetable: factory state capture save and reload preserve oscillator envelopes matrix and macros")
{
    const auto* entry = model::findCatalogEntry("SonoraWavetable");
    REQUIRE(entry != nullptr);
    CHECK(entry->implemented);
    CHECK(entry->role == model::DeviceRole::Instrument);
    CHECK(model::deviceCategory(*entry) == "Synthesis");
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    CHECK(device.displayName == "Sonora Prism");
    devices::BuildContext context;
    auto processor = devices::createDevice(device, context);
    auto* synth = dynamic_cast<W*>(processor.get());
    REQUIRE(synth != nullptr);
    synth->setParameter(W::oscParam(0, W::Position), 0.65f);
    synth->setParameter(W::oscParam(0, W::Unison), 5);
    synth->setParameter(W::oscParam(0, W::Detune), 24);
    synth->setParameter(W::oscParam(1, W::On), 1);
    synth->setParameter(W::oscParam(1, W::Table), W::Hollow);
    synth->setParameter(W::oscParam(1, W::Octave), -1);
    synth->setParameter(W::oscParam(1, W::Level), 0.3f);
    synth->setParameter(W::oscParam(0, W::Warp1Mode), W::Bend);
    synth->setParameter(W::oscParam(0, W::Warp1Amount), 0.2f);
    synth->setParameter(W::envParam(2, W::Attack), 0.7f);
    synth->setParameter(W::lfoParam(8, W::Shape), 3);
    synth->setParameter(W::lfoParam(8, W::Mode), W::Retrigger);
    route(*synth, 63, W::Lfo9, W::Osc1Position, 0.35f);
    synth->setParameter(W::modParam(63, W::AuxSource), W::Macro8);
    synth->setParameter(W::macroParam(7), 0.73f);
    devices::captureDeviceState(device, processor.get());
    REQUIRE(device.parameters.size() == static_cast<size_t>(W::ParameterCount));

    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_tests");
    REQUIRE(directory.createDirectory().wasOk());
    const auto source = directory.getChildFile("Source.als"), destination = directory.getChildFile("Wavetable.als");
    io::NewSetOptions options;
    options.midiTracks = 1;
    options.audioTracks = 0;
    options.returnTracks = 0;
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tracks[0].name = "Sonora Wavetable";
    model::allocateDeviceIds(song, device);
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    model::EditHistory history;
    history.perform(song, model::insertDevice(0, device));
    song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    model::Clip clip;
    REQUIRE(model::makeEmptyMidiClip(song, 0, 4, clip));
    clip.name = "Generated native wavetable notes";
    for (int step = 0; step < 4; ++step)
        clip.notes.push_back({static_cast<double>(step), 0.5, 60 + step * 3, 110});
    history.perform(song, model::insertClip(0, clip, "Add Native Notes"));
    REQUIRE(io::saveAls(song, destination, io::AlsSaveOptions{}).ok);
    auto again = io::loadAls(destination);
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].devices.size() == 1);
    const auto& saved = again.song->tracks[0].devices[0];
    CHECK(saved.type == "SonoraWavetable");
    CHECK(saved.kind == model::DeviceKind::Native);
    CHECK(! saved.plugin.has_value());
    auto restored = devices::createDevice(saved, context);
    REQUIRE(dynamic_cast<W*>(restored.get()) != nullptr);
    for (int index = 0; index < W::ParameterCount; ++index)
        CHECK_NEAR(restored->getParameter(index), synth->getParameter(index), 1e-5);
    CHECK(again.song->tracks[0].arrangementClips.size() == 1);
    CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == 4);
}

namespace
{
std::vector<float> ownStereoTable()
{
    constexpr int length = devices::WavetableData::nativeFrameLength;
    std::vector<float> samples(static_cast<size_t>(length) * 2 * 2);
    for (int frame = 0; frame < 2; ++frame)
        for (int sample = 0; sample < length; ++sample)
        {
            const double phase = 2 * dsp::kPi * sample / length;
            const size_t index = static_cast<size_t>(frame * length + sample) * 2;
            samples[index] = static_cast<float>(std::sin(phase) * (frame == 0 ? 1 : -1));
            samples[index + 1] = static_cast<float>(std::cos(phase) * (frame == 0 ? 1.0 : 0.25));
        }
    return samples;
}
void writeOwnTable(const juce::File& file, const std::vector<float>& interleaved, int channels)
{
    REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    REQUIRE(stream != nullptr);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(channels).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    const int count = static_cast<int>(interleaved.size() / static_cast<size_t>(channels));
    juce::AudioBuffer<float> samples(channels, count);
    for (int sample = 0; sample < count; ++sample)
        for (int channel = 0; channel < channels; ++channel)
            samples.setSample(channel, sample, interleaved[static_cast<size_t>(sample) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] * 0.75f);
    REQUIRE(writer->writeFromAudioSampleBuffer(samples, 0, count));
}
}

TEST_CASE("wavetable: imported matrix pitch level pan and cutoff match their clamped static parameter settings")
{
    const auto same = [](const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b) {
        REQUIRE(a.getNumSamples() == b.getNumSamples());
        for (int c = 0; c < 2; ++c) for (int i = 0; i < a.getNumSamples(); ++i)
            CHECK_NEAR(a.getSample(c, i), b.getSample(c, i), 8e-6);
    };
    for (const float amount : {0.25f, 0.5f, 1.0f, -1.0f})
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        REQUIRE(modulated.setImportedModulationDomain(W::Osc1Pitch, W::ImportedModulationDomain::CoarsePitch128));
        route(modulated, 0, W::Macro1, W::Osc1Pitch, amount); modulated.setParameter(W::macroParam(0), 1);
        fixed.setParameter(W::coarseParam(0), std::clamp(amount * 128, -64.0f, 64.0f));
        same(render(modulated, note()), render(fixed, note()));
    }
    for (const bool envelopeSource : {false, true})
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        REQUIRE(modulated.setImportedModulationDomain(W::Osc1Pitch, W::ImportedModulationDomain::CoarsePitch128));
        route(modulated, 63, envelopeSource ? W::Env2 : W::Macro1, W::Osc1Pitch, 0.25f);
        if (envelopeSource)
        {
            modulated.setParameter(W::envParam(1, W::Delay), 0); modulated.setParameter(W::envParam(1, W::Attack), 0);
            modulated.setParameter(W::envParam(1, W::Hold), 0); modulated.setParameter(W::envParam(1, W::Decay), 0);
            modulated.setParameter(W::envParam(1, W::Sustain), 0.5f);
            fixed.setParameter(W::coarseParam(0), 8);
        }
        else
        {
            modulated.setParameter(W::macroParam(0), 0.5f); modulated.setParameter(W::macroParam(4), 0.5f);
            modulated.setParameter(W::modParam(63, W::AuxSource), W::Macro5); fixed.setParameter(W::coarseParam(0), 8);
        }
        same(render(modulated, note()), render(fixed, note()));
    }
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        REQUIRE(modulated.setImportedModulationDomain(W::Osc1Level, W::ImportedModulationDomain::SquaredLevel));
        modulated.setParameter(W::oscParam(0, W::Level), 0.5625f); fixed.setParameter(W::oscParam(0, W::Level), 0.25f);
        route(modulated, 0, W::Macro1, W::Osc1Level, -0.25f); modulated.setParameter(W::macroParam(0), 1);
        same(render(modulated, note()), render(fixed, note()));
    }
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        REQUIRE(modulated.setImportedModulationDomain(W::Osc1Pan, W::ImportedModulationDomain::Pan2));
        route(modulated, 0, W::Macro1, W::Osc1Pan, 0.25f); modulated.setParameter(W::macroParam(0), 1);
        fixed.setParameter(W::oscParam(0, W::OscPan), 0.5f);
        same(render(modulated, note()), render(fixed, note()));
    }
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        REQUIRE(modulated.setImportedModulationDomain(W::Filter1Cutoff, W::ImportedModulationDomain::FilterFrequency));
        modulated.setParameter(W::filterParam(0, W::FilterOn), 1); fixed.setParameter(W::filterParam(0, W::FilterOn), 1);
        modulated.setParameter(W::filterParam(0, W::Cutoff), W::observedFilterHertz(0.5f));
        fixed.setParameter(W::filterParam(0, W::Cutoff), W::observedFilterHertz(0.75f));
        route(modulated, 0, W::Macro1, W::Filter1Cutoff, 0.25f); modulated.setParameter(W::macroParam(0), 1);
        same(render(modulated, note()), render(fixed, note()));
    }
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        route(modulated, 0, W::Macro1, W::Osc1Pitch, 0.25f); modulated.setParameter(W::macroParam(0), 1);
        fixed.setParameter(W::coarseParam(0), 6); // Original own-patch pitch matrix units remain 24 semitones.
        same(render(modulated, note()), render(fixed, note()));
        CHECK(! modulated.setImportedModulationDomain(W::NumDestinations, W::ImportedModulationDomain::CoarsePitch128));
        CHECK(! modulated.setImportedModulationDomain(W::Amplitude, W::ImportedModulationDomain::CoarsePitch128));
    }
}

TEST_CASE("wavetable: imported envelopes interpolate squared sustain endpoints with linear center stages")
{
    W carrier, shaped; isolated(carrier); isolated(shaped);
    shaped.setImportedEnvelopeProfile(true);
    shaped.setParameter(W::envParam(0, W::Attack), 0.1f);
    shaped.setParameter(W::envParam(0, W::AttackCurve), 0);
    const auto dry = render(carrier, note(), 4800), attack = render(shaped, note(), 4800);
    for (int sample : {1200, 2400, 3600})
    {
        REQUIRE(std::abs(dry.getSample(0, sample)) > 0.01f);
        CHECK_NEAR(attack.getSample(0, sample) / dry.getSample(0, sample), (sample + 1.0) / 4800, 2e-4);
    }
    W sustain, full; isolated(sustain); isolated(full);
    sustain.setImportedEnvelopeProfile(true); sustain.setParameter(W::envParam(0, W::Sustain), 0.5f);
    const auto held = render(sustain, note()), baseline = render(full, note());
    for (int c = 0; c < 2; ++c) for (int sample = 0; sample < held.getNumSamples(); ++sample)
        CHECK_NEAR(held.getSample(c, sample), baseline.getSample(c, sample) * 0.25f, 1e-6);
    // Independent fit corroborated by controlled interior-curve carrier regressions; extreme clamp is unverified.
    const auto slow = dsp::ExponentialEnvelopeCurve::fromTension(-0.5f);
    const auto fast = dsp::ExponentialEnvelopeCurve::fromTension(0.5f);
    CHECK_NEAR(slow.eval(0.5f), 0.0625, 2e-6); CHECK_NEAR(fast.eval(0.5f), 0.9375, 2e-6);
    CHECK_NEAR(slow.eval(0.2470637f), 0.012572, 0.0007);
    for (float tension : {-1.0f, -0.8f, 0.0f, 0.8f, 1.0f})
    {
        const auto curve = dsp::ExponentialEnvelopeCurve::fromTension(tension);
        float previous = 0;
        for (int i = 0; i <= 100; ++i)
        {
            const float value = curve.eval(static_cast<float>(i) / 100);
            CHECK(std::isfinite(value)); CHECK(value >= previous); CHECK(value >= 0 && value <= 1); previous = value;
        }
    }
}

TEST_CASE("wavetable: imported master zero and sub-floor levels are preserved before and after native FX")
{
    for (bool withEffects : {false, true})
    {
        W muted, quiet, reference;
        for (auto* synth : {&muted, &quiet, &reference})
        {
            isolated(*synth);
            if (withEffects)
            {
                devices::WavetableEffects::Configuration configuration;
                devices::WavetableEffects::Module module; module.id = 1; module.type = devices::WavetableEffects::Type::Utility;
                configuration.buses[0].push_back(module); std::string error; REQUIRE(synth->setEffectsConfiguration(configuration, error));
            }
            synth->setParameter(W::Volume, -60);
        }
        REQUIRE(muted.setImportedMasterLevel(0)); REQUIRE(quiet.setImportedMasterLevel(0.0001));
        const auto silence = render(muted, note()), low = render(quiet, note()), baseline = render(reference, note());
        CHECK_NEAR(silence.getMagnitude(0, silence.getNumSamples()), 0, 0);
        for (int channel = 0; channel < 2; ++channel)
            for (int frame = 0; frame < low.getNumSamples(); ++frame)
                CHECK_NEAR(low.getSample(channel, frame), baseline.getSample(channel, frame) * std::sqrt(2.0f) * 0.1f, 1e-7);
        muted.setParameter(W::Volume, -12);
        CHECK(render(muted, {}, 1024).getMagnitude(0, 1024) > 0.01f);
        CHECK(! muted.setImportedMasterLevel(-1)); CHECK(! muted.setImportedMasterLevel(2));
        CHECK(! muted.setImportedMasterLevel(std::numeric_limits<double>::quiet_NaN()));
    }
}

TEST_CASE("wavetable: imported free Hz LFO seeds from host block time after silence and advances while held")
{
    const auto configure = [](W& synth, bool global) {
        isolated(synth); synth.setImportedLfoProfile(true);
        dsp::CurveGraph graph; graph.count = 3;
        graph.x[0] = 0; graph.x[1] = 0.9999f; graph.x[2] = 1;
        graph.y[0] = 0; graph.y[1] = 1; graph.y[2] = 0;
        REQUIRE(synth.setLfoCurve(0, graph));
        synth.setParameter(W::lfoParam(0, W::Mode), global ? W::Global : W::Retrigger);
        synth.setParameter(W::lfoParam(0, W::SyncOn), 0); synth.setParameter(W::lfoParam(0, W::Rate), 1);
        synth.setParameter(W::lfoParam(0, W::Bipolar), 0);
        synth.setParameter(W::oscParam(0, W::Level), 0);
        REQUIRE(synth.setImportedModulationDomain(W::Osc1Level, W::ImportedModulationDomain::SquaredLevel));
        route(synth, 0, W::Lfo1, W::Osc1Level, 1);
    };
    W fresh, warmed; configure(fresh, true); configure(warmed, true);
    CHECK_NEAR(render(warmed, {}, 8192).getMagnitude(0, 8192), 0, 0);
    CHECK_NEAR(difference(render(fresh, note(1, 60, 17), 8192), render(warmed, note(1, 60, 17), 8192)), 0, 0);
    W free, shifted, retrigger; configure(free, true); configure(shifted, true); configure(retrigger, false);
    auto events = note(1, 60, 17, 12000);
    events.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 36000);
    events.addEvent(juce::MidiMessage::noteOff(1, 60), 60000);
    const auto a = render(free, events, 72000, 512, 120, 0);
    const auto b = render(shifted, events, 72000, 512, 120, 2); // Integer Hz cycle offset.
    const auto c = render(retrigger, events, 72000);
    CHECK_NEAR(difference(a, b), 0, 1e-10); CHECK(difference(a, c) > 0.01);
    CHECK(a.getRMSLevel(0, 40000, 4000) > c.getRMSLevel(0, 40000, 4000) * 2);
}

TEST_CASE("wavetable: imported secondary envelopes use linear start end and retain their release endpoint")
{
    CHECK(W::EnvLevelStart == 609); CHECK(W::envLevelParam(0, false) == -1);
    for (int envelope = 1; envelope < W::kEnvelopes; ++envelope)
    {
        W carrier, modulated; isolated(carrier); isolated(modulated);
        for (auto* synth : {&carrier, &modulated})
        {
            synth->setImportedEnvelopeProfile(true);
            synth->setParameter(W::oscParam(0, W::Level), 1);
            synth->setParameter(W::envParam(0, W::Release), 2);
        }
        modulated.setParameter(W::oscParam(0, W::Level), 0);
        REQUIRE(modulated.setImportedModulationDomain(W::Osc1Level, W::ImportedModulationDomain::SquaredLevel));
        route(modulated, 0, static_cast<W::Source>(W::Env1 + envelope), W::Osc1Level, 1);
        modulated.setParameter(W::envLevelParam(envelope, false), 0.5f);
        modulated.setParameter(W::envLevelParam(envelope, true), 0.5f);
        modulated.setParameter(W::envParam(envelope, W::Delay), 0);
        modulated.setParameter(W::envParam(envelope, W::Attack), 0.1f);
        modulated.setParameter(W::envParam(envelope, W::Hold), 0);
        modulated.setParameter(W::envParam(envelope, W::Decay), 0);
        modulated.setParameter(W::envParam(envelope, W::Sustain), 1);
        modulated.setParameter(W::envParam(envelope, W::Release), 0.1f);
        for (auto field : {W::AttackCurve, W::DecayCurve, W::ReleaseCurve}) modulated.setParameter(W::envParam(envelope, field), 0);
        const auto events = note(1, 60, 0, 7200);
        const auto dry = render(carrier, events, 14000), audio = render(modulated, events, 14000);
        for (int sample : {1200, 2400, 3600})
        {
            REQUIRE(std::abs(dry.getSample(0, sample)) > 0.01f);
            CHECK_NEAR(std::sqrt(audio.getSample(0, sample) / dry.getSample(0, sample)), 0.5 + 0.5 * (sample + 1) / 4800.0, 0.003);
        }
        REQUIRE(std::abs(dry.getSample(0, 13200)) > 0.01f);
        CHECK_NEAR(audio.getSample(0, 13200) / dry.getSample(0, 13200), 0.25, 2e-5);
        finiteAndBounded(audio);
    }
}

TEST_CASE("wavetable: imported LFO curves apply phase one shot cap and reversed exponential tension")
{
    dsp::CurveGraph graph; graph.count = 3;
    graph.x[0] = 0; graph.x[1] = 0.5f; graph.x[2] = 1;
    graph.y[0] = 0.1f; graph.y[1] = 0.7f; graph.y[2] = 0.1f;
    graph.curve[0] = -0.5f; graph.curve[1] = 0.5f;
    dsp::ExponentialCurveGraph prepared; prepared.prepare(graph);
    CHECK_NEAR(prepared.eval(0.25f), 0.1 + 0.6 * 0.9375, 2e-6);
    CHECK_NEAR(prepared.eval(0.75f), 0.7 - 0.6 * 0.0625, 2e-6);
    CHECK_NEAR(prepared.eval(1), 0.1, 1e-7);
    graph.curve[0] = graph.curve[1] = 0;
    for (const float phase : {0.25f, 0.75f, 1.0f})
    {
        W modulated, fixed; isolated(modulated); isolated(fixed);
        modulated.setImportedLfoProfile(true); REQUIRE(modulated.setLfoCurve(0, graph));
        modulated.setParameter(W::lfoParam(0, W::Mode), W::OneShot);
        modulated.setParameter(W::lfoParam(0, W::SyncOn), 0); modulated.setParameter(W::lfoParam(0, W::Rate), 0);
        modulated.setParameter(W::lfoParam(0, W::LfoPhase), phase); modulated.setParameter(W::lfoParam(0, W::Bipolar), 0);
        for (auto* synth : {&modulated, &fixed})
        {
            synth->setParameter(W::oscParam(0, W::Level), 0);
            REQUIRE(synth->setImportedModulationDomain(W::Osc1Level, W::ImportedModulationDomain::SquaredLevel));
        }
        route(modulated, 0, W::Lfo1, W::Osc1Level, 1);
        route(fixed, 0, W::Macro1, W::Osc1Level, 1);
        fixed.setParameter(W::macroParam(0), graph.eval(std::min(phase, 0.999f)));
        CHECK_NEAR(difference(render(modulated, note()), render(fixed, note())), 0, 2e-6);
    }
}

TEST_CASE("wavetable: immutable embedded stereo media retains original values and interpolates complete 2048-sample frames")
{
    auto source = ownStereoTable();
    const auto result = devices::WavetableData::fromSamples(source, 2, 2048, 44100, "Independent stereo fixture");
    REQUIRE(result);
    CHECK(result.data->channels() == 2);
    CHECK(result.data->frames() == 2);
    CHECK(result.data->frameLength() == 2048);
    CHECK(result.data->mipLevels() >= 11);
    CHECK_NEAR(result.data->sampleRate(), 44100, 0);
    CHECK(result.data->name() == "Independent stereo fixture");
    CHECK(result.data->originalSamples() == source);
    source[100] = 23;
    CHECK(std::abs(result.data->originalSamples()[100]) < 1);
    CHECK_NEAR(result.data->sample(0, 0.25, 0, 0), 1, 2e-6);
    CHECK_NEAR(result.data->sample(1, 0.25, 0, 0), -1, 2e-6);
    CHECK_NEAR(result.data->sample(0.5f, 0.25, 0, 0), 0, 2e-6);
    CHECK_NEAR(result.data->sample(0, 0, 0, 1), 1, 2e-6);
    CHECK_NEAR(result.data->sample(1, 0, 0, 1), 0.25, 2e-6);
    CHECK_NEAR(result.data->sample(0.5f, 0, 0, 1), 0.625, 2e-6);
    CHECK_NEAR(result.data->sample(0, -0.75, 0, 0), result.data->sample(0, 0.25, 0, 0), 0);
    CHECK_NEAR(result.data->sample(0, 1.25, 0, 0), result.data->sample(0, 0.25, 0, 0), 0);
}

TEST_CASE("wavetable: custom media validation bounds channels frames alignment sample values and allocations")
{
    const std::vector<float> valid(2048, 0.125f), empty;
    CHECK(devices::WavetableData::fromSamples(valid, 1));
    CHECK(! devices::WavetableData::fromSamples(valid, 0));
    CHECK(! devices::WavetableData::fromSamples(valid, 3));
    CHECK(! devices::WavetableData::fromSamples(empty, 1));
    CHECK(! devices::WavetableData::fromSamples(std::span<const float>(valid).first(2047), 1));
    CHECK(! devices::WavetableData::fromSamples(std::span<const float>(valid).first(2047), 2));
    for (const int length : {0, 63, 2047, 4096}) CHECK(! devices::WavetableData::fromSamples(valid, 1, length));
    for (const double rate : {0.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        CHECK(! devices::WavetableData::fromSamples(valid, 1, 2048, rate));
    auto malformed = valid;
    malformed[15] = std::numeric_limits<float>::quiet_NaN();
    CHECK(! devices::WavetableData::fromSamples(malformed, 1));
    malformed[15] = 33;
    CHECK(! devices::WavetableData::fromSamples(malformed, 1));
    std::vector<float> excessive(static_cast<size_t>(2048) * (devices::WavetableData::maxFrames + 1), 0);
    CHECK(! devices::WavetableData::fromSamples(excessive, 1));
    // Maximum frame/channel bounds are accepted at a smaller test frame length, with a finite bank budget.
    const std::vector<float> maximum(static_cast<size_t>(64) * devices::WavetableData::maxFrames * 2, 0.25f);
    const auto bounded = devices::WavetableData::fromSamples(maximum, 2, 64);
    REQUIRE(bounded);
    CHECK(bounded.data->frames() == devices::WavetableData::maxFrames);
    CHECK_NEAR(bounded.data->sample(0.73f, 0.11, bounded.data->mipLevels() - 1, 1), 0.25, 2e-6);
}

TEST_CASE("wavetable: custom FFT mip levels remove harmonics above the playback band while retaining the fundamental")
{
    std::vector<float> source(2048);
    for (size_t sample = 0; sample < source.size(); ++sample)
    {
        const double phase = 2 * dsp::kPi * static_cast<double>(sample) / static_cast<double>(source.size());
        source[sample] = static_cast<float>(std::sin(phase) + 0.5 * std::sin(63 * phase));
    }
    const auto result = devices::WavetableData::fromSamples(source, 1);
    REQUIRE(result);
    const int mip = result.data->mipForIncrement(0.01);
    CHECK(mip > 0);
    CHECK_NEAR(result.data->sample(0, 0.25, 0), 0.5, 2e-5);
    CHECK_NEAR(result.data->sample(0, 0.25, mip), 1, 2e-5);
    for (int sample = 0; sample < 256; ++sample)
    {
        const double phase = static_cast<double>(sample) / 256;
        CHECK_NEAR(result.data->sample(0, phase, mip), std::sin(2 * dsp::kPi * phase), 2e-5);
    }
    CHECK(result.data->mipForIncrement(std::numeric_limits<double>::infinity()) == result.data->mipLevels() - 1);
    CHECK_NEAR(result.data->sample(0, std::numeric_limits<double>::quiet_NaN(), 0), 0, 0);
}

TEST_CASE("wavetable: external own WAV tables load with stereo frame boundaries and malformed files fail nonfatally")
{
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_media");
    REQUIRE(directory.createDirectory().wasOk());
    const auto file = directory.getChildFile("Independent Stereo.wav");
    writeOwnTable(file, ownStereoTable(), 2);
    const auto loaded = devices::WavetableData::fromFile(file);
    REQUIRE(loaded);
    CHECK(loaded.data->name() == "Independent Stereo");
    CHECK(loaded.data->frames() == 2);
    CHECK(loaded.data->channels() == 2);
    CHECK_NEAR(loaded.data->sample(0, 0.25, 0, 0), 0.75, 2e-5);
    CHECK_NEAR(loaded.data->sample(1, 0, 0, 1), 0.1875, 2e-5);
    const auto incomplete = directory.getChildFile("Incomplete.wav");
    writeOwnTable(incomplete, std::vector<float>(2047, 0.25f), 1);
    CHECK(! devices::WavetableData::fromFile(incomplete));
    const auto malformed = directory.getChildFile("Malformed.wav");
    REQUIRE(malformed.replaceWithText("independent invalid audio fixture"));
    CHECK(! devices::WavetableData::fromFile(malformed));
    CHECK(! devices::WavetableData::fromFile(directory.getChildFile("Missing.wav")));
}

TEST_CASE("wavetable: all oscillators use immutable custom stereo banks without callback ownership changes")
{
    const auto table = devices::WavetableData::fromSamples(ownStereoTable(), 2, 2048, 44100, "Own stereo table");
    REQUIRE(table);
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        W synth;
        isolated(synth);
        for (int o = 0; o < W::kOscillators; ++o) synth.setParameter(W::oscParam(o, W::On), o == oscillator ? 1.0f : 0.0f);
        REQUIRE(synth.setCustomTable(oscillator, table.data));
        synth.setParameter(W::oscParam(oscillator, W::Position), 0);
        CHECK_NEAR(synth.getParameter(W::oscParam(oscillator, W::Table)), W::Custom, 0);
        CHECK(synth.customTable(oscillator) == table.data.get());
        CHECK(synth.customTableName(oscillator) == "Own stereo table");
        CHECK_NEAR(synth.waveformAt(oscillator, 0.25f, 0), 1, 2e-6);
        CHECK_NEAR(synth.waveformAt(oscillator, 0.25f, 1), 0, 2e-6);
        CHECK_NEAR(synth.waveformAt(oscillator, 0.25f), 0.5, 2e-6);
        const auto references = table.data.use_count();
        const auto audio = render(synth, note());
        CHECK(table.data.use_count() == references);
        CHECK(audio.getRMSLevel(0, 100, 1800) > 0.01f);
        CHECK(audio.getRMSLevel(1, 100, 1800) > 0.01f);
        double stereoDifference = 0;
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            stereoDifference += std::abs(audio.getSample(0, sample) - audio.getSample(1, sample));
        CHECK(stereoDifference > 0.1);
        finiteAndBounded(audio);
        synth.reset();
        CHECK(synth.customTable(oscillator) == table.data.get());
        CHECK(! synth.setCustomTable(-1, table.data));
        REQUIRE(synth.setCustomTable(oscillator, {}));
        CHECK(synth.customTable(oscillator) == nullptr);
        CHECK_NEAR(synth.getParameter(W::oscParam(oscillator, W::Table)), W::Basic, 0);
    }
}

TEST_CASE("wavetable: arbitrary prepared LFO curves and custom stereo media keep uneven-block timing invariant")
{
    const auto table = devices::WavetableData::fromSamples(ownStereoTable(), 2);
    REQUIRE(table);
    dsp::CurveGraph curve;
    curve.count = 3;
    curve.x[0] = 0; curve.x[1] = 0.3f; curve.x[2] = 1;
    curve.y[0] = 0.1f; curve.y[1] = 0.9f; curve.y[2] = 0.2f;
    curve.curve[0] = 0.25f; curve.curve[1] = -0.25f;
    W whole, split;
    for (auto* synth : {&whole, &split})
    {
        isolated(*synth, 4096);
        REQUIRE(synth->setCustomTable(0, table.data));
        synth->setParameter(W::oscParam(0, W::Position), 0.3f);
        synth->setParameter(W::oscParam(0, W::Unison), 5);
        REQUIRE(synth->setLfoCurve(9, curve));
        CHECK(synth->hasCustomLfoCurve(9));
        CHECK_NEAR(synth->getParameter(W::lfoParam(9, W::Shape)), W::CustomLfoShape, 0);
        CHECK(synth->lfoCurve(9).count == curve.count);
        synth->setParameter(W::lfoParam(9, W::Shape), 2);
        CHECK_NEAR(synth->lfoCurve(9).y[0], dsp::CurveGraph::preset(2).y[0], 0);
        synth->setParameter(W::lfoParam(9, W::Shape), W::CustomLfoShape);
        synth->setParameter(W::lfoParam(9, W::SyncOn), 0);
        synth->setParameter(W::lfoParam(9, W::Rate), 5.25f);
        route(*synth, 63, W::Lfo10, W::Osc1Position, 0.25f);
        auto invalid = curve;
        invalid.y[1] = std::numeric_limits<float>::quiet_NaN();
        CHECK(! synth->setLfoCurve(9, invalid));
        CHECK_NEAR(synth->lfoCurve(9).y[1], curve.y[1], 0);
        CHECK(! synth->setLfoCurve(10, curve));
        synth->prepare(sampleRate, 4096);
        CHECK(synth->hasCustomLfoCurve(9));
    }
    const auto a = render(whole, note(2, 63, 17, 3500), 4096, 4096);
    const auto b = render(split, note(2, 63, 17, 3500), 4096, 97);
    CHECK_NEAR(difference(a, b), 0, 2e-6);
    finiteAndBounded(a);
    split.clearLfoCurve(9);
    CHECK(! split.hasCustomLfoCurve(9));
    CHECK_NEAR(split.getParameter(W::lfoParam(9, W::Shape)), 1, 0);
    CHECK(split.lfoCurve(9).count > 0);
}


TEST_CASE("wavetable: all measured LFO sync bins preserve legacy choices and enabled timing")
{
    using D = dsp::LfoTempoDivisions;
    constexpr std::array<double, 13> legacy{16, 8, 4, 2, 1, .5, .25, .125, .0625, 1.0/3, 2.0/3, 1.5, 3};
    std::set<int> unique;
    for (int native = 0; native < D::count; ++native)
    {
        const int bin = D::binFor(native); unique.insert(bin);
        CHECK(D::nativeIndex(bin) == native); CHECK(! D::label(native).empty());
        const double u = static_cast<double>(bin) / 42;
        const double stored = 100 * u * u * u * u;
        CHECK(D::fromStoredRate(stored, true, true) == native);
        const double base = std::ldexp(128.0, -(bin / 3));
        CHECK_NEAR(W::divisionBeats(native), base * (bin % 3 == 0 ? 1 : bin % 3 == 1 ? .75 : 2.0/3), 1e-12);
        for (const bool dotted : {false, true}) for (const bool triplets : {false, true})
        {
            int expected = bin;
            while (expected > 0 && ((expected % 3 == 1 && ! dotted) || (expected % 3 == 2 && ! triplets))) --expected;
            CHECK(D::binFor(D::fromStoredRate(stored, dotted, triplets)) == expected);
        }
    }
    CHECK(unique.size() == 43);
    for (int i = 0; i < 13; ++i) CHECK_NEAR(W::divisionBeats(i), legacy[static_cast<size_t>(i)], 1e-12);
    for (int bin = 0; bin < 42; ++bin)
    {
        const double transition = (bin + .5) / 42;
        for (const double offset : {-1e-7, 1e-7})
        {
            const double u = transition + offset;
            CHECK(D::binFor(D::fromStoredRate(100*u*u*u*u, true, true)) == bin + (offset > 0 ? 1 : 0));
        }
    }
    CHECK_NEAR(W::divisionBeats(D::nativeIndex(42)), 1.0/128, 0);
}

TEST_CASE("wavetable: matrix source curve via curve bipolar and Out match static imported pitch")
{
    dsp::CurveGraph main = dsp::CurveGraph::preset(3), via = main;
    main.curve[0] = -.5f; via.curve[0] = .5f;
    for (const bool bipolar : {false, true})
    {
        W shaped, reference; isolated(shaped); isolated(reference);
        REQUIRE(shaped.setImportedModulationDomain(W::Osc1Pitch, W::ImportedModulationDomain::CoarsePitch128));
        REQUIRE(shaped.setModulationCurve(63, false, main, true, W::CurveLaw::ImportedExponential));
        REQUIRE(shaped.setModulationCurve(63, true, via, true, W::CurveLaw::ImportedExponential));
        CHECK_NEAR(shaped.modulationCurveAt(63, false, .5f), .9375, 1e-6);
        CHECK_NEAR(shaped.modulationCurveAt(63, true, .5f), .0625, 1e-6);
        shaped.setParameter(W::macroParam(0), .5f); shaped.setParameter(W::macroParam(1), .5f);
        route(shaped, 63, W::Macro1, W::Osc1Pitch, .25f);
        shaped.setParameter(W::modParam(63, W::AuxSource), W::Macro2);
        shaped.setParameter(W::modParam(63, W::ModBipolar), bipolar ? 1 : 0);
        shaped.setParameter(W::outParam(63), .5f);
        const float pitch = 128 * .25f * (.9375f - (bipolar ? .5f : 0)) * .0625f * .5f;
        reference.setParameter(W::coarseParam(0), pitch);
        CHECK_NEAR(difference(render(shaped, note()), render(reference, note())), 0, 1e-6);
        shaped.setParameter(W::outParam(63), 0); reference.setParameter(W::coarseParam(0), 0);
        shaped.reset(); reference.reset();
        CHECK_NEAR(difference(render(shaped, note()), render(reference, note())), 0, 1e-7);
        main.x[1] = .9f;
        CHECK(! shaped.setModulationCurve(63, false, main, true, W::CurveLaw::NativePower));
        CHECK(shaped.modulationCurve(63, false).law == W::CurveLaw::ImportedExponential);
        main.x[1] = 1;
    }
}


TEST_CASE("wavetable: imported Free LFO voices retain independent clocks across overlap and host jumps")
{
    const auto capture = [](W& synth, const juce::MidiBuffer& events, double jump) {
        juce::AudioBuffer<float> output(2, 48000); output.clear(); juce::MidiBuffer midi; midi.ensureSize(256);
        for (int start = 0; start < output.getNumSamples(); start += 512)
        {
            const int count = std::min(512, output.getNumSamples() - start);
            juce::AudioBuffer<float> block(output.getArrayOfWritePointers(), 2, start, count);
            midi.clear(); midi.addEvents(events, start, count, -start);
            synth.setHostInfo({120, static_cast<double>(start) / 24000 + (start >= 12288 ? jump : 0), true, sampleRate});
            synth.process(block, midi);
        }
        return output;
    };
    const auto configure = [](W& synth, bool synced, bool anchored) {
        isolated(synth); synth.setImportedLfoProfile(true);
        dsp::CurveGraph graph; graph.count = 3;
        graph.x[0] = 0; graph.x[1] = .9999f; graph.x[2] = 1; graph.y[1] = 1;
        REQUIRE(synth.setLfoCurve(0, graph));
        synth.setParameter(W::lfoParam(0, W::Mode), W::Global);
        synth.setParameter(W::lfoParam(0, W::SyncOn), synced ? 1 : 0);
        synth.setParameter(W::lfoAnchorParam(0), anchored ? 1 : 0);
        synth.setParameter(W::lfoParam(0, W::Bipolar), 0); synth.setParameter(W::lfoParam(0, W::Rate), 1);
        synth.setParameter(W::oscParam(0, W::Level), 0);
        REQUIRE(synth.setImportedModulationDomain(W::Osc1Level, W::ImportedModulationDomain::SquaredLevel));
        route(synth, 0, W::Lfo1, W::Osc1Level, 1);
    };
    for (const bool synced : {false, true}) for (const bool anchored : {false, true})
    {
        W combined, first, second, firstBaseline, secondBaseline;
        for (auto* synth : {&combined, &first, &second, &firstBaseline, &secondBaseline}) configure(*synth, synced, anchored);
        const auto firstNote = note(1, 60, 17), secondNote = note(1, 72, 36000);
        auto events = firstNote; events.addEvents(secondNote, 0, 48000, 0);
        const auto actual = capture(combined, events, .5), one = capture(first, firstNote, .5), two = capture(second, secondNote, .5);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 36000; sample < 48000; ++sample)
                CHECK_NEAR(actual.getSample(channel, sample), one.getSample(channel, sample) + two.getSample(channel, sample), 1e-7);
        const auto plainOne = capture(firstBaseline, firstNote, 0), plainTwo = capture(secondBaseline, secondNote, 0);
        if (synced && anchored) CHECK(difference(one, plainOne, 14000) > .001);
        else CHECK_NEAR(difference(one, plainOne), 0, 1e-10);
        if (synced || anchored) CHECK(difference(two, plainTwo, 38000) > .001);
        else CHECK_NEAR(difference(two, plainTwo), 0, 1e-10);
    }
}

TEST_CASE("wavetable: native mono fallback duplicate policy global sustain and envelope legato are audible")
{
    W mono; isolated(mono); mono.setImportedVoiceProfile(true); mono.setParameter(W::voiceModeParam(W::MonoOn), 1);
    auto repeated = note(2, 69, 17, 4096); repeated.addEvent(juce::MidiMessage::noteOn(2, 69, static_cast<juce::uint8>(110)), 2048);
    const auto duplicate = render(mono, repeated, 8192);
    CHECK(mono.activeVoiceCount() == 0); CHECK_NEAR(duplicate.getMagnitude(4096, 4096), 0, 0);
    W sustained; isolated(sustained); sustained.setImportedVoiceProfile(true); sustained.setParameter(W::voiceModeParam(W::MonoOn), 1);
    auto pedal = note(2, 69, 17, 4096);
    pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 100);
    pedal.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 6000);
    const auto held = render(sustained, pedal, 8192);
    CHECK(held.getRMSLevel(0, 4500, 1000) > .05); CHECK_NEAR(held.getMagnitude(6000, 2192), 0, 0);
    for (const bool legato : {false, true})
    {
        W shaped, carrier;
        for (auto* synth : {&shaped, &carrier})
        {
            isolated(*synth); synth->setImportedVoiceProfile(true); synth->setImportedEnvelopeProfile(true);
            synth->setParameter(W::voiceModeParam(W::MonoOn), 1); synth->setParameter(W::voiceModeParam(W::LegatoOn), legato ? 1 : 0);
        }
        shaped.setParameter(W::envParam(0, W::Attack), .1f); shaped.setParameter(W::envParam(0, W::Sustain), .5f);
        shaped.setParameter(W::envParam(0, W::AttackCurve), 0);
        auto events = note(1, 69, 17); events.addEvent(juce::MidiMessage::noteOn(1, 81, static_cast<juce::uint8>(110)), 36000);
        const auto actual = render(shaped, events, 48000), reference = render(carrier, events, 48000);
        const int sample = 37200; REQUIRE(std::abs(reference.getSample(0, sample)) > .01);
        CHECK_NEAR(actual.getSample(0, sample) / reference.getSample(0, sample), legato ? .25 : .43765625, .002);
        CHECK(shaped.activeVoiceCount() == 1);
    }
}

TEST_CASE("wavetable: imported unison follows the measured Serum law for detune spacing, level classes, pair panning and width")
{
    // Sine table, fixed phases. Magnitude of the voice at `cents` from 440 Hz, Hann-windowed.
    const auto magnitudeAt = [](const juce::AudioBuffer<float>& audio, int channel, double cents, int start, int count) {
        const double frequency = 440.0 * std::pow(2.0, cents / 1200.0);
        double re = 0, im = 0, window = 0;
        for (int i = 0; i < count; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / count), phase = 2 * 3.14159265358979 * frequency * (start + i) / sampleRate;
            const double v = audio.getSample(channel, start + i) * w;
            re += v * std::cos(phase); im -= v * std::sin(phase); window += w;
        }
        return 2 * std::hypot(re, im) / window;
    };
    const auto run = [&](int count, float width, float blend) {
        W synth; isolated(synth); synth.setImportedVoiceProfile(true);
        synth.setParameter(W::oscParam(0, W::Unison), static_cast<float>(count));
        synth.setParameter(W::oscParam(0, W::Detune), 40.0f);
        synth.setParameter(W::oscParam(0, W::Width), width);
        synth.setParameter(W::oscParam(0, W::Blend), blend);
        return render(synth, note(1, 69, 17), 65536);
    };
    // One voice is the reference level.
    const auto single = run(1, 1.0f, 0.75f);
    const double reference = magnitudeAt(single, 0, 0, 24000, 32768);
    REQUIRE(reference > 0.01);
    // Three voices, mono width: centre-class 0.3016, sides 0.2346 of a single voice, all voices in both channels
    const auto three = run(3, 0.0f, 0.75f);
    CHECK_NEAR(magnitudeAt(three, 0, 0, 24000, 32768) / reference, 0.3016, 0.004);
    CHECK_NEAR(magnitudeAt(three, 1, 40, 24000, 32768) / reference, 0.2346, 0.004);
    CHECK_NEAR(magnitudeAt(three, 0, -40, 24000, 32768) / reference, 0.2346, 0.004);
    // Full width: the low voice is hard right, the high one hard left (pair 0), at (1 + 1)(1 + 0.2615) of the mono level; the centre voice stays in both
    const auto wide = run(3, 1.0f, 0.75f);
    CHECK_NEAR(magnitudeAt(wide, 0, 40, 24000, 32768) / reference, 0.2346 * 2 * 1.2615, 0.006);
    CHECK(magnitudeAt(wide, 1, 40, 24000, 32768) / reference < 0.004);
    CHECK_NEAR(magnitudeAt(wide, 1, -40, 24000, 32768) / reference, 0.2346 * 2 * 1.2615, 0.006);
    CHECK_NEAR(magnitudeAt(wide, 0, 0, 24000, 32768) / reference, 0.3016, 0.004);
    CHECK_NEAR(magnitudeAt(wide, 1, 0, 24000, 32768) / reference, 0.3016, 0.004);
    // Four voices: offsets are linear (+-40, +-13.33 cents); the second pair swaps sides
    const auto four = run(4, 1.0f, 0.75f);
    CHECK(magnitudeAt(four, 0, 40, 24000, 32768) > 10 * magnitudeAt(four, 1, 40, 24000, 32768));
    CHECK(magnitudeAt(four, 1, 13.3333, 24000, 32768) > 10 * magnitudeAt(four, 0, 13.3333, 24000, 32768));
    // Blend 1 lowers the centre class below the sides (ratio 0.75), blend 0 leaves almost only the centre
    const auto top = run(3, 0.0f, 1.0f), bottom = run(3, 0.0f, 0.0f);
    CHECK_NEAR(magnitudeAt(top, 0, 0, 24000, 32768) / magnitudeAt(top, 0, 40, 24000, 32768), 0.7507, 0.02);
    CHECK(magnitudeAt(bottom, 0, 0, 24000, 32768) > 15 * magnitudeAt(bottom, 0, 40, 24000, 32768));
}

TEST_CASE("wavetable: Self PD feeds each voice's own previous table value back into its phase at 0.6 a^2 cycles")
{
    std::vector<float> sine(2048);
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(2 * M_PI * static_cast<double>(i) / 2048));
    const auto table = devices::WavetableData::fromSamples(sine, 1, 2048, 44100, "Own sine", devices::WavetableData::PlaybackProfile::Octave16Approx);
    REQUIRE(table);
    const auto harmonics = [&](float amount) {
        W synth; isolated(synth);
        REQUIRE(synth.setCustomTable(0, table.data));
        synth.setParameter(W::oscParam(0, W::Table), W::Custom);
        synth.setParameter(W::oscParam(0, W::Warp1Mode), W::SelfPd);
        synth.setParameter(W::oscParam(0, W::Warp1Amount), amount);
        const auto audio = render(synth, note(), 24000);
        std::array<double, 2> magnitude{};
        for (size_t k = 0; k < magnitude.size(); ++k)
        {
            std::complex<double> sum;
            for (int n = 12000; n < 24000; ++n) // exactly 110 cycles of 440 Hz
                sum += static_cast<double>(audio.getSample(0, n)) * std::polar(1.0, -2 * M_PI * 440.0 * static_cast<double>(k + 1) * n / sampleRate);
            magnitude[k] = std::abs(sum);
        }
        return magnitude;
    };
    const auto off = harmonics(0), weak = harmonics(0.2f);
    CHECK(off[1] < off[0] * 1e-3);
    // sin(theta + b y[n-1]) ~ sin(theta) + (b / 2) sin(2 theta) for small b = 2 pi 0.6 a^2.
    CHECK_NEAR(weak[1] / weak[0], M_PI * 0.6 * 0.04, 0.003);
}

TEST_CASE("wavetable: imported start phases follow the reference mt19937_64 stream (shared slot 0, 407 draws per note)")
{
    std::vector<float> sine(2048);
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(2 * M_PI * static_cast<double>(i) / 2048));
    const auto table = devices::WavetableData::fromSamples(sine, 1, 2048, 44100, "Own sine", devices::WavetableData::PlaybackProfile::Octave16Approx);
    REQUIRE(table);
    // Two notes; the phase of each note relative to the Random 0 render must be the measured draw: #320 for the first note, +407 per note.
    const auto phases = [&](float random) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true);
        REQUIRE(synth.setCustomTable(0, table.data));
        synth.setParameter(W::oscParam(0, W::Table), W::Custom);
        synth.setParameter(W::oscParam(0, W::Phase), 0.0f);
        synth.setParameter(W::oscParam(0, W::Random), random);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOff(1, 69), 14000);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 24000);
        const auto audio = render(synth, midi, 40000);
        std::array<double, 2> result{};
        for (size_t note = 0; note < result.size(); ++note)
        {
            std::complex<double> sum;
            const int from = static_cast<int>(note) * 24000 + 1200;
            for (int n = from; n < from + 12000; ++n) // 110 cycles of 440 Hz
                sum += static_cast<double>(audio.getSample(0, n)) * std::polar(1.0, -2 * M_PI * 440.0 * n / sampleRate);
            result[note] = std::arg(sum) / (2 * M_PI);
        }
        return result;
    };
    std::mt19937_64 reference(5489);
    reference.discard(320);
    const double first = static_cast<float>(reference()) * 5.421011e-20f;
    reference.discard(406);
    const double second = static_cast<float>(reference()) * 5.421011e-20f;
    const auto fixed = phases(0), random = phases(1);
    const auto offset = [&](size_t note) { const double d = random[note] - fixed[note]; return d - std::floor(d); };
    const auto near = [](double a, double b) { const double d = std::abs(a - b); return std::min(d, 1 - d); };
    CHECK(near(offset(0), first) < 1e-4);
    CHECK(near(offset(1), second) < 1e-4);
}

TEST_CASE("wavetable: imported note sources follow the reference stream, alternate per note and publish Discrete one start late")
{
    // Each note's pitch carries one source through a 12-semitone route; the measured frequency gives the source value per note.
    const auto values = [](W::Source source, int slot, const std::vector<std::pair<int, int>>& notes, const std::vector<std::pair<int, int>>& windows) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true);
        route(synth, slot, source, W::Pitch, 1.0f);
        juce::MidiBuffer midi;
        for (const auto& [at, pitch] : notes) midi.addEvent(pitch > 0 ? juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(100)) : juce::MidiMessage::noteOff(1, -pitch), at);
        const auto audio = render(synth, midi, 72000);
        std::vector<double> out;
        for (const auto& [from, pitch] : windows)
        {
            // Zero crossings over 9600 samples estimate the frequency of the only (or the highest-pitched) sounding voice.
            int crossings = 0; double first = -1, last = -1;
            for (int n = from; n < from + 9600; ++n)
                if (audio.getSample(0, n - 1) <= 0 && audio.getSample(0, n) > 0)
                {
                    const double frac = audio.getSample(0, n - 1) / (audio.getSample(0, n - 1) - audio.getSample(0, n));
                    const double at = n - 1 + frac;
                    if (first < 0) first = at; last = at; ++crossings;
                }
            const double frequency = (crossings - 1) * sampleRate / (last - first);
            out.push_back(12 * std::log2(frequency / (440.0 * std::exp2((pitch - 69) / 12.0))));
        }
        return out;
    };
    const auto draw = [&](int index) { std::mt19937_64 g(5489); g.discard(static_cast<unsigned long long>(index)); return static_cast<float>(g()) * 5.421011e-20f; };
    // Sequential notes: note starts consume 404 draws, each release 3: u = 320, 727, 1134.
    const std::vector<std::pair<int, int>> sequential{{0, 69}, {10000, -69}, {20000, 69}, {30000, -69}, {40000, 69}, {50000, -69}};
    const std::vector<std::pair<int, int>> windows{{200, 69}, {20200, 69}, {40200, 69}};
    const auto rand1 = values(W::NoteRandom, 0, sequential, windows), rand2 = values(W::NoteRandom2, 0, sequential, windows);
    const auto alternate = values(W::NoteAlternate, 0, sequential, windows), alternate2 = values(W::NoteAlternate2, 0, sequential, windows);
    const auto discrete = values(W::NoteRandomDiscrete, 5, sequential, windows);
    const double scale = rand1[0] / draw(321); // semitones per unit source
    CHECK(scale > 0.5);
    for (size_t note = 0; note < 3; ++note)
    {
        const int u = 320 + 407 * static_cast<int>(note);
        CHECK_NEAR(rand1[note], scale * draw(u + 1), 0.01);
        CHECK_NEAR(rand2[note], scale * draw(u + 2), 0.01);
        CHECK_NEAR(alternate[note], note % 2 == 0 ? scale : 0, 0.01);
        CHECK_NEAR(alternate2[note], note % 2 == 0 ? 0 : scale, 0.01);
        CHECK_NEAR(discrete[note], note == 0 ? 0 : scale * draw(u - 407 + 3 + 5), 0.01);
    }
}

TEST_CASE("wavetable: imported same-note limit cuts the old voice with a 20 ms release and starts a fresh voice")
{
    const auto render2 = [](bool limit, bool both) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true);
        synth.setImportedSameNoteLimit(limit);
        synth.setParameter(W::envParam(0, W::Release), 0.5f);
        juce::MidiBuffer midi;
        if (both) midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 6000);
        return render(synth, midi, 12000);
    };
    const auto cut = render2(true, true), alone = render2(true, false), layered = render2(false, true);
    double afterFade = 0, duringFade = 0, layeredDifference = 0;
    for (int n = 6000; n < 12000; ++n)
    {
        const double d = std::abs(cut.getSample(0, n) - alone.getSample(0, n));
        if (n >= 6000 + 1000) afterFade = std::max(afterFade, d); else duringFade = std::max(duringFade, d);
        if (n >= 7000) layeredDifference = std::max(layeredDifference, static_cast<double>(std::abs(layered.getSample(0, n) - alone.getSample(0, n))));
    }
    CHECK(duringFade > 1e-3);       // the old voice is still fading during the first 20 ms
    CHECK(afterFade < 1e-6);        // then only the fresh voice remains
    CHECK(layeredDifference > 1e-2); // without the limit both voices keep sounding
}

TEST_CASE("wavetable: imported mono retrigger keeps phases and consumes the measured reference draws")
{
    const auto draw = [](int index) { std::mt19937_64 g(5489); g.discard(static_cast<unsigned long long>(index)); return static_cast<float>(g()) * 5.421011e-20f; };
    // NoteOn Rand1 -> pitch; the last note's frequency gives its Rand1, i.e. which draw it received.
    const auto lastValue = [](bool legato) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true);
        synth.setParameter(W::voiceModeParam(W::MonoOn), 1);
        synth.setParameter(W::voiceModeParam(W::LegatoOn), legato ? 1.0f : 0.0f);
        route(synth, 0, W::NoteRandom, W::Pitch, 1.0f);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 71, static_cast<juce::uint8>(100)), 10000); // overlap: retrigger or legato
        midi.addEvent(juce::MidiMessage::noteOff(1, 69), 20000);                                 // not sounding: no draws
        midi.addEvent(juce::MidiMessage::noteOff(1, 71), 30000);                                 // release: 3 draws
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 40000);
        const auto audio = render(synth, midi, 52000);
        std::vector<double> semitones;
        for (const int from : {200, 40200})
        {
            int crossings = 0; double first = -1, last = -1;
            for (int n = from; n < from + 9600; ++n)
                if (audio.getSample(0, n - 1) <= 0 && audio.getSample(0, n) > 0)
                {
                    const double at = static_cast<double>(n - 1) + static_cast<double>(audio.getSample(0, n - 1) / (audio.getSample(0, n - 1) - audio.getSample(0, n)));
                    if (first < 0) first = at; last = at; ++crossings;
                }
            semitones.push_back(12 * std::log2((crossings - 1) * sampleRate / (last - first) / 440.0));
        }
        return semitones;
    };
    for (const bool legato : {false, true})
    {
        const auto values = lastValue(legato);
        const double scale = values[0] / draw(321);
        CHECK(scale > 0.5);
        // First note u = 320 (404 draws); retrigger 118 or legato 4; release 3; the next note's Rand1 is u + 1.
        CHECK_NEAR(values[1], scale * draw(legato ? 732 : 846), 0.01);
    }
}

TEST_CASE("wavetable: imported polyphony steals in place and the pedal restarts a struck pitch in its own voice")
{
    const auto level = [](const juce::AudioBuffer<float>& audio, double frequency, int from, int count) {
        std::complex<double> sum;
        for (int n = from; n < from + count; ++n) sum += static_cast<double>(audio.getSample(0, n)) * std::polar(1.0, -2 * M_PI * frequency * n / sampleRate);
        return 2 * std::abs(sum) / count;
    };
    const auto frequency = [](int note) { return 440.0 * std::exp2((note - 69) / 12.0); };
    {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true); synth.setImportedPolyphony(2);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)), 2000);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 4000); // steals 57 (the oldest held voice)
        const auto audio = render(synth, midi, 24000);
        CHECK(level(audio, frequency(57), 12000, 9600) < 1e-3);
        CHECK(level(audio, frequency(64), 12000, 9600) > 0.05);
        CHECK(level(audio, frequency(69), 12000, 9600) > 0.05);
    }
    const auto restrike = [&](bool pedal) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true);
        juce::MidiBuffer midi;
        if (pedal) midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 69, static_cast<juce::uint8>(100)), 1050); // held pitch struck again
        return level(render(synth, midi, 24000), 440.0, 12000, 9600);
    };
    W single; isolated(single); single.setImportedVoiceProfile(true);
    const double one = level(render(single, note(), 24000), 440.0, 12000, 9600);
    CHECK_NEAR(restrike(true), one, one * 0.01);      // one voice: restarted in place
    CHECK(std::abs(restrike(false) - one) > one * 0.05); // two layered voices of the same pitch
}

TEST_CASE("wavetable: imported unit and envelope-time modulation domains equal the directly set value")
{
    // A macro route in the measured normalized domain must render exactly like the target set to base + macro x amount.
    const auto renderWith = [](auto configure) {
        W synth; isolated(synth);
        synth.setImportedVoiceProfile(true); synth.setImportedEnvelopeProfile(true);
        configure(synth);
        return render(synth, note(), 12000);
    };
    {
        const auto routed = renderWith([](W& synth) {
            synth.setParameter(W::oscParam(0, W::Warp1Mode), W::Sync); synth.setParameter(W::oscParam(0, W::Warp1Amount), 0.2f);
            synth.setImportedModulationDomain(W::Osc1Warp1, W::ImportedModulationDomain::Unit);
            synth.setParameter(W::macroParam(0), 0.6f); route(synth, 0, W::Macro1, W::Osc1Warp1, 0.4f);
        });
        const auto direct = renderWith([](W& synth) {
            synth.setParameter(W::oscParam(0, W::Warp1Mode), W::Sync); synth.setParameter(W::oscParam(0, W::Warp1Amount), 0.44f);
        });
        CHECK(difference(routed, direct) < 1e-5);
    }
    {
        const float base = 0.05f, amount = 0.3f, macro = 0.5f;
        const float direct = 32 * std::pow(std::pow(base / 32, 0.2f) + macro * amount, 5.0f); // seconds = 32 u^5
        const auto routed = renderWith([&](W& synth) {
            synth.setParameter(W::envParam(0, W::Attack), base);
            synth.setImportedModulationDomain(W::Env1Attack, W::ImportedModulationDomain::EnvelopeTime);
            synth.setParameter(W::macroParam(0), macro); route(synth, 0, W::Macro1, W::Env1Attack, amount);
        });
        const auto set = renderWith([&](W& synth) { synth.setParameter(W::envParam(0, W::Attack), direct); });
        CHECK(difference(routed, set) < 1e-5);
    }
}
