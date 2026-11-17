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
// Kick x Bass (PLG-24c, Mode): does the bass reinforce or cancel the kick where they meet? Put it on the bass track with the
// kick routed to its sidechain (or on a group with the kick panned left and the bass right: Kick From = Left). Both are
// low-passed below kLowEndHz and compared only where they sound together:
//
//   weight       = from rho = 2 |k b| / (k^2 + b^2), numerator and denominator each smoothed over 10 ms (1 = as loud as
//                  each other, ~0 = only one of them sounds; no memory of a kick that stopped before the bass came in):
//                  0 while the quieter one is ~30 dB down, 1 from ~12 dB down
//   correlation  = sum(w k b) / sqrt(sum(w k^2) sum(w b^2)) over Integration: +1 they add up where they meet, -1 they
//                  cancel, 0 different pitches (neither)
//   shared       = sum(w (k^2 + b^2)) / sum(k^2 + b^2): how much of the low end sounds together (psytrance K-B-B-B barely)
//   sum / bass inverted = the energy of k + b (k - b) where they meet against the two apart, in dB (+3 = equal and in
//                  phase, -inf = equal and inverted)
//   best shift   = the delay of the bass (+-12 ms) that would correlate best, from a weighted cross-correlation
// The bands then compare the kick with the bass too (weighted the same way), the meter, holds and history follow the kick x
// bass correlation, and a ring keeps both low-passed signals at ~3 kHz for the face's beat view (from the last kick onset).
// Listen Bass Inverted flips the bass's polarity to hear the alternative with the kick track playing.
//
// Integration can follow the song's tempo (Sync): a note value from 1/64 to 2 bars (1/4 = one beat, a kick and its bass).
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
    // New parameters go at the end: saved states and the VST3's host parameters keep their indices.
    enum Param { Integration, Hold, ScopeMode, ScopeAuto, ScopeZoom, Listen, Mode, KickFrom, Sync, IntegrationNote, NumParams };
    enum HoldTime { Hold1s, Hold3s, Hold10s, HoldForever };
    enum Scope { Lissajous, Polar };
    // Bass Inverted: the bass's polarity flipped (Kick From Sidechain: the whole input; Left: the right channel).
    enum ListenMode { ListenStereo, ListenMono, ListenSide, ListenLeft, ListenRight, ListenBassInverted };
    enum Measure { MeasureStereo, MeasureKickBass };
    enum KickSource { KickSidechain, KickLeft };
    static constexpr int kNotes = 8;               // IntegrationNote: 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars
    static double noteBeats(int note, int numerator = 4, int denominator = 4); // quarter-note beats
    static std::string noteLabel(int note);
    static constexpr float kMaxShiftMs = 12.0f;    // the best-shift search: +- this
    static constexpr float kMinShared = 0.01f;     // below this share of the low end, kick and bass "take turns"

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
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    // PLG-24c: the kick arrives through the sidechain (the app's Sidechain section, the VST3's sidechain bus).
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override { sidechain_ = audio; sidechainGain_ = gain; }
    bool takesSidechain() const override { return true; }
    std::vector<UiGroup> uiGroups() const override;
    // The window the readings average over now, in ms (Integration, or the note value at the song's tempo).
    float integrationMs() const { return integrationShown_.load(std::memory_order_relaxed); }
    double tempo() const { return tempoShown_.load(std::memory_order_relaxed); }

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

        // Kick x Bass (Mode). Then `signal`/`correlation`/held values are the kick x bass reading where they overlap, the
        // levels are the kick's (L) and the bass's (R), and the bands compare the kick with the bass.
        bool kickBass = false;
        bool sidechain = false;            // Kick From Sidechain: a sidechain was delivered
        bool kick = false, bass = false;   // each has sound in the low end
        bool overlap = false;              // they share enough to measure (shared >= kMinShared)
        float shared = 0;                  // 0..1 of the low end's energy sounds together
        float sumDb = 0, invertedDb = 0;   // where they meet: kick + bass, kick - bass, against the two apart
        float shiftMs = 0;                 // delaying the bass by this (negative: earlier) would correlate best
        float shiftCorrelation = 0;        // ... at this correlation
        float kickDb = -120, bassDb = -120; // low-end RMS dBFS
    };
    Reading reading() const;

    struct Band
    {
        bool active = false;   // inside the sample rate's range
        bool signal = false;   // above kBandFloorDb
        bool measured = false; // has signal and (Kick x Bass) the kick and the bass meet in it
        float correlation = 0, minHeld = 0;
        float levelDb = -120;  // mean of L and R energy, dBFS
    };
    Band band(int index) const;

    struct HistoryPoint { bool signal = false, lowSignal = false; float correlation = 0, lowEnd = 0, shared = 0; };
    HistoryPoint history(int newestFirst) const; // 0 = now
    uint32_t historyCount() const { return historyCount_.load(std::memory_order_acquire); }

    // The newest `count` scope samples, oldest first; returns how many there were (fewer before the ring fills).
    int scope(float* x, float* y, int count) const;
    uint64_t samplesSeen() const { return written_.load(std::memory_order_acquire); }

    // Kick x Bass beat view: the low-passed kick and bass (and the overlap weight) from `preMs` before the last complete kick
    // onset for `spanMs` (one beat at the song's tempo when 0); oldest first, at most `count` points. Returns how many were
    // written (0 before there is anything), and the ms between points. Without onsets: the newest span.
    int beat(float* kick, float* bass, float* weight, int count, float preMs, float spanMs, float& msPerPoint, bool& fromOnset) const;
    static constexpr int kBeatPoints = 8192; // ~2.7 s at ~3 kHz

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
        double wll = 0, wrr = 0, wlr = 0; // Kick x Bass: weighted by the overlap
        float minHeld = 1, holdLeft = 0;
        bool active = false;
    };
    struct Iir // a direct-form II transposed biquad (the Kick x Bass low end)
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
    static constexpr int kMaxLag = 48;          // decimated samples each way (12 ms at up to 4 kHz)
    static constexpr int kLagSlots = 2 * kMaxLag + 1;
    static constexpr int kOnsetLook = 64;       // decimated samples of kick envelope kept for the onset rule
    static double run(const Biquad&, State&, double x);
    void setCoefficients(float ms);
    void publish();
    void clearHolds();
    void clearMeasurements();
    float wantedIntegrationMs() const;
    void pairStep(double kick, double bass, bool frozen); // at the decimated rate

    double sampleRate_ = 44100.0;
    float integrationMs_ = -1;
    HostInfo host_;
    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1.0f;
    int measure_ = MeasureStereo, kickFrom_ = KickSidechain;
    bool sidechainNow_ = false;

    // ---- Kick x Bass (audio thread)
    Iir kickLow_[3], bassLow_[3];   // high-pass 25 Hz, then two low-passes at kLowEndHz (Linkwitz-Riley)
    int decimation_ = 16, decimationPhase_ = 0, lags_ = 36;
    double pairRate_ = 3000.0;
    double envKeep_ = 0, envTake_ = 1, pairKeep_ = 0, pairTake_ = 1, lagKeep_ = 0, lagTake_ = 1;
    double envKick_ = 0, envBass_ = 0, weight_ = 0; // envKick_ = smoothed 2|k b|, envBass_ = smoothed k^2 + b^2
    double akk_ = 0, abb_ = 0, wkk_ = 0, wbb_ = 0, wkb_ = 0;
    std::array<double, kLagSlots> lagKb_{}, lagBb_{};
    double lagKk_ = 0;
    std::array<double, kLagSlots> ringKick_{}, ringBass_{}, ringWeight_{};
    int ringAt_ = 0;
    double onsetEnv_ = 0, onsetPeak_ = 0, onsetRelease_ = 0, onsetPeakRelease_ = 0;
    std::array<double, kOnsetLook> onsetLook_{};
    int onsetBack_ = 24, onsetGap_ = 210;
    uint64_t pairCount_ = 0, lastOnset_ = 0;
    bool anyOnset_ = false;

    double keep_ = 0, take_ = 1;
    double ll_ = 0, rr_ = 0, lr_ = 0;
    float peakL_ = 0, peakR_ = 0;
    int sinceReading_ = 0, readingInterval_ = 1000;
    std::array<BandState, kBands> bands_{};
    float minHeld_ = 1, maxHeld_ = -1, minHold_ = 0, maxHold_ = 0; // held values and seconds left on each
    int historyNext_ = 0;
    int listenFrom_ = ListenStereo, listenTo_ = ListenStereo, fadeLeft_ = 0, fadeLen_ = 1;

    std::atomic<bool> resetRequested_{false}, frozen_{false};
    std::atomic<float> integrationShown_{300}, pairRateShown_{3000};
    std::atomic<double> tempoShown_{120};
    std::atomic<bool> kickBass_{false}, sidechainShown_{false}, kickShown_{false}, bassShown_{false}, overlap_{false};
    std::atomic<float> shared_{0}, sumDb_{0}, invertedDb_{0}, shiftMs_{0}, shiftCorrelation_{0}, kickDb_{-120}, bassDb_{-120};
    std::array<std::atomic<float>, kBeatPoints> beatKick_{}, beatBass_{}, beatWeight_{};
    uint64_t beatLocal_ = 0;
    std::atomic<uint64_t> beatWritten_{0}, onsetLatest_{0}, onsetPrevious_{0};
    std::atomic<int> onsets_{0}; // 0, 1 or 2 onsets recorded (latest, previous)
    std::array<std::atomic<float>, kHistory> historyShared_{};
    std::atomic<bool> signal_{false}, lowSignal_{false};
    std::atomic<float> correlation_{0}, minShown_{0}, maxShown_{0}, lowEnd_{0}, balance_{0}, width_{0}, mono_{0}, levelL_{-120},
        levelR_{-120}, peakShownL_{-120}, peakShownR_{-120};
    std::array<std::atomic<float>, kBands> bandCorrelation_{}, bandMin_{}, bandLevel_{};
    std::array<std::atomic<bool>, kBands> bandActive_{}, bandMeasured_{};
    std::array<std::atomic<float>, kHistory> historyCorrelation_{}, historyLow_{};
    std::array<std::atomic<uint8_t>, kHistory> historyFlags_{};
    std::atomic<int> historyHead_{0};
    std::atomic<uint32_t> historyCount_{0};
    std::array<std::atomic<float>, kScopePoints> scopeX_{}, scopeY_{};
    uint64_t writtenLocal_ = 0;
    std::atomic<uint64_t> written_{0};
};
} // namespace sonora::devices
