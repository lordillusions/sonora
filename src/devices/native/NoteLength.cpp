#include "devices/native/NoteLength.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{

namespace
{
// SyncedLength in beats, as the reference labels it (read in Live 12.4.6, DEV-21c): 0, 1/64, 1/48, 1/32, 1/24, 1/16,
// 1/12, 1/8, 1/6, 1/4 note. Index 0 ("0") outputs nothing.
constexpr std::array<double, 10> kLengths{0.0, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 1.0};
// Note Off trigger: velocity = note-on velocity x exp(-kDecayRate x held / Decay), i.e. 2^-14 after one Decay time;
// Key Scale scales the rate by 1 - KeyScale x (pitch - 64) / 64. Fitted to 31 notes exported from the reference
// (RMS 0.2 dB through Simpler's 0.4 dB/velocity law, DEV-21c); results below velocity 1 still play at velocity 1.
constexpr double kDecayRate = 9.70;
constexpr int kByteLimit = 64000;
} // namespace

NoteLength::NoteLength() : NativeDevice([] {
    using N = NoteLength;
    std::vector<ParameterInfo> p(N::NumParams);
    p[N::Trigger] = makeParam("Mode", "Note Off", 0, 1, 0, "", true);
    p[N::Latch] = makeParam("Latch", "Latch", 0, 1, 0, "", true);
    p[N::Sync] = makeParam("SyncState", "Sync", 0, 1, 0, "", true);
    p[N::SyncedLength] = makeParam("SyncedLength", "Length", 0, 9, 4, "", false,
                                   {"0", "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4"});
    p[N::TimeLength] = makeParam("TimeLength", "Length", 10, 60000, 100, "ms");
    p[N::Gate] = makeParam("Gate", "Gate", 1, 200, 100, "pct");
    p[N::ReleaseVelocity] = makeParam("Balance", "Release Vel", 0, 100, 50, "pct");
    p[N::DecayTime] = makeParam("DecayTime", "Decay", 10, 60000, 60000, "ms");
    p[N::KeyScale] = makeParam("DecayKeyScale", "Key Scale", -1, 1, 0);
    return p;
}())
{
}

std::vector<DeviceProcessor::UiGroup> NoteLength::uiGroups() const
{
    return {
        {"Trigger", {Trigger, Latch}},
        {"Length", {Sync, SyncedLength, TimeLength, Gate}},
        {"Note Off", {ReleaseVelocity, DecayTime, KeyScale}},
    };
}

bool NoteLength::parameterShown(int index) const
{
    const bool latch = value(Latch) > 0.5f, noteOff = value(Trigger) > 0.5f;
    switch (index)
    {
        case Sync: case Gate: return ! latch; // Latch replaces the length
        case SyncedLength: return ! latch && value(Sync) > 0.5f;
        case TimeLength: return ! latch && value(Sync) <= 0.5f;
        case ReleaseVelocity: case DecayTime: case KeyScale: return noteOff && ! latch;
        default: return true;
    }
}

double NoteLength::syncedLengthBeats(int index)
{
    return kLengths[(size_t) std::clamp(index, 0, (int) kLengths.size() - 1)];
}

double NoteLength::lengthSamples() const
{
    const double base = value(Sync) > 0.5f ? syncedLengthBeats((int) std::lround(value(SyncedLength))) * 60.0 / tempo_ * sampleRate_
                                           : value(TimeLength) * 0.001 * sampleRate_;
    return base * value(Gate) * 0.01;
}

double NoteLength::decayedVelocity(double velocity, double heldMs, int pitch, double decayMs, double keyScale)
{
    const double rate = kDecayRate * std::max(0.0, 1.0 - keyScale * (pitch - 64) / 64.0);
    return velocity * std::exp(-rate * std::max(0.0, heldMs) / std::max(1.0, decayMs));
}

void NoteLength::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    scratch_.ensureSize(kByteLimit + 4096);
    reset();
}

void NoteLength::reset()
{
    numSounding_ = numHeld_ = numPending_ = 0;
    owned_ = {};
    sustain_ = false;
}

void NoteLength::emit(const uint8_t* data, int size, int64_t at)
{
    if (bytes_ + size + 8 > kByteLimit) { overflow_ = true; return; }
    scratch_.addEvent(data, size, (int) std::clamp<int64_t>(at - blockStart_, 0, std::max(0, blockLength_ - 1)));
    bytes_ += size + 8;
}

void NoteLength::stop(int index, int64_t at)
{
    const auto s = sounding_[(size_t) index];
    const uint8_t message[]{(uint8_t) (0x80 | s.channel), s.pitch, 64};
    emit(message, 3, at);
    sounding_[(size_t) index] = sounding_[(size_t) --numSounding_];
}

void NoteLength::start(int pitch, int channel, int velocity, int64_t at, bool latched)
{
    for (int i = 0; i < numSounding_; ++i)
        if (sounding_[(size_t) i].pitch == pitch && sounding_[(size_t) i].channel == channel) { stop(i, at); break; }
    const double length = lengthSamples();
    if (! latched && length < 0.5) return; // a length of 0 plays nothing
    if (numSounding_ >= kMaxNotes) stop(0, at);
    const uint8_t message[]{(uint8_t) (0x90 | channel), (uint8_t) pitch, (uint8_t) std::clamp(velocity, 1, 127)};
    emit(message, 3, at);
    sounding_[(size_t) numSounding_++] = {(uint8_t) pitch, (uint8_t) channel,
                                          latched ? -1 : at + std::max<int64_t>(1, (int64_t) std::llround(length))};
}

void NoteLength::releaseDue(int64_t limit)
{
    for (;;)
    {
        int index = -1;
        for (int i = 0; i < numSounding_; ++i)
            if (sounding_[(size_t) i].offAt >= 0 && sounding_[(size_t) i].offAt < limit
                && (index < 0 || sounding_[(size_t) i].offAt < sounding_[(size_t) index].offAt))
                index = i;
        if (index < 0) return;
        stop(index, sounding_[(size_t) index].offAt);
    }
}

void NoteLength::releaseAll(int64_t at)
{
    while (numSounding_ > 0) stop(numSounding_ - 1, at);
}

void NoteLength::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    scratch_.clear();
    bytes_ = 0;
    overflow_ = false;
    blockStart_ = clock_;
    blockLength_ = audio.getNumSamples();
    if (blockLength_ <= 0) return;
    const int64_t blockEnd = clock_ + blockLength_;

    if (isBypassed())
    {
        if (! wasBypassed_) { releaseAll(clock_); numHeld_ = numPending_ = 0; owned_ = {}; wasBypassed_ = true; }
        for (const auto meta : midi) emit(meta.data, meta.numBytes, clock_ + meta.samplePosition);
    }
    else
    {
        wasBypassed_ = false;
        const bool noteOffTrigger = value(Trigger) > 0.5f, latch = value(Latch) > 0.5f;
        auto triggerPending = [&](int64_t at) { // Note Off + Latch: the last key (and the pedal) went up
            if (numPending_ == 0 || numHeld_ > 0 || sustain_) return;
            releaseAll(at);
            for (int i = 0; i < numPending_; ++i)
                start(pending_[(size_t) i].pitch, pending_[(size_t) i].channel, pending_[(size_t) i].velocity, at, true);
            numPending_ = 0;
        };
        for (const auto meta : midi)
        {
            const int64_t at = blockStart_ + std::clamp(meta.samplePosition, 0, blockLength_ - 1);
            releaseDue(at);
            clock_ = at;
            const auto* d = meta.data;
            const int status = d[0] & 0xf0, channel = d[0] & 15;
            const bool on = meta.numBytes == 3 && status == 0x90 && d[2] != 0 && d[1] < 128;
            const bool off = meta.numBytes == 3 && (status == 0x80 || (status == 0x90 && d[2] == 0)) && d[1] < 128;
            if (on && ! noteOffTrigger)
            {
                if (latch)
                    for (int i = numSounding_ - 1; i >= 0; --i)
                        if (sounding_[(size_t) i].offAt < 0) stop(i, at); // the next note ends the latched ones
                start(d[1], channel, d[2], at, latch);
                owned_[(size_t) channel][d[1]] = true;
                continue;
            }
            if (on)
            {
                int i = 0;
                while (i < numHeld_ && ! (held_[(size_t) i].pitch == d[1] && held_[(size_t) i].channel == channel)) ++i;
                if (i == numHeld_ && numHeld_ < kMaxNotes) ++numHeld_;
                if (i < numHeld_) held_[(size_t) i] = {d[1], (uint8_t) channel, d[2], at};
                continue;
            }
            if (off && ! noteOffTrigger)
            {
                auto& owned = owned_[(size_t) channel][d[1]];
                if (! owned) emit(d, 3, at); // a key we did not take over (e.g. pressed while bypassed) still ends
                owned = false;
                continue;
            }
            if (off)
            {
                int i = 0;
                while (i < numHeld_ && ! (held_[(size_t) i].pitch == d[1] && held_[(size_t) i].channel == channel)) ++i;
                if (i == numHeld_) { emit(d, 3, at); continue; }
                const auto key = held_[(size_t) i];
                held_[(size_t) i] = held_[(size_t) --numHeld_];
                if (latch)
                {
                    if (numPending_ < kMaxNotes) pending_[(size_t) numPending_++] = key;
                    triggerPending(at);
                    continue;
                }
                // The note-on velocity decays over Decay; Release Velocity blends in the note-off velocity linearly.
                const double heldMs = (double) (at - key.onAt) * 1000.0 / sampleRate_;
                const double decayed = decayedVelocity(key.velocity, heldMs, d[1], value(DecayTime), value(KeyScale));
                const double balance = value(ReleaseVelocity) * 0.01;
                const int velocity = (int) std::lround((1.0 - balance) * decayed + balance * (status == 0x80 ? d[2] : 0));
                start(d[1], channel, std::max(1, velocity), at, false);
                continue;
            }
            if (meta.numBytes == 3 && status == 0xb0 && d[1] == 64)
            {
                sustain_ = d[2] >= 64;
                if (noteOffTrigger && latch) triggerPending(at);
            }
            if (meta.numBytes == 3 && status == 0xb0 && (d[1] == 123 || d[1] == 120))
            {
                releaseAll(at);
                numHeld_ = numPending_ = 0;
                owned_ = {};
            }
            emit(d, meta.numBytes, at);
        }
        releaseDue(blockEnd);
    }

    if (overflow_)
    {
        scratch_.clear();
        reset();
        for (int ch = 0; ch < 16; ++ch)
        {
            const uint8_t message[]{(uint8_t) (0xb0 | ch), 120, 0};
            scratch_.addEvent(message, 3, 0);
        }
    }
    clock_ = blockEnd;
    midi.clear();
    midi.addEvents(scratch_, 0, -1, 0);
}

} // namespace sonora::devices
