#include "TestFramework.h"
#include "devices/native/WavetableDownsampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using Downsampler = sonora::devices::WavetableDownsampler;
constexpr double pi = 3.14159265358979323846;
struct SineEstimate { double gain = 0, delay = 0; };
SineEstimate measureSine(double frequency)
{
    Downsampler downsampler;
    if (! downsampler.prepare(2)) return {};
    Downsampler::Frame first{}, second{}, output{};
    constexpr int warmup = 4096, samples = 16384;
    const double omega = 2 * pi * frequency / 48000;
    double ss = 0, cc = 0, sc = 0, sy = 0, cy = 0;
    for (int sample = 0; sample < warmup + samples; ++sample)
    {
        const double angle = omega * sample;
        first[0] = static_cast<float>(std::sin(angle));
        second[0] = static_cast<float>(std::sin(angle + omega * 0.5));
        downsampler.processFrame(first, second, output);
        if (sample < warmup) continue;
        const double sine = std::sin(angle), cosine = std::cos(angle);
        ss += sine * sine; cc += cosine * cosine; sc += sine * cosine;
        sy += sine * output[0]; cy += cosine * output[0];
    }
    const double denominator = ss * cc - sc * sc;
    const double sine = (sy * cc - cy * sc) / denominator;
    const double cosine = (cy * ss - sy * sc) / denominator;
    return {std::hypot(sine, cosine), std::atan2(-cosine, sine) / omega};
}
}

TEST_CASE("wavetable downsampler standard preparation and factor1 identity")
{
    Downsampler downsampler;
    Downsampler::Frame first{}, second{}, output{};
    for (size_t lane = 0; lane < Downsampler::laneCount; ++lane)
    {
        first[lane] = static_cast<float>(lane) * 0.1f - 0.4f;
        second[lane] = -first[lane];
    }
    CHECK(downsampler.rateFactor() == 1);
    CHECK(downsampler.integerPipelineDelay() == 0);
    downsampler.processFrame(first, second, output);
    CHECK(output == first);
    REQUIRE(downsampler.prepare(2));
    CHECK(downsampler.rateFactor() == 2);
    CHECK(downsampler.integerPipelineDelay() == 2);
    CHECK(downsampler.alpha().size() == 10);
    for (const float pole : downsampler.alpha()) CHECK(pole > 0 && pole < 1);
    CHECK(! downsampler.prepare(0));
    CHECK(! downsampler.prepare(4));
    CHECK(downsampler.rateFactor() == 2);
    REQUIRE(downsampler.prepare(1));
    downsampler.processFrame(first, second, output);
    CHECK(output == first);
}

TEST_CASE("wavetable downsampler matches analytic C4 phase and passband gain")
{
    const auto c4 = measureSine(261.62556295066355);
    CHECK_NEAR(c4.gain, 1, 2e-6);
    // Standard generated poles and the explicit two-sample queue predict this
    // transfer delay. Complete instrument/producer ordering is not admitted.
    CHECK_NEAR(c4.delay, 3.673216810879332, 2e-4);
    const auto high = measureSine(10000);
    CHECK_NEAR(high.gain, 1, 3e-6);
    const auto low = measureSine(440);
    CHECK_NEAR(low.delay, 3.6733980115932523, 2e-4);
}

TEST_CASE("wavetable downsampler rejects authored frequencies above output Nyquist")
{
    CHECK(measureSine(30000).gain < 1e-5);
    CHECK(measureSine(34000).gain < 1e-5);
    CHECK(measureSine(44000).gain < 1e-5);
    CHECK(measureSine(20000).gain > 0.999);
}

TEST_CASE("wavetable downsampler unity DC and eight lane isolation")
{
    Downsampler downsampler;
    REQUIRE(downsampler.prepare(2));
    Downsampler::Frame first{}, second{}, output{};
    for (size_t lane = 0; lane < Downsampler::laneCount; ++lane)
        first[lane] = second[lane] = (static_cast<float>(lane) - 3.5f) * 0.1f;
    for (int sample = 0; sample < 4096; ++sample) downsampler.processFrame(first, second, output);
    for (size_t lane = 0; lane < Downsampler::laneCount; ++lane)
        CHECK_NEAR(output[lane], first[lane], 2e-6);
    for (size_t active = 0; active < Downsampler::laneCount; ++active)
    {
        downsampler.reset(); first = {}; second = {};
        bool heard = false;
        for (int sample = 0; sample < 128; ++sample)
        {
            first[active] = sample == 0 ? 1.0f : 0.0f;
            downsampler.processFrame(first, second, output);
            for (size_t lane = 0; lane < Downsampler::laneCount; ++lane)
                if (lane != active) CHECK_NEAR(output[lane], 0, 0);
            if (sample < 2) CHECK_NEAR(output[active], 0, 0);
            heard |= std::abs(output[active]) > 1e-6f;
        }
        CHECK(heard);
    }
}

TEST_CASE("wavetable downsampler reset clears IIR and explicit pipeline history")
{
    Downsampler downsampler;
    REQUIRE(downsampler.prepare(2));
    Downsampler::Frame first{}, second{}, output{};
    first.fill(1); second.fill(-0.3f);
    for (int sample = 0; sample < 11; ++sample) downsampler.processFrame(first, second, output);
    downsampler.reset(); first = {}; second = {};
    for (int sample = 0; sample < 256; ++sample)
    {
        downsampler.processFrame(first, second, output);
        for (const float value : output) CHECK_NEAR(value, 0, 0);
    }
}

TEST_CASE("wavetable downsampler frame grouping preserves exact independent stream state")
{
    Downsampler straight, partitioned;
    REQUIRE(straight.prepare(2)); REQUIRE(partitioned.prepare(2));
    constexpr int samples = 2048;
    std::vector<Downsampler::Frame> first(samples), second(samples), expected(samples), actual(samples);
    for (int sample = 0; sample < samples; ++sample)
        for (size_t lane = 0; lane < Downsampler::laneCount; ++lane)
        {
            const double phase = static_cast<double>(sample) * 0.071 + static_cast<double>(lane);
            first[static_cast<size_t>(sample)][lane] = static_cast<float>(std::sin(phase));
            second[static_cast<size_t>(sample)][lane] = static_cast<float>(std::sin(phase + 0.0355));
        }
    for (int sample = 0; sample < samples; ++sample)
        straight.processFrame(first[static_cast<size_t>(sample)], second[static_cast<size_t>(sample)], expected[static_cast<size_t>(sample)]);
    for (int start = 0; start < samples; start += 97)
        for (int sample = start; sample < std::min(start + 97, samples); ++sample)
            partitioned.processFrame(first[static_cast<size_t>(sample)], second[static_cast<size_t>(sample)], actual[static_cast<size_t>(sample)]);
    CHECK(actual == expected);
}

TEST_CASE("wavetable downsampler invalid lane samples do not poison other histories")
{
    Downsampler downsampler;
    REQUIRE(downsampler.prepare(2));
    Downsampler::Frame first{}, second{}, output{};
    first.fill(0.25f); second.fill(0.25f);
    for (int sample = 0; sample < 4096; ++sample) downsampler.processFrame(first, second, output);
    first[2] = std::numeric_limits<float>::quiet_NaN();
    second[5] = std::numeric_limits<float>::infinity();
    downsampler.processFrame(first, second, output);
    for (const float value : output) CHECK(std::isfinite(value));
    CHECK_NEAR(output[2], 0, 0); CHECK_NEAR(output[5], 0, 0);
    CHECK_NEAR(output[0], 0.25, 2e-6);
    first.fill(0.25f); second.fill(0.25f);
    for (int sample = 0; sample < 4096; ++sample) downsampler.processFrame(first, second, output);
    for (const float value : output) CHECK_NEAR(value, 0.25, 2e-6);
}
