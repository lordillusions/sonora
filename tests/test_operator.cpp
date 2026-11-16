// DEV-28a: the reference's Operator as a native device: parameter layout, import, pitch, levels, algorithms, envelopes,
// voices and real-time invariants. Values here are Sonora-chosen; nothing is copied from the reference's presets.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/Operator.h"
#include "io/als/AlsReader.h"

#include <cmath>
#include <set>

using namespace sonora;
using O = devices::Operator;

namespace
{
constexpr double kRate = 48000.0;

std::unique_ptr<O> makeOperator()
{
    auto o = std::make_unique<O>();
    o->prepare(kRate, 512);
    o->setParameter(O::FilterOn, 0); // raw oscillators unless a test wants the filter
    return o;
}

struct Note { int note, velocity; double on, off; };

juce::AudioBuffer<float> play(O& o, std::vector<Note> notes, double seconds, int block = 512)
{
    const int total = (int) (seconds * kRate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min(block, total - pos);
        juce::AudioBuffer<float> chunk(2, n);
        juce::MidiBuffer midi;
        for (const auto& note : notes)
        {
            const int on = (int) (note.on * kRate), off = note.off < 0 ? -1 : (int) (note.off * kRate);
            if (on >= pos && on < pos + n) midi.addEvent(juce::MidiMessage::noteOn(1, note.note, (juce::uint8) note.velocity), on - pos);
            if (off >= pos && off < pos + n) midi.addEvent(juce::MidiMessage::noteOff(1, note.note), off - pos);
        }
        o.process(chunk, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, pos, chunk, ch, 0, n);
    }
    return out;
}

juce::AudioBuffer<float> hold(O& o, int note, double seconds, int velocity = 127) { return play(o, {{note, velocity, 0, -1}}, seconds); }

// Magnitude of `hz` (Goertzel, Hann window) in channel 0 over [start, start + length), normalised to a full-scale sine = 1.
double magnitude(const juce::AudioBuffer<float>& a, int start, int length, double hz)
{
    const double w = 2 * M_PI * hz / kRate, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < length; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(2 * M_PI * i / (length - 1));
        const double s0 = a.getSample(0, start + i) * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / (length * 0.25);
}

double peak(const juce::AudioBuffer<float>& a, int from = 0, int to = -1)
{
    double p = 0;
    for (int i = from; i < (to < 0 ? a.getNumSamples() : to); ++i) p = std::max(p, (double) std::abs(a.getSample(0, i)));
    return p;
}

int indexOf(const O& o, const std::string& id)
{
    const auto& p = o.parameters();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].id == id) return (int) i;
    return -1;
}

double hz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }
} // namespace

TEST_CASE("Operator: parameter ids are the reference's paths, unique, with its defaults")
{
    O o;
    const auto& p = o.parameters();
    CHECK((int) p.size() == O::NumParams);
    std::set<std::string> ids;
    for (const auto& info : p) ids.insert(info.id);
    CHECK(ids.size() == p.size());
    CHECK(indexOf(o, "Operator.0/Envelope/AttackTime") == O::envParam(0, O::AttackTime));
    CHECK(indexOf(o, "Operator.3/Tune/FixedFrequencyMulti") == O::opParam(3, O::OpFixedMulti));
    CHECK(indexOf(o, "Filter/Envelope/SustainLevel") == O::otherEnvParam(O::EnvFilter, O::SustainLevel));
    CHECK(indexOf(o, "PitchEnv/PitchEnv/EnvelopeAmount") == O::PitchEnvAmount);
    CHECK(indexOf(o, "Lfo/LfoEnv/ReleaseTime") == O::otherEnvParam(O::EnvLfo, O::ReleaseTime));
    CHECK(indexOf(o, "Globals/Algorithm") == O::Algorithm);
    CHECK(indexOf(o, "MidiCtrl/MidiCtrl.2/ModConnections.1/Connection") == O::midiModParam(4, 1, true));
    CHECK(indexOf(o, "Operator.2/UserHarmonics/Harmonics.63") == O::harmonicParam(2, 63));
    CHECK(p[O::NumVoices].plainValue && p[O::harmonicParam(0, 5)].plainValue && ! p[O::Algorithm].plainValue);
    // The reference's default patch: A at 0 dB, B-D at -inf, Volume -18 dB, Tone 70 %, LP 24 12 kHz, res 28 %.
    CHECK_NEAR(o.getParameter(O::opParam(0, O::OpVolume)), 1.0, 1e-6);
    CHECK(o.getParameter(O::opParam(1, O::OpVolume)) < 0.001f);
    CHECK_NEAR(20 * std::log10(o.getParameter(O::Volume)), -18.0, 0.01);
    CHECK_NEAR(o.getParameter(O::Tone), 0.7, 1e-6);
    CHECK_NEAR(o.getParameter(O::FilterFreq), 12000.0, 1e-3);
    CHECK(o.voiceLimit() == 6);
    CHECK(std::string(O::waveName(22)) == "User");
    CHECK(std::string(O::waveName(19)) == "Triangle");
    CHECK(std::string(O::waveShortName(10)) == "SwD");
}

TEST_CASE("Operator: an Operator element imports into the native device")
{
    const char* xml = R"(<Operator Id="3"><On><Manual Value="true"/></On><UserName Value="Test Bell"/>
        <Operator.0><Volume><Manual Value="0.5"/></Volume><WaveForm><Manual Value="3"/></WaveForm>
          <Tune><Coarse><Manual Value="2"/></Coarse><FixedFrequencyOn><Manual Value="false"/></FixedFrequencyOn></Tune>
          <UserHarmonics><Harmonics.1 Value="0.75"/><NumHarmonics Value="1"/></UserHarmonics></Operator.0>
        <Globals><Algorithm><Manual Value="10"/></Algorithm><NumVoices Value="3"/><RetriggerMode Value="true"/></Globals>
        <Filter><OnOff><Manual Value="false"/></OnOff></Filter>
        <MidiCtrl><VelDst><ModConnections.0><Amount Value="40"/><Connection Value="5"/></ModConnections.0></VelDst></MidiCtrl>
        </Operator>)";
    auto element = juce::XmlDocument::parse(xml);
    REQUIRE(element != nullptr);
    std::vector<std::string> warnings;
    const auto device = io::readDeviceXml(*element, warnings);
    CHECK(device.type == "Operator");
    CHECK(device.displayName == "Test Bell");
    CHECK(warnings.empty());
    auto processor = devices::createNative(device, &warnings);
    REQUIRE(processor != nullptr);
    CHECK(processor->name() == "Operator");
    CHECK_NEAR(processor->getParameter(O::opParam(0, O::OpVolume)), 0.5, 1e-6);
    CHECK_NEAR(processor->getParameter(O::opParam(0, O::OpWave)), 3.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::opParam(0, O::OpCoarse)), 2.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::Algorithm), 10.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::FilterOn), 0.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::NumVoices), 3.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::RetriggerMode), 1.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::harmonicParam(0, 1)), 0.75, 1e-6);
    CHECK_NEAR(processor->getParameter(O::numHarmonicsParam(0)), 1.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::midiModParam(1, 0, false)), 40.0, 1e-6);
    CHECK_NEAR(processor->getParameter(O::midiModParam(1, 0, true)), 5.0, 1e-6);
}

TEST_CASE("Operator: the default patch is a sine at the note's pitch and the set level")
{
    auto o = makeOperator();
    const auto out = hold(*o, 69, 0.5, 127);
    const int start = (int) (0.1 * kRate), length = (int) (0.3 * kRate);
    CHECK_NEAR(magnitude(out, start, length, 440.0), std::pow(10.0, -18.0 / 20.0) * 2.25, 0.004); // Vel 50 %, velocity 127: (0.5 + 1)^2
    CHECK(magnitude(out, start, length, 880.0) < 1e-4);
    // Vel 50 % (measured law (1 - s + 2 s v)^2): velocity 64 plays at (0.5 + 64/127)^2.
    auto soft = makeOperator();
    const auto quiet = hold(*soft, 69, 0.5, 64);
    CHECK_NEAR(magnitude(quiet, start, length, 440.0) / magnitude(out, start, length, 440.0), std::pow(0.5 + 64.0 / 127.0, 2) / 2.25, 0.005);
}

TEST_CASE("Operator: Coarse / Fine ratios, Coarse 0 = 0.5, fixed frequency ignores the note")
{
    CHECK_NEAR(O::ratio(0, 0), 0.5, 1e-12);
    CHECK_NEAR(O::ratio(3, 0), 3.0, 1e-12);
    CHECK_NEAR(O::fixedFrequency(100, 4), 100.0, 1e-9);
    auto o = makeOperator();
    o->setParameter(O::opParam(0, O::OpCoarse), 3);
    const int start = (int) (0.1 * kRate), length = (int) (0.3 * kRate);
    auto out = hold(*o, 57, 0.5);
    CHECK(magnitude(out, start, length, 3 * hz(57)) > 0.1);
    auto f = makeOperator();
    f->setParameter(O::opParam(0, O::OpFixed), 1);
    f->setParameter(O::opParam(0, O::OpFixedFreq), 500);
    out = hold(*f, 40, 0.5);
    CHECK(magnitude(out, start, length, 500.0) > 0.1);
    auto g = makeOperator();
    g->setParameter(O::opParam(0, O::OpFixed), 1);
    g->setParameter(O::opParam(0, O::OpFixedFreq), 500);
    out = hold(*g, 80, 0.5);
    CHECK(magnitude(out, start, length, 500.0) > 0.1);
}

TEST_CASE("Operator: algorithms route modulators into carriers; the additive one sums all four")
{
    for (int a = 0; a < O::kAlgorithms; ++a)
    {
        CHECK(O::carriers(a) != 0);
        for (int op = 0; op < 4; ++op)
        {
            CHECK((O::modulators(a, op) & (1 << op)) == 0); // nobody modulates itself through the structure
            // Signal flows downwards (D -> A): a modulator always has a higher index than its target.
            CHECK((O::modulators(a, op) & ((1 << (op + 1)) - 1)) == 0);
        }
    }
    CHECK(O::carriers(0) == 1 && O::modulators(0, 0) == 2 && O::modulators(0, 1) == 4 && O::modulators(0, 2) == 8);
    CHECK(O::carriers(10) == 15);

    const int start = (int) (0.1 * kRate), length = (int) (0.3 * kRate);
    const int ratios[4] = {2, 3, 5, 13};
    auto setup = [&](O& o, int algorithm) {
        o.setParameter(O::Algorithm, (float) algorithm);
        for (int op = 0; op < 4; ++op)
        {
            o.setParameter(O::opParam(op, O::OpVolume), 0.25f);
            o.setParameter(O::opParam(op, O::OpCoarse), (float) ratios[op]);
        }
    };
    auto additive = makeOperator();
    setup(*additive, 10);
    auto out = hold(*additive, 45, 0.5);
    const double dAlone = magnitude(out, start, length, 13 * hz(45));
    for (int op = 0; op < 4; ++op)
        CHECK(magnitude(out, start, length, ratios[op] * hz(45)) > 0.25 * 0.1258 * 0.8);
    auto stack = makeOperator();
    setup(*stack, 0);
    for (int op = 1; op < 4; ++op) stack->setParameter(O::opParam(op, O::OpVolume), 0.02f); // small indices (2 cycles per unit)
    out = hold(*stack, 45, 0.5);
    CHECK(magnitude(out, start, length, 2 * hz(45)) > 0.01);                // A sounds
    CHECK(magnitude(out, start, length, (2 + 3) * hz(45)) > 0.002);         // with B's sidebands
    CHECK(magnitude(out, start, length, 13 * hz(45)) < 0.5 * dAlone); // D is not heard by itself (only far sidebands land there)
}

TEST_CASE("Operator: feedback and a modulator add harmonics; levels at -inf are silent")
{
    const int start = (int) (0.1 * kRate), length = (int) (0.3 * kRate);
    auto plain = makeOperator();
    auto out = hold(*plain, 57, 0.5);
    const double pure = magnitude(out, start, length, 2 * hz(57));
    auto fb = makeOperator();
    fb->setParameter(O::Algorithm, 10); // feedback acts only on an oscillator nothing modulates (A is modulated in Alg. 1)
    fb->setParameter(O::opParam(0, O::OpFeedback), 100);
    out = hold(*fb, 57, 0.5);
    CHECK(magnitude(out, start, length, 2 * hz(57)) > pure + 0.001);
    auto fm = makeOperator();
    fm->setParameter(O::opParam(1, O::OpVolume), 0.5f);
    out = hold(*fm, 57, 0.5);
    CHECK(magnitude(out, start, length, 2 * hz(57)) > 0.002);
    auto silent = makeOperator();
    silent->setParameter(O::opParam(0, O::OpVolume), 0.0003162277571f);
    CHECK(peak(hold(*silent, 57, 0.3)) < 1e-6);
}

TEST_CASE("Operator: envelope stages, release ends the voice, sustain pedal holds it")
{
    auto o = makeOperator();
    o->setParameter(O::envParam(0, O::AttackTime), 100);
    o->setParameter(O::envParam(0, O::ReleaseTime), 50);
    auto out = play(*o, {{60, 127, 0, 0.3}}, 0.6);
    CHECK(peak(out, 0, (int) (0.02 * kRate)) < peak(out, (int) (0.15 * kRate), (int) (0.25 * kRate)) * 0.4); // attack rising
    CHECK(peak(out, (int) (0.45 * kRate)) < 1e-5);                                                            // released
    CHECK(o->activeVoices() == 0);
    auto pedal = makeOperator();
    juce::AudioBuffer<float> buffer(2, 4800);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 10);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    pedal->process(buffer, midi);
    for (int i = 0; i < 20; ++i) { juce::MidiBuffer none; pedal->process(buffer, none); }
    CHECK(pedal->activeVoices() == 1);
    CHECK(peak(buffer) > 0.01);
}

TEST_CASE("Operator: voice limit, mono legato and block-size invariance")
{
    auto o = makeOperator();
    o->setParameter(O::NumVoices, 1); // 2 voices
    play(*o, {{60, 100, 0, -1}, {64, 100, 0.01, -1}, {67, 100, 0.02, -1}}, 0.1);
    CHECK(o->activeVoices() == 2);
    auto mono = makeOperator();
    mono->setParameter(O::NumVoices, 0);
    play(*mono, {{60, 100, 0, -1}, {64, 100, 0.01, -1}}, 0.1);
    CHECK(mono->activeVoices() == 1);

    auto a = makeOperator(), b = makeOperator();
    for (auto* x : {a.get(), b.get()})
    {
        x->setParameter(O::opParam(1, O::OpVolume), 0.5f);
        x->setParameter(O::LfoOn, 1);
        x->setParameter(O::PitchEnvOn, 1);
        x->setParameter(O::FilterOn, 1);
    }
    const std::vector<Note> notes{{60, 100, 0, 0.2}, {67, 90, 0.05, 0.25}};
    const auto big = play(*a, notes, 0.4, 512), small = play(*b, notes, 0.4, 37);
    double diff = 0;
    for (int i = 0; i < big.getNumSamples(); ++i) diff = std::max(diff, (double) std::abs(big.getSample(0, i) - small.getSample(0, i)));
    CHECK(diff < 1e-6);
}

TEST_CASE("Operator: user harmonics, waveforms and the filter / shaper stay finite")
{
    const int start = (int) (0.1 * kRate), length = (int) (0.3 * kRate);
    auto user = makeOperator();
    user->setParameter(O::opParam(0, O::OpWave), 22);
    user->setParameter(O::harmonicParam(0, 0), 0);
    user->setParameter(O::harmonicParam(0, 2), 1); // the third partial only
    auto out = hold(*user, 45, 0.5);
    CHECK(magnitude(out, start, length, 3 * hz(45)) > 0.05);
    CHECK(magnitude(out, start, length, hz(45)) < 1e-3);
    for (int wave = 0; wave < O::kWaveforms; ++wave)
    {
        auto o = makeOperator();
        o->setParameter(O::opParam(0, O::OpWave), (float) wave);
        o->setParameter(O::opParam(1, O::OpVolume), 1);
        o->setParameter(O::opParam(1, O::OpWave), (float) wave);
        o->setParameter(O::opParam(0, O::OpFeedback), 100);
        o->setParameter(O::FilterOn, 1);
        o->setParameter(O::FilterRes, 1.25f);
        o->setParameter(O::ShaperType, (float) (wave % 5));
        out = play(*o, {{36, 127, 0, 0.1}, {96, 127, 0, 0.1}}, 0.2);
        bool finite = true;
        for (int i = 0; i < out.getNumSamples(); ++i) finite = finite && std::isfinite(out.getSample(0, i)) && std::abs(out.getSample(0, i)) < 8;
        CHECK(finite);
    }
}

TEST_CASE("Operator: bypass is silent and drops MIDI")
{
    auto o = makeOperator();
    o->setBypassed(true);
    CHECK(peak(hold(*o, 60, 0.1)) == 0.0);
}
