#pragma once
// Sonora Phase Align (PLG-21): keeps a bass in phase with the kick, dynamically. It sits on the bass track and listens to
// the kick through its sidechain input.
//
// Two signals at different pitches cannot be "in phase" everywhere; what matters is the low end where kick and bass
// overlap, while the kick sounds. So:
//
//   bass -> Linkwitz-Riley split at Crossover -> low band -> Hilbert pair (two allpass chains 90 degrees apart: A, B)
//        -> rotated low = cos(angle) A - sin(angle) B   (angle 180 = polarity flip)  -> + high band (allpass-matched)
//   kick (sidechain) -> the same low-pass -> onset detector (Threshold) and the reference for the correlation
//
// For every kick hit, over Window ms from the onset, it sums Ca = sum(k A) and Cb = sum(k B). The rotation that makes the
// rotated bass correlate best with the kick is angle = atan2(-Cb, Ca): no search. Mode Auto glides to the new angle at
// Speed; Hold keeps the angle learned so far (stops learning); Manual uses the Angle control. Coherence (0..1) tells how
// much the kick and the bass share at that moment (|C| / sqrt(sum k^2 sum bass^2)); hits under Min. Coherence are ignored.
//
// Listen: Bass (the output), Sum (bass + kick, to hear the low end together; the device's output then includes the kick)
// or Original Sum (the bass as it came in + kick, for A/B). Rotating the low band moves its phase against the high band
// around the crossover, so keep Crossover just above where kick and bass overlap. No latency: a new angle applies from the end of the hit's window, so
// repeating patterns settle within a couple of hits.
//
// Display (the face): the angle, coherence, the low-end gain each hit got (sum energy after vs before, dB), a history of
// the last hits and the waveforms of the last hit (kick, bass before, bass after), written by the audio thread into atomics.

#include "devices/NativeDevice.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sonora::devices
{
class PhaseAlign final : public NativeDevice
{
public:
    enum Param { Crossover, ModeParam, Angle, Speed, Threshold, Window, MinCoherence, Listen, Output, NumParams };
    enum Mode { Auto, Hold, Manual };
    enum ListenMode { ListenBass, ListenSum, ListenOriginalSum };
    // What the device can tell about the kick and the bass right now.
    enum Verdict { NoKick, Waiting, Silent, Unrelated, InPhase, Aligning, ManualAngle };
    static constexpr float kWorthDb = 0.25f; // a hit whose best angle adds less is "already in phase"

    PhaseAlign();
    std::string name() const override { return "Sonora Phase Align"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override { sidechain_ = audio; sidechainGain_ = gain; }
    bool takesSidechain() const override { return true; }
    std::vector<UiGroup> uiGroups() const override;

    // ---- display (message thread reads; audio thread writes)
    float currentAngle() const { return angleShown_.load(std::memory_order_relaxed); } // degrees, -180..180
    float targetAngle() const { return target_.load(std::memory_order_relaxed); }
    float lastCoherence() const { return coherence_.load(std::memory_order_relaxed); }
    float lastGainDb() const { return gainDb_.load(std::memory_order_relaxed); }         // the applied angle vs the bass as is
    float lastPotentialDb() const { return potentialDb_.load(std::memory_order_relaxed); } // the best angle vs the bass as is
    float lastBassUnderKickDb() const { return bassUnderKickDb_.load(std::memory_order_relaxed); } // bass vs kick low energy
    float processedAmount() const { return mixShown_.load(std::memory_order_relaxed); } // 0 = untouched bass, 1 = rotated
    Verdict verdict() const { return (Verdict) verdict_.load(std::memory_order_relaxed); }
    uint32_t hitCount() const { return hits_.load(std::memory_order_relaxed); }
    bool sidechainPresent() const { return sidechainPresent_.load(std::memory_order_relaxed); }

    static constexpr int kHistory = 48;
    struct HitRecord { float angle = 0, coherence = 0, gainDb = 0, potentialDb = 0, bassUnderKickDb = -120; bool used = false; };
    HitRecord history(int newestFirst) const; // 0 = the last hit

    static constexpr int kShapeBins = 160;
    struct Shape // the last hit's window: kick, bass before and after the rotation (low bands)
    {
        int bins = 0;
        std::array<float, kShapeBins> kick{}, before{}, after{};
        uint32_t serial = 0;
    };
    bool takeShape(Shape& out, uint32_t serial) const;

private:
    struct Svf { float ic1 = 0, ic2 = 0; };
    struct SvfCoeffs { float a1 = 0, a2 = 0, a3 = 0, k = 1.41421356f; void set(double sampleRate, float hz); };
    struct Lr4 { Svf lp[2], hp[2]; };
    static void split(Lr4&, const SvfCoeffs&, float x, float& low, float& high);
    struct HilbertChain { std::array<float, 4> x1{}, x2{}, y1{}, y2{}; };
    static float hilbertStep(HilbertChain&, const float (&coefficients)[4], float x);

    void finishHit();

    double sampleRate_ = 44100.0;
    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1.0f;
    SvfCoeffs crossover_;
    float crossoverHz_ = 0;
    Lr4 bassSplit_[2], kickSplit_;
    HilbertChain chainA_[2], chainB_[2];
    float delayA_[2]{}; // the A chain's output one sample late: A and B are then 90 degrees apart (Niemitalo pair)
    float fast_ = 0, slow_ = 0;
    bool armed_ = true, inHit_ = false;
    int hitSamples_ = 0, windowSamples_ = 1, sinceHit_ = 1 << 30;
    double ca_ = 0, cb_ = 0, kk_ = 0, aa_ = 0, bb_ = 0, kl_ = 0, ll_ = 0, hh_ = 0, kh_ = 0;
    double cosA_ = 1, sinA_ = 0; // current rotation
    float angle_ = 0;            // degrees, the applied one (glides)
    float learned_ = 0;          // Auto/Hold target
    // last-hit capture
    int shapeBin_ = 0, shapeCounter_ = 0, shapeSamplesPerBin_ = 1;
    float binKick_ = 0, binLow_ = 0, binA_ = 0, binB_ = 0;
    std::array<float, kShapeBins> capKick_{}, capLow_{}, capA_{}, capB_{};
    int rotationCounter_ = 0;
    float mix_ = 0, mixTarget_ = 0; // processed amount (transparent until it helps)
    uint32_t recent_ = 0;           // the last hits that had something to gain (bit 0 = the newest)
    std::atomic<int> historyNext_{0};

    std::atomic<float> angleShown_{0}, target_{0}, coherence_{0}, gainDb_{0}, potentialDb_{0}, bassUnderKickDb_{-120}, mixShown_{0};
    std::atomic<int> verdict_{NoKick};
    std::atomic<uint32_t> hits_{0};
    std::atomic<bool> sidechainPresent_{false};
    std::array<std::atomic<float>, kHistory> histAngle_{}, histCoherence_{}, histGain_{}, histPotential_{}, histBass_{};
    std::array<std::atomic<bool>, kHistory> histUsed_{};
    std::array<std::atomic<float>, kShapeBins> shapeKick_{}, shapeBefore_{}, shapeAfter_{};
    std::atomic<int> shapeBins_{0};
    std::atomic<uint32_t> shapeSerial_{0};
};
} // namespace sonora::devices
