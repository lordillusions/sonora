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
    static constexpr const char* names[] = {"tempo.set", "track.set_gain", "track.rename", "track.set_color", "loop.set",
                                            "clip.move", "clip.duplicate", "clip.delete", "notes.add", "notes.update",
                                            "notes.delete", "notes.replace"};
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

std::vector<MidiNote> keyedNotes(const Clip& clip)
{
    auto notes = clip.notes;
    assignNoteKeys(notes);
    return notes;
}

} // namespace sonora::model
