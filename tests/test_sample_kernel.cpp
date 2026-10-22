#include "TestFramework.h"
#include "devices/native/NoiseSampleVoice.h"
#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableData.h"
#include "dsp/PreparedSampleKernel.h"

#include <algorithm>
#include <array>
#include <numbers>

namespace
{
using Kernel = sonora::dsp::PreparedSampleKernel;
using Profile = sonora::dsp::SampleKernelProfile;
using Voice = sonora::devices::NoiseSampleVoice;
using Table = sonora::devices::WavetableData;
}

TEST_CASE("sample kernel: immutable preparation preserves unity profile and bounded minimax ripple")
{
    const auto* unity = Kernel::forProfile(Profile::UnityKaiser);
    const auto* minimax = Kernel::forProfile(Profile::MinimaxApprox);
    REQUIRE(unity != nullptr);
    REQUIRE(minimax != nullptr);
    CHECK(Kernel::forProfile(static_cast<Profile>(2)) == nullptr);
    CHECK(Kernel::forProfile(Profile::MinimaxApprox) == minimax);
    double smallest = 2, largest = 0;
    for (int phase = 0; phase <= Kernel::phases; ++phase)
    {
        double dc = 0, ownDc = 0;
        for (int tap = 0; tap < Kernel::taps; ++tap)
        {
            const auto index = static_cast<size_t>(phase * Kernel::taps + tap);
            CHECK(std::isfinite(minimax->data()[index]));
            dc += minimax->data()[index];
            ownDc += unity->data()[index];
        }
        CHECK_NEAR(ownDc, 1, 2e-7);
        smallest = std::min(smallest, dc);
        largest = std::max(largest, dc);
    }
    CHECK(smallest > 1.00313);
    CHECK(largest < 1.00318);
    CHECK_NEAR(minimax->data()[15], 0, 0);
    CHECK_NEAR(minimax->data()[Kernel::phases * Kernel::taps], 0, 0);
}

TEST_CASE("sample kernel: explicit default noise remains bit identical and minimax stays optional")
{
    std::array<float, 512> source{};
    for (size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * static_cast<double>(frame) / source.size()));
    const auto sample = sonora::devices::OscillatorSample::fromSamples(source, 1, 44100);
    REQUIRE(sample);
    Voice implicit, explicitUnity, minimax;
    REQUIRE(implicit.start(sample.data.get(), 96000, 0.137));
    REQUIRE(explicitUnity.start(sample.data.get(), 96000, 0.137, Profile::UnityKaiser));
    REQUIRE(minimax.start(sample.data.get(), 96000, 0.137, Profile::MinimaxApprox));
    CHECK(implicit.kernelProfile() == Profile::UnityKaiser);
    CHECK(minimax.kernelProfile() == Profile::MinimaxApprox);
    double difference = 0;
    for (int frame = 0; frame < 8192; ++frame)
    {
        const auto a = implicit.next(1.37), b = explicitUnity.next(1.37), c = minimax.next(1.37);
        CHECK_NEAR(a.left, b.left, 0);
        CHECK_NEAR(a.right, b.right, 0);
        CHECK(std::isfinite(c.left));
        difference = std::max(difference, std::abs(static_cast<double>(c.left) - a.left));
    }
    CHECK(difference > 0.001);
    CHECK(difference < 0.002);
    CHECK_NEAR(implicit.position(), minimax.position(), 0);
    CHECK(! minimax.start(sample.data.get(), 48000, 0, static_cast<Profile>(2)));
    CHECK(! minimax.active());
    CHECK(Voice::maxPlaybackRatio >= 4096);
}

TEST_CASE("sample kernel: minimax octave banks preserve immutable PCM geometry frames and stereo")
{
    std::vector<float> source(2048 * 2 * 2);
    for (int frame = 0; frame < 2; ++frame)
        for (int sample = 0; sample < 2048; ++sample)
        {
            const auto index = static_cast<size_t>((frame * 2048 + sample) * 2);
            source[index] = static_cast<float>(std::sin(2 * std::numbers::pi * sample / 2048)) * (frame == 0 ? 1.0f : 0.25f);
            source[index + 1] = source[index] * -0.5f;
        }
    const auto old = Table::fromSamples(source, 2, 2048, 48000, {}, Table::PlaybackProfile::Octave16Approx);
    REQUIRE(old);
    const auto next = old.data->withProfile(Table::PlaybackProfile::Octave16Minimax);
    REQUIRE(next);
    CHECK(next.data->originalSamples() == source);
    CHECK(next.data->recommendedRateFactor() == 2);
    CHECK(next.data->mipLevels() == old.data->mipLevels());
    CHECK(next.data->storageBytes() <= Table::maxBankBytes);
    for (int mip = 0; mip < next.data->mipLevels(); ++mip)
    {
        CHECK(next.data->mipLength(mip) == old.data->mipLength(mip));
        CHECK(next.data->harmonicLimit(mip) == old.data->harmonicLimit(mip));
        for (int sample = 0; sample < 128; ++sample)
        {
            const double phase = static_cast<double>(sample) / 127;
            const float value = next.data->sample(0, phase, mip);
            CHECK(std::isfinite(value));
            CHECK_NEAR(next.data->sample(0, phase, mip, 1), value * -0.5f, 2e-6);
            CHECK_NEAR(next.data->sample(0.5f, phase, mip), value * 0.625f, 2e-6);
            CHECK_NEAR(next.data->sample(0, phase + 3, mip), value, 2e-6);
        }
    }
    CHECK(next.data->mipForIncrement(261.62556295 / 96000) == 3);
    CHECK(! next.data->withProfile(static_cast<Table::PlaybackProfile>(3)));
    const auto restored = next.data->withProfile(Table::PlaybackProfile::Octave16Approx);
    REQUIRE(restored);
    for (int sample = 0; sample < 256; ++sample)
        CHECK_NEAR(restored.data->sample(0.37f, static_cast<double>(sample) / 255, 4),
                   old.data->sample(0.37f, static_cast<double>(sample) / 255, 4), 0);
}

TEST_CASE("sample kernel: minimax actual sine output has designed gain and continuous phase rollover")
{
    std::array<float, 2048> source{};
    for (size_t sample = 0; sample < source.size(); ++sample)
        source[sample] = static_cast<float>(std::sin(2 * std::numbers::pi * static_cast<double>(sample) / source.size()));
    const auto table = Table::fromSamples(source, 1, 2048, 48000, {}, Table::PlaybackProfile::Octave16Minimax);
    REQUIRE(table);
    double real = 0, imaginary = 0;
    constexpr int samples = 65536;
    for (int sample = 0; sample < samples; ++sample)
    {
        const double phase = static_cast<double>(sample) / samples, angle = 2 * std::numbers::pi * phase;
        const double value = table.data->sample(0, phase, 3);
        real += value * std::cos(angle);
        imaginary += value * std::sin(angle);
    }
    CHECK_NEAR(2 * std::hypot(real, imaginary) / samples, 1.00312246, 3e-6);
    CHECK_NEAR(2 * real / samples, 0, 1e-7);
    for (int sample = 0; sample < 256; ++sample)
    {
        const double phase = static_cast<double>(sample) / 256;
        CHECK_NEAR(table.data->sample(0, phase - 1e-10, 3), table.data->sample(0, phase + 1e-10, 3), 3e-7);
    }
}
