#pragma once
// COL-7: the audio thread's side of collab voice.
//
// The live device callback (AudioEngine::audioDeviceIOCallbackWithContext, after renderBlock) hands the tap the
// physical inputs and the finished output. The tap copies one input channel (the microphone) into a capture ring and
// adds the other people's voices to the first two outputs after Main, ducking the music while someone talks. Offline
// renders (export, freeze, bounce) never call it, and recording reads the inputs, so voice is never exported or
// recorded.
//
// Threads: process() is the audio thread (single producer of the capture ring, single consumer of the playback ring;
// no locks, no allocation). readCapture()/writePlayback() are one worker thread (collab::VoiceChannel). Settings are
// atomics any thread may set.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sonora::engine
{

// Single-producer single-consumer ring of floats with a power-of-two capacity, allocated once.
class SpscFloatRing
{
public:
    explicit SpscFloatRing(size_t capacityPow2);
    size_t write(const float* data, size_t n) noexcept; // returns how many fit
    size_t read(float* data, size_t n) noexcept;        // returns how many were there
    size_t available() const noexcept;                  // readable now
    size_t capacity() const noexcept { return buffer_.size(); }
    void clearFromReader() noexcept;                    // consumer side: drop everything readable

private:
    std::vector<float> buffer_;
    size_t mask_;
    std::atomic<size_t> head_{0}, tail_{0}; // head: next write, tail: next read (monotonic)
};

class VoiceTap
{
public:
    VoiceTap();

    // ---- audio thread
    void process(const float* const* physical, int numPhysical, float* const* outputs, int numOutputs, int numSamples,
                 double sampleRate) noexcept;

    // ---- worker thread
    size_t readCapture(float* data, size_t n) noexcept { return capture_.read(data, n); }
    size_t captureAvailable() const noexcept { return capture_.available(); }
    size_t writePlayback(const float* data, size_t n) noexcept { return playback_.write(data, n); }
    size_t playbackQueued() const noexcept { return playback_.available(); }
    void dropCapture() noexcept { capture_.clearFromReader(); }

    // ---- any thread
    std::atomic<bool> active{false};     // a session with voice runs: capture and mix
    std::atomic<bool> capturing{false};  // the mic is sent (on, and push-to-talk held when that is on)
    std::atomic<int> micChannel{-1};     // physical input; -1: the first active one
    std::atomic<float> voiceGain{1.0f};  // the other people's voices
    std::atomic<float> duckGain{0.5f};   // music gain while someone talks (1: no ducking)
    std::atomic<float> testToneHz{0.0f}; // > 0: a sine at -12 dBFS replaces the mic (SONORA_COLLAB_VOICE_TONE autotests)
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    float micPeak() const noexcept { return micPeak_.load(std::memory_order_relaxed); }     // last ~50 ms, linear
    bool hasInput() const noexcept { return hasInput_.load(std::memory_order_relaxed); }    // an input channel was there
    uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    static constexpr int maxChunk = 4096; // process() works in chunks of this many frames

private:
    SpscFloatRing capture_, playback_;
    std::array<float, maxChunk> voice_{}, tone_{};
    double tonePhase_ = 0.0;
    std::atomic<double> sampleRate_{48000.0};
    std::atomic<float> micPeak_{0.0f};
    std::atomic<bool> hasInput_{false};
    std::atomic<uint64_t> underruns_{0};
    float duck_ = 1.0f, peakHold_ = 0.0f;
    int peakSamples_ = 0, holdLeft_ = 0;
    bool wasPlaying_ = false;
};

} // namespace sonora::engine
