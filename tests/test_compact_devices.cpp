#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/VectorSynth.h"
#include "devices/native/Impulse.h"
#include "devices/native/Echo.h"
#include "core/Edit.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

using namespace sonora;
namespace
{
juce::AudioBuffer<float> emptyAudio(int length)
{
    juce::AudioBuffer<float> result(2, length); result.clear(); return result;
}
std::shared_ptr<engine::SampleSlot> testSample()
{
    auto data = std::make_shared<engine::SampleData>();
    data->sampleRate = 48000; data->audio.setSize(2, 4800);
    for (int i = 0; i < 4800; ++i)
        for (int ch = 0; ch < 2; ++ch) data->audio.setSample(ch, i, 0.5f * (float) std::cos(2 * dsp::kPi * 1000 * i / 48000));
    return engine::SampleSlot::makeReady(data);
}
}
TEST_CASE("compact vector: sample offsets, position changes, channel sustain and finite polyphony")
{
    using V = devices::VectorSynth;
    V synth; synth.prepare(48000, 4096);
    auto audio = emptyAudio(4096); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), 128);
    synth.process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(0, 128), 0, 0); CHECK(audio.getRMSLevel(0, 128, 3968) > 0.01f);
    synth.setParameter(V::Release, 1);
    midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(2, 60), 1);
    audio.clear(); synth.process(audio, midi); CHECK(audio.getRMSLevel(0, 2048, 2048) > 0.005f);
    midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 0);
    audio.clear(); synth.process(audio, midi); CHECK(audio.getRMSLevel(0, 2048, 2048) > 0.005f);
    midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(2, 64, 0), 0);
    audio.clear(); synth.process(audio, midi); CHECK_NEAR(audio.getMagnitude(256, 3840), 0, 0);
    synth.reset(); midi.clear();
    for (int i = 0; i < 100; ++i) midi.addEvent(juce::MidiMessage::noteOn(i % 16 + 1, i + 20, (juce::uint8) 127), i);
    synth.setParameter(V::LfoAmount, 1); synth.setParameter(V::Resonance, 1); synth.setParameter(V::Position1, 1);
    audio.clear(); synth.process(audio, midi);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < audio.getNumSamples(); ++i) REQUIRE(std::isfinite(audio.getSample(ch, i)));
}
TEST_CASE("compact vector: MIDI splits and render block size preserve the output")
{
    devices::VectorSynth a, b; a.prepare(48000, 2048); b.prepare(48000, 2048);
    a.setParameter(devices::VectorSynth::LfoAmount, 0.3f); b.setParameter(devices::VectorSynth::LfoAmount, 0.3f);
    auto whole = emptyAudio(2048), split = emptyAudio(2048);
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 33);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 10240), 700); midi.addEvent(juce::MidiMessage::noteOff(1, 67), 1021);
    a.process(whole, midi);
    for (int start = 0; start < 2048; start += 128)
    {
        juce::AudioBuffer<float> view(split.getArrayOfWritePointers(), 2, start, 128);
        juce::MidiBuffer events; events.addEvents(midi, start, 128, -start); b.process(view, events);
    }
    for (int i = 0; i < 2048; ++i) CHECK_NEAR(whole.getSample(0, i), split.getSample(0, i), 1e-6);
}
TEST_CASE("compact impulse: eight independent slots, sparse slots, exact triggering and channel panic")
{
    using I = devices::Impulse;
    std::vector<devices::Simpler::Zone> zones;
    for (int i : {0, 7})
    {
        devices::Simpler::Zone z; z.part.rootKey = I::notes[(size_t) i]; z.part.sampleEnd = 4800; z.slot = testSample(); zones.push_back(z);
    }
    I instrument(zones); instrument.prepare(48000, 1024);
    instrument.setParameter(I::slotParam(7, I::Pan), 1);
    auto audio = emptyAudio(1024); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(3, I::notes[7], (juce::uint8) 127), 100);
    instrument.process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(0, 0, 1024), 0, 0);
    CHECK_NEAR(audio.getMagnitude(1, 0, 100), 0, 0);
    CHECK(audio.getMagnitude(1, 100, 924) > 0.1f); CHECK(instrument.activePads() == 128u);
    midi.clear(); midi.addEvent(juce::MidiMessage::allSoundOff(2), 0); audio.clear(); instrument.process(audio, midi);
    CHECK(audio.getMagnitude(1, 0, 1024) > 0.01f);
    midi.clear(); midi.addEvent(juce::MidiMessage::allSoundOff(3), 0); audio.clear(); instrument.process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(0, 1024), 0, 0); CHECK(instrument.activePads() == 0);
    instrument.audition(0); midi.clear(); audio.clear(); instrument.process(audio, midi); CHECK(instrument.activePads() == 1);
    instrument.reset(); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, I::notes[1], (juce::uint8) 127), 0);
    audio.clear(); instrument.process(audio, midi); CHECK_NEAR(audio.getMagnitude(0, 1024), 0, 0);
}
TEST_CASE("compact echo: free and tempo timing, ping pong routing, feedback polarity, dry bypass and reset")
{
    using E = devices::Echo;
    for (int mode = 0; mode < 3; ++mode)
    {
        E effect; effect.setParameter(E::SyncL, 0); effect.setParameter(E::SyncR, 0);
        effect.setParameter(E::TimeL, 10); effect.setParameter(E::TimeR, 10); effect.setParameter(E::Mode, (float) mode);
        effect.setParameter(E::Feedback, 0.5f); effect.setParameter(E::FilterOn, 0); effect.setParameter(E::DryWet, 1);
        effect.prepare(48000, 2048);
        auto audio = emptyAudio(2048); audio.setSample(0, 0, 1); juce::MidiBuffer midi; effect.process(audio, midi);
        CHECK_NEAR(audio.getMagnitude(0, 480), 0, 0);
        CHECK_NEAR(audio.getSample(0, 480), mode == 1 ? 0.5f : 1, 1e-6);
        CHECK_NEAR(audio.getSample(1, 480), 0, 1e-6);
        if (mode == 1) CHECK_NEAR(audio.getSample(1, 960), 0.25, 1e-6);
        else CHECK_NEAR(audio.getSample(0, 960), 0.5, 1e-6);
        effect.reset(); effect.setParameter(E::Invert, 1); audio.clear(); audio.setSample(0, 0, 1); effect.process(audio, midi);
        CHECK_NEAR(audio.getSample(mode == 1 ? 1 : 0, 960), mode == 1 ? -0.25 : -0.5, 1e-6);
        effect.reset(); audio.clear(); effect.process(audio, midi); CHECK_NEAR(audio.getMagnitude(0, 2048), 0, 0);
        effect.setBypassed(true); audio.setSample(0, 0, 0.75f); effect.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.75, 0);
    }
    E tempo; devices::HostInfo info; info.tempo = 120; tempo.setHostInfo(info);
    CHECK_NEAR(tempo.delayMilliseconds(0), 250, 1e-9);
    info.tempo = 60; tempo.setHostInfo(info); CHECK_NEAR(tempo.delayMilliseconds(0), 500, 1e-9);
}
TEST_CASE("compact echo: block invariance and bounded long feedback at extreme controls")
{
    using E = devices::Echo;
    E a, b;
    for (auto* e : {&a, &b})
    {
        e->setParameter(E::SyncL, 0); e->setParameter(E::SyncR, 0); e->setParameter(E::TimeL, 2); e->setParameter(E::TimeR, 3);
        e->setParameter(E::Feedback, 0.98f); e->setParameter(E::ModAmount, 1); e->setParameter(E::Input, 24); e->prepare(48000, 8192);
    }
    auto whole = emptyAudio(8192), split = emptyAudio(8192); whole.setSample(0, 0, 1); split.setSample(0, 0, 1); juce::MidiBuffer midi;
    a.process(whole, midi);
    for (int start = 0; start < 8192; start += 64)
    { juce::AudioBuffer<float> view(split.getArrayOfWritePointers(), 2, start, 64); b.process(view, midi); }
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 8192; ++i)
    { REQUIRE(std::isfinite(whole.getSample(ch, i))); CHECK_NEAR(whole.getSample(ch, i), split.getSample(ch, i), 1e-6); }
}
TEST_CASE("compact devices: new instruments and effect retain parameters and sparse sample slots through ALS save")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-compact-devices");
    REQUIRE(dir.createDirectory().wasOk()); const auto destination = dir.getChildFile("Compact.als");
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(), destination, error));
    auto result = io::loadAls(destination); REQUIRE(result); auto& song = *result.song;
    model::EditHistory history;
    for (const auto* type : {"SonoraVector", "SonoraImpulse", "SonoraEcho"})
    {
        auto model = devices::nativeDeviceModel(type); model::allocateDeviceIds(song, model);
        if (model.type == "SonoraImpulse")
        {
            model::SamplePart sample; sample.rootKey = sample.keyMin = sample.keyMax = devices::Impulse::notes[7];
            sample.name = "Last slot"; sample.sample.absolutePath = "/tmp/own-sample.wav"; sample.sampleEnd = 4800;
            model.sampleParts.push_back(sample); model.samplePartsEdited = true;
        }
        model.parameters[0].value = model.type == "SonoraVector" ? 0.82 : -7;
        model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
        history.perform(song, model::insertDevice(0, std::move(model)));
    }
    REQUIRE(io::saveAls(song, destination, {}).ok); auto loaded = io::loadAls(destination); REQUIRE(loaded);
    REQUIRE(loaded.song->tracks[0].devices.size() == 3);
    devices::BuildContext context;
    for (const auto& model : loaded.song->tracks[0].devices)
    {
        auto processor = devices::createDevice(model, context); REQUIRE(processor);
        CHECK(processor->name().find("placeholder") == std::string::npos);
        CHECK_NEAR(processor->getParameter(0), model.type == "SonoraVector" ? 0.82 : -7, 1e-6);
        if (model.type == "SonoraImpulse") { REQUIRE(model.sampleParts.size() == 1); CHECK(model.sampleParts[0].rootKey == 72); }
    }
}
