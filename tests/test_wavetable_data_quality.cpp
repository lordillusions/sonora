#include "TestFramework.h"
#include "devices/native/WavetableData.h"

#include <algorithm>
#include <array>
#include <complex>
#include <limits>
#include <numbers>

using namespace sonora;
namespace
{
using Data = devices::WavetableData;
constexpr double twoPi = 2 * std::numbers::pi;
using Partial = std::pair<int, float>;

std::vector<float> harmonicFrame(std::initializer_list<Partial> partials, int length = 2048)
{
    std::vector<float> source(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i)
        for (const auto& [harmonic, amplitude] : partials)
            source[static_cast<size_t>(i)] += amplitude * static_cast<float>(std::sin(twoPi * harmonic * i / length));
    return source;
}

double harmonicMagnitude(const Data& data, int mip, int harmonic, float position = 0, int channel = 0)
{
    constexpr int count = 8192;
    std::complex<double> sum{};
    for (int i = 0; i < count; ++i)
    {
        const double phase = (static_cast<double>(i) + 0.37) / count;
        const double angle = twoPi * harmonic * phase;
        sum += static_cast<double>(data.sample(position, phase, mip, channel)) * std::complex<double>(std::cos(angle), -std::sin(angle));
    }
    return 2 * std::abs(sum) / count;
}

double renderedMagnitude(const Data& data, int mip, double frequency, double measured)
{
    constexpr int count = 48000;
    const double angle = twoPi * measured / count;
    const std::complex<double> step(std::cos(angle), -std::sin(angle));
    std::complex<double> phasor{1, 0}, sum{};
    for (int i = 0; i < count; ++i)
    {
        sum += static_cast<double>(data.sample(0, static_cast<double>(i) * frequency / count + 0.00317, mip)) * phasor;
        phasor *= step;
    }
    return 2 * std::abs(sum) / count;
}
} // namespace

TEST_CASE("wavetable data quality: dense C4 band retains legal harmonics 80 and 90 while excluding harmonics above Nyquist")
{
    const auto source = harmonicFrame({{1, 0.5f}, {80, 0.15f}, {90, 0.12f}, {100, 0.1f}, {190, 0.08f}});
    const auto loaded = Data::fromSamples(source, 1); REQUIRE(loaded);
    const auto& data = *loaded.data;
    constexpr double frequency = 261.6255653005986;
    const int mip = data.mipForIncrement(frequency / 48000);
    CHECK(data.harmonicLimit(mip) == 90); CHECK(data.mipLength(mip) >= 4 * data.harmonicLimit(mip));
    CHECK_NEAR(harmonicMagnitude(data, mip, 1), 0.5, 2e-6);
    CHECK_NEAR(harmonicMagnitude(data, mip, 80), 0.15, 0.0004);
    CHECK_NEAR(harmonicMagnitude(data, mip, 90), 0.12, 0.0004);
    CHECK(harmonicMagnitude(data, mip, 100) < 2e-6); CHECK(harmonicMagnitude(data, mip, 190) < 2e-6);
}

TEST_CASE("wavetable data quality: resampled upper legal partial survives and rejected harmonics do not fold into the audible band")
{
    const auto loaded = Data::fromSamples(harmonicFrame({{1, 0.5f}, {90, 0.2f}, {500, 0.35f}}), 1); REQUIRE(loaded);
    const auto& data = *loaded.data;
    constexpr double frequency = 250;
    const int mip = data.mipForIncrement(frequency / 48000);
    CHECK_NEAR(renderedMagnitude(data, mip, frequency, frequency), 0.5, 2e-5);
    CHECK_NEAR(renderedMagnitude(data, mip, frequency, 90 * frequency), 0.2, 0.0007);
    CHECK(renderedMagnitude(data, mip, frequency, 19000) < 2e-6); // 500th partial would fold from 125 kHz.
    CHECK(renderedMagnitude(data, mip, frequency, 9500) < 0.0001); // First interpolation image (512 - 90).
    const auto high = Data::fromSamples(harmonicFrame({{1, 0.3f}, {3, 0.2f}, {6, 0.4f}}), 1); REQUIRE(high);
    const int highMip = high.data->mipForIncrement(6000.0 / 48000);
    CHECK(high.data->harmonicLimit(highMip) == 3);
    CHECK_NEAR(renderedMagnitude(*high.data, highMip, 6000, 18000), 0.2, 2e-6);
    CHECK(renderedMagnitude(*high.data, highMip, 6000, 12000) < 2e-6); // Rejected sixth partial folds from 36 kHz.
}

TEST_CASE("wavetable data quality: variable FFT lengths and periodic interpolation preserve stereo frame blends DC and source Nyquist")
{
    constexpr int length = 64;
    std::vector<float> samples(length * 2 * 2);
    for (int frame = 0; frame < 2; ++frame)
        for (int i = 0; i < length; ++i)
        {
            const auto offset = static_cast<size_t>(frame * length + i) * 2;
            const double phase = twoPi * i / length;
            samples[offset] = static_cast<float>((frame == 0 ? 0.75 : -0.5) * std::sin(phase) + 0.1);
            samples[offset + 1] = static_cast<float>((frame == 0 ? 0.25 : 0.625) * std::cos(phase) - 0.2);
        }
    const auto loaded = Data::fromSamples(samples, 2, length, 32000, "Own variable-length stereo"); REQUIRE(loaded);
    const auto& data = *loaded.data; CHECK(data.originalSamples() == samples); CHECK(data.frameLength() == length);
    CHECK(data.mipLength(0) > length);
    for (int i = -11; i < 141; ++i)
    {
        const double phase = static_cast<double>(i) / 137;
        CHECK_NEAR(data.sample(0.4f, phase, 0, 0), 0.25 * std::sin(twoPi * phase) + 0.1, 2e-6);
        CHECK_NEAR(data.sample(0.4f, phase, 0, 1), 0.4 * std::cos(twoPi * phase) - 0.2, 2e-6);
        CHECK_NEAR(data.sample(0.4f, phase + 7, 0, 1), data.sample(0.4f, phase, 0, 1), 2e-6);
    }
    std::vector<float> nyquist(length);
    for (int i = 0; i < length; ++i) nyquist[static_cast<size_t>(i)] = i % 2 == 0 ? 0.8f : -0.8f;
    const auto nyquistData = Data::fromSamples(nyquist, 1, length); REQUIRE(nyquistData);
    CHECK_NEAR(harmonicMagnitude(*nyquistData.data, 0, length / 2), 0.8, 0.025);
    const int limited = nyquistData.data->mipForIncrement(0.5 / (length / 2));
    CHECK(harmonicMagnitude(*nyquistData.data, limited, length / 2) < 2e-6);
    for (int i = 0; i < 32; ++i)
        CHECK_NEAR(data.sample(0.4f, static_cast<double>(i) / 29, data.mipLevels() - 1, 0), 0.1, 2e-6);
}

TEST_CASE("wavetable data quality: mip selection is monotonic dense and strictly below Nyquist for every MIDI pitch")
{
    const auto loaded = Data::fromSamples(harmonicFrame({{1, 0.5f}}), 1); REQUIRE(loaded); const auto& data = *loaded.data;
    int previous = 0;
    for (int note = 0; note <= 127; ++note)
    {
        const double frequency = 440 * std::exp2(static_cast<double>(note - 69) / 12);
        const double increment = frequency / 48000; const int mip = data.mipForIncrement(increment);
        const int harmonics = data.harmonicLimit(mip);
        CHECK(mip >= previous); previous = mip; CHECK(harmonics * increment < 0.5);
        const int highestLegal = std::min(1024, static_cast<int>(std::ceil(0.5 / increment) - 1));
        CHECK(harmonics >= std::max(1, static_cast<int>(highestLegal * 0.84)));
        CHECK(data.mipForIncrement(-increment) == mip);
    }
    CHECK(data.mipForIncrement(0) == 0);
    CHECK(data.harmonicLimit(data.mipForIncrement(0.5)) == 0);
    CHECK(data.harmonicLimit(data.mipForIncrement(3)) == 0);
    CHECK(data.mipForIncrement(std::numeric_limits<double>::quiet_NaN()) == data.mipLevels() - 1);
    CHECK(data.mipForIncrement(std::numeric_limits<double>::infinity()) == data.mipLevels() - 1);
    CHECK_NEAR(data.sample(0, std::numeric_limits<double>::quiet_NaN(), 0), 0, 0);
}

TEST_CASE("wavetable data quality: all 256 stereo 2048 frames remain supported within a 64 MiB immutable bank")
{
    constexpr int length = 2048, channels = 2, frames = Data::maxFrames;
    std::vector<float> samples(static_cast<size_t>(length) * channels * frames);
    for (int frame = 0; frame < frames; ++frame)
        for (int i = 0; i < length; ++i)
        {
            const auto offset = static_cast<size_t>(frame * length + i) * channels;
            samples[offset] = static_cast<float>(0.2 * std::sin(twoPi * i / length) + 0.15 * std::sin(twoPi * 90 * i / length));
            samples[offset + 1] = static_cast<float>(0.3 * std::cos(twoPi * i / length) * frame / (frames - 1));
        }
    const auto loaded = Data::fromSamples(samples, channels, length, 48000, "Own maximum bank"); REQUIRE(loaded);
    const auto& data = *loaded.data;
    CHECK(data.frames() == frames); CHECK(data.channels() == channels); CHECK(data.originalSamples() == samples);
    CHECK(data.storageBytes() <= Data::maxBankBytes); CHECK(data.mipLevels() >= 40);
    const int mip = data.mipForIncrement(261.6255653005986 / 48000); CHECK(data.harmonicLimit(mip) == 90);
    CHECK_NEAR(harmonicMagnitude(data, mip, 90, 0.5f), 0.15, 0.0005);
    CHECK_NEAR(data.sample(0.5f, 0, mip, 1), 0.15, 2e-6);
    for (int band = 0; band < data.mipLevels(); ++band)
    {
        CHECK(data.mipLength(band) >= Data::minFrameLength); CHECK(data.mipLength(band) <= Data::maxFrameLength);
        CHECK((data.mipLength(band) & (data.mipLength(band) - 1)) == 0);
        for (int frame = 0; frame < frames; frame += 31)
            CHECK(std::isfinite(data.sample(static_cast<float>(frame) / (frames - 1), -0.234567, band, 1)));
    }
}
