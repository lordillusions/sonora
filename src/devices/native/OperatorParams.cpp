// Operator (DEV-28a): parameter table, value names and the laws shared by the device and its face.
// Value ranges and defaults are the reference's (MidiControllerRange of its default preset and the schema, docs/OPERATOR.md).
#include "devices/native/Operator.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace sonora::devices
{
namespace
{
using O = Operator;

constexpr const char* kWaveNames[O::kWaveforms] = {
    "Sin", "Sin 4 Bit", "Sin 8 Bit", "Saw 3", "Saw 4", "Saw 6", "Saw 8", "Saw 16", "Saw 32", "Saw 64", "Saw D",
    "Square 3", "Square 4", "Square 6", "Square 8", "Square 16", "Square 32", "Square 64", "Square D", "Triangle",
    "Noise Looped", "Noise White", "User"};
constexpr const char* kWaveShort[O::kWaveforms] = {
    "Sin", "Sin 4", "Sin 8", "Sw3", "Sw4", "Sw6", "Sw8", "Sw16", "Sw32", "Sw64", "SwD",
    "Sq3", "Sq4", "Sq6", "Sq8", "Sq16", "Sq32", "Sq64", "SqD", "Tri", "NoL", "NoW", "Usr"};
// LfoType enum. TODO(DEV-28a2): the order after Sine is assumed (classic shapes, S&H, noise) until measured.
constexpr const char* kLfoWaves[7] = {"Sine", "Square", "Triangle", "Saw Up", "Saw Down", "S&H", "Noise"};
constexpr const char* kLoopModes[5] = {"None", "Loop", "Beat", "Sync", "Trigger"};
// RepeatTime enum (quarter notes), the sampling engine's table (index 3 = 1/16 measured on Simpler).
constexpr const char* kRepeatNames[15] = {"1/64", "1/48", "1/32", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "1/3",
                                          "3/8", "1/2", "3/4", "1", "2"};
// LfoRateSynced enum: the same 15 note values as the envelopes' RepeatTime (measured: index 3 = 1/16, 8 Hz at 120 BPM,
// o-lfo-sync); cycle length in beats (quarter notes). The other entries follow the RepeatTime table (assumed).
constexpr const char* kSyncedNames[15] = {"1/64", "1/48", "1/32", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "1/3",
                                          "3/8", "1/2", "3/4", "1", "2"};
constexpr double kSyncedBeats[15] = {1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 4, 1.0 / 3, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1, 4.0 / 3,
                                     3.0 / 2, 2, 3, 4, 8};
// FixedFrequencyMulti enum: decades with index 4 = x1 (factory presets: a 55 Hz fixed bass oscillator at index 4, hi-hat
// noise at 2000 Hz x1, slow FM wobbles at index 1-2). TODO(DEV-28a2): measure.
constexpr const char* kMultiNames[6] = {"0.0001", "0.001", "0.01", "0.1", "1", "10"};
// Dest B / MIDI routing Connection enum (0..23). TODO(DEV-28a2): only "Off" (0) is known; the rest are kept and saved but
// their targets are not identified yet, so they do nothing.
constexpr int kVoiceCounts[] = {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 24, 32};

// Algorithm structures (modulator masks per oscillator A..D, carrier mask). Signal flows from D (top) to A (bottom).
// Measured (DEV-28a2, 2026-11-14): suite cases o-alg1..11 run all four oscillators at -12 dB with ratios A 8, B 11, C 13,
// D 17; in the reference's renders a carrier shows its own partial and each direct modulator first-order sidebands.
struct Algo { int mod[4]; int carriers; };
constexpr int A = 1, B = 2, C = 4, D = 8;
constexpr Algo kAlgos[O::kAlgorithms] = {
    {{B, C, D, 0}, A},             //  1: D > C > B > A
    {{B, C | D, 0, 0}, A},         //  2: (C + D) > B > A
    {{B | D, C, 0, 0}, A},         //  3: C > B, (B + D) > A
    {{B | C, D, D, 0}, A},         //  4: D > B, D > C, (B + C) > A
    {{C, C, D, 0}, A | B},         //  5: D > C, C > A, C > B    (A, B out)
    {{0, C, D, 0}, A | B},         //  6: D > C > B, A           (A, B out)
    {{B | C | D, 0, 0, 0}, A},     //  7: (B + C + D) > A
    {{B, 0, D, 0}, A | C},         //  8: B > A, D > C           (A, C out)
    {{D, D, D, 0}, A | B | C},     //  9: D > A, B, C            (A, B, C out)
    {{0, 0, D, 0}, A | B | C},     // 10: D > C, A and B         (A, B, C out)
    {{0, 0, 0, 0}, A | B | C | D}, // 11: all four out (additive)
};

std::vector<ParameterInfo> envParams(const std::string& prefix, float initial, float peak, float sustain, float end,
                                     float decayMs, float releaseMs, float levelMin, float levelMax, const char* levelUnit)
{
    std::vector<ParameterInfo> e((size_t) O::kEnvFields);
    e[O::AttackTime] = makeParam(prefix + "AttackTime", "Attack", 0.1f, 20000, 0.1f, "ms");
    e[O::AttackLevel] = makeParam(prefix + "AttackLevel", "Initial", levelMin, levelMax, initial, levelUnit);
    e[O::AttackSlope] = makeParam(prefix + "AttackSlope", "A.Slope", -1, 1, 0);
    e[O::DecayTime] = makeParam(prefix + "DecayTime", "Decay", 1, 60000, decayMs, "ms");
    e[O::DecayLevel] = makeParam(prefix + "DecayLevel", "Peak", levelMin, levelMax, peak, levelUnit);
    e[O::DecaySlope] = makeParam(prefix + "DecaySlope", "D.Slope", -1, 1, 1);
    e[O::SustainLevel] = makeParam(prefix + "SustainLevel", "Sustain", levelMin, levelMax, sustain, levelUnit);
    e[O::ReleaseTime] = makeParam(prefix + "ReleaseTime", "Release", 1, 60000, releaseMs, "ms");
    e[O::ReleaseLevel] = makeParam(prefix + "ReleaseLevel", "End", levelMin, levelMax, end, levelUnit);
    e[O::ReleaseSlope] = makeParam(prefix + "ReleaseSlope", "R.Slope", -1, 1, 1);
    e[O::LoopMode] = makeParam(prefix + "LoopMode", "Loop", 0, 4, 0, "", false, {kLoopModes, kLoopModes + 5});
    e[O::LoopTime] = makeParam(prefix + "LoopTime", "Loop Time", 0.2f, 20000, 100, "ms");
    e[O::RepeatTime] = makeParam(prefix + "RepeatTime", "Repeat", 0, 14, 3, "", false, {kRepeatNames, kRepeatNames + 15});
    e[O::TimeVelScale] = makeParam(prefix + "TimeVelScale", "Time<Vel", -100, 100, 0, "%");
    return e;
}

std::vector<ParameterInfo> operatorParameters()
{
    std::vector<ParameterInfo> p((size_t) O::NumParams);
    constexpr float kFloor = 0.0003162277571f; // -70 dB: the reference's "-inf dB"
    const std::vector<std::string> waves(kWaveNames, kWaveNames + O::kWaveforms);
    for (int op = 0; op < O::kOperators; ++op)
    {
        const std::string o = "Operator." + std::to_string(op) + "/";
        auto at = [&](int field) -> ParameterInfo& { return p[(size_t) O::opParam(op, field)]; };
        at(O::OpOn) = makeParam(o + "IsOn", "On", 0, 1, 1, "", true);
        at(O::OpCoarse) = makeParam(o + "Tune/Coarse", "Coarse", 0, 48, 1);
        at(O::OpFine) = makeParam(o + "Tune/Fine", "Fine", 0, 1000, 0);
        at(O::OpFixed) = makeParam(o + "Tune/FixedFrequencyOn", "Fixed", 0, 1, 0, "", true);
        at(O::OpFixedFreq) = makeParam(o + "Tune/FixedFrequency", "Freq", 10, 2000, 100, "Hz");
        at(O::OpFixedMulti) = makeParam(o + "Tune/FixedFrequencyMulti", "Multi", 0, 5, 4, "", false, {kMultiNames, kMultiNames + 6});
        at(O::OpVelCoarse) = makeParam(o + "Tune/VelCoarseScale", "Osc<Vel", -100, 100, 0, "%");
        at(O::OpVelCoarseQuantized) = makeParam(o + "Tune/VelCoarseScaleQuantized", "Q", 0, 1, 1, "", true);
        at(O::OpVolume) = makeParam(o + "Volume", "Level", kFloor, 1, op == 0 ? 1.0f : kFloor, "lin");
        at(O::OpRetrigger) = makeParam(o + "Retrigger", "R", 0, 1, 1, "", true);
        at(O::OpPhase) = makeParam(o + "Phase", "Phase", 0, 100, 0, "%");
        at(O::OpFeedback) = makeParam(o + "Feedback", "Feedback", 0, 100, 0, "%");
        at(O::OpWave) = makeParam(o + "WaveForm", "Wave", 0, O::kWaveforms - 1, 0, "", false, waves);
        at(O::OpVelScale) = makeParam(o + "VelScale", "Vel", -100, 100, 50, "%");
        at(O::OpKeyScale) = makeParam(o + "KeyScale", "Key", -100, 100, 0, "%");
        at(O::OpLfoOn) = makeParam(o + "LfoOn", "LFO", 0, 1, 1, "", true);
        at(O::OpPitchEnvOn) = makeParam(o + "PitchEnvOn", "Pitch Env", 0, 1, 1, "", true);
        auto env = envParams(o + "Envelope/", kFloor, 1, 1, kFloor, 1000, 400, kFloor, 1, "lin");
        for (int f = 0; f < O::kEnvFields; ++f)
            p[(size_t) O::envParam(op, f)] = env[(size_t) f];
        for (int h = 0; h < O::kHarmonics; ++h)
            p[(size_t) O::harmonicParam(op, h)] = makePlainParam(makeParam(o + "UserHarmonics/Harmonics." + std::to_string(h),
                                                                           "H" + std::to_string(h + 1), 0, 1, h == 0 ? 1.0f : 0.0f));
        p[(size_t) O::numHarmonicsParam(op)] = makePlainParam(makeParam(o + "UserHarmonics/NumHarmonics", "Harmonics", 0, 2, 0, "", false, {"16", "32", "64"}));
        p[(size_t) O::expansionParam(op)] = makePlainParam(makeParam(o + "UserHarmonics/ExpansionMode", "Repeat", 0, 3, 0));
        p[(size_t) O::normalizeParam(op)] = makePlainParam(makeParam(o + "UserHarmonics/Normalize", "Normalize", 0, 1, 1, "", true));
    }
    const struct { int index; const char* prefix; float initial, peak, sustain, end, decay, release, lo, hi; const char* unit; } others[] = {
        {O::EnvFilter, "Filter/Envelope/", 0, 1, 0, 0, 600, 50, 0, 1, "%"},
        {O::EnvPitch, "PitchEnv/PitchEnv/", 12, 12, 0, 0, 400, 50, -48, 48, "st"},
        {O::EnvLfo, "Lfo/LfoEnv/", kFloor, 1, 1, kFloor, 600, 50, kFloor, 1, "lin"},
    };
    for (const auto& o : others)
    {
        auto env = envParams(o.prefix, o.initial, o.peak, o.sustain, o.end, o.decay, o.release, o.lo, o.hi, o.unit);
        for (int f = 0; f < O::kEnvFields; ++f)
            p[(size_t) O::otherEnvParam(o.index, f)] = env[(size_t) f];
    }
    std::vector<std::string> algorithms;
    for (int a = 0; a < O::kAlgorithms; ++a) algorithms.push_back("Alg. " + std::to_string(a + 1));
    p[O::Algorithm] = makeParam("Globals/Algorithm", "Algorithm", 0, O::kAlgorithms - 1, 0, "", false, algorithms);
    p[O::Transpose] = makeParam("Globals/Transpose", "Transpose", -48, 48, 0, "st");
    p[O::PitchBendRange] = makeParam("Globals/PitchBendRange", "PB Range", 0, 24, 5, "st");
    p[O::Volume] = makeParam("Globals/Volume", "Volume", kFloor, 1.99526238f, 0.1258925349f, "lin");
    p[O::Pan] = makeParam("Globals/Panorama", "Pan", -1, 1, 0);
    p[O::PanKey] = makeParam("Globals/PanoramaKeyScale", "Pan<Key", 0, 100, 0, "%");
    p[O::PanRandom] = makeParam("Globals/PanoramaRndScale", "Pan<Rnd", 0, 100, 0, "%");
    p[O::Tone] = makeParam("Globals/Tone", "Tone", 0, 1, 0.7f, "%");
    p[O::Spread] = makeParam("Globals/SpreadAmount", "Spread", 0, 100, 0, "%");
    p[O::GlideOn] = makeParam("Globals/PortamentoOn", "Glide", 0, 1, 0, "", true);
    p[O::GlideTime] = makeParam("Globals/PortamentoTime", "Glide Time", 0.1f, 10000, 50, "ms");
    p[O::EnvTime] = makeParam("EnvScale/EnvTime", "Time", -100, 100, 0, "%");
    p[O::EnvTimeKey] = makeParam("EnvScale/EnvTimeKeyScale", "Time<Key", -100, 100, 0, "%");
    p[O::EnvTimeIncludeAttack] = makeParam("EnvScale/EnvTimeIncludeAttack", "Attack", 0, 1, 1, "", true);
    p[O::LfoOn] = makeParam("Lfo/LfoOn", "LFO", 0, 1, 0, "", true);
    p[O::LfoType] = makeParam("Lfo/LfoType", "Wave", 0, 6, 0, "", false, {kLfoWaves, kLfoWaves + 7});
    p[O::LfoRange] = makeParam("Lfo/LfoRange", "Range", 0, 2, 0, "", false, {"L", "H", "S"});
    p[O::LfoRate] = makeParam("Lfo/LfoRate", "Rate", 0, 127, 100);
    p[O::LfoRateSynced] = makeParam("Lfo/LfoRateSynced", "Rate", 0, 14, 3, "", false, {kSyncedNames, kSyncedNames + 15});
    p[O::LfoRateKey] = makeParam("Lfo/LfoRateKeyScale", "Rate<Key", 0, 1, 0, "%");
    p[O::LfoRetrigger] = makeParam("Lfo/LfoRetrigger", "R", 0, 1, 1, "", true);
    p[O::LfoAmount] = makeParam("Lfo/LfoAmount", "Amount", 0, 1, 0.1f, "%");
    p[O::LfoAmountA] = makeParam("Lfo/LfoAmountA", "Dest A", -100, 100, 100, "%");
    p[O::LfoAmountVel] = makeParam("Lfo/LfoAmountVelScale", "Amt<Vel", -1, 1, 0, "%");
    p[O::LfoPitchEnvOn] = makeParam("Lfo/LfoPitchEnvOn", "Pitch Env", 0, 1, 1, "", true);
    p[O::LfoDestBAmount] = makeParam("Lfo/ModDst/Amount", "Dest B", -100, 100, 100, "%");
    std::vector<std::string> destinations;
    for (int d = 0; d < O::kDestinations; ++d) destinations.push_back(O::destinationName(d));
    p[O::LfoDestB] = makeParam("Lfo/ModDst/Connection", "Dest B", 0, O::kDestinations - 1, 0, "", false, destinations);
    p[O::PitchEnvOn] = makeParam("PitchEnv/PitchEnvOn", "Pitch Env", 0, 1, 0, "", true);
    p[O::PitchEnvAmount] = makeParam("PitchEnv/PitchEnv/EnvelopeAmount", "Amount", -1, 1, 1, "%");
    p[O::PitchAmountA] = makeParam("PitchEnv/PitchEnvAmountA", "Dest A", -100, 100, 100, "%");
    p[O::PitchDestBAmount] = makeParam("PitchEnv/ModDst/Amount", "Dest B", -100, 100, 100, "%");
    p[O::PitchDestB] = makeParam("PitchEnv/ModDst/Connection", "Dest B", 0, O::kDestinations - 1, 0, "", false, destinations);
    p[O::FilterOn] = makeParam("Filter/OnOff", "Filter", 0, 1, 1, "", true);
    p[O::FilterType] = makeParam("Filter/Type", "Type", 0, 4, 0, "", false, {"Lowpass", "Highpass", "Bandpass", "Notch", "Morph"});
    p[O::FilterLegacyType] = makeParam("Filter/LegacyType", "Legacy", 0, 13, 1);
    p[O::FilterCircuitLpHp] = makeParam("Filter/CircuitLpHp", "Circuit", 0, 4, 0, "", false, {"Clean", "OSR", "MS2", "SMP", "PRD"});
    p[O::FilterCircuitBpNoMo] = makeParam("Filter/CircuitBpNoMo", "Circuit", 0, 1, 0, "", false, {"Clean", "OSR"});
    p[O::FilterSlope] = makeParam("Filter/Slope", "Slope", 0, 1, 1, "", true);
    p[O::FilterFreq] = makeParam("Filter/Frequency", "Freq", 30, 18500, 12000, "Hz");
    p[O::FilterLegacyQ] = makeParam("Filter/LegacyQ", "Q", 0.3f, 10, 1);
    p[O::FilterRes] = makeParam("Filter/Resonance", "Res", 0, 1.25f, 0.277777791f, "%");
    p[O::FilterMorph] = makeParam("Filter/Morph", "Morph", 0, 1, 0, "%");
    p[O::FilterDrive] = makeParam("Filter/Drive", "Drive", 0, 24, 0, "dB");
    p[O::FilterFreqVel] = makeParam("Filter/FrequencyVelScale", "Freq<Vel", -100, 100, 0, "%");
    p[O::FilterFreqKey] = makeParam("Filter/FrequencyKeyScale", "Freq<Key", -200, 200, 100, "%");
    p[O::FilterEnvAmount] = makeParam("Filter/EnvelopeAmount", "Envelope", -100, 100, 0, "%");
    p[O::FilterLfoOn] = makeParam("Filter/LfoOn", "LFO", 0, 1, 0, "", true);
    p[O::ShaperType] = makeParam("Shaper/Type", "Shaper", 0, 4, 0, "", false, {"Off", "Soft", "Hard", "Sine", "4 Bit"});
    p[O::ShaperDryWet] = makeParam("Shaper/DryWet", "Dry/Wet", 0, 100, 100, "%");
    p[O::ShaperDrive] = makeParam("Shaper/Drive", "Shp. Drive", -12, 12, 0, "dB");
    std::vector<std::string> voices;
    for (int v : kVoiceCounts) voices.push_back(std::to_string(v));
    p[O::NumVoices] = makePlainParam(makeParam("Globals/NumVoices", "Voices", 0, (float) voices.size() - 1, 5, "", false, voices));
    p[O::RetriggerMode] = makePlainParam(makeParam("Globals/RetriggerMode", "Retrigger", 0, 1, 0, "", true));
    p[O::Interpolation] = makePlainParam(makeParam("Globals/Interpolation", "Interpolation", 0, 1, 1, "", true));
    p[O::AntiAlias] = makePlainParam(makeParam("Globals/AntiAlias", "Antialias", 0, 1, 1, "", true));
    const char* sources[5] = {"MidiCtrl/KeyDst/", "MidiCtrl/VelDst/", "MidiCtrl/MidiCtrl.0/", "MidiCtrl/MidiCtrl.1/", "MidiCtrl/MidiCtrl.2/"};
    const char* labels[5] = {"Key", "Vel", "Ctrl 1", "Ctrl 2", "Ctrl 3"};
    for (int s = 0; s < 5; ++s)
        for (int slot = 0; slot < 2; ++slot)
        {
            const std::string base = std::string(sources[s]) + "ModConnections." + std::to_string(slot) + "/";
            p[(size_t) O::midiModParam(s, slot, false)] = makePlainParam(makeParam(base + "Amount", std::string(labels[s]) + " Amount", -100, 100, 0, "%"));
            p[(size_t) O::midiModParam(s, slot, true)] = makePlainParam(makeParam(base + "Connection", std::string(labels[s]) + " Dest", 0, O::kDestinations - 1, 0, "", false, destinations));
        }
    return p;
}
} // namespace

Operator::Operator() : NativeDevice(operatorParameters()) {}

const char* Operator::waveName(int wave) { return kWaveNames[std::clamp(wave, 0, kWaveforms - 1)]; }
const char* Operator::waveShortName(int wave) { return kWaveShort[std::clamp(wave, 0, kWaveforms - 1)]; }
const char* Operator::lfoWaveName(int type) { return kLfoWaves[std::clamp(type, 0, 6)]; }
const char* Operator::loopModeName(int mode) { return kLoopModes[std::clamp(mode, 0, 4)]; }
const char* Operator::repeatName(int index) { return kRepeatNames[std::clamp(index, 0, 14)]; }
const char* Operator::fixedMultiName(int index) { return kMultiNames[std::clamp(index, 0, 5)]; }
double Operator::fixedMultiplier(int index) { return std::pow(10.0, std::clamp(index, 0, 5) - 4); }
const char* Operator::syncedRateName(int index) { return kSyncedNames[std::clamp(index, 0, 14)]; }
double Operator::syncedRateBeats(int index) { return kSyncedBeats[std::clamp(index, 0, 14)]; }
int Operator::voiceCount(int index) { return kVoiceCounts[std::clamp(index, 0, (int) std::size(kVoiceCounts) - 1)]; }

const char* Operator::destinationName(int connection)
{
    static const char* const names[kDestinations] = {
        "Off", "Dest 1", "Dest 2", "Dest 3", "Dest 4", "Dest 5", "Dest 6", "Dest 7", "Dest 8", "Dest 9", "Dest 10", "Dest 11",
        "Dest 12", "Dest 13", "Dest 14", "Dest 15", "Dest 16", "Dest 17", "Dest 18", "Dest 19", "Dest 20", "Dest 21",
        "Dest 22", "Dest 23"};
    return names[std::clamp(connection, 0, kDestinations - 1)];
}

int Operator::modulators(int algorithm, int op) { return kAlgos[std::clamp(algorithm, 0, kAlgorithms - 1)].mod[std::clamp(op, 0, 3)]; }
int Operator::carriers(int algorithm) { return kAlgos[std::clamp(algorithm, 0, kAlgorithms - 1)].carriers; }

double Operator::ratio(float coarse, float fine)
{
    // Coarse 0 is a ratio of 0.5; Fine adds thousandths of the coarse ratio. TODO(DEV-28a2): measure Fine's law.
    const double c = coarse < 0.5f ? 0.5 : std::round((double) coarse);
    return c * (1.0 + (double) fine / 1000.0);
}

double Operator::fixedFrequency(float freq, int multi) { return (double) freq * fixedMultiplier(multi); }

double Operator::lfoHz(float rate, int range)
{
    // Low (measured, DEV-28a2: o-lfo-L100 6.48 Hz, o-lfo-L127 30.87 Hz, rate 80 2.02 Hz): 0.01945 e^(0.05803 Rate),
    // 0.02 Hz (50 s) .. 30.9 Hz. High: the same slope from 8 Hz (measured: Rate 64 = 330 Hz sidebands, o-lfo-H64), so
    // 8 Hz .. 12.7 kHz.
    const double r = std::clamp((double) rate, 0.0, 127.0);
    return (range == 1 ? 8.0 : 0.01945) * std::exp(0.05803 * r);
}

int Operator::voiceLimit() const { return voiceCount((int) std::lround(value(NumVoices))); }

} // namespace sonora::devices
