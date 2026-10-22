// ENG-9b interoperability probe: record our own channel-2 sine into an empty Audio track.
// Never overwrites the source or an existing destination. No sound card or microphone is used.
#include "core/Edit.h"
#include "core/InputRouting.h"
#include "engine/Recorder.h"
#include "engine/RecordingManager.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

#include <juce_events/juce_events.h>
#include <cmath>
#include <cstdio>

using namespace sonora;

int main(int argc, char** argv)
{
    const bool controllers = argc == 4 && std::string(argv[3]) == "--controllers";
    const bool overdub = argc == 4 && std::string(argv[3]) == "--overdub";
    const bool punch = argc == 4 && std::string(argv[3]) == "--punch";
    const bool looping = argc == 4 && std::string(argv[3]) == "--loop";
    const bool independent = argc == 4 && std::string(argv[3]) == "--independent";
    if (argc != 3 && ! punch && ! looping && ! overdub && ! controllers && ! independent)
    {
        std::fprintf(stderr, "usage: recording_probe source.als new-copy.als [--punch|--loop|--overdub|--controllers|--independent]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI init;
    const auto source = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    const auto destination = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
    if (source == destination || destination.exists())
    {
        std::fprintf(stderr, "Destination must be a separate, new file\n");
        return 2;
    }
    auto loaded = io::loadAls(source);
    if (! loaded) { std::fprintf(stderr, "%s\n", loaded.error.c_str()); return 1; }
    auto& song = *loaded.song;
    if (independent)
    {
        std::vector<int> tracks, scenes;
        for (size_t t = 0; t < song.tracks.size() && tracks.size() < 2; ++t)
            if (song.tracks[t].kind == model::TrackKind::Midi)
            {
                int scene = 0; while ((size_t) scene < song.tracks[t].slots.size() && song.tracks[t].slots[(size_t) scene].clip) ++scene;
                tracks.push_back((int) t); scenes.push_back(scene);
            }
        if (tracks.size() != 2) { std::fprintf(stderr, "Source needs two MIDI tracks\n"); return 1; }
        engine::ControlBank controls(song.tracks.size());
        for (size_t i = 0; i < 2; ++i)
        {
            model::InputSelection route{model::InputKind::Midi, (int) i + 1, {}};
            model::setInputSelection(song.tracks[(size_t) tracks[i]], route);
            controls[(size_t) tracks[i]].armed.store(true); controls[(size_t) tracks[i]].inputRoute.store(model::packInput(route));
        }
        engine::RecordingManager recorder; model::EditHistory history; std::string error;
        engine::Recorder::SessionPlan plan; plan.quant = 1; plan.targets = {{tracks[0], scenes[0], -1}};
        const auto folder = destination.getParentDirectory().getChildFile("Samples/Recorded");
        if (! recorder.start(song, controls, folder, 1000, error, false, &plan)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        auto tick = [&](double beat, const juce::MidiBuffer& midi) {
            recorder.collectRetiredPreviews(); recorder.beginBlock();
            const engine::BlockSegment segment{0, 100, beat, 100}; recorder.prepareBlock(&segment, 1, true, false);
            recorder.capture(nullptr, 0, midi, beat, 100, 100, true, false, false);
        };
        auto collect = [&]() {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
            while (! recorder.finished() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(5);
            if (! recorder.finished()) return false;
            auto result = recorder.takeResult(); if (! result.warning.empty() || result.session.size() != 1) return false;
            std::vector<model::EditCommand> commands;
            auto& take = result.session[0]; const int track = take.track, scene = take.scene;
            auto targets = model::assignRecordedMidiTargets(song, track, {&take.clip}); if (targets.apply) commands.push_back(std::move(targets));
            const double length = take.clip.loop.loopEnd;
            commands.push_back(model::setSessionClip(song, track, scene, std::move(take.clip)));
            history.perform(song, model::compose("Record Session Take", std::move(commands)));
            std::printf("track %d scene %d: %.3f beats; another capture %s\n", track, scene, length, recorder.active() ? "continues" : "finished");
            return true;
        };
        juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 90), 30); tick(0.5, midi);
        plan.targets = {{tracks[1], scenes[1], -1}};
        if (! recorder.start(song, controls, folder, 1000, error, false, &plan)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 100), 40);
        midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 60); tick(1.5, midi);
        recorder.requestSessionStop(tracks[0]); midi.clear(); tick(2.5, midi);
        if (! collect() || ! recorder.active()) { std::fprintf(stderr, "First independent drain failed\n"); return 1; }
        midi.addEvent(juce::MidiMessage::noteOff(2, 67), 20); midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 40); tick(3.5, midi);
        recorder.requestSessionStop(tracks[1]); midi.clear(); tick(4.5, midi);
        if (! collect() || recorder.busy()) { std::fprintf(stderr, "Second independent drain failed\n"); return 1; }
        const auto saved = io::saveAls(song, destination, true); if (! saved.ok) { std::fprintf(stderr, "%s\n", saved.error.c_str()); return 1; }
        song.sourcePath = destination.getFullPathName().toStdString(); io::refreshClipIdentities(song);
        auto again = io::loadAls(destination); if (! again) return 1;
        for (size_t i = 0; i < 2; ++i)
        {
            const auto& clip = *again.song->tracks[(size_t) tracks[i]].slots[(size_t) scenes[i]].clip;
            if (! clip.loop.enabled || clip.notes.size() != 1 || std::abs(clip.loop.loopEnd - (i == 0 ? 2 : 3)) > 1e-9) return 1;
        }
        if (! history.undo(song) || ! io::saveAls(song, destination, true).ok) return 1;
        again = io::loadAls(destination); if (! again || again.song->tracks[(size_t) tracks[1]].slots[(size_t) scenes[1]].clip) return 1;
        if (! history.redo(song) || ! io::saveAls(song, destination, true).ok) return 1;
        std::printf("Saved independent takes; second-track save/undo/reload/redo verified: %s\n", destination.getFullPathName().toRawUTF8());
        return 0;
    }
    if (overdub || controllers)
    {
        int trackIndex = -1, scene = -1;
        for (size_t t = 0; t < song.tracks.size() && trackIndex < 0; ++t)
            for (size_t slot = 0; slot < song.tracks[t].slots.size(); ++slot)
                if (song.tracks[t].slots[slot].clip && song.tracks[t].slots[slot].clip->kind == model::Clip::Kind::Midi
                    && ! song.tracks[t].slots[slot].clip->notes.empty())
                { trackIndex = (int) t; scene = (int) slot; break; }
        if (trackIndex < 0) { std::fprintf(stderr, "Source needs a populated MIDI Session clip\n"); return 1; }
        const auto& original = *song.tracks[(size_t) trackIndex].slots[(size_t) scene].clip;
        engine::ControlBank controls(song.tracks.size()); controls[(size_t) trackIndex].armed.store(true);
        engine::Recorder recorder; engine::Recorder::SessionPlan plan;
        plan.targets = {{trackIndex, scene, original.uid}}; plan.quant = 1;
        std::string error;
        if (! recorder.start(song, controls, destination.getParentDirectory().getChildFile("Samples/Recorded"), 1000, error, false, &plan))
        { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
        recorder.beginSessionContent(trackIndex, 0);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 74, (juce::uint8) 100), 75);
        midi.addEvent(juce::MidiMessage::noteOff(1, 74), 125);
        if (controllers)
        {
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 60);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 75);
            midi.addEvent(juce::MidiMessage::pitchWheel(1, 12288), 90);
            midi.addEvent(juce::MidiMessage::channelPressureChange(1, 85), 100);
            midi.addEvent(juce::MidiMessage::pitchWheel(1, 8192), 130);
            midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 140);
        }
        auto range = recorder.prepareCaptureRange({0, 150, 0.5, 100}, true, false);
        recorder.capture(nullptr, 0, midi, 0.5, 100, 150, true, false, false, nullptr, 0, &range);
        recorder.requestSessionStop(); midi.clear();
        range = recorder.prepareCaptureRange({0, 100, 2, 100}, true, false);
        recorder.capture(nullptr, 0, midi, 2, 100, 100, true, false, false, nullptr, 0, &range);
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
        while (! recorder.finished() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(10);
        if (! recorder.finished()) { std::fprintf(stderr, "Session drain timed out\n"); return 1; }
        auto result = recorder.takeResult();
        if (! result.warning.empty() || result.session.size() != 1)
        { std::fprintf(stderr, "Session recording failed: %s\n", result.warning.c_str()); return 1; }
        model::EditHistory history;
        std::vector<model::EditCommand> commands;
        auto targets = model::assignRecordedMidiTargets(song, trackIndex, {&result.session[0].clip});
        if (targets.apply) commands.push_back(std::move(targets));
        commands.push_back(model::modifyClip(song, trackIndex, original.uid, std::move(result.session[0].clip), "Record Session Take", scene));
        history.perform(song, model::compose("Record Session Take", std::move(commands)));
        const auto saved = io::saveAls(song, destination, true);
        if (! saved.ok) { std::fprintf(stderr, "%s\n", saved.error.c_str()); return 1; }
        auto again = io::loadAls(destination);
        if (! again) { std::fprintf(stderr, "%s\n", again.error.c_str()); return 1; }
        const auto& after = *again.song->tracks[(size_t) trackIndex].slots[(size_t) scene].clip;
        std::printf("Session overdub: track %d scene %d, %zu notes, loop %.3f -> %.3f\n%s\n",
                    trackIndex, scene, after.notes.size(), after.loop.loopStart, after.loop.loopEnd, destination.getFullPathName().toRawUTF8());
        if (controllers) for (const auto& env : after.envelopes)
            if (model::isMidiController(env.midiController)) std::printf("Controller target %d, pointee %d, %zu points\n", env.midiController, env.targetId, env.points.size());
        return 0;
    }
    if (punch)
    {
        song.loopStart = 1; song.loopLength = 1; song.loopOn = false;
        song.punchIn = song.punchOut = song.punchEdited = true;
    }
    if (looping)
    {
        song.loopStart = 1; song.loopLength = 1; song.loopOn = true;
        song.punchIn = song.punchOut = false; song.punchEdited = true;
    }
    int trackIndex = -1;
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == model::TrackKind::Audio)
        {
            if (trackIndex < 0) trackIndex = (int) i;
            song.tracks[i].arrangementClips.clear();
            for (auto& slot : song.tracks[i].slots) slot.clip.reset();
        }
    if (trackIndex < 0) { std::fprintf(stderr, "Source needs an Audio track\n"); return 1; }
    auto& track = song.tracks[(size_t) trackIndex];
    model::setInputSelection(track, {model::InputKind::AudioMono, 1, {}});
    track.monitoring = 2;
    track.monitoringEdited = true;
    engine::ControlBank controls(song.tracks.size());
    controls[(size_t) trackIndex].armed.store(true);
    controls[(size_t) trackIndex].inputRoute.store(model::packInput(model::parseInput(track)));
    engine::Recorder recorder;
    std::string error;
    constexpr double rate = 48000.0;
    const double samplesPerBeat = rate * 60.0 / song.tempo;
    if (! recorder.start(song, controls, destination.getParentDirectory().getChildFile("Samples/Recorded"), rate, error))
    { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> input(2, 512);
    const int total = looping ? 108000 : 96000;
    for (int frame = 0; frame < total; frame += 512)
    {
        const int count = std::min(512, total - frame);
        for (int i = 0; i < count; ++i)
            for (int channel = 0; channel < 2; ++channel)
                input.setSample(channel, i, (float) (0.2 * std::sin(juce::MathConstants<double>::twoPi
                    * (channel == 0 ? 440.0 : 660.0) * (frame + i) / rate)));
        if (looping)
        {
            for (int offset = 0; offset < count;)
            {
                const double elapsed = (frame + offset) / samplesPerBeat;
                const double beat = elapsed < 2 ? elapsed : 1 + std::fmod(elapsed - 2, 1);
                const int toEnd = (int) std::ceil((2 - beat) * samplesPerBeat - 1e-8);
                const int segment = std::min(count - offset, std::max(1, toEnd));
                recorder.capture(input.getArrayOfReadPointers(), 2, midi, beat, samplesPerBeat, segment,
                                 true, false, segment == toEnd, nullptr, offset);
                offset += segment;
            }
        }
        else recorder.capture(input.getArrayOfReadPointers(), 2, midi, (punch ? 0.0 : 8.0) + frame / samplesPerBeat,
                              samplesPerBeat, count, true, false, false);
    }
    recorder.requestStop();
    recorder.capture(nullptr, 0, midi, 8.0 + 96000 / samplesPerBeat, samplesPerBeat, 0, true, false, false);
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000;
    while (! recorder.finished() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(10);
    if (! recorder.finished()) { std::fprintf(stderr, "Recording drain timed out\n"); return 1; }
    auto result = recorder.takeResult();
    if (! result.warning.empty() || result.clips.size() != 1)
    { std::fprintf(stderr, "Recording failed: %s\n", result.warning.c_str()); return 1; }
    const auto path = result.clips[0].second.sample.absolutePath;
    model::EditHistory history;
    if (looping) history.perform(song, model::recordArrangementPasses(song, trackIndex, std::move(result.passes[0].second)));
    else history.perform(song, model::recordArrangementTake(song, trackIndex, std::move(result.clips[0].second)));
    const auto saved = io::saveAls(song, destination, true);
    if (! saved.ok) { std::fprintf(stderr, "%s\n", saved.error.c_str()); return 1; }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto reader = std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(juce::File(path)));
    const auto expectedFrames = punch ? (int64_t) std::ceil(2 * samplesPerBeat - 1e-8) - (int64_t) std::ceil(samplesPerBeat - 1e-8) : total;
    if (! reader || reader->numChannels != 1 || reader->lengthInSamples != expectedFrames || reader->bitsPerSample != 24)
    { std::fprintf(stderr, "Recorded WAV metadata mismatch\n"); return 1; }
    std::printf("Recorded channel 2: mono, 24-bit, 48000 Hz, %lld frames\n%s\n%s\n",
                (long long) expectedFrames, path.c_str(), destination.getFullPathName().toRawUTF8());
    return 0;
}
