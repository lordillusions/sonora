// PLG-18: Sonora Stutter, the vocal stutter / retrigger effect.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/BeatRepeat.h"
#include "devices/native/Stutter.h"
#include "plugins/SonoraPluginState.h"

#include <cmath>
#include <vector>

using namespace sonora;
namespace
{
using S = devices::Stutter;
constexpr double kRate = 48000.0;

// A ramp (sample index / n) on both channels: every sample tells where it came from.
juce::AudioBuffer<float> ramp(int n)
{
    juce::AudioBuffer<float> a(2, n);
    for (int s = 0; s < n; ++s)
        for (int ch = 0; ch < 2; ++ch) a.setSample(ch, s, (float) s / (float) n);
    return a;
}

juce::AudioBuffer<float> run(devices::DeviceProcessor& s, const juce::AudioBuffer<float>& input, devices::HostInfo host, int block = 256,
                             std::vector<std::pair<int, juce::MidiMessage>> notes = {})
{
    juce::AudioBuffer<float> out(input);
    const double beatsPerSample = host.tempo / 60.0 / kRate;
    const double startBeat = host.beats;
    for (int start = 0; start < out.getNumSamples(); start += block)
    {
        const int n = std::min(block, out.getNumSamples() - start);
        juce::MidiBuffer midi;
        for (const auto& [at, message] : notes)
            if (at >= start && at < start + n) midi.addEvent(message, at - start);
        host.beats = startBeat + (double) start * beatsPerSample;
        s.setHostInfo(host);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, n);
        s.process(view, midi);
    }
    return out;
}

void setup(S& s)
{
    s.prepare(kRate, 256);
    s.setParameter(S::Smooth, 0.5f);
}

devices::HostInfo stopped(double tempo = 120.0)
{
    devices::HostInfo h;
    h.tempo = tempo;
    h.sampleRate = kRate;
    h.playing = false;
    return h;
}
} // namespace

TEST_CASE("stutter: idle, the input passes untouched; slice length follows Rate and tempo")
{
    S s;
    setup(s);
    const auto in = ramp(4800);
    const auto out = run(s, in, stopped());
    for (int i = 0; i < in.getNumSamples(); ++i) CHECK_NEAR(out.getSample(0, i), in.getSample(0, i), 0.0);
    s.setParameter(S::Rate, 6.0f); // 1/16 at 120 BPM = 0.125 s
    s.setHostInfo(stopped());
    CHECK_NEAR(s.sliceSamples(), 0.125 * kRate, 1.0);
    s.setParameter(S::Sync, 0.0f);
    s.setParameter(S::LengthMs, 50.0f);
    CHECK_NEAR(s.sliceSamples(), 0.05 * kRate, 1.0);
    CHECK(model::findCatalogEntry("SonoraStutter") != nullptr);
}

TEST_CASE("stutter: Repeat loops the slice that starts where it was engaged")
{
    S s;
    setup(s);
    s.setParameter(S::Hold, 1.0f); // engaged from the first sample, transport stopped: no quantize wait
    const int n = 48000;
    const auto in = ramp(n);
    const auto out = run(s, in, stopped());
    const int slice = (int) std::lround(0.125 * kRate);
    // First pass: the live input. Then the same slice again and again.
    CHECK_NEAR(out.getSample(0, 1000), in.getSample(0, 1000), 1e-4);
    for (int repeat = 1; repeat < 7; ++repeat)
        CHECK_NEAR(out.getSample(0, repeat * slice + 1000), in.getSample(0, 1000), 2e-4);
}

TEST_CASE("stutter: Reverse plays the slice before the engage point backwards")
{
    S s;
    setup(s);
    const int n = 24000, engageAt = 12000;
    const auto in = ramp(n);
    juce::AudioBuffer<float> out(in);
    // Run the first half idle, then hold Reverse.
    juce::AudioBuffer<float> first(out.getArrayOfWritePointers(), 2, 0, engageAt), second(out.getArrayOfWritePointers(), 2, engageAt, n - engageAt);
    juce::MidiBuffer none;
    s.setHostInfo(stopped());
    s.process(first, none);
    s.setParameter(S::ModeParam, (float) S::Reverse);
    s.setParameter(S::Hold, 1.0f);
    s.process(second, none);
    const int slice = (int) std::lround(0.125 * kRate);
    // 100 samples in, we hear the input from 101 samples before the engage point.
    CHECK_NEAR(out.getSample(0, engageAt + 100), in.getSample(0, engageAt - 101), 2e-4);
    CHECK(out.getSample(0, engageAt + 200) < out.getSample(0, engageAt + 100)); // going backwards
    CHECK_NEAR(out.getSample(0, engageAt + slice + 100), in.getSample(0, engageAt - 101), 2e-4); // and again
}

TEST_CASE("stutter: Shrink makes each repeat shorter, Decay quieter, Pitch reads faster")
{
    S s;
    setup(s);
    s.setParameter(S::Hold, 1.0f);
    s.setParameter(S::Shrink, 1.0f); // each repeat half the one before
    juce::AudioBuffer<float> in(2, 48000);
    in.clear();
    in.setSample(0, 100, 1.0f); // one click 100 samples into the slice (past the edge fade)
    const auto out = run(s, in, stopped());
    // Repeats start at 1/16 s (6000), then each half as long: 9000, 10500.
    CHECK(out.getSample(0, 6000 + 100) > 0.9f);
    CHECK(out.getSample(0, 9000 + 100) > 0.9f);
    CHECK(out.getSample(0, 10500 + 100) > 0.9f);
    CHECK(std::abs(out.getSample(0, 12000 + 100)) < 1e-6f); // the old spacing would have put it here

    S d;
    setup(d);
    d.setParameter(S::Hold, 1.0f);
    d.setParameter(S::Decay, 0.5f);
    juce::AudioBuffer<float> dc(2, 30000);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(dc.getWritePointer(ch), 0.8f, 30000);
    const auto decayed = run(d, dc, stopped());
    CHECK_NEAR(decayed.getSample(0, 6000 + 3000), 0.4f, 1e-3);   // repeat 1: half
    CHECK_NEAR(decayed.getSample(0, 12000 + 3000), 0.2f, 1e-3);  // repeat 2: a quarter

    S p;
    setup(p);
    p.setParameter(S::Hold, 1.0f);
    p.setParameter(S::Pitch, 12.0f);
    const auto in2 = ramp(24000);
    const auto pitched = run(p, in2, stopped());
    // Repeat 1, 1000 samples in: an octave up reads 2000 samples into the slice.
    CHECK_NEAR(pitched.getSample(0, 6000 + 1000), in2.getSample(0, 2000), 2e-4);
}

TEST_CASE("stutter: Stop slows down to silence over one slice, Gate chops the live input")
{
    S s;
    setup(s);
    s.setParameter(S::ModeParam, (float) S::Stop);
    s.setParameter(S::Hold, 1.0f);
    s.setParameter(S::Rate, 2.0f); // 1/4 = 0.5 s at 120 BPM
    const auto in = ramp(48000);
    const auto out = run(s, in, stopped());
    // The tape lags further and further behind, then stops.
    CHECK(out.getSample(0, 12000) < in.getSample(0, 12000) - 0.05f);
    CHECK(out.getSample(0, 20000) > out.getSample(0, 12000)); // still moving forwards
    CHECK_NEAR(out.getSample(0, 30000), 0.0f, 1e-6);          // stopped after the slice

    S g;
    setup(g);
    g.setParameter(S::ModeParam, (float) S::Gate);
    g.setParameter(S::Hold, 1.0f);
    g.setParameter(S::Spacing, 0.5f);
    juce::AudioBuffer<float> dc(2, 24000);
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(dc.getWritePointer(ch), 0.5f, 24000);
    const auto gated = run(g, dc, stopped());
    CHECK_NEAR(gated.getSample(0, 1500), 0.5f, 1e-4); // first half of each 1/16: open
    CHECK_NEAR(gated.getSample(0, 4500), 0.0f, 1e-4); // second half: closed
    CHECK_NEAR(gated.getSample(0, 7500), 0.5f, 1e-4);
}

TEST_CASE("stutter: the pattern fires on its steps while the transport plays; Hold waits for the quantize grid")
{
    S s;
    setup(s);
    s.setParameter(S::PatternOn, 1.0f);
    for (int i = 0; i < S::kSteps; ++i) s.setParameter(S::stepParam(i), 0.0f);
    s.setParameter(S::stepParam(4), 1.0f + (float) S::Gate); // beat 2 (steps of 1/16)
    s.setParameter(S::Spacing, 0.5f);
    auto host = stopped();
    host.playing = true;
    juce::AudioBuffer<float> dc(2, 48000); // one bar at 120 BPM = 2 s; this is the first second
    for (int ch = 0; ch < 2; ++ch) juce::FloatVectorOperations::fill(dc.getWritePointer(ch), 0.5f, 48000);
    const auto out = run(s, dc, host);
    const int sixteenth = 6000;
    CHECK_NEAR(out.getSample(0, 3 * sixteenth + 4500), 0.5f, 1e-4); // step 4: untouched
    CHECK_NEAR(out.getSample(0, 4 * sixteenth + 4500), 0.0f, 1e-4); // step 5: gated (second half closed)
    CHECK_NEAR(out.getSample(0, 5 * sixteenth + 4500), 0.5f, 1e-4); // step 6: untouched again

    S q;
    setup(q);
    q.setParameter(S::ModeParam, (float) S::Gate);
    q.setParameter(S::Spacing, 0.5f);
    q.setParameter(S::Quantize, 3.0f); // 1/4
    q.setParameter(S::Hold, 1.0f);
    auto late = host;
    late.beats = 0.1; // engaged just after beat 0: waits for beat 1 (0.45 s later)
    const auto waited = run(q, dc, late);
    CHECK_NEAR(waited.getSample(0, 20000), 0.5f, 1e-4);           // before the grid: dry
    CHECK_NEAR(waited.getSample(0, 21600 + 4500), 0.0f, 1e-4);    // after it: gated
}

TEST_CASE("stutter: MIDI notes engage modes; C3 and up play pitched repeats of what came before")
{
    S s;
    setup(s);
    const auto in = ramp(24000);
    // C1 (36) holds Repeat from sample 3000 to 15000.
    const auto out = run(s, in, stopped(), 256,
                         {{3000, juce::MidiMessage::noteOn(1, 36, 1.0f)}, {15000, juce::MidiMessage::noteOff(1, 36)}});
    CHECK_NEAR(out.getSample(0, 1000), in.getSample(0, 1000), 1e-6);
    // The engage lands on the block holding the note (3000 is inside the block starting at 2816) at the note's sample.
    CHECK_NEAR(out.getSample(0, 3000 + 6000 + 500), in.getSample(0, 3000 + 500), 2e-4);
    CHECK_NEAR(out.getSample(0, 20000), in.getSample(0, 20000), 1e-4); // released

    S p;
    setup(p);
    const auto pitched = run(p, in, stopped(), 256, {{12000, juce::MidiMessage::noteOn(1, 72, 1.0f)}}); // C4: +12
    // Right away an octave up, from the slice that ended at the note: 500 samples in reads 1000 into it.
    CHECK_NEAR(pitched.getSample(0, 12000 + 500), in.getSample(0, 12000 - 6000 + 1000), 2e-4);
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
    a.setParameter(S::stepParam(3), 2.0f);
    const auto bytes = plugins::encodeStutterState(a);
    S b;
    std::string error;
    REQUIRE(plugins::decodeStutterState(bytes.data(), bytes.size(), b, error));
    for (int i = 0; i < S::NumParams; ++i) CHECK_NEAR(b.getParameter(i), a.getParameter(i), 1e-6);
    CHECK(! plugins::decodeStutterState("nope", 4, b, error));
}

TEST_CASE("stutter: Freeze holds the sound; every mode stays finite and bounded")
{
    for (int mode = 0; mode < S::kModes; ++mode)
    {
        S s;
    setup(s);
        s.setParameter(S::ModeParam, (float) mode);
        s.setParameter(S::Hold, 1.0f);
        s.setParameter(S::Tone, mode % 2 == 0 ? -0.6f : 0.6f);
        s.setParameter(S::Wide, 1.0f);
        s.setParameter(S::Ramp, 3.0f);
        juce::AudioBuffer<float> noise(2, 48000);
        juce::Random random(7);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i) noise.setSample(ch, i, random.nextFloat() * 1.6f - 0.8f);
        const auto out = run(s, noise, stopped(), 211);
        float peak = 0;
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i)
            {
                finite = finite && std::isfinite(out.getSample(ch, i));
                peak = std::max(peak, std::abs(out.getSample(ch, i)));
            }
        CHECK(finite);
        CHECK(peak < 2.0f);
        if (mode == S::Freeze) CHECK(peak > 0.05f); // grains keep sounding
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
    const auto out = run(b, in, host);
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
    const auto never = run(c, in, host);
    for (int i = 0; i < 96000; i += 997) CHECK_NEAR(never.getSample(0, i), in.getSample(0, i), 1e-6);
    c.setParameter(B::FilterOn, 0.0f);
    c.setParameter(B::InstantRepeat, 1.0f); // Repeat pressed: repeats at once even with Chance 0
    c.reset();
    const auto instant = run(c, in, host);
    CHECK(c.repeating());
    CHECK_NEAR(instant.getSample(0, 6000 + 1000), in.getSample(0, 6000 + 1000) + in.getSample(0, 1000), 1e-3); // Mix: both
}
