#pragma once
// TRK-4 Capture Audio: the audio counterpart of Capture MIDI. The last `kSeconds` of up to `kMaxChannels` active interface
// inputs are always kept in preallocated rings, whether or not anything is armed or recording, so a passage played before
// pressing Record can still become a clip.
//
// Threads: prepare() runs on the device thread while the callback is stopped (it allocates); write() is the audio thread
// (single writer, no locks, no allocation); read() is the message thread. prepare() and read() share a mutex the audio
// thread never takes. read() discards whatever the writer overwrote while it copied.

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace sonora::engine
{

class AudioCapture
{
public:
    static constexpr int kMaxChannels = 8;
    static constexpr double kSeconds = 60.0;

    // `active[i]` = physical input i is enabled on the device (the first kMaxChannels enabled ones are kept).
    void prepare(const std::vector<bool>& active, double sampleRate);
    // Audio thread. `physical[i]` is physical input i (nullptr = inactive).
    void write(const float* const* physical, int numPhysical, int numSamples) noexcept;
    // Message thread: everything kept for physical `channel` (and `channel + 1` when `stereo`), oldest first. False when
    // that channel is not kept or nothing was written yet.
    bool read(int channel, bool stereo, juce::AudioBuffer<float>& out, double& sampleRate) const;
    bool keeps(int channel) const;

private:
    mutable std::mutex mutex_;
    std::array<int, 64> slotOfPhysical_{};
    std::vector<std::vector<float>> rings_;
    int capacity_ = 0;
    double sampleRate_ = 0.0;
    std::atomic<bool> ready_{false};
    std::atomic<int64_t> written_{0};
};

} // namespace sonora::engine
