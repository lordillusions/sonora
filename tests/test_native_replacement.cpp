#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "plugins/NativeReplacement.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <set>
#include <zstd.h>

// PLG-16: missing Serum 2 / LFOTool instances found by plug-in identity and swapped for Sonora devices in one undo step.
using namespace sonora;

namespace
{
using Bytes = std::vector<uint8_t>;
// A synthetic Serum 2 processor state (XferJson container, zstd CBOR), authored here like test_serum_state.cpp's.
void argument(Bytes& out, uint8_t major, uint64_t value)
{
    if (value < 24) { out.push_back(static_cast<uint8_t>((uint64_t) major << 5 | value)); return; }
    out.push_back(static_cast<uint8_t>(major << 5 | 25));
    out.push_back(static_cast<uint8_t>(value >> 8)); out.push_back(static_cast<uint8_t>(value));
}
void text(Bytes& out, std::string_view s) { argument(out, 3, s.size()); out.insert(out.end(), s.begin(), s.end()); }
void number(Bytes& out, double n)
{
    out.push_back(0xfb);
    const auto bits = std::bit_cast<uint64_t>(n);
    for (unsigned i = 8; i-- > 0;) out.push_back(static_cast<uint8_t>(bits >> (i * 8)));
}
void little(Bytes& out, uint64_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
Bytes serumState(double macro1Percent)
{
    Bytes cbor;
    argument(cbor, 5, 5);
    text(cbor, "product"); text(cbor, "Serum2");
    text(cbor, "component"); text(cbor, "processor");
    text(cbor, "version"); number(cbor, 9);
    text(cbor, "productVersion"); text(cbor, "2.0.24");
    text(cbor, "Macro0"); argument(cbor, 5, 1); text(cbor, "plainParams"); argument(cbor, 5, 1);
    text(cbor, "kParamValue"); number(cbor, macro1Percent);
    Bytes compressed(ZSTD_compressBound(cbor.size()));
    const auto size = ZSTD_compress(compressed.data(), compressed.size(), cbor.data(), cbor.size(), 3);
    REQUIRE(! ZSTD_isError(size));
    compressed.resize(size);
    const std::string meta = "{\"component\":\"processor\",\"product\":\"Serum2\",\"productVersion\":\"2.0.24\",\"version\":9.0}";
    Bytes out{'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0};
    little(out, meta.size(), 8);
    out.insert(out.end(), meta.begin(), meta.end());
    little(out, cbor.size(), 4);
    little(out, 2, 4);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

uint32_t fourCC(const char* c) { return (uint32_t) c[0] << 24 | (uint32_t) c[1] << 16 | (uint32_t) c[2] << 8 | (uint32_t) c[3]; }

model::Device plugin(model::DeviceKind kind, std::string name, int id)
{
    model::Device d;
    d.kind = kind;
    d.type = kind == model::DeviceKind::PluginAu ? "AuPluginDevice" : "PluginDevice";
    d.displayName = name;
    d.id = id;
    d.plugin.emplace();
    d.plugin->name = std::move(name);
    return d;
}
model::Device serum2(int id)
{
    auto d = plugin(model::DeviceKind::PluginVst3, "Serum 2", id);
    d.plugin->vst3Uid = {1448297816u, 1718833267u, 1701999981u, 540147712u}; // as the reference sets store it
    d.plugin->isInstrument = true;
    d.plugin->state = serumState(75);
    return d;
}
model::Device lfoTool(int id)
{
    auto d = plugin(model::DeviceKind::PluginVst2, "LFOTool", id);
    d.plugin->vst2UniqueId = 1483105871; // 'XffO'
    return d;
}
model::PluginParam param(std::string name, double value, int target)
{
    model::PluginParam p;
    p.name = std::move(name); p.value = value; p.automationTargetId = target;
    return p;
}
const model::Parameter* nativeParam(const model::Device& d, const std::string& path)
{
    const auto it = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == path; });
    return it != d.parameters.end() ? &*it : nullptr;
}
bool contains(const std::vector<std::string>& notes, const std::string& part)
{
    return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.find(part) != std::string::npos; });
}

// A MIDI track with Serum 2 (instrument) + Utility, a return with LFOTool inside an effect rack, a frozen track.
model::Song song()
{
    model::Song s;
    s.sourcePath = "/tmp/sonora_tests/native_replacement/Set.als";
    s.nextDeviceId = 100;
    s.nextPointeeId = 1000;
    model::Track midi;
    midi.kind = model::TrackKind::Midi; midi.id = 1; midi.name = "1-Lead";
    auto synth = serum2(10);
    synth.enabled = false;
    synth.enabledTargetId = 400;
    synth.pluginParams = {param("Macro 1", 0.75, 501), param("Main Vol", 0.5, 502), param("Macro 2", 0.1234567687, -1)};
    midi.devices.push_back(synth);
    model::Device utility;
    utility.type = "StereoGain"; utility.displayName = "Utility"; utility.id = 11;
    midi.devices.push_back(utility);
    model::Clip clip;
    clip.kind = model::Clip::Kind::Midi; clip.uid = 1; clip.currentEnd = 4;
    clip.notes.push_back({});
    clip.envelopes.push_back({502, false, {{0.0, 0.2}, {1.0, 0.8}}});
    midi.arrangementClips.push_back(clip);
    midi.automation.push_back({501, false, {{0.0, 0.1}, {4.0, 0.9}}});
    s.tracks.push_back(midi);

    model::Track frozen = midi;
    frozen.id = 2; frozen.name = "2-Frozen"; frozen.frozen = true;
    frozen.devices = {serum2(12)};
    s.tracks.push_back(frozen);

    model::Track ret;
    ret.kind = model::TrackKind::Return; ret.id = 3; ret.name = "A-Pump";
    model::Device rack;
    rack.kind = model::DeviceKind::Rack; rack.type = "AudioEffectGroupDevice"; rack.displayName = "Rack"; rack.id = 20;
    rack.chains.resize(2);
    rack.chains[0].devices.push_back(utility);
    rack.chains[0].devices.back().id = 21;
    auto lfo = lfoTool(22);
    lfo.pluginParams = {param("Phase", 0.25, 601), param("Cutoff", 0.1234567687, 602), param("Vel->PWM", 0.9, -1), param("Rate", 0.3, 603)};
    rack.chains[0].devices.push_back(lfo);
    ret.devices.push_back(rack);
    ret.automation.push_back({601, false, {{0.0, 0.0}, {2.0, 1.0}}});
    ret.automation.push_back({602, false, {{0.0, 0.5}}});
    ret.automation.push_back({603, false, {{0.0, 0.5}}});
    s.returns.push_back(ret);
    return s;
}
const auto missing = [](const model::Device&) { return false; };
} // namespace

TEST_CASE("native replacement: Serum 2 and LFOTool are recognized by plug-in identity, not by name")
{
    using A = plugins::NativeAlternative;
    CHECK(plugins::nativeAlternativeFor(serum2(1)) == A::Prism);
    CHECK(plugins::nativeAlternativeFor(lfoTool(1)) == A::CurveLfo);

    auto renamed = serum2(1); renamed.plugin->name = "Serum";
    CHECK(plugins::nativeAlternativeFor(renamed) == A::None); // the name guards the class id
    auto serum1 = plugin(model::DeviceKind::PluginVst3, "Serum 2", 1);
    serum1.plugin->vst3Uid = {1448301656u, 1718835315u, 1701999981u, 0u}; // Serum (1)'s class, renamed
    CHECK(plugins::nativeAlternativeFor(serum1) == A::None);
    auto otherVst2 = lfoTool(1); otherVst2.plugin->vst2UniqueId = 1483101268; // OTT
    CHECK(plugins::nativeAlternativeFor(otherVst2) == A::None);
    model::Device native; native.type = "Serum 2"; native.displayName = "Serum 2";
    CHECK(plugins::nativeAlternativeFor(native) == A::None);

    auto au = plugin(model::DeviceKind::PluginAu, "Serum 2", 1);
    au.plugin->auType = fourCC("aumu"); au.plugin->auSubType = fourCC("Xf2X"); au.plugin->auManufacturer = fourCC("XFER");
    CHECK(plugins::nativeAlternativeFor(au) == A::Prism);
    auto fx = au; fx.plugin->name = "Serum 2 FX"; fx.plugin->auType = fourCC("aumf"); fx.plugin->auSubType = fourCC("Xf2Y");
    CHECK(plugins::nativeAlternativeFor(fx) == A::None);
    auto lfoAu = plugin(model::DeviceKind::PluginAu, "LFOTool", 1);
    lfoAu.plugin->auType = fourCC("aumf"); lfoAu.plugin->auSubType = fourCC("XffO"); lfoAu.plugin->auManufacturer = fourCC("XFER");
    CHECK(plugins::nativeAlternativeFor(lfoAu) == A::CurveLfo);
    auto lfoMidi = lfoAu; lfoMidi.plugin->auType = fourCC("aumi"); // the MIDI-effect build has no audio-effect equivalent
    CHECK(plugins::nativeAlternativeFor(lfoMidi) == A::None);
}

TEST_CASE("native replacement: only missing instances on editable tracks are offered, racks included")
{
    const auto s = song();
    CHECK(plugins::findNativeReplacements(s, [](const model::Device&) { return true; }).empty());
    const auto found = plugins::findNativeReplacements(s, missing);
    REQUIRE(found.size() == 2); // the frozen track's Serum 2 is not offered
    CHECK(found[0].track == 0);
    CHECK(found[0].path == std::vector<int>{0});
    CHECK(found[0].alternative == plugins::NativeAlternative::Prism);
    CHECK(found[0].trackName == "1-Lead");
    CHECK(found[1].track == 2); // tracks, then returns
    CHECK((found[1].path == std::vector<int>{0, 0, 1}));
    CHECK(found[1].alternative == plugins::NativeAlternative::CurveLfo);

    // Honest notes: the importer's own warnings, the curve that was not imported, the lanes that do not carry over.
    CHECK(contains(found[0].notes, "calibration"));
    CHECK(contains(found[0].notes, "1 automation lane of Serum 2 parameters has no Sonora Prism equivalent"));
    CHECK(contains(found[0].notes, "1 automation lane now drives Sonora Prism"));
    CHECK(! contains(found[0].notes, "could not be read"));
    CHECK(contains(found[1].notes, "curve is not readable"));
    CHECK(contains(found[1].notes, "Restored 2 LFOTool settings"));
    CHECK(contains(found[1].notes, "1 automation lane of LFOTool parameters has no Sonora Curve LFO equivalent"));
    CHECK(contains(found[1].notes, "2 automation lanes now drive Sonora Curve LFO"));

    const auto offer = plugins::describeNativeReplacements(found);
    CHECK(offer.title == "Serum 2 and LFOTool are not installed");
    CHECK(offer.message.find("This set uses them 2 times.") != std::string::npos);
    CHECK(offer.message.find("\u2022 Serum 2 \u2192 Sonora Prism (1): 1-Lead") != std::string::npos);
    CHECK(offer.message.find("\u2022 LFOTool \u2192 Sonora Curve LFO (1): A-Pump") != std::string::npos);
    CHECK(offer.message.find("starts from its default curve") != std::string::npos);
    CHECK(offer.message.find(std::to_string(found[0].notes.size() + found[1].notes.size()) + " compatibility notes go") != std::string::npos);
    CHECK(plugins::describeNativeReplacements({found[0]}).title == "Serum 2 is not installed");
}

TEST_CASE("native replacement: one undoable swap keeps position, clips, switch and mapped automation")
{
    auto s = song();
    const auto before = s;
    const auto found = plugins::findNativeReplacements(s, missing);
    model::EditHistory history;
    auto command = plugins::replaceWithNative(s, found);
    CHECK(command.label == "Replace with Sonora Devices");
    CHECK(command.devicesChanged);
    history.perform(s, std::move(command));

    const auto& lead = s.tracks[0];
    REQUIRE(lead.devices.size() == 2);
    const auto& prism = lead.devices[0];
    CHECK(prism.type == "SonoraWavetable");
    CHECK(prism.kind == model::DeviceKind::Native);
    CHECK(! prism.plugin);
    REQUIRE(prism.nativeContent);
    CHECK(prism.nativeContent->data == before.tracks[0].devices[0].plugin->state); // the source patch is retained
    CHECK(! prism.enabled);
    CHECK(prism.enabledTargetId == 400);
    CHECK(prism.id >= 100); // a fresh processor identity
    REQUIRE(nativeParam(prism, "Macro.0"));
    CHECK(nativeParam(prism, "Macro.0")->automationTargetId == 501);
    CHECK_NEAR(nativeParam(prism, "Macro.0")->value, 0.75, 1e-9); // from the saved patch
    CHECK(nativeParam(prism, "Macro.1")->automationTargetId >= 1000); // no lane: a fresh target
    CHECK(lead.devices[1].type == "StereoGain");
    CHECK(lead.devices[1].id == 11);
    CHECK(lead.arrangementClips.size() == 1);
    CHECK(lead.arrangementClips[0].notes.size() == 1);
    CHECK(lead.automation.size() == 1); // lanes are kept as they were
    CHECK(lead.arrangementClips[0].envelopes.size() == 1);
    CHECK(s.tracks[1].devices[0].type == "PluginDevice"); // frozen: untouched

    const auto& rack = s.returns[0].devices[0];
    CHECK(rack.chainsEdited);
    CHECK(rack.id >= 100);
    REQUIRE(rack.chains[0].devices.size() == 2);
    CHECK(rack.chains[0].devices[0].type == "StereoGain");
    const auto& lfo = rack.chains[0].devices[1];
    CHECK(lfo.type == "SonoraCurveLfo");
    CHECK_NEAR(nativeParam(lfo, "Phase")->value, 0.25, 1e-9);
    CHECK(nativeParam(lfo, "Phase")->automationTargetId == 601);
    CHECK_NEAR(nativeParam(lfo, "Midi.VelPwm")->value, 1.0, 0);
    CHECK_NEAR(nativeParam(lfo, "Filter.Cutoff")->value, devices::nativeDeviceModel("SonoraCurveLfo").param("Filter.Cutoff", -1), 1e-9);
    CHECK(nativeParam(lfo, "Filter.Cutoff")->automationTargetId == 602); // Live's unread value is skipped, the lane is not

    std::set<int> ids;
    const std::function<void(const std::vector<model::Device>&)> unique = [&](const std::vector<model::Device>& devices) {
        for (const auto& d : devices) { CHECK(ids.insert(d.id).second); for (const auto& c : d.chains) unique(c.devices); }
    };
    for (const auto& t : s.tracks) unique(t.devices);
    for (const auto& t : s.returns) unique(t.devices);

    REQUIRE(history.undo(s));
    CHECK(s.tracks[0].devices[0].type == "PluginDevice");
    CHECK(s.tracks[0].devices[0].plugin->name == "Serum 2");
    CHECK(s.tracks[0].devices[0].id == 10);
    CHECK(s.returns[0].devices[0].id == 20);
    CHECK(! s.returns[0].devices[0].chainsEdited);
    CHECK(s.returns[0].devices[0].chains[0].devices[1].plugin->name == "LFOTool");
    REQUIRE(history.redo(s));
    CHECK(s.tracks[0].devices[0].type == "SonoraWavetable");
    CHECK(s.returns[0].devices[0].chains[0].devices[1].type == "SonoraCurveLfo");
}

TEST_CASE("native replacement: AU Serum 2 states and unreadable patches")
{
    model::Song s;
    model::Track t;
    t.kind = model::TrackKind::Midi; t.name = "AU";
    auto au = plugin(model::DeviceKind::PluginAu, "Serum 2", 5);
    au.plugin->auType = fourCC("aumu"); au.plugin->auSubType = fourCC("Xf2X"); au.plugin->auManufacturer = fourCC("XFER");
    const auto processor = serumState(40);
    juce::String plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>Controller State</key>\n\t<data>\n\t</data>\n"
                         "\t<key>Processor State</key>\n\t<data>\n\t"
                         + juce::Base64::toBase64(processor.data(), processor.size()) + "\n\t</data>\n\t<key>name</key>\n\t<string>Lead</string>\n</dict>\n</plist>\n";
    const auto utf8 = plist.toStdString();
    au.plugin->state.assign(utf8.begin(), utf8.end());
    t.devices.push_back(au);
    auto broken = serum2(6);
    broken.plugin->state = {1, 2, 3};
    t.devices.push_back(broken);
    s.tracks.push_back(t);

    const auto found = plugins::findNativeReplacements(s, missing);
    REQUIRE(found.size() == 2);
    REQUIRE(found[0].device.nativeContent);
    CHECK(found[0].device.nativeContent->data == processor);
    CHECK_NEAR(nativeParam(found[0].device, "Macro.0")->value, 0.4, 1e-9);
    CHECK(found[1].device.type == "SonoraWavetable");
    CHECK(! found[1].device.nativeContent);
    CHECK(contains(found[1].notes, "could not be read"));
    CHECK(plugins::replaceWithNative(s, found).label == "Replace with Sonora Devices");
}
