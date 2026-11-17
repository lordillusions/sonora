// seraph_demo — renders Sonora Seraph (PLG-27) for listening, and measures what it costs.
//
//   seraph_demo <folder> [--rate 44100] [--only <text>]   one WAV per preset (an A minor chord for chord presets, a held A
//                                                           for chants and solos, 8 bars at 138 BPM), a dry version of the
//                                                           first preset of each category, three "in context" files
//   seraph_demo --cpu [--rate 48000] [--block 512]         time 64 singers and the default preset against the block deadline
//   seraph_demo --make-set <out.als> [--stress]            a set with Seraph on a MIDI track playing a chord, for engine_probe
//                                                           (--stress: Mixed, Men Octave, 8 singers, a 4-note chord = 64 singers)
//   seraph_demo <folder> --real-only [--real a.wav b.wav ...] the Real presets (PLG-27b) only: from sung notes Seraph
//                                                           synthesises itself, or from the files given (your own vocals)
//   The Seraph Voices pack (PLG-27h; driven by scripts/packs/make_seraph_voices.py):
//   seraph_demo --pack-notes <file>                         the held notes in a recording: "start end midi" per line (s)
//   seraph_demo --pack-cut <in> <start> <end> <out.flac>    cuts a note, levels it (sustain RMS -20 dBFS, peak under -1 dBFS),
//                                                           10 ms fades, mono 44.1 kHz 24-bit FLAC
//   seraph_demo --pack-check <files...>                     Seraph's own analysis of each file: "ok <midi> <held s>" or the reason
//   seraph_demo <folder> --pack-demos <packs root>          PLG-27h: the Real Choir presets sung by an installed Seraph Voices pack
//   seraph_demo --default-note <out.wav> [key] [section] [drift] [seed]  PLG-27d: one dry singer, default human controls, 3.5 s
//   seraph_demo --measure <files...>                        PLG-27d: per held note, "vibrato <Hz> <cents> drift <cents> jitter <%>
//                                                           shimmer <%>" (the human controls' measures, as Seraph's tests take them)
//
// Nothing here is shipped with the app; the WAVs are for the owner's ears (docs/SERAPH.md "Session log").

#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Seraph.h"
#include "devices/native/SeraphReal.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace sonora;
using S = devices::Seraph;

namespace
{
constexpr double kTempo = 138.0;
constexpr double kPi = 3.14159265358979323846;

struct Note
{
    double beat, beats;
    int key;
    float velocity = 0.85f;
};

double beatsToSeconds(double beats) { return beats * 60.0 / kTempo; }

// Renders the device over `notes` (song position from beat 0, transport playing) for `seconds`.
juce::AudioBuffer<float> render(S& s, double rate, double seconds, const std::vector<Note>& notes)
{
    const int total = (int) (seconds * rate), block = 256;
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int start = 0; start < total; start += block)
    {
        const int len = std::min(block, total - start);
        devices::HostInfo host;
        host.tempo = kTempo;
        host.sampleRate = rate;
        host.playing = true;
        host.beats = start / rate * kTempo / 60.0;
        s.setHostInfo(host);
        juce::MidiBuffer midi;
        for (const auto& n : notes)
        {
            const int on = (int) std::lround(beatsToSeconds(n.beat) * rate), off = (int) std::lround(beatsToSeconds(n.beat + n.beats) * rate);
            if (on >= start && on < start + len) midi.addEvent(juce::MidiMessage::noteOn(1, n.key, n.velocity), on - start);
            if (off >= start && off < start + len) midi.addEvent(juce::MidiMessage::noteOff(1, n.key), off - start);
        }
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        s.process(view, midi);
    }
    return out;
}

void fadeOut(juce::AudioBuffer<float>& a, double rate, double seconds)
{
    const int n = std::min(a.getNumSamples(), (int) (seconds * rate));
    a.applyGainRamp(a.getNumSamples() - n, n, 1.0f, 0.0f);
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

std::string fileSafe(std::string s)
{
    for (auto& c : s)
        if (c == '/' || c == ':' || c == '\'') c = c == '\'' ? '_' : '-';
    return s;
}

// The note a "held A" preset sings: in the section's own range.
int heldA(const S& s)
{
    switch ((int) std::lround(s.getParameter(S::Section)))
    {
        case S::Women: case S::Boys: case S::SoloSoprano: case S::SoloAlto: return 69;
        case S::SoloBass: return 45;
        default: return 57;
    }
}

// ---- a kick and a rolling bass for the context render (simple synthesis; only for listening)
void addKickAndBass(juce::AudioBuffer<float>& out, double rate, int bars, int fromBar)
{
    const double beatSeconds = 60.0 / kTempo;
    for (int beat = fromBar * 4; beat < bars * 4; ++beat)
    {
        // kick: a sine falling from 160 Hz to 45 Hz with a 250 ms tail
        const int at = (int) (beat * beatSeconds * rate);
        double phase = 0;
        for (int i = 0; i < (int) (0.3 * rate) && at + i < out.getNumSamples(); ++i)
        {
            const double t = i / rate;
            const double hz = 45.0 + 115.0 * std::exp(-t * 28.0);
            phase += 2 * kPi * hz / rate;
            const float v = (float) (0.75 * std::sin(phase) * std::exp(-t * 9.0) * std::min(1.0, t * 2000.0));
            out.addSample(0, at + i, v);
            out.addSample(1, at + i, v);
        }
        // bass: A1 on the three sixteenths after the kick, 60 % of a sixteenth, a closing low-pass
        for (int s = 1; s < 4; ++s)
        {
            const int start = (int) ((beat + s * 0.25) * beatSeconds * rate), length = (int) (0.25 * beatSeconds * 0.6 * rate);
            double ph = 0, lp = 0;
            for (int i = 0; i < length && start + i < out.getNumSamples(); ++i)
            {
                const double t = i / rate;
                ph += 55.0 / rate;
                const double saw = 2.0 * (ph - std::floor(ph)) - 1.0;
                const double cutoff = 300.0 + 1800.0 * std::exp(-t * 40.0);
                lp += (saw - lp) * (1.0 - std::exp(-2.0 * kPi * cutoff / rate));
                const double env = std::min(1.0, t * 1000.0) * std::min(1.0, (length - i) / (0.003 * rate));
                const float v = (float) (0.35 * lp * env);
                out.addSample(0, start + i, v);
                out.addSample(1, start + i, v);
            }
        }
    }
}

int presetIndex(const char* name)
{
    const auto& list = S::presets();
    for (size_t i = 0; i < list.size(); ++i)
        if (std::strcmp(list[i].name, name) == 0) return (int) i;
    return 0;
}

std::vector<Note> progression(int bars) // Am F C G, two bars each, voiced around C4
{
    static const int chords[4][3] = {{57, 60, 64}, {57, 60, 65}, {55, 60, 64}, {55, 59, 62}};
    std::vector<Note> notes;
    for (int bar = 0; bar < bars; bar += 2)
        for (int k : chords[(bar / 2) % 4]) notes.push_back({bar * 4.0, 8.0, k});
    return notes;
}

// A dry, held sung note from the Voices engine: the stand-in recording for the Real demos (nothing recorded is used).
std::shared_ptr<const devices::seraph::RealVoice> synthesisedNote(double rate, int section, int key, float vowel, const std::string& name)
{
    S s;
    s.setParameter(S::Section, (float) section);
    for (int p : {S::Space, S::Detune, S::Timing, S::Width, S::Scoop}) s.setParameter(p, 0.0f);
    s.setParameter(S::Vowel, vowel);
    s.setParameter(S::Vibrato, 25);
    s.setParameter(S::Fold, 0);
    s.prepare(rate, 512);
    auto out = render(s, rate, 3.0, {{0.0, 2.8 * kTempo / 60.0, key}});
    std::string reason;
    auto voice = devices::seraph::analyseRealVoice(out.getArrayOfReadPointers(), 1, out.getNumSamples(), rate, name, reason);
    if (voice == nullptr) std::fprintf(stderr, "%s: %s\n", name.c_str(), reason.c_str());
    return voice;
}

int realDemos(const juce::File& folder, double rate, const std::vector<std::string>& files)
{
    const auto target = folder.getChildFile(files.empty() ? "Real (from synthesised notes)" : "Real (your files)");
    if (! target.createDirectory().wasOk()) return 1;
    std::vector<std::shared_ptr<const devices::seraph::RealVoice>> voices;
    if (files.empty())
    {
        voices.push_back(synthesisedNote(rate, S::SoloBass, 45, 3, "man_oh_A2"));
        voices.push_back(synthesisedNote(rate, S::SoloTenor, 52, 0, "man_ah_E3"));
        voices.push_back(synthesisedNote(rate, S::SoloAlto, 64, 0, "woman_ah_E4"));
        voices.push_back(synthesisedNote(rate, S::SoloSoprano, 69, 3, "woman_oh_A4"));
    }
    else
        for (const auto& f : files)
        {
            std::string error;
            if (auto v = S::analyseFile(f, error)) voices.push_back(v);
            else std::fprintf(stderr, "%s\n", error.c_str());
        }
    voices.erase(std::remove(voices.begin(), voices.end(), nullptr), voices.end());
    if (voices.empty()) { std::fprintf(stderr, "no usable recording\n"); return 1; }
    const auto& list = S::presets();
    const double eightBars = beatsToSeconds(32.0);
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (std::string(list[i].category) != "Real") continue;
        S s;
        s.applyPreset((int) i);
        for (const auto& v : voices) s.addRealVoice(v);
        s.prepare(rate, 512);
        const bool line = std::string(list[i].name) == "Real Soloist" || std::string(list[i].name) == "Real Chant";
        std::vector<Note> notes;
        if (line) notes.push_back({0.0, 32.0, heldA(s)});
        else notes = progression(8);
        auto out = render(s, rate, eightBars + 4.0, notes);
        fadeOut(out, rate, 1.0);
        const auto name = fileSafe(std::string("Real - ") + list[i].name) + ".wav";
        writeWav(target.getChildFile(name), out, rate);
        std::printf("%-48s peak %.2f\n", name.c_str(), std::max(out.getMagnitude(0, 0, out.getNumSamples()), out.getMagnitude(1, 0, out.getNumSamples())));
    }
    // one recording, one singer, then the same recording as a section of six
    {
        S solo;
        solo.setParameter(S::Source, S::RealSource);
        solo.setParameter(S::Section, S::SoloTenor);
        solo.addRealVoice(voices.front());
        solo.prepare(rate, 512);
        S section;
        section.setParameter(S::Source, S::RealSource);
        section.setParameter(S::Section, S::Men);
        section.setParameter(S::Singers, 6);
        section.addRealVoice(voices.front());
        section.prepare(rate, 512);
        auto a = render(solo, rate, 6.0, {{0.0, 5.0, 50}}), b = render(section, rate, 6.0, {{0.0, 5.0, 50}});
        juce::AudioBuffer<float> both(2, a.getNumSamples() + b.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
        {
            both.copyFrom(ch, 0, a, ch, 0, a.getNumSamples());
            both.copyFrom(ch, a.getNumSamples(), b, ch, 0, b.getNumSamples());
        }
        writeWav(target.getChildFile("Context - One recording, then six singers from it.wav"), both, rate);
        std::printf("Context - One recording, then six singers from it.wav\n");
    }
    return 0;
}

int packDemos(const juce::File& folder, double rate, const std::string& packsRoot)
{
    const auto target = folder.getChildFile("Real Choir (Seraph Voices pack)");
    if (! target.createDirectory().wasOk()) return 1;
    if (S::findPackVoices(packsRoot).folder.empty()) { std::fprintf(stderr, "no Seraph Voices pack in %s\n", packsRoot.c_str()); return 1; }
    const auto& list = S::presets();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (std::string(list[i].category) != "Real Choir") continue;
        S s;
        s.packsRoot = packsRoot;
        const auto t0 = std::chrono::steady_clock::now();
        s.applyPreset((int) i);
        const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        s.prepare(rate, 512);
        const bool line = std::string(list[i].name) == "Recorded Chant";
        std::vector<Note> notes;
        if (line) notes.push_back({0.0, 32.0, heldA(s)});
        else notes = progression(8);
        auto out = render(s, rate, beatsToSeconds(32.0) + 4.0, notes);
        fadeOut(out, rate, 1.0);
        const auto name = fileSafe(std::string("Real Choir - ") + list[i].name) + ".wav";
        writeWav(target.getChildFile(name), out, rate);
        std::printf("%-44s %zu recordings loaded in %.2f s, peak %.2f\n", name.c_str(), s.realVoices().size(), loadSeconds,
                    std::max(out.getMagnitude(0, 0, out.getNumSamples()), out.getMagnitude(1, 0, out.getNumSamples())));
    }
    target.getChildFile("CREDIT.txt").replaceWithText(juce::String(S::findPackVoices(packsRoot).credit) + "\n");
    return 0;
}

int demos(const juce::File& folder, double rate, const std::string& only)
{
    if (! folder.createDirectory().wasOk()) { std::fprintf(stderr, "cannot create %s\n", folder.getFullPathName().toRawUTF8()); return 1; }
    const auto& list = S::presets();
    const double eightBars = beatsToSeconds(32.0);
    std::string lastCategory;
    for (size_t i = 0; i < list.size(); ++i)
    {
        const auto& p = list[i];
        if (! only.empty() && std::string(p.name).find(only) == std::string::npos) continue;
        if (std::string(p.category).rfind("Real", 0) == 0) continue; // they need recordings: --real-only, --pack-demos
        for (int dry = 0; dry < 2; ++dry)
        {
            if (dry == 1 && lastCategory == p.category) continue; // one dry version per category: its first preset
            S s;
            s.applyPreset((int) i);
            if (dry == 1) s.setParameter(S::Space, 0.0f);
            s.prepare(rate, 512);
            const bool line = std::string(p.category) == "Solo" || std::string(p.category) == "Chants";
            std::vector<Note> notes;
            if (line) notes.push_back({0.0, 32.0, heldA(s)});
            else for (int k : {57, 60, 64}) notes.push_back({0.0, 32.0, k});
            const double space = dry == 1 ? 0.0 : std::min(6.0, (double) S::kSpaceSeconds[(int) std::lround(s.getParameter(S::SpaceSize))]);
            auto out = render(s, rate, eightBars + s.getParameter(S::Release) / 1000.0 + space * 0.6 + 0.5, notes);
            fadeOut(out, rate, 0.5);
            const auto name = fileSafe(std::string(p.category) + " - " + p.name + (dry == 1 ? " (dry)" : "")) + ".wav";
            if (! writeWav(folder.getChildFile(name), out, rate)) { std::fprintf(stderr, "could not write %s\n", name.c_str()); return 1; }
            std::printf("%-48s peak %.2f\n", name.c_str(), std::max(out.getMagnitude(0, 0, out.getNumSamples()), out.getMagnitude(1, 0, out.getNumSamples())));
        }
        lastCategory = p.category;
    }
    if (! only.empty()) return 0;

    // 1. a gated women's pad over a kick and rolling bass (16 bars: 4 bars of pad alone, then the floor comes in)
    {
        S s;
        s.applyPreset(presetIndex("Gated Angels"));
        s.prepare(rate, 512);
        auto out = render(s, rate, beatsToSeconds(64.0) + 2.0, progression(16));
        out.applyGain(0.8f);
        addKickAndBass(out, rate, 16, 4);
        fadeOut(out, rate, 1.5);
        writeWav(folder.getChildFile("Context - Gated women over kick and bass.wav"), out, rate);
        std::printf("Context - Gated women over kick and bass.wav\n");
    }
    // 2. a monks' chant with Fifth organum and Ison (16 bars on a held A3)
    {
        S s;
        s.applyPreset(presetIndex("Phrygian Monks"));
        s.prepare(rate, 512);
        auto out = render(s, rate, beatsToSeconds(64.0) + 4.0, {{0.0, 64.0, 57}});
        fadeOut(out, rate, 2.0);
        writeWav(folder.getChildFile("Context - Monks chant with fifth organum and ison.wav"), out, rate);
        std::printf("Context - Monks chant with fifth organum and ison.wav\n");
    }
    // 3. a mixed choir swell over Am F C G
    {
        S s;
        s.applyPreset(presetIndex("Soft Swell"));
        s.prepare(rate, 512);
        auto out = render(s, rate, beatsToSeconds(64.0) + 5.0, progression(16));
        fadeOut(out, rate, 2.0);
        writeWav(folder.getChildFile("Context - Mixed choir swell.wav"), out, rate);
        std::printf("Context - Mixed choir swell.wav\n");
    }
    return 0;
}

double timeRender(S& s, double rate, int block, const std::vector<Note>& notes, double seconds, double& worstBlockMs, int& singers)
{
    const int total = (int) (seconds * rate);
    juce::AudioBuffer<float> audio(2, block);
    double sum = 0;
    worstBlockMs = 0;
    singers = 0;
    for (int start = 0; start < total; start += block)
    {
        audio.clear();
        devices::HostInfo host;
        host.tempo = kTempo;
        host.sampleRate = rate;
        host.playing = true;
        host.beats = start / rate * kTempo / 60.0;
        s.setHostInfo(host);
        juce::MidiBuffer midi;
        if (start == 0)
            for (const auto& n : notes) midi.addEvent(juce::MidiMessage::noteOn(1, n.key, n.velocity), 0);
        const auto t = std::chrono::steady_clock::now();
        s.process(audio, midi);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        if (start > (int) rate) // after the first second (the onsets are spread)
        {
            sum += ms;
            worstBlockMs = std::max(worstBlockMs, ms);
            singers = std::max(singers, s.activeSingers());
        }
    }
    return sum / 1000.0 / (seconds - 1.0); // fraction of one core
}

int cpu(double rate, int block)
{
    const double budgetMs = block / rate * 1000.0;
    {
        S s;
        s.setParameter(S::Section, S::Mixed);
        s.setParameter(S::MenOctave, 1);
        s.setParameter(S::Singers, 8);
        s.prepare(rate, block);
        double worst = 0;
        int singers = 0;
        const double load = timeRender(s, rate, block, {{0, 64, 57}, {0, 64, 60}, {0, 64, 64}, {0, 64, 69}}, 10.0, worst, singers);
        std::printf("64 singers (%d sounding), %.0f Hz, %d-sample blocks: %.1f %% of one core, worst block %.2f ms of %.2f ms\n", singers,
                    rate, block, load * 100.0, worst, budgetMs);
    }
    {
        S s;
        s.prepare(rate, block);
        double worst = 0;
        int singers = 0;
        const double load = timeRender(s, rate, block, {{0, 64, 57}, {0, 64, 60}, {0, 64, 64}}, 10.0, worst, singers);
        std::printf("default preset, A minor chord (%d singers): %.1f %% of one core, worst block %.2f ms of %.2f ms\n", singers, load * 100.0,
                    worst, budgetMs);
    }
    return 0;
}

int makeSet(const juce::File& file, bool stress)
{
    std::string error;
    if (! io::writeAlsDom(*io::createNewSetDom(), file, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    auto result = io::loadAls(file);
    if (! result) { std::fprintf(stderr, "cannot reload the new set\n"); return 1; }
    auto& song = *result.song;
    int track = -1;
    for (size_t t = 0; t < song.tracks.size() && track < 0; ++t)
        if (song.tracks[t].kind == model::TrackKind::Midi) track = (int) t;
    if (track < 0) { std::fprintf(stderr, "no MIDI track in the new set\n"); return 1; }
    model::EditHistory history;
    auto device = devices::nativeDeviceModel("SonoraSeraph");
    auto setParam = [&](const char* id, double v) {
        for (auto& p : device.parameters)
            if (p.path == id) p.value = v;
    };
    if (stress)
    {
        setParam("Section", S::Mixed);
        setParam("MenOctave", 1);
        setParam("Singers", 8);
    }
    model::allocateDeviceIds(song, device);
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    history.perform(song, model::insertDevice(track, std::move(device)));
    if (! song.midiClipTemplate) song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    model::Clip clip;
    if (! model::makeEmptyMidiClip(song, 0.0, 64.0, clip)) { std::fprintf(stderr, "cannot make a clip\n"); return 1; }
    clip.name = stress ? "64 singers" : "A minor";
    for (int k : stress ? std::vector<int>{57, 60, 64, 69} : std::vector<int>{57, 60, 64})
    {
        model::MidiNote n;
        n.start = 0;
        n.duration = 64;
        n.pitch = k;
        n.velocity = 100;
        clip.notes.push_back(n);
    }
    history.perform(song, model::insertClip(track, clip, "Seraph chord"));
    if (! io::saveAls(song, file, {}).ok) { std::fprintf(stderr, "cannot save\n"); return 1; }
    std::printf("wrote %s (Seraph on track %d)\n", file.getFullPathName().toRawUTF8(), track + 1);
    return 0;
}
} // namespace

namespace
{
// ---- the Seraph Voices pack's audio steps (PLG-27h)
std::unique_ptr<juce::AudioBuffer<float>> readMono(const std::string& path, double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String::fromUTF8(path.c_str()))));
    if (reader == nullptr) return nullptr;
    rate = reader->sampleRate;
    const int n = (int) std::min<int64_t>(reader->lengthInSamples, (int64_t) (rate * 600));
    juce::AudioBuffer<float> all((int) reader->numChannels, n);
    reader->read(&all, 0, n, 0, true, true);
    auto mono = std::make_unique<juce::AudioBuffer<float>>(1, n);
    mono->clear();
    for (int ch = 0; ch < all.getNumChannels(); ++ch) mono->addFrom(0, 0, all, ch, 0, n, 1.0f / (float) all.getNumChannels());
    return mono;
}

int packNotes(const std::string& path)
{
    double rate = 0;
    auto audio = readMono(path, rate);
    if (audio == nullptr) { std::fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
    const auto track = devices::seraph::pitchTrack(audio->getReadPointer(0), audio->getNumSamples(), rate);
    const double hop = 256.0 / rate;
    // held notes: voiced runs whose pitch smoothed over 200 ms stays within 50 cents, at least 1.5 s
    const int reach = (int) (0.1 / hop);
    std::vector<float> smooth(track.size(), -1e9f);
    for (size_t f = 0; f < track.size(); ++f)
    {
        double sum = 0;
        int count = 0;
        for (size_t g = (size_t) std::max(0, (int) f - reach); g < std::min(track.size(), f + (size_t) reach + 1); ++g)
            if (track[g] > 0) { sum += 1200.0 * std::log2(track[g] / 440.0); ++count; }
        if (count > reach) smooth[f] = (float) (sum / count);
    }
    for (size_t f = 0; f < track.size();)
    {
        if (track[f] <= 0 || smooth[f] < -1e8f) { ++f; continue; }
        const float reference = smooth[f];
        size_t g = f;
        while (g < track.size() && track[g] > 0 && std::abs(smooth[g] - reference) < 50.0f) ++g;
        if ((double) (g - f) * hop >= 1.5)
        {
            std::vector<float> cents;
            for (size_t k = f; k < g; ++k) cents.push_back(1200.0f * std::log2(track[k] / 440.0f));
            std::nth_element(cents.begin(), cents.begin() + (long) cents.size() / 2, cents.end());
            std::printf("%.3f %.3f %.2f\n", (double) f * hop, (double) g * hop + 0.02, 69.0 + cents[cents.size() / 2] / 100.0);
        }
        f = std::max(g, f + 1);
    }
    return 0;
}

int packCut(const std::string& in, double from, double to, const std::string& out)
{
    double rate = 0;
    auto audio = readMono(in, rate);
    if (audio == nullptr) { std::fprintf(stderr, "cannot read %s\n", in.c_str()); return 1; }
    const int a = std::clamp((int) (from * rate), 0, audio->getNumSamples()), b = std::clamp((int) (to * rate), a, audio->getNumSamples());
    juce::AudioBuffer<float> note(1, b - a);
    note.copyFrom(0, 0, *audio, 0, a, b - a);
    // level: the second half's RMS (the sustain) to -20 dBFS, the peak kept under -1 dBFS
    double sum = 0;
    for (int i = note.getNumSamples() / 2; i < note.getNumSamples(); ++i) sum += (double) note.getSample(0, i) * note.getSample(0, i);
    const double rms = std::sqrt(sum / std::max(1, note.getNumSamples() / 2));
    float gain = rms > 1e-6 ? (float) (std::pow(10.0, -20.0 / 20.0) / rms) : 1.0f;
    const float top = note.getMagnitude(0, 0, note.getNumSamples()) * gain;
    if (top > 0.89f) gain *= 0.89f / top;
    note.applyGain(gain);
    const int fade = (int) (0.01 * rate);
    note.applyGainRamp(0, std::min(fade, note.getNumSamples()), 0.0f, 1.0f);
    note.applyGainRamp(std::max(0, note.getNumSamples() - fade), std::min(fade, note.getNumSamples()), 1.0f, 0.0f);
    // 44.1 kHz (VocalSet is): written as FLAC
    const juce::File file(juce::String::fromUTF8(out.c_str()));
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    juce::FlacAudioFormat flac;
    auto writer = flac.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(24));
    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer(note, 0, note.getNumSamples())) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    return 0;
}

int packCheck(const std::vector<std::string>& files)
{
    int refused = 0;
    for (const auto& f : files)
    {
        std::string error;
        if (const auto v = S::analyseFile(f, error))
            std::printf("ok %.2f %.2f %s\n", (double) v->midi, v->sustainSeconds(), f.c_str());
        else
        {
            std::printf("refused %s: %s\n", f.c_str(), error.c_str());
            ++refused;
        }
    }
    return refused > 0 ? 3 : 0;
}

// PLG-27d: what a real held note does, in the Voices engine's terms.
int measure(const std::vector<std::string>& files)
{
    for (const auto& f : files)
    {
        std::string error;
        const auto v = S::analyseFile(f, error);
        if (v == nullptr) { std::printf("refused %s\n", f.c_str()); continue; }
        // pitch per period over the sustain (from the marks), in cents around its median
        std::vector<double> cents, amplitude, periods;
        for (int i = v->sustainBegin + 1; i < v->sustainEnd; ++i)
        {
            const double p = v->marks[(size_t) i] - v->marks[(size_t) i - 1];
            periods.push_back(p);
            double e = 0;
            for (int k = (int) v->marks[(size_t) i - 1]; k < (int) v->marks[(size_t) i]; ++k) e += (double) v->audio[(size_t) k] * v->audio[(size_t) k];
            amplitude.push_back(std::sqrt(e / std::max(1.0, p)));
        }
        if (periods.size() < 50) continue;
        std::vector<double> sorted = periods;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        for (double p : periods) cents.push_back(1200.0 * std::log2(median / p));
        // smoothed pitch (about 5 periods): vibrato and drift without the period-to-period jitter
        std::vector<double> smooth(cents.size()), slow(cents.size());
        for (size_t i = 0; i < cents.size(); ++i)
        {
            double s = 0, sl = 0;
            int n = 0, nl = 0;
            const int window = (int) std::max(2.0, 0.5 * v->sampleRate / median); // half a second of periods
            for (int k = -2; k <= 2; ++k)
                if ((int) i + k >= 0 && (size_t) ((int) i + k) < cents.size()) { s += cents[(size_t) ((int) i + k)]; ++n; }
            for (int k = -window / 2; k <= window / 2; ++k)
                if ((int) i + k >= 0 && (size_t) ((int) i + k) < cents.size()) { sl += cents[(size_t) ((int) i + k)]; ++nl; }
            smooth[i] = s / n;
            slow[i] = sl / nl;
        }
        double vib = 0, drift = 0, slowMean = 0;
        int crossings = 0;
        for (size_t i = 0; i < smooth.size(); ++i)
        {
            const double d = smooth[i] - slow[i];
            vib += d * d;
            slowMean += slow[i];
            if (i > 0 && ((smooth[i - 1] - slow[i - 1]) < 0) != (d < 0)) ++crossings;
        }
        slowMean /= (double) slow.size();
        for (double s : slow) drift += (s - slowMean) * (s - slowMean);
        double seconds = 0;
        for (double p : periods) seconds += p / v->sampleRate;
        double jitter = 0, shimmer = 0, meanAmp = 0;
        for (size_t i = 1; i < periods.size(); ++i)
        {
            jitter += std::abs(periods[i] - periods[i - 1]);
            shimmer += std::abs(amplitude[i] - amplitude[i - 1]);
            meanAmp += amplitude[i];
        }
        const double count = (double) periods.size() - 1;
        std::printf("note %.1f vibrato %.2f %.1f drift %.1f jitter %.2f shimmer %.2f %s\n", (double) v->midi, crossings * 0.5 / seconds,
                    std::sqrt(vib / (double) smooth.size()) * std::sqrt(2.0), std::sqrt(drift / (double) slow.size()),
                    100.0 * (jitter / count) / median, 100.0 * (shimmer / count) / (meanAmp / count), f.c_str());
    }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--pack-notes") return packNotes(argv[2]);
    if (argc >= 6 && std::string(argv[1]) == "--pack-cut") return packCut(argv[2], std::atof(argv[3]), std::atof(argv[4]), argv[5]);
    if (argc >= 3 && std::string(argv[1]) == "--pack-check") return packCheck(std::vector<std::string>(argv + 2, argv + argc));
    if (argc >= 3 && std::string(argv[1]) == "--measure") return measure(std::vector<std::string>(argv + 2, argv + argc));
    if (argc >= 3 && std::string(argv[1]) == "--default-note")
    {
        S s;
        s.setParameter(S::Section, argc >= 5 ? (float) std::atoi(argv[4]) : (float) S::SoloTenor);
        if (argc >= 6) s.setParameter(S::Drift, (float) std::atof(argv[5]));
        if (argc >= 7) s.setParameter(S::Seed, (float) std::atof(argv[6]));
        s.setParameter(S::Space, 0);
        s.setParameter(S::Fold, 0);
        s.prepare(44100, 512);
        auto out = render(s, 44100, 3.8, {{0.0, 3.5 * kTempo / 60.0, argc >= 4 ? std::atoi(argv[3]) : 57}});
        return writeWav(juce::File(juce::String::fromUTF8(argv[2])), out, 44100) ? 0 : 1;
    }
    double rate = 0;
    int block = 512;
    std::string only, folder, setPath;
    std::vector<std::string> realFiles;
    std::string packDemoRoot;
    bool doCpu = false, stress = false, realOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--block" && i + 1 < argc) block = std::atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--cpu") doCpu = true;
        else if (a == "--make-set" && i + 1 < argc) setPath = argv[++i];
        else if (a == "--stress") stress = true;
        else if (a == "--real-only") realOnly = true;
        else if (a == "--pack-demos" && i + 1 < argc) packDemoRoot = argv[++i];
        else if (a == "--real")
            while (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) realFiles.push_back(argv[++i]);
        else if (folder.empty() && a.rfind("--", 0) != 0) folder = a;
    }
    if (doCpu) return cpu(rate > 0 ? rate : 48000.0, std::max(16, block));
    if (! setPath.empty()) return makeSet(juce::File(juce::String::fromUTF8(setPath.c_str())), stress);
    if (folder.empty())
    {
        std::fprintf(stderr, "usage: seraph_demo <folder> [--rate R] [--only TEXT] | --cpu [--rate R] [--block N] | --make-set out.als [--stress]\n");
        return 2;
    }
    const juce::File out(juce::String::fromUTF8(folder.c_str()));
    if (! packDemoRoot.empty()) return packDemos(out, rate > 0 ? rate : 44100.0, packDemoRoot);
    if (realOnly || ! realFiles.empty()) return realDemos(out, rate > 0 ? rate : 44100.0, realFiles);
    const int result = demos(out, rate > 0 ? rate : 44100.0, only);
    return result != 0 || ! only.empty() ? result : realDemos(out, rate > 0 ? rate : 44100.0, {});
}
