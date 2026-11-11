#pragma once
// Sonora Hush (PLG-14): a dynamic resonance suppressor. Every hop it analyses the spectrum, finds bins that stand out
// from their neighbourhood (soft mode: only relative to an adaptive local envelope, so input level does not matter;
// hard mode: also against an absolute threshold set by depth), spreads that excess over a frequency width set by
// Detail, turns it into a per-bin reduction shaped by the depth curve, smooths it with attack/release, and applies it as
// a minimum-phase (or linear-phase) filter to the delayed input.
//
// Clean-room design: the behaviour was measured black-box on a licensed copy of the reference plug-in (notch depth and
// width against depth/detail/frequency/level, time constants, band shapes, latencies; docs/HUSH.md) and the constants
// here are fitted to those measurements. No code, presets or assets of any other product.
//
// Latency: 2304 samples at 44.1/48 kHz (x2 at 88.2/96, x4 above); linear phase 3456 (same scaling); low latency mode 0
// (no lookahead, a short time-domain filter updated every hop).

#include "devices/NativeDevice.h"
#include "devices/native/HushCurve.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace sonora::devices
{

class Hush : public NativeDevice // HushDevice (HushSession.h) adds the face's session
{
public:
    enum Param
    {
        Mode, // soft, hard
        Depth,
        Detail,
        Attack,
        Release,
        StereoMode, // L/R, M/S
        Link,       // 0..100 %
        Focus,      // 0..1, 0.5 = even
        DetailTiltLow,
        DetailTiltHigh, // tilts: multipliers 0.1..10
        AttackTiltLow,
        AttackTiltHigh,
        ReleaseTiltLow,
        ReleaseTiltHigh,
        MaxCut,  // dB, 24 = off
        WetTrim, // dB
        Bypass,
        Delta,
        Mix,     // 0..100 %
        OutTrim, // dB
        Quality, // normal, high, ultra
        LinearPhase,
        LowLatency,
        Sidechain,
        SidechainListen,
        BandListen, // 0 = off, 1..8 = band
        BandStart
    };
    enum BandField
    {
        BandUsed,
        BandEnabled,
        BandShape,
        BandFrequency,
        BandDepth,
        BandQ,
        BandSlope,
        BandFocus,
        kBandFields
    };
    static constexpr int kBands = hush::kBands;
    static constexpr int kParams = BandStart + kBands * kBandFields;
    static constexpr int bandParam(int band, int field) { return BandStart + band * kBandFields + field; }
    static constexpr float kMaxCutOff = 24.0f;

    static constexpr int kDisplayPoints = 320; // reduction graph, log-spaced 20 Hz .. 20 kHz
    static float displayFrequency(int point);

    Hush();
    std::string name() const override { return "Sonora Hush"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override;
    bool takesSidechain() const override { return true; }
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    // Latency for a configuration (the plug-in tells the host when it changes).
    static int latencyFor(double sampleRate, bool linearPhase, bool lowLatency);
    int currentLatency() const; // from the current parameter values
    double sampleRate() const { return sampleRate_; }
    // Settings shared by every instance: quality forced while rendering offline (-1 = none), low latency for all.
    void setOverrides(int quality, bool lowLatency);
    bool lowLatencyActive() const;
    int activeQuality() const;

    // Band listen plays the band's share of the delta (true) or the band region solo (false). Settings menu.
    std::atomic<bool> bandListenDelta{true};

    // Current bands (any thread; reads atomics).
    std::array<hush::Band, kBands> bands() const;
    void setBand(int index, const hush::Band&); // message thread (editor)

    // Reduction graph (message thread): dB of reduction (>= 0, mix included) per display point for processing channel 0
    // (left/mid) and 1 (right/side), and the largest reduction per channel since the last hop.
    float displayReduction(int channel, int point) const
    {
        return display_[(size_t) (channel & 1)][(size_t) point].load(std::memory_order_relaxed);
    }
    float maxReduction(int channel) const { return maxReduction_[(size_t) (channel & 1)].load(std::memory_order_relaxed); }
    unsigned displayRevision() const { return displayRevision_.load(std::memory_order_relaxed); }

    // Fitted constants (docs/HUSH.md "Calibration"). Public so tests and the calibration harness can read them.
    struct Tuning
    {
        float softGain = 0.1301f, softCurve = 0.0464f; // drive per depth unit, and its growth with depth
        float hardGain = 0.0602f, hardCurve = 0.045f, hardLevel = 0.1925f, hardThreshold = -5.441f, hardThresholdSlope = 6.3f;
        float knee = 9.0f;                             // compressive law R = K (sqrt(1 + 2u/K) - 1)
        float envelopeRatio = 3.385f;                    // local envelope width / spread width
        float floorDb = -100.0f, floorRange = 80.0f;   // ignore bins this far under the loudest
        float excessFloor = 7.359f;                      // dB of excess that plain noise shows anyway
        float excessFloorSlope = 0.0678f;                 // dB the floor drops per depth step above 5 (rises below)
        float widthScale = 0.8f;                       // kernel width / measured notch width
        float widthComp = 0.5f;                        // 0..1: how much of the f^0.55 width growth is compensated
        float curveBoost = 1.0f, curveCut = 1.15f;     // drive dB per depth-curve dB (boosts, cuts)
        float curveSaturation = 10.0f;                 // drive at which a boost has half its effect
        float temporalGain = 0.0206f, temporalTime = 0.3f, temporalFloor = 3.629f, temporalFloorSlope = 8.298f; // envelope rise
        float envelopePower = 0.4947f; // 0..1: local envelope as a dB mean (0) or a power mean (1); dense harmonics raise the latter
        // Built-in frequency emphasis of the drive at emphasisHz (log-interpolated), relative to 1 kHz. These and the
        // gains above were refitted on 2026-10-28 against the reference on real stems as well as test tones (docs/HUSH.md).
        std::array<float, 17> emphasisGain{1.055f, 1.385f, 0.967f, 0.9535f, 0.9646f, 0.988f, 1.104f, 1.301f, 1.793f, 2.607f, 3.728f, 3.613f, 3.782f, 4.4f, 4.49f, 3.596f, 2.66f};
    };
    static constexpr std::array<float, 17> emphasisHz{20, 50, 100, 200, 300, 500, 700, 1000, 1400, 2000, 2800, 4000,
                                                      5600, 8000, 12000, 16000, 24000};
    static Tuning& tuning();

private:
    struct Config
    {
        int factor = 1, analysisSize = 4096, analysisWindow = 2560, hop = 512, synthHop = 128, frame = 512;
        int convSize = 4096, filterLength = 2048, latency = 2304;
        bool linear = false, lowLatency = false;
        bool operator==(const Config&) const = default;
    };
    Config configFor(int quality, bool linear, bool lowLatency) const;
    void updateDerived();
    void runHop(long long t);
    void analyse(long long t);
    void computeReduction();
    void designMinimumPhase(const float* gain, float* impulse, int length);
    void designLinearPhase(const float* gain, float* impulse, int length);
    void spectrumOf(const float* impulse, int length, float* spectrum);
    void synthesise(long long t);
    float lowLatencyFilter(int channel, float x);

    juce::dsp::FFT& fft(int size);

    double sampleRate_ = 44100.0;
    int factor_ = 1, maxBlock_ = 512, bins_ = 2049;
    Config cfg_;
    float forwardScale_ = 1.0f, inverseScale_ = 1.0f;
    std::array<std::unique_ptr<juce::dsp::FFT>, 17> ffts_;

    // Ring buffers indexed by absolute sample count & ringMask_.
    int ringSize_ = 1 << 15, ringMask_ = (1 << 15) - 1;
    std::array<std::vector<float>, 2> mainRing_, detRing_, outRing_;
    long long count_ = 0;
    bool primed_ = false;

    // Per hop.
    std::vector<float> analysisWindow_, synthWindow_, fftA_, fftB_, impulse_;
    std::array<std::vector<float>, 2> power_, excess_, spread_, level_, reduction_, smoothed_, gain_, spec_, slowEnvelope_;
    std::vector<double> prefix_;
    std::vector<float> envPower_, envPowerMean_;
    std::vector<float> binHz_, attackCoef_, releaseCoef_, spreadBins_, widthComp_, emphasis_, curveWeight_[2], curveDb_[2], listenRegion_;
    std::array<bool, 2> flat_{true, true}, identity_{true, true};

    // Low latency: short time-domain filters, crossfaded over a hop.
    int llTaps_ = 512;
    std::array<std::vector<float>, 2> llHistory_, llCoef_, llPrev_;
    int llPos_ = 0, llFade_ = 0;

    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1.0f;
    juce::SmoothedValue<float> mix_, wetGain_, outGain_, bypassMix_;

    std::array<std::array<std::atomic<float>, kDisplayPoints>, 2> display_{};
    std::array<std::atomic<float>, 2> maxReduction_{};
    std::atomic<unsigned> displayRevision_{0};
    std::atomic<int> latency_{2304};
    std::atomic<int> forceQuality_{-1};
    std::atomic<bool> forceLowLatency_{false};
    std::vector<int> displayLo_, displayHi_;
};

} // namespace sonora::devices
