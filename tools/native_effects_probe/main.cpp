// DEV-18d: controlled native FX render using plugin_probe's deterministic reference stimulus.
#include "devices/native/WavetableEffects.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using E = sonora::devices::WavetableEffects;
namespace
{
constexpr int sampleRate = 48000, blockSize = 512, totalSamples = 96000, prerollSamples = 4096;
bool number(const char* input, double& value)
{
    char* end = nullptr; value = std::strtod(input, &end);
    return end != input && *end == '\0' && std::isfinite(value);
}
juce::File path(const char* input) { return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(input)); }
bool writeAudio(const juce::File& file, const juce::AudioBuffer<float>& audio)
{
    if (! file.getParentDirectory().createDirectory()) return false;
    std::ofstream stream(file.getFullPathName().toStdString(), std::ios::binary);
    const auto u16 = [&](uint16_t value) { for (unsigned shift = 0; shift < 16; shift += 8) stream.put(static_cast<char>((value >> shift) & 255)); };
    const auto u32 = [&](uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) stream.put(static_cast<char>((value >> shift) & 255)); };
    const auto bytes = static_cast<uint32_t>(audio.getNumSamples()) * 8;
    stream.write("RIFF", 4); u32(36 + bytes); stream.write("WAVEfmt ", 8); u32(16);
    u16(3); u16(2); u32(sampleRate); u32(sampleRate * 8); u16(8); u16(32); stream.write("data", 4); u32(bytes);
    for (int frame = 0; frame < audio.getNumSamples(); ++frame)
        for (int channel = 0; channel < 2; ++channel) u32(std::bit_cast<uint32_t>(audio.getSample(channel, frame)));
    return stream.good();
}
}
int main(int argc, char** argv)
{
    int code = 12, bus = 0;
    double amplitude = 0.1, frequency = 220, rightScale = 1, tempo = 120;
    bool enabled = true, align = true;
    std::string stimulus = "sine";
    E::MixLaw mixLaw = E::MixLaw::Shaped;
    juce::File out, report, impulse;
    std::vector<std::pair<std::string, float>> edits;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--bypass") enabled = false;
        else if (argument == "--unaligned") align = false;
        else if (argument == "--mix-law" && i + 1 < argc)
        {
            const std::string law = argv[++i];
            if (law != "shaped" && law != "equal-power") return 2;
            mixLaw = law == "shaped" ? E::MixLaw::Shaped : E::MixLaw::EqualPower;
        }
        else if ((argument == "--out" || argument == "--json" || argument == "--ir") && i + 1 < argc)
        {
            const auto file = path(argv[++i]);
            if (argument == "--out") out = file; else if (argument == "--json") report = file; else impulse = file;
        }
        else if (argument == "--stimulus" && i + 1 < argc)
        {
            stimulus = argv[++i];
            if (stimulus != "sine" && stimulus != "impulse" && stimulus != "step" && stimulus != "noise") return 2;
        }
        else if (argument == "--set" && i + 1 < argc)
        {
            const std::string edit = argv[++i]; const auto equals = edit.find('='); double value = 0;
            if (equals == std::string::npos || equals == 0 || ! number(edit.c_str() + equals + 1, value) || std::abs(value) > 1e9) return 2;
            edits.emplace_back(edit.substr(0, equals), static_cast<float>(value));
        }
        else if ((argument == "--type" || argument == "--bus" || argument == "--amplitude" || argument == "--frequency"
                  || argument == "--right-scale" || argument == "--tempo") && i + 1 < argc)
        {
            double value = 0; if (! number(argv[++i], value)) return 2;
            if (argument == "--type") { if (value < 0 || value > 14 || std::floor(value) < value) return 2; code = static_cast<int>(value); }
            else if (argument == "--bus") { if (value < 0 || value > 2 || std::floor(value) < value) return 2; bus = static_cast<int>(value); }
            else if (argument == "--amplitude") { if (value < 0 || value > 1) return 2; amplitude = value; }
            else if (argument == "--frequency") { if (value <= 0 || value >= sampleRate * 0.5) return 2; frequency = value; }
            else if (argument == "--right-scale") { if (value < -1 || value > 1) return 2; rightScale = value; }
            else { if (value < 1 || value > 1000) return 2; tempo = value; }
        }
        else
        {
            std::fprintf(stderr, "usage: native_effects_probe [--type 0..14] [--bus 0..2] [--set descriptor=plain] [--ir file] [--bypass] [--unaligned] [--mix-law shaped|equal-power] [--stimulus sine|impulse|step|noise] [--amplitude 0..1] [--frequency Hz] [--right-scale -1..1] [--tempo BPM] [--out file.wav] [--json report.json]\n");
            return 2;
        }
    }
    const auto type = E::typeFromCode(code); if (! type) return 2;
    E::Module module; module.id = 1; module.type = *type; module.enabled = enabled;
    const auto descriptors = E::parameterDescriptors(*type);
    for (const auto& descriptor : descriptors) module.values.push_back(descriptor.initial);
    for (const auto& [id, value] : edits)
    {
        const int at = E::parameterIndex(*type, id);
        if (at < 0 || value < descriptors[static_cast<size_t>(at)].minimum || value > descriptors[static_cast<size_t>(at)].maximum)
        { std::fprintf(stderr, "Invalid effect parameter %s\n", id.c_str()); return 2; }
        module.values[static_cast<size_t>(at)] = value;
    }
    juce::ScopedJuceInitialiser_GUI init;
    if (impulse != juce::File())
    {
        if (*type != E::Type::Convolution) return 2;
        const auto loaded = sonora::devices::ConvolutionIR::fromFile(impulse);
        if (! loaded) { std::fprintf(stderr, "%s\n", loaded.error.c_str()); return 1; }
        module.impulse = loaded.data;
    }
    E::Configuration configuration; configuration.alignLatency = align;
    configuration.mixLaw = mixLaw;
    configuration.buses[static_cast<size_t>(bus)] = {module};
    E effects; std::string error;
    if (! effects.prepare(configuration, sampleRate, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    effects.setTempo(tempo);
    std::array<std::array<float, blockSize>, 6> buffers{};
    const std::array<E::StereoBuffer, 3> buses{{{buffers[0].data(), buffers[1].data()}, {buffers[2].data(), buffers[3].data()}, {buffers[4].data(), buffers[5].data()}}};
    juce::AudioBuffer<float> audio(2, totalSamples);
    uint32_t noise = 0x5eed1234u;
    bool finite = true; double peak = 0, energy = 0; int firstAudio = -1;
    for (int start = -prerollSamples; start < totalSamples; start += blockSize)
    {
        const int count = std::min(blockSize, totalSamples - start);
        for (auto& channel : buffers) channel.fill(0);
        if (start >= 0)
            for (int sample = 0; sample < count; ++sample)
            {
                const int frame = start + sample; double value = 0;
                if (stimulus == "impulse") value = frame == 17 ? amplitude : 0;
                else if (stimulus == "step") value = frame >= 17 ? amplitude : 0;
                else if (stimulus == "noise")
                { noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; value = amplitude * (2.0 * noise / 4294967295.0 - 1); }
                else value = amplitude * std::sin(6.2831853071795864769 * frequency * frame / sampleRate);
                buffers[static_cast<size_t>(bus * 2)][static_cast<size_t>(sample)] = static_cast<float>(value);
                buffers[static_cast<size_t>(bus * 2 + 1)][static_cast<size_t>(sample)] = static_cast<float>(value * rightScale);
            }
        effects.process(buses, count);
        if (start < 0) continue;
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < count; ++sample)
            {
                const float value = buffers[static_cast<size_t>(channel)][static_cast<size_t>(sample)];
                audio.setSample(channel, start + sample, value); finite = finite && std::isfinite(value);
                peak = std::max(peak, std::abs(static_cast<double>(value))); energy += static_cast<double>(value) * value;
                if (std::abs(value) > 1e-12f && (firstAudio < 0 || start + sample < firstAudio)) firstAudio = start + sample;
            }
    }
    juce::var result(new juce::DynamicObject()); auto* object = result.getDynamicObject();
    object->setProperty("nativeType", code); object->setProperty("nativeName", juce::String(E::typeName(*type).data()));
    object->setProperty("bus", bus); object->setProperty("enabled", enabled); object->setProperty("alignLatency", align);
    object->setProperty("mixLaw", mixLaw == E::MixLaw::Shaped ? "shaped" : "equal-power");
    object->setProperty("sampleRate", sampleRate); object->setProperty("blockSize", blockSize);
    object->setProperty("samples", totalSamples); object->setProperty("prerollSamples", prerollSamples);
    object->setProperty("stimulus", juce::String(stimulus)); object->setProperty("amplitude", amplitude);
    object->setProperty("frequency", frequency); object->setProperty("rightScale", rightScale); object->setProperty("tempo", tempo);
    object->setProperty("latencySamples", effects.latencySamples()); object->setProperty("preparedBytes", static_cast<juce::int64>(effects.preparedBytes()));
    object->setProperty("finite", finite); object->setProperty("peak", peak); object->setProperty("rms", std::sqrt(energy / (totalSamples * 2)));
    object->setProperty("firstAudioSample", firstAudio);
    juce::var controls(new juce::DynamicObject());
    for (size_t i = 0; i < descriptors.size(); ++i)
        controls.getDynamicObject()->setProperty(juce::Identifier(juce::String(descriptors[i].id.data())), effects.getParameter(1, static_cast<int>(i)));
    object->setProperty("parameters", controls);
    const auto json = juce::JSON::toString(result, true); std::printf("%s\n", json.toRawUTF8());
    if (out != juce::File() && ! writeAudio(out, audio)) return 1;
    if (report != juce::File() && (! report.getParentDirectory().createDirectory() || ! report.replaceWithText(json))) return 1;
    return finite ? 0 : 1;
}
