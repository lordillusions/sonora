#include "TestFramework.h"
#include "engine/TempoAnalysis.h"
#include "engine/stretch/Transients.h"
#include <limits>
#include <numbers>
using namespace sonora;

TEST_CASE("tempo analysis: regular attacks recover source BPM rather than the project tempo")
{
    for (double bpm : {77.0, 100.0, 127.3, 180.0})
    {
        std::vector<double> attacks;
        for (int i = 0; i < 24; ++i) attacks.push_back(i * 60 / bpm + (i % 3 - 1) * 0.0005 + 0.001);
        const auto estimate = engine::estimateSampleTempo(attacks, 120);
        CHECK_NEAR(estimate.bpm, bpm, 0.2); CHECK(estimate.confidence > 0.9);
    }
    CHECK_NEAR(engine::estimateSampleTempo({}, 120).bpm, 0.0, 1e-12);
    const std::vector<double> oneShot{0, 0.06}; CHECK_NEAR(engine::estimateSampleTempo(oneShot, 120).bpm, 0.0, 1e-12);
    const std::vector<double> bad{0, 0.5, 1, std::numeric_limits<double>::quiet_NaN()};
    CHECK_NEAR(engine::estimateSampleTempo(bad, 120).bpm, 0.0, 1e-12);
}

TEST_CASE("tempo analysis: decoded rhythmic audio produces usable first-Warp tempo")
{
    constexpr int rate = 16000; constexpr double bpm = 125;
    juce::AudioBuffer<float> audio(1, rate * 8); audio.clear();
    for (int beat = 0; beat < 16; ++beat)
    {
        const int start = (int) std::lround(beat * rate * 60 / bpm);
        for (int i = 0; i < 600 && start + i < audio.getNumSamples(); ++i)
            audio.setSample(0, start + i, (float) (std::exp(-i / 90.0) * std::sin(2 * std::numbers::pi * 1600 * i / rate)));
    }
    const auto estimate = engine::estimateSampleTempo(engine::detectTransients(audio, rate), 110);
    CHECK_NEAR(estimate.bpm, bpm, 0.3); CHECK(estimate.confidence > 0.8);
}
