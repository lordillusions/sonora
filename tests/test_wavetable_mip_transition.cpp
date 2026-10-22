#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/WavetableMipTransition.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
using W = sonora::devices::WavetableSynth;
using Table = sonora::devices::WavetableData;
constexpr double pi = 3.14159265358979323846;
void prepareMipFixture(W& synth, const std::shared_ptr<const Table>& table)
{
    synth.setCustomTable(0, table); synth.prepare(48000, 512);
    synth.setParameter(W::Volume, 0); synth.setParameter(W::SubOn, 0); synth.setParameter(W::NoiseOn, 0);
    for (int oscillator = 0; oscillator < 3; ++oscillator)
        synth.setParameter(W::oscParam(oscillator, W::On), oscillator == 0 ? 1.0f : 0.0f);
    synth.setParameter(W::oscParam(0, W::Table), W::Custom);
    synth.setParameter(W::oscParam(0, W::Level), 1); synth.setParameter(W::oscParam(0, W::Phase), .25f);
    synth.setParameter(W::oscParam(0, W::Random), 0); synth.setParameter(W::oscParam(0, W::Unison), 1);
    for (int filter = 0; filter < 2; ++filter) synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    for (const auto parameter : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release})
        synth.setParameter(W::envParam(0, parameter), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
}
}

TEST_CASE("wavetable mip transition retains each preceding level for exactly 64 internal samples")
{
    sonora::dsp::WavetableMipTransition transition;
    transition.select(3); CHECK(! transition.active()); CHECK(transition.current() == 3);
    transition.select(4); CHECK(transition.previous() == 3);
    for (int sample = 0; sample < 64; ++sample)
    {
        transition.select(4); CHECK(transition.active());
        CHECK_NEAR(transition.blend(0.25f, -.75f), -.75 + static_cast<double>(sample) / 64, 0);
        transition.advance();
    }
    CHECK(! transition.active()); CHECK_NEAR(transition.blend(.25f, -.75f), .25, 0);
    transition.select(5); transition.advance(); transition.select(2);
    CHECK(transition.previous() == 5); CHECK(transition.current() == 2);
    CHECK_NEAR(transition.blend(0, 1), 1, 0);
    transition.reset(); transition.select(7); CHECK(! transition.active()); CHECK(transition.previous() == 7);
}

TEST_CASE("wavetable mip transition smooths an actual pitch octave switch across uneven host blocks")
{
    std::vector<float> source(2048);
    for (int sample = 0; sample < 2048; ++sample)
        source[static_cast<size_t>(sample)] = static_cast<float>(.3 * std::sin(2 * pi * sample / 2048)
            + .4 * std::sin(2 * pi * 80 * sample / 2048));
    const auto loaded = Table::fromSamples(source, 1, 2048, 48000, "Own transition fixture", Table::PlaybackProfile::Octave16Approx);
    REQUIRE(loaded); W synth; prepareMipFixture(synth, loaded.data);
    juce::AudioBuffer<float> preroll(2, 2048); preroll.clear(); juce::MidiBuffer note;
    note.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100)), 0);
    synth.process(preroll, note); synth.setParameter(W::Transpose, 12);
    juce::AudioBuffer<float> changed(2, 256); changed.clear(); juce::MidiBuffer empty;
    int from = 0;
    for (const int length : {7, 23, 1, 33, 192})
    {
        juce::AudioBuffer<float> block(changed.getArrayOfWritePointers(), 2, from, length);
        synth.process(block, empty); from += length;
    }
    const double originalIncrement = 440 * std::exp2((48.0 - 69) / 12) / 48000;
    double phase = .25 + 2048 * originalIncrement; phase -= std::floor(phase);
    double residual = 0, differenceFromAbrupt = 0;
    for (int sample = 0; sample < 256; ++sample)
    {
        const float wrapped = static_cast<float>(phase);
        const float current = loaded.data->sample(0, wrapped, 4), preceding = loaded.data->sample(0, wrapped, 3);
        const float weight = static_cast<float>(std::max(0, 64 - sample)) / 64;
        const float expected = current + weight * (preceding - current);
        for (int channel = 0; channel < 2; ++channel)
            residual = std::max(residual, std::abs(static_cast<double>(changed.getSample(channel, sample) - expected)));
        if (sample < 64) differenceFromAbrupt += std::abs(expected - current);
        phase += originalIncrement * 2; phase -= std::floor(phase);
    }
    CHECK_NEAR(residual, 0, 2e-5); CHECK(differenceFromAbrupt > 1);
    synth.reset(); juce::AudioBuffer<float> restarted(2, 256); restarted.clear(); synth.process(restarted, note);
    W fresh; prepareMipFixture(fresh, loaded.data); fresh.setParameter(W::Transpose, 12);
    juce::AudioBuffer<float> expectedRestart(2, 256); expectedRestart.clear(); fresh.process(expectedRestart, note);
    for (int sample = 0; sample < 256; ++sample)
        CHECK_NEAR(restarted.getSample(0, sample), expectedRestart.getSample(0, sample), 0);
}
