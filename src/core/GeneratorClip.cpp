#include "core/GeneratorClip.h"

#include "core/PatternGrid.h"

#include <algorithm>
#include <cmath>

namespace sonora::model
{
namespace
{
// Song-beat notes -> the clip's content span [loopStart, loopEnd): notes that start inside it, cut at its end.
std::vector<MidiNote> toContent(const std::vector<MidiNote>& rendered, Beats origin, Beats loopStart, Beats loopEnd)
{
    std::vector<MidiNote> out;
    out.reserve(rendered.size());
    for (auto note : rendered)
    {
        note.start = note.start - origin + loopStart;
        if (note.start < loopStart - 1e-9 || note.start >= loopEnd - 1e-9) continue;
        note.start = std::max(note.start, loopStart);
        note.duration = std::max(1e-4, std::min(note.duration, loopEnd - note.start));
        note.noteId = -1;
        note.key = 0;
        out.push_back(note);
    }
    return out;
}

std::string notesWritten(const std::string& device, size_t count)
{
    return device + " wrote " + std::to_string(count) + (count == 1 ? " note" : " notes");
}
} // namespace

Beats generatorClipOrigin(const Clip& clip, const ClipLocation& location)
{
    if (location.kind == ClipLocation::Kind::Session) return 0.0;
    return clip.currentStart - clip.loop.startRelative;
}

EditCommand applyGeneratorToClip(const Song& song, const ClipLocation& location, const GeneratorClipSpec& spec, std::string& message)
{
    const auto* clip = findClip(song, location);
    if (clip == nullptr || clip->kind != Clip::Kind::Midi)
    {
        message = spec.deviceName + " writes into MIDI clips";
        return {};
    }
    if (! spec.render)
    {
        message = spec.deviceName + " cannot render here";
        return {};
    }
    const Beats loopStart = clip->loop.loopStart, loopEnd = clip->loop.loopEnd;
    if (loopEnd - loopStart < 1.0 / 64)
    {
        message = "The clip is too short";
        return {};
    }
    const Beats origin = generatorClipOrigin(*clip, location);
    std::vector<MidiNote> input;
    if (spec.usesInput)
        for (auto note : clip->notes)
        {
            if (! note.enabled || note.start >= loopEnd || note.start + note.duration <= loopStart) continue;
            const Beats end = std::min(note.start + note.duration, loopEnd);
            note.start = std::max(note.start, loopStart);
            note.duration = end - note.start;
            note.start = note.start - loopStart + origin;
            input.push_back(note);
        }
    auto generated = toContent(spec.render(input, origin, origin + (loopEnd - loopStart)), origin, loopStart, loopEnd);
    message = notesWritten(spec.deviceName, generated.size());
    if (generated.empty() && spec.usesInput && input.empty()) message = spec.deviceName + " needs notes in the clip to play from";

    // Notes outside the span stay where they are.
    std::vector<MidiNote> notes;
    for (const auto& note : clip->notes)
        if (note.start < loopStart - 1e-9 || note.start >= loopEnd - 1e-9) notes.push_back(note);
    notes.insert(notes.end(), generated.begin(), generated.end());
    std::stable_sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    return setNotes(song, location, std::move(notes), "Apply " + spec.deviceName + " to Clip");
}

EditCommand insertGeneratorClip(const Song& song, int track, Beats start, Beats length, const GeneratorClipSpec& spec,
                                std::string& message)
{
    if (track < 0 || (size_t) track >= song.tracks.size() || song.tracks[(size_t) track].kind != TrackKind::Midi)
    {
        message = spec.deviceName + " writes clips on MIDI tracks";
        return {};
    }
    if (! spec.render || length < 1.0 / 64)
    {
        message = spec.deviceName + " cannot render here";
        return {};
    }
    start = std::max(0.0, start);
    for (const auto& clip : song.tracks[(size_t) track].arrangementClips)
        if (clip.currentStart < start + length - 1e-9 && clip.currentEnd > start + 1e-9)
        {
            message = "A clip is in the way: select it to write " + spec.deviceName + " into it";
            return {};
        }
    Clip clip;
    if (! makeEmptyMidiClip(song, start, length, clip))
    {
        message = "This set has no MIDI clip format to create a clip from";
        return {};
    }
    clip.colorIndex = song.tracks[(size_t) track].colorIndex;
    clip.name = spec.deviceName;
    clip.notes = toContent(spec.render({}, start, start + length), start, 0.0, length);
    message = notesWritten(spec.deviceName, clip.notes.size()) + " into a new clip";
    return insertClip(track, std::move(clip), "Apply " + spec.deviceName + " to Clip");
}

Beats generatorClipStart(const Song& song, Beats playhead, bool loopOn, Beats loopStart)
{
    if (loopOn) return std::max(0.0, loopStart);
    return std::max(0.0, grid::startOf(playhead, grid::beatsPerBar(song.timeSigNumerator, song.timeSigDenominator)));
}
} // namespace sonora::model
