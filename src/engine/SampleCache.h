#pragma once
// Loads audio files fully into memory and shares them between clips (ENG-8).
//
// Loading happens on a background pool (setLoaderPool) so opening a big set never blocks the
// UI: buildGraph() asks for a SampleSlot per clip and the clip simply stays silent until the
// slot is published. Without a pool (tests, command-line tools) request() loads synchronously.
// TODO(ENG-8b): stream files that do not fit comfortably in RAM instead of decoding them whole.

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sonora::engine
{

struct SampleData
{
    juce::AudioBuffer<float> audio; // always 2 channels (mono is duplicated)
    double sampleRate = 44100.0;
    std::vector<double> onsets; // seconds, detected once at load (Beats-mode slicing), starts with 0.0
    // The file as stored (sample editor header): channel count, bit depth (0 = unknown) and float samples.
    int fileChannels = 2;
    int fileBits = 0;
    bool fileFloat = false;
};

// What a clip holds on to. The audio thread only calls get() (one acquire load); the loader
// publishes exactly once, after which `owner_` is never touched again.
class SampleSlot
{
public:
    const SampleData* get() const { return ready_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }

    void publish(std::shared_ptr<const SampleData> data)
    {
        owner_ = std::move(data);
        ready_.store(owner_.get(), std::memory_order_release);
    }
    void markFailed() { failed_.store(true, std::memory_order_relaxed); }

    static std::shared_ptr<SampleSlot> makeReady(std::shared_ptr<const SampleData> data)
    {
        auto s = std::make_shared<SampleSlot>();
        s->publish(std::move(data));
        return s;
    }

private:
    std::shared_ptr<const SampleData> owner_;
    std::atomic<const SampleData*> ready_{nullptr};
    std::atomic<bool> failed_{false};
};

class SampleCache
{
public:
    SampleCache();

    // nullptr = load synchronously inside request(). The pool must outlive the cache's jobs.
    void setLoaderPool(juce::ThreadPool* pool) { pool_ = pool; }

    // Returns a slot that becomes ready when the file is decoded (immediately when cached or
    // when no pool is set). A slot that failed to load stays empty and is reported in failedFiles().
    std::shared_ptr<SampleSlot> request(const std::string& path);

    // Blocking load (loader threads, tests).
    std::shared_ptr<const SampleData> load(const std::string& path);

    // Jobs queued by older requests are skipped (a new song replaced the old one).
    void cancelQueued() { ++generation_; }

    int pendingLoads() const { return pending_.load(); }
    std::vector<std::string> failedFiles() const;
    // Waits until nothing is pending. Returns false on timeout.
    bool waitForLoads(int timeoutMs) const;

    void clearUnused();

private:
    juce::AudioFormatManager formats_;
    std::map<std::string, std::weak_ptr<const SampleData>> cache_;
    mutable std::mutex mutex_;
    juce::ThreadPool* pool_ = nullptr;
    std::atomic<int> pending_{0};
    std::atomic<int> generation_{0};
    std::vector<std::string> failed_;
};

} // namespace sonora::engine
