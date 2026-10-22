#include "TestFramework.h"

#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/SampleDrop.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <set>

using namespace sonora;

namespace
{
int audioTrackIndex(const model::Song& song)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == model::TrackKind::Audio) return (int) i;
    return -1;
}

int midiTrackIndex(const model::Song& song)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == model::TrackKind::Midi) return (int) i;
    return -1;
}
} // namespace

TEST_CASE("sample drop: several files stack end to end on one track in a single undo step")
{
    io::NewSetOptions options; options.scenes = 2; options.tempo = 120.0;
    auto song = io::parseAls(*io::createNewSetDom(options), "/tmp/sonora_sample_drop.als").song;
    REQUIRE(song != nullptr);
    const int audio = audioTrackIndex(*song);
    REQUIRE(audio >= 0);
    const auto clipsBefore = song->tracks[(size_t) audio].arrangementClips.size();
    const auto tracksBefore = song->tracks.size();

    // 2 s and 1 s at 120 BPM: 4 and 2 beats. The unreadable file is skipped.
    const std::vector<io::DroppedSample> samples{{"/tmp/Kick.wav", 48000.0, 96000}, {"/tmp/broken.m4a", 0.0, 0},
                                                 {"/tmp/Vocal.m4a", 44100.0, 44100}};
    auto plan = io::placeSamples(*song, samples, {audio, 8.0, -1, false});
    REQUIRE(plan.command.apply != nullptr);
    CHECK(plan.placed == 2);
    CHECK(plan.firstTrack == audio);
    CHECK(! plan.command.structureChanged);
    CHECK(plan.command.label == "Insert 2 Samples");
    plan.command.apply(*song);
    const auto& clips = song->tracks[(size_t) audio].arrangementClips;
    REQUIRE(clips.size() == clipsBefore + 2);
    CHECK(song->tracks.size() == tracksBefore);
    CHECK(clips[clipsBefore].name == "Kick");
    CHECK(std::abs(clips[clipsBefore].currentStart - 8.0) < 1e-9);
    CHECK(std::abs(clips[clipsBefore].currentEnd - 12.0) < 1e-9);
    CHECK(clips[clipsBefore + 1].name == "Vocal");
    CHECK(clips[clipsBefore + 1].sample.absolutePath == "/tmp/Vocal.m4a");
    CHECK(std::abs(clips[clipsBefore + 1].currentStart - 12.0) < 1e-9);
    CHECK(std::abs(clips[clipsBefore + 1].currentEnd - 14.0) < 1e-9);
    CHECK(clips[clipsBefore].sourceXml != nullptr);
    // Like Live, a new clip is warped (Beats) with the sample's tempo taken to be the set's: anchors at both ends, loop in beats.
    CHECK(clips[clipsBefore].warped);
    CHECK(clips[clipsBefore].warpMode == model::WarpMode::Beats);
    REQUIRE(clips[clipsBefore].warpMarkers.size() == 2);
    CHECK(std::abs(clips[clipsBefore].warpMarkers[1].seconds - 2.0) < 1e-9);
    CHECK(std::abs(clips[clipsBefore].warpMarkers[1].beats - 4.0) < 1e-9);
    CHECK(std::abs(clips[clipsBefore].loop.loopEnd - 4.0) < 1e-9);
    const int firstUid = clips[clipsBefore].uid;
    plan.command.revert(*song);
    CHECK(song->tracks[(size_t) audio].arrangementClips.size() == clipsBefore);
    plan.command.apply(*song); // redo keeps the clip identities
    CHECK(song->tracks[(size_t) audio].arrangementClips[clipsBefore].uid == firstUid);
}

TEST_CASE("sample drop: one track per file inserts new audio tracks right after the target")
{
    io::NewSetOptions options; options.scenes = 2;
    auto song = io::parseAls(*io::createNewSetDom(options), "/tmp/sonora_sample_drop.als").song;
    REQUIRE(song != nullptr);
    const int audio = audioTrackIndex(*song);
    REQUIRE(audio >= 0);
    const auto before = *song;
    const std::vector<io::DroppedSample> samples{{"/tmp/A.wav", 48000.0, 48000}, {"/tmp/B.wav", 48000.0, 48000}, {"/tmp/C.wav", 48000.0, 48000}};

    auto arrangement = io::placeSamples(*song, samples, {audio, 4.0, -1, true});
    REQUIRE(arrangement.command.apply != nullptr);
    CHECK(arrangement.command.structureChanged);
    arrangement.command.apply(*song);
    REQUIRE(song->tracks.size() == before.tracks.size() + 2);
    std::set<int> ids;
    for (const auto& track : song->tracks) ids.insert(track.id);
    CHECK(ids.size() == song->tracks.size()); // fresh track ids
    for (int i = 0; i < 3; ++i)
    {
        const auto& track = song->tracks[(size_t) (audio + i)];
        CHECK(track.kind == model::TrackKind::Audio);
        REQUIRE(! track.arrangementClips.empty());
        CHECK(std::abs(track.arrangementClips.back().currentStart - 4.0) < 1e-9);
        CHECK(track.slots.size() == song->scenes.size());
    }
    CHECK(song->tracks[(size_t) audio + 2].arrangementClips.back().name == "C");
    CHECK(song->tracks[(size_t) audio + 3].id == before.tracks[(size_t) audio + 1].id); // the old neighbour moved right
    arrangement.command.revert(*song);
    CHECK(song->tracks.size() == before.tracks.size());
    CHECK(song->tracks[(size_t) audio].arrangementClips.size() == before.tracks[(size_t) audio].arrangementClips.size());

    // Session: the same scene on each track.
    auto session = io::placeSamples(*song, samples, {audio, 0.0, 1, true});
    REQUIRE(session.command.apply != nullptr);
    session.command.apply(*song);
    for (int i = 0; i < 3; ++i)
    {
        const auto& slots = song->tracks[(size_t) (audio + i)].slots;
        REQUIRE(slots.size() > 1);
        CHECK(slots[1].clip.has_value());
        CHECK(! slots[0].clip.has_value());
    }
    session.command.revert(*song);
    CHECK(song->tracks.size() == before.tracks.size());
    CHECK(! song->tracks[(size_t) audio].slots[1].clip.has_value());
}

TEST_CASE("sample drop: empty space makes a new audio track; Session stacks add scenes; the set saves and reloads")
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_sample_drop");
    folder.deleteRecursively();
    REQUIRE(folder.createDirectory());
    io::NewSetOptions options; options.scenes = 2;
    const auto source = folder.getChildFile("Drop.als");
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto tracksBefore = song.tracks.size();
    const auto scenesBefore = song.scenes.size();
    REQUIRE(scenesBefore == 2);

    const std::vector<io::DroppedSample> samples{{"/tmp/One.m4a", 44100.0, 44100}, {"/tmp/Two.caf", 44100.0, 22050}};
    auto plan = io::placeSamples(song, samples, {-1, 0.0, 1, false});
    REQUIRE(plan.command.apply != nullptr);
    plan.command.apply(song);
    REQUIRE(song.tracks.size() == tracksBefore + 1);
    CHECK(plan.firstTrack == (int) tracksBefore);
    const auto& created = song.tracks.back();
    CHECK(created.kind == model::TrackKind::Audio);
    CHECK(song.scenes.size() == 3); // scenes 1 and 2: one scene was added
    for (const auto& track : song.tracks) CHECK(track.slots.size() == 3);
    REQUIRE(created.slots[1].clip.has_value());
    REQUIRE(created.slots[2].clip.has_value());
    CHECK(created.slots[1].clip->name == "One");
    CHECK(created.slots[2].clip->name == "Two");

    const auto saved = folder.getChildFile("Saved.als");
    REQUIRE(io::saveAls(song, saved).ok);
    auto again = io::loadAls(saved);
    REQUIRE(again);
    REQUIRE(again.song->tracks.size() == tracksBefore + 1);
    const auto& reloaded = again.song->tracks.back();
    CHECK(reloaded.kind == model::TrackKind::Audio);
    REQUIRE(reloaded.slots.size() == 3);
    REQUIRE(reloaded.slots[2].clip.has_value());
    CHECK(reloaded.slots[2].clip->sample.absolutePath == "/tmp/Two.caf");
    CHECK(reloaded.slots[2].clip->warped);
    CHECK(reloaded.slots[2].clip->hiQ);
    CHECK(! reloaded.slots[2].clip->ram);

    // Fade / RAM / HiQ are saved with the clip.
    auto edited = *again.song->tracks.back().slots[2].clip;
    edited.ram = true; edited.hiQ = false; edited.clipFade = false;
    again.song->tracks.back().slots[2].clip = edited;
    const auto savedFlags = folder.getChildFile("Flags.als");
    REQUIRE(io::saveAls(*again.song, savedFlags).ok);
    auto flags = io::loadAls(savedFlags);
    REQUIRE(flags);
    const auto& flagged = *flags.song->tracks.back().slots[2].clip;
    CHECK(flagged.ram);
    CHECK(! flagged.hiQ);
    CHECK(! flagged.clipFade);

    plan.command.revert(song);
    CHECK(song.tracks.size() == tracksBefore);
    CHECK(song.scenes.size() == scenesBefore);
    for (const auto& track : song.tracks) CHECK(track.slots.size() == scenesBefore);
    folder.deleteRecursively();
}

TEST_CASE("sample drop: a MIDI target gets a new audio track after it; unreadable files are refused")
{
    auto song = io::parseAls(*io::createNewSetDom(), "/tmp/sonora_sample_drop.als").song;
    REQUIRE(song != nullptr);
    const int midi = midiTrackIndex(*song);
    REQUIRE(midi >= 0);
    const std::vector<io::DroppedSample> good{{"/tmp/A.wav", 48000.0, 48000}};
    auto onMidi = io::placeSamples(*song, good, {midi, 2.0, -1, false});
    REQUIRE(onMidi.command.apply != nullptr);
    CHECK(onMidi.newTrackForKind);
    CHECK(onMidi.firstTrack == midi + 1);
    {
        auto copy = *song;
        const auto tracksBefore = copy.tracks.size();
        onMidi.command.apply(copy);
        REQUIRE(copy.tracks.size() == tracksBefore + 1);
        CHECK(copy.tracks[(size_t) midi].kind == model::TrackKind::Midi); // the MIDI track is untouched
        CHECK(copy.tracks[(size_t) midi + 1].kind == model::TrackKind::Audio);
        REQUIRE(copy.tracks[(size_t) midi + 1].arrangementClips.size() == 1);
        CHECK_NEAR(copy.tracks[(size_t) midi + 1].arrangementClips[0].currentStart, 2.0, 1e-9);
    }
    auto unreadable = io::placeSamples(*song, {{"/tmp/x.m4a", 0.0, 0}}, {audioTrackIndex(*song), 0.0, -1, false});
    CHECK(unreadable.command.apply == nullptr);
    CHECK(unreadable.error == "Could not read the audio file");
    CHECK(io::placeSamples(*song, good, {(int) song->tracks.size() + 3, 0.0, -1, false}).command.apply == nullptr);
}
