#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "devices/native/FmSynth.h"
#include "devices/native/LfoShaper.h"
#include "devices/native/MidiVelocity.h"
#include "devices/native/Simpler.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "midi/MappingStore.h"
#include <limits>
#include <numbers>

using namespace sonora;
namespace
{
juce::AudioBuffer<float> blank(int n, float level = 0)
{
    juce::AudioBuffer<float> audio(2, n);
    for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < n; ++s) audio.setSample(ch, s, level);
    return audio;
}
void finite(const juce::AudioBuffer<float>& audio)
{
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int s = 0; s < audio.getNumSamples(); ++s) REQUIRE(std::isfinite(audio.getSample(ch, s)));
}
}

TEST_CASE("native plugins: catalog devices create actual processors and expose stable defaults")
{
    devices::BuildContext context;
    for (const auto& entry : model::deviceCatalog())
    {
        if (! entry.implemented) continue;
        auto d = devices::nativeDeviceModel(std::string(entry.xmlTag));
        auto processor = devices::createDevice(d, context);
        REQUIRE(processor);
        CHECK(! processor->parameters().empty());
        CHECK(model::deviceCategory(entry).size() > 0);
        CHECK(processor->name().find("placeholder") == std::string::npos);
    }
}

TEST_CASE("native MIDI insertion: existing device mappings follow insertion undo and structural rack edits")
{
    model::Song song; song.tracks.resize(1);
    auto fm = devices::nativeDeviceModel("SonoraFm"); fm.id = 7;
    auto rack = devices::nativeDeviceModel("InstrumentGroupDevice"); rack.id = 8;
    song.tracks[0].devices = {fm, rack};
    auto original = midi::deviceOrder(song);
    midi::Mapping mapping; mapping.target.kind = midi::ControlTarget::Kind::DeviceParameter;
    mapping.target.param = "Volume"; mapping.target.device = 0;
    auto on = mapping; on.target.kind = midi::ControlTarget::Kind::DeviceOn; on.target.device = 1;
    std::vector<midi::Mapping> mappings{mapping, on};
    auto velocity = devices::nativeDeviceModel("SonoraVelocity"); velocity.id = 9;
    song.tracks[0].devices.insert(song.tracks[0].devices.begin(), velocity);
    auto inserted = midi::deviceOrder(song); midi::rebindDeviceMappings(original, inserted, mappings);
    CHECK(mappings[0].target.device == 1); CHECK(mappings[1].target.device == 2);
    midi::rebindDeviceMappings(inserted, original, mappings);
    CHECK(mappings[0].target.device == 0); CHECK(mappings[1].target.device == 1);
    song.tracks[0].devices = {fm, rack}; song.tracks[0].devices[1].id = 20;
    auto edited = midi::deviceOrder(song); midi::rebindDeviceMappings(original, edited, mappings);
    CHECK(mappings[1].target.device == 1);
    song.tracks[0].devices.erase(song.tracks[0].devices.begin());
    midi::rebindDeviceMappings(edited, midi::deviceOrder(song), mappings);
    CHECK(mappings[0].target.device == -1); CHECK(mappings[1].target.device == 0);
}

TEST_CASE("native FM: sample-exact note timing, all algorithms, channel sustain and reset")
{
    using F = devices::FmSynth;
    for (int algorithm = 0; algorithm < 8; ++algorithm)
    {
        F synth; synth.prepare(48000, 4096); synth.setParameter(F::Algorithm, (float) algorithm);
        synth.setParameter(F::oscParam(1, F::Level), 0.7f); synth.setParameter(F::oscParam(2, F::Level), 0.3f);
        synth.setParameter(F::oscParam(3, F::Level), 0.2f); synth.setParameter(F::Feedback, 0.5f);
        auto audio = blank(4096); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 79);
        midi.addEvent(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 90), 700);
        synth.process(audio, midi); finite(audio);
        CHECK_NEAR(audio.getMagnitude(0, 79), 0, 0);
        CHECK(audio.getRMSLevel(0, 80, 3000) > 0.005f);
        CHECK(audio.getMagnitude(0, 4096) < 1);
        synth.reset(); audio.clear(); midi.clear(); synth.process(audio, midi); CHECK_NEAR(audio.getMagnitude(0, 4096), 0, 0);
    }
    F synth; synth.prepare(48000, 4096);
    for (int o = 0; o < 4; ++o) synth.setParameter(F::oscParam(o, F::Release), 1);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::noteOn(2, 67, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    midi.addEvent(juce::MidiMessage::allSoundOff(2), 100);
    auto audio = blank(4096); synth.process(audio, midi);
    CHECK(audio.getRMSLevel(0, 1000, 2000) > 0.01f); // channel 1 is held by its own pedal
    audio.clear(); midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0); synth.process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(200, 3000), 0, 1e-6);
    synth.setParameter(F::Feedback, std::numeric_limits<float>::quiet_NaN());
    CHECK(std::isfinite(synth.getParameter(F::Feedback)));
}

TEST_CASE("native FM: rendering is invariant to host block size and MIDI remains available downstream")
{
    devices::FmSynth whole, split;
    whole.prepare(48000, 2048); split.prepare(48000, 128);
    auto a = blank(2048), b = blank(2048);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(3, 69, (juce::uint8) 127), 31);
    midi.addEvent(juce::MidiMessage::pitchWheel(3, 10000), 555);
    midi.addEvent(juce::MidiMessage::noteOff(3, 69), 1027);
    whole.process(a, midi); CHECK(midi.getNumEvents() == 3);
    for (int from = 0; from < 2048; from += 128)
    {
        juce::AudioBuffer<float> block(b.getArrayOfWritePointers(), 2, from, 128);
        juce::MidiBuffer events; events.addEvents(midi, from, 128, -from); split.process(block, events);
    }
    for (int s = 0; s < 2048; ++s) CHECK_NEAR(a.getSample(0, s), b.getSample(0, s), 1e-6);
}

TEST_CASE("native LFO: synced custom envelope is block-invariant and wet/depth/bypass are exact")
{
    using L = devices::LfoShaper;
    L whole, split;
    for (auto* lfo : {&whole, &split})
    {
        lfo->prepare(48000, 4800); lfo->setParameter(L::Shape, 6); lfo->setParameter(L::Smooth, 0);
        lfo->setParameter(L::Rate, 6);
        for (int i = 0; i < L::kPoints; ++i) lfo->setParameter(L::PointStart + i, (float) i / 16);
    }
    juce::MidiBuffer midi;
    auto a = blank(4800, 1), b = blank(4800, 1);
    whole.setHostInfo({120, 0, true, 48000}); whole.process(a, midi);
    for (int from = 0; from < 4800; from += 96)
    {
        juce::AudioBuffer<float> block(b.getArrayOfWritePointers(), 2, from, 96);
        split.setHostInfo({120, (double) from / 24000, true, 48000}); split.process(block, midi);
    }
    CHECK_NEAR(a.getSample(0, 0), 0, 1e-6); CHECK_NEAR(a.getSample(0, 3000), 0.5, 1e-5);
    for (int s = 0; s < 4800; ++s) CHECK_NEAR(a.getSample(0, s), b.getSample(0, s), 1e-5);
    whole.setParameter(L::Depth, 0); a = blank(200, 0.7f); whole.process(a, midi); CHECK_NEAR(a.getSample(0, 100), 0.7, 1e-6);
    whole.setParameter(L::Depth, 1); whole.setParameter(L::Mix, 0); a = blank(200, 0.7f); whole.process(a, midi); CHECK_NEAR(a.getSample(0, 100), 0.7, 1e-6);
    whole.setBypassed(true); a = blank(200, 0.7f); whole.process(a, midi); CHECK_NEAR(a.getSample(0, 100), 0.7, 1e-6);
}

TEST_CASE("native LFO: note one-shot triggers at the event sample then returns to unity")
{
    using L = devices::LfoShaper;
    L lfo; lfo.prepare(1000, 2000); lfo.setParameter(L::Sync, 0); lfo.setParameter(L::Hertz, 2);
    lfo.setParameter(L::Trigger, 3); lfo.setParameter(L::Shape, 3); lfo.setParameter(L::Smooth, 0);
    auto audio = blank(1000, 1); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 37); lfo.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 36), 1, 1e-6); CHECK_NEAR(audio.getSample(0, 37), 0, 1e-6);
    CHECK_NEAR(audio.getSample(0, 287), 0.5, 1e-5); CHECK_NEAR(audio.getSample(0, 540), 1, 1e-6);
    CHECK(midi.getNumEvents() == 1);
}

TEST_CASE("native velocity: changes note-ons without altering audio, releases, channels or SysEx")
{
    devices::MidiVelocity velocity; velocity.prepare(48000, 512);
    velocity.setParameter(0, 60); velocity.setParameter(1, 60);
    auto audio = blank(512, 0.4f); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(4, 70, (juce::uint8) 127), 12);
    midi.addEvent(juce::MidiMessage::noteOff(4, 70, (juce::uint8) 35), 80);
    const char sysex[] = {0x01, 0x02, 0x03}; midi.addEvent(juce::MidiMessage::createSysExMessage(sysex, 3), 99);
    velocity.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 100), 0.4, 1e-6);
    CHECK(midi.getNumEvents() == 3); auto it = midi.begin();
    CHECK((*it).samplePosition == 12); CHECK((*it).getMessage().getVelocity() == 60); CHECK((*it).getMessage().getChannel() == 4);
    ++it; CHECK((*it).getMessage().getVelocity() == 35); ++it; CHECK((*it).getMessage().isSysEx());
}

TEST_CASE("native sampler: independent key/velocity layers play the expected zones")
{
    using S = devices::Simpler;
    auto data = std::make_shared<engine::SampleData>(); data->sampleRate = 48000; data->audio = blank(4800, 0.5f);
    auto slot = engine::SampleSlot::makeReady(data);
    S::Zone low, high; low.slot = high.slot = slot;
    low.part.keyMin = 0; low.part.keyMax = 59; low.part.rootKey = 48; low.part.volume = 0.2;
    high.part.keyMin = 60; high.part.keyMax = 127; high.part.velMin = 100; high.part.rootKey = 60;
    S sampler({low, high}, S::Mode::Classic, 2, true); sampler.prepare(48000, 1024);
    CHECK(sampler.name() == "Sonora Sampler");
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 60), 0);
    auto audio = blank(1024); sampler.process(audio, midi); CHECK_NEAR(audio.getMagnitude(0, 1024), 0, 0);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127), 0); sampler.process(audio, midi);
    CHECK(audio.getRMSLevel(0, 500, 500) > 0.3f); finite(audio);
}

TEST_CASE("native drum rack: choke groups stop one-shot samples across MIDI channels")
{
    using S = devices::Simpler;
    auto data = std::make_shared<engine::SampleData>(); data->sampleRate = 48000; data->audio = blank(4800, 0.5f);
    S::Zone zone; zone.slot = engine::SampleSlot::makeReady(data);
    std::vector<devices::RackProcessor::Chain> chains;
    for (int i = 0; i < 2; ++i)
    {
        devices::RackProcessor::Chain chain; chain.receivingNote = 36 + i; chain.sendingNote = 60; chain.chokeGroup = 1;
        auto part = zone; if (i == 1) part.part.volume = 0;
        chain.devices.push_back(std::make_unique<S>(std::vector<S::Zone>{part}, S::Mode::OneShot, 2)); chains.push_back(std::move(chain));
    }
    devices::RackProcessor rack("Drums", true, true, std::move(chains)); rack.prepare(48000, 512);
    auto audio = blank(512); juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(5, 36, (juce::uint8) 127), 0);
    rack.process(audio, midi); CHECK(audio.getRMSLevel(0, 100, 300) > 0.1f);
    audio.clear(); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(9, 37, (juce::uint8) 127), 17); rack.process(audio, midi);
    CHECK(audio.getMagnitude(0, 17) > 0.1f); CHECK_NEAR(audio.getMagnitude(17, 495), 0, 1e-6);
}

TEST_CASE("native plugins: authored instruments, samples, racks, macros, undo and reload retain their state")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_native_plugin_tests");
    dir.createDirectory();
    const auto source = dir.getChildFile("Source.als"), destination = dir.getChildFile("Native Plugins.als");
    io::NewSetOptions options; options.midiTracks = 4; options.audioTracks = 1; options.returnTracks = 0;
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    const auto sample = dir.getChildFile("Sonora Sine.wav"); sample.deleteFile();
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> output(sample.createOutputStream()); REQUIRE(output);
    auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24)); REQUIRE(writer);
    auto tone = blank(48000);
    for (int s = 0; s < 48000; ++s)
        for (int ch = 0; ch < 2; ++ch) tone.setSample(ch, s, 0.5f * (float) std::sin(2 * std::numbers::pi * 261.625565 * s / 48000));
    REQUIRE(writer->writeFromAudioSampleBuffer(tone, 0, 48000)); writer.reset();
    model::SamplePart zone; zone.name = "Sonora Sine"; zone.sample.absolutePath = sample.getFullPathName().toStdString();
    zone.sampleRate = 48000; zone.sampleEnd = zone.lengthSamples = zone.loopEnd = 48000; zone.loopMode = 1;
    auto fm = devices::nativeDeviceModel("SonoraFm"), sampler = devices::nativeDeviceModel("MultiSampler");
    sampler.sampleParts = {zone}; sampler.samplePartsEdited = true;
    auto drums = devices::nativeDeviceModel("DrumGroupDevice"), rack = devices::nativeDeviceModel("InstrumentGroupDevice");
    model::Device::Chain pad; pad.name = "Sonora Sine"; pad.receivingNote = 36; pad.sendingNote = 60; pad.chokeGroup = 1; pad.devices = {sampler};
    drums.chains = {pad}; drums.chainsEdited = true;
    model::Device::Chain layer; layer.name = "FM Layer"; layer.keyMax = 95; layer.devices = {fm};
    auto& mapped = layer.devices[0].parameters[devices::FmSynth::Volume];
    mapped.macroIndex = 0; mapped.rangeMin = -36; mapped.rangeMax = -6; mapped.macroMappingEdited = true;
    rack.chains = {layer}; layer.name = "Sample Layer"; layer.devices = {sampler}; rack.chains.push_back(layer); rack.chainsEdited = true;
    rack.parameters[0].value = 100; rack.macroNames = {"FM Volume"};
    auto lfo = devices::nativeDeviceModel("SonoraLfo"); lfo.parameters[devices::LfoShaper::Shape].value = 6;
    const std::array<model::Device, 5> devices = {fm, sampler, drums, rack, lfo};
    model::EditHistory history;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        auto d = devices[i]; model::allocateDeviceIds(song, d); d.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(d));
        song.tracks[i].name = d.displayName; history.perform(song, model::insertDevice((int) i, std::move(d)));
        if (i < 4)
        {
            song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
            model::Clip clip; REQUIRE(model::makeEmptyMidiClip(song, 0, 4, clip)); clip.name = "Sonora Pattern";
            for (int note = 0; note < 4; ++note) clip.notes.push_back({(double) note, 0.5, i == 2 ? 36 : 60 + note * 2, 100});
            history.perform(song, model::insertClip((int) i, clip, "Add Demo Notes"));
        }
    }
    io::AlsSaveOptions saveOptions;
    const auto saved = io::saveAls(song, destination, saveOptions); REQUIRE(saved.ok);
    auto again = io::loadAls(destination); REQUIRE(again);
    CHECK(again.song->tracks[1].devices[0].sampleParts.size() == 1);
    CHECK(again.song->tracks[1].devices[0].sampleParts[0].loopMode == 1);
    const auto& reloaded = again.song->tracks[3].devices[0]; REQUIRE(reloaded.chains.size() == 2);
    CHECK(reloaded.chains[0].keyMax == 95);
    const auto& mappedParams = reloaded.chains[0].devices[0].parameters;
    const auto mappedIt = std::find_if(mappedParams.begin(), mappedParams.end(), [](const auto& p) { return p.path == "Volume"; });
    REQUIRE(mappedIt != mappedParams.end()); CHECK(mappedIt->macroIndex == 0);
    devices::BuildContext context; context.song = again.song.get(); engine::SampleCache samples; context.samples = &samples;
    auto processor = devices::createDevice(reloaded, context); auto* rp = dynamic_cast<devices::RackProcessor*>(processor.get()); REQUIRE(rp);
    CHECK_NEAR(rp->chains()[0].devices[0]->getParameter(devices::FmSynth::Volume), -36 + 30.0 * 100 / 127, 1e-5);
    auto changed = song.tracks[1].devices[0]; changed.sampleParts[0].rootKey = 48; changed.id = -1; model::allocateDeviceIds(song, changed);
    history.perform(song, model::replaceDevice(song, 1, 0, changed, "Edit Sample Root"));
    CHECK(song.tracks[1].devices[0].sampleParts[0].rootKey == 48); history.undo(song);
    CHECK(song.tracks[1].devices[0].sampleParts[0].rootKey == 60); history.redo(song);
    CHECK(song.tracks[1].devices[0].sampleParts[0].rootKey == 48);
    REQUIRE(io::saveAls(song, destination, saveOptions).ok);
    again = io::loadAls(destination); REQUIRE(again); CHECK(again.song->tracks[1].devices[0].sampleParts[0].rootKey == 48);
}

TEST_CASE("native rack: state capture retains unsupported child identity and nested edits survive XML reload")
{
    auto rack = devices::nativeDeviceModel("InstrumentGroupDevice");
    model::Device unknown; unknown.type = "UnknownEffect"; unknown.displayName = "Retained";
    unknown.sourceXml = std::make_shared<const std::string>("<UnknownEffect Id=\"6\"><UnknownData Value=\"keep\"/></UnknownEffect>");
    auto sampler = devices::nativeDeviceModel("MultiSampler");
    model::SamplePart zone; zone.name = "Original"; zone.rootKey = 60;
    zone.sourceXml = std::make_shared<const std::string>("<MultiSamplePart Id=\"0\"><UnparsedZone Value=\"preserve\"/></MultiSamplePart>");
    sampler.sampleParts = {zone}; sampler.samplePartsEdited = true;
    model::Device::Chain chain; chain.devices = {unknown, sampler}; chain.name = "Layer";
    chain.sourceXml = std::make_shared<const std::string>("<InstrumentBranch Id=\"0\"><UnparsedBranch Value=\"preserve\"/></InstrumentBranch>");
    rack.chains = {chain}; rack.chainsEdited = true;
    devices::BuildContext context; context.placeholderForMissingInstruments = false;
    auto processor = devices::createDevice(rack, context); auto* rp = dynamic_cast<devices::RackProcessor*>(processor.get()); REQUIRE(rp);
    REQUIRE(rp->chains()[0].processorForModelDevice.size() == 2);
    CHECK(rp->chains()[0].processorForModelDevice[0] == nullptr);
    auto* sp = rp->chains()[0].processorForModelDevice[1]; REQUIRE(sp);
    sp->setParameter(devices::Simpler::Transpose, 7); sp->setBypassed(true);
    devices::captureDeviceState(rack, processor.get());
    CHECK(rack.chains[0].devices[0].parameters.empty());
    CHECK_NEAR(rack.chains[0].devices[1].param("Pitch/TransposeKey", 0), 7, 0);
    CHECK(! rack.chains[0].devices[1].enabled);
    auto root = io::createNewSetDom(); REQUIRE(root);
    auto* list = root->getChildByName("LiveSet")->getChildByName("Tracks")->getFirstChildElement()->getChildByName("DeviceChain")
                     ->getChildByName("DeviceChain")->getChildByName("Devices"); REQUIRE(list);
    model::Song ids; model::allocateDeviceIds(ids, rack);
    const auto xml = io::deviceTemplate(rack);
    CHECK(xml.find("UnparsedZone") != std::string::npos); CHECK(xml.find("UnparsedBranch") != std::string::npos);
    auto fragment = juce::XmlDocument::parse(juce::String(xml)); REQUIRE(fragment); list->addChildElement(fragment.release());
    auto loaded = io::parseAls(*root, "/tmp/native_source.xml"); REQUIRE(loaded);
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_native_unknown.xml");
    std::string error; REQUIRE(io::writeAlsDom(*root, temp, error)); loaded.song->sourcePath = temp.getFullPathName().toStdString();
    auto& changed = loaded.song->tracks[0].devices[0];
    changed.chains[0].devices[1].sampleParts[0].rootKey = 49;
    changed.chains[0].devices[1].samplePartsEdited = true; changed.chainsEdited = true;
    const auto saved = temp.getSiblingFile("sonora_native_unknown_saved.xml"); REQUIRE(io::saveAls(*loaded.song, saved).ok);
    const auto text = saved.loadFileAsString(); CHECK(text.contains("UnparsedZone")); CHECK(text.contains("UnparsedBranch")); CHECK(text.contains("UnknownData"));
    auto again = io::loadAls(saved); REQUIRE(again);
    CHECK(again.song->tracks[0].devices[0].chains[0].devices[1].sampleParts[0].rootKey == 49);
    CHECK(! again.song->tracks[0].devices[0].chains[0].devices[1].enabled);
    CHECK_NEAR(again.song->tracks[0].devices[0].chains[0].devices[1].param("Pitch/TransposeKey", 0), 7, 0);
}
