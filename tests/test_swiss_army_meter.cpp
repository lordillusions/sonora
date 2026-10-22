#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/SwissArmyMeter.h"
#include "dsp/Dsp.h"
#include "dsp/Loudness.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"

#include <juce_core/juce_core.h>

#include <cmath>

using namespace sonora;

namespace
{
constexpr double kRate = 48000.0;

// Stereo sine; `rightSign` -1 makes the channels opposite in phase.
void sine(juce::AudioBuffer<float>& audio, float amplitude, float rightSign = 1.0f, int offset = 0)
{
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float v = amplitude * (float) std::sin(2.0 * dsp::kPi * 1000.0 * (double) (i + offset) / kRate);
        audio.setSample(0, i, v);
        audio.setSample(1, i, rightSign * v);
    }
}

// Runs the meter over `seconds` of a 1 kHz sine in 512-sample blocks.
void run(devices::SwissArmyMeter& meter, double seconds, float amplitude, bool playing, float rightSign = 1.0f)
{
    devices::HostInfo host;
    host.playing = playing;
    host.sampleRate = kRate;
    juce::MidiBuffer midi;
    const int blocks = (int) std::ceil(seconds * kRate / 512.0);
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> audio(2, 512);
        sine(audio, amplitude, rightSign, b * 512);
        meter.setHostInfo(host);
        meter.process(audio, midi);
    }
}

juce::String setWithMaxDevices()
{
    const auto fixture = juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml").loadFileAsString();
    const juce::String devices = R"(</Eq8>
							<MxDeviceAudioEffect Id="9">
								<On><Manual Value="true" /><AutomationTarget Id="106" /></On>
								<UserName Value="" />
								<PatchSlot><Value><MxPatchRef Id="1"><FileRef>
									<RelativePathType Value="3" />
									<RelativePath Value="Presets/Audio Effects/Max Audio Effect/Imported/Swiss Army Meter.amxd" />
									<Path Value="/Users/someone/Music/Song Project/Presets/Audio Effects/Max Audio Effect/Imported/Swiss Army Meter.amxd" />
								</FileRef></MxPatchRef></Value></PatchSlot>
								<ParameterList><ParameterList /></ParameterList>
								<BlobSlot><Value><MxDBlob Id="2"><Blob>7B7D00</Blob></MxDBlob></Value></BlobSlot>
							</MxDeviceAudioEffect>
							<MxDeviceAudioEffect Id="10">
								<On><Manual Value="true" /></On>
								<UserName Value="" />
								<PatchSlot><Value><MxPatchRef Id="1"><FileRef>
									<RelativePath Value="" />
									<Path Value="/Applications/Some DAW.app/Devices/Audio Effects/LFO/LFO.amxd" />
								</FileRef></MxPatchRef></Value></PatchSlot>
							</MxDeviceAudioEffect>)";
    return fixture.replaceFirstOccurrenceOf("</Eq8>", devices);
}
} // namespace

TEST_CASE("loudness: realtime mode matches the gated integrated loudness and ramps up from the first hop")
{
    dsp::LoudnessMeter reference(kRate), realtime;
    realtime.setRealtime(true);
    realtime.prepare(kRate);
    juce::AudioBuffer<float> audio(2, 4800);
    // 6 s loud, 6 s 20 dB quieter (inside the relative gate's reach), 6 s silence (below the absolute gate).
    for (int hop = 0; hop < 180; ++hop)
    {
        sine(audio, hop < 60 ? 0.5f : hop < 120 ? 0.05f : 0.0f, 1.0f, hop * 4800);
        reference.process(audio.getReadPointer(0), audio.getReadPointer(1), 4800);
        realtime.process(audio.getReadPointer(0), audio.getReadPointer(1), 4800);
        if (hop == 0)
        {
            // After one 100 ms hop the reference has nothing yet; the realtime windows are full of silence, so momentary
            // reads 1/4 and short-term 1/30 of the steady power.
            CHECK(reference.momentary() <= dsp::kSilenceDb);
            CHECK(reference.shortTerm() <= dsp::kSilenceDb);
            CHECK(realtime.momentary() > -20.0f);
            CHECK_NEAR(realtime.shortTerm() - realtime.momentary(), 10.0f * std::log10(4.0f / 30.0f), 0.01);
        }
    }
    CHECK(realtime.hops() == 180);
    // Same gates; the realtime ramp adds three partly silent blocks (-6, -3, -1.2 LU below the tone) above the gate.
    CHECK_NEAR(realtime.integrated(), reference.integrated(), 0.15);
    CHECK(realtime.integrated() < reference.integrated());
    CHECK_NEAR(realtime.truePeakDb(), reference.truePeakDb(), 1e-6);
    CHECK_NEAR(realtime.loudnessRange(), 0.0, 0.0); // not measured in realtime mode
    // A -6 dBFS 1 kHz sine is about -3 LUFS per channel pair; each channel alone reads 3 dB less.
    reference.reset();
    sine(audio, 0.5f);
    for (int hop = 0; hop < 10; ++hop) reference.process(audio.getReadPointer(0), audio.getReadPointer(1), 4800);
    CHECK_NEAR(reference.momentaryChannel(0), reference.momentary() - 3.01f, 0.05);
    CHECK_NEAR(reference.momentaryChannel(1), reference.momentary() - 3.01f, 0.05);
}

TEST_CASE("swiss army meter: measures only while playing, holds when stopped, restarts on play and keeps audio")
{
    devices::SwissArmyMeter meter;
    meter.prepare(kRate, 512);
    run(meter, 1.0, 0.5f, false);
    auto r = meter.reading();
    CHECK(! r.measuring);
    CHECK(r.shortTerm <= dsp::kSilenceDb);

    // Audio passes unchanged.
    {
        juce::AudioBuffer<float> audio(2, 512), copy(2, 512);
        sine(audio, 0.5f);
        copy.makeCopyOf(audio);
        devices::HostInfo host;
        host.playing = true;
        juce::MidiBuffer midi;
        meter.setHostInfo(host);
        meter.process(audio, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 512; ++i) CHECK_NEAR(audio.getSample(ch, i), copy.getSample(ch, i), 0.0);
    }

    run(meter, 4.0, 0.5f, true);
    r = meter.reading();
    CHECK(r.measuring);
    const float loud = r.integrated;
    CHECK_NEAR(r.shortTerm, loud, 0.2);
    CHECK_NEAR(r.truePeak, -6.02, 0.1);
    CHECK_NEAR(r.momentaryL, loud - 3.01f, 0.2);
    CHECK_NEAR(r.correlation, 1.0, 0.01);

    run(meter, 1.0, 0.05f, false); // stopped: the readouts hold, the meters and correlation drop
    r = meter.reading();
    CHECK(! r.measuring);
    CHECK_NEAR(r.integrated, loud, 1e-6);
    CHECK_NEAR(r.shortTerm, loud, 0.2);
    CHECK(r.momentaryL <= dsp::kSilenceDb);
    CHECK_NEAR(r.correlation, 0.0, 0.0);

    run(meter, 4.0, 0.05f, true, -1.0f); // playing again: a fresh measurement of the quieter, out-of-phase signal
    r = meter.reading();
    CHECK_NEAR(r.integrated, loud - 20.0f, 0.2);
    CHECK_NEAR(r.truePeak, -26.02, 0.1);
    CHECK_NEAR(r.correlation, -1.0, 0.01);

    meter.requestReset();
    run(meter, 0.05, 0.05f, true, -1.0f);
    CHECK(meter.reading().integrated <= dsp::kSilenceDb); // no 100 ms hop has closed since the reset
    meter.setBypassed(true);
    run(meter, 0.5, 0.5f, true);
    CHECK(! meter.reading().measuring);
}

TEST_CASE("swiss army meter: the patch name is read from Windows-style and Mac-style paths on every OS")
{
    // Sets saved on Windows store backslashes; juce::File splits only on the host separator (COM-7).
    auto text = setWithMaxDevices()
                    .replace("Presets/Audio Effects/Max Audio Effect/Imported/Swiss Army Meter.amxd",
                             "Presets\\Audio Effects\\Max Audio Effect\\Imported\\Swiss Army Meter.amxd")
                    .replace("/Applications/Some DAW.app/Devices/Audio Effects/LFO/LFO.amxd",
                             "C:\\ProgramData\\Some DAW\\Devices\\Audio Effects\\LFO\\LFO.amxd");
    juce::TemporaryFile source(".xml");
    REQUIRE(source.getFile().replaceWithText(text));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    const auto& devices = loaded.song->tracks[0].devices;
    REQUIRE(devices.size() == 4);
    CHECK(devices[2].maxPatch == "Swiss Army Meter");
    CHECK(devices[3].maxPatch == "LFO");
}

TEST_CASE("swiss army meter: a Max for Live Swiss Army Meter loads as the native meter and saves back unchanged")
{
    juce::TemporaryFile source(".xml");
    REQUIRE(source.getFile().replaceWithText(setWithMaxDevices()));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    auto& devices = loaded.song->tracks[0].devices;
    REQUIRE(devices.size() == 4);
    CHECK(devices[2].kind == model::DeviceKind::MaxForLive);
    CHECK(devices[2].maxPatch == "Swiss Army Meter");
    CHECK(devices[2].displayName == "Swiss Army Meter");
    CHECK(devices[3].maxPatch == "LFO");
    CHECK(devices[3].displayName == "LFO");
    int skipped = 0;
    for (const auto& w : loaded.song->warnings)
    {
        CHECK(w.find("Swiss Army Meter") == std::string::npos);
        skipped += w == "Max for Live device skipped (unsupported): LFO";
    }
    CHECK(skipped == 1);

    devices::BuildContext build;
    auto meter = devices::createDevice(devices[2], build);
    REQUIRE(meter != nullptr);
    CHECK(dynamic_cast<devices::SwissArmyMeter*>(meter.get()) != nullptr);
    CHECK(devices::createDevice(devices[3], build) == nullptr);

    devices[2].enabled = false; // the On switch is the only state Sonora changes
    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(*loaded.song, out.getFile()).ok);
    std::string error;
    auto dom = io::readAlsDom(out.getFile(), error);
    REQUIRE(dom != nullptr);
    const juce::XmlElement* saved = nullptr;
    std::function<void(const juce::XmlElement&)> findDevice = [&](const juce::XmlElement& e) {
        if (e.hasTagName("MxDeviceAudioEffect") && e.getIntAttribute("Id") == 9) saved = &e;
        for (const auto* child : e.getChildIterator()) if (! saved) findDevice(*child);
    };
    findDevice(*dom);
    REQUIRE(saved != nullptr);
    CHECK(saved->getChildByName("BlobSlot") != nullptr);
    CHECK(saved->getChildByName("PatchSlot") != nullptr);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].devices[2].maxPatch == "Swiss Army Meter");
    CHECK(! again.song->tracks[0].devices[2].enabled);
}

TEST_CASE("swiss army meter: the browser entry inserts a Max for Live device that saves, reloads and runs natively")
{
    // Kept out of the parameter catalogue, but resolvable by its browser key like a catalogue device.
    for (const auto& entry : model::deviceCatalog()) CHECK(entry.xmlTag != "max:Swiss Army Meter");
    REQUIRE(model::maxForLiveReplacements().size() == 1);
    const auto* entry = model::findCatalogEntry("max:Swiss Army Meter");
    REQUIRE(entry != nullptr);
    CHECK(entry->implemented);
    CHECK(entry->role == model::DeviceRole::AudioEffect);
    CHECK(model::deviceCategory(*entry) == "Utilities");

    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    // What AppController::dropBrowserItem does for a native browser device.
    auto device = devices::nativeDeviceModel("max:Swiss Army Meter");
    CHECK(device.kind == model::DeviceKind::MaxForLive);
    CHECK(device.type == "MxDeviceAudioEffect");
    CHECK(device.maxPatch == "Swiss Army Meter");
    CHECK(device.displayName == "Swiss Army Meter");
    device.maxPatchPath = "/Users/someone/Music/Ableton/User Library/Presets/Audio Effects/Max Audio Effect/Swiss Army Meter.amxd";
    model::allocateDeviceIds(song, device);
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
    const auto xml = juce::XmlDocument::parse(juce::String(*device.sourceXml));
    REQUIRE(xml != nullptr);
    CHECK(xml->getStringAttribute("Id").getIntValue() == device.id);
    const auto* ref = xml->getChildByName("PatchSlot")->getChildByName("Value")->getChildByName("MxPatchRef")->getChildByName("FileRef");
    REQUIRE(ref != nullptr);
    CHECK(ref->getChildByName("RelativePath")->getStringAttribute("Value") == "Presets/Audio Effects/Max Audio Effect/Imported/Swiss Army Meter.amxd");
    CHECK(ref->getChildByName("Path")->getStringAttribute("Value") == juce::String(device.maxPatchPath));
    for (const char* tag : {"ParameterList", "FileDropList", "IdRefList", "BlobSlot", "Routables", "MpeEnabled"})
        CHECK(xml->getChildByName(tag) != nullptr);
    devices::BuildContext build;
    CHECK(dynamic_cast<devices::SwissArmyMeter*>(devices::createDevice(device, build).get()) != nullptr);

    song.tracks[0].devices.push_back(device);
    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto& reloaded = again.song->tracks[0].devices.back();
    CHECK(reloaded.kind == model::DeviceKind::MaxForLive);
    CHECK(reloaded.maxPatch == "Swiss Army Meter");
    CHECK(reloaded.maxPatchPath == device.maxPatchPath);
    CHECK(reloaded.displayName == "Swiss Army Meter");
    for (const auto& w : again.song->warnings) CHECK(w.find("Max for Live") == std::string::npos);
    CHECK(dynamic_cast<devices::SwissArmyMeter*>(devices::createDevice(reloaded, build).get()) != nullptr);
}
