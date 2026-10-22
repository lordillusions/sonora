// COL-3/4: hashing, invites, the peer wire protocol and whole sessions over in-memory links.
#include "TestFramework.h"
#include "collab/BarePort.h"
#include "collab/Invite.h"
#include "core/AppDataFolder.h"
#include "core/ModelCodec.h"
#include "core/Sha256.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

#include <random>

using namespace sonora;

namespace
{
std::string hex(const crypto::Digest& d) { return crypto::toHex(d); }

model::Song loadFixture()
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    return *loaded.song;
}

std::string canonical(const model::Song& s)
{
    std::string out = model::codec::encodeDocument(s);
    for (const auto& k : collab::objectKeys(s)) out += collab::toString(k) + *collab::encodeObject(s, k);
    return out;
}

struct DropPort : collab::BarePort
{
    juce::File dropped;
    collab::AuthorId by = 0;
    std::string performDrop(collab::AuthorId author, const juce::File& file, const collab::DropRequestMsg&) override
    {
        dropped = file;
        by = author;
        return "Drop";
    }
};

struct Rig
{
    std::shared_ptr<collab::MemoryHub> hub = collab::MemoryHub::create();
    DropPort hostPort;
    std::unique_ptr<collab::CollabSession> host;
    std::vector<std::unique_ptr<collab::BarePort>> guestPorts;
    std::vector<std::unique_ptr<collab::CollabSession>> guests;

    explicit Rig(model::Song song, juce::File project = {})
    {
        hostPort.document = std::make_unique<model::Song>(std::move(song));
        collab::CollabSession::Options o;
        o.name = "Hana";
        o.sessionName = "Test Set";
        o.projectDirectory = project;
        host = collab::CollabSession::host(hostPort, hub->hostSide(), o);
        hostPort.session = host.get();
    }
    collab::CollabSession& join(const std::string& name)
    {
        guestPorts.push_back(std::make_unique<collab::BarePort>());
        collab::CollabSession::Options o;
        o.name = name;
        guests.push_back(collab::CollabSession::guest(*guestPorts.back(), hub->guestSide(), o));
        guestPorts.back()->session = guests.back().get();
        return *guests.back();
    }
    void pump(int rounds = 50)
    {
        for (int i = 0; i < rounds; ++i)
        {
            host->poll();
            for (auto& g : guests) g->poll();
            juce::Thread::sleep(1);
        }
    }
};
} // namespace

TEST_CASE("collab crypto: SHA-256 and HMAC-SHA256 match the standard vectors; base64url round-trips")
{
    CHECK(hex(crypto::sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex(crypto::sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    crypto::Sha256 million;
    const std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(a);
    CHECK(hex(million.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // RFC 4231 test cases 1 and 2.
    CHECK(hex(crypto::hmacSha256(std::string(20, '\x0b'), "Hi There")) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(hex(crypto::hmacSha256("Jefe", "what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    for (size_t n = 0; n < 40; ++n)
    {
        const auto bytes = crypto::randomBytes(n);
        std::string back;
        CHECK(crypto::fromBase64Url(crypto::base64Url(bytes), back) && back == bytes);
    }
    std::string ignored;
    CHECK(! crypto::fromBase64Url("abc$", ignored));
    CHECK(crypto::randomBytes(16) != crypto::randomBytes(16));
}

TEST_CASE("collab invite: links round-trip, bad links are refused, signals and safety codes authenticate")
{
    auto invite = collab::makeInvite();
    invite.sessionId = "s_Ab3";
    invite.relay = "https://relay.example.org";
    std::string error;
    auto parsed = collab::parseInvite("  " + collab::formatInvite(invite) + "\n", error);
    REQUIRE(parsed);
    CHECK(parsed->sessionId == "s_Ab3" && parsed->relay == invite.relay && parsed->ticket == invite.ticket && parsed->secret == invite.secret);
    invite.relay.clear();
    invite.lanAddress = "192.168.1.20:47100";
    parsed = collab::parseInvite(collab::formatInvite(invite), error);
    REQUIRE(parsed);
    CHECK(parsed->lan() && parsed->lanAddress == "192.168.1.20:47100");
    CHECK(! collab::parseInvite("https://example.com/join", error));
    CHECK(! collab::parseInvite("sonora://join/s?relay=http%3A%2F%2Fevil.example&ticket=t#AAAAAAAAAAAAAAAAAAAAAA", error)); // plain http
    CHECK(! collab::parseInvite("sonora://join/s?relay=https%3A%2F%2Fr.example&ticket=t", error));                     // no secret
    CHECK(! collab::parseInvite("sonora://join/s/../x?lan=1.2.3.4%3A5&ticket=t#AAAAAAAAAAAAAAAAAAAAAA", error));       // bad id
    CHECK(collab::parseInvite("sonora://join/s?relay=http%3A%2F%2F127.0.0.1%3A8787&ticket=t#AAAAAAAAAAAAAAAAAAAAAA", error).has_value());

    const auto tag = collab::signalTag(invite.secret, "s", "p1", "p2", "{\"sdp\":1}");
    CHECK(collab::verifySignal(invite.secret, "s", "p1", "p2", "{\"sdp\":1}", tag));
    CHECK(! collab::verifySignal(invite.secret, "s", "p1", "p2", "{\"sdp\":2}", tag));
    CHECK(! collab::verifySignal(invite.secret, "s", "p2", "p1", "{\"sdp\":1}", tag));
    CHECK(! collab::verifySignal(std::string(16, 'x'), "s", "p1", "p2", "{\"sdp\":1}", tag));

    const auto code = collab::safetyCode(invite.secret, "AA:BB", "cc:dd");
    CHECK(code == collab::safetyCode(invite.secret, "CC:DD", "aa:bb"));
    CHECK(code != collab::safetyCode(invite.secret, "AA:BB", "CC:DE"));
    CHECK(std::count(code.begin(), code.end(), '-') == 3);
    CHECK(collab::ticketHash("t").size() == 64);
}

TEST_CASE("collab protocol: every message survives the wire; garbage is rejected")
{
    collab::SnapshotMsg snap;
    snap.seq = 7;
    snap.objects = {{collab::ObjectKey::clip(5), std::string("abc")}, {collab::ObjectKey::track(3), std::nullopt}};
    snap.revisions[collab::ObjectKey::song()] = 4;
    snap.sourceFile = std::string("\0\1\2", 3);
    snap.assets.push_back({"/a.wav", "Samples/a.wav", "/proj", std::string(64, 'a'), 1234, ".wav"});
    snap.transport = {true, 12.5};
    collab::CommitMsg commit;
    commit.seq = 9;
    commit.author = 2;
    commit.intentId = 3;
    commit.kind = collab::CommitKind::Undo;
    commit.label = "Move Clip";
    commit.objects = snap.objects;
    collab::IntentMsg intent;
    intent.intentId = 4;
    intent.bases = {{collab::ObjectKey::order(12), 5, 0}, {collab::ObjectKey::clip(5), 0, 3}};
    intent.objects = snap.objects;
    collab::LiveMsg live;
    live.values = {{12, -1, collab::LiveValue::Volume, 0.5f}, {12, 40, 3, 0.25f}};
    collab::PresenceMsg presence;
    presence.view = collab::PresenceMsg::View::Clip;
    presence.beat = 17.25;
    presence.pitch = 60;
    std::vector<collab::Message> messages = {collab::HelloMsg{1, "1.0", "Ana", 2, "tok", 5}, collab::WelcomeMsg{}, snap, commit, intent,
                                             collab::RejectMsg{4, "conflict", "changed", 1, true}, collab::UndoMsg{true},
                                             collab::ParticipantsMsg{{{0, "Hana", collab::Role::Host, 0, true}}}, presence, live,
                                             collab::PluginStateMsg{1, 12, 40, {1, 2}, {3}},
                                             collab::TransportMsg{collab::TransportMsg::Action::Seek, 8.0, 123456, 1},
                                             collab::ClockMsg{true, 10, 20}, collab::AssetRequestMsg{std::string(64, 'b'), 65536},
                                             collab::AssetChunkMsg{std::string(64, 'b'), 0, 3, "xyz", false}, collab::AssetAddMsg{snap.assets},
                                             collab::UploadMsg{1, "kick.wav", 0, 3, "abc"}, collab::DropRequestMsg{1, 12, 4.0, -1},
                                             collab::RoleMsg{2, collab::Role::Listener}, collab::NoticeMsg{"hi"}, collab::ByeMsg{"bye"}};
    for (const auto& m : messages)
    {
        const auto bytes = collab::encodeMessage(m);
        const auto back = collab::decodeMessage(bytes);
        CHECK(back.index() == m.index());
        CHECK(collab::encodeMessage(back) == bytes);
    }
    const auto decoded = std::get<collab::SnapshotMsg>(collab::decodeMessage(collab::encodeMessage(snap)));
    CHECK(decoded.objects.size() == 2 && ! decoded.objects[1].full && decoded.revisions.at(collab::ObjectKey::song()) == 4);
    CHECK(decoded.sourceFile.size() == 3 && decoded.transport.playing);

    auto rejects = [](std::string bytes) {
        try { (void) collab::decodeMessage(bytes); } catch (const model::CodecError&) { return true; }
        return false;
    };
    CHECK(rejects(""));
    CHECK(rejects(std::string(1, char(200))));               // unknown type
    CHECK(rejects(collab::encodeMessage(commit) + "x"));      // trailing bytes
    const auto full = collab::encodeMessage(snap);
    for (size_t n = 1; n < full.size(); ++n) CHECK(rejects(full.substr(0, n)));
    std::mt19937 random(3);
    for (int i = 0; i < 5000; ++i)
    {
        auto b = collab::encodeMessage(messages[(size_t) random() % messages.size()]);
        b[random() % b.size()] = static_cast<char>(random());
        try { (void) collab::decodeMessage(b); } catch (const model::CodecError&) {}
    }
}

TEST_CASE("collab live session: two guests join over memory links, edit, undo, and converge with the host")
{
    Rig rig(loadFixture());
    auto& ana = rig.join("Ana");
    auto& ben = rig.join("Ben");
    rig.pump();
    REQUIRE(rig.host->state() == collab::CollabSession::State::Active);
    REQUIRE(ana.state() == collab::CollabSession::State::Active);
    REQUIRE(ben.state() == collab::CollabSession::State::Active);
    CHECK(rig.host->participants().size() == 3 && ana.participants().size() == 3);
    CHECK(ana.self() != ben.self() && ana.role() == collab::Role::Editor);
    CHECK(canonical(*rig.guestPorts[0]->document) == canonical(*rig.hostPort.document));

    // Edits from all three sides.
    model::EditHistory h;
    auto& hostSong = *rig.hostPort.document;
    auto& anaSong = *rig.guestPorts[0]->document;
    auto& benSong = *rig.guestPorts[1]->document;
    h.perform(hostSong, model::setTempo(hostSong, 111.0));
    rig.host->localEdit("Set Tempo");
    h.perform(anaSong, model::renameTrack(anaSong, 0, "Ana's Track"));
    ana.localEdit("Rename Track");
    const int uid = anaSong.tracks[0].arrangementClips[0].uid;
    h.perform(benSong, model::duplicateClip(benSong, 0, uid, 32.0));
    ben.localEdit("Duplicate");
    rig.pump();
    CHECK(hostSong.tracks[0].name == "Ana's Track" && hostSong.tracks[0].arrangementClips.size() == 2);
    CHECK(canonical(anaSong) == canonical(hostSong) && canonical(benSong) == canonical(hostSong));
    // Ben's new clip came from his own UID range.
    CHECK(hostSong.tracks[0].arrangementClips.back().uid >= 1000000000);

    // Ana undoes her rename (per user): the tempo and Ben's clip stay.
    ana.undo();
    rig.pump();
    CHECK(hostSong.tracks[0].name != "Ana's Track" && std::abs(hostSong.tempo - 111.0) < 1e-9 && hostSong.tracks[0].arrangementClips.size() == 2);
    CHECK(canonical(anaSong) == canonical(hostSong) && canonical(benSong) == canonical(hostSong));

    // Presence and live values are relayed by the host; transport requests are scheduled for everyone.
    collab::PresenceMsg p;
    p.trackId = hostSong.tracks[0].id;
    p.beat = 9.5;
    ana.sendPresence(p);
    ana.sendLive({{hostSong.tracks[0].id, -1, collab::LiveValue::Volume, 0.7f}});
    ben.requestTransport(collab::TransportMsg::Action::Play, 4.0);
    rig.pump();
    REQUIRE(ben.presence().count(ana.self()) == 1);
    CHECK(std::abs(ben.presence().at(ana.self()).presence.beat - 9.5) < 1e-9);
    CHECK(rig.host->presence().count(ana.self()) == 1);
    CHECK(! rig.guestPorts[1]->live.empty() && ! rig.hostPort.live.empty());
    CHECK(! rig.hostPort.transports.empty() && ! rig.guestPorts[0]->transports.empty());
    CHECK(rig.guestPorts[0]->transports.back().action == collab::TransportMsg::Action::Play);

    // A listener can't edit; the host can promote them back.
    rig.host->setRole(ben.self(), collab::Role::Listener);
    rig.pump();
    CHECK(! ben.canEdit());
    h.perform(benSong, model::setTempo(benSong, 70.0));
    ben.localEdit("Set Tempo");
    rig.pump();
    CHECK(std::abs(hostSong.tempo - 111.0) < 1e-9 && std::abs(benSong.tempo - 111.0) < 1e-9); // rolled back

    // Leaving: the host sees it, the guest's session ends.
    ana.leave();
    rig.pump();
    CHECK(rig.host->participants().size() == 2);
    CHECK(rig.guestPorts[0]->ended_);
}

TEST_CASE("collab live session: samples are fetched by hash into the cache and resolved only through it")
{
    // A private data folder: the user's real CollabCache stays untouched, and a stale copy can't hide the download.
    const auto previousFolder = core::appDataFolderName();
    core::setAppDataFolderName("Sonora Collab Test");
    collab::collabCacheDirectory().deleteRecursively();
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-collab-assets-test");
    dir.deleteRecursively();
    dir.createDirectory();
    const auto sample = dir.getChildFile("Kick.wav");
    juce::MemoryBlock bytes;
    for (int i = 0; i < 300000; ++i) bytes.append(&i, 1); // ~5 chunks
    sample.replaceWithData(bytes.getData(), bytes.getSize());

    auto song = loadFixture();
    model::SamplePart part;
    part.sample.absolutePath = sample.getFullPathName().toStdString();
    auto& device = song.tracks[0].devices.back();
    device.sampleParts.push_back(part);
    Rig rig(std::move(song), dir);
    auto& ana = rig.join("Ana");
    for (int i = 0; i < 400 && ana.state() != collab::CollabSession::State::Active; ++i) rig.pump(1);
    rig.pump(200);
    REQUIRE(ana.state() == collab::CollabSession::State::Active);
    CHECK(rig.host->assets().entries().size() == 1);
    const auto entry = rig.host->assets().entries().front();
    CHECK(entry.size == bytes.getSize() && entry.extension == ".wav");
    REQUIRE(! rig.guestPorts[0]->arrived.empty());
    // The guest resolves the host's reference to its cache copy, never to the host's path.
    const auto resolved = io::resolveSamplePath(*rig.guestPorts[0]->document, part.sample);
    CHECK(! resolved.empty() && resolved != part.sample.absolutePath);
    juce::MemoryBlock copy;
    REQUIRE(juce::File(resolved).loadFileAsData(copy));
    CHECK(copy == bytes);
    model::FileRef stranger;
    stranger.absolutePath = "/etc/hosts";
    CHECK(io::resolveSamplePath(*rig.guestPorts[0]->document, stranger).empty());

    // A guest drop: the file is uploaded into the host's project folder, then the host performs the drop for the guest.
    const auto local = dir.getChildFile("guest").getChildFile("Snare One.aif");
    local.getParentDirectory().createDirectory();
    local.replaceWithText("not really audio");
    CHECK(ana.uploadAndDrop(local, rig.hostPort.document->tracks[0].id, 8.0, -1));
    rig.pump(100);
    CHECK(rig.hostPort.by == ana.self());
    CHECK(rig.hostPort.dropped.getParentDirectory() == dir.getChildFile("Samples").getChildFile("Collab"));
    CHECK(rig.hostPort.dropped.getFileName() == "Snare One.aif" && rig.hostPort.dropped.loadFileAsString() == "not really audio");
    ana.leave();
    rig.pump();
    CHECK(io::resolveSamplePath(*rig.guestPorts[0]->document, part.sample) == part.sample.absolutePath); // override removed
    core::appDataDirectory().deleteRecursively(); // "Sonora Collab Test" only
    core::setAppDataFolderName(previousFolder);
    dir.deleteRecursively();
}
