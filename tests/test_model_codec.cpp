// COL-2: exact model codec (core/ModelCodec.h).
#include "TestFramework.h"
#include "core/ModelCodec.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>

#include <cstring>
#include <map>
#include <random>

using namespace sonora;
namespace codec = sonora::model::codec;

namespace
{
template <class T> bool exact(T a, T b) { return std::memcmp(&a, &b, sizeof a) == 0; }

model::Song loadFixture(const char* name)
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile(name));
    REQUIRE(loaded);
    return *loaded.song;
}

std::string patchedXml(const model::Song& song)
{
    std::string error;
    auto dom = io::readAlsDom(juce::File(song.sourcePath), error);
    REQUIRE(dom);
    REQUIRE(io::patchAlsDom(song, *dom).ok);
    return dom->toString().toStdString();
}

// Exercises the fields a plain fixture leaves at their defaults.
void decorate(model::Song& song)
{
    REQUIRE(! song.tracks.empty());
    auto& track = song.tracks.front();
    track.mixer.volume = 0.123456789012345;
    track.mixer.sends = {0.5, 0.25};
    track.mixer.sendTargetIds = {91, 92};
    track.hidden = true;
    track.midiControllerTargets[7] = 4242;
    model::Device device;
    device.kind = model::DeviceKind::Rack;
    device.type = "InstrumentGroupDevice";
    device.id = 77;
    device.sourceXml = std::make_shared<const std::string>("<InstrumentGroupDevice Id=\"7\"/>");
    device.plain["Globals/NumVoices"] = 3.0;
    model::Parameter p;
    p.path = "Macro.0";
    p.value = 0.3333333333333333;
    p.midi.channel = 2;
    device.parameters.push_back(p);
    model::Device::Chain chain;
    chain.sourceXml = device.sourceXml; // shared holder: encoded once, still shared after decoding
    chain.name = "Chain";
    model::Device child;
    child.kind = model::DeviceKind::PluginVst3;
    child.type = "PluginDevice";
    child.id = 78;
    child.plugin.emplace();
    child.plugin->name = "Synth";
    child.plugin->vst3Uid = {1u, 2u, 3u, 0xffffffffu};
    child.plugin->state = {1, 2, 3, 255};
    auto content = std::make_shared<model::NativeContent>();
    content->format = "sonora-wavetable-v1";
    content->data = {9, 8, 7};
    child.nativeContent = content;
    chain.devices.push_back(child);
    device.chains.push_back(chain);
    track.devices.push_back(device);
    model::Clip clip;
    clip.uid = 123456;
    clip.kind = model::Clip::Kind::Midi;
    clip.name = "Clip \xe2\x9c\x93";
    clip.notes.push_back({0.1, 0.2, 61, 99.5f, 64.0f, 0.5f, true, 3, 1.5f, 9});
    clip.warpMode = model::WarpMode::ComplexPro;
    track.arrangementClips.push_back(clip);
    model::Scene scene;
    scene.name = "Scene";
    scene.tempo = 128.5;
    song.scenes.push_back(scene);
    song.savedClips[123456] = {track.id, 4, -1};
}
} // namespace

TEST_CASE("model codec: every fixture survives the full form byte for byte, with identical writer output")
{
    for (const char* name : {"minimal_set.xml", "two_tracks.xml", "recording_set.xml"})
    {
        auto song = loadFixture(name);
        const auto bytes = codec::encodeSong(song);
        auto decoded = codec::decodeSong(bytes);
        CHECK(codec::encodeSong(decoded) == bytes);
        CHECK(patchedXml(decoded) == patchedXml(song));
    }
}

TEST_CASE("model codec: decorated fields, shared fragments and exact doubles round-trip")
{
    auto song = loadFixture("two_tracks.xml");
    decorate(song);
    const auto bytes = codec::encodeSong(song);
    auto decoded = codec::decodeSong(bytes);
    CHECK(codec::encodeSong(decoded) == bytes);
    const auto& track = decoded.tracks.front();
    CHECK(exact(track.mixer.volume, 0.123456789012345));
    CHECK(track.midiControllerTargets[7] == 4242);
    REQUIRE(! track.devices.empty());
    const auto& rack = track.devices.back();
    CHECK(exact(rack.plain.at("Globals/NumVoices"), 3.0));
    REQUIRE(rack.chains.size() == 1);
    CHECK(rack.chains[0].sourceXml.get() == rack.sourceXml.get());
    REQUIRE(rack.chains[0].devices.size() == 1);
    const auto& plugin = rack.chains[0].devices[0];
    CHECK(plugin.plugin->vst3Uid[3] == 0xffffffffu);
    CHECK(plugin.plugin->state == std::vector<uint8_t>({1, 2, 3, 255}));
    CHECK(plugin.nativeContent && plugin.nativeContent->data == std::vector<uint8_t>({9, 8, 7}));
    CHECK(decoded.scenes.back().tempo && exact(*decoded.scenes.back().tempo, 128.5));
    CHECK(decoded.savedClips.at(123456).ordinal == 4);
    const auto& clip = track.arrangementClips.back();
    CHECK(clip.notes.back().key == 9 && exact(clip.notes.back().velocity, 99.5f));
    CHECK(clip.warpMode == model::WarpMode::ComplexPro);
}

TEST_CASE("model codec: the identity form ignores live values and follows structural edits")
{
    auto song = loadFixture("two_tracks.xml");
    decorate(song);
    auto& track = song.tracks.front();
    const auto header = codec::encodeTrackHeader(track, codec::Form::Identity);
    const auto devices = codec::encodeDevices(track.devices, codec::Form::Identity);
    const auto fullDevices = codec::encodeDevices(track.devices, codec::Form::Full);

    track.mixer.volume = 0.9;
    track.mixer.sends[1] = 0.75;
    track.mixer.solo = true;
    auto& rack = track.devices.back();
    rack.parameters[0].value = 0.9;
    rack.plain["Globals/NumVoices"] = 8.0;
    rack.enabled = false;
    rack.chains[0].devices[0].plugin->state = {4, 5};
    // Re-stored native content with identical bytes is still the same content.
    rack.chains[0].devices[0].nativeContent = std::make_shared<model::NativeContent>(*rack.chains[0].devices[0].nativeContent);
    CHECK(codec::encodeTrackHeader(track, codec::Form::Identity) == header);
    CHECK(codec::encodeDevices(track.devices, codec::Form::Identity) == devices);
    CHECK(codec::encodeDevices(track.devices, codec::Form::Full) != fullDevices);

    track.name = "Renamed";
    CHECK(codec::encodeTrackHeader(track, codec::Form::Identity) != header);
    rack.chains[0].name = "Other";
    CHECK(codec::encodeDevices(track.devices, codec::Form::Identity) != devices);
}

namespace
{
struct Fingerprint
{
    std::string identity;
    uint64_t hash = 0;
};

// Identity form and identity hash of every codec object of a song (song header, then per track its header, devices
// and clips), keyed by position.
std::map<std::string, Fingerprint> fingerprints(const model::Song& song)
{
    std::map<std::string, Fingerprint> out;
    out["song"] = {codec::encodeSongHeader(song, codec::Form::Identity), codec::hashSongHeader(song)};
    const auto track = [&](const model::Track& t, const std::string& name) {
        out[name] = {codec::encodeTrackHeader(t, codec::Form::Identity), codec::hashTrackHeader(t)};
        out[name + "/devices"] = {codec::encodeDevices(t.devices, codec::Form::Identity), codec::hashDevices(t.devices)};
        for (size_t i = 0; i < t.arrangementClips.size(); ++i)
            out[name + "/a" + std::to_string(i)] = {codec::encodeClip(t.arrangementClips[i], codec::Form::Identity),
                                                    codec::hashClip(t.arrangementClips[i])};
        for (size_t i = 0; i < t.slots.size(); ++i)
            if (t.slots[i].clip)
                out[name + "/s" + std::to_string(i)] = {codec::encodeClip(*t.slots[i].clip, codec::Form::Identity),
                                                        codec::hashClip(*t.slots[i].clip)};
    };
    for (size_t i = 0; i < song.tracks.size(); ++i) track(song.tracks[i], "t" + std::to_string(i));
    for (size_t i = 0; i < song.returns.size(); ++i) track(song.returns[i], "r" + std::to_string(i));
    track(song.main, "main");
    return out;
}

// For every object present in both: the identity forms differ exactly when the hashes do. Returns how many differ.
int agreeingChanges(const std::map<std::string, Fingerprint>& a, const std::map<std::string, Fingerprint>& b)
{
    int changed = 0, disagreements = 0;
    for (const auto& [key, before] : a)
    {
        const auto it = b.find(key);
        if (it == b.end()) continue;
        const bool bytesDiffer = before.identity != it->second.identity;
        if (bytesDiffer != (before.hash != it->second.hash)) ++disagreements;
        if (bytesDiffer) ++changed;
    }
    CHECK(disagreements == 0);
    return changed;
}
} // namespace

TEST_CASE("model codec: identity hashes match identity forms on the fixtures and a decorated song")
{
    for (const char* name : {"minimal_set.xml", "two_tracks.xml", "recording_set.xml", "decorated"})
    {
        const bool decorated = std::string(name) == "decorated";
        auto song = loadFixture(decorated ? "two_tracks.xml" : name);
        if (decorated) decorate(song);
        const auto base = fingerprints(song);

        // Across all objects of the song: equal forms, equal hashes; different forms, different hashes.
        int pairs = 0, collisions = 0;
        for (const auto& [ka, a] : base)
            for (const auto& [kb, b] : base)
            {
                ++pairs;
                if ((a.identity == b.identity) != (a.hash == b.hash)) ++collisions;
            }
        CHECK(pairs > 1 && collisions == 0);

        // A copy shares every fragment: nothing changed. A decoded copy holds new fragment pointers: both forms see that.
        CHECK(agreeingChanges(base, fingerprints(model::Song(song))) == 0);
        (void) agreeingChanges(base, fingerprints(codec::decodeSong(codec::encodeSong(song))));
    }

    // One edit at a time on the decorated song: live values change neither, structural edits change both.
    auto song = loadFixture("two_tracks.xml");
    decorate(song);
    const auto base = fingerprints(song);
    using Edit = void (*)(model::Song&);
    const std::pair<Edit, bool> edits[] = {
        // live values
        {[](model::Song& s) { s.tracks[0].mixer.volume = 0.9; }, false},
        {[](model::Song& s) { s.tracks[0].mixer.sends[1] = 0.75; }, false},
        {[](model::Song& s) { s.tracks[0].mixer.solo = true; }, false},
        {[](model::Song& s) { s.tracks[0].devices.back().parameters[0].value = 0.9; }, false},
        {[](model::Song& s) { s.tracks[0].devices.back().plain["Globals/NumVoices"] = 8.0; }, false},
        {[](model::Song& s) { s.tracks[0].devices.back().enabled = false; }, false},
        {[](model::Song& s) { s.tracks[0].devices.back().chains[0].devices[0].plugin->state = {4, 5}; }, false},
        {[](model::Song& s) {
             auto& c = s.tracks[0].devices.back().chains[0].devices[0].nativeContent;
             c = std::make_shared<model::NativeContent>(*c);
         }, false},
        // structure
        {[](model::Song& s) { s.tracks[0].name = "Renamed"; }, true},
        {[](model::Song& s) { s.tracks[0].mixer.sendTargetIds[0] = 93; }, true},
        {[](model::Song& s) { s.tracks[0].devices.back().chains[0].name = "Other"; }, true},
        {[](model::Song& s) { s.tracks[0].devices.back().macroNames = {"ab", "c"}; }, true},
        {[](model::Song& s) { s.tracks[0].devices.back().parameters[0].automationTargetId = 5; }, true},
        {[](model::Song& s) {
             auto& d = s.tracks[0].devices.back();
             d.plain["Globals/NumVoicez"] = d.plain["Globals/NumVoices"];
             d.plain.erase("Globals/NumVoices");
         }, true},
        {[](model::Song& s) { s.tracks[0].devices.back().sourceXml = std::make_shared<const std::string>("<X/>"); }, true},
        {[](model::Song& s) {
             auto& c = s.tracks[0].devices.back().chains[0].devices[0].nativeContent;
             auto changed = std::make_shared<model::NativeContent>(*c);
             changed->data[1] = 0;
             c = changed;
         }, true},
        {[](model::Song& s) { s.tracks[0].arrangementClips.back().notes[0].pitch = 62; }, true},
        {[](model::Song& s) { s.tracks[0].arrangementClips.back().notes[0].velocity = 99.50001f; }, true},
        {[](model::Song& s) { s.tracks[0].arrangementClips.back().name = "Clip"; }, true},
        {[](model::Song& s) { s.scenes.back().tempo.reset(); }, true},
        {[](model::Song& s) { s.tempo = 121.0; }, true},
    };
    for (const auto& [edit, structural] : edits)
    {
        auto copy = song;
        edit(copy);
        CHECK((agreeingChanges(base, fingerprints(copy)) > 0) == structural);
    }
    // Neighbouring strings are delimited: {"ab", "c"} and {"a", "bc"} differ.
    auto a = song, b = song;
    a.tracks[0].devices.back().macroNames = {"ab", "c"};
    b.tracks[0].devices.back().macroNames = {"a", "bc"};
    CHECK(codec::hashDevices(a.tracks[0].devices) != codec::hashDevices(b.tracks[0].devices));
}

TEST_CASE("model codec: truncated and corrupted input is rejected without crashing")
{
    auto song = loadFixture("two_tracks.xml");
    decorate(song);
    const auto clip = codec::encodeClip(song.tracks.front().arrangementClips.back(), codec::Form::Full);
    const auto devices = codec::encodeDevices(song.tracks.front().devices, codec::Form::Full);
    int rejected = 0;
    for (size_t n = 0; n < clip.size(); ++n)
    {
        try { (void) codec::decodeClip(std::string_view(clip.data(), n)); }
        catch (const model::CodecError&) { ++rejected; }
    }
    CHECK(rejected == (int) clip.size());
    std::mt19937 random(7);
    for (int round = 0; round < 3000; ++round)
    {
        auto bytes = devices;
        for (int flips = 0; flips < 4; ++flips)
            bytes[random() % bytes.size()] = static_cast<char>(random() & 0xff);
        try { (void) codec::decodeDevices(bytes); }
        catch (const model::CodecError&) {}
    }
    // Huge counts and deep nesting are refused before allocating.
    model::ByteWriter w;
    w.varint(codec::formatVersion);
    w.varint(uint64_t(1) << 40);
    bool threw = false;
    try { (void) codec::decodeDevices(w.data()); } catch (const model::CodecError&) { threw = true; }
    CHECK(threw);
    model::Device nested;
    nested.type = "AudioEffectGroupDevice";
    for (int depth = 0; depth < codec::maxDeviceDepth + 2; ++depth)
    {
        model::Device parent;
        parent.type = "AudioEffectGroupDevice";
        parent.chains.emplace_back().devices.push_back(nested);
        nested = parent;
    }
    threw = false;
    try { (void) codec::decodeDevices(codec::encodeDevices({nested}, codec::Form::Full)); } catch (const model::CodecError&) { threw = true; }
    CHECK(threw);
}
