#include "devices/native/Tension.h"

#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
using T = Tension;
constexpr double kTwoPi = 2.0 * dsp::kPi;

// Exciter strengths in the waveguide's units (string impedance 1; calibrated against the reference's default levels).
constexpr double kPlectrumForce = 0.00022, kBowSpeed = 0.000164;

// ------------------------------------------------------------------------------------------------ value laws
// Shown values were read from 24 captures of the reference's face with every parameter at k / 11 (docs/TENSION.md).

// Log-linear interpolation through measured (value, shown) points.
template <size_t N>
float logTable(const std::array<std::pair<float, float>, N>& points, float v)
{
    v = std::clamp(v, points.front().first, points.back().first);
    for (size_t i = 1; i < N; ++i)
        if (v <= points[i].first)
        {
            const auto [x0, y0] = points[i - 1];
            const auto [x1, y1] = points[i];
            const float t = (v - x0) / std::max(1e-6f, x1 - x0);
            return y0 * std::pow(y1 / y0, t);
        }
    return points.back().second;
}

constexpr const char* kFilterNames[T::kFilterTypes] = {"LP12", "LP24", "BP6", "BP12", "N2P", "N4P", "HP12", "HP24", "F6", "F12"};
constexpr const char* kTargetNames[T::kTargets] = {"None", "Voice Volume", "Vibrato Amount", "Vibrato Speed", "Unison Detune",
                                                   "String Inharmon.", "LFO Rate", "F. Cutoff", "F. Cut. LFO Depth",
                                                   "F. Cut. Env Depth", "F. Q", "F. Q LFO Depth", "F. Q Env Depth"};
// LFO Sync: eight divisions, each dotted / plain / triplet. Indices 18-23 were read from captures (1/16d .. 1/32t);
// the order of 0-17 follows the same pattern (assumed).
constexpr const char* kSyncNames[T::kSyncRates] = {"4d", "4", "4t", "2d", "2", "2t", "1d", "1", "1t", "1/2d", "1/2", "1/2t",
                                                   "1/4d", "1/4", "1/4t", "1/8d", "1/8", "1/8t", "1/16d", "1/16", "1/16t",
                                                   "1/32d", "1/32", "1/32t"};
constexpr int kPolyphony[T::kPolyphonies] = {1, 2, 4, 8, 12, 16, 20, 24, 28, 32};

std::vector<ParameterInfo> tensionParameters()
{
    std::vector<ParameterInfo> p((size_t) T::NumParams);
    auto set = [&](int i, std::string id, std::string name, float lo, float hi, float def, std::string unit = {},
                   bool toggle = false, std::vector<std::string> choices = {}) {
        p[(size_t) i] = makeParam(std::move(id), std::move(name), lo, hi, def, std::move(unit), toggle, std::move(choices));
    };
    std::vector<std::string> voices;
    for (int i = 0; i < T::kPolyphonies; ++i) voices.push_back(i == 0 ? "Mono" : std::to_string(kPolyphony[i]));
    // Defaults = the reference's default preset (Core Library/Defaults/Instruments/Tension.adv).
    set(T::Polyphony, "Polyphony", "Voices", 0, 9, 4, "", false, voices);
    set(T::PitchBendRange, "PitchBendRange", "PB Range", 0, 12, 2, "st");
    set(T::Octave, "Octave", "Octave", -3, 3, 0);
    set(T::Transpose, "Transpose", "Semi", -12, 12, 0, "st");
    set(T::FineTune, "KeyboardFineTune", "Detune", 0, 1, 0.5f);
    set(T::Priority, "KeyboardPriority", "Priority", 0, 2, 0, "", false, {"High", "Low", "Last"});
    set(T::UnisonOn, "KeyboardUnisonToggle", "Unison", 0, 1, 0, "", true);
    set(T::UnisonVoices, "KeyboardUnison", "Voices", 0, 1, 0, "", false, {"2", "4"});
    set(T::UnisonDetune, "KeyboardDetune", "Detune", 0, 1, 0, "%");
    set(T::UnisonDelay, "KeyboardUnisonDelay", "Delay", 0, 1, 0.5f);
    set(T::Stretch, "KeyboardStretch", "Stretch", 0, 1, 0.5f);
    set(T::KeyError, "KeyboardError", "Error", 0, 1, 0, "%");
    set(T::VibratoOn, "VibratoToggle", "Vibrato", 0, 1, 0, "", true);
    set(T::VibratoRate, "VibratoSpeed", "Rate", 0, 1, 0.5f);
    set(T::VibratoFadeIn, "VibratoFadeIn", "Attack", 0, 1, 0.5f);
    set(T::VibratoAmount, "VibratoAmount", "Amount", 0, 1, 0.5f, "%");
    set(T::VibratoError, "VibratoError", "Error", 0, 1, 0.5f, "%");
    set(T::VibratoDelay, "VibratoDelay", "Delay", 0, 1, 0.5f);
    set(T::VibratoModWheel, "VibratoModWheel", "Mod", 0, 1, 0.5f, "%");
    set(T::PortamentoOn, "PortamentoToggle", "Portamento", 0, 1, 0, "", true);
    set(T::PortamentoTime, "PortamentoTime", "Time", 0, 1, 0.5f, "%");
    set(T::PortamentoProportional, "PortamentoProportional", "Prop.", 0, 1, 0, "", true);
    set(T::PortamentoLegato, "PortamentoLegato", "Legato", 0, 1, 0, "", true);
    set(T::ExcPosition, "GeoExcitatorPosition", "Position", 0, 1, 0.5f);
    set(T::ExcPositionAbs, "GeoExcitatorAbsolute", "Fix.Pos", 0, 1, 0, "", true);
    set(T::ExcPositionKey, "GeoExcitatorKbdModulation", "Position < Key", -1, 1, 0);
    set(T::ExcPositionVel, "GeoExcitatorVelModulation", "Position < Vel", -1, 1, 0);
    set(T::DamperPosition, "GeoDamperPosition", "Position", 0, 1, 0.5f);
    set(T::DamperPositionAbs, "GeoDamperAbsolute", "Fix.Pos", 0, 1, 0, "", true);
    set(T::DamperPositionKey, "GeoDamperKbdModulation", "Position < Key", -1, 1, 0);
    set(T::DamperPositionVel, "GeoDamperVelModulation", "Position < Vel", -1, 1, 0);
    set(T::ExcOn, "ExcitatorToggle", "Exciter", 0, 1, 1, "", true);
    set(T::ExcType, "ExcitatorType", "Type", 0, 3, 3, "", false, {"Bow", "Hammer", "Hammer (bouncing)", "Plectrum"});
    set(T::ExcX, "ExcitatorParameterX", "Protrusion", 0, 1, 0.5f, "%");
    set(T::ExcXKey, "ExcitatorParameterXKbdMod", "Protrusion < Key", -1, 1, 0);
    set(T::ExcXVel, "ExcitatorParameterXVelMod", "Protrusion < Vel", -1, 1, 0);
    set(T::ExcStiffness, "ExcitatorStiffness", "Stiffness", 0, 1, 0.5f, "%");
    set(T::ExcStiffnessKey, "ExcitatorStiffnessKbdMod", "Stiffness < Key", -1, 1, 0);
    set(T::ExcStiffnessVel, "ExcitatorStiffnessVelMod", "Stiffness < Vel", -1, 1, 0);
    set(T::ExcVelocity, "ExcitatorVelocity", "Velocity", 0, 1, 0.5f, "%");
    set(T::ExcVelocityKey, "ExcitatorVelocityKbdMod", "Velocity < Key", -1, 1, 0);
    set(T::ExcVelocityVel, "ExcitatorVelocityVelMod", "Velocity < Vel", -1, 1, 0);
    set(T::ExcDamping, "ExcitatorDamping", "Damping", 0, 1, 0.5f, "%");
    set(T::PickupOn, "PickupToggle", "Pickup", 0, 1, 0, "", true);
    set(T::PickupPosition, "PickupPosition", "Position", 0, 1, 0.5f);
    set(T::DamperOn, "DamperToggle", "Damper", 0, 1, 0, "", true);
    set(T::DamperMass, "DamperMass", "Mass", 0, 1, 0.5f, "%");
    set(T::DamperMassKey, "DamperMassKbdMod", "Mass < Key", -1, 1, 0);
    set(T::DamperStiffness, "DamperStiffness", "Stiffness", 0, 1, 0.5f, "%");
    set(T::DamperStiffnessKey, "DamperStiffnessKbdMod", "Stiffness < Key", -1, 1, 0);
    set(T::DamperVelocity, "DamperVelocity", "Velocity", 0, 1, 0.5f, "%");
    set(T::DamperVelocityKey, "DamperVelocityKbdMod", "Velocity < Key", -1, 1, 0);
    set(T::DamperDamping, "DamperDamping", "Damping", 0, 1, 0.5f, "%");
    set(T::DamperGated, "DamperGated", "Gated", 0, 1, 0, "", true);
    set(T::StringDamping, "StringDamping", "Damping", 0, 1, 0.5f, "%");
    set(T::StringDampingKey, "StringDampingKbdMod", "Damping < Key", -1, 1, 0);
    set(T::StringDecay, "StringDecay", "Decay", 0, 1, 1, "%");
    set(T::StringDecayKey, "StringDecayKbdMod", "Decay < Key", -1, 1, 0);
    set(T::StringRatio, "StringDecayRatio", "Ratio", 0, 1, 0.5f, "%");
    set(T::StringInharm, "StringInharmonicity", "Inharm", 0, 1, 0.5f, "%");
    set(T::TermOn, "TerminationToggle", "Termination", 0, 1, 0, "", true);
    set(T::TermFingerStiffness, "TerminationFingerStiffness", "Finger Stiff", 0, 1, 0.5f, "%");
    set(T::TermFingerMass, "TerminationFingerForce", "Finger Mass", 0, 1, 0.5f, "%");
    set(T::TermFingerMassKey, "TerminationFingerForceKbdMod", "Finger Mass < Key", -1, 1, 0);
    set(T::TermFingerMassVel, "TerminationFingerForceVelMod", "Finger Mass < Vel", -1, 1, 0);
    set(T::TermFretStiffness, "TerminationFretStiffness", "Fret Stiff", 0, 1, 0.5f, "%");
    set(T::LfoOn, "LFOToggle", "F. LFO", 0, 1, 0, "", true);
    set(T::LfoShape, "LFOWaveShape", "Shape", 0, 4, 0, "", false, {"Sine", "Triangle", "Rectangle", "Random", "Random Smooth"});
    set(T::LfoSync, "LFOSync", "Sync Rate", 0, 23, 13, "", false, std::vector<std::string>(kSyncNames, kSyncNames + T::kSyncRates));
    set(T::LfoSyncOn, "LFOSyncToggle", "Sync", 0, 1, 0, "", false, {"Hz", "Sync"});
    set(T::LfoDelay, "LFODelay", "Delay", 0, 1, 0.5f);
    set(T::LfoRate, "LFOSpeed", "Rate", 0, 1, 0.5f);
    set(T::LfoFadeIn, "LFOFadeIn", "Attack", 0, 1, 0.5f);
    set(T::FilterOn, "FilterToggle", "Filter", 0, 1, 1, "", true);
    set(T::FilterType, "FilterType", "Type", 0, 9, 1, "", false, std::vector<std::string>(kFilterNames, kFilterNames + T::kFilterTypes));
    set(T::FilterFreq, "FilterCutoffFrequency", "Freq", 0, 1, 1);
    set(T::FilterFreqKey, "FilterCutoffFrequencyKbdMod", "Freq < Key", -1, 1, 0);
    set(T::FilterFreqLfo, "FilterCutoffFrequencyLFOMod", "Freq < LFO", -1, 1, 0);
    set(T::FilterFreqEnv, "FilterCutoffFrequencyEnvMod", "Freq < Env", -1, 1, 0);
    set(T::FilterRes, "FilterQFactor", "Res", 0, 1, 0, "%");
    set(T::FilterResKey, "FilterQFactorKbdMod", "Res < Key", -1, 1, 0);
    set(T::FilterResLfo, "FilterQFactorLFOMod", "Res < LFO", -1, 1, 0);
    set(T::FilterResEnv, "FilterQFactorEnvMod", "Res < Env", -1, 1, 0);
    set(T::FenvOn, "FilterEnvelopeToggle", "Filter Envelope", 0, 1, 0, "", true);
    set(T::FenvAttack, "FilterEnvelopeAttack", "A", 0, 1, 0);
    set(T::FenvDecay, "FilterEnvelopeDecay", "D", 0, 1, 0.5f);
    set(T::FenvSustain, "FilterEnvelopeSustain", "S", 0, 1, 0.5f, "%");
    set(T::FenvRelease, "FilterEnvelopeRelease", "R", 0, 1, 0.5f);
    set(T::FenvAttackVel, "FilterEnvelopeAttackMod", "A < Vel", 0, 1, 0);
    set(T::FenvAmountVel, "FilterEnvelopeAmpMod", "Env < Vel", 0, 1, 0);
    set(T::BodyOn, "BodyToggle", "Body", 0, 1, 0, "", true);
    set(T::BodyType, "BodyType", "Type", 0, 3, 0, "", false, {"Piano", "Guitar", "Violin", "Generic"});
    set(T::BodySize, "BodySize", "Size", 0, 4, 2, "", false, {"XS", "S", "M", "L", "XL"});
    set(T::BodyDecay, "BodyDecay", "Decay", 0, 1, 0, "%");
    set(T::BodyLowCut, "BodyLowCut", "Low Cut", 0, 1, 0, "%");
    set(T::BodyHighCut, "BodyHighCut", "High Cut", 0, 1, 0, "%");
    set(T::Volume, "BodyLevel", "Volume", 0, 1, 0.705f);
    set(T::BodyMix, "BodyMix", "Str/Body", 0, 1, 0.5f);
    set(T::NotePbRange, "PerNotePitchBendRange", "Note PB Range", 0, 48, 48, "st");
    std::vector<std::string> targets(kTargetNames, kTargetNames + T::kTargets);
    set(T::PressureTarget1, "ChannelPressureTarget1", "Pressure 1", 0, 12, 5, "", false, targets);
    set(T::PressureAmount1, "ChannelPressureRange1", "Pressure 1 Amount", -1, 1, 0.25f, "%");
    set(T::PressureTarget2, "ChannelPressureTarget2", "Pressure 2", 0, 12, 0, "", false, targets);
    set(T::PressureAmount2, "ChannelPressureRange2", "Pressure 2 Amount", -1, 1, 0.5f, "%");
    set(T::SlideTarget1, "SlideTarget1", "Slide 1", 0, 12, 0, "", false, targets);
    set(T::SlideAmount1, "SlideRange1", "Slide 1 Amount", -1, 1, 0.5f, "%");
    set(T::SlideTarget2, "SlideTarget2", "Slide 2", 0, 12, 0, "", false, targets);
    set(T::SlideAmount2, "SlideRange2", "Slide 2 Amount", -1, 1, 0.5f, "%");
    p[T::CurrentTab] = makePlainParam(makeParam("CurrentTab", "Tab", 0, 1, 0, "", false, {"String", "Filter/Global"}));
    return p;
}

float uniform(uint32_t& state) // 0..1
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (float) (state >> 8) / 16777216.0f;
}

// One first-order allpass section y = c x + x1 - c y1 (phase delay > 1 sample at DC for c < 0).
inline float allpass(float x, float c, float& state)
{
    const float y = c * x + state;
    state = x - c * y;
    return y;
}

// Phase delay (samples) of a first-order allpass with coefficient c at angular frequency w.
double allpassDelay(double c, double w)
{
    w = std::max(w, 1e-6);
    const double phase = -w + 2.0 * std::atan2(c * std::sin(w), 1.0 + c * std::cos(w)); // of (c + z^-1) / (1 + c z^-1)
    return -phase / w;
}

// Phase delay of the one-pole loss filter g (1 - p) / (1 - p z^-1) at w.
double onePoleDelay(double p, double w)
{
    w = std::max(w, 1e-6);
    return std::atan2(p * std::sin(w), 1.0 - p * std::cos(w)) / w;
}

// Key tracking: the reference's Key / Vel boxes (keyAmount) move their parameter by keyAmount / 32 per octave from C3
// (fitted on t-dampkbd: Key 1 = 16 moves Damping 0.5 per octave), and by velocity around 64 likewise.
float keyOffset(float amount, int note) { return T::keyAmount(amount) / 32.0f * (float) (note - 60) / 12.0f; }
float velOffset(float amount, float velocity) { return T::keyAmount(amount) / 32.0f * (velocity * 127.0f - 64.0f) / 32.0f; }

// Body modes per type (Hz at size M, relative gain, base Q): Sonora's own resonance sets (the reference's bodies are
// not measured mode by mode, docs/TENSION.md); sizes scale the frequencies.
struct Mode { float hz, gain, q; };
constexpr int kBodyModes = 12;
constexpr Mode kBodies[4][kBodyModes] = {
    // Piano: dense soundboard modes, broad
    {{110, 0.7f, 4}, {190, 0.8f, 5}, {320, 1.0f, 6}, {460, 0.8f, 6}, {700, 0.7f, 7}, {980, 0.6f, 7},
     {1400, 0.5f, 8}, {1900, 0.45f, 8}, {2600, 0.4f, 9}, {3500, 0.35f, 9}, {4700, 0.3f, 10}, {6200, 0.25f, 10}},
    // Guitar: air (Helmholtz) and top-plate modes
    {{98, 1.0f, 8}, {196, 1.0f, 10}, {245, 0.7f, 10}, {390, 0.6f, 12}, {550, 0.5f, 12}, {800, 0.45f, 14},
     {1150, 0.4f, 14}, {1600, 0.4f, 16}, {2200, 0.35f, 16}, {3000, 0.3f, 18}, {4100, 0.25f, 18}, {5500, 0.2f, 20}},
    // Violin: A0 / B1- / B1+ and the bridge hill
    {{275, 0.8f, 10}, {450, 0.9f, 12}, {550, 0.9f, 12}, {800, 0.6f, 14}, {1100, 0.5f, 14}, {1600, 0.5f, 16},
     {2300, 0.7f, 10}, {2800, 0.8f, 9}, {3400, 0.6f, 10}, {4200, 0.4f, 12}, {5200, 0.3f, 14}, {6500, 0.2f, 16}},
    // Generic: evenly spread, smooth
    {{150, 0.5f, 3}, {250, 0.5f, 3}, {400, 0.5f, 3}, {630, 0.5f, 3}, {1000, 0.5f, 3}, {1600, 0.5f, 3},
     {2500, 0.5f, 3}, {4000, 0.5f, 3}, {6300, 0.5f, 3}, {10000, 0.4f, 3}, {13000, 0.3f, 3}, {16000, 0.2f, 3}},
};
constexpr float kBodySizes[5] = {2.0f, 1.41f, 1.0f, 0.71f, 0.5f};

// Vowel formants (F1, F2, F3 Hz) along the formant filter's cutoff: I, E, A, O, U (the face's axis, bottom to top).
constexpr float kVowels[5][3] = {{270, 2290, 3010}, {530, 1840, 2480}, {730, 1090, 2440}, {570, 840, 2410}, {300, 870, 2240}};
} // namespace

// ------------------------------------------------------------------------------------------------ public laws

float Tension::filterHz(float v) { return 30.0f * std::pow(22000.0f / 30.0f, std::clamp(v, 0.0f, 1.0f)); }

float Tension::envelopeMs(float v)
{
    static const std::array<std::pair<float, float>, 11> points{{{0.0f, 5.0f}, {0.0909f, 10.0f}, {0.1818f, 22.0f},
        {0.2727f, 45.0f}, {0.3636f, 94.0f}, {0.4545f, 196.0f}, {0.5f, 283.0f}, {0.5455f, 406.0f}, {0.6364f, 835.0f},
        {0.7273f, 1720.0f}, {1.0f, 15000.0f}}};
    return logTable(points, v);
}

float Tension::vibratoMs(float v) { return v <= 0.0f ? 0.0f : 51.8f * std::pow(96.5f, std::min(v, 1.0f)); } // 509 ms, 5.00 s
float Tension::vibratoHz(float v) { return 0.2f + 9.8f * std::clamp(v, 0.0f, 1.0f); }
float Tension::lfoHz(float v) { v = std::clamp(v, 0.0f, 1.0f); return 0.1f + 6.4f * v * v * v; }
float Tension::lfoMs(float v) { return v <= 0.0f ? 0.0f : 200.0f * std::pow(25.0f, std::min(v, 1.0f)); }
float Tension::unisonMs(float v) { return v <= 0.0f ? 0.0f : 1.96f * std::pow(51.0f, std::min(v, 1.0f)); }

float Tension::keyAmount(float v)
{
    if (std::abs(v) < 1e-7f) return 0.0f;
    const float a = std::min(std::abs(v), 1.0f);
    return std::copysign(0.101f * std::pow(158.4f, a), v); // 16.00 at 1
}

float Tension::cutoffEnvAmount(float v) { const float a = std::min(std::abs(v), 1.0f); return std::copysign(16.0f * a * a * a * a, v); }
float Tension::modAmount(float v) { return 4.0f * v * std::abs(v); }
float Tension::volumeDb(float v) { const float g = volumeGain(v); return g <= 0 ? -200.0f : 20.0f * std::log10(g); }

double Tension::syncBeats(int index)
{
    index = std::clamp(index, 0, kSyncRates - 1);
    const double division = 16.0 / std::pow(2.0, index / 3); // 4 bars = 16 beats ... 1/32 = 0.125 beat
    const int variant = index % 3;
    return variant == 0 ? division * 1.5 : variant == 2 ? division * 2.0 / 3.0 : division;
}
const char* Tension::syncName(int index) { return kSyncNames[std::clamp(index, 0, kSyncRates - 1)]; }
const char* Tension::filterTypeName(int type) { return kFilterNames[std::clamp(type, 0, kFilterTypes - 1)]; }
const char* Tension::targetName(int target) { return kTargetNames[std::clamp(target, 0, (int) kTargets - 1)]; }
int Tension::polyphonyVoices(int index) { return kPolyphony[std::clamp(index, 0, kPolyphonies - 1)]; }

// ------------------------------------------------------------------------------------------------ setup

Tension::Tension() : NativeDevice(tensionParameters()) {}

std::vector<int> Tension::displayParameters() const
{
    return {ExcType, ExcX, ExcStiffness, ExcVelocity, ExcPosition, StringDecay, StringDamping, StringInharm, FilterFreq,
            FilterRes, BodyMix, Volume};
}

std::vector<DeviceProcessor::UiGroup> Tension::uiGroups() const
{
    return {{"Exciter", {ExcOn, ExcType, ExcX, ExcStiffness, ExcVelocity, ExcPosition, ExcDamping}},
            {"String", {StringDecay, StringRatio, StringInharm, StringDamping}},
            {"Damper", {DamperOn, DamperGated, DamperMass, DamperStiffness, DamperVelocity, DamperPosition, DamperDamping}},
            {"Body", {BodyOn, BodyType, BodySize, BodyDecay, BodyMix, BodyLowCut, BodyHighCut, Volume}},
            {"Filter", {FilterOn, FilterType, FilterFreq, FilterRes, FenvOn, FenvAttack, FenvDecay, FenvSustain, FenvRelease}},
            {"Global", {Polyphony, Octave, Transpose, VibratoOn, VibratoAmount, UnisonOn, PortamentoOn, PortamentoTime}}};
}

void Tension::prepare(double sampleRate, int)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    // Lowest string: 15 Hz -> half a period per direction plus room for the interpolator.
    const int needed = (int) std::ceil(sampleRate_ / 15.0 * 0.5) + 8;
    lineLength_ = 64;
    while (lineLength_ < needed) lineLength_ <<= 1;
    lineMask_ = lineLength_ - 1;
    lineR_.assign((size_t) kMaxStrings * (size_t) lineLength_, 0.0f);
    lineL_.assign((size_t) kMaxStrings * (size_t) lineLength_, 0.0f);
    reset();
}

void Tension::reset()
{
    for (auto& s : strings_) s = Str{};
    std::fill(lineR_.begin(), lineR_.end(), 0.0f);
    std::fill(lineL_.begin(), lineL_.end(), 0.0f);
    bend_ = {};
    slide_ = {};
    pressure_ = {};
    modWheel_ = channelPressure_ = 0;
    sustainPedal_ = false;
    lastNote_ = -1;
    held_ = {};
    heldAt_ = {};
    counter_ = 0;
    random_ = 0x6C8E9CF5u;
    for (auto& state : modeState_) state = {};
    lowState_ = highState_ = {};
    for (auto& key : bodyKey_) key = -1;
    activeVoices_.store(0);
}

int Tension::voiceLimit() const { return polyphonyVoices((int) std::lround(k(Polyphony))); }

// ------------------------------------------------------------------------------------------------ MIDI

void Tension::handleEvent(const uint8_t* data, int size)
{
    if (size < 1) return;
    const int status = data[0] & 0xF0, channel = data[0] & 0x0F;
    if (status == 0x90 && size >= 3 && data[2] > 0) noteOn(data[1], channel, data[2] / 127.0f);
    else if ((status == 0x80 || (status == 0x90 && size >= 3)) && size >= 3) noteOff(data[1], channel);
    else if (status == 0xE0 && size >= 3) bend_[(size_t) channel] = (float) (((data[2] << 7) | data[1]) - 8192) / 8192.0f;
    else if (status == 0xD0 && size >= 2) { pressure_[(size_t) channel] = data[1] / 127.0f; channelPressure_ = pressure_[(size_t) channel]; }
    else if (status == 0xB0 && size >= 3)
    {
        if (data[1] == 1) modWheel_ = data[2] / 127.0f;
        else if (data[1] == 74) slide_[(size_t) channel] = data[2] / 127.0f;
        else if (data[1] == 64)
        {
            sustainPedal_ = data[2] >= 64;
            if (! sustainPedal_)
                for (auto& s : strings_)
                    if (s.active && s.sustained) { s.sustained = false; releaseString(s); }
        }
        else if (data[1] == 120 || data[1] == 123)
            for (auto& s : strings_)
                if (s.active && (data[1] == 120 || s.gate)) { s.sustained = false; releaseString(s); if (data[1] == 120) s.silence = 1; }
    }
}

void Tension::noteOn(int note, int channel, float velocity)
{
    held_[(size_t) note] = 1;
    heldAt_[(size_t) note] = ++heldOrder_;
    const int voices = voiceLimit();
    const int members = k(UnisonOn) >= 0.5f ? (k(UnisonVoices) >= 0.5f ? 4 : 2) : 1;
    if (voices == 1)
    {
        // Mono: the priority decides which held key sounds; legato keeps the strings ringing and glides.
        const int priority = (int) std::lround(k(Priority));
        int chosen = note;
        if (priority != 2)
            for (int n = 0; n < 128; ++n)
                if (held_[(size_t) n] && (priority == 0 ? n > chosen : n < chosen)) chosen = n;
        if (chosen != note) return; // a higher / lower held key keeps sounding
        bool legato = false;
        for (auto& s : strings_)
            if (s.active && s.gate && ! s.released) legato = true;
        const bool glideLegatoOnly = k(PortamentoLegato) >= 0.5f;
        int member = 0;
        if (legato && k(PortamentoOn) >= 0.5f)
        {
            for (auto& s : strings_)
                if (s.active && s.gate && ! s.released)
                {
                    s.note = note;
                    startString(s, note, channel, velocity, s.member, s.members, true);
                }
            lastNote_ = note;
            return;
        }
        (void) glideLegatoOnly;
        for (auto& s : strings_)
            if (s.active && ! s.released) releaseString(s);
        for (auto& s : strings_)
            if (member < members && ! s.active)
                startString(s, note, channel, velocity, member++, members, false);
        lastNote_ = note;
        return;
    }
    // Poly: steal the oldest note when the voice count is reached.
    int sounding = 0;
    for (const auto& s : strings_)
        if (s.active && s.member == 0 && ! s.released) ++sounding;
    if (sounding >= voices)
    {
        uint64_t oldest = UINT64_MAX;
        int victim = -1;
        for (const auto& s : strings_)
            if (s.active && s.member == 0 && ! s.released && s.age < oldest) { oldest = s.age; victim = s.note; }
        for (auto& s : strings_)
            if (s.active && s.note == victim && ! s.released) releaseString(s);
    }
    int member = 0;
    for (int pass = 0; pass < 2 && member < members; ++pass)
        for (auto& s : strings_)
        {
            if (member >= members) break;
            const bool free = ! s.active || (pass == 1 && s.released);
            if (! free) continue;
            startString(s, note, channel, velocity, member++, members, false);
        }
    lastNote_ = note;
}

void Tension::noteOff(int note, int channel)
{
    held_[(size_t) note] = 0;
    if (voiceLimit() == 1)
    {
        // Mono: return to another held key by priority (legato, without a new excitation when gliding).
        int next = -1;
        const int priority = (int) std::lround(k(Priority));
        uint64_t latest = 0;
        for (int n = 0; n < 128; ++n)
            if (held_[(size_t) n])
            {
                if (priority == 0 && n > next) next = n;
                else if (priority == 1 && (next < 0 || n < next)) next = n;
                else if (priority == 2 && heldAt_[(size_t) n] > latest) { latest = heldAt_[(size_t) n]; next = n; }
            }
        bool sounding = false;
        for (auto& s : strings_) sounding |= s.active && s.note == note && ! s.released;
        if (next >= 0 && sounding)
        {
            for (auto& s : strings_)
                if (s.active && s.note == note && ! s.released)
                    startString(s, next, channel, s.velocity, s.member, s.members, k(PortamentoOn) >= 0.5f);
            return;
        }
    }
    for (auto& s : strings_)
        if (s.active && s.note == note && s.gate && ! s.released)
        {
            if (sustainPedal_) s.sustained = true;
            else releaseString(s);
        }
}

void Tension::releaseString(Str& s)
{
    s.gate = false;
    s.released = true;
    s.fenv.stage = s.fenv.stage == 0 ? 0 : 4;
    s.fenv.t = 0;
    // A held bow leaves the string; the gated damper falls now.
    if (s.exciter == Bow) s.exc.active = false;
    if (k(DamperOn) >= 0.5f && k(DamperGated) >= 0.5f) { s.damperDown = true; s.damper.active = true; s.damper.y = 0; }
    s.countdown = 0;
}

// ------------------------------------------------------------------------------------------------ string set-up

void Tension::tuneString(Str& s, double frequency)
{
    // Loop delay = R side (fractional, Lagrange-read) + L side (integer) + dispersion + loss + nut filters, tuned so the
    // fundamental lands on `frequency`.
    frequency = std::clamp(frequency, 15.0, sampleRate_ * 0.24);
    const double period = sampleRate_ / frequency, w = kTwoPi / period;
    // The finger at the nut is not compensated: it lowers the pitch like the reference (-12 cents at the defaults).
    double filters = onePoleDelay(s.lossPole, w);
    for (int i = 0; i < s.apStages; ++i) filters += allpassDelay(s.apCoeff, w);
    // The read after the write adds one sample on each side.
    const double length = std::max(6.0, period - filters - 2.0);
    s.mInt = std::clamp((int) std::floor(length * 0.5), 3, lineLength_ - 8);
    s.mFrac = std::clamp(length - s.mInt, 3.0, (double) lineLength_ - 8.0);
}

void Tension::startString(Str& s, int note, int channel, float velocity, int member, int members, bool legato)
{
    const int index = (int) (&s - strings_.data());
    float* R = lineR_.data() + (size_t) index * (size_t) lineLength_;
    float* L = lineL_.data() + (size_t) index * (size_t) lineLength_;
    const bool retrigger = ! legato;
    const double base = note + 12.0 * std::round(k(Octave)) + std::round(k(Transpose));
    if (retrigger)
    {
        s = Str{};
        std::fill(R, R + lineLength_, 0.0f);
        std::fill(L, L + lineLength_, 0.0f);
        s.semis = s.targetSemis = base;
        s.errorCents = (uniform(random_) * 2.0f - 1.0f) * 50.0f * k(KeyError);
        s.vibratoErrorRate = 1.0f + (uniform(random_) * 2.0f - 1.0f) * 0.5f * k(VibratoError);
        s.vibratoErrorAmount = 1.0f + (uniform(random_) * 2.0f - 1.0f) * 0.5f * k(VibratoError);
        s.vibPhase = uniform(random_);
    }
    s.active = true;
    s.gate = true;
    s.released = s.sustained = false;
    s.note = note;
    s.channel = channel;
    s.velocity = velocity;
    s.member = member;
    s.members = members;
    s.age = ++counter_;
    s.targetSemis = base;
    if (legato && k(PortamentoOn) >= 0.5f)
    {
        // Portamento Time 0..100 %: 0 .. 1 s (assumed, not measured); Proportional scales by the interval / octave.
        const double seconds = 1.0 * k(PortamentoTime);
        const double interval = std::abs(base - s.semis);
        const double time = k(PortamentoProportional) >= 0.5f ? seconds * interval / 12.0 : seconds;
        s.glideStep = time > 1e-4 ? (base - s.semis) / (time * sampleRate_) : 0.0;
        if (s.glideStep == 0.0) s.semis = base;
    }
    else if (! legato && k(PortamentoOn) >= 0.5f && k(PortamentoLegato) < 0.5f && lastNote_ >= 0)
    {
        const double from = lastNote_ + 12.0 * std::round(k(Octave)) + std::round(k(Transpose));
        const double seconds = 1.0 * k(PortamentoTime);
        const double time = k(PortamentoProportional) >= 0.5f ? seconds * std::abs(base - from) / 12.0 : seconds;
        s.semis = from;
        s.glideStep = time > 1e-4 ? (base - from) / (time * sampleRate_) : 0.0;
        if (s.glideStep == 0.0) s.semis = base;
    }
    else s.semis = base, s.glideStep = 0;

    // Unison: members spread evenly over +-Detune (0..100 % = +-80 cents) and start Delay apart.
    if (members > 1)
    {
        const float spread = members == 2 ? (member == 0 ? -1.0f : 1.0f) : -1.0f + 2.0f * (float) member / (float) (members - 1);
        s.detuneCents = spread * 80.0f * k(UnisonDetune); // t-unison: 50 % = +-40 cents
        s.startDelay = retrigger ? (int) std::lround(unisonMs(k(UnisonDelay)) * 0.001 * sampleRate_ * member) : 0;
    }
    else s.detuneCents = 0, s.startDelay = 0;
    if (! retrigger) return;

    // ---- string losses (fitted on the reference's stems, docs/TENSION.md "String"):
    // decay rate (dB/s) of partial k = a + b k^2: a depends on Decay only (ln|a| = 7.25 - 6.8 Decay: 1.6 dB/s at 100 %,
    // 48 dB/s at 50 %), b on Damping and the pitch (1.9 dB/s at C3 and 50 %, ~0 at 100 %, a few periods at 0 %).
    const int key = note;
    const float decay = std::clamp(k(StringDecay) + keyOffset(k(StringDecayKey), key), 0.0f, 1.0f);
    const float damping = std::clamp(k(StringDamping) + keyOffset(k(StringDampingKey), key), -0.5f, 1.0f);
    const double f0 = 440.0 * std::pow(2.0, (s.semis - 69.0) / 12.0);
    const double w0 = kTwoPi * f0 / sampleRate_;
    const double a = std::exp(7.25 - 6.8 * decay); // dB/s at DC
    // One-pole pole for the high-frequency loss: b(C3, 50 %) = 1.87 dB/s per k^2 <=> p = 0.41 at 44.1 kHz.
    const double p = std::clamp(damping >= 1.0f ? 0.0 : 0.975 * std::pow(1.0 - (double) std::max(damping, 0.0f), 1.6)
                                                         + (damping < 0 ? 0.024 * (-damping) * 2.0 : 0.0),
                                0.0, 0.999);
    s.lossPole = p;
    s.decay = decay;
    s.lossGain = std::pow(10.0, -a / 20.0 / f0); // per period at DC
    // Termination: the finger at the nut is a lossy, slightly compliant end (lower pitch, double high-frequency loss).
    if (k(TermOn) >= 0.5f)
    {
        const float mass = std::clamp(k(TermFingerMass) + keyOffset(k(TermFingerMassKey), key) + velOffset(k(TermFingerMassVel), velocity), 0.0f, 1.0f);
        const float stiff = 0.5f * (k(TermFingerStiffness) + k(TermFretStiffness));
        s.nutPole = std::clamp(0.05 + 0.2 * (1.0 - stiff) * (0.5 + mass), 0.0, 0.8);
        s.nutGain = 0.99995 - 0.0002 * (1.0 - stiff);
    }
    else s.nutPole = 0, s.nutGain = 1.0;
    // Inharmonicity: one first-order allpass in the loop (fitted: one section reproduces the reference's partial
    // stretch within 0.2 % up to k = 10 at 50 % and 100 %, more sections stretch the high partials too much). Partial 4
    // is sharpened by 0.036 x Inharm^2 at C3 (0.88 % at 50 %, 3.6 % at 100 %), x 0.83 / 0.89 / 1 / 1.25 / 1.89 at
    // C1 / C2 / C3 / C4 / C5 (t-keys); the coefficient follows the note so the stretch is per harmonic.
    const double inharm = std::clamp((double) k(StringInharm) + targetMod(s, TInharm), 0.0, 1.0);
    s.apStages = inharm > 1e-3 ? 1 : 0;
    if (s.apStages > 0)
    {
        static constexpr double notes[5] = {36, 48, 60, 72, 84}, factors[5] = {0.825, 0.89, 1.0, 1.25, 1.89};
        const double n = std::clamp(s.semis, 36.0, 84.0);
        const int i = std::min(3, (int) ((n - 36.0) / 12.0));
        const double factor = factors[i] * std::pow(factors[i + 1] / factors[i], (n - notes[i]) / 12.0);
        const double target = 0.036 * inharm * inharm * factor; // f4 / (4 f1) - 1
        double lo = -0.9995, hi = 0.0;
        const double period = sampleRate_ / f0;
        for (int it = 0; it < 40; ++it)
        {
            const double c = 0.5 * (lo + hi);
            const double stretch = allpassDelay(c, w0) - allpassDelay(c, 4 * w0);
            const double ratio = period / (period - stretch); // f4 / (4 f1), the other delays unchanged
            if (ratio - 1.0 > target) lo = c; else hi = c; // too stretched: towards 0
        }
        s.apCoeff = (float) (0.5 * (lo + hi));
    }
    tuneString(s, f0);

    // ---- geometry: positions are fractions 0..0.5 of the string from the bridge; Fix.Pos keeps the distance of C3.
    auto position = [&](float value, bool absolute, float keyMod, float velMod) {
        float fraction = 0.5f * std::clamp(value + keyOffset(keyMod, key) + velOffset(velMod, velocity), 0.0f, 1.0f);
        const double half = 0.5 * sampleRate_ / f0; // the string's length in samples (half the loop)
        if (absolute) fraction = (float) std::min(0.5, fraction * (sampleRate_ / 261.63 * 0.5) / std::max(1.0, half));
        // The bridge's filters (loss, dispersion, the read delay) extend the string beyond the tap at the bridge end:
        // measure from the effective end so 25 % nulls the 4th harmonic like the reference.
        const double bridge = onePoleDelay(s.lossPole, w0) + (s.apStages > 0 ? allpassDelay(s.apCoeff, w0) : 0.0) + 1.0;
        return std::clamp((int) std::lround(fraction * half - 0.5 * bridge), 1, std::max(1, s.mInt - 2));
    };
    s.excPos = position(k(ExcPosition), k(ExcPositionAbs) >= 0.5f, k(ExcPositionKey), k(ExcPositionVel));
    s.damperPos = position(k(DamperPosition), k(DamperPositionAbs) >= 0.5f, k(DamperPositionKey), k(DamperPositionVel));
    s.pickupPos = position(k(PickupPosition), false, 0, 0);

    // ---- exciter (normalised physical units: string impedance 1, displacement in string units, time in samples)
    s.exciter = (int) std::lround(k(ExcType));
    const float x = std::clamp(k(ExcX) + keyOffset(k(ExcXKey), key) + velOffset(k(ExcXVel), velocity), 0.0f, 1.0f);
    const float stiffness = std::clamp(k(ExcStiffness) + keyOffset(k(ExcStiffnessKey), key) + velOffset(k(ExcStiffnessVel), velocity), 0.0f, 1.0f);
    const float speed = std::clamp(k(ExcVelocity) + keyOffset(k(ExcVelocityKey), key) + velOffset(k(ExcVelocityVel), velocity), 0.0f, 1.0f);
    const float dampingX = k(ExcDamping);
    const double rate = 44100.0 / sampleRate_; // per-sample quantities scale with the sample rate
    s.exc = {};
    s.exc.active = k(ExcOn) >= 0.5f;
    if (s.exciter == Bow)
    {
        // Bowed string (friction curve of McIntyre / Schumacher / Woodhouse in the normalised form of STK's BowTable):
        // the velocity difference dv / kBowSpeed through min(1, (|dv slope| + 0.75)^-e). Force sets the slope (0 = no
        // contact: silent, like the reference), Friction the exponent, Velocity the bow speed.
        s.bowForce = std::min(1.0, 4.0 * x);
        s.bowSlope = 1.0 + 9.0 * std::pow(1.0 - x, 1.5);
        s.bowOffset = 3.0 + 2.0 * stiffness; // exponent
        s.bowVelocity = kBowSpeed * std::exp(3.0 * (speed - 0.5));
    }
    else if (s.exciter == Plectrum)
    {
        // The tip pushes the string for T (a force ramp), then slips off: the force falls with tau. Level laws fitted on
        // the reference's law cases (dB relative to the default pluck, docs/TENSION.md "Plectrum"): Protrusion 0 / 100 %
        // -10.8 / +11.9, Velocity -8.9 / +20.7, Damping -11 / +25.3; Stiffness 0 = -47 dB (a soft tip slips off slowly:
        // tau = 137 ms; 0.26 ms at 50 % gives the reference's h2..h8 roll-off), 100 % +1 dB. Velocity also sets the push time (onset 11 ms at 0 %, 1.2 ms at 50 %).
        auto piece = [](float v, float atZero, float atOne) { return v < 0.5f ? atZero * (0.5f - v) * 2.0f : atOne * (v - 0.5f) * 2.0f; };
        const float db = piece(x, -10.8f, 11.9f) + piece(speed, -7.4f, 23.8f) + piece(dampingX, -11.0f, 25.3f) + piece(stiffness, 0.0f, 1.0f);
        s.plPeak = std::pow(10.0, db / 20.0);
        s.plPush = std::max(1.0, 0.0012 * std::exp(-4.49 * (speed - 0.5)) * sampleRate_);
        const double tau = stiffness < 0.5f ? 0.00026 * std::pow(1.0 / 0.00026, (0.5 - stiffness) * 2.0) : 0.00026 * std::pow(1.5, (stiffness - 0.5) * 2.0);
        s.plTau = std::max(0.3, tau * sampleRate_);
        s.plPush = std::max(s.plPush, s.plTau); // a soft tip also bends slowly
        s.plTime = 0;
    }
    else
    {
        // Hammer: mass, felt stiffness (power 2.3 law), felt damping, strike velocity. The bouncing hammer is pushed back
        // onto the string while the key is held.
        s.excMass = 15.0 * std::exp(2.5 * (x - 0.5));
        s.excK = 0.005 * std::exp(5.0 * (stiffness - 0.5)) * rate * rate;
        s.excR = 0.05 * std::exp(4.0 * (dampingX - 0.5)) * rate;
        s.excVelocity = 0.0015 * std::exp(4.5 * (speed - 0.5)) * rate;
        s.exc.v = s.excVelocity;
        s.exc.y = -s.excVelocity * 2.0;
    }
    // ---- damper: mass / felt stiffness / damping / landing velocity
    const float dMass = std::clamp(k(DamperMass) + keyOffset(k(DamperMassKey), key), 0.0f, 1.0f);
    const float dStiff = std::clamp(k(DamperStiffness) + keyOffset(k(DamperStiffnessKey), key), 0.0f, 1.0f);
    const float dVel = std::clamp(k(DamperVelocity) + keyOffset(k(DamperVelocityKey), key), 0.0f, 1.0f);
    s.damperMass = 4.0 * std::exp(4.0 * (dMass - 0.5));
    // Felt: stiffness and damping to rest, scaled by the mass; Velocity sets how fast it lands (1 .. 30 ms). The
    // per-sample dashpot absorbs most at R = 1 (matched to the string); closer to 2 it reflects and, with the spring,
    // can gain energy, so R stays <= 1.
    const double massScale = 0.5 + dMass;
    s.damperK = std::min(0.02, 0.002 * std::exp(5.0 * (dStiff - 0.5)) * massScale) * rate * rate;
    s.damperR = std::min(1.0, 0.15 * std::exp(3.0 * (k(DamperDamping) - 0.5)) * massScale * (0.5 + dStiff));
    s.damperV = 1.0 / (sampleRate_ * 0.03 * std::pow(30.0, -dVel) + 1.0);
    s.damper = {};
    s.damperDown = k(DamperOn) >= 0.5f && k(DamperGated) < 0.5f;
    s.damper.active = s.damperDown;
    s.damper.y = 0;

    // ---- filter envelope and modulators
    s.fenv = {};
    s.fenv.stage = k(FenvOn) >= 0.5f ? 1 : 0;
    s.fenvAttackScale = 1.0f / (1.0f + envelopeVelAmount(k(FenvAttackVel)) * velocity * 0.25f);
    s.fenvPeak = 1.0f - k(FenvAmountVel) + k(FenvAmountVel) * velocity;
    s.lfoPhase = 0;
    s.lfoAge = s.age_s = 0;
    s.lfoHeld = uniform(random_) * 2 - 1;
    s.lfoTarget = uniform(random_) * 2 - 1;
    s.countdown = 0;
    s.level = 1.0f;
    s.silence = 0;
}

// ------------------------------------------------------------------------------------------------ modulation

float Tension::targetMod(const Str& s, int target) const
{
    float sum = 0;
    const int ch = std::clamp(s.channel, 0, 15);
    const float pressure = std::max(pressure_[(size_t) ch], channelPressure_), slide = slide_[(size_t) ch];
    if ((int) std::lround(k(PressureTarget1)) == target) sum += pressure * k(PressureAmount1);
    if ((int) std::lround(k(PressureTarget2)) == target) sum += pressure * k(PressureAmount2);
    if ((int) std::lround(k(SlideTarget1)) == target) sum += slide * k(SlideAmount1);
    if ((int) std::lround(k(SlideTarget2)) == target) sum += slide * k(SlideAmount2);
    return sum;
}

float Tension::stepEnv(Str& s, double samples)
{
    auto& e = s.fenv;
    if (e.stage == 0) return 0;
    const double ms = 1000.0 / sampleRate_ * samples;
    if (e.stage == 1)
    {
        const double length = envelopeMs(k(FenvAttack)) * s.fenvAttackScale;
        e.t += ms;
        e.level = (float) std::min(1.0, e.t / std::max(0.1, length));
        if (e.t >= length) { e.stage = 2; e.t = 0; }
    }
    else if (e.stage == 2)
    {
        const double length = envelopeMs(k(FenvDecay));
        e.t += ms;
        const double x = std::min(1.0, e.t / length);
        const float sustain = k(FenvSustain);
        e.level = (float) (sustain + (1.0 - sustain) * std::exp(-5.0 * x) * (1.0 - x)); // exponential-like fall
        if (x >= 1.0) { e.stage = 3; e.level = sustain; }
    }
    else if (e.stage == 3) e.level = k(FenvSustain);
    else if (e.stage == 4)
    {
        const double length = envelopeMs(k(FenvRelease));
        e.level *= (float) std::exp(-5.0 * ms / length);
        if (e.level < 1e-4f) { e.level = 0; e.stage = 5; }
    }
    return e.level * s.fenvPeak;
}

float Tension::stepLfo(Str& s, double samples)
{
    if (k(LfoOn) < 0.5f) return 0;
    const double seconds = samples / sampleRate_;
    s.lfoAge += seconds;
    const double delay = lfoMs(k(LfoDelay)) * 0.001;
    if (s.lfoAge < delay) return 0;
    double hz = k(LfoSyncOn) >= 0.5f ? tempo_ / 60.0 / syncBeats((int) std::lround(k(LfoSync))) : lfoHz(k(LfoRate));
    hz *= std::exp2(2.0 * targetMod(s, TLfoRate));
    const double before = s.lfoPhase;
    s.lfoPhase += hz * seconds;
    if (s.lfoPhase >= 1.0)
    {
        s.lfoPhase -= std::floor(s.lfoPhase);
        s.lfoHeld = s.lfoTarget;
        s.lfoTarget = uniform(random_) * 2 - 1;
    }
    (void) before;
    const double ph = s.lfoPhase;
    float value = 0;
    switch ((int) std::lround(k(LfoShape)))
    {
        case 0: value = (float) std::sin(kTwoPi * ph); break;
        case 1: value = (float) (ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4); break;
        case 2: value = ph < 0.5 ? 1.0f : -1.0f; break;
        case 3: value = s.lfoHeld; break;
        default: value = s.lfoHeld + (s.lfoTarget - s.lfoHeld) * (float) ph; break;
    }
    const double fade = lfoMs(k(LfoFadeIn)) * 0.001;
    const double since = s.lfoAge - delay;
    return value * (fade > 0 ? (float) std::min(1.0, since / fade) : 1.0f);
}

// Control rate (every kControl samples): pitch, filter coefficients.
void Tension::control(Str& s)
{
    const double block = kControl;
    s.age_s += block / sampleRate_;
    if (s.glideStep != 0.0)
    {
        s.semis += s.glideStep * block;
        if ((s.glideStep > 0 && s.semis >= s.targetSemis) || (s.glideStep < 0 && s.semis <= s.targetSemis))
        { s.semis = s.targetSemis; s.glideStep = 0; }
    }
    const int ch = std::clamp(s.channel, 0, 15);
    double semis = s.semis + (k(FineTune) - 0.5) + (s.errorCents + s.detuneCents * (1.0f + targetMod(s, TUnisonDetune))) / 100.0;
    semis += bend_[(size_t) ch] * std::round(k(PitchBendRange));
    // Stretch: +-100 % = +-? cents per octave from C3 (assumed 10 cents / octave at 100 %).
    semis += (k(Stretch) * 2.0 - 1.0) * 0.1 * (s.semis - 60.0) / 12.0;
    // Vibrato: Delay, then Attack fades it in; Amount 100 % = +-1 semitone (assumed), plus the mod wheel.
    if (k(VibratoOn) >= 0.5f)
    {
        const double delay = vibratoMs(k(VibratoDelay)) * 0.001, attack = vibratoMs(k(VibratoFadeIn)) * 0.001;
        if (s.age_s > delay)
        {
            const double fade = attack > 0 ? std::min(1.0, (s.age_s - delay) / attack) : 1.0;
            const double rate = vibratoHz(k(VibratoRate)) * s.vibratoErrorRate * std::exp2(2.0 * targetMod(s, TVibratoRate));
            s.vibPhase += rate * block / sampleRate_;
            s.vibPhase -= std::floor(s.vibPhase);
            const double amount = (k(VibratoAmount) + modWheel_ * k(VibratoModWheel) + targetMod(s, TVibratoAmount)) * s.vibratoErrorAmount;
            semis += std::sin(kTwoPi * s.vibPhase) * amount * fade;
        }
    }
    const double f0 = 440.0 * std::pow(2.0, (semis - 69.0) / 12.0);
    // Losses per period at DC: the held decay, or after the key (and pedal) the release decay = Decay x (1 - Ratio)
    // (fitted: Ratio 0 keeps the held decay, 50 % at Decay 100 % = 1 s, 100 % stops within ~30 ms). With a gated damper
    // the damper ends the note instead.
    const bool gatedDamper = k(DamperOn) >= 0.5f && k(DamperGated) >= 0.5f;
    const float decay = s.released && ! gatedDamper ? s.decay * (1.0f - k(StringRatio)) : s.decay;
    s.lossGain = std::pow(10.0, -std::exp(7.25 - 6.8 * decay) / 20.0 / f0);
    tuneString(s, f0);

    // Filter: cutoff law of the face, modulation boxes in octaves (Env 16 = 16 octaves?, others 4 = +-4 octaves; assumed).
    const float env = stepEnv(s, block), lfo = stepLfo(s, block);
    if (k(FilterOn) >= 0.5f)
    {
        const float note = (float) (s.semis - 60.0) / 12.0f;
        double octaves = std::log2(filterHz(k(FilterFreq)))
                       + cutoffEnvAmount(k(FilterFreqEnv)) * (1.0f + targetMod(s, TCutoffEnv)) * env * 0.5
                       + modAmount(k(FilterFreqLfo)) * (1.0f + targetMod(s, TCutoffLfo)) * lfo * 0.5
                       + modAmount(k(FilterFreqKey)) * note * 0.25 + 8.0 * targetMod(s, TCutoff);
        const double hz = std::clamp(std::exp2(octaves), 20.0, std::min(20000.0, sampleRate_ * 0.45));
        const float res = std::clamp(k(FilterRes) + modAmount(k(FilterResEnv)) * (1.0f + targetMod(s, TResEnv)) * env * 0.25f
                                         + modAmount(k(FilterResLfo)) * (1.0f + targetMod(s, TResLfo)) * lfo * 0.25f
                                         + modAmount(k(FilterResKey)) * note * 0.125f + targetMod(s, TRes),
                                     0.0f, 1.0f);
        // Peak gain G(res) (fitted, t-q05 / t-q1 / t-cut05): 0.09 at 0 %, 3.5 at 50 %, 120 at 100 %. One-stage types
        // (LP12, HP12, BP6, N2P, F6) use Q = G; the 24 dB types split it over two stages (Q = sqrt G each); BP12 = two BP6.
        const double gain = res < 0.5f ? 0.09 * std::pow(3.5 / 0.09, res / 0.5) : 3.5 * std::pow(120.0 / 3.5, (res - 0.5) / 0.5);
        const int type = std::clamp((int) std::lround(k(FilterType)), 0, kFilterTypes - 1);
        using BT = BiquadCoeffs::Type;
        auto make = [&](BT t, double freq, double q) { return SvfCoeffs::fromBiquad(BiquadCoeffs::make(t, sampleRate_, freq, q, 0.0)); };
        switch (type)
        {
            case 0: s.filter[0] = make(BT::LowPass, hz, gain); s.filterStages = 1; break;
            case 1: s.filter[0] = s.filter[1] = make(BT::LowPass, hz, std::sqrt(gain)); s.filterStages = 2; break;
            case 2: s.filter[0] = make(BT::BandPass, hz, std::max(0.3, gain)); s.filter[0].mBand *= std::max(0.3, gain); s.filterStages = 1; break;
            case 3:
                s.filter[0] = make(BT::BandPass, hz, std::max(0.3, gain));
                s.filter[0].mBand *= std::max(0.3, gain);
                s.filter[1] = s.filter[0];
                s.filterStages = 2;
                break;
            case 4: s.filter[0] = make(BT::Notch, hz, std::max(0.3, gain)); s.filterStages = 1; break;
            case 5: s.filter[0] = s.filter[1] = make(BT::Notch, hz, std::max(0.3, std::sqrt(gain))); s.filterStages = 2; break;
            case 6: s.filter[0] = make(BT::HighPass, hz, gain); s.filterStages = 1; break;
            case 7: s.filter[0] = s.filter[1] = make(BT::HighPass, hz, std::sqrt(gain)); s.filterStages = 2; break;
            default:
            {
                // Formant: the cutoff walks I > E > A > O > U; two formants as band-passes (F6 one, F12 both stages).
                const double position = std::clamp((std::log2(hz) - std::log2(30.0)) / std::log2(22000.0 / 30.0), 0.0, 1.0) * 4.0;
                const int v0 = std::min(3, (int) position);
                const double t = position - v0;
                const double f1 = kVowels[v0][0] + (kVowels[v0 + 1][0] - kVowels[v0][0]) * t;
                const double f2 = kVowels[v0][1] + (kVowels[v0 + 1][1] - kVowels[v0][1]) * t;
                const double q = 2.0 + 6.0 * res;
                s.filter[0] = make(BT::BandPass, f1, q);
                s.filter[0].mBand *= 7.5;
                s.filter[0].mLow += 0.2;
                s.filter[1] = make(BT::BandPass, f2, q);
                s.filter[1].mBand *= 5.4;
                s.filter[1].mLow += 0.25;
                s.filterStages = type == 8 ? 1 : 2;
                break;
            }
        }
    }
    else s.filterStages = 0;
    const float exciterGain = s.exciter == Hammer || s.exciter == HammerBouncing ? 0.22f : 1.0f; // fitted: hammer default -5.9 dBFS (law case) vs the factory presets' levels
    s.level = exciterGain * volumeGain(std::clamp(k(Volume), 0.0f, 1.0f)) * std::max(0.0f, 1.0f + targetMod(s, TVolume));
}

// One sample of one string. Returns the string's output before the body.
float Tension::tick(Str& s)
{
    const int index = (int) (&s - strings_.data());
    float* R = lineR_.data() + (size_t) index * (size_t) lineLength_;
    float* L = lineL_.data() + (size_t) index * (size_t) lineLength_;
    const int mask = lineMask_, w = s.write;
    // Right-going wave arriving at the bridge, read at the fractional length (4-point Lagrange).
    const double frac = s.mFrac - std::floor(s.mFrac);
    const int base = (int) std::floor(s.mFrac);
    const float y0 = R[(w - base + 1) & mask], y1 = R[(w - base) & mask], y2 = R[(w - base - 1) & mask], y3 = R[(w - base - 2) & mask];
    const double d = frac;
    const double c0 = -d * (d - 1) * (d - 2) / 6, c1 = (d + 1) * (d - 1) * (d - 2) / 2, c2 = -(d + 1) * d * (d - 2) / 2,
                 c3 = (d + 1) * d * (d - 1) / 6;
    const float atBridge = (float) (c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3);
    const float atNut = L[(w - s.mInt) & mask];

    // Bridge reflection: loss filter (one-pole low-pass, DC gain per period) and dispersion.
    s.lossState = (1.0 - s.lossPole) * (double) atBridge + s.lossPole * s.lossState;
    float reflected = (float) (-s.lossState * s.lossGain);
    for (int i = 0; i < s.apStages; ++i) reflected = allpass(reflected, s.apCoeff, s.apState[i]);
    // Nut / termination reflection.
    float nut = -atNut;
    if (s.nutPole > 0)
    {
        s.nutState = (1.0 - s.nutPole) * (double) nut + s.nutPole * s.nutState;
        nut = (float) (s.nutState * s.nutGain);
    }
    const int nw = (w + 1) & mask;
    R[nw] = nut;
    L[nw] = reflected;
    s.write = nw;
    // Spatial taps: R at x (from the nut) = R[nw - x]; L at x = L[nw - (mInt - x)]. Positions are measured from the bridge.
    auto at = [&](int fromBridge, float*& r, float*& l) {
        const int x = std::clamp(s.mInt - fromBridge, 1, s.mInt - 1);
        r = &R[(nw - x) & mask];
        l = &L[(nw - (s.mInt - x)) & mask];
    };

    // ---- exciter
    if (s.startDelay > 0) --s.startDelay;
    else if (s.exc.active)
    {
        float* r; float* l;
        at(s.excPos, r, l);
        const double vs = (double) *r + *l;
        if (s.exciter == Bow)
        {
            const double dv = s.bowVelocity - vs;
            const double table = std::min(1.0, std::pow(std::abs(dv / kBowSpeed * s.bowSlope * 0.08) + 0.75, -s.bowOffset));
            const double add = dv * table * s.bowForce;
            *r += (float) add;
            *l += (float) add;
        }
        else
        {
            s.exc.ys += vs;
            const double delta = s.exc.y - s.exc.ys;
            double force = 0;
            if (s.exciter == Plectrum)
            {
                const double t = s.plTime++;
                force = t < s.plPush ? s.plPeak * t / s.plPush : s.plPeak * std::exp(-(t - s.plPush) / s.plTau);
                force *= kPlectrumForce;
                if (t > s.plPush + 30.0 * s.plTau) s.exc.active = false;
            }
            else
            {
                // Hammer: m dv = -F; felt F = K delta^2.3 + R d(delta)/dt while in contact.
                if (delta > 0) force = s.excK * std::pow(delta / 0.001, 2.3) * 0.001 + s.excR * std::max(0.0, delta - s.exc.lastDelta);
                // Explicit contact: never more force than reverses the relative velocity within one sample.
                force = std::min(force, 2.0 * s.excMass * std::abs(s.exc.v - vs) + 1e-9);
                s.exc.v -= force / s.excMass;
                if (s.exciter == HammerBouncing && s.gate) s.exc.v += s.excVelocity * 0.0002; // pushed back by the key
                s.exc.y += s.exc.v;
                if (s.exciter == Hammer && delta < -0.01 && s.exc.v < 0) s.exc.active = false; // flew off
            }
            s.exc.lastDelta = delta;
            const double add = force * 0.5;
            *r += (float) add;
            *l += (float) add;
        }
    }
    // ---- damper: a felt pad (spring + dashpot to rest) that settles onto the string over its landing time.
    if (s.damper.active)
    {
        float* r; float* l;
        at(s.damperPos, r, l);
        const double vs = (double) *r + *l;
        s.damper.ys = s.damper.ys * 0.9999 + vs;
        s.damper.y = std::min(1.0, s.damper.y + s.damperV); // engagement 0..1
        // A spring term (K x displacement) through the explicit coupling gains energy: the felt is a dashpot only.
        const double force = -s.damperR * vs * s.damper.y;
        *r += (float) (force * 0.5);
        *l += (float) (force * 0.5);
    }
    // ---- output: the pickup (velocity at its position) or the force on the bridge
    float out;
    if (k(PickupOn) >= 0.5f)
    {
        float* r; float* l;
        at(s.pickupPos, r, l);
        out = (*r + *l) * 0.75f;
    }
    else out = atBridge;
    return out;
}

// ------------------------------------------------------------------------------------------------ body

void Tension::updateBody()
{
    const float type = std::round(k(BodyType)), size = std::round(k(BodySize)), decay = k(BodyDecay),
                cuts = k(BodyLowCut) * 1000.0f + k(BodyHighCut);
    const auto same = [](float a, float b) { return std::abs(a - b) < 1e-7f; };
    if (same(type, bodyKey_[0]) && same(size, bodyKey_[1]) && same(decay, bodyKey_[2]) && same(cuts, bodyKey_[3])) return;
    bodyKey_[0] = type; bodyKey_[1] = size; bodyKey_[2] = decay; bodyKey_[3] = cuts;
    const auto& modes = kBodies[std::clamp((int) type, 0, 3)];
    const float scale = kBodySizes[std::clamp((int) size, 0, 4)];
    for (int i = 0; i < kModes; ++i)
    {
        const double hz = std::min((double) modes[i].hz * scale, sampleRate_ * 0.45);
        const double q = modes[i].q * std::pow(25.0, decay);
        modeCoeffs_[(size_t) i] = BiquadCoeffs::make(BiquadCoeffs::Type::BandPass, sampleRate_, hz, q, 0.0);
        // Peak gain grows with Q: the reference's body rings much louder with Decay (body-decay1: +10 .. +60 dB).
        modeGain_[(size_t) i] = modes[i].gain * (float) std::min(q / modes[i].q, 25.0) * 0.9f;
    }
    // Low Cut 0..100 %: high-pass 20 Hz .. 2 kHz; High Cut: low-pass 20 kHz .. 1 kHz (assumed ranges).
    lowCut_ = BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate_, 20.0 * std::pow(100.0, k(BodyLowCut)), 0.707, 0);
    highCut_ = BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate_, std::min(sampleRate_ * 0.45, 20000.0 * std::pow(0.05, k(BodyHighCut))), 0.707, 0);
}

float Tension::bodyTick(float x)
{
    double sum = 0;
    for (int i = 0; i < kModes; ++i)
    {
        const auto& c = modeCoeffs_[(size_t) i];
        auto& z = modeState_[(size_t) i];
        const double y = c.b0 * x + z[0];
        z[0] = c.b1 * x - c.a1 * y + z[1];
        z[1] = c.b2 * x - c.a2 * y;
        sum += y * modeGain_[(size_t) i] * 0.7;
    }
    auto biquad = [](const BiquadCoeffs& c, std::array<double, 2>& z, double in) {
        const double y = c.b0 * in + z[0];
        z[0] = c.b1 * in - c.a1 * y + z[1];
        z[1] = c.b2 * in - c.a2 * y;
        return y;
    };
    sum = biquad(lowCut_, lowState_, sum);
    sum = biquad(highCut_, highState_, sum);
    return (float) sum;
}

// ------------------------------------------------------------------------------------------------ render

void Tension::render(float* left, float* right, int from, int to)
{
    if (from >= to) return;
    const bool body = k(BodyOn) >= 0.5f;
    if (body) updateBody();
    const float mix = k(BodyMix);
    // Output scale (fitted): the default plucked C3 peaks at -13.6 dBFS with Volume 0 dB.
    constexpr float kOutput = 2000.0f;
    for (int i = from; i < to; ++i)
    {
        float sum = 0;
        for (auto& s : strings_)
        {
            if (! s.active) continue;
            if (s.countdown-- <= 0) { control(s); s.countdown = kControl - 1; }
            float y = tick(s);
            if (! std::isfinite(y) || std::abs(y) > 0.05f) // ~70x full scale at the output
            {
                // Safety: a string that ran away (an extreme preset) is silenced rather than blowing up the mix.
                const int index = (int) (&s - strings_.data());
                std::fill_n(lineR_.data() + (size_t) index * (size_t) lineLength_, lineLength_, 0.0f);
                std::fill_n(lineL_.data() + (size_t) index * (size_t) lineLength_, lineLength_, 0.0f);
                s.active = false;
                continue;
            }
            // DC blocker (10 Hz): the bow and the bouncing hammer push the string one way.
            const float dc = y - s.dcIn + 0.9986f * s.dcOut;
            s.dcIn = y;
            s.dcOut = dc;
            y = dc;
            for (int st = 0; st < s.filterStages; ++st) y = s.fstate[st].process(s.filter[st], y);
            y *= s.level;
            const float magnitude = std::abs(y);
            s.silence = magnitude < 1e-6f ? s.silence + 1 : 0;
            if (s.released && s.silence > sampleRate_ * 0.05) s.active = false;
            if (! s.released && s.exciter != Bow && s.silence > sampleRate_ * 2.0) s.active = false;
            sum += y;
        }
        sum *= kOutput;
        if (body) sum = (1.0f - mix) * sum + mix * bodyTick(sum);
        left[i] += sum;
        right[i] += sum;
    }
}

void Tension::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed() || lineR_.empty()) return;
    parametersChanged.store(false, std::memory_order_relaxed);
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    int position = 0;
    const int n = audio.getNumSamples();
    for (const auto metadata : midi)
    {
        const int offset = std::clamp(metadata.samplePosition, position, n);
        render(left, right, position, offset);
        handleEvent(metadata.data, metadata.numBytes);
        position = offset;
    }
    render(left, right, position, n);
    int active = 0;
    for (const auto& s : strings_) active += s.active && s.member == 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

} // namespace sonora::devices
