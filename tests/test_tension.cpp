// DEV-28j: the reference's Tension (StringStudio) as a native device: face value laws, import, pitch, dispersion, release
// and stability. Values here are Sonora-chosen; nothing is copied from the reference's presets.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/Tension.h"
#include "io/als/AlsReader.h"

#include <cmath>
#include <set>

using namespace sonora;
using T = devices::Tension;

namespace
{
constexpr double kRate = 48000.0;

std::unique_ptr<T> makeTension()
{
    auto t = std::make_unique<T>();
    t->prepare(kRate, 512);
    return t;
}

// Renders `seconds` with a note held for `hold` seconds (or the whole time when hold < 0).
juce::AudioBuffer<float> play(T& t, int note, double seconds, double hold = -1, int velocity = 100)
{
    const int total = (int) (seconds * kRate), block = 512;
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    const int off = hold < 0 ? -1 : (int) (hold * kRate);
    for (int pos = 0; pos < total; pos += block)
    {
        const int n = std::min(block, total - pos);
        juce::AudioBuffer<float> chunk(2, n);
        chunk.clear();
        juce::MidiBuffer midi;
        if (pos == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) velocity), 0);
        if (off >= pos && off < pos + n) midi.addEvent(juce::MidiMessage::noteOff(1, note), off - pos);
        t.process(chunk, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, pos, chunk, ch, 0, n);
    }
    return out;
}

// Frequency of the strongest Goertzel peak within +-span (relative) of `around`, 0.05 Hz steps.
double peakNear(const juce::AudioBuffer<float>& a, int start, int length, double around, double span)
{
    double best = 0, bestHz = around;
    for (double hz = around * (1 - span); hz <= around * (1 + span); hz += 0.05)
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
        const double power = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (power > best) { best = power; bestHz = hz; }
    }
    return bestHz;
}

bool finiteAndBounded(const juce::AudioBuffer<float>& a, float limit)
{
    for (int i = 0; i < a.getNumSamples(); ++i)
        if (! std::isfinite(a.getSample(0, i)) || std::abs(a.getSample(0, i)) > limit) return false;
    return true;
}

int indexOf(const T& t, const std::string& id)
{
    const auto& p = t.parameters();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].id == id) return (int) i;
    return -1;
}
} // namespace

TEST_CASE("Tension: face value laws read from the reference's captures")
{
    CHECK_NEAR(T::filterHz(0), 30.0, 0.01);
    CHECK_NEAR(T::filterHz(1), 22000.0, 1.0);
    CHECK_NEAR(T::filterHz(0.5f), 812.0, 5.0);          // shown "812 Hz"
    CHECK_NEAR(T::envelopeMs(0), 5.0, 0.01);             // "5 ms"
    CHECK_NEAR(T::envelopeMs(0.5f), 283.0, 0.5);         // "283 ms"
    CHECK_NEAR(T::envelopeMs(1), 15000.0, 1.0);          // "15.0 s"
    CHECK_NEAR(T::vibratoMs(0), 0.0, 0.0);
    CHECK_NEAR(T::vibratoMs(0.5f), 509.0, 3.0);          // "509 ms"
    CHECK_NEAR(T::vibratoHz(0.5f), 5.1, 0.01);           // "5.1 Hz"
    CHECK_NEAR(T::lfoMs(0.5f), 1000.0, 15.0);            // "1.01 s"
    CHECK_NEAR(T::unisonMs(0.5f), 14.0, 0.3);            // "14 ms"
    CHECK_NEAR(T::keyAmount(1), 16.0, 0.05);
    CHECK_NEAR(T::keyAmount(-1), -16.0, 0.05);
    CHECK_NEAR(T::keyAmount(0.4545f), 1.02, 0.02);
    CHECK_NEAR(T::keyAmount(0), 0.0, 0.0);
    CHECK_NEAR(T::cutoffEnvAmount(-0.818f), -7.2, 0.05);
    CHECK_NEAR(T::modAmount(-0.818f), -2.68, 0.02);
    CHECK_NEAR(T::volumeDb(0.705f), 0.0, 0.05);           // "0.0 dB"
    CHECK_NEAR(T::volumeDb(1), 6.0, 0.08);                // "6.0 dB"
    CHECK(std::string(T::syncName(19)) == "1/16");
    CHECK(std::string(T::syncName(23)) == "1/32t");
    CHECK(std::string(T::filterTypeName(8)) == "F6");
    CHECK(std::string(T::targetName(5)) == "String Inharmon.");
    CHECK(T::polyphonyVoices(0) == 1);
    CHECK(T::polyphonyVoices(4) == 12);
    CHECK_NEAR(T::syncBeats(13), 1.0, 1e-9); // 1/4
}

TEST_CASE("Tension: parameter ids are the set's StringStudio tags, unique")
{
    T t;
    const auto& p = t.parameters();
    CHECK((int) p.size() == T::NumParams);
    std::set<std::string> ids;
    for (const auto& info : p) ids.insert(info.id);
    CHECK(ids.size() == p.size());
    for (const char* tag : {"ExcitatorType", "StringInharmonicity", "TerminationFingerForce", "BodyLevel", "BodyMix",
                            "FilterEnvelopeAmpMod", "ChannelPressureTarget1", "SlideRange2", "PerNotePitchBendRange", "CurrentTab"})
        CHECK(indexOf(t, tag) >= 0);
    CHECK(p[T::CurrentTab].plainValue);
}

TEST_CASE("Tension: a StringStudio element imports into the native device")
{
    const char* xml = R"(<StringStudio Id="7"><On><Manual Value="true"/></On><UserName Value="Test Pluck"/>
        <ExcitatorType><Manual Value="1"/></ExcitatorType><StringDecay><Manual Value="0.25"/></StringDecay>
        <BodyToggle><Manual Value="true"/></BodyToggle><BodySize><Manual Value="4"/></BodySize>
        <Polyphony><Manual Value="2"/></Polyphony><CurrentTab Value="1"/></StringStudio>)";
    auto element = juce::XmlDocument::parse(xml);
    REQUIRE(element != nullptr);
    std::vector<std::string> warnings;
    const auto device = io::readDeviceXml(*element, warnings);
    CHECK(device.type == "StringStudio");
    CHECK(device.displayName == "Test Pluck");
    CHECK(warnings.empty());
    auto processor = devices::createNative(device);
    REQUIRE(processor != nullptr);
    CHECK(processor->name() == "Tension");
    CHECK(processor->isInstrument());
    CHECK_NEAR(processor->getParameter(T::ExcType), 1.0, 1e-6);
    CHECK_NEAR(processor->getParameter(T::StringDecay), 0.25, 1e-6);
    CHECK_NEAR(processor->getParameter(T::BodyOn), 1.0, 1e-6);
    CHECK_NEAR(processor->getParameter(T::BodySize), 4.0, 1e-6);
    CHECK_NEAR(processor->getParameter(T::CurrentTab), 1.0, 1e-6);
    CHECK_NEAR(processor->getParameter(T::Volume), 0.705, 1e-3); // not in the element: the default preset's value
}

TEST_CASE("Tension: pitch and the reference's partial stretch")
{
    auto t = makeTension();
    t->setParameter(T::StringInharm, 0.0f);
    const auto clean = play(*t, 60, 1.0);
    CHECK_NEAR(peakNear(clean, 2400, 24000, 261.63, 0.03), 261.63, 0.5);
    CHECK_NEAR(peakNear(clean, 2400, 24000, 4 * 261.63, 0.03) / (4 * 261.63), 1.0, 0.0015);

    // Inharm 50 % (default): partial 4 is 0.88 % sharp at C3 (fitted on the reference's stems; one allpass section).
    auto d = makeTension();
    const auto stretched = play(*d, 60, 1.0);
    CHECK_NEAR(peakNear(stretched, 2400, 24000, 261.63, 0.03), 261.63, 0.5);
    CHECK_NEAR(peakNear(stretched, 2400, 24000, 4 * 261.63, 0.03) / (4 * 261.63), 1.0088, 0.002);

    // Octave / Semi / Detune move the string.
    auto o = makeTension();
    o->setParameter(T::Octave, 1);
    o->setParameter(T::Transpose, -12);
    o->setParameter(T::FineTune, 0.0f); // -0.5 semitone
    o->setParameter(T::StringInharm, 0.0f);
    const auto low = play(*o, 60, 1.0);
    CHECK_NEAR(peakNear(low, 2400, 24000, 254.2, 0.03), 261.63 * std::pow(2.0, -0.5 / 12.0), 0.6);
}

TEST_CASE("Tension: Decay sets the held decay and Ratio the release")
{
    auto level = [](const juce::AudioBuffer<float>& a, double from, double to) {
        return a.getRMSLevel(0, (int) (from * kRate), (int) ((to - from) * kRate));
    };
    auto held = makeTension();
    const auto long1 = play(*held, 60, 2.0);
    CHECK(level(long1, 1.5, 1.9) > level(long1, 0.05, 0.4) * 0.5); // Decay 100 %: still ringing

    auto shortDecay = makeTension();
    shortDecay->setParameter(T::StringDecay, 0.0f);
    const auto dead = play(*shortDecay, 60, 1.0);
    CHECK(level(dead, 0.3, 0.6) < level(dead, 0.0, 0.05) * 0.01);

    auto stop = makeTension();
    stop->setParameter(T::StringRatio, 1.0f);
    const auto released = play(*stop, 60, 1.0, 0.3);
    CHECK(level(released, 0.5, 0.9) < level(released, 0.1, 0.3) * 0.01);

    auto ring = makeTension();
    ring->setParameter(T::StringRatio, 0.0f);
    const auto ringing = play(*ring, 60, 1.0, 0.3);
    CHECK(level(ringing, 0.5, 0.9) > level(ringing, 0.1, 0.3) * 0.5);
}

TEST_CASE("Tension: every exciter and section stays finite at extreme settings")
{
    for (int exciter = 0; exciter < 4; ++exciter)
        for (float extreme : {0.0f, 1.0f})
        {
            auto t = makeTension();
            const auto& p = t->parameters();
            for (int i = 0; i < T::NumParams; ++i)
                if (p[(size_t) i].choices.empty() && ! p[(size_t) i].isToggle && p[(size_t) i].maxValue <= 1.0f)
                    t->setParameter(i, p[(size_t) i].minValue + (p[(size_t) i].maxValue - p[(size_t) i].minValue) * extreme);
            for (int toggle : {T::DamperOn, T::TermOn, T::PickupOn, T::BodyOn, T::FilterOn, T::FenvOn, T::LfoOn, T::VibratoOn, T::UnisonOn})
                t->setParameter(toggle, 1.0f);
            t->setParameter(T::ExcType, (float) exciter);
            t->setParameter(T::ExcOn, 1.0f);
            t->setParameter(T::Volume, 0.705f);
            const auto out = play(*t, 36 + 24 * (int) extreme, 1.5, 0.8, 127);
            CHECK(finiteAndBounded(out, 50.0f));
        }
}

TEST_CASE("Tension: polyphony limit and mono priority")
{
    auto t = makeTension();
    t->setParameter(T::Polyphony, 1); // 2 voices
    t->setParameter(T::StringRatio, 1.0f); // stolen notes stop at once
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    for (int note : {60, 64, 67}) midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
    buffer.clear();
    t->process(buffer, midi);
    CHECK(t->activeVoices() <= 3); // the stolen note is releasing
    juce::MidiBuffer none;
    for (int i = 0; i < 40; ++i) { buffer.clear(); t->process(buffer, none); }
    CHECK(t->activeVoices() <= 2);
}
