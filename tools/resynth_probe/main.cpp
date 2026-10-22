#include "core/ResynthModel.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Resynth.h"
#include "engine/ResynthAnalysis.h"
#include "engine/ResynthEdits.h"
#include "engine/SampleExport.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace sonora;
namespace
{
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
juce::AudioBuffer<float> demo()
{
    constexpr int rate = 48000;
    juce::AudioBuffer<float> audio(1, rate * 3); audio.clear();
    constexpr std::array<int, 4> pitches{33, 36, 31, 33};
    for (int note = 0; note < 4; ++note)
        for (int i = 0; i < 28800; ++i)
        {
            const double time = (double) i / rate, frequency = 440 * std::exp2((pitches[(size_t) note] - 69.0) / 12);
            const double phase = 2 * std::numbers::pi * frequency * time;
            const double envelope = std::min(1.0, time / 0.004) * std::min(1.0, (0.6 - time) / 0.03) * std::exp(-time * 0.7);
            const double brightness = std::exp(-time * 5);
            const float sample = (float) (envelope * (0.55 * std::sin(phase) + 0.23 * brightness * std::sin(2 * phase + 0.5)
                + 0.12 * brightness * std::sin(3 * phase + 0.9) + 0.06 * brightness * std::sin(5 * phase)));
            audio.setSample(0, note * 36000 + i, sample);
        }
    return audio;
}
juce::AudioBuffer<float> render(const std::shared_ptr<const model::ResynthModel>& model, int mode, int transpose, double& worst, double& total, bool edited = false)
{
    devices::Resynth synth(model); synth.prepare(48000, 128); synth.setParameter(devices::Resynth::Mode, (float) mode);
    synth.setParameter(devices::Resynth::Release, 30);
    synth.setParameter(devices::Resynth::PitchMotion, 1); synth.setParameter(devices::Resynth::Sustain, 1);
    if (edited)
    {
        using R = devices::Resynth;
        synth.setParameter(R::SpectralTilt, 1.5f); synth.setParameter(R::SubLevel, 0.2f);
        synth.setParameter(R::Tone, 1200); synth.setParameter(R::Resonance, 0.4f); synth.setParameter(R::FilterEnv, 24);
        synth.setParameter(R::LfoDestination, 2); synth.setParameter(R::LfoDepth, 0.25f);
    }
    juce::MidiBuffer events;
    for (size_t i = 0; i < model->notes.size(); ++i)
    {
        const auto& note = model->notes[i];
        const int pitch = std::clamp((int) std::lround(note.root) + transpose, 0, 127);
        events.addEvent(juce::MidiMessage::controllerEvent(1, 119, (int) i + 1), (int) std::lround(note.start * 48000));
        events.addEvent(juce::MidiMessage::noteOn(1, pitch, 1.0f), (int) std::lround(note.start * 48000));
        events.addEvent(juce::MidiMessage::noteOff(1, pitch), (int) std::lround(note.end * 48000));
    }
    const int count = (int) std::ceil((model->duration() + 0.2) * 48000);
    juce::AudioBuffer<float> audio(2, count); audio.clear(); juce::MidiBuffer midi; midi.ensureSize(65536);
    worst = 0; const auto start = Clock::now();
    for (int offset = 0; offset < count; offset += 128)
    {
        const int length = std::min(128, count - offset); midi.clear(); midi.addEvents(events, offset, length, -offset);
        juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), 2, offset, length);
        const auto block = Clock::now(); synth.process(part, midi); worst = std::max(worst, milliseconds(block));
    }
    total = milliseconds(start); return audio;
}
}
int main(int argc, char** argv)
{
    if (argc != 3)
    { std::fprintf(stderr, "usage: resynth_probe <source.wav|--demo> <output-directory>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    const auto path = [](const char* value) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(value)); };
    const auto folder = path(argv[2]); if (! folder.createDirectory()) return 1;
    juce::AudioBuffer<float> source; double rate = 48000; std::string name = "Generated bass";
    if (std::string(argv[1]) == "--demo") source = demo();
    else
    {
        juce::AudioFormatManager formats; formats.registerBasicFormats(); const auto file = path(argv[1]);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (! reader || reader->lengthInSamples <= 0 || (double) reader->lengthInSamples > reader->sampleRate * 30
            || reader->sampleRate < 8000 || reader->sampleRate > 96000 || reader->numChannels < 1 || reader->numChannels > 2)
        { std::fprintf(stderr, "Invalid source; select <=30 seconds, mono/stereo, 8–96 kHz\n"); return 1; }
        source.setSize((int) reader->numChannels, (int) reader->lengthInSamples); rate = reader->sampleRate;
        if (! reader->read(&source, 0, source.getNumSamples(), 0, true, true)) return 1;
        name = file.getFileNameWithoutExtension().toStdString();
    }
    std::atomic<bool> cancel{false}; std::atomic<float> progress{0};
    const auto start = Clock::now(); const auto analysis = engine::analyzeResynth(source, rate, name, cancel, progress);
    const double elapsed = milliseconds(start);
    if (! analysis.model) { std::fprintf(stderr, "%s\n", analysis.error.c_str()); return 1; }
    std::string error; auto content = model::encodeResynth(*analysis.model, error); if (! content) return 1;
    auto stripped = *analysis.model; model::removeResynthSource(stripped);
    const auto strippedContent = model::encodeResynth(stripped, error); if (! strippedContent) return 1;
    const auto sourceFree = model::decodeResynth(*strippedContent, error); if (! sourceFree) return 1;
    if (! folder.getChildFile("model.bin").replaceWithData(content->data.data(), content->data.size())) return 1;
    if (! engine::writeCroppedWav(source, rate, 0, source.getNumSamples(), folder.getChildFile("original.wav")).empty()) return 1;
    juce::DynamicObject::Ptr report = new juce::DynamicObject;
    report->setProperty("analysisMs", elapsed); report->setProperty("notes", (int) analysis.model->notes.size());
    const auto preparationStart = Clock::now();
    devices::Resynth prepared(analysis.model);
    report->setProperty("preparationMs", milliseconds(preparationStart));
    report->setProperty("preparedBytes", (juce::int64) prepared.preparedBytes());
    report->setProperty("modelBytes", (juce::int64) content->data.size()); report->setProperty("channel", analysis.model->analysisChannel + 1);
    report->setProperty("sourceFreeBytes", (juce::int64) strippedContent->data.size());
    juce::Array<juce::var> regions;
    for (size_t i = 0; i < analysis.model->notes.size(); ++i)
    {
        const auto& note = analysis.model->notes[i];
        juce::DynamicObject::Ptr region = new juce::DynamicObject;
        region->setProperty("start", note.start); region->setProperty("end", note.end);
        region->setProperty("rootMidi", note.root); region->setProperty("confidence", note.confidence); regions.add(juce::var(region.get()));
    }
    report->setProperty("regions", regions);
    juce::MidiMessageSequence sequence;
    sequence.addEvent(juce::MidiMessage::tempoMetaEvent(500000));
    for (size_t i = 0; i < analysis.model->notes.size(); ++i)
    {
        const auto& note = analysis.model->notes[i];
        auto example = juce::MidiMessage::controllerEvent(1, 119, (int) i + 1); example.setTimeStamp(note.start * 1920.0); sequence.addEvent(example);
        auto on = juce::MidiMessage::noteOn(1, std::clamp((int) std::lround(note.root), 0, 127), 1.0f);
        auto off = juce::MidiMessage::noteOff(1, on.getNoteNumber());
        on.setTimeStamp(note.start * 1920.0); off.setTimeStamp(note.end * 1920.0);
        sequence.addEvent(on); sequence.addEvent(off);
    }
    sequence.updateMatchedPairs(); juce::MidiFile midiFile; midiFile.setTicksPerQuarterNote(960); midiFile.addTrack(sequence);
    const auto midiPath = folder.getChildFile("phrase.mid"); midiPath.deleteFile();
    if (auto stream = midiPath.createOutputStream(); ! stream || ! midiFile.writeTo(*stream)) return 1;
    for (int version = 0; version < 4; ++version)
    {
        const char* label = version == 0 ? "synth" : version == 1 ? "hybrid" : version == 2 ? "source" : "octave";
        double worst = 0, total = 0; const auto audio = render(analysis.model, version == 3 ? 0 : version, version == 3 ? 12 : 0, worst, total);
        for (int channel = 0; channel < audio.getNumChannels(); ++channel) for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            if (! std::isfinite(audio.getSample(channel, sample))) { std::fprintf(stderr, "Non-finite render\n"); return 1; }
        error = engine::writeCroppedWav(audio, 48000, 0, audio.getNumSamples(), folder.getChildFile(juce::String(label) + ".wav"));
        if (! error.empty()) return 1;
        report->setProperty(juce::Identifier(juce::String(label) + "RenderMs"), total);
        report->setProperty(juce::Identifier(juce::String(label) + "WorstBlockMs"), worst);
    }
    {
        double worst = 0, total = 0;
        const auto full = render(analysis.model, 0, 0, worst, total), empty = render(sourceFree, 0, 0, worst, total);
        double difference = 0;
        for (int channel = 0; channel < full.getNumChannels(); ++channel)
            for (int sample = 0; sample < full.getNumSamples(); ++sample)
                difference = std::max(difference, (double) std::abs(full.getSample(channel, sample) - empty.getSample(channel, sample)));
        report->setProperty("sourceFreeMaxDifference", difference);
        if (difference > 0) { std::fprintf(stderr, "Removing recording changed Synth output\n"); return 1; }
        if (! engine::writeCroppedWav(empty, 48000, 0, empty.getNumSamples(), folder.getChildFile("source-free.wav")).empty()) return 1;
        const auto designed = render(sourceFree, 0, 0, worst, total, true);
        for (int channel = 0; channel < designed.getNumChannels(); ++channel)
            for (int sample = 0; sample < designed.getNumSamples(); ++sample)
                if (! std::isfinite(designed.getSample(channel, sample))) return 1;
        report->setProperty("editedSynthRenderMs", total); report->setProperty("editedSynthWorstBlockMs", worst);
        if (! engine::writeCroppedWav(designed, 48000, 0, designed.getNumSamples(), folder.getChildFile("edited-synth.wav")).empty()) return 1;
    }
    const auto file = folder.getChildFile("Resynth.als");
    if (! io::writeAlsDom(*io::createNewSetDom(), file, error)) return 1;
    auto song = io::loadAls(file); if (! song) return 1;
    auto device = devices::nativeDeviceModel("SonoraResynth"); device.nativeContent = content; device.nativeContentEdited = true;
    const auto edit = engine::resynthTrack(*song.song, *analysis.model, std::move(device), 0, 0);
    if (! edit.apply) return 1;
    edit.apply(*song.song); if (! io::saveAls(*song.song, file).ok) return 1;
    auto& instrument = song.song->tracks[1].devices[0]; instrument.nativeContent = strippedContent; instrument.nativeContentEdited = true;
    if (! io::saveAls(*song.song, folder.getChildFile("Resynth-SourceFree.als")).ok) return 1;
    if (! folder.getChildFile("report.json").replaceWithText(juce::JSON::toString(juce::var(report.get()), false))) return 1;
    std::printf("%zu notes; analysis %.1f ms; model %.2f MiB; %s\n", analysis.model->notes.size(), elapsed,
                 (double) content->data.size() / (1024 * 1024), folder.getFullPathName().toRawUTF8());
    return 0;
}
