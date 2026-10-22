#include "TestFramework.h"

#include "core/Edit.h"
#include <algorithm>

using namespace sonora::model;

namespace
{
Song makeSong()
{
    Song s;
    s.tempo = 120.0;
    Track t;
    t.id = 1;
    t.kind = TrackKind::Midi;
    for (int i = 0; i < 3; ++i)
    {
        Clip c;
        c.kind = Clip::Kind::Midi;
        c.uid = 100 + i;
        c.xmlId = 10 + i;
        c.currentStart = c.time = 4.0 * i;
        c.currentEnd = c.currentStart + 4.0;
        c.loop = {0.0, 4.0, 0.0, true};
        c.notes = {{0.0, 1.0, 60, 100.0f}};
        t.arrangementClips.push_back(c);
    }
    s.tracks.push_back(t);
    return s;
}
} // namespace

TEST_CASE("edit: moving a clip keeps its uid, re-sorts the track and undoes cleanly")
{
    Song s = makeSong();
    EditHistory h;
    const Clip* first = findArrangementClip(s, 0, 100);
    REQUIRE(first != nullptr);
    h.perform(s, modifyClip(s, 0, 100, movedClip(*first, 10.0), "Move Clip"));
    CHECK(h.canUndo());
    CHECK(h.undoLabel() == "Move Clip");
    // The moved clip (start 10) is now last in time order, still reachable by its uid.
    CHECK(s.tracks[0].arrangementClips.back().uid == 100);
    CHECK_NEAR(findArrangementClip(s, 0, 100)->currentStart, 10.0, 1e-9);
    CHECK_NEAR(findArrangementClip(s, 0, 100)->currentEnd, 14.0, 1e-9);

    const auto* undone = h.undo(s);
    REQUIRE(undone != nullptr);
    CHECK(undone->tracks == std::vector<int>{0});
    CHECK_NEAR(findArrangementClip(s, 0, 100)->currentStart, 0.0, 1e-9);
    CHECK(s.tracks[0].arrangementClips.front().uid == 100); // sorted back to the front
    CHECK(h.canRedo());
    h.redo(s);
    CHECK_NEAR(findArrangementClip(s, 0, 100)->currentStart, 10.0, 1e-9);
    CHECK(h.undo(s) != nullptr);
    CHECK(h.undo(s) == nullptr); // nothing left
}

TEST_CASE("edit: resize and trim")
{
    Song s = makeSong();
    const Clip c = *findArrangementClip(s, 0, 101); // 4..8
    CHECK_NEAR(withEnd(c, 6.0).currentEnd, 6.0, 1e-9);
    CHECK_NEAR(withEnd(c, 4.0).currentEnd, 4.0 + 0.0625, 1e-9); // never collapses to nothing
    const Clip trimmed = withStart(c, 5.0, 120.0);              // cut the first beat off
    CHECK_NEAR(trimmed.currentStart, 5.0, 1e-9);
    CHECK_NEAR(trimmed.currentEnd, 8.0, 1e-9);
    CHECK_NEAR(trimmed.loop.startRelative, 1.0, 1e-9);          // the content now starts one beat in
    // Unwarped audio keeps its loop in seconds: 1 beat at 120 BPM == 0.5 s.
    Clip audio = c;
    audio.kind = Clip::Kind::Audio;
    audio.warped = false;
    CHECK_NEAR(withStart(audio, 5.0, 120.0).loop.startRelative, 0.5, 1e-9);
}

TEST_CASE("edit: duplicate, delete and insert, with undo")
{
    Song s = makeSong();
    EditHistory h;
    h.perform(s, duplicateClip(s, 0, 102)); // clip at 8..12 -> copy at 12..16
    REQUIRE(s.tracks[0].arrangementClips.size() == 4);
    const Clip& copy = s.tracks[0].arrangementClips.back();
    CHECK_NEAR(copy.currentStart, 12.0, 1e-9);
    CHECK_NEAR(copy.currentEnd, 16.0, 1e-9);
    CHECK(copy.uid != 102);                       // a new identity
    CHECK(copy.xmlId == -1);                      // not in the file yet
    CHECK(copy.cloneOfXmlId == 12);               // the writer will clone clip element 12
    h.undo(s);
    CHECK(s.tracks[0].arrangementClips.size() == 3);
    h.redo(s);
    CHECK(s.tracks[0].arrangementClips.size() == 4);
    const int dupUid = s.tracks[0].arrangementClips.back().uid;

    h.perform(s, deleteClip(s, 0, 100));
    CHECK(s.tracks[0].arrangementClips.size() == 3);
    CHECK(findArrangementClip(s, 0, 100) == nullptr);
    h.undo(s);
    CHECK(findArrangementClip(s, 0, 100) != nullptr);
    CHECK(findArrangementClip(s, 0, dupUid) != nullptr);
}

TEST_CASE("edit: notes, tempo and loop edits")
{
    Song s = makeSong();
    EditHistory h;
    h.perform(s, setNotes(s, 0, 100, {{2.0, 0.5, 64, 90.0f}, {0.0, 1.0, 60, 100.0f}, {0.0, 1.0, 55, 100.0f}}));
    const auto& notes = findArrangementClip(s, 0, 100)->notes;
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].pitch == 55); // sorted by start then pitch
    CHECK(notes[2].start > 1.0);
    h.undo(s);
    CHECK(findArrangementClip(s, 0, 100)->notes.size() == 1);

    h.perform(s, setTempo(s, 140.0));
    CHECK_NEAR(s.tempo, 140.0, 1e-9);
    h.perform(s, setLoop(s, true, 8.0, 16.0));
    CHECK(s.loopOn);
    CHECK_NEAR(s.loopStart, 8.0, 1e-9);
    const int dirty = h.version();
    h.undo(s);
    CHECK(! s.loopOn);
    CHECK(h.version() > dirty); // undo also changes the document
    h.undo(s);
    CHECK_NEAR(s.tempo, 120.0, 1e-9);
}

TEST_CASE("edit: set tempo keeps Live's static tempo point in step, not real tempo automation")
{
    Song s = makeSong();
    s.main.mixer.tempoTargetId = 8;
    AutomationEnvelope tempo;
    tempo.targetId = 8;
    tempo.points = {{-63072000.0, 120.0}};
    s.main.automation = {tempo};
    EditHistory h;
    h.perform(s, setTempo(s, 145.0));
    CHECK_NEAR(s.main.automation[0].points[0].value, 145.0, 1e-9);
    h.undo(s);
    CHECK_NEAR(s.main.automation[0].points[0].value, 120.0, 1e-9);

    s.main.automation[0].points.push_back({16.0, 90.0}); // real automation: left alone
    h.perform(s, setTempo(s, 145.0));
    CHECK_NEAR(s.main.automation[0].points[0].value, 120.0, 1e-9);
}

TEST_CASE("edit: automation creation, stepped lanes, undo and redo on tracks, returns and main")
{
    Song s = makeSong();
    s.returns.emplace_back();
    EditHistory h;
    for (int track : {0, 1, 2})
    {
        AutomationEnvelope env;
        env.targetId = 42;
        env.points = {{8, 0}, {-63072000, 1}, {4, 1}};
        env.stepped = true;
        env.eventType = "BoolEvent";
        h.perform(s, setAutomation(s, track, env));
        REQUIRE(trackAt(s, track)->automation.size() == 1);
        CHECK(trackAt(s, track)->automation[0].points.front().time < 0);
        env.points = {{0, 0}};
        h.perform(s, setAutomation(s, track, env));
        CHECK(trackAt(s, track)->automation[0].points.size() == 1);
        h.undo(s);
        CHECK(trackAt(s, track)->automation[0].points.size() == 3);
        h.undo(s);
        CHECK(trackAt(s, track)->automation.empty());
        h.redo(s);
        CHECK(trackAt(s, track)->automation[0].eventType == "BoolEvent");
    }
}

TEST_CASE("edit: dirty state follows the saved history state across undo redo and branching")
{
    Song song = makeSong();
    EditHistory history;
    CHECK(! history.isDirty());
    history.perform(song, setTempo(song, 140));
    CHECK(history.isDirty());
    history.markSaved();
    CHECK(! history.isDirty());
    history.perform(song, setTempo(song, 150));
    CHECK(history.isDirty());
    history.undo(song);
    CHECK(! history.isDirty());
    history.redo(song);
    CHECK(history.isDirty());
    history.markSaved();
    history.undo(song);
    CHECK(history.isDirty());
    history.redo(song);
    CHECK(! history.isDirty());
    history.undo(song);
    history.perform(song, setTempo(song, 155));
    CHECK(history.isDirty()); // same depth as saved, different branch
    CHECK(! history.canRedo());
    history.clear();
    CHECK(! history.isDirty());
}

TEST_CASE("recording edit: replace a punched range, preserve clip tails/content and undo as one step")
{
    Song song = makeSong();
    song.tracks[0].arrangementClips.resize(1);
    auto& existing = song.tracks[0].arrangementClips[0];
    existing.currentStart = existing.time = 0;
    existing.currentEnd = 8;
    existing.loop = {0, 8, 0, false};
    Clip take;
    take.currentStart = take.time = 2;
    take.currentEnd = 6;
    take.notes = {{0, 1, 70, 100}};
    take.cloneOfXmlId = existing.xmlId;
    take.cloneOfTrackId = song.tracks[0].id;
    EditHistory history;
    history.perform(song, recordArrangementTake(song, 0, take));
    const auto& clips = song.tracks[0].arrangementClips;
    REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentEnd, 2, 1e-9);
    CHECK_NEAR(clips[1].currentStart, 2, 1e-9);
    CHECK_NEAR(clips[1].currentEnd, 6, 1e-9);
    CHECK_NEAR(clips[2].currentStart, 6, 1e-9);
    CHECK_NEAR(clips[2].currentEnd, 8, 1e-9);
    CHECK_NEAR(clips[2].loop.startRelative, 6, 1e-9);
    CHECK(clips[2].cloneOfXmlId == 10);
    history.undo(song);
    REQUIRE(clips.size() == 1);
    CHECK_NEAR(clips[0].currentEnd, 8, 1e-9);
    CHECK(! history.canUndo());
    history.redo(song);
    CHECK(clips.size() == 3);
    history.undo(song);
    take.currentStart = 0.001;
    take.currentEnd = 7.999;
    history.perform(song, recordArrangementTake(song, 0, take));
    REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentEnd, 0.001, 1e-9);
    CHECK_NEAR(clips[2].currentStart, 7.999, 1e-9);
    CHECK_NEAR(clips[2].loop.startRelative, 7.999, 1e-9);
}

TEST_CASE("recording replacement: equivalent floating-point boundaries do not leave microscopic fragments")
{
    auto song = makeSong();
    song.tracks[0].arrangementClips.resize(1);
    auto& existing = song.tracks[0].arrangementClips[0];
    existing.currentStart = 1; existing.currentEnd = 2;
    auto take = existing; take.currentStart = 1 + 4e-16; take.currentEnd = 2 - 4e-16;
    EditHistory history; history.perform(song, recordArrangementTake(song, 0, take));
    CHECK(song.tracks[0].arrangementClips.size() == 1);
    REQUIRE(history.undo(song)); CHECK(song.tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 1, 1e-12);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 2, 1e-12);
}

TEST_CASE("take editing: explicit addresses survive reordering and reject stale lanes and Session slots")
{
    Song song = makeSong(); EditHistory history;
    TakeLane lane; lane.name = "Pass"; lane.clips = {song.tracks[0].arrangementClips[0]};
    history.perform(song, appendTakeLanes(song, 0, {lane, lane}));
    const int id = song.tracks[0].takeLanes[0].id, uid = song.tracks[0].takeLanes[0].clips[0].uid;
    const auto address = ClipLocation::take(0, uid, id);
    REQUIRE(findClip(song, address)); CHECK(! findClip(song, ClipLocation::arrangement(0, uid)));
    history.perform(song, moveTakeLane(song, 0, id, 1));
    history.perform(song, setNotes(song, address, {{0, 1, 72, 80}}));
    CHECK(findClip(song, address)->notes[0].pitch == 72);
    CHECK(song.tracks[0].arrangementClips[0].notes[0].pitch == 60);
    CHECK(song.tracks[0].takeLanes[0].clips[0].notes[0].pitch == 60);
    const auto* undone = history.undo(song); REQUIRE(undone); CHECK(undone->takeLanesOnly);
    CHECK(findClip(song, address)->notes[0].pitch == 60);
    history.redo(song); history.perform(song, deleteTakeLane(song, 0, id));
    CHECK(! findClip(song, address));
    auto stale = modifyClip(song, address, lane.clips[0], "Stale"); stale.apply(song);
    CHECK(song.tracks[0].arrangementClips[0].notes[0].pitch == 60);
    history.undo(song); CHECK(findClip(song, address)->notes[0].pitch == 72);
    history.perform(song, setSessionClip(song, 0, 0, lane.clips[0]));
    const auto session = ClipLocation::session(0, song.tracks[0].slots[0].clip->uid, 0);
    history.perform(song, setSessionClip(song, 0, 0, lane.clips[0]));
    CHECK(! findClip(song, session)); CHECK(findClip(song, address));
}

TEST_CASE("take editing: comp ranges preserve source offsets, sides and redo identities")
{
    Song song = makeSong(); song.tracks[0].arrangementClips.resize(1);
    auto& main = song.tracks[0].arrangementClips[0]; main.currentEnd = 8; main.loop.loopEnd = 8;
    TakeLane lane; lane.name = "Alternative"; auto take = main;
    take.currentStart = 1; take.currentEnd = 6; take.loop = {10, 15, 0, false}; take.takeId = 17;
    take.notes = {{11, 2, 65, 90}}; lane.clips = {take};
    EditHistory history; history.perform(song, appendTakeLanes(song, 0, {lane}));
    const int id = song.tracks[0].takeLanes[0].id;
    history.perform(song, compTakeRange(song, 0, id, 2, 4));
    REQUIRE(song.tracks[0].arrangementClips.size() == 3);
    const auto comp = song.tracks[0].arrangementClips[1];
    CHECK_NEAR(comp.currentStart, 2, 1e-9); CHECK_NEAR(comp.currentEnd, 4, 1e-9);
    CHECK_NEAR(comp.loop.loopStart + comp.loop.startRelative, 11, 1e-9); CHECK(comp.takeId == 17);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 2, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[2].loop.startRelative, 4, 1e-9);
    history.undo(song); CHECK(song.tracks[0].arrangementClips.size() == 1);
    history.redo(song); CHECK(song.tracks[0].arrangementClips[1].uid == comp.uid);
    CHECK_NEAR(song.tracks[0].takeLanes[0].clips[0].currentStart, 1, 1e-9);
    auto audio = take; audio.kind = Clip::Kind::Audio; audio.warped = false; audio.loop = {3, 8, 0, false};
    song.tracks[0].takeLanes[0].clips = {audio};
    history.perform(song, compTakeRange(song, 0, id, 2, 3));
    CHECK_NEAR(song.tracks[0].arrangementClips[1].loop.startRelative, 0.5, 1e-9); // 1 beat at 120 BPM in seconds
    history.perform(song, duplicateTakeLane(song, 0, id));
    const auto duplicate = song.tracks[0].takeLanes.back();
    CHECK(duplicate.id != id); CHECK(duplicate.clips[0].uid != audio.uid); CHECK(duplicate.clips[0].takeId != 17);
    history.undo(song); history.redo(song);
    CHECK(song.tracks[0].takeLanes.back().clips[0].takeId == duplicate.clips[0].takeId);
}

#include "io/als/AlsReader.h"
#include <juce_core/juce_core.h>

using namespace sonora;

TEST_CASE("arrangement overdub: a MIDI take inside one clip merges into its notes, anything else replaces like a normal take")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song song = *loaded.song;
    model::EditHistory history;
    const auto& clips = song.tracks[0].arrangementClips;
    REQUIRE(clips.size() == 1);
    const double start = clips[0].currentStart, end = clips[0].currentEnd; // 4..12
    const size_t notesBefore = clips[0].notes.size();

    model::Clip take;
    take.kind = model::Clip::Kind::Midi;
    take.currentStart = take.time = start + 2.0;
    take.currentEnd = start + 4.0;
    take.loop = {0.0, 2.0, 0.0, false};
    take.notes.push_back({0.25, 0.5, 70, 100.0f});
    take.notes.push_back({1.5, 0.25, 72, 90.0f});

    history.perform(song, model::recordArrangementOverdub(song, 0, take));
    REQUIRE(song.tracks[0].arrangementClips.size() == 1); // merged, no extra clip, nothing trimmed
    const auto& merged = song.tracks[0].arrangementClips[0];
    CHECK_NEAR(merged.currentStart, start, 1e-9);
    CHECK_NEAR(merged.currentEnd, end, 1e-9);
    REQUIRE(merged.notes.size() == notesBefore + 2);
    bool a = false, b = false;
    const double base = merged.loop.loopStart + merged.loop.startRelative;
    for (const auto& n : merged.notes)
    {
        if (n.pitch == 70 && std::fabs(n.start - (base + 2.25)) < 1e-9) a = true;
        if (n.pitch == 72 && std::fabs(n.start - (base + 3.5)) < 1e-9) b = true;
        CHECK(n.noteId == -1 || n.pitch == 65 || n.pitch == 60); // new notes get ids when saved
    }
    CHECK(a);
    CHECK(b);
    history.undo(song);
    CHECK(song.tracks[0].arrangementClips[0].notes.size() == notesBefore);

    // a take that starts in empty space, or crosses the clip's end, is a normal replacing take
    model::Clip outside = take;
    outside.currentStart = outside.time = end + 4.0;
    outside.currentEnd = end + 8.0;
    history.perform(song, model::recordArrangementOverdub(song, 0, outside));
    CHECK(song.tracks[0].arrangementClips.size() == 2);
    history.undo(song);
    model::Clip crossing = take;
    crossing.currentStart = crossing.time = end - 1.0;
    crossing.currentEnd = end + 1.0;
    history.perform(song, model::recordArrangementOverdub(song, 0, crossing));
    CHECK(song.tracks[0].arrangementClips.size() == 2); // the old clip was trimmed, the take stands next to it
    history.undo(song);

    // controller envelopes in the take also fall back to replacement (they are not merged)
    model::Clip withController = take;
    withController.envelopes.push_back({});
    history.perform(song, model::recordArrangementOverdub(song, 0, withController));
    CHECK(song.tracks[0].arrangementClips.size() >= 2);
}

#include "core/DeviceCatalog.h"

namespace
{
Device chainDevice(int id, const char* type)
{
    Device d; d.id = id; d.type = type; d.displayName = type; return d;
}
Device chainPlugin(int id, bool instrument)
{
    auto d = chainDevice(id, "PluginDevice"); d.kind = DeviceKind::PluginVst3;
    d.plugin.emplace(); d.plugin->name = instrument ? "Serum 2" : "Serum 2 FX";
    d.plugin->isInstrument = instrument; d.plugin->state = {(uint8_t) id, 77};
    return d;
}
std::vector<int> deviceIds(const Track& t)
{
    std::vector<int> ids; for (const auto& d : t.devices) ids.push_back(d.id); return ids;
}
}

TEST_CASE("device chain policy: one instrument with atomic replacement and retained live state across undo redo")
{
    Song song; Track track; track.kind = TrackKind::Midi;
    track.devices = {chainDevice(1, "MidiPitcher"), chainPlugin(2, true), chainDevice(3, "Delay"), chainPlugin(4, false)};
    song.tracks.push_back(track); EditHistory history;
    CHECK(deviceRole(track.devices[1]) == DeviceRole::Instrument);
    CHECK(deviceRole(track.devices[3]) == DeviceRole::AudioEffect);
    auto command = insertTrackDevice(song, 0, chainPlugin(5, true));
    // Simulate capture after command creation, before apply, as the app's structural edit lifecycle does.
    song.tracks[0].devices[1].plugin->state = {91, 92, 93};
    history.perform(song, std::move(command));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 5, 3, 4}));
    CHECK(history.undoLabel() == "Replace Instrument");
    song.tracks[0].devices[1].plugin->state = {80, 81};
    REQUIRE(history.undo(song));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 2, 3, 4}));
    CHECK(song.tracks[0].devices[1].plugin->state == std::vector<uint8_t>({91, 92, 93}));
    REQUIRE(history.redo(song));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 5, 3, 4}));
    CHECK(song.tracks[0].devices[1].plugin->state == std::vector<uint8_t>({80, 81}));
    REQUIRE(history.undo(song)); CHECK(! history.canUndo());
}

TEST_CASE("device chain policy: FX version retains empty instrument slot and instruments insert between MIDI and audio effects")
{
    Song song; Track track; track.kind = TrackKind::Midi;
    track.devices = {chainDevice(1, "MidiPitcher"), chainPlugin(2, false), chainDevice(3, "Delay")};
    song.tracks.push_back(track); EditHistory history;
    CHECK(instrumentIndex(song.tracks[0]) == -1);
    history.perform(song, insertTrackDevice(song, 0, chainPlugin(4, true)));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 4, 2, 3}));
    CHECK(instrumentIndex(song.tracks[0]) == 1);
    history.perform(song, insertTrackDevice(song, 0, chainDevice(5, "Reverb")));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 4, 2, 3, 5}));
    REQUIRE(history.undo(song)); REQUIRE(history.undo(song));
    CHECK(instrumentIndex(song.tracks[0]) == -1);
    song.tracks[0].kind = TrackKind::Audio;
    CHECK(! insertTrackDevice(song, 0, chainPlugin(4, true)).apply);
    CHECK(! insertTrackDevice(song, 0, chainDevice(9, "MidiPitcher")).apply);
}

TEST_CASE("device chain policy: audio effect reorder preserves identity state and instrument boundary")
{
    Song song; Track track; track.kind = TrackKind::Midi;
    track.devices = {chainDevice(1, "MidiPitcher"), chainPlugin(2, true), chainPlugin(3, false), chainDevice(4, "Delay"), chainDevice(5, "Reverb")};
    song.tracks.push_back(track); EditHistory history;
    CHECK(! canMoveAudioEffect(song, 0, 5, 0)); CHECK(! canMoveAudioEffect(song, 0, 5, 1));
    CHECK(! canMoveAudioEffect(song, 0, 2, 3)); CHECK(! canMoveAudioEffect(song, 0, 1, 3));
    CHECK(! canMoveAudioEffect(song, 0, 5, 4)); CHECK(! canMoveAudioEffect(song, 0, 999, 2));
    CHECK(! canMoveAudioEffect(song, 0, 5, -1)); CHECK(! canMoveAudioEffect(song, 0, 5, 5));
    auto command = moveAudioEffect(song, 0, 5, 2); REQUIRE(command.apply);
    CHECK(command.devicesChanged); history.perform(song, std::move(command));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 2, 5, 3, 4}));
    song.tracks[0].devices[3].plugin->state = {9, 8, 7};
    REQUIRE(history.undo(song));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 2, 3, 4, 5}));
    CHECK(song.tracks[0].devices[2].plugin->state == std::vector<uint8_t>({9, 8, 7}));
    REQUIRE(history.redo(song));
    CHECK(song.tracks[0].devices[3].plugin->state == std::vector<uint8_t>({9, 8, 7}));
    history.perform(song, moveAudioEffect(song, 0, 5, 4));
    CHECK(deviceIds(song.tracks[0]) == std::vector<int>({1, 2, 3, 4, 5}));
    song.tracks[0].kind = TrackKind::Audio; CHECK(canMoveAudioEffect(song, 0, 5, 0));
    song.returns.push_back(song.tracks[0]); CHECK(canMoveAudioEffect(song, 1, 5, 0));
    song.main = song.tracks[0]; CHECK(canMoveAudioEffect(song, 2, 5, 0));
}

TEST_CASE("duplicate region: intersects source clips, overwrites destination, preserves tails and one-step undo")
{
    Song song = makeSong();
    auto source = song.tracks[0].arrangementClips[0];
    source.currentEnd = 12;
    song.tracks[0].arrangementClips = {source};
    Track untouched = song.tracks[0]; untouched.id = 2; song.tracks.push_back(untouched);
    EditHistory history;
    history.perform(song, duplicateArrangementRegion(song, 0, 0, 2, 4));
    const auto& clips = song.tracks[0].arrangementClips;
    REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentStart, 0, 1e-9); CHECK_NEAR(clips[0].currentEnd, 4, 1e-9);
    CHECK_NEAR(clips[1].currentStart, 4, 1e-9); CHECK_NEAR(clips[1].currentEnd, 6, 1e-9);
    CHECK_NEAR(clips[1].loop.startRelative, 2, 1e-9);
    CHECK_NEAR(clips[2].currentStart, 6, 1e-9); CHECK_NEAR(clips[2].currentEnd, 12, 1e-9);
    CHECK_NEAR(clips[2].loop.startRelative, 6, 1e-9);
    CHECK(clips[0].uid != clips[1].uid); CHECK(clips[1].uid != clips[2].uid);
    CHECK(song.tracks[1].arrangementClips.size() == 1);
    const int copyUid = clips[1].uid;
    REQUIRE(history.undo(song)); CHECK(! history.canUndo());
    REQUIRE(song.tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentEnd, 12, 1e-9);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].arrangementClips[1].uid == copyUid);
}

TEST_CASE("duplicate region: gaps replace destination and unwarped content offsets stay in seconds")
{
    Song song = makeSong(); song.tempo = 120;
    auto source = song.tracks[0].arrangementClips[0];
    source.kind = Clip::Kind::Audio; source.warped = false;
    source.currentStart = 1; source.currentEnd = 2; source.loop.startRelative = 3;
    auto destination = source; destination.uid = 200; destination.currentStart = 4; destination.currentEnd = 8;
    song.tracks[0].arrangementClips = {source, destination};
    EditHistory history;
    history.perform(song, duplicateArrangementRegion(song, 0, 0, 0, 4));
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    const auto& copy = song.tracks[0].arrangementClips[1];
    CHECK_NEAR(copy.currentStart, 5, 1e-9); CHECK_NEAR(copy.currentEnd, 6, 1e-9);
    CHECK_NEAR(copy.loop.startRelative, 3, 1e-9);
    history.undo(song);
    song.tracks[0].arrangementClips = {source};
    history.perform(song, duplicateArrangementRegion(song, 0, 0, 1.5, 2));
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].loop.startRelative, 3.25, 1e-9);
}

TEST_CASE("duplicate region: multiple automation lanes retain source and destination boundary jumps")
{
    Song song = makeSong();
    AutomationEnvelope first; first.targetId = 20; first.points = {{0, 0}, {8, 8}};
    auto second = first; second.targetId = 21; second.stepped = true;
    auto manual = first; manual.targetId = 22; manual.points = {{-63072000, 0.75}};
    song.tracks[0].automation = {first, second, manual};
    EditHistory history;
    history.perform(song, duplicateArrangementRegion(song, 0, 0, 2, 4));
    REQUIRE(song.tracks[0].automation.size() == 3);
    CHECK(song.tracks[0].automation[2].points.size() == 1);
    const auto& points = song.tracks[0].automation[0].points;
    REQUIRE(points.size() == 6);
    CHECK_NEAR(points[1].time, 4, 1e-9); CHECK_NEAR(points[1].value, 4, 1e-9);
    CHECK_NEAR(points[2].time, 4, 1e-9); CHECK_NEAR(points[2].value, 2, 1e-9);
    CHECK_NEAR(points[3].time, 6, 1e-9); CHECK_NEAR(points[3].value, 4, 1e-9);
    CHECK_NEAR(points[4].time, 6, 1e-9); CHECK_NEAR(points[4].value, 6, 1e-9);
    CHECK(song.tracks[0].automation[1].points.size() == 6);
    REQUIRE(history.undo(song)); CHECK(song.tracks[0].automation[0].points.size() == 2);
    CHECK(song.tracks[0].automation[1].points.size() == 2);
}

namespace
{
Clip regionTestClip(int uid, double start, double end, Clip::Kind kind = Clip::Kind::Midi)
{
    Clip clip; clip.uid = uid; clip.xmlId = uid; clip.name = std::to_string(uid); clip.kind = kind;
    clip.currentStart = clip.time = start; clip.currentEnd = end; clip.loop = {0, 32, 0, false};
    clip.notes = {{0, 0.5, 60, 90}};
    return clip;
}
}

TEST_CASE("move region: multiple clips and mixed channels keep spacing gaps and partial source tails with one undo")
{
    Song song; song.tempo = 120; song.tracks.resize(3);
    for (int i = 0; i < 3; ++i) { song.tracks[(size_t) i].id = i; song.tracks[(size_t) i].kind = TrackKind::Midi; }
    song.tracks[0].arrangementClips = {regionTestClip(701, 0, 3), regionTestClip(702, 4, 5), regionTestClip(703, 6, 10)};
    song.tracks[1].kind = TrackKind::Audio;
    auto audio = regionTestClip(704, 1, 8, Clip::Kind::Audio); audio.warped = false; audio.loop.startRelative = 2;
    song.tracks[1].arrangementClips = {audio};
    song.tracks[2].arrangementClips = {regionTestClip(705, 0, 30)};
    EditHistory history;
    auto command = transferArrangementRegion(song, 0, 1, 2, 7, 12); REQUIRE(command.apply);
    CHECK(command.tracks == std::vector<int>({0, 1})); history.perform(song, std::move(command));
    const auto& clips = song.tracks[0].arrangementClips;
    REQUIRE(clips.size() == 5);
    CHECK_NEAR(clips[0].currentStart, 0, 1e-9); CHECK_NEAR(clips[0].currentEnd, 2, 1e-9);
    CHECK_NEAR(clips[1].currentStart, 7, 1e-9); CHECK_NEAR(clips[1].loop.startRelative, 1, 1e-9);
    CHECK_NEAR(clips[2].currentStart, 12, 1e-9); CHECK_NEAR(clips[2].currentEnd, 13, 1e-9);
    CHECK_NEAR(clips[2].loop.startRelative, 2, 1e-9);
    CHECK_NEAR(clips[3].currentStart, 14, 1e-9); CHECK_NEAR(clips[3].currentEnd, 15, 1e-9); CHECK(clips[3].uid == 702);
    CHECK_NEAR(clips[4].currentStart, 16, 1e-9); CHECK_NEAR(clips[4].currentEnd, 17, 1e-9);
    const auto& wave = song.tracks[1].arrangementClips;
    REQUIRE(wave.size() == 3); CHECK_NEAR(wave[2].currentStart, 12, 1e-9); CHECK_NEAR(wave[2].currentEnd, 17, 1e-9);
    CHECK_NEAR(wave[2].loop.startRelative, 2.5, 1e-9); CHECK_NEAR(wave[1].loop.startRelative, 5, 1e-9);
    CHECK(song.tracks[2].arrangementClips[0].uid == 705); CHECK_NEAR(song.tracks[2].arrangementClips[0].currentStart, 0, 1e-9);
    CHECK(history.undoLabel() == "Move Region"); REQUIRE(history.undo(song)); CHECK(! history.canUndo());
    REQUIRE(song.tracks[0].arrangementClips.size() == 3); CHECK(song.tracks[0].arrangementClips[1].uid == 702);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].arrangementClips[3].uid == 702);
}

TEST_CASE("move region: overlapping destination replaces gaps without erasing its own moved material")
{
    Song song; song.tracks.resize(1); song.tracks[0].kind = TrackKind::Midi;
    song.tracks[0].arrangementClips = {regionTestClip(710, 1, 2), regionTestClip(711, 3, 4), regionTestClip(712, 5, 8)};
    EditHistory history; auto command = transferArrangementRegion(song, 0, 0, 0, 4, 2); REQUIRE(command.apply);
    history.perform(song, std::move(command));
    const auto& clips = song.tracks[0].arrangementClips; REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentStart, 3, 1e-9); CHECK(clips[0].uid == 710);
    CHECK_NEAR(clips[1].currentStart, 5, 1e-9); CHECK(clips[1].uid == 711);
    CHECK_NEAR(clips[2].currentStart, 6, 1e-9); CHECK_NEAR(clips[2].currentEnd, 8, 1e-9);
    CHECK_NEAR(clips[2].loop.startRelative, 1, 1e-9);
    REQUIRE(history.undo(song));
    history.perform(song, transferArrangementRegion(song, 0, 0, 3, 7, 0));
    REQUIRE(song.tracks[0].arrangementClips.size() == 3);
    CHECK_NEAR(song.tracks[0].arrangementClips[0].currentStart, 0, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, 2, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[2].currentStart, 7, 1e-9);
}

TEST_CASE("move region: vertical overlap stages all tracks first and incompatible destinations reject the entire block")
{
    Song song; song.tracks.resize(4);
    for (int i = 0; i < 4; ++i)
    {
        song.tracks[(size_t) i].id = i; song.tracks[(size_t) i].kind = TrackKind::Midi;
        song.tracks[(size_t) i].arrangementClips = {regionTestClip(720 + i, 0, 4)};
    }
    EditHistory history; history.perform(song, transferArrangementRegion(song, 0, 1, 0, 4, 0, 1));
    CHECK(song.tracks[0].arrangementClips.empty()); REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK(song.tracks[1].arrangementClips[0].uid == 720); CHECK(song.tracks[2].arrangementClips[0].uid == 721);
    CHECK(song.tracks[3].arrangementClips[0].uid == 723);
    REQUIRE(history.undo(song));
    song.tracks[2].kind = TrackKind::Audio;
    CHECK(! canTransferArrangementRegion(song, 0, 1, 0, 4, 1, 1));
    CHECK(! transferArrangementRegion(song, 0, 1, 0, 4, 1, 1).apply);
    CHECK(! transferArrangementRegion(song, 0, 1, 0, 4, -1).apply);
    CHECK(! transferArrangementRegion(song, 0, 1, 0, 4, 2, -1).apply);
    CHECK(! transferArrangementRegion(song, 0, 1, 0, 4, 2, 3).apply);
    CHECK(! transferArrangementRegion(song, 0, 1, 0, 4, 0).apply);
    CHECK(song.tracks[0].arrangementClips[0].uid == 720);
}

TEST_CASE("move region: automation follows the range through overlap and target remapping while static values stay static")
{
    Song song; song.tracks.resize(2);
    for (int i = 0; i < 2; ++i)
    { song.tracks[(size_t) i].kind = TrackKind::Audio; song.tracks[(size_t) i].mixer.volumeTargetId = 100 + i; }
    AutomationEnvelope envelope; envelope.targetId = 100; envelope.points = {{-63072000, 0}, {0, 0}, {4, 4}, {8, 8}, {12, 12}};
    auto manual = envelope; manual.targetId = 500; manual.points = {{-63072000, 0.75}};
    song.tracks[0].automation = {envelope, manual};
    EditHistory history; history.perform(song, transferArrangementRegion(song, 0, 0, 2, 6, 4));
    auto at = [](const AutomationEnvelope& lane, double beat) {
        const auto next = std::upper_bound(lane.points.begin(), lane.points.end(), beat, [](double time, const auto& p) { return time < p.time; });
        if (next == lane.points.begin()) return next->value;
        const auto& prev = *std::prev(next);
        return next == lane.points.end() ? prev.value : prev.value + (next->value - prev.value) * (beat - prev.time) / (next->time - prev.time);
    };
    CHECK_NEAR(at(song.tracks[0].automation[0], 1), 1, 1e-8);
    CHECK_NEAR(at(song.tracks[0].automation[0], 3), 2, 1e-8); // cleared source holds its edge value
    CHECK_NEAR(at(song.tracks[0].automation[0], 5), 3, 1e-8);
    CHECK_NEAR(at(song.tracks[0].automation[0], 7), 5, 1e-8);
    CHECK_NEAR(at(song.tracks[0].automation[0], 9), 9, 1e-8);
    CHECK(song.tracks[0].automation[1].points.size() == 1);
    REQUIRE(history.undo(song));
    auto command = transferArrangementRegion(song, 0, 0, 2, 6, 10, 1); REQUIRE(command.apply);
    history.perform(song, std::move(command)); REQUIRE(song.tracks[1].automation.size() == 1);
    CHECK(song.tracks[1].automation[0].targetId == 101); CHECK_NEAR(at(song.tracks[1].automation[0], 11), 3, 1e-8);
    REQUIRE(history.undo(song));
    song.tracks[1].mixer.volumeTargetId = -1;
    CHECK(! transferArrangementRegion(song, 0, 0, 2, 6, 10, 1).apply);
}

TEST_CASE("move region: Option copying keeps sources and allocates independent destination identities")
{
    Song song; song.tracks.resize(2);
    for (auto& track : song.tracks) track.kind = TrackKind::Midi;
    song.tracks[0].arrangementClips = {regionTestClip(740, 0, 2)};
    song.tracks[1].arrangementClips = {regionTestClip(741, 1, 3)};
    EditHistory history; history.perform(song, transferArrangementRegion(song, 0, 1, 0, 4, 8, 0, true));
    CHECK(history.undoLabel() == "Copy Region"); REQUIRE(song.tracks[0].arrangementClips.size() == 2);
    CHECK(song.tracks[0].arrangementClips[0].uid == 740); CHECK(song.tracks[0].arrangementClips[1].uid != 740);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, 8, 1e-9);
    CHECK_NEAR(song.tracks[1].arrangementClips[1].currentStart, 9, 1e-9);
    REQUIRE(history.undo(song)); CHECK(song.tracks[0].arrangementClips.size() == 1);
}

TEST_CASE("move region: cross-channel clip envelopes rebind controllers and native parameters or reject the whole drop")
{
    Song song; song.tracks.resize(2);
    for (int i = 0; i < 2; ++i)
    {
        auto& track = song.tracks[(size_t) i]; track.kind = TrackKind::Midi;
        track.midiControllerTargets[3] = 201 + i;
        Device device; device.type = "Delay";
        Parameter parameter; parameter.path = "Feedback"; parameter.value = 0.5; parameter.automationTargetId = 301 + i;
        device.parameters.push_back(parameter); track.devices.push_back(device);
    }
    auto clip = regionTestClip(750, 0, 2);
    AutomationEnvelope controller; controller.targetId = 201; controller.midiController = 3; controller.points = {{0, 20}, {1, 100}};
    auto parameter = controller; parameter.midiController = -1; parameter.targetId = 301;
    clip.envelopes = {controller, parameter}; song.tracks[0].arrangementClips = {clip};
    EditHistory history; auto command = transferArrangementRegion(song, 0, 0, 0, 2, 4, 1); REQUIRE(command.apply);
    history.perform(song, std::move(command)); REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK(song.tracks[1].arrangementClips[0].envelopes[0].targetId == 202);
    CHECK(song.tracks[1].arrangementClips[0].envelopes[0].midiController == 3);
    CHECK(song.tracks[1].arrangementClips[0].envelopes[1].targetId == 302);
    REQUIRE(history.undo(song)); song.tracks[1].devices[0].parameters[0].automationTargetId = -1;
    CHECK(! transferArrangementRegion(song, 0, 0, 0, 2, 4, 1).apply);
    CHECK(song.tracks[0].arrangementClips[0].envelopes[0].targetId == 201);
}

TEST_CASE("track move: nested subtrees and multiple roots reorder atomically with routing and history")
{
    Song song;
    for (int i = 0; i < 6; ++i) { Track t; t.id = 20 + i; t.kind = TrackKind::Audio; t.outputRouting = "AudioOut/Main"; song.tracks.push_back(t); }
    song.tracks[0].kind = song.tracks[2].kind = TrackKind::Group;
    song.tracks[1].groupId = song.tracks[2].groupId = 20; song.tracks[3].groupId = 22;
    song.tracks[0].folded = true;
    EditHistory history;
    auto command = moveTracks(song, {4, 5}, 1, 20); REQUIRE(command.apply); CHECK(command.structureChanged);
    history.perform(song, std::move(command));
    CHECK(song.tracks[1].id == 24); CHECK(song.tracks[2].id == 25);
    CHECK(song.tracks[1].groupId == 20); CHECK(song.tracks[2].outputRouting == "AudioOut/GroupTrack"); CHECK(! song.tracks[0].folded);
    REQUIRE(history.undo(song)); CHECK(song.tracks[4].id == 24); CHECK(song.tracks[4].groupId == -1); CHECK(song.tracks[0].folded);
    history.perform(song, moveTracks(song, {2}, 6));
    CHECK(song.tracks[4].id == 22); CHECK(song.tracks[5].id == 23); CHECK(song.tracks[4].groupId == -1); CHECK(song.tracks[5].groupId == 22);
    REQUIRE(history.undo(song)); REQUIRE(history.redo(song)); CHECK(song.tracks[5].id == 23);
}

TEST_CASE("track move: cycles broken trees invalid targets and identical drops never produce an edit")
{
    Song song;
    for (int i = 0; i < 4; ++i) { Track t; t.id = i; t.kind = i < 2 ? TrackKind::Group : TrackKind::Midi; song.tracks.push_back(t); }
    song.tracks[1].groupId = 0; song.tracks[2].groupId = 1;
    CHECK(! moveTracks(song, {0}, 2, 1).apply);
    CHECK(! moveTracks(song, {1}, 3, 2).apply);
    CHECK(! moveTracks(song, {3}, 1, -1).apply); // would split the outer group's subtree
    CHECK(! moveTracks(song, {2}, 2, 1).apply);
    CHECK(! moveTracks(song, {4}, 0).apply); CHECK(! moveTracks(song, {3}, 99).apply);
    CHECK(! moveTracks(song, {3}, 0, 1000).apply);
    auto command = moveTracks(song, {3}, 3, 1); REQUIRE(command.apply);
    command.apply(song); CHECK(song.tracks[3].groupId == 1);
}
