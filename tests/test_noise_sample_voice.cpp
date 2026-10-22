#include "TestFramework.h"
#include "devices/native/NoiseSampleVoice.h"
#include "devices/native/OscillatorMedia.h"

#include <algorithm>
#include <array>
#include <limits>
#include <numbers>
#include <span>

using namespace sonora;

namespace
{
using Voice = devices::NoiseSampleVoice;
using Sample = devices::OscillatorSample;

std::shared_ptr<const Sample> checked(std::span<const float> pcm, int channels = 1, double rate = 48000)
{
    const auto result = Sample::fromSamples(pcm, channels, rate, "Authored periodic noise fixture");
    REQUIRE(result);
    return result.data;
}

std::vector<float> harmonic(int frames = 2048, int channels = 1, int cycles = 3)
{
    std::vector<float> pcm(static_cast<size_t>(frames) * static_cast<size_t>(channels));
    for (int frame = 0; frame < frames; ++frame)
        for (int channel = 0; channel < channels; ++channel)
            pcm[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)]
                = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * cycles * frame / frames + channel * std::numbers::pi * 0.5));
    return pcm;
}

std::vector<Voice::Stereo> render(Voice& voice, int samples, std::span<const int> blocks, double ratio)
{
    REQUIRE(! blocks.empty());
    std::vector<Voice::Stereo> output(static_cast<size_t>(samples));
    int start = 0;
    size_t block = 0;
    while (start < samples)
    {
        const int count = std::min(blocks[block++ % blocks.size()], samples - start);
        REQUIRE(count > 0);
        for (int frame = start; frame < start + count; ++frame)
            output[static_cast<size_t>(frame)] = voice.next(ratio);
        start += count;
    }
    return output;
}

void writeOwnAudio(const juce::File& file, std::span<const float> pcm, bool aiff)
{
    REQUIRE(file.getParentDirectory().createDirectory().wasOk());
    REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    REQUIRE(stream != nullptr);
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aif;
    juce::AudioFormat& format = aiff ? static_cast<juce::AudioFormat&>(aif) : static_cast<juce::AudioFormat&>(wav);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(24000).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    juce::AudioBuffer<float> buffer(2, static_cast<int>(pcm.size() / 2));
    for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
        for (int channel = 0; channel < 2; ++channel)
            buffer.setSample(channel, frame, pcm[static_cast<size_t>(frame * 2 + channel)]);
    REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()));
}
}

TEST_CASE("noise sample voice: non owning fixed state validates start reset rates and finite controls")
{
    const std::array<float, 4> pcm{0.25f, -0.5f, 0.5f, -0.25f};
    const auto sample = checked(pcm);
    const auto owners = sample.use_count();
    Voice voice;
    CHECK(! voice.active());
    CHECK_NEAR(voice.next().left, 0, 0);
    REQUIRE(voice.start(sample.get(), 48000));
    CHECK(voice.active());
    CHECK(voice.sample() == sample.get());
    CHECK(sample.use_count() == owners);
    CHECK_NEAR(voice.sourceFramesPerOutputSample(), 1, 0);
    const double position = voice.position();
    const auto invalid = voice.next(std::numeric_limits<double>::quiet_NaN());
    CHECK_NEAR(invalid.left, 0, 0);
    CHECK_NEAR(invalid.right, 0, 0);
    CHECK_NEAR(voice.position(), position, 0);
    voice.next(std::numeric_limits<double>::infinity());
    CHECK_NEAR(voice.position(), position, 0);
    for (double badRate : {0.0, -1.0, 768001.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        CHECK(! voice.start(sample.get(), badRate));
        CHECK(! voice.active());
    }
    CHECK(! voice.start(sample.get(), 48000, std::numeric_limits<double>::quiet_NaN()));
    CHECK(! voice.start(nullptr, 48000));
    REQUIRE(voice.start(sample.get(), 48000));
    voice.next();
    voice.reset();
    CHECK(! voice.active());
    CHECK(voice.sample() == nullptr);
    CHECK_NEAR(voice.position(), 0, 0);
    CHECK_NEAR(voice.next().left, 0, 0);
}

TEST_CASE("noise sample voice: normalized endpoint starts at final frame and signed ratios wrap periodically")
{
    const auto sample = checked(harmonic(32));
    Voice voice;
    REQUIRE(voice.start(sample.get(), 48000, 1));
    CHECK_NEAR(voice.position(), 31, 0);
    voice.next();
    CHECK_NEAR(voice.position(), 0, 0);
    voice.next(-1);
    CHECK_NEAR(voice.position(), 31, 0);
    voice.next(-0.5);
    CHECK_NEAR(voice.position(), 30.5, 0);
    REQUIRE(voice.start(sample.get(), 48000, 0.25));
    CHECK_NEAR(voice.position(), 8, 0);
    REQUIRE(voice.start(sample.get(), 48000, -2));
    CHECK_NEAR(voice.position(), 0, 0);
    REQUIRE(voice.start(sample.get(), 48000, 2));
    CHECK_NEAR(voice.position(), 31, 0);
    REQUIRE(voice.start(sample.get(), 48000));
    voice.next(-std::numeric_limits<double>::denorm_min());
    CHECK(voice.position() >= 0 && voice.position() < 32);
    CHECK(std::isfinite(voice.next().left));
}

TEST_CASE("noise sample voice: DC is unity for mono stereo one frame and fractional phase")
{
    for (int frames : {1, 2, 7, 16, 257})
    {
        std::vector<float> stereo(static_cast<size_t>(frames * 2));
        for (int frame = 0; frame < frames; ++frame)
        {
            stereo[static_cast<size_t>(frame * 2)] = 0.375f;
            stereo[static_cast<size_t>(frame * 2 + 1)] = -0.25f;
        }
        const auto sample = checked(stereo, 2, 44100);
        Voice voice;
        REQUIRE(voice.start(sample.get(), 48000, 0.137));
        double leftError = 0, rightError = 0;
        for (int output = 0; output < 4096; ++output)
        {
            const auto sound = voice.next(output % 2 == 0 ? 1.731 : -0.537);
            leftError = std::max(leftError, std::abs(static_cast<double>(sound.left) - 0.375));
            rightError = std::max(rightError, std::abs(static_cast<double>(sound.right) + 0.25));
        }
        CHECK(leftError < 2e-7);
        CHECK(rightError < 2e-7);
    }
    const std::array<float, 1> mono{0.25f};
    const auto sample = checked(mono);
    Voice voice;
    REQUIRE(voice.start(sample.get(), 48000));
    const auto sound = voice.next();
    CHECK_NEAR(sound.left, sound.right, 0);
    CHECK_NEAR(sound.left, 0.25, 2e-7);
}

TEST_CASE("noise sample voice: own low harmonics follow analytical source rate phase and pitch")
{
    constexpr int frames = 2048, cycles = 3;
    constexpr double sourceRate = 44100, outputRate = 96000, phase = 0.137;
    const auto sample = checked(harmonic(frames, 2, cycles), 2, sourceRate);
    for (double ratio : {0.0, 0.25, 1.0, 1.73, -0.83})
    {
        Voice voice;
        REQUIRE(voice.start(sample.get(), outputRate, phase));
        double error = 0;
        for (int output = 0; output < 8192; ++output)
        {
            const auto sound = voice.next(ratio);
            const double angle = 2 * std::numbers::pi * cycles * (phase * frames + output * sourceRate / outputRate * ratio) / frames;
            error = std::max(error, std::abs(sound.left - 0.4 * std::sin(angle)));
            error = std::max(error, std::abs(sound.right - 0.4 * std::cos(angle)));
        }
        CHECK(error < 2e-5);
    }
}

TEST_CASE("noise sample voice: periodic PCM and output are invariant across unequal blocks and independent voices")
{
    const auto sample = checked(harmonic(257, 2), 2);
    const auto fingerprint = sample->pcmFingerprint();
    const auto* pcm = sample->originalSamples().data();
    Voice a, b, other;
    REQUIRE(a.start(sample.get(), 48000, 0.17));
    REQUIRE(b.start(sample.get(), 48000, 0.17));
    REQUIRE(other.start(sample.get(), 96000, 0.7));
    constexpr std::array<int, 1> whole{8192};
    constexpr std::array<int, 7> split{1, 73, 512, 7, 2053, 19, 257};
    const auto expected = render(a, 8192, whole, -1.371);
    const auto actual = render(b, 8192, split, -1.371);
    for (size_t frame = 0; frame < expected.size(); ++frame)
    {
        CHECK_NEAR(actual[frame].left, expected[frame].left, 0);
        CHECK_NEAR(actual[frame].right, expected[frame].right, 0);
        other.next(2.313);
    }
    CHECK_NEAR(a.position(), b.position(), 0);
    CHECK(sample->originalSamples().data() == pcm);
    CHECK(sample->pcmFingerprint() == fingerprint);
    const auto beforeReset = b.position();
    other.reset();
    CHECK_NEAR(b.position(), beforeReset, 0);
    a.reset();
    REQUIRE(a.start(sample.get(), 48000, 0.17));
    CHECK_NEAR(a.next(-1.371).left, expected.front().left, 0);
}

TEST_CASE("noise sample voice: repeating PCM has no edge truncation or one shot silence")
{
    const auto sample = checked(harmonic(32, 2, 1), 2);
    Voice voice;
    REQUIRE(voice.start(sample.get(), 48000, 0.27));
    std::array<Voice::Stereo, 32> first{};
    for (auto& frame : first) frame = voice.next();
    double energy = 0, error = 0;
    for (int frame = 0; frame < 4096; ++frame)
    {
        const auto sound = voice.next();
        const auto& expected = first[static_cast<size_t>(frame % 32)];
        error = std::max(error, std::abs(static_cast<double>(sound.left) - expected.left));
        error = std::max(error, std::abs(static_cast<double>(sound.right) - expected.right));
        energy += sound.left * sound.left + sound.right * sound.right;
    }
    CHECK(voice.active());
    CHECK(error < 2e-7);
    CHECK(energy > 500);
}

TEST_CASE("noise sample voice: authored WAV and AIFF PCM play identically to their immutable embedded data")
{
    const auto pcm = harmonic(257, 2, 1);
    for (bool aiff : {false, true})
    {
        const juce::File file(aiff ? "/tmp/sonora_tests/sonora_noise_sample/Own noise.aiff"
                                  : "/tmp/sonora_tests/sonora_noise_sample/Own noise.wav");
        writeOwnAudio(file, pcm, aiff);
        const auto loaded = Sample::fromFile(file);
        REQUIRE(loaded);
        const auto embedded = checked(loaded.data->originalSamples(), 2, 24000);
        CHECK(loaded.data->pcmFingerprint() == embedded->pcmFingerprint());
        Voice fromFile, fromPcm;
        REQUIRE(fromFile.start(loaded.data.get(), 48000, 1));
        REQUIRE(fromPcm.start(embedded.get(), 48000, 1));
        for (int frame = 0; frame < 2048; ++frame)
        {
            const auto a = fromFile.next(1.37), b = fromPcm.next(1.37);
            CHECK_NEAR(a.left, b.left, 0);
            CHECK_NEAR(a.right, b.right, 0);
        }
        CHECK(loaded.data->originalSamples() == embedded->originalSamples());
    }
}

TEST_CASE("noise sample voice: maximum bounded rate ratios stay finite and clamp deterministically")
{
    const auto sample = checked(harmonic(17, 2), 2, Voice::maxSampleRate);
    Voice large, clamped;
    REQUIRE(large.start(sample.get(), 1));
    REQUIRE(clamped.start(sample.get(), 1));
    for (int frame = 0; frame < 4096; ++frame)
    {
        const bool negative = frame % 2 == 0;
        const auto a = large.next(negative ? -1e300 : 1e300);
        const auto b = clamped.next(negative ? -Voice::maxPlaybackRatio : Voice::maxPlaybackRatio);
        CHECK(std::isfinite(a.left) && std::isfinite(a.right));
        CHECK(std::abs(a.left) < 2 && std::abs(a.right) < 2);
        CHECK_NEAR(a.left, b.left, 0);
        CHECK_NEAR(a.right, b.right, 0);
        CHECK(large.position() >= 0 && large.position() < sample->sampleFrames());
    }
    const auto slow = checked(harmonic(17), 1, 1);
    REQUIRE(large.start(slow.get(), Voice::maxSampleRate));
    for (int frame = 0; frame < 1024; ++frame)
    {
        const auto sound = large.next();
        CHECK(std::isfinite(sound.left));
        CHECK_NEAR(sound.left, sound.right, 0);
    }
}
