#pragma once
// Undoable edits of the Song model (ROADMAP UI-3 / UI-10 / ALS-10).
//
// The Song is plain data; every user edit is an EditCommand holding enough state to apply and revert
// itself, pushed through an EditHistory. Clips are addressed by (track index, clip uid) so commands
// stay valid when clips move around or the arrangement list is re-sorted.
//
// This layer has no engine and no UI dependency: the app applies a command, then tells the engine which
// tracks changed (AudioEngine::updateTrackClips) and the UI to refresh.

#include "core/Model.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sonora::model
{

struct EditCommand
{
    std::string label;                   // "Move Clip", shown in the Edit menu
    std::function<void(Song&)> apply;
    std::function<void(Song&)> revert;
    std::vector<int> tracks;             // track indices whose clips changed (engine refresh)
    std::vector<int> gainTracks;         // AGT-2: publish authored gain on apply/undo/redo, independently of clip edits
    bool takeLanesOnly = false;           // update prepared takes without replacing main/Session clips
    bool songLevel = false;              // tempo / loop changed (transport refresh)
    bool devicesChanged = false;         // device chain changed (UI-7)
    bool inputChanged = false;           // input source/channel; take membership is fixed while recording
    bool inputSettingsChanged = false;   // includes monitor-only edits
    bool recordingRangeChanged = false; // immutable punch/loop schedule during a take
    bool structureChanged = false;       // tracks were inserted/removed: the engine graph is rebuilt, track indices shift
    uint64_t historyBefore = 0, historyAfter = 0; // assigned by EditHistory, unique across branches
};

class EditHistory
{
public:
    // Applies `cmd` and records it; clears the redo stack.
    void perform(Song& song, EditCommand cmd);
    // Applies `extra` and folds it into the last performed command, so one undo reverts both (TRK-5 linked-clip updates).
    void amendLast(Song& song, EditCommand extra);
    const EditCommand* nextUndo() const { return done_.empty() ? nullptr : &done_.back(); }
    const EditCommand* nextRedo() const { return undone_.empty() ? nullptr : &undone_.back(); }
    bool canUndo() const { return ! done_.empty(); }
    bool canRedo() const { return ! undone_.empty(); }
    bool undoChangesDevices() const { return ! done_.empty() && done_.back().devicesChanged; }
    bool redoChangesDevices() const { return ! undone_.empty() && undone_.back().devicesChanged; }
    bool undoChangesInput() const { return ! done_.empty() && done_.back().inputChanged; }
    bool redoChangesInput() const { return ! undone_.empty() && undone_.back().inputChanged; }
    bool undoChangesRecordingRange() const { return ! done_.empty() && done_.back().recordingRangeChanged; }
    bool redoChangesRecordingRange() const { return ! undone_.empty() && undone_.back().recordingRangeChanged; }
    std::string undoLabel() const { return done_.empty() ? std::string() : done_.back().label; }
    std::string redoLabel() const { return undone_.empty() ? std::string() : undone_.back().label; }
    // Return the command that was undone / redone (nullptr when there was nothing to do) so the caller
    // can refresh the engine and UI for the tracks it touched.
    const EditCommand* undo(Song& song);
    const EditCommand* redo(Song& song);
    void clear()
    {
        done_.clear();
        undone_.clear();
        currentState_ = savedState_ = 0;
    }
    int version() const { return version_; } // monotonic UI refresh version; use isDirty for saved state
    void markSaved() { savedState_ = currentState_; }
    // COL-2: someone else changed the song (a collab commit): unsaved, but nothing to undo here.
    void markExternalChange()
    {
        currentState_ = nextState_++;
        ++version_;
    }
    bool isDirty() const { return currentState_ != savedState_; }

private:
    std::vector<EditCommand> done_, undone_;
    int version_ = 0;
    uint64_t currentState_ = 0, savedState_ = 0, nextState_ = 1;
};

// A location identifies a container as well as a clip; stale Session slots or deleted lanes never
// redirect an edit to another clip. Lane IDs remain stable when lanes are reordered.
struct ClipLocation
{
    enum class Kind { Arrangement, Session, Take };
    Kind kind = Kind::Arrangement;
    int track = -1, uid = -1, scene = -1, laneId = -1;
    static ClipLocation arrangement(int track, int uid) { return {Kind::Arrangement, track, uid, -1, -1}; }
    static ClipLocation session(int track, int uid, int scene) { return {Kind::Session, track, uid, scene, -1}; }
    static ClipLocation take(int track, int uid, int laneId) { return {Kind::Take, track, uid, -1, laneId}; }
};
const TakeLane* findTakeLane(const Song&, int track, int laneId);
TakeLane* findTakeLane(Song&, int track, int laneId);
const Clip* findClip(const Song&, ClipLocation);
Clip* findClip(Song&, ClipLocation);
EditCommand modifyClip(const Song&, ClipLocation, Clip after, std::string label);
EditCommand setNotes(const Song&, ClipLocation, std::vector<MidiNote>, std::string label = "Edit Notes");
EditCommand deleteClip(const Song&, ClipLocation);
EditCommand duplicateClip(const Song&, ClipLocation);
EditCommand renameTakeLane(const Song&, int track, int laneId, std::string name);
EditCommand moveTakeLane(const Song&, int track, int laneId, int delta);
EditCommand deleteTakeLane(const Song&, int track, int laneId);
EditCommand duplicateTakeLane(const Song&, int track, int laneId);
EditCommand auditionTakeLane(const Song&, int track, int laneId); // -1 returns to main
EditCommand compTakeRange(const Song&, int track, int laneId, Beats start, Beats end);

// Assign typed recorded envelopes to this track, allocating persistent IDs on the message thread.
// Mutates only local clips and the monotonic Song allocator; returned command is part of the take undo.
EditCommand assignRecordedMidiTargets(Song&, int track, const std::vector<Clip*>& clips);
// UI-5b: linked MIDI Ctrl envelope editing and descriptor allocation as one history entry.
// Empty points remove this lane; other envelopes and clip contents remain intact.
EditCommand setClipMidiEnvelope(const Song&, ClipLocation, AutomationEnvelope);

// ---- finding things
// scene < 0 addresses Arrangement; otherwise the exact Session slot and UID must match.
Clip* findClip(Song& song, int track, int uid, int scene = -1);
const Clip* findClip(const Song& song, int track, int uid, int scene = -1);
Clip* findArrangementClip(Song& song, int track, int uid);
const Clip* findArrangementClip(const Song& song, int track, int uid);
int arrangementClipIndex(const Song& song, int track, int uid);
const Track* trackAt(const Song& song, int index);
Track* trackAt(Song& song, int index); // tracks, then returns, then main

// ---- commands (all clips are arrangement clips unless noted)
// Replaces the clip with the given uid by `after` (keeps the uid), then re-sorts the track's clips.
EditCommand modifyClip(const Song& song, int track, int uid, Clip after, std::string label, int scene = -1);
EditCommand insertClip(int track, Clip clip, std::string label); // clip.uid is assigned on apply
EditCommand deleteClip(const Song& song, int track, int uid);
// Duplicate: a copy starting right after the original (or at `at` when >= 0); the writer clones its XML.
EditCommand duplicateClip(const Song& song, int track, int uid, Beats at = -1.0);
// TRK-5 linked clips (MIDI). Duplicate as Linked places a copy after the clip and links both (joining the clip's group when it
// has one); unlinkClip turns one member into an independent variation.
EditCommand duplicateClipLinked(const Song& song, int track, int uid);
EditCommand unlinkClip(const Song& song, ClipLocation location);
// Notes of every linked clip by uid, taken before an edit; syncLinkedClips then copies the notes of any member the edit changed
// to the rest of its group (empty command when nothing needs to change). Take-lane clips are never linked.
std::map<int, std::vector<MidiNote>> snapshotLinkedNotes(const Song& song);
EditCommand syncLinkedClips(const Song& song, const std::map<int, std::vector<MidiNote>>& before);
std::vector<ClipLocation> linkedClipLocations(const Song& song, int group);
// Split every intersecting clip at one cursor, or both edges of a time selection.
EditCommand splitArrangementClips(const Song&, int firstTrack, int lastTrack, Beats start, Beats end = -1);
// TRK-3 Strip Silence: keeps only the [start, end) timeline ranges of one Arrangement clip (each becomes a trimmed fragment with
// split semantics: content offsets survive, inner fades reset). No ranges deletes the clip; ranges covering it are a no-op.
EditCommand stripArrangementClip(const Song&, int track, int uid, const std::vector<std::pair<Beats, Beats>>& keep);
// Copies a selected time range immediately after itself, including its gaps. Replaces only
// the destination interval on the selected tracks; source/tail content offsets survive trimming.
EditCommand duplicateArrangementRegion(const Song&, int firstTrack, int lastTrack, Beats start, Beats end);
// A region moves/copies as one block. Track offsets require matching track kinds and transferable
// envelope targets; invalid destinations return an empty command without changing any source.
bool canTransferArrangementRegion(const Song&, int firstTrack, int lastTrack, Beats start, Beats end, Beats at, int trackOffset = 0);
EditCommand transferArrangementRegion(const Song&, int firstTrack, int lastTrack, Beats start, Beats end,
                                      Beats at, int trackOffset = 0, bool copy = false);
// Copies a clip to another track (or the same one) at `at`; the original stays. Both must be arrangement tracks.
EditCommand copyClipToTrack(const Song& song, int fromTrack, int uid, int toTrack, Beats at);
// Moves a clip to another track at `at` (delete + insert as one undo step).
EditCommand moveClipToTrack(const Song& song, int fromTrack, int uid, int toTrack, Beats at);
// An empty MIDI clip of `length` beats at `start`. The writer needs an existing MIDI clip to copy the file
// format from; returns false when the song has none.
bool makeEmptyMidiClip(const Song& song, Beats start, Beats length, Clip& out);
// Several commands as one undo step.
EditCommand compose(std::string label, std::vector<EditCommand> parts);

// ---- track groups
// The selection (track indices) as one contiguous range [first, first + count) that can be wrapped in a group: tracks of one
// parent (or the descendants of a group in the range), and every child of a grouped track inside the range.
bool groupableRange(const Song& song, const std::vector<int>& selection, int& first, int& count);
// Any selection of tracks (they need not be adjacent): `members` receives the sorted indices that will move into the group, the
// selection plus every child of a selected group. False when the tops of the selection do not share one parent group.
bool groupableSelection(const Song& song, const std::vector<int>& selection, std::vector<int>& members);
// Groups `members` (from groupableSelection): the group is inserted where the first member was and the members follow it in their
// original order, so tracks between them that are not selected end up after the group.
// Appends authored tracks after the last regular track (their XML comes from Track::sourceXml); `pointeeEnd` is the first
// automation id the tracks did not use. Structure change: the engine graph is rebuilt.
EditCommand insertTracks(std::vector<Track> tracks, int pointeeEnd, std::string label, int before = -1);
EditCommand groupTracks(const Song& song, const std::vector<int>& members, Track group, int pointeeEnd);
// Wraps song.tracks[first, first + count) in `group` (an authored GroupTrack, see io::createGroupTrack). Members that played
// into Main now play into the group. `pointeeEnd` is the first automation id the group did not use.
EditCommand groupTracks(const Song& song, int first, int count, Track group, int pointeeEnd);
// Opens / closes a group track (its children's lanes show or hide; saved as <TrackUnfolded>).
// UI-34: before is an original source-track index (or tracks.size()); parent is a group ID or -1.
// Selected groups move with their full subtree. Invalid/cyclic/no-op destinations return an empty command.
EditCommand moveTracks(const Song&, const std::vector<int>& selection, int before, TrackId parent = -1);
// UI-37: control indices may include returns; Main is protected. Groups include their descendants.
EditCommand deleteTracks(const Song&, const std::vector<int>& selection);
EditCommand setTrackFolded(const Song& song, int track, bool folded);
// TRK-1: control indices (tracks, then returns; Main is ignored). Hidden tracks leave the views and keep playing; deactivated
// tracks are built empty by the engine (devices unloaded, clips silent) and keep everything in the model. Deactivating a
// group also deactivates its descendants. Saved as <SonoraTrack>. No-op selections return an empty command.
// TRK-2: everything a new return track adds to the set, authored by io::createReturnTrack. `holders` has one entry per track then
// per existing return (nullptr leaves that track's sends untouched); `holderTargets` are their send automation ids.
// The reference app (12.4.6) crashes while opening a set with more than 12 returns (verified 2026-10-17), so Sonora stops there.
constexpr size_t maxReturnTracks = 12;
struct ReturnInsert
{
    Track track;
    std::vector<std::shared_ptr<const std::string>> holders;
    std::vector<int> holderTargets;
    std::shared_ptr<const std::string> sendPre;
    int pointeeEnd = 0;
};
EditCommand insertReturnTrack(const Song& song, ReturnInsert insert);
EditCommand setTracksHidden(const Song& song, const std::vector<int>& selection, bool hidden);
EditCommand setTracksDeactivated(const Song& song, const std::vector<int>& selection, bool deactivated);
// Removes the group track at `groupIndex`; its children move up to its parent (or the top level).
EditCommand ungroupTrack(const Song& song, int groupIndex);
EditCommand setNotes(const Song& song, int track, int uid, std::vector<MidiNote> notes, std::string label = "Edit Notes", int scene = -1);
EditCommand recordArrangementTake(const Song& song, int track, Clip clip);
EditCommand appendTakeLanes(const Song& song, int track, std::vector<TakeLane> lanes);
// MIDI Arrangement Overdub: a finished MIDI take that lies inside one existing MIDI clip is merged into that clip's notes (the
// take clip itself is not inserted). Anything else - audio, takes with controller envelopes, takes that cross clip borders or
// start in empty space - is stored like recordArrangementTake (replacing what it covers).
EditCommand recordArrangementOverdub(const Song& song, int track, Clip take);
EditCommand recordArrangementPasses(const Song& song, int track, std::vector<Clip> passes);
EditCommand setSessionClip(const Song& song, int track, int scene, Clip clip);
EditCommand insertDevice(int track, Device device, int position = -1);
// Browser insertion policy: MIDI effects -> one instrument -> audio effects. A new instrument replaces
// the existing instrument(s) in one undo step; imported unsupported chains are left alone until edited.
EditCommand insertTrackDevice(const Song& song, int track, Device device);
// destination is the final zero-based model index after removing the source. Only audio effects move;
// on MIDI tracks their destination must follow every MIDI effect and instrument.
bool canMoveAudioEffect(const Song& song, int track, int deviceId, int destination);
EditCommand moveAudioEffect(const Song& song, int track, int deviceId, int destination);
// Removes the device with this id from the track (the device chain is rebuilt; undo puts it back at the same position
// with the state it had when it was removed).
EditCommand removeDevice(const Song& song, int track, int deviceId);
EditCommand replaceDevice(const Song& song, int track, int device, Device replacement, std::string label);
void allocateDeviceIds(Song& song, Device& device);
EditCommand setTrackInput(const Song& song, int track, std::string target, std::string upper, std::string lower, int monitoring);
EditCommand setAutomation(const Song& song, int track, AutomationEnvelope envelope);
EditCommand setTempo(const Song& song, double bpm);
EditCommand setTimeSignature(const Song&, int numerator, int denominator);
EditCommand setGlobalQuantisation(const Song& song, int value);
// Set scale (UI-30b): root pitch class 0..11 and model::kScales index. No-op when unchanged.
EditCommand setScale(const Song& song, int root, int name);
EditCommand setTrackColor(const Song&, const std::vector<int>& tracks, int color);
EditCommand renameTrack(const Song& song, int track, std::string name);
EditCommand addLocator(const Song& song, Beats time, std::string name);
EditCommand setLoop(const Song& song, bool on, Beats start, Beats length);
EditCommand setPunch(const Song& song, bool in, bool out);

// ---- clip geometry helpers (used by the arrangement UI)
// Moves the clip start to `newStart` keeping its length.
Clip movedClip(Clip c, Beats newStart);
// Trims/extends the right edge.
Clip withEnd(Clip c, Beats newEnd);
// Trims/extends the left edge; the content stays where it is on the timeline
// (startRelative absorbs the change; `tempo` converts beats to seconds for unwarped audio).
Clip withStart(Clip c, Beats newStart, double tempo);

} // namespace sonora::model
