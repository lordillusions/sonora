#pragma once

// Immutable periodic sample interpolation (DEV-18d). Prepare before publication;
// callbacks use only raw pointers to fixed shared banks, without ownership changes.

#include <array>

namespace sonora::dsp
{

enum class SampleKernelProfile { UnityKaiser = 0, MinimaxApprox = 1 };

class PreparedSampleKernel final
{
public:
    static constexpr int taps = 16, phases = 512;
    // Both fixed banks are generated before the first preparation-thread lookup.
    // Unknown enum values return nullptr; no lazy allocation/locking occurs here.
    static const PreparedSampleKernel* forProfile(SampleKernelProfile profile) noexcept;
    const float* data() const noexcept { return coefficients_.data(); } //513 consecutive16-tap rows.
    SampleKernelProfile profile() const noexcept { return profile_; }

private:
    explicit PreparedSampleKernel(SampleKernelProfile profile) noexcept;
    static const PreparedSampleKernel unityKaiser_, minimaxApprox_;
    SampleKernelProfile profile_;
    std::array<float, (phases + 1) * taps> coefficients_{};
};

} // namespace sonora::dsp
