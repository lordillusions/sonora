#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <algorithm>
#include <bit>
#include <limits>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Media = devices::OscillatorMedia;
using Sample = devices::OscillatorSample;
using Zone = devices::OscillatorZone;
const juce::File directory("/tmp/sonora_tests/sonora_wavetable_media_reference");

void writeAudio(const juce::File& file, int channels, int frames = 4096, bool aiff = false, float amplitude = 0.5f)
{
    REQUIRE(file.getParentDirectory().createDirectory().wasOk()); REQUIRE(file.deleteFile());
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream()); REQUIRE(stream);
    juce::WavAudioFormat wav; juce::AiffAudioFormat aif;
    auto& format = aiff ? static_cast<juce::AudioFormat&>(aif) : static_cast<juce::AudioFormat&>(wav);
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(channels).withBitsPerSample(24));
    REQUIRE(writer);
    juce::AudioBuffer<float> audio(channels, frames);
    for (int channel = 0; channel < channels; ++channel)
        for (int sample = 0; sample < frames; ++sample)
            audio.setSample(channel, sample, amplitude * static_cast<float>(std::sin(sample * 0.13 + channel * 0.3)));
    REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, frames));
}

std::shared_ptr<const Media> ownBank(std::string_view suffix)
{
    const auto wav = directory.getChildFile(juce::String(std::string(suffix)) + ".wav");
    const auto aiff = directory.getChildFile(juce::String(std::string(suffix)) + ".aiff");
    writeAudio(wav, 1); writeAudio(aiff, 2, 6144, true);
    const auto a = Sample::fromFile(wav, true, 8), b = Sample::fromFile(aiff); REQUIRE(a); REQUIRE(b);
    Zone low; low.sample = a.data;
    low.lowKey = 24; low.highKey = 60; low.lowVelocity = 1; low.highVelocity = 79; low.rootKey = 48;
    low.tuneCents = 12.5f; low.gainDb = -4.25f; low.pan = -0.35f; low.velocityTracking = 0.5f; low.keyTracking = 0.75f;
    low.attackSeconds = 0.002; low.releaseSeconds = 0.07; low.offset = 7; low.end = 4085;
    low.loopStart = 17; low.loopEnd = 4075; low.loopCrossfade = 8; low.loop = devices::MediaLoop::Sustain;
    Zone high; high.sample = b.data;
    high.lowKey = 60; high.highKey = 100; high.lowVelocity = 64; high.highVelocity = 127; high.rootKey = 72;
    high.tuneCents = -11; high.gainDb = -3.125f; high.pan = 0.45f; high.velocityTracking = 0.8f; high.keyTracking = 0.6f;
    high.attackSeconds = 0.01; high.releaseSeconds = 0.2; high.offset = 11; high.end = 6133;
    high.loopStart = 23; high.loopEnd = 6100; high.loopCrossfade = 4; high.loop = devices::MediaLoop::PingPong; high.loopWhileHeld = true;
    auto shared = low; shared.lowKey = 61; shared.highKey = 72; shared.rootKey = 64;
    const auto bank = Media::fromZones({low, high, shared}, "Own external bank"); REQUIRE(bank);
    return bank.data;
}

juce::var value(const model::Device& device)
{
    REQUIRE(device.nativeContent);
    const auto& bytes = device.nativeContent->data;
    juce::var result;
    REQUIRE(juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size())), result).wasOk());
    REQUIRE(result.isObject()); return result;
}

void replaceValue(model::Device& device, const juce::var& root)
{
    auto content = std::make_shared<model::NativeContent>(*device.nativeContent);
    const auto json = juce::JSON::toString(root, true);
    const auto* bytes = reinterpret_cast<const uint8_t*>(json.toRawUTF8());
    content->data.assign(bytes, bytes + json.getNumBytesAsUTF8()); device.nativeContent = std::move(content);
}

model::Device deviceFor(const Media& bank, bool origin = false)
{
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    device.displayName = "Own referenced instrument";
    auto content = std::make_shared<model::NativeContent>();
    content->controllerData = {0, 1, 127, 128, 255}; content->sourceDirectory = directory.getFullPathName().toStdString();
    // Independently authored invalid transport bytes exercise lossless origin retention, not a vendor fixture.
    if (origin) { content->format = "serum2-preset"; content->data = {17, 34, 51, 68, 85}; }
    else { content->format = "sonora-wavetable-v1"; content->data = {'{', '"', 'v', 'e', 'r', 's', 'i', 'o', 'n', '"', ':', '1', '}'}; }
    device.nativeContent = std::move(content);
    std::string error;
    REQUIRE(devices::storeWavetableLfo(device, 3, dsp::CurveGraph::preset(2), error));
    REQUIRE(devices::storeOscillatorMedia(device, 1, bank, error, true));
    CHECK(error.empty()); return device;
}

void sameBank(const Media& actual, const Media& expected)
{
    CHECK(actual.name() == expected.name()); REQUIRE(actual.zones().size() == expected.zones().size());
    CHECK(actual.pcmBytes() == expected.pcmBytes()); CHECK(actual.spectralBytes() == expected.spectralBytes());
    CHECK_NEAR(actual.maximumSpectralHopSeconds(), expected.maximumSpectralHopSeconds(), 0);
    for (size_t index = 0; index < expected.zones().size(); ++index)
    {
        const auto& a = actual.zones()[index]; const auto& b = expected.zones()[index]; REQUIRE(a.sample);
        CHECK(a.sample->sourceFile() == b.sample->sourceFile()); CHECK(a.sample->name() == b.sample->name());
        CHECK(a.sample->pcmFingerprint() == b.sample->pcmFingerprint()); CHECK(a.sample->originalSamples() == b.sample->originalSamples());
        CHECK(a.sample->channels() == b.sample->channels()); CHECK(a.sample->sampleFrames() == b.sample->sampleFrames());
        CHECK_NEAR(a.sample->sampleRate(), b.sample->sampleRate(), 0);
        CHECK(a.sample->spectralFrames().size() == b.sample->spectralFrames().size()); CHECK(a.sample->spectralHop() == b.sample->spectralHop());
        CHECK(a.lowKey == b.lowKey); CHECK(a.highKey == b.highKey); CHECK(a.lowVelocity == b.lowVelocity); CHECK(a.highVelocity == b.highVelocity); CHECK(a.rootKey == b.rootKey);
        CHECK_NEAR(a.tuneCents, b.tuneCents, 0); CHECK_NEAR(a.gainDb, b.gainDb, 0); CHECK_NEAR(a.pan, b.pan, 0);
        CHECK_NEAR(a.velocityTracking, b.velocityTracking, 0); CHECK_NEAR(a.keyTracking, b.keyTracking, 0);
        CHECK_NEAR(a.attackSeconds, b.attackSeconds, 1e-12); CHECK_NEAR(a.releaseSeconds, b.releaseSeconds, 1e-12);
        CHECK(a.offset == b.offset); CHECK(a.end == b.end); CHECK(a.loopStart == b.loopStart); CHECK(a.loopEnd == b.loopEnd);
        CHECK(a.loopCrossfade == b.loopCrossfade); CHECK(a.loop == b.loop); CHECK(a.loopWhileHeld == b.loopWhileHeld);
    }
    CHECK((actual.zones()[0].sample == actual.zones()[2].sample) == (expected.zones()[0].sample == expected.zones()[2].sample));
    CHECK(actual.zones()[0].sample->originalSamples().data() == actual.zones()[2].sample->originalSamples().data());
}

void restored(const model::Device& device, const Media& expected)
{
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    const auto* media = synth.oscillatorMedia(1); REQUIRE(media); sameBank(*media, expected);
    CHECK(synth.hasCustomLfoCurve(3));
}

template <typename Corrupt>
void invalidReference(Corrupt corrupt)
{
    const auto bank = ownBank("Malformed"); auto device = deviceFor(*bank);
    auto root = value(device); auto* media = root.getProperty("media", {}).getArray(); REQUIRE(media); REQUIRE(media->size() == 1);
    auto* samples = media->getReference(0).getProperty("samples", {}).getArray(); REQUIRE(samples); REQUIRE(samples->size() == 2);
    corrupt(samples->getReference(0)); replaceValue(device, root);
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty()); CHECK(synth.oscillatorMedia(1) == nullptr); CHECK(synth.hasCustomLfoCurve(3));
    CHECK(device.nativeContent->controllerData == std::vector<uint8_t>({0, 1, 127, 128, 255}));
}

void writeNonfiniteWave(const juce::File& file)
{
    std::vector<uint8_t> bytes;
    const auto text = [&](const char* str) { for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<uint8_t>(str[index])); };
    const auto number = [&](uint32_t numberValue, int count) { for (int index = 0; index < count; ++index) bytes.push_back(static_cast<uint8_t>(numberValue >> (index * 8))); };
    text("RIFF"); number(40, 4); text("WAVE"); text("fmt "); number(16, 4);
    number(3, 2); number(1, 2); number(48000, 4); number(192000, 4); number(4, 2); number(32, 2);
    text("data"); number(4, 4); number(std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()), 4);
    REQUIRE(file.replaceWithData(bytes.data(), bytes.size()));
}
}

TEST_CASE("wavetable media reference: WAV AIFF fingerprints spectral density zone metadata and shared assets restore exactly")
{
    const auto bank = ownBank("Schema"); auto device = deviceFor(*bank);
    const auto immutable = device.nativeContent; const auto bytes = immutable->data;
    const auto root = value(device); const auto entries = root.getProperty("media", {}).getArray(); REQUIRE(entries);
    const auto samples = entries->getReference(0).getProperty("samples", {}).getArray(); REQUIRE(samples); CHECK(samples->size() == 2);
    for (int index = 0; index < samples->size(); ++index)
    {
        const auto& asset = samples->getReference(index); const auto& source = bank->zones()[static_cast<size_t>(index)].sample;
        CHECK(asset.getProperty("encoding", {}).toString() == "audio-file"); CHECK(asset.getProperty("pcm", {}).isVoid());
        CHECK(asset.getProperty("sourceFile", {}).toString().toStdString() == source->sourceFile());
        CHECK(asset.getProperty("fingerprintAlgorithm", {}).toString() == "fnv1a64-f32le");
        CHECK(! asset.getProperty("pcmFingerprint", {}).toString().isEmpty());
        CHECK(static_cast<int>(asset.getProperty("frames", {})) == source->sampleFrames());
        CHECK(static_cast<bool>(asset.getProperty("spectral", {})) == source->hasSpectralAnalysis());
    }
    restored(device, *bank);
    CHECK(device.nativeContent == immutable); CHECK(immutable->data == bytes);
    std::string error; REQUIRE(devices::storeOscillatorMedia(device, 2, *bank, error, true));
    CHECK(device.nativeContent != immutable); CHECK(immutable->data == bytes);
    CHECK(device.nativeContent->controllerData == immutable->controllerData);
}

TEST_CASE("wavetable media reference: external references preserve opaque origin controller and source directory")
{
    const auto bank = ownBank("Origin"); const auto device = deviceFor(*bank, true);
    const auto root = value(device); const auto origin = root.getProperty("origin", {});
    CHECK(origin.getProperty("format", {}).toString() == "serum2-preset");
    juce::MemoryBlock bytes; REQUIRE(bytes.fromBase64Encoding(origin.getProperty("data", {}).toString()));
    const std::array<uint8_t, 5> expected{17, 34, 51, 68, 85};
    CHECK(bytes == juce::MemoryBlock(expected.data(), expected.size()));
    CHECK(device.nativeContent->controllerData == std::vector<uint8_t>({0, 1, 127, 128, 255}));
    CHECK(device.nativeContent->sourceDirectory == directory.getFullPathName().toStdString());
    restored(device, *bank);
}

TEST_CASE("wavetable media reference: distinct spectral densities share decoded PCM for the same source file")
{
    const auto original = ownBank("SharedAnalysis");
    auto zones = original->zones();
    const auto coarser = Sample::withSpectralAnalysis(zones[0].sample, 4); REQUIRE(coarser);
    zones[2].sample = coarser.data;
    const auto bank = Media::fromZones(zones, original->name()); REQUIRE(bank);
    const auto device = deviceFor(*bank.data);
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(warnings.empty());
    const auto* restoredBank = synth.oscillatorMedia(1); REQUIRE(restoredBank);
    sameBank(*restoredBank, *bank.data);
    CHECK(restoredBank->zones()[0].sample != restoredBank->zones()[2].sample);
    CHECK(restoredBank->zones()[0].sample->spectralFrames().size() == 8);
    CHECK(restoredBank->zones()[2].sample->spectralFrames().size() == 4);
    CHECK(restoredBank->zones()[0].sample->pcmFingerprint() == restoredBank->zones()[2].sample->pcmFingerprint());
    CHECK(restoredBank->pcmBytes() == original->pcmBytes());
}

TEST_CASE("wavetable media reference: ALS save reload and undo redo retain references and immutable source identity")
{
    REQUIRE(directory.createDirectory().wasOk());
    io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
    std::string error; const auto source = directory.getChildFile("Source.als");
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    auto initial = devices::nativeDeviceModel("SonoraWavetable"); model::allocateDeviceIds(song, initial);
    initial.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(initial));
    model::EditHistory history; history.perform(song, model::insertDevice(0, initial)); history.clear();
    const auto bank = ownBank("Lifecycle"); auto edited = deviceFor(*bank, true); edited.id = initial.id; edited.sourceXml = initial.sourceXml;
    history.perform(song, model::replaceDevice(song, 0, 0, edited, "Reference own media"));
    const auto immutable = song.tracks[0].devices[0].nativeContent;
    const auto saved = directory.getChildFile("Referenced.als"); REQUIRE(io::saveAls(song, saved, io::AlsSaveOptions{}).ok);
    auto roundtrip = io::loadAls(saved); REQUIRE(roundtrip); const auto& device = roundtrip.song->tracks[0].devices[0];
    REQUIRE(device.nativeContent); CHECK(device.nativeContent->data == immutable->data);
    CHECK(device.nativeContent->controllerData == immutable->controllerData); CHECK(device.nativeContent->sourceDirectory == immutable->sourceDirectory);
    CHECK(device.displayName == edited.displayName); restored(device, *bank);
    REQUIRE(history.undo(song)); CHECK(! song.tracks[0].devices[0].nativeContent);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].devices[0].nativeContent == immutable);
    const auto redone = directory.getChildFile("ReferencedRedo.als"); REQUIRE(io::saveAls(song, redone, io::AlsSaveOptions{}).ok);
    auto reloaded = io::loadAls(redone); REQUIRE(reloaded); restored(reloaded.song->tracks[0].devices[0], *bank);
}

TEST_CASE("wavetable media reference: missing changed malformed and nonfinite source files warn without discarding independent content")
{
    for (int corruption = 0; corruption < 4; ++corruption)
    {
        const auto bank = ownBank("SourceFailure"); const auto device = deviceFor(*bank);
        const juce::File file(juce::String(bank->zones()[0].sample->sourceFile()));
        if (corruption == 0) REQUIRE(file.deleteFile());
        else if (corruption == 1) writeAudio(file, 1, 4096, false, 0.25f);
        else if (corruption == 2) REQUIRE(file.replaceWithText("Own malformed audio fixture"));
        else writeNonfiniteWave(file);
        W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
        CHECK(! warnings.empty()); CHECK(synth.oscillatorMedia(1) == nullptr); CHECK(synth.hasCustomLfoCurve(3));
        CHECK(device.nativeContent->controllerData == std::vector<uint8_t>({0, 1, 127, 128, 255}));
    }
}

TEST_CASE("wavetable media reference: malformed metadata and integrity declarations cannot publish mismatched assets")
{
    for (const auto& [field, replacement] : std::vector<std::pair<const char*, juce::var>>{
             {"sourceFile", "relative.wav"}, {"sourceFile", 0}, {"channels", 0}, {"channels", 3}, {"channels", "1"},
             {"sampleRate", 0}, {"sampleRate", "48000"}, {"sampleRate", 48001},
             {"frames", 0}, {"frames", -1}, {"frames", 4096.5}, {"frames", "4096"},
             {"pcmFingerprint", ""}, {"pcmFingerprint", "10000000000000000"}, {"pcmFingerprint", "not-hex"},
             {"fingerprintAlgorithm", "unknown"}, {"spectral", 1}, {"spectralFrameLimit", 1},
             {"spectralFrameLimit", Sample::maxSpectralFrames + 1}, {"spectralFrameLimit", 2.5}, {"encoding", "unknown"}})
        invalidReference([&](juce::var& asset) { asset.getDynamicObject()->setProperty(field, replacement); });
    invalidReference([](juce::var& asset) { asset.getDynamicObject()->removeProperty("sourceFile"); });
    invalidReference([](juce::var& asset) { asset.getDynamicObject()->removeProperty("pcmFingerprint"); });
    invalidReference([](juce::var& asset) { asset.getDynamicObject()->removeProperty("frames"); });
}

TEST_CASE("wavetable media reference: forcing references for embedded PCM fails transactionally")
{
    const auto media = Media::fromSamples(std::array<float, 4>{0, 0.25f, -0.25f, 0}, 1, 48000); REQUIRE(media);
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    const auto before = device.nativeContent; const auto parameters = device.parameters.size();
    std::string error;
    CHECK(! devices::storeOscillatorMedia(device, 0, *media.data, error, true)); CHECK(! error.empty());
    CHECK(device.nativeContent == before); CHECK(device.parameters.size() == parameters);
}
