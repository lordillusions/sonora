// PLG-14g: Sonora Hush as a built-in device of the app (catalog "SonoraHush"), with the VST3's DSP and session state.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/HushSession.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <cmath>

using namespace sonora;
namespace
{
using H = devices::Hush;

// Pink-ish noise with two resonances, the material Hush exists for.
void fill(juce::AudioBuffer<float>& audio, juce::Random& rng, int block)
{
    for (int s = 0; s < audio.getNumSamples(); ++s)
    {
        const float t = (float) (block * audio.getNumSamples() + s) / 48000.0f;
        const float v = 0.05f * (rng.nextFloat() * 2 - 1) + 0.4f * std::sin(6.2831853f * 3000.0f * t)
                        + 0.3f * std::sin(6.2831853f * 700.0f * t);
        audio.setSample(0, s, v);
        audio.setSample(1, s, 0.8f * v);
    }
}

devices::HushDevice* asHush(const std::unique_ptr<devices::DeviceProcessor>& p) { return dynamic_cast<devices::HushDevice*>(p.get()); }
} // namespace

TEST_CASE("hush built-in: listed with the audio effects and inserted with XML-safe parameters and its session")
{
    const auto* entry = model::findCatalogEntry("SonoraHush");
    REQUIRE(entry != nullptr);
    CHECK(entry->implemented);
    CHECK(entry->displayName == "Sonora Hush");
    CHECK(entry->role == model::DeviceRole::AudioEffect);
    CHECK(model::deviceCategory(*entry) == "Dynamics");

    const auto d = devices::nativeDeviceModel("SonoraHush");
    CHECK(d.kind == model::DeviceKind::Native);
    CHECK(d.displayName == "Sonora Hush");
    CHECK(d.parameters.size() == (size_t) H::kParams);
    for (const auto& p : d.parameters)
    {
        CHECK(p.path.find(' ') == std::string::npos); // .als element names
        CHECK(! juce::String(p.path).equalsIgnoreCase("SideChain")); // the .als sidechain routing element
    }
    REQUIRE(d.nativeContent != nullptr);
    CHECK(d.nativeContent->format == devices::HushDevice::kContentFormat);
    // The inserted device's XML is well formed (a parameter path with a space would not be).
    const auto xml = juce::XmlDocument::parse(juce::String(io::deviceTemplate(d)));
    REQUIRE(xml != nullptr);
    CHECK(xml->getChildByName("Band_2_frequency") != nullptr);
    CHECK(xml->getChildByName("SonoraState") != nullptr);

    devices::BuildContext context;
    auto processor = devices::createDevice(d, context);
    auto* hush = asHush(processor);
    REQUIRE(hush != nullptr);
    // Shown and automated under the reference names; preset files and states keep using those.
    CHECK(hush->parameters()[(size_t) H::bandParam(1, H::BandFrequency)].name == "Band 2 frequency");
    CHECK(hush->parameters()[(size_t) H::bandParam(1, H::BandFrequency)].id == "Band_2_frequency");
    CHECK(devices::HushSession::formatValue(hush->parameters()[(size_t) H::bandParam(1, H::BandFrequency)], 4000.0f) == "4.0 kHz");
    CHECK(devices::HushSession::formatValue(hush->parameters()[(size_t) H::MaxCut], H::kMaxCutOff) == "off");
}

TEST_CASE("hush built-in: processes audio with the same DSP as a plain Hush, finite and cutting resonances")
{
    devices::BuildContext context;
    auto model = devices::nativeDeviceModel("SonoraHush");
    for (auto& p : model.parameters)
        if (p.path == "Depth") p.value = 8.0;
    auto processor = devices::createDevice(model, context);
    auto* builtIn = asHush(processor);
    REQUIRE(builtIn != nullptr);
    H plain;
    for (int i = 0; i < H::kParams; ++i) plain.setParameter(i, builtIn->getParameter(i));
    CHECK_NEAR(plain.getParameter(H::Depth), 8.0, 1e-6);
    builtIn->prepare(48000, 512);
    plain.prepare(48000, 512);
    CHECK(builtIn->latencySamples() == 2304); // reported to the graph for delay compensation
    juce::AudioBuffer<float> a(2, 512), b(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng(11);
    double maxDiff = 0, energy = 0;
    for (int block = 0; block < 160; ++block)
    {
        fill(a, rng, block);
        b.makeCopyOf(a);
        builtIn->process(a, midi);
        plain.process(b, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < 512; ++s)
            {
                REQUIRE(std::isfinite(a.getSample(ch, s)));
                maxDiff = std::max(maxDiff, (double) std::abs(a.getSample(ch, s) - b.getSample(ch, s)));
                energy += a.getSample(ch, s) * a.getSample(ch, s);
            }
    }
    CHECK(maxDiff == 0.0);
    CHECK(energy > 0.0);
    CHECK(builtIn->maxReduction(0) > 1.0f);
}

TEST_CASE("hush built-in: save and reopen keep every parameter, the preset, the A/B slot and the view")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-hush-builtin");
    REQUIRE(dir.createDirectory().wasOk());
    const auto destination = dir.getChildFile("Hush built-in.als");
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), destination, error));
    auto result = io::loadAls(destination);
    REQUIRE(result);
    auto& song = *result.song;
    model::EditHistory history;
    constexpr int kTrack = 2; // the new set's first audio track
    REQUIRE(song.tracks.size() > (size_t) kTrack);
    REQUIRE(song.tracks[kTrack].kind == model::TrackKind::Audio);
    {
        auto model = devices::nativeDeviceModel("SonoraHush");
        model::allocateDeviceIds(song, model);
        model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
        history.perform(song, model::insertDevice(kTrack, std::move(model)));
    }
    REQUIRE(song.tracks[kTrack].devices.size() == 1);
    auto& inserted = song.tracks[kTrack].devices[0];
    for (const auto& p : inserted.parameters) CHECK(p.automationTargetId >= 0); // automatable like any built-in control

    // Play with it as a user would: a factory preset, slot B, some edits, the side panel.
    devices::BuildContext context;
    auto processor = devices::createDevice(inserted, context);
    auto* hush = asHush(processor);
    REQUIRE(hush != nullptr);
    auto& session = hush->session();
    const auto& presets = devices::HushSession::factoryPresets();
    const auto vocal = std::find_if(presets.begin(), presets.end(), [](const auto& p) { return p.name == "Vocal polish"; });
    REQUIRE(vocal != presets.end());
    session.loadPreset(*vocal);
    session.copyToOtherSlot();
    session.selectSlot(1);
    session.setFromEditor(H::Depth, 2.5f);
    session.selectSlot(0);
    session.setFromEditor(H::bandParam(5, H::BandUsed), 1.0f);
    session.setFromEditor(H::bandParam(5, H::BandFrequency), 1234.0f);
    session.setFromEditor(H::Mix, 70.0f);
    session.sidePanelOpen = true;
    session.graphRange = 18.0f;
    hush->setBypassed(true);
    std::vector<float> values(H::kParams);
    for (int i = 0; i < H::kParams; ++i) values[(size_t) i] = hush->getParameter(i);
    devices::captureDeviceState(inserted, processor.get());
    const auto content = inserted.nativeContent;
    devices::captureDeviceState(inserted, processor.get()); // nothing changed: the same content object stays
    CHECK(inserted.nativeContent == content);

    REQUIRE(io::saveAls(song, destination, {}).ok);
    auto loaded = io::loadAls(destination);
    REQUIRE(loaded);
    CHECK(loaded.song->warnings.empty());
    REQUIRE(loaded.song->tracks[kTrack].devices.size() == 1);
    const auto& reread = loaded.song->tracks[kTrack].devices[0];
    CHECK(reread.type == "SonoraHush");
    CHECK(! reread.enabled);
    REQUIRE(reread.nativeContent != nullptr);
    CHECK(reread.nativeContent->data == content->data);
    std::vector<std::string> warnings;
    auto reopened = devices::createNative(reread, &warnings);
    CHECK(warnings.empty());
    auto* again = dynamic_cast<devices::HushDevice*>(reopened.get());
    REQUIRE(again != nullptr);
    for (int i = 0; i < H::kParams; ++i) CHECK_NEAR(again->getParameter(i), values[(size_t) i], 1e-4);
    CHECK(again->isBypassed());
    CHECK(again->session().presetName() == "Vocal polish");
    CHECK(again->session().sidePanelOpen);
    CHECK_NEAR(again->session().graphRange, 18.0, 1e-6);
    again->session().selectSlot(1);
    CHECK_NEAR(again->getParameter(H::Depth), 2.5, 1e-6);
    CHECK(again->session().canUndo() == false); // undo history is not saved, as in the VST3

    // A damaged content block never fails the load: the parameters still come back.
    auto damaged = reread;
    auto bad = std::make_shared<model::NativeContent>(*reread.nativeContent);
    bad->data.resize(9);
    damaged.nativeContent = bad;
    warnings.clear();
    auto fallback = devices::createNative(damaged, &warnings);
    REQUIRE(dynamic_cast<devices::HushDevice*>(fallback.get()) != nullptr);
    CHECK(! warnings.empty());
    CHECK_NEAR(fallback->getParameter(H::Mix), 70.0, 1e-4);
    // Kept, switched on, for ui_snapshot checks of the face in the device chain (docs/screenshots/hush-builtin-chain.png).
    loaded.song->tracks[kTrack].devices[0].enabled = true;
    CHECK(io::saveAls(*loaded.song, juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_hush_builtin.als"), {}).ok);
    dir.deleteRecursively();
}
