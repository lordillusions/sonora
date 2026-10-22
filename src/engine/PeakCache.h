#pragma once
// Waveform overviews for the UI (ROADMAP UI-4).
//
// A Peaks object holds min/max of the mono mix and each stereo channel in a power-of-two pyramid. It is built on a
// background pool (or synchronously without one) from the same decoded data the engine plays, and queried
// from the message thread while painting: request() never blocks, it returns nullptr until the overview is
// ready and calls onReady (on the loader thread) once when it is.

#include "core/Model.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sonora::engine
{

class SampleCache;
struct SampleData;

struct Peaks
{
    static constexpr int kBucket = 64;

    double sampleRate = 44100.0;
    int64_t numFrames = 0;
    struct Level
    {
        std::array<std::vector<float>, 3> mins, maxs; // mono mix, left, right
    };
    std::vector<Level> levels; // level n covers kBucket * 2^n frames per bucket
    // The decoded audio the overview was built from: zoomed-in views draw real samples instead of buckets.
    std::shared_ptr<const SampleData> source;

    double lengthSeconds() const { return sampleRate > 0 ? (double) numFrames / sampleRate : 0.0; }
    // Extremes between two times (seconds, any order). false when the span lies outside the file.
    bool range(double t0, double t1, float& lo, float& hi, int channel = -1) const;
};

std::shared_ptr<const Peaks> computePeaks(const juce::AudioBuffer<float>& audio, double sampleRate,
                                          std::shared_ptr<const SampleData> source = nullptr);

class PeakCache
{
public:
    explicit PeakCache(SampleCache& samples) : samples_(samples) {}

    // nullptr = compute synchronously inside request().
    void setPool(juce::ThreadPool* pool) { pool_ = pool; }
    std::function<void()> onReady;

    // Overview of `path` (a resolved file path) or nullptr while it is being computed / when the file is unreadable.
    std::shared_ptr<const Peaks> request(const std::string& path);
    // Same for a clip's sample; the resolved path is remembered so painting does not touch the file system.
    std::shared_ptr<const Peaks> request(const model::Song& song, const model::Clip& clip);

    void clear();

private:
    struct Entry
    {
        std::shared_ptr<const Peaks> peaks;
        bool pending = false, failed = false;
    };
    void compute(const std::string& path);

    SampleCache& samples_;
    juce::ThreadPool* pool_ = nullptr;
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::string> resolved_; // clip file key -> resolved path
    std::shared_ptr<std::atomic<int>> generation_ = std::make_shared<std::atomic<int>>(0);
};

} // namespace sonora::engine
