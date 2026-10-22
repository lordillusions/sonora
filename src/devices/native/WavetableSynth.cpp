#include "devices/native/WavetableSynth.h"
#include "devices/native/SerumDistortion.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>
#include <stdexcept>

namespace sonora::devices
{
namespace
{
using W = WavetableSynth;
constexpr double tau = 2.0 * std::numbers::pi;
constexpr int tableSamples = 1024, tableFrames = 10, tableMips = 8, tableHarmonics = 128;
constexpr int controlPeriod = 16;

float unit(float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0; }
int choice(float x, int count) { return std::clamp(static_cast<int>(std::lround(x)), 0, count - 1); }
double wrap(double phase) { return phase - std::floor(phase); }
float balanceLeft(float pan) { return std::min(1.0f, 1.0f - pan); }
float balanceRight(float pan) { return std::min(1.0f, 1.0f + pan); }

// Serum 2 unison law, measured with sine wavetables on the reference: voice offsets are linear over
// [-D, D] cents (D = 200 detune^1.25); the voice(s) nearest the centre form the "centre class" (one voice for odd counts, two for even) with a
// different level from the others; every non-centre voice is panned to one side, alternating by pair; the stereo width scales the pair with
// (1 +- t)(1 +- 0.2615 t). Levels follow rational fits of 1/gain in the voice count at the default blend 0.75 and separable polynomials in blend^2.
struct SerumUnisonVoice { double cents; float gainCentre, gainSide; int side; }; // side: +1 left, -1 right, 0 centre
float serumUnisonCountGain(int count, bool centreClass) noexcept
{
    if (count <= 1) return 1.0f;
    if (count == 2) return 0.3206f;
    const double n = count;
    return static_cast<float>(1.0 / (centreClass ? 3.0564 + 0.08082 * n + 0.001904 * n * n : 3.917 + 0.1079 * n + 0.002244 * n * n));
}

// Sourced warps: kind 0 none, 1 FM, 2 PD, 3 AM, 4 RM; source 0 Osc1, 1 Osc2, 2 Sub.
struct SourcedWarp { int kind = 0, source = 0; };
SourcedWarp sourcedWarp(int mode) noexcept
{
    switch (mode)
    {
        case W::FmSub: return {1, 2};
        case W::FmOsc1: return {1, 0};
        case W::FmOsc2: return {1, 1};
        case W::PdOsc1: return {2, 0};
        case W::PdOsc2: return {2, 1};
        case W::PdSub: return {2, 2};
        case W::AmOsc1: return {3, 0};
        case W::AmOsc2: return {3, 1};
        case W::AmSub: return {3, 2};
        case W::RmOsc1: return {4, 0};
        case W::RmOsc2: return {4, 1};
        case W::RmSub: return {4, 2};
        default: return {};
    }
}
float serumUnisonBlendCentre(float blend) noexcept { return static_cast<float>((1.0 - 0.5547 * blend * blend) / 0.68795); }
float serumUnisonBlendSide(float blend) noexcept
{
    const double x = static_cast<double>(blend) * blend;
    return static_cast<float>(0.0832 + 2.404 * x - 1.379 * x * x);
}

std::vector<std::string> sourceNames()
{
    std::vector<std::string> out;
    for (int i = 0; i < W::NumSources; ++i) out.emplace_back(W::sourceName(i));
    return out;
}
std::vector<std::string> destinationNames()
{
    std::vector<std::string> out;
    for (int i = 0; i < W::NumDestinations; ++i) out.emplace_back(W::destinationName(i));
    return out;
}
constexpr const char* kFilterTypeNames[]{"Classic Low 12", "Classic Low 24", "Classic High 12", "Classic High 24", "Classic Band", "Classic Notch",
    "Morph", "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24", "Low 6", "Low 12", "Low 18", "Low 24", "High 6", "High 12", "High 18",
    "High 24", "Band 12", "Band 24", "Notch 12", "Notch 24", "Ladder MG", "Thru", "Dirty MG", "Ladder EMS",
    "Peak 12", "Peak 24", "LH 6", "LH 12", "LB 12", "LP 12", "LN 12", "HB 12", "HP 12", "HN 12", "BP 12", "BN 12", "PP 12", "PN 12",
    "NN 12", "L/B/H 12", "L/B/H 24", "L/P/H 12", "L/P/H 24", "L/N/H 12", "L/N/H 24", "B/P/N 12", "B/P/N 24", "Ladder Acid",
    "Comb +", "Comb -", "Comb L6 +", "Comb L6 -", "Comb H6 +", "Comb H6 -", "Comb HL6 +", "Comb HL6 -",
    "Flange +", "Flange -", "Flange L6 +", "Flange L6 -", "Flange H6 +", "Flange H6 -", "Flange HL6 +", "Flange HL6 -",
    "Phase 12 +", "Phase 12 -", "Phase 24 +", "Phase 24 -", "Phase 36 +", "Phase 36 -", "Phase 48 +", "Phase 48 -",
    "Phase 48 L6 +", "Phase 48 L6 -", "Phase 48 H6 +", "Phase 48 H6 -", "Phase 48 HL6 +", "Phase 48 HL6 -",
    "Flange-Phase HL6 +", "Flange-Phase HL6 -"};
static_assert(std::size(kFilterTypeNames) == W::NumFilterTypes);
std::vector<std::string> filterTypeNames() { return {std::begin(kFilterTypeNames), std::end(kFilterTypeNames)}; }
std::vector<ParameterInfo> infos()
{
    std::vector<ParameterInfo> p{
        makeParam("Volume", "Volume", -60, 12, -12, "dB"), makeParam("Transpose", "Transpose", -48, 48, 0, "st"),
        makeParam("BendUp", "Bend Up", -48, 48, 2, "st"), makeParam("BendDown", "Bend Down", -48, 48, 2, "st"),
        makeParam("Pan", "Pan", -1, 1, 0), makeParam("FilterRouting", "Filter Route", 0, 1, 0, "", false, {"Serial", "Parallel"}),
        makeParam("SubOn", "Sub", 0, 1, 0, "", true),
        makeParam("SubWave", "Sub Wave", 0, 3, 0, "", false, {"Sine", "Triangle", "Saw", "Square"}),
        makeParam("SubOctave", "Sub Octave", -4, 4, -1, "int"), makeParam("SubLevel", "Sub Level", 0, 1, 0.5f, "%"),
        makeParam("SubPan", "Sub Pan", -1, 1, 0), makeParam("SubDirect", "Sub Direct", 0, 1, 0, "", true),
        makeParam("NoiseOn", "Noise", 0, 1, 0, "", true), makeParam("NoiseLevel", "Noise Level", 0, 1, 0.1f, "%"),
        makeParam("NoisePan", "Noise Pan", -1, 1, 0), makeParam("NoiseColor", "Noise Color", 0, 1, 0, "%")
    };
    const std::vector<std::string> warps{"Off", "Bend", "Sync", "Mirror", "Pulse", "Phase Mod", "Ring Mod", "FM (Sub)", "FM (Osc)", "FM (Osc 2)", "PD (Osc)", "PD (Osc 2)", "PD (Sub)", "AM (Osc)", "AM (Osc 2)", "AM (Sub)", "RM (Osc)", "RM (Osc 2)", "RM (Sub)", "Self PD",
                                       "Dist Tube", "Dist Soft Clip", "Dist Hard Clip", "Dist Diode 1", "Dist Diode 2", "Dist Zero Square",
                                       "Dist Asym", "Dist Rectify", "Dist Stomp Box", "Dist Tape Sat", "Dist Soft Sat", "Filter LPF", "Filter HPF"};
    for (int i = 0; i < W::kOscillators; ++i)
    {
        const auto path = "Osc." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "On", "On", 0, 1, i == 0 ? 1.0f : 0.0f, "", true));
        p.push_back(makeParam(path + "Table", "Table", 0, W::kTableFamilies - 1, 0, "", false, {"Default Shapes", "Harmonic", "Hollow", "Formant", "Custom"}));
        p.push_back(makeParam(path + "Position", "Position", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Level", "Level", 0, 1, 0.8f, "%"));
        p.push_back(makeParam(path + "Octave", "Octave", -4, 4, 0, "int"));
        p.push_back(makeParam(path + "Semi", "Semitone", -12, 12, 0, "st"));
        p.push_back(makeParam(path + "Fine", "Fine", -100, 100, 0, "ct"));
        p.push_back(makeParam(path + "Unison", "Unison", 1, W::kUnison, 1, "int"));
        p.push_back(makeParam(path + "Detune", "Detune", 0, 200, 12, "ct"));
        p.push_back(makeParam(path + "Blend", "Blend", 0, 1, 0.7f, "%"));
        p.push_back(makeParam(path + "Width", "Width", -1, 1, 0.8f, "%"));
        p.push_back(makeParam(path + "Pan", "Pan", -1, 1, 0));
        p.push_back(makeParam(path + "Phase", "Phase", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Random", "Random", 0, 1, 0.25f, "%"));
        p.push_back(makeParam(path + "Warp1Mode", "Warp 1", 0, W::NumWarps - 1, 0, "", false, warps));
        p.push_back(makeParam(path + "Warp1Amount", "Amount 1", -1, 1, 0));
        p.push_back(makeParam(path + "Warp2Mode", "Warp 2", 0, W::NumWarps - 1, 0, "", false, warps));
        p.push_back(makeParam(path + "Warp2Amount", "Amount 2", -1, 1, 0));
        p.push_back(makeParam(path + "RouteFilter1", "Filter 1", 0, 1, 1, "", true));
        p.push_back(makeParam(path + "RouteFilter2", "Filter 2", 0, 1, 0, "", true));
    }
    for (int i = 0; i < W::kFilters; ++i)
    {
        const auto path = "Filter." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "On", "On", 0, 1, i == 0 ? 1.0f : 0.0f, "", true));
        p.push_back(makeParam(path + "Mode", "Type", 0, W::NumFilterTypes - 1, 0, "", false,
                              filterTypeNames()));
        p.push_back(makeParam(path + "Cutoff", "Cutoff", 20, 20000, 10000, "Hz"));
        p.push_back(makeParam(path + "Resonance", "Resonance", 0, 1, 0.05f, "%"));
        p.push_back(makeParam(path + "Drive", "Drive", 0, 24, 0, "dB"));
        p.push_back(makeParam(path + "Keytrack", "Key Track", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "EnvAmount", "Env Amount", -8, 8, 0, "oct"));
        p.push_back(makeParam(path + "EnvSource", "Envelope", 0, W::kEnvelopes - 1, 1, "", false, {"Env 1", "Env 2", "Env 3", "Env 4"}));
        p.push_back(makeParam(path + "Mix", "Mix", 0, 1, 1, "%"));
        p.push_back(makeParam(path + "Morph", "Morph", 0, 1, 0, "%"));
    }
    for (int i = 0; i < W::kEnvelopes; ++i)
    {
        const auto path = "Env." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "Delay", "Delay", 0, 32, 0, "s"));
        p.push_back(makeParam(path + "Attack", "Attack", 0, 32, i == 0 ? 0.005f : 0.01f, "s"));
        p.push_back(makeParam(path + "Hold", "Hold", 0, 32, 0, "s"));
        p.push_back(makeParam(path + "Decay", "Decay", 0, 32, i == 0 ? 0.35f : 0.7f, "s"));
        p.push_back(makeParam(path + "Sustain", "Sustain", 0, 1, i == 0 ? 0.8f : 0.0f, "%"));
        p.push_back(makeParam(path + "Release", "Release", 0, 32, 0.2f, "s"));
        p.push_back(makeParam(path + "AttackCurve", "Attack Curve", -1, 1, 0));
        p.push_back(makeParam(path + "DecayCurve", "Decay Curve", -1, 1, 0));
        p.push_back(makeParam(path + "ReleaseCurve", "Release Curve", -1, 1, 0));
    }
    std::vector<std::string> divisions;
    for (int division = 0; division < W::kLfoDivisions; ++division) divisions.emplace_back(dsp::LfoTempoDivisions::label(division));
    std::vector<std::string> shapes;
    for (int s = 0; s < dsp::CurveGraph::kShapeCount; ++s) shapes.emplace_back(dsp::CurveGraph::shapeName(s));
    shapes.emplace_back("Custom");
    for (int i = 0; i < W::kLfos; ++i)
    {
        const auto path = "Lfo." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "Shape", "Shape", 0, W::CustomLfoShape, 1, "", false, shapes));
        p.push_back(makeParam(path + "Rate", "Rate", 0, 100, 1, "Hz"));
        p.push_back(makeParam(path + "Sync", "Sync", 0, 1, 1, "", true));
        p.push_back(makeParam(path + "Division", "Division", 0, W::kLfoDivisions - 1, 4, "", false, divisions));
        p.push_back(makeParam(path + "Mode", "Trigger", 0, 2, 1, "", false, {"Global", "Retrigger", "One Shot"}));
        p.push_back(makeParam(path + "Phase", "Phase", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Bipolar", "Bipolar", 0, 1, 1, "", true));
        p.push_back(makeParam(path + "Fade", "Fade In", 0, 8, 0, "s"));
    }
    const auto sources = sourceNames(), destinations = destinationNames();
    for (int i = 0; i < W::kModSlots; ++i)
    {
        const auto path = "Mod." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "Source", "Source", 0, W::NumSources - 1, 0, "", false, sources));
        p.push_back(makeParam(path + "Destination", "Destination", 0, W::NumDestinations - 1, 0, "", false, destinations));
        p.push_back(makeParam(path + "Amount", "Amount", -1, 1, 0));
        p.push_back(makeParam(path + "AuxSource", "Via", 0, W::NumSources - 1, 0, "", false, sources));
        p.push_back(makeParam(path + "Bipolar", "Bipolar", 0, 1, 0, "", true));
    }
    for (int i = 0; i < W::kMacros; ++i) p.push_back(makeParam("Macro." + std::to_string(i), "Macro " + std::to_string(i + 1), 0, 1, 0, "%"));
    for (int i = 0; i < W::kOscillators; ++i)
    {
        const auto path = "Media." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "Mode", "Engine", 0, W::NumOscillatorModes - 1, 0, "", false, {"Wavetable", "Sample", "Multisample", "Granular", "Spectral"}));
        p.push_back(makeParam(path + "Start", "Start", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Loop", "Loop", 0, 4, 0, "", false, {"Asset", "Off", "Forward", "Sustain", "Pingpong"}));
        p.push_back(makeParam(path + "Reverse", "Reverse", 0, 1, 0, "", true));
        p.push_back(makeParam(path + "OneShot", "One Shot", 0, 1, 0, "", true));
        p.push_back(makeParam(path + "ScanRate", "Scan Rate", -8, 8, 1, "x"));
        p.push_back(makeParam(path + "GrainSize", "Grain Size", 0.001f, 8, 0.05f, "s"));
        p.push_back(makeParam(path + "Density", "Density", 0.1f, 1000, 8));
        p.push_back(makeParam(path + "DensityMode", "Density Unit", 0, 2, 1, "", false, {"Hz", "Grains", "Tempo"}));
        p.push_back(makeParam(path + "DensityBeats", "Density Beats", 0.015625f, 64, 0.25f, "beats"));
        p.push_back(makeParam(path + "GrainLimit", "Max Grains", 1, OscillatorMediaVoice::maxGrains, 16, "int"));
        p.push_back(makeParam(path + "PositionSpread", "Position Spread", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "PitchSpread", "Pitch Spread", 0, 24, 0, "st"));
        p.push_back(makeParam(path + "PanSpread", "Pan Spread", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "DirectionSpread", "Direction Spread", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "LengthSpread", "Length Spread", 0, 0.95f, 0, "%"));
        p.push_back(makeParam(path + "GainSpread", "Gain Spread", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Window", "Window", 0, 2, 0, "", false, {"Hann", "Triangle", "Rectangle"}));
        p.push_back(makeParam(path + "WindowAmount", "Window Amount", 0, 1, 1, "%"));
        p.push_back(makeParam(path + "WindowSkew", "Window Skew", -0.95f, 0.95f, 0));
        p.push_back(makeParam(path + "JumpStart", "Jump Start", 0, 1, 0, "", true));
        p.push_back(makeParam(path + "VelocityTracking", "Asset Velocity", 0, 1, 1, "", true));
    }
    for (int i = 0; i < W::kOscillators; ++i)
        p.push_back(makeParam("Osc." + std::to_string(i) + "/Coarse", "Coarse", -64, 64, 0, "st"));
    for (int i = 1; i < W::kEnvelopes; ++i)
    {
        const auto path = "Env." + std::to_string(i) + "/";
        p.push_back(makeParam(path + "Start", "Start Level", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "End", "End Level", 0, 1, 0, "%"));
    }
    for (int i = 0; i < W::kModSlots; ++i)
        p.push_back(makeParam("Mod." + std::to_string(i) + "/Out", "Out", 0, 1, 1, "%"));
    for (int i = 0; i < W::kLfos; ++i)
        p.push_back(makeParam("Lfo." + std::to_string(i) + "/Anchor", "Anchor", 0, 1, 1, "", true));
    for (int i = 0; i < W::kLfos; ++i)
        p.push_back(makeParam("Lfo." + std::to_string(i) + "/Rate10x", "Rate 10x", 0, 1, 0, "", true));
    p.push_back(makeParam("Voice/Mono", "Mono", 0, 1, 0, "", true));
    p.push_back(makeParam("Voice/Legato", "Legato", 0, 1, 0, "", true));
    p.push_back(makeParam("Voice/PortaAlways", "Always", 0, 1, 0, "", true));
    p.push_back(makeParam("Voice/PortaScaled", "Scaled", 0, 1, 0, "", true));
    p.push_back(makeParam("Voice/PortaTime", "Portamento", 0, 8, 0, "s"));
    p.push_back(makeParam("Voice/PortaCurve", "Porta Curve", -100, 100, 0));
    p.push_back(makeParam("Voice/Priority", "Priority", 0, 2, 0, "", false, {"Last", "Low", "High"}));
    for (int source = 0; source < W::kRoutingSources; ++source)
    {
        const auto path = "Routing." + std::to_string(source) + "/";
        p.push_back(makeParam(path + "Destination", "Destination", 0, 3, source == 0 ? 0 : 1, "", false, {"Filters", "Main FX", "Direct", "None"}));
        p.push_back(makeParam(path + "FilterBalance", "Filter Balance", -1, 1, -1));
        p.push_back(makeParam(path + "Bus1", "Bus 1", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Bus2", "Bus 2", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "Envelope", "Via Env 1", 0, 1, 1, "", true));
    }
    p.push_back(makeParam("FxRouting/DirectGain", "Direct Volume", 0, 4, 1, "x"));
    p.push_back(makeParam("Routing/On", "Source Routing", 0, 1, 0, "", true));
    for (int filter = 0; filter < W::kFilters; ++filter)
        p.push_back(makeParam("Filter." + std::to_string(filter) + "/OutputLevel", "Output Level", 0, 4, 1, "x"));
    p.push_back(makeParam("Sub/Phase", "Sub Phase", 0, 1, 0, "%"));
    p.push_back(makeParam("Sub/Continuous", "Sub Continue", 0, 1, 0, "", true));
    p.push_back(makeParam("Noise/Pitch", "Noise Pitch", -192, 48, 0, "st"));
    p.push_back(makeParam("Noise/Fine", "Noise Fine", -96, 96, 0, "ct"));
    p.push_back(makeParam("Noise/Keytrack", "Noise Keytrack", 0, 1, 0, "", true));
    p.push_back(makeParam("Noise/Phase", "Noise Phase", 0, 1, 0, "%"));
    p.push_back(makeParam("Noise/Random", "Noise Random", 0, 1, 0, "%"));
    p.push_back(makeParam("Arp/On", "Arp", 0, 1, 0, "", true));
    p.push_back(makeParam("Arp/Mode", "Arp Mode", 0, W::NumArpPatterns - 1, 0, "", false, {"Up", "Down", "Up/Down", "Down/Up", "As Played", "Random"}));
    p.push_back(makeParam("Arp/Rate", "Arp Rate", 0, W::kArpRates - 1, 3, "", false, {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"}));
    p.push_back(makeParam("Arp/Octaves", "Octaves", 1, 4, 1, "int"));
    p.push_back(makeParam("Arp/Gate", "Gate", 0.05f, 1, 0.5f, "%"));
    for (const char* curve : {"Velocity", "Note"})
    {
        const std::string path = std::string(curve) + "Curve/", name = std::string(curve) == "Velocity" ? "Velo" : "Note";
        p.push_back(makeParam(path + "Start", name + " Start", 0, 1, 0, "%"));
        p.push_back(makeParam(path + "MidX", name + " Mid In", 0, 1, 0.5f, "%"));
        p.push_back(makeParam(path + "MidY", name + " Mid Out", 0, 1, 0.5f, "%"));
        p.push_back(makeParam(path + "End", name + " End", 0, 1, 1, "%"));
    }
    p.push_back(makeParam("Voice/Polyphony", "Poly", 1, W::kVoices, W::kVoices, "int"));
    p.push_back(makeParam("Noise/Type", "Noise Type", 0, W::NumNoiseTypes - 1, 0, "", false, {"Sample", "White", "Pink", "Brown", "Geiger"}));
    for (int i = 0; i < W::kLfos; ++i)
        p.push_back(makeParam("Lfo." + std::to_string(i) + "/Type", "LFO Type", 0, W::NumLfoTypes - 1, 0, "", false,
                              {"Normal", "Chaos: Lorenz", "Chaos: Rossler", "Random S&H"}));
    return p;
}

float harmonicCoefficient(int family, float position, int harmonic)
{
    const double h = harmonic;
    if (family == W::Basic)
    {
        const auto coefficient = [h, harmonic](int shape)
        {
            if (shape == 0) return harmonic == 1 ? 1.0 : 0.0;
            if (shape == 1) return harmonic % 2 == 0 ? 0.0 : (harmonic % 4 == 1 ? 1.0 : -1.0) * 8.0 / (std::numbers::pi * std::numbers::pi * h * h);
            if (shape == 2) return (harmonic % 2 == 1 ? 1.0 : -1.0) * 2.0 / (std::numbers::pi * h);
            return harmonic % 2 == 0 ? 0.0 : 4.0 / (std::numbers::pi * h);
        };
        const float where = position * 3;
        const int shape = std::min(2, static_cast<int>(where));
        const double mix = where - static_cast<float>(shape);
        return static_cast<float>(coefficient(shape) * (1 - mix) + coefficient(shape + 1) * mix);
    }
    if (family == W::Harmonic)
    {
        const double emphasis = std::sin(std::numbers::pi * (static_cast<double>(position) * 3 + h * 0.137));
        return static_cast<float>((0.2 + 0.8 * emphasis * emphasis) / h);
    }
    if (family == W::Hollow)
    {
        const double odd = harmonic % 2 == 1 ? 1.0 : static_cast<double>(position) * 0.35;
        return static_cast<float>(odd * std::cos(h * (0.1 + static_cast<double>(position) * 1.3)) / std::pow(h, 1.2));
    }
    const double center = 2 + static_cast<double>(position) * 24, width = 1.5 + static_cast<double>(position) * 5;
    const double distance = (h - center) / width;
    return static_cast<float>(0.45 * std::exp(-0.5 * distance * distance) / std::sqrt(h) + (harmonic == 1 ? 0.3 : 0));
}
} // namespace

struct WavetableSynth::TableBank
{
    using Wave = std::array<float, tableSamples + 1>;
    std::array<std::array<std::array<Wave, tableMips>, tableFrames>, kGeneratedTableFamilies> wave{};
    TableBank()
    {
        std::array<std::array<float, tableSamples>, tableHarmonics> basis{};
        for (int h = 1; h <= tableHarmonics; ++h)
            for (int i = 0; i < tableSamples; ++i)
                basis[static_cast<size_t>(h - 1)][static_cast<size_t>(i)] = static_cast<float>(std::sin(tau * h * i / tableSamples));
        for (int family = 0; family < kGeneratedTableFamilies; ++family)
            for (int frame = 0; frame < tableFrames; ++frame)
            {
                std::array<float, tableHarmonics> coefficients{};
                for (int h = 1; h <= tableHarmonics; ++h)
                    coefficients[static_cast<size_t>(h - 1)] = harmonicCoefficient(family, static_cast<float>(frame) / (tableFrames - 1), h);
                for (int mip = 0; mip < tableMips; ++mip)
                {
                    auto& output = wave[static_cast<size_t>(family)][static_cast<size_t>(frame)][static_cast<size_t>(mip)];
                    for (int h = 1; h <= (tableHarmonics >> mip); ++h)
                    {
                        const auto& sine = basis[static_cast<size_t>(h - 1)];
                        const float coefficient = coefficients[static_cast<size_t>(h - 1)];
                        for (int i = 0; i < tableSamples; ++i) output[static_cast<size_t>(i)] += sine[static_cast<size_t>(i)] * coefficient;
                    }
                    output[tableSamples] = output[0];
                }
                float peak = 0;
                for (const auto& output : wave[static_cast<size_t>(family)][static_cast<size_t>(frame)])
                    for (const auto s : output) peak = std::max(peak, std::abs(s));
                const float normalization = 1.0f / std::max(1.0f, peak);
                for (auto& output : wave[static_cast<size_t>(family)][static_cast<size_t>(frame)])
                    for (auto& s : output) s *= normalization;
            }
    }
    float sample(int family, float position, int mip, double phase) const
    {
        const float frame = unit(position) * (tableFrames - 1);
        const int first = std::min(tableFrames - 2, static_cast<int>(frame));
        const float frameMix = frame - static_cast<float>(first);
        const double offset = wrap(phase) * tableSamples;
        const int index = static_cast<int>(offset);
        const float mix = static_cast<float>(offset - index);
        const auto& a = wave[static_cast<size_t>(family)][static_cast<size_t>(first)][static_cast<size_t>(mip)];
        const auto& b = wave[static_cast<size_t>(family)][static_cast<size_t>(first + 1)][static_cast<size_t>(mip)];
        const auto at = static_cast<size_t>(index);
        const float left = a[at] + (a[at + 1] - a[at]) * mix;
        const float right = b[at] + (b[at + 1] - b[at]) * mix;
        return left + (right - left) * frameMix;
    }
};

float WavetableSynth::observedLevel(float normalized) { const float u = unit(normalized); return u * u; }
float WavetableSynth::observedEnvelopeSeconds(float normalized) { const float u = unit(normalized); return 32 * u * u * u * u * u; }
float WavetableSynth::observedLfoHertz(float normalized) { const float u = unit(normalized); return 100 * u * u * u * u; }
float WavetableSynth::observedTableIndex(float normalized) { return 1 + 255 * unit(normalized); }
float WavetableSynth::observedPanGain(float normalized, bool right)
{
    // Settled public-control sweeps establish a shared quadratic boost. The
    // physical center gain is kept at one; the immutable source bank owns its scale.
    constexpr float boost = .415890134f, center = .5f + .25f * boost;
    const float u = unit(normalized);
    return ((right ? u : 1 - u) + boost * u * (1 - u)) / center;
}
float WavetableSynth::observedFilterHertz(float normalized, double sampleRate)
{
    const double rate = std::isfinite(sampleRate) ? std::max(1.0, sampleRate) : 44100;
    const float u = std::isfinite(normalized) ? unit(normalized) : 0;
    return static_cast<float>(std::min(rate * 0.5, 440 * std::exp2(11.4 * u - 5.75)));
}
double WavetableSynth::divisionBeats(int division)
{
    return dsp::LfoTempoDivisions::beats(division);
}
const char* WavetableSynth::tableName(int table)
{
    static constexpr const char* names[]{"Default Shapes", "Harmonic", "Hollow", "Formant", "Custom"};
    return names[std::clamp(table, 0, kTableFamilies - 1)];
}
double WavetableSynth::arpRateBeats(int rate)
{
    static constexpr double beats[kArpRates]{1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};
    return beats[std::clamp(rate, 0, kArpRates - 1)];
}
std::shared_ptr<const WavetableData> WavetableSynth::defaultShapesTable()
{
    // Nine 2048-sample frames, each an elementary waveform over one cycle p in [0, 1): a saw rising through zero to its
    // peak at mid-cycle, a sine, a triangle, a split ramp (falling 1 -> 0, then -1 -> 0), a square, then pulses of width
    // 1/4, 1/8, 1/16 and 1/32 high just before mid-cycle. Built once on the first instance (message thread).
    static const std::shared_ptr<const WavetableData> table = [] {
        constexpr int length = WavetableData::nativeFrameLength, frames = 9;
        std::vector<float> samples(static_cast<size_t>(length * frames));
        for (int frame = 0; frame < frames; ++frame)
            for (int i = 0; i < length; ++i)
            {
                const double p = static_cast<double>(i) / length;
                const bool first = i < length / 2;
                double value = 0;
                switch (frame)
                {
                    case 0: value = first ? 2 * p : 2 * p - 2; break;
                    case 1: value = std::sin(tau * p); break;
                    case 2: value = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; break;
                    case 3: value = first ? 1 - 2 * p : 2 * p - 2; break;
                    case 4: value = first ? 1 : -1; break;
                    default:
                    {
                        const int width = length >> (frame - 3); // 1/4 .. 1/32 of the cycle
                        value = i >= length / 2 - width && first ? 1 : -1;
                    }
                }
                samples[static_cast<size_t>(frame * length + i)] = static_cast<float>(value);
            }
        return WavetableData::fromSamples(samples, 1, length, 44100, "Default Shapes", WavetableData::PlaybackProfile::Dense12,
                                          {WavetableFrameLayout::Steps, 1, false}).data;
    }();
    return table;
}
const WavetableData* WavetableSynth::oscillatorTable(int oscillator, int table) const
{
    if (table == Custom)
        if (const auto* custom = customTables_[static_cast<size_t>(oscillator)].get()) return custom;
    return table == Basic || table == Custom ? defaultShapes_.get() : nullptr;
}
int WavetableSynth::tableFrameCount(int oscillator) const
{
    if (oscillator < 0 || oscillator >= kOscillators) return 0;
    const auto* data = oscillatorTable(oscillator, choice(value(oscParam(oscillator, Table)), kTableFamilies));
    return data != nullptr ? data->frames() : tableFrames;
}
float WavetableSynth::tableFrameAt(int oscillator, int frame, float phase) const
{
    if (oscillator < 0 || oscillator >= kOscillators || ! std::isfinite(phase)) return 0;
    const int table = choice(value(oscParam(oscillator, Table)), kTableFamilies);
    const auto* data = oscillatorTable(oscillator, table);
    const double p = wrap(phase);
    if (data == nullptr) return tables_->sample(table, static_cast<float>(std::clamp(frame, 0, tableFrames - 1)) / (tableFrames - 1), 0, p);
    const int frames = data->frames(), length = data->frameLength(), channels = data->channels();
    const auto& original = data->originalSamples();
    const int index = std::clamp(frame, 0, frames - 1) * length + std::min(length - 1, static_cast<int>(p * length));
    return original[static_cast<size_t>(index * channels)];
}
float WavetableSynth::tableFramePosition(int oscillator) const
{
    if (oscillator < 0 || oscillator >= kOscillators) return 0;
    return tableFramePosition(oscillator, value(oscParam(oscillator, Position)));
}
const char* WavetableSynth::sourceName(int source)
{
    static constexpr const char* names[]{"Off", "Env 1", "Env 2", "Env 3", "Env 4", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8", "LFO 9", "LFO 10", "Velocity", "Key Track", "Mod Wheel", "Pressure", "Note Random", "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8",
        "Note Random 2", "Note Alternate", "Note Alternate 2", "Note Random (Discrete)", "Fixed",
        "LFO 1 Y", "LFO 2 Y", "LFO 3 Y", "LFO 4 Y", "LFO 5 Y", "LFO 6 Y", "LFO 7 Y", "LFO 8 Y", "LFO 9 Y", "LFO 10 Y"};
    static_assert(std::size(names) == NumSources);
    return names[std::clamp(source, 0, NumSources - 1)];
}
float WavetableSynth::dirtyMgFeedback(float resonance)
{
    // Fitted ladder feedback k at resonance 0 / 25 / 50 / 75 % (0, 0.126, 0.971, 3.266), self-oscillating (4) at 100 %.
    static constexpr float k[]{0.0f, 0.126f, 0.971f, 3.266f, 4.0f};
    const float x = std::clamp(resonance, 0.0f, 1.0f) * 4;
    const int i = std::min(3, static_cast<int>(x));
    return (k[i] + (k[i + 1] - k[i]) * (x - static_cast<float>(i))) / 4;
}
const char* WavetableSynth::filterTypeName(int type) { return kFilterTypeNames[std::clamp(type, 0, NumFilterTypes - 1)]; }
const char* WavetableSynth::destinationName(int destination)
{
    static constexpr const char* names[]{"Off", "Pitch", "Volume", "Pan", "A Position", "B Position", "C Position", "A Level", "B Level", "C Level", "A Pitch", "B Pitch", "C Pitch", "A Pan", "B Pan", "C Pan", "A Warp 1", "B Warp 1", "C Warp 1", "A Warp 2", "B Warp 2", "C Warp 2", "Filter 1 Cut", "Filter 2 Cut", "Filter 1 Res", "Filter 2 Res", "Sub Level", "Noise Level", "A Scan Rate", "B Scan Rate", "C Scan Rate", "A Media Position", "B Media Position", "C Media Position", "A Grain Size", "B Grain Size", "C Grain Size", "A Grain Density", "B Grain Density", "C Grain Density",
        "A Bus 1 Send", "B Bus 1 Send", "C Bus 1 Send", "Noise Bus 1 Send", "Sub Bus 1 Send", "Filter 1 Bus 1 Send", "Filter 2 Bus 1 Send",
        "A Bus 2 Send", "B Bus 2 Send", "C Bus 2 Send", "Noise Bus 2 Send", "Sub Bus 2 Send", "Filter 1 Bus 2 Send", "Filter 2 Bus 2 Send",
        "Env 1 Attack", "Env 1 Decay", "Env 1 Sustain", "Env 1 Release", "Env 2 Attack", "Env 2 Decay", "Env 2 Sustain", "Env 2 Release",
        "Env 3 Attack", "Env 3 Decay", "Env 3 Sustain", "Env 3 Release", "Env 4 Attack", "Env 4 Decay", "Env 4 Sustain", "Env 4 Release", "FX Parameter",
        "A Fine", "B Fine", "C Fine", "A Detune", "B Detune", "C Detune", "A Octave", "B Octave", "C Octave", "Filter 1 Mix", "Filter 2 Mix",
        "LFO 1 Rate", "LFO 2 Rate", "LFO 3 Rate", "LFO 4 Rate", "LFO 5 Rate", "LFO 6 Rate", "LFO 7 Rate", "LFO 8 Rate", "LFO 9 Rate", "LFO 10 Rate",
        "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8",
        "Master Tuning", "Filter 1 Level", "Filter 2 Level", "Filter 1 Drive", "Filter 2 Drive", "Voice Amp", "Noise Pan", "Sub Pan", "Filter 1 Var", "Filter 2 Var",
        "LFO Point Bus 1", "LFO Point Bus 2", "LFO Point Bus 3", "LFO Point Bus 4", "LFO Point Bus 5", "LFO Point Bus 6", "LFO Point Bus 7",
        "LFO Point Bus 8", "LFO Point Bus 9", "LFO Point Bus 10", "LFO Point Bus 11", "LFO Point Bus 12", "LFO Point Bus 13",
        "LFO Point Bus 14", "LFO Point Bus 15", "LFO Point Bus 16", "A Semitone", "B Semitone", "C Semitone", "Sub Coarse",
        "A Filter Balance", "B Filter Balance", "C Filter Balance", "Noise Filter Balance", "Sub Filter Balance",
        "A Blend", "B Blend", "C Blend", "A Width", "B Width", "C Width", "Transpose"};
    static_assert(std::size(names) == NumDestinations);
    return names[std::clamp(destination, 0, NumDestinations - 1)];
}

WavetableSynth::WavetableSynth() : NativeDevice(infos()), mediaVoices_(std::make_unique<MediaVoices>())
{
    // This static initialization (allocation-free table storage, Fourier generation) occurs here, never in process().
    static const TableBank bank;
    tables_ = &bank;
    snapshot();
}
void WavetableSynth::prepare(double sampleRate, int maxBlockSize)
{
    hostSampleRate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 44100;
    voiceRateFactor_ = requestedVoiceRateFactor_;
    voiceRatePreparationError_.clear();
    fxBlockCapacity_ = std::clamp(maxBlockSize, 1, maxFxBlockSize);
    try
    {
        if (! voiceDownsampler_.prepare(voiceRateFactor_)) throw std::runtime_error("Could not prepare voice rate converter");
        for (auto& lane : internalVoiceBuffers_)
            lane.resize(voiceRateFactor_ == 2 ? static_cast<size_t>(fxBlockCapacity_ * 2) : 0);
    }
    catch (const std::exception& error)
    {
        voiceRatePreparationError_ = error.what(); voiceRateFactor_ = 1;
        voiceDownsampler_.prepare(1);
        for (auto& lane : internalVoiceBuffers_) lane.clear();
    }
    sampleRate_ = hostSampleRate_ * voiceRateFactor_;
    combLineLength_ = dsp::MeasuredCombFilter::lineLength(sampleRate_);
    combLines_.assign(static_cast<size_t>(kVoices * kFilters * 2) * static_cast<size_t>(combLineLength_), 0.0f);
    smoothCoefficient_ = dsp::timeToCoef(2.0f, sampleRate_);
    masterSmoothCoefficient_ = dsp::timeToCoef(2.0f, hostSampleRate_);
    monoVoice_.prepare(sampleRate_);
    if (effects_ != nullptr)
    {
        const auto configuration = effects_->snapshotConfiguration();
        effectsReady_ = setEffectsConfiguration(configuration, effectsPreparationError_);
        // A failed preparation keeps controls/ownership available for save and a descriptive load warning.
        // The callback falls back to the unprocessed synth rather than rendering an IR at the wrong rate.
    }
    reset();
    snapshot();
}
float* WavetableSynth::combLine(const Voice& voice, int filter, int channel) const
{
    const auto index = &voice - voices_.data();
    if (combLineLength_ <= 0 || index < 0 || index >= kVoices) return nullptr; // the FX modulation voice renders no audio
    const auto line = (static_cast<size_t>(index) * kFilters + static_cast<size_t>(filter)) * 2 + static_cast<size_t>(channel);
    return combLines_.data() + line * static_cast<size_t>(combLineLength_);
}
void WavetableSynth::resetEffects()
{
    voiceDownsampler_.reset();
    if (effects_ != nullptr) effects_->reset();
    for (auto& target : fxTargets_) target.applied = -1;
    fxVoice_ = Voice{}; fxVoiceSlot_ = -1; fxDiscreteSlot_ = -1; fxModulationClock_ = 0;
    for (auto& line : directSubAlignment_) if (line.capacity() > 0) line.reset();
    masterGainInitialized_ = false;
}
void WavetableSynth::reset()
{
    voices_ = {};
    monoVoice_.reset();
    for (auto& row : *mediaVoices_) for (auto& media : row) media.reset();
    bends_ = {}; pressures_ = {}; pedals_ = {};
    wheels_.fill(importedModWheel_);
    globalLfoPhase_ = {}; globalLfoValue_ = {}; processingEndLfoPhase_ = {};
    retainedSubPhase_ = 0;
    hasHostClock_ = false; expectedHostBeat_ = 0;
    age_ = 0; sampleClock_ = 0; randomSeed_ = 0x13579bdu;
    serumRandom_.seed(5489); serumRandom_.discard(kSerumInitialDraws);
    discreteRandom_ = {}; pendingDiscreteRandom_ = {}; lastStartedVoice_ = -1; alternateState_ = true; ghosts_ = {};
    arp_ = ArpState{};
    wasBypassed_ = false;
    activeVoiceCount_.store(0, std::memory_order_relaxed);
    resetEffects();
}
bool WavetableSynth::setEffectsConfiguration(const WavetableEffects::Configuration& configuration, std::string& error)
{
    error.clear();
    try
    {
        auto next = std::make_unique<WavetableEffects>();
        if (! next->prepare(configuration, hostSampleRate_, error)) return false;
        auto infos = NativeDevice::parameters();
        std::vector<FxBinding> bindings;
        static constexpr std::array<const char*, FxRoutingParams> routeIds{"MainToBus1", "MainToBus2", "Bus1ToMain", "Bus2ToMain", "MainGain"};
        static constexpr std::array<const char*, FxRoutingParams> routeNames{"Send Bus 1", "Send Bus 2", "Return Bus 1", "Return Bus 2", "Main Bus Gain"};
        for (int parameter = 0; parameter < FxRoutingParams; ++parameter)
        {
            const auto at = static_cast<size_t>(parameter);
            infos.push_back(makeParam(std::string("FxRouting/") + routeIds[at], routeNames[at], 0,
                parameter == Bus1ToMain || parameter == Bus2ToMain ? 4 : 2, next->getRoutingParameter(parameter)));
            bindings.push_back({0, -1, parameter});
        }
        std::vector<WavetableEffects::ModuleId> ids(next->moduleIds().begin(), next->moduleIds().end());
        std::sort(ids.begin(), ids.end());
        for (const auto id : ids)
        {
            const auto type = next->moduleType(id);
            if (! type) continue;
            const std::string prefix = "Fx." + std::to_string(id) + "/";
            infos.push_back(makeParam(prefix + "On", "On", 0, 1, next->enabled(id) ? 1.0f : 0.0f, "", true));
            bindings.push_back({id, -1, -1});
            const auto descriptors = WavetableEffects::parameterDescriptors(*type);
            for (size_t parameter = 0; parameter < descriptors.size(); ++parameter)
            {
                const auto& descriptor = descriptors[parameter];
                std::vector<std::string> choices;
                for (const auto choiceName : descriptor.choices) choices.emplace_back(choiceName);
                const bool toggle = descriptor.discrete && choices.empty() && descriptor.minimum >= 0
                    && descriptor.minimum < 0.1f && descriptor.maximum > 0.9f && descriptor.maximum <= 1;
                infos.push_back(makeParam(prefix + std::string(descriptor.id), std::string(descriptor.label),
                    descriptor.minimum, descriptor.maximum, next->getParameter(id, static_cast<int>(parameter)),
                    std::string(descriptor.unit), toggle, std::move(choices)));
                bindings.push_back({id, static_cast<int>(parameter), -1});
            }
        }
        std::array<std::vector<float>, WavetableEffects::busCount * 2> buffers;
        std::array<std::vector<float>, 2> subBuffers;
        for (auto& channel : buffers) channel.assign(static_cast<size_t>(fxBlockCapacity_), 0);
        for (auto& channel : subBuffers) channel.assign(static_cast<size_t>(fxBlockCapacity_), 0);
        const int latency = configuration.alignLatency ? next->latencySamples() : 0;
        std::array<dsp::FractionalDelay, 2> subAlignment;
        for (auto& line : subAlignment) line.prepare(latency);
        effects_ = std::move(next); fxInfos_ = std::move(infos); fxBindings_ = std::move(bindings);
        fxBuffers_ = std::move(buffers); directSubBuffers_ = std::move(subBuffers); directSubAlignment_ = std::move(subAlignment);
        directSubLatency_ = latency; effectsReady_ = true; effectsPreparationError_.clear();
        fxReturnBeforeMain_ = configuration.returnBeforeMain;
        reset();
        return true;
    }
    catch (const std::bad_alloc&) { error = "Unable to allocate synth effect-routing storage"; return false; }
    catch (const std::length_error&) { error = "Synth effect-routing storage length exceeded"; return false; }
}
WavetableEffects::Configuration WavetableSynth::effectsConfiguration() const
{
    if (effects_ != nullptr) return effects_->snapshotConfiguration();
    WavetableEffects::Configuration configuration; configuration.mixLaw = WavetableEffects::MixLaw::Shaped;
    return configuration;
}
int WavetableSynth::latencySamples() const
{
    return voiceDownsampler_.integerPipelineDelay()
        + (effectsReady_ && effects_ != nullptr ? effects_->latencySamples() : 0);
}
const std::vector<ParameterInfo>& WavetableSynth::parameters() const
{
    return fxInfos_.empty() ? NativeDevice::parameters() : fxInfos_;
}
float WavetableSynth::getParameter(int index) const
{
    if (index == DirectGain && effects_ != nullptr) return effects_->getRoutingParameter(5);
    if (index < ParameterCount) return NativeDevice::getParameter(index);
    const auto parameter = static_cast<size_t>(index - ParameterCount);
    if (effects_ == nullptr || parameter >= fxBindings_.size()) return 0;
    const auto& binding = fxBindings_[parameter];
    if (binding.routing >= 0) return effects_->getRoutingParameter(binding.routing);
    return binding.parameter < 0 ? (effects_->enabled(binding.module) ? 1.0f : 0.0f) : effects_->getParameter(binding.module, binding.parameter);
}
void WavetableSynth::setParameter(int index, float plainValue)
{
    if (index < ParameterCount)
    {
        NativeDevice::setParameter(index, plainValue);
        if (index == DirectGain && effects_ != nullptr) effects_->setRoutingParameter(5, NativeDevice::getParameter(index));
        return;
    }
    const auto parameter = static_cast<size_t>(index - ParameterCount);
    if (effects_ == nullptr || parameter >= fxBindings_.size() || ! std::isfinite(plainValue)) return;
    const auto& binding = fxBindings_[parameter];
    if (binding.routing >= 0) effects_->setRoutingParameter(binding.routing, plainValue);
    else if (binding.parameter < 0) effects_->setEnabled(binding.module, plainValue >= 0.5f);
    else effects_->setParameter(binding.module, binding.parameter, plainValue);
}
int WavetableSynth::fxRoutingParameterIndex(FxRoutingParam parameter) const
{
    return effects_ != nullptr && parameter >= MainToBus1 && parameter < FxRoutingParams ? ParameterCount + parameter : -1;
}
int WavetableSynth::fxEnabledParameterIndex(WavetableEffects::ModuleId id) const
{
    for (size_t index = FxRoutingParams; index < fxBindings_.size(); ++index)
        if (fxBindings_[index].module == id && fxBindings_[index].parameter < 0) return ParameterCount + static_cast<int>(index);
    return -1;
}
int WavetableSynth::fxParameterIndex(WavetableEffects::ModuleId id, int descriptorIndex) const
{
    if (descriptorIndex < 0) return -1;
    for (size_t index = FxRoutingParams; index < fxBindings_.size(); ++index)
        if (fxBindings_[index].module == id && fxBindings_[index].parameter == descriptorIndex) return ParameterCount + static_cast<int>(index);
    return -1;
}
bool WavetableSynth::setCustomTable(int oscillator, std::shared_ptr<const WavetableData> table)
{
    if (oscillator < 0 || oscillator >= kOscillators) return false;
    customTables_[static_cast<size_t>(oscillator)] = std::move(table);
    if (customTables_[static_cast<size_t>(oscillator)] != nullptr)
    {
        setParameter(oscParam(oscillator, Table), Custom);
        setParameter(mediaParam(oscillator, MediaMode), WavetableMode);
    }
    else if (choice(value(oscParam(oscillator, Table)), kTableFamilies) == Custom) setParameter(oscParam(oscillator, Table), Basic);
    return true;
}
const WavetableData* WavetableSynth::customTable(int oscillator) const
{
    return oscillator >= 0 && oscillator < kOscillators ? customTables_[static_cast<size_t>(oscillator)].get() : nullptr;
}
std::string WavetableSynth::customTableName(int oscillator) const
{
    const auto* table = customTable(oscillator);
    return table != nullptr ? table->name() : "No Custom Table";
}
bool WavetableSynth::setNoiseSample(std::shared_ptr<const OscillatorSample> sample)
{
    // Clear raw references before releasing the prepared owner. Held voices restart
    // this source on their next control update; the rest of their state is retained.
    for (auto& voice : voices_) { voice.noiseSample.reset(); voice.controlCountdown = 0; }
    noiseSample_ = std::move(sample);
    noiseSampleExpected_ = noiseSample_ != nullptr;
    return true;
}
std::string WavetableSynth::noiseSampleName() const
{
    return noiseSample_ != nullptr ? noiseSample_->name() : noiseSampleExpected_ ? "Missing Noise Sample" : "Generated Noise";
}
float WavetableSynth::noiseWaveformAt(float position, int channel) const
{
    if (noiseSample_ == nullptr || ! std::isfinite(position)) return 0;
    const double frame = static_cast<double>(unit(position)) * (noiseSample_->sampleFrames() - 1);
    if (channel < 0) return .5f * (noiseSample_->sampleLinear(frame, 0) + noiseSample_->sampleLinear(frame, 1));
    return noiseSample_->sampleLinear(frame, channel);
}
bool WavetableSynth::setOscillatorMedia(int oscillator, std::shared_ptr<const OscillatorMedia> media)
{
    if (oscillator < 0 || oscillator >= kOscillators) return false;
    // This setter runs before publication or with processing suspended. Clear raw references before
    // releasing an old owner, and reinitialize held notes at the first resumed output sample.
    for (size_t voice = 0; voice < voices_.size(); ++voice)
    {
        (*mediaVoices_)[voice][static_cast<size_t>(oscillator)].reset();
        voices_[voice].oscillator[static_cast<size_t>(oscillator)].mediaMode = -1;
        voices_[voice].controlCountdown = 0;
    }
    oscillatorMedia_[static_cast<size_t>(oscillator)] = std::move(media);
    const auto* asset = oscillatorMedia_[static_cast<size_t>(oscillator)].get();
    setParameter(mediaParam(oscillator, MediaMode), asset == nullptr ? WavetableMode : asset->zones().size() > 1 ? MultisampleMode : SampleMode);
    return true;
}
const OscillatorMedia* WavetableSynth::oscillatorMedia(int oscillator) const
{
    return oscillator >= 0 && oscillator < kOscillators ? oscillatorMedia_[static_cast<size_t>(oscillator)].get() : nullptr;
}
std::string WavetableSynth::oscillatorMediaName(int oscillator) const
{
    const auto* media = oscillatorMedia(oscillator);
    return media != nullptr ? media->name() : "No Media";
}
bool WavetableSynth::setImportedModulationDomain(int destination, ImportedModulationDomain domain)
{
    if (destination <= DestOff || destination >= NumDestinations) return false;
    const bool supported = domain == ImportedModulationDomain::Native
        || (domain == ImportedModulationDomain::CoarsePitch128 && destination >= Osc1Pitch && destination <= Osc3Pitch)
        || (domain == ImportedModulationDomain::SquaredLevel && ((destination >= Osc1Level && destination <= Osc3Level) || destination == NoiseAmplitude || destination == SubAmplitude))
        || (domain == ImportedModulationDomain::Pan2 && ((destination >= Osc1Pan && destination <= Osc3Pan) || destination == NoisePanDest || destination == SubPanDest))
        || (domain == ImportedModulationDomain::FilterFrequency && destination >= Filter1Cutoff && destination <= Filter2Cutoff)
        || (domain == ImportedModulationDomain::SourceSend && destination >= Osc1Bus1Send && destination <= Filter2Bus2Send)
        || (domain == ImportedModulationDomain::Unit && ((destination >= Osc1Position && destination <= Osc3Position)
            || (destination >= Osc1Warp1 && destination <= Osc3Warp2) || (destination >= Filter1Resonance && destination <= Filter2Resonance)
            || (destination >= Env1Attack && destination <= Env4Release && (destination - Env1Attack) % 4 == 2)))
        || (domain == ImportedModulationDomain::EnvelopeTime && destination >= Env1Attack && destination <= Env4Release && (destination - Env1Attack) % 4 != 2)
        || (domain == ImportedModulationDomain::Unit && destination == FxParameter)
        || (domain == ImportedModulationDomain::FineCents200 && destination >= Osc1Fine && destination <= Osc3Fine)
        || (domain == ImportedModulationDomain::DetuneCurve && destination >= Osc1Detune && destination <= Osc3Detune)
        || (domain == ImportedModulationDomain::OctaveSteps && destination >= Osc1Octave && destination <= Osc3Octave)
        || (domain == ImportedModulationDomain::Unit && destination >= Filter1Mix && destination <= Filter2Mix)
        || (domain == ImportedModulationDomain::Unit && ((destination >= Lfo1Rate && destination <= VoiceAmp && destination != MasterTuning)
            || destination == Filter1Var || destination == Filter2Var || (destination >= LfoPointBus1 && destination <= LfoPointBus16)
            || (destination >= Osc1Semi && destination <= Osc3Semi) || (destination >= Source1Balance && destination <= Source5Balance)
            || (destination >= Osc1Blend && destination <= Osc3Width) || destination == GlobalTranspose))
        || (domain == ImportedModulationDomain::CoarsePitch128 && destination == SubCoarse)
        || (domain == ImportedModulationDomain::CoarsePitch128 && destination == MasterTuning);
    if (! supported) return false;
    importedDomains_[static_cast<size_t>(destination)] = domain;
    parametersChanged.store(true, std::memory_order_release);
    for (auto& voice : voices_) voice.controlCountdown = 0;
    return true;
}
void WavetableSynth::setImportedModWheel(float value)
{
    importedModWheel_ = std::isfinite(value) ? std::clamp(value / 100, 0.0f, 1.0f) : 0;
    wheels_.fill(importedModWheel_);
}
void WavetableSynth::setImportedLfoPointAssignments(const std::vector<LfoPointAssignment>& assignments)
{
    lfoPointAssignmentCount_ = 0;
    for (const auto& a : assignments)
        if (lfoPointAssignmentCount_ < kLfoPointAssignments && a.bus >= 0 && a.bus < 16 && a.lfo >= 0 && a.lfo < kLfos
            && a.point >= 0 && a.point < dsp::CurveGraph::kMaxPoints && a.target >= 0 && a.target <= 2)
            lfoPointAssignments_[static_cast<size_t>(lfoPointAssignmentCount_++)] = a;
}
void WavetableSynth::setImportedMediaEnvelope(int oscillator, double attackSeconds, double releaseSeconds)
{
    if (oscillator < 0 || oscillator >= kOscillators) return;
    importedMediaEnvelope_[static_cast<size_t>(oscillator)] = {std::isfinite(attackSeconds) ? attackSeconds : -1, std::isfinite(releaseSeconds) ? releaseSeconds : -1};
}
void WavetableSynth::setImportedFilterCutoffPosition(int filter, float normalized)
{
    if (filter >= 0 && filter < kFilters) importedCutoffPosition_[static_cast<size_t>(filter)] = std::isfinite(normalized) ? unit(normalized) : -1;
}
void WavetableSynth::setImportedLfoSyncDivisions(int lfo, bool dotted, bool triplets)
{
    if (lfo >= 0 && lfo < kLfos) importedLfoSync_[static_cast<size_t>(lfo)] = {dotted, triplets};
}
int WavetableSynth::addImportedFxTarget(WavetableEffects::ModuleId module, float base, std::vector<FxModulationCurve> curves)
{
    if (! std::isfinite(base) || curves.empty()) return -1;
    for (const auto& curve : curves)
        if (curve.parameter < 0 || curve.values.size() < 2
            || std::any_of(curve.values.begin(), curve.values.end(), [](float v) { return ! std::isfinite(v); })) return -1;
    fxTargets_.push_back({module, unit(base), -1, std::move(curves)});
    return static_cast<int>(fxTargets_.size()) - 1;
}
bool WavetableSynth::setImportedFxSlot(int slot, int target)
{
    if (slot < 0 || slot >= kModSlots || target < -1 || target >= static_cast<int>(fxTargets_.size())) return false;
    fxSlotTargets_[static_cast<size_t>(slot)] = target;
    return true;
}
void WavetableSynth::selectFxVoice()
{
    // FX controls are global; per-voice sources come from one selected voice (measured with Env 2 -> Utility output):
    // the most recently started held voice, else the releasing one that was selected. Each voice keeps its own FX
    // modulation state, which only runs while that voice is selected: a note that loses the focus to a newer one is
    // frozen and resumes from there when the newer note is released first.
    int chosen = -1;
    for (int index = 0; index < kVoices; ++index)
    {
        const auto& voice = voices_[static_cast<size_t>(index)];
        if (voice.note >= 0 && ! voice.released && (chosen < 0 || voice.age > voices_[static_cast<size_t>(chosen)].age)) chosen = index;
    }
    if (chosen < 0 && fxVoiceSlot_ >= 0 && voices_[static_cast<size_t>(fxVoiceSlot_)].note >= 0
        && voices_[static_cast<size_t>(fxVoiceSlot_)].fxStarted) chosen = fxVoiceSlot_;
    if (chosen != fxVoiceSlot_ || (chosen >= 0 && ! voices_[static_cast<size_t>(chosen)].fxStarted))
    {
        if (fxVoiceSlot_ >= 0)
        {
            auto& previous = voices_[static_cast<size_t>(fxVoiceSlot_)];
            if (previous.note >= 0 && previous.fxStarted)
            {
                previous.fxEnvelope = fxVoice_.envelope; previous.fxLfoPhase = fxVoice_.lfoPhase; previous.fxNoteSamples = fxVoice_.noteSamples;
            }
        }
        if (chosen < 0) fxVoice_.released = true; // the focused note ended: its modulation keeps releasing (measured in the tails)
        else
        {
            fxVoice_ = Voice{};
            auto& voice = voices_[static_cast<size_t>(chosen)];
            if (! voice.fxStarted)
            {
                voice.fxEnvelope = voice.envelope; voice.fxLfoPhase = voice.lfoPhase; voice.fxNoteSamples = voice.noteSamples;
                voice.fxStarted = true;
            }
            fxVoice_.note = voice.note; fxVoice_.channel = voice.channel; fxVoice_.age = voice.age;
            fxVoice_.velocity = voice.velocity; fxVoice_.pressure = voice.pressure;
            fxVoice_.random = voice.random; fxVoice_.random2 = voice.random2; fxVoice_.alternate = voice.alternate;
            fxVoice_.envelopeStages = voice.envelopeStages; fxVoice_.envelopeStagesModulated = voice.envelopeStagesModulated;
            fxVoice_.envelope = voice.fxEnvelope; fxVoice_.lfoPhase = voice.fxLfoPhase; fxVoice_.noteSamples = voice.fxNoteSamples;
            fxDiscreteSlot_ = chosen;
        }
        fxVoiceSlot_ = chosen;
    }
    if (chosen >= 0) fxVoice_.released = voices_[static_cast<size_t>(chosen)].released;
}
void WavetableSynth::applyFxModulation(int frames)
{
    // Evaluated at the start of each FX sub-block, then the focused voice's modulation state runs for `frames` host samples.
    if (fxVoice_.note >= 0) updateLfoValues(fxVoice_);
    for (size_t index = 0; index < fxTargets_.size(); ++index)
    {
        auto& target = fxTargets_[index];
        float position = target.base;
        for (int i = 0; i < routeCount_; ++i)
        {
            const auto& route = routes_[static_cast<size_t>(i)];
            if (route.destination == FxParameter && fxSlotTargets_[static_cast<size_t>(route.slot)] == static_cast<int>(index))
                position += routeValue(fxVoice_, route) * route.amount;
        }
        position = unit(position);
        if (std::abs(position - target.applied) < 1e-7f) continue; // unchanged since the previous write
        target.applied = position;
        for (const auto& curve : target.curves)
        {
            const float scaled = position * static_cast<float>(curve.values.size() - 1);
            const size_t at = std::min(static_cast<size_t>(scaled), curve.values.size() - 2);
            const float fraction = scaled - static_cast<float>(at);
            effects_->setParameter(target.module, curve.parameter, curve.values[at] + (curve.values[at + 1] - curve.values[at]) * fraction);
        }
    }
    if (fxVoice_.note < 0) return;
    for (int sample = 0; sample < frames * voiceRateFactor_; ++sample)
    {
        for (int e = 0; e < kEnvelopes; ++e)
            nextEnvelope(fxVoice_.envelope[static_cast<size_t>(e)], e, fxVoice_.released,
                         fxVoice_.envelopeStagesModulated ? &fxVoice_.envelopeStages[static_cast<size_t>(e)] : nullptr);
        for (int l = 0; l < kLfos; ++l)
        {
            auto& phase = fxVoice_.lfoPhase[static_cast<size_t>(l)];
            phase += lfoIncrement_[static_cast<size_t>(l)];
            phase = choice(block_[static_cast<size_t>(lfoParam(l, Mode))], 3) == OneShot ? std::min(1.0, phase) : wrap(phase);
        }
        ++fxVoice_.noteSamples;
    }
}
void WavetableSynth::setImportedEnvelopeProfile(bool enabled)
{
    importedEnvelopeProfile_ = enabled;
    parametersChanged.store(true, std::memory_order_relaxed);
    for (auto& voice : voices_) voice.controlCountdown = 0;
}
void WavetableSynth::setImportedLfoProfile(bool enabled)
{
    importedLfoProfile_ = enabled;
    parametersChanged.store(true, std::memory_order_relaxed);
    for (auto& voice : voices_) voice.controlCountdown = 0;
}
void WavetableSynth::setImportedSubProfile(bool enabled)
{
    importedSubProfile_ = enabled;
    // 1.00316: the read gain every reference table carries (measured on the Sub level and its PD/FM source depth).
    importedSubSineScale_ = enabled ? -dsp::dbToGain(5.9f) / 3 * 1.00316f : 1;
}
bool WavetableSynth::setVoiceRateFactor(int factor)
{
    if (factor != 1 && factor != 2) return false;
    requestedVoiceRateFactor_ = factor;
    return true;
}
float WavetableSynth::envelopeCurveAt(int envelope, EnvParam curve, float phase) const
{
    if (envelope < 0 || envelope >= kEnvelopes || curve < AttackCurve || curve > ReleaseCurve) return unit(phase);
    const float tension = getParameter(envParam(envelope, curve));
    return importedEnvelopeProfile_ ? dsp::ExponentialEnvelopeCurve::fromTension(tension).eval(phase)
                                    : dsp::CurveGraph::shape(phase, tension);
}
bool WavetableSynth::setImportedMasterLevel(double storedLevel)
{
    if (! std::isfinite(storedLevel) || storedLevel < 0 || storedLevel > 1) return false;
    const double amplitude = storedLevel * std::sqrt(2.0);
    const double db = amplitude > 0 ? 20 * std::log10(amplitude) : -60;
    importedMasterDb_ = static_cast<float>(std::clamp(db, -60.0, 12.0));
    importedMasterCorrection_ = static_cast<float>(amplitude / dsp::dbToGain(importedMasterDb_));
    parametersChanged.store(true, std::memory_order_relaxed);
    for (auto& voice : voices_) voice.controlCountdown = 0;
    return true;
}
float WavetableSynth::importedDetuneCents(float detune, float modulation, float scale)
{
    if (std::abs(modulation) > 0)
    {
        const float position = unit(std::sqrt(std::pow(std::max(0.0f, detune) / 200, 0.8f)) + modulation);
        detune = 200 * std::pow(position * position, 1.25f);
    }
    return detune * scale;
}
float WavetableSynth::masterGainFor(float db) const
{
    return dsp::dbToGain(db) * (std::abs(db - importedMasterDb_) < 1e-5f ? importedMasterCorrection_ : 1);
}
bool WavetableSynth::setLfoCurve(int lfo, const dsp::CurveGraph& curve)
{
    if (lfo < 0 || lfo >= kLfos || curve.count < 2 || curve.count > dsp::CurveGraph::kMaxPoints) return false;
    float previousX = 0;
    for (int i = 0; i < curve.count; ++i)
    {
        if (! std::isfinite(curve.x[i]) || ! std::isfinite(curve.y[i]) || ! std::isfinite(curve.curve[i])
            || curve.x[i] < previousX || curve.x[i] > 1 || curve.y[i] < 0 || curve.y[i] > 1 || std::abs(curve.curve[i]) > 1) return false;
        previousX = curve.x[i];
    }
    if (std::abs(curve.x[0]) > 1e-6f || std::abs(curve.x[curve.count - 1] - 1) > 1e-6f) return false;
    customCurves_[static_cast<size_t>(lfo)] = curve;
    hasCustomCurves_[static_cast<size_t>(lfo)] = true;
    setParameter(lfoParam(lfo, Shape), CustomLfoShape);
    return true;
}
void WavetableSynth::clearLfoCurve(int lfo)
{
    if (lfo >= 0 && lfo < kLfos)
    {
        hasCustomCurves_[static_cast<size_t>(lfo)] = false;
        if (choice(value(lfoParam(lfo, Shape)), CustomLfoShape + 1) == CustomLfoShape) setParameter(lfoParam(lfo, Shape), 1);
    }
}
bool WavetableSynth::hasCustomLfoCurve(int lfo) const
{
    return lfo >= 0 && lfo < kLfos && hasCustomCurves_[static_cast<size_t>(lfo)];
}
dsp::CurveGraph WavetableSynth::lfoCurve(int lfo) const
{
    if (lfo < 0 || lfo >= kLfos) return dsp::CurveGraph::preset(0);
    if (hasCustomCurves_[static_cast<size_t>(lfo)] && choice(value(lfoParam(lfo, Shape)), CustomLfoShape + 1) == CustomLfoShape)
        return customCurves_[static_cast<size_t>(lfo)];
    return dsp::CurveGraph::preset(choice(value(lfoParam(lfo, Shape)), dsp::CurveGraph::kShapeCount));
}
bool WavetableSynth::setModulationCurve(int slot, bool auxiliary, const dsp::CurveGraph& graph, bool enabled, CurveLaw law)
{
    if (slot < 0 || slot >= kModSlots || graph.count < 2 || graph.count > dsp::CurveGraph::kMaxPoints
        || (law != CurveLaw::NativePower && law != CurveLaw::ImportedExponential)) return false;
    if (std::abs(graph.x[0]) > 1e-6f || std::abs(graph.x[graph.count - 1] - 1) > 1e-6f) return false;
    for (int i = 0; i < graph.count; ++i)
        if (! std::isfinite(graph.x[i]) || ! std::isfinite(graph.y[i]) || ! std::isfinite(graph.curve[i])
            || graph.x[i] < 0 || graph.x[i] > 1 || graph.y[i] < 0 || graph.y[i] > 1 || std::abs(graph.curve[i]) > 1
            || (i > 0 && graph.x[i] < graph.x[i - 1])) return false;
    const auto row = static_cast<size_t>(slot), column = static_cast<size_t>(auxiliary ? 1 : 0);
    modulationCurves_[row][column] = {graph, enabled, law};
    exponentialModulationCurves_[row][column].prepare(graph);
    for (auto& voice : voices_) voice.controlCountdown = 0;
    return true;
}
WavetableSynth::ModulationCurve WavetableSynth::modulationCurve(int slot, bool auxiliary) const
{
    return slot >= 0 && slot < kModSlots ? modulationCurves_[static_cast<size_t>(slot)][static_cast<size_t>(auxiliary ? 1 : 0)] : ModulationCurve{};
}
float WavetableSynth::modulationCurveAt(int slot, bool auxiliary, float source) const
{
    if (slot < 0 || slot >= kModSlots) return source;
    const auto row = static_cast<size_t>(slot), column = static_cast<size_t>(auxiliary ? 1 : 0);
    const auto& curve = modulationCurves_[row][column];
    const float shaped = ! curve.enabled ? source
        : curve.law == CurveLaw::ImportedExponential ? exponentialModulationCurves_[row][column].eval(source) : curve.graph.eval(source);
    return auxiliary && auxiliaryInverted_[row].load(std::memory_order_relaxed) ? 1 - shaped : shaped;
}
void WavetableSynth::setAuxiliaryInverted(int slot, bool inverted)
{
    if (slot < 0 || slot >= kModSlots) return;
    auxiliaryInverted_[static_cast<size_t>(slot)].store(inverted, std::memory_order_relaxed);
    for (auto& voice : voices_) voice.controlCountdown = 0;
}
bool WavetableSynth::auxiliaryInverted(int slot) const
{
    return slot >= 0 && slot < kModSlots && auxiliaryInverted_[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
}
void WavetableSynth::snapshot()
{
    for (int i = 0; i < ParameterCount; ++i) block_[static_cast<size_t>(i)] = value(i);
    block_[DirectGain] = getParameter(DirectGain);
    for (int source = 0; source < kRoutingSources; ++source)
    {
        auto& route = sourceRoutes_[static_cast<size_t>(source)];
        route.destination = choice(block_[static_cast<size_t>(sourceRoutingParam(source, SourceDestination))], 4);
        route.filter2 = unit((block_[static_cast<size_t>(sourceRoutingParam(source, SourceFilterBalance))] + 1) * .5f);
        route.filter1 = 1 - route.filter2;
        route.envelope = block_[static_cast<size_t>(sourceRoutingParam(source, SourceEnvelope))] >= .5f;
    }
    polyphony_ = std::clamp(static_cast<int>(std::lround(block_[static_cast<size_t>(PolyphonyParam)])), 1, kVoices);
    const bool mono = block_[voiceModeParam(MonoOn)] >= 0.5f;
    if (mono != monoActive_)
    {
        voices_ = {}; monoVoice_.reset();
        for (auto& row : *mediaVoices_) for (auto& media : row) media.reset();
        monoActive_ = mono;
    }
    WavetableVoiceMode::Settings voiceSettings;
    voiceSettings.legato = block_[voiceModeParam(LegatoOn)] >= 0.5f;
    voiceSettings.portaAlways = block_[voiceModeParam(PortaAlways)] >= 0.5f;
    voiceSettings.portaScaled = block_[voiceModeParam(PortaScaled)] >= 0.5f;
    voiceSettings.portaSeconds = block_[voiceModeParam(PortaTime)];
    voiceSettings.portaCurve = block_[voiceModeParam(PortaCurve)];
    voiceSettings.priority = static_cast<WavetableVoiceMode::Priority>(choice(block_[voiceModeParam(NotePriority)], 3));
    voiceSettings.deduplicate = importedVoiceProfile_;
    voiceSettings.globalSustain = importedVoiceProfile_;
    voiceSettings.exponentialPortamento = importedVoiceProfile_;
    const auto voiceTransition = monoVoice_.configure(voiceSettings);
    if (monoActive_) applyMonoTransition(voiceTransition);
    if (importedEnvelopeProfile_)
        for (int envelope = 0; envelope < kEnvelopes; ++envelope)
            for (int curve = 0; curve < 3; ++curve)
                importedEnvelopeCurves_[static_cast<size_t>(envelope)][static_cast<size_t>(curve)]
                    = dsp::ExponentialEnvelopeCurve::fromTension(block_[static_cast<size_t>(envParam(envelope, static_cast<EnvParam>(AttackCurve + curve)))]);
    routeCount_ = 0;
    for (int i = 0; i < kModSlots; ++i)
    {
        ModRoute route;
        route.slot = i;
        route.source = choice(block_[static_cast<size_t>(modParam(i, ModSource))], NumSources);
        route.destination = choice(block_[static_cast<size_t>(modParam(i, ModDestination))], NumDestinations);
        route.amount = block_[static_cast<size_t>(modParam(i, ModAmount))];
        route.output = block_[static_cast<size_t>(outParam(i))];
        route.auxiliary = choice(block_[static_cast<size_t>(modParam(i, AuxSource))], NumSources);
        route.bipolar = block_[static_cast<size_t>(modParam(i, ModBipolar))] >= 0.5f;
        if (route.source != SourceOff && route.destination != DestOff && std::abs(route.amount) > 1e-7f)
            routes_[static_cast<size_t>(routeCount_++)] = route;
    }
    for (int l = 0; l < kLfos; ++l)
    {
        curves_[static_cast<size_t>(l)] = hasCustomCurves_[static_cast<size_t>(l)] && choice(block_[static_cast<size_t>(lfoParam(l, Shape))], CustomLfoShape + 1) == CustomLfoShape
            ? customCurves_[static_cast<size_t>(l)]
            : dsp::CurveGraph::preset(choice(block_[static_cast<size_t>(lfoParam(l, Shape))], dsp::CurveGraph::kShapeCount));
        if (importedLfoProfile_) importedLfoCurves_[static_cast<size_t>(l)].prepare(curves_[static_cast<size_t>(l)]);
        const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
        lfoIncrement_[static_cast<size_t>(l)] = block_[static_cast<size_t>(lfoParam(l, SyncOn))] >= 0.5f
            ? tempo / (60 * sampleRate_ * divisionBeats(choice(block_[static_cast<size_t>(lfoParam(l, Division))], kLfoDivisions)))
            : block_[static_cast<size_t>(lfoParam(l, Rate))] * (block_[static_cast<size_t>(lfoRateScaleParam(l))] >= 0.5f ? 10 : 1) / sampleRate_;
    }
    parametersChanged.store(false, std::memory_order_relaxed);
}

float WavetableSynth::randomSigned(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 8388608.0f - 1;
}
// Measured reference note-off bookkeeping: nothing happens without a held voice of the pitch. Otherwise, for each held voice, every alive voice
// of the pitch (held, releasing or a same-note ghost) is released with 3 draws; with no Env 1 release time the released voices end at once,
// so each is released once. Audibly only the held voices change (they all enter their release together).
void WavetableSynth::addSameNoteGhost(int note, int channel)
{
    auto* ghost = &ghosts_[0];
    for (auto& g : ghosts_) if (g.note < 0) { ghost = &g; break; }
    *ghost = {note, channel};
}
void WavetableSynth::importedNoteOff(int note, int channel, bool pedalUp)
{
    // Under the sustain pedal a note-off only lifts the key (no draws); the release and its draws happen at pedal-up, where the sustained
    // voices of the pitch count as the held ones (measured).
    const bool pedalDown = pedals_[static_cast<size_t>(channel)];
    if (! pedalUp && pedalDown)
    {
        bool lifted = false;
        for (auto& voice : voices_)
            if (voice.note == note && voice.channel == channel && voice.keyDown) { voice.keyDown = false; lifted = true; }
        // The ghosts of the pitch start their release here (measured), though the sustained voices keep sounding.
        if (lifted)
            for (auto& ghost : ghosts_)
                if (ghost.note == note && ghost.channel == channel) ghost = {};
        return;
    }
    const auto isHeld = [pedalUp](const Voice& voice) { return pedalUp ? (! voice.keyDown && ! voice.released) : voice.keyDown; };
    const float releaseSeconds = block_[static_cast<size_t>(envParam(0, Release))];
    int held = 0, alive = 0;
    for (const auto& voice : voices_)
        if (voice.note == note && voice.channel == channel)
        {
            if (voice.counted) { ++alive; if (isHeld(voice)) ++held; }
            else if (voice.cutHeld && ! pedalUp) { ++alive; if (voice.cutReleasedAt < 0) ++held; } // still fading from a same-note cut of a held voice
            else if (! pedalUp) ++alive; // still fading from a same-note cut of a releasing voice
        }
    if (held == 0) return;
    if (pedalUp)
    {
        // Pedal-up releases only the sustained voices themselves: 3 draws each, ghosts and releasing voices are not counted (measured).
        serumRandom_.discard(static_cast<unsigned long long>(kSerumReleaseDraws) * static_cast<unsigned long long>(held));
        for (auto& voice : voices_)
            if (voice.note == note && voice.channel == channel && isHeld(voice)) { voice.keyDown = false; voice.released = true; }
        return;
    }
    for (const auto& ghost : ghosts_)
        if (ghost.note == note && ghost.channel == channel) ++alive; // released ghosts are removed (measured)
    serumRandom_.discard(static_cast<unsigned long long>(kSerumReleaseDraws) * static_cast<unsigned long long>(releaseSeconds > 0 ? held * alive : alive));
    for (auto& voice : voices_)
        if (voice.note == note && voice.channel == channel)
        {
            if (voice.counted && isHeld(voice)) { voice.keyDown = false; voice.released = true; }
            else if (voice.cutHeld && voice.cutReleasedAt < 0) voice.cutReleasedAt = static_cast<int64_t>(sampleClock_);
        }
    for (auto& ghost : ghosts_)
        if (ghost.note == note && ghost.channel == channel) ghost = {};
}
void WavetableSynth::release(Voice& voice)
{
    // Reference voice release: each oscillator's multisample engine draws once.
    if (importedVoiceProfile_) serumRandom_.discard(kSerumReleaseDraws);
    voice.keyDown = false; voice.released = ! pedals_[static_cast<size_t>(voice.channel)];
}
void WavetableSynth::startVoice(Voice& voice, int note, int channel, int velocity, bool monoRetrigger)
{
    voice = Voice{};
    voice.note = note; voice.pitchNote = note; voice.channel = channel; voice.keyDown = true;
    voice.velocity = static_cast<float>(velocity) / 127; voice.midiVelocity = velocity;
    voice.pressure = pressures_[static_cast<size_t>(channel)]; voice.age = ++age_;
    voice.random = 0.5f + 0.5f * randomSigned(randomSeed_);
    voice.noiseSeed = randomSeed_ ^ static_cast<uint32_t>(age_ * 0x9e3779b9u);
    voice.alternate = alternateState_ ? 1.0f : 0.0f;
    alternateState_ = ! alternateState_;
    if (lastStartedVoice_ >= 0) discreteRandom_[static_cast<size_t>(lastStartedVoice_)] = pendingDiscreteRandom_;
    lastStartedVoice_ = static_cast<int>(&voice - voices_.data());
    for (auto& envelope : voice.envelope) envelope.stage = 1;
    if (importedLfoProfile_)
        for (int lfo = 0; lfo < kLfos; ++lfo)
            if (choice(block_[static_cast<size_t>(lfoParam(lfo, Mode))], 3) == Global)
            {
                const auto at = static_cast<size_t>(lfo);
                const bool synced = block_[static_cast<size_t>(lfoParam(lfo, SyncOn))] >= 0.5f;
                const bool anchored = block_[static_cast<size_t>(lfoAnchorParam(lfo))] >= 0.5f;
                const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
                const double beats = std::isfinite(host_.beats) ? host_.beats : 0;
                const double phase = synced ? beats / divisionBeats(choice(block_[static_cast<size_t>(lfoParam(lfo, Division))], kLfoDivisions))
                    : anchored ? beats * 60 / tempo * lfoIncrement_[at] * sampleRate_ : processingEndLfoPhase_[at];
                voice.lfoPhase[at] = wrap(phase);
            }
    if (importedVoiceProfile_)
    {
        // Measured reference note start (black-box draw indices): one shared draw is unison slot 0 of every
        // oscillator, 66 note-random modulation draws follow, then per oscillator (enabled or not) 16 reset draws, slots 1..15 and 80 media
        // draws; then the Noise draw and 3 more. Slot k is the k-th voice by pitch; phase = frac(Phase + u Random^2).
        const float shared = serumUniform();
        // Then NoteOn Rand1 and Rand2, then one NoteOn Rand (Discrete) value per matrix slot (published at the next note start).
        // A note start consumes 404 draws in all; every voice release 3 more.
        voice.random = serumUniform();
        voice.random2 = serumUniform();
        for (auto& value : pendingDiscreteRandom_) value = serumUniform();
        if (monoRetrigger)
        {
            // Measured mono retrigger (legato off, voice sounding): the same note-random draws, then 51 more and no oscillator/Noise
            // restart (the caller keeps the running phases); 118 draws in all.
            static_cast<void>(shared);
            serumRandom_.discard(kSerumMonoRetriggerTailDraws);
        }
        else
        {
        for (int o = 0; o < kOscillators; ++o)
        {
            serumRandom_.discard(kSerumOscResetDraws);
            const double initial = block_[static_cast<size_t>(oscParam(o, Phase))];
            const double random = block_[static_cast<size_t>(oscParam(o, Random))];
            auto& phases = voice.phase[static_cast<size_t>(o)];
            phases[0] = wrap(initial + static_cast<double>(shared) * random * random);
            for (size_t u = 1; u < phases.size(); ++u) phases[u] = wrap(initial + static_cast<double>(serumUniform()) * random * random);
            serumRandom_.discard(kSerumOscMediaDraws);
        }
        voice.serumNoiseRandom = static_cast<double>(serumRandom_()) * 5.421010862427522e-20;
        serumRandom_.discard(kSerumNoteTailDraws);
        }
    }
    else
    {
        voice.random2 = 0.5f + 0.5f * randomSigned(randomSeed_);
        for (auto& value : pendingDiscreteRandom_) value = 0.5f + 0.5f * randomSigned(randomSeed_);
        for (int o = 0; o < kOscillators; ++o)
            for (int u = 0; u < kUnison; ++u)
                voice.phase[static_cast<size_t>(o)][static_cast<size_t>(u)] = wrap(block_[static_cast<size_t>(oscParam(o, Phase))]
                    + (0.5f + 0.5f * randomSigned(randomSeed_)) * block_[static_cast<size_t>(oscParam(o, Random))]);
    }
    voice.subPhase = block_[SubContinuous] >= .5f ? retainedSubPhase_ : wrap(block_[SubPhase]);
}
void WavetableSynth::applyMonoTransition(const WavetableVoiceMode::Transition& transition)
{
    if (! transition.changed) return;
    auto& voice = voices_[0];
    if (transition.release)
    {
        if (importedVoiceProfile_ && voice.note >= 0 && voice.keyDown) serumRandom_.discard(kSerumReleaseDraws);
        voice.keyDown = false; voice.released = true; return;
    }
    if (! transition.after.active()) return;
    // Measured mono legato change (either direction, also a repeated pitch): 4 draws, and the pending NoteOn Rand (Discrete) values
    // are published to the voice (no new ones are drawn; Rand1/2 and Alt. stay).
    if (importedVoiceProfile_ && transition.legato && voice.note >= 0)
    {
        serumRandom_.discard(kSerumLegatoDraws);
        if (lastStartedVoice_ >= 0) discreteRandom_[static_cast<size_t>(lastStartedVoice_)] = pendingDiscreteRandom_;
        lastStartedVoice_ = static_cast<int>(&voice - voices_.data());
    }
    if (transition.retrigger || voice.note < 0)
    {
        const auto previousEnvelopes = voice.envelope;
        const auto previousPhases = voice.phase;
        const double previousSubPhase = voice.subPhase;
        const auto previousNoise = voice.noiseSample;
        // Measured: any still-sounding mono voice (held or releasing) is retriggered in place.
        const bool continuing = importedVoiceProfile_ && voice.note >= 0;
        startVoice(voice, transition.after.note, transition.after.channel, transition.after.velocity, continuing);
        if (continuing)
        {
            for (int envelope = 0; envelope < kEnvelopes; ++envelope)
                voice.envelope[static_cast<size_t>(envelope)].attackStart = previousEnvelopes[static_cast<size_t>(envelope)].level;
            // Measured: a mono retrigger keeps the running oscillator phases (no restart).
            voice.phase = previousPhases;
            voice.subPhase = previousSubPhase;
            voice.noiseSample = previousNoise;
        }
    }
    voice.note = transition.after.note; voice.channel = transition.after.channel;
    voice.keyDown = transition.after.keyDown; voice.released = false;
    voice.velocity = static_cast<float>(transition.after.velocity) / 127; voice.midiVelocity = transition.after.velocity;
    voice.pressure = pressures_[static_cast<size_t>(voice.channel)];
    voice.pitchNote = monoVoice_.currentPitch(); voice.controlCountdown = 0;
}
void WavetableSynth::event(const uint8_t* data, int size)
{
    if (size < 1 || size > 3 || data == nullptr) return;
    const uint8_t status = data[0];
    if (status == 0xff) { const bool bypass = wasBypassed_; reset(); wasBypassed_ = bypass; return; }
    if (status < 0x80 || status >= 0xf0) return;
    const size_t channel = status & 15u;
    const uint8_t kind = status & 0xf0u;
    if (size < 2 || data[1] >= 128 || ((kind != 0xc0 && kind != 0xd0) && (size != 3 || data[2] >= 128))) return;
    if (kind == 0x90 && data[2] > 0)
    {
        if (wasBypassed_) return;
        if (monoActive_)
        {
            applyMonoTransition(monoVoice_.noteOn(static_cast<int>(channel), data[1], data[2]));
            return;
        }
        Voice* voice = nullptr;
        // Measured: a pitch still sustained by the pedal (key up, not released) is restarted in its own voice: the old sound stops and the
        // new note starts fresh there, with no release draws.
        // With the pedal down any sounding voice of the pitch (held, sustained or releasing) is restarted, whatever the same-note limit.
        // A reused voice restarts fresh when it is the only sounding voice; otherwise it is retriggered in place (phases kept) like a
        // steal (measured).
        bool stolen = false, reused = false;
        if (importedVoiceProfile_)
            for (auto& v : voices_)
                if (v.note == data[1] && v.channel == static_cast<int>(channel)
                    && ((! v.keyDown && ! v.released && v.counted) || pedals_[channel]))
                {
                    voice = &v; reused = true;
                    stolen = std::any_of(voices_.begin(), voices_.end(), [&v](const Voice& o) { return &o != &v && o.note >= 0 && o.counted; });
                    // Reusing a held voice leaves a ghost of it for the note-off bookkeeping, like a same-note cut (measured).
                    if (v.keyDown && block_[static_cast<size_t>(envParam(0, Release))] > 0) addSameNoteGhost(v.note, v.channel);
                    break;
                }
        // Measured reference polyphony (Global kParamPolyCount, default 8): at the limit the oldest releasing voice, else the oldest held one,
        // is restarted in place like a mono retrigger (phases kept, envelopes re-attack from their level, 118 draws).
        if (voice == nullptr && importedVoiceProfile_)
        {
            // Every sounding voice occupies a slot, including one still fading from a same-note cut (measured).
            int active = 0; Voice* victim = nullptr;
            for (auto& v : voices_)
                if (v.note >= 0)
                {
                    ++active;
                    if (victim == nullptr || (v.released && ! victim->released) || (v.released == victim->released && v.age < victim->age)) victim = &v;
                }
            if (active >= polyphony_ && victim != nullptr) { voice = victim; stolen = true; }
        }
        if (voice == nullptr && ! importedVoiceProfile_ && polyphony_ < kVoices)
        {
            // Own patches at the POLY limit restart the oldest voice, releasing ones first.
            int active = 0; Voice* victim = nullptr;
            for (auto& v : voices_)
                if (v.note >= 0)
                {
                    ++active;
                    if (victim == nullptr || (v.released && ! victim->released) || (v.released == victim->released && v.age < victim->age)) victim = &v;
                }
            if (active >= polyphony_) voice = victim;
        }
        if (voice == nullptr)
            for (auto& v : voices_) if (v.note < 0) { voice = &v; break; }
        if (voice == nullptr)
        {
            voice = &voices_[0];
            for (auto& v : voices_)
                if ((v.released && ! voice->released) || (v.released == voice->released && v.age < voice->age)) voice = &v;
        }
        // A voice still fading from a cut of a held note leaves its ghost when its slot is taken over (measured).
        if (voice->cutHeld && voice->cutReleasedAt < 0) addSameNoteGhost(voice->note, voice->channel);
        if (stolen)
        {
            const auto phases = voice->phase; const double sub = voice->subPhase; const auto envelopes = voice->envelope; const auto noise = voice->noiseSample;
            const auto age = voice->age;
            startVoice(*voice, data[1], static_cast<int>(channel), data[2], true);
            voice->phase = phases; voice->subPhase = sub; voice->noiseSample = noise;
            if (reused) voice->age = age; // a same-pitch reuse keeps the voice's original start for later steal choices; a steal does not (measured)
            for (int e = 0; e < kEnvelopes; ++e)
            {
                auto& envelope = voice->envelope[static_cast<size_t>(e)];
                envelope.attackStart = envelopes[static_cast<size_t>(e)].level;
                envelope.releaseTime = envelopes[static_cast<size_t>(e)].releaseTime; // a cut's short release survives the retrigger (measured)
            }
        }
        else startVoice(*voice, data[1], static_cast<int>(channel), data[2]);
        // Measured same-note limit (kParamLimitSameNotePolyphony on): the other sounding voices of this note are cut by a 20 ms release with
        // the envelopes' own release curves (no draws); the new note is an ordinary fresh voice. Without the limit they keep sounding.
        if (importedVoiceProfile_ && importedSameNoteLimit_)
            for (auto& other : voices_)
                if (&other != voice && other.note == data[1] && other.channel == static_cast<int>(channel))
                {
                    // The cut draws nothing. A voice cut while held still counts as a held voice of its pitch during the fade and then lives on
                    // as a silent ghost when Env 1 has a release time (measured).
                    if (other.keyDown && block_[static_cast<size_t>(envParam(0, Release))] > 0) { other.cutHeld = true; other.cutReleasedAt = -1; }
                    other.counted = false;
                    other.keyDown = false; other.released = true;
                    // A releasing envelope keeps its progress p and finishes at the rate 1 / (time already released + 20 ms) (measured).
                    for (auto& envelope : other.envelope)
                    {
                        if (envelope.stage == 6)
                        {
                            const float duration = envelope.releaseTime >= 0 ? envelope.releaseTime
                                : block_[static_cast<size_t>(envParam(static_cast<int>(&envelope - other.envelope.data()), Release))];
                            const auto released = static_cast<float>(envelope.elapsed); // since the release (or its last re-timing) began
                            envelope.releaseOffset = duration > 0 ? std::min(1.0f, envelope.releaseOffset + released / duration) : 1.0f;
                            envelope.releaseTime = released + kSameNoteFadeSeconds;
                            envelope.elapsed = 0;
                        }
                        else if (envelope.stage > 0)
                        { envelope.stage = 6; envelope.elapsed = 0; envelope.start = envelope.level; envelope.releaseOffset = 0; envelope.releaseTime = kSameNoteFadeSeconds; }
                    }
                }
        return;
    }
    if (kind == 0x80 || (kind == 0x90 && data[2] == 0))
    {
        if (monoActive_)
        {
            applyMonoTransition(monoVoice_.noteOff(static_cast<int>(channel), data[1]));
            return;
        }
        if (importedVoiceProfile_) { importedNoteOff(data[1], static_cast<int>(channel)); return; }
        // Own patches release the oldest held voice.
        Voice* found = nullptr;
        for (auto& voice : voices_)
            if (voice.note == data[1] && voice.channel == static_cast<int>(channel) && voice.keyDown && (found == nullptr || voice.age < found->age)) found = &voice;
        if (found != nullptr) release(*found);
        return;
    }
    if (kind == 0xe0)
    {
        const int bend = (static_cast<int>(data[2]) << 7) + data[1] - 8192;
        bends_[channel] = static_cast<float>(bend) / (bend < 0 ? 8192.0f : 8191.0f);
        if (monoActive_ && importedVoiceProfile_) bends_.fill(bends_[channel]);
    }
    else if (kind == 0xd0)
    {
        pressures_[channel] = static_cast<float>(data[1]) / 127;
        for (auto& voice : voices_) if (voice.channel == static_cast<int>(channel)) voice.pressure = pressures_[channel];
    }
    else if (kind == 0xa0)
    {
        for (auto& voice : voices_) if (voice.channel == static_cast<int>(channel) && voice.note == data[1]) voice.pressure = static_cast<float>(data[2]) / 127;
    }
    else if (kind == 0xb0)
    {
        const int controller = data[1];
        if (controller == 1) wheels_[channel] = static_cast<float>(data[2]) / 127;
        else if (controller == 64)
        {
            if (monoActive_) applyMonoTransition(monoVoice_.sustain(static_cast<int>(channel), data[2] >= 64));
            const bool wasDown = pedals_[channel];
            pedals_[channel] = data[2] >= 64;
            if (! monoActive_ && ! pedals_[channel] && wasDown && importedVoiceProfile_)
            {
                // Release every sustained pitch with the note-off bookkeeping (in voice order).
                for (auto& voice : voices_)
                    if (voice.channel == static_cast<int>(channel) && voice.note >= 0 && ! voice.keyDown && ! voice.released)
                        importedNoteOff(voice.note, static_cast<int>(channel), true);
            }
            else if (! monoActive_ && ! pedals_[channel])
                for (auto& voice : voices_) if (voice.channel == static_cast<int>(channel) && ! voice.keyDown) voice.released = true;
        }
        else if (controller == 120)
        {
            if (monoActive_) applyMonoTransition(monoVoice_.allSoundOff(static_cast<int>(channel)));
            for (auto& voice : voices_) if (voice.channel == static_cast<int>(channel)) voice = Voice{};
            // Shared effect tails cannot be separated by MIDI channel. Clear them once no source voice remains.
            if (std::none_of(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.note >= 0; })) resetEffects();
        }
        else if (controller == 123)
        {
            if (monoActive_) applyMonoTransition(monoVoice_.allNotesOff(static_cast<int>(channel)));
            else for (auto& voice : voices_) if (voice.channel == static_cast<int>(channel)) release(voice);
        }
        else if (controller == 121)
        {
            if (monoActive_) applyMonoTransition(monoVoice_.resetControllers(static_cast<int>(channel)));
            bends_[channel] = 0; wheels_[channel] = 0; pressures_[channel] = 0; pedals_[channel] = false;
            for (auto& voice : voices_) if (! monoActive_ && voice.channel == static_cast<int>(channel))
            { voice.pressure = 0; if (! voice.keyDown) voice.released = true; }
        }
    }
    for (auto& voice : voices_)
        if ((monoActive_ && importedVoiceProfile_) || voice.channel == static_cast<int>(channel)) voice.controlCountdown = 0;
}

float WavetableSynth::nextEnvelope(EnvelopeState& state, int envelope, bool released, const std::array<float, 4>* stages) const
{
    const auto parameter = [this, envelope, stages](EnvParam p) {
        if (stages != nullptr)
        {
            if (p == Attack) return (*stages)[0];
            if (p == Decay) return (*stages)[1];
            if (p == Sustain) return (*stages)[2];
            if (p == Release) return (*stages)[3];
        }
        return block_[static_cast<size_t>(envParam(envelope, p))];
    };
    const float storedSustain = parameter(Sustain);
    const float sustain = importedEnvelopeProfile_ ? storedSustain * storedSustain : storedSustain;
    const float startLevel = state.attackStart >= 0 ? state.attackStart
        : envelope > 0 ? block_[static_cast<size_t>(envLevelParam(envelope, false))] : 0;
    const float endLevel = envelope > 0 ? block_[static_cast<size_t>(envLevelParam(envelope, true))] : 0;
    const auto shaped = [this, envelope, &parameter](float t, EnvParam curve) {
        return importedEnvelopeProfile_
            ? importedEnvelopeCurves_[static_cast<size_t>(envelope)][static_cast<size_t>(curve - AttackCurve)].eval(t)
            : dsp::CurveGraph::shape(t, parameter(curve));
    };
    if (released && state.stage > 0 && state.stage < 6)
    { state.stage = 6; state.elapsed = 0; state.start = state.level; }
    if (state.stage == 0) return 0;
    // Modulated stage times are read every sample (measured: the reference recomputes progress = elapsed / time).
    state.elapsed += 1.0 / sampleRate_;
    for (int transitions = 0; transitions < 6; ++transitions)
    {
        double duration = 0;
        switch (state.stage)
        {
            case 1: duration = parameter(Delay); state.level = startLevel; break;
            case 2:
                duration = parameter(Attack);
                state.level = startLevel + (1 - startLevel) * shaped(duration > 0 ? static_cast<float>(state.elapsed / duration) : 1.0f, AttackCurve);
                break;
            case 3: duration = parameter(Hold); state.level = 1; break;
            case 4:
                duration = parameter(Decay);
                state.level = 1 - (1 - sustain) * shaped(duration > 0 ? static_cast<float>(state.elapsed / duration) : 1.0f, DecayCurve);
                break;
            case 5: state.level = sustain; return state.level;
            case 6:
                duration = state.releaseTime >= 0 ? state.releaseTime : parameter(Release);
                state.level = state.start + (endLevel - state.start)
                    * shaped(duration > 0 ? static_cast<float>(state.releaseOffset + state.elapsed / duration) : 1.0f, ReleaseCurve);
                if (endLevel <= 0 && state.level <= 1e-8f) { state = {}; return 0; }
                break;
            case 7: state.level = endLevel; return state.level;
            default: return 0;
        }
        if (state.elapsed < duration) return state.level;
        state.elapsed -= duration;
        if (state.stage == 6)
        {
            if (endLevel <= 0) { state = {}; return 0; }
            state.stage = 7; state.elapsed = 0; state.level = endLevel; return state.level;
        }
        ++state.stage;
    }
    return state.level;
}

float WavetableSynth::sourceValue(const Voice& voice, int source, int slot) const
{
    if (source >= Env1 && source <= Env4)
    {
        const float level = voice.envelope[static_cast<size_t>(source - Env1)].level;
        return importedEnvelopeProfile_ ? level : level * level;
    }
    if (source >= Lfo1 && source <= Lfo10) return voice.lfoValue[static_cast<size_t>(source - Lfo1)];
    if (source >= Lfo1Y && source <= Lfo10Y) return voice.lfoY[static_cast<size_t>(source - Lfo1Y)];
    if (source >= Macro1 && source <= Macro8)
        return voice.macroModulated ? voice.macroValue[static_cast<size_t>(source - Macro1)] : block_[static_cast<size_t>(macroParam(source - Macro1))];
    switch (source)
    {
        case Velocity: return curveSource(VelocityCurve, voice.velocity);
        case Keytrack: return curveSource(NoteCurve, static_cast<float>(voice.note) / 127);
        case ModWheel: return wheels_[static_cast<size_t>(voice.channel)];
        case Pressure: return voice.pressure;
        case NoteRandom: return voice.random;
        case NoteRandom2: return voice.random2;
        case NoteAlternate: return voice.alternate;
        case NoteAlternate2: return 1 - voice.alternate;
        case NoteRandomDiscrete:
        {
            const bool own = std::less_equal<const Voice*>{}(voices_.data(), &voice) && std::less<const Voice*>{}(&voice, voices_.data() + kVoices);
            const int index = own ? static_cast<int>(&voice - voices_.data()) : fxDiscreteSlot_;
            return index < 0 ? 0 : discreteRandom_[static_cast<size_t>(index)][static_cast<size_t>(std::clamp(slot, 0, kModSlots - 1))];
        }
        case Fixed: return 1;
        default: return 0;
    }
}

float WavetableSynth::routeValue(const Voice& voice, const ModRoute& route) const
{
    float source = sourceValue(voice, route.source, route.slot);
    const int lfoIndex = route.source >= Lfo1 && route.source <= Lfo10 ? route.source - Lfo1
        : route.source >= Lfo1Y && route.source <= Lfo10Y ? route.source - Lfo1Y : -1;
    const bool alreadyBipolar = lfoIndex >= 0 && block_[static_cast<size_t>(lfoParam(lfoIndex, Bipolar))] >= 0.5f;
    if (alreadyBipolar && modulationCurves_[static_cast<size_t>(route.slot)][0].enabled)
        source = 2 * modulationCurveAt(route.slot, false, (source + 1) * 0.5f) - 1;
    else if (! alreadyBipolar) source = modulationCurveAt(route.slot, false, source);
    if (route.bipolar && ! alreadyBipolar)
        source = importedDomains_[static_cast<size_t>(route.destination)] == ImportedModulationDomain::Native
            ? source * 2 - 1 : source - 0.5f;
    if (route.auxiliary != SourceOff) source *= modulationCurveAt(route.slot, true, sourceValue(voice, route.auxiliary, route.slot));
    return source * route.output;
}

void WavetableSynth::advanceChaos(Chaos& state, int type, double cycles, uint32_t& seed, float& held)
{
    if (type == LfoRandomHold)
    {
        // y holds the running cycle position; each completed cycle draws a new value.
        state.y += cycles;
        if (! state.started || state.y >= 1) { held = (randomSigned(seed) + 1) * 0.5f; state.y -= std::floor(state.y); state.started = true; }
        return;
    }
    if (! state.started)
    {
        // Start on the attractor (a point of its long-run orbit) so the first cycles already swing.
        if (type == LfoLorenz) { state.x = -7.69; state.y = -15.62; state.z = 13.3; }
        else { state.x = 1.4; state.y = -6.9; state.z = 0.02; }
        state.started = true;
    }
    // One LFO cycle ~ one orbit: Lorenz loops in about 0.75 time units, Rossler in about 6.
    const double time = cycles * (type == LfoLorenz ? 0.75 : 6.0);
    const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(time) / 0.004)), 1, 4096);
    const double h = time / steps;
    for (int i = 0; i < steps; ++i)
    {
        double dx, dy, dz;
        if (type == LfoLorenz) { dx = 10 * (state.y - state.x); dy = state.x * (28 - state.z) - state.y; dz = state.x * state.y - 8.0 / 3 * state.z; }
        else { dx = -state.y - state.z; dy = state.x + 0.2 * state.y; dz = 0.2 + state.z * (state.x - 5.7); }
        state.x += h * dx; state.y += h * dy; state.z += h * dz;
    }
    if (! std::isfinite(state.x) || ! std::isfinite(state.y) || ! std::isfinite(state.z)) state = Chaos{};
}
std::array<float, 2> WavetableSynth::chaosPoint(const Chaos& state, int type)
{
    if (type == LfoLorenz) return {unit(static_cast<float>((state.x + 22) / 44)), unit(static_cast<float>(state.z / 52))};
    return {unit(static_cast<float>((state.x + 11) / 23)), unit(static_cast<float>((state.y + 12) / 21))};
}
const std::vector<std::array<float, 2>>& WavetableSynth::chaosTrajectory(int type)
{
    static const auto build = [](int kind) {
        std::vector<std::array<float, 2>> points;
        Chaos state; uint32_t seed = 1; float held = 0;
        advanceChaos(state, kind, 4, seed, held); // settle onto the attractor
        for (int i = 0; i < 6000; ++i) { advanceChaos(state, kind, 0.005, seed, held); points.push_back(chaosPoint(state, kind)); }
        return points;
    };
    static const std::vector<std::array<float, 2>> lorenz = build(LfoLorenz), rossler = build(LfoRossler), none;
    return type == LfoLorenz ? lorenz : type == LfoRossler ? rossler : none;
}
void WavetableSynth::updateLfoValues(Voice& voice, const std::array<float, 16>* pointBuses)
{
    for (int l = 0; l < kLfos; ++l)
    {
        const auto at = static_cast<size_t>(l);
        if (const int type = choice(block_[static_cast<size_t>(lfoTypeParam(l))], NumLfoTypes); type != LfoNormal)
        {
            const int mode = choice(block_[static_cast<size_t>(lfoParam(l, Mode))], 3);
            float lfo = 0, y = 0;
            if (mode == Global)
            {
                lfo = globalLfoValue_[at];
                y = type == LfoRandomHold ? 0 : chaosPoint(globalChaos_[at], type)[1];
            }
            else
            {
                advanceChaos(voice.chaos[at], type, voice.lfoIncrement[at] * controlPeriod, voice.noiseSeed, voice.heldRandom[at]);
                lfo = type == LfoRandomHold ? voice.heldRandom[at] : chaosPoint(voice.chaos[at], type)[0];
                y = type == LfoRandomHold ? 0 : chaosPoint(voice.chaos[at], type)[1];
            }
            if (block_[static_cast<size_t>(lfoParam(l, Bipolar))] >= 0.5f) { lfo = lfo * 2 - 1; y = y * 2 - 1; }
            const float fade = block_[static_cast<size_t>(lfoParam(l, Fade))];
            if (fade > 0)
            {
                const float rise = std::min(1.0f, static_cast<float>(static_cast<double>(voice.noteSamples) / (fade * sampleRate_)));
                lfo *= rise; y *= rise;
            }
            voice.lfoValue[at] = lfo; voice.lfoY[at] = y;
            continue;
        }
        const int mode = choice(block_[static_cast<size_t>(lfoParam(l, Mode))], 3);
        const bool localFree = importedLfoProfile_ && mode == Global
            && ! (block_[static_cast<size_t>(lfoParam(l, SyncOn))] >= 0.5f && block_[static_cast<size_t>(lfoAnchorParam(l))] >= 0.5f);
        const float offset = importedLfoProfile_ && mode == Global ? 0 : block_[static_cast<size_t>(lfoParam(l, LfoPhase))];
        const float phase = static_cast<float>(mode == OneShot
            ? std::min(importedLfoProfile_ ? 0.999 : 1.0, voice.lfoPhase[at] + offset)
            : wrap(voice.lfoPhase[at] + offset));
        float lfo = mode == Global && ! localFree ? globalLfoValue_[at]
            : importedLfoProfile_ ? importedLfoCurves_[at].eval(phase) : curves_[at].eval(phase);
        if (importedLfoProfile_ && pointBuses != nullptr && ! (mode == Global && ! localFree))
        {
            // LFO point modulation: a per-voice copy of the curve with the assigned points moved by their bus values.
            bool moved = false;
            dsp::CurveGraph graph = importedLfoCurves_[at].graph;
            for (int a = 0; a < lfoPointAssignmentCount_; ++a)
            {
                const auto& assignment = lfoPointAssignments_[static_cast<size_t>(a)];
                const float bus = (*pointBuses)[static_cast<size_t>(assignment.bus)];
                if (assignment.lfo != l || assignment.point >= graph.count || std::abs(bus) <= 0) continue;
                const auto p = static_cast<size_t>(assignment.point);
                if (assignment.target == 0)
                {
                    const float low = p > 0 ? graph.x[p - 1] : 0, high = static_cast<int>(p) + 1 < graph.count ? graph.x[p + 1] : 1;
                    graph.x[p] = std::clamp(graph.x[p] + bus, low, high);
                }
                else if (assignment.target == 1) graph.y[p] = unit(graph.y[p] + bus);
                else graph.curve[p] = std::clamp(graph.curve[p] + 2 * bus, -1.0f, 1.0f); // stored curve value is (c + 1) / 2
                moved = true;
            }
            if (moved)
            {
                dsp::ExponentialCurveGraph shaped;
                shaped.prepare(graph);
                lfo = shaped.eval(phase);
            }
        }
        if (block_[static_cast<size_t>(lfoParam(l, Bipolar))] >= 0.5f) lfo = lfo * 2 - 1;
        const float fade = block_[static_cast<size_t>(lfoParam(l, Fade))];
        if (fade > 0) lfo *= std::min(1.0f, static_cast<float>(static_cast<double>(voice.noteSamples) / (fade * sampleRate_)));
        voice.lfoValue[at] = lfo;
    }
}

void WavetableSynth::updateControl(Voice& voice, int sample)
{
    const auto voiceIndex = static_cast<size_t>(&voice - voices_.data());
    // LFO point buses first (their sources read the previous control period's LFO values), then the LFOs themselves.
    std::array<float, 16> pointBuses{};
    bool pointModulated = false;
    if (lfoPointAssignmentCount_ > 0)
        for (int i = 0; i < routeCount_; ++i)
            if (const auto& route = routes_[static_cast<size_t>(i)]; route.destination >= LfoPointBus1 && route.destination <= LfoPointBus16)
            {
                pointBuses[static_cast<size_t>(route.destination - LfoPointBus1)] += routeValue(voice, route) * route.amount;
                pointModulated = true;
            }
    updateLfoValues(voice, pointModulated ? &pointBuses : nullptr);
    std::array<float, NumDestinations> modulation{};
    std::array<float, NumDestinations> normalizedModulation{};
    // Macro -> macro (measured): the target macro is its stored value plus amount x source, clamped. One pass over the
    // stored source values before every other route reads the macros.
    voice.macroModulated = false;
    for (int i = 0; i < routeCount_; ++i)
        if (const auto& route = routes_[static_cast<size_t>(i)]; route.destination >= Macro1Value && route.destination <= Macro8Value)
        {
            if (! voice.macroModulated)
                for (int m = 0; m < kMacros; ++m) voice.macroValue[static_cast<size_t>(m)] = block_[static_cast<size_t>(macroParam(m))];
            normalizedModulation[static_cast<size_t>(route.destination)] += routeValue(voice, route) * route.amount;
            voice.macroModulated = true;
        }
    if (voice.macroModulated)
        for (int m = 0; m < kMacros; ++m)
            voice.macroValue[static_cast<size_t>(m)] = unit(block_[static_cast<size_t>(macroParam(m))] + normalizedModulation[static_cast<size_t>(Macro1Value + m)]);
    for (int i = 0; i < routeCount_; ++i)
    {
        const auto& route = routes_[static_cast<size_t>(i)];
        if (route.destination >= Macro1Value && route.destination <= Macro8Value) continue;
        const float source = routeValue(voice, route);
        const auto at = static_cast<size_t>(route.destination);
        if (importedDomains_[at] == ImportedModulationDomain::Native)
            modulation[at] = std::clamp(modulation[at] + source * route.amount, -8.0f, 8.0f);
        else normalizedModulation[at] += source * route.amount;
    }
    const float bend = bends_[static_cast<size_t>(voice.channel)];
    for (int source = 0; source < 5; ++source)
        voice.filterShare[static_cast<size_t>(source)] = unit(sourceRoutes_[static_cast<size_t>(source)].filter2
                                                              + normalizedModulation[static_cast<size_t>(Source1Balance + source)]);
    for (int source = 0; source < kRoutingSources; ++source) for (int bus = 0; bus < 2; ++bus)
    {
        const auto destination = static_cast<size_t>(sourceSendDestination(source, bus));
        const float base = block_[static_cast<size_t>(sourceRoutingParam(source, bus == 0 ? SourceBus1 : SourceBus2))];
        const float send = unit(base + modulation[destination] + normalizedModulation[destination]);
        voice.sourceSends[static_cast<size_t>(source)][static_cast<size_t>(bus)] = send * send * send;
    }
    // Master tuning (measured): 128 semitones per unit of route amount, clamped to +-64.
    const float tuning = importedDomains_[static_cast<size_t>(MasterTuning)] == ImportedModulationDomain::CoarsePitch128
        ? unit(0.5f + normalizedModulation[static_cast<size_t>(MasterTuning)]) * 128 - 64 : 0;
    const float transposeModulation = normalizedModulation[static_cast<size_t>(GlobalTranspose)];
    const float transpose = std::abs(transposeModulation) > 0
        ? std::clamp(std::floor(49 * ((std::round(block_[Transpose]) + 24) / 48 + transposeModulation)), 0.0f, 48.0f) - 24
        : block_[Transpose];
    const double pitch = voice.pitchNote - 69 + transpose + tuning
        + bend * block_[bend >= 0 ? BendUp : BendDown] + modulation[Pitch] * 24;
    for (int l = 0; l < kLfos; ++l)
    {
        const auto at = static_cast<size_t>(l);
        const float rateModulation = normalizedModulation[static_cast<size_t>(Lfo1Rate + l)];
        voice.lfoIncrement[at] = lfoIncrement_[at];
        if (std::abs(rateModulation) <= 0) continue;
        // LFO rate (measured): the stored rate's normalized position u (Hz = 100 u^4) plus the routes, clamped; a synced
        // LFO snaps the result to the nearest division its dotted/triplet switches allow.
        const float rate = block_[static_cast<size_t>(lfoParam(l, Rate))];
        const double u = unit(static_cast<float>(std::sqrt(std::sqrt(std::clamp(rate / 100.0, 0.0, 1.0)))) + rateModulation);
        if (block_[static_cast<size_t>(lfoParam(l, SyncOn))] >= 0.5f)
        {
            const int stored = choice(block_[static_cast<size_t>(lfoParam(l, Division))], kLfoDivisions);
            const auto& sync = importedLfoSync_[at];
            const double from = dsp::LfoTempoDivisions::fromStoredRate(rate, sync[0], sync[1]) == stored ? std::sqrt(std::sqrt(std::clamp(rate / 100.0, 0.0, 1.0)))
                : dsp::LfoTempoDivisions::binFor(stored) / 42.0;
            const double moved = unit(static_cast<float>(from + rateModulation));
            const int division = dsp::LfoTempoDivisions::fromStoredRate(100 * moved * moved * moved * moved, sync[0], sync[1]);
            const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
            voice.lfoIncrement[at] = tempo / (60 * sampleRate_ * divisionBeats(division));
        }
        else voice.lfoIncrement[at] = 100 * u * u * u * u * (block_[static_cast<size_t>(lfoRateScaleParam(l))] >= 0.5f ? 10 : 1) / sampleRate_;
    }
    const double frequency = std::clamp(440 * std::exp2(pitch / 12), 0.01, sampleRate_ * 0.45);
    const float outputVolume = effectsReady_ ? 1.0f : masterGainFor(block_[Volume]);
    // Voice amp (measured): gain linear in u, the stored value (default 0.5) plus the routes.
    const float voiceAmp = unit(importedVoiceAmp_ + normalizedModulation[static_cast<size_t>(VoiceAmp)]) * 2;
    voice.targetGain = outputVolume * voiceAmp * std::exp2(std::clamp(modulation[Amplitude] * 4, -12.0f, 3.0f));
    if (voice.noteSamples == 0) voice.gain = voice.targetGain;
    const float pan = std::clamp(block_[Pan] + modulation[StereoPan], -1.0f, 1.0f);
    voice.panLeft = balanceLeft(pan); voice.panRight = balanceRight(pan);
    const auto subAmplitude = static_cast<size_t>(SubAmplitude);
    const float normalizedSubLevel = unit(std::sqrt(unit(block_[SubLevel])) + normalizedModulation[subAmplitude]);
    voice.subLevel = block_[SubOn] < 0.5f ? 0
        : importedDomains_[subAmplitude] == ImportedModulationDomain::SquaredLevel ? normalizedSubLevel * normalizedSubLevel
        : unit(block_[SubLevel] + modulation[subAmplitude]);
    // Imported sub/noise pan targets (Pan2): the stored pan's 0..1 position plus the routes, clamped.
    const float subPan = unit((block_[SubPan] + 1) * .5f + normalizedModulation[static_cast<size_t>(SubPanDest)]);
    voice.subLeft = importedSubProfile_ ? observedPanGain(subPan, false) : balanceLeft(block_[SubPan]);
    voice.subRight = importedSubProfile_ ? observedPanGain(subPan, true) : balanceRight(block_[SubPan]);
    const auto noiseAmplitude = static_cast<size_t>(NoiseAmplitude);
    const float normalizedNoiseLevel = unit(std::sqrt(unit(block_[NoiseLevel])) + normalizedModulation[noiseAmplitude]);
    const float noiseLevel = importedDomains_[noiseAmplitude] == ImportedModulationDomain::SquaredLevel
        ? normalizedNoiseLevel * normalizedNoiseLevel
        : unit(block_[NoiseLevel] + modulation[noiseAmplitude]);
    voice.noiseLevel = block_[NoiseOn] >= 0.5f ? noiseLevel : 0;
    const float noisePan = unit((block_[NoisePan] + 1) * .5f + normalizedModulation[static_cast<size_t>(NoisePanDest)]);
    voice.noiseLeft = importedNoiseProfile_ ? observedPanGain(noisePan, false) : balanceLeft(block_[NoisePan]);
    voice.noiseRight = importedNoiseProfile_ ? observedPanGain(noisePan, true) : balanceRight(block_[NoisePan]);
    const double noiseTracking = block_[NoiseKeytrack] >= .5f ? pitch + 69 - 72 : 0;
    voice.noiseRatio = std::exp2((block_[NoisePitch] + noiseTracking) / 12 + block_[NoiseFine] / 1200);
    if (noiseSample_ != nullptr && ! voice.noiseSample.active())
    {
        // Imported notes use their draw from the reference stream; the start is clamped, not wrapped
        // (min(N - c, (phase + u rand^2) N)). Own patches keep their deterministic seed.
        const float randomAmount = importedNoiseProfile_ ? block_[NoiseRandom] * block_[NoiseRandom] : block_[NoiseRandom];
        const double phase = voice.serumNoiseRandom >= 0 ? block_[NoisePhase] + voice.serumNoiseRandom * randomAmount
            : randomAmount > 0 ? wrap(block_[NoisePhase] + static_cast<float>(voice.noiseSeed >> 8) / 16777216.0f * randomAmount) : block_[NoisePhase];
        voice.noiseSample.start(noiseSample_.get(), sampleRate_, phase);
    }
    const float subCoarse = importedDomains_[static_cast<size_t>(SubCoarse)] == ImportedModulationDomain::CoarsePitch128
        ? unit(0.5f + normalizedModulation[static_cast<size_t>(SubCoarse)]) * 128 - 64 : 0;
    voice.subIncrement = std::min(0.45, frequency * std::exp2(std::round(block_[SubOctave]) + subCoarse / 12.0) / sampleRate_);
    // Envelope stage targets (measured): times add in the host's normalized time domain (seconds = 32 u^5), sustain adds linearly; clamped.
    voice.envelopeStagesModulated = false;
    for (int e = 0; e < kEnvelopes; ++e)
    {
        const std::array<EnvParam, 4> stageParams{Attack, Decay, Sustain, Release};
        for (size_t k = 0; k < stageParams.size(); ++k)
        {
            const auto at = static_cast<size_t>(Env1Attack + e * 4 + static_cast<int>(k));
            const float base = block_[static_cast<size_t>(envParam(e, stageParams[k]))];
            float value = base;
            if (importedDomains_[at] == ImportedModulationDomain::EnvelopeTime && std::abs(normalizedModulation[at]) > 0)
            {
                value = observedEnvelopeSeconds(std::pow(std::max(0.0f, base) / 32, 0.2f) + normalizedModulation[at]);
                voice.envelopeStagesModulated = true;
            }
            else if (importedDomains_[at] == ImportedModulationDomain::Unit && std::abs(normalizedModulation[at]) > 0)
            {
                value = unit(base + normalizedModulation[at]);
                voice.envelopeStagesModulated = true;
            }
            else if (importedDomains_[at] == ImportedModulationDomain::Native && std::abs(modulation[at]) > 0)
            {
                // Own patches move the editor's control: times along its fifth-power taper, sustain linearly.
                value = k == 2 ? unit(base + modulation[at]) : observedEnvelopeSeconds(std::pow(std::max(0.0f, base) / 32, 0.2f) + modulation[at]);
                voice.envelopeStagesModulated = true;
            }
            voice.envelopeStages[static_cast<size_t>(e)][k] = value;
        }
    }
    for (int o = 0; o < kOscillators; ++o)
    {
        auto& control = voice.oscillator[static_cast<size_t>(o)];
        const auto parameter = [this, o](OscParam p) { return block_[static_cast<size_t>(oscParam(o, p))]; };
        control.on = parameter(On) >= 0.5f;
        control.table = choice(parameter(Table), kTableFamilies);
        // Unit domain (measured): the stored 0..1 value plus the summed normalized modulation, clamped.
        const auto unitTarget = [&](int destination, float base) {
            const auto at = static_cast<size_t>(destination);
            return importedDomains_[at] == ImportedModulationDomain::Unit ? unit(base + normalizedModulation[at]) : base + modulation[at];
        };
        control.targetPosition = unit(unitTarget(Osc1Position + o, parameter(Position)));
        const auto levelAt = static_cast<size_t>(Osc1Level + o);
        if (importedDomains_[levelAt] == ImportedModulationDomain::SquaredLevel)
        {
            const float normalized = unit(std::sqrt(unit(parameter(Level))) + normalizedModulation[levelAt]);
            control.targetLevel = normalized * normalized;
        }
        else control.targetLevel = unit(parameter(Level) + modulation[levelAt]);
        if (voice.noteSamples == 0) { control.position = control.targetPosition; control.level = control.targetLevel; }
        control.count = std::clamp(static_cast<int>(std::lround(parameter(Unison))), 1, kUnison);
        control.warp1 = choice(parameter(Warp1Mode), NumWarps); control.warp2 = choice(parameter(Warp2Mode), NumWarps);
        control.warpAmount1 = std::clamp(unitTarget(Osc1Warp1 + o, parameter(Warp1Amount)), -1.0f, 1.0f);
        control.warpAmount2 = std::clamp(unitTarget(Osc1Warp2 + o, parameter(Warp2Amount)), -1.0f, 1.0f);
        control.distScale1 = distortionWarpScale(control.warp1, control.warpAmount1);
        control.distScale2 = distortionWarpScale(control.warp2, control.warpAmount2);
        for (int w = 0; w < 2; ++w)
        {
            const float amount = std::abs(w == 0 ? control.warpAmount1 : control.warpAmount2);
            const double hz = std::min(41.5 * std::exp2(10.0 * amount), sampleRate_ * 0.499);
            const double g = std::tan(std::numbers::pi * hz / sampleRate_);
            control.warpFilterG[static_cast<size_t>(w)] = static_cast<float>(g / (1 + g));
        }
        control.route1 = parameter(RouteFilter1) >= 0.5f; control.route2 = parameter(RouteFilter2) >= 0.5f;
        const auto pitchAt = static_cast<size_t>(Osc1Pitch + o);
        const float coarse = block_[static_cast<size_t>(coarseParam(o))];
        const float modulatedCoarse = importedDomains_[pitchAt] == ImportedModulationDomain::CoarsePitch128
            ? unit((coarse + 64) / 128 + normalizedModulation[pitchAt]) * 128 - 64 : coarse;
        // Imported fine (±100 cents) and octave (±4) targets: the stored value's normalized position plus the route sum, clamped.
        const float fineMod = normalizedModulation[static_cast<size_t>(Osc1Fine + o)], octaveMod = normalizedModulation[static_cast<size_t>(Osc1Octave + o)];
        const float fine = std::abs(fineMod) > 0 ? unit((parameter(Fine) + 100) / 200 + fineMod) * 200 - 100 : parameter(Fine);
        const float octave = std::abs(octaveMod) > 0 ? std::round(unit((parameter(Octave) + 4) / 8 + octaveMod) * 8 - 4) : std::round(parameter(Octave));
        const float semiModulation = normalizedModulation[static_cast<size_t>(Osc1Semi + o)];
        const float semitones = std::abs(semiModulation) > 0
            ? std::clamp(std::floor(25 * ((std::round(parameter(Semi)) + 12) / 24 + semiModulation)), 0.0f, 24.0f) - 12
            : std::round(parameter(Semi));
        const double oscillatorPitch = octave * 12 + semitones + fine * 0.01 + modulatedCoarse
            + modulation[static_cast<size_t>(Osc1Pitch + o)] * 24;
        const double oscillatorFrequency = frequency * std::exp2(oscillatorPitch / 12);
        const auto panAt = static_cast<size_t>(Osc1Pan + o);
        const float oscPan = importedDomains_[panAt] == ImportedModulationDomain::Pan2
            ? unit((parameter(OscPan) + 1) * 0.5f + normalizedModulation[panAt]) * 2 - 1
            : std::clamp(parameter(OscPan) + modulation[panAt], -1.0f, 1.0f);
        const auto mediaParameter = [this, o](MediaParam p) { return block_[static_cast<size_t>(mediaParam(o, p))]; };
        const int oscillatorMode = choice(mediaParameter(MediaMode), NumOscillatorModes);
        auto& media = (*mediaVoices_)[voiceIndex][static_cast<size_t>(o)];
        const float mediaPosition = unit(mediaParameter(MediaStartPosition) + modulation[static_cast<size_t>(Osc1MediaPosition + o)]);
        if (control.mediaMode != oscillatorMode)
        {
            control.mediaMode = oscillatorMode; control.mediaReleased = false;
            if (oscillatorMode == WavetableMode) media.reset();
            else
            {
                media.start(oscillatorMedia_[static_cast<size_t>(o)].get(), static_cast<OscillatorMediaVoice::Mode>(oscillatorMode - 1),
                            voice.note, voice.midiVelocity, sampleRate_, voice.noiseSeed ^ static_cast<uint32_t>(o * 0x1f123bb5));
                control.mediaRootNote = voice.note;
                if (mediaPosition > 0) media.seek(mediaPosition);
            }
            control.mediaSeekPosition = mediaPosition;
        }
        else if (oscillatorMode != WavetableMode && std::abs(mediaPosition - control.mediaSeekPosition) > 1e-6f)
        {
            media.seek(mediaPosition); control.mediaSeekPosition = mediaPosition;
        }
        if (oscillatorMode != WavetableMode)
        {
            auto& options = control.mediaControls;
            options.pitchSemitones = pitch - static_cast<double>(control.mediaRootNote - 69) + oscillatorPitch;
            options.timeRate = std::clamp(mediaParameter(MediaScanRate) + modulation[static_cast<size_t>(Osc1MediaScan + o)] * 8, -8.0f, 8.0f);
            options.grainSeconds = std::clamp(static_cast<double>(mediaParameter(MediaGrainSize))
                * std::exp2(modulation[static_cast<size_t>(Osc1GrainSize + o)] * 4), 0.001, 8.0);
            options.density = std::clamp(static_cast<double>(mediaParameter(MediaDensity))
                * std::exp2(modulation[static_cast<size_t>(Osc1GrainDensity + o)] * 4), 0.1, 1000.0);
            options.densityMode = static_cast<OscillatorMediaVoice::DensityMode>(choice(mediaParameter(MediaDensityMode), 3));
            options.densityBeats = mediaParameter(MediaDensityBeats);
            options.tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
            options.grainLimit = choice(mediaParameter(MediaGrainLimit) - 1, OscillatorMediaVoice::maxGrains) + 1;
            options.positionJitter = mediaParameter(MediaPositionSpread); options.pitchJitter = mediaParameter(MediaPitchSpread);
            options.panJitter = mediaParameter(MediaPanSpread); options.directionJitter = mediaParameter(MediaDirectionSpread);
            options.lengthJitter = mediaParameter(MediaLengthSpread); options.gainJitter = mediaParameter(MediaGainSpread);
            options.window = static_cast<OscillatorMediaVoice::Window>(choice(mediaParameter(MediaWindow), 3));
            options.windowAmount = mediaParameter(MediaWindowAmount); options.windowSkew = mediaParameter(MediaWindowSkew);
            options.jumpStart = mediaParameter(MediaJumpStart) >= 0.5f; options.reverse = mediaParameter(MediaReverse) >= 0.5f;
            options.oneShot = mediaParameter(MediaOneShot) >= 0.5f; options.applyVelocity = mediaParameter(MediaVelocityTracking) >= 0.5f;
            options.loopOverride = static_cast<OscillatorMediaVoice::LoopOverride>(choice(mediaParameter(MediaLoopMode), 5));
            options.serumProfile = importedVoiceProfile_;
            options.attackOverride = importedMediaEnvelope_[static_cast<size_t>(o)][0];
            options.releaseOverride = importedMediaEnvelope_[static_cast<size_t>(o)][1];
            control.mediaLeft = balanceLeft(oscPan); control.mediaRight = balanceRight(oscPan);
            continue; // One fixed media runtime per oscillator, independently of WT unison.
        }
        double largestIncrement = 0;
        float totalWeight = 0;
        // Imported detune target (measured): Serum's 0..1 detune d (cents = 200 d^1.25) moves in the square-root domain,
        // d = (sqrt(d0) + routes)^2 clamped; Unison Range then scales the cents (range / 2).
        const float detune = importedDetuneCents(parameter(Detune), normalizedModulation[static_cast<size_t>(Osc1Detune + o)],
                                                 importedDetuneScale_[static_cast<size_t>(o)]);
        const float blend = std::clamp(parameter(Blend) + normalizedModulation[static_cast<size_t>(Osc1Blend + o)], 0.0f, 1.0f);
        const float width = std::clamp(parameter(Width) + 2 * normalizedModulation[static_cast<size_t>(Osc1Width + o)], -1.0f, 1.0f);
        for (int u = 0; u < control.count; ++u)
        {
            // Odd counts include a center voice. Even counts include a symmetric center pair.
            const float spread = control.count == 1 ? 0 : (2.0f * static_cast<float>(u) / static_cast<float>(control.count - 1) - 1);
            if (importedVoiceProfile_ && control.count > 1)
            {
                // Measured reference law (see serumUnisonCountGain): linear detune, two level classes, alternating pair panning, quadratic-product width law.
                const int count = control.count;
                const bool odd = (count & 1) != 0;
                const int pair = std::min(u, count - 1 - u);
                const bool centreVoice = odd && u == (count - 1) / 2;
                const bool centreClass = centreVoice || (! odd && (u == count / 2 - 1 || u == count / 2));
                const bool low = u < count - 1 - u;
                const int side = centreVoice ? 0 : ((((pair & 1) == 0) == low) ? -1 : 1); // pair 0: low -> right, high -> left; pair 1 swaps
                const double cents = static_cast<double>(spread) * detune;
                const double increment = std::clamp(oscillatorFrequency * std::exp2(cents / 1200.0) / sampleRate_, 0.0, 0.45);
                control.increment[static_cast<size_t>(u)] = increment;
                largestIncrement = std::max(largestIncrement, increment);
                const float gain = serumUnisonCountGain(count, centreClass) * (centreClass ? serumUnisonBlendCentre(blend) : serumUnisonBlendSide(blend));
                constexpr float k = 0.2615f;
                const float main = (1 + std::abs(width)) * (1 + k * std::abs(width)), other = (1 - std::abs(width)) * (1 - k * std::abs(width));
                // width > 0: the "left" voices stay left; width < 0 swaps the channels.
                const bool toLeft = (side > 0) == (width >= 0);
                const float panLeft = side == 0 ? 1.0f : (toLeft ? main : other), panRight = side == 0 ? 1.0f : (toLeft ? other : main);
                const float oscLeft = observedPanGain((oscPan + 1) * .5f, false), oscRight = observedPanGain((oscPan + 1) * .5f, true);
                control.left[static_cast<size_t>(u)] = gain * panLeft * oscLeft;
                control.right[static_cast<size_t>(u)] = gain * panRight * oscRight;
                totalWeight = 1; // the law carries its own normalisation
                continue;
            }
            const float shaped = std::copysign(spread * spread, spread);
            const double increment = std::clamp(oscillatorFrequency * std::exp2(shaped * detune / 1200.0) / sampleRate_, 0.0, 0.45);
            control.increment[static_cast<size_t>(u)] = increment;
            largestIncrement = std::max(largestIncrement, increment);
            const bool center = std::abs(spread) <= 1.01f / static_cast<float>(std::max(1, control.count - 1));
            const float weight = control.count == 1 || center ? 1 : blend;
            const float unisonPan = std::clamp(oscPan + spread * width, -1.0f, 1.0f);
            control.left[static_cast<size_t>(u)] = weight * (importedVoiceProfile_ && control.count == 1
                ? observedPanGain((unisonPan + 1) * .5f, false) : balanceLeft(unisonPan));
            control.right[static_cast<size_t>(u)] = weight * (importedVoiceProfile_ && control.count == 1
                ? observedPanGain((unisonPan + 1) * .5f, true) : balanceRight(unisonPan));
            totalWeight += weight;
        }
        const float normalization = 1 / std::max(1.0f, totalWeight);
        for (int u = 0; u < control.count; ++u)
        { control.left[static_cast<size_t>(u)] *= normalization; control.right[static_cast<size_t>(u)] *= normalization; }
        // Warp generates extra harmonics. Select a conservative lower-bandwidth table; nonlinear sidebands can remain.
        control.bandwidthScale = 1;
        for (const auto [mode, amount] : {std::pair{control.warp1, control.warpAmount1}, std::pair{control.warp2, control.warpAmount2}})
            if (mode == FilterLpfWarp || mode == FilterHpfWarp) continue; // linear: no new harmonics
            else if (isDistortionWarp(mode)) control.bandwidthScale *= 1 + std::abs(amount); // shaped per sample; keep most of the table's band
            else if (mode != WarpOff && mode != RingMod && mode != SelfPd && sourcedWarp(mode).kind == 0) control.bandwidthScale *= 1 + std::abs(amount) * (mode == Sync ? (importedVoiceProfile_ ? 15 * amount * amount : 7) : 3);
        const double bandwidth = largestIncrement * control.bandwidthScale;
        const auto* custom = oscillatorTable(o, control.table);
        if (custom == nullptr || custom->playbackProfile() == WavetableData::PlaybackProfile::Dense12)
            for (auto& transition : control.mipTransition) transition.reset();
        if (custom != nullptr) control.mip = custom->mipForIncrement(bandwidth);
        else
        {
            control.mip = 0;
            while (control.mip < tableMips - 1 && bandwidth * (tableHarmonics >> control.mip) > 0.45) ++control.mip;
        }
    }
    for (int f = 0; f < kFilters; ++f)
    {
        const auto parameter = [this, f](FilterParam p) { return block_[static_cast<size_t>(filterParam(f, p))]; };
        const float envelope = voice.envelope[static_cast<size_t>(choice(parameter(EnvSource), kEnvelopes))].level;
        // Measured (DEV-18i): imported keytrack moves the cutoff 1 octave per octave around MIDI note 32 (native patches: 60).
        const float octaves = parameter(FilterKeytrack) * static_cast<float>(voice.note - (importedVoiceProfile_ ? kImportedKeytrackCenter : 60)) / 12
            + parameter(EnvAmount) * envelope + modulation[static_cast<size_t>(Filter1Cutoff + f)] * 8;
        const auto cutoffAt = static_cast<size_t>(Filter1Cutoff + f);
        float baseCutoff = parameter(Cutoff);
        const bool importedCutoff = importedDomains_[cutoffAt] == ImportedModulationDomain::FilterFrequency;
        if (importedCutoff)
        {
            float normalized = (std::log2(baseCutoff / 440) + 5.75f) / 11.4f;
            // Below 20 Hz the control clamps; the reference position is exact while the control still shows it.
            if (const float stored = importedCutoffPosition_[static_cast<size_t>(f)];
                stored >= 0 && std::abs(std::clamp(observedFilterHertz(stored, sampleRate_), 20.0f, 20000.0f) - baseCutoff) <= 0.001f * baseCutoff)
                normalized = stored;
            baseCutoff = observedFilterHertz(unit(normalized + normalizedModulation[cutoffAt]), sampleRate_);
        }
        const float cutoff = std::clamp(baseCutoff * std::exp2(std::clamp(octaves, -16.0f, 16.0f)), importedCutoff ? 8.0f : 20.0f,
                                        static_cast<float>(sampleRate_ * 0.45));
        const auto resonanceAt = static_cast<size_t>(Filter1Resonance + f);
        const float res = unit(parameter(Resonance) + (importedDomains_[resonanceAt] == ImportedModulationDomain::Unit ? normalizedModulation[resonanceAt] : modulation[resonanceAt]));
        voice.filterResonance[static_cast<size_t>(f)] = res;
        voice.filterDrive[static_cast<size_t>(f)] = dsp::dbToGain(parameter(Drive));
        const int filterMode = choice(parameter(FilterMode), NumFilterTypes);
        if (const float driveModulation = normalizedModulation[static_cast<size_t>(Filter1Drive + f)];
            std::abs(driveModulation) > 0 && (isLadderFilter(filterMode) || isMeasuredSvfFilter(filterMode) || filterMode > SerumLadderEms))
            voice.filterDrive[static_cast<size_t>(f)] = dsp::ladderDriveGain(unit(dsp::ladderDriveForGain(dsp::dbToGain(parameter(Drive))) + driveModulation));
        // Output level (measured): 4 u^2 with u the stored level's normalized position plus the routes, clamped.
        const float storedLevel = block_[static_cast<size_t>(filterOutputParam(f))];
        const float levelModulation = normalizedModulation[static_cast<size_t>(Filter1Level + f)];
        const float levelU = unit(std::sqrt(std::max(0.0f, storedLevel) / 4) + levelModulation);
        voice.filterLevel[static_cast<size_t>(f)] = std::abs(levelModulation) > 0 ? 4 * levelU * levelU : storedLevel;
        // Var (measured: the stored 0..1 value plus the routes, clamped) scales the resonance level compensation.
        voice.filterVar[static_cast<size_t>(f)] = unit(parameter(FilterMorph) + normalizedModulation[static_cast<size_t>(Filter1Var + f)]);
        voice.filterMix[static_cast<size_t>(f)] = parameter(FilterOn) >= 0.5f
            ? unit(parameter(FilterMix) + normalizedModulation[static_cast<size_t>(Filter1Mix + f)]) : 0;
        for (int channel = 0; channel < 2; ++channel)
        {
            if (isLadderFilter(filterMode))
                voice.ladder[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, filterMode == SerumDirtyMg ? cutoff * 0.841f : cutoff,
                    filterMode == SerumDirtyMg ? dirtyMgFeedback(res) : res, filterMode == SerumLadderMg || filterMode == SerumDirtyMg ? 4 : filterMode - MgLow6 + 1);
            else if (filterMode == SerumLadderEms)
                voice.staggered[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, res);
            else if (isMeasuredSvfFilter(filterMode))
                voice.measured[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, res, voice.filterVar[static_cast<size_t>(f)], filterMode - SerumLow6);
            else if (isMeasuredMultiFilter(filterMode))
                voice.multi[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, res, voice.filterVar[static_cast<size_t>(f)], filterMode - SerumPeak12);
            else if (filterMode == SerumLadderAcid)
                voice.diode[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, res);
            else if (isMeasuredCombFilter(filterMode))
                voice.comb[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, res, voice.filterVar[static_cast<size_t>(f)],
                                                                                    filterMode - SerumCombP, combLineLength_);
            voice.filter[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, 0.55f + res * res * 14);
            voice.filterSecond[static_cast<size_t>(f)][static_cast<size_t>(channel)].setup(sampleRate_, cutoff, 0.70710678f);
        }
    }
    voice.controlCountdown = controlPeriod;
    (void) sample;
}

namespace
{
int distortionShaperMode(int warp)
{
    // Reference shaper indices (serum_distortion) of DistTube..DistSoftSat.
    static constexpr int modes[]{0, 1, 2, 3, 4, 7, 9, 10, 14, 15, 17};
    return modes[std::clamp(warp - WavetableSynth::DistTube, 0, static_cast<int>(std::size(modes)) - 1)];
}
} // namespace
float WavetableSynth::distortionWarpScale(int, float)
{
    // Measured: no renormalisation; the shaper's own output level carries through (Soft Clip at full drive is +2 dB louder).
    return 1;
}
float WavetableSynth::distortionWarp(int warp, float amount, float sample, float scale)
{
    if (! isDistortionWarp(warp)) return sample;
    return serum_distortion::shaper(distortionShaperMode(warp), std::abs(amount), 0.5f * sample) * scale;
}
float WavetableSynth::warpedPhase(float phase, int mode, float amount, float modulator, bool measured)
{
    const float a = std::clamp(amount, -1.0f, 1.0f);
    if (mode == Bend)
    {
        const float power = 1 + std::abs(a) * 3;
        return a >= 0 ? std::pow(phase, power) : 1 - std::pow(1 - phase, power);
    }
    // Measured Serum 2 hard sync: the slave runs at 1 + 15 |amount|^3 times the master frequency and restarts with every master cycle.
    if (mode == Sync) return static_cast<float>(wrap(phase * (measured ? 1 + 15 * std::abs(a) * std::abs(a) * std::abs(a) : 1 + std::abs(a) * 7)));
    if (mode == Mirror)
    {
        const float folded = 1 - std::abs(2 * phase - 1);
        return phase + (folded - phase) * std::abs(a);
    }
    if (mode == Pulse)
    {
        const float width = 0.5f + a * 0.45f;
        return phase < width ? 0.5f * phase / width : 0.5f + 0.5f * (phase - width) / (1 - width);
    }
    if (mode == PhaseMod) return static_cast<float>(wrap(phase + modulator * a * 0.5f));
    return phase;
}
float WavetableSynth::filterSample(Voice& voice, int filter, int channel, float input) const
{
    const auto at = static_cast<size_t>(filter), ch = static_cast<size_t>(channel);
    if (voice.filterMix[at] <= 0) return input;
    const float drive = voice.filterDrive[at];
    const int mode = choice(block_[static_cast<size_t>(filterParam(filter, FilterMode))], NumFilterTypes);
    if (isLadderFilter(mode))
    {
        float ladder = voice.ladder[at][ch].process(input * drive); // the drive gain feeds the ladder's own input clipper
        // Measured: Ladder MG restores the ladder's resonance level loss (1 + 4 r) at 0.80; on the MG Low ladders Var
        // restores the fraction var of it (output x (1 + 4 r var), negative Var acts as 0).
        if (mode == SerumLadderMg) ladder *= 0.8007f * (1 + 4 * voice.filterResonance[at]);
        else if (mode == SerumDirtyMg) ladder *= 0.893f;
        else ladder *= 1 + 4 * voice.filterResonance[at] * voice.filterVar[at];
        return input + (ladder - input) * voice.filterMix[at];
    }
    if (mode == FilterThrough)
    {
        // Measured on the Diffuser (a pass-through approximation): the Drive stage still runs, as on the normal family (ladder
        // gain table into the voice soft clipper). Without drive the type is level-only.
        if (drive <= 1.0001f) return input;
        const float driven = dsp::LadderFilter::inputClip(input * drive);
        return input + (driven - input) * voice.filterMix[at];
    }
    if (mode == SerumLadderEms)
    {
        // Measured: on Ladder EMS the drive is a plain input gain (the ladder table); nothing saturates.
        const float filtered = voice.staggered[at][ch].process(input * drive);
        return input + (filtered - input) * voice.filterMix[at];
    }
    if (isMeasuredSvfFilter(mode))
    {
        // Measured: Drive is the ladder's input gain table in front of a soft clipper, then the filter.
        const float filtered = voice.measured[at][ch].process(dsp::LadderFilter::inputClip(input * drive));
        return input + (filtered - input) * voice.filterMix[at];
    }
    if (isMeasuredMultiFilter(mode) || mode == SerumLadderAcid || isMeasuredCombFilter(mode))
    {
        // Drive as on the normal family (assumed: the same input gain table and soft clipper; not measured per type).
        const float driven = dsp::LadderFilter::inputClip(input * drive);
        float filtered = 0;
        if (isMeasuredMultiFilter(mode)) filtered = voice.multi[at][ch].process(driven);
        else if (mode == SerumLadderAcid) filtered = voice.diode[at][ch].process(driven);
        else filtered = voice.comb[at][ch].process(driven, combLine(voice, filter, channel), combLineLength_ - 1);
        return input + (filtered - input) * voice.filterMix[at];
    }
    const float driven = drive > 1.0001f ? std::tanh(input * drive) / std::sqrt(drive) : input;
    auto& first = voice.filter[at][ch];
    auto& second = voice.filterSecond[at][ch];
    float output = 0;
    if (mode == Low12 || mode == Low24)
    {
        output = first.process(driven, dsp::Svf::Mode::LowPass);
        if (mode == Low24) output = second.process(output, dsp::Svf::Mode::LowPass);
    }
    else if (mode == High12 || mode == High24)
    {
        output = first.process(driven, dsp::Svf::Mode::HighPass);
        if (mode == High24) output = second.process(output, dsp::Svf::Mode::HighPass);
    }
    else if (mode == BandPass) output = first.process(driven, dsp::Svf::Mode::BandPass) / (0.55f + voice.filterResonance[at] * voice.filterResonance[at] * 14);
    else if (mode == Notch) output = first.process(driven, dsp::Svf::Mode::Notch);
    else
    {
        // Own complementary low/high morph: the high branch is the driven signal minus the low branch.
        const float low = first.process(driven, dsp::Svf::Mode::LowPass);
        const float high = driven - low;
        const float morph = block_[static_cast<size_t>(filterParam(filter, FilterMorph))];
        output = low + (high - low) * morph;
    }
    return input + (output - input) * voice.filterMix[at];
}

void WavetableSynth::render(juce::AudioBuffer<float>& audio, int from, int to)
{
    if (from >= to) return;
    float* left = audio.getNumChannels() > 0 ? audio.getWritePointer(0) : nullptr;
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : nullptr;
    if (! effectsReady_ || effects_ == nullptr)
    {
        renderPreparedVoices(left, right, from, to, 0);
        return;
    }
    const float targetMasterGain = masterGainFor(block_[Volume]);
    if (! masterGainInitialized_) { masterGain_ = targetMasterGain; masterGainInitialized_ = true; }
    for (int start = from; start < to;)
    {
        const int frames = std::min(fxBlockCapacity_, to - start);
        for (auto& channel : fxBuffers_) std::fill_n(channel.data(), frames, 0);
        for (auto& channel : directSubBuffers_) std::fill_n(channel.data(), frames, 0);
        const bool modulatedEffects = ! fxTargets_.empty();
        if (modulatedEffects) selectFxVoice();
        renderPreparedVoices(fxBuffers_[0].data(), fxBuffers_[1].data(), start, start + frames, start,
                     directSubBuffers_[0].data(), directSubBuffers_[1].data(),
                     fxBuffers_[2].data(), fxBuffers_[3].data(), fxBuffers_[4].data(), fxBuffers_[5].data());
        // Imported FX routes follow their sources every sample in the reference (measured: an Env 2 step lands at once and
        // ramps track the envelope); the graph re-derives its controls every 16 samples, so modulated graphs run in
        // 16-sample sub-blocks aligned to that clock with the route values written before each.
        for (int offset = 0; offset < frames;)
        {
            const int count = modulatedEffects
                ? std::min(frames - offset, kFxModulationPeriod - static_cast<int>(fxModulationClock_ % kFxModulationPeriod)) : frames;
            if (modulatedEffects) applyFxModulation(count);
            std::array<WavetableEffects::StereoBuffer, WavetableEffects::busCount> buses{};
            for (size_t bus = 0; bus < buses.size(); ++bus)
                buses[bus] = {fxBuffers_[bus * 2].data() + offset, fxBuffers_[bus * 2 + 1].data() + offset};
            effects_->process(buses, count);
            fxModulationClock_ += static_cast<uint64_t>(count);
            offset += count;
        }
        for (int offset = 0; offset < frames; ++offset)
        {
            const auto at = static_cast<size_t>(offset);
            float subLeft = directSubBuffers_[0][at], subRight = directSubBuffers_[1][at];
            if (directSubLatency_ > 0)
            {
                const float delayedLeft = directSubAlignment_[0].read(static_cast<float>(directSubLatency_ - 1));
                const float delayedRight = directSubAlignment_[1].read(static_cast<float>(directSubLatency_ - 1));
                directSubAlignment_[0].write(subLeft); directSubAlignment_[1].write(subRight);
                subLeft = delayedLeft; subRight = delayedRight;
            }
            masterGain_ += masterSmoothCoefficient_ * (targetMasterGain - masterGain_);
            const float outLeft = (fxBuffers_[0][at] + subLeft) * masterGain_;
            const float outRight = (fxBuffers_[1][at] + subRight) * masterGain_;
            const int sample = start + offset;
            if (left != nullptr) left[sample] += right == nullptr ? (outLeft + outRight) * 0.5f : outLeft;
            if (right != nullptr) right[sample] += outRight;
        }
        start += frames;
    }
}
void WavetableSynth::renderPreparedVoices(float* left, float* right, int from, int to, int outputOffset,
                                        float* directLeft, float* directRight,
                                        float* bus1Left, float* bus1Right, float* bus2Left, float* bus2Right)
{
    if (voiceRateFactor_ == 1)
    {
        renderVoices(left, right, from, to, outputOffset, directLeft, directRight, bus1Left, bus1Right, bus2Left, bus2Right);
        return;
    }
    const std::array<float*, WavetableDownsampler::laneCount> outputs{left, right, bus1Left, bus1Right, bus2Left, bus2Right, directLeft, directRight};
    for (int start = from; start < to;)
    {
        const int frames = std::min(fxBlockCapacity_, to - start), internalFrames = frames * 2;
        for (auto& lane : internalVoiceBuffers_) std::fill_n(lane.data(), internalFrames, 0);
        renderVoices(internalVoiceBuffers_[0].data(), internalVoiceBuffers_[1].data(), start * 2, (start + frames) * 2, start * 2,
            directLeft ? internalVoiceBuffers_[6].data() : nullptr, directRight ? internalVoiceBuffers_[7].data() : nullptr,
            bus1Left ? internalVoiceBuffers_[2].data() : nullptr, bus1Right ? internalVoiceBuffers_[3].data() : nullptr,
            bus2Left ? internalVoiceBuffers_[4].data() : nullptr, bus2Right ? internalVoiceBuffers_[5].data() : nullptr);
        for (int frame = 0; frame < frames; ++frame)
        {
            WavetableDownsampler::Frame first{}, second{}, downsampled{};
            for (size_t lane = 0; lane < first.size(); ++lane)
            {
                first[lane] = internalVoiceBuffers_[lane][static_cast<size_t>(frame * 2)];
                second[lane] = internalVoiceBuffers_[lane][static_cast<size_t>(frame * 2 + 1)];
            }
            voiceDownsampler_.processFrame(first, second, downsampled);
            if (! right) downsampled[0] = (downsampled[0] + downsampled[1]) * .5f;
            const int at = start + frame - outputOffset;
            for (size_t lane = 0; lane < outputs.size(); ++lane) if (outputs[lane]) outputs[lane][at] += downsampled[lane];
        }
        start += frames;
    }
}
void WavetableSynth::renderVoices(float* left, float* right, int from, int to, int outputOffset,
                                 float* directSubLeft, float* directSubRight,
                                 float* bus1Left, float* bus1Right, float* bus2Left, float* bus2Right)
{
    const bool parallel = block_[FilterRouting] >= 0.5f;
    const bool sourceRouting = block_[SourceRoutingOn] >= .5f;
    for (int sample = from; sample < to; ++sample)
    {
        uint64_t carriedSubAge = 0;
        if (sampleClock_ % controlPeriod == 0)
        {
            for (int l = 0; l < kLfos; ++l)
            {
                const auto at = static_cast<size_t>(l);
                double phase = globalLfoPhase_[at];
                if (host_.playing && std::isfinite(host_.beats) && block_[static_cast<size_t>(lfoParam(l, SyncOn))] >= 0.5f
                    && (! importedLfoProfile_ || block_[static_cast<size_t>(lfoAnchorParam(l))] >= 0.5f))
                {
                    const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
                    phase = (host_.beats + sample * tempo / (60 * sampleRate_)) / divisionBeats(choice(block_[static_cast<size_t>(lfoParam(l, Division))], kLfoDivisions));
                }
                const float cycle = static_cast<float>(wrap(phase + (importedLfoProfile_ ? 0 : block_[static_cast<size_t>(lfoParam(l, LfoPhase))])));
                if (const int type = choice(block_[static_cast<size_t>(lfoTypeParam(l))], NumLfoTypes); type != LfoNormal)
                {
                    advanceChaos(globalChaos_[at], type, lfoIncrement_[at] * controlPeriod, chaosSeed_, globalHeld_[at]);
                    globalLfoValue_[at] = type == LfoRandomHold ? globalHeld_[at] : chaosPoint(globalChaos_[at], type)[0];
                }
                else globalLfoValue_[at] = importedLfoProfile_ ? importedLfoCurves_[at].eval(cycle) : curves_[at].eval(cycle);
            }
        }
        float sumLeft = 0, sumRight = 0, subSumLeft = 0, subSumRight = 0;
        std::array<std::array<float, 2>, 2> busSum{};
        for (auto& voice : voices_)
        {
            if (voice.note < 0) continue;
            if (monoActive_)
            {
                const double previousPitch = voice.pitchNote;
                voice.pitchNote = monoVoice_.nextPitch();
                if (voice.controlCountdown > 0 && std::abs(voice.pitchNote - previousPitch) > 1e-12)
                {
                    const double ratio = std::exp2((voice.pitchNote - previousPitch) / 12);
                    voice.subIncrement = std::min(0.45, voice.subIncrement * ratio);
                    for (auto& oscillator : voice.oscillator)
                    {
                        for (auto& increment : oscillator.increment) increment = std::min(0.45, increment * ratio);
                        oscillator.mediaControls.pitchSemitones += voice.pitchNote - previousPitch;
                    }
                }
            }
            for (int e = 0; e < kEnvelopes; ++e)
                nextEnvelope(voice.envelope[static_cast<size_t>(e)], e, voice.released,
                             voice.envelopeStagesModulated ? &voice.envelopeStages[static_cast<size_t>(e)] : nullptr);
            if (voice.envelope[0].stage == 0)
            {
                // Measured: a sample/multisample oscillator routed around Env 1 keeps the voice until its own release ends.
                bool mediaTail = false;
                if (sourceRouting && voice.released)
                    for (int o = 0; o < kOscillators && ! mediaTail; ++o)
                        mediaTail = voice.oscillator[static_cast<size_t>(o)].on && voice.oscillator[static_cast<size_t>(o)].mediaMode != WavetableMode
                            && ! sourceRoutes_[static_cast<size_t>(o)].envelope && (*mediaVoices_)[static_cast<size_t>(&voice - voices_.data())][static_cast<size_t>(o)].active();
                if (! mediaTail)
                {
                    if (voice.cutHeld && voice.cutReleasedAt < 0) addSameNoteGhost(voice.note, voice.channel);
                    voice = Voice{}; continue;
                }
            }
            if (voice.controlCountdown <= 0) updateControl(voice, sample);
            --voice.controlCountdown;
            std::array<float, 2> direct{}, input1{}, input2{}, directSub{};
            std::array<std::array<float, 2>, 2> sends{};
            const float envelopeLevel = voice.envelope[0].level;
            const float sourceEnvelope = importedEnvelopeProfile_ ? envelopeLevel : envelopeLevel * envelopeLevel;
            const auto routeSource = [&](int source, float signalLeft, float signalRight) {
                const auto& route = sourceRoutes_[static_cast<size_t>(source)];
                const float envelope = route.envelope ? sourceEnvelope : 1;
                const std::array<float, 2> signal{signalLeft, signalRight};
                for (size_t channel = 0; channel < signal.size(); ++channel)
                {
                    sends[0][channel] += signal[channel] * envelope * voice.sourceSends[static_cast<size_t>(source)][0];
                    sends[1][channel] += signal[channel] * envelope * voice.sourceSends[static_cast<size_t>(source)][1];
                    if (route.destination == RouteToMain) direct[channel] += signal[channel] * envelope;
                    else if (route.destination == RouteToDirect) directSub[channel] += signal[channel] * envelope * block_[DirectGain];
                    else if (route.destination == RouteToFilters && source < 5)
                    {
                        // Source Via Env is applied at the filter output, independently of this feed.
                        const float share = voice.filterShare[static_cast<size_t>(source)];
                        input1[channel] += signal[channel] * (1 - share);
                        input2[channel] += signal[channel] * share;
                    }
                }
            };
            const auto previousOscillator = voice.lastOscillator;
            const auto voiceIndex = static_cast<size_t>(&voice - voices_.data());
            // FM (Sub): the oscillator's phase increment gains K |amount|^4 times the raw Sub waveform (unit level, before the amplitude envelope and
            // the Sub volume; the Sub must be on). Measured on the reference: a frequency deviation that is constant in Hz (88198 Hz per unit at amount 1; table read gain 1.00316 included in m).
            float fmSub = 0;
            if (block_[SubOn] >= .5f)
            {
                bool wanted = false;
                for (const auto& oscillator : voice.oscillator)
                {
                    const auto first = sourcedWarp(oscillator.warp1), second = sourcedWarp(oscillator.warp2);
                    wanted = wanted || (oscillator.on && ((first.source == 2 && first.kind > 0) || (second.source == 2 && second.kind > 0)));
                }
                if (wanted)
                {
                    const int wave = choice(block_[SubWave], 4);
                    int mip = 0;
                    while (mip < tableMips - 1 && voice.subIncrement * (tableHarmonics >> mip) > 0.45) ++mip;
                    fmSub = tables_->sample(Basic, static_cast<float>(wave) / 3, mip, voice.subPhase) * (importedSubProfile_ && wave == 0 ? importedSubSineScale_ : 1);
                }
            }
            // Oscillators reading another oscillator through a sourced warp run after their source, so they see its current sample (measured: the reference
            // does not lag); cyclic sourcing falls back to the previous sample.
            std::array<int, kOscillators> order{};
            {
                int count = 0;
                for (int pass = 0; pass < 2; ++pass)
                    for (int o = 0; o < kOscillators; ++o)
                    {
                        const auto& control = voice.oscillator[static_cast<size_t>(o)];
                        const auto first = sourcedWarp(control.warp1), second = sourcedWarp(control.warp2);
                        const bool dependent = (first.kind > 0 && first.source < 2) || (second.kind > 0 && second.source < 2);
                        if (dependent == (pass == 1)) order[static_cast<size_t>(count++)] = o;
                    }
            }
            for (const int o : order)
            {
                const auto at = static_cast<size_t>(o);
                auto& control = voice.oscillator[at];
                if (! control.on) { voice.lastOscillator[at] = 0; continue; }
                float oscLeft = 0, oscRight = 0, mono = 0;
                // Adjacent modulation is a previous-sample signal, making cyclic routing deterministic and bounded.
                control.position += smoothCoefficient_ * (control.targetPosition - control.position);
                // Imported envelope-to-level trajectories already carry their prepared time law.
                // Own patches retain the original manual-control smoothing.
                if (importedDomains_[static_cast<size_t>(Osc1Level + o)] == ImportedModulationDomain::SquaredLevel)
                    control.level = control.targetLevel;
                else control.level += smoothCoefficient_ * (control.targetLevel - control.level);
                const float modulator = previousOscillator[static_cast<size_t>((o + 1) % kOscillators)];
                // Sourced warps (measured): FM adds K a^4 m / fs to the phase increment, PD adds 8 a^4 m cycles to the table phase, AM multiplies by 1 + 2 a m,
                // RM by (1 - a) + 2 a m; m is the source before its own level (the other oscillators in index order, or the raw Sub).
                double fmIncrement = 0, pdCycles = 0;
                float sourcedMultiplier = 1, selfCycles = 0;
                // Self PD (measured): the table phase gains 0.6 a^2 times the voice's own previous raw table value, one sample back at
                // the 2x voice rate (an effective 0.5-sample delay at 48 kHz).
                for (const auto [mode, amount] : {std::pair{control.warp1, control.warpAmount1}, std::pair{control.warp2, control.warpAmount2}})
                    if (mode == SelfPd) selfCycles += 0.6f * amount * std::abs(amount);
                for (const auto [mode, amount] : {std::pair{control.warp1, control.warpAmount1}, std::pair{control.warp2, control.warpAmount2}})
                {
                    const auto [kind, source] = sourcedWarp(mode);
                    if (kind == 0) continue;
                    const auto sourceIndex = static_cast<size_t>(source == 0 ? (o == 0 ? 1 : 0) : (o == 2 ? 1 : 2));
                    // A disabled source bypasses the warp (measured: the reference leaves the oscillator untouched).
                    if (source == 2 ? block_[SubOn] < .5f : ! voice.oscillator[sourceIndex].on) continue;
                    const float m = source == 2 ? fmSub : voice.lastOscillator[sourceIndex];
                    const double fourth = std::pow(std::abs(static_cast<double>(amount)), 4.0) * std::copysign(1.0, amount);
                    if (kind == 1) fmIncrement += fourth * 88198.0 * m / sampleRate_;
                    else if (kind == 2) pdCycles += 8.0 * fourth * (source == 2 ? m : 1.0 + m); // measured: an oscillator source shifts by 8 a^4 (1 + m) cycles, the raw Sub by 8 a^4 m
                    else if (kind == 3) sourcedMultiplier *= 1 + 2 * amount * m;
                    else sourcedMultiplier *= (1 - std::abs(amount)) + 2 * amount * m;
                }
                if (control.mediaMode != WavetableMode)
                {
                    auto& media = (*mediaVoices_)[voiceIndex][at];
                    if (voice.released && ! control.mediaReleased) { media.release(); control.mediaReleased = true; }
                    auto sound = media.next(control.mediaControls);
                    for (int w = 0; w < 2; ++w)
                    {
                        const int warp = w == 0 ? control.warp1 : control.warp2;
                        if (! isDistortionWarp(warp)) continue;
                        const float amount = w == 0 ? control.warpAmount1 : control.warpAmount2, scale = w == 0 ? control.distScale1 : control.distScale2;
                        sound.left = distortionWarp(warp, amount, sound.left, scale);
                        sound.right = distortionWarp(warp, amount, sound.right, scale);
                    }
                    float multiplier = 1;
                    if (control.warp1 == RingMod) multiplier *= 1 - std::abs(control.warpAmount1) + modulator * control.warpAmount1;
                    if (control.warp2 == RingMod) multiplier *= 1 - std::abs(control.warpAmount2) + modulator * control.warpAmount2;
                    oscLeft = sound.left * control.mediaLeft * multiplier;
                    oscRight = sound.right * control.mediaRight * multiplier;
                    mono = (sound.left + sound.right) * multiplier * 0.5f;
                }
                else for (int u = 0; u < control.count; ++u)
                {
                    const auto unison = static_cast<size_t>(u);
                    auto& phase = voice.phase[at][unison];
                    float warped = warpedPhase(static_cast<float>(phase), control.warp1, control.warpAmount1, modulator, importedVoiceProfile_);
                    warped = warpedPhase(warped, control.warp2, control.warpAmount2, modulator, importedVoiceProfile_);
                    if (pdCycles > 0 || pdCycles < 0) warped = static_cast<float>(wrap(static_cast<double>(warped) + pdCycles));
                    auto& feedback = voice.selfFeedback[at][unison];
                    if (selfCycles > 0 || selfCycles < 0) warped = static_cast<float>(wrap(static_cast<double>(warped) + selfCycles * feedback));
                    const auto* custom = oscillatorTable(o, control.table);
                    const bool octaveProfile = custom != nullptr && custom->playbackProfile() != WavetableData::PlaybackProfile::Dense12;
                    auto& transition = control.mipTransition[unison];
                    if (octaveProfile) transition.select(custom->mipForIncrement(control.increment[unison] * control.bandwidthScale));
                    const int mip = octaveProfile ? transition.current() : control.mip;
                    float soundLeft = custom != nullptr ? custom->sample(control.position, warped, mip, 0)
                        : tables_->sample(control.table, control.position, control.mip, warped);
                    float soundRight = custom != nullptr && custom->channels() == 2 ? custom->sample(control.position, warped, mip, 1) : soundLeft;
                    if (octaveProfile)
                    {
                        if (transition.active())
                        {
                            const float previousLeft = custom->sample(control.position, warped, transition.previous(), 0);
                            const float previousRight = custom->channels() == 2 ? custom->sample(control.position, warped, transition.previous(), 1) : previousLeft;
                            soundLeft = transition.blend(soundLeft, previousLeft);
                            soundRight = transition.blend(soundRight, previousRight);
                        }
                        transition.advance();
                    }
                    if (isDistortionWarp(control.warp1))
                    {
                        soundLeft = distortionWarp(control.warp1, control.warpAmount1, soundLeft, control.distScale1);
                        soundRight = distortionWarp(control.warp1, control.warpAmount1, soundRight, control.distScale1);
                    }
                    if (isDistortionWarp(control.warp2))
                    {
                        soundLeft = distortionWarp(control.warp2, control.warpAmount2, soundLeft, control.distScale2);
                        soundRight = distortionWarp(control.warp2, control.warpAmount2, soundRight, control.distScale2);
                    }
                    feedback = (soundLeft + soundRight) * 0.5f;
                    float multiplier = 1;
                    if (control.warp1 == RingMod) multiplier *= 1 - std::abs(control.warpAmount1) + modulator * control.warpAmount1;
                    if (control.warp2 == RingMod) multiplier *= 1 - std::abs(control.warpAmount2) + modulator * control.warpAmount2;
                    multiplier *= sourcedMultiplier;
                    soundLeft *= multiplier; soundRight *= multiplier;
                    mono += (soundLeft + soundRight) * 0.5f;
                    oscLeft += soundLeft * control.left[unison]; oscRight += soundRight * control.right[unison];
                    phase = wrap(phase + control.increment[unison] + fmIncrement);
                }
                for (int w = 0; w < 2; ++w)
                {
                    const int warp = w == 0 ? control.warp1 : control.warp2;
                    if (warp != FilterLpfWarp && warp != FilterHpfWarp) continue;
                    const float amount = w == 0 ? control.warpAmount1 : control.warpAmount2;
                    const float g = control.warpFilterG[static_cast<size_t>(w)];
                    auto& state = control.warpFilterState[static_cast<size_t>(w)];
                    for (int channel = 0; channel < 2; ++channel)
                    {
                        float& x = channel == 0 ? oscLeft : oscRight;
                        const float v = g * (x - state[static_cast<size_t>(channel)]), low = v + state[static_cast<size_t>(channel)];
                        state[static_cast<size_t>(channel)] = low + v;
                        x = warp == FilterHpfWarp ? x - low : (std::abs(amount) > 0 ? low : 0.0f);
                    }
                }
                voice.lastOscillator[at] = control.mediaMode == WavetableMode ? mono / static_cast<float>(control.count) : mono;
                const float signal[2]{oscLeft * control.level, oscRight * control.level};
                if (sourceRouting) { routeSource(o, signal[0], signal[1]); continue; }
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    if (! control.route1 && ! control.route2) direct[channel] += signal[channel];
                    else if (parallel)
                    {
                        const float weight = control.route1 && control.route2 ? 0.5f : 1;
                        if (control.route1) input1[channel] += signal[channel] * weight;
                        if (control.route2) input2[channel] += signal[channel] * weight;
                    }
                    else if (control.route1) input1[channel] += signal[channel];
                    else input2[channel] += signal[channel];
                }
            }
            if (voice.subLevel > 0)
            {
                const int wave = choice(block_[SubWave], 4);
                const float position = static_cast<float>(wave) / 3;
                int mip = 0;
                while (mip < tableMips - 1 && voice.subIncrement * (tableHarmonics >> mip) > 0.45) ++mip;
                // The imported sine's prepared waveform has a measured scale and polarity.
                // Other waveform families still use the independently authored bank pending calibration.
                const float scale = importedSubProfile_ && wave == 0 ? importedSubSineScale_ : 1;
                const float sub = tables_->sample(Basic, position, mip, voice.subPhase) * voice.subLevel * scale;
                const float subLeft = sub * voice.subLeft, subRight = sub * voice.subRight;
                if (sourceRouting) routeSource(4, subLeft, subRight);
                else if (block_[SubDirect] >= 0.5f)
                {
                    if (directSubLeft != nullptr) { directSub[0] += subLeft; directSub[1] += subRight; }
                    else { direct[0] += subLeft; direct[1] += subRight; }
                }
                else { input1[0] += subLeft; input1[1] += subRight; }
            }
            voice.subPhase = wrap(voice.subPhase + voice.subIncrement);
            if (block_[SubOn] >= .5f && block_[SubContinuous] >= .5f && voice.age >= carriedSubAge)
            {
                retainedSubPhase_ = voice.subPhase;
                carriedSubAge = voice.age;
            }
            const int noiseType = choice(block_[static_cast<size_t>(NoiseTypeParam)], NumNoiseTypes);
            if (noiseType == NoiseSampleType && noiseSample_ != nullptr && block_[NoiseOn] >= .5f)
            {
                const auto sound = voice.noiseSample.next(voice.noiseRatio);
                const float noiseLeft = sound.left * voice.noiseLevel * voice.noiseLeft;
                const float noiseRight = sound.right * voice.noiseLevel * voice.noiseRight;
                if (sourceRouting) routeSource(3, noiseLeft, noiseRight);
                else { input1[0] += noiseLeft; input1[1] += noiseRight; }
            }
            else if (voice.noiseLevel > 0 && (noiseType != NoiseSampleType || ! noiseSampleExpected_))
            {
                // Generated colours: white, pink (Kellet's three-pole economy filter), brown (leaky integrator) and Geiger clicks;
                // Color then tilts each one darker as before.
                float raw = randomSigned(voice.noiseSeed);
                if (noiseType == NoisePink)
                {
                    auto& b = voice.pinkNoise;
                    b[0] = 0.99765f * b[0] + raw * 0.0990460f; b[1] = 0.96300f * b[1] + raw * 0.2965164f; b[2] = 0.57000f * b[2] + raw * 1.0526913f;
                    raw = (b[0] + b[1] + b[2] + raw * 0.1848f) * 0.25f;
                }
                else if (noiseType == NoiseBrown) { voice.brownNoise = (voice.brownNoise + 0.02f * raw) / 1.02f; raw = voice.brownNoise * 3.5f; }
                else if (noiseType == NoiseGeiger)
                {
                    voice.geiger *= 0.55f;
                    if ((randomSigned(voice.noiseSeed) + 1) * 0.5f < 0.0025f) voice.geiger = raw < 0 ? -1.0f : 1.0f;
                    raw = voice.geiger;
                }
                const float white = raw;
                voice.noiseLow += 0.06f * (white - voice.noiseLow);
                const float noise = (white * (1 - block_[NoiseColor]) + voice.noiseLow * block_[NoiseColor] * 3) * voice.noiseLevel;
                if (sourceRouting) routeSource(3, noise * voice.noiseLeft, noise * voice.noiseRight);
                else { input1[0] += noise * voice.noiseLeft; input1[1] += noise * voice.noiseRight; }
            }
            float output[2]{};
            if (sourceRouting)
            {
                const auto filtered = [&](int filter, const std::array<float, 2>& input) {
                    const float level = block_[static_cast<size_t>(filterParam(filter, FilterOn))] >= .5f
                        ? voice.filterLevel[static_cast<size_t>(filter)] : 1;
                    return std::array<float, 2>{filterSample(voice, filter, 0, input[0]) * level,
                                              filterSample(voice, filter, 1, input[1]) * level};
                };
                const bool firstToSecond = sourceRoutes_[5].destination == RouteToFilters;
                const bool secondToFirst = sourceRoutes_[6].destination == RouteToFilters;
                if (firstToSecond && ! secondToFirst)
                {
                    const auto first = filtered(0, input1); routeSource(5, first[0], first[1]);
                    input2[0] += first[0]; input2[1] += first[1];
                    const auto second = filtered(1, input2); routeSource(6, second[0], second[1]);
                }
                else if (secondToFirst && ! firstToSecond)
                {
                    const auto second = filtered(1, input2); routeSource(6, second[0], second[1]);
                    input1[0] += second[0]; input1[1] += second[1];
                    const auto first = filtered(0, input1); routeSource(5, first[0], first[1]);
                }
                else
                {
                    // DEV-18d: cyclic filter routing is uncalibrated; keep bounded independent sends.
                    const auto first = filtered(0, input1), second = filtered(1, input2);
                    routeSource(5, first[0], first[1]); routeSource(6, second[0], second[1]);
                }
                output[0] = direct[0]; output[1] = direct[1];
                if (directSubLeft == nullptr) { output[0] += directSub[0]; output[1] += directSub[1]; }
            }
            else for (int channel = 0; channel < 2; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                const float first = filterSample(voice, 0, channel, input1[ch]);
                output[channel] = direct[ch] + (parallel ? first + filterSample(voice, 1, channel, input2[ch])
                                                        : filterSample(voice, 1, channel, first + input2[ch]));
            }
            voice.gain += smoothCoefficient_ * (voice.targetGain - voice.gain);
            const float amplitudeEnvelope = voice.envelope[0].level;
            const float amplitude = voice.gain * (sourceRouting ? 1 : importedEnvelopeProfile_ ? amplitudeEnvelope : amplitudeEnvelope * amplitudeEnvelope);
            const float outLeft = output[0] * amplitude * voice.panLeft, outRight = output[1] * amplitude * voice.panRight;
            if (! std::isfinite(outLeft) || ! std::isfinite(outRight)) { voice = Voice{}; continue; }
            sumLeft += outLeft; sumRight += outRight;
            subSumLeft += directSub[0] * amplitude * voice.panLeft;
            subSumRight += directSub[1] * amplitude * voice.panRight;
            if (sourceRouting)
                for (size_t bus = 0; bus < sends.size(); ++bus)
                {
                    busSum[bus][0] += sends[bus][0] * amplitude * voice.panLeft;
                    busSum[bus][1] += sends[bus][1] * amplitude * voice.panRight;
                }
            for (int l = 0; l < kLfos; ++l)
            {
                auto& phase = voice.lfoPhase[static_cast<size_t>(l)];
                phase += voice.lfoIncrement[static_cast<size_t>(l)];
                if (choice(block_[static_cast<size_t>(lfoParam(l, Mode))], 3) == OneShot) phase = std::min(1.0, phase);
                else phase = wrap(phase);
            }
            ++voice.noteSamples;
        }
        const int output = sample - outputOffset;
        if (sourceRouting && bus1Left == nullptr)
        {
            const float return1 = (effects_ != nullptr ? effects_->getRoutingParameter(Bus1ToMain) : 1)
                * (fxReturnBeforeMain_[0] ? 1 : block_[DirectGain]);
            const float return2 = (effects_ != nullptr ? effects_->getRoutingParameter(Bus2ToMain) : 1)
                * (fxReturnBeforeMain_[1] ? 1 : block_[DirectGain]);
            sumLeft += busSum[0][0] * return1 + busSum[1][0] * return2;
            sumRight += busSum[0][1] * return1 + busSum[1][1] * return2;
        }
        if (left != nullptr) left[output] += right == nullptr ? (sumLeft + sumRight) * 0.5f : sumLeft;
        if (right != nullptr) right[output] += sumRight;
        if (directSubLeft != nullptr) directSubLeft[output] += subSumLeft;
        if (directSubRight != nullptr) directSubRight[output] += subSumRight;
        if (bus1Left != nullptr) bus1Left[output] += busSum[0][0];
        if (bus1Right != nullptr) bus1Right[output] += busSum[0][1];
        if (bus2Left != nullptr) bus2Left[output] += busSum[1][0];
        if (bus2Right != nullptr) bus2Right[output] += busSum[1][1];
        for (int l = 0; l < kLfos; ++l)
            globalLfoPhase_[static_cast<size_t>(l)] = wrap(globalLfoPhase_[static_cast<size_t>(l)] + lfoIncrement_[static_cast<size_t>(l)]);
        ++sampleClock_;
    }
}

void WavetableSynth::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    snapshot();
    // Controlled Free/Hz/Anchor-off captures seed new voices from the processing clock
    // advanced by the actual block length; host-seeded modes use the block start instead.
    for (int lfo = 0; lfo < kLfos; ++lfo)
        processingEndLfoPhase_[static_cast<size_t>(lfo)] = wrap(globalLfoPhase_[static_cast<size_t>(lfo)]
            + audio.getNumSamples() * voiceRateFactor_ * lfoIncrement_[static_cast<size_t>(lfo)]);
    const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
    if (importedLfoProfile_ && hasHostClock_ && host_.playing && std::isfinite(host_.beats))
    {
        const double jump = host_.beats - expectedHostBeat_;
        if (std::abs(jump) > tempo / (30 * hostSampleRate_))
            for (int lfo = 0; lfo < kLfos; ++lfo)
                if (block_[static_cast<size_t>(lfoAnchorParam(lfo))] >= 0.5f
                    && block_[static_cast<size_t>(lfoParam(lfo, SyncOn))] >= 0.5f
                    && choice(block_[static_cast<size_t>(lfoParam(lfo, Mode))], 3) == Retrigger)
                {
                    const double cycles = jump / divisionBeats(choice(block_[static_cast<size_t>(lfoParam(lfo, Division))], kLfoDivisions));
                    for (auto& voice : voices_) if (voice.note >= 0)
                    {
                        voice.lfoPhase[static_cast<size_t>(lfo)] = wrap(voice.lfoPhase[static_cast<size_t>(lfo)] + cycles);
                    }
                }
    }
    hasHostClock_ = host_.playing && std::isfinite(host_.beats);
    expectedHostBeat_ = host_.beats + audio.getNumSamples() * tempo / (60 * hostSampleRate_);
    if (effects_ != nullptr) effects_->setTempo(host_.tempo);
    const bool bypass = isBypassed();
    if (bypass != wasBypassed_)
    {
        voices_ = {};
        monoVoice_.reset();
        for (auto& row : *mediaVoices_) for (auto& media : row) media.reset();
        resetEffects();
        activeVoiceCount_.store(0, std::memory_order_relaxed);
    }
    wasBypassed_ = bypass;
    const bool arpOn = ! bypass && block_[static_cast<size_t>(arpParam(ArpOn))] >= 0.5f;
    if (! arpOn && arp_.active) arpStop();
    arp_.active = arpOn;
    // Editor notes/controllers first, at the block start, through the same input path as the track's MIDI.
    const uint32_t queued = uiMidiWrite_.load(std::memory_order_acquire);
    uint32_t read = uiMidiRead_.load(std::memory_order_relaxed);
    for (; read != queued; ++read)
    {
        const uint32_t packed = uiMidi_[read % kUiMidiCapacity].load(std::memory_order_relaxed);
        const uint8_t bytes[3]{static_cast<uint8_t>(packed >> 16), static_cast<uint8_t>((packed >> 8) & 0x7f), static_cast<uint8_t>(packed & 0x7f)};
        const int size = (bytes[0] & 0xf0) == 0xc0 || (bytes[0] & 0xf0) == 0xd0 ? 2 : 3;
        input(bytes, size);
    }
    uiMidiRead_.store(read, std::memory_order_release);
    int position = 0;
    for (const auto meta : midi)
    {
        renderSpan(audio, position, std::clamp(meta.samplePosition, position, audio.getNumSamples()), bypass);
        if (meta.numBytes <= 3) input(meta.data, meta.numBytes);
    }
    renderSpan(audio, position, audio.getNumSamples(), bypass);
    unsigned count = 0;
    std::array<uint32_t, 4> lit{};
    for (const auto& voice : voices_)
    {
        if (voice.note < 0) continue;
        ++count;
        if (voice.keyDown) lit[static_cast<size_t>(voice.note >> 5)] |= 1u << (voice.note & 31);
    }
    for (int i = 0; i < arp_.count; ++i) lit[static_cast<size_t>(arp_.notes[static_cast<size_t>(i)] >> 5)] |= 1u << (arp_.notes[static_cast<size_t>(i)] & 31);
    for (size_t i = 0; i < lit.size(); ++i) litNotes_[i].store(lit[i], std::memory_order_relaxed);
    wheelDisplay_.store(wheels_[0], std::memory_order_relaxed);
    bendDisplay_.store(bends_[0], std::memory_order_relaxed);
    arpNoteDisplay_.store(arp_.sounding, std::memory_order_relaxed);
    publishLiveState(audio);
    activeVoiceCount_.store(count, std::memory_order_relaxed);
}

void WavetableSynth::publishLiveState(const juce::AudioBuffer<float>& audio)
{
    const Voice* focus = nullptr;
    for (const auto& voice : voices_)
        if (voice.note >= 0 && (focus == nullptr || voice.age > focus->age)) focus = &voice;
    for (int channel = 0; channel < 2; ++channel)
    {
        float peak = 0;
        if (channel < audio.getNumChannels())
        {
            const float* data = audio.getReadPointer(channel);
            for (int i = 0; i < audio.getNumSamples(); ++i) peak = std::max(peak, std::abs(data[i]));
        }
        liveOutputPeak_[static_cast<size_t>(channel)].store(peak, std::memory_order_relaxed);
    }
    for (auto& value : liveRoute_) value.store(0, std::memory_order_relaxed);
    if (focus != nullptr)
        for (int i = 0; i < routeCount_; ++i)
        {
            const auto& route = routes_[static_cast<size_t>(i)];
            liveRoute_[static_cast<size_t>(route.slot)].store(routeValue(*focus, route), std::memory_order_relaxed);
        }
    for (int e = 0; e < kEnvelopes; ++e)
    {
        const auto at = static_cast<size_t>(e);
        const auto* state = focus != nullptr ? &focus->envelope[at] : nullptr;
        liveEnvelopeStage_[at].store(state != nullptr ? state->stage : 0, std::memory_order_relaxed);
        liveEnvelopeElapsed_[at].store(state != nullptr ? static_cast<float>(state->elapsed) : 0, std::memory_order_relaxed);
        liveEnvelopeLevel_[at].store(state != nullptr ? state->level : 0, std::memory_order_relaxed);
    }
    for (int l = 0; l < kLfos; ++l)
    {
        const auto at = static_cast<size_t>(l);
        const int mode = choice(block_[static_cast<size_t>(lfoParam(l, Mode))], 3);
        const float offset = block_[static_cast<size_t>(lfoParam(l, LfoPhase))];
        double phase = globalLfoPhase_[at];
        if (focus != nullptr && mode != Global)
            phase = mode == OneShot ? std::min(1.0, focus->lfoPhase[at] + offset) : wrap(focus->lfoPhase[at] + offset);
        liveLfoPhase_[at].store(static_cast<float>(phase), std::memory_order_relaxed);
        liveLfoValue_[at].store(focus != nullptr ? focus->lfoValue[at] : globalLfoValue_[at], std::memory_order_relaxed);
        const int type = choice(block_[static_cast<size_t>(lfoTypeParam(l))], NumLfoTypes);
        if (type == LfoLorenz || type == LfoRossler)
        {
            const auto point = chaosPoint(focus != nullptr && mode != Global ? focus->chaos[at] : globalChaos_[at], type);
            liveLfoPointX_[at].store(point[0], std::memory_order_relaxed); liveLfoPointY_[at].store(point[1], std::memory_order_relaxed);
        }
    }
    liveNote_.store(focus != nullptr ? focus->note : -1, std::memory_order_relaxed);
    liveVelocity_.store(focus != nullptr ? focus->velocity : 0, std::memory_order_relaxed);
    liveActive_.store(focus != nullptr, std::memory_order_relaxed);
}
WavetableSynth::LiveState WavetableSynth::liveState() const noexcept
{
    LiveState state;
    state.active = liveActive_.load(std::memory_order_relaxed);
    state.note = liveNote_.load(std::memory_order_relaxed);
    state.velocity = liveVelocity_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < state.route.size(); ++i) state.route[i] = liveRoute_[i].load(std::memory_order_relaxed);
    for (size_t e = 0; e < state.envelopeStage.size(); ++e)
    {
        state.envelopeStage[e] = liveEnvelopeStage_[e].load(std::memory_order_relaxed);
        state.envelopeElapsed[e] = liveEnvelopeElapsed_[e].load(std::memory_order_relaxed);
        state.envelopeLevel[e] = liveEnvelopeLevel_[e].load(std::memory_order_relaxed);
    }
    for (size_t l = 0; l < state.lfoPhase.size(); ++l)
    {
        state.lfoPhase[l] = liveLfoPhase_[l].load(std::memory_order_relaxed);
        state.lfoValue[l] = liveLfoValue_[l].load(std::memory_order_relaxed);
        state.lfoPoint[l] = {liveLfoPointX_[l].load(std::memory_order_relaxed), liveLfoPointY_[l].load(std::memory_order_relaxed)};
    }
    for (size_t c = 0; c < 2; ++c) state.outputPeak[c] = liveOutputPeak_[c].load(std::memory_order_relaxed);
    return state;
}
float WavetableSynth::waveformAt(int oscillator, float phase, float position, float warp1, float warp2) const
{
    if (oscillator < 0 || oscillator >= kOscillators || ! std::isfinite(phase)) return 0;
    const int table = choice(value(oscParam(oscillator, Table)), kTableFamilies);
    float p = static_cast<float>(wrap(phase));
    p = warpedPhase(p, choice(value(oscParam(oscillator, Warp1Mode)), NumWarps), std::clamp(warp1, -1.0f, 1.0f), 0);
    p = warpedPhase(p, choice(value(oscParam(oscillator, Warp2Mode)), NumWarps), std::clamp(warp2, -1.0f, 1.0f), 0);
    const auto* data = oscillatorTable(oscillator, table);
    float sample = data != nullptr ? data->sample(unit(position), p, 0, 0) : tables_->sample(table, unit(position), 0, p);
    for (const auto& [mode, amount] : {std::pair{choice(value(oscParam(oscillator, Warp1Mode)), NumWarps), warp1},
                                       std::pair{choice(value(oscParam(oscillator, Warp2Mode)), NumWarps), warp2}})
        sample = distortionWarp(mode, amount, sample, distortionWarpScale(mode, amount));
    return sample;
}
float WavetableSynth::tableFramePosition(int oscillator, float position) const
{
    if (oscillator < 0 || oscillator >= kOscillators) return 0;
    position = unit(position);
    const int table = choice(value(oscParam(oscillator, Table)), kTableFamilies);
    const auto* data = oscillatorTable(oscillator, table);
    const int frames = data != nullptr ? data->frames() : tableFrames;
    if (data != nullptr && data->frameLayout() == WavetableFrameLayout::Steps)
        return static_cast<float>(std::min(static_cast<int>(position * static_cast<float>(frames)), frames - 1));
    return position * static_cast<float>(frames - 1);
}
namespace
{
float piecewise(float input, float start, float midX, float midY, float end)
{
    const float x = unit(input), mx = std::clamp(midX, 0.001f, 0.999f);
    return unit(x <= mx ? start + (midY - start) * x / mx : midY + (end - midY) * (x - mx) / (1 - mx));
}
} // namespace
void WavetableSynth::setImportedWarpLabel(int oscillator, int warp, WarpLabel label)
{
    if (oscillator >= 0 && oscillator < kOscillators && warp >= 0 && warp < 2)
        warpLabels_[static_cast<size_t>(oscillator)][static_cast<size_t>(warp)] = std::move(label);
}
WavetableSynth::WarpLabel WavetableSynth::importedWarpLabel(int oscillator, int warp) const
{
    if (oscillator < 0 || oscillator >= kOscillators || warp < 0 || warp > 1) return {};
    return warpLabels_[static_cast<size_t>(oscillator)][static_cast<size_t>(warp)];
}
float WavetableSynth::curveSource(int curve, float input) const noexcept
{
    const auto at = [this, curve](CurveSourceParam p) { return block_[static_cast<size_t>(curveSourceParam(curve, p))]; };
    return piecewise(input, at(CurveStart), at(CurveMidX), at(CurveMidY), at(CurveEnd));
}
float WavetableSynth::curveSourceAt(int curve, float input) const
{
    if (curve < 0 || curve >= CurveSources) return 0;
    const auto at = [this, curve](CurveSourceParam p) { return value(curveSourceParam(curve, p)); };
    return piecewise(input, at(CurveStart), at(CurveMidX), at(CurveMidY), at(CurveEnd));
}
bool WavetableSynth::queueUiMidi(uint8_t status, uint8_t data1, uint8_t data2) noexcept
{
    const uint32_t write = uiMidiWrite_.load(std::memory_order_relaxed);
    if (write - uiMidiRead_.load(std::memory_order_acquire) >= kUiMidiCapacity) return false;
    uiMidi_[write % kUiMidiCapacity].store((static_cast<uint32_t>(status) << 16) | (static_cast<uint32_t>(data1 & 0x7f) << 8) | (data2 & 0x7fu),
                                           std::memory_order_relaxed);
    uiMidiWrite_.store(write + 1, std::memory_order_release);
    return true;
}
bool WavetableSynth::noteLit(int note) const noexcept
{
    if (note < 0 || note > 127) return false;
    return (litNotes_[static_cast<size_t>(note >> 5)].load(std::memory_order_relaxed) >> (note & 31)) & 1u;
}
void WavetableSynth::renderSpan(juce::AudioBuffer<float>& audio, int& position, int to, bool bypass)
{
    while (position < to)
    {
        int next = to;
        if (arp_.active && (arp_.count > 0 || arp_.sounding >= 0))
        {
            double until = arp_.count > 0 ? arp_.untilStep : 1e18;
            if (arp_.untilOff >= 0) until = std::min(until, arp_.untilOff);
            if (until < static_cast<double>(to - position)) next = position + std::max(0, static_cast<int>(std::ceil(until)));
        }
        if (! bypass && next > position) render(audio, position, next);
        if (arp_.active) arpAdvance(static_cast<double>(next - position));
        position = next;
    }
}
bool WavetableSynth::arpInput(const uint8_t* data, int size)
{
    if (size != 3 || data == nullptr) return false;
    const uint8_t kind = data[0] & 0xf0u;
    if (kind == 0xb0 && (data[1] == 120 || data[1] == 123)) { arpStop(); return false; }
    const bool on = kind == 0x90 && data[2] > 0, off = kind == 0x80 || (kind == 0x90 && data[2] == 0);
    if (! on && ! off) return false;
    const uint8_t note = data[1] & 0x7f;
    int found = -1;
    for (int i = 0; i < arp_.count; ++i) if (arp_.notes[static_cast<size_t>(i)] == note) found = i;
    if (on)
    {
        if (found >= 0) { arp_.velocities[static_cast<size_t>(found)] = data[2]; return true; }
        if (arp_.count >= kArpHeld) return true;
        if (arp_.count == 0) { arp_.step = 0; arp_.untilStep = 0; } // a new chord starts on the next sample
        arp_.notes[static_cast<size_t>(arp_.count)] = note; arp_.velocities[static_cast<size_t>(arp_.count)] = data[2]; ++arp_.count;
        return true;
    }
    if (found < 0) return false; // a key pressed before the arpeggiator was enabled releases its own voice
    for (int i = found; i + 1 < arp_.count; ++i)
    {
        arp_.notes[static_cast<size_t>(i)] = arp_.notes[static_cast<size_t>(i + 1)];
        arp_.velocities[static_cast<size_t>(i)] = arp_.velocities[static_cast<size_t>(i + 1)];
    }
    if (--arp_.count == 0 && arp_.sounding >= 0) arp_.untilOff = 0;
    return true;
}
void WavetableSynth::arpAdvance(double samples)
{
    arp_.untilStep -= samples;
    if (arp_.untilOff >= 0) arp_.untilOff = std::max(0.0, arp_.untilOff - samples);
    if (arp_.untilOff >= 0 && arp_.untilOff < 0.5 && arp_.sounding >= 0)
    {
        const uint8_t off[3]{0x80, static_cast<uint8_t>(arp_.sounding), 0};
        event(off, 3); arp_.sounding = -1; arp_.untilOff = -1;
    }
    if (arp_.count > 0 && arp_.untilStep < 0.5) arpStep();
    if (arp_.count == 0) arp_.untilStep = 0;
}
void WavetableSynth::arpStep()
{
    const double tempo = std::isfinite(host_.tempo) ? std::clamp(host_.tempo, 1.0, 1000.0) : 120;
    const double stepSamples = std::max(1.0, arpRateBeats(choice(block_[static_cast<size_t>(arpParam(ArpRate))], kArpRates)) * 60 / tempo * hostSampleRate_);
    const int octaves = std::clamp(static_cast<int>(std::lround(block_[static_cast<size_t>(arpParam(ArpOctaves))])), 1, 4);
    const int mode = choice(block_[static_cast<size_t>(arpParam(ArpMode))], NumArpPatterns);
    const int count = arp_.count, length = count * octaves;
    std::array<uint8_t, kArpHeld> order{};
    std::array<uint8_t, kArpHeld> velocity{};
    for (int i = 0; i < count; ++i) { order[static_cast<size_t>(i)] = arp_.notes[static_cast<size_t>(i)]; velocity[static_cast<size_t>(i)] = arp_.velocities[static_cast<size_t>(i)]; }
    if (mode != ArpAsPlayed)
        for (int i = 1; i < count; ++i) // insertion sort by pitch, keeping each key's velocity
            for (int j = i; j > 0 && order[static_cast<size_t>(j)] < order[static_cast<size_t>(j - 1)]; --j)
            { std::swap(order[static_cast<size_t>(j)], order[static_cast<size_t>(j - 1)]); std::swap(velocity[static_cast<size_t>(j)], velocity[static_cast<size_t>(j - 1)]); }
    int index = 0;
    const int period = std::max(1, 2 * length - 2);
    switch (mode)
    {
        case ArpDown: index = length - 1 - arp_.step % length; break;
        case ArpUpDown: case ArpDownUp:
        {
            const int k = length > 1 ? arp_.step % period : 0;
            index = k < length ? k : period - k;
            if (mode == ArpDownUp) index = length - 1 - index;
            break;
        }
        case ArpRandom:
            arp_.seed = arp_.seed * 1664525u + 1013904223u;
            index = static_cast<int>((arp_.seed >> 8) % static_cast<uint32_t>(length));
            break;
        default: index = arp_.step % length; break;
    }
    const auto key = static_cast<size_t>(index % count);
    const int note = std::min(127, order[key] + 12 * (index / count));
    if (arp_.sounding >= 0) { const uint8_t off[3]{0x80, static_cast<uint8_t>(arp_.sounding), 0}; event(off, 3); }
    const uint8_t on[3]{0x90, static_cast<uint8_t>(note), std::max<uint8_t>(1, velocity[key])};
    event(on, 3);
    arp_.sounding = note;
    ++arp_.step;
    arp_.untilStep += stepSamples;
    if (arp_.untilStep < 0.5) arp_.untilStep = stepSamples; // a stalled clock never fires twice in one sample
    const float gate = std::clamp(block_[static_cast<size_t>(arpParam(ArpGate))], 0.05f, 1.0f);
    arp_.untilOff = gate < 0.999f ? gate * stepSamples : -1; // a full gate holds until the next step (legato)
}
void WavetableSynth::arpStop()
{
    if (arp_.sounding >= 0) { const uint8_t off[3]{0x80, static_cast<uint8_t>(arp_.sounding), 0}; event(off, 3); }
    const auto seed = arp_.seed;
    arp_ = ArpState{}; arp_.seed = seed;
}

float WavetableSynth::subWaveformAt(float phase) const
{
    if (! std::isfinite(phase)) return 0;
    const int wave = choice(value(SubWave), 4);
    const double offset = value(SubContinuous) >= .5f ? 0 : value(SubPhase);
    const float scale = importedSubProfile_ && wave == 0 ? importedSubSineScale_ : 1;
    return tables_->sample(Basic, static_cast<float>(wave) / 3, 0, wrap(phase + offset)) * scale;
}
float WavetableSynth::waveformAt(int oscillator, float phase) const
{
    return (waveformAt(oscillator, phase, 0) + waveformAt(oscillator, phase, 1)) * 0.5f;
}
float WavetableSynth::waveformAt(int oscillator, float phase, int channel) const
{
    if (oscillator < 0 || oscillator >= kOscillators || ! std::isfinite(phase)) return 0;
    if (choice(value(mediaParam(oscillator, MediaMode)), NumOscillatorModes) != WavetableMode)
    {
        const auto* media = oscillatorMedia(oscillator);
        if (media == nullptr || media->zones().empty()) return 0;
        const auto& zone = media->zones().front();
        const double position = static_cast<double>(zone.offset) + unit(phase) * static_cast<double>(zone.end - zone.offset - 1);
        if (channel < 0) return 0.5f * (zone.sample->sampleLinear(position, 0) + zone.sample->sampleLinear(position, 1));
        return zone.sample->sampleLinear(position, channel);
    }
    const int table = choice(value(oscParam(oscillator, Table)), kTableFamilies);
    float p = static_cast<float>(wrap(phase));
    p = warpedPhase(p, choice(value(oscParam(oscillator, Warp1Mode)), NumWarps), value(oscParam(oscillator, Warp1Amount)), 0);
    p = warpedPhase(p, choice(value(oscParam(oscillator, Warp2Mode)), NumWarps), value(oscParam(oscillator, Warp2Amount)), 0);
    const auto* custom = oscillatorTable(oscillator, table);
    if (custom != nullptr) return custom->sample(value(oscParam(oscillator, Position)), p, 0, channel);
    return tables_->sample(table, value(oscParam(oscillator, Position)), 0, p);
}
std::array<float, WavetableSynth::kPreviewSamples> WavetableSynth::wavePreview(int oscillator) const
{
    std::array<float, kPreviewSamples> out{};
    for (int i = 0; i < kPreviewSamples; ++i) out[static_cast<size_t>(i)] = waveformAt(oscillator, static_cast<float>(i) / kPreviewSamples);
    return out;
}
std::vector<int> WavetableSynth::displayParameters() const
{
    return {Volume, oscParam(0, Table), oscParam(0, Position), oscParam(0, Level), oscParam(0, Unison), oscParam(0, Detune),
            filterParam(0, Cutoff), filterParam(0, Resonance), envParam(0, Attack), envParam(0, Decay), envParam(0, Sustain), envParam(0, Release)};
}
std::vector<DeviceProcessor::UiGroup> WavetableSynth::uiGroups() const
{
    // The dedicated editor exposes every oscillator/envelope/LFO/route. This is a concise fallback panel.
    return {{"Main", {Volume, Transpose, Pan, SubOn, SubLevel, NoiseOn, NoiseLevel}, 4},
            {"Oscillator A", {oscParam(0, On), oscParam(0, Table), oscParam(0, Position), oscParam(0, Level), oscParam(0, Unison), oscParam(0, Detune), oscParam(0, Blend), oscParam(0, Width)}, 4},
            {"Filter 1", {filterParam(0, FilterOn), filterParam(0, FilterMode), filterParam(0, Cutoff), filterParam(0, Resonance), filterParam(0, Drive), filterParam(0, EnvAmount)}, 3},
            {"Amplitude", {envParam(0, Attack), envParam(0, Decay), envParam(0, Sustain), envParam(0, Release)}, 2}};
}
bool WavetableSynth::parameterShown(int index) const
{
    if (index >= LfoStart && index < ModStart)
    {
        const int lfo = (index - LfoStart) / LfoParams, field = (index - LfoStart) % LfoParams;
        const bool synced = value(lfoParam(lfo, SyncOn)) >= 0.5f;
        if (field == Rate) return ! synced;
        if (field == Division) return synced;
    }
    return true;
}

} // namespace sonora::devices
