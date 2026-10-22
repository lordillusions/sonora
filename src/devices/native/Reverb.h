#pragma once
// Reverb (.als "Reverb") — ROADMAP DEV-6 / DEV-6c.
//
// Parameter set, ids, units, enum orders and defaults follow Live 12's Reverb (schema 12.0_12402, Live's default
// device file and the 299 reverbs in 65 real-world sets; see docs/NATIVE_DELAY_REVERB.md).
//
// Signal flow (Sonora's own DSP, laid out like the sections of Live's device):
//   Input Processing   pre-delay -> low cut / high cut around BandFreq (width in octaves)
//   Early Reflections  stereo multi-tap reflections; Spin modulates the tap times; Shape sets how fast they decay and
//                      how late the diffuse tail starts
//   Diffusion Network  series all-pass diffusers (Diffusion = gain, Scale = length, Density = number of stages)
//                      -> 8-line feedback delay network (Hadamard mixing). Decay is RT60; High / Low shelves (or a
//                      low-pass for the "Cut" filter type) damp each pass; Chorus modulates the line lengths; Size
//                      scales the lines and is glided by Size Smoothing (None / Slow / Fast)
//   Freeze             sustains the network almost endlessly; Flat bypasses the shelves while frozen; Cut stops the
//                      input from adding to the frozen tail
//   Output             Stereo (0..120 %, mid/side width; optionally also on the dry signal), Reflect / Diffuse levels,
//                      Dry/Wet (MixDirect: 1.0 == fully wet, as stored on every return-track reverb in real sets)
//
// The numeric laws Live does not document are listed in docs/NATIVE_DELAY_REVERB.md; none is measured against Live audio.

#include "devices/NativeDevice.h"
#include "devices/native/Biquad.h"
#include "dsp/Dsp.h"
#include "dsp/ModLfo.h"

#include <array>

namespace sonora::devices
{

class Reverb final : public NativeDevice
{
public:
    enum Param
    {
        PreDelay,                 // ms
        BandHighOn, BandLowOn,    // measured (DEV-22b): BandHighOn = low cut (high-pass), BandLowOn = high cut (low-pass)
        BandFreq,                 // Hz
        BandWidth,                // octaves
        SpinOn, SpinRate, SpinAmount, // EarlyReflectModFreq / Depth
        Shape,                    // DiffuseDelay
        ShelfHighOn, HighFilterType, ShelfHiFreq, ShelfHiGain,
        ShelfLowOn, ShelfLoFreq, ShelfLoGain,
        ChorusOn, SizeModFreq, SizeModDepth,
        DecayTime,                // ms (RT60)
        Diffusion,                // AllPassGain
        Scale,                    // AllPassSize
        FreezeOn, FlatOn, CutOn,
        RoomSize,                 // 0.22 .. 500 (100 == reference room)
        SizeSmoothing,            // None, Slow, Fast
        StereoSeparation,         // percent, 0..120
        Density,                  // RoomType: Sparse, Low, Mid, High
        MixReflect, MixDiffuse, MixDirect,
        StereoOnDry,              // StereoSeparationOnDrySignal
        NumParams
    };

    Reverb();
    std::string name() const override { return "Reverb"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    std::vector<UiGroup> uiGroups() const override;

private:
    static constexpr int kLines = 8;
    static constexpr int kTaps = 8;
    static constexpr int kAllpass = 4;

    void updateTuning();

    double sampleRate_ = 44100.0;
    dsp::FractionalDelay preDelay_[2];
    dsp::FractionalDelay early_[2];
    dsp::FractionalDelay diffuseOnset_;
    dsp::FractionalDelay allpass_[kAllpass];
    dsp::FractionalDelay line_[kLines];

    // Derived from the parameters by updateTuning().
    std::array<float, kLines> lineTarget_{}, lineGain_{};      // samples, feedback gain
    std::array<float, kLines> lineNow_{};                      // smoothed lengths in samples
    std::array<float, kTaps> tapTimeL_{}, tapTimeR_{}, tapGain_{};
    std::array<float, kAllpass> allpassLen_{};
    int allpassStages_ = 3;
    float diffuseOnsetSamples_ = 0.0f;
    float spinDepthSamples_ = 0.0f;
    float sizeCoef_ = 1.0f;       // per-sample glide of the line lengths
    float hiCoef_ = 0, loCoef_ = 0;
    bool frozen_ = false;

    std::array<float, kLines> lp1_{}, lp2_{}, lo_{};           // damping filter states
    dsp::Svf inLow_[2], inHigh_[2];
    dsp::ModLfo chorusLfo_, spinLfo_;
    float reflectNow_ = 1.0f, diffuseNow_ = 1.0f, wetNow_ = 0.5f, widthNow_ = 1.0f, injectNow_ = 1.0f;
    float wetCalibration_ = 1.0f, reflectCalibration_ = 1.0f; // measured level laws (DEV-22b), see updateTuning()
    BiquadCoeffs wetHighPass_{};  // measured: the reference's wet signal always rolls off below ~80 Hz
    BiquadState wetHighPassState_[2];
    bool tuned_ = false;
};

} // namespace sonora::devices
