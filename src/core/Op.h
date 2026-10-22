#pragma once
// COL-1 (ADR 0009, docs/COLLAB_DESIGN.md section 3): serializable, versioned edit intents.
//
// An Op is plain data addressed by stable identity (TrackId, clip UID plus its container, note key), never by
// display index. opToCommand() resolves it against the current Song into the existing undoable EditCommand, so
// history, engine refresh flags and the writer stay unchanged. Every replica builds its own command from the same
// Op; indices are never transmitted.
//
// Identities created by an Op (clip UIDs) are allocated by the sequencer: recordOp() captures them while the
// command is built and first applied, and replayOp() hands exactly those values back on a replica.
// Note keys need no transport: they are unique per clip and assigned deterministically (assignNoteKeys).

#include "core/Edit.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>

namespace sonora::model
{
inline constexpr int opFormatVersion = 1;
inline constexpr size_t maxNotesPerOp = 4096;
inline constexpr size_t maxNotesPerClip = 65536;

// An Op that cannot apply to the given Song. `code` uses the control API vocabulary: invalid_request,
// target_replaced, unsupported_clip, automation_conflict.
struct OpError : std::runtime_error
{
    OpError(std::string errorCode, const std::string& message) : std::runtime_error(message), code(std::move(errorCode)) {}
    std::string code;
};

// One clip container: Arrangement when scene and laneId are both -1, otherwise one Session slot or one take lane.
struct ClipRef
{
    TrackId trackId = -1;
    int clipUid = -1;
    int scene = -1;
    int laneId = -1;
};

// Fields of one existing note to change; at least one must be set.
struct NoteUpdate
{
    uint32_t key = 0;
    std::optional<int> pitch;
    std::optional<Beats> start, duration;
    std::optional<float> velocity, releaseVelocity, probability, velocityDeviation;
    std::optional<bool> enabled;
};

namespace ops
{
struct SetTempo { double bpm = 120.0; };
struct SetTrackGain { TrackId trackId = -1; double gainDb = 0.0; }; // absolute dB, -90..6
struct RenameTrack { TrackId trackId = -1; std::string name; };
struct SetTrackColor { std::vector<TrackId> trackIds; int color = 0; };
struct SetLoop { bool on = false; Beats start = 0.0, length = 16.0; };
// Arrangement only. Moving to another track gives the clip a new UID (recorded identity).
struct MoveClip { TrackId trackId = -1; int clipUid = -1; TrackId toTrackId = -1; Beats startBeat = 0.0; };
struct DuplicateClip { TrackId trackId = -1; int clipUid = -1; Beats destinationBeat = 0.0; }; // Arrangement only
struct DeleteClip { ClipRef clip; };
// New notes always get fresh keys and no file NoteId.
struct AddNotes { ClipRef clip; std::vector<MidiNote> notes; };
struct UpdateNotes { ClipRef clip; std::vector<NoteUpdate> updates; };
struct DeleteNotes { ClipRef clip; std::vector<uint32_t> keys; };
struct ReplaceNotes { ClipRef clip; std::vector<MidiNote> notes; };
} // namespace ops

using OpPayload = std::variant<ops::SetTempo, ops::SetTrackGain, ops::RenameTrack, ops::SetTrackColor, ops::SetLoop,
                               ops::MoveClip, ops::DuplicateClip, ops::DeleteClip, ops::AddNotes, ops::UpdateNotes,
                               ops::DeleteNotes, ops::ReplaceNotes>;

struct Op
{
    OpPayload payload;
};

// Clip UIDs one Op allocated, in allocation order.
struct OpIdentities
{
    std::vector<int> clipUids;
};

const char* opType(const Op&); // wire name, e.g. "notes.update"

// Validates `op` against `song` and builds its undoable command. Throws OpError; never mutates `song`.
EditCommand opToCommand(const Song& song, const Op& op);

// Sequencer side: as opToCommand, and `identities` receives the clip UIDs allocated while building and by the
// first apply. Later applies (undo/redo) reuse them as usual.
EditCommand recordOp(const Song& song, const Op& op, std::shared_ptr<OpIdentities> identities);

// Replica side: the build and first apply hand out exactly `identities`. `*matched` becomes false when the replica
// needed a different number of UIDs, meaning its state diverged and it must resynchronize from a snapshot.
EditCommand replayOp(const Song& song, const Op& op, OpIdentities identities, std::shared_ptr<bool> matched);

// The current notes of a clip with keys normalized exactly as the next note edit will store them.
std::vector<MidiNote> keyedNotes(const Clip& clip);

} // namespace sonora::model
