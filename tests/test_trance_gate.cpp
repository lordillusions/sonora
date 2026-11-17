// PLG-27 (PLG-30 later): the trance gate. Step edges on the grid to the sample, depth, smoothing without a step, swing,
// triplet rates, factory patterns of the lengths they claim.
#include "TestFramework.h"
#include "dsp/TranceGate.h"

#include <cmath>
#include <vector>

using sonora::dsp::TranceGate;

namespace
{
std::vector<float> run(TranceGate& g, double rate, double tempo, int samples)
{
    std::vector<float> out((size_t) samples);
    const double beatsPerSample = tempo / 60.0 / rate;
    for (int i = 0; i < samples; ++i) out[(size_t) i] = g.next(i * beatsPerSample);
    return out;
}
const float kAlternate[2] = {1.0f, 0.0f};
} // namespace

TEST_CASE("trance gate: step edges fall on the grid to the sample at 44.1, 48 and 96 kHz")
{
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        TranceGate g;
        g.prepare(rate);
        g.setPattern(kAlternate, 2);
        g.setShape(TranceGate::Sixteenth, 1.0f, 1.0f, 1.0f, 0.0f);
        const double tempo = 138.0, beatsPerSample = tempo / 60.0 / rate;
        const auto gain = run(g, rate, tempo, (int) (rate * 2));
        int checked = 0;
        for (int step = 1; step < 12; ++step)
        {
            const int edge = (int) std::ceil(step * 0.25 / beatsPerSample - 1e-9); // first sample at or after the boundary
            CHECK(std::abs(gain[(size_t) edge - 1] - (step % 2 == 1 ? 1.0f : 0.0f)) < 1e-6f); // still the old level
            CHECK(std::abs(gain[(size_t) edge] - gain[(size_t) edge - 1]) > 1e-6f);          // moving from this sample
            ++checked;
        }
        CHECK(checked == 11);
    }
}

TEST_CASE("trance gate: depth, smoothing without a step, Length")
{
    constexpr double rate = 48000;
    TranceGate g;
    g.prepare(rate);
    g.setPattern(kAlternate, 2);
    g.setShape(TranceGate::Sixteenth, 1.0f, 5.0f, 0.5f, 0.0f);
    auto gain = run(g, rate, 120, 48000);
    CHECK_NEAR(gain[9000], 0.5f, 1e-5); // a closed step at depth 50 %
    CHECK_NEAR(gain[3000], 1.0f, 1e-5);
    float largest = 0;
    for (size_t i = 1; i < gain.size(); ++i) largest = std::max(largest, std::abs(gain[i] - gain[i - 1]));
    CHECK(largest <= 0.5f / (0.005f * 48000) + 1e-5f); // the ramp spreads the edge over Smooth
    // Length 50 %: an open step closes halfway (120 BPM: a sixteenth is 6000 samples)
    const float open[1] = {1.0f};
    g.reset();
    g.setPattern(open, 1);
    g.setShape(TranceGate::Sixteenth, 0.5f, 1.0f, 1.0f, 0.0f);
    gain = run(g, rate, 120, 12000);
    CHECK(gain[2900] > 0.99f);
    CHECK(gain[3100] < 0.01f);
    CHECK(gain[6100] > 0.99f);
}

TEST_CASE("trance gate: swing delays every second step; triplet rates")
{
    constexpr double rate = 48000;
    TranceGate g;
    g.prepare(rate);
    g.setPattern(kAlternate, 2);
    g.setShape(TranceGate::Sixteenth, 1.0f, 1.0f, 1.0f, 0.5f); // odd steps start a quarter of a step late
    const double bps = 120.0 / 60.0 / rate;
    CHECK(g.target(0.25 * 1.2 - bps) > 0.99f);  // still open after the straight edge
    CHECK(g.target(0.25 * 1.25 + bps) < 0.01f); // closed after the swung edge
    CHECK(g.stepAt(0.26) == 0);
    CHECK(g.stepAt(0.32) == 1);
    CHECK(g.target(0.5 + bps) > 0.99f);         // the next pair starts on the grid
    g.setShape(TranceGate::SixteenthTriplet, 1.0f, 1.0f, 1.0f, 0.0f);
    CHECK(g.stepAt(1.0 / 6.0 + 1e-6) == 1);
    CHECK(g.stepAt(1.0 / 3.0 + 1e-6) == 0);
    CHECK_NEAR(TranceGate::stepBeats(TranceGate::EighthTriplet), 1.0 / 3.0, 1e-12);
    CHECK_NEAR(TranceGate::stepBeats(TranceGate::ThirtySecond), 0.125, 1e-12);
}

TEST_CASE("trance gate: at least 24 factory patterns, each a whole number of bars at its rate")
{
    const auto& list = TranceGate::factory();
    CHECK(list.size() >= 24);
    for (const auto& p : list)
    {
        CHECK(p.steps == 12 || p.steps == 16 || p.steps == 24 || p.steps == 32);
        const double beats = p.steps * TranceGate::stepBeats(p.rate < 0 ? TranceGate::Sixteenth : p.rate);
        CHECK(std::abs(beats / 4.0 - std::round(beats / 4.0)) < 1e-9);
        bool any = false, beyond = false;
        for (int i = 0; i < TranceGate::kMaxSteps; ++i)
        {
            if (i < p.steps && p.level[i] > 0.0f) any = true;
            if (i >= p.steps && p.level[i] > 0.0f) beyond = true;
            CHECK(p.level[i] >= 0.0f && p.level[i] <= 1.0f);
        }
        CHECK(any);
        CHECK(! beyond);
        // the pattern repeats exactly after its length
        TranceGate g;
        g.prepare(48000);
        g.setPattern(p.level, p.steps);
        g.setShape(p.rate < 0 ? TranceGate::Sixteenth : p.rate, 0.65f, 5.0f, 1.0f, 0.0f);
        for (double b = 0.01; b < beats; b += 0.07) CHECK(std::abs(g.target(b) - g.target(b + beats)) < 1e-6f);
    }
}
