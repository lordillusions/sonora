#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sonora::devices
{

// Independently authored, prepared 2x decimator (DEV-18d). The standard
// halfband design matches observed poles; producer lane ordering and the
// complete instrument response still require controlled runtime comparison.
class WavetableDownsampler final
{
public:
    static constexpr size_t laneCount = 8, poleCount = 10;
    using Frame = std::array<float, laneCount>;

    // Message/load thread before graph publication; the JUCE designer allocates.
    // Invalid factors leave the previous preparation intact.
    bool prepare(int rateFactor = 2);
    void reset() noexcept;
    int rateFactor() const noexcept { return factor_; }
    int integerPipelineDelay() const noexcept { return factor_ == 2 ? 2 : 0; }
    std::span<const float> alpha() const noexcept { return alpha_; }

    // Audio thread: fixed storage, no allocation/ownership/locks. First and
    // second frames are consecutive internal samples. Factor1 uses first only.
    // Lanes are independent; callers may assign Main/Bus1/Bus2/Direct L/R pairs.
    void processFrame(const Frame& first, const Frame& second, Frame& output) noexcept;

private:
    struct AllpassState { float input = 0, output = 0; };
    struct LaneState
    {
        std::array<AllpassState, poleCount / 2> first{}, second{};
        std::array<float, 2> pipeline{};
        size_t cursor = 0;
    };
    std::array<float, poleCount> alpha_{};
    std::array<LaneState, laneCount> lanes_{};
    int factor_ = 1;
};

} // namespace sonora::devices
