#pragma once
// MidiReleaseFilter (ENG-14d): remembers the notes, sustain-type pedals and pitch bend one device has been sent,
// so the engine's blanket releases (All Notes Off on all 16 channels plus pedal-up and centred bend, sent at every
// transport loop wrap, launch boundary and locate) can be replaced by note-offs for the notes that are actually held.
//
// Why: some hosted instruments treat CC 123 as a full voice reset. Serum 2 spends about 1 ms per instance on it, even when
// nothing is playing, and a set with 11 instances overran the audio deadline every time the loop wrapped.
// Live sends note-offs at the loop end too, not All Notes Off.
//
// Audio thread only. No allocation: the scratch buffer is reserved in prepare() and the rewrite is skipped (the
// original messages pass through) whenever it could not fit.

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <bit>
#include <cstdint>

namespace sonora::devices
{

class MidiReleaseFilter
{
public:
    static constexpr int scratchBytes = 32768;
    static constexpr int maxRewriteBytes = 8192; // larger buffers pass through (scratchBytes covers 2048 note-offs on top)

    void prepare()
    {
        scratch_.ensureSize((size_t) scratchBytes);
        prepared_ = true;
    }

    // Updates the held state from `midi` and, when `thin`, rewrites it in place: CC 123 becomes note-offs for the
    // held notes of that channel, pedal-ups and centred bends that change nothing are dropped. With `thin` false
    // (the transport stopped: a real panic is wanted) messages are only tracked.
    void process(juce::MidiBuffer& midi, bool thin)
    {
        if (midi.isEmpty())
            return;
        if (! thin || ! prepared_ || midi.data.size() > maxRewriteBytes || ! wouldChange(midi))
        {
            for (const auto event : midi)
                track(event.data, event.numBytes);
            return;
        }

        scratch_.clear();
        for (const auto event : midi)
        {
            const auto* d = event.data;
            const int pos = event.samplePosition;
            if (isController(d, event.numBytes) && d[1] == 123)
            {
                const int ch = (d[0] & 0x0f) + 1;
                auto& held = notes_[(size_t) ch - 1];
                for (int word = 0; word < 2; ++word)
                    for (auto bits = held[(size_t) word]; bits != 0; bits &= bits - 1)
                    {
                        const uint8_t off[3] = {(uint8_t) (0x80 | (ch - 1)), (uint8_t) (word * 64 + std::countr_zero(bits)), 0};
                        scratch_.addEvent(off, 3, pos);
                    }
                held = {};
                continue;
            }
            if (redundant(d, event.numBytes))
                continue;
            track(d, event.numBytes);
            scratch_.addEvent(d, event.numBytes, pos);
        }
        // The note-offs may outgrow the original: then the original goes through (the device still gets its release).
        if (scratch_.data.size() > midi.data.size())
            return;
        midi.clear(); // keeps the allocation, and the rewrite is no larger
        for (const auto event : scratch_)
            midi.addEvent(event.data, event.numBytes, event.samplePosition);
    }

    bool anyNoteHeld() const
    {
        for (const auto& c : notes_)
            if ((c[0] | c[1]) != 0) return true;
        return false;
    }

private:
    static bool isController(const uint8_t* d, int n) { return n >= 3 && (d[0] & 0xf0) == 0xb0; }
    static bool isBend(const uint8_t* d, int n) { return n >= 3 && (d[0] & 0xf0) == 0xe0; }
    static int bendValue(const uint8_t* d) { return d[1] | (d[2] << 7); }
    static int pedalBit(int controller) { return controller == 64 ? 1 : controller == 66 ? 2 : controller == 69 ? 4 : 0; }

    bool redundant(const uint8_t* d, int n) const
    {
        const auto c = (size_t) (d[0] & 0x0f);
        if (isController(d, n))
        {
            const int bit = pedalBit(d[1]);
            return bit != 0 && d[2] < 64 && (pedals_[c] & bit) == 0;
        }
        return isBend(d, n) && bendValue(d) == 8192 && (bentChannels_ & (1u << c)) == 0;
    }

    bool wouldChange(const juce::MidiBuffer& midi) const
    {
        for (const auto event : midi)
            if ((isController(event.data, event.numBytes) && event.data[1] == 123) || redundant(event.data, event.numBytes))
                return true;
        return false;
    }

    void track(const uint8_t* d, int n)
    {
        if (n < 2 || d[0] < 0x80 || d[0] >= 0xf0)
            return;
        const auto c = (size_t) (d[0] & 0x0f);
        const int type = d[0] & 0xf0;
        const auto noteBit = [&] { return uint64_t{1} << (d[1] & 63); };
        if (n >= 3 && (type == 0x90 || type == 0x80))
        {
            auto& word = notes_[c][(size_t) ((d[1] >> 6) & 1)];
            if (type == 0x90 && d[2] > 0) word |= noteBit();
            else word &= ~noteBit();
        }
        else if (isController(d, n))
        {
            if (d[1] == 123 || d[1] == 120) // All Notes Off / All Sound Off
                notes_[c] = {};
            else if (const int bit = pedalBit(d[1]))
                pedals_[c] = (uint8_t) (d[2] >= 64 ? (pedals_[c] | bit) : (pedals_[c] & ~bit));
        }
        else if (isBend(d, n))
        {
            if (bendValue(d) == 8192) bentChannels_ &= ~(1u << c);
            else bentChannels_ |= 1u << c;
        }
    }

    std::array<std::array<uint64_t, 2>, 16> notes_{};
    std::array<uint8_t, 16> pedals_{};
    uint32_t bentChannels_ = 0;
    bool prepared_ = false;
    juce::MidiBuffer scratch_;
};

} // namespace sonora::devices
