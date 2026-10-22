#pragma once
// ITU-R BS.1770-4 / EBU R128 loudness measurement (momentary, short-term, integrated, loudness range,
// sample and true peak). By default not audio-thread safe (it allocates while it grows its history): feed it from
// an analysis thread or an offline render loop. setRealtime(true) makes process() / reset() allocation free for meter
// devices on the audio thread. JUCE free so it is unit tested against reference signals.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonora::dsp
{

constexpr float kSilenceDb = -200.0f; // reported for "no signal yet"

class LoudnessMeter
{
public:
    explicit LoudnessMeter(double sampleRate = 48000.0) { prepare(sampleRate); }

    // (Re)starts the measurement at a sample rate; forgets everything measured so far.
    void prepare(double sampleRate);
    // Forgets the integrated loudness, loudness range and peaks; keeps the sample rate.
    void reset();
    // Realtime display mode (call before prepare()): nothing allocates after prepare(); the integrated loudness is
    // gated from a fixed 0.05 LU histogram instead of the block list; the loudness range is not measured (0); and the
    // windows start full of silence, so momentary / short-term read from the first 100 ms hop and ramp up like a
    // hardware meter instead of staying silent for 0.4 / 3 s.
    void setRealtime(bool on) { realtime_ = on; }

    // Feeds a block of stereo audio. Mono callers pass the same pointer twice.
    void process(const float* left, const float* right, int numSamples);

    // LUFS. kSilenceDb until the window has been filled once (400 ms for momentary, 3 s for short-term).
    float momentary() const { return momentary_; }
    float shortTerm() const { return shortTerm_; }
    float integrated() const; // gated (-70 LUFS absolute, -10 LU relative); kSilenceDb until a block passes the gate
    float loudnessRange() const; // LU, EBU Tech 3342 (-70 absolute, -20 LU relative gate, 10th..95th percentile)

    // dBFS maxima since reset(): sample peak and 4x oversampled true peak.
    float samplePeakDb() const;
    float truePeakDb() const;
    float momentaryMax() const { return momentaryMax_; }
    // LUFS of one channel's own 400 ms window (per-channel meters); same start rules as momentary().
    float momentaryChannel(int channel) const { return momentaryChannel_[(size_t) (channel != 0)]; }
    // Number of closed 100 ms hops since reset(); readers poll it to know when the values changed.
    uint64_t hops() const { return hops_; }

    double sampleRate() const { return sampleRate_; }

    static float powerToLufs(double power);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    static constexpr int kTaps = 48, kPhases = 4, kPerPhase = kTaps / kPhases;
    static constexpr int kHistogramBins = 1600; // -70 .. +10 LUFS in 0.05 LU steps (realtime mode)
    static constexpr float kHistogramStep = 0.05f;

    void design();
    void closeBlock();
    void trackTruePeak(int channel, float x);

    double sampleRate_ = 48000.0;
    int hopSamples_ = 4800;
    std::array<std::array<Biquad, 2>, 2> filters_{}; // [channel][stage]

    // Mean square (summed over channels) of each 100 ms sub-block; the last 30 cover short-term.
    std::array<double, 30> recent_{};
    int recentCount_ = 0, recentPos_ = 0;
    double hopSum_ = 0;
    int hopFill_ = 0;

    bool realtime_ = false;
    uint64_t hops_ = 0;
    std::array<double, 2> hopSumChannel_{};
    std::array<std::array<double, 4>, 2> recentChannel_{}; // per-channel mean squares of the last 4 sub-blocks
    std::array<float, 2> momentaryChannel_{-200.0f, -200.0f};
    std::array<double, kHistogramBins> histogramPower_{}; // realtime mode: summed block power per loudness bin
    std::array<uint32_t, kHistogramBins> histogramCount_{};

    std::vector<float> blockPowers_;     // every 400 ms block (100 ms hop) for the integrated measurement
    std::vector<float> shortTermPowers_; // every 3 s window (100 ms hop) for the loudness range
    float momentary_ = -200.0f, shortTerm_ = -200.0f, momentaryMax_ = -200.0f;
    float samplePeak_ = 0, truePeak_ = 0;

    std::array<float, kTaps> firPhases_{};      // windowed-sinc 4x interpolator, polyphase order [phase][tap]
    std::array<std::array<float, kPerPhase>, 2> history_{};
    int historyPos_ = 0;
};

} // namespace sonora::dsp
