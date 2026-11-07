// DEV-4c: Sonora Simpler parity with the reference's Simpler: name, playback modes, slicing, One-Shot fades and the
// reference-shaped sample zone it writes (so a sample loaded in Sonora opens in the reference).
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Simpler.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"

#include <juce_core/juce_core.h>

using namespace sonora;
using S = devices::Simpler;

namespace
{
// A sample of `frames` frames at 44.1 kHz holding a constant 0.5 (levels read straight off the output).
std::shared_ptr<engine::SampleSlot> flatSample(int frames, std::vector<double> onsets = {})
{
    auto d = std::make_shared<engine::SampleData>();
    d->sampleRate = 44100.0;
    d->audio.setSize(2, frames);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < frames; ++i) d->audio.setSample(ch, i, 0.5f);
    d->onsets = std::move(onsets);
    return engine::SampleSlot::makeReady(d);
}

std::unique_ptr<S> makeSimpler(S::Mode mode, model::SamplePart part, std::shared_ptr<engine::SampleSlot> slot)
{
    S::Zone zone;
    zone.part = std::move(part);
    zone.slot = std::move(slot);
    auto s = std::make_unique<S>(std::vector<S::Zone>{zone}, mode, 5);
    s->prepare(44100.0, 512);
    for (auto [param, value] : {std::pair{S::Volume, 0.0f}, {S::VelToVolume, 0.0f}, {S::FilterOn, 0.0f}, {S::LoopOn, 0.0f}, {S::Snap, 0.0f},
                                {S::FadeIn, 0.0f}, {S::FadeOut, 0.0f}})
        s->setParameter((int) param, value);
    return s;
}

// Renders `blocks` blocks of 512; `events(block, midi)` adds MIDI to each block. Returns the left channel.
std::vector<float> render(S& s, int blocks, const std::function<void(int, juce::MidiBuffer&)>& events)
{
    std::vector<float> out;
    juce::AudioBuffer<float> audio(2, 512);
    for (int b = 0; b < blocks; ++b)
    {
        audio.clear();
        juce::MidiBuffer midi;
        events(b, midi);
        s.process(audio, midi);
        out.insert(out.end(), audio.getReadPointer(0), audio.getReadPointer(0) + 512);
    }
    return out;
}

float peakOf(const std::vector<float>& v, size_t from, size_t to)
{
    float p = 0;
    for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

model::SamplePart part(int frames)
{
    model::SamplePart p;
    p.sampleEnd = frames;
    p.lengthSamples = frames;
    p.sampleRate = 44100;
    return p;
}
} // namespace

TEST_CASE("sonora simpler: the device is called Sonora Simpler and stays OriginalSimpler in the set")
{
    const auto* entry = model::findCatalogEntry("OriginalSimpler");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Simpler");
    const auto fresh = devices::nativeDeviceModel("OriginalSimpler");
    CHECK(fresh.type == "OriginalSimpler");
    CHECK(fresh.displayName == "Sonora Simpler");
    S simpler({}, S::Mode::Classic, 5);
    CHECK(simpler.name() == "Sonora Simpler");
    S sampler({}, S::Mode::Classic, 5, true);
    CHECK(sampler.name() == "Sonora Sampler");
    // The written device keeps the reference's tag and no user name, so the reference shows its own Simpler.
    const auto xml = juce::parseXML(juce::String(io::deviceTemplate(fresh)));
    REQUIRE(xml != nullptr);
    CHECK(xml->getTagName() == "OriginalSimpler");
    CHECK(xml->getChildByName("UserName")->getStringAttribute("Value").isEmpty());
}

TEST_CASE("sonora simpler: slices by Region, Beat, Manual and the reference's saved transients")
{
    const int frames = 44100;
    auto p = part(frames);
    p.slicingStyle = 2; p.slicingRegions = 4;
    auto s = makeSimpler(S::Mode::Slice, p, flatSample(frames, {0.1, 0.3}));
    const auto& zone = s->zones()[0];
    const auto* sample = zone.slot->get();
    std::array<double, S::kMaxSlices> starts{};
    REQUIRE(s->slices(zone, *sample, 0, frames, starts.data(), S::kMaxSlices) == 4);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(starts[(size_t) i], frames * i / 4.0, 1e-6);

    // Beat: 1/4 at the sample's own grid (0.25 s per beat from its warp markers) -> 4 slices in 1 s.
    p.slicingStyle = 1; p.slicingBeatGrid = 4; p.secondsPerBeat = 0.25;
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 4);
    CHECK_NEAR(starts[1], 0.25 * 44100, 1e-6);
    p.slicingBeatGrid = 8; // 1 Bar = 4 beats = 1 s: one slice
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 1);
    CHECK_NEAR(S::divisionBeats(0), 0.25, 1e-12);
    CHECK_NEAR(S::divisionBeats(10), 16.0, 1e-12);

    // Manual: the region start, then the points in time order (unsorted input).
    p.slicingStyle = 3; p.manualSlicePoints = {0.75, 0.25};
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    REQUIRE(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 3);
    CHECK_NEAR(starts[0], 0, 1e-6); CHECK_NEAR(starts[1], 0.25 * 44100, 1e-6); CHECK_NEAR(starts[2], 0.75 * 44100, 1e-6);

    // Transient: the reference's saved points win over Sonora's onsets; Sensitivity drops the weak ones.
    p.slicingStyle = 0; p.slicePoints = {0.0, 0.2, 0.5}; p.sliceEnergies = {1.0, 0.1, 0.6}; p.slicingThreshold = 100;
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames, {0.9}));
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 3);
    p.slicingThreshold = 50;
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames, {0.9}));
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 2);
    CHECK_NEAR(starts[1], 0.5 * 44100, 1e-6);
    p.slicePoints.clear(); p.sliceEnergies.clear(); // none saved: Sonora's own onsets
    s = makeSimpler(S::Mode::Slice, p, flatSample(frames, {0.1, 0.9}));
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0, frames, starts.data(), S::kMaxSlices) == 2);
    // Only slices inside the played region count.
    CHECK(s->slices(s->zones()[0], *s->zones()[0].slot->get(), 0.5 * 44100, frames, starts.data(), S::kMaxSlices) == 1);
}

TEST_CASE("sonora simpler: slice Playback Mono / Poly / Thru and Trigger / Gate")
{
    const int frames = 44100;
    auto p = part(frames);
    p.slicingStyle = 2; p.slicingRegions = 4; // 4 slices of 11025 frames (250 ms) from C1
    const auto notes = [](int block, juce::MidiBuffer& m) {
        if (block == 0) { m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0); m.addEvent(juce::MidiMessage::noteOn(1, 37, (juce::uint8) 100), 10); }
    };
    // Mono: the second slice takes over; Poly: both sound.
    auto mono = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    render(*mono, 2, notes);
    CHECK(mono->activeVoices() == 1);
    auto poly = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    poly->setParameter(S::SlicePlayback, 1);
    render(*poly, 2, notes);
    CHECK(poly->activeVoices() == 2);

    // Mono stops at the end of its slice (250 ms = 11025 frames), Thru plays on to the end of the region.
    const auto one = [](int block, juce::MidiBuffer& m) { if (block == 0) m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0); };
    auto slice = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    auto out = render(*slice, 60, one);
    CHECK(peakOf(out, 5000, 10000) > 0.3f);
    CHECK(peakOf(out, 11100, 20000) < 1e-4f);
    auto thru = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    thru->setParameter(S::SlicePlayback, 2);
    out = render(*thru, 90, one);
    CHECK(peakOf(out, 30000, 40000) > 0.3f);
    CHECK(peakOf(out, 44200, 46000) < 1e-4f);
    // A note past the last slice is silent.
    auto past = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    out = render(*past, 4, [](int block, juce::MidiBuffer& m) { if (block == 0) m.addEvent(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 100), 0); });
    CHECK(peakOf(out, 0, out.size()) < 1e-6f);

    // Gate: a slice fades out on release over Fade Out (100 ms), Trigger plays on.
    const auto held = [](int block, juce::MidiBuffer& m) {
        if (block == 0) m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0);
        if (block == 4) m.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
    };
    auto gate = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    gate->setParameter(S::OneShotGate, 1); gate->setParameter(S::FadeOut, 100);
    out = render(*gate, 20, held);
    const size_t release = 4 * 512, fade = 4410;
    CHECK(peakOf(out, release - 100, release) > 0.4f);
    CHECK(peakOf(out, release + fade / 2 - 50, release + fade / 2 + 50) < 0.35f); // halfway down (linear)
    CHECK(peakOf(out, release + fade + 200, release + fade + 2000) < 1e-3f);
    auto trigger = makeSimpler(S::Mode::Slice, p, flatSample(frames));
    trigger->setParameter(S::FadeOut, 100);
    out = render(*trigger, 20, held);
    CHECK(peakOf(out, release + fade + 200, release + fade + 400) > 0.3f);
}

TEST_CASE("sonora simpler: One-Shot Fade In / Fade Out shape the level; Gate releases over Fade Out")
{
    const int frames = 44100;
    const auto held = [](int block, juce::MidiBuffer& m) {
        if (block == 0) m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        if (block == 20) m.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    };
    auto shot = makeSimpler(S::Mode::OneShot, part(frames), flatSample(frames));
    shot->setParameter(S::FadeIn, 100); shot->setParameter(S::FadeOut, 200);
    auto out = render(*shot, 90, held);
    CHECK_NEAR(out[2205], 0.25f, 0.02f);  // half way through the 100 ms fade in
    CHECK(peakOf(out, 20 * 512, 30000) > 0.45f); // Trigger: the note-off changes nothing
    CHECK_NEAR(out[44100 - 4410], 0.25f, 0.02f); // half way through the 200 ms fade out before the end
    auto gated = makeSimpler(S::Mode::OneShot, part(frames), flatSample(frames));
    gated->setParameter(S::OneShotGate, 1); gated->setParameter(S::FadeOut, 50);
    out = render(*gated, 90, held);
    CHECK(peakOf(out, 20 * 512 + 2205 + 200, 20 * 512 + 8000) < 1e-3f); // gone 50 ms after the release
    // The ADSR does not apply to One-Shot: a long attack is ignored.
    auto attack = makeSimpler(S::Mode::OneShot, part(frames), flatSample(frames));
    attack->setParameter(S::Attack, 5000);
    out = render(*attack, 4, held);
    CHECK(out[100] > 0.45f);
}

TEST_CASE("sonora simpler: a loaded sample's zone is written the way the reference writes it, and reads back")
{
    model::SamplePart p;
    p.name = "Probe Hit";
    p.sample.absolutePath = "/private/tmp/Sonora Simpler Probe/hit.wav";
    p.sampleRate = 44100; p.lengthSamples = 77255; p.sampleEnd = 77254;
    p.slicingStyle = 3; p.manualSlicePoints = {0.25, 0.5};
    const auto xml = io::samplePartXml(p, 0);
    REQUIRE(xml != nullptr);
    // Every node the reference writes for a sample it loads itself.
    for (const char* tag : {"LomId", "Name", "Selection", "IsActive", "Solo", "KeyRange", "VelocityRange", "SelectorRange", "RootKey", "Detune",
                            "TuneScale", "Panorama", "Volume", "Link", "SampleStart", "SampleEnd", "SustainLoop", "ReleaseLoop", "SampleRef",
                            "SlicingThreshold", "SlicingBeatGrid", "SlicingRegions", "SlicingStyle", "SampleWarpProperties",
                            "InitialSlicePointsFromOnsets", "SlicePoints", "ManualSlicePoints", "BeatSlicePoints", "RegionSlicePoints",
                            "UseDynamicBeatSlices", "UseDynamicRegionSlices", "AreSlicesFromOnsetsEditable"})
        CHECK(xml->getChildByName(tag) != nullptr);
    const auto* ref = xml->getChildByName("SampleRef");
    for (const char* tag : {"FileRef", "LastModDate", "SourceContext", "SampleUsageHint", "DefaultDuration", "DefaultSampleRate", "SamplesToAutoWarp"})
        CHECK(ref->getChildByName(tag) != nullptr);
    const auto* file = ref->getChildByName("FileRef");
    for (const char* tag : {"RelativePathType", "RelativePath", "Path", "Type", "LivePackName", "LivePackId", "OriginalFileSize", "OriginalCrc", "SourceHint"})
        CHECK(file->getChildByName(tag) != nullptr);
    CHECK(file->getChildByName("Path")->getStringAttribute("Value") == "/private/tmp/Sonora Simpler Probe/hit.wav");
    CHECK(file->getChildByName("Type")->getStringAttribute("Value") == "2");
    CHECK(xml->getChildByName("SampleEnd")->getStringAttribute("Value") == "77254");
    CHECK(xml->getChildByName("SustainLoop")->getChildByName("End")->getStringAttribute("Value") == "77254"); // unused loop: whole region
    CHECK(xml->getChildByName("ReleaseLoop")->getChildByName("Mode")->getStringAttribute("Value") == "3");
    CHECK(xml->getChildByName("SampleWarpProperties")->getChildByName("WarpMarkers")->getNumChildElements() == 2);
    CHECK(xml->getChildByName("ManualSlicePoints")->getNumChildElements() == 2);

    // Read -> write -> read inside a whole device: playback mode, slicing, slice Playback, warp grid.
    auto device = devices::nativeDeviceModel("OriginalSimpler");
    device.id = 3;
    device.plain["Globals/PlaybackMode"] = 2;
    device.plain["SimplerSlicing/PlaybackMode"] = 2;
    device.sampleParts = {p};
    device.samplePartsEdited = true;
    auto written = juce::parseXML(juce::String(io::deviceTemplate(device)));
    REQUIRE(written != nullptr);
    if (auto* warp = written->getChildByName("Player")->getChildByName("MultiSampleMap")->getChildByName("SampleParts")
                         ->getChildByName("MultiSamplePart")->getChildByName("SampleWarpProperties"))
    {
        auto* markers = warp->getChildByName("WarpMarkers");
        markers->getChildElement(1)->setAttribute("SecTime", 2.0);
        markers->getChildElement(1)->setAttribute("BeatTime", 4.0); // 120 BPM
    }
    std::vector<std::string> warnings;
    const auto read = io::readDeviceXml(*written, warnings);
    REQUIRE(read.sampleParts.size() == 1);
    const auto& back = read.sampleParts[0];
    CHECK(back.name == "Probe Hit");
    CHECK(back.sampleEnd == 77254); CHECK(back.lengthSamples == 77255);
    CHECK(back.slicingStyle == 3);
    REQUIRE(back.manualSlicePoints.size() == 2);
    CHECK_NEAR(back.manualSlicePoints[1], 0.5, 1e-12);
    CHECK_NEAR(back.secondsPerBeat, 0.5, 1e-12);
    CHECK(! back.warped);
    CHECK_NEAR(read.plain.at("Globals/PlaybackMode"), 2.0, 1e-12);
    CHECK_NEAR(read.plain.at("SimplerSlicing/PlaybackMode"), 2.0, 1e-12);
    // Written again from its own XML, the part keeps the reference's other nodes and the edited values.
    auto edited = back;
    edited.slicingStyle = 2; edited.slicingRegions = 16; edited.manualSlicePoints = {0.1};
    const auto again = io::samplePartXml(edited, 0);
    CHECK(again->getChildByName("SlicingStyle")->getStringAttribute("Value") == "2");
    CHECK(again->getChildByName("SlicingRegions")->getStringAttribute("Value") == "16");
    CHECK(again->getChildByName("ManualSlicePoints")->getNumChildElements() == 1);
    CHECK(again->getChildByName("SelectorRange") != nullptr);
    // The device built from it plays in Slicing mode with Thru.
    devices::BuildContext build;
    auto processor = devices::createDevice(read, build);
    auto* simpler = dynamic_cast<S*>(processor.get());
    REQUIRE(simpler != nullptr);
    CHECK(simpler->playbackMode() == S::Mode::Slice);
    CHECK_NEAR(simpler->getParameter(S::SlicePlayback), 2.0f, 1e-6f);
}
