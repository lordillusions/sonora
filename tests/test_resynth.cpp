#include "TestFramework.h"
#include "core/ResynthModel.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Resynth.h"
#include "engine/ResynthAnalysis.h"
#include "engine/ResynthEdits.h"
#include "engine/AudioEngine.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"
#include <numbers>

using namespace sonora;
namespace
{
juce::AudioBuffer<float> bass(double frequency, double rate = 48000, double seconds = 0.65, bool opposed = false)
{
    juce::AudioBuffer<float> audio(2, (int) (rate * seconds));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const double phase = 2 * std::numbers::pi * frequency * i / rate;
        const float envelope = (float) std::min(1.0, i / (rate * 0.006));
        const float sample = envelope * (float) (0.5 * std::sin(phase) + 0.2 * std::sin(2 * phase + 0.4) + 0.1 * std::cos(3 * phase));
        audio.setSample(0, i, sample); audio.setSample(1, i, opposed ? -sample : sample);
    }
    return audio;
}
std::shared_ptr<const model::ResynthModel> analyze(const juce::AudioBuffer<float>& audio, double rate = 48000)
{
    std::atomic<bool> cancel{false}; std::atomic<float> progress{0};
    const auto result = engine::analyzeResynth(audio, rate, "Own bass", cancel, progress);
    if (! result.model) std::printf("    analysis: %s\n", result.error.c_str());
    REQUIRE(result.model); CHECK_NEAR(progress.load(), 1, 0); return result.model;
}
juce::AudioBuffer<float> play(devices::Resynth& synth, int note, int block, int count = 48000, int mode = 0)
{
    synth.prepare(48000, block); synth.setParameter(devices::Resynth::Mode, (float) mode);
    juce::AudioBuffer<float> audio(2, count); audio.clear();
    for (int offset = 0; offset < count; offset += block)
    {
        const int size = std::min(block, count - offset); juce::MidiBuffer midi;
        if (offset == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 17);
        const int release = 30000;
        if (offset <= release && offset + size > release) midi.addEvent(juce::MidiMessage::noteOff(1, note), release - offset);
        juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), 2, offset, size); synth.process(part, midi);
    }
    return audio;
}
double amplitude(const juce::AudioBuffer<float>& audio, double hz)
{
    double cosine = 0, sine = 0, weightSum = 0;
    for (int i = 12000; i < 24000; ++i)
    {
        const double phase = 2 * std::numbers::pi * hz * i / 48000;
        const double weight = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * (i - 12000) / 11999.0);
        cosine += weight * audio.getSample(0, i) * std::cos(phase); sine += weight * audio.getSample(0, i) * std::sin(phase);
        weightSum += weight;
    }
    return 2 * std::hypot(cosine, sine) / weightSum;
}
}

TEST_CASE("resynth: continuous bass pitch, evolving harmonic amplitudes and anti-phase stereo")
{
    for (const double hz : {31.0, 55.0, 110.0, 220.0})
    {
        const auto model = analyze(bass(hz, 48000, 0.65, true));
        REQUIRE(model->notes.size() == 1); const auto& note = model->notes[0];
        CHECK_NEAR(note.root, 69 + 12 * std::log2(hz / 440), 0.1);
        const auto& frame = note.frames[note.frames.size() / 2];
        CHECK_NEAR(std::hypot(frame.cosine[0], frame.sine[0]), 0.5, 0.035);
        CHECK_NEAR(std::hypot(frame.cosine[1], frame.sine[1]), 0.2, 0.035);
        CHECK(note.confidence > 0.8f);
    }
}
TEST_CASE("resynth: silent invalid oversized and cancelled inputs fail without a partial model")
{
    std::atomic<bool> cancel{false}; std::atomic<float> progress{0};
    juce::AudioBuffer<float> audio(1, 48000); audio.clear();
    auto result = engine::analyzeResynth(audio, 48000, "", cancel, progress); CHECK(! result.model); CHECK(! result.error.empty());
    audio.setSample(0, 300, std::numeric_limits<float>::quiet_NaN());
    CHECK(! engine::analyzeResynth(audio, 48000, "", cancel, progress).model);
    CHECK(! engine::analyzeResynth(audio, 0, "", cancel, progress).model);
    juce::AudioBuffer<float> longAudio(1, 240001); longAudio.clear();
    CHECK(! engine::analyzeResynth(longAudio, 8000, "", cancel, progress).model);
    cancel.store(true); result = engine::analyzeResynth(audio, 48000, "", cancel, progress);
    CHECK(result.cancelled); CHECK(! result.model);
}
TEST_CASE("resynth: a phrase over the note limit suggests a cut that keeps exactly 64 whole notes")
{
    constexpr double rate = 24000, spacing = 0.3;
    juce::AudioBuffer<float> phrase(1, (int) (rate * 21.5)); phrase.clear();
    const auto source = bass(55, rate, 0.2);
    for (int note = 0; note < 70; ++note)
        phrase.copyFrom(0, (int) ((0.05 + note * spacing) * rate), source, 0, 0, source.getNumSamples());
    std::atomic<bool> cancel{false}; std::atomic<float> progress{0};
    const auto over = engine::analyzeResynth(phrase, rate, "Long bass", cancel, progress);
    CHECK(! over.model); CHECK(over.error.find("Found 70 notes") == 0);
    // Between note 64 (ends ~19.15 s) and note 65 (starts ~19.25 s).
    CHECK(over.fitSeconds > 19.1); CHECK(over.fitSeconds < 19.3);
    juce::AudioBuffer<float> cut(1, (int) (over.fitSeconds * rate)); cut.copyFrom(0, 0, phrase, 0, 0, cut.getNumSamples());
    const auto model = analyze(cut, rate);
    CHECK((int) model->notes.size() == model::ResynthModel::maxNotes);
    CHECK_NEAR(model->notes.back().end - model->notes.back().start, 0.2, 0.03);
}

TEST_CASE("resynth: model checksum counts and source independence survive native state roundtrip")
{
    const auto model = analyze(bass(55)); std::string error;
    const auto content = model::encodeResynth(*model, error); REQUIRE(content); CHECK(error.empty());
    auto decoded = model::decodeResynth(*content, error); REQUIRE(decoded);
    CHECK(decoded->source == model->source); CHECK_NEAR(decoded->notes[0].root, model->notes[0].root, 0);
    auto bad = *content; bad.data[bad.data.size() / 2] ^= 1; CHECK(! model::decodeResynth(bad, error));
    bad = *content; bad.data.resize(100); CHECK(! model::decodeResynth(bad, error));
    bad = *content; bad.data[4] = 99; CHECK(! model::decodeResynth(bad, error));
    auto changed = *model; changed.notes[0].frames[0].cosine[0] = std::numeric_limits<float>::infinity();
    CHECK(! model::encodeResynth(changed, error));
    changed = *model; changed.name = std::string(1, (char) 0xff); CHECK(! model::encodeResynth(changed, error));
}
TEST_CASE("resynth: synth recreates harmonics at new pitches and is independent of source waveform")
{
    const auto model = analyze(bass(55));
    devices::Resynth original(model); auto audio = play(original, 45, 128);
    CHECK_NEAR(amplitude(audio, 110), 0.5, 0.035); CHECK_NEAR(amplitude(audio, 220), 0.2, 0.035);
    CHECK_NEAR(amplitude(audio, 330), 0.1, 0.035);
    for (int i = 0; i < 17; ++i) CHECK_NEAR(audio.getSample(0, i), 0, 0);
    auto noSource = std::make_shared<model::ResynthModel>(*model); std::fill(noSource->source.begin(), noSource->source.end(), 0);
    devices::Resynth independent(noSource); const auto generated = play(independent, 45, 127);
    for (int i = 0; i < audio.getNumSamples(); ++i) CHECK_NEAR(audio.getSample(0, i), generated.getSample(0, i), 1e-6);
    devices::Resynth source(noSource); const auto reference = play(source, 45, 128, 48000, 2);
    CHECK_NEAR(reference.getMagnitude(0, 48000), 0, 0);
    CHECK_NEAR(audio.getMagnitude(47000, 1000), 0, 0);
}
TEST_CASE("resynth: native creation capture save reopen undo and missing content stay loadable")
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests/resynth");
    folder.createDirectory(); std::string xmlError;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), folder.getChildFile("Resynth.als"), xmlError));
    auto opened = io::loadAls(folder.getChildFile("Resynth.als")); REQUIRE(opened);
    auto device = devices::nativeDeviceModel("SonoraResynth"); std::string error;
    auto sourceFree = *analyze(bass(55)); model::removeResynthSource(sourceFree);
    device.nativeContent = model::encodeResynth(sourceFree, error); device.nativeContentEdited = true;
    model::allocateDeviceIds(*opened.song, device);
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    model::EditHistory history; history.perform(*opened.song, model::insertDevice(0, device));
    devices::BuildContext build; auto synth = devices::createDevice(opened.song->tracks[0].devices[0], build); REQUIRE(synth);
    synth->setParameter(devices::Resynth::Noise, 0.3f);
    synth->setParameter(devices::Resynth::Harmonic0 + 5, 0.4f); synth->setParameter(devices::Resynth::LfoDepth, 0.6f);
    devices::captureDeviceState(opened.song->tracks[0].devices[0], synth.get());
    const auto saved = io::saveAls(*opened.song, folder.getChildFile("Saved.als"));
    if (! saved.ok) std::printf("    save: %s\n", saved.error.c_str());
    REQUIRE(saved.ok);
    const auto reload = io::loadAls(folder.getChildFile("Saved.als")); REQUIRE(reload);
    const auto& found = reload.song->tracks[0].devices[0]; REQUIRE(found.nativeContent);
    auto processor = devices::createDevice(found, build); auto* native = dynamic_cast<devices::Resynth*>(processor.get());
    REQUIRE(native); CHECK(native->ready()); CHECK_NEAR(native->getParameter(devices::Resynth::Noise), 0.3, 1e-6);
    CHECK(native->model()->source.empty()); CHECK_NEAR(native->getParameter(devices::Resynth::Harmonic0 + 5), 0.4, 1e-6);
    CHECK_NEAR(native->getParameter(devices::Resynth::LfoDepth), 0.6, 1e-6);
    CHECK(history.undo(*opened.song)); CHECK(opened.song->tracks[0].devices.empty()); CHECK(history.redo(*opened.song));
    auto broken = found; auto invalid = std::make_shared<model::NativeContent>(*found.nativeContent); invalid->data.clear(); broken.nativeContent = invalid;
    std::vector<std::string> warnings; build.log = &warnings; processor = devices::createDevice(broken, build);
    REQUIRE(dynamic_cast<devices::Resynth*>(processor.get())); CHECK(! dynamic_cast<devices::Resynth*>(processor.get())->ready()); CHECK(! warnings.empty());
}

TEST_CASE("resynth: repeated and changing notes at multiple rates retain phrase timing")
{
    for (double rate : {44100.0, 96000.0})
    {
        juce::AudioBuffer<float> phrase(1, (int) (rate * 1.6)); phrase.clear();
        for (int note = 0; note < 3; ++note)
        {
            const auto source = bass(note < 2 ? 55 : 110, rate, 0.38);
            phrase.copyFrom(0, (int) ((0.08 + note * 0.5) * rate), source, 0, 0, source.getNumSamples());
        }
        const auto model = analyze(phrase, rate); REQUIRE(model->notes.size() == 3);
        for (size_t i = 0; i < model->notes.size(); ++i)
        {
            CHECK_NEAR(model->notes[i].root, i < 2 ? 33 : 45, 0.1);
            CHECK_NEAR(model->notes[i].start, 0.08 + (double) i * 0.5, 0.045);
        }
        const auto notes = engine::resynthPhrase(*model, 2);
        CHECK_NEAR(notes[1].start, model->notes[1].start * 2, 1e-6); CHECK(notes[0].duration > 0.6);
    }
}
TEST_CASE("resynth: trimming a note resamples its evolving model and validates bounds")
{
    auto model = *analyze(bass(55)); const auto original = model.notes[0];
    REQUIRE(model::trimResynthNote(model, 0, original.start + 0.1f, original.end - 0.1f));
    CHECK_NEAR(model.notes[0].start, original.start + 0.1, 1e-6);
    CHECK_NEAR(model.notes[0].frames.front().time, 0, 0);
    CHECK_NEAR(model.notes[0].frames.back().time, model.notes[0].end - model.notes[0].start, 1e-6);
    std::string error; CHECK(model::validateResynth(model, error));
    CHECK(! model::trimResynthNote(model, 0, 0, original.end));
    CHECK(! model::trimResynthNote(model, 0, 0.2f, 0.201f));
}
TEST_CASE("resynth: phrase track is one portable undoable edit in Arrangement or Session")
{
    const auto analysis = analyze(bass(55)); const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests/resynth-phrase");
    folder.createDirectory(); const auto source = folder.getChildFile("Source.als"); std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), source, error));
    for (int scene : {-1, 3})
    {
        auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
        auto instrument = devices::nativeDeviceModel("SonoraResynth"); instrument.nativeContent = model::encodeResynth(*analysis, error);
        model::EditHistory history; const auto count = song.tracks.size();
        const auto command = engine::resynthTrack(song, *analysis, std::move(instrument), 0, 8, scene); REQUIRE(command.apply);
        history.perform(song, command); REQUIRE(song.tracks.size() == count + 1);
        const auto& track = song.tracks[1]; CHECK(track.devices[0].nativeContent);
        const auto& clip = scene < 0 ? track.arrangementClips[0] : *track.slots[(size_t) scene].clip;
        CHECK_NEAR(clip.currentStart, scene < 0 ? 8 : 0, 0); CHECK(clip.notes.size() == analysis->notes.size());
        REQUIRE(clip.envelopes.size() == 1); CHECK(clip.envelopes[0].midiController == 121);
        CHECK(clip.envelopes[0].targetId == track.midiControllerTargets[121]);
        CHECK_NEAR(track.devices[0].param("PitchMotion", 0), 1, 0);
        const auto saved = folder.getChildFile(scene < 0 ? "Arrangement.als" : "Session.als");
        io::AlsSaveOptions options; options.collectAll = true;
        const auto result = io::saveAls(song, saved, options); if (! result.ok) std::printf("    phrase save: %s\n", result.error.c_str()); REQUIRE(result.ok);
        const auto reopened = io::loadAls(saved); REQUIRE(reopened); REQUIRE(reopened.song->tracks[1].devices[0].nativeContent);
        CHECK(model::decodeResynth(*reopened.song->tracks[1].devices[0].nativeContent, error));
        const auto& restored = scene < 0 ? reopened.song->tracks[1].arrangementClips[0] : *reopened.song->tracks[1].slots[(size_t) scene].clip;
        REQUIRE(restored.envelopes.size() == 1); CHECK(restored.envelopes[0].midiController == 121);
        CHECK(history.undo(song)); CHECK(song.tracks.size() == count); CHECK(history.redo(song)); CHECK(song.tracks.size() == count + 1);
    }
}
TEST_CASE("resynth: note channels sustain bend panic and bounded release voices")
{
    const auto model = analyze(bass(55)); devices::Resynth synth(model); synth.prepare(48000, 512);
    synth.setParameter(devices::Resynth::Release, 10);
    juce::AudioBuffer<float> block(2, 512); juce::MidiBuffer midi; midi.ensureSize(65536);
    const auto run = [&] { block.clear(); synth.process(block, midi); midi.clear(); return block.getMagnitude(0, 512); };
    midi.addEvent(juce::MidiMessage::noteOn(2, 33, 1.0f), 0); CHECK(run() > 0.1f);
    midi.addEvent(juce::MidiMessage::noteOff(1, 33), 0); for (int i = 0; i < 4; ++i) CHECK(run() > 0.1f);
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(2, 33), 0); for (int i = 0; i < 4; ++i) CHECK(run() > 0.1f);
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 0); run(); CHECK_NEAR(run(), 0, 0);
    for (int i = 0; i < 200; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, i % 100 + 20, 1.0f), i % 512);
    CHECK(std::isfinite(run())); midi.addEvent(juce::MidiMessage::allSoundOff(1), 0); CHECK_NEAR(run(), 0, 0);
    synth.setParameter(devices::Resynth::Volume, std::numeric_limits<float>::quiet_NaN()); CHECK(std::isfinite(synth.getParameter(devices::Resynth::Volume)));
}

TEST_CASE("resynth: editing converted MIDI changes the instrument audio through the real engine")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto song = io::parseAls(*io::createNewSetDom(), "/tmp/resynth-engine.xml"); REQUIRE(song);
    const auto analysis = analyze(bass(55)); auto instrument = devices::nativeDeviceModel("SonoraResynth"); std::string error;
    instrument.nativeContent = model::encodeResynth(*analysis, error); REQUIRE(instrument.nativeContent);
    auto command = engine::resynthTrack(*song.song, *analysis, std::move(instrument), 0, 0); REQUIRE(command.apply); command.apply(*song.song);
    song.song->loopOn = false;
    const auto render = [&] {
        plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine engine(host, input);
        engine.loadSong(*song.song); engine.transport().play();
        const int count = (int) (engine.sampleRate() * 0.5); juce::AudioBuffer<float> output(2, count); output.clear();
        for (int offset = 0; offset < count; offset += 256)
        {
            juce::AudioBuffer<float> part(output.getArrayOfWritePointers(), 2, offset, std::min(256, count - offset));
            engine.renderBlock(part.getArrayOfWritePointers(), 2, part.getNumSamples());
        }
        return output;
    };
    const auto at55 = render();
    REQUIRE(song.song->tracks[1].arrangementClips[0].notes.size() == 1);
    song.song->tracks[1].arrangementClips[0].notes[0].pitch += 12;
    const auto at110 = render();
    const auto energyAt = [](const juce::AudioBuffer<float>& audio, double frequency) {
        double cosine = 0, sine = 0;
        for (int i = 4410; i < 17640; ++i)
        {
            const double phase = 2 * std::numbers::pi * frequency * i / 44100;
            cosine += audio.getSample(0, i) * std::cos(phase); sine += audio.getSample(0, i) * std::sin(phase);
        }
        return std::hypot(cosine, sine) / 13230;
    };
    CHECK(energyAt(at55, 55) > 0.1); CHECK(energyAt(at110, 110) > 0.1);
    CHECK(energyAt(at110, 55) < energyAt(at55, 55) * 0.1);
}

TEST_CASE("resynth: repeated-pitch phrase MIDI selects each captured articulation at the note sample")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto model = std::make_shared<model::ResynthModel>(*analyze(bass(55, 48000, 0.9)));
    auto first = model->notes[0], second = first;
    first.start = 0; first.end = 0.3f; first.sustain = 0.2f;
    second.start = 0.5f; second.end = 0.8f; second.sustain = 0.2f;
    for (size_t i = 0; i < first.frames.size(); ++i)
    {
        first.frames[i].time = second.frames[i].time = 0.3f * (float) i / (float) (first.frames.size() - 1);
        first.frames[i].cosine.fill(0); first.frames[i].sine.fill(0); first.frames[i].cosine[0] = 0.15f; first.frames[i].noise.fill(0);
        second.frames[i] = first.frames[i]; second.frames[i].cosine[0] = 0.6f;
    }
    model->notes = {first, second};
    devices::Resynth synth(model); synth.prepare(48000, 512);
    juce::AudioBuffer<float> output(2, 512); output.clear(); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 119, 2), 37);
    midi.addEvent(juce::MidiMessage::noteOn(1, 33, 1.0f), 37); synth.process(output, midi);
    CHECK(synth.activeExample() == 1); CHECK(output.getMagnitude(200, 312) > 0.55f);
    for (int i = 0; i < 37; ++i) CHECK_NEAR(output.getSample(0, i), 0, 0);
    synth.reset(); output.clear(); midi.clear();
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 119, 0), 37);
    midi.addEvent(juce::MidiMessage::noteOn(1, 33, 1.0f), 37); synth.process(output, midi);
    CHECK(synth.activeExample() == 0); CHECK(output.getMagnitude(200, 312) < 0.2f);
    auto song = io::parseAls(*io::createNewSetDom(), "/tmp/resynth-articulation.xml"); REQUIRE(song);
    auto instrument = devices::nativeDeviceModel("SonoraResynth"); std::string error;
    instrument.nativeContent = model::encodeResynth(*model, error); REQUIRE(instrument.nativeContent);
    const auto command = engine::resynthTrack(*song.song, *model, std::move(instrument), 0, 0); REQUIRE(command.apply); command.apply(*song.song);
    song.song->loopOn = false;
    plugins::PluginHost host; midi::MidiManager input; engine::AudioEngine engine(host, input);
    engine.loadSong(*song.song); engine.transport().play();
    juce::AudioBuffer<float> phrase(2, 44100); phrase.clear();
    for (int offset = 0; offset < phrase.getNumSamples(); offset += 512)
    {
        juce::AudioBuffer<float> part(phrase.getArrayOfWritePointers(), 2, offset, std::min(512, phrase.getNumSamples() - offset));
        engine.renderBlock(part.getArrayOfWritePointers(), 2, part.getNumSamples());
    }
    const float quiet = phrase.getRMSLevel(0, 4410, 4410), loud = phrase.getRMSLevel(0, 26460, 4410);
    CHECK(quiet > 0.05f); CHECK(loud > quiet * 3.5f); CHECK(loud < quiet * 4.5f);
}

TEST_CASE("resynth: removing PCM retains duration MIDI and identical synthesis through v2 state")
{
    const auto original = analyze(bass(55)); std::string error;
    const auto full = model::encodeResynth(*original, error); REQUIRE(full);
    auto stripped = *original; model::removeResynthSource(stripped);
    REQUIRE(stripped.source.empty()); CHECK_NEAR(stripped.duration(), original->duration(), 0);
    const auto content = model::encodeResynth(stripped, error); REQUIRE(content);
    CHECK(content->format == "sonora-resynth-v2"); CHECK(full->data.size() - content->data.size() == original->source.size() * 4);
    const auto restored = model::decodeResynth(*content, error); REQUIRE(restored); CHECK(restored->source.empty());
    CHECK_NEAR(engine::resynthPhrase(*restored, 2)[0].duration, engine::resynthPhrase(*original, 2)[0].duration, 0);
    devices::Resynth withRecording(original); const auto expected = play(withRecording, 33, 128);
    for (int mode : {0, 1, 2})
    {
        devices::Resynth withoutRecording(restored); const auto actual = play(withoutRecording, 33, 127, 48000, mode);
        double maximum = 0;
        for (int i = 0; i < actual.getNumSamples(); ++i) maximum = std::max(maximum, (double) std::abs(actual.getSample(0, i) - expected.getSample(0, i)));
        CHECK(maximum < 1e-6); // Hybrid/Reference state safely falls back to Synth when no PCM exists.
    }
    // A legacy payload omits the separate stored-PCM count. Preserve v1 sessions when upgrading.
    auto legacy = *full; legacy.format = "sonora-resynth-v1"; legacy.data[4] = 1;
    const size_t count = 8 + original->name.size() + 1 + 8 + 4 + 4;
    legacy.data.erase(legacy.data.begin() + (std::ptrdiff_t) count, legacy.data.begin() + (std::ptrdiff_t) count + 4);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < legacy.data.size() - 4; ++i) hash = (hash ^ legacy.data[i]) * 16777619u;
    for (int i = 0; i < 4; ++i) legacy.data[legacy.data.size() - 4 + (size_t) i] = (uint8_t) (hash >> (i * 8));
    const auto old = model::decodeResynth(legacy, error); REQUIRE(old); CHECK(old->source == original->source); CHECK_NEAR(old->duration(), original->duration(), 0);
}

TEST_CASE("resynth: spectral controls operate on individual harmonics and generate a new sub oscillator")
{
    const auto model = analyze(bass(55)); using R = devices::Resynth;
    R edited(model); edited.setParameter(R::Noise, 0); edited.setParameter(R::Harmonic0 + 1, 0);
    auto audio = play(edited, 33, 128);
    CHECK_NEAR(amplitude(audio, 55), 0.5, 0.035); CHECK(amplitude(audio, 110) < 0.01); CHECK_NEAR(amplitude(audio, 165), 0.1, 0.035);
    edited.setParameter(R::Harmonic0 + 1, 1); edited.setParameter(R::Fundamental, 0); edited.setParameter(R::OddLevel, 0);
    audio = play(edited, 33, 128); CHECK(amplitude(audio, 55) < 0.01); CHECK(amplitude(audio, 165) < 0.01); CHECK_NEAR(amplitude(audio, 110), 0.2, 0.035);
    edited.setParameter(R::SpectralTilt, 3); audio = play(edited, 33, 128); CHECK_NEAR(amplitude(audio, 110), 0.2 * std::pow(10, 3.0 / 20), 0.035);
    edited.setParameter(R::PartialCount, 1); edited.setParameter(R::SubLevel, 1);
    audio = play(edited, 33, 128); CHECK_NEAR(amplitude(audio, 27.5), 0.5, 0.035); CHECK(amplitude(audio, 110) < 0.01);
}

TEST_CASE("resynth: amp filter LFO and edited-spectrum processing are block invariant")
{
    const auto model = analyze(bass(55)); using R = devices::Resynth;
    const auto configure = [](R& synth) {
        synth.setParameter(R::Harmonic0 + 1, 0.7f); synth.setParameter(R::AmpDecay, 60); synth.setParameter(R::AmpSustain, 0.3f);
        synth.setParameter(R::Tone, 500); synth.setParameter(R::Resonance, 0.4f); synth.setParameter(R::FilterEnv, 24);
        synth.setParameter(R::FilterAttack, 25); synth.setParameter(R::FilterDecay, 190); synth.setParameter(R::FilterSustain, 0.2f);
        synth.setParameter(R::LfoDepth, 0.2f); synth.setParameter(R::LfoDestination, 2); synth.setParameter(R::LfoRate, 3.5f);
        synth.setParameter(R::SubLevel, 0.2f); synth.setParameter(R::Drive, 4);
    };
    R a(model), b(model); configure(a); configure(b);
    const auto first = play(a, 33, 127), second = play(b, 33, 512);
    double difference = 0;
    for (int i = 0; i < first.getNumSamples(); ++i)
    { CHECK(std::isfinite(first.getSample(0, i))); difference = std::max(difference, (double) std::abs(first.getSample(0, i) - second.getSample(0, i))); }
    CHECK(difference < 1e-6); CHECK(first.getMagnitude(0, 48000) > 0.05f);
    R envelope(model); envelope.setParameter(R::AmpDecay, 20); envelope.setParameter(R::AmpSustain, 0.2f);
    auto audio = play(envelope, 33, 128); CHECK_NEAR(amplitude(audio, 55), 0.1, 0.02);
    R filter(model); filter.setParameter(R::Tone, 40); filter.setParameter(R::FilterType, 0);
    audio = play(filter, 33, 128); CHECK(amplitude(audio, 165) < 0.01);
}

TEST_CASE("resynth: Poly 8 holds independent MIDI pitches and tempo-synced amplitude LFO follows host tempo")
{
    using R = devices::Resynth; const auto model = analyze(bass(55));
    for (int mode : {0, 2})
    {
        R synth(model); synth.prepare(48000, 24000); synth.setParameter(R::Noise, 0); synth.setParameter(R::PartialCount, 1);
        synth.setParameter(R::PlayMode, (float) mode); synth.setParameter(R::Release, 5);
        juce::AudioBuffer<float> audio(2, 24000); audio.clear(); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 33, 1.0f), 0); midi.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 2400);
        synth.process(audio, midi); CHECK(amplitude(audio, 82.4069) > 0.35); CHECK(mode == 2 ? amplitude(audio, 55) > 0.35 : amplitude(audio, 55) < 0.01);
    }
    R slow(model), fast(model);
    for (auto* synth : {&slow, &fast})
    { synth->setParameter(R::Noise, 0); synth->setParameter(R::LfoDepth, 0.5f); synth->setParameter(R::LfoDestination, 3); synth->setParameter(R::LfoSync, 1); }
    devices::HostInfo host; host.tempo = 60; slow.setHostInfo(host); host.tempo = 120; fast.setHostInfo(host);
    const auto a = play(slow, 33, 128), b = play(fast, 33, 128);
    CHECK(a.getRMSLevel(0, 17200, 800) > b.getRMSLevel(0, 17200, 800) * 2);
}

TEST_CASE("resynth: frozen timbre selects spectral frames and legato fallback keeps the envelope")
{
    using R = devices::Resynth;
    auto model = std::make_shared<model::ResynthModel>(); model->sourceFrames = 48000;
    model::ResynthNote note; note.end = 1; note.root = 33; note.sustain = 0.7f;
    model::ResynthFrame first; first.cosine[0] = 0.5f; first.pitch = 33;
    auto last = first; last.time = 1; last.cosine[0] = 0; last.cosine[1] = 0.5f; note.frames = {first, last}; model->notes = {note};
    R start(model), end(model);
    for (auto* synth : {&start, &end}) { synth->setParameter(R::ScanMode, 2); synth->setParameter(R::Sustain, 1); }
    end.setParameter(R::TimbreStart, 0.95f);
    const auto a = play(start, 33, 128), b = play(end, 33, 128);
    CHECK_NEAR(amplitude(a, 55), 0.5, 0.02); CHECK(amplitude(a, 110) < 0.01);
    CHECK(amplitude(b, 55) < 0.04); CHECK_NEAR(amplitude(b, 110), 0.475, 0.02);
    R legato(model); legato.prepare(48000, 24000); legato.setParameter(R::PlayMode, 1);
    legato.setParameter(R::ScanMode, 2); legato.setParameter(R::Attack, 100); legato.setParameter(R::Release, 2);
    juce::AudioBuffer<float> audio(2, 24000); audio.clear(); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 33, 1.0f), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 3840);
    midi.addEvent(juce::MidiMessage::noteOff(1, 40), 7680);
    midi.addEvent(juce::MidiMessage::noteOff(1, 33), 20000); legato.process(audio, midi);
    CHECK(audio.getRMSLevel(0, 10000, 2000) > 0.3f); CHECK_NEAR(audio.getMagnitude(22000, 2000), 0, 0);
}

TEST_CASE("resynth: dry periodic bass residual noise stays below the tonal signal")
{
    for (double frequency : {31.0, 55.0, 110.0, 220.0})
    {
        const auto model = analyze(bass(frequency));
        double noise = 0, tone = 0;
        for (const auto& frame : model->notes[0].frames)
        {
            if (frame.time < 0.15 || frame.time > 0.45) continue;
            for (float band : frame.noise) noise += band * band;
            for (size_t h = 0; h < frame.cosine.size(); ++h) tone += 0.5 * (frame.cosine[h] * frame.cosine[h] + frame.sine[h] * frame.sine[h]);
        }
        std::printf("    residual %.0f Hz: %.5f power ratio\n", frequency, noise / tone);
        CHECK(noise < tone * 0.001);
    }
}

TEST_CASE("resynth: noise bands add calibrated RMS power and new bass patches start without noise")
{
    auto model = std::make_shared<model::ResynthModel>(); model->sourceFrames = 48000;
    model::ResynthNote note; note.end = 1; note.root = 33;
    model::ResynthFrame first; first.pitch = 33; first.noise = {0.02f, 0.03f, 0.04f, 0.05f};
    auto last = first; last.time = 1; note.frames = {first, last}; model->notes = {note};
    devices::Resynth quiet(model); CHECK_NEAR(quiet.getParameter(devices::Resynth::Noise), 0, 0);
    const auto silence = play(quiet, 33, 128); CHECK_NEAR(silence.getMagnitude(0, silence.getNumSamples()), 0, 0);
    devices::Resynth synth(model); synth.setParameter(devices::Resynth::Noise, 1); synth.setParameter(devices::Resynth::FilterOn, 0);
    const auto noise = play(synth, 33, 128);
    CHECK_NEAR(noise.getRMSLevel(0, 10000, 18000), std::sqrt(0.02 * 0.02 + 0.03 * 0.03 + 0.04 * 0.04 + 0.05 * 0.05), 0.005);
    devices::Resynth other(model); other.setParameter(devices::Resynth::Noise, 1); other.setParameter(devices::Resynth::FilterOn, 0);
    const auto differentBlocks = play(other, 33, 127);
    for (int i = 0; i < noise.getNumSamples(); ++i) CHECK_NEAR(noise.getSample(0, i), differentBlocks.getSample(0, i), 1e-6);
}
