// TRK-1: hidden and deactivated tracks (edits, <SonoraTrack> persistence, empty engine nodes, view filtering).
#include "TestFramework.h"
#include "core/Edit.h"
#include "engine/AudioCapture.h"
#include "engine/AudioEngine.h"
#include "engine/ClipRender.h"
#include "engine/SampleExport.h"
#include "engine/StripSilence.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/als/SampleDrop.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <numbers>
#include <set>

using namespace sonora;

namespace
{
struct TrackStateRig
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-track-state-" + juce::Uuid().toString());
    std::unique_ptr<model::Song> song;
    TrackStateRig()
    {
        REQUIRE(folder.createDirectory()); auto root = io::createNewSetDom(); std::string error;
        REQUIRE(io::writeAlsDom(*root, folder.getChildFile("Source.als"), error));
        auto loaded = io::loadAls(folder.getChildFile("Source.als")); REQUIRE(loaded); song = std::move(loaded.song);
        REQUIRE(song->tracks.size() >= 3); REQUIRE(! song->returns.empty());
    }
    ~TrackStateRig() { folder.deleteRecursively(); }
    model::Clip tone()
    {
        juce::AudioBuffer<float> buffer(2, 44100);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch) buffer.setSample(ch, i, (float) (0.5 * std::sin(2 * std::numbers::pi * 220 * i / 44100)));
        const auto file = folder.getChildFile("Tone.wav");
        REQUIRE(engine::writeCroppedWav(buffer, 44100, 0, buffer.getNumSamples(), file).empty());
        return io::makeSampleClip(*song, {file.getFullPathName().toStdString(), 44100, buffer.getNumSamples()}, 16, 0);
    }
    int audioTrack() const
    {
        for (size_t i = 0; i < song->tracks.size(); ++i) if (song->tracks[i].kind == model::TrackKind::Audio) return (int) i;
        return -1;
    }
};
} // namespace

TEST_CASE("track state: hide and deactivate are undoable, cover returns and skip Main")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int ret = (int) s.tracks.size(), main = ret + (int) s.returns.size();
    model::EditHistory history;
    auto hide = model::setTracksHidden(s, {0, ret, main}, true); REQUIRE(hide.apply);
    CHECK(! hide.structureChanged);
    history.perform(s, std::move(hide));
    CHECK(s.tracks[0].hidden); CHECK(s.returns[0].hidden); CHECK(! s.main.hidden); CHECK(! s.tracks[1].hidden);
    CHECK(! model::setTracksHidden(s, {0}, true).apply); // already hidden
    REQUIRE(history.undo(s)); CHECK(! s.tracks[0].hidden); CHECK(! s.returns[0].hidden);
    REQUIRE(history.redo(s)); CHECK(s.tracks[0].hidden);

    auto off = model::setTracksDeactivated(s, {1}, true); REQUIRE(off.apply);
    CHECK(off.structureChanged); // the engine rebuilds the graph so devices unload
    history.perform(s, std::move(off)); CHECK(s.tracks[1].deactivated);
    REQUIRE(history.undo(s)); CHECK(! s.tracks[1].deactivated);
}

TEST_CASE("track state: deactivating a group deactivates its descendants")
{
    TrackStateRig rig; auto& s = *rig.song;
    s.tracks[0].kind = model::TrackKind::Group;
    s.tracks[1].groupId = s.tracks[0].id;
    s.tracks[2].groupId = s.tracks[1].id; // nested below a member
    auto off = model::setTracksDeactivated(s, {0}, true); REQUIRE(off.apply); off.apply(s);
    CHECK(s.tracks[0].deactivated); CHECK(s.tracks[1].deactivated); CHECK(s.tracks[2].deactivated);
    off.revert(s);
    CHECK(! s.tracks[0].deactivated); CHECK(! s.tracks[1].deactivated); CHECK(! s.tracks[2].deactivated);
    // hiding a group only flags the group: the views hide members through it
    auto hide = model::setTracksHidden(s, {0}, true); hide.apply(s);
    CHECK(s.tracks[0].hidden); CHECK(! s.tracks[1].hidden);
}

TEST_CASE("track state: SonoraTrack round-trips through the .als and disappears when cleared")
{
    TrackStateRig rig; auto& s = *rig.song;
    s.tracks[0].hidden = true; s.tracks[1].deactivated = true; s.returns[0].hidden = s.returns[0].deactivated = true;
    const auto saved = rig.folder.getChildFile("Flags.als"); REQUIRE(io::saveAls(s, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload);
    CHECK(reload.song->tracks[0].hidden); CHECK(! reload.song->tracks[0].deactivated);
    CHECK(reload.song->tracks[1].deactivated); CHECK(! reload.song->tracks[1].hidden);
    CHECK(reload.song->returns[0].hidden); CHECK(reload.song->returns[0].deactivated);
    CHECK(! reload.song->tracks[2].hidden); CHECK(! reload.song->tracks[2].deactivated);

    auto& again = *reload.song;
    again.tracks[0].hidden = false; again.tracks[1].deactivated = false; again.returns[0].hidden = again.returns[0].deactivated = false;
    const auto cleared = rig.folder.getChildFile("Cleared.als"); REQUIRE(io::saveAls(again, cleared).ok);
    juce::MemoryBlock gz; REQUIRE(cleared.loadFileAsData(gz));
    juce::MemoryInputStream raw(gz, false); juce::GZIPDecompressorInputStream xml(&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
    CHECK(! xml.readEntireStreamAsString().contains("SonoraTrack"));
    auto clean = io::loadAls(cleared); REQUIRE(clean);
    for (const auto& t : clean.song->tracks) { CHECK(! t.hidden); CHECK(! t.deactivated); }
}

TEST_CASE("track state: a deactivated track is built without clips or devices and is never audible")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int index = rig.audioTrack(); REQUIRE(index >= 0);
    s.tracks[(size_t) index].arrangementClips.push_back(rig.tone());
    model::Device utility; utility.type = "StereoGain"; utility.id = 9001;
    s.tracks[(size_t) index].devices.push_back(utility);
    const auto deviceCount = s.tracks[(size_t) index].devices.size();

    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi);
    engine.loadSong(s);
    const auto* graph = engine.latestGraph(); REQUIRE(graph);
    CHECK(! graph->nodes[(size_t) index].clips.empty());
    CHECK(graph->nodes[(size_t) index].devices.size() == deviceCount);
    CHECK(engine::isTrackAudible(*graph, *graph->controls, graph->nodes[(size_t) index], false));

    model::setTracksDeactivated(s, {index}, true).apply(s);
    engine.loadSong(s);
    graph = engine.latestGraph(); REQUIRE(graph);
    const auto& node = graph->nodes[(size_t) index];
    CHECK(node.deactivated); CHECK(node.clips.empty()); CHECK(node.devices.empty()); CHECK(node.processorForModelDevice.empty());
    for (char has : node.hasSessionClip) CHECK(has == 0);
    CHECK(! engine::isTrackAudible(*graph, *graph->controls, node, false));
    CHECK(node.sendEnabled.size() == s.tracks[(size_t) index].mixer.sends.size());
    // the model keeps everything for reactivation
    CHECK(s.tracks[(size_t) index].devices.size() == deviceCount); CHECK(s.tracks[(size_t) index].arrangementClips.size() == 1);
    // clip edits on a deactivated track publish an empty set too
    engine.updateTrackClips(index, s, false);
    CHECK((*graph->controls)[(size_t) node.controlIndex].active.load()); // the activator itself is untouched
}

TEST_CASE("return tracks: inserting adds a -inf send everywhere, a SendPreBool, and undoes cleanly after saving")
{
    TrackStateRig rig; auto& s = *rig.song;
    const auto tracks = s.tracks.size(), returns = s.returns.size();
    auto insert = io::createReturnTrack(s);
    CHECK(insert.track.kind == model::TrackKind::Return);
    CHECK(insert.track.name == std::string(1, (char) ('A' + returns)) + "-Return");
    REQUIRE(insert.holders.size() == tracks + returns);
    for (size_t i = 0; i < tracks; ++i) { REQUIRE(insert.holders[i]); CHECK(insert.holders[i]->find("EnabledByUser Value=\"true\"") != std::string::npos); }
    CHECK(insert.pointeeEnd > s.nextPointeeId);
    model::EditHistory history;
    auto command = model::insertReturnTrack(s, insert); REQUIRE(command.apply); CHECK(command.structureChanged);
    history.perform(s, std::move(command));
    REQUIRE(s.returns.size() == returns + 1);
    for (const auto& t : s.tracks)
    {
        REQUIRE(t.mixer.sends.size() == returns + 1); CHECK(t.mixer.sends.back() < 0.001);
        CHECK(t.mixer.sendTargetIds.back() >= 0); CHECK(t.mixer.sendMidi.size() == returns + 1);
    }
    CHECK(s.sendPreXml.size() == returns + 1);
    // send automation ids are unique across the set
    std::set<int> ids;
    for (const auto& t : s.tracks) for (int id : t.mixer.sendTargetIds) CHECK(ids.insert(id).second);

    const auto saved = rig.folder.getChildFile("Returns.als"); REQUIRE(io::saveAls(s, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload);
    REQUIRE(reload.song->returns.size() == returns + 1);
    CHECK(reload.song->returns.back().name == insert.track.name);
    for (const auto& t : reload.song->tracks) CHECK(t.mixer.sends.size() == returns + 1);
    CHECK(reload.song->sendPreXml.size() == returns + 1);
    for (size_t i = 0; i < tracks; ++i)
        CHECK(reload.song->tracks[i].mixer.sendTargetIds.back() == s.tracks[i].mixer.sendTargetIds.back());

    // a second insert on the saved set chains automation ids after the first
    auto second = io::createReturnTrack(*reload.song); CHECK(second.pointeeEnd > insert.pointeeEnd);

    REQUIRE(history.undo(s));
    CHECK(s.returns.size() == returns);
    for (const auto& t : s.tracks) CHECK(t.mixer.sends.size() == returns);
    const auto undone = rig.folder.getChildFile("Undone.als"); REQUIRE(io::saveAls(s, undone).ok);
    auto back = io::loadAls(undone); REQUIRE(back);
    CHECK(back.song->returns.size() == returns);
    for (const auto& t : back.song->tracks) CHECK(t.mixer.sends.size() == returns);
    CHECK(back.song->sendPreXml.size() == returns);
}

TEST_CASE("return tracks: the engine builds a node and a send per new return")
{
    TrackStateRig rig; auto& s = *rig.song;
    const auto returns = s.returns.size();
    model::insertReturnTrack(s, io::createReturnTrack(s)).apply(s);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi);
    engine.loadSong(s);
    const auto* graph = engine.latestGraph(); REQUIRE(graph);
    CHECK(graph->returnNodes.size() == returns + 1);
    CHECK(graph->nodes[0].sendEnabled.size() == returns + 1);
    CHECK(! graph->nodes[0].sendEnabled.back()); // -inf
}


TEST_CASE("return tracks: inserting stops at 12 returns")
{
    TrackStateRig rig; auto& s = *rig.song;
    while (s.returns.size() < model::maxReturnTracks)
    {
        auto command = model::insertReturnTrack(s, io::createReturnTrack(s)); REQUIRE(command.apply); command.apply(s);
    }
    CHECK(s.returns.back().name == "L-Return");
    CHECK(! model::insertReturnTrack(s, io::createReturnTrack(s)).apply);
    for (const auto& t : s.tracks) CHECK(t.mixer.sends.size() == model::maxReturnTracks);
}

TEST_CASE("strip silence: sound regions bridge short gaps, drop blips and pad edges")
{
    const double rate = 44100;
    juce::AudioBuffer<float> audio(2, (int) (rate * 4)); audio.clear();
    auto burst = [&](double from, double to, float level) {
        for (int i = (int) (from * rate); i < (int) (to * rate); ++i)
            for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, i, level * (float) std::sin(2 * std::numbers::pi * 440 * i / rate));
    };
    burst(0.5, 1.0, 0.5f);
    burst(1.1, 1.4, 0.5f);   // 100 ms gap: bridged into the first region
    burst(2.0, 2.005, 0.5f); // 5 ms blip: dropped
    burst(3.0, 3.5, 0.002f); // -54 dB: silence at the default -40 dB
    const auto regions = engine::findSoundRegions(audio, rate);
    REQUIRE(regions.size() == 1);
    CHECK_NEAR(regions[0].first / rate, 0.5 - 0.005, 0.011);
    CHECK_NEAR(regions[0].second / rate, 1.4 + 0.05, 0.011);
    engine::StripSilenceOptions quiet; quiet.thresholdDb = -60;
    CHECK(engine::findSoundRegions(audio, rate, quiet).size() == 2);
    juce::AudioBuffer<float> empty(2, 1000); empty.clear();
    CHECK(engine::findSoundRegions(empty, rate).empty());
}

TEST_CASE("strip silence: transient stepping finds neighbours and stays put at the ends")
{
    const std::vector<double> marks{0, 1, 2.5, 4};
    CHECK_NEAR(engine::stepToMarker(marks, 1, 1), 2.5, 1e-12);
    CHECK_NEAR(engine::stepToMarker(marks, 1, -1), 0, 1e-12);
    CHECK_NEAR(engine::stepToMarker(marks, 1.2, -1), 1, 1e-12);
    CHECK_NEAR(engine::stepToMarker(marks, 4, 1), 4, 1e-12);
    CHECK_NEAR(engine::stepToMarker(marks, 0, -1), 0, 1e-12);
}

TEST_CASE("strip silence: the clip edit keeps regions as trimmed fragments and undoes")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int index = rig.audioTrack(); REQUIRE(index >= 0);
    auto clip = rig.tone(); clip.uid = model::allocateClipUid();
    s.tracks[(size_t) index].arrangementClips.push_back(clip);
    const auto& original = s.tracks[(size_t) index].arrangementClips.back();
    const double start = original.currentStart, end = original.currentEnd, offset = original.loop.startRelative;
    REQUIRE(end - start > 1.0);
    model::EditHistory history;
    auto keep = model::stripArrangementClip(s, index, clip.uid, {{start + 0.25, start + 0.5}, {start + 0.75, end + 3}});
    REQUIRE(keep.apply);
    history.perform(s, std::move(keep));
    const auto& clips = s.tracks[(size_t) index].arrangementClips;
    REQUIRE(clips.size() == 2);
    CHECK_NEAR(clips[0].currentStart, start + 0.25, 1e-9); CHECK_NEAR(clips[0].currentEnd, start + 0.5, 1e-9);
    CHECK_NEAR(clips[1].currentStart, start + 0.75, 1e-9); CHECK_NEAR(clips[1].currentEnd, end, 1e-9); // clamped to the clip
    CHECK(clips[0].loop.startRelative > offset); // content stays where it was in time
    CHECK(clips[0].uid != clips[1].uid);
    REQUIRE(history.undo(s));
    REQUIRE(s.tracks[(size_t) index].arrangementClips.size() == 1);
    CHECK_NEAR(s.tracks[(size_t) index].arrangementClips[0].currentStart, start, 1e-9);
    CHECK(! model::stripArrangementClip(s, index, clip.uid, {{start - 1, end + 1}}).apply); // nothing to strip
    auto all = model::stripArrangementClip(s, index, clip.uid, {}); REQUIRE(all.apply); all.apply(s);
    CHECK(s.tracks[(size_t) index].arrangementClips.empty());
}

TEST_CASE("strip silence: a rendered clip with a silent gap splits at the gap in song beats")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int index = rig.audioTrack(); REQUIRE(index >= 0);
    const double rate = 44100;
    juce::AudioBuffer<float> buffer(2, (int) (rate * 2)); buffer.clear();
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const double t = i / rate;
        if (t < 0.5 || t >= 1.5)
            for (int ch = 0; ch < 2; ++ch) buffer.setSample(ch, i, (float) (0.5 * std::sin(2 * std::numbers::pi * 220 * t)));
    }
    const auto file = rig.folder.getChildFile("Gap.wav");
    REQUIRE(engine::writeCroppedWav(buffer, rate, 0, buffer.getNumSamples(), file).empty());
    auto clip = io::makeSampleClip(s, {file.getFullPathName().toStdString(), rate, buffer.getNumSamples()}, 16, 4);
    clip.uid = model::allocateClipUid();
    s.tracks[(size_t) index].arrangementClips.push_back(clip);
    engine::SampleCache cache; const std::atomic<bool> cancel{false};
    auto rendered = engine::renderClipRange(s, {clip}, clip.currentStart, clip.currentEnd, rate, cache, cancel);
    REQUIRE(rendered.sample);
    const double beatsPerSample = s.tempo / 60.0 / rate;
    std::vector<std::pair<model::Beats, model::Beats>> keep;
    for (const auto& [a, b] : engine::findSoundRegions(rendered.sample->audio, rate))
        keep.push_back({clip.currentStart + a * beatsPerSample, clip.currentStart + b * beatsPerSample});
    REQUIRE(keep.size() == 2);
    const double beatsPerSecond = s.tempo / 60.0;
    CHECK_NEAR(keep[0].first, clip.currentStart, 0.02);
    CHECK_NEAR(keep[0].second, clip.currentStart + (0.5 + 0.05) * beatsPerSecond, 0.03);
    CHECK_NEAR(keep[1].first, clip.currentStart + (1.5 - 0.005) * beatsPerSecond, 0.03);
    auto command = model::stripArrangementClip(s, index, clip.uid, keep); REQUIRE(command.apply); command.apply(s);
    CHECK(s.tracks[(size_t) index].arrangementClips.size() == 2);
    // the second fragment still plays the second burst: render it and expect sound right at its start
    const auto& second = s.tracks[(size_t) index].arrangementClips[1];
    auto tail = engine::renderClipRange(s, {second}, second.currentStart, second.currentEnd, rate, cache, cancel); REQUIRE(tail.sample);
    CHECK(tail.sample->audio.getMagnitude(0, (int) (0.01 * rate), (int) (0.05 * rate)) > 0.3f);
}

TEST_CASE("capture audio: the ring keeps the last minute of each enabled input and reads it oldest first")
{
    engine::AudioCapture capture;
    const double rate = 1000; // small ring: 60 000 frames
    capture.prepare({true, false, true}, rate);
    CHECK(capture.keeps(0)); CHECK(! capture.keeps(1)); CHECK(capture.keeps(2)); CHECK(! capture.keeps(3));
    juce::AudioBuffer<float> out; double sr = 0;
    CHECK(! capture.read(0, false, out, sr)); // nothing written yet
    std::vector<float> a(512), c(512);
    int64_t frame = 0;
    const int blocks = 130; // 66 560 frames: wraps the 60 000-frame ring
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i) { a[(size_t) i] = (float) (frame + i); c[(size_t) i] = -(float) (frame + i); }
        const float* inputs[3] = {a.data(), nullptr, c.data()};
        capture.write(inputs, 3, 512);
        frame += 512;
    }
    REQUIRE(capture.read(0, false, out, sr));
    CHECK_NEAR(sr, rate, 0);
    REQUIRE(out.getNumSamples() == 60000);
    CHECK_NEAR(out.getSample(0, 0), (float) (frame - 60000), 0.5); // oldest kept frame
    CHECK_NEAR(out.getSample(0, 59999), (float) (frame - 1), 0.5); // newest
    CHECK_NEAR(out.getSample(1, 0), out.getSample(0, 0), 0); // mono read duplicates the channel
    REQUIRE(capture.read(2, false, out, sr));
    CHECK_NEAR(out.getSample(0, 59999), -(float) (frame - 1), 0.5);
    CHECK(! capture.read(0, true, out, sr)); // physical 1 is not kept, so 0+1 is not a stereo pair
    capture.prepare({}, rate);
    CHECK(! capture.keeps(0));
}

TEST_CASE("capture audio: the latest passage ends at a long silence and skips older takes")
{
    const double rate = 8000;
    juce::AudioBuffer<float> audio(2, (int) (rate * 40)); audio.clear();
    auto play = [&](double from, double to) {
        for (int i = (int) (from * rate); i < (int) (to * rate); ++i)
            for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, i, 0.3f * (float) std::sin(2 * std::numbers::pi * 200 * i / rate));
    };
    play(2, 6);      // an older take
    play(20, 24);    // 14 s later: a new passage...
    play(27, 30);    // ...continued after a 3 s pause (shorter than 8 s)
    const auto [first, end] = engine::latestPassage(audio, rate);
    CHECK_NEAR(first / rate, 20 - 0.05, 0.02);
    CHECK_NEAR(end / rate, 30 + 0.3, 0.02);
    juce::AudioBuffer<float> silent(2, 8000); silent.clear();
    const auto none = engine::latestPassage(silent, rate);
    CHECK(none.second <= none.first);
}

namespace
{
int midiTrackWithClip(TrackStateRig& rig)
{
    auto& s = *rig.song;
    s.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    for (int i = 0; i < (int) s.tracks.size(); ++i)
        if (s.tracks[(size_t) i].kind == model::TrackKind::Midi)
        {
            model::Clip clip; REQUIRE(model::makeEmptyMidiClip(s, 0, 4, clip));
            clip.uid = model::allocateClipUid();
            clip.notes = {{0, 1, 60}, {1, 1, 64}, {2, 1, 67}};
            s.tracks[(size_t) i].arrangementClips.push_back(clip);
            return i;
        }
    return -1;
}
} // namespace

TEST_CASE("linked clips: duplicate as linked, propagate note edits in one undo step, unlink")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int t = midiTrackWithClip(rig); REQUIRE(t >= 0);
    const int first = s.tracks[(size_t) t].arrangementClips[0].uid;
    model::EditHistory history;
    auto apply = [&](model::EditCommand c) { // mirrors AppController::applyEdit
        const auto linked = model::snapshotLinkedNotes(s);
        history.perform(s, std::move(c));
        if (auto sync = model::syncLinkedClips(s, linked); sync.apply) history.amendLast(s, std::move(sync));
    };
    apply(model::duplicateClipLinked(s, t, first));
    auto& clips = s.tracks[(size_t) t].arrangementClips;
    REQUIRE(clips.size() == 2);
    const int group = clips[0].linkGroup;
    CHECK(group >= 0); CHECK(clips[1].linkGroup == group);
    CHECK_NEAR(clips[1].currentStart, clips[0].currentEnd, 1e-9);
    apply(model::duplicateClipLinked(s, t, clips[1].uid)); // a member's duplicate joins the same group
    REQUIRE(clips.size() == 3); CHECK(clips[2].linkGroup == group);
    CHECK(model::linkedClipLocations(s, group).size() == 3);

    auto edited = clips[1].notes; edited.push_back({3, 0.5, 72});
    apply(model::setNotes(s, t, clips[1].uid, edited));
    for (const auto& clip : clips) { REQUIRE(clip.notes.size() == 4); CHECK(clip.notes[3].pitch == 72); }
    REQUIRE(history.undo(s)); // one step reverts the edit and its propagation
    for (const auto& clip : clips) CHECK(clip.notes.size() == 3);
    REQUIRE(history.redo(s));
    for (const auto& clip : clips) CHECK(clip.notes.size() == 4);

    apply(model::unlinkClip(s, model::ClipLocation::arrangement(t, clips[2].uid)));
    CHECK(clips[2].linkGroup == -1);
    auto variation = clips[2].notes; variation.pop_back();
    apply(model::setNotes(s, t, clips[2].uid, variation));
    CHECK(clips[2].notes.size() == 3); CHECK(clips[0].notes.size() == 4); // the variation no longer drives the group

    apply(model::duplicateClip(s, t, clips[0].uid, 100)); // an ordinary copy is independent
    CHECK(clips.back().linkGroup == -1);
}

TEST_CASE("linked clips: the link survives save and reload, audio clips cannot be linked")
{
    TrackStateRig rig; auto& s = *rig.song;
    const int t = midiTrackWithClip(rig); REQUIRE(t >= 0);
    model::duplicateClipLinked(s, t, s.tracks[(size_t) t].arrangementClips[0].uid).apply(s);
    const auto saved = rig.folder.getChildFile("Linked.als"); REQUIRE(io::saveAls(s, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload);
    const auto& clips = reload.song->tracks[(size_t) t].arrangementClips;
    REQUIRE(clips.size() == 2);
    CHECK(clips[0].linkGroup >= 0); CHECK(clips[0].linkGroup == clips[1].linkGroup);
    // unlinking and saving removes the element again
    auto& again = *reload.song;
    model::unlinkClip(again, model::ClipLocation::arrangement(t, clips[0].uid)).apply(again);
    model::unlinkClip(again, model::ClipLocation::arrangement(t, clips[1].uid)).apply(again);
    const auto cleared = rig.folder.getChildFile("Unlinked.als"); REQUIRE(io::saveAls(again, cleared).ok);
    auto back = io::loadAls(cleared); REQUIRE(back);
    for (const auto& clip : back.song->tracks[(size_t) t].arrangementClips) CHECK(clip.linkGroup == -1);

    const int audio = rig.audioTrack(); REQUIRE(audio >= 0);
    auto tone = rig.tone(); tone.uid = model::allocateClipUid(); s.tracks[(size_t) audio].arrangementClips.push_back(tone);
    CHECK(! model::duplicateClipLinked(s, audio, tone.uid).apply);
}
