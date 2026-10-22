// COL-2: replicated objects, the host sequencer and guest clients over an in-memory transport.
#include "TestFramework.h"
#include "collab/Session.h"
#include "core/ModelCodec.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <random>
#include <variant>

using namespace sonora;
using collab::ObjectKey;

namespace
{
model::Song loadFixture(const char* name = "two_tracks.xml")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile(name));
    REQUIRE(loaded);
    return *loaded.song;
}

std::string patchedXml(const model::Song& song)
{
    std::string error;
    auto dom = io::readAlsDom(juce::File(song.sourcePath), error);
    REQUIRE(dom);
    REQUIRE(io::patchAlsDom(song, *dom).ok);
    return dom->toString().toStdString();
}

bool same(const model::Song& a, const model::Song& b)
{
    // Per object: one song-wide encoding would also compare which fragments happen to be shared in memory.
    auto canonical = [](const model::Song& s) {
        std::string out = model::codec::encodeDocument(s);
        for (const auto& k : collab::objectKeys(s)) out += collab::toString(k) + *collab::encodeObject(s, k);
        return out;
    };
    if (canonical(a) == canonical(b)) return true;
    if (model::codec::encodeDocument(a) != model::codec::encodeDocument(b))
        std::printf("      document differs: savedClips %zu vs %zu\n", a.savedClips.size(), b.savedClips.size());
    auto ka = collab::objectKeys(a), kb = collab::objectKeys(b);
    for (const auto& k : ka)
        if (collab::encodeObject(a, k) != collab::encodeObject(b, k)) std::printf("      differs: %s\n", collab::toString(k).c_str());
    for (const auto& k : kb)
        if (! collab::encodeObject(a, k)) std::printf("      missing: %s\n", collab::toString(k).c_str());
    return false;
}

// A replica made the way a guest gets it: from the host's snapshot objects only.
model::Song fromSnapshot(const collab::Sequencer::Snapshot& snapshot, const model::Song& host)
{
    auto song = collab::songFromObjects(snapshot.objects);
    model::codec::decodeDocument(model::codec::encodeDocument(host), song);
    return song;
}

collab::Applier applierFor(model::Song& song)
{
    return [&song](const std::vector<collab::ObjectState>& states) { collab::applyObjects(song, states); };
}

int midiClipUid(const model::Song& song, int track)
{
    for (const auto& c : song.tracks[(size_t) track].arrangementClips)
        if (c.kind == model::Clip::Kind::Midi) return c.uid;
    return -1;
}

std::vector<model::MidiNote> someNotes(int seed, int count)
{
    std::vector<model::MidiNote> notes;
    for (int i = 0; i < count; ++i)
    {
        model::MidiNote n;
        n.pitch = 48 + (seed * 7 + i * 5) % 24;
        n.start = 0.25 * i;
        n.duration = 0.25;
        n.velocity = static_cast<float>(60 + (seed + i) % 60);
        notes.push_back(n);
    }
    return notes;
}
} // namespace

TEST_CASE("collab objects: a snapshot rebuilds the song exactly, and object patches replay a diff")
{
    auto host = loadFixture();
    collab::Sequencer sequencer;
    sequencer.reset(host);
    const auto snapshot = sequencer.snapshot(host);
    CHECK(collab::validateObjects(model::Song{}, snapshot.objects).empty());
    auto guest = fromSnapshot(snapshot, host);
    CHECK(same(guest, host));
    CHECK(patchedXml(guest) == patchedXml(host));

    // Edits that touch every object kind: song header, track header, devices, structure-free clip moves and deletions.
    collab::ObjectIndex index;
    index.reset(host);
    model::EditHistory history;
    history.perform(host, model::setTempo(host, 99.5));
    history.perform(host, model::renameTrack(host, 1, "Bass"));
    const int uid = midiClipUid(host, 0);
    REQUIRE(uid >= 0);
    history.perform(host, model::setNotes(host, 0, uid, someNotes(3, 5)));
    history.perform(host, model::duplicateClip(host, 0, uid));
    history.perform(host, model::moveClipToTrack(host, 0, uid, 1, 8.0));
    history.perform(host, model::removeDevice(host, 0, host.tracks[0].devices.back().id));
    const auto changes = index.diff(host);
    std::vector<collab::ObjectState> states;
    for (const auto& c : changes) states.push_back({c.key, c.after});
    CHECK(collab::validateObjects(guest, states).empty());
    const auto fx = collab::applyObjects(guest, states);
    CHECK(fx.songHeader && ! fx.structure && fx.devices.count(host.tracks[0].id) && fx.clips.count(host.tracks[1].id));
    CHECK(same(guest, host));
    CHECK(patchedXml(guest) == patchedXml(host));
    // Live values (here the mixer) are not part of change detection.
    index.reset(host);
    host.tracks[0].mixer.volume = 0.25;
    CHECK(index.diff(host).empty());
}

TEST_CASE("collab objects: inconsistent patches are rejected by validation")
{
    auto song = loadFixture();
    const int track = song.tracks[0].id;
    const int uid = midiClipUid(song, 0);
    // A clip order naming a clip that does not exist.
    collab::Order order;
    order.arrangement = {uid, 424242};
    order.slots.assign(song.tracks[0].slots.size(), -1);
    CHECK(! collab::validateObjects(song, {{ObjectKey::order(track), collab::encodeOrder(order)}}).empty());
    // The same clip placed on two tracks.
    collab::Order other;
    other.arrangement = {uid};
    other.slots.assign(song.tracks[1].slots.size(), -1);
    CHECK(! collab::validateObjects(song, {{ObjectKey::order(song.tracks[1].id), collab::encodeOrder(other)}}).empty());
    // A non-finite clip value.
    auto clip = song.tracks[0].arrangementClips[0];
    clip.currentEnd = std::numeric_limits<double>::infinity();
    CHECK(! collab::validateObjects(song, {{ObjectKey::clip(clip.uid), model::codec::encodeClip(clip, model::codec::Form::Full)}}).empty());
    // A header whose id disagrees with its key, and garbage bytes.
    CHECK(! collab::validateObjects(song, {{ObjectKey::track(999), model::codec::encodeTrackHeader(song.tracks[0], model::codec::Form::Full)}}).empty());
    CHECK(! collab::validateObjects(song, {{ObjectKey::clip(uid), std::string("\x01\xff\xff", 3)}}).empty());
    // Removing a track without removing it from the structure.
    CHECK(! collab::validateObjects(song, {{ObjectKey::track(track), std::nullopt}}).empty());
    // A tempo outside the supported range.
    auto header = song;
    header.tempo = 5000.0;
    CHECK(! collab::validateObjects(song, {{ObjectKey::song(), model::codec::encodeSongHeader(header, model::codec::Form::Full)}}).empty());
    // A guest may only reference files that are already part of the session.
    auto audio = song.tracks[0].arrangementClips[0];
    audio.sample.absolutePath = "/etc/passwd";
    const auto paths = collab::referencedPaths({{ObjectKey::clip(audio.uid), model::codec::encodeClip(audio, model::codec::Form::Full)}});
    CHECK(paths.count("/etc/passwd") == 1);
    CHECK(collab::referencedPaths(song).count("/etc/passwd") == 0);
}

TEST_CASE("collab session: host and guest edits converge; the later edit of the same object is rejected")
{
    auto host = loadFixture();
    collab::Sequencer sequencer;
    sequencer.reset(host);
    auto snapshot = sequencer.snapshot(host);
    auto guest = fromSnapshot(snapshot, host);
    collab::Client client(1);
    client.reset(guest, snapshot.seq, snapshot.revisions);
    model::EditHistory hostHistory, guestHistory;
    const auto allowed = collab::referencedPaths(host);

    // Host edit reaches the guest.
    hostHistory.perform(host, model::renameTrack(host, 0, "Lead"));
    auto c1 = sequencer.captureLocal(host, "Rename Track");
    REQUIRE(c1);
    client.receiveCommit(*c1, guest, applierFor(guest));
    CHECK(guest.tracks[0].name == "Lead" && same(guest, host));

    // Guest edit becomes an intent, the host commits it, the guest sees its own commit.
    guestHistory.perform(guest, model::setTempo(guest, 140.0));
    auto i1 = client.captureLocal(guest, "Set Tempo");
    REQUIRE(i1);
    CHECK(client.pending() == 1);
    auto r1 = sequencer.receiveIntent(1, *i1, host, applierFor(host), allowed);
    REQUIRE(r1.commit);
    client.receiveCommit(*r1.commit, guest, applierFor(guest));
    CHECK(client.pending() == 0 && same(guest, host));

    // Concurrent edits of different clips: both commit.
    const int uid = midiClipUid(host, 0);
    hostHistory.perform(host, model::setNotes(host, 0, uid, someNotes(1, 4)));
    guestHistory.perform(guest, model::renameTrack(guest, 1, "Keys"));
    auto i2 = client.captureLocal(guest, "Rename Track");
    auto c2 = sequencer.captureLocal(host, "Edit Notes");
    REQUIRE(i2 && c2);
    auto r2 = sequencer.receiveIntent(1, *i2, host, applierFor(host), allowed);
    CHECK(r2.commit.has_value());
    client.receiveCommit(*c2, guest, applierFor(guest));
    client.receiveCommit(*r2.commit, guest, applierFor(guest));
    CHECK(same(guest, host));

    // Concurrent edits of the same clip: the host's arrives first, the guest's is rolled back and rejected.
    hostHistory.perform(host, model::setNotes(host, 0, uid, someNotes(2, 3)));
    guestHistory.perform(guest, model::setNotes(guest, 0, uid, someNotes(9, 6)));
    auto i3 = client.captureLocal(guest, "Edit Notes");
    auto c3 = sequencer.captureLocal(host, "Edit Notes");
    REQUIRE(i3 && c3);
    client.receiveCommit(*c3, guest, applierFor(guest)); // rolls the pending edit back
    CHECK(client.pending() == 0);
    auto r3 = sequencer.receiveIntent(1, *i3, host, applierFor(host), allowed);
    REQUIRE(r3.reject);
    CHECK(r3.reject->code == "conflict" && r3.reject->hasConflictAuthor && r3.reject->conflictAuthor == collab::hostAuthor);
    client.receiveReject(*r3.reject, guest, applierFor(guest));
    CHECK(same(guest, host));
    CHECK(guest.tracks[0].arrangementClips[0].notes.size() == 3);

    // Two pending guest edits of the same object: the second builds on the first and both commit.
    guestHistory.perform(guest, model::setNotes(guest, 0, uid, someNotes(4, 2)));
    auto i4 = client.captureLocal(guest, "Edit Notes");
    guestHistory.perform(guest, model::setNotes(guest, 0, uid, someNotes(5, 7)));
    auto i5 = client.captureLocal(guest, "Edit Notes");
    REQUIRE(i4 && i5);
    CHECK(i5->bases[0].afterIntent == i4->intentId);
    auto r4 = sequencer.receiveIntent(1, *i4, host, applierFor(host), allowed);
    auto r5 = sequencer.receiveIntent(1, *i5, host, applierFor(host), allowed);
    REQUIRE(r4.commit && r5.commit);
    client.receiveCommit(*r4.commit, guest, applierFor(guest));
    client.receiveCommit(*r5.commit, guest, applierFor(guest));
    CHECK(client.pending() == 0 && same(guest, host));
    CHECK(host.tracks[0].arrangementClips[0].notes.size() == 7);
}

TEST_CASE("collab session: undo is per user and refused when someone else changed the object since")
{
    auto host = loadFixture();
    collab::Sequencer sequencer;
    sequencer.reset(host);
    auto snapshot = sequencer.snapshot(host);
    auto guest = fromSnapshot(snapshot, host);
    collab::Client client(1);
    client.reset(guest, snapshot.seq, snapshot.revisions);
    model::EditHistory hostHistory, guestHistory;
    const auto allowed = collab::referencedPaths(host);
    auto deliver = [&](const collab::Sequencer::Result& r) {
        REQUIRE(r.commit);
        client.receiveCommit(*r.commit, guest, applierFor(guest));
    };

    guestHistory.perform(guest, model::renameTrack(guest, 0, "Guest Name"));
    deliver(sequencer.receiveIntent(1, *client.captureLocal(guest, "Rename Track"), host, applierFor(host), allowed));
    hostHistory.perform(host, model::setTempo(host, 90.0));
    client.receiveCommit(*sequencer.captureLocal(host, "Set Tempo"), guest, applierFor(guest));

    // The guest's undo reverts its rename, not the host's later tempo change.
    CHECK(sequencer.undoLabel(1) == "Rename Track");
    deliver(sequencer.undo(1, host, applierFor(host)));
    CHECK(host.tracks[0].name != "Guest Name" && std::abs(host.tempo - 90.0) < 1e-9 && same(guest, host));
    deliver(sequencer.redo(1, host, applierFor(host)));
    CHECK(host.tracks[0].name == "Guest Name" && same(guest, host));

    // The host renames the same track: the guest's undo is refused and names the host.
    hostHistory.perform(host, model::renameTrack(host, 0, "Host Name"));
    client.receiveCommit(*sequencer.captureLocal(host, "Rename Track"), guest, applierFor(guest));
    auto refused = sequencer.undo(1, host, applierFor(host));
    REQUIRE(refused.reject);
    CHECK(refused.reject->code == "conflict" && refused.reject->conflictAuthor == collab::hostAuthor);
    // The host can still undo its own edits, newest first.
    deliver(sequencer.undo(collab::hostAuthor, host, applierFor(host)));
    CHECK(host.tracks[0].name == "Guest Name" && same(guest, host));
}

TEST_CASE("collab session: reconnect replays missed commits, or needs a snapshot once the log is trimmed")
{
    auto host = loadFixture();
    collab::Sequencer::Limits limits;
    limits.logEntries = 3;
    collab::Sequencer sequencer(limits);
    sequencer.reset(host);
    auto snapshot = sequencer.snapshot(host);
    auto guest = fromSnapshot(snapshot, host);
    collab::Client client(1);
    client.reset(guest, snapshot.seq, snapshot.revisions);
    model::EditHistory h;
    // A pending guest edit that never reached the host is restored on reconnect.
    h.perform(guest, model::renameTrack(guest, 1, "Lost"));
    REQUIRE(client.captureLocal(guest, "Rename Track"));
    for (int i = 0; i < 2; ++i)
    {
        h.perform(host, model::setTempo(host, 100.0 + i));
        REQUIRE(sequencer.captureLocal(host, "Set Tempo"));
    }
    CHECK(client.dropPending(guest, applierFor(guest)) == 1);
    auto missed = sequencer.since(client.seq());
    REQUIRE(missed && missed->size() == 2);
    for (const auto& c : *missed) client.receiveCommit(c, guest, applierFor(guest));
    CHECK(same(guest, host));
    for (int i = 0; i < 5; ++i)
    {
        h.perform(host, model::setTempo(host, 110.0 + i));
        REQUIRE(sequencer.captureLocal(host, "Set Tempo"));
    }
    CHECK(! sequencer.since(client.seq()).has_value());
}

namespace
{
// A tiny network: per-link FIFO queues delivered in random interleavings.
struct Net
{
    using Message = std::variant<collab::CommitMsg, collab::RejectMsg, collab::IntentMsg, int /* undo: 0, redo: 1 */>;
    std::vector<std::deque<Message>> toGuest, toHost;
    explicit Net(size_t guests) : toGuest(guests), toHost(guests) {}
    bool idle() const
    {
        for (const auto& q : toGuest) if (! q.empty()) return false;
        for (const auto& q : toHost) if (! q.empty()) return false;
        return true;
    }
};

// One random edit through the ordinary model command factories.
bool randomEdit(model::Song& song, model::EditHistory& history, std::mt19937& random)
{
    const int track = (int) (random() % song.tracks.size());
    auto& t = song.tracks[(size_t) track];
    std::vector<int> uids;
    for (const auto& c : t.arrangementClips) uids.push_back(c.uid);
    const int uid = uids.empty() ? -1 : uids[random() % uids.size()];
    model::EditCommand cmd;
    switch (random() % 9)
    {
        case 0: cmd = model::setTempo(song, 80.0 + (double) (random() % 80)); break;
        case 1: cmd = model::renameTrack(song, track, "T" + std::to_string(random() % 1000)); break;
        case 2: cmd = model::setTrackColor(song, {track}, (int) (random() % 20)); break;
        case 3: if (uid >= 0) cmd = model::setNotes(song, track, uid, someNotes((int) (random() % 50), 1 + (int) (random() % 6))); break;
        case 4: if (uid >= 0 && uids.size() < 6) cmd = model::duplicateClip(song, track, uid, 4.0 * (double) (random() % 16)); break;
        case 5: if (uid >= 0 && uids.size() > 1) cmd = model::deleteClip(song, track, uid); break;
        case 6:
            if (uid >= 0)
                if (const auto* c = model::findArrangementClip(song, track, uid))
                    cmd = model::modifyClip(song, track, uid, model::movedClip(*c, 4.0 * (double) (random() % 16)), "Move Clip");
            break;
        case 7: cmd = model::setLoop(song, random() % 2 == 0, 4.0 * (double) (random() % 4), 4.0 + 4.0 * (double) (random() % 4)); break;
        case 8:
            if (uid >= 0 && song.tracks.size() > 1)
            {
                const int to = (track + 1) % (int) song.tracks.size();
                if (song.tracks[(size_t) to].kind == t.kind) cmd = model::moveClipToTrack(song, track, uid, to, 4.0 * (double) (random() % 8));
            }
            break;
    }
    if (! cmd.apply) return false;
    history.perform(song, std::move(cmd));
    return true;
}
} // namespace

TEST_CASE("collab session: fuzz - a host and two guests editing at random always converge")
{
    for (unsigned seed = 1; seed <= 12; ++seed)
    {
        std::mt19937 random(seed);
        auto host = loadFixture();
        collab::Sequencer sequencer;
        sequencer.reset(host);
        const auto snapshot = sequencer.snapshot(host);
        const size_t guests = 2;
        std::vector<model::Song> songs;
        std::vector<collab::Client> clients;
        for (size_t g = 0; g < guests; ++g)
        {
            songs.push_back(fromSnapshot(snapshot, host));
            clients.emplace_back((collab::AuthorId) (g + 1));
            clients.back().reset(songs.back(), snapshot.seq, snapshot.revisions);
        }
        Net net(guests);
        model::EditHistory history;
        const auto allowed = collab::referencedPaths(host);
        auto broadcast = [&](const collab::CommitMsg& c) { for (auto& q : net.toGuest) q.push_back(c); };
        int commits = 0, rejects = 0;
        auto handle = [&](size_t g, const collab::Sequencer::Result& r) {
            if (r.commit) { ++commits; broadcast(*r.commit); }
            if (r.reject && r.reject->intentId != 0) { ++rejects; net.toGuest[g].push_back(*r.reject); }
        };

        for (int step = 0; step < 400; ++step)
        {
            const int action = (int) (random() % 10);
            if (action < 2)
            {
                if (randomEdit(host, history, random))
                    if (auto c = sequencer.captureLocal(host, "edit")) { ++commits; broadcast(*c); }
            }
            else if (action < 5)
            {
                const size_t g = random() % guests;
                if (random() % 8 == 0) net.toHost[g].push_back(Net::Message(int(random() % 2)));
                else if (randomEdit(songs[g], history, random))
                    if (auto i = clients[g].captureLocal(songs[g], "edit")) net.toHost[g].push_back(*i);
            }
            else
            {
                // Deliver one message on a random non-empty link.
                const size_t link = random() % (guests * 2);
                const size_t g = link % guests;
                auto& q = link < guests ? net.toGuest[g] : net.toHost[g];
                if (q.empty()) continue;
                auto message = std::move(q.front());
                q.pop_front();
                if (link < guests)
                {
                    if (auto* c = std::get_if<collab::CommitMsg>(&message)) clients[g].receiveCommit(*c, songs[g], applierFor(songs[g]));
                    else if (auto* r = std::get_if<collab::RejectMsg>(&message)) clients[g].receiveReject(*r, songs[g], applierFor(songs[g]));
                }
                else
                {
                    const auto author = (collab::AuthorId) (g + 1);
                    if (auto* i = std::get_if<collab::IntentMsg>(&message)) handle(g, sequencer.receiveIntent(author, *i, host, applierFor(host), allowed));
                    else if (auto* u = std::get_if<int>(&message))
                        handle(g, *u == 0 ? sequencer.undo(author, host, applierFor(host)) : sequencer.redo(author, host, applierFor(host)));
                }
            }
        }
        // Drain.
        while (! net.idle())
            for (size_t g = 0; g < guests; ++g)
            {
                while (! net.toHost[g].empty())
                {
                    auto message = std::move(net.toHost[g].front());
                    net.toHost[g].pop_front();
                    const auto author = (collab::AuthorId) (g + 1);
                    if (auto* i = std::get_if<collab::IntentMsg>(&message)) handle(g, sequencer.receiveIntent(author, *i, host, applierFor(host), allowed));
                    else if (auto* u = std::get_if<int>(&message))
                        handle(g, *u == 0 ? sequencer.undo(author, host, applierFor(host)) : sequencer.redo(author, host, applierFor(host)));
                }
                while (! net.toGuest[g].empty())
                {
                    auto message = std::move(net.toGuest[g].front());
                    net.toGuest[g].pop_front();
                    if (auto* c = std::get_if<collab::CommitMsg>(&message)) clients[g].receiveCommit(*c, songs[g], applierFor(songs[g]));
                    else if (auto* r = std::get_if<collab::RejectMsg>(&message)) clients[g].receiveReject(*r, songs[g], applierFor(songs[g]));
                }
            }
        for (size_t g = 0; g < guests; ++g)
        {
            CHECK(clients[g].pending() == 0);
            CHECK(clients[g].seq() == sequencer.seq());
            CHECK(same(songs[g], host));
        }
        CHECK(patchedXml(songs[0]) == patchedXml(host));
        CHECK(commits > 40);
        if (seed == 1) std::printf("    fuzz seed 1: %d commits, %d rejects\n", commits, rejects);
    }
}
