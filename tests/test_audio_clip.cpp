#include "TestFramework.h"

#include "core/Edit.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "engine/ClipPlayers.h"
#include "ui/AudioClipEdits.h"
#include "ui/MidiClipEdits.h"

#include <juce_core/juce_core.h>

using namespace sonora;

namespace
{
model::Clip warpedClip()
{
    model::Clip c;
    c.kind = model::Clip::Kind::Audio;
    c.warped = true;
    c.sampleRate = 48000;
    c.lengthSamples = 96000; // 2 s
    c.warpMarkers = {{0.0, 0.0}, {2.0, 4.0}}; // 120 BPM
    c.loop = {0.0, 4.0, 1.0, true};
    c.currentStart = 8.0;
    c.currentEnd = 12.0;
    return c;
}
} // namespace

TEST_CASE("audio clip edits: BPM comes from the warp markers and /2 x2 rescale the clip consistently")
{
    const auto c = warpedClip();
    CHECK_NEAR(ui::audioedit::clipBpm(c, 100.0), 120.0, 1e-9);
    model::Clip none;
    CHECK_NEAR(ui::audioedit::clipBpm(none, 97.0), 97.0, 1e-9);

    const auto doubled = ui::audioedit::scaleTempo(c, 2.0);
    CHECK_NEAR(ui::audioedit::clipBpm(doubled, 100.0), 240.0, 1e-9);
    CHECK_NEAR(doubled.warpMarkers[1].beats, 8.0, 1e-9);
    CHECK_NEAR(doubled.warpMarkers[1].seconds, 2.0, 1e-9); // the source does not move
    CHECK_NEAR(doubled.loop.loopEnd, 8.0, 1e-9);
    CHECK_NEAR(doubled.loop.startRelative, 2.0, 1e-9);
    CHECK_NEAR(doubled.currentEnd - doubled.currentStart, 8.0, 1e-9);

    const auto back = ui::audioedit::scaleTempo(doubled, 0.5);
    CHECK_NEAR(back.warpMarkers[1].beats, 4.0, 1e-9);
    CHECK_NEAR(back.loop.loopEnd, 4.0, 1e-9);
    CHECK_NEAR(back.currentEnd, 12.0, 1e-9);

    // Unwarped clips and nonsense factors are left alone.
    auto unwarped = c;
    unwarped.warped = false;
    CHECK_NEAR(ui::audioedit::scaleTempo(unwarped, 2.0).loop.loopEnd, 4.0, 1e-9);
    CHECK_NEAR(ui::audioedit::scaleTempo(c, 0.0).loop.loopEnd, 4.0, 1e-9);
}

TEST_CASE("audio clip edits: warp toggle keeps the start and loop at the same source time")
{
    auto c = warpedClip();
    const auto off = ui::audioedit::toggleWarp(c, 120.0);
    REQUIRE(! off.warped);
    CHECK_NEAR(off.loop.loopStart, 0.0, 1e-9);
    CHECK_NEAR(off.loop.loopEnd, 2.0, 1e-9); // beat 4 at marker (2 s, 4 beats)
    CHECK_NEAR(off.loop.loopStart + off.loop.startRelative, 0.5, 1e-9); // beat 1 == 0.5 s
    const auto on = ui::audioedit::toggleWarp(off, 120.0);
    REQUIRE(on.warped);
    CHECK_NEAR(on.loop.loopEnd, 4.0, 1e-9);
    CHECK_NEAR(on.loop.loopStart + on.loop.startRelative, 1.0, 1e-9);

    model::Clip fresh;
    fresh.kind = model::Clip::Kind::Audio;
    fresh.sampleRate = 44100;
    fresh.lengthSamples = 44100;
    fresh.loop = {0.0, 1.0, 0.0, false};
    const auto warped = ui::audioedit::toggleWarp(fresh, 120.0);
    REQUIRE(warped.warped);
    REQUIRE(warped.warpMarkers.size() == 2); // anchors are created for a clip that had none
    CHECK_NEAR(warped.warpMarkers[1].beats, 2.0, 1e-9);
}

TEST_CASE("audio clip edits: reversing mirrors markers and loop around the sample")
{
    auto c = warpedClip();
    c.warpMarkers = {{0.0, 0.0}, {0.5, 1.0}, {2.0, 4.0}};
    c.loop = {1.0, 3.0, 0.5, true};
    const auto r = ui::audioedit::mirrorForReverse(c, 2.0);
    REQUIRE(r.warpMarkers.size() == 3);
    CHECK_NEAR(r.warpMarkers[0].seconds, 0.0, 1e-9);
    CHECK_NEAR(r.warpMarkers[0].beats, 0.0, 1e-9);
    CHECK_NEAR(r.warpMarkers[1].seconds, 1.5, 1e-9); // 2 - 0.5
    CHECK_NEAR(r.warpMarkers[1].beats, 3.0, 1e-9);   // 4 - 1
    CHECK_NEAR(r.warpMarkers[2].seconds, 2.0, 1e-9);
    CHECK_NEAR(r.warpMarkers[2].beats, 4.0, 1e-9);
    CHECK_NEAR(r.loop.loopStart, 1.0, 1e-9);
    CHECK_NEAR(r.loop.loopEnd, 3.0, 1e-9);
    CHECK_NEAR(r.loop.startRelative, 0.0, 1e-9);
    // Reversing twice restores the loop region.
    const auto twice = ui::audioedit::mirrorForReverse(r, 2.0);
    CHECK_NEAR(twice.loop.loopStart, 1.0, 1e-9);
    CHECK_NEAR(twice.warpMarkers[1].seconds, 0.5, 1e-9);

    auto unwarped = c;
    unwarped.warped = false;
    unwarped.loop = {0.25, 1.5, 0.0, false};
    const auto u = ui::audioedit::mirrorForReverse(unwarped, 2.0);
    CHECK_NEAR(u.loop.loopStart, 0.5, 1e-9);
    CHECK_NEAR(u.loop.loopEnd, 1.75, 1e-9);
}

TEST_CASE("audio clip edits: positions format like Live's displays")
{
    CHECK(ui::audioedit::formatBeats(0.0, 4, true) == "1. 1. 1");
    CHECK(ui::audioedit::formatBeats(4.0, 4, true) == "2. 1. 1");
    CHECK(ui::audioedit::formatBeats(5.75, 4, true) == "2. 2. 4");
    CHECK(ui::audioedit::formatBeats(4.0, 4, false) == "1. 0. 0");
    CHECK(ui::audioedit::formatBeats(1.25, 4, false) == "0. 1. 1");
    CHECK(ui::audioedit::formatSeconds(0.58) == "0: 0.580");
    CHECK(ui::audioedit::formatSeconds(75.5) == "1: 15.500");
}

TEST_CASE("audio clip settings (warp mode options, gain, pitch) are written to the set and reload")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    REQUIRE(song.tracks.size() >= 2);
    const auto original = song.tracks[1].arrangementClips[0];
    auto changed = original;
    changed.warpMode = model::WarpMode::Texture;
    changed.transientResolution = 3;
    changed.transientLoopMode = 0;
    changed.transientEnvelope = 40.0;
    changed.granularityTones = 40.0;
    changed.granularityTexture = 120.0;
    changed.fluctuationTexture = 55.0;
    changed.gain = 0.5;
    changed.pitchCoarse = -5;
    changed.pitchFine = 20.0;
    model::EditHistory history;
    history.perform(song, model::modifyClip(song, 1, original.uid, changed, "Edit Clip Settings"));

    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto& c = again.song->tracks[1].arrangementClips[0];
    CHECK(c.warpMode == model::WarpMode::Texture);
    CHECK(c.transientResolution == 3);
    CHECK(c.transientLoopMode == 0);
    CHECK_NEAR(c.transientEnvelope, 40.0, 1e-9);
    CHECK_NEAR(c.granularityTexture, 120.0, 1e-9);
    CHECK_NEAR(c.fluctuationTexture, 55.0, 1e-9);
    CHECK_NEAR(c.gain, 0.5, 1e-9);
    CHECK(c.pitchCoarse == -5);
    CHECK_NEAR(c.pitchFine, 20.0, 1e-9);

    // Undo restores the original values exactly.
    REQUIRE(history.undo(song));
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto restored = io::loadAls(out.getFile());
    REQUIRE(restored);
    const auto& r = restored.song->tracks[1].arrangementClips[0];
    CHECK(r.transientResolution == original.transientResolution);
    CHECK_NEAR(r.granularityTexture, original.granularityTexture, 1e-9);
}

#include "engine/SampleCache.h"
#include "engine/SampleExport.h"

#include <juce_audio_formats/juce_audio_formats.h>

TEST_CASE("reverse: the written copy plays the sample backwards and a clip can be pointed at it")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_reverse_test");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());
    const auto source = dir.getChildFile("ramp.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream(source.createOutputStream());
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(24));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> ramp(2, 1000);
        for (int i = 0; i < 1000; ++i) { ramp.setSample(0, i, (float) i / 1000.0f); ramp.setSample(1, i, -(float) i / 1000.0f); }
        REQUIRE(writer->writeFromAudioSampleBuffer(ramp, 0, 1000));
    }
    engine::SampleCache cache;
    const auto data = cache.load(source.getFullPathName().toStdString());
    REQUIRE(data != nullptr);
    const auto target = dir.getChildFile("Samples/Processed/Reverse/ramp [reversed].wav");
    CHECK(engine::writeReversedWav(data->audio, data->sampleRate, target).empty());
    REQUIRE(target.existsAsFile());
    const auto reversed = cache.load(target.getFullPathName().toStdString());
    REQUIRE(reversed != nullptr);
    REQUIRE(reversed->audio.getNumSamples() == 1000);
    CHECK_NEAR(reversed->audio.getSample(0, 0), 999.0 / 1000.0, 1e-4);
    CHECK_NEAR(reversed->audio.getSample(0, 999), 0.0, 1e-4);
    CHECK_NEAR(reversed->audio.getSample(1, 0), -999.0 / 1000.0, 1e-4);
    CHECK(! engine::writeReversedWav(juce::AudioBuffer<float>(), 44100.0, dir.getChildFile("empty.wav")).empty());
    dir.deleteRecursively();
}

TEST_CASE("crop: the kept range is the loop region and markers shift so the same audio plays")
{
    // Warped: loop beats 1..3 at 120 BPM (markers 0s=0b, 2s=4b) -> source 0.5 s .. 1.5 s; start marker inside the loop.
    auto c = warpedClip();
    c.loop = {1.0, 3.0, 0.5, true};
    const auto range = ui::audioedit::cropRange(c, 120.0);
    CHECK_NEAR(range.start, 0.5, 1e-9);
    CHECK_NEAR(range.end, 1.5, 1e-9);
    const auto cut = ui::audioedit::cropped(c, range.start);
    CHECK_NEAR(cut.warpMarkers[0].seconds, -0.5, 1e-9);
    CHECK_NEAR(cut.warpMarkers[1].seconds, 1.5, 1e-9);
    // Same beat -> same audio: old seconds - 0.5 == new seconds
    const double oldAt = engine::warpBeatsToSeconds(c.warpMarkers, 2.0, 0.5);
    const double newAt = engine::warpBeatsToSeconds(cut.warpMarkers, 2.0, 0.5);
    CHECK_NEAR(newAt, oldAt - 0.5, 1e-9);
    CHECK_NEAR(cut.loop.loopStart, 1.0, 1e-9); // beats do not change when warped

    // Unwarped clips keep their loop in seconds, so it moves with the file.
    model::Clip u;
    u.kind = model::Clip::Kind::Audio;
    u.loop = {0.25, 1.0, -0.1, true}; // start marker before the loop start
    const auto ur = ui::audioedit::cropRange(u, 120.0);
    CHECK_NEAR(ur.start, 0.15, 1e-9);
    CHECK_NEAR(ur.end, 1.0, 1e-9);
    const auto uc = ui::audioedit::cropped(u, ur.start);
    CHECK_NEAR(uc.loop.loopStart, 0.1, 1e-9);
    CHECK_NEAR(uc.loop.loopEnd, 0.85, 1e-9);
}

TEST_CASE("crop: the written file holds exactly the chosen frames")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_crop_test");
    dir.deleteRecursively();
    dir.createDirectory();
    juce::AudioBuffer<float> ramp(2, 1000);
    for (int i = 0; i < 1000; ++i) { ramp.setSample(0, i, (float) i / 1000.0f); ramp.setSample(1, i, -(float) i / 1000.0f); }
    const auto target = dir.getChildFile("Samples/Processed/Crop/ramp [cropped].wav");
    CHECK(engine::writeCroppedWav(ramp, 44100.0, 200, 300, target).empty());
    engine::SampleCache cache;
    const auto data = cache.load(target.getFullPathName().toStdString());
    REQUIRE(data != nullptr);
    REQUIRE(data->audio.getNumSamples() == 300);
    CHECK_NEAR(data->audio.getSample(0, 0), 0.2, 1e-4);
    CHECK_NEAR(data->audio.getSample(0, 299), 0.499, 1e-4);
    CHECK_NEAR(data->audio.getSample(1, 0), -0.2, 1e-4);
    // Out-of-range requests clamp; an empty result is an error
    CHECK(engine::writeCroppedWav(ramp, 44100.0, 5000, 10, dir.getChildFile("none.wav")).size() > 0);
    dir.deleteRecursively();
}

namespace
{
model::Clip notesClip()
{
    model::Clip c;
    c.kind = model::Clip::Kind::Midi;
    c.currentStart = 0.0;
    c.currentEnd = 4.0;
    c.loop = {0.0, 4.0, 0.0, true};
    c.notes.push_back({0.0, 0.5, 60, 100.0f, 64.0f, 1.0f, true, 7});
    c.notes.push_back({1.0, 0.5, 64, 90.0f, 64.0f, 1.0f, true, 8});
    c.notes.push_back({3.0, 1.0, 67, 80.0f, 64.0f, 1.0f, true, 9});
    return c;
}
} // namespace

TEST_CASE("midi clip edits: reverse mirrors positions inside the loop and keeps durations")
{
    const auto r = ui::midiedit::reverse(notesClip());
    REQUIRE(r.notes.size() == 3);
    CHECK_NEAR(r.notes[0].start, 3.5, 1e-9); // 0..0.5 -> 3.5..4
    CHECK_NEAR(r.notes[1].start, 2.5, 1e-9);
    CHECK_NEAR(r.notes[2].start, 0.0, 1e-9); // 3..4 -> 0..1
    CHECK_NEAR(r.notes[2].duration, 1.0, 1e-9);
    const auto twice = ui::midiedit::reverse(r);
    for (size_t i = 0; i < 3; ++i)
        CHECK_NEAR(twice.notes[i].start, notesClip().notes[i].start, 1e-9);
}

TEST_CASE("midi clip edits: invert mirrors pitches around the range, legato joins notes, notes outside the loop stay")
{
    auto clip = notesClip();
    clip.notes.push_back({6.0, 1.0, 100, 90.0f, 64.0f, 1.0f, true, 10}); // after the loop end
    const auto inv = ui::midiedit::invert(clip);
    CHECK(inv.notes[0].pitch == 67);
    CHECK(inv.notes[1].pitch == 63);
    CHECK(inv.notes[2].pitch == 60);
    CHECK(inv.notes[3].pitch == 100); // untouched

    const auto leg = ui::midiedit::legato(notesClip());
    CHECK_NEAR(leg.notes[0].duration, 1.0, 1e-9); // up to the next note at beat 1
    CHECK_NEAR(leg.notes[1].duration, 2.0, 1e-9); // up to beat 3
    CHECK_NEAR(leg.notes[2].duration, 1.0, 1e-9); // the last one is kept
}

TEST_CASE("midi clip edits: duplicate loop doubles the loop and repeats notes with fresh ids; stretch scales everything")
{
    const auto dup = ui::midiedit::duplicateLoop(notesClip());
    CHECK_NEAR(dup.loop.loopEnd, 8.0, 1e-9);
    CHECK_NEAR(dup.currentEnd, 8.0, 1e-9);
    REQUIRE(dup.notes.size() == 6);
    CHECK_NEAR(dup.notes[3].start, 4.0, 1e-9);
    CHECK_NEAR(dup.notes[5].start, 7.0, 1e-9);
    CHECK(dup.notes[3].noteId == -1);
    CHECK(dup.notes[0].noteId == 7); // the originals keep theirs

    const auto twice = ui::midiedit::stretch(notesClip(), 2.0);
    CHECK_NEAR(twice.notes[1].start, 2.0, 1e-9);
    CHECK_NEAR(twice.notes[1].duration, 1.0, 1e-9);
    CHECK_NEAR(twice.loop.loopEnd, 8.0, 1e-9);
    CHECK_NEAR(twice.currentEnd, 8.0, 1e-9);
    const auto half = ui::midiedit::stretch(notesClip(), 0.5);
    CHECK_NEAR(half.notes[2].start, 1.5, 1e-9);
    CHECK_NEAR(half.loop.loopEnd, 2.0, 1e-9);
}

TEST_CASE("clip fades are written to the set (authored when missing, patched when present) and reload")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto original = song.tracks[1].arrangementClips[0];
    CHECK_NEAR(original.fadeIn, 0.0, 1e-12);
    CHECK(original.fadeInDefault);
    model::EditHistory history;
    auto faded = original;
    faded.fadeIn = 1.5;
    faded.fadeOut = 0.75;
    faded.fadeInDefault = faded.fadeOutDefault = false;
    history.perform(song, model::modifyClip(song, 1, original.uid, faded, "Change Fade"));

    juce::TemporaryFile first(".als");
    REQUIRE(io::saveAls(song, first.getFile()).ok);
    auto again = io::loadAls(first.getFile());
    REQUIRE(again);
    const auto& c = again.song->tracks[1].arrangementClips[0];
    CHECK_NEAR(c.fadeIn, 1.5, 1e-9);
    CHECK_NEAR(c.fadeOut, 0.75, 1e-9);
    CHECK(! c.fadeInDefault);
    CHECK(! c.fadeOutDefault);

    // an existing <Fades> element is patched, and going back to the default writes the default flags
    auto& song2 = *again.song;
    auto back = c;
    back.fadeIn = 0.0;
    back.fadeInDefault = true;
    back.fadeOut = 2.0;
    model::EditHistory history2;
    history2.perform(song2, model::modifyClip(song2, 1, c.uid, back, "Change Fade"));
    juce::TemporaryFile second(".als");
    REQUIRE(io::saveAls(song2, second.getFile()).ok);
    auto last = io::loadAls(second.getFile());
    REQUIRE(last);
    const auto& d = last.song->tracks[1].arrangementClips[0];
    CHECK_NEAR(d.fadeIn, 0.0, 1e-9);
    CHECK(d.fadeInDefault);
    CHECK_NEAR(d.fadeOut, 2.0, 1e-9);
    CHECK(! d.fadeOutDefault);
}

#include "ui/ClipEditing.h"
TEST_CASE("audio clip edits: Option lower edge stretch preserves source pitch and anchored edge")
{
    auto clip = warpedClip();
    auto stretched = ui::dragClip(clip, ui::DragMode::StretchEnd, 4, 1, true, 120);
    CHECK_NEAR(stretched.currentStart, 8, 1e-9); CHECK_NEAR(stretched.currentEnd, 16, 1e-9);
    CHECK_NEAR(stretched.warpMarkers.back().seconds, 2, 1e-9); CHECK_NEAR(stretched.warpMarkers.back().beats, 8, 1e-9);
    CHECK(stretched.pitchCoarse == clip.pitchCoarse);
    auto left = ui::dragClip(clip, ui::DragMode::StretchStart, -4, 1, true, 120);
    CHECK_NEAR(left.currentStart, 4, 1e-9); CHECK_NEAR(left.currentEnd, 12, 1e-9);
    clip.warped = false; clip.warpMarkers.clear(); clip.loop = {0, 2, 0, false};
    auto enabled = ui::dragClip(clip, ui::DragMode::StretchEnd, 4, 1, true, 120);
    CHECK(enabled.warped); CHECK(enabled.warpMarkers.size() == 2); CHECK_NEAR(enabled.currentEnd, 16, 1e-9);
    CHECK(ui::hitZone(8, 100) == ui::DragMode::TrimStart); CHECK(ui::hitZone(92, 100) == ui::DragMode::ResizeEnd);
}
