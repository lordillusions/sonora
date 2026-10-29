#include "core/Op.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

namespace sonora::model
{
namespace
{
constexpr double maxBeat = 1000000.0;
constexpr int maxId = 1000000000;

[[noreturn]] void invalid(const std::string& message) { throw OpError("invalid_request", message); }

double bounded(double value, double lo, double hi, const char* name)
{
    if (! std::isfinite(value) || value < lo || value > hi) invalid(std::string("Out of range: ") + name);
    return value;
}

int trackIndex(const Song& song, TrackId id)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].id == id) return static_cast<int>(i);
    throw OpError("target_replaced", "Track ID no longer exists");
}

// Control index (tracks, then returns) of a renamable/recolourable track; Main is never addressed by ID.
int controlIndex(const Song& song, TrackId id)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].id == id) return static_cast<int>(i);
    for (size_t i = 0; i < song.returns.size(); ++i)
        if (song.returns[i].id == id) return static_cast<int>(song.tracks.size() + i);
    throw OpError("target_replaced", "Track ID no longer exists");
}

ClipLocation location(const Song& song, const ClipRef& ref)
{
    if (ref.scene >= 0 && ref.laneId >= 0) invalid("Choose scene OR laneId");
    if (ref.clipUid < 0 || ref.clipUid > maxId) invalid("Out of range: clipId");
    const int track = trackIndex(song, ref.trackId);
    const auto at = ref.scene >= 0 ? ClipLocation::session(track, ref.clipUid, ref.scene)
                  : ref.laneId >= 0 ? ClipLocation::take(track, ref.clipUid, ref.laneId)
                                    : ClipLocation::arrangement(track, ref.clipUid);
    if (! findClip(song, at)) throw OpError("target_replaced", "Clip no longer exists at the specified location");
    return at;
}

const Clip& midiClip(const Song& song, const ClipLocation& at)
{
    const auto* clip = findClip(song, at);
    if (clip->kind != Clip::Kind::Midi) throw OpError("unsupported_clip", "Note edits require a MIDI clip");
    return *clip;
}

void validate(const MidiNote& note)
{
    if (note.pitch < 0 || note.pitch > 127) invalid("Out of range: pitch");
    bounded(note.start, 0, maxBeat, "startBeat");
    bounded(note.duration, 1e-6, maxBeat, "durationBeats");
    if (note.start + note.duration > maxBeat) invalid("Note end exceeds 1000000 beats");
    bounded(note.velocity, 0, 127, "velocity");
    bounded(note.releaseVelocity, 0, 127, "releaseVelocity");
    bounded(note.probability, 0, 1, "probability");
    bounded(note.velocityDeviation, -127, 127, "velocityDeviation");
}

void appendNew(std::vector<MidiNote>& notes, const std::vector<MidiNote>& added)
{
    for (auto note : added)
    {
        validate(note);
        note.key = 0;     // the clip allocates keys
        note.noteId = -1; // a new note has no per-note file data
        notes.push_back(note);
    }
}

std::unordered_map<uint32_t, size_t> keyIndex(const std::vector<MidiNote>& notes)
{
    std::unordered_map<uint32_t, size_t> index;
    index.reserve(notes.size());
    for (size_t i = 0; i < notes.size(); ++i) index.emplace(notes[i].key, i);
    return index;
}

EditCommand noteCommand(const Song& song, const ClipLocation& at, std::vector<MidiNote> notes)
{
    if (notes.size() > maxNotesPerClip) invalid("Note edit exceeds 65536 notes per clip");
    return setNotes(song, at, std::move(notes), "Edit Notes");
}

void checkCount(size_t count, bool allowEmpty)
{
    if (count > maxNotesPerOp || (count == 0 && ! allowEmpty)) invalid("Expected 1–4096 note changes (replace also accepts empty)");
}

EditCommand build(const Song& song, const ops::SetTempo& op)
{
    return setTempo(song, bounded(op.bpm, 20, 999, "bpm"));
}

EditCommand build(const Song& song, const ops::SetTrackGain& op)
{
    const int index = trackIndex(song, op.trackId);
    const auto& track = song.tracks[static_cast<size_t>(index)];
    for (const auto& lane : track.automation)
        if (lane.targetId == track.mixer.volumeTargetId && ! lane.points.empty())
            throw OpError("automation_conflict", "Gain has automation; edit the envelope instead");
    const double after = std::pow(10.0, bounded(op.gainDb, -90, 6, "gainDb") / 20.0), before = track.mixer.volume;
    EditCommand command;
    command.label = "Set Track Gain";
    command.gainTracks = {index};
    command.apply = [index, after](Song& s) { s.tracks[static_cast<size_t>(index)].mixer.volume = after; };
    command.revert = [index, before](Song& s) { s.tracks[static_cast<size_t>(index)].mixer.volume = before; };
    return command;
}

EditCommand build(const Song& song, const ops::RenameTrack& op)
{
    if (op.name.size() > 256) invalid("Track name exceeds 256 bytes");
    return renameTrack(song, controlIndex(song, op.trackId), op.name);
}

EditCommand build(const Song& song, const ops::SetTrackColor& op)
{
    if (op.trackIds.empty() || op.trackIds.size() > 1024) invalid("Expected 1–1024 trackIds");
    if (op.color < 0 || op.color >= 70) invalid("Out of range: color");
    std::set<TrackId> unique;
    std::vector<int> indices;
    for (const auto id : op.trackIds)
    {
        if (! unique.insert(id).second) invalid("Duplicate trackId");
        indices.push_back(controlIndex(song, id));
    }
    return setTrackColor(song, indices, op.color);
}

EditCommand build(const Song& song, const ops::SetLoop& op)
{
    return setLoop(song, op.on, bounded(op.start, 0, maxBeat, "startBeat"), bounded(op.length, 1e-6, maxBeat, "lengthBeats"));
}

EditCommand build(const Song& song, const ops::MoveClip& op)
{
    const int from = trackIndex(song, op.trackId);
    const int to = trackIndex(song, op.toTrackId < 0 ? op.trackId : op.toTrackId);
    const auto* clip = findArrangementClip(song, from, op.clipUid);
    if (! clip) throw OpError("target_replaced", "Clip no longer exists");
    const Beats start = bounded(op.startBeat, 0, maxBeat, "startBeat");
    if (from == to) return modifyClip(song, from, op.clipUid, movedClip(*clip, start), "Move Clip");
    const auto kind = song.tracks[static_cast<size_t>(to)].kind;
    if ((clip->kind == Clip::Kind::Midi) != (kind == TrackKind::Midi) || (clip->kind == Clip::Kind::Audio) != (kind == TrackKind::Audio))
        throw OpError("unsupported_clip", "Destination track cannot hold this clip kind");
    return moveClipToTrack(song, from, op.clipUid, to, start);
}

EditCommand build(const Song& song, const ops::DuplicateClip& op)
{
    const int index = trackIndex(song, op.trackId);
    if (! findArrangementClip(song, index, op.clipUid)) throw OpError("target_replaced", "Clip no longer exists");
    return duplicateClip(song, index, op.clipUid, bounded(op.destinationBeat, 0, maxBeat, "destinationBeat"));
}

EditCommand build(const Song& song, const ops::DeleteClip& op)
{
    return deleteClip(song, location(song, op.clip));
}

EditCommand build(const Song& song, const ops::AddNotes& op)
{
    const auto at = location(song, op.clip);
    checkCount(op.notes.size(), false);
    auto notes = keyedNotes(midiClip(song, at));
    appendNew(notes, op.notes);
    return noteCommand(song, at, std::move(notes));
}

EditCommand build(const Song& song, const ops::UpdateNotes& op)
{
    const auto at = location(song, op.clip);
    checkCount(op.updates.size(), false);
    auto notes = keyedNotes(midiClip(song, at));
    const auto index = keyIndex(notes);
    std::set<uint32_t> seen;
    for (const auto& update : op.updates)
    {
        const auto found = index.find(update.key);
        if (update.key == 0 || found == index.end()) throw OpError("target_replaced", "Note key no longer exists");
        if (! seen.insert(update.key).second) invalid("Duplicate note key");
        if (! (update.pitch || update.start || update.duration || update.velocity || update.releaseVelocity
               || update.probability || update.velocityDeviation || update.enabled))
            invalid("Note update must contain a changed field");
        auto& note = notes[found->second];
        if (update.pitch) note.pitch = *update.pitch;
        if (update.start) note.start = *update.start;
        if (update.duration) note.duration = *update.duration;
        if (update.velocity) note.velocity = *update.velocity;
        if (update.releaseVelocity) note.releaseVelocity = *update.releaseVelocity;
        if (update.probability) note.probability = *update.probability;
        if (update.velocityDeviation) note.velocityDeviation = *update.velocityDeviation;
        if (update.enabled) note.enabled = *update.enabled;
        validate(note);
    }
    return noteCommand(song, at, std::move(notes));
}

EditCommand build(const Song& song, const ops::DeleteNotes& op)
{
    const auto at = location(song, op.clip);
    checkCount(op.keys.size(), false);
    auto notes = keyedNotes(midiClip(song, at));
    const auto index = keyIndex(notes);
    std::set<uint32_t> removed;
    for (const auto key : op.keys)
    {
        if (key == 0 || ! index.count(key)) throw OpError("target_replaced", "Note key no longer exists");
        if (! removed.insert(key).second) invalid("Duplicate note key");
    }
    std::erase_if(notes, [&removed](const MidiNote& note) { return removed.count(note.key) > 0; });
    return noteCommand(song, at, std::move(notes));
}

EditCommand build(const Song& song, const ops::ReplaceNotes& op)
{
    const auto at = location(song, op.clip);
    checkCount(op.notes.size(), true);
    midiClip(song, at);
    std::vector<MidiNote> notes;
    appendNew(notes, op.notes);
    return noteCommand(song, at, std::move(notes));
}

const Locator& locator(const Song& song, int id)
{
    if (id < 0 || id > maxId) invalid("Out of range: locatorId");
    const auto* found = findLocator(song, id);
    if (! found) throw OpError("target_replaced", "Locator ID no longer exists");
    return *found;
}

void checkLocatorFields(const std::optional<std::string>& name, const std::optional<std::string>& infoText, std::optional<int> color)
{
    if (name && name->size() > 256) invalid("Locator name exceeds 256 bytes");
    if (infoText && infoText->size() > 4096) invalid("Locator info text exceeds 4096 bytes");
    if (color && (*color < -1 || *color >= 70)) invalid("Out of range: color");
}

void checkFreeBeat(const Song& song, Beats beat, int except)
{
    for (const auto& other : song.locators)
        if (other.id != except && std::abs(other.time - beat) < 1e-9) invalid("A locator already exists at this beat");
}

EditCommand build(const Song& song, const ops::AddLocator& op)
{
    const Beats beat = bounded(op.beat, 0, maxBeat, "beat");
    checkLocatorFields(op.name, {}, op.color);
    checkFreeBeat(song, beat, -1);
    return addLocator(song, beat, op.name ? *op.name : defaultLocatorName(song), op.color);
}

EditCommand build(const Song& song, const ops::UpdateLocator& op)
{
    auto after = locator(song, op.locatorId);
    if (! (op.beat || op.name || op.infoText || op.color || op.songStart)) invalid("Locator update must contain a changed field");
    checkLocatorFields(op.name, op.infoText, op.color);
    if (op.beat) { after.time = bounded(*op.beat, 0, maxBeat, "beat"); checkFreeBeat(song, after.time, op.locatorId); }
    if (op.name) after.name = *op.name;
    if (op.infoText) after.annotation = *op.infoText;
    if (op.color) after.colorIndex = *op.color;
    if (op.songStart) after.isSongStart = *op.songStart;
    const char* label = op.beat && ! (op.name || op.infoText || op.color || op.songStart) ? "Move Locator"
                      : op.name && ! (op.beat || op.infoText || op.color || op.songStart) ? "Rename Locator"
                      : op.color && ! (op.beat || op.name || op.infoText || op.songStart) ? "Change Locator Color"
                                                                                           : "Edit Locator";
    return changeLocator(song, op.locatorId, std::move(after), label);
}

EditCommand build(const Song& song, const ops::DeleteLocator& op)
{
    locator(song, op.locatorId);
    return deleteLocator(song, op.locatorId);
}

EditCommand build(const Song& song, const ops::SetTracksFolded& op)
{
    if (op.trackIds.size() > 1024) invalid("Expected at most 1024 trackIds");
    std::vector<int> groups;
    if (op.trackIds.empty())
    {
        for (size_t i = 0; i < song.tracks.size(); ++i)
            if (song.tracks[i].kind == TrackKind::Group) groups.push_back(static_cast<int>(i));
    }
    else
    {
        std::set<TrackId> unique;
        for (const auto id : op.trackIds)
        {
            if (! unique.insert(id).second) invalid("Duplicate trackId");
            const int index = trackIndex(song, id);
            if (song.tracks[static_cast<size_t>(index)].kind != TrackKind::Group) invalid("Only group tracks fold: trackId " + std::to_string(id));
            groups.push_back(index);
        }
    }
    std::vector<EditCommand> parts;
    for (const int index : groups)
        if (song.tracks[static_cast<size_t>(index)].folded != op.folded) parts.push_back(setTrackFolded(song, index, op.folded));
    auto command = compose(op.folded ? "Fold Groups" : "Unfold Groups", std::move(parts));
    if (! command.apply) command.apply = command.revert = [](Song&) {}; // already in that state: an empty undo step
    return command;
}

// An Arrangement region [start, end) of track `index` that no clip occupies.
void requireFreeRegion(const Song& song, int index, Beats start, Beats end)
{
    for (const auto& clip : song.tracks[static_cast<size_t>(index)].arrangementClips)
        if (clip.currentStart < end - 1e-9 && clip.currentEnd > start + 1e-9)
            throw OpError("region_occupied", "Clip " + std::to_string(clip.uid) + " already occupies part of that region; move or delete it first");
}

EditCommand build(const Song& song, const ops::CreateClip& op)
{
    const int index = trackIndex(song, op.trackId);
    const auto& track = song.tracks[static_cast<size_t>(index)];
    if (track.kind != TrackKind::Midi) throw OpError("unsupported_clip", "Only MIDI tracks take a new MIDI clip; copy an audio clip instead");
    const Beats length = bounded(op.lengthBeats, 1.0 / 64, 65536, "lengthBeats");
    const Beats start = op.scene >= 0 ? 0.0 : bounded(op.startBeat, 0, maxBeat - length, "startBeat");
    if (op.name && op.name->size() > 256) invalid("Clip name exceeds 256 bytes");
    if (op.notes.size() > maxNotesPerOp) invalid("Expected at most 4096 notes");
    Clip clip;
    if (! makeEmptyMidiClip(song, start, length, clip))
        throw OpError("unsupported_clip", "This set has no MIDI clip format to create a clip from");
    clip.colorIndex = track.colorIndex;
    if (op.name) clip.name = *op.name;
    appendNew(clip.notes, op.notes);
    if (op.scene >= 0)
    {
        if (op.scene > 999) invalid("Out of range: scene");
        if (static_cast<size_t>(op.scene) < track.slots.size() && track.slots[static_cast<size_t>(op.scene)].clip)
            throw OpError("region_occupied", "That Session slot already holds a clip");
        auto command = setSessionClip(song, index, op.scene, std::move(clip));
        command.label = "Insert MIDI Clip";
        return command;
    }
    requireFreeRegion(song, index, start, start + length);
    return insertClip(index, std::move(clip), "Insert MIDI Clip");
}

EditCommand build(const Song& song, const ops::CopyClip& op)
{
    const int from = trackIndex(song, op.trackId);
    const int to = trackIndex(song, op.toTrackId < 0 ? op.trackId : op.toTrackId);
    const auto* clip = findArrangementClip(song, from, op.clipUid);
    if (! clip) throw OpError("target_replaced", "Clip no longer exists");
    const Beats start = bounded(op.destinationBeat, 0, maxBeat, "destinationBeat");
    const auto kind = song.tracks[static_cast<size_t>(to)].kind;
    if ((clip->kind == Clip::Kind::Midi) != (kind == TrackKind::Midi) || (clip->kind == Clip::Kind::Audio) != (kind == TrackKind::Audio))
        throw OpError("unsupported_clip", "Destination track cannot hold this clip kind");
    requireFreeRegion(song, to, start, start + (clip->currentEnd - clip->currentStart));
    return copyClipToTrack(song, from, op.clipUid, to, start);
}

constexpr double preRollTime = -63072000.0;

// The lane's value at `beat` as the engine evaluates it (engine::AutomationLane::valueAt, kept in core for edits).
double envelopeValue(const AutomationEnvelope& envelope, Beats beat)
{
    const auto& points = envelope.points;
    if (points.empty()) return 0.0;
    const auto next = std::upper_bound(points.begin(), points.end(), beat, [](Beats t, const AutomationPoint& p) { return t < p.time; });
    if (next == points.begin()) return next->value;
    const auto& prev = *(next - 1);
    if (next == points.end() || envelope.stepped) return prev.value;
    return prev.value + (next->value - prev.value) * segmentShape(prev, (beat - prev.time) / (next->time - prev.time));
}

// The device (top level or inside a rack chain) owning `targetId`; fills info's parameter, stepped and normalized.
const Device* findDevice(const std::vector<Device>& devices, int targetId, AutomationTargetInfo& info)
{
    for (const auto& device : devices)
    {
        if (device.enabledTargetId == targetId) { info.parameter = "on"; info.stepped = true; return &device; }
        for (const auto& p : device.parameters)
            if (p.automationTargetId == targetId) { info.parameter = p.path; return &device; }
        for (const auto& p : device.pluginParams)
            if (p.automationTargetId == targetId) { info.parameter = std::to_string(p.parameterId); info.normalized = true; return &device; }
        for (const auto& chain : device.chains)
            if (const auto* inner = findDevice(chain.devices, targetId, info)) return inner;
    }
    return nullptr;
}

// The manual (unautomated) value of a target, used as the value outside a new lane.
double manualValue(const Track& track, const AutomationTargetInfo& info, int targetId)
{
    const auto& mixer = track.mixer;
    if (info.kind == "volume") return mixer.volume;
    if (info.kind == "pan") return mixer.pan;
    if (info.kind == "activator") return mixer.speakerOn ? 1.0 : 0.0;
    if (info.kind == "send") return static_cast<size_t>(info.sendIndex) < mixer.sends.size() ? mixer.sends[static_cast<size_t>(info.sendIndex)] : 0.0;
    AutomationTargetInfo found;
    if (const auto* device = findDevice(track.devices, targetId, found))
    {
        if (found.parameter == "on") return device->enabled ? 1.0 : 0.0;
        for (const auto& p : device->parameters) if (p.automationTargetId == targetId) return p.value;
        for (const auto& p : device->pluginParams) if (p.automationTargetId == targetId) return p.value;
    }
    return 0.0;
}

void checkValue(const AutomationTargetInfo& info, double value)
{
    const auto range = [value](double lo, double hi) { if (! std::isfinite(value) || value < lo || value > hi) invalid("Out of range: automation value"); };
    if (info.stepped) { if (value != 0.0 && value != 1.0) invalid("On/off automation values are 0 or 1"); return; }
    if (info.kind == "volume") range(0.0, 1.995262315);
    else if (info.kind == "pan") range(-1.0, 1.0);
    else if (info.kind == "send") range(0.0, 1.0);
    else if (info.kind == "tempo") range(20.0, 999.0);
    else if (info.normalized) range(0.0, 1.0); // plug-in parameters automate their normalized value
    else range(-1e12, 1e12);
}

EditCommand build(const Song& song, const ops::SetAutomation& op)
{
    const int index = controlIndex(song, op.trackId);
    const auto& track = *trackAt(song, index);
    const auto info = describeAutomationTarget(track, op.targetId);
    if (info.kind.empty() || info.kind == "midi") throw OpError("target_replaced", "Automation target is not on this track");
    if (! op.eventType.empty() && op.eventType != "FloatEvent" && op.eventType != "BoolEvent" && op.eventType != "EnumEvent")
        invalid("eventType must be FloatEvent, BoolEvent or EnumEvent");
    if (op.points.size() > maxNotesPerOp) invalid("Expected at most 4096 automation points");
    const auto existing = std::find_if(track.automation.begin(), track.automation.end(), [&op](const AutomationEnvelope& e) { return e.targetId == op.targetId; });
    if (op.clear)
    {
        if (! op.points.empty() || op.rangeStart || op.rangeEnd) invalid("automation.clear takes no points or range");
        if (existing == track.automation.end()) invalid("That target has no automation to clear");
        const auto before = track.automation;
        auto after = before;
        after.erase(after.begin() + (existing - track.automation.begin()));
        EditCommand command;
        command.label = "Delete Automation";
        command.tracks = {index};
        command.apply = [index, after](Song& s) { if (auto* t = trackAt(s, index)) t->automation = after; };
        command.revert = [index, before](Song& s) { if (auto* t = trackAt(s, index)) t->automation = before; };
        return command;
    }
    if (op.rangeStart.has_value() != op.rangeEnd.has_value()) invalid("Give both rangeStartBeat and rangeEndBeat, or neither");
    const bool ranged = op.rangeStart.has_value();
    if (ranged)
    {
        bounded(*op.rangeStart, 0, maxBeat, "rangeStartBeat");
        bounded(*op.rangeEnd, 0, maxBeat, "rangeEndBeat");
        if (*op.rangeEnd <= *op.rangeStart) invalid("rangeEndBeat must be after rangeStartBeat");
    }
    else if (op.points.empty()) invalid("Give at least one point (automation.clear removes a lane)");
    auto points = op.points;
    std::stable_sort(points.begin(), points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
    const bool stepped = info.stepped || op.eventType == "BoolEvent" || op.eventType == "EnumEvent"
                      || (existing != track.automation.end() && existing->stepped && op.eventType.empty());
    for (auto& point : points)
    {
        bounded(point.time, 0, maxBeat, "beat");
        if (ranged && (point.time < *op.rangeStart || point.time > *op.rangeEnd)) invalid("Every point must lie inside the range");
        checkValue(info, point.value);
        point.x1 = point.y1 = point.x2 = point.y2 = 0.5;
    }
    AutomationEnvelope envelope;
    if (existing != track.automation.end()) envelope = *existing;
    else
    {
        envelope.targetId = op.targetId;
        envelope.eventType = stepped ? (op.eventType.empty() ? "BoolEvent" : op.eventType) : "FloatEvent";
        envelope.points.push_back({preRollTime, manualValue(track, info, op.targetId)});
    }
    if (! op.eventType.empty()) envelope.eventType = op.eventType;
    envelope.stepped = stepped;
    if (! ranged)
    {
        // The whole lane: the value before the first point holds the first point's value.
        envelope.points.clear();
        envelope.points.push_back({preRollTime, points.front().value});
        envelope.points.insert(envelope.points.end(), points.begin(), points.end());
    }
    else
    {
        // Replace only [start, end]; the previous value is pinned at both edges so the lane outside is unchanged.
        const Beats start = *op.rangeStart, end = *op.rangeEnd;
        const double atStart = envelopeValue(envelope, start - 1e-9), atEnd = envelopeValue(envelope, end);
        std::vector<AutomationPoint> kept;
        for (const auto& point : envelope.points)
            if (point.time < start || point.time > end) kept.push_back(point);
        if (points.empty() || points.front().time > start) kept.push_back({start, atStart});
        kept.insert(kept.end(), points.begin(), points.end());
        if (points.empty() || points.back().time < end) kept.push_back({end, atEnd});
        std::stable_sort(kept.begin(), kept.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
        envelope.points = std::move(kept);
    }
    if (envelope.points.size() > maxNotesPerClip) invalid("Automation lane exceeds 65536 points");
    return setAutomation(song, index, std::move(envelope));
}

// Runs the first apply of `command` under a scope, then restores the plain apply for undo/redo.
template<class Before, class After>
EditCommand wrapFirstApply(EditCommand command, Before before, After after)
{
    auto inner = std::make_shared<std::function<void(Song&)>>(std::move(command.apply));
    auto first = std::make_shared<bool>(true);
    command.apply = [inner, first, before, after](Song& song) {
        if (! *first)
        {
            (*inner)(song);
            return;
        }
        *first = false;
        auto scope = before();
        (*inner)(song);
        after(*scope);
    };
    return command;
}
} // namespace

const char* opType(const Op& op)
{
    if (const auto* automation = std::get_if<ops::SetAutomation>(&op.payload); automation && automation->clear) return "automation.clear";
    static constexpr const char* names[] = {"tempo.set", "track.set_gain", "track.rename", "track.set_color", "loop.set",
                                            "clip.move", "clip.duplicate", "clip.delete", "notes.add", "notes.update",
                                            "notes.delete", "notes.replace", "locator.add", "locator.update", "locator.delete",
                                            "track.set_folded", "clip.create", "clip.copy", "automation.set"};
    static_assert(std::size(names) == std::variant_size_v<OpPayload>);
    return names[op.payload.index()];
}

EditCommand opToCommand(const Song& song, const Op& op)
{
    return std::visit([&song](const auto& payload) { return build(song, payload); }, op.payload);
}

EditCommand recordOp(const Song& song, const Op& op, std::shared_ptr<OpIdentities> identities)
{
    if (! identities) identities = std::make_shared<OpIdentities>();
    EditCommand command;
    {
        ClipUidScope scope(ClipUidScope::Mode::Record);
        command = opToCommand(song, op);
        identities->clipUids = scope.uids();
    }
    return wrapFirstApply(std::move(command),
                          [] { return std::make_unique<ClipUidScope>(ClipUidScope::Mode::Record); },
                          [identities](const ClipUidScope& scope) {
                              identities->clipUids.insert(identities->clipUids.end(), scope.uids().begin(), scope.uids().end());
                          });
}

EditCommand replayOp(const Song& song, const Op& op, OpIdentities identities, std::shared_ptr<bool> matched)
{
    if (! matched) matched = std::make_shared<bool>(true);
    *matched = true;
    EditCommand command;
    auto remaining = std::make_shared<std::vector<int>>();
    {
        ClipUidScope scope(ClipUidScope::Mode::Replay, identities.clipUids);
        command = opToCommand(song, op);
        if (scope.overran()) *matched = false;
        remaining->assign(identities.clipUids.begin() + static_cast<std::ptrdiff_t>(scope.consumed()), identities.clipUids.end());
    }
    return wrapFirstApply(std::move(command),
                          [remaining] { return std::make_unique<ClipUidScope>(ClipUidScope::Mode::Replay, *remaining); },
                          [matched](const ClipUidScope& scope) { if (! scope.replayedExactly()) *matched = false; });
}

AutomationTargetInfo describeAutomationTarget(const Track& track, int targetId)
{
    AutomationTargetInfo info;
    if (targetId < 0) return info;
    const auto& mixer = track.mixer;
    if (targetId == mixer.volumeTargetId) info.kind = "volume";
    else if (targetId == mixer.panTargetId) info.kind = "pan";
    else if (targetId == mixer.speakerTargetId) { info.kind = "activator"; info.stepped = true; }
    else if (targetId == mixer.tempoTargetId) info.kind = "tempo";
    for (size_t i = 0; info.kind.empty() && i < mixer.sendTargetIds.size(); ++i)
        if (mixer.sendTargetIds[i] == targetId) { info.kind = "send"; info.sendIndex = static_cast<int>(i); }
    if (! info.kind.empty()) return info;
    if (const auto* device = findDevice(track.devices, targetId, info))
    {
        info.kind = "device";
        info.deviceId = device->id;
        return info;
    }
    for (size_t i = 0; i < track.midiControllerTargets.size(); ++i)
        if (track.midiControllerTargets[i] == targetId) info.kind = "midi";
    return info;
}

std::vector<MidiNote> keyedNotes(const Clip& clip)
{
    auto notes = clip.notes;
    assignNoteKeys(notes);
    return notes;
}

} // namespace sonora::model
