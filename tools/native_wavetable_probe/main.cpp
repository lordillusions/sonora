// DEV-18d: deterministic offline native import/render for comparison with plugin_probe.
#include "devices/DeviceFactory.h"
#include "core/Edit.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/WavetableContent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "plugins/SerumPatch.h"
#include "plugins/Vst3PresetBuilder.h"

#include <juce_events/juce_events.h>
#include <bit>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace sonora;

namespace
{
bool read(const juce::File& file, std::vector<uint8_t>& data)
{
    if (! file.existsAsFile() || file.getSize() <= 0 || file.getSize() > static_cast<int64_t>(model::NativeContent::maxBytes)) return false;
    juce::MemoryBlock bytes;
    if (! file.loadFileAsData(bytes) || bytes.getSize() > model::NativeContent::maxBytes) return false;
    const auto* begin = static_cast<const uint8_t*>(bytes.getData());
    data.assign(begin, begin + bytes.getSize()); return true;
}
juce::File path(const char* value) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(value)); }
bool wav(const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    if (! file.getParentDirectory().createDirectory()) return false;
    std::ofstream out(file.getFullPathName().toStdString(), std::ios::binary);
    const auto u16 = [&](uint16_t v) { for (unsigned s = 0; s < 16; s += 8) out.put(static_cast<char>((v >> s) & 255)); };
    const auto u32 = [&](uint32_t v) { for (unsigned s = 0; s < 32; s += 8) out.put(static_cast<char>((v >> s) & 255)); };
    const auto bytes = static_cast<uint32_t>(audio.getNumSamples()) * 8;
    out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8); u32(16);
    u16(3); u16(2); u32(48000); u32(48000 * 8); u16(8); u16(32); out.write("data", 4); u32(bytes);
    for (int i = 0; i < audio.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c) u32(std::bit_cast<uint32_t>(audio.getSample(c, i)));
    return out.good();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: native_wavetable_probe <SerumPreset|vstpreset|component.bin|-> [--controller f] [--set native-id=plain] [--note N] [--velocity N] [--voice-rate-factor 1|2] [--wavetable-profile dense12|octave16] [--render-samples 48000..480000] [--note-off sample] [--midi-event sample:hex ...] [--out f.wav] [--json report.json] [--save-set f.als]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI init;
    juce::File out, report, controllerFile, setFile;
    std::vector<std::pair<std::string, float>> edits;
    int note = 60, velocity = 100;
    int voiceRateOverride = 0, tableProfileOverride = -1;
    int total = 96000, off = 24000;
    bool durationSet = false, noteOffSet = false;
    std::vector<std::pair<int, juce::MidiMessage>> events; // --midi-event sample:hexbytes replaces the single --note
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if ((a == "--out" || a == "--json" || a == "--controller" || a == "--save-set") && i + 1 < argc)
        {
            const auto file = path(argv[++i]);
            if (a == "--out") out = file;
            else if (a == "--json") report = file;
            else if (a == "--controller") controllerFile = file;
            else setFile = file;
        }
        else if ((a == "--note" || a == "--velocity") && i + 1 < argc)
        {
            char* end = nullptr;
            const long value = std::strtol(argv[++i], &end, 10);
            if (*end != '\0' || value < (a == "--note" ? 0 : 1) || value > 127) return 2;
            if (a == "--note") note = static_cast<int>(value); else velocity = static_cast<int>(value);
        }
        else if ((a == "--render-samples" || a == "--note-off") && i + 1 < argc)
        {
            const bool duration = a == "--render-samples";
            if (duration ? durationSet : noteOffSet) return 2;
            const std::string value = argv[++i];
            if (value.empty() || ! std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) return 2;
            char* end = nullptr; const long parsed = std::strtol(value.c_str(), &end, 10);
            if (*end != '\0' || parsed < (duration ? 48000 : 18) || parsed > 480000) return 2;
            if (duration) { total = static_cast<int>(parsed); durationSet = true; }
            else { off = static_cast<int>(parsed); noteOffSet = true; }
        }
        else if (a == "--voice-rate-factor" && i + 1 < argc)
        {
            if (voiceRateOverride != 0) return 2;
            const std::string value = argv[++i];
            if (value != "1" && value != "2") return 2;
            voiceRateOverride = value == "2" ? 2 : 1;
        }
        else if (a == "--wavetable-profile" && i + 1 < argc)
        {
            if (tableProfileOverride >= 0) return 2;
            const std::string value = argv[++i];
            if (value != "dense12" && value != "octave16") return 2;
            tableProfileOverride = value == "octave16" ? 1 : 0;
        }
        else if (a == "--midi-event" && i + 1 < argc)
        {
            const std::string event = argv[++i]; const auto colon = event.find(':');
            if (colon == std::string::npos || colon == 0) return 2;
            char* end = nullptr; const long at = std::strtol(event.c_str(), &end, 10);
            const std::string hex = event.substr(colon + 1);
            if (end != event.c_str() + colon || at < 0 || at > 480000 || hex.empty() || hex.size() > 6 || (hex.size() & 1) != 0
                || ! std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; })) return 2;
            std::array<uint8_t, 3> data{};
            for (size_t b = 0; b < hex.size() / 2; ++b) data[b] = static_cast<uint8_t>(std::stoi(hex.substr(b * 2, 2), nullptr, 16));
            events.emplace_back(static_cast<int>(at), juce::MidiMessage(data.data(), static_cast<int>(hex.size() / 2)));
        }
        else if (a == "--set" && i + 1 < argc)
        {
            const std::string edit = argv[++i]; const auto equals = edit.find('=');
            if (equals == std::string::npos) return 2;
            char* end = nullptr; const auto value = std::strtof(edit.c_str() + equals + 1, &end);
            if (*end != '\0' || ! std::isfinite(value)) return 2;
            edits.emplace_back(edit.substr(0, equals), value);
        }
        else return 2;
    }
    if (off >= total) return 2;
    model::Device device;
    std::vector<std::string> warnings;
    if (std::strcmp(argv[1], "-") == 0) device = devices::nativeDeviceModel("SonoraWavetable");
    else
    {
        const auto input = path(argv[1]);
        std::vector<uint8_t> bytes, controller;
        if (! read(input, bytes) || (controllerFile != juce::File() && ! read(controllerFile, controller)))
        { std::fprintf(stderr, "Cannot read bounded input\n"); return 1; }
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "VST3", 4) == 0)
        {
            model::PluginInfo preset;
            if (! plugins::readVstPreset(bytes, preset)
                || plugins::vst3ClassIdString(preset.vst3Uid) != "56534558667350736572756D20320000")
            { std::fprintf(stderr, "Not a Serum 2 instrument VST3 preset\n"); return 1; }
            bytes = std::move(preset.state); controller = std::move(preset.controllerState);
        }
        auto imported = plugins::importSerumPatch(bytes, input.getParentDirectory().getFullPathName().toStdString(), controller);
        if (! imported) { std::fprintf(stderr, "%s\n", imported.error.c_str()); return 1; }
        device = std::move(imported.device); warnings = std::move(imported.warnings);
    }
    auto synth = devices::createNative(device, &warnings);
    if (! synth) return 1;
    auto* wavetable = dynamic_cast<devices::WavetableSynth*>(synth.get()); if (! wavetable) return 1;
    if (voiceRateOverride != 0)
    {
        if (! wavetable->setVoiceRateFactor(voiceRateOverride)) return 2;
        std::string error; if (! devices::storeWavetableVoiceRate(device, voiceRateOverride, error))
        { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    }
    if (tableProfileOverride >= 0)
        for (int oscillator = 0; oscillator < devices::WavetableSynth::kOscillators; ++oscillator)
            if (const auto* table = wavetable->customTable(oscillator))
            {
                const auto converted = table->withPlaybackProfile(tableProfileOverride == 0
                    ? devices::WavetableData::PlaybackProfile::Dense12 : devices::WavetableData::PlaybackProfile::Octave16Approx);
                if (! converted || ! wavetable->setCustomTable(oscillator, converted.data)) return 1;
                std::string error; if (! devices::storeWavetableTable(device, oscillator, *converted.data, error))
                { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
            }
    const auto& infos = synth->parameters();
    for (const auto& [id, value] : edits)
    {
        const auto it = std::find_if(infos.begin(), infos.end(), [&](const auto& info) { return info.id == id; });
        if (it == infos.end() || value < it->minValue || value > it->maxValue)
        { std::fprintf(stderr, "Invalid native parameter %s\n", id.c_str()); return 2; }
        synth->setParameter(static_cast<int>(it - infos.begin()), value);
    }
    synth->prepare(48000, 512);
    if (! wavetable->voiceRatePreparationError().empty())
    { std::fprintf(stderr, "%s\n", wavetable->voiceRatePreparationError().c_str()); return 1; }
    constexpr int preroll = 4096, on = 17;
    juce::AudioBuffer<float> block(2, 512), output(2, total);
    juce::MidiBuffer midi;
    midi.ensureSize(256);
    devices::HostInfo host; host.sampleRate = 48000; host.playing = true;
    for (int start = -preroll; start < total; start += 512)
    {
        const int count = std::min(512, total - start);
        block.setSize(2, count, false, false, true); block.clear(); midi.clear();
        if (! events.empty())
        {
            for (const auto& [at, message] : events)
                if (at >= start && at < start + count) midi.addEvent(message, at - start);
        }
        else
        {
            if (on >= start && on < start + count) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(velocity)), on - start);
            if (off >= start && off < start + count) midi.addEvent(juce::MidiMessage::noteOff(1, note), off - start);
        }
        host.beats = static_cast<double>(start) / 24000; synth->setHostInfo(host); synth->process(block, midi);
        if (start >= 0) for (int c = 0; c < 2; ++c) output.copyFrom(c, start, block, c, 0, count);
    }
    bool finite = true; double peak = 0, sum = 0;
    int firstAudio = -1;
    for (int i = 0; i < total; ++i) for (int c = 0; c < 2; ++c)
    {
        const double value = output.getSample(c, i);
        finite = finite && std::isfinite(value); peak = std::max(peak, std::abs(value)); sum += value * value;
        if (firstAudio < 0 && std::abs(value) > 1e-12) firstAudio = i;
    }
    if (out != juce::File() && ! wav(out, output)) return 1;
    devices::captureDeviceState(device, synth.get());
    if (setFile != juce::File())
    {
        auto dom = io::createNewSetDom(); auto song = io::parseAls(*dom, {});
        if (! song || song.song->tracks.empty()) { std::fprintf(stderr, "Cannot create native verification set: %s\n", song.error.c_str()); return 1; }
        model::allocateDeviceIds(*song.song, device);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        song.song->tracks[0].devices = {device}; song.song->tracks[0].name = device.displayName;
        auto patched = io::patchAlsDom(*song.song, *dom); std::string error;
        if (! patched.ok || ! setFile.getParentDirectory().createDirectory() || ! io::writeAlsDom(*dom, setFile, error))
        { std::fprintf(stderr, "Cannot save native verification set: %s %s\n", patched.error.c_str(), error.c_str()); return 1; }
    }
    juce::var result(new juce::DynamicObject()); auto* object = result.getDynamicObject();
    object->setProperty("device", juce::String(device.displayName)); object->setProperty("finite", finite);
    object->setProperty("peak", peak); object->setProperty("rms", std::sqrt(sum / (2 * total)));
    object->setProperty("firstAudioSample", firstAudio); object->setProperty("sampleRate", 48000);
    object->setProperty("note", note); object->setProperty("velocity", velocity);
    object->setProperty("samples", total); object->setProperty("preroll", preroll);
    object->setProperty("noteOn", on); object->setProperty("noteOff", off);
    object->setProperty("voiceRateFactor", wavetable->voiceRateFactor());
    object->setProperty("wavetableProfileOverride", tableProfileOverride);
    object->setProperty("completeFeatureSupport", false); object->setProperty("audioVerified", false);
    juce::Array<juce::var> notes, parameters;
    for (const auto& warning : warnings) notes.add(juce::String(warning));
    object->setProperty("warnings", notes);
    for (size_t i = 0; i < infos.size(); ++i)
    {
        juce::var p(new juce::DynamicObject());
        p.getDynamicObject()->setProperty("id", juce::String(infos[i].id));
        p.getDynamicObject()->setProperty("value", synth->getParameter(static_cast<int>(i)));
        parameters.add(p);
    }
    object->setProperty("parameters", parameters);
    const auto json = juce::JSON::toString(result, true);
    if (report != juce::File() && (! report.getParentDirectory().createDirectory() || ! report.replaceWithText(json))) return 1;
    std::printf("finite=%s peak=%.9g rms=%.9g firstAudio=%d warnings=%zu\n", finite ? "true" : "false", peak, std::sqrt(sum / (2 * total)), firstAudio, warnings.size());
    return finite ? 0 : 1;
}
