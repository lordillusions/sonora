#pragma once
// PLG-25: the harmony link channels of Sonora Phrase (docs/PHRASE.md "Harmony"). Four channels (A-D); an instance with
// Chords = Progression and a channel LEADS it (publishes key, scale and progression), instances with Chords = Link
// FOLLOW it. One leader per channel: an instance that starts leading takes the channel over, and the previous leader sees
// another id there and follows (its face says so).
//
// Process-wide in Sonora (HarmonyLink::shared()). Between separate VST3 bundles in another host each bundle has its own
// statics, so only instances of the same plug-in share it (PLG-25h). Tests make their own instance.
//
// Lock-free, no allocation: a channel's payload is stored in atomic words under a sequence counter. Writers (the audio
// threads of leaders, the message thread) take a try-lock and give up when another writer holds it (they retry at the
// next block); readers copy the words and retry while the sequence moved (at most a few times, then keep their copy).

#include "core/Harmony.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace sonora::model
{
struct HarmonyLinkPayload
{
    uint64_t leader = 0;   // 0: nobody leads
    uint32_t version = 0;  // changes with every publish
    int32_t key = 0, scale = 1;
    Progression progression;
};

class HarmonyLink
{
public:
    static constexpr int kChannels = 4;

    static HarmonyLink& shared()
    {
        static HarmonyLink link;
        return link;
    }
    // A unique, non-zero id for a leader.
    static uint64_t newId()
    {
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    // Leader: write the channel (and take it over when `claim`; without it, only the current leader may write).
    // False when another writer was busy or the channel belongs to someone else.
    bool publish(int channel, uint64_t id, int key, int scale, const Progression& progression, bool claim)
    {
        if (channel < 0 || channel >= kChannels || id == 0) return false;
        auto& c = channels_[(size_t) channel];
        if (c.writing.test_and_set(std::memory_order_acquire)) return false;
        HarmonyLinkPayload p;
        readWords(c, p);
        bool written = false;
        if (claim || p.leader == id || p.leader == 0)
        {
            p.leader = id;
            p.key = key;
            p.scale = scale;
            p.progression = progression;
            ++p.version;
            writeWords(c, p);
            written = true;
        }
        c.writing.clear(std::memory_order_release);
        return written;
    }
    // A leader that stops leading (mode or channel changed, device removed) frees the channel if it still holds it.
    void release(int channel, uint64_t id)
    {
        if (channel < 0 || channel >= kChannels || id == 0) return;
        auto& c = channels_[(size_t) channel];
        bool locked = false;
        for (int attempt = 0; attempt < 1024 && ! locked; ++attempt) locked = ! c.writing.test_and_set(std::memory_order_acquire);
        if (! locked) return; // a writer is busy for long: the next publish by anyone else claims the channel anyway
        HarmonyLinkPayload p;
        readWords(c, p);
        if (p.leader == id)
        {
            p.leader = 0;
            ++p.version;
            writeWords(c, p);
        }
        c.writing.clear(std::memory_order_release);
    }
    // Reader: false when the channel is out of range or a consistent copy could not be taken now.
    bool read(int channel, HarmonyLinkPayload& out) const
    {
        if (channel < 0 || channel >= kChannels) return false;
        const auto& c = channels_[(size_t) channel];
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const uint32_t before = c.sequence.load(std::memory_order_acquire);
            if ((before & 1u) != 0) continue;
            HarmonyLinkPayload copy;
            readWords(c, copy);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (c.sequence.load(std::memory_order_relaxed) == before)
            {
                out = copy;
                return true;
            }
        }
        return false;
    }
    uint64_t leader(int channel) const
    {
        HarmonyLinkPayload p;
        return read(channel, p) ? p.leader : 0;
    }

private:
    static_assert(std::is_trivially_copyable_v<HarmonyLinkPayload>);
    static constexpr size_t kWords = (sizeof(HarmonyLinkPayload) + 7) / 8;
    struct Channel
    {
        std::atomic<uint32_t> sequence{0};
        std::atomic_flag writing = ATOMIC_FLAG_INIT;
        std::array<std::atomic<uint64_t>, kWords> words{};
    };
    static void readWords(const Channel& c, HarmonyLinkPayload& out)
    {
        std::array<uint64_t, kWords> raw{};
        for (size_t i = 0; i < kWords; ++i) raw[i] = c.words[i].load(std::memory_order_relaxed);
        std::memcpy(&out, raw.data(), sizeof(HarmonyLinkPayload));
    }
    static void writeWords(Channel& c, const HarmonyLinkPayload& in)
    {
        std::array<uint64_t, kWords> raw{};
        std::memcpy(raw.data(), &in, sizeof(HarmonyLinkPayload));
        c.sequence.fetch_add(1, std::memory_order_acq_rel); // odd: writing
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) c.words[i].store(raw[i], std::memory_order_relaxed);
        c.sequence.fetch_add(1, std::memory_order_release); // even: done
    }
    std::array<Channel, kChannels> channels_{};
};
} // namespace sonora::model
