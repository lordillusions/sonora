#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <bit>
#include <limits>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;

std::shared_ptr<const devices::WavetableData> generatedTable(int length = 64, std::string name = "Own stereo table")
{
    std::vector<float> samples(static_cast<size_t>(length) * 4);
    for (int frame = 0; frame < 2; ++frame)
        for (int i = 0; i < length; ++i)
        {
            const double phase = 6.283185307179586 * i / length;
            const size_t index = static_cast<size_t>(frame * length + i) * 2;
            samples[index] = static_cast<float>(std::sin(phase) * (frame == 0 ? 0.75 : -0.5));
            samples[index + 1] = static_cast<float>(std::cos(phase) * (frame == 0 ? 0.25 : 0.625));
        }
    auto result = devices::WavetableData::fromSamples(samples, 2, length, 48000, std::move(name));
    REQUIRE(result); return result.data;
}

std::shared_ptr<const devices::OscillatorMedia> generatedMedia()
{
    std::vector<float> mono(64);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = static_cast<float>(std::sin(6.283185307179586 * static_cast<double>(i) / 64));
    const auto first = devices::OscillatorSample::fromSamples(mono, 1, 22050, "Own low zone"); REQUIRE(first);
    const auto stereo = generatedTable(2048)->originalSamples();
    const auto second = devices::OscillatorSample::fromSamples(stereo, 2, 48000, "Own high zone", true); REQUIRE(second);
    devices::OscillatorZone low; low.sample = first.data;
    low.lowKey = 20; low.highKey = 60; low.lowVelocity = 0; low.highVelocity = 80; low.rootKey = 48;
    low.tuneCents = 12; low.gainDb = -3; low.pan = -0.4f; low.velocityTracking = 0.7f; low.keyTracking = 0.8f;
    low.attackSeconds = 0.002; low.releaseSeconds = 0.05; low.offset = 8; low.end = 64;
    low.loopStart = 16; low.loopEnd = 48; low.loopCrossfade = 4; low.loop = devices::MediaLoop::Forward;
    devices::OscillatorZone high; high.sample = second.data;
    high.lowKey = 60; high.highKey = 100; high.lowVelocity = 50; high.highVelocity = 127; high.rootKey = 72;
    high.tuneCents = -6; high.gainDb = -5; high.pan = 0.2f; high.velocityTracking = 0.3f; high.keyTracking = 0.6f;
    high.attackSeconds = 0.01; high.releaseSeconds = 0.2; high.offset = 32; high.end = 2048;
    high.loopStart = 64; high.loopEnd = 2000; high.loopCrossfade = 16;
    high.loop = devices::MediaLoop::PingPong; high.loopWhileHeld = true;
    const auto media = devices::OscillatorMedia::fromZones({low, high}, "Own two-zone instrument"); REQUIRE(media); return media.data;
}

void sameMedia(const devices::OscillatorMedia& actual, const devices::OscillatorMedia& expected)
{
    CHECK(actual.name() == expected.name()); REQUIRE(actual.zones().size() == expected.zones().size());
    for (size_t i = 0; i < expected.zones().size(); ++i)
    {
        const auto& a = actual.zones()[i]; const auto& b = expected.zones()[i]; REQUIRE(a.sample);
        CHECK(a.sample->name() == b.sample->name()); CHECK(a.sample->channels() == b.sample->channels());
        CHECK_NEAR(a.sample->sampleRate(), b.sample->sampleRate(), 0);
        CHECK(a.sample->originalSamples() == b.sample->originalSamples());
        CHECK(a.sample->hasSpectralAnalysis() == b.sample->hasSpectralAnalysis());
        CHECK(a.lowKey == b.lowKey); CHECK(a.highKey == b.highKey); CHECK(a.rootKey == b.rootKey);
        CHECK(a.lowVelocity == b.lowVelocity); CHECK(a.highVelocity == b.highVelocity);
        CHECK_NEAR(a.tuneCents, b.tuneCents, 1e-7); CHECK_NEAR(a.gainDb, b.gainDb, 1e-7); CHECK_NEAR(a.pan, b.pan, 1e-7);
        CHECK_NEAR(a.velocityTracking, b.velocityTracking, 1e-7); CHECK_NEAR(a.keyTracking, b.keyTracking, 1e-7);
        CHECK_NEAR(a.attackSeconds, b.attackSeconds, 1e-12); CHECK_NEAR(a.releaseSeconds, b.releaseSeconds, 1e-12);
        CHECK(a.offset == b.offset); CHECK(a.end == b.end); CHECK(a.loopStart == b.loopStart); CHECK(a.loopEnd == b.loopEnd);
        CHECK(a.loopCrossfade == b.loopCrossfade); CHECK(a.loop == b.loop); CHECK(a.loopWhileHeld == b.loopWhileHeld);
    }
}

dsp::CurveGraph generatedCurve()
{
    dsp::CurveGraph curve;
    curve.count = 4;
    constexpr std::array<float, 4> x{0, 0.25f, 0.75f, 1}, y{0.1f, 0.9f, 0.3f, 0.1f}, tension{0.2f, -0.35f, 0.6f, 0};
    std::copy(x.begin(), x.end(), curve.x); std::copy(y.begin(), y.end(), curve.y);
    std::copy(tension.begin(), tension.end(), curve.curve);
    return curve;
}

juce::var parseContent(const model::Device& device)
{
    REQUIRE(device.nativeContent);
    const auto& bytes = device.nativeContent->data;
    juce::var value;
    REQUIRE(juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()),
                                                  static_cast<int>(bytes.size())), value).wasOk());
    REQUIRE(value.isObject()); return value;
}

void setContent(model::Device& device, const juce::var& root)
{
    const auto json = juce::JSON::toString(root, true);
    auto content = std::make_shared<model::NativeContent>();
    if (device.nativeContent) *content = *device.nativeContent;
    content->format = "sonora-wavetable-v1";
    const auto* begin = reinterpret_cast<const uint8_t*>(json.toRawUTF8());
    content->data.assign(begin, begin + json.getNumBytesAsUTF8());
    device.nativeContent = std::move(content);
}

model::Device authoredDevice()
{
    auto device = devices::nativeDeviceModel("SonoraWavetable");
    juce::var root(new juce::DynamicObject());
    root.getDynamicObject()->setProperty("version", 1);
    root.getDynamicObject()->setProperty("authorNote", "Synthetic test assets only");
    setContent(device, root);
    auto metadata = std::make_shared<model::NativeContent>(*device.nativeContent);
    metadata->sourceDirectory = "/tmp/sonora-own-content";
    metadata->controllerData = {0, 1, 127, 128, 255};
    device.nativeContent = std::move(metadata);
    device.displayName = "Generated Glass / 02";
    std::string error;
    REQUIRE(devices::storeWavetableTable(device, 2, *generatedTable(), error));
    REQUIRE(devices::storeWavetableLfo(device, 9, generatedCurve(), error));
    return device;
}

void sameCurve(const dsp::CurveGraph& actual, const dsp::CurveGraph& expected)
{
    REQUIRE(actual.count == expected.count);
    for (int i = 0; i < actual.count; ++i)
    {
        CHECK_NEAR(actual.x[i], expected.x[i], 1e-7);
        CHECK_NEAR(actual.y[i], expected.y[i], 1e-7);
        CHECK_NEAR(actual.curve[i], expected.curve[i], 1e-7);
    }
}

void restoredAssets(const model::Device& device)
{
    std::vector<std::string> warnings;
    auto processor = devices::createNative(device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(warnings.empty());
    const auto* table = synth->customTable(2); REQUIRE(table);
    const auto source = generatedTable();
    CHECK(table->originalSamples() == source->originalSamples());
    CHECK(table->channels() == 2); CHECK(table->frames() == 2); CHECK(table->frameLength() == 64);
    CHECK(table->name() == source->name()); CHECK_NEAR(table->sampleRate(), 48000, 0);
    CHECK_NEAR(synth->getParameter(W::oscParam(2, W::Table)), W::Custom, 0);
    CHECK(synth->hasCustomLfoCurve(9));
    CHECK_NEAR(synth->getParameter(W::lfoParam(9, W::Shape)), W::CustomLfoShape, 0);
    sameCurve(synth->lfoCurve(9), generatedCurve());
}

template <typename Mutate>
void invalidTable(Mutate mutate)
{
    auto device = authoredDevice(); auto root = parseContent(device);
    auto* array = root.getProperty("tables", {}).getArray(); REQUIRE(array); REQUIRE(array->size() == 1);
    mutate(array->getReference(0)); setContent(device, root);
    W synth; std::vector<std::string> warnings;
    devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty());
    for (int i = 0; i < W::kOscillators; ++i) CHECK(synth.customTable(i) == nullptr);
    CHECK(synth.hasCustomLfoCurve(9)); // one bad asset does not discard independent valid content
}

template <typename Mutate>
void invalidCurve(Mutate mutate)
{
    auto device = authoredDevice(); auto root = parseContent(device);
    auto* array = root.getProperty("lfos", {}).getArray(); REQUIRE(array); REQUIRE(array->size() == 1);
    mutate(array->getReference(0)); setContent(device, root);
    W synth; std::vector<std::string> warnings;
    devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty());
    for (int i = 0; i < W::kLfos; ++i) CHECK(! synth.hasCustomLfoCurve(i));
    CHECK(synth.customTable(2) != nullptr);
}

template <typename Mutate>
void invalidMedia(Mutate mutate)
{
    auto device = authoredDevice(); std::string error; REQUIRE(devices::storeOscillatorMedia(device, 0, *generatedMedia(), error));
    auto root = parseContent(device); auto* array = root.getProperty("media", {}).getArray(); REQUIRE(array); REQUIRE(array->size() == 1);
    mutate(array->getReference(0)); setContent(device, root);
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty());
    for (int i = 0; i < W::kOscillators; ++i) CHECK(synth.oscillatorMedia(i) == nullptr);
    CHECK(synth.customTable(2) != nullptr); CHECK(synth.hasCustomLfoCurve(9));
}
} // namespace

TEST_CASE("wavetable content: authored stereo tables and custom curves restore with immutable metadata")
{
    auto device = authoredDevice();
    const auto before = device.nativeContent; const auto beforeBytes = before->data;
    CHECK(device.nativeContentEdited); restoredAssets(device);
    const auto replacement = generatedTable(2048, "Own replacement");
    std::string error;
    REQUIRE(devices::storeWavetableTable(device, 2, *replacement, error));
    CHECK(device.nativeContent != before); CHECK(before->data == beforeBytes);
    CHECK(device.nativeContent->controllerData == before->controllerData);
    CHECK(device.nativeContent->sourceDirectory == before->sourceDirectory);
    const auto root = parseContent(device);
    CHECK(root.getProperty("authorNote", {}).toString() == "Synthetic test assets only");
    REQUIRE(root.getProperty("tables", {}).getArray()); CHECK(root.getProperty("tables", {}).getArray()->size() == 1);
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(warnings.empty()); REQUIRE(synth.customTable(2));
    CHECK(synth.customTable(2)->originalSamples() == replacement->originalSamples());
    CHECK(synth.customTableName(2) == "Own replacement"); sameCurve(synth.lfoCurve(9), generatedCurve());
    synth.setParameter(W::lfoParam(9, W::Shape), 1);
    CHECK(synth.hasCustomLfoCurve(9)); CHECK(synth.lfoCurve(9).count > 2);
    synth.setParameter(W::lfoParam(9, W::Shape), W::CustomLfoShape); sameCurve(synth.lfoCurve(9), generatedCurve());
}

TEST_CASE("wavetable content: rejected edits preserve the previous shared content and parameter state")
{
    auto device = authoredDevice(); const auto content = device.nativeContent;
    const auto parameters = device.parameters; const auto table = generatedTable(); std::string error;
    for (int index : {-1, W::kOscillators})
    { CHECK(! devices::storeWavetableTable(device, index, *table, error)); CHECK(! error.empty()); CHECK(device.nativeContent == content); }
    for (int index : {-1, W::kLfos})
    { CHECK(! devices::storeWavetableLfo(device, index, generatedCurve(), error)); CHECK(device.nativeContent == content); }
    auto curve = generatedCurve(); curve.x[0] = 0.1f;
    CHECK(! devices::storeWavetableLfo(device, 0, curve, error));
    curve = generatedCurve(); curve.x[2] = 0.1f; CHECK(! devices::storeWavetableLfo(device, 0, curve, error));
    curve = generatedCurve(); curve.y[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(! devices::storeWavetableLfo(device, 0, curve, error));
    curve = generatedCurve(); curve.curve[0] = 1.01f; CHECK(! devices::storeWavetableLfo(device, 0, curve, error));
    CHECK(device.nativeContent == content); REQUIRE(device.parameters.size() == parameters.size());
    for (size_t i = 0; i < parameters.size(); ++i) CHECK_NEAR(device.parameters[i].value, parameters[i].value, 0);
    device.type = "StereoGain"; CHECK(! devices::storeWavetableTable(device, 0, *table, error));
    CHECK(device.nativeContent == content);
}

TEST_CASE("wavetable content: ALS title controller bytes and authored content survive save undo redo and reload")
{
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_content_tests");
    REQUIRE(directory.createDirectory().wasOk());
    io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
    const auto source = directory.getChildFile("Source.als"), saved = directory.getChildFile("Content.als");
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    auto initial = devices::nativeDeviceModel("SonoraWavetable"); model::allocateDeviceIds(song, initial);
    initial.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(initial));
    model::EditHistory history; history.perform(song, model::insertDevice(0, initial)); history.clear();
    auto authored = authoredDevice(); authored.id = initial.id; authored.sourceXml = initial.sourceXml;
    history.perform(song, model::replaceDevice(song, 0, 0, authored, "Import own table and curve"));
    const auto immutable = song.tracks[0].devices[0].nativeContent; REQUIRE(immutable);
    REQUIRE(io::saveAls(song, saved, io::AlsSaveOptions{}).ok);
    auto roundtrip = io::loadAls(saved); REQUIRE(roundtrip); REQUIRE(roundtrip.song->tracks[0].devices.size() == 1);
    const auto& restored = roundtrip.song->tracks[0].devices[0]; REQUIRE(restored.nativeContent);
    CHECK(restored.displayName == authored.displayName);
    CHECK(restored.nativeContent->format == immutable->format);
    CHECK(restored.nativeContent->data == immutable->data);
    CHECK(restored.nativeContent->controllerData == immutable->controllerData);
    CHECK(restored.nativeContent->sourceDirectory == immutable->sourceDirectory); restoredAssets(restored);
    REQUIRE(history.undo(song)); CHECK(! song.tracks[0].devices[0].nativeContent);
    const auto undone = directory.getChildFile("Undo.als"); REQUIRE(io::saveAls(song, undone, io::AlsSaveOptions{}).ok);
    auto undoneReloaded = io::loadAls(undone); REQUIRE(undoneReloaded);
    CHECK(! undoneReloaded.song->tracks[0].devices[0].nativeContent);
    CHECK(undoneReloaded.song->tracks[0].devices[0].displayName == "Sonora Prism");
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].devices[0].nativeContent == immutable);
    const auto redone = directory.getChildFile("Redo.als"); REQUIRE(io::saveAls(song, redone, io::AlsSaveOptions{}).ok);
    auto redoneReloaded = io::loadAls(redone); REQUIRE(redoneReloaded); restoredAssets(redoneReloaded.song->tracks[0].devices[0]);
    CHECK(redoneReloaded.song->tracks[0].devices[0].displayName == authored.displayName);
    // A second edit replaces only its own immutable asset, then undo returns the exact original payload.
    auto edited = song.tracks[0].devices[0]; REQUIRE(devices::storeWavetableTable(edited, 0, *generatedTable(2048), error));
    history.perform(song, model::replaceDevice(song, 0, 0, edited, "Add second own table"));
    CHECK(song.tracks[0].devices[0].nativeContent != immutable); REQUIRE(history.undo(song));
    CHECK(song.tracks[0].devices[0].nativeContent == immutable); restoredAssets(song.tracks[0].devices[0]);
}

TEST_CASE("wavetable content: malformed table indices bytes and metadata are bounded without misrouting assets")
{
    invalidTable([](juce::var& value) { value.getDynamicObject()->removeProperty("index"); });
    for (const juce::var& index : {juce::var(-1), juce::var(W::kOscillators), juce::var(1.5), juce::var("2")})
        invalidTable([index](juce::var& value) { value.getDynamicObject()->setProperty("index", index); });
    for (int channels : {0, 3})
        invalidTable([channels](juce::var& value) { value.getDynamicObject()->setProperty("channels", channels); });
    for (int length : {0, 63, 65, 4096})
        invalidTable([length](juce::var& value) { value.getDynamicObject()->setProperty("frameLength", length); });
    invalidTable([](juce::var& value) { value.getDynamicObject()->setProperty("sampleRate", 0); });
    invalidTable([](juce::var& value) { value.getDynamicObject()->setProperty("samples", "broken encoding"); });
    invalidTable([](juce::var& value) {
        const std::array<uint8_t, 3> bytes{0, 1, 2};
        value.getDynamicObject()->setProperty("samples", juce::MemoryBlock(bytes.data(), bytes.size()).toBase64Encoding());
    });
    invalidTable([](juce::var& value) {
        const uint32_t bits = std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity());
        std::vector<uint8_t> bytes(64 * 2 * 4);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(bits >> (8 * (i % 4)));
        value.getDynamicObject()->setProperty("samples", juce::MemoryBlock(bytes.data(), bytes.size()).toBase64Encoding());
    });
    auto device = authoredDevice(); auto root = parseContent(device);
    auto* array = root.getProperty("tables", {}).getArray(); REQUIRE(array);
    array->add(array->getReference(0).clone()); array->getReference(1).getDynamicObject()->setProperty("name", "Duplicate should not replace");
    setContent(device, root); W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty()); CHECK(synth.customTableName(2) == "Own stereo table");
    root.getDynamicObject()->setProperty("version", 2); setContent(device, root);
    W newer; warnings.clear(); devices::prepareWavetableContent(newer, device, warnings);
    CHECK(! warnings.empty()); CHECK(newer.customTable(2) == nullptr); CHECK(! newer.hasCustomLfoCurve(9));
}

TEST_CASE("wavetable content: explicit authored lookup profiles restore exactly and legacy or unknown metadata remain bounded")
{
    using T = devices::WavetableData;
    const auto table = generatedTable()->withPlaybackProfile(T::PlaybackProfile::Octave16Approx); REQUIRE(table);
    auto device = authoredDevice(); std::string error; REQUIRE(devices::storeWavetableTable(device, 2, *table.data, error));
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(warnings.empty()); REQUIRE(synth.customTable(2));
    CHECK(synth.customTable(2)->playbackProfile() == T::PlaybackProfile::Octave16Approx);
    CHECK(synth.customTable(2)->originalSamples() == table.data->originalSamples());
    CHECK_NEAR(synth.customTable(2)->sample(.3f, .17125, 2, 1), table.data->sample(.3f, .17125, 2, 1), 0);
    auto value = parseContent(device); auto entry = value.getProperty("tables", {}).getArray()->getReference(0);
    entry.getDynamicObject()->removeProperty("playbackProfile"); setContent(device, value);
    W legacy; warnings.clear(); devices::prepareWavetableContent(legacy, device, warnings);
    CHECK(warnings.empty()); REQUIRE(legacy.customTable(2)); CHECK(legacy.customTable(2)->playbackProfile() == T::PlaybackProfile::Dense12);
    entry.getDynamicObject()->setProperty("playbackProfile", "future unsupported kernel"); setContent(device, value);
    W invalid; warnings.clear(); devices::prepareWavetableContent(invalid, device, warnings);
    CHECK(invalid.customTable(2) == nullptr);
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto& w) { return w.find("playback profile") != std::string::npos; }));
    CHECK(device.nativeContent->controllerData == std::vector<uint8_t>({0, 1, 127, 128, 255}));
}

TEST_CASE("wavetable content: immutable voice quality survives capture and supports reverting to the legacy rate")
{
    auto device = authoredDevice(); const auto original = device.nativeContent;
    std::string error; REQUIRE(devices::storeWavetableVoiceRate(device, 2, error));
    CHECK(device.nativeContent != original); CHECK(parseContent(device).getProperty("authorNote", {}).toString() == "Synthetic test assets only");
    auto processor = devices::createNative(device); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(synth->requestedVoiceRateFactor() == 2); synth->prepare(48000, 97);
    CHECK(synth->voiceRateFactor() == 2); CHECK(synth->voiceRatePreparationError().empty());
    devices::captureDeviceState(device, synth); auto rebuilt = devices::createNative(device); auto* next = dynamic_cast<W*>(rebuilt.get()); REQUIRE(next);
    CHECK(next->requestedVoiceRateFactor() == 2); CHECK(device.nativeContent->controllerData == original->controllerData);
    REQUIRE(synth->setVoiceRateFactor(1)); devices::captureDeviceState(device, synth);
    rebuilt = devices::createNative(device); next = dynamic_cast<W*>(rebuilt.get()); REQUIRE(next); CHECK(next->requestedVoiceRateFactor() == 1);
    const auto current = device.nativeContent; CHECK(! devices::storeWavetableVoiceRate(device, 3, error)); CHECK(device.nativeContent == current);
    for (const auto& invalid : std::vector<juce::var>{0, 3, 1.5, "2", true})
    {
        auto copy = device; auto root = parseContent(copy); root.getDynamicObject()->setProperty("voiceRateFactor", invalid); setContent(copy, root);
        W rejected; std::vector<std::string> warnings; devices::prepareWavetableContent(rejected, copy, warnings);
        CHECK(rejected.requestedVoiceRateFactor() == 1); CHECK(rejected.customTable(2) != nullptr);
        CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto& w) { return w.find("voice rate") != std::string::npos; }));
    }
}

TEST_CASE("wavetable content: malformed LFO indices point bounds and duplicates cannot replace valid curves")
{
    invalidCurve([](juce::var& value) { value.getDynamicObject()->removeProperty("index"); });
    for (const juce::var& index : {juce::var(-1), juce::var(W::kLfos), juce::var(2.5), juce::var("9")})
        invalidCurve([index](juce::var& value) { value.getDynamicObject()->setProperty("index", index); });
    invalidCurve([](juce::var& value) { value.getDynamicObject()->setProperty("points", "not an array"); });
    for (const char* field : {"x", "y", "tension"})
        invalidCurve([field](juce::var& value) {
            auto* points = value.getProperty("points", {}).getArray(); REQUIRE(points);
            points->getReference(1).getDynamicObject()->setProperty(field, 2);
        });
    invalidCurve([](juce::var& value) {
        auto* points = value.getProperty("points", {}).getArray(); REQUIRE(points);
        points->getReference(0).getDynamicObject()->setProperty("x", 0.05);
    });
    auto device = authoredDevice(); auto root = parseContent(device);
    auto* array = root.getProperty("lfos", {}).getArray(); REQUIRE(array); array->add(array->getReference(0).clone());
    auto* points = array->getReference(1).getProperty("points", {}).getArray(); REQUIRE(points);
    points->getReference(1).getDynamicObject()->setProperty("y", 0.5);
    setContent(device, root); W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty()); sameCurve(synth.lfoCurve(9), generatedCurve());
}

TEST_CASE("wavetable content: malformed containers and excessive entry counts are rejected without loading their assets")
{
    for (const char* key : {"tables", "lfos"})
    {
        auto device = authoredDevice(); auto root = parseContent(device);
        root.getDynamicObject()->setProperty(key, "invalid container"); setContent(device, root);
        W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
        CHECK(! warnings.empty());
        if (std::string_view(key) == "tables") CHECK(synth.customTable(2) == nullptr);
        else CHECK(! synth.hasCustomLfoCurve(9));
        root = parseContent(authoredDevice());
        auto* array = root.getProperty(key, {}).getArray(); REQUIRE(array);
        const auto entry = array->getReference(0).clone();
        const int maximum = std::string_view(key) == "tables" ? W::kOscillators : W::kLfos;
        while (array->size() <= maximum) array->add(entry.clone());
        setContent(device, root); W excessive; warnings.clear(); devices::prepareWavetableContent(excessive, device, warnings);
        CHECK(! warnings.empty());
        if (std::string_view(key) == "tables") CHECK(excessive.customTable(2) == nullptr);
        else CHECK(! excessive.hasCustomLfoCurve(9));
    }
    auto device = authoredDevice(); auto corrupt = std::make_shared<model::NativeContent>(*device.nativeContent);
    corrupt->data = {'{', '"', 'v', 'e', 'r', 's', 'i', 'o', 'n', '"', ':'}; device.nativeContent = corrupt;
    W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty()); CHECK(synth.customTable(2) == nullptr); CHECK(! synth.hasCustomLfoCurve(9));
    std::string error; CHECK(! devices::storeWavetableTable(device, 0, *generatedTable(), error));
    CHECK(! error.empty()); CHECK(device.nativeContent == corrupt);
}

TEST_CASE("wavetable content: immutable multisample PCM zones spectral analysis and chosen engine mode survive ALS save and reload")
{
    auto device = authoredDevice(); const auto previous = device.nativeContent; const auto previousBytes = previous->data;
    const auto media = generatedMedia(); std::string error;
    REQUIRE(devices::storeOscillatorMedia(device, 0, *media, error));
    CHECK(device.nativeContent != previous); CHECK(previous->data == previousBytes);
    CHECK(device.nativeContent->controllerData == previous->controllerData);
    CHECK(device.nativeContent->sourceDirectory == previous->sourceDirectory);
    CHECK(parseContent(device).getProperty("authorNote", {}).toString() == "Synthetic test assets only");
    restoredAssets(device);
    std::vector<std::string> warnings;
    auto processor = devices::createNative(device, &warnings); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(warnings.empty()); REQUIRE(synth->oscillatorMedia(0)); sameMedia(*synth->oscillatorMedia(0), *media);
    CHECK_NEAR(synth->getParameter(W::mediaParam(0, W::MediaMode)), W::MultisampleMode, 0);
    synth->setParameter(W::mediaParam(0, W::MediaMode), W::GranularMode);
    synth->setParameter(W::mediaParam(0, W::MediaReverse), 1);
    synth->setParameter(W::mediaParam(0, W::MediaGrainSize), 0.024f);
    synth->setParameter(W::mediaParam(0, W::MediaScanRate), -0.5f);
    devices::captureDeviceState(device, synth);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_content_tests"); REQUIRE(directory.createDirectory().wasOk());
    const auto source = directory.getChildFile("Media Source.als"), saved = directory.getChildFile("Media.als");
    io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    model::allocateDeviceIds(song, device); device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    model::EditHistory history; history.perform(song, model::insertDevice(0, device));
    REQUIRE(io::saveAls(song, saved, io::AlsSaveOptions{}).ok);
    auto reloaded = io::loadAls(saved); REQUIRE(reloaded); REQUIRE(reloaded.song->tracks[0].devices.size() == 1);
    const auto& restored = reloaded.song->tracks[0].devices[0]; restoredAssets(restored);
    warnings.clear(); auto rebuilt = devices::createNative(restored, &warnings); auto* savedSynth = dynamic_cast<W*>(rebuilt.get()); REQUIRE(savedSynth);
    CHECK(warnings.empty()); REQUIRE(savedSynth->oscillatorMedia(0)); sameMedia(*savedSynth->oscillatorMedia(0), *media);
    CHECK_NEAR(savedSynth->getParameter(W::mediaParam(0, W::MediaMode)), W::GranularMode, 0);
    CHECK_NEAR(savedSynth->getParameter(W::mediaParam(0, W::MediaReverse)), 1, 0);
    CHECK_NEAR(savedSynth->getParameter(W::mediaParam(0, W::MediaGrainSize)), 0.024, 1e-7);
    CHECK_NEAR(savedSynth->getParameter(W::mediaParam(0, W::MediaScanRate)), -0.5, 0);
    const auto content = device.nativeContent;
    CHECK(! devices::storeOscillatorMedia(device, -1, *media, error)); CHECK(device.nativeContent == content);
    CHECK(! devices::storeOscillatorMedia(device, W::kOscillators, *media, error)); CHECK(device.nativeContent == content);
}

TEST_CASE("wavetable content: malformed media identities PCM metadata and zone ranges cannot misroute or load a bank")
{
    invalidMedia([](juce::var& value) { value.getDynamicObject()->removeProperty("index"); });
    for (const juce::var& index : {juce::var(-1), juce::var(W::kOscillators), juce::var(1.5), juce::var("0")})
        invalidMedia([index](juce::var& value) { value.getDynamicObject()->setProperty("index", index); });
    invalidMedia([](juce::var& value) { value.getDynamicObject()->setProperty("zones", "invalid list"); });
    invalidMedia([](juce::var& value) { value.getDynamicObject()->setProperty("samples", "invalid list"); });
    const auto sampleField = [](const char* key, const juce::var& field) {
        invalidMedia([key, field](juce::var& value) {
            auto* array = value.getProperty("samples", {}).getArray(); REQUIRE(array);
            array->getReference(0).getDynamicObject()->setProperty(key, field);
        });
    };
    sampleField("channels", 0); sampleField("channels", 3); sampleField("channels", "1");
    sampleField("sampleRate", 0); sampleField("spectral", 1); sampleField("pcm", "broken");
    const auto zoneField = [](const char* key, const juce::var& field) {
        invalidMedia([key, field](juce::var& value) {
            auto* array = value.getProperty("zones", {}).getArray(); REQUIRE(array);
            array->getReference(0).getDynamicObject()->setProperty(key, field);
        });
    };
    zoneField("sample", 2); zoneField("sample", "0"); zoneField("loop", 5); zoneField("loopWhileHeld", 1);
    zoneField("lowKey", -1); zoneField("highKey", 128); zoneField("rootKey", 48.5);
    zoneField("offset", -1); zoneField("end", 65); zoneField("loopCrossfade", -1);
    zoneField("pan", 2); zoneField("keyTracking", 2); zoneField("attackSeconds", -1); zoneField("tuneCents", "12");
    auto device = authoredDevice(); std::string error; REQUIRE(devices::storeOscillatorMedia(device, 0, *generatedMedia(), error));
    auto root = parseContent(device); auto* array = root.getProperty("media", {}).getArray(); REQUIRE(array);
    array->add(array->getReference(0).clone()); array->getReference(1).getDynamicObject()->setProperty("name", "Duplicate bank");
    setContent(device, root); W synth; std::vector<std::string> warnings; devices::prepareWavetableContent(synth, device, warnings);
    CHECK(! warnings.empty()); CHECK(synth.oscillatorMediaName(0) == "Own two-zone instrument");
    root.getDynamicObject()->setProperty("media", "invalid container"); setContent(device, root);
    W invalid; warnings.clear(); devices::prepareWavetableContent(invalid, device, warnings);
    CHECK(! warnings.empty()); CHECK(invalid.oscillatorMedia(0) == nullptr);
}


TEST_CASE("wavetable content: independent matrix curves enable states Out and metadata survive native capture")
{
    auto device = authoredDevice();
    const auto graph = generatedCurve(); std::string error;
    REQUIRE(devices::storeWavetableModulationCurve(device, 63, false, graph, true, error));
    auto auxiliary = graph; auxiliary.y[1] = .25f;
    REQUIRE(devices::storeWavetableModulationCurve(device, 63, true, auxiliary, false, error));
    std::vector<std::string> warnings;
    auto processor = devices::createNative(device, &warnings); auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
    CHECK(warnings.empty()); sameCurve(synth->modulationCurve(63, false).graph, graph);
    sameCurve(synth->modulationCurve(63, true).graph, auxiliary);
    CHECK(synth->modulationCurve(63, false).enabled); CHECK(! synth->modulationCurve(63, true).enabled);
    CHECK(synth->modulationCurve(63, false).law == W::CurveLaw::NativePower);
    synth->setParameter(W::outParam(63), .125f); devices::captureDeviceState(device, synth);
    auto restored = devices::createNative(device, &warnings); auto* saved = dynamic_cast<W*>(restored.get()); REQUIRE(saved);
    CHECK_NEAR(saved->getParameter(W::outParam(63)), .125, 0);
    sameCurve(saved->modulationCurve(63, false).graph, graph);
    CHECK(parseContent(device).getDynamicObject()->getProperty("authorNote").toString() == "Synthetic test assets only");
    const auto original = device.nativeContent;
    auto bad = graph; bad.x[0] = .1f;
    CHECK(! devices::storeWavetableModulationCurve(device, 63, false, bad, true, error));
    CHECK(device.nativeContent == original);
    CHECK(! devices::storeWavetableModulationCurve(device, 64, false, graph, true, error));
    CHECK(device.nativeContent == original);
}

TEST_CASE("wavetable content: duplicate corrupt matrix curves cannot override a valid law or prevent unrelated assets")
{
    auto device = authoredDevice(); std::string error; const auto graph = generatedCurve();
    REQUIRE(devices::storeWavetableModulationCurve(device, 0, false, graph, true, error));
    auto root = parseContent(device); auto* entries = root.getDynamicObject()->getProperty("modulationCurves").getArray(); REQUIRE(entries);
    auto entry = entries->getReference(0);
    entry.getDynamicObject()->setProperty("law", "exponential");
    entries->add(entry.clone());
    auto corrupt = entry.clone(); corrupt.getDynamicObject()->setProperty("index", 128); entries->add(corrupt);
    setContent(device, root);
    std::vector<std::string> warnings; auto processor = devices::createNative(device, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth); CHECK(warnings.size() == 2);
    CHECK(synth->customTable(2)); CHECK(synth->hasCustomLfoCurve(9));
    sameCurve(synth->modulationCurve(0, false).graph, graph);
    CHECK(synth->modulationCurve(0, false).law == W::CurveLaw::ImportedExponential);
    REQUIRE(devices::storeWavetableModulationCurve(device, 0, false, graph, false, error));
    warnings.clear(); auto disabled = devices::createNative(device, &warnings);
    auto* rebuilt = dynamic_cast<W*>(disabled.get()); REQUIRE(rebuilt);
    CHECK(! rebuilt->modulationCurve(0, false).enabled);
    CHECK(rebuilt->modulationCurve(0, false).law == W::CurveLaw::ImportedExponential);
}
