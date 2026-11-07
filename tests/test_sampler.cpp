// DEV-14b: Sonora Sampler (MultiSampler) — the parameter model, the laws it plays (zones, crossfades, round robin, loops,
// envelopes, LFOs, matrix, MIDI) and the .als round trip of every factory Sampler preset. docs/SAMPLER.md.
#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Sampler.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/als/DevicePresets.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <functional>
#include <set>

using namespace sonora;
using P = devices::Sampler;
using S = devices::Simpler;

namespace
{
constexpr double kRate = 44100.0;

std::shared_ptr<engine::SampleSlot> sampleOf(int frames, const std::function<float(int)>& f, int channels = 2)
{
    auto d = std::make_shared<engine::SampleData>();
    d->sampleRate = kRate;
    d->audio.setSize(channels, frames);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < frames; ++i) d->audio.setSample(ch, i, f(i));
    return engine::SampleSlot::makeReady(d);
}

model::SamplePart partOf(int frames, int keyMin = 0, int keyMax = 127, int velMin = 1, int velMax = 127)
{
    model::SamplePart p;
    p.keyMin = p.keyFadeMin = keyMin; p.keyMax = p.keyFadeMax = keyMax;
    p.velMin = p.velFadeMin = velMin; p.velMax = p.velFadeMax = velMax;
    p.rootKey = 60;
    p.sampleEnd = frames - 1;
    p.lengthSamples = frames;
    p.sampleRate = kRate;
    return p;
}

// A "flat" Sampler: 0 dB, no velocity scaling, instant attack, linear interpolation (exact reads of ramps).
std::unique_ptr<P> makeSampler(std::vector<P::Zone> zones)
{
    auto s = std::make_unique<P>(std::move(zones), 1);
    s->prepare(kRate, 512);
    for (auto [param, value] : {std::pair{(int) S::Volume, 0.0f}, {(int) S::VelToVolume, 0.0f}, {(int) S::FilterOn, 0.0f},
                                {(int) S::Interpolation, 0.0f}, {(int) S::Voices, 13.0f}, {(int) S::Snap, 0.0f}})
        s->setParameter(param, value);
    return s;
}

P::Zone zoneOf(model::SamplePart part, std::shared_ptr<engine::SampleSlot> slot)
{
    P::Zone z;
    z.part = std::move(part);
    z.slot = std::move(slot);
    return z;
}

// Renders `blocks` x 512 frames; `events(block, midi)` adds MIDI. Returns {left, right}.
std::pair<std::vector<float>, std::vector<float>> render(P& s, int blocks, const std::function<void(int, juce::MidiBuffer&)>& events)
{
    std::vector<float> left, right;
    juce::AudioBuffer<float> audio(2, 512);
    for (int b = 0; b < blocks; ++b)
    {
        audio.clear();
        juce::MidiBuffer midi;
        events(b, midi);
        s.process(audio, midi);
        left.insert(left.end(), audio.getReadPointer(0), audio.getReadPointer(0) + 512);
        right.insert(right.end(), audio.getReadPointer(1), audio.getReadPointer(1) + 512);
    }
    return {left, right};
}

std::function<void(int, juce::MidiBuffer&)> noteAt(int note, int velocity, int offBlock = -1, int channel = 1)
{
    return [=](int b, juce::MidiBuffer& m) {
        if (b == 0) m.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8) velocity), 0);
        if (b == offBlock) m.addEvent(juce::MidiMessage::noteOff(channel, note, (juce::uint8) 64), 0);
    };
}

int paramIndex(const devices::DeviceProcessor& p, const std::string& id)
{
    const auto& infos = p.parameters();
    for (size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == id) return (int) i;
    return -1;
}

bool eq(double a, double b) { return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(a)); }

// Zero crossings per second of a signal (frequency of a sine-like waveform).
double frequencyOf(const std::vector<float>& x, size_t from, size_t to)
{
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i) crossings += (x[i - 1] <= 0) != (x[i] <= 0) ? 1 : 0;
    return crossings / 2.0 / ((double) (to - from) / kRate);
}
} // namespace

TEST_CASE("sampler: parameters keep Simpler's indices and paths, then every Sampler control by its XML path")
{
    P sampler({});
    const auto simpler = S::parameterInfos();
    const auto& infos = sampler.parameters();
    REQUIRE(infos.size() == (size_t) P::NumParams);
    for (size_t i = 0; i < simpler.size(); ++i) CHECK(infos[i].id == simpler[i].id);
    std::set<std::string> ids;
    for (const auto& info : infos) ids.insert(info.id);
    for (const auto& info : infos) CHECK(! info.id.empty() && ! info.name.empty());
    CHECK(ids.size() == infos.size()); // every parameter has its own path
    // A few paths the face binds (docs/SAMPLER.md).
    for (const char* id : {"Player/SubOsc/Slot/Value/SimplerSubOsc/Envelope/AttackLevel", "AuxEnv/Slot/Value/SimplerAuxEnvelope/LoopMode",
                           "AuxLfos.1/Slot/Value/SimplerAuxLfo/Spin", "MidiCtrl.2/ModConnections.0/Connection", "KeyDst/ModConnections.1/Amount",
                           "Player/MultiSampleMap/RoundRobinMode", "Globals/EnvScale/EnvTimeKeyScale", "Globals/MpePitchBendRange",
                           "Shaper/Slot/Value/SimplerShaper/Structure", "Filter/Slot/Value/SimplerFilter/Envelope/ReleaseLevel",
                           "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/TimeVelScale", "Player/Reverse", "Player/SampleSelector"})
        CHECK(ids.count(id) == 1);
    // Matrix connections, Pitch Bend ranges and the map's settings are plain leaves (no <Manual>).
    CHECK(infos[(size_t) paramIndex(sampler, "VelDst/ModConnections.0/Amount")].plainValue);
    CHECK(infos[(size_t) paramIndex(sampler, "Globals/PitchBendRange")].plainValue);
    CHECK(! infos[(size_t) paramIndex(sampler, "Player/SampleSelector")].plainValue);
    // Two-state enums are written as numbers (schema TimeableEnum).
    CHECK(infos[(size_t) paramIndex(sampler, "AuxLfos.0/Slot/Value/SimplerAuxLfo/RateType")].intNode);
    CHECK(infos[(size_t) paramIndex(sampler, "Lfo/Slot/Value/SimplerLfo/RateType")].intNode);
    CHECK(infos[(size_t) paramIndex(sampler, "Shaper/Slot/Value/SimplerShaper/Structure")].isToggle);
    CHECK(! infos[(size_t) paramIndex(sampler, "Player/SubOsc/Slot/Value/SimplerSubOsc/Mode")].isToggle); // FM / AM enum
    CHECK(std::string(P::destinationName(P::DstLoopLength)) == "Loop Length"); // KeyDst 4 in the Lovely Keys capture
    CHECK(std::string(P::destinationName(P::DstVolume)) == "Volume");          // LFO 3 A = 18 in the capture
    CHECK(std::string(P::destinationName(P::DstFilterRes)) == "Filter Res");   // "Pressure -> filter resonance" = 14
    CHECK(sampler.name() == "Sonora Sampler");
}

TEST_CASE("sampler: zone crossfades are 1/(n+1) steps, adjacent linear fades sum to one, Pow is constant power")
{
    // Key zone 41..47 with full level 43..45 (a factory piano's layout).
    CHECK_NEAR(P::zoneFade(40, 41, 47, 43, 45, false), 0.0, 1e-9);
    CHECK_NEAR(P::zoneFade(41, 41, 47, 43, 45, false), 1.0 / 3, 1e-6);
    CHECK_NEAR(P::zoneFade(42, 41, 47, 43, 45, false), 2.0 / 3, 1e-6);
    CHECK_NEAR(P::zoneFade(44, 41, 47, 43, 45, false), 1.0, 1e-9);
    CHECK_NEAR(P::zoneFade(46, 41, 47, 43, 45, false), 2.0 / 3, 1e-6);
    CHECK_NEAR(P::zoneFade(48, 41, 47, 43, 45, false), 0.0, 1e-9);
    // The next zone 46..52 (full 48..50): the overlap sums to 1.
    for (int key : {46, 47}) CHECK_NEAR(P::zoneFade(key, 41, 47, 43, 45, false) + P::zoneFade(key, 46, 52, 48, 50, false), 1.0, 1e-6);
    CHECK_NEAR(P::zoneFade(41, 41, 47, 43, 45, true), std::sin(juce::MathConstants<double>::pi / 6), 1e-6);
    CHECK_NEAR(P::zoneFade(64, 0, 127, 0, 127, true), 1.0, 1e-9); // no fade
}

TEST_CASE("sampler: zones by key, velocity and Sample Select with crossfade gains; solo, inactive and Zone Shift")
{
    auto slot = sampleOf(4410, [](int) { return 0.5f; });
    auto soft = partOf(4410, 0, 127, 1, 74), loud = partOf(4410, 0, 127, 75, 127), sel = partOf(4410, 60, 72);
    sel.selMin = 64; sel.selMax = 127; sel.selFadeMin = 96; sel.selFadeMax = 127;
    auto off = partOf(4410); off.active = false;
    P sampler({zoneOf(soft, slot), zoneOf(loud, slot), zoneOf(sel, slot), zoneOf(off, slot)});
    std::array<int, P::kMaxLayers> idx{};
    std::array<float, P::kMaxLayers> gain{};
    CHECK(sampler.selectZones(40, 50, 0, idx.data(), gain.data()) == 1); CHECK(idx[0] == 0);
    CHECK(sampler.selectZones(40, 100, 0, idx.data(), gain.data()) == 1); CHECK(idx[0] == 1);
    // Selector 80 inside 64..127, fading in until 96: (80 - 64 + 1) / 33.
    REQUIRE(sampler.selectZones(64, 100, 80, idx.data(), gain.data()) == 2);
    CHECK(idx[1] == 2); CHECK_NEAR(gain[1], 17.0 / 33.0, 1e-6);
    // Key Zone Shift +12: note 76 selects with key 64 (Simpler's sign).
    sampler.setParameter(S::KeyZoneShift, 12);
    CHECK(sampler.selectZones(76, 100, 100, idx.data(), gain.data()) == 2);
    sampler.setParameter(S::KeyZoneShift, 0);
    // Solo: only soloed zones play.
    auto soloed = zoneOf(partOf(4410), slot); soloed.part.solo = true;
    P solo({zoneOf(partOf(4410), slot), soloed});
    CHECK(solo.selectZones(60, 100, 0, idx.data(), gain.data()) == 1); CHECK(idx[0] == 1);
}

TEST_CASE("sampler: round robin cycles the zones of a key zone Forward, Backward, Other (never twice) and Random")
{
    auto slot = sampleOf(4410, [](int) { return 0.5f; });
    std::vector<P::Zone> zones{zoneOf(partOf(4410, 60, 60), slot), zoneOf(partOf(4410, 60, 60), slot), zoneOf(partOf(4410, 60, 60), slot),
                               zoneOf(partOf(4410, 61, 61), slot)};
    P sampler(zones, 7);
    sampler.setParameter(P::RoundRobin, 1);
    std::array<int, P::kMaxLayers> idx{};
    std::array<float, P::kMaxLayers> gain{};
    std::vector<int> picks;
    for (int i = 0; i < 4; ++i) { CHECK(sampler.selectZones(60, 100, 0, idx.data(), gain.data()) == 1); picks.push_back(idx[0]); }
    CHECK((picks == std::vector<int>{0, 1, 2, 0}));
    CHECK(sampler.selectZones(61, 100, 0, idx.data(), gain.data()) == 1); CHECK(idx[0] == 3); // its own group
    P backward(zones, 7);
    backward.setParameter(P::RoundRobin, 1); backward.setParameter(P::RoundRobinMode, 1);
    picks.clear();
    for (int i = 0; i < 3; ++i) { backward.selectZones(60, 100, 0, idx.data(), gain.data()); picks.push_back(idx[0]); }
    CHECK((picks == std::vector<int>{2, 1, 0}));
    P other(zones, 7);
    other.setParameter(P::RoundRobin, 1); other.setParameter(P::RoundRobinMode, 2);
    int previous = -1;
    bool repeated = false;
    for (int i = 0; i < 50; ++i) { other.selectZones(60, 100, 0, idx.data(), gain.data()); repeated |= idx[0] == previous; previous = idx[0]; }
    CHECK(! repeated);
    // Without RR, zones sharing a key zone all play (layers).
    P layered(zones, 7);
    CHECK(layered.selectZones(60, 100, 0, idx.data(), gain.data()) == 3);
}

TEST_CASE("sampler: envelope levels (Initial / Peak / Sustain), Time < Key / Time / Time < Vel, loop and trigger modes")
{
    auto s = makeSampler({zoneOf(partOf(44100 * 2), sampleOf(44100 * 2, [](int) { return 0.5f; }))});
    // Initial 0 dB: the note starts at full level although the attack is long (the Lovely Keys preset).
    s->setParameter(P::AmpInitial, 1.0f); s->setParameter(S::Attack, 1000.0f);
    s->setParameter(P::AmpPeak, 1.0f); s->setParameter(S::Sustain, 0.5f); s->setParameter(S::Decay, 100.0f);
    auto [l, r] = render(*s, 4, noteAt(60, 100));
    CHECK_NEAR(l[10], 0.5, 1e-3);
    // Linear attack from silence to Peak 0.5 over 10 ms: half way at 5 ms.
    auto lin = makeSampler({zoneOf(partOf(44100), sampleOf(44100, [](int) { return 0.5f; }))});
    lin->setParameter(S::Attack, 10.0f); lin->setParameter(P::AmpPeak, 0.5f); lin->setParameter(S::Sustain, 0.5f);
    auto [a, ar] = render(*lin, 2, noteAt(60, 100));
    CHECK_NEAR(a[220], 0.5 * 0.5 * 0.5, 0.01);
    CHECK_NEAR(a[600], 0.25, 0.01); // Peak, then decays slowly towards Sustain 1
    // Time law (TODO(DEV-14b): unverified): +100 % = 4x, Time < Key -21 % one octave up = 2^-0.21, Time < Vel 100 % = 1/4.
    CHECK_NEAR(P::timeScale(100, 0, 60, 0, 0), 4.0, 1e-9);
    CHECK_NEAR(P::timeScale(0, -21, 72, 0, 0), std::exp2(-0.21), 1e-9);
    CHECK_NEAR(P::timeScale(0, 0, 60, 100, 1.0f), 0.25, 1e-9);
    // Trigger: note-off during the decay is ignored, the release follows the decay.
    auto trigger = makeSampler({zoneOf(partOf(44100), sampleOf(44100, [](int) { return 0.5f; }))});
    trigger->setParameter(S::AmpLoopMode, 4); trigger->setParameter(S::Decay, 50.0f); trigger->setParameter(S::Sustain, 1.0f);
    trigger->setParameter(S::Release, 1.0f);
    auto [t, tr] = render(*trigger, 8, noteAt(60, 100, 1));
    CHECK(t[600] > 0.4f);      // released at block 1, still playing (decay at 1.0)
    CHECK(std::abs(t[3000]) < 1e-3f); // released right after the decay (50 ms = 2205 frames)
    // Loop: after the decay the level returns to Initial over Loop Time, then attacks again.
    auto loop = makeSampler({zoneOf(partOf(44100), sampleOf(44100, [](int) { return 0.5f; }))});
    loop->setParameter(S::AmpLoopMode, 1); loop->setParameter(S::Decay, 10.0f); loop->setParameter(S::Sustain, 1.0f);
    loop->setParameter(S::AmpLoopTime, 20.0f); loop->setParameter(S::Attack, 0.1f);
    auto [lp, lpr] = render(*loop, 4, noteAt(60, 100));
    CHECK_NEAR(lp[441 + 441], 0.25, 0.02); // 10 ms into the 20 ms fall from 1 to 0
    CHECK(lp[441 + 882 + 20] > 0.45f);     // attacked again
}

TEST_CASE("sampler: sustain loop forward / back-and-forth and every Release Mode; Reverse; Link")
{
    const int frames = 20000;
    auto ramp = sampleOf(frames, [](int i) { return (float) i / 20000.0f; });
    auto loopPart = [&](int sustainMode, int releaseMode) {
        auto p = partOf(frames);
        p.loopStart = 4000; p.loopEnd = 6000; p.loopMode = sustainMode; p.releaseMode = releaseMode; p.releaseStart = 15000;
        return p;
    };
    const auto make = [&](model::SamplePart p) {
        auto s = makeSampler({zoneOf(std::move(p), ramp)});
        s->setParameter(S::Release, 60000.0f);
        return s;
    };
    const auto maxOf = [](const std::vector<float>& x, size_t from, size_t to) { float m = 0; for (size_t i = from; i < to; ++i) m = std::max(m, x[i]); return m; };
    const auto minOf = [](const std::vector<float>& x, size_t from, size_t to) { float m = 1; for (size_t i = from; i < to; ++i) m = std::min(m, x[i]); return m; };
    // Forward sustain loop, Release Mode off: loops inside 4000..6000 before and after note-off.
    auto off = make(loopPart(1, 3));
    auto [o, orr] = render(*off, 80, noteAt(60, 100, 20));
    CHECK(maxOf(o, 7000, 10240) < 0.3f + 1e-3f && minOf(o, 7000, 10240) > 0.2f - 1e-3f);
    CHECK(maxOf(o, 20000, 40000) < 0.3f + 1e-3f);
    // ->|: on release the loop lets go and plays on to the sample end, then the voice ends.
    auto on = make(loopPart(1, 0));
    auto [n, nr] = render(*on, 80, noteAt(60, 100, 20));
    CHECK(maxOf(n, 10240, 30000) > 0.9f);
    CHECK(maxOf(n, 35000, 40960) < 1e-9f);
    // Forward release loop: after the end it loops 15000 .. end.
    auto rel = make(loopPart(1, 1));
    auto [rl, rlr] = render(*rel, 80, noteAt(60, 100, 20));
    CHECK(minOf(rl, 34000, 40000) > 0.65f); // 0.75 .. 1 times the slow release
    CHECK(maxOf(rl, 34000, 40000) > 0.9f);
    // Back-and-forth sustain loop turns at the loop end and plays backwards.
    auto bf = make(loopPart(2, 3));
    auto [bb, bbr] = render(*bf, 20, noteAt(60, 100));
    CHECK_NEAR(bb[6000 + 500], (6000 - 500) / 20000.0, 0.002);
    // Reverse: from the sample end backwards.
    auto rev = make(partOf(frames));
    rev->setParameter(P::Reverse, 1);
    auto [rv, rvr] = render(*rev, 4, noteAt(60, 100));
    CHECK_NEAR(rv[100], (frames - 1 - 100) / 20000.0, 0.002);
    // Link: the sample starts at the loop start.
    auto linkPart = loopPart(1, 3); linkPart.link = true;
    auto link = make(linkPart);
    auto [lk, lkr] = render(*link, 1, noteAt(60, 100));
    CHECK_NEAR(lk[10], 4010 / 20000.0, 0.002);
}

TEST_CASE("sampler: Tune Scale, Pitch Bend range, Note PB (MPE) range and Portamento glide")
{
    // A 441 Hz sine (100 frames per cycle) with the root at C3.
    auto sine = sampleOf(44100 * 2, [](int i) { return (float) std::sin(2 * juce::MathConstants<double>::pi * i / 100.0); });
    auto s = makeSampler({zoneOf(partOf(44100 * 2), sine)});
    auto [c3, x] = render(*s, 8, noteAt(72, 100));
    CHECK_NEAR(frequencyOf(c3, 1000, 4000), 882.0, 10.0); // one octave up
    auto flat = partOf(44100 * 2); flat.tuneScale = 0;
    auto noTrack = makeSampler({zoneOf(flat, sine)});
    auto [n, nx] = render(*noTrack, 8, noteAt(72, 100));
    CHECK_NEAR(frequencyOf(n, 1000, 4000), 441.0, 6.0);
    // Pitch Bend range 12: full bend up = one octave.
    auto bend = makeSampler({zoneOf(partOf(44100 * 2), sine)});
    bend->setParameter(P::PitchBendRangeParam, 12);
    auto [b, bx] = render(*bend, 8, [](int block, juce::MidiBuffer& m) {
        if (block == 0) { m.addEvent(juce::MidiMessage::pitchWheel(1, 16383), 0); m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 1); }
    });
    CHECK_NEAR(frequencyOf(b, 1000, 4000), 882.0, 10.0);
    // Note PB on an MPE member channel uses the MPE range (48): +1/4 of the range = +12 st on that note only.
    auto mpe = makeSampler({zoneOf(partOf(44100 * 2), sine)});
    auto [mp, mx] = render(*mpe, 8, [](int block, juce::MidiBuffer& m) {
        if (block == 0) { m.addEvent(juce::MidiMessage::pitchWheel(2, 8192 + 2048), 0); m.addEvent(juce::MidiMessage::noteOn(2, 60, (juce::uint8) 100), 1); }
    });
    CHECK_NEAR(frequencyOf(mp, 1000, 4000), 882.0, 10.0);
    // Portamento: a new note glides from the previous note over Glide Time (linear in semitones).
    auto glide = makeSampler({zoneOf(partOf(44100 * 2), sine)});
    glide->setParameter(S::GlideMode, 2); glide->setParameter(S::GlideTime, 100.0f); glide->setParameter(S::Release, 1.0f);
    auto [g, gx] = render(*glide, 40, [](int block, juce::MidiBuffer& m) {
        if (block == 0) m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        if (block == 9) m.addEvent(juce::MidiMessage::noteOff(1, 60, (juce::uint8) 64), 0);
        if (block == 10) m.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 100), 0);
    });
    CHECK(frequencyOf(g, 10 * 512 + 100, 10 * 512 + 900) < 540.0);           // starts near the previous note (441 Hz)
    CHECK_NEAR(frequencyOf(g, 10 * 512 + 5000, 10 * 512 + 9000), 882.0, 12.0); // arrived after 100 ms
}

TEST_CASE("sampler: LFO 2 to Volume (tremolo), stereo Phase, Mod Wheel to Filter Freq, Velocity to Volume")
{
    auto flat = sampleOf(44100 * 3, [](int) { return 0.5f; });
    auto s = makeSampler({zoneOf(partOf(44100 * 3), flat)});
    // LFO 2: 10 Hz sine, A = Volume 50 %, Stereo Phase 180: left and right swing in opposite directions.
    s->setParameter(P::Lfo2 + P::LfoOnField, 1); s->setParameter(P::Lfo2 + P::LfoFrequency, 10);
    s->setParameter(P::Lfo2 + P::LfoPhase, 180);
    s->setParameter(P::Lfo2 + P::LfoFieldCount + P::ModDst0, (float) P::DstVolume);
    s->setParameter(P::Lfo2 + P::LfoFieldCount + P::ModAmount0, 50);
    auto [l, r] = render(*s, 20, noteAt(60, 100));
    float lo = 1, hi = 0;
    for (size_t i = 2000; i < 9000; ++i) { lo = std::min(lo, l[i]); hi = std::max(hi, l[i]); }
    CHECK_NEAR(hi, 0.5 * 1.5, 0.02);
    CHECK_NEAR(lo, 0.5 * 0.5, 0.02);
    CHECK_NEAR(l[3000] + r[3000], 1.0, 0.02); // opposite phases around 0.5
    // Mod Wheel (MidiCtrl.2) -> Filter Freq -100 %: closes a 2 kHz low-pass on a 2.2 kHz tone.
    auto tone = sampleOf(44100 * 2, [](int i) { return (float) std::sin(2 * juce::MathConstants<double>::pi * i * 2205.0 / kRate); });
    auto f = makeSampler({zoneOf(partOf(44100 * 2), tone)});
    f->setParameter(S::FilterOn, 1); f->setParameter(S::FilterFreq, 12000); f->setParameter(S::FilterKeyTrack, 0);
    f->setParameter(P::MidiMod + P::RowModWheel * P::ModFieldCount + P::ModDst0, (float) P::DstFilterFreq);
    f->setParameter(P::MidiMod + P::RowModWheel * P::ModFieldCount + P::ModAmount0, -100);
    const auto level = [&](int wheel) {
        f->reset();
        auto [x, y] = render(*f, 12, [wheel](int block, juce::MidiBuffer& m) {
            if (block == 0) { m.addEvent(juce::MidiMessage::controllerEvent(1, 1, wheel), 0); m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 1); }
        });
        float peak = 0;
        for (size_t i = 3000; i < 6000; ++i) peak = std::max(peak, std::abs(x[i]));
        return peak;
    };
    CHECK(level(0) > 0.8f);
    CHECK(level(127) < 0.05f);
    // Velocity (VelDst) -> Volume 100 %: velocity 127 doubles, velocity 1 keeps about the base level.
    auto v = makeSampler({zoneOf(partOf(44100), flat)});
    v->setParameter(P::VelMod + P::ModDst0, (float) P::DstVolume); v->setParameter(P::VelMod + P::ModAmount0, 100);
    auto [vl, vr] = render(*v, 2, noteAt(60, 127));
    CHECK_NEAR(vl[300], 1.0, 0.01);
}

TEST_CASE("sampler: modulation oscillator AM / FM, Shaper curves, Spread, all extremes stay finite")
{
    CHECK_NEAR(P::shape(0, 0.5f, 0), std::tanh(0.5), 1e-6);
    CHECK_NEAR(P::shape(1, 2.0f, 0), 1.0, 1e-6);
    CHECK_NEAR(P::shape(3, 0.5f, 0), std::round(0.5 * 7.5) / 7.5, 1e-6);
    CHECK_NEAR(P::oscRatio(3, 0), 3.0, 1e-9);
    CHECK_NEAR(P::oscRatio(-2, 0), 0.125, 1e-9); // the manual's 0.125 .. 48
    CHECK_NEAR(P::oscRatio(1, 500), 1.5, 1e-9);
    unsigned noise = 1;
    CHECK_NEAR(P::oscWave(0, 0.25, noise), 1.0, 1e-3);
    CHECK_NEAR(P::oscWave(19, 0.25, noise), 1.0, 1e-3); // triangle peak
    CHECK_NEAR(P::lfoBeatQuarters(6), 0.5, 1e-9);       // 1/8 (the Lovely Keys LFO 3 capture)
    auto flat = sampleOf(44100, [](int) { return 0.5f; });
    // AM at full volume with a fixed 10 Hz sine: the level swings 0 .. 2x.
    auto am = makeSampler({zoneOf(partOf(44100), flat)});
    for (auto [param, value] : {std::pair{(int) P::OscOn, 1.0f}, {(int) P::OscMode, 1.0f}, {(int) P::OscVolume, 1.0f}, {(int) P::OscFixed, 1.0f},
                                {(int) P::OscFixedFreq, 10.0f}, {(int) P::OscFixedMul, 3.0f}, {P::OscEnv + P::EnvInitial, 1.0f}})
        am->setParameter(param, value);
    auto [a, ar] = render(*am, 12, noteAt(60, 100));
    float hi = 0, lo = 1;
    for (size_t i = 100; i < 5000; ++i) { hi = std::max(hi, a[i]); lo = std::min(lo, a[i]); }
    CHECK(hi > 0.95f);
    CHECK(lo < 0.05f);
    // Every section on at extreme settings: finite output.
    auto tone = sampleOf(44100, [](int i) { return (float) std::sin(i * 0.05); });
    auto all = makeSampler({zoneOf(partOf(44100), tone)});
    const auto& infos = all->parameters();
    for (size_t i = 0; i < infos.size(); ++i)
        if (infos[i].isToggle) all->setParameter((int) i, 1);
    for (const auto& [index, value] : std::initializer_list<std::pair<int, float>>{
             {S::FilterRes, 1.25f}, {S::FilterDrive, 24}, {S::FilterCircuit, 4}, {P::ShaperAmount, 100}, {P::OscVolume, 1},
             {P::OscType, 20}, {S::Spread, 100}, {S::FilterEnvAmount, 72}, {S::PitchEnvAmount, 48}, {S::LfoPitch, 1},
             {P::AuxMod + P::ModDst0, (float) P::DstPitch}, {P::AuxMod + P::ModAmount0, 100},
             {P::Lfo3 + P::LfoFieldCount + P::ModDst0, (float) P::DstFilterFreq}, {P::Lfo3 + P::LfoFieldCount + P::ModAmount0, -100}})
        all->setParameter(index, value);
    auto [x, y] = render(*all, 20, noteAt(60, 127, 10));
    bool finite = true;
    for (size_t i = 0; i < x.size(); ++i) finite &= std::isfinite(x[i]) && std::isfinite(y[i]) && std::abs(x[i]) < 100;
    CHECK(finite);
}

TEST_CASE("sampler: a fresh device and a Sampler zone write the reference's nodes; zone settings round-trip")
{
    auto device = devices::nativeDeviceModel("MultiSampler");
    device.id = 5;
    const auto xml = juce::parseXML(juce::String(io::deviceTemplate(device)));
    REQUIRE(xml != nullptr);
    CHECK(xml->getChildByName("Globals")->getChildByName("IsSimpler")->getStringAttribute("Value") == "false");
    const auto* rateType = xml->getChildByName("AuxLfos.0")->getChildByName("Slot")->getChildByName("Value")->getChildByName("SimplerAuxLfo");
    REQUIRE(rateType != nullptr);
    CHECK(rateType->getIntAttribute("Id", -1) == 0);
    CHECK(rateType->getChildByName("RateType")->getChildByName("Manual")->getStringAttribute("Value") == "0");
    CHECK(xml->getChildByName("Player")->getChildByName("MultiSampleMap")->getChildByName("RoundRobin")->getStringAttribute("Value") == "false");
    CHECK(xml->getChildByName("MidiCtrl.2")->getChildByName("ModConnections.0")->getChildByName("Connection")->getStringAttribute("Value") == "0");
    // A zone with crossfades, selector, tune scale and a release loop: written and read back.
    auto part = partOf(1000, 40, 50, 20, 100);
    part.keyFadeMin = 42; part.keyFadeMax = 48; part.velFadeMin = 30; part.selMin = 10; part.selMax = 90; part.selFadeMax = 80;
    part.tuneScale = 0; part.link = true; part.solo = true; part.loopDetune = -12; part.releaseMode = 1; part.releaseStart = 500;
    part.releaseEnd = 999; part.releaseCrossfade = 64; part.releaseDetune = 7;
    const auto written = io::samplePartXml(part, 0);
    juce::XmlElement device2("MultiSampler");
    auto* parts = device2.createNewChildElement("Player")->createNewChildElement("MultiSampleMap")->createNewChildElement("SampleParts");
    parts->addChildElement(new juce::XmlElement(*written));
    std::vector<std::string> warnings;
    const auto read = io::readDeviceXml(device2, warnings);
    REQUIRE(read.sampleParts.size() == 1);
    const auto& back = read.sampleParts[0];
    CHECK(back.keyFadeMin == 42); CHECK(back.keyFadeMax == 48); CHECK(back.velFadeMin == 30); CHECK(back.velFadeMax == 100);
    CHECK(back.selMin == 10); CHECK(back.selMax == 90); CHECK(back.selFadeMin == 10); CHECK(back.selFadeMax == 80);
    CHECK_NEAR(back.tuneScale, 0, 1e-9); CHECK(back.link); CHECK(back.solo); CHECK(back.active); CHECK_NEAR(back.loopDetune, -12, 1e-9);
    CHECK(back.releaseMode == 1); CHECK(back.releaseStart == 500); CHECK(back.releaseCrossfade == 64); CHECK_NEAR(back.releaseDetune, 7, 1e-9);
}

TEST_CASE("sampler: every factory Sampler preset loads, plays its parameters unclamped and saves back equal (round trip)")
{
    juce::File library(juce::SystemStats::getEnvironmentVariable("SONORA_REFERENCE_LIBRARY", {}));
    if (! library.isDirectory()) library = juce::File("/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Core Library");
    const auto folder = library.getChildFile("Devices/Instruments/Sampler");
    if (! folder.isDirectory())
    {
        std::printf("    (no reference Sampler presets here: round trip skipped)\n");
        return;
    }
    auto presets = folder.findChildFiles(juce::File::findFiles, true, "*.adv");
    std::sort(presets.begin(), presets.end());
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    int checked = 0, unequal = 0, clamped = 0;
    for (const auto& file : presets)
    {
        auto song = *loaded.song;
        auto preset = io::readDevicePreset(file, song, {});
        if (! preset.error.empty() || preset.device.type != "MultiSampler") continue;
        auto device = preset.device;
        // Load into the device: every parameter the file holds comes out of the processor unchanged (ranges hold).
        devices::BuildContext context;
        auto processor = devices::createDevice(device, context);
        REQUIRE(dynamic_cast<P*>(processor.get()) != nullptr);
        const auto before = device;
        devices::captureDeviceState(device, processor.get());
        const auto& infos = processor->parameters();
        for (size_t i = 0; i < infos.size(); ++i)
        {
            double original = infos[i].defaultValue;
            bool present = false;
            if (infos[i].plainValue) { if (auto it = before.plain.find(infos[i].id); it != before.plain.end()) { original = it->second; present = true; } }
            else for (const auto& p : before.parameters) if (p.path == infos[i].id) { original = p.value; present = true; }
            if (present && std::abs(original - processor->getParameter((int) i)) > 1e-5 * std::max(1.0, std::abs(original)))
            {
                if (++clamped <= 5) std::printf("    %s: %s = %g read as %g\n", file.getFileName().toRawUTF8(), infos[i].id.c_str(), original, processor->getParameter((int) i));
            }
        }
        // Save in a set and read it back: parameters, plain leaves and zones are equal.
        song.nextDeviceId = device.id + 1; song.nextPointeeId = preset.pointeeEnd; song.pointeeIdsAllocated = true;
        model::allocateDeviceIds(song, device);
        auto command = model::insertTrackDevice(song, 0, device);
        REQUIRE(command.apply);
        command.apply(song);
        juce::TemporaryFile temp(".als");
        REQUIRE(io::saveAls(song, temp.getFile()).ok);
        auto reread = io::loadAls(temp.getFile());
        REQUIRE(reread);
        const model::Device* again = nullptr;
        for (const auto& d : reread.song->tracks[0].devices) if (d.type == "MultiSampler") again = &d;
        REQUIRE(again != nullptr);
        bool same = again->sampleParts.size() == before.sampleParts.size();
        for (const auto& p : before.parameters)
        {
            const auto it = std::find_if(again->parameters.begin(), again->parameters.end(), [&](const auto& q) { return q.path == p.path; });
            same &= it != again->parameters.end() && std::abs(it->value - p.value) <= 1e-6 * std::max(1.0, std::abs(p.value));
        }
        for (const auto& [key, value] : before.plain)
        {
            const auto it = again->plain.find(key);
            same &= it != again->plain.end() && std::abs(it->second - value) <= 1e-6 * std::max(1.0, std::abs(value));
        }
        for (size_t z = 0; same && z < before.sampleParts.size(); ++z)
        {
            const auto &a = before.sampleParts[z], &b = again->sampleParts[z];
            same &= a.keyMin == b.keyMin && a.keyMax == b.keyMax && a.velMin == b.velMin && a.velMax == b.velMax && a.keyFadeMin == b.keyFadeMin
                 && a.keyFadeMax == b.keyFadeMax && a.velFadeMin == b.velFadeMin && a.velFadeMax == b.velFadeMax && a.selMin == b.selMin
                 && a.selMax == b.selMax && a.rootKey == b.rootKey && eq(a.detuneCents, b.detuneCents) && eq(a.volume, b.volume) && eq(a.pan, b.pan)
                 && eq(a.tuneScale, b.tuneScale) && a.link == b.link && a.loopStart == b.loopStart && a.loopEnd == b.loopEnd && a.loopMode == b.loopMode
                 && a.loopCrossfade == b.loopCrossfade && a.releaseMode == b.releaseMode && a.releaseStart == b.releaseStart
                 && a.sampleStart == b.sampleStart && a.sampleEnd == b.sampleEnd && a.sample.relativePath == b.sample.relativePath;
        }
        if (! same && ++unequal <= 5) std::printf("    round trip differs: %s\n", file.getFileName().toRawUTF8());
        ++checked;
    }
    std::printf("    factory Sampler presets round-tripped: %d\n", checked);
    CHECK(checked >= 70);
    CHECK(clamped == 0);
    CHECK(unequal == 0);
}

TEST_CASE("sampler: a new device has the reference's default Sampler values (Defaults/Instruments/Sampler.adv)")
{
    juce::File library(juce::SystemStats::getEnvironmentVariable("SONORA_REFERENCE_LIBRARY", {}));
    if (! library.isDirectory()) library = juce::File("/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Core Library");
    const auto file = library.getChildFile("Defaults/Instruments/Sampler.adv");
    if (! file.existsAsFile())
    {
        std::printf("    (no reference default Sampler here: skipped)\n");
        return;
    }
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto preset = io::readDevicePreset(file, *loaded.song, {});
    REQUIRE(preset.error.empty());
    P sampler({});
    int compared = 0;
    for (const auto& info : sampler.parameters())
    {
        double reference = 0;
        bool present = false;
        if (info.plainValue) { if (auto it = preset.device.plain.find(info.id); it != preset.device.plain.end()) { reference = it->second; present = true; } }
        else for (const auto& p : preset.device.parameters) if (p.path == info.id) { reference = p.value; present = true; }
        if (! present) continue;
        ++compared;
        if (std::abs(reference - info.defaultValue) > 1e-4 * std::max(1.0, std::abs(reference)))
        {
            std::printf("    default differs: %s = %g, Sonora %g\n", info.id.c_str(), reference, (double) info.defaultValue);
            CHECK(false);
        }
    }
    CHECK(compared > 80);
}
