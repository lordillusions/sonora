#include "devices/native/WavetableInstrument.h"

#include "core/WavetableFormat.h"
#include "dsp/Dsp.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
using W = WavetableInstrument;
constexpr double kTwoPi = 2.0 * dsp::kPi;

const auto& kTargetIds = model::wavetable::kTargetIds;
const auto& kTargetNames = model::wavetable::kTargetNames;
// Effect parameter labels per mode (None, FM, Classic, Modern), as the reference names its matrix rows.
constexpr const char* kFxNames[4][2] = {{"FX 1", "FX 2"}, {"FM Pitch", "FM Amt"}, {"PW", "Sync"}, {"Warp", "Fold"}};
constexpr const char* kSourceNames[W::kSources] = {"Amp", "Env 2", "Env 3", "LFO 1", "LFO 2", "Velocity", "Note", "Pitch Bend",
                                                    "Pressure", "Mod Wheel", "Random", "Note PB", "Slide"};
// LFO SyncedRate (22 entries, 8 bars .. 1/64). The order is assumed (DEV-28b2): the presets use indices 0-21.
constexpr const char* kSyncedNames[W::kSyncedRates] = {"8", "6", "4", "3", "2", "1.5", "1", "3/4", "1/2", "3/8", "1/3",
                                                        "5/16", "1/4", "3/16", "1/6", "1/8", "1/12", "1/16", "1/24", "1/32",
                                                        "1/48", "1/64"};
constexpr double kSyncedBeats[W::kSyncedRates] = {32, 24, 16, 12, 8, 6, 4, 3, 2, 1.5, 4.0 / 3, 1.25, 1, 0.75, 2.0 / 3, 0.5,
                                                  1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 0.0625};

// Source polarity: bipolar sources swing -1..1 (LFOs, bends, note), the rest 0..1.
bool bipolar(int source)
{
    return source == W::SrcLfo1 || source == W::SrcLfo2 || source == W::SrcPitchBend || source == W::SrcNotePb || source == W::SrcNote;
}

std::vector<ParameterInfo> wavetableParameters()
{
    std::vector<ParameterInfo> p((size_t) W::NumParams);
    const std::vector<std::string> onOff;
    for (int osc = 0; osc < 2; ++osc)
    {
        const std::string o = "Voice_Oscillator" + std::to_string(osc + 1) + "_";
        const int base = osc == 0 ? W::Osc1On : W::Osc2On;
        p[(size_t) base + 0] = makeParam(o + "On", "Osc " + std::to_string(osc + 1), 0, 1, osc == 0 ? 1.0f : 0.0f, "", true);
        p[(size_t) base + 1] = makeParam(o + "Pitch_Transpose", "Semi", -48, 48, 0, "st");
        p[(size_t) base + 2] = makeParam(o + "Pitch_Detune", "Det", -1, 1, 0, "st");
        p[(size_t) base + 3] = makeParam(o + "Wavetables_WavePosition", "Position", 0, 1, 0, "%");
        p[(size_t) base + 4] = makeParam(o + "Effects_Effect1", "FX 1", -1, 1, 0, "%");
        p[(size_t) base + 5] = makeParam(o + "Effects_Effect2", "FX 2", 0, 1, 0, "%");
        p[(size_t) base + 6] = makeParam(o + "Pan", "Pan", -1, 1, 0);
        p[(size_t) base + 7] = makeParam(o + "Gain", "Gain", 0, 1, 1, "lin");
        p[(size_t) (osc == 0 ? W::Osc1EffectMode : W::Osc2EffectMode)] =
            makePlainParam(makeParam(o + "Effects_EffectMode", "Effect", 0, 3, 0, "", false, {"None", "FM", "Classic", "Modern"}));
    }
    p[W::SubOn] = makeParam("Voice_SubOscillator_On", "Sub", 0, 1, 0, "", true);
    p[W::SubTone] = makeParam("Voice_SubOscillator_Tone", "Tone", 0, 1, 0, "%");
    p[W::SubGain] = makeParam("Voice_SubOscillator_Gain", "Gain", 0, 1, 0.5011875f, "lin");
    p[W::SubTranspose] = makeParam("Voice_SubOscillator_Transpose", "Octave", 0, 2, 1, "", false, {"0", "-1", "-2"});
    for (int f = 0; f < 2; ++f)
    {
        const std::string s = "Voice_Filter" + std::to_string(f + 1) + "_";
        const int base = f == 0 ? W::F1On : W::F2On;
        p[(size_t) base + 0] = makeParam(s + "On", "Filter " + std::to_string(f + 1), 0, 1, f == 0 ? 1.0f : 0.0f, "", true);
        p[(size_t) base + 1] = makeParam(s + "Type", "Type", 0, 4, f == 0 ? 0.0f : 1.0f, "", false, {"Lowpass", "Highpass", "Bandpass", "Notch", "Morph"});
        p[(size_t) base + 2] = makeParam(s + "CircuitLpHp", "Circuit", 0, 4, 0, "", false, {"Clean", "OSR", "MS2", "SMP", "PRD"});
        p[(size_t) base + 3] = makeParam(s + "CircuitBpNoMo", "Circuit", 0, 1, 0, "", false, {"Clean", "OSR"});
        p[(size_t) base + 4] = makeParam(s + "Slope", "Slope", 0, 1, 0, "", false, {"12", "24"});
        p[(size_t) base + 5] = makeParam(s + "Frequency", "Frequency", 20, 22000, f == 0 ? 20480.0f : 20.0f, "Hz");
        p[(size_t) base + 6] = makeParam(s + "Resonance", "Res", 0, 1.25f, 0, "%");
        p[(size_t) base + 7] = makeParam(s + "Drive", "Drive", 0, 24, 0, "dB");
        p[(size_t) base + 8] = makeParam(s + "Morph", "Morph", 0, 1, 0, "%");
    }
    const std::string m = "Voice_Modulators_";
    const std::string a = m + "AmpEnvelope_";
    p[W::AmpAttack] = makeParam(a + "Times_Attack", "A", 0, 20, 0.001f, "s");
    p[W::AmpDecay] = makeParam(a + "Times_Decay", "D", 0.001f, 20, 0.6f, "s");
    p[W::AmpRelease] = makeParam(a + "Times_Release", "R", 0.001f, 20, 0.6f, "s");
    p[W::AmpSlopeAttack] = makeParam(a + "Slopes_Attack", "A Slope", -1, 1, 0);
    p[W::AmpSlopeDecay] = makeParam(a + "Slopes_Decay", "D Slope", -1, 1, 0.5f);
    p[W::AmpSlopeRelease] = makeParam(a + "Slopes_Release", "R Slope", -1, 1, 0.5f);
    p[W::AmpSustain] = makeParam(a + "Sustain", "S", 0, 1, 0.5011876f, "lin");
    p[W::AmpLoop] = makeParam(a + "LoopMode", "Loop", 0, 2, 0, "", false, {"None", "Trigger", "Loop"});
    for (int e = 0; e < 2; ++e)
    {
        const std::string s = m + "Envelope" + std::to_string(e + 2) + "_";
        const int base = e == 0 ? W::E2Attack : W::E3Attack;
        p[(size_t) base + 0] = makeParam(s + "Times_Attack", "A", 0, 20, 0.001f, "s");
        p[(size_t) base + 1] = makeParam(s + "Times_Decay", "D", 0.001f, 20, 0.6f, "s");
        p[(size_t) base + 2] = makeParam(s + "Times_Release", "R", 0.001f, 20, 0.6f, "s");
        p[(size_t) base + 3] = makeParam(s + "Slopes_Attack", "A Slope", -1, 1, 0);
        p[(size_t) base + 4] = makeParam(s + "Slopes_Decay", "D Slope", -1, 1, 0.5f);
        p[(size_t) base + 5] = makeParam(s + "Slopes_Release", "R Slope", -1, 1, 0.5f);
        p[(size_t) base + 6] = makeParam(s + "Values_Initial", "Initial", -1, 1, 0, "%");
        p[(size_t) base + 7] = makeParam(s + "Values_Peak", "Peak", -1, 1, 1, "%");
        p[(size_t) base + 8] = makeParam(s + "Values_Sustain", "S", -1, 1, 0.5f, "%");
        p[(size_t) base + 9] = makeParam(s + "Values_Final", "Final", -1, 1, 0, "%");
        p[(size_t) base + 10] = makeParam(s + "LoopMode", "Loop", 0, 2, 0, "", false, {"None", "Trigger", "Loop"});
    }
    for (int l = 0; l < 2; ++l)
    {
        const std::string s = m + "Lfo" + std::to_string(l + 1) + "_";
        const int base = l == 0 ? W::L1Retrigger : W::L2Retrigger;
        p[(size_t) base + 0] = makeParam(s + "Retrigger", "R", 0, 1, 1, "", true);
        p[(size_t) base + 1] = makeParam(s + "Shape_Type", "Shape", 0, 4, 0, "", false, {"Sine", "Triangle", "Saw", "Square", "Random"});
        p[(size_t) base + 2] = makeParam(s + "Shape_Amount", "Amount", 0, 1, 1, "%");
        p[(size_t) base + 3] = makeParam(s + "Shape_Shaping", "Shape", -1, 1, 0, "%");
        p[(size_t) base + 4] = makeParam(s + "Shape_PhaseOffset", "Offset", 0, 360, 0, "deg");
        p[(size_t) base + 5] = makeParam(s + "Time_Sync", "Sync", 0, 1, 0, "", false, {"Hz", "Sync"});
        p[(size_t) base + 6] = makeParam(s + "Time_Rate", "Rate", 0.01f, 40, 1, "Hz");
        std::vector<std::string> rates(kSyncedNames, kSyncedNames + W::kSyncedRates);
        p[(size_t) base + 7] = makeParam(s + "Time_SyncedRate", "Rate", 0, W::kSyncedRates - 1, 15, "", false, rates);
        p[(size_t) base + 8] = makeParam(s + "Time_AttackTime", "Attack", 0, 20, 0, "s");
    }
    p[W::TimeScale] = makeParam(m + "TimeScale", "Time", -1, 1, 0, "%");
    p[W::ModAmount] = makeParam(m + "Amount", "Amount", 0, 2, 1, "%");
    p[W::UnisonAmount] = makeParam("Voice_Unison_Amount", "Amount", 0, 1, 0.3f, "%");
    p[W::GlobalTranspose] = makeParam("Voice_Global_Transpose", "Transpose", -48, 48, 0, "st");
    p[W::Glide] = makeParam("Voice_Global_Glide", "Glide", 0, 5, 0, "s");
    p[W::Volume] = makeParam("Volume", "Volume", 0, 1.99f, 0.3548134f, "lin");
    p[W::UnisonMode] = makePlainParam(makeParam("Voice_Unison_Mode", "Unison", 0, 6, 0, "", false,
                                                {"None", "Classic", "Shimmer", "Noise", "Phase Sync", "Position Spread", "Random Note"}));
    p[W::UnisonVoices] = makePlainParam(makeParam("Voice_Unison_VoiceCount", "Voices", 2, 8, 4));
    p[W::FilterRouting] = makePlainParam(makeParam("Voice_Global_FilterRouting", "Routing", 0, 2, 0, "", false, {"Serial", "Parallel", "Split"}));
    p[W::MonoPoly] = makePlainParam(makeParam("MonoPoly", "Poly", 0, 1, 1, "", false, {"Mono", "Poly"}));
    p[W::PolyVoices] = makePlainParam(makeParam("PolyVoices", "Voices", 0, 6, 6, "", false, {"2", "3", "4", "5", "6", "7", "8"}));
    p[W::HiQ] = makePlainParam(makeParam("HiQ", "HiQ", 0, 1, 0, "", true));
    for (int t = 0; t < W::kTargets; ++t)
        for (int s = 0; s < W::kSources; ++s)
        {
            // Defaults follow the reference's default preset (Core Library/Defaults/Instruments/Wavetable.adv): velocity to Amp 50 %, pitch bend to Pitch 2 st, MPE note bend to Pitch.
            const float initial = t == W::TAmp && s == W::SrcVelocity ? 0.5f
                                : t == W::TPitch && s == W::SrcPitchBend ? 2.0f / 48.0f
                                : t == W::TPitch && s == W::SrcNotePb ? 1.0f : 0.0f;
            p[(size_t) W::matrixParam(t, s)] = makePlainParam(makeParam(
                "ModulationConnections/ModulationConnectionsForInstrumentVector#" + std::to_string(t) + "/ModulationAmounts."
                    + std::to_string(s),
                std::string(kTargetNames[(size_t) t]) + " < " + kSourceNames[s], -1, 1, initial, "%"));
        }
    return p;
}

// Equal-power pan, unity in the centre (the reference's law measured on Simpler, docs/PARITY.md).
void panGains(float pan, float& left, float& right)
{
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * (float) dsp::kPi * 0.25f;
    left = std::sqrt(2.0f) * std::cos(angle);
    right = std::sqrt(2.0f) * std::sin(angle);
}

// Circuit drive saturation (Simpler's measured law): OSR / MS2 / SMP soft clip with bias, PRD tanh.
float saturate(float x, int circuit)
{
    if (circuit == 4) return 1.35f * std::tanh(x / 1.35f);
    constexpr float knee = 1.537f, room = 1.748f - knee, bias = 0.205f;
    const auto clip = [](float u) {
        const float a = std::abs(u);
        return a <= knee ? u : std::copysign(knee + room * std::tanh((a - knee) / room), u);
    };
    return clip(x + bias) - bias;
}

float uniform(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return (float) (state >> 8) / 16777216.0f;
}
} // namespace

const char* WavetableInstrument::targetName(int target, int effectMode1, int effectMode2)
{
    if (target == TOsc1Fx1 || target == TOsc1Fx2 || target == TOsc2Fx1 || target == TOsc2Fx2)
    {
        static std::string names[4][4]; // built once on the message thread; read-only afterwards
        const bool second = target == TOsc2Fx1 || target == TOsc2Fx2;
        const int mode = std::clamp(second ? effectMode2 : effectMode1, 0, 3);
        const int which = target == TOsc1Fx2 || target == TOsc2Fx2 ? 1 : 0;
        auto& name = names[mode][(second ? 2 : 0) + which];
        if (name.empty()) name = std::string(second ? "Osc 2 " : "Osc 1 ") + kFxNames[mode][which];
        return name.c_str();
    }
    return target >= 0 && target < kTargets ? kTargetNames[(size_t) target] : "";
}
const char* WavetableInstrument::sourceName(int source) { return source >= 0 && source < kSources ? kSourceNames[source] : ""; }
const char* WavetableInstrument::targetId(int target) { return target >= 0 && target < kTargets ? kTargetIds[(size_t) target] : ""; }
double WavetableInstrument::syncedRateBeats(int index) { return kSyncedBeats[std::clamp(index, 0, kSyncedRates - 1)]; }
const char* WavetableInstrument::syncedRateName(int index) { return kSyncedNames[std::clamp(index, 0, kSyncedRates - 1)]; }

float WavetableInstrument::slopeCurve(float x, float slope)
{
    // The reference's stage curve (measured on Simpler, docs/PARITY.md): a blend of linear and exp(-9.115 x); negative
    // slopes mirror it. The capture of Wavetable's envelopes fits the same law (A slope 0.21 / D slope 0.5 midpoints).
    x = std::clamp(x, 0.0f, 1.0f);
    const auto positive = [](float u, float s) {
        const float end = 1.0f - s * std::exp(-9.115f);
        return (1.0f - ((1.0f - s) * (1.0f - u) + s * std::exp(-9.115f * u))) / end;
    };
    slope = std::clamp(slope, -1.0f, 1.0f);
    return slope >= 0 ? positive(x, slope) : 1.0f - positive(1.0f - x, -slope);
}

WavetableInstrument::WavetableInstrument() : NativeDevice(wavetableParameters())
{
    for (auto& d : displayPosition_) d.store(-1.0f);
}

std::vector<int> WavetableInstrument::displayParameters() const
{
    return {Osc1Position, Osc2Position, F1Frequency, F1Resonance, AmpAttack, AmpDecay, AmpSustain, AmpRelease, Volume,
            UnisonAmount, GlobalTranspose, Glide};
}

void WavetableInstrument::setTable(int oscillator, std::shared_ptr<const WavetableData> table, std::string category, std::string name)
{
    const auto index = (size_t) std::clamp(oscillator, 0, 1);
    tables_[index] = std::move(table);
    categories_[index] = std::move(category);
    names_[index] = std::move(name);
}

void WavetableInstrument::prepare(double sampleRate, int)
{
    sampleRate_ = std::max(1000.0, sampleRate);
    reset();
}

void WavetableInstrument::reset()
{
    voices_ = {};
    bend_ = {};
    slide_ = {};
    channelPressure_ = {};
    modWheel_ = 0;
    sustainPedal_ = false;
    lastNote_ = -1;
    counter_ = 0;
    random_ = 0x2545F491u;
    freeLfo_[0] = freeLfo_[1] = {};
    activeVoices_.store(0);
    parametersChanged.store(true);
}

int WavetableInstrument::voiceLimit() const
{
    return value(MonoPoly) < 0.5f ? 1 : polyVoiceCount((int) std::lround(value(PolyVoices)));
}

void WavetableInstrument::refreshMatrix()
{
    routeCount_ = 0;
    for (int t = 0; t < kTargets; ++t)
        for (int s = 0; s < kSources; ++s)
            if (const float amount = value(matrixParam(t, s)); std::abs(amount) > 1e-6f)
                routes_[(size_t) routeCount_++] = {(int16_t) t, (int16_t) s, amount};
}

// ---------------------------------------------------------------- filters

int WavetableInstrument::filterStages(int index, double freq, double resonance, double morph, BiquadCoeffs* out) const
{
    // The circuits share Simpler's measured laws (docs/PARITY.md "Filter (Clean)" / "Circuits"): RBJ stages with damping
    // d = (1 - r) / (1 + 3 r) for Clean, per-circuit damping for OSR / MS2 / SMP, the gain-compensated ladder for PRD;
    // 24 dB adds a second stage with damping (1 + d) / 2. Morph mixes LP > BP > HP > Notch > LP per stage.
    const int base = index == 0 ? F1On : F2On;
    const int type = std::clamp((int) std::lround(value(base + 1)), 0, 4);
    const int circuit = type <= 1 ? std::clamp((int) std::lround(value(base + 2)), 0, 4) : (value(base + 3) > 0.5f ? 1 : 0);
    const bool steep = value(base + 4) >= 0.5f;
    const double r = std::clamp(resonance, 0.0, 1.0);
    const double clean = (1.0 - r) / (1.0 + 3.0 * r);
    freq = std::clamp(freq, 20.0, std::min(22000.0, sampleRate_ * 0.49));
    const double octaves = std::log2(freq / 1000.0), span = std::log2(5.0);
    const double osrScale = octaves < 0 ? 1.0 + (1.0 - 0.14) * std::max(octaves / span, -1.0) : 1.0 + (2.72 - 1.0) * std::min(octaves / span, 1.0);
    const double d = circuit == 1 ? clean + osrScale * r * (0.057 + 0.027 * r)
                   : circuit == 2 ? std::max(clean - 0.006, 0.0205)
                   : circuit == 3 ? std::max(clean + 0.01 * r, 0.029)
                                  : std::max(clean, 0.0274);
    const double d2 = circuit == 1 ? std::max((1.0 + d) * 0.5, 0.595) : (1.0 + d) * 0.5;
    if (circuit == 4 && type <= 1)
    {
        static constexpr double kLadder[5] = {0.0, 0.975, 1.929, 2.843, 3.63};
        const double pos = r * 4.0;
        const int i = std::min((int) pos, 3);
        const double k = (kLadder[i] + (kLadder[i + 1] - kLadder[i]) * (pos - i)) * (steep ? 1.0 : 0.735);
        const double q = std::pow(k, 0.25) / std::sqrt(2.0);
        const double wa = (1 - q) * (1 - q) + q * q, wb = (1 + q) * (1 + q) + q * q;
        const double K = 1.0 / std::tan(dsp::kPi * freq / sampleRate_);
        const auto section = [K](double b2, double b1, double b0, double a2, double a1, double a0) {
            const double kk = K * K, n = a2 * kk + a1 * K + a0;
            return BiquadCoeffs{(b2 * kk + b1 * K + b0) / n, (2 * b0 - 2 * b2 * kk) / n, (b2 * kk - b1 * K + b0) / n,
                                (2 * a0 - 2 * a2 * kk) / n, (a2 * kk - a1 * K + a0) / n};
        };
        if (type == 0)
        {
            out[0] = steep ? section(0, 0, wa, 1, 2 * (1 - q), wa) : section(wa, 2 * wa, wa, 1, 2 * (1 - q), wa);
            out[1] = section(0, 0, wb, 1, 2 * (1 + q), wb);
        }
        else
        {
            out[0] = steep ? section(wa, 0, 0, wa, 2 * (1 - q), 1) : section(wa, 2 * wa, wa, wa, 2 * (1 - q), 1);
            out[1] = section(wb, 0, 0, wb, 2 * (1 + q), 1);
        }
        return 2;
    }
    const auto stageCoeffs = [this, freq](int kind, double q) {
        constexpr BiquadCoeffs::Type kinds[4] = {BiquadCoeffs::Type::LowPass, BiquadCoeffs::Type::HighPass,
                                                 BiquadCoeffs::Type::BandPass, BiquadCoeffs::Type::Notch};
        auto c = BiquadCoeffs::make(kinds[kind], sampleRate_, freq, q, 0.0);
        if (kind == 2) { c.b0 *= q; c.b2 *= q; }
        return c;
    };
    morph = std::clamp(morph, 0.0, 1.0);
    const double lpWeight = std::clamp(morph < 0.5 ? 1.0 - 4.0 * morph : 4.0 * morph - 2.0, 0.0, 1.0);
    const double hpWeight = std::clamp(morph < 0.75 ? 4.0 * morph - 1.0 : 4.0 - 4.0 * morph, 0.0, 1.0);
    const double bpWeight = std::sqrt(std::max(0.0, 1.0 - lpWeight * lpWeight - hpWeight * hpWeight));
    for (int stage = 0; stage < 2; ++stage)
    {
        const double q = 0.5 / (stage == 0 ? d : d2);
        auto c = stageCoeffs(type == 4 ? 0 : type, q);
        if (type == 4)
        {
            const auto hp = stageCoeffs(1, q), bp = stageCoeffs(2, q);
            c.b0 = lpWeight * c.b0 + bpWeight * bp.b0 + hpWeight * hp.b0;
            c.b1 = lpWeight * c.b1 + bpWeight * bp.b1 + hpWeight * hp.b1;
            c.b2 = lpWeight * c.b2 + bpWeight * bp.b2 + hpWeight * hp.b2;
        }
        out[stage] = c;
        if (! steep) return 1;
    }
    return 2;
}

// ---------------------------------------------------------------- MIDI

void WavetableInstrument::handleEvent(const uint8_t* data, int size)
{
    if (size < 1 || data[0] < 0x80 || data[0] >= 0xf0) return;
    const int channel = data[0] & 15;
    const int kind = data[0] & 0xf0;
    if (kind == 0x90 && size >= 3 && data[2] > 0) noteOn(data[1], channel, (float) data[2] / 127.0f);
    else if ((kind == 0x80 || kind == 0x90) && size >= 3) noteOff(data[1], channel);
    else if (kind == 0xe0 && size >= 3) bend_[(size_t) channel] = (float) ((int) data[1] + 128 * (int) data[2] - 8192) / 8192.0f;
    else if (kind == 0xd0 && size >= 2) channelPressure_[(size_t) channel] = (float) data[1] / 127.0f;
    else if (kind == 0xa0 && size >= 3)
    {
        for (auto& v : voices_)
            if (v.active && v.note == data[1] && v.channel == channel) v.pressure = (float) data[2] / 127.0f;
    }
    else if (kind == 0xb0 && size >= 3)
    {
        const int cc = data[1];
        const float amount = (float) data[2] / 127.0f;
        if (cc == 1) modWheel_ = amount;
        else if (cc == 74) slide_[(size_t) channel] = amount;
        else if (cc == 64)
        {
            sustainPedal_ = data[2] >= 64;
            if (! sustainPedal_)
                for (auto& v : voices_)
                    if (v.active && v.sustained) { v.sustained = false; releaseVoice(v); }
        }
        else if (cc == 120)
            for (auto& v : voices_) v.active = false;
        else if (cc == 123)
        {
            for (auto& v : voices_)
                if (v.active && v.gate) releaseVoice(v);
        }
        else if (cc == 121) { bend_ = {}; slide_ = {}; channelPressure_ = {}; modWheel_ = 0; }
    }
}

void WavetableInstrument::noteOn(int note, int channel, float velocity)
{
    const int limit = voiceLimit();
    const bool mono = limit == 1;
    Voice* v = nullptr;
    bool legato = false;
    if (mono)
    {
        v = &voices_[0];
        legato = v->active && v->gate;
        for (size_t i = 1; i < voices_.size(); ++i) voices_[i].active = false;
    }
    else
    {
        for (int i = 0; i < kMaxVoices; ++i)
            if (i >= limit) voices_[(size_t) i].active = false;
        for (int i = 0; i < limit && v == nullptr; ++i)
            if (! voices_[(size_t) i].active) v = &voices_[(size_t) i];
        if (v == nullptr) // steal: the oldest released voice, else the oldest
        {
            for (int i = 0; i < limit; ++i)
            {
                auto& c = voices_[(size_t) i];
                if (! c.gate && (v == nullptr || c.age < v->age)) v = &c;
            }
            if (v == nullptr)
                for (int i = 0; i < limit; ++i)
                    if (v == nullptr || voices_[(size_t) i].age < v->age) v = &voices_[(size_t) i];
        }
    }
    const int fromNote = lastNote_;
    lastNote_ = note;
    if (mono && legato)
    {
        // Legato in Mono: the sounding voice changes pitch (gliding when Glide is set) without retriggering.
        v->glideSemis += (double) (v->note - note);
        v->note = note;
        v->channel = channel;
        v->velocity = velocity;
        v->age = ++counter_;
        v->sustained = false;
        v->glideStep = -v->glideSemis / std::max(1.0, (double) value(Glide) * sampleRate_);
        if (value(Glide) <= 0) v->glideSemis = 0;
        return;
    }
    const bool keepPhase = mono && v->active;
    auto phases = v->phase;
    *v = Voice{};
    v->active = true;
    v->gate = true;
    v->note = note;
    v->channel = channel;
    v->velocity = velocity;
    v->age = ++counter_;
    v->random = uniform(random_);
    v->pressure = 0;
    // Unison: fixed spread positions -1..1, random start phases except None / Phase Sync (phases start at zero).
    const int mode = std::clamp((int) std::lround(value(UnisonMode)), 0, 6);
    const int count = mode == 0 ? 1 : std::clamp((int) std::lround(value(UnisonVoices)), 2, kMaxUnison);
    for (int u = 0; u < kMaxUnison; ++u)
    {
        v->spread[(size_t) u] = count > 1 ? -1.0f + 2.0f * (float) u / (float) (count - 1) : 0.0f;
        if (mode == 6) v->spread[(size_t) u] = uniform(random_) * 2.0f - 1.0f; // Random Note: a new detune per note
        v->jitter[(size_t) u] = v->jitterTarget[(size_t) u] = uniform(random_) * 2.0f - 1.0f;
        for (int osc = 0; osc < 2; ++osc)
            v->phase[(size_t) osc][(size_t) u] = mode == 0 || mode == 4 ? 0.0 : (double) uniform(random_);
    }
    if (keepPhase) v->phase = phases;
    if (value(Glide) > 0 && fromNote >= 0 && fromNote != note)
    {
        v->glideSemis = (double) (fromNote - note);
        v->glideStep = -v->glideSemis / std::max(1.0, (double) value(Glide) * sampleRate_);
    }
    for (int l = 0; l < 2; ++l)
    {
        const int base = l == 0 ? L1Retrigger : L2Retrigger;
        v->lfo[l].phase = value(base + 4) / 360.0;
        v->lfo[l].held = uniform(random_) * 2.0f - 1.0f;
    }
    v->env[0].stage = v->env[1].stage = v->env[2].stage = 1;
    v->env[1].level = v->env[1].from = value(E2Initial);
    v->env[2].level = v->env[2].from = value(E3Initial);
    v->countdown = 0;
}

void WavetableInstrument::releaseVoice(Voice& v)
{
    v.gate = false;
    for (auto& e : v.env)
        if (e.stage >= 1 && e.stage <= 3) { e.stage = 4; e.from = e.level; e.elapsed = 0; }
}

void WavetableInstrument::noteOff(int note, int channel)
{
    for (auto& v : voices_)
        if (v.active && v.gate && v.note == note && v.channel == channel)
        {
            if (sustainPedal_) { v.sustained = true; continue; }
            if (voiceLimit() == 1 && lastNote_ != note) continue; // mono: an older key released under a newer one
            releaseVoice(v);
        }
}

// ---------------------------------------------------------------- modulators

WavetableInstrument::EnvSettings WavetableInstrument::envSettings(int env, const float* t) const
{
    // Times: modulation moves a time along its logarithmic travel (0.001 .. 20 s, ~14.3 octaves per unit). The Time
    // control scales every envelope time by 10^Time (an assumption until measured, DEV-28b2).
    const float scale = std::pow(10.0f, std::clamp(value(TimeScale) + t[TTime], -1.0f, 1.0f));
    const auto time = [&](int p, int target) { return std::clamp(value(p) * std::exp2(t[target] * 14.3f), 0.0f, 60.0f) * scale; };
    EnvSettings s{};
    if (env == 0)
    {
        s = {time(AmpAttack, TAmpAttack), time(AmpDecay, TAmpDecay), time(AmpRelease, TAmpRelease), value(AmpSlopeAttack),
             value(AmpSlopeDecay), value(AmpSlopeRelease), 0, 1, std::clamp(value(AmpSustain) + t[TAmpSustain], 0.0f, 1.0f), 0,
             (int) std::lround(value(AmpLoop))};
        return s;
    }
    const int b = env == 1 ? E2Attack : E3Attack;
    const int tb = env == 1 ? TE2Attack : TE3Attack;
    const auto level = [&](int p, int target) { return std::clamp(value(p) + t[target], -1.0f, 1.0f); };
    s = {time(b, tb), time(b + 1, tb + 1), time(b + 2, tb + 2), value(b + 3), value(b + 4), value(b + 5), level(b + 6, tb + 3),
         level(b + 7, tb + 4), level(b + 8, tb + 5), level(b + 9, tb + 6), (int) std::lround(value(b + 10))};
    return s;
}

void WavetableInstrument::stepEnv(Env& e, const EnvSettings& s, bool gate, double samples) const
{
    if (e.stage == 0 || e.stage == 5) { e.level = e.stage == 5 ? s.final : e.level; return; }
    if (e.stage == 3)
    {
        e.level = s.sustain;
        if (s.loop != 0) { e.stage = 4; e.from = e.level; e.elapsed = 0; } // Trigger / Loop never hold
        return;
    }
    e.elapsed += samples;
    const double sr = sampleRate_;
    for (int guard = 0; guard < 4; ++guard)
    {
        const float seconds = e.stage == 1 ? s.attack : e.stage == 2 ? s.decay : s.release;
        const double length = std::max(1.0, (double) seconds * sr);
        const float target = e.stage == 1 ? s.peak : e.stage == 2 ? s.sustain : s.final;
        const float slope = e.stage == 1 ? s.slopeA : e.stage == 2 ? s.slopeD : s.slopeR;
        if (e.elapsed < length)
        {
            e.level = e.from + (target - e.from) * slopeCurve((float) (e.elapsed / length), slope);
            return;
        }
        e.elapsed -= length;
        e.level = target;
        e.from = target;
        if (e.stage == 1) e.stage = 2;
        else if (e.stage == 2)
        {
            if (s.loop == 0 && gate) { e.stage = 3; return; }
            e.stage = 4; // Trigger and Loop go on to the release; so does a released note
        }
        else
        {
            // Loop: while the key is held the envelope restarts its attack from where the release ended.
            if (s.loop == 2 && gate) { e.stage = 1; continue; }
            e.stage = 5;
            return;
        }
    }
}

float WavetableInstrument::stepLfo(Lfo& l, int index, float rateMod, float amountMod, float shapingMod, double samples)
{
    const int b = index == 0 ? L1Retrigger : L2Retrigger;
    double rate = 0;
    if (value(b + 5) >= 0.5f)
        rate = tempo_ / 60.0 / syncedRateBeats((int) std::lround(value(b + 7) - rateMod * 21.0f));
    else
        rate = (double) std::clamp(value(b + 6) * std::exp2(rateMod * 12.0f), 0.001f, 200.0f);
    rate /= std::pow(10.0, (double) std::clamp(value(TimeScale), -1.0f, 1.0f));
    l.phase += rate * samples / sampleRate_;
    if (l.phase >= 1.0)
    {
        l.phase -= std::floor(l.phase);
        l.held = uniform(random_) * 2.0f - 1.0f;
    }
    l.age += samples;
    float p = (float) l.phase;
    // Shaping bends the phase (positive: the cycle's first half shrinks); Sonora's reading of the Shape control.
    const float shaping = std::clamp(value(b + 3) + shapingMod, -1.0f, 1.0f);
    if (std::abs(shaping) > 1e-4f) p = std::pow(p, std::exp2(-2.0f * shaping));
    float y = 0;
    switch (std::clamp((int) std::lround(value(b + 1)), 0, 4))
    {
        case 0: y = std::sin((float) kTwoPi * p); break;
        case 1: y = 1.0f - 4.0f * std::abs(std::fmod(p + 0.75f, 1.0f) - 0.5f); break;
        case 2: y = 1.0f - 2.0f * p; break;
        case 3: y = p < 0.5f ? 1.0f : -1.0f; break;
        default: y = l.held; break;
    }
    const double attack = (double) value(b + 8) * sampleRate_;
    const float fade = attack > 1 ? (float) std::min(1.0, l.age / attack) : 1.0f;
    l.value = y * std::clamp(value(b + 2) + amountMod, 0.0f, 1.0f) * fade;
    return l.value;
}

float WavetableInstrument::sourceValue(const Voice& v, int source) const
{
    switch (source)
    {
        case SrcAmp: return v.env[0].level;
        case SrcEnv2: return v.env[1].level;
        case SrcEnv3: return v.env[2].level;
        case SrcLfo1: return value(L1Retrigger) >= 0.5f ? v.lfo[0].value : freeLfo_[0].value;
        case SrcLfo2: return value(L2Retrigger) >= 0.5f ? v.lfo[1].value : freeLfo_[1].value;
        case SrcVelocity: return v.velocity;
        case SrcNote: return std::clamp((float) (v.note - 60) / 60.0f, -1.0f, 1.0f);
        case SrcPitchBend: return bend_[v.channel == 0 ? 0 : 0];
        case SrcPressure: return std::max(v.pressure, channelPressure_[(size_t) v.channel]);
        case SrcModWheel: return modWheel_;
        case SrcRandom: return v.random;
        case SrcNotePb: return v.channel != 0 ? bend_[(size_t) v.channel] : 0.0f; // MPE member channels 2-16
        case SrcSlide: return slide_[(size_t) v.channel];
        default: return 0;
    }
}

void WavetableInstrument::control(Voice& v)
{
    // Matrix: every target collects amount x source. The Global Mod Amount scales the whole matrix.
    float t[kTargets]{};
    float modScale = value(ModAmount);
    for (int i = 0; i < routeCount_; ++i)
        if (routes_[(size_t) i].target == TModAmount) modScale += routes_[(size_t) i].amount * sourceValue(v, routes_[(size_t) i].source);
    modScale = std::clamp(modScale, 0.0f, 2.0f);
    float ampMod = 1.0f;
    for (int i = 0; i < routeCount_; ++i)
    {
        const auto& r = routes_[(size_t) i];
        if (r.target == TModAmount) continue;
        const float s = sourceValue(v, r.source), a = r.amount * modScale;
        if (r.target == TAmp)
            ampMod *= bipolar(r.source) ? std::max(0.0f, 1.0f + a * s) : (a >= 0 ? 1.0f - a + a * s : 1.0f + a * s);
        else
            t[r.target] += a * s;
    }
    v.ampMod = std::max(0.0f, ampMod);
    const double block = kControl;
    v.amp = envSettings(0, t);
    for (int e = 1; e < 3; ++e) stepEnv(v.env[e], envSettings(e, t), v.gate, block);
    for (int l = 0; l < 2; ++l)
    {
        const int retrig = l == 0 ? L1Retrigger : L2Retrigger;
        if (value(retrig) >= 0.5f)
            stepLfo(v.lfo[l], l, t[l == 0 ? TL1Rate : TL2Rate], t[l == 0 ? TL1Amount : TL2Amount], t[l == 0 ? TL1Shaping : TL2Shaping], block);
        else
        {
            v.lfo[l].age += block;
            const double attack = (double) value(retrig + 8) * sampleRate_;
            v.lfo[l].value = freeLfo_[l].value * (attack > 1 ? (float) std::min(1.0, v.lfo[l].age / attack) : 1.0f);
        }
    }
    // Glide (linear in semitones over the Glide time, modulated along a 0..5 s travel).
    if (std::abs(v.glideSemis) > 1e-9)
    {
        v.glideSemis += v.glideStep * block * std::exp2(-t[TGlide] * 4.0f);
        if ((v.glideStep < 0 && v.glideSemis < 0) || (v.glideStep > 0 && v.glideSemis > 0)) v.glideSemis = 0;
    }
    const float base = (float) v.note + value(GlobalTranspose) + (float) v.glideSemis + t[TPitch] * 48.0f;
    for (int osc = 0; osc < 2; ++osc)
    {
        const int b = osc == 0 ? Osc1On : Osc2On;
        const int tb = osc == 0 ? TOsc1Pitch : TOsc2Pitch;
        v.pitch[osc] = base + value(b + 1) + value(b + 2) + t[tb] * 48.0f;
        v.position[osc] = std::clamp(value(b + 3) + t[tb + 1], 0.0f, 1.0f);
        v.fx1[osc] = std::clamp(value(b + 4) + t[tb + 2], -1.0f, 1.0f);
        v.fx2[osc] = std::clamp(value(b + 5) + t[tb + 3], 0.0f, 1.0f);
        panGains(value(b + 6) + t[tb + 4], v.panL[osc], v.panR[osc]);
        v.gain[osc] = value(b) >= 0.5f ? std::clamp(value(b + 7) + t[tb + 5], 0.0f, 1.0f) : 0.0f;
    }
    v.subPitch = base - 12.0f * (float) std::lround(value(SubTranspose));
    v.subTone = std::clamp(value(SubTone) + t[TSubTone], 0.0f, 1.0f);
    v.subGain = value(SubOn) >= 0.5f ? std::clamp(value(SubGain) + t[TSubGain], 0.0f, 1.0f) : 0.0f;
    v.unisonAmount = std::clamp(value(UnisonAmount) + t[TUnisonAmount], 0.0f, 1.0f);
    // Filters: frequency modulation moves along the log travel of 20 Hz - 20 kHz (~10 octaves per unit).
    for (int f = 0; f < 2; ++f)
    {
        const int b = f == 0 ? F1On : F2On;
        const int tb = f == 0 ? TF1Freq : TF2Freq;
        if (value(b) < 0.5f) continue;
        const double freq = (double) value(b + 5) * std::exp2((double) t[tb] * 9.97);
        const double res = std::clamp((double) (value(b + 6) + t[tb + 1] * 1.25f), 0.0, 1.25);
        const double morph = (double) (value(b + 8) + t[tb + 3]);
        BiquadCoeffs stages[2];
        v.filter[f].stages = filterStages(f, freq, res, morph, stages);
        for (int s = 0; s < v.filter[f].stages; ++s) v.filter[f].c[s] = SvfCoeffs::fromBiquad(stages[s]);
        const int type = std::clamp((int) std::lround(value(b + 1)), 0, 4);
        const int circuit = type <= 1 ? (int) std::lround(value(b + 2)) : (value(b + 3) > 0.5f ? 1 : 0);
        v.drive[f] = circuit == 0 ? 1.0f : dsp::dbToGain(std::clamp(value(b + 7) + t[tb + 2] * 24.0f, 0.0f, 24.0f));
    }
    // Unison jitter (Shimmer: slow random drift, Noise: a new random value per control block).
    const int mode = (int) std::lround(value(UnisonMode));
    for (int u = 0; u < kMaxUnison; ++u)
    {
        auto& j = v.jitter[(size_t) u];
        if (mode == 3) j = uniform(random_) * 2.0f - 1.0f;
        else if (mode == 2)
        {
            if (uniform(random_) < 0.02f) v.jitterTarget[(size_t) u] = uniform(random_) * 2.0f - 1.0f;
            j += (v.jitterTarget[(size_t) u] - j) * 0.05f;
        }
    }
    for (int s = 0; s < kSources; ++s) v.mods[s] = sourceValue(v, s);
}

// ---------------------------------------------------------------- audio

float WavetableInstrument::renderOscillator(Voice& v, int osc, int u, int voices, float& left, float& right)
{
    const auto& table = tables_[(size_t) osc];
    if (! table) return 0;
    const int mode = (int) std::lround(value(UnisonMode));
    float detune = 0, posOffset = 0, pan = 0;
    if (voices > 1)
    {
        const float spread = v.spread[(size_t) u], amount = v.unisonAmount;
        switch (mode)
        {
            case 1: case 4: detune = spread * amount * 0.5f; break;           // Classic / Phase Sync: +-50 ct at 100 %
            case 2: detune = (spread * 0.1f + v.jitter[(size_t) u] * 0.4f) * amount; break; // Shimmer
            case 3: detune = (spread * 0.2f + v.jitter[(size_t) u] * 0.3f) * amount; break; // Noise
            case 5: detune = spread * amount * 0.1f; posOffset = spread * amount * 0.5f; break; // Position Spread
            case 6: detune = spread * amount * 0.5f; break;                   // Random Note
            default: break;
        }
        pan = spread * 0.7f;
    }
    const double inc = 440.0 * std::exp2(((double) v.pitch[osc] + detune - 69.0) / 12.0) / sampleRate_;
    double& phase = v.phase[(size_t) osc][(size_t) u];
    double read = phase;
    double bandwidth = inc;
    const int effect = (int) std::lround(value(osc == 0 ? Osc1EffectMode : Osc2EffectMode));
    const float fx1 = v.fx1[osc], fx2 = v.fx2[osc];
    if (effect == 1)
    {
        // FM: a sine modulator at 2^(2 FX1) x the oscillator frequency, phase-modulating the table by up to 1.5 cycles.
        double& mod = v.modPhase[(size_t) osc][(size_t) u];
        const double ratio = std::exp2(2.0 * fx1);
        read += 1.5 * fx2 * std::sin(kTwoPi * mod);
        mod += inc * ratio;
        mod -= std::floor(mod);
        bandwidth *= 1.0 + 3.0 * fx2 * ratio;
    }
    else if (effect == 2)
    {
        // Classic: pulse width (phase distortion around the half cycle) and hard sync up to 8x.
        const double width = 0.5 * (1.0 - 0.95 * std::clamp((double) std::abs(fx1), 0.0, 1.0));
        const double sync = 1.0 + 7.0 * fx2;
        double p = read - std::floor(read);
        p = p < width ? 0.5 * p / width : 0.5 + 0.5 * (p - width) / (1.0 - width);
        p *= sync;
        read = p - std::floor(p);
        bandwidth *= sync / std::max(0.05, width * 2.0);
    }
    else if (effect == 3)
    {
        // Modern: Warp bends the phase (power law), Fold is applied to the sample below.
        double p = read - std::floor(read);
        if (std::abs(fx1) > 1e-4f) p = std::pow(p, std::exp2(-2.0 * fx1));
        read = p;
        bandwidth *= std::exp2(2.0 * std::abs(fx1));
    }
    const int hiq = value(HiQ) >= 0.5f ? 1 : 0;
    const int mip = std::max(0, table->mipForIncrement(bandwidth) - hiq);
    float y = table->sample(std::clamp(v.position[osc] + posOffset, 0.0f, 1.0f), read, mip);
    if (effect == 3 && fx2 > 1e-4f)
    {
        const float folded = std::sin(0.5f * (float) dsp::kPi * y * (1.0f + 4.0f * fx2));
        y += (folded - y) * std::min(1.0f, fx2 * 2.0f);
    }
    phase += inc;
    phase -= std::floor(phase);
    float pl = 1, pr = 1;
    panGains(pan, pl, pr);
    left += y * pl * v.panL[osc];
    right += y * pr * v.panR[osc];
    return y;
}

void WavetableInstrument::render(float* left, float* right, int from, int to)
{
    const int mode = std::clamp((int) std::lround(value(UnisonMode)), 0, 6);
    const int unison = mode == 0 ? 1 : std::clamp((int) std::lround(value(UnisonVoices)), 2, kMaxUnison);
    const float unisonGain = 1.0f / std::sqrt((float) unison);
    const int routing = std::clamp((int) std::lround(value(FilterRouting)), 0, 2);
    const bool f1 = value(F1On) >= 0.5f, f2 = value(F2On) >= 0.5f;
    const float volume = value(Volume);
    // Free-running LFOs (Retrigger off) advance once per control block on a grid that does not depend on block sizes.
    for (int i = from; i < to; ++i)
    {
        if ((counter_ & (uint64_t) (kControl - 1)) == 0)
        {
            for (int l = 0; l < 2; ++l)
                if (value(l == 0 ? L1Retrigger : L2Retrigger) < 0.5f) stepLfo(freeLfo_[l], l, 0, 0, 0, kControl);
        }
        ++counter_;
        float outL = 0, outR = 0;
        for (auto& v : voices_)
        {
            if (! v.active) continue;
            if (v.countdown <= 0) { control(v); v.countdown = kControl; }
            --v.countdown;
            stepEnv(v.env[0], v.amp, v.gate, 1.0); // the amplitude envelope runs per sample
            if (v.env[0].stage == 5 || (v.env[0].stage == 4 && v.env[0].level < 1e-5f && v.env[0].elapsed > 64))
            {
                v.active = false;
                continue;
            }
            float l[2]{}, r[2]{}; // per oscillator stereo (Split routes them to different filters)
            for (int osc = 0; osc < 2; ++osc)
            {
                if (v.gain[osc] <= 0) continue;
                float sl = 0, sr = 0;
                for (int u = 0; u < unison; ++u) renderOscillator(v, osc, u, unison, sl, sr);
                l[osc] = sl * unisonGain * v.gain[osc];
                r[osc] = sr * unisonGain * v.gain[osc];
            }
            float sub = 0;
            if (v.subGain > 0)
            {
                // Sub: a sine an octave (or two) below, its Tone pushing it towards a rounded square (Sonora's reading).
                const double inc = 440.0 * std::exp2(((double) v.subPitch - 69.0) / 12.0) / sampleRate_;
                const float s = (float) std::sin(kTwoPi * v.subPhase);
                const float k = 1.0f + 6.0f * v.subTone;
                sub = std::tanh(k * s) / std::tanh(k) * v.subGain;
                v.subPhase += inc;
                v.subPhase -= std::floor(v.subPhase);
            }
            const auto runFilter = [&](int f, float& a, float& b) {
                auto& flt = v.filter[f];
                if (v.drive[f] != 1.0f)
                {
                    const int base = f == 0 ? F1On : F2On;
                    const int type = std::clamp((int) std::lround(value(base + 1)), 0, 4);
                    const int circuit = type <= 1 ? (int) std::lround(value(base + 2)) : 1;
                    a = saturate(a * v.drive[f], circuit);
                    b = saturate(b * v.drive[f], circuit);
                }
                for (int s = 0; s < flt.stages; ++s)
                {
                    a = flt.s[s][0].process(flt.c[s], a);
                    b = flt.s[s][1].process(flt.c[s], b);
                }
            };
            float yl = 0, yr = 0;
            if (routing == 2) // Split: oscillator 1 to filter 1, oscillator 2 to filter 2, the sub to both
            {
                float al = l[0] + 0.5f * sub, ar = r[0] + 0.5f * sub, bl = l[1] + 0.5f * sub, br = r[1] + 0.5f * sub;
                if (f1) runFilter(0, al, ar);
                if (f2) runFilter(1, bl, br);
                yl = al + bl;
                yr = ar + br;
            }
            else
            {
                float al = l[0] + l[1] + sub, ar = r[0] + r[1] + sub;
                if (routing == 0)
                {
                    if (f1) runFilter(0, al, ar);
                    if (f2) runFilter(1, al, ar);
                    yl = al;
                    yr = ar;
                }
                else // Parallel: both filters see the mix and are summed; a switched-off filter passes nothing
                {
                    if (! f1 && ! f2) { yl = al; yr = ar; }
                    else
                    {
                        float bl = al, br = ar;
                        if (f1) runFilter(0, al, ar); else al = ar = 0;
                        if (f2) runFilter(1, bl, br); else bl = br = 0;
                        yl = al + bl;
                        yr = ar + br;
                    }
                }
            }
            const float gain = v.env[0].level * v.ampMod * volume * 0.5f;
            outL += yl * gain;
            outR += yr * gain;
        }
        left[i] += std::isfinite(outL) ? outL : 0.0f;
        right[i] += std::isfinite(outR) ? outR : 0.0f;
    }
}

void WavetableInstrument::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    if (parametersChanged.exchange(false, std::memory_order_acquire)) refreshMatrix();
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
    const Voice* newest = nullptr;
    for (const auto& v : voices_)
        if (v.active)
        {
            ++active;
            if (newest == nullptr || v.age > newest->age) newest = &v;
        }
    activeVoices_.store(active, std::memory_order_relaxed);
    for (int osc = 0; osc < 2; ++osc)
        displayPosition_[(size_t) osc].store(newest != nullptr ? newest->position[osc] : -1.0f, std::memory_order_relaxed);
}

} // namespace sonora::devices
