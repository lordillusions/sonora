#include "TestFramework.h"
#include "engine/TempoAnalysis.h"
#include <cmath>
#include <limits>
#include <numbers>
#include <random>
using namespace sonora;

namespace
{
constexpr double kRate = 44100;

// A drum-like hit: a decaying tone plus a noise burst, added at `seconds`.
void addHit(juce::AudioBuffer<float>& audio, double seconds, float gain, double frequency, std::mt19937& random)
{
    std::uniform_real_distribution<float> noise(-1, 1);
    const int start = (int) std::lround(seconds * kRate);
    for (int i = 0; i < 6000 && start + i < audio.getNumSamples(); ++i)
    {
        if (start + i < 0) continue;
        const double t = i / kRate;
        const float v = gain * (float) (std::exp(-t * 30) * (0.7 * std::sin(2 * std::numbers::pi * frequency * t) + 0.3 * noise(random)));
        for (int c = 0; c < audio.getNumChannels(); ++c) audio.addSample(c, start + i, v);
    }
}

juce::AudioBuffer<float> silence(double seconds) { juce::AudioBuffer<float> audio(2, (int) std::lround(seconds * kRate)); audio.clear(); return audio; }

// `beats` beats at `bpm`, exactly that long (a loop), with a hit on each listed sixteenth of every bar.
juce::AudioBuffer<float> loop(double bpm, int beats, std::initializer_list<int> sixteenths, unsigned seed = 1)
{
    std::mt19937 random(seed);
    auto audio = silence(beats * 60 / bpm);
    for (int bar = 0; bar < beats / 4; ++bar)
        for (int sixteenth : sixteenths)
            addHit(audio, (bar * 16 + sixteenth) * 15 / bpm, sixteenth % 4 == 0 ? 0.8f : 0.45f, sixteenth % 4 == 0 ? 90 : 2500, random);
    return audio;
}
}

TEST_CASE("tempo analysis: exact-length loops get their tempo from the length, refined to it (UI-47b)")
{
    // Sparse and irregular: a kick on 1, a snare on 3 and a ghost on the 'a' of 2, so the attacks alone are ambiguous.
    for (int beats : {8, 16, 32})
    {
        const auto audio = loop(140, beats, {0, 7, 8, 14});
        const auto estimate = engine::estimateSampleTempo(audio, kRate, 120);
        CHECK_NEAR(estimate.bpm, beats * 60 * kRate / audio.getNumSamples(), 1e-9); // exactly the length
        CHECK_NEAR(estimate.bpm, 140, 1e-3);
        CHECK(estimate.fromLength);
        CHECK(estimate.confidence > 0);
        CHECK(estimate.firstBeat < 0.03 || estimate.firstBeat > 60 / 140.0 - 0.03); // the loop starts on the beat
    }
    // Other tempos and the set's tempo do not pull it.
    CHECK_NEAR(engine::estimateSampleTempo(loop(97, 16, {0, 4, 8, 10, 12}), kRate, 140).bpm, 97, 1e-3);
    CHECK_NEAR(engine::estimateSampleTempo(loop(174, 32, {0, 6, 10, 12}), kRate, 87).bpm, 174, 1e-3);
}

TEST_CASE("tempo analysis: octave and 3-sixteenth pulses resolve to the beat, not half or 4/3 of it (UI-47b)")
{
    // A pattern repeating every half bar (kick on 1 and 3, off-beat bass): the strongest period is 2 beats (70 BPM).
    CHECK_NEAR(engine::estimateSampleTempo(loop(140, 32, {0, 2, 3, 6, 8, 10, 11, 14}), kRate, 120).bpm, 140, 1e-3);
    // Psytrance 3-sixteenth grouping (3+3+3+3+4): the obvious pulse is 186.7 or 93.3 BPM.
    CHECK_NEAR(engine::estimateSampleTempo(loop(140, 32, {0, 3, 6, 9, 12}), kRate, 120).bpm, 140, 1e-3);
    // Straight sixteenths in a 4-beat loop: 280 / 70 are the octave errors.
    CHECK_NEAR(engine::estimateSampleTempo(loop(140, 4, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}), kRate, 70).bpm, 140, 1e-3);
}

TEST_CASE("tempo analysis: a long swung performance (not a loop) finds its tempo and first beat (UI-47b)")
{
    // 75 s of a slightly loose, swung groove at 123.5 BPM that starts 0.25 s into the file and ends with a tail.
    constexpr double bpm = 123.5, offset = 0.25;
    std::mt19937 random(7);
    std::normal_distribution<double> jitter(0, 0.004);
    auto audio = silence(75.3);
    const double beat = 60 / bpm;
    for (int i = 0; offset + i * beat < 74; ++i)
    {
        const double at = offset + i * beat;
        addHit(audio, at + jitter(random), i % 2 == 0 ? 0.9f : 0.6f, i % 2 == 0 ? 80 : 1800, random);
        addHit(audio, at + beat * 0.62 + jitter(random), 0.3f, 6000, random); // swung eighth
        if (i % 8 == 7) addHit(audio, at + beat * 0.83 + jitter(random), 0.4f, 1800, random);
    }
    const auto estimate = engine::estimateSampleTempo(audio, kRate, 100);
    CHECK_NEAR(estimate.bpm, bpm, 0.3);
    CHECK(! estimate.fromLength);
    const double phase = std::fmod(estimate.firstBeat - offset + 10 * beat, beat);
    CHECK(std::min(phase, beat - phase) < 0.03);
}

TEST_CASE("tempo analysis: one-shots, noise, silence and bad input have no tempo")
{
    std::mt19937 random(3);
    auto hit = silence(0.9);
    addHit(hit, 0, 1, 60, random);
    CHECK_NEAR(engine::estimateSampleTempo(hit, kRate, 120).bpm, 0.0, 1e-12);

    auto noise = silence(4.37);
    std::uniform_real_distribution<float> sample(-0.5f, 0.5f);
    for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample(0, i, sample(random));
    CHECK_NEAR(engine::estimateSampleTempo(noise, kRate, 120).bpm, 0.0, 1e-12);

    CHECK_NEAR(engine::estimateSampleTempo(silence(8), kRate, 120).bpm, 0.0, 1e-12);
    CHECK_NEAR(engine::estimateSampleTempo(juce::AudioBuffer<float>(2, 0), kRate, 120).bpm, 0.0, 1e-12);
    CHECK_NEAR(engine::estimateSampleTempo(loop(140, 8, {0, 4, 8, 12}), 0, 120).bpm, 0.0, 1e-12);
    CHECK_NEAR(engine::estimateSampleTempo(loop(140, 8, {0, 4, 8, 12}), std::numeric_limits<double>::quiet_NaN(), 120).bpm, 0.0, 1e-12);

    // A pad with no attacks but an exact 4-bar length at a round tempo is still taken as a loop (like Live).
    auto pad = silence(16 * 60 / 128.0);
    for (int i = 0; i < pad.getNumSamples(); ++i) pad.setSample(0, i, (float) (0.3 * std::sin(2 * std::numbers::pi * 220 * i / kRate)));
    const auto padTempo = engine::estimateSampleTempo(pad, kRate, 120);
    CHECK_NEAR(padTempo.bpm, 128, 1e-3);
    CHECK(padTempo.fromLength);
}
