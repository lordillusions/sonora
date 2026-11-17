// UI-93: multi-clip note editing, the pure part (core/MultiClipNotes): which clips a time selection shows, merging their
// notes into Arrangement time and splitting an edited list back into one undoable command.
#include "TestFramework.h"
#include "core/Edit.h"
#include "core/MultiClipNotes.h"

#include <cmath>

using namespace sonora;

namespace
{
model::Clip clip(int uid, double start, double end, model::LoopSettings loop, std::vector<model::MidiNote> notes)
{
    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.uid = uid;
    c.currentStart = c.time = start;
    c.currentEnd = end;
    c.loop = loop;
    c.notes = std::move(notes);
    model::assignNoteKeys(c.notes);
    return c;
}

model::Song song()
{
    model::Song s;
    s.tempo = 120;
    for (int t = 0; t < 3; ++t)
    {
        model::Track track;
        track.kind = t == 2 ? model::TrackKind::Audio : model::TrackKind::Midi;
        s.tracks.push_back(track);
    }
    // Track 0: a clip at 0..8 playing content 0..8, another at 16..20 whose start marker is content beat 4 (unlooped 4..8).
    s.tracks[0].arrangementClips = {clip(1, 0, 8, {0, 8, 0, false}, {{1.0, 0.5, 60, 100.0f}, {2.0, 0.5, 62, 100.0f}}),
                                    clip(2, 16, 20, {4, 8, 0, false}, {{5.0, 1.0, 64, 90.0f}})};
    // Track 1: a looped clip at 4..12 with the start marker at loop start 0.
    s.tracks[1].arrangementClips = {clip(3, 4, 12, {0, 4, 0, true}, {{0.5, 0.25, 48, 80.0f}})};
    return s;
}
} // namespace

TEST_CASE("multi-clip notes: the time selection picks MIDI clips by track then time, at most eight tracks")
{
    const auto s = song();
    auto members = model::midiClipsInSelection(s, 0, 2, 0.0, 24.0);
    REQUIRE(members.size() == 3);
    CHECK(members[0].location.track == 0); CHECK(members[0].location.uid == 1);
    CHECK(members[1].location.track == 0); CHECK(members[1].location.uid == 2);
    CHECK(members[2].location.track == 1); CHECK(members[2].location.uid == 3);
    CHECK_NEAR(members[1].offset, 12.0, 1e-9); // content 4 sits at Arrangement 16
    CHECK_NEAR(members[2].offset, 4.0, 1e-9);
    // Overlap only: a selection 12..16 touches no clip; 6..17 touches all three.
    CHECK(model::midiClipsInSelection(s, 0, 1, 12.0, 16.0).empty());
    CHECK(model::midiClipsInSelection(s, 0, 1, 6.0, 17.0).size() == 3);
    CHECK(model::midiClipsInSelection(s, 1, 1, 0.0, 24.0).size() == 1);

    model::Song wide;
    for (int t = 0; t < 10; ++t)
    {
        model::Track track; track.kind = model::TrackKind::Midi;
        track.arrangementClips = {clip(100 + t, 0, 4, {0, 4, 0, true}, {})};
        wide.tracks.push_back(track);
    }
    const auto eight = model::midiClipsInSelection(wide, 0, 9, 0.0, 4.0);
    REQUIRE(eight.size() == 8);
    CHECK(eight.back().location.track == 7);
}

TEST_CASE("multi-clip notes: merged notes are in Arrangement time and edits split back as one undo step")
{
    auto s = song();
    const auto members = model::midiClipsInSelection(s, 0, 1, 0.0, 24.0);
    const auto merged = model::mergeNotes(s, members);
    REQUIRE(merged.notes.size() == 4);
    CHECK_NEAR(merged.notes[0].start, 1.0, 1e-9);
    CHECK_NEAR(merged.notes[2].start, 17.0, 1e-9); // clip 2's content 5 = Arrangement 17
    CHECK_NEAR(merged.notes[3].start, 4.5, 1e-9);  // clip 3's content 0.5 = Arrangement 4.5
    CHECK(merged.origin[3].member == 2);

    // Transpose clip 1's first note, move clip 3's note a beat later, add a note at 18 (inside clip 2) and one at 13
    // (no clip there: the nearest clip on the focused track, clip 2 at 16..20).
    auto edited = merged.notes;
    edited[0].pitch = 61;
    edited[3].start += 1.0;
    model::MidiNote added; added.start = 18.0; added.pitch = 70; edited.push_back(added);
    model::MidiNote gap; gap.start = 13.0; gap.pitch = 71; edited.push_back(gap);
    auto command = model::setMergedNotes(s, members, merged, edited, 0, "Edit Notes");
    REQUIRE((bool) command.apply);
    CHECK(command.label == "Edit Notes");
    model::EditHistory history;
    history.perform(s, std::move(command));
    const auto& a = s.tracks[0].arrangementClips[0].notes;
    const auto& b = s.tracks[0].arrangementClips[1].notes;
    const auto& c = s.tracks[1].arrangementClips[0].notes;
    REQUIRE(a.size() == 2); REQUIRE(b.size() == 3); REQUIRE(c.size() == 1);
    CHECK(a[0].pitch == 61);
    CHECK_NEAR(a[1].start, 2.0, 1e-12);       // untouched notes keep their stored start exactly
    CHECK_NEAR(c[0].start, 1.5, 1e-9);        // back in the clip's content time
    // clip 2 holds content 1 (from Arrangement 13), 5 (its own note) and 6 (from Arrangement 18), sorted by start.
    CHECK_NEAR(b[0].start, 1.0, 1e-9); CHECK(b[0].pitch == 71);
    CHECK_NEAR(b[1].start, 5.0, 1e-12); CHECK(b[1].pitch == 64);
    CHECK_NEAR(b[2].start, 6.0, 1e-9); CHECK(b[2].pitch == 70);
    CHECK(a[0].key != 0); CHECK(b[0].key != 0);

    history.undo(s); // one step restores all three clips
    CHECK(s.tracks[0].arrangementClips[0].notes[0].pitch == 60);
    CHECK(s.tracks[0].arrangementClips[1].notes.size() == 1);
    CHECK_NEAR(s.tracks[1].arrangementClips[0].notes[0].start, 0.5, 1e-9);

    // Nothing changed: no command; one clip changed: only that clip's track is refreshed.
    CHECK(! model::setMergedNotes(s, members, merged, merged.notes, 0, "Edit Notes").apply);
    auto one = merged.notes;
    one[3].velocity = 20.0f;
    const auto single = model::setMergedNotes(s, members, merged, one, 0, "Change Velocity");
    REQUIRE((bool) single.apply);
    REQUIRE(single.tracks.size() == 1);
    CHECK(single.tracks[0] == 1);
}

TEST_CASE("multi-clip notes: a new note goes to the clip under it, the focused clip first")
{
    const auto s = song();
    const auto members = model::midiClipsInSelection(s, 0, 1, 0.0, 24.0);
    const auto merged = model::mergeNotes(s, members);
    model::MidiNote n;
    n.start = 5.0; // inside clip 1 (track 0, 0..8) and clip 3 (track 1, 4..12)
    CHECK(model::memberOfNote(merged, members, n, 2) == 2); // focused clip 3 wins
    CHECK(model::memberOfNote(merged, members, n, 1) == 0); // focus on clip 2 (track 0): clip 1 on the same track
    n.start = 10.0; // only clip 3
    CHECK(model::memberOfNote(merged, members, n, 0) == 2);
    auto existing = merged.notes[2]; // keeps its clip wherever it moves
    existing.start = 1.0;
    CHECK(model::memberOfNote(merged, members, existing, 0) == 1);
}
