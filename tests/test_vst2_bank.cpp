// PLG-15: Live's <VstPreset> (bank chunk or parameter bank) <-> the FXB bank JUCE's VST2 host reads and writes.
#include "TestFramework.h"
#include "plugins/Vst2Bank.h"

#include <cstring>

using namespace sonora;

namespace
{
uint32_t be(const std::vector<uint8_t>& b, size_t at)
{
    return (uint32_t) b[at] << 24 | (uint32_t) b[at + 1] << 16 | (uint32_t) b[at + 2] << 8 | b[at + 3];
}
} // namespace

TEST_CASE("vst2 bank: a bank chunk round-trips through the FXB JUCE loads")
{
    model::PluginInfo live;
    live.vst2PresetType = plugins::fourCC("FBCh");
    live.vst2ProgramCount = 128;
    live.vst2PluginVersion = 1100;
    live.state = {1, 2, 3, 4, 5, 0xFF, 0};
    const auto fxb = plugins::buildVst2Bank(live, 0x58667350);
    REQUIRE(fxb.size() == 160 + live.state.size());
    CHECK(be(fxb, 0) == plugins::fourCC("CcnK"));
    CHECK(be(fxb, 4) == fxb.size() - 8);
    CHECK(be(fxb, 8) == plugins::fourCC("FBCh"));
    CHECK(be(fxb, 16) == 0x58667350u);
    CHECK(be(fxb, 20) == 1100u);
    CHECK(be(fxb, 24) == 128u);
    CHECK(be(fxb, 156) == live.state.size());
    CHECK(std::memcmp(fxb.data() + 160, live.state.data(), live.state.size()) == 0);

    model::PluginInfo back;
    REQUIRE(plugins::readVst2Bank(fxb, back));
    CHECK(back.vst2PresetType == live.vst2PresetType);
    CHECK(back.vst2ProgramCount == 128);
    CHECK(back.vst2PluginVersion == 1100);
    CHECK(back.state == live.state);
}

TEST_CASE("vst2 bank: parameter banks swap Live's little-endian floats to big-endian programs")
{
    // Two programs of three parameters, as Live stores OTT-style banks: name[28] + little-endian floats.
    model::PluginInfo live;
    live.vst2PresetType = plugins::fourCC("FxBk");
    live.vst2ParameterCount = 3;
    live.vst2ProgramCount = 2;
    const float values[2][3] = {{0.25f, 0.5f, 1.0f}, {0.0f, 0.75f, 0.125f}};
    for (int p = 0; p < 2; ++p)
    {
        const char* name = p == 0 ? "Init 1" : "Init 2";
        std::vector<uint8_t> program(28, 0);
        std::memcpy(program.data(), name, std::strlen(name));
        for (float v : values[p])
        {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            for (int i = 0; i < 4; ++i) program.push_back((uint8_t) (bits >> (8 * i)));
        }
        live.state.insert(live.state.end(), program.begin(), program.end());
    }
    const auto fxb = plugins::buildVst2Bank(live, 1234);
    REQUIRE(fxb.size() == 156 + 2 * (56 + 12));
    CHECK(be(fxb, 8) == plugins::fourCC("FxBk"));
    CHECK(be(fxb, 24) == 2u);
    const size_t second = 156 + 68;
    CHECK(be(fxb, second) == plugins::fourCC("CcnK"));
    CHECK(be(fxb, second + 8) == plugins::fourCC("FxCk"));
    CHECK(be(fxb, second + 24) == 3u);
    CHECK(std::memcmp(fxb.data() + second + 28, "Init 2", 6) == 0);
    float f = 0;
    const uint32_t bits = be(fxb, second + 56 + 4);
    std::memcpy(&f, &bits, 4);
    CHECK(f == 0.75f);

    model::PluginInfo back;
    REQUIRE(plugins::readVst2Bank(fxb, back));
    CHECK(back.vst2ParameterCount == 3);
    CHECK(back.vst2ProgramCount == 2);
    CHECK(back.state == live.state);

    live.vst2ParameterCount = 4; // the buffer no longer splits into programs
    CHECK(plugins::buildVst2Bank(live, 1234).empty());
}

TEST_CASE("vst2 bank: malformed input is refused without touching the target")
{
    model::PluginInfo info;
    info.state = {9};
    CHECK(! plugins::readVst2Bank({}, info));
    CHECK(! plugins::readVst2Bank(std::vector<uint8_t>(40, 0), info));
    model::PluginInfo chunk;
    chunk.vst2PresetType = plugins::fourCC("FBCh");
    chunk.state = {1, 2, 3};
    auto fxb = plugins::buildVst2Bank(chunk, 1);
    fxb.resize(fxb.size() - 2); // chunk size now overruns the data
    CHECK(! plugins::readVst2Bank(fxb, info));
    CHECK(info.state == std::vector<uint8_t>{9});
    model::PluginInfo unknown;
    unknown.vst2PresetType = 0x12345678;
    unknown.state = {1};
    CHECK(plugins::buildVst2Bank(unknown, 1).empty());
}

#include "core/Edit.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_core/juce_core.h>

namespace
{
const model::Device* findKind(const model::Track& track, model::DeviceKind kind)
{
    for (const auto& d : track.devices)
        if (d.kind == kind) return &d;
    return nullptr;
}
} // namespace

TEST_CASE("vst2 bank: VST2 and AU devices are written the way Live stores them and reload with their state")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    model::Song& song = *loaded.song;
    model::EditHistory history;

    model::Device vst2;
    vst2.type = "PluginDevice";
    vst2.kind = model::DeviceKind::PluginVst2;
    model::PluginInfo info;
    info.name = "OTT";
    info.path = "/Library/Audio/Plug-Ins/VST/OTT.vst";
    info.vst2UniqueId = 1483109460;
    info.vst2PresetType = plugins::fourCC("FxBk");
    info.vst2ProgramCount = 1;
    info.vst2ParameterCount = 2;
    info.vst2PluginVersion = 1;
    info.state = std::vector<uint8_t>(28 + 8, 0x3F);
    vst2.plugin = info;
    vst2.displayName = "OTT";
    model::allocateDeviceIds(song, vst2);
    vst2.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(vst2));
    history.perform(song, model::insertTrackDevice(song, 0, vst2));

    model::Device au;
    au.type = "AuPluginDevice";
    au.kind = model::DeviceKind::PluginAu;
    model::PluginInfo auInfo;
    auInfo.name = "OTT";
    auInfo.vendor = "Xfer Records";
    auInfo.auType = plugins::fourCC("aufx");
    auInfo.auSubType = plugins::fourCC("XfTT");
    auInfo.auManufacturer = plugins::fourCC("XFER");
    const std::string plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict/></plist>";
    auInfo.state.assign(plist.begin(), plist.end());
    au.plugin = auInfo;
    au.displayName = "OTT";
    model::allocateDeviceIds(song, au);
    au.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(au));
    history.perform(song, model::insertTrackDevice(song, 0, au));

    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto* v = findKind(again.song->tracks[0], model::DeviceKind::PluginVst2);
    const auto* a = findKind(again.song->tracks[0], model::DeviceKind::PluginAu);
    REQUIRE(v != nullptr);
    REQUIRE(a != nullptr);
    CHECK(v->type == "PluginDevice");
    CHECK(v->plugin->name == "OTT");
    CHECK(v->plugin->vst2UniqueId == 1483109460);
    CHECK(v->plugin->vst2PresetType == plugins::fourCC("FxBk"));
    CHECK(v->plugin->vst2ParameterCount == 2);
    CHECK(v->plugin->state == info.state);
    CHECK(a->type == "AuPluginDevice");
    CHECK(a->plugin->auSubType == plugins::fourCC("XfTT"));
    CHECK(a->plugin->auManufacturer == plugins::fourCC("XFER"));
    CHECK(a->plugin->state == auInfo.state);

    // State captured from the running plug-ins later (a chunk now, another program) patches the stored preset.
    model::Song edited = *again.song;
    for (auto& d : edited.tracks[0].devices)
        if (d.kind == model::DeviceKind::PluginVst2)
        {
            d.plugin->vst2PresetType = plugins::fourCC("FBCh");
            d.plugin->vst2ProgramNumber = 3;
            d.plugin->state = {7, 7, 7};
        }
        else if (d.kind == model::DeviceKind::PluginAu)
            d.plugin->state.push_back(' ');
    juce::TemporaryFile out2(".als");
    REQUIRE(io::saveAls(edited, out2.getFile()).ok);
    auto third = io::loadAls(out2.getFile());
    REQUIRE(third);
    v = findKind(third.song->tracks[0], model::DeviceKind::PluginVst2);
    a = findKind(third.song->tracks[0], model::DeviceKind::PluginAu);
    REQUIRE(v != nullptr);
    REQUIRE(a != nullptr);
    CHECK(v->plugin->vst2PresetType == plugins::fourCC("FBCh"));
    CHECK(v->plugin->vst2ProgramNumber == 3);
    CHECK((v->plugin->state == std::vector<uint8_t>{7, 7, 7}));
    CHECK(a->plugin->state.size() == auInfo.state.size() + 1);
}
