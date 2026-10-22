#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using namespace sonora;
using W = devices::WavetableSynth;
constexpr double pi = 3.14159265358979323846;
std::shared_ptr<const devices::OscillatorSample> ownNoise(bool constant = false)
{
    std::vector<float> pcm(constant ? 62 : 4096);
    for (size_t frame = 0; frame < pcm.size() / 2; ++frame)
    {
        pcm[frame * 2] = constant ? .25f : static_cast<float>(.5 * std::sin(2 * pi * 16 * static_cast<double>(frame) / 2048));
        pcm[frame * 2 + 1] = -.5f * pcm[frame * 2];
    }
    const auto loaded = devices::OscillatorSample::fromSamples(pcm, 2, 48000, "Own stereo Noise");
    REQUIRE(loaded); return loaded.data;
}
void prepareNoise(W& synth, const std::shared_ptr<const devices::OscillatorSample>& sample, int factor = 1)
{
    synth.setNoiseSample(sample); synth.setVoiceRateFactor(factor); synth.prepare(48000, 512);
    synth.setParameter(W::Volume, 0); synth.setParameter(W::SubOn, 0); synth.setParameter(W::NoiseOn, 1);
    synth.setParameter(W::NoiseLevel, 1); synth.setParameter(W::NoisePitch, 0); synth.setParameter(W::NoiseRandom, 0);
    for (int oscillator = 0; oscillator < 3; ++oscillator) synth.setParameter(W::oscParam(oscillator, W::On), 0);
    for (int filter = 0; filter < 2; ++filter) synth.setParameter(W::filterParam(filter, W::FilterOn), 0);
    for (const auto parameter : {W::Delay, W::Attack, W::Hold, W::Decay, W::Release}) synth.setParameter(W::envParam(0, parameter), 0);
    synth.setParameter(W::envParam(0, W::Sustain), 1);
}
juce::AudioBuffer<float> renderNoise(W& synth, int note = 72, int blockSize = 512, int count = 8192)
{
    juce::AudioBuffer<float> audio(2, count); audio.clear(); juce::MidiBuffer midi; midi.ensureSize(128);
    for (int start = 0; start < count; start += blockSize)
    {
        const int frames = std::min(blockSize, count - start); midi.clear();
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), 17);
        juce::AudioBuffer<float> block(audio.getArrayOfWritePointers(), 2, start, frames); synth.process(block, midi);
    }
    return audio;
}
void sameNoise(const juce::AudioBuffer<float>& actual, const juce::AudioBuffer<float>& expected, double gain = 1)
{
    REQUIRE(actual.getNumSamples() == expected.getNumSamples()); double error = 0;
    for (int channel = 0; channel < 2; ++channel) for (int sample = 0; sample < actual.getNumSamples(); ++sample)
        error = std::max(error, std::abs(actual.getSample(channel, sample) - expected.getSample(channel, sample) * gain));
    CHECK_NEAR(error, 0, 3e-6);
}
juce::var noiseContent(const model::Device& device)
{
    REQUIRE(device.nativeContent); juce::var value;
    const auto& bytes = device.nativeContent->data;
    REQUIRE(juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())), value).wasOk());
    return value;
}
void replaceNoiseContent(model::Device& device, const juce::var& value)
{
    const auto json = juce::JSON::toString(value, true); auto content = std::make_shared<model::NativeContent>(*device.nativeContent);
    const auto* begin = reinterpret_cast<const uint8_t*>(json.toRawUTF8()); content->data.assign(begin, begin + json.getNumBytesAsUTF8());
    device.nativeContent = std::move(content);
}
void writeNoiseFile(const juce::File& file, float amplitude)
{
    REQUIRE(file.getParentDirectory().createDirectory().wasOk()); REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream()); REQUIRE(stream);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    REQUIRE(writer); juce::AudioBuffer<float> audio(2, 4096);
    for (int frame = 0; frame < 4096; ++frame)
    {
        const float value = amplitude * static_cast<float>(std::sin(2 * pi * 16 * frame / 2048));
        audio.setSample(0, frame, value); audio.setSample(1, frame, -.5f * value);
    }
    REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, 4096));
}
}

TEST_CASE("wavetable Noise: PCM pitch fine phase keytrack and stereo follow host seconds")
{
    const auto sample = ownNoise();
    for (const float pitch : {-12.0f, 0.0f, 12.0f}) for (const float fine : {-96.0f, 0.0f, 96.0f})
    {
        W synth; prepareNoise(synth, sample); synth.setParameter(W::NoisePitch, pitch); synth.setParameter(W::NoiseFine, fine);
        synth.setParameter(W::NoisePhase, .25f); const auto audio = renderNoise(synth, 60, 173);
        const double frequency = 375 * std::exp2(pitch / 12 + fine / 1200); double residual = 0;
        for (int frame = 17; frame < audio.getNumSamples(); ++frame)
        {
            const double expected = .5 * std::sin(2 * pi * (frequency * (frame - 17) / 48000 + 4));
            residual = std::max(residual, std::abs(audio.getSample(0, frame) - expected));
            CHECK_NEAR(audio.getSample(1, frame), -.5 * audio.getSample(0, frame), 2e-7);
        }
        CHECK_NEAR(residual, 0, 2e-4);
    }
    W untracked48, untracked84; prepareNoise(untracked48, sample); prepareNoise(untracked84, sample);
    sameNoise(renderNoise(untracked48, 48), renderNoise(untracked84, 84));
    W tracked48, lowerPitch; prepareNoise(tracked48, sample); prepareNoise(lowerPitch, sample);
    tracked48.setParameter(W::NoiseKeytrack, 1); lowerPitch.setParameter(W::NoisePitch, -24);
    sameNoise(renderNoise(tracked48, 48), renderNoise(lowerPitch, 72));
}

TEST_CASE("wavetable Noise: source stays independent across rate2 uneven blocks and reset")
{
    const auto sample = ownNoise(); W whole, uneven; prepareNoise(whole, sample, 2); prepareNoise(uneven, sample, 2);
    whole.setParameter(W::NoiseRandom, .7f); uneven.setParameter(W::NoiseRandom, .7f);
    const auto reference = renderNoise(whole, 72, 8192), result = renderNoise(uneven, 72, 137);
    sameNoise(result, reference); uneven.reset(); sameNoise(renderNoise(uneven, 72, 317), reference);
    CHECK(uneven.noiseSample() == sample.get()); CHECK(uneven.noiseSampleName() == sample->name());
    CHECK_NEAR(uneven.noiseWaveformAt(.37f, 1), -.5 * uneven.noiseWaveformAt(.37f, 0), 1e-7);
    CHECK_NEAR(uneven.noiseWaveformAt(std::numeric_limits<float>::quiet_NaN()), 0, 0);
}

TEST_CASE("wavetable Noise: normalized imported level modulation precedes the square and own level stays linear")
{
    const auto sample = ownNoise(true); W imported, own; prepareNoise(imported, sample); prepareNoise(own, sample);
    for (auto* synth : {&imported, &own})
    {
        synth->setParameter(W::NoiseLevel, .0625f); synth->setParameter(W::macroParam(0), .5f);
        synth->setParameter(W::modParam(0, W::ModSource), W::Macro1);
        synth->setParameter(W::modParam(0, W::ModDestination), W::NoiseAmplitude);
        synth->setParameter(W::modParam(0, W::ModAmount), .5f);
    }
    REQUIRE(imported.setImportedModulationDomain(W::NoiseAmplitude, W::ImportedModulationDomain::SquaredLevel));
    const auto squared = renderNoise(imported), linear = renderNoise(own);
    CHECK_NEAR(squared.getSample(0, 1000), .25 * .25, 1e-7);
    CHECK_NEAR(linear.getSample(0, 1000), .25 * .3125, 1e-7);
    W missing; prepareNoise(missing, nullptr); missing.setImportedNoiseProfile(true);
    const auto silence = renderNoise(missing); CHECK_NEAR(silence.getMagnitude(0, silence.getNumSamples()), 0, 0);
}

TEST_CASE("wavetable Noise: PCM follows main direct filters None and independent cubic sends")
{
    const auto sample = ownNoise(); W baseline; prepareNoise(baseline, sample); const auto reference = renderNoise(baseline);
    for (const int destination : {W::RouteToFilters, W::RouteToMain, W::RouteToDirect, W::RouteToNone})
    {
        W synth; prepareNoise(synth, sample); synth.setParameter(W::SourceRoutingOn, 1);
        synth.setParameter(W::sourceRoutingParam(3, W::SourceDestination), static_cast<float>(destination));
        if (destination == W::RouteToDirect) synth.setParameter(W::DirectGain, .5f);
        if (destination == W::RouteToNone) synth.setParameter(W::sourceRoutingParam(3, W::SourceBus1), .5f);
        sameNoise(renderNoise(synth), reference, destination == W::RouteToDirect ? .5 : destination == W::RouteToNone ? .125 : 1);
    }
}

TEST_CASE("wavetable Noise content: embedded PCM preserves oscillator media controller and ALS save reload")
{
    auto device = devices::nativeDeviceModel("SonoraWavetable"); const auto sample = ownNoise(); std::string error;
    const auto media = devices::OscillatorMedia::fromSamples(sample->originalSamples(), sample->channels(), sample->sampleRate(), "Own oscillator0");
    REQUIRE(media); REQUIRE(devices::storeOscillatorMedia(device, 0, *media.data, error));
    auto content = std::make_shared<model::NativeContent>(*device.nativeContent); content->controllerData = {0, 127, 255}; device.nativeContent = content;
    auto value = noiseContent(device); value.getDynamicObject()->setProperty("unknownSentinel", "retain"); replaceNoiseContent(device, value);
    const auto originalMedia = juce::JSON::toString(noiseContent(device).getProperty("media", {}), true);
    const auto originalParameters = device.parameters;
    REQUIRE(devices::storeWavetableNoise(device, sample, error));
    CHECK(juce::JSON::toString(noiseContent(device).getProperty("media", {}), true) == originalMedia);
    CHECK(noiseContent(device).getProperty("unknownSentinel", {}).toString() == "retain");
    CHECK(device.nativeContent->controllerData == content->controllerData); CHECK(device.parameters.size() == originalParameters.size());
    for (size_t index = 0; index < device.parameters.size(); ++index)
    { CHECK(device.parameters[index].path == originalParameters[index].path); CHECK_NEAR(device.parameters[index].value, originalParameters[index].value, 0); }
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests/noise_content"); directory.createDirectory();
    io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
    const auto source = directory.getChildFile("Source.als"), file = directory.getChildFile("Own Noise.als");
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto initial = io::loadAls(source); REQUIRE(initial); auto& song = *initial.song;
    model::allocateDeviceIds(song, device); device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    model::EditHistory history; history.perform(song, model::insertDevice(0, device));
    REQUIRE(io::saveAls(song, file, io::AlsSaveOptions{}).ok);
    const auto loaded = io::loadAls(file); REQUIRE(loaded);
    std::vector<std::string> warnings; auto restored = devices::createNative(loaded.song->tracks[0].devices[0], &warnings);
    auto* synth = dynamic_cast<W*>(restored.get()); REQUIRE(synth); CHECK(warnings.empty()); REQUIRE(synth->noiseSample());
    CHECK(synth->noiseSample()->originalSamples() == sample->originalSamples()); REQUIRE(synth->oscillatorMedia(0));
    CHECK(synth->oscillatorMedia(0)->name() == "Own oscillator0");
    REQUIRE(devices::storeWavetableNoise(device, nullptr, error));
    auto cleared = devices::createNative(device); auto* generated = dynamic_cast<W*>(cleared.get()); REQUIRE(generated); CHECK(generated->noiseSample() == nullptr);
}

TEST_CASE("wavetable Noise content: malformed assets retain the prepared sample and independent oscillator bank")
{
    auto device = devices::nativeDeviceModel("SonoraWavetable"); const auto sample = ownNoise(); std::string error;
    REQUIRE(devices::storeWavetableNoise(device, sample, error));
    for (const bool duplicate : {false, true})
    {
        auto invalid = device; auto value = noiseContent(invalid); auto noise = value.getProperty("noise", {});
        if (duplicate)
        {
            auto samples = noise.getProperty("samples", {}); REQUIRE(samples.getArray()); samples.getArray()->add(samples.getArray()->getFirst());
            noise.getDynamicObject()->setProperty("samples", samples);
        }
        else noise.getDynamicObject()->setProperty("samples", "broken");
        value.getDynamicObject()->setProperty("noise", noise); replaceNoiseContent(invalid, value);
        W synth; synth.setNoiseSample(sample); std::vector<std::string> warnings;
        devices::prepareWavetableContent(synth, invalid, warnings); CHECK(! warnings.empty()); CHECK(synth.noiseSample() == sample.get());
        W missing; warnings.clear(); devices::prepareWavetableContent(missing, invalid, warnings);
        CHECK(missing.expectsNoiseSample()); CHECK(missing.noiseSampleName() == "Missing Noise Sample");
        prepareNoise(missing, nullptr); missing.setNoiseSampleExpected(true);
        const auto silent = renderNoise(missing); CHECK_NEAR(silent.getMagnitude(0, silent.getNumSamples()), 0, 0);
    }
}

TEST_CASE("wavetable Noise content: validated external PCM references reject changed and missing files without losing source state")
{
    const juce::File file("/tmp/sonora_tests/noise_content/Referenced Noise.wav"); writeNoiseFile(file, .5f);
    const auto loaded = devices::OscillatorSample::fromFile(file); REQUIRE(loaded);
    auto device = devices::nativeDeviceModel("SonoraWavetable"); std::string error;
    REQUIRE(devices::storeWavetableNoise(device, loaded.data, error, true));
    const auto value = noiseContent(device); const auto noise = value.getProperty("noise", {});
    const auto samples = noise.getProperty("samples", {}); REQUIRE(samples.getArray());
    CHECK(samples.getArray()->getFirst().getProperty("encoding", {}).toString() == "audio-file");
    CHECK(samples.getArray()->getFirst().getProperty("pcm", {}).isVoid());
    const auto saved = device.nativeContent;
    std::vector<std::string> warnings; auto restored = devices::createNative(device, &warnings); auto* synth = dynamic_cast<W*>(restored.get());
    REQUIRE(synth); REQUIRE(synth->noiseSample()); CHECK(warnings.empty()); CHECK(synth->noiseSample()->pcmFingerprint() == loaded.data->pcmFingerprint());
    for (const bool remove : {false, true})
    {
        if (remove) REQUIRE(file.deleteFile()); else writeNoiseFile(file, .25f);
        W retained; retained.setNoiseSample(loaded.data); warnings.clear(); devices::prepareWavetableContent(retained, device, warnings);
        CHECK(! warnings.empty()); CHECK(retained.noiseSample() == loaded.data.get());
        warnings.clear(); auto unavailable = devices::createNative(device, &warnings); auto* missing = dynamic_cast<W*>(unavailable.get());
        REQUIRE(missing); CHECK(! warnings.empty()); CHECK(missing->expectsNoiseSample()); CHECK(missing->noiseSample() == nullptr);
        CHECK(missing->noiseSampleName() == "Missing Noise Sample"); CHECK(device.nativeContent == saved);
    }
}
