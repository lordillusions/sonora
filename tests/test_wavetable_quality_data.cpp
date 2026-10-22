#include "TestFramework.h"
#include "devices/native/WavetableData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
using Table = sonora::devices::WavetableData;
using Profile = Table::PlaybackProfile;
constexpr double pi = 3.14159265358979323846;
std::vector<float> qualityWave(int harmonic = 1, int frames = 1, int channels = 1, int length = 2048)
{
    std::vector<float> samples(static_cast<size_t>(frames * channels * length));
    for (int frame = 0; frame < frames; ++frame)
        for (int sample = 0; sample < length; ++sample)
            for (int channel = 0; channel < channels; ++channel)
                samples[static_cast<size_t>((frame * length + sample) * channels + channel)] =
                    static_cast<float>(std::sin(2 * pi * harmonic * sample / length))
                    * (channel == 0 ? 1.0f : -0.5f) * (frame == 0 ? 1.0f : 0.25f);
    return samples;
}
double harmonicAmplitude(const Table& table, int mip, int harmonic)
{
    double real = 0, imaginary = 0;
    constexpr int count = 4096;
    for (int sample = 0; sample < count; ++sample)
    {
        const double phase = static_cast<double>(sample) / count;
        const double value = table.sample(0, phase, mip);
        real += value * std::cos(2 * pi * harmonic * phase);
        imaginary += value * std::sin(2 * pi * harmonic * phase);
    }
    return 2 * std::hypot(real, imaginary) / count;
}
}

TEST_CASE("wavetable quality data keeps Dense12 default and immutable source")
{
    const auto source = qualityWave(3, 2, 2);
    const auto legacy = Table::fromSamples(source, 2, 2048, 48000, "Own quality fixture");
    const auto explicitDense = Table::fromSamples(source, 2, 2048, 48000, "Own quality fixture", Profile::Dense12);
    REQUIRE(legacy); REQUIRE(explicitDense);
    CHECK(legacy.data->playbackProfile() == Profile::Dense12);
    CHECK(legacy.data->recommendedRateFactor() == 1);
    CHECK(legacy.data->originalSamples() == source);
    for (int sample = 0; sample < 400; ++sample)
    {
        const double phase = static_cast<double>(sample) / 399;
        CHECK_NEAR(legacy.data->sample(0.37f, phase, 9, 1), explicitDense.data->sample(0.37f, phase, 9, 1), 0);
    }
    const auto octave = legacy.data->withProfile(Profile::Octave16Approx);
    REQUIRE(octave);
    CHECK(octave.data->playbackProfile() == Profile::Octave16Approx);
    CHECK(octave.data->recommendedRateFactor() == 2);
    CHECK(octave.data->originalSamples() == source);
    CHECK(octave.data->channels() == 2);
    CHECK(octave.data->frames() == 2);
    CHECK(octave.data->name() == legacy.data->name());
    CHECK_NEAR(octave.data->sampleRate(), 48000, 0);
    CHECK(legacy.data->playbackProfile() == Profile::Dense12);
    const auto restored = octave.data->withPlaybackProfile(Profile::Dense12);
    REQUIRE(restored);
    CHECK_NEAR(restored.data->sample(0.37f, 0.153, 9, 1), legacy.data->sample(0.37f, 0.153, 9, 1), 0);
}

TEST_CASE("wavetable quality data octave lengths and internal increment boundaries")
{
    const auto loaded = Table::fromSamples(qualityWave(), 1, 2048, 44100, {}, Profile::Octave16Approx);
    REQUIRE(loaded);
    const auto& table = *loaded.data;
    CHECK(table.mipLevels() == 12);
    for (int level = 0; level < 12; ++level)
    {
        CHECK(table.mipLength(level) == (2048 >> level));
        CHECK(table.harmonicLimit(level) == ((2048 >> level) / 2));
    }
    CHECK(table.mipForIncrement(0) == 0);
    CHECK(table.mipForIncrement(1.0 / 2048) == 0);
    CHECK(table.mipForIncrement(1.0001 / 2048) == 1);
    CHECK(table.mipForIncrement(2.0 / 2048) == 1);
    CHECK(table.mipForIncrement(2.0001 / 2048) == 2);
    CHECK(table.mipForIncrement(261.62556295 / 96000) == 3);
    CHECK(table.mipForIncrement(261.62556295 / 48000) == 4);
    CHECK(table.mipForIncrement(-261.62556295 / 96000) == 3);
    CHECK(table.mipForIncrement(0.5) == 10);
    CHECK(table.mipForIncrement(1) == 11);
    CHECK(table.mipForIncrement(std::numeric_limits<double>::infinity()) == 11);
    CHECK(table.mipForIncrement(std::numeric_limits<double>::quiet_NaN()) == 11);
}

TEST_CASE("wavetable quality data generated kernel has unity DC across every phase and level")
{
    const std::vector<float> source(2048, 0.25f);
    const auto loaded = Table::fromSamples(source, 1, 2048, 44100, {}, Profile::Octave16Approx);
    REQUIRE(loaded);
    for (int level = 0; level < loaded.data->mipLevels(); ++level)
        for (int phase = 0; phase <= 1024; ++phase)
            CHECK_NEAR(loaded.data->sample(0, static_cast<double>(phase) / 1024, level), 0.25, 2e-7);
}

TEST_CASE("wavetable quality data octave filtering retains and removes authored harmonics")
{
    const auto loaded = Table::fromSamples(qualityWave(80), 1, 2048, 48000, {}, Profile::Octave16Approx);
    REQUIRE(loaded);
    CHECK(harmonicAmplitude(*loaded.data, 3, 80) > 0.97);
    CHECK(harmonicAmplitude(*loaded.data, 4, 80) < 1e-5);
    CHECK(harmonicAmplitude(*loaded.data, 11, 80) < 1e-6);
    // Our FFT convention folds the two source Nyquist partners when decimating.
    // This checks finite bounded handling; reference boundary bins remain unverified.
    const auto edge = Table::fromSamples(qualityWave(64), 1, 2048, 48000, {}, Profile::Octave16Approx);
    REQUIRE(edge);
    for (int phase = 0; phase < 1024; ++phase)
        CHECK(std::isfinite(edge.data->sample(0, static_cast<double>(phase) / 1024, 4)));
}

TEST_CASE("wavetable quality data frames stereo and periodic guards remain continuous")
{
    const auto loaded = Table::fromSamples(qualityWave(1, 2, 2), 2, 2048, 48000, {}, Profile::Octave16Approx);
    REQUIRE(loaded);
    for (int level = 0; level < loaded.data->mipLevels(); ++level)
        for (int sample = 0; sample < 100; ++sample)
        {
            const double phase = static_cast<double>(sample) / 99;
            const float first = loaded.data->sample(0, phase, level);
            CHECK_NEAR(loaded.data->sample(1, phase, level), first * 0.25f, 2e-6);
            CHECK_NEAR(loaded.data->sample(0.5f, phase, level), first * 0.625f, 2e-6);
            CHECK_NEAR(loaded.data->sample(0, phase, level, 1), first * -0.5f, 2e-6);
            CHECK_NEAR(loaded.data->sample(0, phase - 3, level), first, 2e-6);
        }
    CHECK_NEAR(loaded.data->sample(0, -1e-9, 0), loaded.data->sample(0, 1 - 1e-9, 0), 1e-8);
    CHECK_NEAR(loaded.data->sample(0, 0, 0), loaded.data->sample(0, 1, 0), 0);
    for (const Profile profile : {Profile::Dense12, Profile::Octave16Approx})
    {
        const auto boundary = loaded.data->withProfile(profile);
        REQUIRE(boundary);
        for (int level = 0; level < boundary.data->mipLevels(); ++level)
            CHECK_NEAR(boundary.data->sample(0, -std::numeric_limits<double>::denorm_min(), level),
                       boundary.data->sample(0, 0, level), 0);
    }
}

TEST_CASE("wavetable quality data maximum bank fits budget and invalid profiles fail")
{
    const std::vector<float> maximum(static_cast<size_t>(2048) * 256 * 2, 0.125f);
    const auto loaded = Table::fromSamples(maximum, 2, 2048, 44100, "Own maximum bank", Profile::Octave16Approx);
    REQUIRE(loaded);
    CHECK(loaded.data->storageBytes() <= Table::maxBankBytes);
    CHECK(loaded.data->originalSamples() == maximum);
    CHECK(loaded.data->frames() == 256);
    CHECK_NEAR(loaded.data->sample(0.73f, 0.125, 11, 1), 0.125, 2e-7);
    CHECK(! Table::fromSamples(maximum, 2, 2048, 44100, {}, static_cast<Profile>(3)));
    CHECK(! loaded.data->withProfile(static_cast<Profile>(-1)));
    const auto small = Table::fromSamples(qualityWave(1, 1, 1, 64), 1, 64, 44100, {}, Profile::Octave16Approx);
    REQUIRE(small);
    CHECK(small.data->mipLevels() == 7);
    CHECK(small.data->mipLength(6) == 1);
    for (int level = 0; level < 7; ++level)
        CHECK(std::isfinite(small.data->sample(0, -0.07, level)));
}
