// PLG-18 / PLG-23: Sonora Stutter, the granular voice stutter.
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "devices/DeviceFactory.h"
#include "devices/native/BeatRepeat.h"
#include "devices/native/Stutter.h"
#include "plugins/SonoraPluginState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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

namespace
{
// A sung line: a 180 Hz voice with a 4 Hz syllable envelope that never pauses, from `from` to `to` seconds, over a
// -70 dBFS floor (a real vocal stem has no gaps the detector could call a pause).
juce::AudioBuffer<float> sung(int samples, double from, double to)
{
    juce::AudioBuffer<float> a(2, samples);
    juce::Random random(11);
    for (int s = 0; s < samples; ++s)
    {
        const double t = (double) s / kRate;
        float v = (random.nextFloat() * 2.0f - 1.0f) * 0.0003f;
        if (t >= from && t < to)
        {
            const double syllable = 0.55 + 0.45 * std::sin(2 * kPi * 4.0 * (t - from));
            v += (float) (0.4 * syllable * std::sin(2 * kPi * 180.0 * t) + 0.15 * syllable * std::sin(2 * kPi * 540.0 * t));
        }
        a.setSample(0, s, v);
        a.setSample(1, s, v);
    }
    return a;
}

devices::HostInfo playing(double tempo = 120.0)
{
    auto h = stopped(tempo);
    h.playing = true;
    return h;
}

// Runs in blocks (the transport advancing when it plays) and records, per block, what Auto did.
struct AutoTrace
{
    std::vector<bool> engaged, resting;
    std::vector<int> modes, moves;
    std::vector<double> position;
    std::vector<S::View> views; // every 10th block
    size_t at(double seconds) const { return (size_t) (seconds * kRate / 480.0); }
};
AutoTrace runTraced(S& s, juce::AudioBuffer<float>& audio, devices::HostInfo host, int block = 480)
{
    AutoTrace trace;
    const double beatsPerSample = host.tempo / 60.0 / kRate;
    for (int start = 0, index = 0; start < audio.getNumSamples(); start += block, ++index)
    {
        const int n = std::min(block, audio.getNumSamples() - start);
        if (host.playing) host.beats = (double) start * beatsPerSample;
        s.setHostInfo(host);
        juce::AudioBuffer<float> view(audio.getArrayOfWritePointers(), 2, start, n);
        juce::MidiBuffer midi;
        s.process(view, midi);
        devices::Stutter::View v;
        s.readView(v);
        trace.engaged.push_back(v.autoEngaged);
        trace.resting.push_back(v.autoResting);
        trace.modes.push_back(v.mode);
        trace.moves.push_back(v.autoMove);
        trace.position.push_back(v.autoPosition);
        if (index % 10 == 0) trace.views.push_back(v);
    }
    return trace;
}

double rmsOf(const juce::AudioBuffer<float>& a, double from, double to)
{
    double sum = 0;
    const int a0 = (int) (from * kRate), a1 = (int) (to * kRate);
    for (int i = a0; i < a1; ++i) sum += (double) a.getSample(0, i) * a.getSample(0, i);
    return std::sqrt(sum / std::max(1, a1 - a0));
}

int moveNamed(const S::Preset& p, const std::string& label)
{
    for (int i = 0; i < p.count; ++i)
        if (label == p.moves[(size_t) i].label) return i;
    return -1;
}
} // namespace

TEST_CASE("stutter Auto presets: the tables are sound and the moves ramp")
{
    for (int k = 0; k < S::kPresets; ++k)
    {
        const auto& p = S::preset(k);
        CHECK(std::string(p.name).size() > 2);
        CHECK(std::string(p.blurb).rfind(p.name, 0) == 0); // the help line starts with the name
        CHECK(p.count > 0);
        CHECK(p.cycleBeats >= 8.0);
        bool tierOne = false;
        for (int i = 0; i < p.count; ++i)
        {
            const auto& m = p.moves[(size_t) i];
            CHECK(m.at >= 0.0f);
            CHECK(m.length > 0.0f);
            CHECK((double) (m.at + m.length) <= p.cycleBeats + 1e-6);
            CHECK(m.tier >= 1);
            CHECK(m.tier <= 3);
            CHECK(std::string(m.label).size() > 0);
            if (i > 0) CHECK(m.at >= p.moves[(size_t) i - 1].at); // sorted
            tierOne = tierOne || m.tier == 1;
        }
        CHECK(tierOne); // something plays at the lowest Amount
    }
    const S probe;
    const auto names = probe.parameters()[S::AutoPreset].choices;
    REQUIRE(names.size() == (size_t) S::kPresets);
    CHECK(names[0] == "Vocal Chop");
    CHECK(probe.parameters()[S::AutoPreset].defaultValue < 0.5f); // Vocal Chop by default

    // Tape Stop winds down: real time and in tune at the start, stopped two octaves down at the end.
    const auto& tape = S::preset(3);
    const int stop = moveNamed(tape, "TAPE STOP");
    REQUIRE(stop >= 0);
    auto a = S::moveSettings(tape.moves[(size_t) stop], 0.0f, 120.0);
    auto b = S::moveSettings(tape.moves[(size_t) stop], 1.0f, 120.0);
    CHECK_NEAR(a.speed, 1.0f, 1e-5);
    CHECK_NEAR(a.pitch, 0.0f, 1e-5);
    CHECK_NEAR(b.speed, 0.0f, 1e-5);
    CHECK_NEAR(b.pitch, -24.0f, 1e-5);
    // Build-Up rolls faster every bar.
    const auto& build = S::preset(2);
    CHECK_NEAR(S::moveSettings(build.moves[(size_t) moveNamed(build, "1/8")], 0.5f, 120.0).everyBeats, 0.5, 1e-9);
    CHECK_NEAR(S::moveSettings(build.moves[(size_t) moveNamed(build, "1/16")], 0.5f, 120.0).everyBeats, 0.25, 1e-9);
    CHECK_NEAR(S::moveSettings(build.moves[(size_t) moveNamed(build, "1/32")], 0.5f, 120.0).everyBeats, 0.125, 1e-9);
    CHECK_NEAR(S::moveSettings(build.moves[(size_t) moveNamed(build, "1/64")], 0.5f, 120.0).everyBeats, 1.0 / 16, 1e-9);
    // moveAt / nextMove: Vocal Chop at Amount 50 %.
    const auto& chop = S::preset(0);
    CHECK(S::moveAt(chop, 1.0, 2) == -1);
    CHECK(S::moveAt(chop, 7.5, 2) == moveNamed(chop, "ROLL"));
    CHECK(S::moveAt(chop, 10.2, 2) == -1);                       // GLITCH is tier 3
    CHECK(S::moveAt(chop, 10.2, 3) == moveNamed(chop, "GLITCH"));
    double away = 0;
    CHECK(S::nextMove(chop, 4.0, 2, away) == moveNamed(chop, "ROLL"));
    CHECK_NEAR(away, 3.0, 1e-9);
    CHECK(S::nextMove(chop, 15.5, 1, away) == moveNamed(chop, "ROLL")); // wraps to the next cycle
    CHECK_NEAR(away, 7.5, 1e-9);
}

TEST_CASE("stutter Auto presets: a sung line with no pauses plays the moves on the bars, dry in between")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::Auto, 1.0f); // one click: Vocal Chop, Bars, Amount 50 %
    auto audio = sung(17 * 48000, 0.0, 17.0);
    const auto in = audio;
    const auto trace = runTraced(s, audio, playing()); // 120 BPM: a beat is 0.5 s, the 4-bar cycle 8 s
    const auto& chop = S::preset(0);
    const auto engagedAt = [&](double seconds) { return (bool) trace.engaged[trace.at(seconds)]; };
    const auto moveAtTime = [&](double seconds) { return trace.moves[trace.at(seconds)]; };
    for (double cycle : {0.0, 8.0})
    {
        CHECK(! engagedAt(cycle + 1.0));                                    // bar 1: dry
        CHECK(engagedAt(cycle + 1.85));                                     // 1/16 at beat 3.5 (tier 2)
        CHECK(moveAtTime(cycle + 1.85) == moveNamed(chop, "1/16"));
        CHECK(! engagedAt(cycle + 2.5));
        CHECK(engagedAt(cycle + 3.75));                                     // ROLL on beat 4 of bar 2
        CHECK(trace.modes[trace.at(cycle + 3.75)] == S::Retrigger);
        CHECK(! engagedAt(cycle + 5.1));                                    // GLITCH is tier 3: not at 50 %
        CHECK(engagedAt(cycle + 6.5));                                      // STRETCH
        CHECK(trace.modes[trace.at(cycle + 6.5)] == S::Swell);
        CHECK(engagedAt(cycle + 7.25));                                     // REWIND
        CHECK(trace.modes[trace.at(cycle + 7.25)] == S::Rewind);
        CHECK(engagedAt(cycle + 7.75));                                     // BUILD
        CHECK(moveAtTime(cycle + 7.75) == moveNamed(chop, "BUILD"));
    }
    CHECK_NEAR(trace.position[trace.at(9.0)], 2.0, 0.05); // 9 s = beat 18 = beat 2 of the second cycle
    // Between the moves the voice passes untouched.
    for (int i = (int) (0.6 * kRate); i < (int) (1.6 * kRate); i += 97) CHECK_NEAR(audio.getSample(0, i), in.getSample(0, i), 1e-6);
    for (int i = (int) (4.2 * kRate); i < (int) (5.9 * kRate); i += 97) CHECK_NEAR(audio.getSample(0, i), in.getSample(0, i), 1e-6);
    // The moves change the sound.
    double difference = 0;
    for (int i = (int) (3.55 * kRate); i < (int) (3.95 * kRate); ++i) difference += std::abs(audio.getSample(0, i) - in.getSample(0, i));
    CHECK(difference / (0.4 * kRate) > 0.02);
}

TEST_CASE("stutter Auto presets: only while the voice is there, grabbing the last syllable")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::Auto, 1.0f);
    auto audio = sung(9 * 48000, 0.0, 3.0); // the voice stops at 3.0 s (beat 6)
    const auto in = audio;
    const auto trace = runTraced(s, audio, playing());
    CHECK(trace.engaged[trace.at(3.75)]);    // ROLL at beat 7: the voice stopped a beat ago, it still plays
    CHECK(! trace.engaged[trace.at(6.5)]);   // STRETCH at beat 12: three seconds of silence, skipped
    CHECK(trace.resting[trace.at(6.5)]);
    CHECK(! trace.engaged[trace.at(7.75)]);
    // The roll repeats the end of the line (read just before the voice stopped) over the silence.
    CHECK(rmsOf(in, 3.55, 3.95) < 0.001);
    CHECK(rmsOf(audio, 3.55, 3.95) > 0.03);
    bool readTheLine = false;
    for (const auto& v : trace.views)
        if (v.autoEngaged)
            for (const auto& g : v.grains)
                if (g.at > (int64_t) (3.5 * kRate)) readTheLine = readTheLine || g.read < (int64_t) (3.04 * kRate);
    CHECK(readTheLine);

    { // A long move starts late when the voice comes in during it (Cloud: the whole 4 bars).
        S late;
        late.prepare(kRate, 256);
        late.setParameter(S::Auto, 1.0f);
        late.setParameter(S::AutoPreset, 7.0f); // Cloud
        late.setParameter(S::AutoAmount, 0.0f);
        auto a = sung(8 * 48000, 1.2, 8.0);
        const auto t = runTraced(late, a, playing());
        CHECK(! t.engaged[t.at(1.0)]);
        CHECK(t.engaged[t.at(1.6)]);
        CHECK(t.modes[t.at(1.6)] == S::Spread);
    }
    { // No voice at all: never engages.
        S quiet;
        quiet.prepare(kRate, 256);
        quiet.setParameter(S::Auto, 1.0f);
        auto silence = sung(9 * 48000, 100.0, 101.0);
        const auto t = runTraced(quiet, silence, playing());
        CHECK(std::none_of(t.engaged.begin(), t.engaged.end(), [](bool e) { return e; }));
    }
}

TEST_CASE("stutter Auto presets: Amount, Follow Voice, a stopped transport, and the hand wins")
{
    { // Amount: light plays only the main moves; wild plays them all.
        S light;
        light.prepare(kRate, 256);
        light.setParameter(S::Auto, 1.0f);
        light.setParameter(S::AutoAmount, 0.0f);
        auto a = sung(8 * 48000, 0.0, 8.0);
        auto t = runTraced(light, a, playing());
        CHECK(! t.engaged[t.at(1.85)]); // 1/16 (tier 2)
        CHECK(t.engaged[t.at(3.75)]);   // ROLL (tier 1)
        CHECK(! t.engaged[t.at(7.25)]); // REWIND (tier 2)
        S wild;
        wild.prepare(kRate, 256);
        wild.setParameter(S::Auto, 1.0f);
        wild.setParameter(S::AutoAmount, 1.0f);
        auto b = sung(8 * 48000, 0.0, 8.0);
        t = runTraced(wild, b, playing());
        CHECK(t.engaged[t.at(5.1)]);    // GLITCH (tier 3)
        CHECK(t.modes[t.at(5.1)] == S::Shatter);
    }
    { // Transport stopped: the cycle starts when the voice comes in.
        S s;
        s.prepare(kRate, 256);
        s.setParameter(S::Auto, 1.0f);
        auto a = sung(7 * 48000, 1.3, 7.0);
        const auto t = runTraced(s, a, stopped());
        CHECK(t.position[t.at(1.0)] < 0);         // no voice yet: no clock
        CHECK_NEAR(t.position[t.at(2.3)], 2.0, 0.05); // a second after the voice came in = 2 beats
        CHECK(! t.engaged[t.at(4.6)]);
        CHECK(t.engaged[t.at(5.05)]);             // ROLL: beat 7 after the voice came in (1.3 + 3.5 s)
    }
    { // Follow Voice with the transport playing: the cycle starts on the beat nearest to the voice coming in.
        S s;
        s.prepare(kRate, 256);
        s.setParameter(S::Auto, 1.0f);
        s.setParameter(S::AutoFollow, (float) S::FollowVoice);
        auto a = sung(7 * 48000, 1.3, 7.0); // comes in at beat 2.6: the cycle starts on beat 3 (1.5 s)
        const auto t = runTraced(s, a, playing());
        CHECK(! t.engaged[t.at(3.75)]);           // where Bars would have rolled
        CHECK(! t.engaged[t.at(4.9)]);
        CHECK(t.engaged[t.at(5.25)]);             // ROLL at 1.5 + 3.5 s
    }
    { // Hold wins over Auto
        S s;
        s.prepare(kRate, 256);
        s.setParameter(S::Auto, 1.0f);
        s.setParameter(S::Hold, 1.0f);
        s.setParameter(S::ModeParam, (float) S::Rewind);
        auto a = sung(5 * 48000, 0.0, 5.0);
        const auto t = runTraced(s, a, playing());
        for (size_t b = 0; b < t.engaged.size(); b += 7) CHECK(! t.engaged[b]);
        devices::Stutter::View v;
        s.readView(v);
        CHECK(v.engaged);
        CHECK(v.mode == S::Rewind);
    }
}

TEST_CASE("stutter: a NaN from the host counts as silence and does not stop Auto")
{
    S s;
    s.prepare(kRate, 256);
    s.setParameter(S::Auto, 1.0f);
    juce::AudioBuffer<float> bad(2, 512);
    for (int i = 0; i < 512; ++i) bad.setSample(0, i, std::numeric_limits<float>::quiet_NaN());
    bad.clear(1, 0, 512);
    juce::MidiBuffer midi;
    s.setHostInfo(playing());
    s.process(bad, midi);
    auto audio = sung(5 * 48000, 0.0, 5.0);
    const auto t = runTraced(s, audio, playing());
    CHECK(t.engaged[t.at(3.75)]);
    bool finite = true;
    for (int i = 0; i < audio.getNumSamples(); ++i) finite = finite && std::isfinite(audio.getSample(0, i));
    CHECK(finite);
}

TEST_CASE("stutter Auto presets: every preset plays every move, finite and bounded")
{
    for (int k = 0; k < S::kPresets; ++k)
    {
        const auto& p = S::preset(k);
        S s;
        s.prepare(kRate, 256);
        s.setParameter(S::Auto, 1.0f);
        s.setParameter(S::AutoPreset, (float) k);
        s.setParameter(S::AutoAmount, 1.0f);
        const double seconds = p.cycleBeats * 0.5 + 0.5;
        auto audio = sung((int) (seconds * kRate), 0.0, seconds);
        const auto t = runTraced(s, audio, playing());
        for (int i = 0; i < p.count; ++i)
        {
            const auto& m = p.moves[(size_t) i];
            const double middle = (double) (m.at + m.length * 0.5f);
            if (S::moveAt(p, middle, 3) != i) continue; // covered by a later move
            const double at = middle * 0.5;
            CHECK(t.engaged[t.at(at)]);
            CHECK(t.moves[t.at(at)] == i);
            CHECK(t.modes[t.at(at)] == m.mode);
        }
        float peak = 0;
        bool finite = true;
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const float v = audio.getSample(0, i);
            finite = finite && std::isfinite(v);
            peak = std::max(peak, std::abs(v));
        }
        CHECK(finite);
        CHECK(peak > 0.05f);
        // Never louder than the voice (0.55 peak), give or take the dry voice kept under a move (Mix < 100 %).
        CHECK(peak < 0.55f * (k == 7 || k == 1 ? 1.6f : 1.15f));
    }
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
