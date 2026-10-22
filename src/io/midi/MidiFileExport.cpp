#include "io/midi/MidiFileExport.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace sonora::io
{

namespace
{
void put32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((uint8_t) (v >> shift));
}

void putVarLen(std::vector<uint8_t>& out, uint32_t v)
{
    uint8_t bytes[5];
    int n = 0;
    bytes[n++] = (uint8_t) (v & 0x7f);
    while ((v >>= 7) != 0) bytes[n++] = (uint8_t) (0x80 | (v & 0x7f));
    while (n > 0) out.push_back(bytes[--n]);
}

struct Event
{
    uint32_t tick;
    int order; // at equal ticks: meta first, then note-offs, then note-ons
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> track(std::vector<Event> events, uint32_t endTick)
{
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
    std::vector<uint8_t> body;
    uint32_t now = 0;
    for (const auto& e : events)
    {
        putVarLen(body, e.tick - now);
        now = e.tick;
        body.insert(body.end(), e.bytes.begin(), e.bytes.end());
    }
    putVarLen(body, endTick > now ? endTick - now : 0);
    body.insert(body.end(), {0xff, 0x2f, 0x00}); // end of track
    std::vector<uint8_t> chunk{'M', 'T', 'r', 'k'};
    put32(chunk, (uint32_t) body.size());
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}
} // namespace

std::vector<uint8_t> midiClipToSmf(const model::Clip& clip, double tempo, int numerator, int denominator, int ppq)
{
    ppq = std::clamp(ppq, 24, 32767);
    const double start = clip.loop.loopStart + clip.loop.startRelative;
    const double end = std::max(start, clip.loop.loopEnd);
    auto toTick = [&](double beat) { return (uint32_t) std::llround(std::max(0.0, beat - start) * ppq); };
    const uint32_t endTick = toTick(end);

    // Track 0: name, tempo and meter.
    std::vector<Event> meta;
    const auto name = clip.name.empty() ? std::string("MIDI Clip") : clip.name;
    std::vector<uint8_t> nameEvent{0xff, 0x03};
    putVarLen(nameEvent, (uint32_t) name.size());
    nameEvent.insert(nameEvent.end(), name.begin(), name.end());
    meta.push_back({0, 0, nameEvent});
    const auto microsPerQuarter = (uint32_t) std::clamp(std::llround(60'000'000.0 / std::max(1.0, tempo)), 1LL, 0xffffffLL);
    meta.push_back({0, 0, {0xff, 0x51, 0x03, (uint8_t) (microsPerQuarter >> 16), (uint8_t) (microsPerQuarter >> 8), (uint8_t) microsPerQuarter}});
    int denominatorPower = 0;
    while ((1 << denominatorPower) < std::max(1, denominator) && denominatorPower < 7) ++denominatorPower;
    meta.push_back({0, 0, {0xff, 0x58, 0x04, (uint8_t) std::clamp(numerator, 1, 255), (uint8_t) denominatorPower, 24, 8}});

    // Track 1: the notes (channel 1).
    std::vector<Event> notes;
    notes.push_back({0, 0, nameEvent});
    for (const auto& note : clip.notes)
    {
        if (! note.enabled || note.pitch < 0 || note.pitch > 127 || ! std::isfinite(note.start) || ! std::isfinite(note.duration)) continue;
        const double noteEnd = std::min(end, note.start + std::max(0.0, note.duration));
        if (note.start < start - 1e-9 || note.start >= end - 1e-9 || noteEnd <= note.start) continue;
        const auto on = toTick(note.start), off = std::max(on + 1, toTick(noteEnd));
        const auto velocity = (uint8_t) std::clamp((int) std::lround(note.velocity), 1, 127);
        const auto release = (uint8_t) std::clamp((int) std::lround(note.releaseVelocity), 0, 127);
        notes.push_back({on, 2, {0x90, (uint8_t) note.pitch, velocity}});
        notes.push_back({off, 1, {0x80, (uint8_t) note.pitch, release}});
    }

    std::vector<uint8_t> out{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, (uint8_t) (ppq >> 8), (uint8_t) ppq};
    const auto first = track(std::move(meta), endTick), second = track(std::move(notes), endTick);
    out.insert(out.end(), first.begin(), first.end());
    out.insert(out.end(), second.begin(), second.end());
    return out;
}

bool exportMidiClip(const model::Clip& clip, const model::Song& song, const std::string& path, std::string& error)
{
    if (clip.kind != model::Clip::Kind::Midi) { error = "Only MIDI clips can be exported as MIDI files"; return false; }
    const auto bytes = midiClipToSmf(clip, song.tempo, clip.timeSigNumerator, clip.timeSigDenominator);
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    if (! file.replaceWithData(bytes.data(), bytes.size()))
    {
        error = "Could not write " + file.getFullPathName().toStdString();
        return false;
    }
    return true;
}

} // namespace sonora::io
