#pragma once
// Sonora Correlation (PLG-24): a stereo correlation meter, the built-in device and the VST3. It measures how much the left
// and right channels have in common and where, and says in words whether the mix survives a mono sum. Audio passes
// through untouched (bit-exact) unless Listen is set to a check mode (Mono, Side, Left, Right).
//
//   correlation  = sum(L R) / sqrt(sum(L^2) sum(R^2)) over an exponential window (Integration): +1 = mono, 0 = unrelated,
//                  -1 = one side inverted (cancels in mono)
//   per band     = the same on 31 third-octave bands (ISO centres 20 Hz .. 20 kHz; each band two cascaded band-passes,
//                  -3 dB at the third-octave edges), each band's window at least 8 periods of its centre
//   low end      = the bands up to 160 Hz together (where a mono sum hurts most: club systems, phones)
//   mono sum     = energy of (L+R)/2 against the mean of L and R: 0 dB = nothing lost, -3 dB = unrelated sides, -inf = inverted
//   width        = side / mid amplitude: 0 = mono, 1 = as much side as mid (unrelated), > 1 more side than mid
//   balance      = R against L in dB (RMS)
//
// The audio thread publishes readings ~40 times a second through atomics: the readings, held extremes (Hold), an 8 s
// history and per-band values; and every sample into a ring the scope reads (x = (R - L)/sqrt 2, y = (L + R)/sqrt 2: mono
// is a vertical line, left-only leans left, inverted is horizontal). Nothing on the audio thread allocates or locks.

#include "devices/NativeDevice.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace sonora::devices
{
class Correlation final : public NativeDevice
{
public:
    enum Param { Integration, Hold, ScopeMode, ScopeAuto, ScopeZoom, Listen, NumParams };
    enum HoldTime { Hold1s, Hold3s, Hold10s, HoldForever };
    enum Scope { Lissajous, Polar };
    enum ListenMode { ListenStereo, ListenMono, ListenSide, ListenLeft, ListenRight };

    static constexpr int kBands = 31;              // ISO third-octave centres, 20 Hz .. 20 kHz
    static constexpr float kLowEndHz = 160.0f;     // the bands up to here are "the low end"
    static constexpr int kHistory = 320;           // history points, kHistoryRate per second (8 s)
    static constexpr float kHistoryRate = 40.0f;
    static constexpr int kScopePoints = 8192;      // newest samples kept for the scope
    static constexpr float kSilenceDb = -70.0f;    // below this (RMS) nothing is measured
    static constexpr float kBandFloorDb = -84.0f;  // a band below this has no signal
    static float bandCentre(int band);             // Hz
    static std::string bandLabel(int band);        // "20", "31.5", "1k", "12.5k"

    Correlation();
    std::string name() const override { return "Sonora Correlation"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    std::vector<UiGroup> uiGroups() const override;

    // ---- display (any thread reads; the audio thread writes)
    struct Reading
    {
        bool signal = false;               // the input is above kSilenceDb
        float correlation = 0;             // -1 .. +1 (0 while silent)
        float minHeld = 0, maxHeld = 0;    // extremes over the Hold time
        bool lowSignal = false;            // the low end has something to measure
        float lowEnd = 0;                  // correlation of the bands up to kLowEndHz
        float balanceDb = 0;               // R against L (+ = right louder)
        float width = 0;                   // side / mid amplitude
        float monoDb = 0;                  // the mono sum against the stereo mean (<= 0)
        float levelL = -120, levelR = -120; // RMS dBFS over the window
        float peakL = -120, peakR = -120;   // sample peaks since the last reading, dBFS
        bool frozen = false;
    };
    Reading reading() const;

    struct Band
    {
        bool active = false;   // inside the sample rate's range
        bool signal = false;   // above kBandFloorDb
        float correlation = 0, minHeld = 0;
        float levelDb = -120;  // mean of L and R energy, dBFS
    };
    Band band(int index) const;

    struct HistoryPoint { bool signal = false, lowSignal = false; float correlation = 0, lowEnd = 0; };
    HistoryPoint history(int newestFirst) const; // 0 = now
    uint32_t historyCount() const { return historyCount_.load(std::memory_order_acquire); }

    // The newest `count` scope samples, oldest first; returns how many there were (fewer before the ring fills).
    int scope(float* x, float* y, int count) const;
    uint64_t samplesSeen() const { return written_.load(std::memory_order_acquire); }

    void requestReset() { resetRequested_.store(true, std::memory_order_relaxed); } // holds and history, next block
    void setFrozen(bool f) { frozen_.store(f, std::memory_order_relaxed); }         // stops the readings (audio passes)
    bool frozen() const { return frozen_.load(std::memory_order_relaxed); }

private:
    struct Biquad { double b0 = 0, b2 = 0, a1 = 0, a2 = 0; };   // band-pass: b1 = 0, b2 = -b0
    struct State { double z1 = 0, z2 = 0; };
    struct BandState
    {
        Biquad c;
        State l[2], r[2];
        double ll = 0, rr = 0, lr = 0, keep = 0, take = 0;
        float minHeld = 1, holdLeft = 0;
        bool active = false;
    };
    static double run(const Biquad&, State&, double x);
    void setCoefficients();
    void publish();
    void clearHolds();

    double sampleRate_ = 44100.0;
    float integrationMs_ = -1;
    double keep_ = 0, take_ = 1;
    double ll_ = 0, rr_ = 0, lr_ = 0;
    float peakL_ = 0, peakR_ = 0;
    int sinceReading_ = 0, readingInterval_ = 1000;
    std::array<BandState, kBands> bands_{};
    float minHeld_ = 1, maxHeld_ = -1, minHold_ = 0, maxHold_ = 0; // held values and seconds left on each
    int historyNext_ = 0;
    int listenFrom_ = ListenStereo, listenTo_ = ListenStereo, fadeLeft_ = 0, fadeLen_ = 1;

    std::atomic<bool> resetRequested_{false}, frozen_{false};
    std::atomic<bool> signal_{false}, lowSignal_{false};
    std::atomic<float> correlation_{0}, minShown_{0}, maxShown_{0}, lowEnd_{0}, balance_{0}, width_{0}, mono_{0}, levelL_{-120},
        levelR_{-120}, peakShownL_{-120}, peakShownR_{-120};
    std::array<std::atomic<float>, kBands> bandCorrelation_{}, bandMin_{}, bandLevel_{};
    std::array<std::atomic<bool>, kBands> bandActive_{};
    std::array<std::atomic<float>, kHistory> historyCorrelation_{}, historyLow_{};
    std::array<std::atomic<uint8_t>, kHistory> historyFlags_{};
    std::atomic<int> historyHead_{0};
    std::atomic<uint32_t> historyCount_{0};
    std::array<std::atomic<float>, kScopePoints> scopeX_{}, scopeY_{};
    uint64_t writtenLocal_ = 0;
    std::atomic<uint64_t> written_{0};
};
} // namespace sonora::devices
