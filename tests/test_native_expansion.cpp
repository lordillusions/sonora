#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "core/Edit.h"
#include "devices/native/Redux.h"
#include "devices/native/ClassicEffects.h"
#include "devices/native/Tuner.h"
#include "devices/native/Simpler.h"
#include "devices/native/MidiNoteEffects.h"
#include "engine/EngineGraph.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include <numbers>
using namespace sonora;
namespace
{
juce::AudioBuffer<float> tone(double hz, double rate, int count)
{
    juce::AudioBuffer<float> audio(2, count);
    for (int i = 0; i < count; ++i) for (int ch = 0; ch < 2; ++ch)
        audio.setSample(ch, i, 0.7f * (float) std::sin(2 * std::numbers::pi * hz * i / rate));
    return audio;
}
void process(devices::DeviceProcessor& processor, juce::AudioBuffer<float>& audio, int block)
{
    juce::MidiBuffer midi;
    for (int i = 0; i < audio.getNumSamples(); i += block)
    {
        juce::AudioBuffer<float> part(audio.getArrayOfWritePointers(), 2, i, std::min(block, audio.getNumSamples() - i));
        processor.process(part, midi);
    }
}
}
TEST_CASE("native expansion: Redux bit depth, dry wet, bypass, reset and block invariance")
{
    using R = devices::Redux;
    // Measured quantiser (DEV-22b): step 2^(1 - Bits); a 1-bit quantiser silences a +-0.5 sine; DC Shift = mid-rise
    // levels with zero on the lower level; Shape 1 = mu-law 255 levels.
    CHECK_NEAR(R::quantize(0.3f, 4, 0, false), 0.25, 1e-6);
    CHECK_NEAR(R::quantize(0.49f, 1, 0, false), 0.0, 1e-6);
    CHECK_NEAR(R::quantize(0.0f, 4, 0, true), -0.0625, 1e-6);
    CHECK_NEAR(R::quantize(0.001f, 4, 0, true), 0.0625, 1e-6);
    CHECK_NEAR(R::quantize(0.4f, 4, 1, false), 127.0 / 255.0, 1e-4);
    R redux; redux.prepare(48000, 512); redux.setParameter(R::Bits, 2); redux.setParameter(R::Rate, 12000);
    CHECK(redux.latencySamples() == 64);
    auto audio = tone(3100, 48000, 4096); process(redux, audio, 512);
    double peak = 0;
    for (int i = 0; i < 4096; ++i) peak = std::max(peak, (double) std::abs(audio.getSample(0, i)));
    CHECK(peak > 0.2 && peak < 1.5);
    for (bool bypass : {false, true})
    {
        R fresh; fresh.prepare(48000, 512);
        fresh.setBypassed(bypass); fresh.setParameter(R::DryWet, 0);
        auto dry = tone(440, 48000, 400); const auto original = dry; process(fresh, dry, 57);
        const int shift = bypass ? 0 : fresh.latencySamples(); // dry path is delayed by the reported latency
        for (int i = shift; i < 400; ++i) CHECK_NEAR(dry.getSample(0, i), original.getSample(0, i - shift), 0);
    }
    R a, b; a.prepare(48000, 512); b.prepare(48000, 512);
    for (auto* effect : {&a, &b})
    {
        effect->setParameter(R::Rate, 1721); effect->setParameter(R::Jitter, 0.8f); effect->setParameter(R::Bits, 3.5f);
        effect->setParameter(R::PreFilter, 1); effect->setParameter(R::PostFilter, 1); effect->setParameter(R::Shape, 0.8f); effect->setParameter(R::Dc, 1);
    }
    auto whole = tone(3000, 48000, 4096), split = whole;
    process(a, whole, 4096); process(b, split, 127);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4096; ++i) CHECK_NEAR(whole.getSample(ch, i), split.getSample(ch, i), 1e-6);
    a.reset(); auto silence = tone(0, 48000, 512); process(a, silence, 512);
    for (int i = 0; i < 512; ++i) CHECK(std::isfinite(silence.getSample(0, i)));
}
TEST_CASE("native expansion: Tuner detects pitch at several sample rates and leaves audio unchanged")
{
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (double hz : {55.0, 110.0, 440.0, 1000.0})
        {
            devices::Tuner tuner; tuner.prepare(rate, 512);
            auto audio = tone(hz, rate, (int) rate / 2); const auto original = audio;
            process(tuner, audio, 333);
            CHECK_NEAR(1200 * std::log2(tuner.frequency() / hz), 0, 3);
            CHECK(tuner.confidence() > 0.9f);
            for (int i = 0; i < audio.getNumSamples(); ++i) CHECK_NEAR(audio.getSample(0, i), original.getSample(0, i), 0);
            audio.clear(); process(tuner, audio, 512); CHECK_NEAR(tuner.frequency(), 0, 0);
        }
}
TEST_CASE("native expansion: new devices retain parameters and plain settings through capture save and reload")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-native-expansion"); REQUIRE(dir.createDirectory().wasOk());
    io::NewSetOptions options; options.audioTracks = 1; options.midiTracks = options.returnTracks = 0;
    const auto input = dir.getChildFile("Source.als"), output = dir.getChildFile("Devices.als");
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), input, error));
    auto loaded = io::loadAls(input); REQUIRE(loaded); auto& song = *loaded.song;
    for (const auto* type : {"Redux2", "Tuner", "OriginalSimpler", "MidiPitcher", "MidiChord", "MidiScale", "MidiRandom", "MidiVelocity", "Gate", "AutoPan"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build); REQUIRE(processor);
        for (size_t i = 0; i < processor->parameters().size(); ++i)
        {
            const auto& info = processor->parameters()[i];
            float value = info.minValue + (info.maxValue - info.minValue) * 0.75f;
            if (info.isToggle || ! info.choices.empty()) value = std::round(value);
            processor->setParameter((int) i, value);
        }
        devices::captureDeviceState(model, processor.get()); model::allocateDeviceIds(song, model);
        model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model)); song.tracks[0].devices.push_back(model);
    }
    REQUIRE(io::saveAls(song, output, {}).ok); auto reopened = io::loadAls(output); REQUIRE(reopened);
    devices::BuildContext build;
    for (size_t d = 0; d < song.tracks[0].devices.size(); ++d)
    {
        const auto& before = song.tracks[0].devices[d]; const auto& after = reopened.song->tracks[0].devices[d];
        auto p = devices::createDevice(before, build), q = devices::createDevice(after, build); REQUIRE(p); REQUIRE(q);
        for (size_t i = 0; i < p->parameters().size(); ++i) CHECK_NEAR(p->getParameter((int) i), q->getParameter((int) i), 1e-5);
        for (const auto& info : q->parameters()) if (info.plainValue) CHECK(after.plain.contains(info.id));
        for (size_t i = 0; i < q->parameters().size(); ++i) q->setParameter((int) i, q->parameters()[i].minValue);
        devices::captureDeviceState(reopened.song->tracks[0].devices[d], q.get());
    }
    const auto updated = dir.getChildFile("Updated.als"); REQUIRE(io::saveAls(*reopened.song, updated, {}).ok);
    auto edited = io::loadAls(updated); REQUIRE(edited);
    for (const auto& model : edited.song->tracks[0].devices)
    {
        auto processor = devices::createDevice(model, build); REQUIRE(processor);
        for (size_t i = 0; i < processor->parameters().size(); ++i) CHECK_NEAR(processor->getParameter((int) i), processor->parameters()[i].minValue, 1e-5);
    }
}
TEST_CASE("native expansion: MIDI Pitch and Chord release original outputs after parameter changes on distinct channels")
{
    using M = devices::MidiNoteEffects;
    for (auto kind : {M::Kind::Pitch, M::Kind::Chord, M::Kind::Scale, M::Kind::Random})
    {
        M effect(kind); effect.prepare(48000, 512);
        if (kind == M::Kind::Pitch) effect.setParameter(0, 12);
        if (kind == M::Kind::Chord) { effect.setParameter(0, 4); effect.setParameter(3, 7); }
        if (kind == M::Kind::Random) effect.setParameter(0, 1);
        juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer events; events.ensureSize(147456);
        events.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), 17);
        effect.process(audio, events); std::vector<int> outputs;
        for (const auto event : events) { CHECK(event.samplePosition == 17); auto message = event.getMessage(); CHECK(message.getChannel() == 2); outputs.push_back(message.getNoteNumber()); }
        REQUIRE(! outputs.empty()); CHECK(outputs.size() == (kind == M::Kind::Chord ? 3u : 1u));
        effect.setParameter(0, 0); events.clear(); events.addEvent(juce::MidiMessage::noteOff(2, 60), 37);
        effect.process(audio, events); size_t i = 0;
        for (const auto event : events) { CHECK(event.samplePosition == 37); CHECK(event.getMessage().isNoteOff()); CHECK(event.getMessage().getChannel() == 2); CHECK(event.getMessage().getNoteNumber() == outputs[i++]); }
        CHECK(i == outputs.size());
        events.clear(); events.addEvent(juce::MidiMessage::pitchWheel(9, 12345), 25); effect.process(audio, events);
        CHECK(events.getNumEvents() == 1); CHECK((*events.begin()).getMessage().getChannel() == 9); CHECK((*events.begin()).getMessage().getPitchWheelValue() == 12345);
    }
}
TEST_CASE("native expansion: MIDI velocity, scale mapping and bounded chord overflow")
{
    using M = devices::MidiNoteEffects; juce::AudioBuffer<float> audio(2, 512); audio.clear();
    juce::MidiBuffer midi; midi.ensureSize(147456);
    M velocity(M::Kind::Velocity); velocity.prepare(48000, 512); velocity.setParameter(3, 2); velocity.setParameter(5, 77);
    midi.addEvent(juce::MidiMessage::noteOn(3, 60, (juce::uint8) 15), 1); velocity.process(audio, midi);
    CHECK((*midi.begin()).getMessage().getVelocity() == 77);
    M scale(M::Kind::Scale); scale.prepare(48000, 512); scale.setParameter(5 + 1, 0); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(3, 61, (juce::uint8) 100), 0); scale.process(audio, midi);
    CHECK((*midi.begin()).getMessage().getNoteNumber() == 60);
    M chord(M::Kind::Chord); chord.prepare(48000, 512);
    for (int i = 0; i < 6; ++i) chord.setParameter(i * 3, (float) (i + 1));
    midi.clear(); for (int i = 0; i < 2048; ++i) midi.addEvent(juce::MidiMessage::noteOn(i % 16 + 1, i % 80 + 20, (juce::uint8) 100), i / 8);
    chord.process(audio, midi); CHECK(midi.getNumEvents() == 16);
    for (const auto event : midi) CHECK(event.getMessage().isAllSoundOff());
}
TEST_CASE("native expansion: Simpler onset slices, voice limit, retrigger and LFO are audible")
{
    using S = devices::Simpler;
    auto data = std::make_shared<engine::SampleData>(); data->sampleRate = 48000;
    data->audio.setSize(2, 4800); data->onsets = {0, 0.05};
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4800; ++i) data->audio.setSample(ch, i, i < 2400 ? 0.5f : -0.5f);
    model::SamplePart part; part.sampleEnd = part.lengthSamples = 4800;
    std::vector<S::Zone> zones{{part, engine::SampleSlot::makeReady(data)}};
    for (int slice = 0; slice < 2; ++slice)
    {
        S sampler(zones, S::Mode::Slice, 5); sampler.prepare(48000, 512); sampler.setParameter(S::Volume, 0); sampler.setParameter(S::VelToVolume, 0);
        sampler.setParameter(S::FilterOn, 0); sampler.setParameter(S::Snap, 0);
        auto audio = tone(0, 48000, 512); juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 36 + slice, (juce::uint8) 100), 0);
        sampler.process(audio, midi); CHECK(slice == 0 ? audio.getSample(0, 100) > 0.4f : audio.getSample(0, 100) < -0.4f);
    }
    S sampler(zones, S::Mode::Classic, 5); sampler.prepare(48000, 512); sampler.setParameter(S::Voices, 0);
    auto audio = tone(0, 48000, 512); juce::MidiBuffer midi;
    for (int i = 0; i < 12; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, 60 + i, (juce::uint8) 100), 0);
    sampler.process(audio, midi); CHECK(sampler.activeVoices() == 1);
    sampler.reset(); sampler.setParameter(S::Voices, 6); sampler.setParameter(S::Retrigger, 1); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 100);
    audio.clear(); sampler.process(audio, midi); CHECK(sampler.activeVoices() == 1);
    S a(zones, S::Mode::Classic, 5), b(zones, S::Mode::Classic, 5); a.prepare(48000, 512); b.prepare(48000, 512);
    b.setParameter(S::LfoOn, 1); b.setParameter(S::LfoVolume, 1); b.setParameter(S::LfoRate, 20);
    auto plain = tone(0, 48000, 2000), modulated = plain; midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    a.process(plain, midi); b.process(modulated, midi); CHECK(std::abs(plain.getSample(0, 1800) - modulated.getSample(0, 1800)) > 0.1f);
}

TEST_CASE("native expansion: Gate closes below threshold, opens from external input and implements lookahead")
{
    using G = devices::Gate;
    G gate; gate.setParameter(G::Threshold, 0.2f); gate.setParameter(G::Attack, 0.01f); gate.setParameter(G::Release, 0.1f); gate.setParameter(G::Hold, 0);
    gate.setParameter(G::Floor, -75); // floor in dB; -75 == -inf
    gate.prepare(48000, 512); juce::MidiBuffer midi;
    juce::AudioBuffer<float> audio(2, 1024); audio.clear(); audio.applyGain(1);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 1024; ++i) audio.setSample(ch, i, 0.1f);
    gate.process(audio, midi); CHECK(audio.getMagnitude(0, 1024) < 1e-6f);
    juce::AudioBuffer<float> sidechain(2, 1024); for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 1024; ++i) { audio.setSample(ch, i, 0.1f); sidechain.setSample(ch, i, 0.7f); }
    gate.setSidechain(&sidechain, 1); gate.process(audio, midi); CHECK_NEAR(audio.getSample(0, 1000), 0.1, 1e-5);
    gate.setSidechain(nullptr, 1); gate.setParameter(G::Lookahead, 2); gate.prepare(48000, 512); CHECK(gate.latencySamples() == 480);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 1024; ++i) audio.setSample(ch, i, 0.5f);
    gate.process(audio, midi); CHECK_NEAR(audio.getMagnitude(0, 480), 0, 0); CHECK_NEAR(audio.getSample(0, 481), 0.5, 1e-4);
    gate.setParameter(G::Flip, 1); gate.reset(); gate.setParameter(G::Lookahead, 0); gate.prepare(48000, 512);
    gate.process(audio, midi); CHECK(std::isfinite(gate.reduction()));
}
TEST_CASE("native expansion: Auto Pan preserves zero amount and opposite stereo phase and is block invariant")
{
    using A = devices::AutoPan;
    A a, b; a.prepare(48000, 512); b.prepare(48000, 512);
    for (auto* effect : {&a, &b}) { effect->setParameter(A::Amount, 1); effect->setParameter(A::Phase, 180); effect->setParameter(A::Rate, 3); }
    juce::AudioBuffer<float> whole(2, 4800);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4800; ++i) whole.setSample(ch, i, 1);
    auto split = whole; process(a, whole, 4800); process(b, split, 127);
    for (int i = 0; i < 4800; ++i)
    {
        CHECK_NEAR(whole.getSample(0, i) + whole.getSample(1, i), 1, 1e-5);
        CHECK_NEAR(whole.getSample(0, i), split.getSample(0, i), 1e-6);
    }
    a.setParameter(A::Amount, 0); auto dry = tone(133, 48000, 500); const auto original = dry; process(a, dry, 128);
    for (int i = 0; i < 500; ++i) CHECK_NEAR(dry.getSample(0, i), original.getSample(0, i), 0);
    a.setParameter(A::Amount, 1); a.setParameter(A::Sync, 1); a.setHostInfo({180, 0, true, 48000});
    for (int shape = 0; shape < 6; ++shape) { a.setParameter(A::Shape, (float) shape); process(a, dry, 100); for (int i = 0; i < 500; ++i) CHECK(std::isfinite(dry.getSample(0, i))); }
}

TEST_CASE("native expansion: Pitch range modes, Scale octave fold, Random alternate and pressure use the transformed notes")
{
    using M = devices::MidiNoteEffects; juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer midi; midi.ensureSize(147456);
    M pitch(M::Kind::Pitch); pitch.prepare(48000, 512); pitch.setParameter(0, 12); pitch.setParameter(1, 60); pitch.setParameter(2, 12);
    for (int mode = 0; mode < 3; ++mode)
    {
        pitch.reset(); pitch.setParameter(3, (float) mode); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 61, (juce::uint8) 100), 0); pitch.process(audio, midi);
        if (mode == 0) CHECK(midi.isEmpty());
        else { REQUIRE(! midi.isEmpty()); CHECK((*midi.begin()).getMessage().getNoteNumber() == (mode == 1 ? 61 : 72)); }
    }
    M scale(M::Kind::Scale); scale.prepare(48000, 512); scale.setParameter(5, 9); scale.setParameter(2, 1); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0); scale.process(audio, midi);
    REQUIRE(! midi.isEmpty()); CHECK((*midi.begin()).getMessage().getNoteNumber() == 57);
    midi.clear(); midi.addEvent(juce::MidiMessage::aftertouchChange(1, 60, 80), 23); scale.process(audio, midi);
    CHECK((*midi.begin()).getMessage().getNoteNumber() == 57); CHECK((*midi.begin()).samplePosition == 23);
    M random(M::Kind::Random); random.prepare(48000, 512); random.setParameter(0, 1); random.setParameter(1, 2); random.setParameter(2, 2); random.setParameter(4, 1);
    for (int i = 0; i < 4; ++i)
    {
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0); random.process(audio, midi);
        int output = -1; for (const auto event : midi) if (event.getMessage().isNoteOn()) output = event.getMessage().getNoteNumber();
        CHECK(output == 60 + i % 2 * 2);
    }
    M velocity(M::Kind::Velocity); velocity.prepare(48000, 512); velocity.setParameter(5, 100); velocity.setParameter(6, 80); velocity.setParameter(2, 60);
    for (int i = 1; i < 128; ++i)
    {
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) i), 0); velocity.process(audio, midi);
        for (const auto event : midi) if (event.getMessage().isNoteOn()) { CHECK(event.getMessage().getVelocity() >= 80); CHECK(event.getMessage().getVelocity() <= 100); }
    }
}
TEST_CASE("native expansion: Simpler loop crossfade smooths its wrap and synced LFO follows host tempo")
{
    using S = devices::Simpler;
    auto data = std::make_shared<engine::SampleData>(); data->sampleRate = 48000; data->audio.setSize(2, 4800);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 4800; ++i) data->audio.setSample(ch, i, i < 2400 ? 0.5f : -0.5f);
    model::SamplePart part; part.sampleEnd = part.lengthSamples = part.loopEnd = 4800; part.loopMode = 1;
    auto slot = engine::SampleSlot::makeReady(data);
    S dry({{part, slot}}, S::Mode::Classic, 5, true); part.loopCrossfade = 600; // Sampler zones: their own sustain loop
    S faded({{part, slot}}, S::Mode::Classic, 5, true); dry.prepare(48000, 512); faded.prepare(48000, 512);
    for (auto* sampler : {&dry, &faded}) { sampler->setParameter(S::Volume, 0); sampler->setParameter(S::FilterOn, 0); sampler->setParameter(S::LfoOn, 0); }
    auto a = tone(0, 48000, 6000), b = a; juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    dry.process(a, midi); faded.process(b, midi);
    CHECK(std::abs(a.getSample(0, 4800) - a.getSample(0, 4799)) > 0.9f);
    CHECK(std::abs(b.getSample(0, 4800) - b.getSample(0, 4799)) < 0.01f);
    for (auto* sampler : {&dry, &faded}) { sampler->reset(); sampler->setParameter(S::LfoOn, 1); sampler->setParameter(S::LfoVolume, 1); sampler->setParameter(S::LfoSync, 1); sampler->setParameter(S::LfoBeat, 4); }
    dry.setHostInfo({60, 0, true, 48000}); faded.setHostInfo({120, 0, true, 48000}); a.clear(); b.clear(); dry.process(a, midi); faded.process(b, midi);
    CHECK(std::abs(a.getSample(0, 1500) - b.getSample(0, 1500)) > 0.05f);
}

TEST_CASE("native expansion: malformed notes panic safely without growing a prepared MIDI output buffer")
{
    devices::MidiNoteEffects chord(devices::MidiNoteEffects::Kind::Chord); chord.prepare(48000, 512);
    juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer midi; midi.ensureSize(147456);
    const auto* storage = midi.data.getRawDataPointer();
    const uint8_t malformed[]{0x90, 200, 100}; midi.addEvent(malformed, 3, 0); chord.process(audio, midi);
    CHECK(midi.data.getRawDataPointer() == storage); CHECK(midi.getNumEvents() == 16);
    for (const auto event : midi) CHECK(event.getMessage().isAllSoundOff());
}
