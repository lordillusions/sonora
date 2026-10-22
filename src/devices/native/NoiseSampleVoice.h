#pragma once

#include "dsp/PreparedSampleKernel.h"

// Periodic PCM noise playback (DEV-18d). This is an independently authored
// interpolation path; matching the proprietary noise kernel is not established.

namespace sonora::devices
{

class OscillatorSample;

class NoiseSampleVoice final
{
public:
    static constexpr int interpolationTaps = 16, interpolationPhases = 512;
    static constexpr double maxSampleRate = 768000, maxPlaybackRatio = 4096;
    struct Stereo { float left = 0, right = 0; };

    // Construct before graph publication: the shared standard interpolation bank
    // is generated off audio. A voice owns no PCM and never retains a shared_ptr.
    NoiseSampleVoice() noexcept;
    // The prepared graph must retain this immutable sample until reset/destruction.
    // Phase is clamped to0..1, then min(phase * frameCount, frameCount - 1).
    // This endpoint is an own bounded convention, not an admitted vendor mapping.
    // Invalid sample/rate/phase clears the voice and returns false. Rates1..768000.
    bool start(const OscillatorSample* sample, double outputSampleRate, double normalizedPhase = 0,
               dsp::SampleKernelProfile kernelProfile = dsp::SampleKernelProfile::UnityKaiser) noexcept;
    void reset() noexcept;
    // Source PCM is always periodic. Finite ratio is clamped to±4096; negative values
    // run backwards and0 holds the current sample. Nonfinite ratio returns silence
    // without moving. This kernel does not add adaptive antialiasing for large ratios.
    Stereo next(double playbackRatio = 1) noexcept;
    bool active() const noexcept { return sample_ != nullptr; }
    double position() const noexcept { return position_; } // Source frame coordinates.
    const OscillatorSample* sample() const noexcept { return sample_; }
    double sourceFramesPerOutputSample() const noexcept { return rateRatio_; }
    dsp::SampleKernelProfile kernelProfile() const noexcept;

private:
    const OscillatorSample* sample_ = nullptr;
    const float* pcm_ = nullptr;
    const float* kernel_ = nullptr;
    int frames_ = 0, channels_ = 0;
    double position_ = 0, rateRatio_ = 0;
};

} // namespace sonora::devices
