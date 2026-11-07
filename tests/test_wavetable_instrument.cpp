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
    // Measured on the reference's stems (DEV-28b2): slope 0.5 releases 1 s from 0.894 to 0.1189 at 0.5 s (covers 0.867),
    // 0.0098 left at 0.9 s; slope 0 (attack) is linear.
    CHECK_NEAR(W::slopeCurve(0.5f, 0.5f), 1 - 0.11882 / 0.89370, 0.002);
    CHECK_NEAR(W::slopeCurve(0.05f, 0.5f), 1 - 0.73716 / 0.89370, 0.002);
    CHECK_NEAR(W::slopeCurve(0.9f, 0.5f), 1 - 0.00979 / 0.89370, 0.002);
    CHECK_NEAR(W::slopeCurve(0.5f, 0.0f), 0.5, 1e-6);
    CHECK_NEAR(W::slopeCurve(0.5f, -0.5f), 0.11882 / 0.89370, 0.002);
}

TEST_CASE("InstrumentVector: table files load as whole 1024-sample frames; factory tables resolve when installed")
{
    juce::TemporaryFile file(".wav");
    {
        juce::WavAudioFormat wav;
        auto stream = file.getFile().createOutputStream();
        REQUIRE(stream != nullptr);
        std::unique_ptr<juce::OutputStream> out(std::move(stream));
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> b(1, 1024 * 3 + 700);
        for (int i = 0; i < b.getNumSamples(); ++i) b.setSample(0, i, (float) std::sin(2 * M_PI * i / 1024.0) * 0.5f);
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
        // The reference plays "Basic Shapes" (4096 samples) as sine / triangle / saw / square: 4 frames of 1024.
        if (const auto* shapes = devices::findFactoryWavetable("Basic Shapes"))
            if (const auto basic = devices::loadWavetableFile(shapes->file, error)) CHECK(basic->frames() == 4);
    }
}

TEST_CASE("InstrumentVector: a fresh Wavetable from the browser saves, reloads and keeps the reference's default patch")
{
    auto loaded = io::loadAls(wavetableFixture());
    REQUIRE(loaded);
    auto device = devices::nativeDeviceModel("InstrumentVector");
    CHECK(device.displayName == "Sonora Wavetable");
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

// ------------------------------------------------------------------------------------------------ DEV-28b2 laws
// Laws measured on the reference's exported stems (suite tools/parity_probe/suites/wavetable_laws.txt, docs/WAVETABLE.md
// "Parity"). Numbers in the comments are the reference's.

namespace
{
constexpr double kSr = 44100;

std::unique_ptr<W> lawSynth()
{
    auto synth = std::make_unique<W>();
    synth->setTable(0, sawTable(), "Test", "Sine Saw"); // frame 0 sine, frame 1 saw
    synth->setTable(1, sawTable(), "Test", "Sine Saw");
    synth->prepare(kSr, 4096);
    synth->setParameter(W::AmpSustain, 1);
    synth->setParameter(W::Volume, 1);
    return synth;
}

// Renders `seconds` with the given notes ({sample, note, velocity}; velocity 0 = note off) in 512-sample blocks.
juce::AudioBuffer<float> renderNotes(W& synth, double seconds, std::vector<std::array<int, 3>> events)
{
    const int total = (int) (seconds * kSr);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int start = 0; start < total; start += 512)
    {
        const int n = std::min(512, total - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, n);
        juce::MidiBuffer midi;
        for (const auto& e : events)
            if (e[0] >= start && e[0] < start + n)
                midi.addEvent(e[2] > 0 ? juce::MidiMessage::noteOn(1, e[1], (juce::uint8) e[2]) : juce::MidiMessage::noteOff(1, e[1]),
                              e[0] - start);
        synth.process(view, midi);
    }
    return out;
}

// Amplitude of the component at `freq` (Hann-windowed projection over [start, start + length)).
double amplitudeAt(const juce::AudioBuffer<float>& a, int channel, double freq, int start, int length)
{
    double re = 0, im = 0, sum = 0;
    for (int i = 0; i < length; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / length);
        const double x = a.getSample(channel, start + i) * w;
        re += x * std::cos(2 * M_PI * freq * (start + i) / kSr);
        im += x * std::sin(2 * M_PI * freq * (start + i) / kSr);
        sum += w;
    }
    return 2 * std::hypot(re, im) / sum;
}

double db(double x) { return 20 * std::log10(std::max(x, 1e-12)); }
const double kC3 = 440 * std::exp2((60 - 69) / 12.0);
} // namespace

TEST_CASE("InstrumentVector: output level, velocity and envelope laws measured on the reference")
{
    // A full-scale sine at Volume 1, Sustain 1, velocity 127 peaks at 0.70707 (P20); velocity -> Amp 50 % is linear
    // (velocity 100: 0.894, P12).
    for (int velocity : {127, 100})
    {
        auto synth = lawSynth();
        const auto a = renderNotes(*synth, 1.0, {{{0, 60, velocity}}});
        CHECK_NEAR(amplitudeAt(a, 0, kC3, 22050, 22050), 0.70711 * (0.5 + 0.5 * velocity / 127.0), 0.002);
        CHECK_NEAR(amplitudeAt(a, 1, kC3, 22050, 22050), 0.70711 * (0.5 + 0.5 * velocity / 127.0), 0.002);
    }
    // Volume and Sustain are linear: 0.3548 x 0.501 -> 0.1257 (P01).
    auto synth = lawSynth();
    synth->setParameter(W::Volume, 0.3548134f);
    synth->setParameter(W::AmpSustain, 0.5011876f);
    const auto a = renderNotes(*synth, 2.0, {{{0, 60, 127}}});
    CHECK_NEAR(amplitudeAt(a, 0, kC3, 66150, 22050), 0.12574, 0.001);
    // Release, slope 0.5, 1 s from 0.894: 0.11882 left at 0.5 s (P12, normalised to the full-scale level).
    auto r = lawSynth();
    r->setParameter(W::AmpRelease, 1);
    const auto b = renderNotes(*r, 2.0, {{{0, 60, 100}}, {{44100, 60, 0}}});
    CHECK_NEAR(amplitudeAt(b, 0, kC3, 44100 + 22050 - 1102, 2205) / 0.70711, 0.11882, 0.006);
}

TEST_CASE("InstrumentVector: sub oscillator, resonance, FM and Classic follow the measured laws")
{
    // Sub, Tone 0: a pure sine an octave down at the oscillators' level (P13: 1.000, harmonics < -100 dB).
    {
        auto synth = lawSynth();
        synth->setParameter(W::Osc1On, 0);
        synth->setParameter(W::SubOn, 1);
        synth->setParameter(W::SubGain, 1);
        const auto a = renderNotes(*synth, 1.0, {{{0, 60, 127}}});
        CHECK_NEAR(amplitudeAt(a, 0, kC3 / 2, 22050, 22050), 0.70711, 0.003);
        CHECK(db(amplitudeAt(a, 0, kC3, 22050, 22050) / 0.70711) < -80);
    }
    // Sub, Tone 1: a +-0.921 pulse high for 59.7 % of the cycle (h5 / h10 39 dB down, P14).
    {
        float hp = 0;
        int high = 0;
        float peak = 0;
        const int n = 10000;
        for (int i = 0; i < n; ++i)
        {
            const float y = W::subOscillator((i + 0.5) / n, 1.0 / n, 1.0f, hp, 0.0f);
            high += y > 0 ? 1 : 0;
            peak = std::max(peak, std::abs(y));
        }
        CHECK_NEAR(high / (double) n, 0.597, 0.002);
        CHECK_NEAR(peak, 0.921, 0.005);
    }
    // Clean resonance: d = 1 - r. LP 1 kHz, 24 dB, r 0.5 = stages of Q 1.0 and 0.67 (P08 fit).
    {
        auto synth = lawSynth();
        synth->setParameter(W::F1Slope, 1);
        devices::BiquadCoeffs stages[2];
        REQUIRE(synth->filterStages(0, 1000, 0.5, 0, stages) == 2);
        CHECK_NEAR(stages[0].magnitudeAt(1000, kSr), 1.0, 0.02);
        CHECK_NEAR(stages[1].magnitudeAt(1000, kSr), 1.0 / 1.5, 0.02);
    }
    // FM 0.5 / 0.5 on a sine: ratio 2, index 5 rad (P16: h1 -6.0, h3 -8.6, h5 -7.8, h9 -3.7 dB re the voice level).
    {
        auto synth = lawSynth();
        synth->setParameter(W::Osc1EffectMode, 1);
        synth->setParameter(W::Osc1Effect1, 0.5f);
        synth->setParameter(W::Osc1Effect2, 0.5f);
        const auto a = renderNotes(*synth, 1.0, {{{0, 60, 127}}});
        const double expected[][2] = {{1, -6.0}, {3, -8.6}, {5, -7.8}, {9, -3.7}};
        for (const auto& e : expected) CHECK_NEAR(db(amplitudeAt(a, 0, kC3 * e[0], 22050, 22050) / 0.70711), e[1], 0.5);
        CHECK(db(amplitudeAt(a, 0, kC3 * 2, 22050, 22050) / 0.70711) < -60); // odd harmonics only
    }
    // Classic PW 0.5 / Sync 0.5: the table plays in the first 51.9 % of each cycle, 2.51 times; silent otherwise (P17).
    {
        auto synth = lawSynth();
        synth->setParameter(W::Osc1EffectMode, 2);
        synth->setParameter(W::Osc1Effect1, 0.5f);
        synth->setParameter(W::Osc1Effect2, 0.5f);
        synth->setParameter(W::F1On, 0);
        const auto a = renderNotes(*synth, 1.0, {{{0, 36, 127}}}); // C1: 674 samples per cycle
        int silent = 0;
        const int from = 22050, length = 13480; // 20 cycles
        for (int i = from; i < from + length; ++i) silent += std::abs(a.getSample(0, i)) < 1e-4f ? 1 : 0;
        CHECK_NEAR(silent / (double) length, 1 - 0.5186, 0.01);
    }
}

TEST_CASE("InstrumentVector: unison panning, legato-only glide, parallel routing and LFO phase follow the reference")
{
    // Classic unison, 4 voices, Amount 0.5: voices at -25 / -8.3 / +8.3 / +25 ct, hard left / right / left / right,
    // each 0.707 of a single voice (P15).
    {
        auto synth = lawSynth();
        synth->setParameter(W::UnisonMode, 1);
        synth->setParameter(W::UnisonVoices, 4);
        synth->setParameter(W::UnisonAmount, 0.5f);
        const auto a = renderNotes(*synth, 3.0, {{{0, 60, 127}}});
        const double c = std::exp2(1 / 1200.0);
        const int start = 22050, length = 110250; // 2.5 s resolves 1.3 Hz spacing
        CHECK_NEAR(amplitudeAt(a, 0, kC3 * std::pow(c, -25.0), start, length), 0.70711 * 0.70711, 0.02);
        CHECK(amplitudeAt(a, 1, kC3 * std::pow(c, -25.0), start, length) < 0.03);
        CHECK_NEAR(amplitudeAt(a, 1, kC3 * std::pow(c, -25.0 / 3), start, length), 0.70711 * 0.70711, 0.02);
        CHECK_NEAR(amplitudeAt(a, 0, kC3 * std::pow(c, 25.0 / 3), start, length), 0.70711 * 0.70711, 0.02);
        CHECK_NEAR(amplitudeAt(a, 1, kC3 * std::pow(c, 25.0), start, length), 0.70711 * 0.70711, 0.02);
    }
    // Mono glide only between legato notes: a note after a released one starts at its own pitch (P19).
    {
        auto synth = lawSynth();
        synth->setParameter(W::MonoPoly, 0);
        synth->setParameter(W::Glide, 0.2f);
        synth->setParameter(W::AmpRelease, 0.5f);
        const auto a = renderNotes(*synth, 1.5, {{{0, 72, 127}}, {{11025, 72, 0}}, {{22050, 48, 127}}});
        const double f48 = 440 * std::exp2((48 - 69) / 12.0);
        CHECK(amplitudeAt(a, 0, f48, 22050 + 441, 2205) > 0.5); // at 48 within 10 ms, no glide from 72
    }
    // Parallel routing splits the mix: with both filters open it plays at the Serial level (Flux Capacitor).
    {
        double level[2]{};
        for (int routing = 0; routing < 2; ++routing)
        {
            auto synth = lawSynth();
            synth->setParameter(W::F2On, 1);
            synth->setParameter(W::F2Type, 1);
            synth->setParameter(W::F2Frequency, 20);
            synth->setParameter(W::FilterRouting, (float) routing);
            const auto a = renderNotes(*synth, 1.0, {{{0, 60, 127}}});
            level[routing] = amplitudeAt(a, 0, kC3, 22050, 22050);
        }
        CHECK_NEAR(db(level[1] / level[0]), 0, 0.3);
    }
    // The triangle LFO rises first, in phase with the sine (Spacefolder's notch sweep).
    {
        auto synth = lawSynth();
        synth->setParameter(W::Osc1Position, 0.5f);
        synth->setParameter(W::L1Type, 1);
        synth->setParameter(W::L1Rate, 1);
        synth->setParameter(W::matrixParam(W::TOsc1Pos, W::SrcLfo1), 0.4f);
        juce::AudioBuffer<float> audio(2, 4410);
        audio.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127), 0);
        synth->process(audio, midi); // 0.1 s: the triangle is at +0.4
        CHECK_NEAR(synth->displayPosition(0), 0.5f + 0.4f * 0.4f, 0.02f);
    }
}

TEST_CASE("InstrumentVector: MS2 drive is a clean gain into slew-limited integrators")
{
    // P09: MS2 LP 1 kHz with Drive 12 dB is 11.8 dB above the Clean, undriven filter (the input is not clipped).
    double level[2]{};
    for (int k = 0; k < 2; ++k)
    {
        auto synth = lawSynth();
        synth->setParameter(W::Osc1Position, 1); // saw frame
        synth->setParameter(W::F1Frequency, 1000);
        synth->setParameter(W::Volume, 0.25f);
        if (k == 1)
        {
            synth->setParameter(W::F1CircuitLpHp, 2);
            synth->setParameter(W::F1Drive, 12);
        }
        const auto a = renderNotes(*synth, 1.0, {{{0, 60, 127}}});
        level[k] = rms(a, 0, 22050, 22050);
    }
    CHECK_NEAR(db(level[1] / level[0]), 11.8, 1.0);
}
