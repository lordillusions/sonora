#include "NoiseSampleVoice.h"
#include "OscillatorMedia.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
NoiseSampleVoice::NoiseSampleVoice() noexcept
    : kernel_(dsp::PreparedSampleKernel::forProfile(dsp::SampleKernelProfile::UnityKaiser)->data()) {}

void NoiseSampleVoice::reset() noexcept
{
    sample_ = nullptr;
    pcm_ = nullptr;
    frames_ = 0;
    channels_ = 0;
    position_ = 0;
    rateRatio_ = 0;
}

bool NoiseSampleVoice::start(const OscillatorSample* sample, double outputSampleRate, double normalizedPhase,
                            dsp::SampleKernelProfile kernelProfile) noexcept
{
    reset();
    const auto* preparedKernel = dsp::PreparedSampleKernel::forProfile(kernelProfile);
    if (preparedKernel == nullptr) return false;
    if (sample == nullptr || ! std::isfinite(outputSampleRate) || outputSampleRate < 1 || outputSampleRate > maxSampleRate
        || ! std::isfinite(normalizedPhase)) return false;
    const int frames = sample->sampleFrames(), channels = sample->channels();
    const double sourceRate = sample->sampleRate();
    if (frames < 1 || channels < 1 || channels > 2 || ! std::isfinite(sourceRate) || sourceRate < 1 || sourceRate > maxSampleRate) return false;
    const auto& samples = sample->originalSamples();
    if (samples.size() != static_cast<size_t>(frames) * static_cast<size_t>(channels)) return false;
    sample_ = sample;
    pcm_ = samples.data();
    frames_ = frames;
    channels_ = channels;
    kernel_ = preparedKernel->data();
    position_ = std::min(std::clamp(normalizedPhase, 0.0, 1.0) * frames, static_cast<double>(frames - 1));
    rateRatio_ = sourceRate / outputSampleRate;
    return true;
}

dsp::SampleKernelProfile NoiseSampleVoice::kernelProfile() const noexcept
{
    return kernel_ == dsp::PreparedSampleKernel::forProfile(dsp::SampleKernelProfile::MinimaxApprox)->data()
        ? dsp::SampleKernelProfile::MinimaxApprox : dsp::SampleKernelProfile::UnityKaiser;
}

NoiseSampleVoice::Stereo NoiseSampleVoice::next(double playbackRatio) noexcept
{
    if (! active() || ! std::isfinite(playbackRatio)) return {};
    const int base = static_cast<int>(position_);
    const double scaledPhase = (position_ - base) * interpolationPhases;
    const int phase = std::clamp(static_cast<int>(scaledPhase), 0, interpolationPhases - 1);
    const float mix = static_cast<float>(scaledPhase - phase);
    const float* first = kernel_ + phase * interpolationTaps;
    const float* second = first + interpolationTaps;
    Stereo result;
    for (int tap = 0; tap < interpolationTaps; ++tap)
    {
        int frame = base + tap - 7;
        if (frames_ >= interpolationTaps)
        {
            if (frame < 0) frame += frames_;
            else if (frame >= frames_) frame -= frames_;
        }
        else
        {
            frame %= frames_;
            if (frame < 0) frame += frames_;
        }
        const float weight = first[tap] + (second[tap] - first[tap]) * mix;
        const auto at = static_cast<size_t>(frame) * static_cast<size_t>(channels_);
        result.left += pcm_[at] * weight;
        if (channels_ == 2) result.right += pcm_[at + 1] * weight;
    }
    if (channels_ == 1) result.right = result.left;
    position_ += rateRatio_ * std::clamp(playbackRatio, -maxPlaybackRatio, maxPlaybackRatio);
    if (position_ < 0 || position_ >= frames_)
    {
        position_ = std::fmod(position_, frames_);
        if (position_ < 0) position_ += frames_;
        // Adding a tiny negative remainder can round to exactly frameCount.
        if (position_ >= frames_) position_ = 0;
    }
    return result;
}

} // namespace sonora::devices
