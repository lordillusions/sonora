#pragma once
// Utility (.als tag "StereoGain").
// Implemented: Gain, Balance, Stereo Width, Mono, Mute, Phase invert L/R, Channel mode.
// Bass Mono (Linkwitz-Riley crossover) with audition, DC filter and the Mid/Side balance are included. The width knob
// is either Width or Mid/Side (MidSideBalanceOn), as in the reference.
// Not yet: LegacyGain handling (old sets store the gain there).

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"

namespace sonora::devices
{

class Utility final : public NativeDevice
{
public:
    enum Param
    {
        Gain,         // linear, 0..~56.2 (+35 dB)
        Balance,      // -1..1
        StereoWidth,  // 0..4 (1 == 100 %)
        ChannelMode,  // 0 Left, 1 Stereo, 2 Right, 3 Swap
        Mono,
        Mute,
        PhaseInvertL,
        PhaseInvertR,
        MidSideBalance, // 0..2, 1 == neutral
        BassMono,
        BassMonoFrequency, // Hz
        DcFilter,
        MidSideOn,        // plain leaf: the width knob is the Mid/Side balance (StereoWidth is then ignored)
        BassMonoAudition, // plain leaf: output only the summed low band
        NumParams
    };

    Utility();

    std::string name() const override { return "Utility"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;

private:
    juce::SmoothedValue<float> gainL_, gainR_;
    double sampleRate_ = 44100.0;
    SvfCoeffs lp_, hp_; // state-variable form: Bass Freq moves must not click (DEV-24)
    float lastBassFreq_ = -1.0f;
    SvfState lowL_[2], lowR_[2], highL_[2], highR_[2]; // two stages each == Linkwitz-Riley 4
    float dcXL_ = 0, dcYL_ = 0, dcXR_ = 0, dcYR_ = 0;
};

} // namespace sonora::devices
