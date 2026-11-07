// UI-71: clip geometry like the reference app (docs/CLIP_EDITING.md): marker storage, edge drags, magnetic snapping and
// overwriting placement. Pure core functions; the gestures that use them are in test_clip_editing_ui.cpp.
#include "TestFramework.h"

#include "core/ClipGeometry.h"
#include "core/Edit.h"
#include "core/TimeUtils.h"

#include <cmath>
#include <memory>
#include <string>

using namespace sonora::model;

namespace
{
Clip midiClip(double start, double end, LoopSettings loop)
{
    Clip c;
    c.kind = Clip::Kind::Midi;
    c.currentStart = c.time = start;
    c.currentEnd = end;
    c.loop = loop;
    c.notes = {{0.0, 0.5, 60, 100.0f}};
    return c;
}
// The reference app's invariant for unlooped Arrangement clips (true for every one of the 35,000 in the real sets).
bool liveUnlooped(const Clip& c, double unitsPerBeat = 1.0)
{
    return std::fabs(c.loop.startRelative) < 1e-9
        && std::fabs((c.currentEnd - c.currentStart) * unitsPerBeat - (c.loop.loopEnd - c.loop.loopStart)) < 1e-9;
}
double contentAt(const Clip& c, double beat)
{
    return clipContentPosition(beat, c.currentStart, c.loop.loopStart, c.loop.loopEnd, c.loop.startRelative, c.loop.enabled);
}
} // namespace

TEST_CASE("clip geometry: the editor fits the clip's region, not the Arrangement length of a looped clip")
{
    // 16 bars in the Arrangement, a one-bar loop: the editor shows that bar (Live), not 16 bars.
    auto looped = midiClip(16, 80, {0, 4, 0, true});
    auto range = clipEditorRange(looped, true, 120);
    CHECK_NEAR(range.first, 0, 1e-9);
    CHECK_NEAR(range.second, 4, 1e-9);
    // A fragment of a split, unlooped clip (a real set: CS 574, LS 92, LE 94): content 92..94 = clip bars 24.1 .. 24.3.
    auto fragment = midiClip(574, 576, {92, 94, 0, false});
    range = clipEditorRange(fragment, true, 120);
    CHECK_NEAR(range.first, 92, 1e-9);
    CHECK_NEAR(range.second, 94, 1e-9);
    // Loop on with the start marker inside the brace: from the loop start to the loop end.
    auto offset = midiClip(0, 8, {4, 8, 2, true});
    range = clipEditorRange(offset, true, 120);
    CHECK_NEAR(range.first, 4, 1e-9);
    CHECK_NEAR(range.second, 8, 1e-9);
    // A Session clip with a lead-in (start marker before the loop).
    auto leadIn = midiClip(0, 4, {4, 8, -2, true});
    range = clipEditorRange(leadIn, false, 120);
    CHECK_NEAR(range.first, 2, 1e-9);
    CHECK_NEAR(range.second, 8, 1e-9);
    // Unlooped Arrangement clip whose stored end marker is stale (written by older Sonora): its right edge decides.
    auto stale = midiClip(184, 191, {0, 8, 0, false});
    const auto m = clipMarkers(stale, true, 120);
    CHECK_NEAR(m.start, 0, 1e-9);
    CHECK_NEAR(m.end, 7, 1e-9);
}

TEST_CASE("clip geometry: a clip reopens at the view the reference app saved with it")
{
    auto clip = midiClip(240, 272, {0, 392, 48, true}); // a real set's melody clip: saved view 88 .. 120
    clip.sourceXml = std::make_shared<const std::string>(
        "<MidiClip Id=\"0\" Time=\"240\"><CurrentStart Value=\"240\" /><ScrollerTimePreserver><LeftTime Value=\"88\" />"
        "<RightTime Value=\"120\" /></ScrollerTimePreserver><TimeSelection><AnchorTime Value=\"142\" /></TimeSelection></MidiClip>");
    auto saved = savedEditorRange(clip, true, 138);
    REQUIRE(saved.has_value());
    CHECK_NEAR(saved->first, 88, 1e-9);
    CHECK_NEAR(saved->second, 120, 1e-9);
    CHECK_NEAR(openingEditorRange(clip, true, 138).first, 88, 1e-9);
    // 0 .. 0 means the clip was never shown: its region.
    clip.sourceXml = std::make_shared<const std::string>(
        "<MidiClip><ScrollerTimePreserver><LeftTime Value=\"0\" /><RightTime Value=\"0\" /></ScrollerTimePreserver></MidiClip>");
    CHECK(! savedEditorRange(clip, true, 138).has_value());
    CHECK_NEAR(openingEditorRange(clip, true, 138).second, 392, 1e-9);
    // Fresh content (new clips, replaced samples) does not inherit the old view.
    clip.sourceXml = std::make_shared<const std::string>(
        "<MidiClip><ScrollerTimePreserver><LeftTime Value=\"8\" /><RightTime Value=\"16\" /></ScrollerTimePreserver></MidiClip>");
    clip.resetSourceMarkers = true;
    CHECK(! savedEditorRange(clip, true, 138).has_value());
}

TEST_CASE("clip geometry: right edge - unlooped clips move their end marker, looped ones repeat, audio stops at the sample")
{
    auto unlooped = midiClip(8, 16, {0, 8, 0, false});
    auto longer = resizedEnd(unlooped, 20, 120, 0.25);
    CHECK_NEAR(longer.currentEnd, 20, 1e-9);
    CHECK_NEAR(longer.loop.loopEnd, 12, 1e-9); // the end marker follows: more of the clip's content is revealed
    CHECK(liveUnlooped(longer));
    auto shorter = resizedEnd(unlooped, 8.1, 120, 1.0);
    CHECK_NEAR(shorter.currentEnd, 9, 1e-9); // never shorter than one grid step

    auto looped = midiClip(8, 16, {0, 4, 0, true});
    auto repeated = resizedEnd(looped, 24, 120, 0.25);
    CHECK_NEAR(repeated.currentEnd, 24, 1e-9);
    CHECK_NEAR(repeated.loop.loopEnd, 4, 1e-9); // the loop repeats; its brace does not move

    // Unwarped audio: 4 s of sample at 120 BPM = 8 beats. The clip shows 0..2 s (beats 0..4) and cannot pass 4 s.
    Clip audio;
    audio.kind = Clip::Kind::Audio;
    audio.warped = false;
    audio.sampleRate = 48000;
    audio.lengthSamples = 4 * 48000;
    audio.currentStart = audio.time = 0;
    audio.currentEnd = 4;
    audio.loop = {0, 2, 0, false};
    auto revealed = resizedEnd(audio, 12, 120, 0.25);
    CHECK_NEAR(revealed.currentEnd, 8, 1e-9);
    CHECK_NEAR(revealed.loop.loopEnd, 4, 1e-9); // seconds
    CHECK(liveUnlooped(revealed, 0.5));
    // Warped audio: warp markers 0 s = beat 0, 2 s = beat 4, so the 4 s sample ends at beat 8.
    audio.warped = true;
    audio.warpMarkers = {{0, 0}, {2, 4}};
    audio.loop = {0, 4, 0, false};
    revealed = resizedEnd(audio, 12, 120, 0.25);
    CHECK_NEAR(revealed.currentEnd, 8, 1e-9);
    // A warped audio clip with its loop on repeats like a MIDI clip.
    audio.loop.enabled = true;
    CHECK_NEAR(resizedEnd(audio, 12, 120, 0.25).currentEnd, 12, 1e-9);
}

TEST_CASE("clip geometry: left edge - the content stays on the timeline, loops wrap, audio stops at the sample start")
{
    auto unlooped = midiClip(8, 16, {0, 8, 0, false});
    auto trimmed = trimmedStart(unlooped, 10, 120, 0.25);
    CHECK_NEAR(trimmed.currentStart, 10, 1e-9);
    CHECK_NEAR(trimmed.loop.loopStart, 2, 1e-9); // the start marker moved two beats in
    CHECK(liveUnlooped(trimmed));
    CHECK_NEAR(contentAt(trimmed, 12), contentAt(unlooped, 12), 1e-9);
    // Extending to the left reveals content before the old start marker (a MIDI clip may start before 1.1.1).
    auto earlier = trimmedStart(unlooped, 6, 120, 0.25);
    CHECK_NEAR(earlier.loop.loopStart, -2, 1e-9);
    CHECK(liveUnlooped(earlier));
    CHECK_NEAR(trimmedStart(unlooped, 15.9, 120, 1.0).currentStart, 15, 1e-9); // one grid step stays

    // Looped: the start marker moves through the loop and wraps (content at every beat is unchanged).
    auto looped = midiClip(8, 24, {0, 4, 0, true});
    auto wrapped = trimmedStart(looped, 6, 120, 0.25);
    CHECK_NEAR(wrapped.currentStart, 6, 1e-9);
    CHECK_NEAR(wrapped.loop.startRelative, 2, 1e-9);
    for (double beat : {8.0, 9.5, 13.0, 20.0}) CHECK_NEAR(contentAt(wrapped, beat), contentAt(looped, beat), 1e-9);
    auto later = trimmedStart(looped, 13, 120, 0.25);
    CHECK_NEAR(later.loop.startRelative, 1, 1e-9); // 5 beats in = one loop + 1
    for (double beat : {13.0, 15.0, 20.0}) CHECK_NEAR(contentAt(later, beat), contentAt(looped, beat), 1e-9);

    // Unwarped audio cannot start before its first sample.
    Clip audio;
    audio.kind = Clip::Kind::Audio;
    audio.sampleRate = 44100;
    audio.lengthSamples = 10 * 44100;
    audio.currentStart = audio.time = 8;
    audio.currentEnd = 12;
    audio.loop = {1, 3, 0, false}; // 1 s .. 3 s at 120 BPM = 4 beats
    auto revealed = trimmedStart(audio, 2, 120, 0.25);
    CHECK_NEAR(revealed.loop.loopStart, 0, 1e-9);
    CHECK_NEAR(revealed.currentStart, 6, 1e-9); // 1 s = 2 beats earlier, not 6
    CHECK(liveUnlooped(revealed, 0.5));
}

TEST_CASE("clip geometry: magnetic snapping to the grid and to other clips' edges and locators")
{
    CHECK_NEAR(snapWithTargets(5.3, 1.0, {}), 5.0, 1e-9);
    CHECK_NEAR(snapWithTargets(5.3, 1.0, {5.4}), 5.4, 1e-9);  // a clip edge nearer than the grid line wins
    CHECK_NEAR(snapWithTargets(5.3, 1.0, {5.9}), 5.0, 1e-9);
    CHECK_NEAR(snapWithTargets(5.3, 0.0, {5.4}), 5.3, 1e-9);  // snapping off: free
    // A 3.5-beat clip dragged to 6.4: its start snaps to 6; next to a clip starting at 10.1, its end docks there instead.
    CHECK_NEAR(snapClipStart(6.4, 3.5, 1.0, {}, false), 6.0, 1e-9);
    CHECK_NEAR(snapClipStart(6.4, 3.5, 1.0, {10.1}, false), 6.6, 1e-9);
    CHECK_NEAR(snapClipStart(6.4, 3.5, 1.0, {}, true), 6.5, 1e-9); // Shift: the end on the grid
    CHECK_NEAR(snapClipStart(-3, 3.5, 1.0, {}, false), 0.0, 1e-9);
}

TEST_CASE("clip geometry: markers - an unlooped Arrangement clip's length follows Start and End")
{
    auto unlooped = midiClip(16, 24, {0, 8, 0, false});
    auto m = clipMarkers(unlooped, true, 120);
    m.end = 6;
    auto shorter = withMarkers(unlooped, m, true, 120);
    CHECK_NEAR(shorter.currentStart, 16, 1e-9);
    CHECK_NEAR(shorter.currentEnd, 22, 1e-9);
    CHECK(liveUnlooped(shorter));
    m = clipMarkers(unlooped, true, 120);
    m.start = 2;
    auto later = withMarkers(unlooped, m, true, 120);
    CHECK_NEAR(later.currentStart, 16, 1e-9); // the clip stays where it is; it now plays from content beat 2
    CHECK_NEAR(later.currentEnd, 22, 1e-9);
    CHECK_NEAR(later.loop.loopStart, 2, 1e-9);
    // Looped: the loop changes, the Arrangement length does not.
    auto looped = midiClip(16, 48, {0, 4, 0, true});
    m = clipMarkers(looped, true, 120);
    m.loopEnd = 8;
    m.start = 9; // past the loop end: wraps into the loop
    auto relooped = withMarkers(looped, m, true, 120);
    CHECK_NEAR(relooped.currentEnd, 48, 1e-9);
    CHECK_NEAR(relooped.loop.loopEnd, 8, 1e-9);
    CHECK_NEAR(relooped.loop.startRelative, 1, 1e-9);
    // A Session clip's markers never touch its (unused) Arrangement extent.
    m = clipMarkers(unlooped, false, 120);
    m.looping = false;
    m.end = 2;
    CHECK_NEAR(withMarkers(unlooped, m, false, 120).currentEnd, 24, 1e-9);
}

TEST_CASE("clip geometry: edits keep unlooped clips the way the reference stores them")
{
    Song song;
    song.tempo = 120;
    song.tracks.resize(1);
    song.tracks[0].kind = TrackKind::Midi;
    auto clip = midiClip(0, 16, {0, 16, 0, false});
    clip.uid = 900;
    song.tracks[0].arrangementClips = {clip};
    auto split = splitArrangementClips(song, 0, 0, 5, 11);
    REQUIRE(split.apply);
    split.apply(song);
    REQUIRE(song.tracks[0].arrangementClips.size() == 3);
    for (const auto& part : song.tracks[0].arrangementClips) CHECK(liveUnlooped(part));
    CHECK_NEAR(song.tracks[0].arrangementClips[1].loop.loopStart, 5, 1e-9);
    CHECK(liveUnlooped(withEnd(clip, 10)));
    CHECK(liveUnlooped(withStart(clip, 3, 120)));
}

TEST_CASE("clip placement: a clip dropped onto others overwrites what it covers, in one undo step")
{
    Song song;
    song.tempo = 120;
    song.tracks.resize(2);
    for (auto& t : song.tracks) t.kind = TrackKind::Midi;
    auto a = midiClip(0, 4, {0, 4, 0, false});
    a.uid = 1;
    auto b = midiClip(8, 16, {0, 8, 0, false});
    b.uid = 2;
    auto inside = midiClip(10, 11, {0, 1, 0, false});
    inside.uid = 3;
    song.tracks[0].arrangementClips = {a, b};
    EditHistory history;
    // Move A to 6..10: B (8..16) loses its first two beats and keeps its content where it was.
    int uid = -1;
    auto command = placeArrangementClip(song, 0, 1, 0, movedClip(a, 6), false, "Move Clip", &uid);
    REQUIRE(command.apply);
    history.perform(song, std::move(command));
    CHECK(uid == 1); // a moved clip keeps its identity on its own track
    const auto& clips = song.tracks[0].arrangementClips;
    REQUIRE(clips.size() == 2);
    CHECK_NEAR(clips[0].currentStart, 6, 1e-9);
    CHECK_NEAR(clips[1].currentStart, 10, 1e-9);
    CHECK_NEAR(clips[1].currentEnd, 16, 1e-9);
    CHECK_NEAR(clips[1].loop.loopStart, 2, 1e-9);
    CHECK(liveUnlooped(clips[1]));
    REQUIRE(history.undo(song));
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, 8, 1e-9);

    // Option-copy A into the middle of B: B is split around it, the original A stays.
    command = placeArrangementClip(song, 0, 1, 0, movedClip(a, 10), true, "Copy Clip", &uid);
    history.perform(song, std::move(command));
    CHECK(uid != 1);
    REQUIRE(song.tracks[0].arrangementClips.size() == 4);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentEnd, 10, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[3].currentStart, 14, 1e-9);
    REQUIRE(history.undo(song));

    // Moving to another track removes clips that lie entirely under it.
    song.tracks[1].arrangementClips = {inside};
    command = placeArrangementClip(song, 0, 2, 1, b, false, "Move Clip", &uid);
    history.perform(song, std::move(command));
    REQUIRE(song.tracks[1].arrangementClips.size() == 1);
    CHECK_NEAR(song.tracks[1].arrangementClips[0].currentStart, 8, 1e-9);
    CHECK(song.tracks[0].arrangementClips.size() == 1);
    REQUIRE(history.undo(song));
    CHECK(song.tracks[1].arrangementClips[0].uid == 3);

    // Cmd+D: the copy goes right after the clip and replaces what it covers.
    song.tracks[0].arrangementClips = {a, midiClip(5, 7, {0, 2, 0, false})};
    song.tracks[0].arrangementClips[1].uid = 5;
    history.perform(song, duplicateClip(song, 0, 1));
    REQUIRE(song.tracks[0].arrangementClips.size() == 2); // 5..7 lay under the copy (4..8)
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentStart, 4, 1e-9);
    CHECK_NEAR(song.tracks[0].arrangementClips[1].currentEnd, 8, 1e-9);
    REQUIRE(history.undo(song));
    CHECK(song.tracks[0].arrangementClips.size() == 2);
    CHECK(song.tracks[0].arrangementClips[1].uid == 5);
}
