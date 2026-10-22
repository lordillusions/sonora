#include "TestFramework.h"
#include "core/ClipOperations.h"
#include "core/TimeUtils.h"
#include "engine/AudioToMidi.h"
#include "engine/ClipRender.h"
#include "engine/Exporter.h"
#include "engine/PeakCache.h"
#include "engine/SampleExport.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/als/SampleDrop.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include <numbers>
using namespace sonora;
namespace
{
struct MediaRig
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-media-" + juce::Uuid().toString());
    std::unique_ptr<model::Song> song;
    MediaRig()
    {
        REQUIRE(folder.createDirectory()); auto root = io::createNewSetDom(); std::string error;
        REQUIRE(io::writeAlsDom(*root, folder.getChildFile("Source.als"), error));
        auto loaded = io::loadAls(folder.getChildFile("Source.als")); REQUIRE(loaded); song = std::move(loaded.song);
        song->midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    }
    ~MediaRig() { folder.deleteRecursively(); }
    model::Clip audio(double at = 0, double seconds = 1)
    {
        juce::AudioBuffer<float> buffer(2, (int) (seconds * 44100));
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch) buffer.setSample(ch, i, (float) (0.5 * std::sin(2 * std::numbers::pi * 220 * i / 44100)));
        const auto file = folder.getChildFile("Tone.wav");
        REQUIRE(engine::writeCroppedWav(buffer, 44100, 0, buffer.getNumSamples(), file).empty());
        return io::makeSampleClip(*song, {file.getFullPathName().toStdString(), 44100, buffer.getNumSamples()}, 16, at);
    }
};
}

TEST_CASE("media edits: precise peaks preserve sub-bucket samples and channel polarity")
{
    auto source = std::make_shared<engine::SampleData>(); source->sampleRate = 1000;
    source->audio.setSize(2, 128); source->audio.clear(); source->audio.setSample(0, 20, 1);
    source->audio.setSample(1, 20, -0.5f);
    const auto peaks = engine::computePeaks(source->audio, 1000, source); float lo = 0, hi = 0;
    REQUIRE(peaks->range(0.001, 0.003, lo, hi, 0)); CHECK_NEAR(hi, 0, 1e-8);
    REQUIRE(peaks->range(0.020, 0.020, lo, hi, 0)); CHECK_NEAR(lo, 1, 1e-8); CHECK_NEAR(hi, 1, 1e-8);
    REQUIRE(peaks->range(0.020, 0.020, lo, hi, 1)); CHECK_NEAR(hi, -0.5, 1e-8);
}
TEST_CASE("media edits: fade skew is monotone and survives authoring save reload")
{
    for (double shape : {-0.95, -0.6, 0.0, 0.6, 0.95})
    {
        double before = 0;
        for (int step = 0; step <= 100; ++step) { const auto gain = model::clipFadeGain(step / 100.0, shape); CHECK(gain >= before); before = gain; }
        CHECK_NEAR(model::clipFadeGain(0, shape), 0, 1e-12); CHECK_NEAR(model::clipFadeGain(1, shape), 1, 1e-12);
    }
    CHECK(model::clipFadeGain(0.5, -0.6) > model::clipFadeGain(0.5, 0.6));
    MediaRig rig; auto clip = rig.audio(); clip.fadeIn = 0.5; clip.fadeOut = 1; clip.fadeInCurve = -0.4; clip.fadeOutCurve = 0.7;
    model::insertClip(2, clip, "Audio").apply(*rig.song);
    const auto saved = rig.folder.getChildFile("Fades.als"); REQUIRE(io::saveAls(*rig.song, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload); const auto& found = reload.song->tracks[2].arrangementClips.front();
    CHECK_NEAR(found.fadeInCurve, -0.4, 1e-9); CHECK_NEAR(found.fadeOutCurve, 0.7, 1e-9);
}
TEST_CASE("media edits: MIDI consolidation preserves gaps loops trimmed notes and one undo")
{
    MediaRig rig; model::Clip first; REQUIRE(model::makeEmptyMidiClip(*rig.song, 4, 4, first));
    first.loop = {0, 2, 0, true}; first.notes = {{0, 0.5, 60}, {1, 1, 64}};
    model::insertClip(0, first, "Add").apply(*rig.song);
    auto second = first; second.currentStart = second.time = 12; second.currentEnd = 16; second.loop.enabled = false; second.notes = {{1, 1, 67}};
    model::insertClip(0, second, "Add").apply(*rig.song);
    const auto merged = model::consolidatedMidi(*rig.song, 0, 4.25, 15);
    REQUIRE(merged.notes.size() == 5); CHECK_NEAR(merged.notes.front().start, 0, 1e-9); CHECK_NEAR(merged.notes.front().duration, 0.25, 1e-9);
    CHECK_NEAR(merged.notes.back().start, 8.75, 1e-9);
    model::EditHistory history; history.perform(*rig.song, model::replaceArrangementRange(*rig.song, 0, 4.25, 15, merged));
    REQUIRE(rig.song->tracks[0].arrangementClips.size() == 3);
    CHECK_NEAR(rig.song->tracks[0].arrangementClips[0].currentEnd, 4.25, 1e-9);
    CHECK_NEAR(rig.song->tracks[0].arrangementClips[2].currentStart, 15, 1e-9);
    REQUIRE(history.undo(*rig.song)); CHECK(rig.song->tracks[0].arrangementClips.size() == 2);
    REQUIRE(history.redo(*rig.song));
    REQUIRE(io::saveAls(*rig.song, rig.folder.getChildFile("Consolidated.als")).ok);
    auto reload = io::loadAls(rig.folder.getChildFile("Consolidated.als")); REQUIRE(reload);
    CHECK(reload.song->tracks[0].arrangementClips[1].notes.size() == 5);
}
TEST_CASE("media edits: audio consolidation renders gain fades and silence between clips")
{
    MediaRig rig; auto clip = rig.audio(0); clip.gain = 0.5; clip.fadeOut = 0.5;
    auto next = model::movedClip(clip, 4); std::atomic<bool> cancel{false}; engine::SampleCache cache;
    auto result = engine::renderClipRange(*rig.song, {clip, next}, 0, 6, 44100, cache, cancel); REQUIRE(result.sample);
    const auto& data = result.sample->audio; CHECK(data.getMagnitude(0, 22050) > 0.2f); CHECK(data.getMagnitude(44100, 44100) < 1e-5f);
    CHECK(data.getMagnitude(88200, 22050) > 0.2f);
    cancel = true; CHECK(! engine::renderClipRange(*rig.song, {clip}, 0, 2, 44100, cache, cancel).sample);
}
TEST_CASE("media edits: audio to MIDI detects melody harmony silence cancellation and slices")
{
    constexpr int rate = 22050;
    juce::AudioBuffer<float> audio(2, rate * 2); audio.clear();
    for (int i = 0; i < rate * 2; ++i)
    {
        const int pitch = i < rate ? 60 : 64;
        const double frequency = 440 * std::pow(2.0, (pitch - 69) / 12.0);
        const float value = (float) (0.4 * std::sin(2 * std::numbers::pi * frequency * i / rate));
        for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, i, value);
    }
    std::atomic<bool> cancel{false}; std::atomic<float> progress{0};
    auto melody = engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Melody, cancel, progress);
    CHECK(std::any_of(melody.notes.begin(), melody.notes.end(), [](const auto& n) { return n.pitch == 60 && n.duration > 1.5; }));
    CHECK(std::any_of(melody.notes.begin(), melody.notes.end(), [](const auto& n) { return n.pitch == 64 && n.duration > 1.5; }));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        double value = 0;
        for (int pitch : {60, 64, 67}) value += 0.2 * std::sin(2 * std::numbers::pi * 440 * std::pow(2.0, (pitch - 69) / 12.0) * i / rate);
        for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, i, (float) value);
    }
    auto harmony = engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Harmony, cancel, progress);
    for (int pitch : {60, 64, 67}) CHECK(std::any_of(harmony.notes.begin(), harmony.notes.end(), [pitch](const auto& n) { return n.pitch == pitch && n.duration > 1; }));
    cancel = true; CHECK(engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Melody, cancel, progress).cancelled);
    cancel = false; audio.clear();
    CHECK(engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Harmony, cancel, progress).notes.empty());
    for (int hit = 0; hit < 3; ++hit) for (int i = 0; i < rate / 5; ++i)
    {
        const float value = (float) (0.8 * std::exp(-25.0 * i / rate) * std::sin(2 * std::numbers::pi * 70 * i / rate));
        for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, hit * rate / 2 + i, value);
    }
    auto drums = engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Drums, cancel, progress);
    REQUIRE(drums.notes.size() >= 3); CHECK(drums.notes[0].pitch == 36);
    auto slices = engine::audioToMidi(audio, rate, 120, engine::AudioToMidiMode::Slices, cancel, progress);
    REQUIRE(slices.notes.size() >= 3); CHECK(slices.notes[0].pitch == 36); CHECK(slices.notes[1].pitch == 37);
}
TEST_CASE("media edits: pre-mixer bounce includes devices once and frozen playback retains mixer and original data")
{
    MediaRig rig; auto clip = rig.audio(); model::insertClip(2, clip, "Audio").apply(*rig.song);
    auto& track = rig.song->tracks[2]; track.mixer.volume = 0.25; track.mixer.pan = 0.5;
    model::Device utility; utility.type = "StereoGain"; utility.id = 700; model::Parameter gain; gain.path = "Gain"; gain.value = 0.5; utility.parameters.push_back(gain);
    utility.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(utility)); track.devices.push_back(utility);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi); engine.loadSong(*rig.song);
    REQUIRE(engine.sampleCache().waitForLoads(30000));
    REQUIRE(engine.beginOffline()); engine::Exporter exporter(engine, *rig.song);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    engine::ExportOptions options; options.target = engine::ExportTarget::SingleTrack; options.trackIndex = 2;
    options.preMixer = true; options.startBeat = 0; options.endBeat = 2; options.bitDepth = 32; options.dither = false;
    options.destination = rig.folder.getChildFile("Freeze.wav");
    auto result = exporter.run(options, progress, cancel); REQUIRE(result.ok); engine.endOffline();
    engine::SampleCache samples; auto rendered = samples.load(options.destination.getFullPathName().toStdString()); REQUIRE(rendered);
    CHECK_NEAR(rendered->audio.getMagnitude(10000, 10000), 0.25, 0.01); // neither fader nor pan is baked
    auto frozen = track; frozen.frozen = true;
    frozen.frozenArrangement = {io::makeSampleClip(*rig.song, {options.destination.getFullPathName().toStdString(), rendered->sampleRate, rendered->audio.getNumSamples()}, 16, 0)};
    model::EditHistory history; history.perform(*rig.song, model::replaceTrack(*rig.song, 2, frozen, "Freeze Track"));
    engine.loadSong(*rig.song); const auto* graph = engine.latestGraph(); REQUIRE(graph);
    CHECK(graph->nodes[2].devices.empty()); CHECK(graph->nodes[2].clips.size() == 1);
    CHECK(rig.song->tracks[2].devices.size() == 1); CHECK_NEAR((*graph->controls)[2].volume.load(), 0.25, 1e-6);
    const auto saved = rig.folder.getChildFile("Frozen.als"); REQUIRE(io::saveAls(*rig.song, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload); CHECK(reload.song->tracks[2].frozen); CHECK(reload.song->tracks[2].devices.size() == 1);
    CHECK(reload.song->tracks[2].frozenArrangement.size() == 1);
    REQUIRE(history.undo(*rig.song)); CHECK(! rig.song->tracks[2].frozen); CHECK(rig.song->tracks[2].devices.size() == 1);
    REQUIRE(history.redo(*rig.song)); CHECK(rig.song->tracks[2].frozen);
}
TEST_CASE("media edits: replacing MIDI with bounced audio changes XML track type and undo restores it")
{
    MediaRig rig; int end = 0; auto audio = io::createAudioTrack(*rig.song, "Bounced", 16, rig.song->tracks[0].id, -1, end);
    audio.arrangementClips.push_back(rig.audio()); audio.arrangementClips[0].uid = model::allocateClipUid();
    model::EditHistory history; history.perform(*rig.song, model::replaceTrack(*rig.song, 0, audio, "Bounce Track in Place", end));
    CHECK(rig.song->nextPointeeId >= end);
    auto save = rig.folder.getChildFile("Bounce.als"); REQUIRE(io::saveAls(*rig.song, save).ok);
    auto reload = io::loadAls(save); REQUIRE(reload); CHECK(reload.song->tracks[0].kind == model::TrackKind::Audio); CHECK(reload.song->tracks[0].arrangementClips.size() == 1);
    REQUIRE(history.undo(*rig.song)); REQUIRE(io::saveAls(*rig.song, rig.folder.getChildFile("Undo.als")).ok);
    auto undone = io::loadAls(rig.folder.getChildFile("Undo.als")); REQUIRE(undone); CHECK(undone.song->tracks[0].kind == model::TrackKind::Midi);
}

TEST_CASE("media edits: consolidation retains controller and mixer envelopes across loops and gaps")
{
    MediaRig rig; model::Clip clip; REQUIRE(model::makeEmptyMidiClip(*rig.song, 0, 4, clip));
    clip.loop = {0, 2, 0, true};
    model::AutomationEnvelope pedal; pedal.targetId = 700; pedal.midiController = 66; pedal.stepped = true;
    pedal.points = {{0, 127}, {1, 0}};
    model::AutomationEnvelope volume; volume.targetId = rig.song->tracks[0].mixer.volumeTargetId; volume.points = {{0, 0.5}, {2, 1}};
    clip.envelopes = {pedal, volume}; model::insertClip(0, clip, "Add").apply(*rig.song);
    auto second = model::movedClip(clip, 8); model::insertClip(0, second, "Add").apply(*rig.song);
    const auto consolidated = model::consolidatedMidi(*rig.song, 0, 0, 12); REQUIRE(consolidated.envelopes.size() == 2);
    const auto& lane = consolidated.envelopes[0]; engine::AutomationLane curve(lane);
    CHECK_NEAR(curve.valueAt(0.5), 127, 1e-6); CHECK_NEAR(curve.valueAt(1.5), 0, 1e-6);
    CHECK_NEAR(curve.valueAt(2.5), 127, 1e-6); CHECK_NEAR(curve.valueAt(6), 0, 1e-6); CHECK_NEAR(curve.valueAt(8.5), 127, 1e-6);
}
TEST_CASE("media edits: frozen rendered references travel with Save As and survive source removal")
{
    MediaRig rig; const auto external = rig.audio();
    engine::SampleCache samples; const auto data = samples.load(external.sample.absolutePath); REQUIRE(data);
    const auto file = rig.folder.getChildFile("Samples/Processed/Freeze/Rendered.wav");
    REQUIRE(engine::writeCroppedWav(data->audio, data->sampleRate, 0, data->audio.getNumSamples(), file).empty());
    auto clip = io::makeSampleClip(*rig.song, {file.getFullPathName().toStdString(), data->sampleRate, data->audio.getNumSamples()}, 16, 0);
    clip.sample.relativePath = "Samples/Processed/Freeze/Rendered.wav"; clip.sample.relativePathType = 3;
    auto& track = rig.song->tracks[0]; track.frozen = true; track.frozenArrangement = {clip};
    track.frozenSlots.resize(rig.song->scenes.size()); track.frozenSlots[0].clip = clip;
    const auto destination = rig.folder.getChildFile("New Project/Set.als"); REQUIRE(destination.getParentDirectory().createDirectory());
    io::AlsSaveOptions options; options.portableReferences = options.copyProjectSamples = true;
    REQUIRE(io::saveAls(*rig.song, destination, options).ok); REQUIRE(file.deleteFile());
    auto reload = io::loadAls(destination); REQUIRE(reload); REQUIRE(reload.song->tracks[0].frozenArrangement.size() == 1);
    CHECK(! io::resolveSamplePath(*reload.song, reload.song->tracks[0].frozenArrangement[0].sample).empty());
    REQUIRE(reload.song->tracks[0].frozenSlots[0].clip);
    CHECK(! io::resolveSamplePath(*reload.song, reload.song->tracks[0].frozenSlots[0].clip->sample).empty());
}
TEST_CASE("media edits: Session freeze renders the requested slot independently of Arrangement")
{
    MediaRig rig; auto clip = rig.audio(); clip.loop.enabled = true;
    model::setSessionClip(*rig.song, 2, 0, clip).apply(*rig.song);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi); engine.loadSong(*rig.song);
    REQUIRE(engine.sampleCache().waitForLoads(30000));
    REQUIRE(engine.beginOffline()); engine::Exporter exporter(engine, *rig.song);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    engine::ExportOptions options; options.target = engine::ExportTarget::SingleTrack; options.trackIndex = 2;
    options.preMixer = true; options.startBeat = 0; options.endBeat = 2; options.bitDepth = 32; options.dither = false;
    options.sessionScene = 0; options.destination = rig.folder.getChildFile("Session.wav");
    auto exported = exporter.run(options, progress, cancel); REQUIRE(exported.ok); engine.endOffline();
    engine::SampleCache samples; auto data = samples.load(options.destination.getFullPathName().toStdString()); REQUIRE(data);
    CHECK(data->audio.getMagnitude(0, data->audio.getNumSamples()) > 0.4f); CHECK_NEAR(exported.seconds, 1, 0.001);
    REQUIRE(exported.warpMarkers.size() >= 2); CHECK_NEAR(exported.warpMarkers.front().seconds, 0, 1e-9);
    CHECK_NEAR(exported.warpMarkers.back().beats / exported.warpMarkers.back().seconds, 2, 0.001);
}

TEST_CASE("media edits: automatic track tails ignore unrelated tracks continuing in Main")
{
    MediaRig rig; auto clip = rig.audio(); model::insertClip(2, clip, "Audio").apply(*rig.song);
    clip.loop.enabled = true; clip.currentEnd = 40; model::insertClip(3, clip, "Other audio").apply(*rig.song);
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi); engine.loadSong(*rig.song);
    REQUIRE(engine.sampleCache().waitForLoads(30000));
    REQUIRE(engine.beginOffline()); engine::Exporter exporter(engine, *rig.song);
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    engine::ExportOptions options; options.target = engine::ExportTarget::SingleTrack; options.trackIndex = 2;
    options.preMixer = true; options.autoTail = true; options.startBeat = 0; options.endBeat = 2;
    options.bitDepth = 32; options.dither = false; options.destination = rig.folder.getChildFile("Auto tail.wav");
    auto exported = exporter.run(options, progress, cancel); REQUIRE(exported.ok); engine.endOffline();
    CHECK(exported.seconds > 1.49); CHECK(exported.seconds < 1.6);
}

TEST_CASE("media edits: split at cursor and selection edges preserves source clocks history and saved fragments")
{
    MediaRig rig; model::Clip midi; REQUIRE(model::makeEmptyMidiClip(*rig.song, 8, 8, midi));
    midi.loop = {1, 5, 0.5, true}; midi.notes = {{1.5, 2, 48}};
    model::insertClip(0, midi, "MIDI").apply(*rig.song);
    auto audio = rig.audio(8, 4); audio.warped = false; audio.loop.loopEnd = 4; audio.warpMarkers.clear(); audio.loop.startRelative = 0.25;
    model::insertClip(2, audio, "Audio").apply(*rig.song);
    const int midiUid = rig.song->tracks[0].arrangementClips[0].uid;
    const auto audioBefore = rig.song->tracks[2].arrangementClips[0];
    model::EditHistory history;
    auto split = model::splitArrangementClips(*rig.song, 0, 2, 10, 12); REQUIRE(split.apply);
    history.perform(*rig.song, std::move(split));
    const auto& parts = rig.song->tracks[0].arrangementClips;
    REQUIRE(parts.size() == 3); CHECK(parts[0].uid == midiUid); CHECK(parts[1].uid != parts[2].uid);
    CHECK_NEAR(parts[1].currentStart, 10, 0); CHECK_NEAR(parts[1].currentEnd, 12, 0);
    CHECK_NEAR(parts[1].loop.startRelative, 2.5, 0); REQUIRE(parts[1].notes.size() == 1); CHECK_NEAR(parts[1].notes[0].start, midi.notes[0].start, 0);
    const auto ids = std::pair{parts[1].uid, parts[2].uid};
    const auto& audioParts = rig.song->tracks[2].arrangementClips;
    REQUIRE(audioParts.size() == 3);
    CHECK_NEAR(audioParts[1].loop.startRelative, audioBefore.loop.startRelative + 2 * 60 / rig.song->tempo, 1e-9);
    const auto saved = rig.folder.getChildFile("Split.als"); REQUIRE(io::saveAls(*rig.song, saved).ok);
    auto reload = io::loadAls(saved); REQUIRE(reload); REQUIRE(reload.song->tracks[0].arrangementClips.size() == 3);
    CHECK_NEAR(reload.song->tracks[0].arrangementClips[1].loop.startRelative, 2.5, 1e-9);
    REQUIRE(history.undo(*rig.song)); CHECK(rig.song->tracks[0].arrangementClips.size() == 1);
    REQUIRE(history.redo(*rig.song)); CHECK(rig.song->tracks[0].arrangementClips[1].uid == ids.first);
    CHECK(rig.song->tracks[0].arrangementClips[2].uid == ids.second);
    CHECK(! model::splitArrangementClips(*rig.song, 0, 0, 10).apply); // already a boundary
    CHECK(! model::splitArrangementClips(*rig.song, 0, 0, 100).apply);
    rig.song->tracks[0].frozen = true; CHECK(! model::splitArrangementClips(*rig.song, 0, 0, 9).apply);
}

TEST_CASE("media edits: bounce span and rendered audio retain nonzero Arrangement placement")
{
    MediaRig rig; model::insertClip(2, rig.audio(16, 1), "Audio").apply(*rig.song);
    auto muted = rig.audio(0, 1); muted.disabled = true; model::insertClip(2, muted, "Disabled").apply(*rig.song);
    const auto span = model::arrangementClipSpan(*rig.song, {2}); REQUIRE(span);
    CHECK_NEAR(span->first, 16, 0); CHECK_NEAR(span->second, 18, 1e-8);
    CHECK(! model::arrangementClipSpan(*rig.song, {0, 1, -1, 999}));
    plugins::PluginHost host; midi::MidiManager midi; engine::AudioEngine engine(host, midi); engine.loadSong(*rig.song);
    REQUIRE(engine.sampleCache().waitForLoads(30000));
    REQUIRE(engine.beginOffline()); engine::Exporter exporter(engine, *rig.song);
    engine::ExportOptions options; options.target = engine::ExportTarget::SingleTrack; options.trackIndex = 2; options.preMixer = true;
    options.startBeat = span->first; options.endBeat = span->second; options.bitDepth = 32; options.dither = false;
    options.destination = rig.folder.getChildFile("Bounce-Later.wav");
    std::atomic<float> progress{0}; std::atomic<bool> cancel{false};
    const auto result = exporter.run(options, progress, cancel); engine.endOffline(); REQUIRE(result.ok);
    CHECK_NEAR(result.seconds, 1, 0.01); REQUIRE(result.warpMarkers.size() >= 2);
    CHECK_NEAR(result.warpMarkers.front().beats, 0, 1e-8); CHECK_NEAR(result.warpMarkers.back().beats, 2, 0.01);
    engine::SampleCache samples; auto rendered = samples.load(options.destination.getFullPathName().toStdString()); REQUIRE(rendered);
    CHECK(rendered->audio.getMagnitude(1000, 10000) > 0.45); // no sixteen-beat silence prefix
    auto bounced = io::makeSampleClip(*rig.song, {options.destination.getFullPathName().toStdString(), rendered->sampleRate, rendered->audio.getNumSamples()}, 16, span->first);
    CHECK_NEAR(bounced.currentStart, 16, 0); CHECK_NEAR(bounced.currentEnd, 18, 0.01);
    model::insertClip(3, rig.audio(12, 1), "Earlier member").apply(*rig.song);
    const auto group = model::arrangementClipSpan(*rig.song, {2, 3}); REQUIRE(group);
    CHECK_NEAR(group->first, 12, 0); CHECK_NEAR(group->second, 18, 0.01);
}
