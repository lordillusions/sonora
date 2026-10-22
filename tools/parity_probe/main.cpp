// DEV-22: native device parity against the reference application.
//
//   parity_probe make <dir> <suite.txt> [--library <Core Library folder>]
//       Writes <dir>/Parity.als (one MIDI track per case: an arrangement clip plus the case's device chain),
//       our own generated test samples in <dir>/Samples and <dir>/cases.tsv. Device XML starts from the
//       reference application's installed default/factory presets, read at run time from --library (never
//       copied into this repository) and patched with the case's parameter edits.
//   parity_probe render <dir>
//       Sonora renders every track of <dir>/Parity.als as a 32-bit float stem into <dir>/sonora.
//   parity_probe compare <dir> <reference stems folder>
//       Matches stems by track name ("P<NN> <case>") and prints level / envelope / spectrum / null-test
//       differences per case; writes <dir>/report.tsv and per-case envelope CSVs into <dir>/curves.
//
// Reference stems: open Parity.als in the reference application and export "All Individual Tracks"
// (44.1 kHz, 32-bit float, no normalize/dither) for the loop range (bars 1-5).
//
// Suite format (one case per line, '#' comments):
//   case <name> [sample=<saw|ramp|noiseburst>] [notes=<melody|single|velocities|keys|legato|chords>]
//     device <template.adv relative to the library, or absolute> [<path>=<value> ...] [part:<path>=<value> ...]
//     device ...
//   presets <folder relative to the library> [max=<n>] [source=<case name>]
//       one case per factory preset in the folder, each placed after the device chain of `source`.

#include "core/Edit.h"
#include "core/DeviceCatalog.h"
#include "engine/AudioEngine.h"
#include "engine/Exporter.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <sstream>

using namespace sonora;
using juce::XmlElement;

namespace
{
constexpr double kSampleRate = 44100.0;
constexpr double kTempo = 120.0;
constexpr double kLengthBeats = 20.0; // five 4/4 bars: notes end by beat 15, the rest is release/tail
const char* kDefaultLibrary = "/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Core Library";

struct Edit
{
    std::string path, value;
    bool part = false;
};
struct DeviceSpec
{
    juce::File templateFile;
    std::vector<Edit> edits;
};
struct Case
{
    std::string name, sample = "saw", notes = "melody";
    std::vector<DeviceSpec> chain;
};

// ------------------------------------------------------------------------------------------------ samples
// Our own deterministic test material (44.1 kHz stereo float WAV); nothing comes from the reference library.

juce::AudioBuffer<float> makeSample(const std::string& kind)
{
    const int frames = (int) kSampleRate; // 1 s: long notes run past the end (tests end / loop behaviour)
    juce::AudioBuffer<float> b(2, frames);
    b.clear();
    const double twoPi = 2.0 * std::numbers::pi;
    if (kind == "saw") // band-limited saw at C3 (261.63 Hz), root key 60
    {
        const double f = 261.6255653005986;
        for (int h = 1; f * h < 20000.0; ++h)
            for (int i = 0; i < frames; ++i)
            {
                const float v = (float) (0.5 * (2.0 / std::numbers::pi) * std::sin(twoPi * f * h * i / kSampleRate) / h * (h % 2 ? 1 : -1));
                b.addSample(0, i, v);
                b.addSample(1, i, v);
            }
    }
    else if (kind == "ramp") // L: 440 Hz rising 0 -> 0.8, R: 660 Hz falling 0.8 -> 0 (start/loop/pan positions are visible)
    {
        for (int i = 0; i < frames; ++i)
        {
            const double t = (double) i / frames;
            b.setSample(0, i, (float) (0.8 * t * std::sin(twoPi * 440.0 * i / kSampleRate)));
            b.setSample(1, i, (float) (0.8 * (1.0 - t) * std::sin(twoPi * 660.0 * i / kSampleRate)));
        }
    }
    else if (kind.rfind("tone", 0) == 0) // tone<Hz>[-<seconds>]: pure sine at -6 dBFS (loop-point snapping probes)
    {
        const auto dash = kind.find('-');
        const double hz = std::atof(kind.substr(4, dash == std::string::npos ? std::string::npos : dash - 4).c_str());
        const double seconds = dash == std::string::npos ? 1.0 : std::atof(kind.substr(dash + 1).c_str());
        b.setSize(2, (int) (seconds * kSampleRate));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = (float) (0.5 * std::sin(twoPi * hz * i / kSampleRate));
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
    }
    else if (kind == "sine") // pure C3 sine at -6 dBFS (levels, envelopes, pan law)
    {
        for (int i = 0; i < frames; ++i)
        {
            const float v = (float) (0.5 * std::sin(twoPi * 261.6255653005986 * i / kSampleRate));
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
    }
    else if (kind == "noise") // white noise, identical channels (filter responses)
    {
        juce::Random random(11);
        for (int i = 0; i < frames; ++i)
        {
            const float v = (random.nextFloat() * 2.0f - 1.0f) * 0.25f;
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
    }
    else // noiseburst: 4 decaying noise hits (onsets at 0, 250, 500, 750 ms) over a quiet 110 Hz sine
    {
        juce::Random random(7);
        for (int i = 0; i < frames; ++i)
        {
            const int hit = i % (frames / 4);
            const float noise = (random.nextFloat() * 2.0f - 1.0f) * 0.7f * std::exp(-(float) hit / 2000.0f);
            const float sine = 0.1f * (float) std::sin(twoPi * 110.0 * i / kSampleRate);
            b.setSample(0, i, noise + sine);
            b.setSample(1, i, noise * 0.8f + sine);
        }
    }
    return b;
}

bool writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr) return false;
    auto writer = juce::WavAudioFormat().createWriterFor(
        stream, juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(audio.getNumChannels()).withBitsPerSample(24));
    return writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

std::vector<model::MidiNote> makeNotes(const std::string& kind)
{
    using N = model::MidiNote;
    auto n = [](double start, double length, int pitch, float velocity) { N x; x.start = start; x.duration = length; x.pitch = pitch; x.velocity = velocity; return x; };
    if (kind == "single") return {n(0, 2, 60, 100)};
    if (kind == "velocities") return {n(0, 1, 60, 1), n(2, 1, 60, 32), n(4, 1, 60, 64), n(6, 1, 60, 96), n(8, 1, 60, 127)};
    if (kind == "keys") return {n(0, 1, 36, 100), n(2, 1, 48, 100), n(4, 1, 60, 100), n(6, 1, 72, 100), n(8, 1, 84, 100)};
    if (kind == "legato") return {n(0, 1.1, 60, 100), n(1, 1.1, 67, 100), n(2, 1.1, 64, 100), n(3, 1, 72, 100), n(6, 0.5, 60, 100), n(7, 0.5, 48, 100)};
    if (kind == "chords") return {n(0, 2, 60, 100), n(0, 2, 64, 100), n(0, 2, 67, 100), n(4, 2, 57, 80), n(4, 2, 60, 80), n(4, 2, 64, 80)};
    if (kind == "vsweep") // velocity 1, 8, 16, ... 120, 127: one half-beat note per beat
    {
        std::vector<N> notes;
        for (int i = 0; i < 17; ++i) notes.push_back(n(i * 1.0 * 16.0 / 17.0, 0.4, 60, i == 0 ? 1.0f : std::min(127.0f, (float) i * 8.0f)));
        return notes;
    }
    if (kind == "long") return {n(0, 8, 60, 127)};
    if (kind == "stack") // 16 simultaneous notes two semitones apart (voice-count probes)
    {
        std::vector<N> notes;
        for (int i = 0; i < 16; ++i) notes.push_back(n(0.0, 4.0, 48 + 2 * i, 100));
        return notes;
    }
    if (kind == "slices") return {n(0, 0.5, 36, 100), n(1, 0.5, 37, 100), n(2, 0.5, 38, 100), n(3, 0.5, 39, 100), n(4, 0.5, 40, 100)};
    // melody: one shot of everything: velocities, release, a chord, a note longer than the sample, a fast legato pair
    return {n(0, 1, 60, 127), n(2, 0.5, 67, 64), n(3, 0.25, 48, 100),
            n(4, 2, 60, 100), n(4, 2, 64, 100), n(4, 2, 67, 100),
            n(8, 3.5, 60, 100),
            n(12, 0.6, 64, 90), n(12.5, 0.75, 65, 90)};
}

// ------------------------------------------------------------------------------------------------ suite

std::vector<std::string> tokens(const std::string& line)
{
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (char c : line)
    {
        if (c == '"') { quoted = ! quoted; continue; }
        if (! quoted && (c == ' ' || c == '\t'))
        {
            if (! current.empty()) out.push_back(current);
            current.clear();
        }
        else current += c;
    }
    if (! current.empty()) out.push_back(current);
    return out;
}

juce::File libraryFile(const juce::File& library, const std::string& path)
{
    return juce::File::isAbsolutePath(path) ? juce::File(path) : library.getChildFile(path);
}

// Root device tag of a preset file ("OriginalSimpler", "Eq8", ...), or empty.
std::string presetDeviceTag(const juce::File& preset)
{
    std::string error;
    auto dom = io::readAlsDom(preset, error);
    if (dom == nullptr || dom->getFirstChildElement() == nullptr) return {};
    return dom->getFirstChildElement()->getTagName().toStdString();
}

bool parseSuite(const juce::File& file, const juce::File& library, std::vector<Case>& cases, std::string& error)
{
    std::ifstream in(file.getFullPathName().toStdString());
    if (! in) { error = "cannot read suite " + file.getFullPathName().toStdString(); return false; }
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line))
    {
        ++lineNumber;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto t = tokens(line);
        if (t.empty()) continue;
        if (t[0] == "case" && t.size() >= 2)
        {
            Case c;
            c.name = t[1];
            for (size_t i = 2; i < t.size(); ++i)
            {
                if (t[i].rfind("sample=", 0) == 0) c.sample = t[i].substr(7);
                else if (t[i].rfind("notes=", 0) == 0) c.notes = t[i].substr(6);
            }
            cases.push_back(std::move(c));
        }
        else if (t[0] == "device" && t.size() >= 2 && ! cases.empty())
        {
            DeviceSpec d;
            d.templateFile = libraryFile(library, t[1]);
            if (! d.templateFile.existsAsFile()) { error = "line " + std::to_string(lineNumber) + ": no template " + t[1]; return false; }
            for (size_t i = 2; i < t.size(); ++i)
            {
                const auto eq = t[i].find('=');
                if (eq == std::string::npos) { error = "line " + std::to_string(lineNumber) + ": bad edit " + t[i]; return false; }
                Edit e{t[i].substr(0, eq), t[i].substr(eq + 1)};
                if (e.path.rfind("part:", 0) == 0) { e.part = true; e.path = e.path.substr(5); }
                d.edits.push_back(e);
            }
            cases.back().chain.push_back(std::move(d));
        }
        else if (t[0] == "presets" && t.size() >= 2)
        {
            int maximum = 1000;
            std::string source;
            for (size_t i = 2; i < t.size(); ++i)
            {
                if (t[i].rfind("max=", 0) == 0) maximum = std::atoi(t[i].c_str() + 4);
                else if (t[i].rfind("source=", 0) == 0) source = t[i].substr(7);
            }
            const auto folder = libraryFile(library, t[1]);
            auto files = folder.findChildFiles(juce::File::findFiles, true, "*.adv");
            files.sort();
            const Case* base = nullptr;
            for (const auto& c : cases)
                if (c.name == source) base = &c;
            if (! source.empty() && base == nullptr) { error = "line " + std::to_string(lineNumber) + ": unknown source " + source; return false; }
            const Case baseCopy = base != nullptr ? *base : Case{};
            int added = 0;
            for (const auto& preset : files)
            {
                if (added >= maximum) break;
                const auto tag = presetDeviceTag(preset);
                const auto* entry = model::findCatalogEntry(tag);
                if (entry == nullptr || ! entry->implemented)
                {
                    std::printf("skip %s (%s has no Sonora implementation)\n", preset.getFileName().toRawUTF8(), tag.c_str());
                    continue;
                }
                Case c = baseCopy;
                c.name = (base != nullptr ? "" : tag + "-") + preset.getFileNameWithoutExtension().removeCharacters(" ").toStdString();
                c.chain.push_back(DeviceSpec{preset, {}});
                cases.push_back(std::move(c));
                ++added;
            }
        }
        else { error = "line " + std::to_string(lineNumber) + ": cannot parse"; return false; }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ XML

XmlElement* childPath(XmlElement* e, const std::string& path)
{
    for (const auto& token : juce::StringArray::fromTokens(juce::String(path), "/", ""))
    {
        if (e == nullptr) return nullptr;
        e = e->getChildByName(token);
    }
    return e;
}

void setValue(XmlElement* e, const char* tag, const juce::String& v)
{
    if (auto* c = e->getChildByName(tag)) c->setAttribute("Value", v);
}

bool applyEdit(XmlElement* root, const Edit& edit, std::string& error)
{
    auto* e = childPath(root, edit.path);
    if (e == nullptr) { error = "no element " + edit.path; return false; }
    if (auto* manual = e->getChildByName("Manual")) manual->setAttribute("Value", juce::String(edit.value));
    else if (e->hasAttribute("Value")) e->setAttribute("Value", juce::String(edit.value));
    else { error = edit.path + " has neither Manual nor Value"; return false; }
    return true;
}

bool isPointee(const juce::String& tag)
{
    return tag.endsWith("AutomationTarget") || tag.endsWith("ModulationTarget") || tag == "Pointee";
}

void renumberPointees(XmlElement& e, int& next)
{
    if (isPointee(e.getTagName()) && e.hasAttribute("Id")) e.setAttribute("Id", next++);
    for (auto* c : e.getChildIterator()) renumberPointees(*c, next);
}

int maxPointee(const XmlElement& e)
{
    int result = isPointee(e.getTagName()) ? e.getIntAttribute("Id", 0) : 0;
    for (const auto* c : e.getChildIterator()) result = std::max(result, maxPointee(*c));
    return result;
}

// Our sample as a MultiSamplePart, using a structural template part taken from an installed factory preset.
std::unique_ptr<XmlElement> samplePart(const XmlElement& templatePart, const juce::File& wav, int frames, int index)
{
    auto p = std::make_unique<XmlElement>(templatePart);
    p->setAttribute("Id", index);
    p->setAttribute("HasImportedSlicePoints", "false");
    p->setAttribute("NeedsAnalysisData", "true");
    p->setAttribute("InitUpdateAreSlicesFromOnsetsEditableAfterRead", "false");
    setValue(p.get(), "Name", wav.getFileNameWithoutExtension());
    setValue(childPath(p.get(), "KeyRange"), "Min", "0"); setValue(childPath(p.get(), "KeyRange"), "Max", "127");
    setValue(p.get(), "RootKey", "60"); setValue(p.get(), "Detune", "0"); setValue(p.get(), "Panorama", "0");
    setValue(p.get(), "Volume", "1"); setValue(p.get(), "SampleStart", "0"); setValue(p.get(), "SampleEnd", juce::String(frames));
    for (const char* loop : {"SustainLoop", "ReleaseLoop"})
        if (auto* l = childPath(p.get(), loop))
        {
            setValue(l, "Start", "0"); setValue(l, "End", juce::String(frames));
            setValue(l, "Mode", juce::String(loop[0] == 'S' ? 0 : 3)); setValue(l, "Crossfade", "0");
        }
    if (auto* ref = childPath(p.get(), "SampleRef"))
    {
        if (auto* f = ref->getChildByName("FileRef"))
        {
            setValue(f, "RelativePathType", "0"); // absolute reference (no project folder)
            setValue(f, "RelativePath", "");
            setValue(f, "Path", wav.getFullPathName());
            setValue(f, "Type", "1");
            setValue(f, "LivePackName", ""); setValue(f, "LivePackId", "");
            setValue(f, "OriginalFileSize", "0"); // size/CRC 0: no content check (as Sonora's own writer)
            setValue(f, "OriginalCrc", "0");
        }
        if (auto* context = ref->getChildByName("SourceContext")) context->deleteAllChildElements();
        setValue(ref, "LastModDate", juce::String(wav.getLastModificationTime().toMilliseconds() / 1000));
        setValue(ref, "DefaultDuration", juce::String(frames));
        setValue(ref, "DefaultSampleRate", juce::String((int) kSampleRate));
        setValue(ref, "SamplesToAutoWarp", "0");
    }
    if (auto* warp = childPath(p.get(), "SampleWarpProperties"))
    {
        if (auto* markers = warp->getChildByName("WarpMarkers"))
        {
            markers->deleteAllChildElements();
            const double seconds = frames / kSampleRate;
            int id = 0;
            for (double s : {0.0, seconds})
            {
                auto* m = markers->createNewChildElement("WarpMarker");
                m->setAttribute("Id", id++);
                m->setAttribute("SecTime", juce::String(s, 17));
                m->setAttribute("BeatTime", juce::String(s * kTempo / 60.0, 17));
            }
        }
        setValue(warp, "IsWarped", "false");
        setValue(warp, "WarpMode", "0");
        if (auto* onsets = childPath(warp, "Onsets/UserOnsets")) onsets->deleteAllChildElements();
        setValue(childPath(warp, "Onsets"), "HasUserOnsets", "false");
    }
    for (const char* list : {"InitialSlicePointsFromOnsets", "SlicePoints", "ManualSlicePoints", "BeatSlicePoints", "RegionSlicePoints"})
        if (auto* l = p->getChildByName(list)) l->deleteAllChildElements();
    return p;
}

std::unique_ptr<XmlElement> findTemplatePart(const juce::File& library)
{
    auto presets = library.getChildFile("Devices/Instruments/Simpler").findChildFiles(juce::File::findFiles, true, "*.adv");
    presets.sort();
    for (const auto& preset : presets)
    {
        std::string error;
        auto dom = io::readAlsDom(preset, error);
        if (dom == nullptr) continue;
        if (auto* part = childPath(dom->getFirstChildElement(), "Player/MultiSampleMap/SampleParts/MultiSamplePart"))
            return std::make_unique<XmlElement>(*part);
    }
    return nullptr;
}

// Factory presets reference installed library samples by library-relative path; point them at the local copy.
void localizeLibrarySamples(XmlElement& e, const juce::File& library)
{
    if (e.getTagName() == "FileRef")
        if (auto* rel = e.getChildByName("RelativePath"); rel != nullptr)
            if (auto* type = e.getChildByName("RelativePathType"); type != nullptr && type->getIntAttribute("Value") == 5)
            {
                const auto local = library.getChildFile(rel->getStringAttribute("Value"));
                if (local.existsAsFile()) setValue(&e, "Path", local.getFullPathName());
            }
    for (auto* c : e.getChildIterator()) localizeLibrarySamples(*c, library);
}

// ------------------------------------------------------------------------------------------------ make

int make(const juce::File& requestedDir, const juce::File& suite, const juce::File& library)
{
    requestedDir.createDirectory();
    const juce::File dir(std::filesystem::canonical(requestedDir.getFullPathName().toStdString()).string()); // /tmp -> /private/tmp: absolute sample paths as the reference resolves them
    std::vector<Case> cases;
    std::string error;
    if (! parseSuite(suite, library, cases, error)) { std::fprintf(stderr, "suite: %s\n", error.c_str()); return 1; }
    if (cases.empty()) { std::fprintf(stderr, "suite has no cases\n"); return 1; }
    dir.createDirectory();
    const auto samples = dir.getChildFile("Samples");
    samples.createDirectory();
    std::map<std::string, std::pair<juce::File, int>> wavs;
    for (const auto& c : cases)
        if (! wavs.count(c.sample))
        {
            const auto audio = makeSample(c.sample);
            const auto wav = samples.getChildFile(c.sample + ".wav");
            if (! writeWav(wav, audio)) { std::fprintf(stderr, "cannot write %s\n", wav.getFullPathName().toRawUTF8()); return 1; }
            wavs[c.sample] = {wav, audio.getNumSamples()};
        }
    const auto templatePart = findTemplatePart(library);
    if (templatePart == nullptr) { std::fprintf(stderr, "no MultiSamplePart template found under %s\n", library.getFullPathName().toRawUTF8()); return 1; }

    io::NewSetOptions options;
    options.midiTracks = (int) cases.size();
    options.audioTracks = 0;
    options.returnTracks = 0;
    options.scenes = 1;
    options.tempo = kTempo;
    auto root = io::createNewSetDom(options);
    auto* live = root->getChildByName("LiveSet");
    int next = std::max(live->getChildByName("NextPointeeId")->getIntAttribute("Value"), maxPointee(*root) + 1);
    // The reference parses device XML with the schema the set header declares, so adopt the newest template schema.
    const auto schemaOf = [](const juce::String& minor) { return minor.fromFirstOccurrenceOf("_", false, false).getIntValue(); };
    for (const auto& c : cases)
        for (const auto& spec : c.chain)
            if (auto dom = io::readAlsDom(spec.templateFile, error); dom != nullptr
                && schemaOf(dom->getStringAttribute("MinorVersion")) > schemaOf(root->getStringAttribute("MinorVersion")))
            {
                root->setAttribute("MinorVersion", dom->getStringAttribute("MinorVersion"));
                root->setAttribute("SchemaChangeCount", dom->getStringAttribute("SchemaChangeCount", root->getStringAttribute("SchemaChangeCount")));
            }
    std::vector<XmlElement*> tracks;
    for (auto* t : live->getChildByName("Tracks")->getChildWithTagNameIterator("MidiTrack")) tracks.push_back(t);
    std::ofstream table(dir.getChildFile("cases.tsv").getFullPathName().toStdString());
    table << "track\tcase\tsample\tnotes\tchain\n";
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const auto& c = cases[i];
        auto* track = tracks[i];
        char trackName[256];
        std::snprintf(trackName, sizeof trackName, "P%02d %s", (int) i + 1, c.name.c_str());
        setValue(track->getChildByName("Name"), "EffectiveName", trackName);
        setValue(track->getChildByName("Name"), "UserName", trackName);
        auto* devices = childPath(track, "DeviceChain/DeviceChain/Devices");
        std::string chainText;
        int deviceId = 0;
        for (const auto& spec : c.chain)
        {
            auto dom = io::readAlsDom(spec.templateFile, error);
            if (dom == nullptr || dom->getFirstChildElement() == nullptr) { std::fprintf(stderr, "%s: %s\n", spec.templateFile.getFullPathName().toRawUTF8(), error.c_str()); return 1; }
            auto device = std::make_unique<XmlElement>(*dom->getFirstChildElement());
            device->setAttribute("Id", deviceId++);
            const auto tag = device->getTagName();
            if (tag == "OriginalSimpler" || tag == "MultiSampler")
            {
                auto* parts = childPath(device.get(), "Player/MultiSampleMap/SampleParts");
                bool ownSample = parts != nullptr && (parts->getNumChildElements() == 0 || std::any_of(spec.edits.begin(), spec.edits.end(), [](const Edit& e) { return e.part; }));
                if (parts != nullptr && ownSample)
                {
                    parts->deleteAllChildElements();
                    const auto& [wav, frames] = wavs[c.sample];
                    parts->addChildElement(samplePart(*templatePart, wav, frames, 0).release());
                }
            }
            localizeLibrarySamples(*device, library);
            for (const auto& edit : spec.edits)
            {
                auto* target = edit.part ? childPath(device.get(), "Player/MultiSampleMap/SampleParts/MultiSamplePart") : device.get();
                if (target == nullptr || ! applyEdit(target, edit, error)) { std::fprintf(stderr, "case %s: %s\n", c.name.c_str(), error.c_str()); return 1; }
            }
            renumberPointees(*device, next);
            chainText += (chainText.empty() ? "" : " > ") + spec.templateFile.getFileNameWithoutExtension().toStdString();
            devices->addChildElement(device.release());
        }
        table << trackName << '\t' << c.name << '\t' << c.sample << '\t' << c.notes << '\t' << chainText << '\n';
    }
    live->getChildByName("NextPointeeId")->setAttribute("Value", next + 8);
    if (auto* transport = live->getChildByName("Transport"))
    {
        setValue(transport, "LoopOn", "true");
        setValue(transport, "LoopStart", "0");
        setValue(transport, "LoopLength", juce::String(kLengthBeats));
    }

    const auto base = dir.getChildFile(".base.als");
    if (! io::writeAlsDom(*root, base, error)) { std::fprintf(stderr, "write: %s\n", error.c_str()); return 1; }
    auto loaded = io::loadAls(base);
    if (! loaded) { std::fprintf(stderr, "reload: %s\n", loaded.error.c_str()); return 1; }
    auto& song = *loaded.song;
    for (const auto& w : song.warnings) std::printf("  load warning: %s\n", w.c_str());
    song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    model::EditHistory history;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        model::Clip clip;
        if (! model::makeEmptyMidiClip(song, 0.0, kLengthBeats, clip)) { std::fprintf(stderr, "cannot make a MIDI clip\n"); return 1; }
        clip.name = cases[i].notes;
        clip.notes = makeNotes(cases[i].notes);
        history.perform(song, model::insertClip((int) i, clip, "Insert MIDI Clip"));
    }
    const auto out = dir.getChildFile("Parity.als");
    const auto saved = io::saveAls(song, out, false);
    base.deleteFile();
    if (! saved.ok) { std::fprintf(stderr, "save: %s\n", saved.error.c_str()); return 1; }
    std::printf("wrote %s (%zu cases)\n", out.getFullPathName().toRawUTF8(), cases.size());
    return 0;
}

// ------------------------------------------------------------------------------------------------ render

int render(const juce::File& dir)
{
    auto r = io::loadAls(dir.getChildFile("Parity.als"));
    if (! r) { std::fprintf(stderr, "load: %s\n", r.error.c_str()); return 1; }
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);
    for (const auto& l : engine.loadSong(*r.song)) std::printf("  log: %s\n", l.c_str());
    engine.sampleCache().waitForLoads(60 * 1000);
    for (const auto& f : engine.failedSamples()) std::printf("  FAILED sample: %s\n", f.c_str());
    const auto out = dir.getChildFile("sonora");
    out.deleteRecursively();
    engine::ExportOptions options;
    options.target = engine::ExportTarget::AllTracks;
    options.startBeat = 0.0;
    options.endBeat = kLengthBeats;
    options.bitDepth = 32;
    options.dither = false;
    options.destination = out;
    options.baseName = "Parity";
    engine::Exporter exporter(engine, *r.song);
    std::atomic<float> progress{0.0f};
    std::atomic<bool> cancel{false};
    engine.beginOffline();
    const auto result = exporter.run(options, progress, cancel);
    engine.endOffline();
    if (! result.ok) { std::fprintf(stderr, "export failed: %s\n", result.error.c_str()); return 1; }
    std::printf("rendered %zu stems (%.1f s at %.0f Hz) into %s\n", result.files.size(), result.seconds, engine.sampleRate(),
                out.getFullPathName().toRawUTF8());
    return 0;
}

// ------------------------------------------------------------------------------------------------ compare

juce::AudioBuffer<float> readAudio(const juce::File& file, double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    juce::AudioBuffer<float> b;
    if (reader == nullptr) return b;
    rate = reader->sampleRate;
    b.setSize(2, (int) reader->lengthInSamples);
    reader->read(&b, 0, (int) reader->lengthInSamples, 0, true, true);
    if (reader->numChannels == 1) b.copyFrom(1, 0, b, 0, 0, b.getNumSamples());
    return b;
}

double rms(const juce::AudioBuffer<float>& b, int from, int to, int channel = -1)
{
    double sum = 0;
    int count = 0;
    for (int c = 0; c < 2; ++c)
    {
        if (channel >= 0 && c != channel) continue;
        const float* d = b.getReadPointer(c);
        for (int i = std::max(0, from); i < std::min(to, b.getNumSamples()); ++i, ++count) sum += (double) d[i] * d[i];
    }
    return count > 0 ? std::sqrt(sum / count) : 0.0;
}

double db(double linear) { return 20.0 * std::log10(std::max(linear, 1e-10)); }

// Lag (frames) of `b` relative to `a` that maximises their correlation, searched over +-maxLag inside the 2 s window
// where `a` has the most energy (a mostly gated / silent start would otherwise give an arbitrary lag). Lag 0 wins
// unless another lag correlates clearly (10 %) better (periodic material correlates almost as well a few periods away).
int bestLag(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int maxLag)
{
    const int total = std::min(a.getNumSamples(), b.getNumSamples());
    const int n = std::min(total, (int) kSampleRate * 2);
    // Both channels (a hard-panned stem is silent on one side).
    std::vector<float> xs((size_t) total), ys((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        xs[(size_t) i] = a.getSample(0, i) + a.getSample(a.getNumChannels() - 1, i);
        ys[(size_t) i] = b.getSample(0, i) + b.getSample(b.getNumChannels() - 1, i);
    }
    const float* x = xs.data();
    const float* y = ys.data();
    int from = 0;
    double bestEnergy = -1;
    for (int start = 0; start + n <= total; start += (int) kSampleRate / 4)
    {
        double e = 0;
        for (int i = start; i < start + n; ++i) e += (double) x[i] * x[i];
        if (e > bestEnergy) { bestEnergy = e; from = start; }
    }
    const auto correlation = [&](int l) {
        double s = 0;
        for (int i = from + std::max(0, -l); i < from + n && i + l < total; ++i) s += (double) x[i] * y[i + l];
        return s;
    };
    const double atZero = correlation(0);
    double best = atZero;
    int lag = 0;
    for (int l = -maxLag; l <= maxLag; ++l)
        if (const double s = correlation(l); s > best) { best = s; lag = l; }
    return best > atZero + 0.1 * std::abs(atZero) ? lag : 0;
}

std::vector<double> bandLevels(const juce::AudioBuffer<float>& b)
{
    // Average power spectrum (Hann, 4096) folded into third-octave bands from 31.5 Hz to 16 kHz.
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<double> power(size / 2, 0.0);
    std::vector<float> frame(size * 2);
    int frames = 0;
    for (int start = 0; start + size <= b.getNumSamples(); start += size / 2)
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill(frame.begin(), frame.end(), 0.0f);
            for (int i = 0; i < size; ++i)
                frame[(size_t) i] = b.getSample(c, start + i) * (float) (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
            fft.performFrequencyOnlyForwardTransform(frame.data());
            for (int k = 0; k < size / 2; ++k) power[(size_t) k] += (double) frame[(size_t) k] * frame[(size_t) k];
        }
        ++frames;
    }
    std::vector<double> bands;
    for (double centre = 31.5; centre <= 16000.0; centre *= std::pow(2.0, 1.0 / 3.0))
    {
        const double lo = centre / std::pow(2.0, 1.0 / 6.0), hi = centre * std::pow(2.0, 1.0 / 6.0);
        double sum = 0;
        for (int k = (int) std::ceil(lo * size / kSampleRate); k <= (int) (hi * size / kSampleRate) && k < size / 2; ++k) sum += power[(size_t) k];
        bands.push_back(10.0 * std::log10(std::max(sum / std::max(1, frames), 1e-12)));
    }
    return bands;
}

juce::File findStem(const juce::File& folder, const juce::String& trackName)
{
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, true, "*"))
        if ((f.hasFileExtension("wav") || f.hasFileExtension("aif") || f.hasFileExtension("aiff"))
            && (f.getFileNameWithoutExtension().endsWith(trackName) || f.getFileNameWithoutExtension().contains(trackName + " ")
                || f.getFileNameWithoutExtension() == trackName))
            return f;
    return {};
}

int compare(const juce::File& dir, const juce::File& referenceFolder)
{
    std::ifstream in(dir.getChildFile("cases.tsv").getFullPathName().toStdString());
    std::string line;
    std::getline(in, line);
    const auto curves = dir.getChildFile("curves");
    curves.createDirectory();
    std::ofstream report(dir.getChildFile("report.tsv").getFullPathName().toStdString());
    report << "track\tlag\tref_db\tsonora_db\tlevel_diff\tnull_db\tenv_mean\tenv_max\tenv_max_at\tband_max\tband_max_hz\tbalance_diff\tverdict\n";
    std::printf("%-34s %5s %7s %7s %6s %6s %6s %6s %7s %6s %6s  %s\n", "case", "lag", "ref dB", "son dB", "diff", "null", "envAvg", "envMax",
                "at s", "band", "bandHz", "verdict");
    int matched = 0, total = 0;
    while (std::getline(in, line))
    {
        std::istringstream fields(line);
        std::string trackName;
        std::getline(fields, trackName, '\t');
        ++total;
        const auto refFile = findStem(referenceFolder, juce::String(trackName));
        const auto ourFile = findStem(dir.getChildFile("sonora"), juce::String(trackName));
        if (! refFile.existsAsFile() || ! ourFile.existsAsFile())
        {
            std::printf("%-34s missing %s stem\n", trackName.c_str(), refFile.existsAsFile() ? "Sonora" : "reference");
            continue;
        }
        double refRate = 0, ourRate = 0;
        auto ref = readAudio(refFile, refRate);
        auto ours = readAudio(ourFile, ourRate);
        if (std::abs(refRate - ourRate) > 0.5) { std::printf("%-34s sample rates differ (%g vs %g)\n", trackName.c_str(), refRate, ourRate); continue; }
        const int lag = bestLag(ref, ours, 512);
        const int n = std::min(ref.getNumSamples(), ours.getNumSamples() - std::max(0, lag)) - std::max(0, -lag);
        juce::AudioBuffer<float> a(2, n), b(2, n), diff(2, n);
        for (int c = 0; c < 2; ++c)
        {
            a.copyFrom(c, 0, ref, c, std::max(0, -lag), n);
            b.copyFrom(c, 0, ours, c, std::max(0, lag), n);
            diff.copyFrom(c, 0, a, c, 0, n);
            diff.addFrom(c, 0, b, c, 0, n, -1.0f);
        }
        const double refRms = rms(a, 0, n), ourRms = rms(b, 0, n);
        const double nullDb = db(rms(diff, 0, n)) - db(refRms);
        // 10 ms RMS envelope where either side is above -60 dBFS
        const int hop = (int) (kSampleRate / 100);
        double envSum = 0, envMax = 0, envMaxAt = 0;
        int envCount = 0;
        std::ofstream curve(curves.getChildFile(juce::String(trackName).replaceCharacter(' ', '_') + ".csv").getFullPathName().toStdString());
        curve << "seconds,ref_db,sonora_db,ref_l,ref_r,sonora_l,sonora_r\n";
        for (int start = 0; start + hop <= n; start += hop)
        {
            const double x = db(rms(a, start, start + hop)), y = db(rms(b, start, start + hop));
            curve << (double) start / kSampleRate << ',' << x << ',' << y << ',' << db(rms(a, start, start + hop, 0)) << ',' << db(rms(a, start, start + hop, 1))
                  << ',' << db(rms(b, start, start + hop, 0)) << ',' << db(rms(b, start, start + hop, 1)) << '\n';
            if (std::max(x, y) < -60.0) continue;
            const double d = std::abs(std::max(x, -80.0) - std::max(y, -80.0));
            envSum += d;
            ++envCount;
            if (d > envMax) { envMax = d; envMaxAt = (double) start / kSampleRate; }
        }
        const double envMean = envCount > 0 ? envSum / envCount : 0.0;
        const auto refBands = bandLevels(a), ourBands = bandLevels(b);
        double bandMax = 0, bandMaxHz = 0, centre = 31.5;
        const double refPeakBand = *std::max_element(refBands.begin(), refBands.end());
        for (size_t k = 0; k < refBands.size(); ++k, centre *= std::pow(2.0, 1.0 / 3.0))
        {
            if (std::max(refBands[k], ourBands[k]) < refPeakBand - 50.0) continue; // ignore bands far below the content
            const double d = std::abs(refBands[k] - ourBands[k]);
            if (d > bandMax) { bandMax = d; bandMaxHz = centre; }
        }
        // Channels below -100 dBFS count as silent (a hard-panned side is -121 dB in the reference, -165 dB here).
        const auto channelDb = [](const juce::AudioBuffer<float>& x, int count, int channel) { return std::max(-100.0, db(rms(x, 0, count, channel))); };
        const double balance = (channelDb(a, n, 0) - channelDb(a, n, 1)) - (channelDb(b, n, 0) - channelDb(b, n, 1));
        const char* verdict = (nullDb < -30 || (envMean < 0.5 && bandMax < 1.0 && std::abs(balance) < 0.5)) ? "match"
                            : (envMean < 1.5 && bandMax < 3.0 && std::abs(balance) < 1.5) ? "close" : "DIFF";
        if (verdict[0] == 'm') ++matched;
        std::printf("%-34s %5d %7.2f %7.2f %6.2f %6.1f %6.2f %6.2f %7.2f %6.2f %6.0f  %s\n", trackName.c_str(), lag, db(refRms), db(ourRms),
                    db(ourRms) - db(refRms), nullDb, envMean, envMax, envMaxAt, bandMax, bandMaxHz, verdict);
        report << trackName << '\t' << lag << '\t' << db(refRms) << '\t' << db(ourRms) << '\t' << db(ourRms) - db(refRms) << '\t' << nullDb << '\t'
               << envMean << '\t' << envMax << '\t' << envMaxAt << '\t' << bandMax << '\t' << bandMaxHz << '\t' << balance << '\t' << verdict << '\n';
    }
    std::printf("%d / %d cases match\n", matched, total);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto arg = [&](int i) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[i])); };
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "make" && argc >= 4)
    {
        juce::File library(kDefaultLibrary);
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--library") library = arg(i + 1);
        return make(arg(2), arg(3), library);
    }
    if (command == "render" && argc >= 3) return render(arg(2));
    if (command == "compare" && argc >= 4) return compare(arg(2), arg(3));
    std::fprintf(stderr, "usage: parity_probe make <dir> <suite.txt> [--library <folder>]\n"
                         "       parity_probe render <dir>\n"
                         "       parity_probe compare <dir> <reference stems folder>\n");
    return 2;
}
