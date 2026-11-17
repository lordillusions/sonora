#include "core/MultiClipNotes.h"

#include "core/ClipGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::model
{

std::vector<MultiClipMember> midiClipsInSelection(const Song& song, int firstTrack, int lastTrack, double start, double end)
{
    std::vector<MultiClipMember> members;
    if (end <= start) return members;
    const int first = std::max(0, firstTrack), last = std::min(lastTrack, (int) song.tracks.size() - 1);
    int tracksUsed = 0;
    for (int t = first; t <= last && tracksUsed < kMaxMultiClipTracks; ++t)
    {
        const auto& track = song.tracks[(size_t) t];
        if (track.kind != TrackKind::Midi) continue;
        bool any = false;
        for (const auto& clip : track.arrangementClips)
        {
            if (clip.kind != Clip::Kind::Midi || clip.currentStart >= end || clip.currentEnd <= start) continue;
            const auto markers = clipMarkers(clip, true, song.tempo);
            members.push_back({ClipLocation::arrangement(t, clip.uid), clip.currentStart - markers.start, clip.currentStart,
                               clip.currentEnd, clip.colorIndex});
            any = true;
        }
        if (any) ++tracksUsed;
    }
    return members;
}

MergedNotes mergeNotes(const Song& song, const std::vector<MultiClipMember>& members)
{
    MergedNotes merged;
    for (int m = 0; m < (int) members.size(); ++m)
    {
        const auto* clip = findClip(song, members[(size_t) m].location);
        if (clip == nullptr) continue;
        for (const auto& note : clip->notes)
        {
            auto shifted = note;
            shifted.start = note.start + members[(size_t) m].offset;
            merged.origin.push_back({m, note.key, note.start});
            shifted.key = (uint32_t) merged.origin.size();
            merged.notes.push_back(shifted);
        }
    }
    return merged;
}

int memberOfNote(const MergedNotes& base, const std::vector<MultiClipMember>& members, const MidiNote& note, int focus)
{
    if (members.empty()) return -1;
    focus = std::clamp(focus, 0, (int) members.size() - 1);
    if (note.key > 0 && note.key <= base.origin.size())
    {
        const int member = base.origin[note.key - 1].member;
        if (member >= 0 && member < (int) members.size()) return member;
    }
    auto contains = [&](int m) { return note.start >= members[(size_t) m].start - 1e-9 && note.start < members[(size_t) m].end - 1e-9; };
    if (contains(focus)) return focus;
    const int track = members[(size_t) focus].location.track;
    for (int m = 0; m < (int) members.size(); ++m)
        if (members[(size_t) m].location.track == track && contains(m)) return m;
    for (int m = 0; m < (int) members.size(); ++m)
        if (contains(m)) return m;
    int nearest = focus;
    double best = std::numeric_limits<double>::max();
    for (int m = 0; m < (int) members.size(); ++m)
    {
        if (members[(size_t) m].location.track != track) continue;
        const double distance = note.start < members[(size_t) m].start ? members[(size_t) m].start - note.start : note.start - members[(size_t) m].end;
        if (distance < best) { best = distance; nearest = m; }
    }
    return nearest;
}

namespace
{
bool sameNote(const MidiNote& a, const MidiNote& b)
{
    return std::abs(a.start - b.start) < 1e-9 && std::abs(a.duration - b.duration) < 1e-9 && a.pitch == b.pitch
           && std::abs(a.velocity - b.velocity) < 1e-6f && std::abs(a.releaseVelocity - b.releaseVelocity) < 1e-6f
           && std::abs(a.probability - b.probability) < 1e-6f && a.enabled == b.enabled && a.noteId == b.noteId
           && std::abs(a.velocityDeviation - b.velocityDeviation) < 1e-6f && a.key == b.key;
}
bool sortedBefore(const MidiNote& a, const MidiNote& b) { return a.start < b.start || (! (b.start < a.start) && a.pitch < b.pitch); }
} // namespace

EditCommand setMergedNotes(const Song& song, const std::vector<MultiClipMember>& members, const MergedNotes& base,
                           const std::vector<MidiNote>& edited, int focus, std::string label)
{
    std::vector<std::vector<MidiNote>> perClip(members.size());
    for (const auto& note : edited)
    {
        const int m = memberOfNote(base, members, note, focus);
        if (m < 0) continue;
        auto local = note;
        local.start = note.start - members[(size_t) m].offset;
        local.key = 0;
        if (note.key > 0 && note.key <= base.origin.size())
        {
            const auto& origin = base.origin[note.key - 1];
            local.key = origin.key;
            // A note that did not move keeps its stored start exactly (no rounding through the offset).
            if (std::abs(base.notes[note.key - 1].start - note.start) < 1e-12) local.start = origin.contentStart;
        }
        perClip[(size_t) m].push_back(local);
    }
    std::vector<EditCommand> parts;
    for (int m = 0; m < (int) members.size(); ++m)
    {
        const auto* clip = findClip(song, members[(size_t) m].location);
        if (clip == nullptr) continue;
        auto& notes = perClip[(size_t) m];
        std::stable_sort(notes.begin(), notes.end(), sortedBefore);
        auto before = clip->notes;
        std::stable_sort(before.begin(), before.end(), sortedBefore);
        const bool unchanged = notes.size() == before.size()
                               && std::equal(notes.begin(), notes.end(), before.begin(), sameNote);
        if (! unchanged) parts.push_back(setNotes(song, members[(size_t) m].location, std::move(notes), label));
    }
    if (parts.empty()) return {};
    if (parts.size() == 1) { parts.front().label = std::move(label); return std::move(parts.front()); }
    return compose(std::move(label), std::move(parts));
}

} // namespace sonora::model
