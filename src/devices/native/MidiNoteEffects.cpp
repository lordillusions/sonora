#include "devices/native/MidiNoteEffects.h"
namespace sonora::devices
{
namespace
{
std::vector<ParameterInfo> infos(MidiNoteEffects::Kind kind)
{
    using K = MidiNoteEffects::Kind;
    if (kind == K::Pitch) return {makeParam("Pitch", "Pitch", -128, 128, 0, "st"), makeParam("Lowest", "Lowest", 0, 127, 0),
        makeParam("Range", "Range", 0, 127, 127), makeParam("RangeMode", "Range Mode", 0, 2, 0, "", false, {"Block", "Fold", "Limit"})};
    if (kind == K::Random) return {makeParam("Chance", "Chance", 0, 1, 0, "%"), makeParam("Choices", "Choices", 1, 24, 12),
        makeParam("Scale", "Scale", 1, 12, 1), makeParam("Sign", "Sign", 0, 2, 0, "", false, {"Add", "Subtract", "Both"}), makeParam("Alternate", "Alternate", 0, 1, 0, "", true)};
    if (kind == K::Velocity) return {makeParam("Drive", "Drive", -1, 1, 0), makeParam("Compand", "Compand", -1, 1, 0),
        makeParam("Random", "Random", 0, 127, 0), makeParam("Mode", "Mode", 0, 2, 0, "", false, {"Clip", "Gate", "Fixed"}),
        makeParam("Operation", "Operation", 0, 2, 0, "", false, {"Note On", "Note Off", "Both"}),
        makeParam("MaxOut", "Out High", 1, 127, 127), makeParam("MinOut", "Out Low", 0, 127, 0),
        makeParam("Range", "Range", 0, 127, 127), makeParam("Lowest", "Lowest", 0, 127, 0)};
    std::vector<ParameterInfo> p;
    if (kind == K::Chord)
    {
        for (int i = 1; i <= 6; ++i)
        {
            const auto suffix = std::to_string(i);
            p.push_back(makeParam("Shift" + suffix, "Shift " + suffix, -36, 36, 0, "st"));
            p.push_back(makeParam("Velocity" + suffix, "Velocity " + suffix, 0, 2, 1, "%"));
            p.push_back(makeParam("Chance" + suffix, "Chance " + suffix, 0, 1, 1, "%"));
        }
    }
    else
    {
        p = {makeParam("Base", "Base", 0, 11, 0), makeParam("Transpose", "Transpose", -36, 36, 0, "st"),
             makeParam("Fold", "Fold", 0, 1, 0, "", true), makeParam("Lowest", "Lowest", 0, 127, 0), makeParam("Range", "Range", 0, 127, 127)};
        for (int i = 0; i < 12; ++i) p.push_back(makeParam("Mapping." + std::to_string(i), "Map " + std::to_string(i), -1, 11, (float) i));
    }
    return p;
}
}
MidiNoteEffects::MidiNoteEffects(Kind kind) : NativeDevice(infos(kind)), kind_(kind) {}
std::string MidiNoteEffects::name() const
{
    switch (kind_) { case Kind::Pitch: return "Pitch"; case Kind::Chord: return "Chord"; case Kind::Scale: return "Scale";
        case Kind::Random: return "Random"; case Kind::Velocity: return "Velocity"; }
    return {};
}
std::vector<int> MidiNoteEffects::displayParameters() const
{
    std::vector<int> result;
    for (int i = 0; i < (int) parameters().size(); ++i) result.push_back(i);
    return result;
}
void MidiNoteEffects::prepare(double, int) { scratch_.ensureSize(131072); reset(); }
void MidiNoteEffects::reset() { held_ = {}; references_ = {}; random_ = 0x789abcdu; alternate_ = 0; }
float MidiNoteEffects::random() { random_ = random_ * 1664525u + 1013904223u; return (float) (random_ >> 8) / 16777216.0f; }
void MidiNoteEffects::emit(const void* data, int size, int sample)
{
    if (bytes_ + size + 8 > 64000) { overflow_ = true; return; }
    scratch_.addEvent(data, size, sample); bytes_ += size + 8;
}
void MidiNoteEffects::release(int channel, int note, int velocity, int sample)
{
    auto& held = held_[(size_t) channel][(size_t) note];
    for (int i = 0; i < held.count; ++i)
    {
        const int output = held.notes[(size_t) i];
        auto& count = references_[(size_t) channel][(size_t) output];
        count = std::max(0, count - 1);
        if (count == 0)
        {
            const uint8_t message[]{(uint8_t) (0x80 | channel), (uint8_t) output, (uint8_t) velocity}; emit(message, 3, sample);
        }
    }
    held.count = 0; held.seen = false;
}
void MidiNoteEffects::process(juce::AudioBuffer<float>&, juce::MidiBuffer& midi)
{
    scratch_.clear(); bytes_ = 0; overflow_ = false;
    for (const auto meta : midi)
    {
        const int status = meta.data[0] & 0xf0, ch = meta.data[0] & 15;
        const bool on = meta.numBytes == 3 && status == 0x90 && meta.data[2] != 0;
        const bool off = meta.numBytes == 3 && (status == 0x80 || (status == 0x90 && meta.data[2] == 0));
        if (meta.numBytes == 3 && (status == 0x80 || status == 0x90 || status == 0xa0) && (meta.data[1] > 127 || meta.data[2] > 127))
        { overflow_ = true; break; }
        if (! on && ! off)
        {
            if (meta.numBytes == 3 && status == 0xa0)
            {
                const auto& held = held_[(size_t) ch][(size_t) meta.data[1]];
                if (held.seen)
                {
                    for (int i = 0; i < held.count; ++i)
                    {
                        const uint8_t message[]{meta.data[0], (uint8_t) held.notes[(size_t) i], meta.data[2]};
                        emit(message, 3, meta.samplePosition);
                    }
                    continue;
                }
            }
            if (meta.numBytes == 3 && status == 0xb0 && (meta.data[1] == 120 || meta.data[1] == 123))
            { held_[(size_t) ch] = {}; references_[(size_t) ch] = {}; }
            emit(meta.data, meta.numBytes, meta.samplePosition); continue;
        }
        const int note = meta.data[1]; int velocity = meta.data[2]; bool gate = false;
        if (! isBypassed() && kind_ == Kind::Velocity)
        {
            const int operation = (int) std::lround(value(4));
            if ((on && operation != 1) || (off && operation != 0))
            {
                const float lowest = value(8), highest = std::min(127.0f, lowest + value(7));
                gate = (int) value(3) == 1 && ((float) velocity < lowest || (float) velocity > highest);
                const float norm = std::clamp(((float) velocity - lowest) / std::max(1.0f, highest - lowest), 0.0f, 1.0f);
                const float driven = std::clamp(norm + value(0) * 0.5f, 0.0f, 1.0f) * 2 - 1;
                const float curved = 0.5f + 0.5f * std::copysign(std::pow(std::abs(driven), std::exp2(-value(1) * 3)), driven);
                const float low = value(6), high = value(5);
                velocity = (int) std::lround(std::clamp(((int) value(3) == 2 ? high : low + (high - low) * curved) + (random() * 2 - 1) * value(2), std::min(low, high), std::max(low, high)));
            }
        }
        if (off)
        {
            auto& held = held_[(size_t) ch][(size_t) note];
            if (! held.seen && references_[(size_t) ch][(size_t) note] == 0) { const uint8_t message[]{(uint8_t) (0x80 | ch), (uint8_t) note, (uint8_t) velocity}; emit(message, 3, meta.samplePosition); }
            else release(ch, note, velocity, meta.samplePosition);
            continue;
        }
        release(ch, note, 0, meta.samplePosition);
        auto& held = held_[(size_t) ch][(size_t) note]; held.seen = true;
        if (gate) continue;
        auto add = [&](int output, int vel) {
            if (output < 0 || output > 127 || held.count >= 7 || vel <= 0) return;
            for (int i = 0; i < held.count; ++i) if (held.notes[(size_t) i] == output) return;
            const uint8_t message[]{(uint8_t) (0x90 | ch), (uint8_t) output, (uint8_t) std::clamp(vel, 1, 127)};
            emit(message, 3, meta.samplePosition); held.notes[(size_t) held.count++] = output;
            ++references_[(size_t) ch][(size_t) output];
        };
        int output = note;
        if (! isBypassed() && kind_ == Kind::Pitch)
        {
            const int low = (int) std::lround(value(1)), high = std::min(127, low + (int) std::lround(value(2)));
            output += (int) std::lround(value(0));
            const int mode = (int) std::lround(value(3));
            if (mode == 1) { while (output < low) output += 12; while (output > high) output -= 12; }
            if (mode == 2) output = std::clamp(output, low, high);
            if (output < low || output > high) continue;
        }
        else if (! isBypassed() && kind_ == Kind::Random && random() < value(0))
        {
            const int choices = std::max(1, (int) std::lround(value(1)));
            const int choice = value(4) > 0.5f ? (int) (alternate_++ % (uint32_t) choices) : 1 + std::min(choices - 1, (int) (random() * (float) choices));
            const int sign = (int) std::lround(value(3));
            output += choice * (int) std::lround(value(2)) * (sign == 1 || (sign == 2 && random() < 0.5f) ? -1 : 1);
        }
        else if (! isBypassed() && kind_ == Kind::Scale)
        {
            const int low = (int) value(3), high = std::min(127, low + (int) value(4));
            if (note < low || note > high) { add(note, velocity); continue; }
            const int base = (int) std::lround(value(0)), relative = (note - base + 120) % 12;
            const int mapping = (int) std::lround(value(5 + relative)); if (mapping < 0) continue;
            int interval = mapping - relative;
            if (value(2) > 0.5f) { if (interval > 6) interval -= 12; else if (interval < -6) interval += 12; }
            output += interval + (int) std::lround(value(1));
        }
        add(output, velocity);
        if (! isBypassed() && kind_ == Kind::Chord)
            for (int i = 0; i < 6; ++i)
            {
                const int shift = (int) std::lround(value(i * 3));
                if (shift != 0 && random() < value(i * 3 + 2)) add(note + shift, (int) std::lround((float) velocity * value(i * 3 + 1)));
            }
    }
    if (overflow_)
    {
        // Refuse a pathological expansion safely, rather than allocating or leaving held notes.
        scratch_.clear(); reset();
        for (int ch = 0; ch < 16; ++ch)
        {
            const uint8_t message[]{(uint8_t) (0xb0 | ch), 120, 0}; scratch_.addEvent(message, 3, 0);
        }
    }
    midi.clear(); midi.addEvents(scratch_, 0, -1, 0);
}
}
