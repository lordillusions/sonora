#include "devices/native/Sampler.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{

namespace
{
using S = Simpler;
using P = Sampler;

constexpr float kSilentLevel = 3.17e-4f; // VolumeEnvelope levels at the -70 dB floor (0.000316) are silence
constexpr float kReleaseCut = 0.001f;    // voices end once the volume release falls below -60 dB (Simpler, measured)
constexpr double kPi = juce::MathConstants<double>::pi;

const char* const kDestinations[P::DstCount] = {
    "Off", "Sample Selector", "Sample Offset", "Loop Start", "Loop Length", "Release Loop", "Pitch", "Pitch Sample",
    "Pitch Osc", "Pitch Env", "Volume Osc", "Shaper Amt", "Filter Freq", "Filter Q (Legacy)", "Filter Res", "Filter Morph",
    "Filter Drive", "Filter Env", "Volume", "Pan", "Time", "LFO 1 Rate", "LFO 1 Amt Vol", "LFO 1 Amt Pan", "LFO 1 Amt Filt",
    "LFO 1 Amt Pitch", "LFO 2 Rate", "LFO 2 Amt A", "LFO 2 Amt B", "LFO 3 Rate", "LFO 3 Amt A", "LFO 3 Amt B"};

const std::vector<std::string> kOscWaves{"Sine", "Sine 4 Bit", "Sine 8 Bit", "Saw 3", "Saw 4", "Saw 6", "Saw 8", "Saw 16",
                                         "Saw 32", "Saw 64", "Saw D", "Square 3", "Square 4", "Square 6", "Square 8", "Square 16",
                                         "Square 32", "Square 64", "Square D", "Triangle", "Noise"};
const std::vector<std::string> kLoopModes{"None", "Loop", "Beat", "Sync", "Trigger"};
const std::vector<std::string> kRepeats{"1/64", "1/48", "1/32", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "1/3", "3/8", "1/2", "3/4", "1", "2"};
const std::vector<std::string> kBeatRates{"1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "5/16",
                                          "1/3", "3/8", "1/2", "3/4", "1", "1.5", "2", "3", "4", "6", "8"};
const std::vector<std::string> kLfoShapes{"Sine", "Square", "Triangle", "Saw Down", "Saw Up", "S&H"};
constexpr dsp::Lfo::Shape kShapes[] = {dsp::Lfo::Shape::Sine, dsp::Lfo::Shape::Square, dsp::Lfo::Shape::Triangle,
                                       dsp::Lfo::Shape::SawDown, dsp::Lfo::Shape::SawUp, dsp::Lfo::Shape::Random};
constexpr int kOscTableSize = 2048;
// Round robin Reset Interval (RoundRobinResetPeriod) in quarter notes; 0 = never. TODO(DEV-14b): law unverified (the
// enum's entries are assumed: 1/16, 1/8, 1/4, 1/2, 1, 2, 4 bars).
constexpr double kResetQuarters[] = {0, 0.25, 0.5, 1, 2, 4, 8, 16};

// Modulation oscillator wavetables (Operator-style additive waves), built once.
const std::array<std::array<float, kOscTableSize + 1>, 21>& oscTables()
{
    static const auto tables = [] {
        std::array<std::array<float, kOscTableSize + 1>, 21> t{};
        constexpr int sawPartials[] = {3, 4, 6, 8, 16, 32, 64};
        for (int i = 0; i <= kOscTableSize; ++i)
        {
            const double x = 2 * kPi * i / kOscTableSize;
            const double sine = std::sin(x);
            t[0][(size_t) i] = (float) sine;
            t[1][(size_t) i] = (float) (std::round(sine * 7.0) / 7.0);
            t[2][(size_t) i] = (float) (std::round(sine * 127.0) / 127.0);
            for (int w = 0; w < 7; ++w)
            {
                double saw = 0, square = 0;
                for (int k = 1; k <= sawPartials[w]; ++k) saw += std::sin(k * x) / k;
                for (int k = 0; k < sawPartials[w]; ++k) square += std::sin((2 * k + 1) * x) / (2 * k + 1);
                t[(size_t) (3 + w)][(size_t) i] = (float) (saw * 2 / kPi);
                t[(size_t) (11 + w)][(size_t) i] = (float) (square * 4 / kPi);
            }
            const double phase = (double) i / kOscTableSize;
            t[10][(size_t) i] = (float) (1.0 - 2.0 * phase);                               // Saw D: the naive (digital) saw
            t[18][(size_t) i] = phase < 0.5 ? 1.0f : -1.0f;                               // Square D
            t[19][(size_t) i] = (float) (phase < 0.25 ? 4 * phase : phase < 0.75 ? 2 - 4 * phase : 4 * phase - 4); // Triangle
        }
        return t;
    }();
    return tables;
}

float hermite(const float* d, int len, double pos)
{
    const int i = (int) std::floor(pos);
    const float t = (float) (pos - i);
    const auto at = [&](int k) { return d[std::clamp(i + k, 0, len - 1)]; };
    const float y0 = at(-1), y1 = at(0), y2 = at(1), y3 = at(2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

std::vector<ParameterInfo> makeInfos()
{
    auto p = S::parameterInfos();
    p.resize(P::NumParams);
    // The reference's default Sampler (Defaults/Instruments/Sampler.adv, pinned by a test): Filter on with Res 9.1 %, no
    // velocity -> volume, LFO 1 and the pitch envelope off. Sections whose slot it leaves empty (Osc, Shaper, Aux, LFO 2 / 3,
    // pitch envelope, LFO 1) take the values the Lovely Keys captures show when switched on.
    // The reference's pitch / filter envelopes reach 0.00 ms (Lovely Keys captures); SamplerFace keeps its own log law.
    for (const int attack : {(int) S::PitchAttack, (int) S::FilterAttack}) p[(size_t) attack].minValue = 0.0f;
    p[S::PitchAttack].defaultValue = 0.0f; // empty slot in the default device; switched on it shows 0.00 ms (the filter's is 0.1)
    p[S::VelToVolume].defaultValue = 0;
    p[S::FilterRes].defaultValue = 0.09090909f;
    p[S::LfoOn].defaultValue = 0;
    p[S::PitchEnvOn].defaultValue = 0;
    p[S::LoopOn].defaultValue = 0;
    p[S::Snap].defaultValue = 0;
    p[S::LfoShape].choices = kLfoShapes;
    p[S::LfoBeat].maxValue = (float) kBeatRates.size() - 1;
    p[S::LfoBeat].choices = kBeatRates;
    p[S::Interpolation].choices = {"Mode 0", "Normal", "Good", "Best"};
    const std::string f = "Filter/Slot/Value/SimplerFilter/", fe = f + "Envelope/", e = "VolumeAndPan/Envelope/";
    const std::string pe = "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/", lfo = "Lfo/Slot/Value/SimplerLfo/";
    const std::string so = "Player/SubOsc/Slot/Value/SimplerSubOsc/", ae = "AuxEnv/Slot/Value/SimplerAuxEnvelope/";
    const std::string sh = "Shaper/Slot/Value/SimplerShaper/", map = "Player/MultiSampleMap/";
    const auto plain = [&](int index) { p[(size_t) index].plainValue = true; };
    // Levels, loop and Time < Vel of an envelope whose times / slopes are Simpler parameters.
    const auto levels = [&](int base, const std::string& at, const char* label, float initial, float peak, float end, float low) {
        const std::string l(label);
        p[(size_t) base] = makeParam(at + "AttackLevel", l + " Initial", low, 1, initial, "%");
        p[(size_t) base + 1] = makeParam(at + "DecayLevel", l + " Peak", low, 1, peak, "%");
        p[(size_t) base + 2] = makeParam(at + "ReleaseLevel", l + " End", low, 1, end, "%");
    };
    levels(P::AmpInitial, e, "Vol", 3.1622776e-4f, 1, 3.1622776e-4f, 0);
    p[P::AmpTimeVel] = makeParam(e + "TimeVelScale", "Vol Time<Vel", -100, 100, 0, "%");
    levels(P::FilterInitial, fe, "F", 0, 1, 0, -1);
    p[P::FilterLoopMode] = makeParam(fe + "LoopMode", "F Loop", 0, 4, 0, "", false, kLoopModes);
    p[P::FilterLoopTime] = makeParam(fe + "LoopTime", "F Time", 0.2f, 20000, 100, "ms");
    p[P::FilterRepeat] = makeParam(fe + "RepeatTime", "F Repeat", 0, (float) kRepeats.size() - 1, 3, "", false, kRepeats);
    p[P::FilterTimeVel] = makeParam(fe + "TimeVelScale", "F Time<Vel", -100, 100, 0, "%");
    levels(P::PitchInitial, pe, "P", 0, 1, 0, -1);
    p[P::PitchLoopMode] = makeParam(pe + "LoopMode", "P Loop", 0, 4, 0, "", false, kLoopModes);
    p[P::PitchLoopTime] = makeParam(pe + "LoopTime", "P Time", 0.2f, 20000, 100, "ms");
    p[P::PitchRepeat] = makeParam(pe + "RepeatTime", "P Repeat", 0, (float) kRepeats.size() - 1, 3, "", false, kRepeats);
    p[P::PitchTimeVel] = makeParam(pe + "TimeVelScale", "P Time<Vel", -100, 100, 0, "%");
    // The reference's pre-Live-10 filter (LegacyMode): read and written, not played.
    p[P::FilterLegacyType] = makeParam(f + "LegacyType", "Legacy Type", 0, 8, 0);
    p[P::FilterLegacyQ] = makeParam(f + "LegacyQ", "Legacy Q", 0.1f, 20, 0.7f);
    p[P::ShaperOn] = makeParam("Shaper/IsOn", "Shaper", 0, 1, 0, "", true);
    p[P::ShaperType] = makeParam(sh + "Type", "Shaper Type", 0, 3, 0, "", false, {"Soft", "Hard", "Sine", "4 Bit"});
    p[P::ShaperAmount] = makeParam(sh + "Amount", "Shaper Amt", 0, 100, 0);
    p[P::ShaperPost] = makeParam(sh + "Structure", "Shaper Post", 0, 1, 0, "", true); // false: shaper -> filter
    p[P::OscOn] = makeParam("Player/SubOsc/IsOn", "Osc", 0, 1, 0, "", true);
    p[P::OscMode] = makeParam(so + "Mode", "Osc Mode", 0, 1, 0, "", false, {"FM", "AM"});
    p[P::OscType] = makeParam(so + "Type", "Osc Type", 0, (float) kOscWaves.size() - 1, 0, "", false, kOscWaves);
    p[P::OscVolume] = makeParam(so + "Volume", "Osc Volume", 0, 1, 1, "lin");
    p[P::OscVolVel] = makeParam(so + "VolumeVelScale", "Osc Vol<Vel", 0, 1, 0, "%");
    p[P::OscFixed] = makeParam(so + "IsFixedFreq", "Fixed", 0, 1, 0, "", true);
    p[P::OscCoarse] = makeParam(so + "TuneCoarse", "Coarse", -2, 48, 1);
    p[P::OscFine] = makeParam(so + "TuneFine", "Fine", 0, 1000, 0);
    p[P::OscFixedFreq] = makeParam(so + "FreqFixed", "Freq", 0.1f, 10000, 100, "Hz");
    p[P::OscFixedMul] = makeParam(so + "FreqFixedMul", "Multi", 0, 6, 3, "", false, {"0.001", "0.01", "0.1", "1", "10", "100", "1000"});
    // A full envelope block (14 fields in XML order).
    const auto envelope = [&](int base, const std::string& at, const std::string& l, bool volume, float decay, float release, float sustain) {
        const float low = volume ? 0.0f : -1.0f, silent = volume ? 3.1622776e-4f : 0.0f;
        p[(size_t) base + P::EnvAttack] = makeParam(at + "AttackTime", l + " Attack", 0.0f, 20000, 0.0f, "ms");
        p[(size_t) base + P::EnvInitial] = makeParam(at + "AttackLevel", l + " Initial", low, 1, silent, "%");
        p[(size_t) base + P::EnvAttackSlope] = makeParam(at + "AttackSlope", l + " A Slope", -1, 1, 0);
        p[(size_t) base + P::EnvDecay] = makeParam(at + "DecayTime", l + " Decay", 1, 60000, decay, "ms");
        p[(size_t) base + P::EnvPeak] = makeParam(at + "DecayLevel", l + " Peak", low, 1, 1, "%");
        p[(size_t) base + P::EnvDecaySlope] = makeParam(at + "DecaySlope", l + " D Slope", -1, 1, 1);
        p[(size_t) base + P::EnvSustain] = makeParam(at + "SustainLevel", l + " Sustain", low, 1, sustain, "%");
        p[(size_t) base + P::EnvRelease] = makeParam(at + "ReleaseTime", l + " Release", 1, 60000, release, "ms");
        p[(size_t) base + P::EnvEnd] = makeParam(at + "ReleaseLevel", l + " End", low, 1, silent, "%");
        p[(size_t) base + P::EnvReleaseSlope] = makeParam(at + "ReleaseSlope", l + " R Slope", -1, 1, 1);
        p[(size_t) base + P::EnvLoopMode] = makeParam(at + "LoopMode", l + " Loop", 0, 4, 0, "", false, kLoopModes);
        p[(size_t) base + P::EnvLoopTime] = makeParam(at + "LoopTime", l + " Time", 0.2f, 20000, 100, "ms");
        p[(size_t) base + P::EnvRepeat] = makeParam(at + "RepeatTime", l + " Repeat", 0, (float) kRepeats.size() - 1, 3, "", false, kRepeats);
        p[(size_t) base + P::EnvTimeVel] = makeParam(at + "TimeVelScale", l + " Time<Vel", -100, 100, 0, "%");
    };
    // TODO(DEV-14b): law unverified — the oscillator's defaults are the factory presets' most common values.
    envelope(P::OscEnv, so + "Envelope/", "Osc", true, 60000, 30000, 1);
    p[P::AuxOn] = makeParam("AuxEnv/IsOn", "Aux", 0, 1, 0, "", true);
    envelope(P::AuxEnv, ae, "Aux", false, 600, 50, 0);
    std::vector<std::string> destinations(kDestinations, kDestinations + P::DstCount);
    const auto matrix = [&](int base, const std::string& at, const std::string& l) {
        for (int c = 0; c < 2; ++c)
        {
            const std::string connection = at + "ModConnections." + std::to_string(c) + "/";
            const std::string side = c == 0 ? " A" : " B";
            p[(size_t) (base + 2 * c)] = makeParam(connection + "Connection", l + side, 0, (float) P::DstCount - 1, 0, "", false, destinations);
            p[(size_t) (base + 2 * c + 1)] = makeParam(connection + "Amount", l + side + " Amt", -100, 100, 0);
            plain(base + 2 * c);
            plain(base + 2 * c + 1);
        }
    };
    matrix(P::AuxMod, ae + "ModDst/", "Aux");
    p[P::Lfo1StereoMode] = makeParam(lfo + "StereoMode", "LFO 1 Stereo", 0, 1, 0, "", false, {"Phase", "Spin"});
    p[P::Lfo1Spin] = makeParam(lfo + "Spin", "LFO 1 Spin", 0, 1, 0, "%");
    p[P::Lfo1Phase] = makeParam(lfo + "Phase", "LFO 1 Phase", 0, 360, 0, "deg");
    p[P::Lfo1Smooth] = makeParam(lfo + "Smooth", "LFO 1 Smooth", 0, 1, 0.5f);
    p[P::Lfo1Width] = makeParam(lfo + "Width", "LFO 1 Width", 0, 1, 0);
    for (int n = 0; n < 2; ++n)
    {
        const int base = n == 0 ? P::Lfo2 : P::Lfo3;
        const std::string holder = "AuxLfos." + std::to_string(n) + "/", at = holder + "Slot/Value/SimplerAuxLfo/";
        const std::string l = "LFO " + std::to_string(n + 2);
        p[(size_t) base + P::LfoOnField] = makeParam(holder + "IsOn", l, 0, 1, 0, "", true);
        p[(size_t) base + P::LfoType] = makeParam(at + "Type", l + " Type", 0, 5, 0, "", false, kLfoShapes);
        p[(size_t) base + P::LfoFrequency] = makeParam(at + "Frequency", l + " Freq", 0.01f, 30, 1, "Hz");
        p[(size_t) base + P::LfoRateType] = makeParam(at + "RateType", l + " Sync", 0, 1, 0, "", true);
        p[(size_t) base + P::LfoRateType].intNode = true; // TimeableEnum in the reference
        p[(size_t) base + P::LfoBeatRate] = makeParam(at + "BeatRate", l + " Beats", 0, (float) kBeatRates.size() - 1, 4, "", false, kBeatRates);
        p[(size_t) base + P::LfoStereoMode] = makeParam(at + "StereoMode", l + " Stereo", 0, 1, 0, "", false, {"Phase", "Spin"});
        p[(size_t) base + P::LfoSpin] = makeParam(at + "Spin", l + " Spin", 0, 1, 0, "%");
        p[(size_t) base + P::LfoPhase] = makeParam(at + "Phase", l + " Phase", 0, 360, 0, "deg");
        p[(size_t) base + P::LfoOffset] = makeParam(at + "Offset", l + " Offset", 0, 360, 0, "deg");
        p[(size_t) base + P::LfoKeyScale] = makeParam(at + "FrequencyKeyScale", l + " Key", 0, 1, 0, "%");
        p[(size_t) base + P::LfoSmooth] = makeParam(at + "Smooth", l + " Smooth", 0, 1, 0.5f);
        p[(size_t) base + P::LfoAttackField] = makeParam(at + "Attack", l + " Attack", 0.1f, 20000, 0.1f, "ms");
        p[(size_t) base + P::LfoRetrig] = makeParam(at + "Retrigger", l + " Retrig", 0, 1, 1, "", true);
        p[(size_t) base + P::LfoWidth] = makeParam(at + "Width", l + " Width", 0, 1, 0);
        matrix(base + P::LfoFieldCount, at + "ModDst/", l);
    }
    p[P::Reverse] = makeParam("Player/Reverse", "Reverse", 0, 1, 0, "", true);
    p[P::SampleSelector] = makeParam("Player/SampleSelector", "Selector", 0, 127, 0);
    p[P::LoadInRam] = makeParam(map + "LoadInRam", "RAM", 0, 1, 0, "", true);
    p[P::LayerCrossfade] = makeParam(map + "LayerCrossfade", "Zone Fade", 0, 1, 0, "", false, {"Lin", "Pow"});
    p[P::RoundRobin] = makeParam(map + "RoundRobin", "RR", 0, 1, 0, "", true);
    p[P::RoundRobinMode] = makeParam(map + "RoundRobinMode", "RR Mode", 0, 3, 0, "", false, {"Forward", "Backward", "Other", "Random"});
    p[P::RoundRobinReset] = makeParam(map + "RoundRobinResetPeriod", "RR Reset", 0, 7, 0, "", false,
                                      {"None", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"});
    for (int index : {(int) P::LoadInRam, (int) P::LayerCrossfade, (int) P::RoundRobin, (int) P::RoundRobinMode, (int) P::RoundRobinReset})
        plain(index);
    matrix(P::KeyMod, "KeyDst/", "Key");
    matrix(P::VelMod, "VelDst/", "Velocity");
    matrix(P::RelVelMod, "RelVelDst/", "Off Vel");
    const char* rows[P::MidiRows] = {"Pressure", "Pitch Bend", "Mod Wheel", "Foot Ctrl", "Slide", "Note PB"};
    for (int row = 0; row < P::MidiRows; ++row)
        matrix(P::MidiMod + row * P::ModFieldCount, "MidiCtrl." + std::to_string(row) + "/", rows[row]);
    p[P::EnvTime] = makeParam("Globals/EnvScale/EnvTime", "Time", -100, 100, 0, "%");
    p[P::EnvTimeKey] = makeParam("Globals/EnvScale/EnvTimeKeyScale", "Time<Key", -100, 100, 0, "%");
    p[P::EnvTimeAttack] = makeParam("Globals/EnvScale/EnvTimeIncludeAttack", "Time Attack", 0, 1, 1, "", true);
    p[P::PitchBendRangeParam] = makePlainParam(makeParam("Globals/PitchBendRange", "PB Range", 0, 48, 5, "st"));
    p[P::MpeBendRange] = makePlainParam(makeParam("Globals/MpePitchBendRange", "Note PB Range", 0, 96, 48, "st"));
    return p;
}
} // namespace

// ---------------------------------------------------------------- laws

const char* Sampler::destinationName(int destination) { return kDestinations[std::clamp(destination, 0, (int) DstCount - 1)]; }

float Sampler::zoneFade(int x, int low, int high, int fadeLow, int fadeHigh, bool constantPower)
{
    // Measured from the factory zones' layout (adjacent zones overlap by their fade lengths): a fade over n keys ramps
    // 1 / (n + 1) .. n / (n + 1), so two adjacent linear fades sum to 1. TODO(DEV-14b): law unverified against audio.
    if (x < low || x > high) return 0.0f;
    fadeLow = std::clamp(fadeLow, low, high);
    fadeHigh = std::clamp(fadeHigh, fadeLow, high);
    float t = 1.0f;
    if (x < fadeLow) t = (float) (x - low + 1) / (float) (fadeLow - low + 1);
    if (x > fadeHigh) t = std::min(t, (float) (high - x + 1) / (float) (high - fadeHigh + 1));
    return constantPower ? (float) std::sin(t * kPi * 0.5) : t;
}

double Sampler::timeScale(double envTime, double timeKey, int note, double timeVel, float velocity)
{
    // TODO(DEV-14b): law unverified. Time +-100 % scales every envelope by 4x / 0.25x; Time < Key scales by
    // 2^(amount x octaves from C3) (negative values shorten high notes, as the factory keys presets use it); Time < Vel
    // shortens the times by up to 4x at full velocity.
    return std::exp2(2.0 * envTime / 100.0) * std::exp2(timeKey / 100.0 * (note - 60) / 12.0) * std::exp2(-2.0 * timeVel / 100.0 * velocity);
}

double Sampler::oscRatio(double coarse, double fine)
{
    // Coarse 1 .. 48 multiplies; 0, -1, -2 are 0.5, 0.25, 0.125 (the manual's 0.125-48). Fine adds thousandths.
    // TODO(DEV-14b): law unverified.
    const double ratio = coarse >= 1 ? std::round(coarse) : std::exp2(std::round(coarse) - 1);
    return ratio * (1.0 + fine / 1000.0);
}

double Sampler::oscFixedMultiplier(int index)
{
    static constexpr double multipliers[] = {0.001, 0.01, 0.1, 1, 10, 100, 1000}; // TODO(DEV-14b): law unverified
    return multipliers[std::clamp(index, 0, 6)];
}

float Sampler::oscWave(int type, double phase, unsigned& noise)
{
    if (type >= 20)
    {
        noise = noise * 1664525u + 1013904223u;
        return (float) (noise >> 8) / 8388608.0f - 1.0f;
    }
    const auto& table = oscTables()[(size_t) std::clamp(type, 0, 19)];
    const double x = (phase - std::floor(phase)) * kOscTableSize;
    const int i = (int) x;
    return table[(size_t) i] + (float) (x - i) * (table[(size_t) i + 1] - table[(size_t) i]);
}

float Sampler::shape(int type, float x, float amount)
{
    // TODO(DEV-14b): law unverified. Amount 0 .. 100 drives 0 .. +24 dB into the curve.
    const float g = std::pow(10.0f, std::clamp(amount, 0.0f, 100.0f) * 0.24f / 20.0f);
    const float u = x * g;
    switch (type)
    {
        case 1: return std::clamp(u, -1.0f, 1.0f);
        case 2: return std::sin(u * juce::MathConstants<float>::halfPi);
        case 3: return std::round(std::clamp(u, -1.0f, 1.0f) * 7.5f) / 7.5f;
        default: return std::tanh(u);
    }
}

double Sampler::lfoBeatQuarters(int index)
{
    if (index < 17) return S::lfoBeatQuarters(index);
    static constexpr double bars[] = {8, 12, 16, 24, 32}; // 2, 3, 4, 6, 8 bars. TODO(DEV-14b): law unverified
    return bars[std::clamp(index - 17, 0, 4)];
}

// ---------------------------------------------------------------- device

Sampler::Sampler(std::vector<Zone> zones, int randomSeed)
    : NativeDevice(makeInfos()), zones_(std::move(zones)), rrRandom_((unsigned) randomSeed | 1u)
{
    oscTables(); // built here, on the message thread
}

std::vector<int> Sampler::displayParameters() const
{
    std::vector<int> result;
    for (int i = 0; i < NumParams; ++i)
        if (! parameters()[(size_t) i].plainValue) result.push_back(i);
    return result;
}

void Sampler::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    reset();
}

void Sampler::reset()
{
    for (auto& v : voices_) v.active = false;
    bendSemis_ = bend_ = modWheel_ = foot_ = pressure_ = 0;
    slide_ = 0;
    sustainPedal_ = false;
    lastNote_ = -1;
    lastPitch_ = -1;
    channelBend_.fill(0); channelPressure_.fill(0); channelSlide_.fill(0);
    for (auto& g : groups_) g = Group{};
    activeVoices_.store(0);
}

int Sampler::filterCircuit() const
{
    const int type = std::clamp((int) std::lround(value(S::FilterType)), 0, 4);
    return type <= 1 ? std::clamp((int) std::lround(value(S::FilterCircuit)), 0, 4) : (value(S::FilterCircuitBp) > 0.5f ? 1 : 0);
}

int Sampler::filterStages(double freq, BiquadCoeffs* out) const
{
    SimplerFilterSettings settings;
    settings.type = std::clamp((int) std::lround(value(S::FilterType)), 0, 4);
    settings.circuit = filterCircuit();
    settings.res = value(S::FilterRes);
    settings.morph = value(S::FilterMorph);
    settings.steep = value(S::FilterSlope) >= 0.5f;
    return simplerFilterStages(settings, sampleRate_, freq, out);
}

// ---------------------------------------------------------------- zones and round robin

int Sampler::selectZones(int note, int velocity, int selector, int* zoneIndices, float* gains)
{
    const bool power = value(LayerCrossfade) > 0.5f;
    bool anySolo = false;
    for (const auto& z : zones_) anySolo |= z.part.solo;
    const int key = note - (int) std::lround(value(S::KeyZoneShift));
    std::array<int, 128> candidates{};
    std::array<float, 128> candidateGains{};
    int count = 0;
    for (int i = 0; i < (int) zones_.size() && count < 128; ++i)
    {
        const auto& part = zones_[(size_t) i].part;
        if (! part.active || (anySolo && ! part.solo)) continue;
        const float gain = zoneFade(key, part.keyMin, part.keyMax, part.keyFadeMin, part.keyFadeMax, power)
                         * zoneFade(velocity, part.velMin, part.velMax, part.velFadeMin, part.velFadeMax, power)
                         * zoneFade(selector, part.selMin, part.selMax, part.selFadeMin, part.selFadeMax, power);
        if (gain <= 0.0f) continue;
        candidates[(size_t) count] = i;
        candidateGains[(size_t) count++] = gain;
    }
    int out = 0;
    if (value(RoundRobin) < 0.5f)
    {
        for (int c = 0; c < count && out < kMaxLayers; ++c)
        {
            zoneIndices[out] = candidates[(size_t) c];
            gains[out++] = candidateGains[(size_t) c];
        }
        return out;
    }
    // Round robin (manual): the zones sharing a key zone take turns; each key zone is a group with its own counter.
    const int mode = std::clamp((int) std::lround(value(RoundRobinMode)), 0, 3);
    std::array<bool, 128> done{};
    for (int c = 0; c < count && out < kMaxLayers; ++c)
    {
        if (done[(size_t) c]) continue;
        const auto& first = zones_[(size_t) candidates[(size_t) c]].part;
        std::array<int, 128> members{};
        int n = 0;
        for (int d = c; d < count; ++d)
        {
            const auto& part = zones_[(size_t) candidates[(size_t) d]].part;
            if (part.keyMin == first.keyMin && part.keyMax == first.keyMax) { members[(size_t) n++] = d; done[(size_t) d] = true; }
        }
        Group* group = nullptr;
        for (auto& g : groups_)
            if (g.keyMin == first.keyMin && g.keyMax == first.keyMax) { group = &g; break; }
        if (group == nullptr)
            for (auto& g : groups_)
                if (g.keyMin < 0) { group = &g; g.keyMin = first.keyMin; g.keyMax = first.keyMax; break; }
        if (group == nullptr) group = &groups_[0];
        int pick = 0;
        if (n > 1)
        {
            if (mode == 0) pick = group->counter % n;
            else if (mode == 1) pick = n - 1 - group->counter % n;
            else
            {
                rrRandom_ = rrRandom_ * 1664525u + 1013904223u;
                pick = (int) ((rrRandom_ >> 8) % (unsigned) n);
                if (mode == 2 && pick == group->last) pick = (pick + 1) % n; // Other: never the same twice in a row
            }
        }
        group->counter++;
        group->last = pick;
        zoneIndices[out] = candidates[(size_t) members[(size_t) pick]];
        gains[out++] = candidateGains[(size_t) members[(size_t) pick]];
    }
    return out;
}

void Sampler::stepRoundRobin()
{
    const int reset = std::clamp((int) std::lround(value(RoundRobinReset)), 0, 7);
    if (reset == 0 || ! host_.playing) return;
    const double period = kResetQuarters[reset];
    const double now = host_.beats;
    if (rrResetBeat_ < 0 || std::floor(now / period) != std::floor(rrResetBeat_ / period) || now < rrResetBeat_)
        for (auto& g : groups_) { g.counter = 0; g.last = -1; }
    rrResetBeat_ = now;
}

// ---------------------------------------------------------------- envelopes

void Sampler::startStage(Env& e, Env::Stage stage, float target, double length, float slope)
{
    // Simpler's measured slope law (DEV-22) between any two levels.
    e.stage = stage;
    e.from = e.level;
    e.target = target;
    e.slope = slope;
    e.elapsed = 0;
    e.length = std::max(1.0, length);
    const double distance = std::abs((double) target - e.from);
    if (slope >= 0.999f)
    {
        e.term = 1.0;
        e.ratio = distance > e.floor ? std::pow(e.floor / distance, 1.0 / e.length) : 0.0;
    }
    else if (slope >= 0)
    {
        e.term = 1.0;
        e.ratio = std::exp(-S::kEnvSlopeRate / e.length);
    }
    else
    {
        e.term = std::exp(-S::kEnvSlopeRate);
        e.ratio = std::exp(S::kEnvSlopeRate / e.length);
    }
}

void Sampler::triggerEnv(Env& e, const EnvShape& shape, double floor)
{
    e = Env{};
    e.active = true;
    e.floor = floor;
    e.level = shape.initial;
    e.nextRepeat = shape.repeat;
    startStage(e, Env::Stage::Attack, shape.peak, shape.attack, shape.attackSlope);
}

float Sampler::stepEnv(Env& e, bool released, const EnvShape& shape)
{
    if (e.stage == Env::Stage::Idle) return e.level;
    // Trigger mode ignores note-off until the decay is over (manual), then releases (Simpler, measured).
    if (released && shape.loopMode != 4 && e.stage != Env::Stage::Release)
        startStage(e, Env::Stage::Release, shape.end, shape.release, shape.releaseSlope);
    // Beat / Sync restart the attack from the current level every repeat period while the note is held (measured).
    e.age += 1.0;
    if ((shape.loopMode == 2 || shape.loopMode == 3) && ! released && e.stage != Env::Stage::Release && e.age >= e.nextRepeat)
    {
        startStage(e, Env::Stage::Attack, shape.peak, shape.attack, shape.attackSlope);
        e.nextRepeat += shape.repeat;
    }
    if (e.stage == Env::Stage::Sustain)
    {
        e.level = shape.sustain;
        return e.level;
    }
    const double x = e.elapsed / e.length;
    if (e.slope >= 0.999f)
        e.level = e.target + (e.from - e.target) * (float) e.term;
    else
    {
        const double k = e.slope >= 0 ? 1.0 - ((1.0 - e.slope) * (1.0 - x) + e.slope * e.term) : (1.0 + e.slope) * x + (-e.slope) * e.term;
        e.level = e.from + (e.target - e.from) * (float) k;
    }
    e.term *= e.ratio;
    e.elapsed += 1.0;
    if (e.elapsed >= e.length)
    {
        e.level = e.target;
        switch (e.stage)
        {
            case Env::Stage::Attack: startStage(e, Env::Stage::Decay, shape.sustain, shape.decay, shape.decaySlope); break;
            case Env::Stage::Decay:
                // Loop: from the sustain level back to the initial level over Loop Time, then the attack again (manual).
                if (shape.loopMode == 1 && ! released) startStage(e, Env::Stage::LoopFall, shape.initial, shape.loopTime, 0.0f);
                else if (shape.loopMode == 4) startStage(e, Env::Stage::Release, shape.end, shape.release, shape.releaseSlope);
                else e.stage = Env::Stage::Sustain;
                break;
            case Env::Stage::LoopFall: startStage(e, Env::Stage::Attack, shape.peak, shape.attack, shape.attackSlope); break;
            case Env::Stage::Release:
            case Env::Stage::Sustain:
            case Env::Stage::Idle: e.stage = Env::Stage::Idle; break;
        }
    }
    return e.level;
}

Sampler::EnvShape Sampler::shapeOf(int block, bool volumeEnvelope, double scale, float velocity, bool scaleAttack) const
{
    const float ms = (float) (sampleRate_ * 0.001);
    const double velScale = timeScale(0, 0, 60, value(block + EnvTimeVel), velocity);
    const auto time = [&](int field, bool scaled) { return std::max(1.0f, value(block + field) * ms * (float) (scaled ? scale * velScale : velScale)); };
    const auto level = [&](int field) { const float l = value(block + field); return volumeEnvelope && l <= kSilentLevel ? 0.0f : l; };
    EnvShape s;
    s.attack = time(EnvAttack, scaleAttack);
    s.decay = time(EnvDecay, true);
    s.release = time(EnvRelease, true);
    s.initial = level(EnvInitial);
    s.peak = level(EnvPeak);
    s.sustain = level(EnvSustain);
    s.end = level(EnvEnd);
    s.attackSlope = value(block + EnvAttackSlope);
    s.decaySlope = value(block + EnvDecaySlope);
    s.releaseSlope = value(block + EnvReleaseSlope);
    s.loopMode = std::clamp((int) std::lround(value(block + EnvLoopMode)), 0, 4);
    s.loopTime = std::max(1.0f, value(block + EnvLoopTime) * ms * (float) scale);
    s.repeat = std::max(1.0f, (float) (S::repeatQuarters((int) std::lround(value(block + EnvRepeat))) * 60.0 / host_.tempo * sampleRate_));
    return s;
}

Sampler::EnvShape Sampler::ampShape(double scale, float velocity, bool scaleAttack) const
{
    // The volume envelope's times / slopes / loop are Simpler parameters; its levels and Time < Vel are Sampler ones.
    const float ms = (float) (sampleRate_ * 0.001);
    const double velScale = timeScale(0, 0, 60, value(AmpTimeVel), velocity);
    const auto time = [&](int index, bool scaled) { return std::max(1.0f, value(index) * ms * (float) (scaled ? scale * velScale : velScale)); };
    const auto level = [&](int index) { const float l = value(index); return l <= kSilentLevel ? 0.0f : l; };
    EnvShape s;
    s.attack = time(S::Attack, scaleAttack);
    s.decay = time(S::Decay, true);
    s.release = time(S::Release, true);
    s.initial = level(AmpInitial);
    s.peak = level(AmpPeak);
    s.sustain = level(S::Sustain);
    s.end = 0.0f; // the volume envelope has no End (always -inf)
    s.attackSlope = value(S::AttackSlope);
    s.decaySlope = value(S::DecaySlope);
    s.releaseSlope = value(S::ReleaseSlope);
    s.loopMode = std::clamp((int) std::lround(value(S::AmpLoopMode)), 0, 4);
    s.loopTime = std::max(1.0f, value(S::AmpLoopTime) * ms * (float) scale);
    s.repeat = std::max(1.0f, (float) (S::repeatQuarters((int) std::lround(value(S::AmpRepeatTime))) * 60.0 / host_.tempo * sampleRate_));
    return s;
}

// ---------------------------------------------------------------- MIDI

void Sampler::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    int pos = 0;
    const int n = audio.getNumSamples();
    for (const auto meta : midi)
    {
        const int at = std::clamp(meta.samplePosition, 0, n);
        render(audio, pos, at);
        pos = at;
        if (meta.numBytes > 3) continue; // SysEx: no heap-backed MidiMessage on the audio thread
        const auto m = meta.getMessage();
        const int channel = std::clamp(m.getChannel(), 1, 16);
        const bool member = channel > 1; // MPE member channel: per-note bend, pressure and slide
        if (m.isNoteOn()) noteOn(m.getNoteNumber(), m.getFloatVelocity(), channel);
        else if (m.isNoteOff()) noteOff(m.getNoteNumber(), m.getFloatVelocity(), channel);
        else if (m.isAllSoundOff()) for (auto& v : voices_) v.active = false;
        else if (m.isAllNotesOff()) for (auto& v : voices_) v.released = true;
        else if (m.isController())
        {
            const int cc = m.getControllerNumber();
            const float value01 = (float) m.getControllerValue() / 127.0f;
            if (cc == 1) modWheel_ = value01;
            else if (cc == 4) foot_ = value01;
            else if (cc == 74) { if (member) channelSlide_[(size_t) channel - 1] = value01; else slide_ = value01; }
            else if (cc == 64)
            {
                sustainPedal_ = m.getControllerValue() >= 64;
                if (! sustainPedal_) for (auto& v : voices_) if (v.active && ! v.keyDown) v.released = true;
            }
            else if (m.isResetAllControllers())
            {
                bend_ = bendSemis_ = modWheel_ = foot_ = pressure_ = 0;
                sustainPedal_ = false;
                for (auto& v : voices_) if (v.active && ! v.keyDown) v.released = true;
            }
        }
        else if (m.isPitchWheel())
        {
            const float bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
            if (member) channelBend_[(size_t) channel - 1] = bend;
            else bend_ = bend;
        }
        else if (m.isChannelPressure())
        {
            const float pressure = (float) m.getChannelPressureValue() / 127.0f;
            if (member) channelPressure_[(size_t) channel - 1] = pressure;
            else pressure_ = pressure;
        }
        else if (m.isAftertouch())
            for (auto& v : voices_)
                if (v.active && v.note == m.getNoteNumber()) v.pressure = (float) m.getAfterTouchValue() / 127.0f;
    }
    render(audio, pos, n);
    samplesSeen_ += n;
    int active = 0;
    for (const auto& v : voices_) active += v.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

void Sampler::noteOn(int note, float velocity, int channel)
{
    const int glide = std::clamp((int) std::lround(value(S::GlideMode)), 0, 2);
    const int limit = glide == 1 ? 1 : S::voiceCount((int) std::lround(value(S::Voices)));
    bool legato = false;
    for (const auto& other : voices_) legato |= other.active && other.keyDown && ! other.released;
    if (value(S::Retrigger) > 0.5f)
        for (auto& v : voices_) if (v.active && v.note == note) v.active = false;
    for (int i = limit; i < kMaxVoices; ++i) voices_[(size_t) i].active = false;
    Voice* voice = nullptr;
    for (int i = 0; i < limit && voice == nullptr; ++i)
        if (! voices_[(size_t) i].active) voice = &voices_[(size_t) i];
    if (voice == nullptr) // steal the oldest
        voice = &*std::min_element(voices_.begin(), voices_.begin() + limit, [](const Voice& a, const Voice& b) { return a.age < b.age; });
    const int fromNote = lastNote_;
    startVoice(*voice, note, velocity, channel, fromNote, legato);
    lastNote_ = note;
}

void Sampler::noteOff(int note, float releaseVelocity, int channel)
{
    for (auto& v : voices_)
        if (v.active && v.note == note && v.keyDown && (v.channel == channel || channel == 1 || v.channel == 1))
        {
            v.keyDown = false;
            v.releaseVelocity = releaseVelocity;
            if (! sustainPedal_) v.released = true;
        }
}

// ---------------------------------------------------------------- voices

void Sampler::startLayer(Voice& v, Layer& layer, const Zone& zone, float gain)
{
    layer = Layer{};
    layer.zone = &zone;
    layer.sample = zone.slot != nullptr ? zone.slot->get() : nullptr;
    if (layer.sample == nullptr || layer.sample->audio.getNumSamples() < 2) { layer.done = true; return; }
    const auto& part = zone.part;
    const auto& sample = *layer.sample;
    const int len = sample.audio.getNumSamples();
    const bool snapOn = value(S::Snap) > 0.5f;
    // Region: SampleEnd is the last frame played (the reference writes DefaultDuration - 1 for a whole file).
    double start = std::clamp((double) part.sampleStart, 0.0, (double) len - 2);
    double end = part.sampleEnd > 0 ? std::min((double) part.sampleEnd + 1, (double) len) : (double) len;
    if (end <= start + 1) end = std::min((double) len, start + 2);
    if (part.loopMode != 0 && part.loopEnd > part.loopStart + 4)
    {
        layer.loopMode = part.loopMode == 2 ? 2 : 1;
        layer.loopStart = std::clamp((double) part.loopStart, start, end - 4);
        layer.loopEnd = std::clamp((double) part.loopEnd, layer.loopStart + 4, end);
        if (part.link) start = layer.loopStart; // Link: Sample Start = Loop Start (manual)
    }
    if (snapOn) // Snap: start and end points to zero crossings of the left channel (manual)
    {
        start = snap(sample, start, 0, end - 2);
        end = std::max(start + 2, snap(sample, end, start + 2, (double) len));
        if (layer.loopMode != 0)
        {
            layer.loopStart = snap(sample, layer.loopStart, start, end - 4);
            layer.loopEnd = std::max(layer.loopStart + 4, snap(sample, layer.loopEnd, layer.loopStart + 4, end));
        }
    }
    layer.start = start;
    layer.end = end;
    if (layer.loopMode == 1) // crossfade before the loop end with the audio before the loop start
        layer.fade = std::max(0.0, std::min({(double) part.loopCrossfade, layer.loopStart - start, (layer.loopEnd - layer.loopStart) * 0.5}));
    layer.relMode = layer.loopMode != 0 ? std::clamp(part.releaseMode, 0, 3) : 0;
    layer.relStart = std::clamp((double) part.releaseStart, start, end - 4);
    if (layer.relMode == 1)
        layer.relFade = std::max(0.0, std::min({(double) part.releaseCrossfade, layer.relStart - start, (end - layer.relStart) * 0.5}));
    const bool reverse = value(Reverse) > 0.5f;
    for (int c = 0; c < 2; ++c)
    {
        layer.direction[c] = reverse ? -1 : 1;
        layer.pos[c] = reverse ? end - 1 : start;
    }
    // Key tracking: Tune Scale 0 % plays every key at the root pitch.
    layer.rootSemis = (v.note - part.rootKey) * part.tuneScale / 100.0 + part.detuneCents / 100.0;
    layer.rate = sample.sampleRate / sampleRate_;
    const float pan = std::clamp((float) part.pan, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    layer.gainL = gain * (float) part.volume * std::sqrt(2.0f) * std::cos(angle);
    layer.gainR = gain * (float) part.volume * std::sqrt(2.0f) * std::sin(angle);
}

double Sampler::snap(const engine::SampleData& sample, double frame, double low, double high) const
{
    // The nearest sign change of the left channel within 4096 frames.
    const float* d = sample.audio.getReadPointer(0);
    const int len = sample.audio.getNumSamples();
    const int at = (int) std::lround(frame);
    for (int k = 0; k < 4096; ++k)
        for (int candidate : {at - k, at + k})
            if (candidate >= std::max(1, (int) low) && candidate <= std::min(len - 1, (int) high)
                && ((d[candidate - 1] <= 0.0f) != (d[candidate] <= 0.0f)))
                return (double) candidate;
    return frame;
}

void Sampler::startVoice(Voice& v, int note, float velocity, int channel, int fromNote, bool legato)
{
    const int vel127 = std::clamp((int) std::lround(velocity * 127.0f), 1, 127);
    // Note-on modulation (sources known before the voice runs): Sample Selector / Sample Offset / loops / Time.
    Voice probe;
    probe.note = note; probe.velocity = velocity; probe.channel = channel;
    Mods start;
    computeMods(probe, start, 0.0f, nullptr, nullptr);
    stepRoundRobin();
    const int selector = std::clamp((int) std::lround(value(SampleSelector) + start.dst[0][DstSampleSelector] * 127.0f), 0, 127);
    std::array<int, kMaxLayers> indices{};
    std::array<float, kMaxLayers> gains{};
    const int count = selectZones(note, vel127, selector, indices.data(), gains.data());
    v.active = false;
    if (count == 0) return;

    v = Voice{};
    v.active = true;
    v.note = note;
    v.channel = channel;
    v.velocity = velocity;
    v.age = ++counter_;
    v.layerCount = count;
    for (int i = 0; i < count; ++i)
    {
        auto& layer = v.layers[(size_t) i];
        startLayer(v, layer, zones_[(size_t) indices[(size_t) i]], gains[(size_t) i]);
        // Sample Offset / Loop Start / Loop Length / Release Loop modulation, fractions of the region at note-on.
        // TODO(DEV-14b): law unverified (and the reference may follow them continuously).
        const double region = layer.end - layer.start;
        if (! layer.done && start.dst[0][DstSampleOffset] > 0 && layer.direction[0] > 0)
            for (double& p : layer.pos) p = std::min(layer.end - 2, p + start.dst[0][DstSampleOffset] * region);
        if (! layer.done && layer.loopMode != 0)
        {
            const double length = layer.loopEnd - layer.loopStart;
            const double shifted = std::clamp(layer.loopStart + start.dst[0][DstLoopStart] * region, layer.start, layer.end - 16);
            const double newLength = std::clamp(length * (1.0 + start.dst[0][DstLoopLength]), 16.0, layer.end - shifted);
            layer.loopStart = shifted;
            layer.loopEnd = shifted + newLength;
            layer.fade = std::min(layer.fade, std::min(layer.loopStart - layer.start, newLength * 0.5));
            layer.relStart = std::clamp(layer.relStart + start.dst[0][DstReleaseLoop] * region, layer.start, layer.end - 4);
        }
    }

    // Volume (Simpler's measured laws): velocity and key scale the level in dB around 70; equal-power pan.
    const float levelDb = value(S::Volume) + value(S::VelToVolume) * 0.4f * ((float) vel127 - 70.0f) + value(S::VolKeyScale) * 0.5f * (float) (note - 70);
    v.levelGain = std::pow(10.0f, levelDb / 20.0f);
    random_ = random_ * 1664525u + 1013904223u;
    const float randomPan = value(S::PanRandom) * ((float) (random_ >> 8) / 8388608.0f - 1.0f);
    v.panBase = value(S::Pan) + value(S::PanKeyScale) * (float) (note - 70) / 64.0f + randomPan;

    // Envelopes with the global Time / Time < Key and the Time destination at note-on.
    const bool scaleAttack = value(EnvTimeAttack) > 0.5f;
    v.timeScale = timeScale(value(EnvTime), value(EnvTimeKey), note, 0, 0) * std::exp2(2.0 * start.dst[0][DstTime]);
    v.ampShape = ampShape(v.timeScale, velocity, scaleAttack);
    // Filter / pitch envelopes: Simpler's time parameters plus the Sampler's levels and loop.
    const auto simplerShape = [&](int attack, int decay, int sustain, int release, int aSlope, int dSlope, int rSlope, int levels, int loop, int timeVel) {
        const float ms = (float) (sampleRate_ * 0.001);
        const double velScale = timeScale(0, 0, 60, value(timeVel), velocity);
        EnvShape s;
        s.attack = std::max(1.0f, value(attack) * ms * (float) ((scaleAttack ? v.timeScale : 1.0) * velScale));
        s.decay = std::max(1.0f, value(decay) * ms * (float) (v.timeScale * velScale));
        s.release = std::max(1.0f, value(release) * ms * (float) (v.timeScale * velScale));
        s.initial = value(levels); s.peak = value(levels + 1); s.end = value(levels + 2); s.sustain = value(sustain);
        s.attackSlope = value(aSlope); s.decaySlope = value(dSlope); s.releaseSlope = value(rSlope);
        s.loopMode = std::clamp((int) std::lround(value(loop)), 0, 4);
        s.loopTime = std::max(1.0f, value(loop + 1) * ms * (float) v.timeScale);
        s.repeat = std::max(1.0f, (float) (S::repeatQuarters((int) std::lround(value(loop + 2))) * 60.0 / host_.tempo * sampleRate_));
        return s;
    };
    v.filterShape = simplerShape(S::FilterAttack, S::FilterDecay, S::FilterSustain, S::FilterRelease, S::FilterAttackSlope,
                                 S::FilterDecaySlope, S::FilterReleaseSlope, FilterInitial, FilterLoopMode, FilterTimeVel);
    v.pitchShape = simplerShape(S::PitchAttack, S::PitchDecay, S::PitchSustain, S::PitchRelease, S::PitchAttackSlope,
                                S::PitchDecaySlope, S::PitchReleaseSlope, PitchInitial, PitchLoopMode, PitchTimeVel);
    v.auxShape = shapeOf(AuxEnv, false, v.timeScale, velocity, scaleAttack);
    v.oscShape = shapeOf(OscEnv, true, v.timeScale, velocity, scaleAttack);
    triggerEnv(v.amp, v.ampShape, S::kEnvSlopeFloor);
    triggerEnv(v.filter, v.filterShape, S::kEnvSlopeFloor);
    triggerEnv(v.pitch, v.pitchShape, S::kPitchSlopeFloor);
    triggerEnv(v.aux, v.auxShape, S::kEnvSlopeFloor);
    triggerEnv(v.osc, v.oscShape, S::kEnvSlopeFloor);
    // Sync loops restart on the song's grid: the first restart waits for the next grid line.
    for (const auto& pair : {std::pair{&v.amp, &v.ampShape}, std::pair{&v.filter, &v.filterShape}, std::pair{&v.pitch, &v.pitchShape},
                       std::pair{&v.aux, &v.auxShape}, std::pair{&v.osc, &v.oscShape}})
        if (pair.second->loopMode == 3 && host_.playing)
        {
            const double period = pair.second->repeat, samplesPerBeat = 60.0 / host_.tempo * sampleRate_;
            const double now = host_.beats * samplesPerBeat;
            pair.first->nextRepeat = std::max(1.0, period - std::fmod(now, period));
        }

    // LFOs: Retrigger starts at Offset (LFO 2 / 3's right channel at Offset + Phase); otherwise the free-running ones.
    const auto startLfo = [&](dsp::Lfo& lfo, const dsp::Lfo& free, bool retrigger, double phaseDegrees) {
        if (retrigger) lfo.setPhase(phaseDegrees / 360.0);
        else lfo = free;
    };
    startLfo(v.lfo1, freeLfo1_, value(S::LfoRetrigger) > 0.5f, value(S::LfoOffset));
    for (int n = 0; n < 2; ++n)
    {
        const int base = n == 0 ? Lfo2 : Lfo3;
        auto* lfo = n == 0 ? v.lfo2 : v.lfo3;
        const auto* free = n == 0 ? freeLfo2_ : freeLfo3_;
        const bool retrigger = value(base + LfoRetrig) > 0.5f;
        const bool phaseMode = value(base + LfoStereoMode) < 0.5f;
        startLfo(lfo[0], free[0], retrigger, value(base + LfoOffset));
        startLfo(lfo[1], free[1], retrigger, value(base + LfoOffset) + (phaseMode ? value(base + LfoPhase) : 0.0f));
    }
    // LFO Key (Simpler, measured): rate x (1 + key 2^((note - 60) / 12)).
    v.lfo1Scale = 1.0 + value(S::LfoKeyScale) * std::exp2((note - 60) / 12.0);
    v.lfo2Scale = 1.0 + value(Lfo2 + LfoKeyScale) * std::exp2((note - 60) / 12.0);
    v.lfo3Scale = 1.0 + value(Lfo3 + LfoKeyScale) * std::exp2((note - 60) / 12.0);
    v.noise = (unsigned) (counter_ * 2654435761u) | 1u;
    // Velocity / key -> cutoff (Simpler, measured).
    v.filterKeySemis = value(S::FilterKeyTrack) * (float) (note - 60) + value(S::FilterVelMod) * ((float) vel127 - 70.0f);

    // Glide (linear in semitones over Time; Simpler, measured): Glide is mono and legato only; Portamento glides every
    // new note from the last one, polyphonically (manual). TODO(DEV-14b): Portamento law unverified.
    const int glide = std::clamp((int) std::lround(value(S::GlideMode)), 0, 2);
    if (fromNote >= 0 && fromNote != note && (glide == 2 || (glide == 1 && legato)))
    {
        v.glideSemis = (double) (fromNote - note);
        v.glideStep = -v.glideSemis / std::max(1.0, (double) value(S::GlideTime) * 0.001 * sampleRate_);
    }
}

// ---------------------------------------------------------------- modulation

void Sampler::computeMods(const Voice& v, Mods& m, const float lfo1, const float* lfo2, const float* lfo3) const
{
    // LFO 2 / 3's A / B amounts are destinations too: they follow the previous sample's modulation.
    float scale[2][4];
    for (int c = 0; c < 2; ++c)
    {
        scale[c][0] = 1.0f + m.dst[c][DstLfo2A]; scale[c][1] = 1.0f + m.dst[c][DstLfo2B];
        scale[c][2] = 1.0f + m.dst[c][DstLfo3A]; scale[c][3] = 1.0f + m.dst[c][DstLfo3B];
    }
    for (auto& channel : m.dst) std::fill(std::begin(channel), std::end(channel), 0.0f);
    const auto route = [&](int base, float left, float right, int scaled = -1) {
        for (int c = 0; c < 2; ++c)
        {
            const int destination = (int) std::lround(value(base + 2 * c));
            if (destination <= 0 || destination >= DstCount) continue;
            const float amount = value(base + 2 * c + 1) / 100.0f;
            m.dst[0][destination] += left * amount * (scaled < 0 ? 1.0f : scale[0][scaled + c]);
            m.dst[1][destination] += right * amount * (scaled < 0 ? 1.0f : scale[1][scaled + c]);
        }
    };
    // Sources (TODO(DEV-14b): law unverified): Key (note - 60) / 64, Velocity / Off Velocity 0..1, Pressure / Mod
    // Wheel / Foot / Slide 0..1, Pitch Bend / Note PB -1..1, Aux envelope level, LFO 2 / 3 -1..1 per channel.
    const float key = (float) (v.note - 60) / 64.0f;
    route(KeyMod, key, key);
    route(VelMod, v.velocity, v.velocity);
    if (v.released) route(RelVelMod, v.releaseVelocity, v.releaseVelocity);
    const size_t ch = (size_t) std::clamp(v.channel, 1, 16) - 1;
    const bool member = v.channel > 1;
    const float pressure = std::max({pressure_, v.pressure, member ? channelPressure_[ch] : 0.0f});
    const float notePb = member ? channelBend_[ch] : 0.0f;
    const float slide = member ? channelSlide_[ch] : slide_;
    const float midi[MidiRows] = {pressure, bend_, modWheel_, foot_, slide, notePb};
    for (int row = 0; row < MidiRows; ++row) route(MidiMod + row * ModFieldCount, midi[row], midi[row]);
    if (v.aux.active && value(AuxOn) > 0.5f) route(AuxMod, v.aux.level, v.aux.level);
    if (lfo2 != nullptr && value(Lfo2 + LfoOnField) > 0.5f) route(Lfo2 + LfoFieldCount, lfo2[0], lfo2[1], 0);
    if (lfo3 != nullptr && value(Lfo3 + LfoOnField) > 0.5f) route(Lfo3 + LfoFieldCount, lfo3[0], lfo3[1], 2);
    (void) lfo1;
}

// ---------------------------------------------------------------- playback

float Sampler::readLayer(const Layer& layer, int channel, double step) const
{
    const auto& audio = layer.sample->audio;
    const float* d = audio.getReadPointer(channel > 0 && audio.getNumChannels() > 1 ? 1 : 0);
    const int len = audio.getNumSamples();
    const int mode = std::clamp((int) std::lround(value(S::Interpolation)), 0, 3);
    // Good / Best: the measured kernel; Normal: cubic; 0: linear. TODO(DEV-14b): law unverified (Normal / Good differ in
    // the reference).
    const auto read = [&](double pos) -> float {
        if (mode >= 2) return S::readSample(d, len, pos, step);
        if (mode == 1) return hermite(d, len, pos);
        const int i = (int) std::floor(pos);
        const float t = (float) (pos - i);
        return d[std::clamp(i, 0, len - 1)] * (1 - t) + d[std::clamp(i + 1, 0, len - 1)] * t;
    };
    const double pos = layer.pos[channel];
    float x = read(pos);
    // Forward loop crossfades: the end of the loop fades into the audio before its start (constant power by default).
    const bool sustainLooping = layer.loopMode == 1 && (! layer.releasing || layer.relMode == 3) && layer.direction[channel] > 0;
    const bool releaseLooping = layer.releasing && layer.relMode == 1 && layer.inRelease && layer.direction[channel] > 0;
    const double loopEnd = sustainLooping ? layer.loopEnd : layer.end;
    const double fade = sustainLooping ? layer.fade : releaseLooping ? layer.relFade : 0.0;
    const double loopLength = sustainLooping ? layer.loopEnd - layer.loopStart : layer.end - layer.relStart;
    if (fade > 0 && pos >= loopEnd - fade && pos < loopEnd)
    {
        const double t = (pos - (loopEnd - fade)) / fade;
        const bool constPower = value(S::ConstPowerFade) > 0.5f;
        const float in = constPower ? (float) std::sin(t * kPi * 0.5) : (float) t;
        const float out = constPower ? (float) std::cos(t * kPi * 0.5) : 1.0f - (float) t;
        x = x * out + read(pos - loopLength) * in;
    }
    return x;
}

bool Sampler::advanceLayer(Layer& layer, int c, double step) const
{
    double& pos = layer.pos[c];
    int& dir = layer.direction[c];
    pos += dir * step;
    const bool sustain = layer.loopMode != 0 && (! layer.releasing || layer.relMode == 3);
    if (dir > 0)
    {
        if (sustain && pos >= layer.loopEnd && ! layer.inRelease)
        {
            layer.inLoop = true;
            if (layer.loopMode == 2) { dir = -1; pos = 2 * layer.loopEnd - pos; }
            else pos = layer.loopStart + std::fmod(pos - layer.loopEnd, layer.loopEnd - layer.loopStart);
        }
        else if (pos >= layer.end)
        {
            // Release loop (after the release has played on to the sample end).
            if (layer.releasing && (layer.relMode == 1 || layer.relMode == 2) && layer.end - layer.relStart > 4)
            {
                layer.inRelease = true;
                if (layer.relMode == 2) { dir = -1; pos = 2 * layer.end - pos - 1; }
                else pos = layer.relStart + std::fmod(pos - layer.end, layer.end - layer.relStart);
            }
            else return false;
        }
    }
    else
    {
        const bool reverse = value(Reverse) > 0.5f;
        // Released out of a back-and-forth sustain loop: play on forward to the sample end (Release Mode ->|, loops).
        if (layer.releasing && layer.relMode != 3 && ! layer.inRelease && ! reverse) { dir = 1; return true; }
        if (layer.inRelease && pos <= layer.relStart) { dir = 1; pos = 2 * layer.relStart - pos; }
        else if (sustain && pos <= layer.loopStart && (layer.inLoop || reverse))
        {
            layer.inLoop = true;
            if (layer.loopMode == 2) { dir = 1; pos = 2 * layer.loopStart - pos; }
            else pos = layer.loopEnd - std::fmod(layer.loopStart - pos, layer.loopEnd - layer.loopStart); // reverse forward loop
        }
        else if (pos < layer.start) return false;
    }
    return pos >= 0 && pos < (double) layer.sample->audio.getNumSamples();
}

void Sampler::render(juce::AudioBuffer<float>& audio, int from, int to)
{
    if (to <= from) return;
    // Free-running LFOs (Retrigger off) keep their phase across notes.
    const auto freeRate = [&](int rateType, int beat, int freq) {
        return value(rateType) > 0.5f ? host_.tempo / 60.0 / lfoBeatQuarters((int) std::lround(value(beat))) : (double) value(freq);
    };
    const double r1 = (value(S::LfoSync) > 0.5f ? host_.tempo / 60.0 / lfoBeatQuarters((int) std::lround(value(S::LfoBeat))) : value(S::LfoRate)) / sampleRate_;
    const double r2 = freeRate(Lfo2 + LfoRateType, Lfo2 + LfoBeatRate, Lfo2 + LfoFrequency) / sampleRate_;
    const double r3 = freeRate(Lfo3 + LfoRateType, Lfo3 + LfoBeatRate, Lfo3 + LfoFrequency) / sampleRate_;
    const double spin2 = value(Lfo2 + LfoStereoMode) > 0.5f ? 1.0 + 0.5 * value(Lfo2 + LfoSpin) : 1.0;
    const double spin3 = value(Lfo3 + LfoStereoMode) > 0.5f ? 1.0 + 0.5 * value(Lfo3 + LfoSpin) : 1.0;
    for (int i = from; i < to; ++i)
    {
        freeLfo1_.next(r1, dsp::Lfo::Shape::Sine);
        freeLfo2_[0].next(r2, dsp::Lfo::Shape::Sine); freeLfo2_[1].next(r2 * spin2, dsp::Lfo::Shape::Sine);
        freeLfo3_[0].next(r3, dsp::Lfo::Shape::Sine); freeLfo3_[1].next(r3 * spin3, dsp::Lfo::Shape::Sine);
    }
    bendSemis_ = bend_ * value(PitchBendRangeParam);
    float* outL = audio.getWritePointer(0);
    float* outR = audio.getWritePointer(audio.getNumChannels() > 1 ? 1 : 0);
    for (auto& v : voices_)
        if (v.active) renderVoice(v, outL, outR, from, to);
}

void Sampler::renderVoice(Voice& v, float* outL, float* outR, int from, int to)
{
    const float smoothing = 1.0f - (float) std::exp(-1.0 / (0.0006 * sampleRate_));
    const bool filterOn = value(S::FilterOn) > 0.5f, filterEnvOn = value(S::FilterEnvOn) > 0.5f;
    const bool pitchEnvOn = value(S::PitchEnvOn) > 0.5f, lfo1On = value(S::LfoOn) > 0.5f;
    const bool lfo2On = value(Lfo2 + LfoOnField) > 0.5f, lfo3On = value(Lfo3 + LfoOnField) > 0.5f;
    const bool auxOn = value(AuxOn) > 0.5f, oscOn = value(OscOn) > 0.5f, shaperOn = value(ShaperOn) > 0.5f;
    const bool shaperPost = value(ShaperPost) > 0.5f, am = value(OscMode) > 0.5f;
    const int shaperType = std::clamp((int) std::lround(value(ShaperType)), 0, 3);
    const int oscType = std::clamp((int) std::lround(value(OscType)), 0, 20);
    const double oscRatioValue = oscRatio(value(OscCoarse), value(OscFine));
    const double oscFixedHz = value(OscFixed) > 0.5f ? value(OscFixedFreq) * oscFixedMultiplier((int) std::lround(value(OscFixedMul))) : 0.0;
    const float oscVelScale = 1.0f - value(OscVolVel) * (1.0f - v.velocity);
    const int circuit = filterOn ? filterCircuit() : 0;
    const double transpose = value(S::Transpose) + value(S::Detune) / 100.0;
    const double spreadCents = value(S::Spread) / 10.0; // Simpler, measured: L down, R up by Spread / 10 cents
    const float pitchEnvAmount = value(S::PitchEnvAmount);
    const double mpeRange = value(MpeBendRange);
    const size_t ch = (size_t) std::clamp(v.channel, 1, 16) - 1;
    const auto lfoRate = [&](int rateType, int beat, int freq) {
        return value(rateType) > 0.5f ? host_.tempo / 60.0 / lfoBeatQuarters((int) std::lround(value(beat))) : (double) value(freq);
    };
    const double rate1 = (value(S::LfoSync) > 0.5f ? host_.tempo / 60.0 / lfoBeatQuarters((int) std::lround(value(S::LfoBeat))) : value(S::LfoRate)) * v.lfo1Scale;
    const double rate2 = lfoRate(Lfo2 + LfoRateType, Lfo2 + LfoBeatRate, Lfo2 + LfoFrequency) * v.lfo2Scale;
    const double rate3 = lfoRate(Lfo3 + LfoRateType, Lfo3 + LfoBeatRate, Lfo3 + LfoFrequency) * v.lfo3Scale;
    const double spin2 = value(Lfo2 + LfoStereoMode) > 0.5f ? 1.0 + 0.5 * value(Lfo2 + LfoSpin) : 1.0;
    const double spin3 = value(Lfo3 + LfoStereoMode) > 0.5f ? 1.0 + 0.5 * value(Lfo3 + LfoSpin) : 1.0;
    const auto shapeOfLfo = [&](int index) { return kShapes[std::clamp((int) std::lround(value(index)), 0, 5)]; };
    const auto shape1 = shapeOfLfo(S::LfoShape), shape2 = shapeOfLfo(Lfo2 + LfoType), shape3 = shapeOfLfo(Lfo3 + LfoType);
    const double attack1 = std::max(1.0, value(S::LfoAttack) * sampleRate_ * 0.001);
    const double attack2 = std::max(1.0, value(Lfo2 + LfoAttackField) * sampleRate_ * 0.001);
    const double attack3 = std::max(1.0, value(Lfo3 + LfoAttackField) * sampleRate_ * 0.001);
    Mods m;

    // One LFO output: Simpler's measured S&H (two steps per cycle) and 0.6 ms two-stage smoothing, faded in over Attack.
    const auto runLfo = [&](dsp::Lfo& lfo, dsp::Lfo::Shape shape, double increment, float (&smooth)[2], double attack, bool simplerRandom) {
        float x = lfo.next(increment * (shape == dsp::Lfo::Shape::Random ? 2.0 : 1.0), shape);
        if (shape == dsp::Lfo::Shape::Random && simplerRandom) x = 0.25f * (x - 1.0f);
        if (v.lfoAge < 0.5) smooth[0] = smooth[1] = x;
        for (float& stage : smooth) x = stage += (x - stage) * smoothing;
        return v.lfoAge < attack ? x * (float) (v.lfoAge / attack) : x;
    };

    for (int i = from; i < to; ++i)
    {
        const float env = stepEnv(v.amp, v.released, v.ampShape);
        if (v.amp.stage == Env::Stage::Release && v.amp.level < kReleaseCut && v.amp.from >= kReleaseCut) v.amp.stage = Env::Stage::Idle;
        if (v.amp.stage == Env::Stage::Idle) { v.active = false; return; }
        if (auxOn) stepEnv(v.aux, v.released, v.auxShape);

        // LFOs (rates follow their matrix Rate destinations from the previous sample).
        float lfo1 = 0, lfo2[2] = {0, 0}, lfo3[2] = {0, 0};
        if (lfo1On) lfo1 = runLfo(v.lfo1, shape1, rate1 * std::exp2(4.0 * m.dst[0][DstLfo1Rate]) / sampleRate_, v.lfoSmooth[0][0], attack1, true);
        for (int c = 0; c < 2; ++c)
        {
            if (lfo2On) lfo2[c] = runLfo(v.lfo2[c], shape2, rate2 * (c == 1 ? spin2 : 1.0) * std::exp2(4.0 * m.dst[c][DstLfo2Rate]) / sampleRate_, v.lfoSmooth[1][c], attack2, false);
            if (lfo3On) lfo3[c] = runLfo(v.lfo3[c], shape3, rate3 * (c == 1 ? spin3 : 1.0) * std::exp2(4.0 * m.dst[c][DstLfo3Rate]) / sampleRate_, v.lfoSmooth[2][c], attack3, false);
        }
        v.lfoAge += 1.0;
        computeMods(v, m, lfo1, lfo2, lfo3);

        // Pitch (semitones) per channel.
        const float pitchEnv = pitchEnvOn ? stepEnv(v.pitch, v.released, v.pitchShape) : 0.0f;
        const float lfo1PitchAmount = std::clamp(value(S::LfoPitch) + m.dst[0][DstLfo1Pitch], 0.0f, 1.0f);
        const double common = transpose + v.glideSemis + bendSemis_ + (v.channel > 1 ? channelBend_[ch] * mpeRange : 0.0)
                            + pitchEnv * (pitchEnvAmount + 48.0f * m.dst[0][DstPitchEnv])
                            + lfo1 * 12.0f * std::log2(1.0f + 0.5f * lfo1PitchAmount);
        // Modulation oscillator (follows the played pitch and the pitch envelope, manual).
        float oscOut = 0, oscLevel = 0;
        if (oscOn)
        {
            const float oscEnv = stepEnv(v.osc, v.released, v.oscShape);
            oscLevel = std::clamp(value(OscVolume) * oscVelScale + m.dst[0][DstOscVolume], 0.0f, 1.0f) * oscEnv;
            const double oscSemis = v.note + common + 12.0 * (m.dst[0][DstPitch] + m.dst[0][DstPitchOsc]) - 69.0;
            const double hz = oscFixedHz > 0 ? oscFixedHz : 440.0 * std::exp2(oscSemis / 12.0) * oscRatioValue;
            v.oscPhase += hz / sampleRate_;
            v.oscPhase -= std::floor(v.oscPhase);
            oscOut = oscWave(oscType, v.oscPhase, v.noise);
        }
        double step[2];
        for (int c = 0; c < 2; ++c)
        {
            // Matrix Pitch / Pitch Sample: +-12 semitones at 100 %. TODO(DEV-14b): law unverified.
            const double semis = common + 12.0 * (m.dst[c][DstPitch] + m.dst[c][DstPitchSample]) + (c == 0 ? -spreadCents : spreadCents) / 100.0;
            step[c] = std::exp2(semis / 12.0);
            // FM: the oscillator swings the playback rate by +-Volume (linear FM). TODO(DEV-14b): law unverified.
            if (oscOn && ! am) step[c] *= std::max(0.0, 1.0 + (double) (oscOut * oscLevel));
        }

        // Layers.
        float x[2] = {0, 0};
        int playing = 0;
        for (int l = 0; l < v.layerCount; ++l)
        {
            auto& layer = v.layers[(size_t) l];
            if (layer.done) continue;
            if (v.released && ! layer.releasing) layer.releasing = true;
            const double tune = std::exp2(layer.rootSemis / 12.0) * layer.rate
                              * (layer.inRelease ? std::exp2(layer.zone->part.releaseDetune / 1200.0)
                                 : layer.inLoop ? std::exp2(layer.zone->part.loopDetune / 1200.0) : 1.0);
            for (int c = 0; c < 2; ++c) x[c] += readLayer(layer, c, step[c] * tune) * (c == 0 ? layer.gainL : layer.gainR);
            bool alive = true;
            for (int c = 0; c < 2; ++c) alive = advanceLayer(layer, c, step[c] * tune) && alive;
            if (! alive) layer.done = true;
            else ++playing;
        }
        if (playing == 0) { v.active = false; return; }

        // Shaper (pre or post filter) and the filter, per channel.
        const float shaperAmount = value(ShaperAmount) + 100.0f * m.dst[0][DstShaper];
        if (shaperOn && ! shaperPost) for (float& s : x) s = shape(shaperType, s, shaperAmount);
        if (filterOn)
        {
            const float filterEnv = filterEnvOn ? stepEnv(v.filter, v.released, v.filterShape) : 0.0f;
            if (--v.filterCountdown <= 0)
            {
                v.filterCountdown = 16;
                for (int c = 0; c < 2; ++c)
                {
                    const auto same = [&](int d) { return std::abs(m.dst[1][d] - m.dst[0][d]) < 1e-7f; };
                    if (c == 1 && same(DstFilterFreq) && same(DstFilterRes) && same(DstFilterQ) && same(DstFilterMorph) && same(DstFilterEnv)
                        && same(DstFilterDrive) && same(DstLfo1Filter))
                    {
                        v.coeffs[1][0] = v.coeffs[0][0]; v.coeffs[1][1] = v.coeffs[0][1];
                        break;
                    }
                    // Matrix: Filter Freq +-60 st, Res / Q +-1, Morph +-1, Env +-72 st at 100 %. TODO(DEV-14b): law unverified.
                    const double lfoFilter = std::clamp(value(S::FilterLfoMod) + 24.0f * m.dst[c][DstLfo1Filter], 0.0f, 24.0f);
                    double semis = v.filterKeySemis + lfoFilter * lfo1 + 60.0 * m.dst[c][DstFilterFreq];
                    if (filterEnvOn) semis += (value(S::FilterEnvAmount) + 72.0 * m.dst[c][DstFilterEnv]) * filterEnv;
                    SimplerFilterSettings settings;
                    settings.type = std::clamp((int) std::lround(value(S::FilterType)), 0, 4);
                    settings.circuit = circuit;
                    settings.res = value(S::FilterRes) + m.dst[c][DstFilterRes] + m.dst[c][DstFilterQ];
                    settings.morph = value(S::FilterMorph) + m.dst[c][DstFilterMorph];
                    settings.steep = value(S::FilterSlope) >= 0.5f;
                    BiquadCoeffs stages[2];
                    v.filterStageCount = simplerFilterStages(settings, sampleRate_, value(S::FilterFreq) * std::exp2(semis / 12.0), stages);
                    for (int s = 0; s < v.filterStageCount; ++s) v.coeffs[c][s] = SvfCoeffs::fromBiquad(stages[s]);
                }
            }
            for (int c = 0; c < 2; ++c)
            {
                float s = x[c];
                if (circuit != 0)
                    s = S::saturate(s * dsp::dbToGain(std::clamp(value(S::FilterDrive) + 24.0f * m.dst[c][DstFilterDrive], 0.0f, 24.0f)), circuit);
                s = v.f[c][0].process(v.coeffs[c][0], s);
                if (v.filterStageCount > 1) s = v.f[c][1].process(v.coeffs[c][1], s);
                x[c] = s;
            }
        }
        if (shaperOn && shaperPost) for (float& s : x) s = shape(shaperType, s, shaperAmount);

        // Gain: envelope, level, LFO 1 volume (Simpler's law), matrix Volume (x (1 + amount), 0 .. 2), AM.
        const float lfo1Volume = std::clamp(value(S::LfoVolume) + m.dst[0][DstLfo1Vol], 0.0f, 1.0f);
        const float lfo1Pan = std::clamp(value(S::LfoPan) + m.dst[0][DstLfo1Pan], 0.0f, 1.0f);
        const float g = env * v.levelGain * (lfo1On ? 1 - lfo1Volume * (1 - lfo1) * 0.5f : 1.0f) * (oscOn && am ? 1.0f + oscOut * oscLevel : 1.0f);
        const float pan = std::clamp(v.panBase + (m.dst[0][DstPan] + m.dst[1][DstPan]) * 0.5f + (lfo1On ? lfo1 * lfo1Pan : 0.0f), -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        outL[i] += x[0] * g * std::clamp(1.0f + m.dst[0][DstVolume], 0.0f, 2.0f) * std::sqrt(2.0f) * std::cos(angle);
        outR[i] += x[1] * g * std::clamp(1.0f + m.dst[1][DstVolume], 0.0f, 2.0f) * std::sqrt(2.0f) * std::sin(angle);

        if (v.glideStep != 0.0)
        {
            v.glideSemis += v.glideStep;
            if ((v.glideStep > 0) == (v.glideSemis > 0)) { v.glideSemis = 0; v.glideStep = 0; }
        }
    }
}

} // namespace sonora::devices
