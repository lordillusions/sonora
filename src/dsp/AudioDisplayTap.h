#pragma once
// DEV-2b: bounded SPSC handoff from an audio processor to its visible display. The producer never
// waits for the UI: a full queue drops new samples, and an unrenewed request expires after 250 ms.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace sonora::dsp
{
class AudioDisplayTap
{
public:
    static constexpr uint32_t kCapacity = 8192;
    using Samples = std::array<float, kCapacity>;
    static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<float>::is_always_lock_free
                  && std::atomic<double>::is_always_lock_free && std::atomic<int>::is_always_lock_free
                  && std::atomic<bool>::is_always_lock_free);

    // Producer only; do not reset queue indices while the consumer may be reading.
    void prepare(double sampleRate) { sampleRate_.store(sampleRate); reset(); }
    void reset() { generation_.fetch_add(1, std::memory_order_release); }
    double sampleRate() const { return sampleRate_.load(); }
    uint32_t generation() const { return generation_.load(std::memory_order_acquire); }

    // One message-thread consumer. Renew only while the device is in the viewport.
    void request(bool spectrum)
    {
        spectrum_.store(spectrum, std::memory_order_relaxed);
        remaining_.store((int) (sampleRate() * 0.25), std::memory_order_relaxed);
    }
    int read(Samples& left, Samples& right, std::array<float, 2>& peaks)
    {
        const auto begin = read_.load(std::memory_order_relaxed), end = written_.load(std::memory_order_acquire);
        const auto n = end - begin;
        peaks.fill(0);
        for (uint32_t i = 0; i < n; ++i)
        {
            left[i] = left_[(begin + i) % kCapacity];
            right[i] = right_[(begin + i) % kCapacity];
            peaks[0] = std::max(peaks[0], std::abs(left[i]));
            peaks[1] = std::max(peaks[1], std::abs(right[i]));
        }
        read_.store(end, std::memory_order_release);
        for (size_t ch = 0; ch < 2; ++ch) peaks[ch] = std::max(peaks[ch], peaks_[ch].exchange(0, std::memory_order_relaxed));
        return (int) n;
    }
    void discard()
    {
        read_.store(written_.load(std::memory_order_acquire), std::memory_order_release);
        for (auto& peak : peaks_) peak.exchange(0, std::memory_order_relaxed);
    }

    // Audio thread: no allocation, locks, FFT or ownership operations.
    void push(const float* left, const float* right, int count)
    {
        if (remaining_.load(std::memory_order_relaxed) <= 0 || count <= 0) return;
        remaining_.fetch_sub(count, std::memory_order_relaxed);
        const auto begin = written_.load(std::memory_order_relaxed);
        const auto space = kCapacity - (begin - read_.load(std::memory_order_acquire));
        const int n = spectrum_.load(std::memory_order_relaxed) ? std::min(count, (int) space) : 0;
        std::array<float, 2> peaks{};
        for (int i = 0; i < count; ++i)
        {
            const float l = std::isfinite(left[i]) ? left[i] : 0;
            const float r = right == nullptr ? l : (std::isfinite(right[i]) ? right[i] : 0);
            peaks[0] = std::max(peaks[0], std::abs(l));
            peaks[1] = std::max(peaks[1], std::abs(r));
            if (i < n)
            {
                const auto slot = (begin + (uint32_t) i) % kCapacity;
                left_[slot] = l; right_[slot] = r;
            }
        }
        written_.store(begin + (uint32_t) n, std::memory_order_release);
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto previous = peaks_[ch].load(std::memory_order_relaxed);
            while (previous < peaks[ch] && ! peaks_[ch].compare_exchange_weak(previous, peaks[ch], std::memory_order_relaxed)) {}
        }
    }

private:
    Samples left_{}, right_{};
    std::atomic<uint32_t> read_{0}, written_{0}, generation_{0};
    std::array<std::atomic<float>, 2> peaks_{};
    std::atomic<double> sampleRate_{44100};
    std::atomic<int> remaining_{0};
    std::atomic<bool> spectrum_{false};
};
} // namespace sonora::dsp
