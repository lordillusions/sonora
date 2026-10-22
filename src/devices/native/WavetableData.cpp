#include "devices/native/WavetableData.h"
#include "dsp/PreparedSampleKernel.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <new>
#include <stdexcept>

namespace sonora::devices
{
namespace
{
bool supportedLength(int length)
{
    return length >= WavetableData::minFrameLength && length <= WavetableData::maxFrameLength
        && (length & (length - 1)) == 0;
}
WavetableLoadResult failure(std::string error) { return {nullptr, std::move(error)}; }
constexpr int kernelPhases = 1024, kernelTaps = 12;
// Initialized before audio starts. Coefficient interpolation avoids polynomial
// products in each voice/unison/channel lookup while retaining the twelve-tap response.
const auto interpolationKernel = [] {
    std::array<std::array<float, kernelTaps>, kernelPhases + 1> kernel{};
    constexpr std::array<double, kernelTaps> reciprocal{-1.0 / 39916800, 1.0 / 3628800, -1.0 / 725760, 1.0 / 241920,
        -1.0 / 120960, 1.0 / 86400, -1.0 / 86400, 1.0 / 120960, -1.0 / 241920, 1.0 / 725760,
        -1.0 / 3628800, 1.0 / 39916800};
    for (int phase = 0; phase <= kernelPhases; ++phase)
    {
        const double mix = static_cast<double>(phase) / kernelPhases;
        std::array<double, kernelTaps + 1> prefix{}, suffix{}; prefix[0] = 1; suffix[kernelTaps] = 1;
        for (int i = 0; i < kernelTaps; ++i) prefix[static_cast<size_t>(i + 1)] = prefix[static_cast<size_t>(i)] * (mix + 5 - i);
        for (int i = kernelTaps - 1; i >= 0; --i) suffix[static_cast<size_t>(i)] = suffix[static_cast<size_t>(i + 1)] * (mix + 5 - i);
        for (size_t i = 0; i < reciprocal.size(); ++i)
            kernel[static_cast<size_t>(phase)][i] = static_cast<float>(prefix[i] * suffix[i + 1] * reciprocal[i]);
    }
    return kernel;
}();

constexpr int octaveKernelPhases = 512, octaveKernelTaps = 16;
double besselI0(double value)
{
    double sum = 1, term = 1;
    const double squared = value * value * 0.25;
    for (int order = 1; order <= 64; ++order)
    {
        term *= squared / static_cast<double>(order * order);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}
// Own standard Kaiser-windowed sinc, generated before audio starts. Static
// observations justify the geometry, not these coefficients: the reference
// kernel has a different response. Normalize each phase to unity DC gain.
const auto octaveInterpolationKernel = [] {
    std::array<std::array<float, octaveKernelTaps>, octaveKernelPhases + 1> kernel{};
    constexpr double beta = 8, cutoff = 1.125, pi = 3.14159265358979323846;
    const double denominator = besselI0(beta);
    for (int phase = 0; phase <= octaveKernelPhases; ++phase)
    {
        const double fraction = static_cast<double>(phase) / octaveKernelPhases;
        std::array<double, octaveKernelTaps> weights{};
        double total = 0;
        for (int tap = 0; tap < octaveKernelTaps; ++tap)
        {
            const double distance = fraction - static_cast<double>(tap - 7);
            if (std::abs(distance) >= 8) continue;
            const double argument = pi * cutoff * distance;
            const double sinc = std::abs(argument) < 1e-12 ? 1 : std::sin(argument) / argument;
            const double window = besselI0(beta * std::sqrt(std::max(0.0, 1 - distance * distance / 64))) / denominator;
            weights[static_cast<size_t>(tap)] = cutoff * sinc * window;
            total += weights[static_cast<size_t>(tap)];
        }
        for (size_t tap = 0; tap < weights.size(); ++tap)
            kernel[static_cast<size_t>(phase)][tap] = static_cast<float>(weights[tap] / total);
    }
    return kernel;
}();
}

WavetableLoadResult WavetableData::fromSamples(std::span<const float> samples, int numChannels,
                                             int frameLength, double sampleRate, std::string name, PlaybackProfile profile,
                                             WavetableShaping shaping)
{
    if (profile != PlaybackProfile::Dense12 && profile != PlaybackProfile::Octave16Approx && profile != PlaybackProfile::Octave16Minimax)
        return failure("Unsupported wavetable playback profile");
    if (numChannels < 1 || numChannels > maxChannels) return failure("Wavetable requires one or two channels");
    if (! supportedLength(frameLength)) return failure("Wavetable frame length must be a power of two from 64 to 2048");
    if (! std::isfinite(sampleRate) || sampleRate < 1 || sampleRate > 768000) return failure("Invalid wavetable sample rate");
    const size_t channelCount = static_cast<size_t>(numChannels), length = static_cast<size_t>(frameLength);
    if (samples.empty() || samples.size() % channelCount != 0) return failure("Wavetable sample/channel alignment is invalid");
    const size_t sampleFrames = samples.size() / channelCount;
    if (sampleFrames % length != 0) return failure("Wavetable samples do not contain complete frames");
    const size_t frames = sampleFrames / length;
    if (frames < 1 || frames > maxFrames) return failure("Wavetable frame count must be from 1 to 256");
    if (shaping.layout != WavetableFrameLayout::Continuous && shaping.layout != WavetableFrameLayout::Steps
        && shaping.layout != WavetableFrameLayout::Interpolated256) return failure("Unsupported wavetable frame layout");
    if (! std::isfinite(shaping.gain) || std::abs(shaping.gain) > 32) return failure("Invalid wavetable gain");
    float peak = 0;
    for (const float sample : samples)
    {
        if (! std::isfinite(sample) || std::abs(sample) > 32) return failure("Wavetable contains nonfinite or excessive sample values");
        peak = std::max(peak, std::abs(sample));
    }
    const float scale = shaping.normalizePeak && peak > 0 ? shaping.gain / peak : shaping.gain;
    if (std::abs(peak * scale) > 32) return failure("Wavetable gain produces excessive sample values");
    try
    {
        auto data = std::shared_ptr<WavetableData>(new WavetableData);
        data->channels_ = numChannels;
        data->frames_ = static_cast<int>(frames);
        data->frameLength_ = frameLength;
        data->sampleRate_ = sampleRate;
        data->profile_ = profile;
        data->layout_ = shaping.layout;
        if (shaping.layout == WavetableFrameLayout::Steps) data->frameCells_ = static_cast<int>(frames);
        else if (shaping.layout == WavetableFrameLayout::Interpolated256)
        {
            data->frameCells_ = maxFrames;
            data->frameStep_ = static_cast<float>(frames - 1) / static_cast<float>(maxFrames - 1);
        }
        if (profile == PlaybackProfile::Octave16Minimax)
            data->minimaxKernel_ = dsp::PreparedSampleKernel::forProfile(dsp::SampleKernelProfile::MinimaxApprox)->data();
        data->name_ = name.empty() ? "Embedded Wavetable" : name.substr(0, 1024);
        data->original_.assign(samples.begin(), samples.end());
        if (scale < 1 || scale > 1) for (auto& sample : data->original_) sample *= scale;
        if (profile != PlaybackProfile::Dense12) data->configureOctaveBands();
        else data->configureBands(6);
        const auto requiredBytes = [&] {
            return (data->original_.capacity() + channelCount * frames * data->frameStride_) * sizeof(float)
                + data->bands_.capacity() * sizeof(Band) + data->name_.capacity() + sizeof(WavetableData);
        };
        // Maximum stereo/frame-count banks use fewer upper bands; the audible lower
        // octaves retain six bands and exactly anchored cutoffs (including harmonic 90).
        if (requiredBytes() > maxBankBytes && profile == PlaybackProfile::Dense12) data->configureBands(3);
        if (requiredBytes() > maxBankBytes) return failure("Wavetable mip bank exceeds the 64 MiB limit");
        const size_t bankSamples = channelCount * frames * data->frameStride_;
        data->bank_.resize(bankSamples);
        data->generate();
        return {std::move(data), {}};
    }
    catch (const std::bad_alloc&) { return failure("Unable to allocate wavetable bank"); }
    catch (const std::length_error&) { return failure("Wavetable allocation length exceeded"); }
}

WavetableLoadResult WavetableData::fromFile(const juce::File& file, int frameLength, PlaybackProfile profile, WavetableShaping shaping)
{
    if (! supportedLength(frameLength)) return failure("Wavetable frame length must be a power of two from 64 to 2048");
    if (! file.existsAsFile()) return failure("Wavetable file does not exist");
    try
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (! reader) return failure("Wavetable audio file is unsupported or malformed");
        if (reader->numChannels < 1 || reader->numChannels > maxChannels) return failure("Wavetable file requires one or two channels");
        const int64_t maximumSamples = static_cast<int64_t>(frameLength) * maxFrames;
        if (reader->lengthInSamples <= 0 || reader->lengthInSamples > maximumSamples || reader->lengthInSamples % frameLength != 0)
            return failure("Wavetable file must contain from 1 to 256 complete frames");
        const int channels = static_cast<int>(reader->numChannels), count = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> decoded(channels, count);
        if (! reader->read(&decoded, 0, count, 0, true, channels == 2)) return failure("Unable to read wavetable audio file");
        std::vector<float> interleaved(static_cast<size_t>(channels) * static_cast<size_t>(count));
        for (int sample = 0; sample < count; ++sample)
            for (int channel = 0; channel < channels; ++channel)
                interleaved[static_cast<size_t>(sample) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] = decoded.getSample(channel, sample);
        return fromSamples(interleaved, channels, frameLength, reader->sampleRate, file.getFileNameWithoutExtension().toStdString(), profile, shaping);
    }
    catch (const std::bad_alloc&) { return failure("Unable to allocate wavetable file buffer"); }
    catch (const std::length_error&) { return failure("Wavetable file allocation length exceeded"); }
}

WavetableLoadResult WavetableData::withPlaybackProfile(PlaybackProfile profile) const
{
    return fromSamples(original_, channels_, frameLength_, sampleRate_, name_, profile, {layout_, 1, false});
}

void WavetableData::configureOctaveBands()
{
    bands_.clear(); frameStride_ = 0;
    for (int length = frameLength_; length >= 1; length /= 2)
    {
        bands_.push_back({length > 1 ? length / 2 : 0, length, frameStride_});
        frameStride_ += static_cast<size_t>(length + 16);
    }
    mipLevels_ = static_cast<int>(bands_.size());
}

void WavetableData::configureBands(int upperBandsPerOctave)
{
    bands_.clear(); frameStride_ = 0;
    const auto append = [&](int harmonics) {
        if (! bands_.empty() && harmonics >= bands_.back().harmonics) return;
        int length = minFrameLength;
        while (length < harmonics * 4 && length < maxFrameLength) length *= 2;
        bands_.push_back({harmonics, length, frameStride_});
        frameStride_ += static_cast<size_t>(length + 11); // Five preceding and six following periodic guards.
    };
    for (int upper = frameLength_ / 2; upper >= 1; upper /= 2)
    {
        const int density = upper > 256 ? upperBandsPerOctave : 6;
        for (int band = 0; band < density; ++band)
            append(std::max(1, static_cast<int>(std::floor(upper * std::exp2(-static_cast<double>(band) / density) + 1e-9))));
    }
    append(0); // No AC harmonic is legal for increments at or above Nyquist.
    mipLevels_ = static_cast<int>(bands_.size());
}
size_t WavetableData::storageBytes() const
{
    return (original_.capacity() + bank_.capacity()) * sizeof(float) + bands_.capacity() * sizeof(Band)
        + name_.capacity() + sizeof(WavetableData);
}
int WavetableData::harmonicLimit(int mip) const
{
    return bands_[static_cast<size_t>(std::clamp(mip, 0, mipLevels_ - 1))].harmonics;
}
int WavetableData::mipLength(int mip) const
{
    return bands_[static_cast<size_t>(std::clamp(mip, 0, mipLevels_ - 1))].length;
}
size_t WavetableData::offset(int channel, int frame, int mip) const
{
    return (static_cast<size_t>(channel) * static_cast<size_t>(frames_) + static_cast<size_t>(frame)) * frameStride_
        + bands_[static_cast<size_t>(mip)].offset;
}
void WavetableData::generate()
{
    if (profile_ != PlaybackProfile::Dense12)
    {
        generateOctaveBands();
        return;
    }
    int order = 0;
    for (int length = frameLength_; length > 1; length >>= 1) ++order;
    const juce::dsp::FFT fft(order);
    using Complex = std::complex<float>;
    const auto length = static_cast<size_t>(frameLength_);
    std::vector<Complex> input(length), spectrum(length), filtered(maxFrameLength), output(maxFrameLength);
    std::array<std::unique_ptr<juce::dsp::FFT>, 12> inverse;
    for (const auto& band : bands_)
    {
        int inverseOrder = 0; for (int n = band.length; n > 1; n /= 2) ++inverseOrder;
        auto& transform = inverse[static_cast<size_t>(inverseOrder)];
        if (! transform) transform = std::make_unique<juce::dsp::FFT>(inverseOrder);
    }
    for (int channel = 0; channel < channels_; ++channel)
        for (int frame = 0; frame < frames_; ++frame)
        {
            for (int sample = 0; sample < frameLength_; ++sample)
            {
                const size_t source = (static_cast<size_t>(frame) * length + static_cast<size_t>(sample)) * static_cast<size_t>(channels_) + static_cast<size_t>(channel);
                input[static_cast<size_t>(sample)] = {original_[source], 0};
            }
            fft.perform(input.data(), spectrum.data(), false);
            for (int mip = 0; mip < mipLevels_; ++mip)
            {
                const auto& band = bands_[static_cast<size_t>(mip)];
                const auto targetLength = static_cast<size_t>(band.length);
                std::fill_n(filtered.begin(), targetLength, Complex{});
                const float scale = static_cast<float>(band.length) / static_cast<float>(frameLength_);
                filtered[0] = spectrum[0] * scale;
                for (int harmonic = 1; harmonic <= band.harmonics; ++harmonic)
                {
                    const auto bin = static_cast<size_t>(harmonic);
                    if (harmonic == frameLength_ / 2 && band.length > frameLength_)
                    {
                        // A real source Nyquist bin becomes a positive/negative pair when upsampled.
                        filtered[bin] = spectrum[bin] * (scale * 0.5f);
                        filtered[targetLength - bin] = spectrum[bin] * (scale * 0.5f);
                    }
                    else
                    {
                        filtered[bin] = spectrum[bin] * scale;
                        if (bin != targetLength - bin) filtered[targetLength - bin] = spectrum[length - bin] * scale;
                    }
                }
                int inverseOrder = 0; for (int n = band.length; n > 1; n /= 2) ++inverseOrder;
                inverse[static_cast<size_t>(inverseOrder)]->perform(filtered.data(), output.data(), true);
                const size_t start = offset(channel, frame, mip);
                for (int sample = -5; sample < band.length + 6; ++sample)
                {
                    const int wrapped = (sample + band.length) % band.length;
                    bank_[start + static_cast<size_t>(sample + 5)] = output[static_cast<size_t>(wrapped)].real();
                }
            }
        }
}

void WavetableData::generateOctaveBands()
{
    int order = 0;
    for (int length = frameLength_; length > 1; length >>= 1) ++order;
    using Complex = std::complex<float>;
    const juce::dsp::FFT forward(order);
    const auto sourceLength = static_cast<size_t>(frameLength_);
    std::vector<Complex> input(sourceLength), spectrum(sourceLength), filtered(sourceLength), output(sourceLength);
    std::array<std::unique_ptr<juce::dsp::FFT>, 12> inverse;
    for (int channel = 0; channel < channels_; ++channel)
        for (int frame = 0; frame < frames_; ++frame)
        {
            for (int sample = 0; sample < frameLength_; ++sample)
            {
                const size_t source = (static_cast<size_t>(frame) * sourceLength + static_cast<size_t>(sample))
                    * static_cast<size_t>(channels_) + static_cast<size_t>(channel);
                input[static_cast<size_t>(sample)] = {original_[source], 0};
            }
            forward.perform(input.data(), spectrum.data(), false);
            for (int mip = 0; mip < mipLevels_; ++mip)
            {
                const auto& band = bands_[static_cast<size_t>(mip)];
                const auto targetLength = static_cast<size_t>(band.length);
                if (mip == 0)
                    std::copy(input.begin(), input.end(), output.begin());
                else if (band.length == 1)
                    output[0] = spectrum[0] / static_cast<float>(frameLength_);
                else
                {
                    std::fill_n(filtered.begin(), targetLength, Complex{});
                    const float scale = static_cast<float>(band.length) / static_cast<float>(frameLength_);
                    filtered[0] = spectrum[0] * scale;
                    for (int harmonic = 1; harmonic <= band.harmonics; ++harmonic)
                    {
                        const auto bin = static_cast<size_t>(harmonic);
                        if (bin == targetLength - bin)
                            filtered[bin] = (spectrum[bin] + spectrum[sourceLength - bin]) * scale;
                        else
                        {
                            filtered[bin] = spectrum[bin] * scale;
                            filtered[targetLength - bin] = spectrum[sourceLength - bin] * scale;
                        }
                    }
                    int inverseOrder = 0;
                    for (int length = band.length; length > 1; length >>= 1) ++inverseOrder;
                    auto& transform = inverse[static_cast<size_t>(inverseOrder)];
                    if (! transform) transform = std::make_unique<juce::dsp::FFT>(inverseOrder);
                    transform->perform(filtered.data(), output.data(), true);
                }
                const size_t start = offset(channel, frame, mip);
                for (int sample = -8; sample < band.length + 8; ++sample)
                {
                    int wrapped = sample % band.length;
                    if (wrapped < 0) wrapped += band.length;
                    bank_[start + static_cast<size_t>(sample + 8)] = output[static_cast<size_t>(wrapped)].real();
                }
            }
        }
}

int WavetableData::mipForIncrement(double increment) const
{
    if (! std::isfinite(increment)) return mipLevels_ - 1;
    if (profile_ != PlaybackProfile::Dense12)
    {
        const float frequency = static_cast<float>(std::abs(increment));
        if (! std::isfinite(frequency)) return mipLevels_ - 1;
        if (frequency <= 0) return 0;
        int exponent = 0;
        const float mantissa = std::frexp(frequency, &exponent);
        const int ceiling = exponent - (mantissa <= 0.5f ? 1 : 0);
        return std::clamp(mipLevels_ - 1 + ceiling, 0, mipLevels_ - 1);
    }
    const double frequency = std::abs(increment);
    int first = 0, last = mipLevels_ - 1;
    while (first < last)
    {
        const int middle = first + (last - first) / 2;
        if (frequency * bands_[static_cast<size_t>(middle)].harmonics >= 0.5) first = middle + 1;
        else last = middle;
    }
    return first;
}
float WavetableData::sample(float position, double phase, int mip, int channel) const
{
    if (! std::isfinite(position) || ! std::isfinite(phase)) return 0;
    channel = std::clamp(channel, 0, channels_ - 1);
    mip = std::clamp(mip, 0, mipLevels_ - 1);
    position = std::clamp(position, 0.0f, 1.0f);
    const float frame = frameCells_ == 0 ? position * static_cast<float>(frames_ - 1)
        : static_cast<float>(std::min(frameCells_ - 1, static_cast<int>(position * static_cast<float>(frameCells_)))) * frameStep_;
    const int first = std::min(frames_ - 1, static_cast<int>(frame)), second = std::min(first + 1, frames_ - 1);
    const float frameMix = frame - static_cast<float>(first);
    phase -= std::floor(phase);
    // A tiny negative phase can round to exactly one after subtracting floor.
    // Keep the periodic lookup inside the prepared guard range.
    if (phase >= 1) phase = 0;
    const double sample = phase * bands_[static_cast<size_t>(mip)].length;
    const auto index = static_cast<size_t>(sample);
    const float mix = static_cast<float>(sample - static_cast<double>(index));
    if (profile_ != PlaybackProfile::Dense12)
    {
        const size_t a = offset(channel, first, mip) + index + 1, b = offset(channel, second, mip) + index + 1;
        const float coefficientPosition = mix * octaveKernelPhases;
        const auto coefficientIndex = static_cast<size_t>(std::min(octaveKernelPhases - 1, static_cast<int>(coefficientPosition)));
        const float coefficientMix = coefficientPosition - static_cast<float>(coefficientIndex);
        const float* lower = minimaxKernel_ != nullptr ? minimaxKernel_ + coefficientIndex * octaveKernelTaps
            : octaveInterpolationKernel[coefficientIndex].data();
        const float* upper = minimaxKernel_ != nullptr ? lower + octaveKernelTaps
            : octaveInterpolationKernel[coefficientIndex + 1].data();
        std::array<float, 4> left{}, right{};
        for (size_t group = 0; group < 4; ++group)
            for (size_t lane = 0; lane < 4; ++lane)
            {
                const size_t tap = group * 4 + lane;
                const float weight = lower[tap] + (upper[tap] - lower[tap]) * coefficientMix;
                left[lane] += bank_[a + tap] * weight;
                right[lane] += bank_[b + tap] * weight;
            }
        const float firstValue = (left[0] + left[1]) + (left[2] + left[3]);
        const float secondValue = (right[0] + right[1]) + (right[2] + right[3]);
        return firstValue + (secondValue - firstValue) * frameMix;
    }
    const size_t a = offset(channel, first, mip) + index, b = offset(channel, second, mip) + index;
    const float coefficientPosition = mix * kernelPhases;
    const auto coefficientIndex = static_cast<size_t>(std::min(kernelPhases - 1, static_cast<int>(coefficientPosition)));
    const float coefficientMix = coefficientPosition - static_cast<float>(coefficientIndex);
    const auto& lower = interpolationKernel[coefficientIndex]; const auto& upper = interpolationKernel[coefficientIndex + 1];
    std::array<float, 4> left{}, right{};
    for (size_t group = 0; group < 3; ++group)
    {
        for (size_t lane = 0; lane < 4; ++lane)
        {
            const size_t i = group * 4 + lane;
            const float weight = lower[i] + (upper[i] - lower[i]) * coefficientMix;
            left[lane] += bank_[a + i] * weight;
            right[lane] += bank_[b + i] * weight;
        }
    }
    const float firstValue = (left[0] + left[1]) + (left[2] + left[3]);
    const float secondValue = (right[0] + right[1]) + (right[2] + right[3]);
    return firstValue + (secondValue - firstValue) * frameMix;
}

} // namespace sonora::devices
