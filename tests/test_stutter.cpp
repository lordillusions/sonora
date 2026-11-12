// PLG-18 / PLG-23: Sonora Stutter, the granular voice stutter.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/BeatRepeat.h"
#include "devices/native/Stutter.h"
#include "plugins/SonoraPluginState.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace sonora;
namespace
{
using S = devices::Stutter;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// A ramp (sample index / n) on both channels: every sample tells where it came from.
juce::AudioBuffer<float> ramp(int n)
{
    juce::AudioBuffer<float> a(2, n);
    for (int s = 0; s < n; ++s)
        for (int ch = 0; ch < 2; ++ch) a.setSample(ch, s, (float) s / (float) n);
    return a;
}

// A sine at `low` Hz up to `switchAt`, at `high` Hz after it.
juce::AudioBuffer<float> twoTones(int n, int switchAt, double low, double high)
{
    juce::AudioBuffer<float> a(2, n);
    double phase = 0;
    for (int s = 0; s < n; ++s)
    {
        phase += 2.0 * kPi * (s < switchAt ? low : high) / kRate;
        for (int ch = 0; ch < 2; ++ch) a.setSample(ch, s, 0.5f * (float) std::sin(phase));
    }
    return a;
}

// Zero crossings per second of channel 0 over [from, to): twice the dominant frequency.
double crossingsHz(const juce::AudioBuffer<float>& a, int from, int to)
{
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if ((a.getSample(0, i - 1) < 0) != (a.getSample(0, i) < 0)) ++crossings;
    return crossings * 0.5 * kRate / (double) (to - from);
}

void run(devices::DeviceProcessor& s, juce::AudioBuffer<float>& out, int from, int to, devices::HostInfo host, int block = 256,
         std::vector<std::pair<int, juce::MidiMessage>> notes = {})
{
    const double beatsPerSample = host.tempo / 60.0 / kRate;
    for (int start = from; start < to; start += block)
    {
        const int n = std::min(block, to - start);
        juce::MidiBuffer midi;
        for (const auto& [at, message] : notes)
            if (at >= start && at < start + n) midi.addEvent(message, at - start);
        host.beats = (double) start * beatsPerSample;
        s.setHostInfo(host);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, n);
        s.process(view, midi);
    }
}

juce::AudioBuffer<float> runAll(devices::DeviceProcessor& s, const juce::AudioBuffer<float>& input, devices::HostInfo host)
{
    juce::AudioBuffer<float> out(input);
    run(s, out, 0, out.getNumSamples(), host);
    return out;
}

devices::HostInfo stopped(double tempo = 120.0)
{
    devices::HostInfo h;
    h.tempo = tempo;
    h.sampleRate = kRate;
    h.playing = false;
    return h;
}

// Hand-set grains: no macro, no room, no jitter, the voice replaced.
void manual(S& s, int mode, float grainMs, float everyMs, float speed)
{
    s.prepare(kRate, 256);
    s.setParameter(S::ModeParam, (float) mode);
    s.setParameter(S::Macro, 0.0f);
    s.setParameter(S::Grain, grainMs);
    s.setParameter(S::Every, everyMs);
    s.setParameter(S::Speed, speed);
    s.setParameter(S::Shape, 0.5f);
    s.setParameter(S::Jitter, 0.0f);
    s.setParameter(S::Width, 0.0f);
    s.setParameter(S::Space, 0.0f);
    s.setParameter(S::Mix, 1.0f);
}
} // namespace

TEST_CASE("stutter: idle, the input passes untouched")
{
    S s;
    s.prepare(kRate, 256);
    auto out = ramp(48000);
    const auto in = out;
    run(s, out, 0, 48000, stopped());
    for (int i = 0; i < 48000; i += 331) CHECK_NEAR(out.getSample(0, i), in.getSample(0, i), 1e-7);
}

TEST_CASE("stutter: Freeze keeps playing what came before the engage point")
{
    S s;
    manual(s, S::Freeze, 60, 30, 0.0f);
    auto out = twoTones(96000, 48000, 200.0, 1000.0);
    const auto in = out;
    run(s, out, 0, 48000, stopped());
    s.setParameter(S::Hold, 1.0f);
    run(s, out, 48000, 96000, stopped());
    CHECK_NEAR(crossingsHz(in, 60000, 96000), 1000.0, 20.0); // the input went on at 1 kHz
    CHECK_NEAR(crossingsHz(out, 60000, 96000), 200.0, 25.0); // the stutter holds the 200 Hz from before
    devices::Stutter::View v;
    s.readView(v);
    CHECK(v.engaged);
    CHECK(v.mode == S::Freeze);
    CHECK(! v.grains.empty());
    CHECK(v.grains.back().read < 48000); // the grains read from before the engage point
    s.setParameter(S::Hold, 0.0f);
    run(s, out, 96000 - 4096, 96000, stopped());
    s.readView(v);
    CHECK(! v.engaged);
}

TEST_CASE("stutter: Speed moves the read head through the recording; negative goes backwards")
{
    for (const float speed : {0.25f, -0.5f})
    {
        S s;
        manual(s, S::Swell, 80, 40, speed);
        auto out = ramp(96000);
        run(s, out, 0, 48000, stopped());
        s.setParameter(S::Hold, 1.0f);
        run(s, out, 48000, 72000, stopped());
        devices::Stutter::View v;
        s.readView(v);
        const double span = 0.08 * kRate;
        const double expected = 48000.0 - span + speed * 24000.0; // half a second engaged
        CHECK_NEAR((double) v.head, expected, 600.0);
        CHECK(v.grains.size() > 8);
        // Grains start at the head: later grains read later (forward) or earlier (backwards).
        const auto& g = v.grains;
        CHECK(speed > 0 ? g.back().read > g[g.size() - 6].read : g.back().read < g[g.size() - 6].read);
    }
}

TEST_CASE("stutter: Rewind plays every grain backwards; Width alternates left and right")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::ModeParam, (float) S::Rewind);
    s.setParameter(S::Motion, 0.5f);
    s.setParameter(S::Hold, 1.0f);
    s.setParameter(S::Macro, 0.0f);
    s.setParameter(S::Width, 0.8f);
    s.setParameter(S::Jitter, 0.0f);
    s.setParameter(S::Every, 30.0f);
    auto out = ramp(48000);
    run(s, out, 0, 48000, stopped());
    devices::Stutter::View v;
    s.readView(v);
    REQUIRE(v.grains.size() >= 4);
    for (size_t i = v.grains.size() - 4; i < v.grains.size(); ++i) CHECK(v.grains[i].reversed);
    const auto& a = v.grains[v.grains.size() - 1];
    const auto& b = v.grains[v.grains.size() - 2];
    CHECK(a.pan * b.pan < 0.0f);
    CHECK_NEAR(std::abs(a.pan), 0.8f, 1e-5);
}

TEST_CASE("stutter: Retrigger with Sync repeats on the song's grid")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::ModeParam, (float) S::Retrigger);
    s.setParameter(S::Sync, 1.0f);
    s.setParameter(S::Motion, 0.5f); // 1/16
    auto host = stopped();
    host.playing = true;
    auto out = ramp(96000);
    run(s, out, 0, 30000, host);
    s.setParameter(S::Hold, 1.0f);
    run(s, out, 30000, 96000, host);
    devices::Stutter::View v;
    s.readView(v);
    CHECK(v.playing.fixedSlice);
    CHECK_NEAR(v.playing.everyBeats, 0.25, 1e-9);
    REQUIRE(v.grains.size() > 6);
    // After the first grain (at once), every grain starts on a 1/16 (6000 samples at 120 BPM).
    for (size_t i = v.grains.size() - 5; i < v.grains.size(); ++i)
    {
        CHECK_NEAR((double) (v.grains[i].at % 6000), 0.0, 256.0);
        CHECK_NEAR((double) (v.grains[i].at - v.grains[i - 1].at), 6000.0, 256.0);
        CHECK(v.grains[i].read == v.grains[i - 1].read); // the same slice, again and again
    }
}

TEST_CASE("stutter: Motion goes from long slow grains to short fast ones; detaching keeps the values")
{
    for (int mode = 0; mode < S::kModes; ++mode)
    {
        const auto slow = S::macroSettings(mode, 0.0f, 120.0, false), fast = S::macroSettings(mode, 1.0f, 120.0, false);
        CHECK(slow.grainMs > fast.grainMs);
        CHECK(slow.everyMs >= fast.everyMs);
        if (mode != S::Freeze && mode != S::Retrigger) CHECK(std::abs(slow.speed) < std::abs(fast.speed));
    }
    CHECK(S::macroSettings(S::Rewind, 0.5f, 120.0, false).reverseGrains);
    CHECK(S::macroSettings(S::Tunnel, 0.5f, 120.0, false).tunnelMs > 0);
    CHECK_NEAR(S::snapToNote(125.0, 120.0), 0.25, 1e-9);
    CHECK(std::string(S::noteName(0.25)) == "1/16");
    const auto synced = S::macroSettings(S::Swell, 0.5f, 120.0, true);
    CHECK(synced.everyBeats > 0);

    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::ModeParam, (float) S::Spread);
    s.setParameter(S::Motion, 0.8f);
    const auto before = s.settings();
    s.detachMacro();
    CHECK(s.getParameter(S::Macro) < 0.5f);
    const auto after = s.settings();
    CHECK_NEAR(after.grainMs, before.grainMs, 0.01);
    CHECK_NEAR(after.everyMs, before.everyMs, 0.01);
    CHECK_NEAR(after.speed, before.speed, 1e-4);
    CHECK_NEAR(after.width, before.width, 1e-5);
    CHECK_NEAR(after.detune, before.detune, 1e-5); // the mode keeps its character
}

TEST_CASE("stutter: MIDI white keys play a mode while held, black keys the selected one; the face's pad too")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::ModeParam, (float) S::Tunnel);
    auto out = ramp(48000);
    run(s, out, 0, 12000, stopped(), 256, {{4000, juce::MidiMessage::noteOn(1, 64, 1.0f)}}); // E: Shatter
    devices::Stutter::View v;
    s.readView(v);
    CHECK(v.engaged);
    CHECK(v.mode == S::Shatter);
    run(s, out, 12000, 24000, stopped(), 256, {{12000, juce::MidiMessage::noteOff(1, 64)}, {16000, juce::MidiMessage::noteOn(1, 61, 1.0f)}});
    s.readView(v);
    CHECK(v.engaged);
    CHECK(v.mode == S::Tunnel); // C#: the selected mode
    run(s, out, 24000, 30000, stopped(), 256, {{24000, juce::MidiMessage::noteOff(1, 61)}});
    s.readView(v);
    CHECK(! v.engaged);
    s.press(S::kPressCurrent);
    run(s, out, 30000, 36000, stopped());
    s.readView(v);
    CHECK(v.engaged);
    s.press(-1);
    run(s, out, 36000, 40000, stopped());
    s.readView(v);
    CHECK(! v.engaged);
}

TEST_CASE("stutter: catalog, factory and state round trip")
{
    const auto* entry = model::findCatalogEntry("SonoraStutter");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->displayName) == "Sonora Stutter");
    CHECK(model::deviceCategory(*entry) == "Delay & Reverb");
    const auto d = devices::nativeDeviceModel("SonoraStutter");
    CHECK(d.parameters.size() == (size_t) S::NumParams);
    CHECK(dynamic_cast<S*>(devices::createNative(d).get()) != nullptr);

    S a;
    a.setParameter(S::ModeParam, (float) S::Freeze);
    a.setParameter(S::Pitch, -7.0f);
    a.setParameter(S::Motion, 0.8f);
    const auto bytes = plugins::encodeStutterState(a);
    S b;
    std::string error;
    REQUIRE(plugins::decodeStutterState(bytes.data(), bytes.size(), b, error));
    for (int i = 0; i < S::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);
    CHECK(! plugins::decodeStutterState("nope", 4, b, error));
}

TEST_CASE("stutter: every mode stays finite and bounded, slow and fast")
{
    for (int mode = 0; mode < S::kModes; ++mode)
        for (const float motion : {0.0f, 1.0f})
        {
            S s;
            s.prepare(kRate, 256);
            s.setParameter(S::ModeParam, (float) mode);
            s.setParameter(S::Motion, motion);
            s.setParameter(S::Hold, 1.0f);
            s.setParameter(S::Tone, mode % 2 == 0 ? -0.6f : 0.6f);
            s.setParameter(S::Pitch, mode == 2 ? 12.0f : 0.0f);
            juce::AudioBuffer<float> noise(2, 72000);
            juce::Random random(7);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 72000; ++i) noise.setSample(ch, i, random.nextFloat() * 1.6f - 0.8f);
            run(s, noise, 0, 72000, stopped(), 211);
            float peak = 0, late = 0;
            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 72000; ++i)
                {
                    finite = finite && std::isfinite(noise.getSample(ch, i));
                    peak = std::max(peak, std::abs(noise.getSample(ch, i)));
                    if (i > 24000) late = std::max(late, std::abs(noise.getSample(ch, i)));
                }
            CHECK(finite);
            CHECK(peak < 3.0f);
            CHECK(late > 0.02f); // it keeps sounding
        }
}

// ---------------------------------------------------------------------------------------------------------------------
// PLG-18b: the reference's Beat Repeat on the same record-and-repeat idea.


TEST_CASE("beat repeat: index tables, catalog and factory")
{
    using B = devices::BeatRepeat;
    CHECK_NEAR(B::intervalBeats(0), 0.125, 1e-9); // 1/32
    CHECK_NEAR(B::intervalBeats(5), 4.0, 1e-9);   // 1 Bar
    CHECK_NEAR(B::gridBeats(6, false), 0.25, 1e-9); // 1/16
    CHECK_NEAR(B::gridBeats(7, false), 1.0 / 3.0, 1e-9); // 1/12 (triplet)
    CHECK_NEAR(B::gridBeats(7, true), 0.25, 1e-9);  // No Triplets: the straight value below
    CHECK_NEAR(B::gateBeats(7), 2.0, 1e-9);  // 8/16
    CHECK_NEAR(B::gateBeats(18), 16.0, 1e-9); // 4 Bars
    const auto* entry = model::findCatalogEntry("BeatRepeat");
    REQUIRE(entry != nullptr);
    CHECK(entry->implemented);
    auto d = devices::nativeDeviceModel("BeatRepeat");
    CHECK(dynamic_cast<B*>(devices::createNative(d).get()) != nullptr);
}

TEST_CASE("beat repeat: repeats one grid from each interval point until the gate ends; Insert mutes the input")
{
    using B = devices::BeatRepeat;
    B b;
    b.prepare(kRate, 256);
    b.setParameter(B::Interval, 5.0f); // 1 bar
    b.setParameter(B::Offset, 4.0f);   // at beat 1 (4 sixteenths in)
    b.setParameter(B::Grid, 6.0f);     // 1/16 = 6000 samples at 120 BPM
    b.setParameter(B::Gate, 3.0f);     // 4/16 = one beat
    b.setParameter(B::FilterOn, 0.0f);
    b.setParameter(B::MixType, (float) B::MixInsert);
    auto host = stopped();
    host.playing = true;
    const auto in = ramp(96000); // one bar
    const auto out = runAll(b, in, host);
    const int beat = 24000, grid = 6000;
    CHECK_NEAR(out.getSample(0, beat - 1000), in.getSample(0, beat - 1000), 1e-6); // before: the input
    CHECK_NEAR(out.getSample(0, beat + 1000), in.getSample(0, beat + 1000), 1e-6); // first grid: the input
    for (int r = 1; r < 4; ++r) CHECK_NEAR(out.getSample(0, beat + r * grid + 1000), in.getSample(0, beat + 1000), 2e-4);
    CHECK_NEAR(out.getSample(0, 2 * beat + 3000), in.getSample(0, 2 * beat + 3000), 1e-4); // gate over: the input again
    CHECK_NEAR(out.getSample(0, 70000), in.getSample(0, 70000), 1e-6); // the next point is the next bar

    B c;
    c.prepare(kRate, 256);
    c.setParameter(B::Interval, 3.0f); // every beat
    c.setParameter(B::Offset, 0.0f);
    c.setParameter(B::Chance, 0.0f);   // never
    const auto never = runAll(c, in, host);
    for (int i = 0; i < 96000; i += 997) CHECK_NEAR(never.getSample(0, i), in.getSample(0, i), 1e-6);
    c.setParameter(B::FilterOn, 0.0f);
    c.setParameter(B::InstantRepeat, 1.0f); // Repeat pressed: repeats at once even with Chance 0
    c.reset();
    const auto instant = runAll(c, in, host);
    CHECK(c.repeating());
    CHECK_NEAR(instant.getSample(0, 6000 + 1000), in.getSample(0, 6000 + 1000) + in.getSample(0, 1000), 1e-3); // Mix: both
}
