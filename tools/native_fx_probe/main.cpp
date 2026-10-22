// DEV-18d: runs the native Serum-FX adapter (plugins/SerumEffectsImport + devices/native/WavetableEffects) offline on a WAV,
// for comparison with the same signal rendered through the hosted Serum 2 FX plugin (plugin_probe --stimulus).
//
//   native_fx_probe <fx-or-instrument-component.bin> <input.wav> <output.wav> [--block N] [--print-warnings]
//
// The state's three FX racks become the Main / Aux 1 / Aux 2 buses of the native graph; the input goes to Main.
// Input: float or PCM stereo/mono WAV. Output: 32-bit float stereo WAV at the input's rate.

#include "devices/native/WavetableDownsampler.h"
#include "devices/native/WavetableEffects.h"
#include "plugins/SerumEffectsImport.h"
#include "plugins/SerumState.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace sonora;

int main(int argc, char** argv)
{
    if (argc == 2 && juce::String(argv[1]) == "--print-halfband")
    {
        devices::WavetableDownsampler downsampler;
        downsampler.prepare(2);
        for (const float a : downsampler.alpha()) std::printf("%.9g\n", (double) a);
        return 0;
    }
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: native_fx_probe <component.bin> <input.wav> <output.wav> [--block N] [--print-warnings]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI init;
    int block = 512;
    bool printWarnings = false;
    for (int i = 4; i < argc; ++i)
    {
        const juce::String a = argv[i];
        if (a == "--block" && i + 1 < argc) block = std::max(16, std::atoi(argv[++i]));
        else if (a == "--print-warnings") printWarnings = true;
    }
    juce::MemoryBlock bytes;
    if (! juce::File(argv[1]).loadFileAsData(bytes)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    const auto decoded = plugins::decodeSerumState({static_cast<const uint8_t*>(bytes.getData()), bytes.getSize()});
    if (! decoded) { std::fprintf(stderr, "state decode failed: %s\n", decoded.error.c_str()); return 1; }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(argv[2])));
    if (reader == nullptr) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    const double rate = reader->sampleRate;
    const int frames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> audio(2, frames);
    reader->read(&audio, 0, frames, 0, true, true);
    if (reader->numChannels == 1) audio.copyFrom(1, 0, audio, 0, 0, frames);

    auto imported = plugins::parseSerumEffects(decoded.state->root(), std::string{}, rate);
    if (printWarnings)
        for (const auto& w : imported.warnings) std::printf("warning: %s\n", w.c_str());
    devices::WavetableEffects effects;
    std::string error;
    if (! effects.prepare(imported.configuration, rate, error)) { std::fprintf(stderr, "prepare failed: %s\n", error.c_str()); return 1; }

    std::vector<float> auxL(static_cast<size_t>(block)), auxR(static_cast<size_t>(block)), bus2L(static_cast<size_t>(block)), bus2R(static_cast<size_t>(block));
    for (int start = 0; start < frames; start += block)
    {
        const int n = std::min(block, frames - start);
        std::fill(auxL.begin(), auxL.end(), 0.0f); std::fill(auxR.begin(), auxR.end(), 0.0f);
        std::fill(bus2L.begin(), bus2L.end(), 0.0f); std::fill(bus2R.begin(), bus2R.end(), 0.0f);
        std::array<devices::WavetableEffects::StereoBuffer, 3> buses{};
        buses[0] = {audio.getWritePointer(0) + start, audio.getWritePointer(1) + start};
        buses[1] = {auxL.data(), auxR.data()};
        buses[2] = {bus2L.data(), bus2R.data()};
        effects.process(buses, n);
    }
    juce::File out(argv[3]);
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(out.createOutputStream());
    if (stream == nullptr) { std::fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(32));
    if (writer == nullptr) return 1;
    writer->writeFromAudioSampleBuffer(audio, 0, frames);
    std::printf("processed %d frames (latency %d samples)\n", frames, effects.latencySamples());
    return 0;
}
