#pragma once
// Sonora Kickstract (PLG-17): pulls a clean kick out of a full mix. The method is the familiar producer trick of copying
// a loop three times, filtering the copies into low / mid / high bands (defaults 150 Hz and 1.5 kHz) and cutting each copy
// short right after every kick hit, then summing the copies. Kickstract does that live:
//
//   input -> 5 ms lookahead delay -> Linkwitz-Riley 4th-order 3-band split (flat sum)
//         -> per band: a gate envelope (attack, Length hold, Fade) opened by the kick detector, Gain, On, Solo
//         -> sum -> Punch (slow-attack compressor) -> Drive (soft clip) -> Output
//
// The detector listens to the undelayed low band. The low band rises late (a kick's click comes first and a 150 Hz
// low-pass adds delay), so on a hit the detector looks back over the lookahead window in a broadband envelope for where the
// attack started, and the gates open exactly there (1 ms ramp ending at the onset). Latency = the lookahead (15 ms). Listen = Kick (the result), Removed (everything the gates took out) or Input (the delayed input).
//
// Display data for the face (ui/views/KickstractFace): spectrum taps of the input and the output, the band gates' current
// levels, the detector level, and a capture of the last kick (per-bin peaks of each band and the result's waveform),
// all written by the audio thread into atomics and read by the message thread.

#include "devices/NativeDevice.h"
#include "dsp/AudioDisplayTap.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sonora::devices
{
class Kickstract final : public NativeDevice
{
public:
    static constexpr int kBands = 3;
    enum Band { Low, Mid, High };
    // Per band: On, Solo, Length, Fade, Gain (in that order from bandParam(b, 0)).
    enum BandField { BandOn, BandSolo, BandLength, BandFade, BandGain, kBandFields };
    enum Param
    {
        Input, LowSplit, HighSplit, Threshold, Retrigger, Chop,
        BandStart, // kBands * kBandFields parameters
        Punch = BandStart + kBands * kBandFields, Drive, Output, Listen,
        NumParams
    };
    enum ListenMode { ListenKick, ListenRemoved, ListenInput };
    static constexpr int bandParam(int band, int field) { return BandStart + band * kBandFields + field; }
    static constexpr double kLookaheadSeconds = 0.025; // room to find the click before a late low-band trigger
    static constexpr double kAttackSeconds = 0.001; // the gates' ramp; it ends at the detected onset

    Kickstract();
    std::string name() const override { return "Sonora Kickstract"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    int latencySamples() const override { return lookahead_; }
    std::vector<UiGroup> uiGroups() const override;

    // The crossover frequencies in use (the high split stays at least 1.25x above the low one).
    float lowSplitHz() const;
    float highSplitHz() const;

    // ---- display (message thread reads; audio thread writes)
    dsp::AudioDisplayTap inputTap, outputTap;
    float bandLevel(int band) const { return bandLevel_[(size_t) band].load(std::memory_order_relaxed); }
    // Loudest detector level (linear) since the last call.
    float takeDetectorPeak() { return detectorPeak_.exchange(0.0f, std::memory_order_relaxed); }
    uint32_t triggerCount() const { return triggers_.load(std::memory_order_relaxed); }

    static constexpr int kCaptureBins = 256;
    struct KickShape // one captured kick, as the face draws it
    {
        int bins = 0;           // filled bins (0 = none yet)
        float windowMs = 0;     // time span of kCaptureBins bins
        std::array<float, kCaptureBins> outMin{}, outMax{}, inPeak{};
        std::array<std::array<float, kCaptureBins>, kBands> band{}; // peak of each band after its gate
        uint32_t serial = 0;    // changes with every published capture
    };
    // Copies the last complete capture; false when nothing new since `serial`.
    bool takeKick(KickShape& out, uint32_t serial) const;

    // ---- timeline (PLG-17e): the last ~10 s of Input (delayed, aligned with the result), Kick and Removed as min/max
    // bins, plus where the gates opened. The audio thread writes bin `written() % kTimelineBins` and then bumps the count.
    static constexpr int kTimelineBins = 4096;
    static constexpr double kTimelineBinSeconds = 0.0025; // 4096 x 2.5 ms = 10.24 s
    enum TimelineLane { LaneInput, LaneKick, LaneRemoved, kTimelineLanes };
    struct TimelineBin
    {
        float min[kTimelineLanes]{}, max[kTimelineLanes]{};
        bool hit = false;
    };
    uint64_t timelineWritten() const { return timelineWritten_.load(std::memory_order_acquire); }
    TimelineBin timelineBin(uint64_t index) const;

private:
    // Topology-preserving state-variable filter (Butterworth Q) giving low and high outputs of one stage.
    struct Svf
    {
        float ic1 = 0, ic2 = 0;
        void reset() { ic1 = ic2 = 0; }
    };
    struct SvfCoeffs
    {
        float g = 0, k = 1.41421356f, a1 = 0, a2 = 0, a3 = 0;
        void set(double sampleRate, float hz);
    };
    // Linkwitz-Riley 4th order: two Butterworth stages for each side; low + high sums to an allpass.
    struct Lr4
    {
        Svf lp[2], hp[2];
        void reset() { for (auto& s : lp) s.reset(); for (auto& s : hp) s.reset(); }
        void split(const SvfCoeffs& c, float x, float& low, float& high);
    };
    struct Allpass // second-order allpass matching an Lr4's low + high
    {
        Svf s;
        float process(const SvfCoeffs& c, float x);
    };
    struct Gate
    {
        enum Stage { Idle, Attack, Hold, Release } stage = Idle;
        float level = 0, releaseFrom = 0;
        int counter = 0, total = 1;
    };
    struct Capture
    {
        std::array<std::atomic<float>, kCaptureBins> outMin{}, outMax{}, inPeak{};
        std::array<std::array<std::atomic<float>, kCaptureBins>, kBands> band{};
        std::atomic<int> bins{0};
        std::atomic<float> windowMs{0};
    };

    void updateCoefficients(bool immediate);
    void trigger(int newestPos);
    void openGates();
    float gateStep(Gate&, int band);
    void captureSample(float outL, float outR, float in, const float (&bands)[kBands]);
    void timelineSample(float in, float kick, float removed);

    double sampleRate_ = 44100.0;
    int lookahead_ = 0, delayPos_ = 0;
    std::vector<float> delay_[2], onsetEnv_; // onsetEnv_: broadband envelope, same ring positions as delay_
    float broadband_ = 0;
    int pendingOpen_ = -1; // samples until the detected onset leaves the delay line (-1: none)
    float lowHz_ = 150, highHz_ = 1500; // smoothed
    SvfCoeffs lowC_, highC_;
    Lr4 split1_[2], split2_[2], detectSplit_;
    Allpass lowAllpass_[2];
    Gate gates_[kBands];
    float fast_ = 0, slow_ = 0, compEnv_ = 0;
    int sinceTrigger_ = 1 << 30;
    bool armed_ = true;

    std::array<std::atomic<float>, kBands> bandLevel_{};
    std::atomic<float> detectorPeak_{0};
    std::atomic<uint32_t> triggers_{0};
    Capture captures_[2];
    struct TimelineStore
    {
        std::array<std::array<std::atomic<float>, kTimelineBins>, kTimelineLanes> min{}, max{};
        std::array<std::atomic<bool>, kTimelineBins> hit{};
    };
    TimelineStore timeline_;
    std::atomic<uint64_t> timelineWritten_{0};
    int timelineBinSamples_ = 110, timelineCounter_ = 0;
    float timelineMin_[kTimelineLanes]{}, timelineMax_[kTimelineLanes]{};
    bool timelineHit_ = false;
    std::atomic<int> published_{-1};
    std::atomic<uint32_t> captureSerial_{0};
    int writing_ = 0, captureBin_ = -1, binSamples_ = 1, binCounter_ = 0;
    float binOutMin_ = 0, binOutMax_ = 0, binIn_ = 0, binBand_[kBands]{};
};
} // namespace sonora::devices
