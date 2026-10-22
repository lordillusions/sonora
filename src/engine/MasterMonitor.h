#pragma once
// Live analysis of the Main output for the Monitor view: LUFS (momentary / short-term / integrated / range),
// peaks, stereo correlation + goniometer, spectrum and oscilloscope.
//
// Threads: the audio thread only calls push() (wait-free copy into a lock-free ring, and only while the monitor
// is enabled). A background thread drains the ring and does all the maths; the UI reads snapshot().

#include "dsp/Analyzers.h"
#include "dsp/Loudness.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace sonora::engine
{

struct MonitorSnapshot
{
    double sampleRate = 48000.0;
    float momentaryLufs = dsp::kSilenceDb, shortTermLufs = dsp::kSilenceDb, integratedLufs = dsp::kSilenceDb;
    float momentaryMaxLufs = dsp::kSilenceDb, loudnessRange = 0.0f;
    float samplePeakDb = dsp::kSilenceDb, truePeakDb = dsp::kSilenceDb;
    float correlation = 0.0f, balance = 0.0f, width = 0.0f;
    std::array<float, dsp::SpectrumAnalyzer::kBars> bars{}, barPeaks{};
    std::array<float, dsp::SpectrumAnalyzer::kCurvePoints> curve{};
    std::array<std::pair<float, float>, dsp::StereoMeter::kGoniometerPoints> goniometer{};
    std::vector<dsp::Scope::Column> scope;
    double integratedSeconds = 0.0; // how long the integrated measurement has been running
    uint64_t serial = 0;            // grows with every published update
    uint64_t droppedFrames = 0;     // audio the analysis thread could not keep up with (diagnostic)
};

class MasterMonitor final : private juce::Thread
{
public:
    static constexpr int kScopeColumns = 480;

    MasterMonitor();
    ~MasterMonitor() override;

    void prepare(double sampleRate); // device (re)start; clears all measurements
    void setEnabled(bool on);        // message thread; the thread only runs while enabled
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void resetLoudness() { resetRequested_.store(true); } // restart integrated loudness / range / peaks
    void setScope(double windowMs, bool triggered)
    {
        scopeWindowMs_.store(windowMs);
        scopeTriggered_.store(triggered);
    }

    // Audio thread: wait-free.
    void push(const float* left, const float* right, int numSamples)
    {
        if (! enabled_.load(std::memory_order_relaxed) || numSamples <= 0)
            return;
        uint64_t w = writeCount_.load(std::memory_order_relaxed);
        for (int i = 0; i < numSamples; ++i, ++w)
        {
            ringL_[(size_t) (w & kRingMask)] = left[i];
            ringR_[(size_t) (w & kRingMask)] = right != nullptr ? right[i] : left[i];
        }
        writeCount_.store(w, std::memory_order_release);
    }

    MonitorSnapshot snapshot() const;

    // Synchronous variant for tests / offline tools: analyse everything pushed so far on the calling thread.
    void drainNow() { drain(); publish(); }

private:
    static constexpr uint64_t kRingFrames = 1u << 17, kRingMask = kRingFrames - 1;

    void run() override;
    void drain();
    void publish();

    std::atomic<bool> enabled_{false}, resetRequested_{false}, scopeTriggered_{true};
    std::atomic<double> scopeWindowMs_{40.0};
    std::vector<float> ringL_, ringR_;
    std::atomic<uint64_t> writeCount_{0};
    uint64_t readCount_ = 0, dropped_ = 0; // analysis side

    std::mutex analysisMutex_; // prepare / reset vs the thread
    double sampleRate_ = 48000.0;
    dsp::LoudnessMeter loudness_;
    dsp::StereoMeter stereo_;
    dsp::SpectrumAnalyzer spectrum_;
    dsp::Scope scope_;
    std::vector<float> chunkL_, chunkR_;
    double lastSpectrumMs_ = 0, lastPublishMs_ = 0, integratedFrames_ = 0;

    mutable std::mutex snapshotMutex_;
    MonitorSnapshot snapshot_;
    uint64_t serial_ = 0;
};

} // namespace sonora::engine
