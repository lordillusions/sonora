// DEV-28b: the reference's Wavetable (InstrumentVector) imported as a native device.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/WavetableInstrument.h"
#include "devices/native/WavetableSprites.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace sonora;
using W = devices::WavetableInstrument;

namespace
{
juce::File wavetableFixture() { return juce::File(SONORA_TEST_FIXTURES).getChildFile("wavetable_set.xml"); }

const model::Device* findVector(const model::Song& song)
{
    for (const auto& t : song.tracks)
        for (const auto& d : t.devices)
            if (d.type == "InstrumentVector") return &d;
    return nullptr;
}

std::shared_ptr<const devices::WavetableData> sawTable()
{
    constexpr int n = devices::WavetableData::nativeFrameLength;
    std::vector<float> samples((size_t) n * 2);
    for (int i = 0; i < n; ++i)
    {
        samples[(size_t) i] = (float) std::sin(2 * M_PI * i / n);
        samples[(size_t) (n + i)] = 1.0f - 2.0f * (float) i / n;
    }
    return devices::WavetableData::fromSamples(samples, 1, n, 44100, "test").data;
}

std::unique_ptr<W> makeSynth()
{
    auto synth = std::make_unique<W>();
    synth->setTable(0, sawTable(), "Test", "Sine Saw");
    synth->setTable(1, sawTable(), "Test", "Sine Saw");
    synth->prepare(48000, 4096);
    return synth;
}

double rms(const juce::AudioBuffer<float>& a, int channel, int start, int length) { return a.getRMSLevel(channel, start, length); }

bool allFinite(const juce::AudioBuffer<float>& a)
{
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            if (! std::isfinite(a.getSample(ch, i))) return false;
    return true;
}
} // namespace

TEST_CASE("InstrumentVector: InstrumentVector parses every section, the matrix and the table references")
{
    auto loaded = io::loadAls(wavetableFixture());
    REQUIRE(loaded);
    const auto* d = findVector(*loaded.song);
    REQUIRE(d != nullptr);
    CHECK(d->displayName == "Fixture Pad");
    CHECK_NEAR(d->param("Voice_Oscillator1_Wavetables_WavePosition", -1), 0.25, 1e-9);
    CHECK_NEAR(d->param("Voice_Filter1_CircuitLpHp", -1), 2, 1e-9);
    CHECK_NEAR(d->plain.at("Voice_Unison_Mode"), 1, 0);
    CHECK_NEAR(d->plain.at("Voice_Unison_VoiceCount"), 3, 0);
    CHECK_NEAR(d->plain.at("Voice_Oscillator2_Effects_EffectMode"), 1, 0);
    CHECK_NEAR(d->plain.at("PolyVoices"), 2, 0);
    CHECK(d->plain.count("ViewData") == 0); // non-numeric leaves are not taken
    CHECK_NEAR(d->plain.at("ModulationConnections/ModulationConnectionsForInstrumentVector#1/ModulationAmounts.3"), 0.25, 1e-9);
    CHECK_NEAR(d->plain.at("ModulationConnections/ModulationConnectionsForInstrumentVector#49/ModulationAmounts.5"), 0.5, 1e-9);
    for (const auto& w : loaded.song->warnings) CHECK(w.find("InstrumentVector") == std::string::npos);

    std::vector<std::string> warnings;
    auto processor = devices::createNative(*d, &warnings);
    auto* synth = dynamic_cast<W*>(processor.get());
    REQUIRE(synth != nullptr);
    CHECK_NEAR(synth->getParameter(W::Osc2Transpose), -12, 1e-6);
    CHECK_NEAR(synth->getParameter(W::Osc2Detune), 0.05, 1e-6);
    CHECK_NEAR(synth->getParameter(W::UnisonVoices), 3, 1e-6);
    CHECK_NEAR(synth->getParameter(W::matrixParam(W::TOsc1Pos, W::SrcLfo1)), 0.25, 1e-6);
    CHECK_NEAR(synth->getParameter(W::matrixParam(W::TAmp, W::SrcVelocity)), 0.5, 1e-6);
    CHECK(synth->voiceLimit() == 4);
    const auto ref = devices::readWavetableSpriteRef(*d, 1);
    CHECK(ref.raw);
    CHECK(ref.userPath.rfind('?', 0) == 0); // the fixture's user table does not exist
    bool standIn = false;
    for (const auto& w : warnings) standIn |= w.find("user wavetable of oscillator 2") != std::string::npos;
    CHECK(standIn);
    CHECK(synth->table(0) != nullptr);
    CHECK(synth->table(1) != nullptr);
    CHECK(std::string(W::targetName(W::TOsc2Fx1, 0, 1)) == "Osc 2 FM Pitch");
    CHECK(std::string(W::targetName(W::TOsc1Fx2, 3, 0)) == "Osc 1 Fold");
}

TEST_CASE("InstrumentVector: saving keeps the set and writes matrix and enum edits to their own elements")
{
    auto loaded = io::loadAls(wavetableFixture());
    REQUIRE(loaded);
    std::string error;
    const auto original = io::readAlsDom(wavetableFixture(), error);
    REQUIRE(original != nullptr);
    juce::TemporaryFile same(".als");
    REQUIRE(io::saveAls(*loaded.song, same.getFile()).ok);
    const auto unchanged = io::readAlsDom(same.getFile(), error);
    REQUIRE(unchanged != nullptr);
    CHECK(original->isEquivalentTo(unchanged.get(), false));

    auto& device = loaded.song->tracks[0].devices[0];
    REQUIRE(device.type == "InstrumentVector");
    auto processor = devices::createNative(device, nullptr);
    REQUIRE(processor != nullptr);
    processor->setParameter(W::matrixParam(W::TF2Morph, W::SrcModWheel), -0.75f);
    processor->setParameter(W::UnisonMode, 5);
    processor->setParameter(W::Osc1Position, 0.9f);
    devices::captureDeviceState(device, processor.get());
    devices::setWavetableSpriteName(device, 0, "Harmonics 1");
    juce::TemporaryFile edited(".als");
    REQUIRE(io::saveAls(*loaded.song, edited.getFile()).ok);
    auto again = io::loadAls(edited.getFile());
    REQUIRE(again);
    const auto* d = findVector(*again.song);
    REQUIRE(d != nullptr);
    CHECK_NEAR(d->plain.at("ModulationConnections/ModulationConnectionsForInstrumentVector#21/ModulationAmounts.9"), -0.75, 1e-6);
    CHECK_NEAR(d->plain.at("Voice_Unison_Mode"), 5, 0);
    CHECK_NEAR(d->param("Voice_Oscillator1_Wavetables_WavePosition", -1), 0.9, 1e-6);
    CHECK(devices::readWavetableSpriteRef(*d, 0).spriteName == "Harmonics 1");
    const auto dom = io::readAlsDom(edited.getFile(), error);
    REQUIRE(dom != nullptr);
    int connections = 0;
    std::function<void(const juce::XmlElement&)> count = [&](const juce::XmlElement& e) {
        if (e.hasTagName("ModulationConnectionsForInstrumentVector")) ++connections;
        for (const auto* c : e.getChildIterator()) count(*c);
    };
    count(*dom);
    CHECK(connections == W::kTargets); // edits address existing connections, none are appended
}

TEST_CASE("InstrumentVector: notes sound, release ends voices, mono legato keeps one voice, output stays finite")
{
    auto synth = makeSynth();
    synth->setParameter(W::AmpRelease, 0.05f);
    juce::AudioBuffer<float> audio(2, 4096);
    audio.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 100);
    synth->process(audio, midi);
    CHECK_NEAR(audio.getMagnitude(0, 0, 100), 0, 0);
    CHECK(rms(audio, 0, 1000, 3000) > 0.01);
    CHECK(synth->activeVoices() == 1);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
    for (int i = 0; i < 4; ++i)
    {
        audio.clear();
        synth->process(audio, midi);
        midi.clear();
    }
    CHECK(synth->activeVoices() == 0);
    CHECK(audio.getMagnitude(0, 0, 4096) < 1e-4f);

    // Poly: voice limit, stealing and every modulator / effect / unison mode at once stay finite.
    synth->setParameter(W::PolyVoices, 1); // 3 voices
    for (int i = 0; i < 20; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, 40 + i, (juce::uint8) 127), i * 10);
    synth->setParameter(W::Osc2On, 1);
    synth->setParameter(W::Osc1EffectMode, 1);
    synth->setParameter(W::Osc2EffectMode, 2);
    synth->setParameter(W::Osc1Effect2, 1);
    synth->setParameter(W::Osc2Effect2, 1);
    synth->setParameter(W::UnisonMode, 3);
    synth->setParameter(W::UnisonVoices, 8);
    synth->setParameter(W::UnisonAmount, 1);
    synth->setParameter(W::F1Resonance, 1.25f);
    synth->setParameter(W::F1CircuitLpHp, 4);
    synth->setParameter(W::F1Drive, 24);
    synth->setParameter(W::F2On, 1);
    synth->setParameter(W::F2Type, 4);
    synth->setParameter(W::FilterRouting, 1);
    synth->setParameter(W::SubOn, 1);
    synth->setParameter(W::matrixParam(W::TF1Freq, W::SrcLfo1), 1);
    synth->setParameter(W::matrixParam(W::TOsc1Pitch, W::SrcEnv2), 1);
    audio.clear();
    synth->process(audio, midi);
    CHECK(synth->activeVoices() == 3);
    CHECK(allFinite(audio));
    CHECK(audio.getMagnitude(0, 4096) < 16.0f);

    // Mono: a legato note takes over the sounding voice.
    synth->reset();
    synth->setParameter(W::MonoPoly, 0);
    midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 100), 2000);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 3000);
    audio.clear();
    synth->process(audio, midi);
    CHECK(synth->activeVoices() == 1);
    CHECK(rms(audio, 0, 3500, 500) > 0.001);
}

TEST_CASE("InstrumentVector: output does not depend on the block size or MIDI split points")
{
    auto a = makeSynth(), b = makeSynth();
    for (auto* s : {a.get(), b.get()})
    {
        s->setParameter(W::UnisonMode, 2); // Shimmer: random drift on the control grid
        s->setParameter(W::UnisonAmount, 0.6f);
        s->setParameter(W::L1Retrigger, 0); // free-running LFO
        s->setParameter(W::matrixParam(W::TOsc1Pos, W::SrcLfo1), 0.5f);
        s->setParameter(W::Glide, 0.05f);
        s->setParameter(W::F1Frequency, 800);
        s->setParameter(W::F1Resonance, 0.7f);
    }
    juce::AudioBuffer<float> whole(2, 8192), split(2, 8192);
    whole.clear();
    split.clear();
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 90), 37);
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 70), 1500);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 12000), 2222);
    midi.addEvent(juce::MidiMessage::noteOff(1, 57), 5000);
    a->process(whole, midi);
    int start = 0;
    const int sizes[] = {1, 63, 128, 500, 7, 1024};
    for (int k = 0; start < 8192; ++k)
    {
        const int n = std::min(sizes[k % 6], 8192 - start);
        juce::AudioBuffer<float> view(split.getArrayOfWritePointers(), 2, start, n);
        juce::MidiBuffer part;
        for (const auto m : midi)
            if (m.samplePosition >= start && m.samplePosition < start + n) part.addEvent(m.data, m.numBytes, m.samplePosition - start);
        b->process(view, part);
        start += n;
    }
    float diff = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 8192; ++i) diff = std::max(diff, std::abs(whole.getSample(ch, i) - split.getSample(ch, i)));
    CHECK(diff < 1e-6f);
    CHECK(rms(whole, 0, 0, 8192) > 0.01);
}

TEST_CASE("InstrumentVector: matrix routes move their targets (LFO to position, velocity to Amp)")
{
    auto synth = makeSynth();
    synth->setParameter(W::Osc1Position, 0.5f);
    synth->setParameter(W::L1Rate, 10);
    synth->setParameter(W::matrixParam(W::TOsc1Pos, W::SrcLfo1), 0.4f);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127), 0);
    float low = 1, high = 0;
    for (int block = 0; block < 20; ++block)
    {
        audio.clear();
        synth->process(audio, midi);
        midi.clear();
        low = std::min(low, synth->displayPosition(0));
        high = std::max(high, synth->displayPosition(0));
    }
    CHECK(low < 0.2f);
    CHECK(high > 0.8f);

    // Velocity -> Amp 50 %: velocity 127 plays at full level, velocity 64 at 1 - 0.5 + 0.5 x 64 / 127.
    const auto level = [](int velocity) {
        auto s = makeSynth();
        s->setParameter(W::matrixParam(W::TAmp, W::SrcVelocity), 0.5f);
        juce::AudioBuffer<float> a(2, 8192);
        a.clear();
        juce::MidiBuffer m;
        m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) velocity), 0);
        s->process(a, m);
        return rms(a, 0, 4096, 4096);
    };
    CHECK_NEAR(level(64) / level(127), 1 - 0.5 + 0.5 * 64.0 / 127.0, 0.01);
}

TEST_CASE("InstrumentVector: filters follow the measured circuit laws, envelopes the measured slope curve")
{
    auto synth = makeSynth();
    synth->setParameter(W::F1Slope, 0);
    devices::BiquadCoeffs stages[2];
    // Clean, r = 0: d = 1, Q = 0.5 -> -6 dB at the cutoff, one stage at 12 dB.
    CHECK(synth->filterStages(0, 1000, 0, 0, stages) == 1);
    CHECK_NEAR(20 * std::log10(stages[0].magnitudeAt(1000, 48000)), -6.02, 0.05);
    synth->setParameter(W::F1Slope, 1);
    CHECK(synth->filterStages(0, 1000, 0.5, 0, stages) == 2);
    synth->setParameter(W::F1CircuitLpHp, 4); // PRD ladder
    CHECK(synth->filterStages(0, 1000, 0.5, 0, stages) == 2);
    CHECK(stages[0].magnitudeAt(100, 48000) * stages[1].magnitudeAt(100, 48000) > 0.9);
    // The capture's Amp envelope: attack slope 0.21 covers ~0.6 at its midpoint, decay slope 0.5 covers 0.75.
    CHECK_NEAR(W::slopeCurve(0.5f, 0.2135f), 0.603, 0.01);
    CHECK_NEAR(W::slopeCurve(0.5f, 0.5f), 0.745, 0.01);
    CHECK_NEAR(W::slopeCurve(0.5f, 0.0f), 0.5, 1e-6);
    CHECK_NEAR(W::slopeCurve(0.5f, -0.5f), 1 - 0.745, 0.01);
}

TEST_CASE("InstrumentVector: table files load as whole 2048-sample frames; factory tables resolve when installed")
{
    juce::TemporaryFile file(".wav");
    {
        juce::WavAudioFormat wav;
        auto stream = file.getFile().createOutputStream();
        REQUIRE(stream != nullptr);
        std::unique_ptr<juce::OutputStream> out(std::move(stream));
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> b(1, 2048 * 3 + 700);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, (float) std::sin(2 * M_PI * i / 2048.0) * 0.5f);
        writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    std::string error;
    const auto table = devices::loadWavetableFile(file.getFile().getFullPathName().toStdString(), error);
    REQUIRE(table != nullptr);
    CHECK(table->frames() == 3);
    CHECK(devices::standInWavetable()->frames() == 16);
    if (! devices::factoryWavetableFolder().empty())
    {
        const auto* sprite = devices::findFactoryWavetable("Harmonics 1");
        REQUIRE(sprite != nullptr);
        CHECK(sprite->category == "Retro");
        const auto factory = devices::loadWavetableFile(sprite->file, error);
        REQUIRE(factory != nullptr);
        CHECK(factory->frames() > 1);
    }
}

TEST_CASE("InstrumentVector: a fresh Wavetable from the browser saves, reloads and keeps the reference's default patch")
{
    auto loaded = io::loadAls(wavetableFixture());
    REQUIRE(loaded);
    auto device = devices::nativeDeviceModel("InstrumentVector");
    CHECK(device.displayName == "Wavetable");
    device.id = 77;
    device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device)); // as the browser / control insert does
    loaded.song->tracks[0].devices.push_back(device);
    juce::TemporaryFile out(".als");
    const auto saved = io::saveAls(*loaded.song, out.getFile());
    if (! saved.ok) std::printf("    save error: %s\n", saved.error.c_str());
    REQUIRE(saved.ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].devices.size() == 2);
    const auto& d = again.song->tracks[0].devices[1];
    CHECK(d.type == "InstrumentVector");
    CHECK_NEAR(d.plain.at("ModulationConnections/ModulationConnectionsForInstrumentVector#49/ModulationAmounts.5"), 0.5, 1e-6);
    auto processor = devices::createNative(d, nullptr);
    REQUIRE(processor != nullptr);
    CHECK_NEAR(processor->getParameter(W::Volume), 0.3548134, 1e-5);
    CHECK(devices::readWavetableSpriteRef(d, 0).spriteName == "Basic Shapes");
    std::string error;
    const auto dom = io::readAlsDom(out.getFile(), error);
    REQUIRE(dom != nullptr);
    int connections = 0, withIds = 0;
    std::function<void(const juce::XmlElement&)> count = [&](const juce::XmlElement& e) {
        if (e.hasTagName("ModulationConnectionsForInstrumentVector")) { ++connections; withIds += e.hasAttribute("TargetId") ? 1 : 0; }
        for (const auto* c : e.getChildIterator()) count(*c);
    };
    count(*dom);
    CHECK(connections == 2 * W::kTargets); // the fixture's device and the new one
    CHECK(withIds == 2 * W::kTargets);
}
