#include "collab/Session.h"

#include <algorithm>
#include <set>

namespace sonora::collab
{

namespace
{
std::vector<ObjectKey> keysOf(const std::vector<ObjectState>& objects)
{
    std::vector<ObjectKey> keys;
    keys.reserve(objects.size());
    for (const auto& o : objects) keys.push_back(o.key);
    return keys;
}

std::vector<ObjectKey> keysOf(const std::vector<Change>& changes)
{
    std::vector<ObjectKey> keys;
    keys.reserve(changes.size());
    for (const auto& c : changes) keys.push_back(c.key);
    return keys;
}

size_t sizeOf(const std::vector<Change>& changes)
{
    size_t n = 0;
    for (const auto& c : changes) n += (c.before ? c.before->size() : 0) + (c.after ? c.after->size() : 0) + 16;
    return n;
}
} // namespace

// ---------------------------------------------------------------------------------------------------- Sequencer

void Sequencer::reset(const model::Song& song)
{
    index_.reset(song);
    revisions_.clear();
    log_.clear();
    logBytes_ = 0;
    seq_ = 0;
    firstLogged_ = 1;
    authors_.clear();
}

CommitMsg Sequencer::commit(AuthorId author, uint64_t intentId, CommitKind kind, const std::string& label, std::vector<Change> changes)
{
    ++seq_;
    CommitMsg msg;
    msg.seq = seq_;
    msg.author = author;
    msg.intentId = intentId;
    msg.kind = kind;
    msg.label = label;
    for (const auto& c : changes)
    {
        revisions_[c.key] = seq_;
        msg.objects.push_back({c.key, c.after});
    }
    Entry e;
    e.seq = seq_;
    e.author = author;
    e.intentId = intentId;
    e.kind = kind;
    e.label = label;
    e.bytes = sizeOf(changes);
    e.changes = std::move(changes);
    logBytes_ += e.bytes;
    log_.push_back(std::move(e));
    trim();
    return msg;
}

void Sequencer::trim()
{
    while (log_.size() > 1 && (log_.size() > limits_.logEntries || logBytes_ > limits_.logBytes))
    {
        logBytes_ -= log_.front().bytes;
        firstLogged_ = log_.front().seq + 1;
        log_.pop_front();
    }
    for (auto& [author, a] : authors_)
    {
        a.undo.erase(std::remove_if(a.undo.begin(), a.undo.end(), [&](uint64_t s) { return s < firstLogged_; }), a.undo.end());
        a.redo.erase(std::remove_if(a.redo.begin(), a.redo.end(), [&](const auto& r) { return r.first < firstLogged_ || r.second < firstLogged_; }),
                     a.redo.end());
        while (a.undo.size() > limits_.undoDepth) a.undo.erase(a.undo.begin());
        while (a.intents.size() > 1000) a.intents.erase(a.intents.begin());
    }
}

const Sequencer::Entry* Sequencer::find(uint64_t seq) const
{
    if (seq < firstLogged_ || log_.empty()) return nullptr;
    const size_t i = static_cast<size_t>(seq - log_.front().seq);
    return i < log_.size() && log_[i].seq == seq ? &log_[i] : nullptr;
}

std::optional<AuthorId> Sequencer::authorOf(uint64_t seq) const
{
    if (const auto* e = find(seq)) return e->author;
    return std::nullopt;
}

std::optional<CommitMsg> Sequencer::captureLocal(const model::Song& song, const std::string& label, AuthorId author)
{
    auto changes = index_.diff(song);
    if (changes.empty()) return std::nullopt;
    index_.update(song, keysOf(changes));
    auto msg = commit(author, 0, CommitKind::Edit, label, std::move(changes));
    auto& a = authors_[author];
    a.undo.push_back(msg.seq);
    a.redo.clear();
    trim();
    return msg;
}

Sequencer::Result Sequencer::receiveIntent(AuthorId author, const IntentMsg& intent, const model::Song& song, const Applier& apply,
                                           const std::set<std::string>& allowedPaths,
                                           const std::function<std::string(const IntentMsg&)>& check)
{
    auto& a = authors_[author];
    auto reject = [&](std::string code, std::string message, std::optional<AuthorId> conflict = std::nullopt) {
        a.intents[intent.intentId] = std::nullopt;
        RejectMsg r;
        r.intentId = intent.intentId;
        r.code = std::move(code);
        r.message = std::move(message);
        if (conflict) { r.conflictAuthor = *conflict; r.hasConflictAuthor = true; }
        Result result;
        result.reject = std::move(r);
        return result;
    };
    if (intent.objects.empty()) return reject("invalid", "The edit changed nothing");
    std::set<ObjectKey> objectKeys;
    for (const auto& o : intent.objects)
        if (! objectKeys.insert(o.key).second) return reject("invalid", "An object appears twice in one edit");
    std::set<ObjectKey> baseKeys;
    for (const auto& b : intent.bases)
    {
        if (! baseKeys.insert(b.key).second) return reject("invalid", "An object has two base revisions");
        uint64_t expected = b.revision;
        if (b.afterIntent != 0)
        {
            const auto it = a.intents.find(b.afterIntent);
            if (it == a.intents.end() || ! it->second)
                return reject("depends_on_rejected", "It built on an earlier edit that was not accepted");
            expected = *it->second;
        }
        if (const uint64_t current = revision(b.key); current != expected)
            return reject("conflict", "Someone changed this first", authorOf(current));
    }
    if (baseKeys != objectKeys) return reject("invalid", "Every changed object needs its base revision");
    if (auto error = validateObjects(song, intent.objects); ! error.empty()) return reject("invalid", error);
    for (const auto& path : referencedPaths(intent.objects))
        if (! allowedPaths.count(path)) return reject("not_allowed", "The edit refers to a file that is not shared in this session");
    if (check)
        if (auto error = check(intent); ! error.empty()) return reject("not_allowed", error);

    std::vector<Change> changes;
    for (const auto& o : intent.objects) changes.push_back({o.key, index_.full(o.key), std::nullopt});
    apply(intent.objects);
    const auto keys = keysOf(intent.objects);
    index_.update(song, keys);
    for (auto& c : changes) c.after = index_.full(c.key); // canonical: re-encoded from the host's song
    Result result;
    result.commit = commit(author, intent.intentId, CommitKind::Edit, intent.label, std::move(changes));
    a.intents[intent.intentId] = result.commit->seq;
    a.undo.push_back(result.commit->seq);
    a.redo.clear();
    trim();
    return result;
}

Sequencer::Result Sequencer::revert(AuthorId author, bool redo, const model::Song& song, const Applier& apply)
{
    auto& a = authors_[author];
    auto reject = [&](std::string code, std::string message, std::optional<AuthorId> conflict = std::nullopt) {
        RejectMsg r;
        r.code = std::move(code);
        r.message = std::move(message);
        if (conflict) { r.conflictAuthor = *conflict; r.hasConflictAuthor = true; }
        Result result;
        result.reject = std::move(r);
        return result;
    };
    const Entry* guard = nullptr;  // the commit whose objects must be unchanged
    const Entry* source = nullptr; // the commit whose before (undo) or after (redo) states are restored
    if (! redo)
    {
        while (! a.undo.empty() && ! find(a.undo.back())) a.undo.pop_back();
        if (a.undo.empty()) return reject("nothing", "Nothing to undo");
        guard = source = find(a.undo.back());
    }
    else
    {
        while (! a.redo.empty() && (! find(a.redo.back().first) || ! find(a.redo.back().second))) a.redo.pop_back();
        if (a.redo.empty()) return reject("nothing", "Nothing to redo");
        guard = find(a.redo.back().first);
        source = find(a.redo.back().second);
    }
    for (const auto& c : guard->changes)
        if (const uint64_t current = revision(c.key); current != guard->seq)
            return reject("conflict", (redo ? "Can't redo \"" : "Can't undo \"") + source->label + "\": it was changed since", authorOf(current));
    std::vector<ObjectState> states;
    for (const auto& c : source->changes) states.push_back({c.key, redo ? c.after : c.before});
    if (auto error = validateObjects(song, states); ! error.empty())
        return reject("conflict", (redo ? "Can't redo \"" : "Can't undo \"") + source->label + "\": " + error);
    const std::string label = source->label;
    std::vector<Change> changes;
    for (const auto& s : states) changes.push_back({s.key, index_.full(s.key), std::nullopt});
    apply(states);
    index_.update(song, keysOf(states));
    for (auto& c : changes) c.after = index_.full(c.key);
    const uint64_t original = source->seq;
    Result result;
    result.commit = commit(author, 0, redo ? CommitKind::Redo : CommitKind::Undo, label, std::move(changes));
    if (! redo)
    {
        a.undo.pop_back();
        a.redo.push_back({result.commit->seq, original});
    }
    else
    {
        a.redo.pop_back();
        a.undo.push_back(result.commit->seq);
    }
    trim();
    return result;
}

Sequencer::Result Sequencer::undo(AuthorId author, const model::Song& song, const Applier& apply) { return revert(author, false, song, apply); }
Sequencer::Result Sequencer::redo(AuthorId author, const model::Song& song, const Applier& apply) { return revert(author, true, song, apply); }

bool Sequencer::canUndo(AuthorId author) const
{
    auto it = authors_.find(author);
    if (it == authors_.end()) return false;
    for (auto s = it->second.undo.rbegin(); s != it->second.undo.rend(); ++s)
        if (find(*s)) return true;
    return false;
}

bool Sequencer::canRedo(AuthorId author) const
{
    auto it = authors_.find(author);
    return it != authors_.end() && ! it->second.redo.empty() && find(it->second.redo.back().second);
}

std::string Sequencer::undoLabel(AuthorId author) const
{
    auto it = authors_.find(author);
    if (it != authors_.end())
        for (auto s = it->second.undo.rbegin(); s != it->second.undo.rend(); ++s)
            if (const auto* e = find(*s)) return e->label;
    return {};
}

std::string Sequencer::redoLabel(AuthorId author) const
{
    auto it = authors_.find(author);
    if (it != authors_.end() && ! it->second.redo.empty())
        if (const auto* e = find(it->second.redo.back().second)) return e->label;
    return {};
}

std::optional<std::vector<CommitMsg>> Sequencer::since(uint64_t seq) const
{
    if (seq > seq_) return std::nullopt;
    if (seq + 1 < firstLogged_) return std::nullopt;
    std::vector<CommitMsg> out;
    for (const auto& e : log_)
    {
        if (e.seq <= seq) continue;
        CommitMsg m;
        m.seq = e.seq;
        m.author = e.author;
        m.intentId = e.intentId;
        m.kind = e.kind;
        m.label = e.label;
        for (const auto& c : e.changes) m.objects.push_back({c.key, c.after});
        out.push_back(std::move(m));
    }
    return out;
}

Sequencer::Snapshot Sequencer::snapshot(const model::Song& song)
{
    index_.reset(song); // fresh full forms (live values included); identities are unchanged
    Snapshot s;
    s.seq = seq_;
    for (const auto& key : objectKeys(song)) s.objects.push_back({key, index_.full(key)});
    s.revisions = revisions_;
    return s;
}

void Sequencer::forget(AuthorId author)
{
    if (auto it = authors_.find(author); it != authors_.end())
    {
        it->second.undo.clear();
        it->second.redo.clear();
    }
}

// ---------------------------------------------------------------------------------------------------- Client

void Client::reset(const model::Song& song, uint64_t seq, std::map<ObjectKey, uint64_t> revisions)
{
    displayed_.reset(song);
    confirmed_.clear();
    revisions_ = std::move(revisions);
    pending_.clear();
    seq_ = seq;
}

bool Client::shadowed(const ObjectKey& k, uint64_t exceptIntent) const
{
    for (const auto& p : pending_)
        if (p.id != exceptIntent)
            for (const auto& o : p.objects)
                if (o.key == k) return true;
    return false;
}

std::optional<IntentMsg> Client::captureLocal(const model::Song& song, const std::string& label)
{
    auto changes = displayed_.diff(song);
    if (changes.empty()) return std::nullopt;
    IntentMsg m;
    m.intentId = nextIntent_++;
    m.label = label;
    Pending p;
    p.id = m.intentId;
    p.label = label;
    for (auto& c : changes)
    {
        BaseRevision b;
        b.key = c.key;
        // The newest pending edit of this object is what this one builds on.
        for (auto it = pending_.rbegin(); it != pending_.rend() && b.afterIntent == 0; ++it)
            for (const auto& o : it->objects)
                if (o.key == c.key) { b.afterIntent = it->id; break; }
        if (b.afterIntent == 0)
        {
            b.revision = revision(c.key);
            confirmed_[c.key] = c.before;
        }
        m.bases.push_back(b);
        m.objects.push_back({c.key, c.after});
    }
    p.objects = m.objects;
    pending_.push_back(std::move(p));
    displayed_.update(song, keysOf(changes));
    return m;
}

std::vector<ObjectState> Client::restoreStates(const std::vector<ObjectKey>& keys)
{
    // Each key shows the newest remaining pending edit of it, else its confirmed state.
    std::vector<ObjectState> states;
    std::set<ObjectKey> done;
    for (const auto& k : keys)
    {
        if (! done.insert(k).second) continue;
        std::optional<std::optional<std::string>> state;
        for (auto it = pending_.rbegin(); it != pending_.rend() && ! state; ++it)
            for (const auto& o : it->objects)
                if (o.key == k) { state = o.full; break; }
        if (! state)
        {
            auto c = confirmed_.find(k);
            if (c == confirmed_.end()) continue; // never shadowed: already displayed as confirmed
            state = c->second;
            confirmed_.erase(c);
        }
        states.push_back({k, *state});
    }
    return states;
}

void Client::restore(const std::vector<ObjectKey>& keys, const model::Song& song, const Applier& apply)
{
    const auto states = restoreStates(keys);
    if (states.empty()) return;
    apply(states);
    displayed_.update(song, keysOf(states));
}

void Client::receiveCommit(const CommitMsg& commit, const model::Song& song, const Applier& apply)
{
    seq_ = commit.seq;
    for (const auto& o : commit.objects) revisions_[o.key] = commit.seq;

    if (commit.author == self_ && commit.intentId != 0 && ! pending_.empty() && pending_.front().id == commit.intentId)
    {
        const Pending own = std::move(pending_.front());
        pending_.pop_front();
        std::vector<ObjectState> differing;
        for (const auto& o : commit.objects)
        {
            if (shadowed(o.key)) { confirmed_[o.key] = o.full; continue; }
            confirmed_.erase(o.key);
            const auto mine = std::find_if(own.objects.begin(), own.objects.end(), [&](const ObjectState& s) { return s.key == o.key; });
            if (mine == own.objects.end() || mine->full != o.full) differing.push_back(o);
        }
        if (! differing.empty())
        {
            apply(differing);
            displayed_.update(song, keysOf(differing));
        }
        return;
    }

    // Someone else's commit. Pending edits of the same objects can no longer be accepted by the host (their base
    // revisions are now stale), nor can later edits that build on them: roll those back first.
    std::set<ObjectKey> touched;
    for (const auto& o : commit.objects) touched.insert(o.key);
    std::vector<ObjectKey> rollback;
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        const bool doomed = std::any_of(it->objects.begin(), it->objects.end(), [&](const ObjectState& o) { return touched.count(o.key) > 0; });
        if (! doomed) { ++it; continue; }
        for (const auto& o : it->objects)
        {
            touched.insert(o.key);
            rollback.push_back(o.key);
        }
        it = pending_.erase(it);
    }
    // The commit's own objects are overwritten below; only the other rolled-back objects need their confirmed state.
    std::set<ObjectKey> committed;
    for (const auto& o : commit.objects)
    {
        committed.insert(o.key);
        confirmed_.erase(o.key);
    }
    rollback.erase(std::remove_if(rollback.begin(), rollback.end(), [&](const ObjectKey& k) { return committed.count(k) > 0; }), rollback.end());
    // One apply: a restored clip order may reference clips the commit brings back, and vice versa.
    auto states = restoreStates(rollback);
    states.insert(states.end(), commit.objects.begin(), commit.objects.end());
    if (states.empty()) return;
    apply(states);
    displayed_.update(song, keysOf(states));
}

std::string Client::receiveReject(const RejectMsg& reject, const model::Song& song, const Applier& apply)
{
    auto it = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.id == reject.intentId; });
    if (it == pending_.end()) return {}; // already rolled back when a conflicting commit arrived
    const std::string label = it->label;
    std::set<ObjectKey> touched;
    std::vector<ObjectKey> rollback;
    for (; it != pending_.end();)
    {
        const bool first = it->id == reject.intentId;
        const bool dependent = std::any_of(it->objects.begin(), it->objects.end(), [&](const ObjectState& o) { return touched.count(o.key) > 0; });
        if (! first && ! dependent) { ++it; continue; }
        for (const auto& o : it->objects)
        {
            touched.insert(o.key);
            rollback.push_back(o.key);
        }
        it = pending_.erase(it);
    }
    restore(rollback, song, apply);
    return label;
}

size_t Client::dropPending(const model::Song& song, const Applier& apply)
{
    std::vector<ObjectKey> keys;
    for (const auto& p : pending_)
        for (const auto& o : p.objects) keys.push_back(o.key);
    const size_t n = pending_.size();
    pending_.clear();
    restore(keys, song, apply);
    return n;
}

} // namespace sonora::collab
