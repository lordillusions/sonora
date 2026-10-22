#include "core/Edit.h"
#include "core/DeviceCatalog.h"
#include "core/TimeUtils.h"

#include <memory>

#include <algorithm>
#include <cmath>
#include <set>

namespace sonora::model
{

namespace
{

void sortClips(Track& t)
{
    std::stable_sort(t.arrangementClips.begin(), t.arrangementClips.end(),
                     [](const Clip& a, const Clip& b) { return a.currentStart < b.currentStart; });
}
// The copy is a new element in the file, cloned from the original's.
void markAsCopy(Clip& copy, const Song& song, int originalTrack)
{
    copy.cloneOfXmlId = copy.xmlId >= 0 ? copy.xmlId : copy.cloneOfXmlId;
    if (copy.xmlId >= 0 && originalTrack >= 0 && (size_t) originalTrack < song.tracks.size())
        copy.cloneOfTrackId = song.tracks[(size_t) originalTrack].id;
    copy.xmlId = -1;
    copy.linkGroup = -1; // TRK-5: only Duplicate as Linked links clips
}
} // namespace

void EditHistory::perform(Song& song, EditCommand cmd)
{
    cmd.apply(song);
    cmd.historyBefore = currentState_;
    currentState_ = cmd.historyAfter = nextState_++;
    done_.push_back(std::move(cmd));
    undone_.clear();
    ++version_;
}

void EditHistory::amendLast(Song& song, EditCommand extra)
{
    if (done_.empty() || ! extra.apply || ! extra.revert) return;
    extra.apply(song);
    auto& last = done_.back();
    last.apply = [first = std::move(last.apply), second = extra.apply](Song& s) { first(s); second(s); };
    last.revert = [first = std::move(last.revert), second = extra.revert](Song& s) { second(s); first(s); };
    for (int t : extra.tracks) if (std::find(last.tracks.begin(), last.tracks.end(), t) == last.tracks.end()) last.tracks.push_back(t);
    ++version_;
}

const EditCommand* EditHistory::undo(Song& song)
{
    if (done_.empty())
        return nullptr;
    undone_.push_back(std::move(done_.back()));
    done_.pop_back();
    undone_.back().revert(song);
    currentState_ = undone_.back().historyBefore;
    ++version_;
    return &undone_.back();
}

const EditCommand* EditHistory::redo(Song& song)
{
    if (undone_.empty())
        return nullptr;
    done_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    done_.back().apply(song);
    currentState_ = done_.back().historyAfter;
    ++version_;
    return &done_.back();
}

const Track* trackAt(const Song& song, int index)
{
    if (index < 0) return nullptr;
    const int nt = (int) song.tracks.size(), nr = (int) song.returns.size();
    if (index < nt) return &song.tracks[(size_t) index];
    if (index < nt + nr) return &song.returns[(size_t) (index - nt)];
    return index == nt + nr ? &song.main : nullptr;
}

Track* trackAt(Song& song, int index)
{
    const int nt = (int) song.tracks.size(), nr = (int) song.returns.size();
    if (index < 0)
        return nullptr;
    if (index < nt)
        return &song.tracks[(size_t) index];
    if (index < nt + nr)
        return &song.returns[(size_t) (index - nt)];
    return index == nt + nr ? &song.main : nullptr;
}

int arrangementClipIndex(const Song& song, int track, int uid)
{
    if (track < 0 || (size_t) track >= song.tracks.size())
        return -1;
    const auto& clips = song.tracks[(size_t) track].arrangementClips;
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].uid == uid)
            return (int) i;
    return -1;
}

const Clip* findArrangementClip(const Song& song, int track, int uid)
{
    const int i = arrangementClipIndex(song, track, uid);
    return i < 0 ? nullptr : &song.tracks[(size_t) track].arrangementClips[(size_t) i];
}

Clip* findArrangementClip(Song& song, int track, int uid)
{
    const int i = arrangementClipIndex(song, track, uid);
    return i < 0 ? nullptr : &song.tracks[(size_t) track].arrangementClips[(size_t) i];
}

const TakeLane* findTakeLane(const Song& song, int track, int laneId)
{
    if (track < 0 || (size_t) track >= song.tracks.size() || laneId < 0) return nullptr;
    for (const auto& lane : song.tracks[(size_t) track].takeLanes)
        if (lane.id == laneId) return &lane;
    return nullptr;
}

TakeLane* findTakeLane(Song& song, int track, int laneId)
{
    return const_cast<TakeLane*>(findTakeLane(static_cast<const Song&>(song), track, laneId));
}

const Clip* findClip(const Song& song, ClipLocation location)
{
    if (location.uid < 0) return nullptr;
    if (location.kind == ClipLocation::Kind::Arrangement)
        return findArrangementClip(song, location.track, location.uid);
    if (location.kind == ClipLocation::Kind::Take)
    {
        if (const auto* lane = findTakeLane(song, location.track, location.laneId))
            for (const auto& clip : lane->clips) if (clip.uid == location.uid) return &clip;
        return nullptr;
    }
    if (location.track < 0 || (size_t) location.track >= song.tracks.size() || location.scene < 0) return nullptr;
    const auto& slots = song.tracks[(size_t) location.track].slots;
    if ((size_t) location.scene >= slots.size() || ! slots[(size_t) location.scene].clip) return nullptr;
    const auto& clip = *slots[(size_t) location.scene].clip;
    return clip.uid == location.uid ? &clip : nullptr;
}

Clip* findClip(Song& song, ClipLocation location)
{
    return const_cast<Clip*>(findClip(static_cast<const Song&>(song), location));
}

const Clip* findClip(const Song& song, int track, int uid, int scene)
{
    return findClip(song, scene < 0 ? ClipLocation::arrangement(track, uid) : ClipLocation::session(track, uid, scene));
}

Clip* findClip(Song& song, int track, int uid, int scene)
{
    return const_cast<Clip*>(findClip(static_cast<const Song&>(song), track, uid, scene));
}

EditCommand modifyClip(const Song& song, ClipLocation location, Clip after, std::string label)
{
    const Clip* current = findClip(song, location);
    Clip before = current != nullptr ? *current : after;
    if (before.sample.absolutePath != after.sample.absolutePath || before.sample.relativePath != after.sample.relativePath)
    {
        after.sample.sourceDirectory.clear();
        after.resetSourceMarkers = true;
    }
    if (! before.envelopes.empty() && after.envelopes.empty()) after.stripSourceEnvelopes = true;
    after.uid = location.uid;
    assignNoteKeys(after.notes);
    EditCommand c;
    c.label = std::move(label); c.tracks = {location.track};
    c.takeLanesOnly = location.kind == ClipLocation::Kind::Take;
    auto replace = [location](Song& s, const Clip& with) {
        if (auto* clip = findClip(s, location))
        {
            *clip = with;
            if (location.kind == ClipLocation::Kind::Arrangement) sortClips(s.tracks[(size_t) location.track]);
            if (location.kind == ClipLocation::Kind::Take)
            {
                auto& clips = findTakeLane(s, location.track, location.laneId)->clips;
                std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.currentStart < b.currentStart; });
                s.tracks[(size_t) location.track].takeLanesNeedPatch = true;
            }
        }
    };
    c.apply = [replace, after](Song& s) { replace(s, after); };
    c.revert = [replace, before](Song& s) { replace(s, before); };
    return c;
}

EditCommand modifyClip(const Song& song, int track, int uid, Clip after, std::string label, int scene)
{
    return modifyClip(song, scene < 0 ? ClipLocation::arrangement(track, uid) : ClipLocation::session(track, uid, scene),
                      std::move(after), std::move(label));
}

EditCommand insertClip(int track, Clip clip, std::string label)
{
    clip.stripSourceEnvelopes |= clip.envelopes.empty();
    assignNoteKeys(clip.notes);
    EditCommand c;
    c.label = std::move(label);
    c.tracks = {track};
    auto uid = std::make_shared<int>(-1); // assigned on the first apply and kept for redo
    auto targets = std::make_shared<EditCommand>();
    auto targetsPrepared = std::make_shared<bool>(false);
    c.apply = [track, clip, uid, targets, targetsPrepared](Song& s) mutable {
        Clip copy = clip;
        if (! *targetsPrepared)
        {
            *targets = assignRecordedMidiTargets(s, track, {&copy});
            *targetsPrepared = true;
        }
        if (targets->apply) targets->apply(s);
        for (auto& env : copy.envelopes)
            if (isMidiController(env.midiController)) env.targetId = s.tracks[(size_t) track].midiControllerTargets[(size_t) env.midiController];
        if (*uid < 0)
            *uid = allocateClipUid();
        copy.uid = *uid;
        s.tracks[(size_t) track].arrangementClips.push_back(std::move(copy));
        sortClips(s.tracks[(size_t) track]);
    };
    c.revert = [track, uid, targets](Song& s) {
        auto& clips = s.tracks[(size_t) track].arrangementClips;
        clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const Clip& x) { return x.uid == *uid; }), clips.end());
        if (targets->revert) targets->revert(s);
    };
    return c;
}

EditCommand deleteClip(const Song& song, int track, int uid)
{
    const int i = arrangementClipIndex(song, track, uid);
    Clip removed = i >= 0 ? song.tracks[(size_t) track].arrangementClips[(size_t) i] : Clip{};
    EditCommand c;
    c.label = "Delete Clip";
    c.tracks = {track};
    c.apply = [track, uid](Song& s) {
        auto& clips = s.tracks[(size_t) track].arrangementClips;
        clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const Clip& x) { return x.uid == uid; }), clips.end());
    };
    c.revert = [track, removed](Song& s) {
        s.tracks[(size_t) track].arrangementClips.push_back(removed);
        sortClips(s.tracks[(size_t) track]);
    };
    return c;
}

EditCommand splitArrangementClips(const Song& song, int firstTrack, int lastTrack, Beats start, Beats end)
{
    if (! std::isfinite(start) || start < 0 || firstTrack < 0 || lastTrack < firstTrack || lastTrack >= (int) song.tracks.size()) return {};
    std::vector<EditCommand> edits;
    for (int track = firstTrack; track <= lastTrack; ++track)
    {
        if (song.tracks[(size_t) track].frozen) continue;
        for (const auto& clip : song.tracks[(size_t) track].arrangementClips)
        {
            std::vector<double> boundaries{clip.currentStart};
            for (double point : {start, end})
                if (std::isfinite(point) && point > boundaries.back() + 1e-9 && point < clip.currentEnd - 1e-9) boundaries.push_back(point);
            if (boundaries.size() == 1) continue;
            boundaries.push_back(clip.currentEnd);
            for (size_t part = 0; part + 1 < boundaries.size(); ++part)
            {
                auto fragment = clip;
                fragment.currentStart = fragment.time = boundaries[part];
                fragment.currentEnd = boundaries[part + 1];
                const double units = clip.kind == Clip::Kind::Audio && ! clip.warped ? 60 / std::max(20.0, song.tempo) : 1;
                fragment.loop.startRelative += (boundaries[part] - clip.currentStart) * units;
                if (part > 0) fragment.fadeIn = 0;
                if (part + 2 < boundaries.size()) fragment.fadeOut = 0;
                if (part == 0) edits.push_back(modifyClip(song, track, clip.uid, std::move(fragment), "Split Clip"));
                else
                {
                    markAsCopy(fragment, song, track);
                    edits.push_back(insertClip(track, std::move(fragment), "Split Clip"));
                }
            }
        }
    }
    return edits.empty() ? EditCommand{} : compose("Split Clip", std::move(edits));
}

EditCommand stripArrangementClip(const Song& song, int track, int uid, const std::vector<std::pair<Beats, Beats>>& keep)
{
    const int index = arrangementClipIndex(song, track, uid);
    if (index < 0 || song.tracks[(size_t) track].frozen) return {};
    const auto& clip = song.tracks[(size_t) track].arrangementClips[(size_t) index];
    std::vector<std::pair<Beats, Beats>> ranges;
    for (auto [a, b] : keep)
    {
        a = std::max(a, clip.currentStart); b = std::min(b, clip.currentEnd);
        if (! std::isfinite(a) || ! std::isfinite(b) || b - a < 1e-6) continue;
        if (! ranges.empty() && a <= ranges.back().second + 1e-9) ranges.back().second = std::max(ranges.back().second, b);
        else ranges.push_back({a, b});
    }
    if (ranges.empty())
    {
        auto c = deleteClip(song, track, uid); c.label = "Strip Silence"; return c;
    }
    if (ranges.size() == 1 && ranges[0].first <= clip.currentStart + 1e-6 && ranges[0].second >= clip.currentEnd - 1e-6) return {};
    std::vector<EditCommand> edits;
    const double units = clip.kind == Clip::Kind::Audio && ! clip.warped ? 60 / std::max(20.0, song.tempo) : 1;
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        auto fragment = clip;
        fragment.currentStart = fragment.time = ranges[i].first;
        fragment.currentEnd = ranges[i].second;
        fragment.loop.startRelative += (ranges[i].first - clip.currentStart) * units;
        if (ranges[i].first > clip.currentStart + 1e-6) fragment.fadeIn = 0;
        if (ranges[i].second < clip.currentEnd - 1e-6) fragment.fadeOut = 0;
        if (i == 0) edits.push_back(modifyClip(song, track, clip.uid, std::move(fragment), "Strip Silence"));
        else
        {
            markAsCopy(fragment, song, track);
            edits.push_back(insertClip(track, std::move(fragment), "Strip Silence"));
        }
    }
    return compose("Strip Silence", std::move(edits));
}

EditCommand duplicateClip(const Song& song, int track, int uid, Beats at)
{
    const int i = arrangementClipIndex(song, track, uid);
    Clip copy = i >= 0 ? song.tracks[(size_t) track].arrangementClips[(size_t) i] : Clip{};
    const Beats length = copy.currentEnd - copy.currentStart;
    const Beats start = at >= 0.0 ? at : copy.currentEnd;
    copy = movedClip(copy, start);
    markAsCopy(copy, song, track);
    (void) length;
    return insertClip(track, std::move(copy), "Duplicate Clip");
}

EditCommand copyClipToTrack(const Song& song, int fromTrack, int uid, int toTrack, Beats at)
{
    const int i = arrangementClipIndex(song, fromTrack, uid);
    Clip copy = i >= 0 ? song.tracks[(size_t) fromTrack].arrangementClips[(size_t) i] : Clip{};
    copy = movedClip(copy, at);
    markAsCopy(copy, song, fromTrack);
    return insertClip(toTrack, std::move(copy), "Copy Clip");
}

EditCommand duplicateArrangementRegion(const Song& song, int firstTrack, int lastTrack, Beats start, Beats end)
{
    std::vector<EditCommand> parts;
    const double length = end - start, destinationEnd = end + length;
    if (! std::isfinite(start) || ! std::isfinite(end) || start < 0 || length <= 1e-9)
        return compose("Duplicate Region", {});
    for (int track = std::max(0, firstTrack); track <= lastTrack && track < (int) song.tracks.size(); ++track)
    {
        const auto& source = song.tracks[(size_t) track];
        for (const auto& clip : source.arrangementClips)
        {
            if (clip.currentStart < destinationEnd && clip.currentEnd > end)
            {
                const bool left = clip.currentStart < end, right = clip.currentEnd > destinationEnd;
                if (left)
                {
                    auto head = clip; head.currentEnd = end;
                    parts.push_back(modifyClip(song, track, clip.uid, std::move(head), "Trim Region"));
                }
                if (right)
                {
                    auto tail = withStart(clip, destinationEnd, song.tempo);
                    // withStart clamps to the editor minimum; range edits retain even tiny fragments.
                    tail.loop.startRelative = clip.loop.startRelative + (destinationEnd - clip.currentStart)
                        * (clip.kind == Clip::Kind::Audio && ! clip.warped ? 60.0 / std::max(20.0, song.tempo) : 1.0);
                    tail.currentStart = tail.time = destinationEnd;
                    if (left)
                    {
                        markAsCopy(tail, song, track);
                        parts.push_back(insertClip(track, std::move(tail), "Keep Region Tail"));
                    }
                    else parts.push_back(modifyClip(song, track, clip.uid, std::move(tail), "Trim Region"));
                }
                if (! left && ! right) parts.push_back(deleteClip(song, track, clip.uid));
            }
            if (clip.currentStart < end && clip.currentEnd > start)
            {
                auto copy = clip;
                const double from = std::max(start, clip.currentStart), to = std::min(end, clip.currentEnd);
                copy.loop.startRelative += (from - clip.currentStart)
                    * (clip.kind == Clip::Kind::Audio && ! clip.warped ? 60.0 / std::max(20.0, song.tempo) : 1.0);
                copy.currentStart = copy.time = from + length;
                copy.currentEnd = to + length;
                markAsCopy(copy, song, track);
                parts.push_back(insertClip(track, std::move(copy), "Duplicate Region"));
            }
        }
        auto automation = source.automation;
        for (auto& changed : automation)
        {
            const auto envelope = changed;
            // A lone pre-roll point is the stored manual value, not an authored automation lane.
            if (std::none_of(envelope.points.begin(), envelope.points.end(), [](const auto& point) { return point.time >= 0; })) continue;
            auto valueAt = [&](double beat) {
                const auto it = std::upper_bound(envelope.points.begin(), envelope.points.end(), beat,
                    [](double time, const AutomationPoint& point) { return time < point.time; });
                if (it == envelope.points.begin()) return it->value;
                const auto& before = *std::prev(it);
                if (it == envelope.points.end() || envelope.stepped) return before.value;
                const double fraction = (beat - before.time) / std::max(1e-12, it->time - before.time);
                return before.value + fraction * (it->value - before.value);
            };
            std::erase_if(changed.points, [&](const auto& point) { return point.time >= end && point.time <= destinationEnd; });
            changed.points.push_back({end, valueAt(end)}); // retain the source segment right up to the boundary
            changed.points.push_back({end, valueAt(start)});
            for (const auto& point : envelope.points)
                if (point.time > start && point.time < end) changed.points.push_back({point.time + length, point.value});
            changed.points.push_back({destinationEnd, valueAt(end)});
            changed.points.push_back({destinationEnd, valueAt(destinationEnd)});
            std::stable_sort(changed.points.begin(), changed.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        }
        if (! automation.empty())
        {
            EditCommand command;
            command.tracks = {track};
            command.apply = [track, automation](Song& s) { s.tracks[(size_t) track].automation = automation; };
            command.revert = [track, before = source.automation](Song& s) { s.tracks[(size_t) track].automation = before; };
            parts.push_back(std::move(command));
        }
    }
    return compose("Duplicate Region", std::move(parts));
}

namespace
{
struct RegionTarget { int id; double base; };

std::optional<RegionTarget> regionTarget(const Track& from, const Track& to, int id)
{
    if (&from == &to) return RegionTarget{id, 0};
    std::optional<RegionTarget> result;
    auto match = [&](int original, int destination, double base) {
        if (id >= 0 && id == original && destination >= 0) result = RegionTarget{destination, base};
    };
    match(from.mixer.volumeTargetId, to.mixer.volumeTargetId, to.mixer.volume);
    match(from.mixer.panTargetId, to.mixer.panTargetId, to.mixer.pan);
    match(from.mixer.speakerTargetId, to.mixer.speakerTargetId, to.mixer.speakerOn ? 1 : 0);
    for (size_t i = 0; i < from.mixer.sendTargetIds.size() && i < to.mixer.sendTargetIds.size(); ++i)
        match(from.mixer.sendTargetIds[i], to.mixer.sendTargetIds[i], i < to.mixer.sends.size() ? to.mixer.sends[i] : 0);
    for (size_t i = 0; i < from.midiControllerTargets.size(); ++i)
        match(from.midiControllerTargets[i], to.midiControllerTargets[i], midiControllerDefault((int) i));
    for (size_t i = 0; i < from.devices.size() && i < to.devices.size(); ++i)
    {
        const auto& a = from.devices[i]; const auto& b = to.devices[i];
        if (a.type != b.type || a.plugin.has_value() != b.plugin.has_value()) continue;
        if (a.plugin && (a.plugin->name != b.plugin->name || a.plugin->vst3Uid != b.plugin->vst3Uid
            || a.plugin->vst2UniqueId != b.plugin->vst2UniqueId || a.plugin->auSubType != b.plugin->auSubType)) continue;
        match(a.enabledTargetId, b.enabledTargetId, b.enabled ? 1 : 0);
        for (const auto& p : a.parameters) if (p.automationTargetId == id)
            for (const auto& q : b.parameters) if (p.path == q.path) match(id, q.automationTargetId, q.value);
        for (const auto& p : a.pluginParams) if (p.automationTargetId == id)
            for (const auto& q : b.pluginParams) if (p.parameterId >= 0 && p.parameterId == q.parameterId) match(id, q.automationTargetId, q.value);
    }
    return result;
}

bool authored(const AutomationEnvelope& envelope)
{
    return envelope.targetId >= 0 && std::any_of(envelope.points.begin(), envelope.points.end(), [](const auto& point) { return point.time >= 0; });
}

double regionValueAt(const AutomationEnvelope& envelope, double beat)
{
    const auto next = std::upper_bound(envelope.points.begin(), envelope.points.end(), beat,
        [](double time, const AutomationPoint& point) { return time < point.time; });
    if (next == envelope.points.begin()) return next == envelope.points.end() ? 0 : next->value;
    const auto& prev = *std::prev(next);
    if (next == envelope.points.end() || envelope.stepped) return prev.value;
    return prev.value + (next->value - prev.value) * (beat - prev.time) / (next->time - prev.time);
}

void replaceRegionPoints(AutomationEnvelope& target, double start, double end, const std::vector<AutomationPoint>& points)
{
    const double before = regionValueAt(target, std::nextafter(start, -INFINITY)), after = regionValueAt(target, end);
    std::erase_if(target.points, [&](const auto& point) { return point.time >= start && point.time <= end; });
    target.points.push_back({start, before});
    target.points.insert(target.points.end(), points.begin(), points.end());
    target.points.push_back({end, after});
    std::stable_sort(target.points.begin(), target.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
}

Clip regionSection(Clip clip, double start, double end, double tempo)
{
    const double from = std::max(start, clip.currentStart);
    clip.loop.startRelative += (from - clip.currentStart)
        * (clip.kind == Clip::Kind::Audio && ! clip.warped ? 60.0 / std::max(20.0, tempo) : 1.0);
    clip.currentStart = clip.time = from;
    clip.currentEnd = std::min(end, clip.currentEnd);
    return clip;
}

void eraseRegionClips(std::vector<Clip>& clips, const Song& song, int track, double start, double end)
{
    std::vector<Clip> remaining;
    for (const auto& clip : clips)
    {
        if (clip.currentEnd <= start || clip.currentStart >= end) { remaining.push_back(clip); continue; }
        const bool left = clip.currentStart < start;
        if (left) remaining.push_back(regionSection(clip, clip.currentStart, start, song.tempo));
        if (clip.currentEnd > end)
        {
            auto tail = regionSection(clip, end, clip.currentEnd, song.tempo);
            if (left) { markAsCopy(tail, song, track); tail.uid = allocateClipUid(); }
            remaining.push_back(std::move(tail));
        }
    }
    clips = std::move(remaining);
}
} // namespace

bool canTransferArrangementRegion(const Song& song, int first, int last, Beats start, Beats end, Beats at, int offset)
{
    if (! std::isfinite(start) || ! std::isfinite(end) || ! std::isfinite(at) || start < 0 || at < 0 || end <= start
        || first < 0 || last < first || last >= (int) song.tracks.size()
        || offset < -first || offset >= (int) song.tracks.size() - last || ! std::isfinite(at + end - start)) return false;
    for (int index = first; index <= last; ++index)
    {
        const auto& source = song.tracks[(size_t) index]; const auto& target = song.tracks[(size_t) (index + offset)];
        if (source.kind != target.kind) return false;
        if (offset == 0) continue;
        for (const auto& lane : source.automation)
            if (authored(lane) && ! regionTarget(source, target, lane.targetId)) return false;
        for (const auto& clip : source.arrangementClips)
            if (clip.currentStart < end && clip.currentEnd > start)
                for (const auto& lane : clip.envelopes)
                    if (lane.targetId >= 0 && ! regionTarget(source, target, lane.targetId)) return false;
    }
    return true;
}

EditCommand transferArrangementRegion(const Song& song, int first, int last, Beats start, Beats end, Beats at, int offset, bool copy)
{
    if (! canTransferArrangementRegion(song, first, last, start, end, at, offset)
        || (offset == 0 && std::abs(at - start) < 1e-9)) return {};
    struct State
    {
        std::vector<Clip> before, after;
        std::vector<AutomationEnvelope> beforeAutomation, afterAutomation;
    };
    const double delta = at - start, destinationEnd = at + end - start;
    auto states = std::make_shared<std::map<int, State>>();
    for (int index = first; index <= last; ++index)
        for (const int track : {index, index + offset})
            if (! states->contains(track))
            {
                const auto& original = song.tracks[(size_t) track];
                states->emplace(track, State{original.arrangementClips, original.arrangementClips, original.automation, original.automation});
            }
    // Clear all sources before any destinations. Overlapping moves read only the immutable originals.
    if (! copy) for (int index = first; index <= last; ++index)
    {
        auto& state = states->at(index);
        eraseRegionClips(state.after, song, index, start, end);
        for (auto& lane : state.afterAutomation) if (authored(lane))
        {
            const double held = regionValueAt(lane, start);
            replaceRegionPoints(lane, start, end, {{start, held}, {end, held}});
        }
    }
    for (int index = first; index <= last; ++index)
        eraseRegionClips(states->at(index + offset).after, song, index + offset, at, destinationEnd);
    for (int index = first; index <= last; ++index)
    {
        const auto& source = song.tracks[(size_t) index]; const auto& target = song.tracks[(size_t) (index + offset)];
        auto& destination = states->at(index + offset);
        for (const auto& clip : source.arrangementClips) if (clip.currentStart < end && clip.currentEnd > start)
        {
            auto moved = regionSection(clip, start, end, song.tempo);
            if (copy || moved.currentStart > clip.currentStart || moved.currentEnd < clip.currentEnd) moved.uid = allocateClipUid();
            const double position = moved.currentStart + delta;
            moved = movedClip(std::move(moved), position);
            if (copy || offset != 0 || moved.uid != clip.uid) markAsCopy(moved, song, index);
            if (offset != 0) for (auto& envelope : moved.envelopes)
                if (envelope.targetId >= 0) envelope.targetId = regionTarget(source, target, envelope.targetId)->id;
            destination.after.push_back(std::move(moved));
        }
        for (const auto& envelope : source.automation) if (authored(envelope))
        {
            const auto binding = *regionTarget(source, target, envelope.targetId);
            auto lane = std::find_if(destination.afterAutomation.begin(), destination.afterAutomation.end(),
                                    [&](const auto& candidate) { return candidate.targetId == binding.id; });
            if (lane == destination.afterAutomation.end())
            {
                auto added = envelope; added.targetId = binding.id; added.points = {{-63072000, binding.base}};
                destination.afterAutomation.push_back(std::move(added)); lane = std::prev(destination.afterAutomation.end());
            }
            std::vector<AutomationPoint> points{{at, regionValueAt(envelope, start)}};
            for (const auto& point : envelope.points) if (point.time > start && point.time < end) points.push_back({point.time + delta, point.value});
            points.push_back({destinationEnd, regionValueAt(envelope, std::nextafter(end, -INFINITY))});
            replaceRegionPoints(*lane, at, destinationEnd, points);
        }
    }
    EditCommand command; command.label = copy ? "Copy Region" : "Move Region";
    for (auto& [index, state] : *states)
    {
        command.tracks.push_back(index);
        std::stable_sort(state.after.begin(), state.after.end(), [](const auto& a, const auto& b) { return a.currentStart < b.currentStart; });
    }
    command.apply = [states](Song& s) {
        for (const auto& [index, state] : *states)
        { s.tracks[(size_t) index].arrangementClips = state.after; s.tracks[(size_t) index].automation = state.afterAutomation; }
    };
    command.revert = [states](Song& s) {
        for (const auto& [index, state] : *states)
        { s.tracks[(size_t) index].arrangementClips = state.before; s.tracks[(size_t) index].automation = state.beforeAutomation; }
    };
    return command;
}

EditCommand moveClipToTrack(const Song& song, int fromTrack, int uid, int toTrack, Beats at)
{
    std::vector<EditCommand> parts;
    parts.push_back(copyClipToTrack(song, fromTrack, uid, toTrack, at));
    parts.push_back(deleteClip(song, fromTrack, uid));
    return compose("Move Clip", std::move(parts));
}

bool makeEmptyMidiClip(const Song& song, Beats start, Beats length, Clip& out)
{
    for (const auto& track : song.tracks)
    {
        const Clip* source = nullptr;
        int sessionSlot = -1;
        for (const auto& clip : track.arrangementClips)
            if (clip.kind == Clip::Kind::Midi && clip.sourceXml) { source = &clip; break; }
        if (! source)
            for (size_t i = 0; i < track.slots.size(); ++i)
                if (track.slots[i].clip && track.slots[i].clip->kind == Clip::Kind::Midi && track.slots[i].clip->sourceXml)
                { source = &*track.slots[i].clip; sessionSlot = (int) i; break; }
        if (! source) continue;
        out = Clip{};
        out.sourceXml = source->sourceXml;
        out.resetSourceMarkers = true;
        out.kind = Clip::Kind::Midi;
        out.colorIndex = track.colorIndex;
        out.currentStart = out.time = start;
        out.currentEnd = start + length;
        out.loop = {0.0, length, 0.0, true};
        out.cloneOfXmlId = source->xmlId;
        out.cloneOfTrackId = track.id;
        out.cloneOfSessionSlot = sessionSlot;
        out.scaleRoot = song.scaleRoot; out.scaleName = song.scaleName; out.inKey = song.inKey; // new clips take the set scale
        return true;
    }
    if (song.midiClipTemplate)
    {
        out = Clip{};
        out.sourceXml = song.midiClipTemplate;
        out.resetSourceMarkers = true;
        out.kind = Clip::Kind::Midi;
        out.colorIndex = song.tracks.empty() ? 0 : song.tracks.front().colorIndex;
        out.currentStart = out.time = start;
        out.currentEnd = start + length;
        out.loop = {0.0, length, 0.0, true};
        out.scaleRoot = song.scaleRoot; out.scaleName = song.scaleName; out.inKey = song.inKey;
        return true;
    }
    return false;
}

EditCommand compose(std::string label, std::vector<EditCommand> parts)
{
    EditCommand c;
    c.label = std::move(label);
    c.takeLanesOnly = ! parts.empty();
    for (const auto& p : parts)
    {
        for (int t : p.gainTracks)
            if (std::find(c.gainTracks.begin(), c.gainTracks.end(), t) == c.gainTracks.end()) c.gainTracks.push_back(t);
        c.takeLanesOnly &= p.takeLanesOnly;
        for (int t : p.tracks)
            if (std::find(c.tracks.begin(), c.tracks.end(), t) == c.tracks.end())
                c.tracks.push_back(t);
        c.songLevel |= p.songLevel;
        c.devicesChanged |= p.devicesChanged;
        c.inputChanged |= p.inputChanged;
        c.inputSettingsChanged |= p.inputSettingsChanged;
        c.recordingRangeChanged |= p.recordingRangeChanged;
        c.structureChanged |= p.structureChanged;
    }
    auto shared = std::make_shared<std::vector<EditCommand>>(std::move(parts));
    c.apply = [shared](Song& s) {
        for (auto& p : *shared)
            p.apply(s);
    };
    c.revert = [shared](Song& s) {
        for (auto it = shared->rbegin(); it != shared->rend(); ++it)
            it->revert(s);
    };
    return c;
}

EditCommand setNotes(const Song& song, int track, int uid, std::vector<MidiNote> notes, std::string label, int scene)
{
    return setNotes(song, scene < 0 ? ClipLocation::arrangement(track, uid) : ClipLocation::session(track, uid, scene),
                    std::move(notes), std::move(label));
}

EditCommand setNotes(const Song& song, ClipLocation location, std::vector<MidiNote> notes, std::string label)
{
    const Clip* current = findClip(song, location);
    Clip after = current != nullptr ? *current : Clip{};
    std::sort(notes.begin(), notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.start < b.start || (! (b.start < a.start) && a.pitch < b.pitch);
    });
    after.notes = std::move(notes);
    return modifyClip(song, location, std::move(after), std::move(label));
}

EditCommand insertDevice(int track, Device device, int position)
{
    EditCommand command;
    command.label = "Insert " + device.displayName;
    command.tracks = {track};
    command.devicesChanged = true;
    const int id = device.id;
    const auto stored = std::make_shared<Device>(std::move(device));
    command.apply = [track, stored, position](Song& song) {
        if (auto* target = trackAt(song, track))
        {
            const size_t at = position < 0 ? target->devices.size() : std::min((size_t) position, target->devices.size());
            target->devices.insert(target->devices.begin() + (std::ptrdiff_t) at, *stored);
        }
    };
    command.revert = [track, id, stored](Song& song) {
        if (auto* target = trackAt(song, track))
        {
            for (const auto& existing : target->devices) if (existing.id == id) *stored = existing;
            std::erase_if(target->devices, [id](const Device& existing) { return existing.id == id; });
        }
    };
    return command;
}

EditCommand removeDevice(const Song& song, int track, int deviceId)
{
    EditCommand command;
    const auto* target = trackAt(song, track);
    if (! target) return command;
    const auto found = std::find_if(target->devices.begin(), target->devices.end(), [deviceId](const Device& d) { return d.id == deviceId; });
    if (found == target->devices.end()) return command;
    command.label = "Delete " + found->displayName;
    command.tracks = {track};
    command.devicesChanged = true;
    // The removed device is captured when the command runs (after the engine state was copied into the model), so undo
    // restores plugin state edited since the set was loaded.
    struct Removed { Device device; size_t index = 0; };
    const auto removed = std::make_shared<Removed>();
    command.apply = [track, deviceId, removed](Song& s) {
        auto* t = trackAt(s, track);
        if (! t) return;
        const auto it = std::find_if(t->devices.begin(), t->devices.end(), [deviceId](const Device& d) { return d.id == deviceId; });
        if (it == t->devices.end()) return;
        removed->device = *it;
        removed->index = (size_t) (it - t->devices.begin());
        t->devices.erase(it);
    };
    command.revert = [track, removed](Song& s) {
        if (auto* t = trackAt(s, track))
            t->devices.insert(t->devices.begin() + (std::ptrdiff_t) std::min(removed->index, t->devices.size()), removed->device);
    };
    return command;
}

EditCommand insertTrackDevice(const Song& song, int track, Device device)
{
    const auto* target = trackAt(song, track);
    if (! target) return {};
    const auto role = deviceRole(device);
    if ((role == DeviceRole::Instrument || role == DeviceRole::MidiEffect) && target->kind != TrackKind::Midi) return {};
    if (role == DeviceRole::MidiEffect) return insertDevice(track, std::move(device), 0);
    if (role != DeviceRole::Instrument) return insertDevice(track, std::move(device));

    int position = 0;
    for (const auto& existing : target->devices)
    {
        if (deviceRole(existing) != DeviceRole::MidiEffect) break;
        ++position;
    }
    std::vector<EditCommand> parts;
    for (const auto& existing : target->devices)
        if (deviceRole(existing) == DeviceRole::Instrument) parts.push_back(removeDevice(song, track, existing.id));
    const bool replacing = ! parts.empty();
    parts.push_back(insertDevice(track, std::move(device), position));
    return replacing ? compose("Replace Instrument", std::move(parts)) : std::move(parts.front());
}

bool canMoveAudioEffect(const Song& song, int track, int deviceId, int destination)
{
    const auto* target = trackAt(song, track);
    if (! target || destination < 0 || (size_t) destination >= target->devices.size()) return false;
    const auto found = std::find_if(target->devices.begin(), target->devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
    if (found == target->devices.end() || deviceRole(*found) != DeviceRole::AudioEffect) return false;
    const int source = (int) (found - target->devices.begin());
    if (source == destination) return false;
    if (target->kind == TrackKind::Midi)
        for (int i = 0; i < (int) target->devices.size(); ++i)
        {
            const auto role = deviceRole(target->devices[(size_t) i]);
            if (role == DeviceRole::Instrument || role == DeviceRole::MidiEffect)
            {
                const int afterRemoval = i - (source < i ? 1 : 0);
                if (destination <= afterRemoval) return false;
            }
        }
    return true;
}

EditCommand moveAudioEffect(const Song& song, int track, int deviceId, int destination)
{
    if (! canMoveAudioEffect(song, track, deviceId, destination)) return {};
    const auto* target = trackAt(song, track);
    const auto found = std::find_if(target->devices.begin(), target->devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
    const int source = (int) (found - target->devices.begin());
    auto move = [track, deviceId](Song& s, int to) {
        auto* t = trackAt(s, track);
        if (! t || to < 0 || (size_t) to >= t->devices.size()) return;
        const auto it = std::find_if(t->devices.begin(), t->devices.end(), [deviceId](const auto& d) { return d.id == deviceId; });
        if (it == t->devices.end()) return;
        const auto dest = t->devices.begin() + to;
        if (it < dest) std::rotate(it, it + 1, dest + 1);
        else if (dest < it) std::rotate(dest, it, it + 1);
    };
    EditCommand command;
    command.label = "Move " + found->displayName;
    command.tracks = {track}; command.devicesChanged = true;
    command.apply = [move, destination](Song& s) { move(s, destination); };
    command.revert = [move, source](Song& s) { move(s, source); };
    return command;
}

void allocateDeviceIds(Song& song, Device& device)
{
    if (device.id < 0) device.id = song.nextDeviceId++;
    if (device.enabledTargetId < 0) device.enabledTargetId = song.nextPointeeId++;
    for (auto& p : device.parameters)
        if (p.automationTargetId < 0) p.automationTargetId = song.nextPointeeId++;
    for (auto& chain : device.chains)
        for (auto& child : chain.devices) allocateDeviceIds(song, child);
    song.pointeeIdsAllocated = true;
}

EditCommand replaceDevice(const Song& song, int track, int device, Device replacement, std::string label)
{
    EditCommand command;
    const auto* target = trackAt(song, track);
    if (! target || device < 0 || (size_t) device >= target->devices.size()) return command;
    command.label = std::move(label); command.tracks = {track}; command.devicesChanged = true;
    const auto before = std::make_shared<Device>(target->devices[(size_t) device]);
    const auto after = std::make_shared<Device>(std::move(replacement));
    command.apply = [track, device, after](Song& s) {
        if (auto* t = trackAt(s, track); t && (size_t) device < t->devices.size()) t->devices[(size_t) device] = *after;
    };
    command.revert = [track, device, before, after](Song& s) {
        if (auto* t = trackAt(s, track); t && (size_t) device < t->devices.size())
        { *after = t->devices[(size_t) device]; t->devices[(size_t) device] = *before; }
    };
    return command;
}

EditCommand setTrackInput(const Song& song, int track, std::string target, std::string upper, std::string lower, int monitoring)
{
    EditCommand command;
    command.label = "Set Track Input";
    command.inputSettingsChanged = true;
    command.tracks = {track};
    struct InputState
    {
        std::string inputRouting, inputUpperDisplay, inputLowerDisplay;
        int monitoring;
        bool inputRoutingEdited, monitoringEdited;
    };
    const auto* current = trackAt(song, track);
    const InputState before{current->inputRouting, current->inputUpperDisplay, current->inputLowerDisplay,
                            current->monitoring, current->inputRoutingEdited, current->monitoringEdited};
    InputState after = before;
    after.inputRouting = std::move(target);
    after.inputUpperDisplay = std::move(upper);
    after.inputLowerDisplay = std::move(lower);
    after.inputRoutingEdited |= after.inputRouting != before.inputRouting;
    command.inputChanged = after.inputRouting != before.inputRouting;
    after.monitoring = std::clamp(monitoring, 0, 2);
    after.monitoringEdited |= after.monitoring != before.monitoring;
    auto replace = [track](Song& destination, const InputState& input) {
        if (auto* selected = trackAt(destination, track))
        {
            selected->inputRouting = input.inputRouting;
            selected->inputUpperDisplay = input.inputUpperDisplay;
            selected->inputLowerDisplay = input.inputLowerDisplay;
            selected->monitoring = input.monitoring;
            selected->inputRoutingEdited = input.inputRoutingEdited;
            selected->monitoringEdited = input.monitoringEdited;
        }
    };
    command.apply = [replace, after](Song& destination) { replace(destination, after); };
    command.revert = [replace, before](Song& destination) { replace(destination, before); };
    return command;
}

EditCommand recordArrangementTake(const Song& song, int track, Clip clip)
{
    std::vector<EditCommand> parts;
    // Recording replaces only the covered range. Preserve both sides of a punched clip, with
    // its content offset intact, so playback cannot double the old material under the take.
    for (const auto& existing : song.tracks[(size_t) track].arrangementClips)
    {
        constexpr double boundaryTolerance = 1e-9; // floating-point beat accumulation, far below one audio sample
        if (existing.currentEnd <= clip.currentStart + boundaryTolerance || existing.currentStart >= clip.currentEnd - boundaryTolerance) continue;
        const bool left = existing.currentStart < clip.currentStart - boundaryTolerance, right = existing.currentEnd > clip.currentEnd + boundaryTolerance;
        if (left)
        {
            auto head = existing;
            head.currentEnd = clip.currentStart; // recording can leave fragments smaller than the editor grid
            parts.push_back(modifyClip(song, track, existing.uid, std::move(head), "Trim Before Take"));
        }
        if (right)
        {
            auto tail = existing;
            const Beats offset = clip.currentEnd - existing.currentStart;
            tail.loop.startRelative += existing.kind == Clip::Kind::Audio && ! existing.warped
                                          ? offset * 60.0 / std::max(20.0, song.tempo) : offset;
            tail.currentStart = tail.time = clip.currentEnd;
            if (left)
            {
                markAsCopy(tail, song, track);
                parts.push_back(insertClip(track, std::move(tail), "Keep After Take"));
            }
            else
                parts.push_back(modifyClip(song, track, existing.uid, std::move(tail), "Trim After Take"));
        }
        if (! left && ! right) parts.push_back(deleteClip(song, track, existing.uid));
    }
    parts.push_back(insertClip(track, std::move(clip), "Record Take"));
    return compose("Record Take", std::move(parts));
}

EditCommand recordArrangementOverdub(const Song& song, int track, Clip take)
{
    constexpr double tol = 1e-9;
    const auto& clips = song.tracks[(size_t) track].arrangementClips;
    const Clip* target = nullptr;
    if (take.kind == Clip::Kind::Midi && take.envelopes.empty())
        for (const auto& existing : clips)
            if (existing.kind == Clip::Kind::Midi && ! existing.disabled && take.currentStart >= existing.currentStart - tol
                && take.currentEnd <= existing.currentEnd + tol)
            {
                target = &existing;
                break;
            }
    if (target == nullptr)
        return recordArrangementTake(song, track, std::move(take));

    Clip merged = *target;
    const double loopStart = merged.loop.loopStart, loopEnd = merged.loop.loopEnd, loopLength = loopEnd - loopStart;
    const double contentStart = loopStart + merged.loop.startRelative; // clip time shown at the clip's left edge
    for (auto note : take.notes)
    {
        double t = contentStart + (take.currentStart + note.start - target->currentStart);
        if (merged.loop.enabled && loopLength > 1e-9 && t >= loopEnd)
            t = loopStart + std::fmod(t - loopStart, loopLength); // a take over several passes lands in the loop
        else if (t >= loopEnd + tol)
            continue;                                              // beyond the clip's content: it would never play
        note.start = t;
        note.noteId = -1;
        merged.notes.push_back(note);
    }
    std::sort(merged.notes.begin(), merged.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.start < b.start || (! (b.start < a.start) && a.pitch < b.pitch);
    });
    return modifyClip(song, track, target->uid, std::move(merged), "Overdub MIDI Clip");
}

EditCommand appendTakeLanes(const Song& song, int track, std::vector<TakeLane> lanes)
{
    EditCommand command;
    command.label = "Record Take Lanes";
    command.takeLanesOnly = true;
    command.tracks = {track};
    const auto before = song.tracks[(size_t) track].takeLanes;
    for (auto& lane : lanes) lane.id = -1; // appending a copied lane always allocates a new container identity
    auto added = std::make_shared<std::vector<TakeLane>>(std::move(lanes));
    command.apply = [track, added](Song& s) {
        auto& target = s.tracks[(size_t) track];
        for (auto& lane : *added)
        {
            if (lane.id < 0)
            {
                lane.id = target.nextTakeLaneId++;
                for (auto& clip : lane.clips) clip.uid = allocateClipUid();
            }
            target.nextTakeLaneId = std::max(target.nextTakeLaneId, lane.id + 1);
            target.takeLanes.push_back(lane);
        }
        target.takeLanesNeedPatch = true;
    };
    command.revert = [track, before](Song& s) { auto& target = s.tracks[(size_t) track]; target.takeLanes = before; target.takeLanesNeedPatch = true; };
    return command;
}

EditCommand recordArrangementPasses(const Song& song, int track, std::vector<Clip> passes)
{
    EditCommand command;
    command.label = "Record Takes"; command.tracks = {track};
    const auto before = song.tracks[(size_t) track].arrangementClips;
    const auto beforeLanes = song.tracks[(size_t) track].takeLanes;
    struct Prepared { bool ready = false; std::vector<Clip> comp; std::vector<TakeLane> lanes; };
    auto prepared = std::make_shared<Prepared>();
    const double tempo = song.tempo;
    command.apply = [track, tempo, recorded = std::move(passes), prepared](Song& s) mutable {
        auto& target = s.tracks[(size_t) track];
        if (! prepared->ready)
        {
            Song staging; staging.tempo = tempo; staging.tracks.resize(1);
            staging.tracks[0].id = target.id;
            staging.tracks[0].arrangementClips = target.arrangementClips;
            staging.tracks[0].takeLanes = target.takeLanes;
            staging.tracks[0].nextTakeLaneId = target.nextTakeLaneId;
            for (auto& pass : recorded)
            {
                pass.takeId = target.nextTakeId++;
                recordArrangementTake(staging, 0, pass).apply(staging);
                TakeLane lane; lane.name = "Take " + std::to_string(pass.takeId); lane.clips = {pass};
                appendTakeLanes(staging, 0, {std::move(lane)}).apply(staging);
            }
            target.nextTakeLaneId = staging.tracks[0].nextTakeLaneId;
            ++target.recorderTakeCounter; target.takeCounterAdvanced = true;
            prepared->comp = std::move(staging.tracks[0].arrangementClips);
            prepared->lanes = std::move(staging.tracks[0].takeLanes);
            prepared->ready = true;
        }
        target.arrangementClips = prepared->comp;
        target.takeLanes = prepared->lanes;
        target.takeLanesNeedPatch = true;
    };
    command.revert = [track, before, beforeLanes](Song& s) {
        auto& target = s.tracks[(size_t) track]; target.arrangementClips = before; target.takeLanes = beforeLanes;
        target.takeLanesNeedPatch = true;
    };
    return command;
}

namespace
{
EditCommand replaceTakeLanes(const Song& song, int track, std::vector<TakeLane> after, std::string label)
{
    EditCommand command; command.label = std::move(label); command.tracks = {track}; command.takeLanesOnly = true;
    const auto before = song.tracks[(size_t) track].takeLanes;
    auto replace = [track](Song& destination, const std::vector<TakeLane>& replacementLanes) {
        destination.tracks[(size_t) track].takeLanes = replacementLanes;
        destination.tracks[(size_t) track].takeLanesNeedPatch = true;
    };
    command.apply = [replace, replacement = std::move(after)](Song& s) { replace(s, replacement); };
    command.revert = [replace, before](Song& s) { replace(s, before); };
    return command;
}
}

EditCommand renameTakeLane(const Song& song, int track, int laneId, std::string name)
{
    auto lanes = song.tracks[(size_t) track].takeLanes;
    for (auto& lane : lanes) if (lane.id == laneId) lane.name = std::move(name);
    return replaceTakeLanes(song, track, std::move(lanes), "Rename Take Lane");
}

EditCommand moveTakeLane(const Song& song, int track, int laneId, int delta)
{
    auto lanes = song.tracks[(size_t) track].takeLanes;
    for (size_t i = 0; i < lanes.size(); ++i) if (lanes[i].id == laneId)
    {
        const int target = std::clamp((int) i + delta, 0, (int) lanes.size() - 1);
        auto lane = std::move(lanes[i]); lanes.erase(lanes.begin() + (int) i);
        lanes.insert(lanes.begin() + target, std::move(lane)); break;
    }
    return replaceTakeLanes(song, track, std::move(lanes), "Move Take Lane");
}

EditCommand deleteTakeLane(const Song& song, int track, int laneId)
{
    auto lanes = song.tracks[(size_t) track].takeLanes;
    std::erase_if(lanes, [laneId](const TakeLane& lane) { return lane.id == laneId; });
    return replaceTakeLanes(song, track, std::move(lanes), "Delete Take Lane");
}

EditCommand duplicateTakeLane(const Song& song, int track, int laneId)
{
    auto lane = *findTakeLane(song, track, laneId);
    lane.name += " Copy"; lane.audition = false;
    // A duplicate is an independent comp source, so it receives new TakeIds as well as UIDs.
    auto command = appendTakeLanes(song, track, {std::move(lane)});
    auto apply = command.apply;
    auto ids = std::make_shared<std::vector<int>>();
    command.apply = [track, apply, ids](Song& s) {
        apply(s); auto& target = s.tracks[(size_t) track]; auto& clips = target.takeLanes.back().clips;
        if (ids->empty()) for (size_t i = 0; i < clips.size(); ++i) ids->push_back(target.nextTakeId++);
        for (size_t i = 0; i < clips.size(); ++i) clips[i].takeId = (*ids)[i];
    };
    command.label = "Duplicate Take Lane";
    return command;
}

EditCommand auditionTakeLane(const Song& song, int track, int laneId)
{
    auto lanes = song.tracks[(size_t) track].takeLanes;
    for (auto& lane : lanes) lane.audition = lane.id == laneId;
    return replaceTakeLanes(song, track, std::move(lanes), "Audition Take Lane");
}

EditCommand deleteClip(const Song& song, ClipLocation location)
{
    if (location.kind == ClipLocation::Kind::Arrangement) return deleteClip(song, location.track, location.uid);
    if (location.kind == ClipLocation::Kind::Take)
    {
        auto lanes = song.tracks[(size_t) location.track].takeLanes;
        for (auto& lane : lanes) if (lane.id == location.laneId)
            std::erase_if(lane.clips, [location](const Clip& clip) { return clip.uid == location.uid; });
        return replaceTakeLanes(song, location.track, std::move(lanes), "Delete Clip");
    }
    const auto before = song.tracks[(size_t) location.track].slots[(size_t) location.scene].clip;
    EditCommand command; command.label = "Delete Clip"; command.tracks = {location.track};
    command.apply = [location](Song& s) { auto& clip = s.tracks[(size_t) location.track].slots[(size_t) location.scene].clip;
        if (clip && clip->uid == location.uid) clip.reset(); };
    command.revert = [location, before](Song& s) { s.tracks[(size_t) location.track].slots[(size_t) location.scene].clip = before; };
    return command;
}

EditCommand duplicateClip(const Song& song, ClipLocation location)
{
    if (location.kind == ClipLocation::Kind::Arrangement) return duplicateClip(song, location.track, location.uid);
    if (location.kind == ClipLocation::Kind::Session)
    {
        const auto& slots = song.tracks[(size_t) location.track].slots;
        int scene = location.scene + 1;
        while ((size_t) scene < slots.size() && slots[(size_t) scene].clip) ++scene;
        auto command = setSessionClip(song, location.track, scene, *findClip(song, location));
        command.label = "Duplicate Clip"; return command;
    }
    auto lanes = song.tracks[(size_t) location.track].takeLanes;
    auto copy = movedClip(*findClip(song, location), findClip(song, location)->currentEnd);
    markAsCopy(copy, song, location.track); copy.uid = allocateClipUid();
    for (auto& lane : lanes) if (lane.id == location.laneId) lane.clips.push_back(std::move(copy));
    return replaceTakeLanes(song, location.track, std::move(lanes), "Duplicate Clip");
}

EditCommand compTakeRange(const Song& song, int track, int laneId, Beats start, Beats end)
{
    // Stage all intersections, so overlapping source clips and existing main fragments are replaced
    // chronologically in one command. Source seconds/beats and TakeId remain tied to the lane source.
    Song staging; staging.tempo = song.tempo; staging.tracks.resize(1);
    staging.tracks[0].id = song.tracks[(size_t) track].id;
    staging.tracks[0].arrangementClips = song.tracks[(size_t) track].arrangementClips;
    if (const auto* lane = findTakeLane(song, track, laneId)) for (auto clip : lane->clips)
    {
        const double left = std::max(start, clip.currentStart), right = std::min(end, clip.currentEnd);
        if (clip.disabled || right <= left + 1e-9) continue;
        const double offset = left - clip.currentStart;
        clip.loop.startRelative += clip.kind == Clip::Kind::Audio && ! clip.warped ? offset * 60.0 / song.tempo : offset;
        clip.currentStart = clip.time = left; clip.currentEnd = right;
        markAsCopy(clip, song, track);
        recordArrangementTake(staging, 0, std::move(clip)).apply(staging);
    }
    EditCommand command; command.label = "Comp Take Range"; command.tracks = {track};
    const auto before = song.tracks[(size_t) track].arrangementClips;
    const auto after = staging.tracks[0].arrangementClips;
    command.apply = [track, after](Song& s) { s.tracks[(size_t) track].arrangementClips = after; };
    command.revert = [track, before](Song& s) { s.tracks[(size_t) track].arrangementClips = before; };
    return command;
}

EditCommand setSessionClip(const Song& song, int track, int scene, Clip clip)
{
    clip.stripSourceEnvelopes |= clip.envelopes.empty();
    assignNoteKeys(clip.notes);
    EditCommand c;
    c.label = "Record Session Clip";
    c.tracks = {track};
    const auto beforeSlots = song.tracks[(size_t) track].slots;
    const auto beforeScenes = song.scenes;
    std::vector<size_t> slotCounts;
    for (const auto& t : song.tracks) slotCounts.push_back(t.slots.size());
    auto uid = std::make_shared<int>(-1);
    c.apply = [track, scene, clip, uid](Song& s) mutable {
        while ((int) s.scenes.size() <= scene)
            s.scenes.push_back({"Scene " + std::to_string(s.scenes.size() + 1), -1, {}});
        for (auto& t : s.tracks)
            if ((int) t.slots.size() <= scene) t.slots.resize((size_t) scene + 1);
        auto& slots = s.tracks[(size_t) track].slots;
        if (*uid < 0) *uid = allocateClipUid();
        Clip copy = clip;
        copy.uid = *uid;
        copy.currentStart = copy.time = 0;
        copy.currentEnd = clip.currentEnd - clip.currentStart;
        slots[(size_t) scene].clip = std::move(copy);
    };
    c.revert = [track, beforeSlots, beforeScenes, slotCounts](Song& s) {
        s.tracks[(size_t) track].slots = beforeSlots;
        for (size_t i = 0; i < s.tracks.size(); ++i)
            if ((int) i != track) s.tracks[i].slots.resize(slotCounts[i]);
        s.scenes = beforeScenes;
    };
    return c;
}

EditCommand setAutomation(const Song& song, int track, AutomationEnvelope envelope)
{
    EditCommand c;
    c.label = "Edit Automation";
    c.tracks = {track};
    const Track* t = trackAt(song, track);
    const auto before = t != nullptr ? t->automation : std::vector<AutomationEnvelope>{};
    auto after = before;
    std::stable_sort(envelope.points.begin(), envelope.points.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
    const auto it = std::find_if(after.begin(), after.end(), [&](const AutomationEnvelope& e) { return e.targetId == envelope.targetId; });
    if (it == after.end())
        after.push_back(std::move(envelope));
    else
        *it = std::move(envelope);
    c.apply = [track, after](Song& s) { if (auto* dst = trackAt(s, track)) dst->automation = after; };
    c.revert = [track, before](Song& s) { if (auto* dst = trackAt(s, track)) dst->automation = before; };
    return c;
}

EditCommand setTrackColor(const Song& song, const std::vector<int>& tracks, int color)
{
    EditCommand command;
    if (color < 0 || color >= 70) return command;
    struct Entry { int index; TrackId id; int before; };
    std::vector<Entry> entries;
    for (int index : tracks)
        if (const auto* track = trackAt(song, index); track && track->kind != TrackKind::Main)
            entries.push_back({index, track->id, track->colorIndex});
    if (entries.empty()) return command;
    command.label = "Change Track Color";
    command.apply = [entries, color](Song& s) {
        for (const auto& entry : entries)
            if (auto* track = trackAt(s, entry.index); track && track->id == entry.id) track->colorIndex = color;
    };
    command.revert = [entries](Song& s) {
        for (const auto& entry : entries)
            if (auto* track = trackAt(s, entry.index); track && track->id == entry.id) track->colorIndex = entry.before;
    };
    return command;
}

EditCommand renameTrack(const Song& song, int track, std::string name)
{
    EditCommand command;
    const auto* original = trackAt(song, track);
    if (! original || original->kind == TrackKind::Main) return command;
    const auto before = original->name;
    const bool automaticBefore = original->nameIsAutomatic;
    const auto id = original->id;
    command.label = "Rename Track";
    command.apply = [track, id, name](Song& s) {
        if (auto* t = trackAt(s, track); t && t->id == id) { t->name = name; t->nameIsAutomatic = name.empty(); }
    };
    command.revert = [track, id, before, automaticBefore](Song& s) {
        if (auto* t = trackAt(s, track); t && t->id == id) { t->name = before; t->nameIsAutomatic = automaticBefore; }
    };
    return command;
}

EditCommand addLocator(const Song& song, Beats time, std::string name)
{
    EditCommand command;
    command.label = "Add Locator";
    const auto before = song.locators;
    command.apply = [time, name](Song& s) { s.locators.push_back({std::max(0.0, time), name}); };
    command.revert = [before](Song& s) { s.locators = before; };
    return command;
}

EditCommand setGlobalQuantisation(const Song& song, int value)
{
    EditCommand command;
    command.label = "Change Launch Quantization";
    const int before = song.globalQuantisation;
    command.apply = [quantisation = std::clamp(value, 0, 13)](Song& s) { s.globalQuantisation = quantisation; };
    command.revert = [before](Song& s) { s.globalQuantisation = before; };
    return command;
}

EditCommand setScale(const Song& song, int root, int name)
{
    root = ((root % 12) + 12) % 12;
    if (root == song.scaleRoot && name == song.scaleName) return {};
    EditCommand command;
    command.label = "Change Scale";
    const int rootBefore = song.scaleRoot, nameBefore = song.scaleName;
    command.apply = [root, name](Song& s) { s.scaleRoot = root; s.scaleName = name; };
    command.revert = [rootBefore, nameBefore](Song& s) { s.scaleRoot = rootBefore; s.scaleName = nameBefore; };
    return command;
}

EditCommand setTimeSignature(const Song& song, int numerator, int denominator)
{
    if (numerator < 1 || numerator > 99 || (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16)
        || (numerator == song.timeSigNumerator && denominator == song.timeSigDenominator)) return {};
    EditCommand command; command.label = "Change Time Signature"; command.songLevel = true;
    const auto automationBefore = song.main.automation;
    command.apply = [numerator, denominator](Song& s) {
        s.timeSigNumerator = numerator; s.timeSigDenominator = denominator;
        for (auto& envelope : s.main.automation)
            if (envelope.targetId >= 0 && envelope.targetId == s.main.mixer.timeSignatureTargetId && envelope.points.size() == 1 && envelope.points[0].time < 0)
                envelope.points[0].value = encodeTimeSignature(numerator, denominator);
    };
    command.revert = [oldNumerator = song.timeSigNumerator, oldDenominator = song.timeSigDenominator, automationBefore](Song& s) {
        s.timeSigNumerator = oldNumerator; s.timeSigDenominator = oldDenominator;
        s.main.automation = automationBefore;
    };
    return command;
}

EditCommand setTempo(const Song& song, double bpm)
{
    EditCommand c;
    c.label = "Set Tempo";
    c.songLevel = true;
    const double old = song.tempo;
    // Live also stores the tempo as a main-track envelope with one pre-roll point; keep that point in step
    // so a saved set reopens at the new tempo. Real tempo automation (more points) is left alone.
    auto setStatic = [](Song& s, double value) {
        for (auto& env : s.main.automation)
            if (env.targetId >= 0 && env.targetId == s.main.mixer.tempoTargetId && env.points.size() == 1 && env.points[0].time < 0)
                env.points[0].value = value;
    };
    c.apply = [bpm, setStatic](Song& s) { s.tempo = bpm; setStatic(s, bpm); };
    c.revert = [old, setStatic](Song& s) { s.tempo = old; setStatic(s, old); };
    return c;
}

EditCommand setLoop(const Song& song, bool on, Beats start, Beats length)
{
    EditCommand c;
    c.label = "Set Loop";
    c.recordingRangeChanged = true;
    c.songLevel = true;
    const bool oldOn = song.loopOn;
    const Beats oldStart = song.loopStart, oldLength = song.loopLength;
    c.apply = [on, start, length](Song& s) {
        s.loopOn = on;
        s.loopStart = start;
        s.loopLength = length;
    };
    c.revert = [oldOn, oldStart, oldLength](Song& s) {
        s.loopOn = oldOn;
        s.loopStart = oldStart;
        s.loopLength = oldLength;
    };
    return c;
}

EditCommand setPunch(const Song& song, bool in, bool out)
{
    EditCommand c;
    c.label = "Set Punch";
    c.recordingRangeChanged = true;
    c.songLevel = true;
    const bool oldIn = song.punchIn, oldOut = song.punchOut, oldEdited = song.punchEdited;
    c.apply = [in, out](Song& s) { s.punchIn = in; s.punchOut = out; s.punchEdited = true; };
    c.revert = [oldIn, oldOut, oldEdited](Song& s) { s.punchIn = oldIn; s.punchOut = oldOut; s.punchEdited = oldEdited; };
    return c;
}

Clip movedClip(Clip c, Beats newStart)
{
    const Beats length = c.currentEnd - c.currentStart;
    c.currentStart = std::max(0.0, newStart);
    c.currentEnd = c.currentStart + length;
    c.time = c.currentStart;
    return c;
}

Clip withEnd(Clip c, Beats newEnd)
{
    c.currentEnd = std::max(c.currentStart + 0.0625, newEnd);
    return c;
}

Clip withStart(Clip c, Beats newStart, double tempo)
{
    newStart = std::clamp(newStart, 0.0, c.currentEnd - 0.0625);
    const Beats delta = newStart - c.currentStart;
    // Content that was at the old start now plays `delta` later: advance the content start accordingly.
    const bool unwarpedAudio = c.kind == Clip::Kind::Audio && ! c.warped;
    c.loop.startRelative += unwarpedAudio ? delta * 60.0 / std::max(20.0, tempo) : delta;
    c.currentStart = newStart;
    c.time = newStart;
    return c;
}

EditCommand assignRecordedMidiTargets(Song& song, int track, const std::vector<Clip*>& clips)
{
    auto* t = trackAt(song, track);
    if (! t || t->kind != TrackKind::Midi) return {};
    const auto before = t->midiControllerTargets;
    auto after = before;
    bool required = false;
    for (const auto* clip : clips) if (clip) for (const auto& env : clip->envelopes)
        if (isMidiController(env.midiController)) required = true;
    if (! required) return {};
    auto allocate = [&](int index) {
        if (after[(size_t) index] >= 0) return;
        after[(size_t) index] = song.nextPointeeId++;
        song.pointeeIdsAllocated = true;
    };
    if (! t->midiControllersPresent)
        for (int index = 0; index < midiControllerTargetCount; ++index) allocate(index);
    for (auto* clip : clips) if (clip) for (auto& env : clip->envelopes)
        if (isMidiController(env.midiController))
        {
            allocate(env.midiController);
            env.targetId = after[(size_t) env.midiController];
        }
    if (before == after) return {};
    EditCommand cmd;
    cmd.label = "Record MIDI Controllers"; cmd.tracks = {track};
    cmd.apply = [track, after](Song& s) { if (auto* x = trackAt(s, track)) { x->midiControllerTargets = after; x->midiControllersEdited = true; } };
    cmd.revert = [track, before](Song& s) { if (auto* x = trackAt(s, track)) { x->midiControllerTargets = before; x->midiControllersEdited = true; } };
    return cmd;
}

EditCommand setClipMidiEnvelope(const Song& song, ClipLocation location, AutomationEnvelope envelope)
{
    const auto* source = findClip(song, location);
    const auto* track = trackAt(song, location.track);
    if (! source || source->kind != Clip::Kind::Midi || ! track || track->kind != TrackKind::Midi
        || ! isMidiController(envelope.midiController)) return {};
    auto after = *source;
    auto it = std::find_if(after.envelopes.begin(), after.envelopes.end(), [&](const auto& e) {
        return e.midiController == envelope.midiController;
    });
    envelope.eventType = "FloatEvent"; envelope.stepped = false;
    std::erase_if(envelope.points, [](const auto& p) { return ! std::isfinite(p.time) || ! std::isfinite(p.value); });
    for (auto& p : envelope.points)
        p.value = std::clamp(p.value, envelope.midiController == 0 ? -8192.0 : 0.0, envelope.midiController == 0 ? 8191.0 : 127.0);
    std::stable_sort(envelope.points.begin(), envelope.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    if (envelope.points.empty())
    {
        if (it == after.envelopes.end()) return {};
        after.envelopes.erase(it);
    }
    else
    {
        if (envelope.points.front().time >= 0)
            envelope.points.insert(envelope.points.begin(), {-63072000, (double) midiControllerDefault(envelope.midiController)});
        if (it == after.envelopes.end())
        {
            const auto first = std::find_if(envelope.points.begin(), envelope.points.end(), [](const auto& p) { return p.time >= 0; });
            if (first != envelope.points.end() && first->time > 0)
                envelope.points.insert(first, {0, (double) midiControllerDefault(envelope.midiController)});
        }
        if (it == after.envelopes.end()) after.envelopes.push_back(std::move(envelope));
        else *it = std::move(envelope);
    }
    // Allocate only when the command is performed, after application recording/edit guards.
    // The prepared command and target IDs survive redo and never inspect mutable UI state.
    auto prepared = std::make_shared<EditCommand>();
    EditCommand command; command.label = "Edit MIDI Envelope"; command.tracks = {location.track};
    bool needsTargets = false;
    for (const auto& env : after.envelopes)
        if (isMidiController(env.midiController) && track->midiControllerTargets[(size_t) env.midiController] < 0) needsTargets = true;
    command.takeLanesOnly = location.kind == ClipLocation::Kind::Take && ! needsTargets;
    command.apply = [location, after, prepared](Song& s) mutable {
        if (! prepared->apply)
        {
            auto targets = assignRecordedMidiTargets(s, location.track, {&after});
            auto edit = modifyClip(s, location, after, "Edit MIDI Envelope");
            *prepared = targets.apply ? compose("Edit MIDI Envelope", {std::move(targets), std::move(edit)}) : std::move(edit);
        }
        prepared->apply(s);
    };
    command.revert = [prepared](Song& s) { prepared->revert(s); };
    return command;
}


// ---------------------------------------------------------------------------------------------------- track groups

namespace
{
bool isMainRouting(const std::string& target) { return target == "AudioOut/Main" || target == "AudioOut/Master"; }

struct MemberState
{
    TrackId id;
    TrackId groupId;
    std::string output;
    bool outputEdited;
};

Track* trackById(Song& s, TrackId id)
{
    for (auto& t : s.tracks)
        if (t.id == id)
            return &t;
    return nullptr;
}
} // namespace

EditCommand deleteTracks(const Song& song, const std::vector<int>& selection)
{
    if (selection.empty()) return {};
    std::set<TrackId> removed;
    std::set<size_t> returns;
    for (int index : selection)
    {
        const auto* track = trackAt(song, index);
        if (! track || index < 0 || track->kind == TrackKind::Main) return {};
        removed.insert(track->id);
        if ((size_t) index >= song.tracks.size()) returns.insert((size_t) index - song.tracks.size());
    }
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const auto& track : song.tracks)
            if (track.groupId >= 0 && removed.count(track.groupId) && removed.insert(track.id).second) grew = true;
    }
    auto beforeTracks = song.tracks, afterTracks = song.tracks;
    auto beforeReturns = song.returns, afterReturns = song.returns;
    auto beforeMain = song.main, afterMain = song.main;
    auto beforePre = song.sendPreXml, afterPre = song.sendPreXml;
    std::erase_if(afterTracks, [&](const Track& t) { return removed.count(t.id); });
    std::erase_if(afterReturns, [&](const Track& t) { return removed.count(t.id); });
    const auto eraseReturns = [&](auto& values) {
        for (auto it = returns.rbegin(); it != returns.rend(); ++it)
            if (*it < values.size()) values.erase(values.begin() + (ptrdiff_t) *it);
    };
    eraseReturns(afterPre);
    const auto referencesRemoved = [&](const std::string& routing) {
        const auto at = routing.find("/Track.");
        if (at == std::string::npos) return false;
        const auto start = at + 7;
        const auto end = routing.find('/', start);
        const auto number = routing.substr(start, end == std::string::npos ? end : end - start);
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) return false;
        for (const auto id : removed) if (number == std::to_string(id)) return true;
        return false;
    };
    const auto cleanTrack = [&](Track& t) {
        if (referencesRemoved(t.inputRouting))
        {
            t.inputRouting = t.kind == TrackKind::Midi ? "MidiIn/None" : "AudioIn/None";
            t.inputUpperDisplay = "No Input"; t.inputLowerDisplay.clear(); t.inputRoutingEdited = true;
        }
        if (referencesRemoved(t.outputRouting))
        {
            t.outputRouting = t.outputRouting.rfind("MidiOut/", 0) == 0 ? "MidiOut/None" : "AudioOut/Main";
            t.outputRoutingEdited = true;
        }
        std::function<void(std::vector<Device>&)> cleanDevices = [&](auto& devices) {
            for (auto& d : devices)
            {
                if (removed.count(d.sidechain.sourceTrackId))
                {
                    d.sidechain.on = false; d.sidechain.sourceTrackId = -1; d.sidechain.edited = true;
                    for (auto& p : d.parameters) if (p.path == "SideChain/OnOff") p.value = 0;
                }
                for (auto& chain : d.chains) cleanDevices(chain.devices);
            }
        };
        cleanDevices(t.devices);
        if (returns.empty()) return;
        std::set<int> targets;
        for (size_t index : returns)
            if (index < t.mixer.sendTargetIds.size() && t.mixer.sendTargetIds[index] >= 0) targets.insert(t.mixer.sendTargetIds[index]);
        eraseReturns(t.mixer.sends); eraseReturns(t.mixer.sendTargetIds); eraseReturns(t.mixer.sendMidi); eraseReturns(t.mixer.sendSourceXml);
        t.mixer.sendsEdited = true;
        if (! t.takeLanes.empty()) t.takeLanesNeedPatch = true;
        const auto cleanEnvelopes = [&](auto& envelopes) { std::erase_if(envelopes, [&](const auto& e) { return targets.count(e.targetId); }); };
        cleanEnvelopes(t.automation);
        for (auto& clip : t.arrangementClips) cleanEnvelopes(clip.envelopes);
        for (auto& slot : t.slots) if (slot.clip) cleanEnvelopes(slot.clip->envelopes);
        for (auto& lane : t.takeLanes) for (auto& clip : lane.clips) cleanEnvelopes(clip.envelopes);
    };
    const auto markRestore = [&](Track& t) {
        if (referencesRemoved(t.inputRouting)) t.inputRoutingEdited = true;
        if (referencesRemoved(t.outputRouting)) t.outputRoutingEdited = true;
        std::function<void(std::vector<Device>&)> mark = [&](auto& devices) {
            for (auto& d : devices)
            {
                if (removed.count(d.sidechain.sourceTrackId)) d.sidechain.edited = true;
                for (auto& chain : d.chains) mark(chain.devices);
            }
        };
        mark(t.devices);
    };
    for (auto& track : beforeTracks) markRestore(track);
    for (auto& track : beforeReturns) markRestore(track);
    markRestore(beforeMain);
    for (auto& track : afterTracks) cleanTrack(track);
    for (auto& track : afterReturns) cleanTrack(track);
    cleanTrack(afterMain);
    // Undo after saving must actively restore the original send holders, not trust the now-reduced source file.
    if (! returns.empty())
    {
        for (auto& track : beforeTracks) { track.mixer.sendsEdited = true; if (! track.takeLanes.empty()) track.takeLanesNeedPatch = true; }
        for (auto& track : beforeReturns) { track.mixer.sendsEdited = true; if (! track.takeLanes.empty()) track.takeLanesNeedPatch = true; }
        beforeMain.mixer.sendsEdited = true;
    }
    EditCommand command;
    command.label = removed.size() == 1 ? "Delete Track" : "Delete Tracks";
    command.structureChanged = true;
    const bool preEdited = song.sendsPreEdited || ! returns.empty();
    command.apply = [afterTracks, afterReturns, afterMain, afterPre, preEdited](Song& s) {
        s.tracks = afterTracks; s.returns = afterReturns; s.main = afterMain; s.sendPreXml = afterPre; s.sendsPreEdited = preEdited;
    };
    command.revert = [beforeTracks, beforeReturns, beforeMain, beforePre, preEdited](Song& s) {
        s.tracks = beforeTracks; s.returns = beforeReturns; s.main = beforeMain; s.sendPreXml = beforePre; s.sendsPreEdited = preEdited;
    };
    return command;
}

EditCommand moveTracks(const Song& song, const std::vector<int>& selection, int before, TrackId parent)
{
    const int n = (int) song.tracks.size();
    if (selection.empty() || before < 0 || before > n) return {};
    std::set<TrackId> moving;
    for (int index : selection)
    {
        if (index < 0 || index >= n) return {};
        const auto& track = song.tracks[(size_t) index];
        if (track.kind != TrackKind::Audio && track.kind != TrackKind::Midi && track.kind != TrackKind::Group) return {};
        moving.insert(track.id);
    }
    for (bool grew = true; grew;)
    {
        grew = false;
        for (const auto& track : song.tracks)
            if (moving.count(track.groupId) && moving.insert(track.id).second) grew = true;
    }
    if (moving.count(parent)) return {}; // a group cannot contain itself or one of its ancestors
    const auto parentIt = std::find_if(song.tracks.begin(), song.tracks.end(), [parent](const auto& t) { return t.id == parent; });
    if (parent >= 0 && (parentIt == song.tracks.end() || parentIt->kind != TrackKind::Group)) return {};
    std::vector<TrackId> oldOrder, order, block;
    std::vector<MemberState> oldState, newState;
    size_t at = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto& track = song.tracks[(size_t) i]; oldOrder.push_back(track.id);
        if (moving.count(track.id))
        {
            block.push_back(track.id);
            if (! moving.count(track.groupId))
            {
                oldState.push_back({track.id, track.groupId, track.outputRouting, track.outputRoutingEdited});
                auto output = track.outputRouting;
                if (isMainRouting(output) || output == "AudioOut/GroupTrack") output = parent < 0 ? "AudioOut/Main" : "AudioOut/GroupTrack";
                newState.push_back({track.id, parent, output, track.outputRoutingEdited || output != track.outputRouting});
            }
        }
        else { if (i < before) ++at; order.push_back(track.id); }
    }
    order.insert(order.begin() + (long) at, block.begin(), block.end());
    auto parentFor = [&](TrackId id) {
        for (const auto& state : newState) if (state.id == id) return state.groupId;
        for (const auto& track : song.tracks) if (track.id == id) return track.groupId;
        return TrackId{-1};
    };
    // Keep a valid preorder tree, including when the pointer is at a folded group's boundary.
    for (size_t index = 0; index < order.size(); ++index)
    {
        auto ancestor = parentFor(order[index]);
        for (int depth = 0; ancestor >= 0; ++depth)
        {
            if (depth >= n) return {};
            const auto found = std::find(order.begin(), order.end(), ancestor);
            if (found == order.end() || found >= order.begin() + (long) index) return {};
            for (auto it = std::next(found); it != order.begin() + (long) index; ++it)
            {
                auto p = parentFor(*it); int limit = n;
                while (p >= 0 && p != ancestor && --limit > 0) p = parentFor(p);
                if (p != ancestor) return {};
            }
            ancestor = parentFor(ancestor);
        }
    }
    bool changed = oldOrder != order;
    for (size_t i = 0; i < oldState.size(); ++i) changed |= oldState[i].groupId != newState[i].groupId;
    if (! changed) return {};
    auto install = [](Song& s, const std::vector<TrackId>& ids, const std::vector<MemberState>& states) {
        std::stable_sort(s.tracks.begin(), s.tracks.end(), [&](const auto& a, const auto& b) {
            return std::find(ids.begin(), ids.end(), a.id) < std::find(ids.begin(), ids.end(), b.id);
        });
        for (const auto& state : states) if (auto* track = trackById(s, state.id))
        { track->groupId = state.groupId; track->outputRouting = state.output; track->outputRoutingEdited = state.outputEdited; }
    };
    const bool wasFolded = parentIt != song.tracks.end() && parentIt->folded;
    EditCommand command; command.label = "Move Tracks"; command.structureChanged = true;
    command.apply = [order, newState, install, parent](Song& s) {
        install(s, order, newState);
        if (auto* group = trackById(s, parent)) group->folded = false;
    };
    command.revert = [oldOrder, oldState, install, parent, wasFolded](Song& s) {
        install(s, oldOrder, oldState);
        if (auto* group = trackById(s, parent)) group->folded = wasFolded;
    };
    return command;
}

bool groupableRange(const Song& song, const std::vector<int>& selection, int& first, int& count)
{
    if (selection.empty())
        return false;
    std::vector<int> sorted = selection;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    if (sorted.front() < 0 || (size_t) sorted.back() >= song.tracks.size())
        return false;
    first = sorted.front();
    count = sorted.back() - sorted.front() + 1;
    if ((int) sorted.size() != count)
        return false; // not contiguous
    const TrackId parent = song.tracks[(size_t) first].groupId;
    std::set<TrackId> groupsInside;
    for (int i = first; i < first + count; ++i)
        if (song.tracks[(size_t) i].kind == TrackKind::Group)
            groupsInside.insert(song.tracks[(size_t) i].id);
    for (int i = first; i < first + count; ++i)
    {
        const auto& t = song.tracks[(size_t) i];
        if (t.kind != TrackKind::Audio && t.kind != TrackKind::Midi && t.kind != TrackKind::Group)
            return false;
        if (t.groupId != parent && ! groupsInside.count(t.groupId))
            return false;
    }
    // a group in the range takes all of its children with it
    for (size_t j = 0; j < song.tracks.size(); ++j)
        if (groupsInside.count(song.tracks[j].groupId) && ((int) j < first || (int) j >= first + count))
            return false;
    return true;
}

bool groupableSelection(const Song& song, const std::vector<int>& selection, std::vector<int>& members)
{
    std::set<int> chosen;
    for (int i : selection)
    {
        if (i < 0 || (size_t) i >= song.tracks.size()) return false;
        const auto kind = song.tracks[(size_t) i].kind;
        if (kind != TrackKind::Audio && kind != TrackKind::Midi && kind != TrackKind::Group) return false;
        chosen.insert(i);
    }
    if (chosen.empty()) return false;
    // a group takes its children (and theirs) with it
    std::set<TrackId> groups;
    for (int i : chosen) if (song.tracks[(size_t) i].kind == TrackKind::Group) groups.insert(song.tracks[(size_t) i].id);
    for (bool grew = true; grew;)
    {
        grew = false;
        for (size_t j = 0; j < song.tracks.size(); ++j)
        {
            const auto& t = song.tracks[j];
            if (groups.count(t.groupId) && ! chosen.count((int) j))
            {
                chosen.insert((int) j);
                if (t.kind == TrackKind::Group) groups.insert(t.id);
                grew = true;
            }
        }
    }
    // the tops of the selection (parent not selected too) must share one parent
    std::set<TrackId> ids;
    for (int i : chosen) ids.insert(song.tracks[(size_t) i].id);
    std::set<TrackId> parents;
    for (int i : chosen)
        if (! ids.count(song.tracks[(size_t) i].groupId)) parents.insert(song.tracks[(size_t) i].groupId);
    if (parents.size() != 1)
        return false;
    members.assign(chosen.begin(), chosen.end());
    return true;
}

EditCommand groupTracks(const Song& song, const std::vector<int>& memberIndices, Track group, int pointeeEnd)
{
    std::set<TrackId> memberIds;
    for (int i : memberIndices) memberIds.insert(song.tracks[(size_t) i].id);
    const TrackId parent = [&] {
        for (int i : memberIndices)
            if (! memberIds.count(song.tracks[(size_t) i].groupId)) return song.tracks[(size_t) i].groupId;
        return TrackId{-1};
    }();
    std::vector<MemberState> before; // the direct members: they change group and routing
    std::vector<TrackId> orderBefore;
    for (const auto& t : song.tracks) orderBefore.push_back(t.id);
    for (int i : memberIndices)
    {
        const auto& t = song.tracks[(size_t) i];
        if (! memberIds.count(t.groupId))
            before.push_back({t.id, t.groupId, t.outputRouting, t.outputRoutingEdited});
    }
    group.groupId = parent;
    const TrackId groupId = group.id;
    const TrackId firstMember = song.tracks[(size_t) memberIndices.front()].id;

    EditCommand c;
    c.label = "Group Tracks";
    c.structureChanged = true;
    c.apply = [group, groupId, before, memberIds, firstMember, pointeeEnd](Song& s) {
        for (const auto& m : before)
            if (auto* t = trackById(s, m.id))
            {
                t->groupId = groupId;
                if (isMainRouting(t->outputRouting))
                    t->outputRouting = "AudioOut/GroupTrack";
                t->outputRoutingEdited = true;
            }
        std::vector<Track> rest, moved;
        size_t at = 0;
        bool found = false;
        for (auto& t : s.tracks)
        {
            if (memberIds.count(t.id)) { if (! found) { found = true; at = rest.size(); } moved.push_back(std::move(t)); }
            else rest.push_back(std::move(t));
        }
        (void) firstMember;
        s.tracks = std::move(rest);
        s.tracks.insert(s.tracks.begin() + (long) at, group);
        s.tracks.insert(s.tracks.begin() + (long) at + 1, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
        s.nextPointeeId = std::max(s.nextPointeeId, pointeeEnd);
        s.pointeeIdsAllocated = true;
    };
    c.revert = [groupId, before, orderBefore](Song& s) {
        s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(), [&](const Track& t) { return t.id == groupId; }), s.tracks.end());
        for (const auto& m : before)
            if (auto* t = trackById(s, m.id))
            {
                t->groupId = m.groupId;
                t->outputRouting = m.output;
                t->outputRoutingEdited = m.outputEdited;
            }
        std::stable_sort(s.tracks.begin(), s.tracks.end(), [&](const Track& a, const Track& b) {
            const auto ia = std::find(orderBefore.begin(), orderBefore.end(), a.id), ib = std::find(orderBefore.begin(), orderBefore.end(), b.id);
            return ia < ib;
        });
    };
    return c;
}

EditCommand insertTracks(std::vector<Track> tracks, int pointeeEnd, std::string label, int before)
{
    std::vector<TrackId> ids;
    for (const auto& t : tracks) ids.push_back(t.id);
    EditCommand c;
    c.label = std::move(label);
    c.structureChanged = true;
    c.apply = [tracks, pointeeEnd, before](Song& s) {
        const auto position = before < 0 ? s.tracks.size() : std::min((size_t) before, s.tracks.size());
        s.tracks.insert(s.tracks.begin() + (ptrdiff_t) position, tracks.begin(), tracks.end());
        s.nextPointeeId = std::max(s.nextPointeeId, pointeeEnd);
        s.pointeeIdsAllocated = true;
        std::function<void(const std::vector<Device>&)> reserve = [&](const auto& list) {
            for (const auto& d : list) { s.nextDeviceId = std::max(s.nextDeviceId, d.id + 1); for (const auto& chain : d.chains) reserve(chain.devices); }
        };
        for (const auto& track : tracks) reserve(track.devices);
    };
    c.revert = [ids](Song& s) {
        s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(),
                                      [&](const Track& t) { return std::find(ids.begin(), ids.end(), t.id) != ids.end(); }),
                       s.tracks.end());
    };
    return c;
}

EditCommand groupTracks(const Song& song, int first, int count, Track group, int pointeeEnd)
{
    std::vector<int> members;
    for (int i = first; i < first + count; ++i) members.push_back(i);
    return groupTracks(song, members, std::move(group), pointeeEnd);
}

EditCommand setTrackFolded(const Song& song, int track, bool folded)
{
    EditCommand c;
    const Track* t = trackAt(song, track);
    if (t == nullptr)
        return c;
    c.label = folded ? "Fold Group" : "Unfold Group";
    const bool before = t->folded;
    const TrackId id = t->id;
    c.apply = [id, folded](Song& s) { for (auto& x : s.tracks) if (x.id == id) x.folded = folded; };
    c.revert = [id, before](Song& s) { for (auto& x : s.tracks) if (x.id == id) x.folded = before; };
    return c;
}

namespace
{
// Ids of the selected tracks/returns (Main excluded); with `descendants`, selected groups add every track inside them.
std::vector<TrackId> selectedTrackIds(const Song& song, const std::vector<int>& selection, bool descendants)
{
    std::vector<TrackId> ids;
    auto add = [&ids](TrackId id) { if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id); };
    const int count = (int) (song.tracks.size() + song.returns.size());
    for (int index : selection)
        if (index >= 0 && index < count)
            add(trackAt(song, index)->id);
    if (descendants)
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const auto& t : song.tracks)
                if (t.groupId != -1 && std::find(ids.begin(), ids.end(), t.groupId) != ids.end()
                    && std::find(ids.begin(), ids.end(), t.id) == ids.end())
                { ids.push_back(t.id); grew = true; }
        }
    return ids;
}

template <typename Flag>
EditCommand setTrackFlag(const Song& song, const std::vector<TrackId>& ids, bool value, Flag flag)
{
    std::vector<std::pair<TrackId, bool>> before;
    auto collect = [&](const Track& t) { if (std::find(ids.begin(), ids.end(), t.id) != ids.end()) before.push_back({t.id, t.*flag}); };
    for (const auto& t : song.tracks) collect(t);
    for (const auto& t : song.returns) collect(t);
    if (std::none_of(before.begin(), before.end(), [value](const auto& b) { return b.second != value; })) return {};
    EditCommand c;
    auto each = [](Song& s, auto&& fn) { for (auto& t : s.tracks) fn(t); for (auto& t : s.returns) fn(t); };
    c.apply = [ids, value, flag, each](Song& s) {
        each(s, [&](Track& t) { if (std::find(ids.begin(), ids.end(), t.id) != ids.end()) t.*flag = value; });
    };
    c.revert = [before, flag, each](Song& s) {
        each(s, [&](Track& t) { for (const auto& [id, old] : before) if (t.id == id) t.*flag = old; });
    };
    return c;
}
} // namespace

EditCommand insertReturnTrack(const Song& song, ReturnInsert insert)
{
    const size_t nt = song.tracks.size(), nr = song.returns.size();
    if (nr >= maxReturnTracks || insert.track.kind != TrackKind::Return || insert.holders.size() != nt + nr || insert.holderTargets.size() != nt + nr || ! insert.sendPre)
        return {};
    auto beforeTracks = song.tracks, beforeReturns = song.returns;
    auto afterTracks = song.tracks, afterReturns = song.returns;
    auto afterPre = song.sendPreXml;
    auto addSend = [&](Track& t, size_t i) {
        if (! insert.holders[i]) return;
        t.mixer.sends.push_back(0.0003162277571); // -inf, Live's "off"
        t.mixer.sendTargetIds.push_back(insert.holderTargets[i]);
        t.mixer.sendMidi.push_back({});
        t.mixer.sendSourceXml.resize(t.mixer.sends.size() - 1);
        t.mixer.sendSourceXml.push_back(insert.holders[i]);
        t.mixer.sendsEdited = true;
        if (! t.takeLanes.empty()) t.takeLanesNeedPatch = true;
    };
    for (size_t i = 0; i < nt; ++i) addSend(afterTracks[i], i);
    for (size_t i = 0; i < nr; ++i) addSend(afterReturns[i], nt + i);
    afterReturns.push_back(insert.track);
    afterPre.resize(nr);
    afterPre.push_back(insert.sendPre);
    // Undo after saving must restore the original holder count actively, not trust the now-longer source file.
    for (size_t i = 0; i < nt; ++i) if (insert.holders[i]) beforeTracks[i].mixer.sendsEdited = true;
    for (size_t i = 0; i < nr; ++i) if (insert.holders[nt + i]) beforeReturns[i].mixer.sendsEdited = true;
    auto beforePre = song.sendPreXml;
    const int pointeeEnd = insert.pointeeEnd;
    EditCommand c;
    c.label = "Insert Return Track";
    c.structureChanged = true;
    c.apply = [afterTracks, afterReturns, afterPre, pointeeEnd](Song& s) {
        s.tracks = afterTracks; s.returns = afterReturns; s.sendPreXml = afterPre; s.sendsPreEdited = true;
        s.nextPointeeId = std::max(s.nextPointeeId, pointeeEnd); s.pointeeIdsAllocated = true;
    };
    c.revert = [beforeTracks, beforeReturns, beforePre](Song& s) {
        s.tracks = beforeTracks; s.returns = beforeReturns; s.sendPreXml = beforePre; s.sendsPreEdited = true;
    };
    return c;
}

EditCommand setTracksHidden(const Song& song, const std::vector<int>& selection, bool hidden)
{
    auto c = setTrackFlag(song, selectedTrackIds(song, selection, false), hidden, &Track::hidden);
    if (c.apply) c.label = hidden ? "Hide Tracks" : "Show Tracks";
    return c;
}

EditCommand setTracksDeactivated(const Song& song, const std::vector<int>& selection, bool deactivated)
{
    auto c = setTrackFlag(song, selectedTrackIds(song, selection, true), deactivated, &Track::deactivated);
    if (! c.apply) return c;
    c.label = deactivated ? "Deactivate Tracks" : "Activate Tracks";
    c.structureChanged = true; // the engine graph is rebuilt: devices unload / load again
    return c;
}

EditCommand ungroupTrack(const Song& song, int groupIndex)
{
    const Track group = song.tracks[(size_t) groupIndex];
    std::vector<MemberState> before;
    for (const auto& t : song.tracks)
        if (t.groupId == group.id)
            before.push_back({t.id, t.groupId, t.outputRouting, t.outputRoutingEdited});

    EditCommand c;
    c.label = "Ungroup Tracks";
    c.structureChanged = true;
    c.apply = [group, before](Song& s) {
        for (const auto& m : before)
            if (auto* t = trackById(s, m.id))
            {
                t->groupId = group.groupId;
                if (t->outputRouting == "AudioOut/GroupTrack")
                    t->outputRouting = group.groupId < 0 ? (isMainRouting(group.outputRouting) ? group.outputRouting : "AudioOut/Main")
                                                         : "AudioOut/GroupTrack";
                t->outputRoutingEdited = true;
            }
        s.tracks.erase(std::remove_if(s.tracks.begin(), s.tracks.end(), [&](const Track& t) { return t.id == group.id; }), s.tracks.end());
    };
    c.revert = [group, groupIndex, before](Song& s) {
        s.tracks.insert(s.tracks.begin() + (long) std::min<size_t>((size_t) groupIndex, s.tracks.size()), group);
        for (const auto& m : before)
            if (auto* t = trackById(s, m.id))
            {
                t->groupId = m.groupId;
                t->outputRouting = m.output;
                t->outputRoutingEdited = m.outputEdited;
            }
    };
    return c;
}


// ---------------------------------------------------------------- TRK-5 linked clips

namespace
{
template <typename Fn>
void forEachLinkableClip(const Song& song, Fn&& fn)
{
    for (int t = 0; t < (int) song.tracks.size(); ++t)
    {
        const auto& track = song.tracks[(size_t) t];
        for (const auto& clip : track.arrangementClips) fn(clip, ClipLocation::arrangement(t, clip.uid));
        for (int scene = 0; scene < (int) track.slots.size(); ++scene)
            if (const auto& clip = track.slots[(size_t) scene].clip) fn(*clip, ClipLocation::session(t, clip->uid, scene));
    }
}

template <typename T>
bool same(T a, T b) { return ! (a < b) && ! (b < a); } // exact equality without -Wfloat-equal

bool sameMusic(const std::vector<MidiNote>& a, const std::vector<MidiNote>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const MidiNote& x, const MidiNote& y) {
        return same(x.start, y.start) && same(x.duration, y.duration) && x.pitch == y.pitch && same(x.velocity, y.velocity)
            && same(x.releaseVelocity, y.releaseVelocity) && same(x.probability, y.probability) && x.enabled == y.enabled
            && same(x.velocityDeviation, y.velocityDeviation);
    });
}
} // namespace

std::vector<ClipLocation> linkedClipLocations(const Song& song, int group)
{
    std::vector<ClipLocation> result;
    if (group < 0) return result;
    forEachLinkableClip(song, [&](const Clip& clip, ClipLocation at) { if (clip.linkGroup == group) result.push_back(at); });
    return result;
}

EditCommand duplicateClipLinked(const Song& song, int track, int uid)
{
    const int i = arrangementClipIndex(song, track, uid);
    if (i < 0 || song.tracks[(size_t) track].frozen) return {};
    const auto& clip = song.tracks[(size_t) track].arrangementClips[(size_t) i];
    if (clip.kind != Clip::Kind::Midi) return {};
    int group = clip.linkGroup;
    if (group < 0)
    {
        group = 0;
        forEachLinkableClip(song, [&](const Clip& other, ClipLocation) { group = std::max(group, other.linkGroup + 1); });
    }
    std::vector<EditCommand> parts;
    if (clip.linkGroup < 0)
    {
        auto linked = clip; linked.linkGroup = group;
        parts.push_back(modifyClip(song, track, uid, std::move(linked), "Duplicate as Linked"));
    }
    auto copy = movedClip(clip, clip.currentEnd);
    markAsCopy(copy, song, track);
    copy.linkGroup = group;
    parts.push_back(insertClip(track, std::move(copy), "Duplicate as Linked"));
    return compose("Duplicate as Linked", std::move(parts));
}

EditCommand unlinkClip(const Song& song, ClipLocation location)
{
    const auto* clip = findClip(song, location);
    if (! clip || clip->linkGroup < 0) return {};
    auto after = *clip; after.linkGroup = -1;
    return modifyClip(song, location, std::move(after), "Unlink Clip");
}

std::map<int, std::vector<MidiNote>> snapshotLinkedNotes(const Song& song)
{
    std::map<int, std::vector<MidiNote>> notes;
    forEachLinkableClip(song, [&](const Clip& clip, ClipLocation) { if (clip.linkGroup >= 0) notes[clip.uid] = clip.notes; });
    return notes;
}

EditCommand syncLinkedClips(const Song& song, const std::map<int, std::vector<MidiNote>>& before)
{
    std::map<int, const Clip*> sources; // group -> the member whose notes this edit changed
    forEachLinkableClip(song, [&](const Clip& clip, ClipLocation) {
        if (clip.linkGroup < 0 || sources.count(clip.linkGroup)) return;
        const auto found = before.find(clip.uid);
        if (found != before.end() && ! sameMusic(found->second, clip.notes)) sources[clip.linkGroup] = &clip;
    });
    std::vector<EditCommand> parts;
    forEachLinkableClip(song, [&](const Clip& clip, ClipLocation at) {
        const auto source = sources.find(clip.linkGroup);
        if (clip.linkGroup < 0 || source == sources.end() || source->second == &clip || sameMusic(source->second->notes, clip.notes)) return;
        auto after = clip;
        after.notes = source->second->notes;
        for (auto& note : after.notes) { note.noteId = -1; note.key = 0; } // per-note file data stays with its own clip
        assignNoteKeys(after.notes);
        parts.push_back(modifyClip(song, at, std::move(after), "Update Linked Clips"));
    });
    return parts.empty() ? EditCommand{} : compose("Update Linked Clips", std::move(parts));
}
} // namespace sonora::model
