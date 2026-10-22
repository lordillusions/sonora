#include "core/ClipOperations.h"
#include "core/TimeUtils.h"
#include <algorithm>
#include <cmath>

namespace sonora::model
{
std::optional<std::pair<Beats, Beats>> arrangementClipSpan(const Song& song, const std::vector<int>& tracks)
{
    std::optional<std::pair<Beats, Beats>> span;
    for (int index : tracks)
    {
        if (index < 0 || index >= (int) song.tracks.size()) continue;
        for (const auto& clip : song.tracks[(size_t) index].arrangementClips)
        {
            if (clip.disabled || clip.currentEnd <= clip.currentStart) continue;
            if (! span) span = {clip.currentStart, clip.currentEnd};
            else { span->first = std::min(span->first, clip.currentStart); span->second = std::max(span->second, clip.currentEnd); }
        }
    }
    return span;
}

EditCommand replaceTrack(const Song& song, int index, Track after, std::string label, int pointeeEnd)
{
    if (index < 0 || index >= (int) song.tracks.size()) return {};
    const auto before = song.tracks[(size_t) index];
    EditCommand cmd;
    cmd.label = std::move(label); cmd.tracks = {index}; cmd.structureChanged = true;
    cmd.apply = [index, after, pointeeEnd](Song& s) {
        s.tracks[(size_t) index] = after;
        s.nextPointeeId = std::max(s.nextPointeeId, pointeeEnd);
        auto reserve = [&](int id) { s.nextPointeeId = std::max(s.nextPointeeId, id + 1); };
        reserve(after.mixer.volumeTargetId); reserve(after.mixer.panTargetId); reserve(after.mixer.speakerTargetId);
        for (int id : after.mixer.sendTargetIds) reserve(id);
        s.pointeeIdsAllocated = true;
    };
    cmd.revert = [index, before](Song& s) { s.tracks[(size_t) index] = before; };
    return cmd;
}

EditCommand replaceArrangementRange(const Song& song, int index, double start, double end, Clip replacement)
{
    if (index < 0 || index >= (int) song.tracks.size() || ! std::isfinite(start) || ! std::isfinite(end) || end <= start) return {};
    const auto before = song.tracks[(size_t) index].arrangementClips;
    std::vector<Clip> after;
    for (const auto& clip : before)
    {
        if (clip.currentEnd <= start || clip.currentStart >= end) { after.push_back(clip); continue; }
        if (clip.currentStart < start)
        {
            auto head = withEnd(clip, start); head.currentEnd = start; after.push_back(std::move(head));
        }
        if (clip.currentEnd > end)
        {
            auto tail = withStart(clip, end, song.tempo); tail.currentStart = tail.time = end;
            tail.uid = allocateClipUid(); after.push_back(std::move(tail));
        }
    }
    assignNoteKeys(replacement.notes);
    replacement.uid = allocateClipUid(); replacement.currentStart = replacement.time = start; replacement.currentEnd = end;
    after.push_back(std::move(replacement));
    std::stable_sort(after.begin(), after.end(), [](const auto& a, const auto& b) { return a.currentStart < b.currentStart; });
    EditCommand cmd; cmd.label = "Consolidate"; cmd.tracks = {index};
    cmd.apply = [index, after](Song& s) { s.tracks[(size_t) index].arrangementClips = after; };
    cmd.revert = [index, before](Song& s) { s.tracks[(size_t) index].arrangementClips = before; };
    return cmd;
}

std::vector<AutomationEnvelope> consolidatedEnvelopes(const Song& song, int index, double start, double end)
{
    std::vector<AutomationEnvelope> output;
    if (index < 0 || index >= (int) song.tracks.size() || end <= start) return output;
    const auto& track = song.tracks[(size_t) index];
    auto baseValue = [&](const AutomationEnvelope& envelope) {
        if (isMidiController(envelope.midiController)) return (double) midiControllerDefault(envelope.midiController);
        if (envelope.targetId == track.mixer.volumeTargetId) return track.mixer.volume;
        if (envelope.targetId == track.mixer.panTargetId) return track.mixer.pan;
        if (envelope.targetId == track.mixer.speakerTargetId) return track.mixer.speakerOn ? 1.0 : 0.0;
        for (size_t i = 0; i < track.mixer.sendTargetIds.size() && i < track.mixer.sends.size(); ++i)
            if (envelope.targetId == track.mixer.sendTargetIds[i]) return track.mixer.sends[i];
        for (const auto& device : track.devices)
        {
            if (envelope.targetId == device.enabledTargetId) return device.enabled ? 1.0 : 0.0;
            for (const auto& param : device.parameters) if (param.automationTargetId == envelope.targetId) return param.value;
            for (const auto& param : device.pluginParams) if (param.automationTargetId == envelope.targetId) return param.value;
        }
        return envelope.points.empty() ? 0.0 : envelope.points.front().value;
    };
    for (const auto& clip : track.arrangementClips)
    {
        if (clip.disabled || clip.currentStart >= end || clip.currentEnd <= start) continue;
        // Clip envelope clocks follow the engine's content-beat mapping.
        constexpr double units = 1.0;
        const double loopLength = clip.loop.loopEnd - clip.loop.loopStart;
        for (const auto& envelope : clip.envelopes)
        {
            if (envelope.points.empty()) continue;
            auto found = std::find_if(output.begin(), output.end(), [&](const auto& candidate) {
                return candidate.targetId == envelope.targetId && candidate.midiController == envelope.midiController;
            });
            const double base = baseValue(envelope);
            if (found == output.end())
            {
                auto fresh = envelope; fresh.points = {{-63072000, base}, {0, base}};
                output.push_back(std::move(fresh)); found = std::prev(output.end());
            }
            auto valueAt = [&](double at) {
                const auto next = std::upper_bound(envelope.points.begin(), envelope.points.end(), at,
                    [](double time, const auto& point) { return time < point.time; });
                if (next == envelope.points.begin()) return next->value;
                const auto& previous = *std::prev(next);
                if (next == envelope.points.end() || envelope.stepped || next->time <= previous.time) return previous.value;
                return previous.value + (next->value - previous.value) * (at - previous.time) / (next->time - previous.time);
            };
            const double from = std::max(start, clip.currentStart), to = std::min(end, clip.currentEnd);
            double position = from;
            while (position < to - 1e-9 && found->points.size() < 1000000)
            {
                const double content = clipContentPosition(position, clip.currentStart, clip.loop.loopStart / units,
                    clip.loop.loopEnd / units, clip.loop.startRelative / units, clip.loop.enabled) * units;
                const double stop = clip.loop.enabled && loopLength > 0 ? std::min(to, position + (clip.loop.loopEnd - content) / units) : to;
                if (stop <= position + 1e-9) break;
                found->points.push_back({position - start, valueAt(content)});
                for (const auto& point : envelope.points)
                    if (point.time > content && point.time < content + (stop - position) * units)
                        found->points.push_back({position - start + (point.time - content) / units, point.value});
                found->points.push_back({std::nextafter(stop - start, -INFINITY), valueAt(content + (stop - position) * units)});
                position = stop;
            }
            found->points.push_back({to - start, base});
        }
    }
    for (auto& envelope : output)
        std::stable_sort(envelope.points.begin(), envelope.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    return output;
}

Clip consolidatedMidi(const Song& song, int index, double start, double end)
{
    Clip out;
    makeEmptyMidiClip(song, start, end - start, out);
    out.notes.clear(); out.noteExpressions.clear(); out.envelopes.clear(); out.stripSourceEnvelopes = true;
    out.resetSourceMarkers = true; out.currentStart = out.time = start; out.currentEnd = end;
    out.loop = {0, end - start, 0, true};
    if (index < 0 || index >= (int) song.tracks.size() || end <= start) return out;
    out.colorIndex = song.tracks[(size_t) index].colorIndex;
    int nextId = 0;
    for (const auto& clip : song.tracks[(size_t) index].arrangementClips)
    {
        if (clip.disabled || clip.kind != Clip::Kind::Midi || clip.currentStart >= end || clip.currentEnd <= start) continue;
        if (out.name.empty()) out.name = clip.name;
        const double from = std::max(start, clip.currentStart), to = std::min(end, clip.currentEnd);
        const double contentStart = clip.loop.loopStart + clip.loop.startRelative;
        const double loopLength = clip.loop.loopEnd - clip.loop.loopStart;
        const bool looping = clip.loop.enabled && loopLength > 0;
        for (const auto& note : clip.notes)
        {
            if (looping && (note.start < clip.loop.loopStart || note.start >= clip.loop.loopEnd)) continue;
            const double first = clip.currentStart + note.start - contentStart;
            const int firstPass = looping ? std::max(0, (int) std::floor((from - first - note.duration) / loopLength)) : 0;
            const int lastPass = looping ? std::max(0, (int) std::ceil((to - first) / loopLength)) : 0;
            for (int pass = firstPass; pass <= lastPass && pass - firstPass < 100000; ++pass)
            {
                const double on = first + (looping ? pass * loopLength : 0);
                const double off = std::min(on + note.duration, looping ? first + (pass + 1) * loopLength : to);
                if (off <= from || on >= to) continue;
                auto n = note; n.start = std::max(on, from) - start; n.duration = std::min(off, to) - std::max(on, from);
                n.noteId = nextId++; n.key = 0; out.notes.push_back(n);
                for (const auto& expression : clip.noteExpressions) if (expression.noteId == note.noteId)
                {
                    auto e = expression; e.noteId = n.noteId; e.edited = true;
                    const double trim = std::max(on, from) - on;
                    for (auto& point : e.points) point.time -= trim;
                    e.points.erase(std::remove_if(e.points.begin(), e.points.end(), [&](const auto& p) { return p.time < 0 || p.time > n.duration; }), e.points.end());
                    out.noteExpressions.push_back(std::move(e));
                }
            }
        }
    }
    out.envelopes = consolidatedEnvelopes(song, index, start, end);
    std::stable_sort(out.notes.begin(), out.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return out;
}

Track frozenPlaybackTrack(const Track& before)
{
    if (! before.frozen) return before;
    auto track = before;
    track.frozen = false;
    track.kind = TrackKind::Audio;
    track.devices.clear(); track.takeLanes.clear();
    track.arrangementClips = before.frozenArrangement;
    track.slots = before.frozenSlots;
    // Device automation is baked. Mixer and send automation remain live and are applied once.
    auto mixerTarget = [&](int id) {
        return id == track.mixer.volumeTargetId || id == track.mixer.panTargetId || id == track.mixer.speakerTargetId
            || std::find(track.mixer.sendTargetIds.begin(), track.mixer.sendTargetIds.end(), id) != track.mixer.sendTargetIds.end();
    };
    track.automation.erase(std::remove_if(track.automation.begin(), track.automation.end(), [&](const auto& e) { return ! mixerTarget(e.targetId); }), track.automation.end());
    return track;
}
}
