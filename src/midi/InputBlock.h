#pragma once
// Bounded copy of input events with source-port identity (ENG-9b). Audio-thread storage, no ownership/allocation.
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace sonora::midi
{
struct InputBlock
{
    static constexpr int maxEvents = 1024, maxBytes = 65536, bufferCapacity = maxBytes + maxEvents * 8;
    struct Event { int offset = 0, size = 0, sample = 0; uint32_t port = 0; };
    std::array<Event, maxEvents> events{};
    std::array<uint8_t, maxBytes> bytes{};
    int count = 0, used = 0;
    void clear() { count = used = 0; }
    bool add(const void* data, int size, int sample, uint32_t port)
    {
        if (size <= 0 || size > 65535 || count >= maxEvents || size > maxBytes - used) return false;
        events[(size_t) count++] = {used, size, sample, port};
        std::memcpy(bytes.data() + used, data, (size_t) size);
        used += size;
        return true;
    }
    void addTo(juce::MidiBuffer& destination, uint32_t port, int channel) const
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& event = events[(size_t) i];
            const auto* data = bytes.data() + event.offset;
            if (port != 0 && event.port != port) continue;
            if (channel != 0 && data[0] >= 0x80 && data[0] < 0xf0 && (data[0] & 0x0f) + 1 != channel) continue;
            destination.addEvent(data, event.size, event.sample);
        }
    }
};
} // namespace sonora::midi
