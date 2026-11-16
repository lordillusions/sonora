// Operator (DEV-28a) DSP. Parameter table and value laws: OperatorParams.cpp. Which laws are measured and which are
// Sonora approximations: docs/OPERATOR.md "Engine".
#include "devices/native/Operator.h"

#include "devices/native/Simpler.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace sonora::devices
{
namespace
{
using O = Operator;
constexpr double kTwoPi = 2.0 * dsp::kPi;
constexpr float kLevelFloor = 0.00032f;   // -70 dB: levels at or below play as silence ("-inf dB")
constexpr double kSlopeRate = Simpler::kEnvSlopeRate;
constexpr double kAmpFloor = Simpler::kEnvSlopeFloor;
// Modulation depth: a modulator's output (its level x Vel / Key scaling x envelope) of 1 shifts its target's phase by
// 4 pi, i.e. 2 cycles (measured, DEV-28a2: B into A at -24 / -12 / -6 / 0 dB on the default patch, whose B envelope
// sustains at -24 dB: 3.97 .. 4.06 pi per unit over the sustained part).
constexpr double kFmDepth = 2.0;
// Feedback 100 % at full level: phase shift in cycles from the oscillator's own last two outputs (averaged).
constexpr double kFeedbackDepth = 0.25;
// LFO Dest A depth: +-34.6 semitones x Amount^2 on oscillator pitch (measured, DEV-28a2: Amount 10 % = +-0.346 st,
// 100 % = +-32.6 st, o-lfo-L* / o-lfo-amount1). The filter uses the same semitone depth (assumed).
constexpr double kLfoPitchSemis = 34.6, kLfoFilterSemis = 34.6;
constexpr double kFilterEnvSemis = 108.0;
// Freq<Key pivots on MIDI 72 (C4), not C3 (measured, DEV-28a2: the default 100 % plays the cutoff an octave down at C3 in
// o-filter-default / -1k / -key; presets with Freq<Key 0 keep their brightness).
constexpr double kFilterKeyCenter = 72.0; // Envelope 100 %: about nine octaves (manual)
constexpr double kSpreadSemis = 0.2;      // Spread 100 %: +-20 cents between the two voices. TODO(DEV-28a2)

// ---- band-limited wave tables (built once, read only afterwards)
constexpr int kTable = 2048;
// Band-limit levels: the highest partial each table keeps. Playback picks the largest that stays below Nyquist, so
// every partial under it sounds (the reference keeps them up to the band edge, o-wave9-keys).
constexpr int kLimits[] = {64, 56, 48, 40, 32, 28, 24, 20, 16, 14, 12, 10, 8, 7, 6, 5, 4, 3, 2, 1};
constexpr int kMips = (int) std::size(kLimits);
struct WaveBank
{
    // tables[wave][mip][kTable + 1] for the additive and quantised waves (0..19); noise and user waves are elsewhere.
    std::vector<float> data;
    float* table(int wave, int mip) { return data.data() + ((size_t) wave * kMips + (size_t) mip) * (kTable + 1); }
};

int maxHarmonics(int wave)
{
    switch (wave)
    {
        case 3: case 11: return 3;
        case 4: case 12: return 4;
        case 5: case 13: return 6;
        case 6: case 14: return 8;
        case 7: case 15: return 16;
        case 8: case 16: return 32;
        case 9: case 17: return 64;
        default: return 64;
    }
}

// The wave at `phase` (0..1) with harmonics up to `limit` (0 = no limit: the digital / quantised shapes are naive).
double waveValue(int wave, double phase, int limit)
{
    const double x = kTwoPi * phase;
    if (wave == 0) return std::sin(x);
    if (wave == 1 || wave == 2) // 4 / 8-bit sine: truncated to 8 / 128 steps per half (measured, o-wave1 / o-wave2)
    {
        const double steps = wave == 1 ? 8.0 : 128.0;
        return std::trunc(std::sin(x) * steps) / steps;
    }
    if (wave == 10) return 1.0 - 2.0 * (phase - std::floor(phase)); // Saw D: naive unit saw (fundamental 2 / pi, measured)
    if (wave == 18) return phase - std::floor(phase) < 0.5 ? 1.0 : -1.0;
    // The additive waves are the Fourier series of the ideal unit waves, truncated (measured, o-wave3..19: every Saw has
    // its fundamental 3.9 dB under the sine, 2 / pi; every Square 2.1 dB over it, 4 / pi; the Triangle 8 / pi^2), so the
    // fundamental keeps its level whatever the partial count. Saw N: N partials; Square N: N odd partials.
    if (wave >= 3 && wave <= 9)
    {
        double s = 0;
        const int n = std::min(maxHarmonics(wave), limit);
        for (int k = 1; k <= n; ++k) s += std::sin(k * x) / k;
        return s * 2.0 / dsp::kPi;
    }
    if (wave >= 11 && wave <= 17)
    {
        double s = 0;
        const int n = maxHarmonics(wave);
        for (int i = 0, k = 1; i < n && k <= limit; ++i, k += 2) s += std::sin(k * x) / k;
        return s * 4.0 / dsp::kPi;
    }
    if (wave == 19)
    {
        double s = 0;
        for (int k = 1, sign = 1; k <= std::min(limit, 63); k += 2, sign = -sign) s += sign * std::sin(k * x) / (k * k);
        return s * 8.0 / (dsp::kPi * dsp::kPi);
    }
    return 0;
}

WaveBank& bank()
{
    static WaveBank b;
    static std::once_flag once;
    std::call_once(once, [] {
        b.data.assign((size_t) 20 * kMips * (kTable + 1), 0.0f);
        for (int wave = 0; wave < 20; ++wave)
            for (int mip = 0; mip < kMips; ++mip)
            {
                float* t = b.table(wave, mip);
                const bool naive = wave <= 2 || wave == 10 || wave == 18;
                const int limit = kLimits[mip];
                for (int i = 0; i < kTable; ++i)
                {
                    // Naive shapes keep their own steps at every level except the top one (a sine), like the
                    // reference's digital waves, which alias on purpose.
                    const double v = naive ? (mip == kMips - 1 ? std::sin(kTwoPi * i / kTable) : waveValue(wave, (double) i / kTable, 0))
                                           : waveValue(wave, (double) i / kTable, limit);
                    t[i] = (float) v;
                }
                t[kTable] = t[0];
            }
    });
    return b;
}

inline float readTable(const float* t, double phase)
{
    const double p = (phase - std::floor(phase)) * kTable;
    const int i = (int) p;
    const float f = (float) (p - i);
    return t[i] + (t[i + 1] - t[i]) * f;
}

inline uint32_t nextRandom(uint32_t& s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
inline float randomBipolar(uint32_t& s) { return (float) ((double) nextRandom(s) / 2147483648.0 - 1.0); }

double slopeFraction(double x, double slope, double term)
{
    return slope >= 0 ? 1.0 - ((1.0 - slope) * (1.0 - x) + slope * term) : (1.0 + slope) * x + (-slope) * term;
}
} // namespace

float Operator::waveSample(int wave, double phase)
{
    if (wave < 0 || wave >= 20) return (float) std::sin(kTwoPi * phase);
    return readTable(bank().table(wave, 0), phase);
}

float Operator::slopeCurve(float x, float slope)
{
    x = std::clamp(x, 0.0f, 1.0f);
    if (slope >= 0.999f) return (float) (1.0 - std::pow(kAmpFloor, (double) x)) / (float) (1.0 - kAmpFloor);
    const double term = slope >= 0 ? std::exp(-kSlopeRate * x) : std::exp(-kSlopeRate * (1.0 - x));
    return (float) slopeFraction(x, slope, term);
}

// ------------------------------------------------------------------------------------------------ lifecycle

void Operator::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    (void) bank();
    uint32_t s = 0x1234567u;
    for (auto& n : noise_) n = randomBipolar(s);
    for (int op = 0; op < kOperators; ++op) userHash_[(size_t) op] = 0xffffffffu; // rebuilt on the first block
    reset();
}

void Operator::reset()
{
    for (auto& v : voices_) v = Voice{};
    bend_ = {};
    modWheel_ = aftertouch_ = 0;
    sustainPedal_ = false;
    lastNote_ = -1;
    activeVoices_.store(0, std::memory_order_relaxed);
}

void Operator::setHostInfo(const HostInfo& host)
{
    tempo_ = host.tempo > 0 ? host.tempo : 120.0;
    beats_ = host.beats;
    playing_ = host.playing;
}

std::vector<int> Operator::displayParameters() const
{
    return {Algorithm, Volume, Tone, Transpose, opParam(0, OpVolume), opParam(1, OpVolume), opParam(2, OpVolume), opParam(3, OpVolume),
            FilterFreq, FilterRes, LfoRate, LfoAmount};
}

int Operator::filterStages(double freq, BiquadCoeffs* out) const { return filterStages(freq, out, 0.0f, 0.0f); }

int Operator::filterStages(double freq, BiquadCoeffs* out, float resOffset, float morphOffset) const
{
    SimplerFilterSettings s;
    s.type = std::clamp((int) std::lround(value(FilterType)), 0, 4);
    s.circuit = s.type <= 1 ? std::clamp((int) std::lround(value(FilterCircuitLpHp)), 0, 4) : (value(FilterCircuitBpNoMo) > 0.5f ? 1 : 0);
    s.res = std::clamp(value(FilterRes) + resOffset, 0.0f, 1.25f);
    s.morph = std::clamp(value(FilterMorph) + morphOffset, 0.0f, 1.0f);
    s.steep = value(FilterSlope) >= 0.5f;
    return simplerFilterStages(s, sampleRate_, freq, out);
}

// ------------------------------------------------------------------------------------------------ user waveforms

void Operator::rebuildUserTable(int op)
{
    // Hash of the harmonic amplitudes / settings: rebuild only on change (bounded work, fixed storage).
    uint32_t h = 2166136261u;
    for (int k = 0; k < kHarmonics; ++k)
    {
        const float a = value(harmonicParam(op, k));
        uint32_t bits;
        std::memcpy(&bits, &a, sizeof bits);
        h = (h ^ bits) * 16777619u;
    }
    for (int p : {numHarmonicsParam(op), normalizeParam(op)})
        h = (h ^ (uint32_t) std::lround(value(p) * 7)) * 16777619u;
    if (h == userHash_[(size_t) op]) return;
    userHash_[(size_t) op] = h;
    const int count = 16 << std::clamp((int) std::lround(value(numHarmonicsParam(op))), 0, 2);
    // Expansion ("Repeat", ExpansionMode) is not applied yet. TODO(DEV-28a2): measure how it extends the partials.
    auto& t = userTables_[(size_t) op];
    double peak = 0;
    for (int i = 0; i < kTableSize; ++i)
    {
        const double x = kTwoPi * i / kTableSize;
        double s = 0;
        for (int k = 0; k < count; ++k)
            if (const float a = value(harmonicParam(op, k)); a > 0) s += a * std::sin((k + 1) * x);
        t[(size_t) i] = (float) s;
        peak = std::max(peak, std::abs(s));
    }
    const bool normalize = value(normalizeParam(op)) > 0.5f;
    if (peak > 0 && (normalize || peak > 1.0))
        for (int i = 0; i < kTableSize; ++i) t[(size_t) i] = (float) (t[(size_t) i] / peak);
    t[kTableSize] = t[0];
}

// ------------------------------------------------------------------------------------------------ envelopes

Operator::EnvShape Operator::envShape(int first, bool amplitude, float timeScale, float velocity) const
{
    const double ms = sampleRate_ * 0.001;
    const float tv = value(first + TimeVelScale) / 100.0f;
    // Time<Vel (measured, DEV-28a2: o-env-timevel): 2^(-s (velocity - 70) / 16), so harder notes shorten the stages and
    // velocity 70 is neutral.
    const float velTime = (float) std::exp2(-tv * (velocity * 127.0f - 70.0f) / 16.0f);
    const bool scaleAttack = value(EnvTimeIncludeAttack) > 0.5f;
    EnvShape s;
    s.amplitude = amplitude;
    s.attack = (float) std::max(1.0, value(first + AttackTime) * ms * velTime * (scaleAttack ? timeScale : 1.0f));
    s.decay = (float) std::max(1.0, value(first + DecayTime) * ms * velTime * timeScale);
    s.release = (float) std::max(1.0, value(first + ReleaseTime) * ms * velTime * timeScale);
    s.initial = value(first + AttackLevel);
    s.peak = value(first + DecayLevel);
    s.sustain = value(first + SustainLevel);
    s.end = value(first + ReleaseLevel);
    if (amplitude)
        for (float* l : {&s.initial, &s.peak, &s.sustain, &s.end})
            if (*l <= kLevelFloor) *l = 0;
    s.attackSlope = value(first + AttackSlope);
    s.decaySlope = value(first + DecaySlope);
    s.releaseSlope = value(first + ReleaseSlope);
    s.loopMode = std::clamp((int) std::lround(value(first + LoopMode)), 0, 4);
    s.loopTime = (float) std::max(1.0, value(first + LoopTime) * ms * timeScale);
    s.repeat = (float) std::max(1.0, Simpler::repeatQuarters((int) std::lround(value(first + RepeatTime))) * 60.0 / tempo_ * sampleRate_);
    return s;
}

void Operator::startStage(Env& e, Env::Stage stage, double length)
{
    e.stage = stage;
    e.from = e.level;
    e.elapsed = 0;
    e.length = std::max(1.0, length);
}

float Operator::stepEnv(Env& e, bool released, const EnvShape& s) const
{
    if (e.stage == Env::Idle || e.stage == Env::Done) return e.level;
    // Trigger ignores note-off (the envelope runs to its sustain level, then releases).
    if (released && s.loopMode != 4 && e.stage != Env::Release) startStage(e, Env::Release, s.release);
    e.age += 1;
    if ((s.loopMode == 2 || s.loopMode == 3) && ! released && e.stage != Env::Release && e.age >= e.nextRepeat)
    {
        if (e.nextRepeat > 0) startStage(e, Env::Attack, s.attack);
        e.nextRepeat += s.repeat;
    }
    if (e.stage == Env::Sustain)
    {
        e.level = s.sustain;
        if (s.loopMode == 1 && ! released) startStage(e, Env::LoopFall, s.loopTime);
        else if (s.loopMode == 4) startStage(e, Env::Release, s.release);
        return e.level;
    }
    const float target = e.stage == Env::Attack ? s.peak : e.stage == Env::Decay ? s.sustain : e.stage == Env::LoopFall ? s.initial : s.end;
    const float slope = e.stage == Env::Attack ? s.attackSlope : e.stage == Env::Decay ? s.decaySlope
                      : e.stage == Env::LoopFall ? 0.0f : s.releaseSlope;
    const double x = e.elapsed / e.length;
    if (slope >= 0.999f)
    {
        // dB-linear approach (Simpler's measured slope +1 law): the remaining distance shrinks to kAmpFloor of itself.
        e.level = target + (e.from - target) * (float) std::pow(kAmpFloor, x);
    }
    else
    {
        const double term = slope >= 0 ? std::exp(-kSlopeRate * x) : std::exp(-kSlopeRate * (1.0 - x));
        e.level = e.from + (target - e.from) * (float) slopeFraction(x, slope, term);
    }
    e.elapsed += 1;
    if (e.elapsed >= e.length)
    {
        e.level = target;
        switch (e.stage)
        {
            case Env::Attack: startStage(e, Env::Decay, s.decay); break;
            case Env::Decay: e.stage = Env::Sustain; break;
            case Env::LoopFall: startStage(e, Env::Attack, s.attack); break;
            case Env::Release: case Env::Idle: case Env::Sustain: case Env::Done: e.stage = Env::Done; break;
        }
    }
    return e.level;
}

// ------------------------------------------------------------------------------------------------ MIDI

void Operator::handleEvent(const uint8_t* data, int size)
{
    if (size < 1) return;
    const int status = data[0] & 0xF0, channel = data[0] & 0x0F;
    if (status == 0x90 && size >= 3 && data[2] > 0) noteOn(data[1], channel, data[2] / 127.0f);
    else if ((status == 0x80 || status == 0x90) && size >= 3) noteOff(data[1], channel);
    else if (status == 0xE0 && size >= 3) bend_[(size_t) channel] = (float) (((data[2] << 7) | data[1]) - 8192) / 8192.0f;
    else if (status == 0xD0 && size >= 2) aftertouch_ = data[1] / 127.0f;
    else if (status == 0xB0 && size >= 3)
    {
        if (data[1] == 1) modWheel_ = data[2] / 127.0f;
        else if (data[1] == 64)
        {
            sustainPedal_ = data[2] >= 64;
            if (! sustainPedal_)
                for (auto& v : voices_)
                    if (v.active && v.sustained) { v.sustained = false; releaseVoice(v); }
        }
        else if (data[1] == 120 || data[1] == 123)
            for (auto& v : voices_)
                if (v.active) { if (data[1] == 120) v.active = false; else releaseVoice(v); }
    }
}

void Operator::startVoice(Voice& v, int note, int channel, float velocity, bool legato)
{
    const int previous = lastNote_;
    const bool wasActive = v.active;
    v.active = true;
    v.gate = true;
    v.sustained = false;
    v.note = note;
    v.channel = channel;
    v.velocity = velocity;
    v.age = ++counter_;
    // Glide (portamento): new notes slide from the last played pitch. Mono (Voices 1) legato keeps the envelopes.
    if (value(GlideOn) > 0.5f && previous >= 0)
    {
        v.glideSemis = previous - note + (wasActive && legato ? v.glideSemis : 0.0);
        const double samples = std::max(1.0, value(GlideTime) * 0.001 * sampleRate_);
        v.glideStep = -v.glideSemis / samples;
    }
    else v.glideSemis = v.glideStep = 0;
    if (legato && wasActive) return;

    // Time and Time<Key (measured, DEV-28a2: o-time-50 / o-time50 / o-timekey): 2^(2.95 t) for Time t in -1..1, and 0.55
    // octave shorter per octave up at Time<Key 100 %, pivoting on MIDI 70.
    const float t = (float) std::exp2(2.95 * value(EnvTime) / 100.0)
                  * (float) std::exp2(-value(EnvTimeKey) / 100.0 * 0.55 * (note - 70) / 12.0);
    v.timeScale = t;
    // Pan: Panorama, Pan<Key around C3, Pan<Rnd per note.
    float pan = value(Pan) + value(PanKey) / 100.0f * (float) (note - 60) / 48.0f + value(PanRandom) / 100.0f * randomBipolar(random_);
    v.pan = std::clamp(pan, -1.0f, 1.0f);
    v.panL = (float) std::cos((v.pan + 1.0) * dsp::kPi / 4.0);
    v.panR = (float) std::sin((v.pan + 1.0) * dsp::kPi / 4.0);
    for (int op = 0; op < kOperators; ++op)
    {
        auto& o = v.osc[(size_t) op];
        auto& o2 = v.spreadOsc[(size_t) op];
        const bool retrigger = value(opParam(op, OpRetrigger)) > 0.5f;
        if (retrigger || ! wasActive)
        {
            const double start = retrigger ? value(opParam(op, OpPhase)) / 100.0 : (double) (nextRandom(random_) & 0xffff) / 65536.0;
            o.phase = o2.phase = start;
            o.last = o.prev = o2.last = o2.prev = 0;
        }
        o.env.level = o2.env.level = 0;
        v.opShape[(size_t) op] = envShape(envParam(op, 0), true, t, velocity);
        const auto& shape = v.opShape[(size_t) op];
        for (Env* e : {&o.env, &o2.env})
        {
            e->age = 0;
            e->nextRepeat = 0;
            e->level = shape.initial;
            startStage(*e, Env::Attack, shape.attack);
        }
        o.noisePos = o2.noisePos = nextRandom(random_) % (uint32_t) kNoiseSize;
    }
    for (Env* e : {&v.filterEnv, &v.pitchEnv, &v.lfoEnv})
    {
        const int index = e == &v.filterEnv ? EnvFilter : e == &v.pitchEnv ? EnvPitch : EnvLfo;
        e->age = 0;
        e->nextRepeat = 0;
        e->level = value(otherEnvParam(index, AttackLevel));
        if (index == EnvLfo && e->level <= kLevelFloor) e->level = 0;
        const auto shape = envShape(otherEnvParam(index, 0), index == EnvLfo, t, velocity);
        (index == EnvFilter ? v.filterShape : index == EnvPitch ? v.pitchShape : v.lfoShape) = shape;
        startStage(*e, Env::Attack, shape.attack);
    }
    if (value(LfoRetrigger) > 0.5f) v.lfoPhase = 0;
    else v.lfoPhase = freeLfoPhase_;
    v.lfoHeld = randomBipolar(random_);
    for (auto& stage : v.svf) for (auto& s : stage) s = {};
    v.countdown = 0;
}

void Operator::noteOn(int note, int channel, float velocity)
{
    const int limit = voiceLimit();
    Voice* target = nullptr;
    if (limit == 1)
    {
        // Mono: one voice, legato while a key is held (only the pitch changes).
        Voice& v = voices_[0];
        const bool legato = v.active && v.gate;
        for (size_t i = 1; i < voices_.size(); ++i) voices_[i].active = false;
        startVoice(v, note, channel, velocity, legato);
        lastNote_ = note;
        return;
    }
    if (value(RetriggerMode) > 0.5f) // a repeated note restarts its own voice
        for (auto& v : voices_)
            if (v.active && v.note == note && v.channel == channel) { target = &v; break; }
    if (target == nullptr)
    {
        int active = 0;
        for (auto& v : voices_) active += v.active ? 1 : 0;
        if (active < limit)
            for (auto& v : voices_)
                if (! v.active) { target = &v; break; }
    }
    if (target == nullptr) // steal: the oldest released voice, else the oldest one
    {
        for (auto& v : voices_)
            if (v.active && ! v.gate && (target == nullptr || v.age < target->age)) target = &v;
        if (target == nullptr)
            for (auto& v : voices_)
                if (v.active && (target == nullptr || v.age < target->age)) target = &v;
    }
    if (target == nullptr) target = &voices_[0];
    const bool keepPhase = target->active && target->note == note;
    if (! keepPhase) target->active = false;
    startVoice(*target, note, channel, velocity, false);
    lastNote_ = note;
}

void Operator::noteOff(int note, int channel)
{
    for (auto& v : voices_)
        if (v.active && v.gate && v.note == note && v.channel == channel)
        {
            if (sustainPedal_) { v.sustained = true; continue; }
            releaseVoice(v);
        }
}

void Operator::releaseVoice(Voice& v) { v.gate = false; }

// ------------------------------------------------------------------------------------------------ modulation routing

// Connection enum (the reference's MIDI / Dest B list): measured with Velocity -> n at 100 % (DEV-28a2, operator_laws2
// od-vel* / od-penv*): 1..4 Volume A..D (gain 1 - a (1 - s): velocity 64 = -6 dB), 5 / 6 Crossfade A/C and B/D (2 (1 - x) /
// 2 x), 8 oscillator frequency and 10 Filter Freq (one semitone per velocity step around 64, +-64 st), 20 Volume, 21 Pan.
// Res (12), Morph (13), F Env (15), LFO Rate (16) and Amount (17) follow the same shapes (assumed); 7 Feedback, 9 FM Drive,
// 11 Filter Q (Legacy), 14 Drive, 18, 19, 22 and 23 are kept and saved but not applied (TODO(DEV-28a2)).
void Operator::applyRoute(Mods& m, int dest, float amount, float s, bool fromZero)
{
    if (dest <= 0 || std::abs(amount) < 1e-6f) return;
    // Velocity / Key / bipolar sources are centred (velocity 64 neutral, measured); controllers that rest at 0 (aftertouch,
    // mod wheel) are neutral at rest and push one way (assumed: no controller data in the probes).
    const float bipolar = amount * (fromZero ? 0.5f * s : s - 0.5f);
    const float scale = std::max(0.0f, fromZero ? 1.0f + amount * s : 1.0f - amount * (1.0f - s));
    switch (dest)
    {
        case 1: case 2: case 3: case 4: m.gain[dest - 1] *= scale; break;
        case 5: case 6:
        {
            const float x = std::clamp(0.5f + bipolar, 0.0f, 1.0f);
            m.gain[dest - 5] *= 2.0f * (1.0f - x);
            m.gain[dest - 3] *= 2.0f * x;
            break;
        }
        case 8: m.oscSemis += bipolar * 128.0f; break;
        case 10: m.filterSemis += bipolar * 128.0f; break;
        case 12: m.res += bipolar * 2.5f; break;
        case 13: m.morph += bipolar * 2.0f; break;
        case 15: m.envAmount *= scale; break;
        case 16: m.lfoRateSemis += bipolar * 128.0f; break;
        case 17: m.lfoAmount *= scale; break;
        case 20: m.volume *= scale; break;
        case 21: m.pan += bipolar * 2.0f; break;
        default: break;
    }
}

// ------------------------------------------------------------------------------------------------ control rate

void Operator::control(Voice& v)
{
    Mods m;
    // MIDI sources: Key (note / 127), Velocity, then MidiCtrl.0..2 = Aftertouch, Pitch Bend, Mod Wheel (order assumed from
    // the reference's MIDI tab, TODO(DEV-28a2)).
    const float sources[5] = {(float) v.note / 127.0f, v.velocity, aftertouch_, 0.5f + 0.5f * bend_[(size_t) v.channel], modWheel_};
    for (int src = 0; src < 5; ++src)
        for (int slot = 0; slot < 2; ++slot)
            applyRoute(m, (int) std::lround(value(midiModParam(src, slot, true))), value(midiModParam(src, slot, false)) / 100.0f, sources[src],
                       src == 2 || src == 4);
    if (value(LfoOn) > 0.5f) // LFO Dest B (bipolar)
        applyRoute(m, (int) std::lround(value(LfoDestB)), value(LfoDestBAmount) / 100.0f, 0.5f + 0.5f * v.lfoValue, false);
    if (value(PitchEnvOn) > 0.5f) // pitch envelope Dest B: +-96 st span the source (measured: 0 st = -6 dB Volume; presets that swell to +48 st play at full level)
        applyRoute(m, (int) std::lround(value(PitchDestB)), value(PitchDestBAmount) / 100.0f,
                   0.5f + v.pitchEnv.level * value(PitchEnvAmount) / 96.0f, false);
    v.mods = m;
    const float pan = std::clamp(v.pan + m.pan, -1.0f, 1.0f);
    v.panL = (float) std::cos((pan + 1.0) * dsp::kPi / 4.0);
    v.panR = (float) std::sin((pan + 1.0) * dsp::kPi / 4.0);
    // Envelope settings follow parameter changes while a note plays (stage lengths apply from the next stage).
    for (int op = 0; op < kOperators; ++op) v.opShape[(size_t) op] = envShape(envParam(op, 0), true, v.timeScale, v.velocity);
    v.filterShape = envShape(otherEnvParam(EnvFilter, 0), false, v.timeScale, v.velocity);
    v.pitchShape = envShape(otherEnvParam(EnvPitch, 0), false, v.timeScale, v.velocity);
    v.lfoShape = envShape(otherEnvParam(EnvLfo, 0), true, v.timeScale, v.velocity);
    // Filter cutoff: Freq, Freq<Vel, Freq<Key (100 % = one octave per octave around C3), envelope and LFO.
    if (value(FilterOn) > 0.5f)
    {
        double semis = value(FilterFreqKey) / 100.0 * (v.note - kFilterKeyCenter) + value(FilterFreqVel) / 100.0 * 52.0 * std::pow((double) v.velocity, 3.0) // Freq<Vel: measured, o-filter-vel
                     + value(FilterEnvAmount) / 100.0 * kFilterEnvSemis * v.filterEnv.level * m.envAmount + m.filterSemis;
        if (value(LfoOn) > 0.5f && value(FilterLfoOn) > 0.5f)
            semis += v.lfoValue * value(LfoAmountA) / 100.0 * kLfoFilterSemis;
        BiquadCoeffs stages[2];
        v.filterStages = filterStages(value(FilterFreq) * std::exp2(semis / 12.0), stages, m.res, m.morph);
        for (int i = 0; i < v.filterStages; ++i) v.coeffs[i] = SvfCoeffs::fromBiquad(stages[i]);
    }
}

float Operator::lfoStep(Voice& v, double inc)
{
    const double before = v.lfoPhase;
    v.lfoPhase += inc;
    if (v.lfoPhase >= 1.0)
    {
        v.lfoPhase -= std::floor(v.lfoPhase);
        v.lfoHeld = randomBipolar(random_);
    }
    const double p = v.lfoPhase;
    (void) before;
    switch (std::clamp((int) std::lround(value(LfoType)), 0, 6))
    {
        case 0: return (float) std::sin(kTwoPi * p);
        case 1: return p < 0.5 ? 1.0f : -1.0f;
        case 2: return (float) (p < 0.5 ? 4 * p - 1 : 3 - 4 * p); // starts at -1 (measured, o-lfo-type2)
        case 3: return (float) (2 * p - 1);
        case 4: return (float) (1 - 2 * p);
        case 5: return v.lfoHeld;
        default: // band-limited noise: smoothed random
            v.lfoNoise += (randomBipolar(random_) - v.lfoNoise) * (float) std::min(1.0, inc * 8.0);
            return std::clamp(v.lfoNoise * 3.0f, -1.0f, 1.0f);
    }
}

// ------------------------------------------------------------------------------------------------ audio

float Operator::oscSample(Osc& o, int op, int wave, double phase)
{
    if (wave == 22) return readTable(userTables_[(size_t) op].data(), phase);
    if (wave == 21) return noise_[(o.noisePos = (o.noisePos + 1) % (uint32_t) kNoiseSize)];
    if (wave == 20) // looped noise: the noise loop played at a rate that follows the oscillator frequency (assumed law)
    {
        const double p = (phase - std::floor(phase)) * 256.0;
        const int i = (int) p;
        const float f = (float) (p - i);
        const float a = noise_[(size_t) (i + (int) o.noisePos) % kNoiseSize], b = noise_[(size_t) (i + 1 + (int) o.noisePos) % kNoiseSize];
        return a + (b - a) * f;
    }
    // Mip level: the highest harmonic stays below Nyquist (Antialias), or the full table without it.
    int mip = 0;
    if (value(AntiAlias) > 0.5f && o.freq > 0)
    {
        const double room = sampleRate_ * 0.5 / o.freq;
        while (mip < kMips - 1 && kLimits[mip] >= room) ++mip;
    }
    return readTable(bank().table(wave, mip), phase);
}

void Operator::render(float* left, float* right, int from, int to)
{
    const int algorithm = std::clamp((int) std::lround(value(Algorithm)), 0, kAlgorithms - 1);
    const int carrierMask = carriers(algorithm);
    int modMask[kOperators];
    bool modulated[kOperators];
    for (int op = 0; op < kOperators; ++op)
    {
        modMask[op] = modulators(algorithm, op);
        modulated[op] = modMask[op] != 0;
    }
    // Processing order: modulators before the oscillators they modulate (D first, A last, as the structures flow).
    constexpr int order[kOperators] = {3, 2, 1, 0};
    const double invSr = 1.0 / sampleRate_;
    const float volume = value(Volume);
    const bool lfoOn = value(LfoOn) > 0.5f, pitchOn = value(PitchEnvOn) > 0.5f, filterOn = value(FilterOn) > 0.5f;
    const int lfoRange = std::clamp((int) std::lround(value(LfoRange)), 0, 2);
    const double lfoBase = lfoRange == 2 ? tempo_ / 60.0 / syncedRateBeats((int) std::lround(value(LfoRateSynced)))
                                         : lfoHz(value(LfoRate), lfoRange);
    const float spread = value(Spread) / 100.0f;
    const int shaper = std::clamp((int) std::lround(value(ShaperType)), 0, 4);
    const float shaperDrive = dsp::dbToGain(value(ShaperDrive)), shaperWet = value(ShaperDryWet) / 100.0f;
    const int circuit = std::clamp((int) std::lround(value(FilterType)), 0, 4) <= 1 ? std::clamp((int) std::lround(value(FilterCircuitLpHp)), 0, 4) : 0;
    const float drive = dsp::dbToGain(value(FilterDrive));
    float opLevel[kOperators], opFeedback[kOperators], velScale[kOperators], keyScale[kOperators];
    int opWave[kOperators];
    bool opOn[kOperators], opLfo[kOperators], opPitch[kOperators], opFixed[kOperators];
    for (int op = 0; op < kOperators; ++op)
    {
        const float level = value(opParam(op, OpVolume));
        opOn[op] = value(opParam(op, OpOn)) > 0.5f;
        opLevel[op] = level <= kLevelFloor ? 0.0f : level;
        opFeedback[op] = modulated[op] ? 0.0f : value(opParam(op, OpFeedback)) / 100.0f;
        opWave[op] = std::clamp((int) std::lround(value(opParam(op, OpWave))), 0, kWaveforms - 1);
        velScale[op] = value(opParam(op, OpVelScale)) / 100.0f;
        keyScale[op] = value(opParam(op, OpKeyScale)) / 100.0f;
        opLfo[op] = value(opParam(op, OpLfoOn)) > 0.5f;
        opPitch[op] = value(opParam(op, OpPitchEnvOn)) > 0.5f;
        opFixed[op] = value(opParam(op, OpFixed)) > 0.5f;
    }
    const float pitchAmount = value(PitchEnvAmount) * value(PitchAmountA) / 100.0f;
    const float lfoAmountA = value(LfoAmountA) / 100.0f;

    int active = 0;
    for (size_t vi = 0; vi < voices_.size(); ++vi)
    {
        auto& v = voices_[vi];
        if (! v.active) continue;
        ++active;
        // Per-voice constants: oscillator gains (Volume, Vel, Key) and base frequencies.
        const double noteHz = 440.0 * std::exp2(((double) v.note - 69.0 + (double) value(Transpose)) / 12.0);
        float gain[kOperators];
        double baseHz[kOperators];
        for (int op = 0; op < kOperators; ++op)
        {
            // Vel (measured, DEV-28a2: o-vel0 / o-velocities / o-vel100 / o-velm100 within 0.1 dB): amplitude
            // (1 - s + 2 s v)^2 for Vel s in -1..1 and v = velocity / 127, so velocity 64 is neutral and Vel 100 % spans
            // -inf..+12 dB. Key: dB per semitone around C3 (assumed, TODO(DEV-28a2)).
            const float vs = velScale[op];
            const float base = std::max(0.0f, 1.0f - vs + 2.0f * vs * v.velocity);
            const float vel = base * base;
            const float key = dsp::dbToGain(keyScale[op] * 0.5f * (float) (v.note - 60));
            gain[op] = opOn[op] ? opLevel[op] * std::max(0.0f, vel) * key : 0.0f;
            const float coarse = value(opParam(op, OpCoarse));
            float effCoarse = coarse;
            if (const float vc = value(opParam(op, OpVelCoarse)); std::abs(vc) > 1e-6f)
            {
                effCoarse = std::max(0.0f, coarse * (1.0f + vc / 100.0f * v.velocity)); // Osc<Vel (assumed law)
                if (value(opParam(op, OpVelCoarseQuantized)) > 0.5f) effCoarse = std::round(effCoarse);
            }
            baseHz[op] = opFixed[op] ? fixedFrequency(value(opParam(op, OpFixedFreq)), (int) std::lround(value(opParam(op, OpFixedMulti))))
                                     : noteHz * ratio(effCoarse, value(opParam(op, OpFine)));
        }
        const float lfoAmount = value(LfoAmount) * value(LfoAmount) * std::clamp(1.0f + value(LfoAmountVel) * (v.velocity - 1.0f), 0.0f, 2.0f);
        const bool spreading = spread > 0;
        for (int i = from; i < to; ++i)
        {
            if (v.countdown-- <= 0) { control(v); v.countdown = kControl; }
            const bool released = ! v.gate && ! v.sustained;
            stepEnv(v.filterEnv, released, v.filterShape);
            const float pitchEnv = pitchOn ? stepEnv(v.pitchEnv, released, v.pitchShape) : 0.0f;
            const float lfoEnv = stepEnv(v.lfoEnv, released, v.lfoShape);
            double lfoRate = lfoBase * std::exp2((double) value(LfoRateKey) * (v.note - 60) / 12.0);
            if (value(LfoPitchEnvOn) > 0.5f && pitchOn) lfoRate *= std::exp2(pitchEnv * pitchAmount / 12.0);
            lfoRate *= std::exp2(v.mods.lfoRateSemis / 12.0);
            v.lfoValue = lfoOn ? lfoStep(v, lfoRate * invSr) * lfoAmount * lfoEnv * v.mods.lfoAmount : 0.0f;
            if (std::abs(v.glideSemis) > 1e-9)
            {
                v.glideSemis += v.glideStep;
                if ((v.glideStep > 0 && v.glideSemis >= 0) || (v.glideStep < 0 && v.glideSemis <= 0)) v.glideSemis = v.glideStep = 0;
            }
            const double bendSemis = bend_[(size_t) v.channel] * value(PitchBendRange);
            const double commonSemis = bendSemis + v.glideSemis;
            float outL = 0, outR = 0;
            bool anyCarrierSounding = false;
            for (int pass = 0; pass < (spreading ? 2 : 1); ++pass)
            {
                auto& oscs = pass == 0 ? v.osc : v.spreadOsc;
                const double spreadSemis = spreading ? (pass == 0 ? -1.0 : 1.0) * spread * kSpreadSemis : 0.0;
                // Modulators feed their previous sample (all four oscillators advance together), with a negative sign
                // (measured, DEV-28a2: a 2:1 modulator nearly cancels the carrier's own partial, J0 - J1, and a 1:1 one
                // leaves the reference's small DC offset).
                float out[kOperators];
                for (int m = 0; m < kOperators; ++m) out[m] = oscs[(size_t) m].last;
                float carrierSum = 0;
                for (int op : order)
                {
                    auto& o = oscs[(size_t) op];
                    const float env = stepEnv(o.env, released, v.opShape[(size_t) op]);
                    double semis = (opFixed[op] ? 0.0 : commonSemis) + spreadSemis + v.mods.oscSemis;
                    if (opPitch[op]) semis += pitchEnv * pitchAmount;
                    if (opLfo[op]) semis += v.lfoValue * lfoAmountA * kLfoPitchSemis;
                    o.freq = baseHz[op] * std::exp2(semis / 12.0);
                    double pm = 0;
                    for (int m = 0; m < kOperators; ++m)
                        if (modMask[op] & (1 << m)) pm -= out[m] * kFmDepth;
                    if (opFeedback[op] > 0) pm -= opFeedback[op] * kFeedbackDepth * 0.5 * (o.last + o.prev);
                    const float s = gain[op] > 0 ? oscSample(o, op, opWave[op], o.phase + pm) * gain[op] * v.mods.gain[op] * env : 0.0f;
                    o.prev = o.last;
                    o.last = s;
                    o.phase += o.freq * invSr;
                    if (o.phase >= 1.0) o.phase -= std::floor(o.phase);
                    if (carrierMask & (1 << op))
                    {
                        carrierSum += s;
                        if (o.env.stage != Env::Done) anyCarrierSounding = true;
                    }
                }
                if (! spreading) { outL = outR = carrierSum; }
                else if (pass == 0) outL = carrierSum;
                else outR = carrierSum;
            }
            // Filter (state-variable form of the measured stages), drive circuits, then the shaper.
            float l = outL, r = outR;
            if (filterOn)
            {
                if (circuit > 0) { l = Simpler::saturate(l * drive, circuit); r = Simpler::saturate(r * drive, circuit); }
                for (int s = 0; s < v.filterStages; ++s)
                {
                    l = v.svf[s][0].process(v.coeffs[s], l);
                    r = v.svf[s][1].process(v.coeffs[s], r);
                }
            }
            // The shaper belongs to the filter section: with the filter off it does nothing (measured, o-shaper1..4 with the
            // filter off are identical to o-filteroff).
            if (filterOn && shaper > 0 && shaperWet > 0)
            {
                // Curves fitted on the reference's absolute h1 / h3 / h5 / h7 of a sine at Drive -12 / 0 / 6 / 12 dB (DEV-28a2,
                // os-type*-d*; output compensated by a power of the Drive gain g): Soft = a hard clip of 1.054 g x over
                // 1.054 sqrt g (1.5 dB rms), Hard = a cubic soft clip of 1.53 g x over 1.53 g^0.12 (3.4 dB rms), Sine =
                // sin(g x pi / 2) / sqrt g (exact). 4 Bit fits poorly (9 dB rms, TODO(DEV-28a2)): 8-step truncation of 1.073 g x.
                auto shape = [&](float x) {
                    const float g = shaperDrive, u = x * g;
                    float y = u;
                    switch (shaper)
                    {
                        case 1: y = std::clamp(1.054f * u, -1.0f, 1.0f) / (1.054f * std::sqrt(g)); break;
                        case 2:
                        {
                            const float w = 1.53f * u;
                            y = (std::abs(w) < 1.0f ? 1.5f * w - 0.5f * w * w * w : std::copysign(1.0f, w)) / (1.53f * std::pow(g, 0.12f));
                            break;
                        }
                        case 3: y = std::sin(u * (float) dsp::kPi * 0.5f) / std::sqrt(g); break;
                        default: y = std::trunc(std::clamp(1.073f * u, -1.0f, 1.0f) * 8.0f) / 8.0f / (1.073f * std::pow(g, 0.8f)); break;
                    }
                    return x + (y - x) * shaperWet;
                };
                l = shape(l);
                r = shape(r);
            }
            left[i] += l * v.panL * volume * v.mods.volume * 1.41421356f;
            right[i] += r * v.panR * volume * v.mods.volume * 1.41421356f;
            if (! anyCarrierSounding && released)
            {
                v.active = false;
                break;
            }
        }
    }
    // The LFO's free-running phase advances with the song for voices without Retrigger.
    freeLfoPhase_ += lfoBase * (to - from) * invSr;
    freeLfoPhase_ -= std::floor(freeLfoPhase_);
    activeVoices_.store(active, std::memory_order_relaxed);
}

void Operator::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    const int n = audio.getNumSamples();
    audio.clear();
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    if (isBypassed()) { midi.clear(); return; }
    for (int op = 0; op < kOperators; ++op)
        if (std::lround(value(opParam(op, OpWave))) == 22) rebuildUserTable(op);
    int position = 0;
    for (const auto meta : midi)
    {
        const int at = std::clamp(meta.samplePosition, 0, n);
        if (at > position) { render(left, right, position, at); position = at; }
        handleEvent(meta.data, meta.numBytes);
    }
    if (position < n) render(left, right, position, n);
    // Tone does not filter the output: it leaves the additive waves unchanged in the reference (o-tone0 / 0.35 / 1 are
    // identical). TODO(DEV-28a2): it acts on modulation / aliasing; not modelled yet.
    if (right == left && audio.getNumChannels() > 1) audio.copyFrom(1, 0, audio, 0, 0, n);
    midi.clear();
}

} // namespace sonora::devices
