#pragma once
// COL-2 (ADR 0010): host-authoritative sequencing of object patches.
//
// The host's Sequencer is the single source of order. Every edit — the host's own, a guest's intent, an undo — is
// committed with the next sequence number and broadcast. Each object carries the sequence number of the commit that
// last changed it (its revision). A guest intent states the revision of every object it changed; the host commits it
// only when those objects are unchanged, so two people editing different objects never conflict and the later of two
// edits to the same object is rejected.
//
// A guest's Client applies its own edits at once (optimistically) and keeps them pending until the host commits or
// rejects them. Objects are whole states, not deltas, so rebasing is trivial: a pending edit's objects stay displayed,
// and a rejection restores the confirmed state of exactly those objects.
//
// Undo is per user: it reverts that user's latest commit when nobody changed its objects since, as a new commit.
//
// Applying object states to the live song (engine refresh, UI) is the caller's job, through an Applier.

#include "collab/Objects.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sonora::collab
{

using AuthorId = uint32_t;
inline constexpr AuthorId hostAuthor = 0;

struct BaseRevision
{
    ObjectKey key;
    uint64_t revision = 0;    // the object's revision this edit started from
    uint64_t afterIntent = 0; // non-zero: the state the same author's pending intent produced
};

struct IntentMsg
{
    uint64_t intentId = 0; // per author, increasing
    std::string label;     // "Move Clip"
    std::vector<BaseRevision> bases;
    std::vector<ObjectState> objects;
};

enum class CommitKind : uint8_t
{
    Edit = 0,
    Undo = 1,
    Redo = 2,
};

struct CommitMsg
{
    uint64_t seq = 0;
    AuthorId author = hostAuthor;
    uint64_t intentId = 0; // the author's intent this commits; 0 for host edits, undo and redo
    CommitKind kind = CommitKind::Edit;
    std::string label;
    std::vector<ObjectState> objects;
};

struct RejectMsg
{
    uint64_t intentId = 0;
    std::string code;    // conflict, invalid, not_allowed, depends_on_rejected
    std::string message;
    AuthorId conflictAuthor = hostAuthor; // who changed the object first (code "conflict")
    bool hasConflictAuthor = false;
};

// Applies object states to the live song and refreshes engine/UI. Must not throw for states that passed validation.
using Applier = std::function<void(const std::vector<ObjectState>&)>;

class Sequencer
{
public:
    struct Limits
    {
        size_t logEntries = 2000;          // commits kept for reconnect replay and undo
        size_t logBytes = 512u << 20;      // ... and their total size
        size_t undoDepth = 100;            // per author
    };

    Sequencer() = default;
    explicit Sequencer(Limits limits) : limits_(limits) {}

    // Session start (or a fresh snapshot): every object at revision 0.
    void reset(const model::Song&);

    // The host's own edit, already applied to `song`. Nothing when it changed no object. `author` attributes an edit the
    // host performed for a guest (their dropped file), so it lands in that guest's undo history.
    std::optional<CommitMsg> captureLocal(const model::Song& song, const std::string& label, AuthorId author = hostAuthor);

    struct Result
    {
        std::optional<CommitMsg> commit;
        std::optional<RejectMsg> reject;
    };
    // A guest's intent. `allowedPaths` are the file paths a guest may reference (paths already in the song and assets
    // shared in this session); `check` may veto an intent for host reasons (recording, role) with a message.
    Result receiveIntent(AuthorId author, const IntentMsg&, const model::Song& song, const Applier& apply,
                         const std::set<std::string>& allowedPaths, const std::function<std::string(const IntentMsg&)>& check = {});
    // Per-user undo / redo. The reject's intentId is 0.
    Result undo(AuthorId author, const model::Song& song, const Applier& apply);
    Result redo(AuthorId author, const model::Song& song, const Applier& apply);
    bool canUndo(AuthorId author) const;
    bool canRedo(AuthorId author) const;
    std::string undoLabel(AuthorId) const;
    std::string redoLabel(AuthorId) const;

    // Commits after `seq` for a reconnecting guest; nullopt when the log no longer reaches back (send a snapshot).
    std::optional<std::vector<CommitMsg>> since(uint64_t seq) const;

    // Snapshot of every object (full form) with its revision.
    struct Snapshot
    {
        uint64_t seq = 0;
        std::vector<ObjectState> objects;
        std::map<ObjectKey, uint64_t> revisions;
    };
    Snapshot snapshot(const model::Song& song);

    uint64_t seq() const { return seq_; }
    uint64_t revision(const ObjectKey& k) const
    {
        auto it = revisions_.find(k);
        return it == revisions_.end() ? 0 : it->second;
    }
    // Author of a commit still in the log.
    std::optional<AuthorId> authorOf(uint64_t seq) const;
    const ObjectIndex& index() const { return index_; }
    // Forgets an author's undo/redo history (they left).
    void forget(AuthorId author);

private:
    struct Entry
    {
        uint64_t seq = 0;
        AuthorId author = hostAuthor;
        uint64_t intentId = 0;
        CommitKind kind = CommitKind::Edit;
        std::string label;
        std::vector<Change> changes;
        size_t bytes = 0;
    };
    struct AuthorState
    {
        std::vector<uint64_t> undo;                       // seqs of this author's own edits, newest last
        std::vector<std::pair<uint64_t, uint64_t>> redo;  // (undo commit seq, original seq), newest last
        std::map<uint64_t, std::optional<uint64_t>> intents; // intent id -> committed seq (nullopt: rejected)
    };
    CommitMsg commit(AuthorId author, uint64_t intentId, CommitKind kind, const std::string& label, std::vector<Change> changes);
    const Entry* find(uint64_t seq) const;
    void trim();
    Result revert(AuthorId author, bool redo, const model::Song& song, const Applier& apply);

    Limits limits_;
    ObjectIndex index_;
    std::map<ObjectKey, uint64_t> revisions_;
    std::deque<Entry> log_;
    size_t logBytes_ = 0;
    uint64_t seq_ = 0;
    uint64_t firstLogged_ = 1; // commits before this are no longer in the log
    std::map<AuthorId, AuthorState> authors_;
};

class Client
{
public:
    explicit Client(AuthorId self = 1) : self_(self) {}

    // After installing a snapshot.
    void reset(const model::Song& song, uint64_t seq, std::map<ObjectKey, uint64_t> revisions);

    // The guest's own edit, already applied to `song`. Nothing when it changed no object.
    std::optional<IntentMsg> captureLocal(const model::Song& song, const std::string& label);
    void receiveCommit(const CommitMsg&, const model::Song& song, const Applier& apply);
    // Returns the label of the rejected edit.
    std::string receiveReject(const RejectMsg&, const model::Song& song, const Applier& apply);
    // Before replaying commits after a reconnect: pending edits may never have reached the host, so they are
    // restored to the confirmed state. Returns how many were dropped.
    size_t dropPending(const model::Song& song, const Applier& apply);

    uint64_t seq() const { return seq_; }
    size_t pending() const { return pending_.size(); }
    AuthorId self() const { return self_; }
    void setSelf(AuthorId self) { self_ = self; }
    uint64_t revision(const ObjectKey& k) const
    {
        auto it = revisions_.find(k);
        return it == revisions_.end() ? 0 : it->second;
    }

private:
    struct Pending
    {
        uint64_t id = 0;
        std::string label;
        std::vector<ObjectState> objects;
    };
    bool shadowed(const ObjectKey& k, uint64_t exceptIntent = 0) const;
    std::vector<ObjectState> restoreStates(const std::vector<ObjectKey>& keys);
    void restore(const std::vector<ObjectKey>& keys, const model::Song& song, const Applier& apply);

    AuthorId self_;
    ObjectIndex displayed_;                                  // identity of what this replica shows
    std::map<ObjectKey, std::optional<std::string>> confirmed_; // confirmed state of keys pending edits shadow
    std::map<ObjectKey, uint64_t> revisions_;
    std::deque<Pending> pending_;
    uint64_t seq_ = 0;
    uint64_t nextIntent_ = 1;
};

} // namespace sonora::collab
