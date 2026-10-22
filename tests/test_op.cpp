// COL-1: serializable operations, runtime note keys and host-allocated identities (docs/COLLAB_DESIGN.md).
#include "TestFramework.h"
#include "core/OpJson.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>

#include <set>

using namespace sonora;

namespace
{
model::Song loadTwoTracks()
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
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

model::MidiNote note(int pitch, double start, double duration, float velocity)
{
    model::MidiNote n;
    n.pitch = pitch; n.start = start; n.duration = duration; n.velocity = velocity;
    return n;
}

struct Peer
{
    model::Song song;
    model::EditHistory history;
};

// Host commits through the sequencer path; the replica receives only the wire text.
model::OpIdentities commit(Peer& host, Peer& replica, const model::Op& op)
{
    auto identities = std::make_shared<model::OpIdentities>();
    host.history.perform(host.song, model::recordOp(host.song, op, identities));
    const auto wire = model::opMessageToString(model::committedOpToJson(op, *identities));
    auto [decoded, received] = model::committedOpFromJson(juce::JSON::parse(wire));
    auto matched = std::make_shared<bool>(false);
    replica.history.perform(replica.song, model::replayOp(replica.song, decoded, received, matched));
    CHECK(*matched);
    return *identities;
}

std::string roundTrip(const model::Op& op)
{
    const auto text = model::opMessageToString(model::opToJson(op));
    const auto again = model::opMessageToString(model::opToJson(model::opFromJson(juce::JSON::parse(text))));
    CHECK(text == again);
    return text.toStdString();
}

std::string errorCode(const model::Song& song, const model::Op& op)
{
    try { model::opToCommand(song, op); }
    catch (const model::OpError& error) { return error.code; }
    return {};
}
} // namespace

TEST_CASE("op: note keys are unique per clip, idempotent and repair duplicates deterministically")
{
    std::vector<model::MidiNote> notes{note(60, 0, 1, 100), note(62, 1, 1, 100), note(64, 2, 1, 100)};
    notes[1].key = 7;
    model::assignNoteKeys(notes);
    CHECK(notes[0].key == 8); CHECK(notes[1].key == 7); CHECK(notes[2].key == 9);
    const auto before = notes;
    model::assignNoteKeys(notes);
    for (size_t i = 0; i < notes.size(); ++i) CHECK(notes[i].key == before[i].key);
    notes.push_back(notes[1]); // a copied note keeps its key until assigned
    model::assignNoteKeys(notes);
    CHECK(notes[1].key == 7); CHECK(notes[3].key == 10);

    const auto song = loadTwoTracks();
    for (const auto& track : song.tracks)
        for (const auto& clip : track.arrangementClips)
        {
            std::set<uint32_t> keys;
            for (const auto& n : clip.notes) { CHECK(n.key != 0); keys.insert(n.key); }
            CHECK(keys.size() == clip.notes.size());
        }
}

TEST_CASE("op: every op type survives the wire form and doubles round-trip exactly")
{
    const model::ClipRef clip{12, 1000123, -1, -1};
    model::NoteUpdate update; update.key = 3; update.pitch = 61; update.start = 1.0 / 3.0; update.enabled = false;
    const std::vector<model::Op> all{
        {model::ops::SetTempo{131.25}}, {model::ops::SetTrackGain{12, -6.5}}, {model::ops::RenameTrack{13, "Bass \xc3\xa9"}},
        {model::ops::SetTrackColor{{12, 13}, 7}}, {model::ops::SetLoop{true, 8, 16}}, {model::ops::MoveClip{12, 1000123, 13, 20}},
        {model::ops::DuplicateClip{12, 1000123, 16}}, {model::ops::DeleteClip{{12, 1000124, 0, -1}}},
        {model::ops::AddNotes{clip, {note(60, 0.5, 0.25, 90)}}}, {model::ops::UpdateNotes{clip, {update}}},
        {model::ops::DeleteNotes{{12, 1000123, -1, 4}, {1, 2}}}, {model::ops::ReplaceNotes{clip, {}}}};
    std::set<std::string> types;
    for (const auto& op : all)
    {
        roundTrip(op);
        types.insert(model::opType(op));
    }
    CHECK(types.size() == std::variant_size_v<model::OpPayload>);

    const double tricky[] = {0.1 + 0.2, 1.0 / 3.0, 123456.78901234567, 3.3e-7, 0.012345678901234567, 999999.99999999988, 7.0};
    model::ops::AddNotes add{clip, {}};
    for (const auto value : tricky) add.notes.push_back(note(60, value, value, 99.7f));
    const auto text = model::opMessageToString(model::opToJson({add}));
    const auto decoded = std::get<model::ops::AddNotes>(model::opFromJson(juce::JSON::parse(text)).payload);
    REQUIRE(decoded.notes.size() == std::size(tricky));
    for (size_t i = 0; i < decoded.notes.size(); ++i)
    {
        CHECK(juce::exactlyEqual(decoded.notes[i].start, tricky[i]));
        CHECK(juce::exactlyEqual(decoded.notes[i].duration, tricky[i]));
        CHECK(juce::exactlyEqual(decoded.notes[i].velocity, 99.7f));
    }

    const auto rejects = [](const char* json, const char* code) {
        try { model::opFromJson(juce::JSON::parse(json)); }
        catch (const model::OpError& error) { CHECK(error.code == code); return; }
        CHECK(false);
    };
    rejects(R"({"type":"tempo.set","bpm":120,"extra":1})", "invalid_request");
    rejects(R"({"type":"tempo.set"})", "invalid_request");
    rejects(R"({"type":"tempo.set","bpm":"fast"})", "invalid_request");
    rejects(R"({"type":"notes.update","trackId":1,"clipId":2,"updates":[{"noteKey":0,"pitch":3}]})", "invalid_request");
    rejects(R"({"type":"notes.update","trackId":1,"clipId":2,"updates":[{"noteIndex":0,"pitch":3}]})", "invalid_request");
    rejects(R"({"type":"track.delete","trackId":1})", "unsupported_operation");
    try { model::committedOpFromJson(juce::JSON::parse(R"({"v":2,"op":{"type":"tempo.set","bpm":120},"clipUids":[]})")); CHECK(false); }
    catch (const model::OpError& error) { CHECK(error.code == "unsupported_version"); }
}

TEST_CASE("op: concurrent edits of different notes in one clip both apply; edits of a deleted note are rejected")
{
    auto song = loadTwoTracks();
    model::EditHistory history;
    const auto& clip = song.tracks[0].arrangementClips[0];
    const model::ClipRef ref{song.tracks[0].id, clip.uid, -1, -1};
    REQUIRE(clip.notes.size() == 2);
    const uint32_t first = clip.notes[0].key, second = clip.notes[1].key;
    const double secondStart = clip.notes[1].start;

    // Both intents are prepared against the same base. A moves the first note after the second, which reorders the
    // array: an index-addressed B would now hit the wrong note, a key-addressed one does not.
    model::NoteUpdate moveFirst; moveFirst.key = first; moveFirst.start = secondStart + 1.0;
    model::NoteUpdate quietSecond; quietSecond.key = second; quietSecond.velocity = 20.0f;
    history.perform(song, model::opToCommand(song, {model::ops::UpdateNotes{ref, {moveFirst}}}));
    history.perform(song, model::opToCommand(song, {model::ops::UpdateNotes{ref, {quietSecond}}}));
    const auto& notes = song.tracks[0].arrangementClips[0].notes;
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].key == second); CHECK(juce::exactlyEqual(notes[0].velocity, 20.0f));
    CHECK(notes[1].key == first); CHECK_NEAR(notes[1].start, secondStart + 1.0, 1e-12);

    history.perform(song, model::opToCommand(song, {model::ops::AddNotes{ref, {note(72, 0, 1, 80)}}}));
    std::set<uint32_t> keys;
    for (const auto& n : song.tracks[0].arrangementClips[0].notes) keys.insert(n.key);
    CHECK(keys.size() == 3); CHECK(keys.count(first) == 1); CHECK(keys.count(second) == 1);

    history.perform(song, model::opToCommand(song, {model::ops::DeleteNotes{ref, {first}}}));
    model::NoteUpdate stale; stale.key = first; stale.pitch = 40;
    CHECK(errorCode(song, {model::ops::UpdateNotes{ref, {stale}}}) == "target_replaced");
    CHECK(errorCode(song, {model::ops::DeleteNotes{ref, {first}}}) == "target_replaced");
    CHECK(errorCode(song, {model::ops::DeleteNotes{ref, {second, second}}}) == "invalid_request");
    CHECK(errorCode(song, {model::ops::AddNotes{ref, {note(200, 0, 1, 80)}}}) == "invalid_request");
    CHECK(errorCode(song, {model::ops::SetTempo{5}}) == "invalid_request");
    CHECK(errorCode(song, {model::ops::DuplicateClip{song.tracks[0].id, 42, 0}}) == "target_replaced");
    CHECK(errorCode(song, {model::ops::RenameTrack{999999, "x"}}) == "target_replaced");
    CHECK(errorCode(song, {model::ops::DeleteClip{{ref.trackId, ref.clipUid, 0, 0}}}) == "invalid_request");
}

TEST_CASE("op: a replica fed only committed wire ops converges byte-for-byte with the host, through undo and redo")
{
    Peer host{loadTwoTracks(), {}};
    Peer replica{host.song, {}}; // stands in for the COL-2 join snapshot, which carries identities
    const model::TrackId synth = host.song.tracks[0].id, midi = host.song.tracks[1].id;
    const int clipUid = host.song.tracks[0].arrangementClips[0].uid;
    const model::ClipRef clip{synth, clipUid, -1, -1};

    commit(host, replica, {model::ops::SetTempo{131.5}});
    commit(host, replica, {model::ops::RenameTrack{midi, "Bass"}});
    commit(host, replica, {model::ops::SetTrackColor{{synth, midi}, 7}});
    commit(host, replica, {model::ops::SetLoop{true, 8, 16}});
    commit(host, replica, {model::ops::SetTrackGain{midi, -6.5}}); // the synth track's gain is automated
    const auto duplicated = commit(host, replica, {model::ops::DuplicateClip{synth, clipUid, 16}});
    REQUIRE(duplicated.clipUids.size() == 1);
    commit(host, replica, {model::ops::AddNotes{clip, {note(67, 1.0 / 3.0, 0.1 + 0.2, 99.7f), note(70, 2, 0.5, 64)}}});
    const auto keys = model::keyedNotes(host.song.tracks[0].arrangementClips[0]);
    model::NoteUpdate raise; raise.key = keys[0].key; raise.pitch = keys[0].pitch + 1;
    commit(host, replica, {model::ops::UpdateNotes{clip, {raise}}});
    commit(host, replica, {model::ops::DeleteNotes{clip, {keys[1].key}}});
    const auto moved = commit(host, replica, {model::ops::MoveClip{synth, duplicated.clipUids[0], midi, 20}});
    REQUIRE(moved.clipUids.size() == 1);
    int scene = -1;
    for (size_t i = 0; i < host.song.tracks[0].slots.size(); ++i)
        if (host.song.tracks[0].slots[i].clip) scene = static_cast<int>(i);
    REQUIRE(scene >= 0);
    const model::ClipRef sessionClip{synth, host.song.tracks[0].slots[(size_t) scene].clip->uid, scene, -1};
    commit(host, replica, {model::ops::ReplaceNotes{sessionClip, {note(48, 0, 4, 100), note(55, 0, 4, 100)}}});

    REQUIRE(host.song.tracks[1].arrangementClips.size() == 1);
    CHECK(host.song.tracks[1].arrangementClips[0].uid == moved.clipUids[0]);
    CHECK(replica.song.tracks[1].arrangementClips[0].uid == moved.clipUids[0]);
    const auto hostXml = patchedXml(host.song);
    CHECK(hostXml == patchedXml(replica.song));
    CHECK(hostXml != patchedXml(loadTwoTracks()));

    for (int i = 0; i < 4; ++i) { host.history.undo(host.song); replica.history.undo(replica.song); }
    CHECK(patchedXml(host.song) == patchedXml(replica.song));
    for (int i = 0; i < 4; ++i) { host.history.redo(host.song); replica.history.redo(replica.song); }
    CHECK(patchedXml(host.song) == hostXml);
    CHECK(patchedXml(replica.song) == hostXml);
}

TEST_CASE("op: replaying with the wrong identities is detected instead of silently diverging")
{
    auto song = loadTwoTracks();
    const model::Op duplicate{model::ops::DuplicateClip{song.tracks[0].id, song.tracks[0].arrangementClips[0].uid, 32}};
    auto matched = std::make_shared<bool>(true);
    auto command = model::replayOp(song, duplicate, {}, matched);
    command.apply(song);
    CHECK(! *matched);

    auto extra = std::make_shared<bool>(true);
    auto tempo = model::replayOp(song, {model::ops::SetTempo{100}}, {{4242}}, extra);
    tempo.apply(song);
    CHECK(! *extra);

    auto exact = std::make_shared<bool>(false);
    auto again = model::replayOp(song, duplicate, {{7777777}}, exact);
    again.apply(song);
    CHECK(*exact);
    CHECK(model::findArrangementClip(song, 0, 7777777) != nullptr);
    CHECK(model::allocateClipUid() > 7777777); // later local allocations never reuse a replayed identity
}
