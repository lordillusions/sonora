#include "plugins/SerumPatch.h"

#include "dsp/LadderFilter.h"
#include "plugins/SerumEffectsImport.h"
#include "plugins/SerumLibrary.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/OscillatorMedia.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace sonora::plugins
{
namespace
{
using W = devices::WavetableSynth;
const SerumValue* child(const SerumValue* value, std::string_view key)
{
    return value != nullptr ? value->find(key) : nullptr;
}
std::string text(const SerumValue* value, std::string_view fallback = {})
{
    return value != nullptr && value->text() != nullptr ? *value->text() : std::string(fallback);
}
double number(const SerumValue* value, double fallback)
{
    return value != nullptr ? value->number().value_or(fallback) : fallback;
}
double plain(const SerumValue* group, std::string_view key, double fallback)
{
    return number(child(child(group, "plainParams"), key), fallback);
}
std::string choice(const SerumValue* group, std::string_view key, std::string_view fallback)
{
    return text(child(child(group, "plainParams"), key), fallback);
}
int sourceDestination(const SerumValue* group, int fallback, std::vector<std::string>& warnings)
{
    const auto* value = child(child(group, "plainParams"), "kParamRoutingDest");
    if (value == nullptr) return fallback;
    static constexpr std::array<std::string_view, 4> names{
        "kRoutingDestFilter", "kRoutingDestMaster", "kRoutingDestDirect", "kRoutingDestNone"};
    if (const auto* name = value->text())
        for (size_t i = 0; i < names.size(); ++i)
            if (*name == names[i]) return static_cast<int>(i);
    if (const auto numeric = value->number(); numeric && *numeric >= 0 && *numeric < 4 && std::floor(*numeric) == *numeric)
        return static_cast<int>(*numeric);
    warnings.push_back("Unrecognized source routing destination; measured default retained with source state");
    return fallback;
}
SerumStateResult decode(const model::NativeContent& content)
{
    if (content.format == "serum2-xferjson") return decodeSerumState(content.data);
    if (content.format == "serum2-preset") return decodeSerumPreset(content.data);
    return {nullptr, "Unsupported preserved native format: " + content.format};
}
// Reference warp menu entries. Sync and the sourced FM/PD/AM/RM/Self PD warps are measured;
// Bend+/-, PWM and Mirror use the nearest native shape warp (unmeasured, reported as approximations); every
// other entry imports Off and keeps its label for display.
struct ImportedWarp { int mode = W::WarpOff; double amount = 0; bool approximate = false; };
ImportedWarp importedWarp(const std::string& name, double amount)
{
    static const std::pair<std::string_view, int> sourced[] = {{"kFM_SUB", W::FmSub}, {"kFM_OSC", W::FmOsc1}, {"kFM_OSC2", W::FmOsc2}, {"kPD_OSC", W::PdOsc1},
            {"kPD_OSC2", W::PdOsc2}, {"kPD_SUB", W::PdSub}, {"kAM_OSC", W::AmOsc1}, {"kAM_OSC2", W::AmOsc2}, {"kAM_SUB", W::AmSub},
            {"kRM_OSC", W::RmOsc1}, {"kRM_OSC2", W::RmOsc2}, {"kRM_SUB", W::RmSub}, {"kSelfPD", W::SelfPd}};
    // Distortion warps (measured: the FX Distortion shaper of the same name, drive = amount, on the table sample).
    static const std::pair<std::string_view, int> distortion[] = {{"kDistTube", W::DistTube}, {"kDistSoftClip", W::DistSoftClip},
            {"kDistHardClip", W::DistHardClip}, {"kDistDiode1", W::DistDiode1}, {"kDistDiode2", W::DistDiode2},
            {"kDistZeroSquare", W::DistZeroSquare}, {"kDistAsym", W::DistAsym}, {"kDistRectify", W::DistRectify},
            {"kDistStompBox", W::DistStompBox}, {"kDistTapeSat", W::DistTapeSat}, {"kDistSoftSat", W::DistSoftSat},
            {"kFilterLPF", W::FilterLpfWarp}, {"kFilterHPF", W::FilterHpfWarp}}; // filters measured: one-pole, 41.5 * 2^(10 a) Hz
    for (const auto& [key, mode] : distortion) if (name == key) return {mode, amount, false};
    if (name == "kSync") return {W::Sync, amount, false};
    for (const auto& [key, mode] : sourced) if (name == key) return {mode, amount, false};
    if (name == "kBendPos") return {W::Bend, amount, true};
    if (name == "kBendNeg") return {W::Bend, -amount, true};
    if (name == "kBendPosNeg") return {W::Bend, 2 * amount - 1, true};
    if (name == "kPWM") return {W::Pulse, amount, true};
    if (name == "kMirror") return {W::Mirror, amount, true};
    return {W::WarpOff, amount, false};
}
// "kBendPosNeg" -> "BEND+/-", "kFM_NOISE" -> "FM (NOISE)", "kDistTube" -> "DIST TUBE".
std::string warpLabel(const std::string& name)
{
    static const std::pair<std::string_view, std::string_view> fixed[] = {{"kBendPos", "BEND+"}, {"kBendNeg", "BEND-"}, {"kBendPosNeg", "BEND+/-"},
        {"kASYMPos", "ASYM+"}, {"kASYMNeg", "ASYM-"}, {"kASYMPosNeg", "ASYM+/-"}, {"kSelfPD", "SELF PD"}, {"kPWM", "PWM"}, {"kDLM", "DLM"}};
    for (const auto& [key, label] : fixed) if (name == key) return std::string(label);
    std::string body = name.rfind("k", 0) == 0 ? name.substr(1) : name, out;
    if (const auto underscore = body.find('_'); underscore != std::string::npos)
        body = body.substr(0, underscore) + " (" + body.substr(underscore + 1) + ")";
    for (size_t i = 0; i < body.size(); ++i)
    {
        const char c = body[i];
        if (i > 0 && std::isupper((unsigned char) c) && std::islower((unsigned char) body[i - 1])) out += ' ';
        out += (char) std::toupper((unsigned char) c);
    }
    return out;
}
juce::File resolveAsset(const model::NativeContent& content, const std::string& reference, const char* folder = "Tables")
{
    if (juce::File::isAbsolutePath(juce::String(reference)))
    {
        const juce::File file{juce::String(reference)};
        if (file.existsAsFile()) return file;
    }
    auto relative = juce::String(reference).replaceCharacter('\\', '/');
    while (relative.startsWithChar('/')) relative = relative.substring(1);
    juce::Array<juce::File> roots;
    if (! content.sourceDirectory.empty()) roots.add(juce::File(juce::String(content.sourceDirectory)));
    roots.add(juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets").getChildFile(folder));
    roots.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
              .getChildFile("Library/Audio/Presets/Xfer Records/Serum 2 Presets").getChildFile(folder));
    roots.add(juce::File("/Library/Audio/Presets/Xfer Records/Serum Presets").getChildFile(folder));
    if (std::string_view(folder) == "Noises")
    {
        roots.add(juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Samples/Factory Non-Tonal/Noises"));
        roots.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library/Audio/Presets/Xfer Records/Serum 2 Presets/Samples/Factory Non-Tonal/Noises"));
    }
    for (const auto& root : roots)
    {
        const auto candidate = root.getChildFile(relative);
        if (candidate.existsAsFile()) return candidate;
    }
    return {};
}
bool lfoCurve(const SerumValue* value, dsp::CurveGraph& output)
{
    const auto* x = child(value, "xVals"), *y = child(value, "yVals"), *c = child(value, "curveVals");
    if (x == nullptr || y == nullptr || c == nullptr || x->array() == nullptr || y->array() == nullptr || c->array() == nullptr) return false;
    const auto* declared = child(value, "numPoints");
    const auto rawCount = declared != nullptr ? declared->number() : std::optional<double>(2);
    if (! rawCount || *rawCount < 1 || *rawCount >= dsp::CurveGraph::kMaxPoints || std::floor(*rawCount) < *rawCount) return false;
    const size_t count = static_cast<size_t>(*rawCount);
    if (x->array()->size() < count || y->array()->size() < count || c->array()->size() < count) return false;
    output = {}; output.count = static_cast<int>(count + 1);
    double previousX = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto px = (*x->array())[i].number(), py = (*y->array())[i].number(), pc = (*c->array())[i].number();
        // A real point may sit on x = 1 (303 of the 1704 factory LFO curves end there); the closing point follows it.
        if (! px || ! py || ! pc || *px < 0 || *px > 1 || *py < 0 || *py > 1 || *pc < 0 || *pc > 1
            || (i > 0 && *px < previousX)) return false;
        previousX = *px;
        output.x[i] = static_cast<float>(*px); output.y[i] = static_cast<float>(1 - *py);
        output.curve[i] = static_cast<float>(2 * *pc - 1);
    }
    if (std::abs(output.x[0]) > 1e-6f) return false;
    // Controlled states ignore the stored extra point and close the real points back to the first Y.
    output.x[count] = 1; output.y[count] = output.y[0]; output.curve[count] = 0;
    return true;
}
bool emptyMap(const SerumValue* value) { return value == nullptr || (value->map() != nullptr && value->map()->empty()); }
bool flexCurve(const SerumValue* value, dsp::CurveGraph& output)
{
    const auto* x = child(value, "xVals"), *y = child(value, "yVals"), *c = child(value, "curveVals");
    if (! x || ! y || ! c || ! x->array() || ! y->array() || ! c->array()) return false;
    const double segments = number(child(value, "numPoints"), 1);
    if (segments < 1 || segments >= dsp::CurveGraph::kMaxPoints || std::floor(segments) != segments) return false;
    const size_t count = static_cast<size_t>(segments) + 1;
    if (x->array()->size() < count || y->array()->size() < count || c->array()->size() < count) return false;
    output = {}; output.count = static_cast<int>(count);
    double previous = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto px = (*x->array())[i].number(), py = (*y->array())[i].number(), pc = (*c->array())[i].number();
        if (! px || ! py || ! pc || *px < 0 || *px > 1 || *py < 0 || *py > 1 || *pc < 0 || *pc > 1) return false;
        // Unlike LFOs, flex numPoints counts segments; the last Y is a real endpoint.
        const double effectiveX = i == 0 ? 0 : i == count - 1 ? 1 : *px;
        if (effectiveX < previous) return false;
        previous = effectiveX;
        output.x[i] = static_cast<float>(effectiveX); output.y[i] = static_cast<float>(1 - *py);
        output.curve[i] = static_cast<float>(2 * *pc - 1);
    }
    return true;
}
bool slotCurve(const SerumValue* slot, bool auxiliary, dsp::CurveGraph& graph, bool& enabled)
{
    const double flag = plain(slot, auxiliary ? "kParamAuxCurveData" : "kParamMainCurveData", 0);
    if (flag < 0 || flag > 1 || std::floor(flag) < flag) return false;
    enabled = flag >= 0.5;
    graph = dsp::CurveGraph::preset(3);
    if (! enabled) return true;
    const auto* flex = child(slot, "flex");
    const size_t index = auxiliary ? 1 : 0;
    return flex && flex->array() && flex->array()->size() == 2 && flexCurve(&(*flex->array())[index], graph);
}
// The matrix's own bend knob (kParamCurveIn, -100..100), used when the slot has no custom main curve. Measured (DEV-18i):
// one exponential segment 0 -> 1 with tension CurveIn / 100 (the imported curve law, within 0.01 at +-10..75, 0.004 at 100),
// applied to the source before Bipolar; ignored while a custom main curve is on. kParamCurveOut changed nothing in any
// measured case (with or without aux, inverted or curved aux, bipolar) and is not imported.
bool slotBend(const SerumValue* slot, dsp::CurveGraph& graph)
{
    const double bend = plain(slot, "kParamCurveIn", 0);
    if (! std::isfinite(bend) || std::abs(bend) < 1e-9 || std::abs(bend) > 100) return false;
    graph = dsp::CurveGraph::preset(3);
    graph.curve[0] = static_cast<float>(-bend / 100); // ExponentialCurveGraph evaluates tension -curve
    return true;
}
// Nearest implemented native filter for a Serum 2 type without its own model yet (-1: keep the default). ZDF_A -> Ladder MG;
// other multi-letter types -> their first response at their slope; ring-mod / formant / reverb / distortion-comb and the newer
// comb / allpass families -> a level-only pass-through (their magnitude is near flat away from the notches).
int approximateFilterType(const std::string& type)
{
    if (type == "ZDF_A") return W::SerumLadderMg;
    const bool steep = type.find("24") != std::string::npos;
    if (type == "LH6") return W::SerumLow6;
    if (type.size() >= 3 && type.size() <= 5 && (type.ends_with("12") || type.ends_with("24")))
    {
        const char first = type[0];
        if (first == 'L') return steep ? W::SerumLow24 : W::SerumLow12;
        if (first == 'H') return steep ? W::SerumHigh24 : W::SerumHigh12;
        if (first == 'B' || first == 'P') return steep ? W::SerumBand24 : W::SerumBand12;
        if (first == 'N') return steep ? W::SerumNotch24 : W::SerumNotch12;
    }
    if (type == "ExpBPF") return W::SerumBand12;
    return W::FilterThrough;
}
int oscillatorMode(const SerumValue* group)
{
    const auto type = choice(group, "kParamType", "kOsc_Wavetable");
    if (type == "kOsc_Wavetable") return W::WavetableMode;
    if (type == "kOsc_Sample") return W::SampleMode;
    if (type == "kOsc_MultiSample") return W::MultisampleMode;
    if (type == "kOsc_Granular") return W::GranularMode;
    if (type == "kOsc_Spectral") return W::SpectralMode;
    return -1;
}
int modulationSource(int vendor)
{
    if (vendor == 0) return W::SourceOff;
    if (vendor == 1) return W::ModWheel;
    if (vendor >= 2 && vendor <= 5) return W::Env1 + vendor - 2;
    if (vendor >= 6 && vendor <= 15) return W::Lfo1 + vendor - 6;
    if (vendor == 16) return W::Velocity;
    if (vendor == 17) return W::Keytrack;
    if (vendor == 18) return W::Pressure;
    if (vendor == 21) return W::NoteRandom;
    if (vendor == 22) return W::NoteRandom2;
    if (vendor == 23) return W::NoteAlternate;
    if (vendor == 24) return W::NoteAlternate2;
    if (vendor == 38) return W::Fixed;
    if (vendor == 59) return W::NoteRandomDiscrete;
    if (vendor >= 25 && vendor <= 32) return W::Macro1 + vendor - 25;
    if (vendor >= 39 && vendor <= 48) return W::Lfo1Y + vendor - 39; // "LFO n Y" (label table at 0xddaa50)
    return -1;
}
struct ModTarget
{
    int destination = W::DestOff;
    W::ImportedModulationDomain domain = W::ImportedModulationDomain::Native;
};
ModTarget modulationTarget(const SerumValue* slot)
{
    const auto module = text(child(slot, "destModuleTypeString"));
    const auto name = text(child(slot, "destModuleParamName"));
    const auto index = number(child(slot, "destModuleID"), -1);
    const auto id = number(child(slot, "destModuleParamID"), -1);
    if (index < 0 || std::floor(index) < index) return {};
    if (module == "Oscillator" && index < W::kOscillators)
    {
        const int oscillator = static_cast<int>(index);
        if (name == "kParamCoarsePit" && std::abs(id - 6) < 1e-6)
            return {W::Osc1Pitch + oscillator, W::ImportedModulationDomain::CoarsePitch128};
        if (name == "kParamVolume" && std::abs(id - 1) < 1e-6)
            return {W::Osc1Level + oscillator, W::ImportedModulationDomain::SquaredLevel};
        if (name == "kParamPan" && std::abs(id - 2) < 1e-6)
            return {W::Osc1Pan + oscillator, W::ImportedModulationDomain::Pan2};
        // Unmeasured: assumed to follow the measured normalized add + clamp law of the other oscillator targets.
        if (name == "kParamFine" && std::abs(id - 5) < 1e-6) return {W::Osc1Fine + oscillator, W::ImportedModulationDomain::FineCents200};
        if (name == "kParamOctave" && std::abs(id - 3) < 1e-6) return {W::Osc1Octave + oscillator, W::ImportedModulationDomain::OctaveSteps};
        if (name == "kParamDetune") return {W::Osc1Detune + oscillator, W::ImportedModulationDomain::DetuneCurve};
        if (name == "kParamPitch" && std::abs(id - 4) < 1e-6) return {W::Osc1Semi + oscillator, W::ImportedModulationDomain::Unit};
        if (name == "kParamDetuneWid") return {W::Osc1Blend + oscillator, W::ImportedModulationDomain::Unit};
        if (name == "kParamUnisonStereo") return {W::Osc1Width + oscillator, W::ImportedModulationDomain::Unit};
    }
    // Oscillator 3 is the noise and 4 the sub (measured: squared volume like the oscillators; pan assumed Pan2 like theirs).
    if (const int generator = static_cast<int>(index); module == "Oscillator" && (generator == 3 || generator == 4))
    {
        if (name == "kParamVolume" && std::abs(id - 1) < 1e-6)
            return {generator == 3 ? W::NoiseAmplitude : W::SubAmplitude, W::ImportedModulationDomain::SquaredLevel};
        if (name == "kParamPan" && std::abs(id - 2) < 1e-6) return {generator == 3 ? W::NoisePanDest : W::SubPanDest, W::ImportedModulationDomain::Pan2};
        if (generator == 4 && name == "kParamCoarsePit" && std::abs(id - 6) < 1e-6) return {W::SubCoarse, W::ImportedModulationDomain::CoarsePitch128};
    }
    // Measured (DEV-18i): normalized add + clamp on the target's stored position; the synth applies each target's law.
    if (module == "LFOPointModBus" && index < 16 && name == "kParamValue" && std::abs(id) < 1e-6)
        return {W::LfoPointBus1 + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "LFO" && index < W::kLfos && name == "kParamRate" && std::abs(id) < 1e-6)
        return {W::Lfo1Rate + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "Macro" && index < W::kMacros && name == "kParamValue" && std::abs(id) < 1e-6)
        return {W::Macro1Value + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "Global" && name == "kParamMasterTuning" && std::abs(id - 1) < 1e-6) return {W::MasterTuning, W::ImportedModulationDomain::CoarsePitch128};
    if (module == "Global" && name == "kParamVoiceAmp" && std::abs(id - 2) < 1e-6) return {W::VoiceAmp, W::ImportedModulationDomain::Unit};
    if (module == "Global" && name == "kParamTranspose" && std::abs(id - 16) < 1e-6) return {W::GlobalTranspose, W::ImportedModulationDomain::Unit};
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamLevelOut" && std::abs(id - 8) < 1e-6)
        return {W::Filter1Level + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamVar" && std::abs(id - 6) < 1e-6)
        return {W::Filter1Var + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamDrive" && std::abs(id - 5) < 1e-6) // ladder types only
        return {W::Filter1Drive + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamWet" && std::abs(id - 1) < 1e-6) // unmeasured
        return {W::Filter1Mix + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamFreq" && std::abs(id - 3) < 1e-6)
        return {W::Filter1Cutoff + static_cast<int>(index), W::ImportedModulationDomain::FilterFrequency};
    // Linear 0..1 targets (measured: modulated == the stored value plus amount x source in the normalized domain, clamped).
    if (module == "VoiceFilter" && index < W::kFilters && name == "kParamReso" && std::abs(id - 4) < 1e-6)
        return {W::Filter1Resonance + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "Env" && index < W::kEnvelopes)
    {
        const int base = W::Env1Attack + static_cast<int>(index) * 4;
        if (name == "kParamAttack" && std::abs(id) < 1e-6) return {base, W::ImportedModulationDomain::EnvelopeTime};
        if (name == "kParamDecay" && std::abs(id - 2) < 1e-6) return {base + 1, W::ImportedModulationDomain::EnvelopeTime};
        if (name == "kParamSustain" && std::abs(id - 3) < 1e-6) return {base + 2, W::ImportedModulationDomain::Unit};
        if (name == "kParamRelease" && std::abs(id - 4) < 1e-6) return {base + 3, W::ImportedModulationDomain::EnvelopeTime};
    }
    if ((module == "SampleOsc" || module == "MultiSampleOsc" || module == "GranularOsc" || module == "SpectralOsc") && index < W::kOscillators)
    {
        if (name == "kParamWarp") return {W::Osc1Warp1 + static_cast<int>(index), W::ImportedModulationDomain::Unit};
        if (name == "kParamWarp2") return {W::Osc1Warp2 + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    }
    if (module == "WTOsc" && index < W::kOscillators)
    {
        const int oscillator = static_cast<int>(index);
        if (name == "kParamWarp" && std::abs(id) < 1e-6) return {W::Osc1Warp1 + oscillator, W::ImportedModulationDomain::Unit};
        if (name == "kParamWarp2" && std::abs(id - 3) < 1e-6) return {W::Osc1Warp2 + oscillator, W::ImportedModulationDomain::Unit};
        if (name == "kParamTablePos" && std::abs(id - 6) < 1e-6) return {W::Osc1Position + oscillator, W::ImportedModulationDomain::Unit};
    }
    // Effect controls with a measured normalized law; the synth follows them through import-time tables (FxParameter).
    if (module.starts_with("FX") && index < 300 && id >= 0 && plugins::serumEffectFieldModulatable(module, name))
        return {W::FxParameter, W::ImportedModulationDomain::Unit};
    if (module == "RoutingSlot" && index < 5 && name == "kParamFilterBalance")
        return {W::Source1Balance + static_cast<int>(index), W::ImportedModulationDomain::Unit};
    if (module == "RoutingSlot" && index < W::kRoutingSources)
    {
        if (name == "kParamFXBus1Level" && std::abs(id - 1) < 1e-6)
            return {W::sourceSendDestination(static_cast<int>(index), 0), W::ImportedModulationDomain::SourceSend};
        if (name == "kParamFXBus2Level" && std::abs(id - 2) < 1e-6)
            return {W::sourceSendDestination(static_cast<int>(index), 1), W::ImportedModulationDomain::SourceSend};
    }
    return {};
}
bool modulationRoute(const SerumValue* slot, int& source, int& auxiliary, ModTarget& target)
{
    const auto* pair = child(slot, "source");
    if (! pair || ! pair->array() || pair->array()->size() != 2) return false;
    const auto a = (*pair->array())[0].number(), b = (*pair->array())[1].number();
    if (! a || ! b || *a < 0 || *a > 59 || *b < 0 || *b > 59 || std::floor(*a) < *a || std::floor(*b) < *b) return false;
    source = modulationSource(static_cast<int>(*a)); auxiliary = modulationSource(static_cast<int>(*b));
    target = modulationTarget(slot);
    const double out = plain(slot, "kParamOut", 100), bipolar = plain(slot, "kParamBipolar", 0);
    dsp::CurveGraph mainCurve, auxiliaryCurve; bool mainEnabled = false, auxiliaryEnabled = false;
    return source > W::SourceOff && auxiliary >= W::SourceOff && target.destination != W::DestOff
        && out >= 0 && out <= 100 && bipolar >= 0 && bipolar <= 1
        && slotCurve(slot, false, mainCurve, mainEnabled) && slotCurve(slot, true, auxiliaryCurve, auxiliaryEnabled);
}
const SerumValue* oscillatorAsset(const SerumValue* group, int mode, int index)
{
    const char* prefix = mode == W::SampleMode ? "SampleOsc" : mode == W::MultisampleMode ? "MultiSampleOsc"
                         : mode == W::GranularMode ? "GranularOsc" : mode == W::SpectralMode ? "SpectralOsc" : "WTOsc";
    return child(group, std::string(prefix) + std::to_string(index));
}
} // namespace

void prepareSerumAssets(W& synth, const model::NativeContent& content, std::vector<std::string>& warnings)
{
    const auto decoded = decode(content);
    if (! decoded) { warnings.push_back(decoded.error); return; }
    const auto& root = decoded.state->root();
    if (text(root.find("product")) != "Serum2"
        || (content.format == "serum2-xferjson" && text(root.find("component")) != "processor"))
    { warnings.push_back("Preserved state is not a Serum 2 instrument processor/preset"); return; }
    synth.setImportedEnvelopeProfile(true);
    synth.setImportedLfoProfile(true);
    synth.setImportedVoiceProfile(true);
    synth.setImportedSubProfile(true);
    synth.setImportedNoiseProfile(true);
    synth.setImportedSameNoteLimit(plain(root.find("Global0"), "kParamLimitSameNotePolyphony", 0) >= .5);
    synth.setImportedPolyphony(static_cast<int>(std::lround(plain(root.find("Global0"), "kParamPolyCount", 8))));
    // Constructor fallback and controlled same-time renders establish this
    // prepared rate. Explicit own voiceRateFactor metadata can override it later.
    synth.setVoiceRateFactor(2);
    if (! synth.setImportedMasterLevel(plain(root.find("Global0"), "kParamMasterVolume", 0.25)))
        warnings.push_back("Invalid imported master level");
    std::map<std::string, std::shared_ptr<const devices::WavetableData>> cache;
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        const auto* group = root.find("Oscillator" + std::to_string(oscillator));
        const int warpMode = oscillatorMode(group);
        const auto* tableGroup = warpMode >= 0 && warpMode != W::WavetableMode ? oscillatorAsset(group, warpMode, oscillator)
                                                                               : child(group, "WTOsc" + std::to_string(oscillator));
        for (int warp = 0; warp < 2; ++warp)
        {
            const auto name = choice(tableGroup, warp == 0 ? "kParamWarpMenu" : "kParamWarpMenu2", "kNoWarp");
            if (name == "kNoWarp") continue;
            const auto imported = importedWarp(name, 0);
            synth.setImportedWarpLabel(oscillator, warp, {imported.mode, warpLabel(name), imported.approximate});
        }
        const int mode = oscillatorMode(group);
        if (mode < 0) { warnings.push_back("Unsupported imported oscillator type"); continue; }
        if (mode != W::WavetableMode)
        {
            const auto* asset = oscillatorAsset(group, mode, oscillator);
            devices::OscillatorMediaResult media;
            if (mode == W::MultisampleMode)
            {
                const auto reference = text(child(asset, "sfzPathRelative"));
                const auto file = resolveAsset(content, reference, "Multisamples");
                const auto embedded = text(child(asset, "embedded_sfz"));
                if (! embedded.empty())
                {
                    const auto directory = file.existsAsFile() ? file.getParentDirectory()
                        : juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Multisamples")
                              .getChildFile(juce::String(reference)).getParentDirectory();
                    media = devices::OscillatorMedia::fromSfzText(embedded, directory, "Imported Multisample", true);
                    if (! file.existsAsFile()) warnings.push_back("Embedded SFZ references require their external sample files");
                }
                else if (file.existsAsFile()) media = devices::OscillatorMedia::fromSfz(file, true);
                else media.error = "Multisample bank not found: " + reference;
            }
            else
            {
                const auto reference = text(child(asset, "samplePathRelative"));
                const auto file = resolveAsset(content, reference, "Samples");
                if (! file.existsAsFile()) media.error = "Oscillator sample not found: " + reference;
                else
                {
                    const auto loaded = devices::OscillatorSample::fromFile(file, true);
                    if (! loaded) media.error = loaded.error;
                    else
                    {
                        devices::OscillatorZone zone; zone.sample = loaded.data;
                        const auto baseNote = number(child(asset, "baseNote"), 60);
                        if (baseNote < 0 || baseNote > 127 || std::floor(baseNote) < baseNote)
                        { warnings.push_back("Invalid oscillator sample root note"); continue; }
                        zone.rootKey = static_cast<int>(baseNote);
                        if (! child(asset, "baseNote")) warnings.push_back("Sample root note requires reference normalization");
                        media = devices::OscillatorMedia::fromZones({zone}, file.getFileNameWithoutExtension().toStdString());
                    }
                }
            }
            warnings.insert(warnings.end(), media.warnings.begin(), media.warnings.end());
            if (! media) { warnings.push_back(media.error); continue; }
            synth.setOscillatorMedia(oscillator, media.data);
            // kParamEnvOverride: the preset's own sampler attack/release (seconds) replace the SFZ envelope.
            if (mode == W::MultisampleMode && plain(asset, "kParamEnvOverride", 0) >= .5)
                synth.setImportedMediaEnvelope(oscillator, plain(asset, "kParamEnvAttack", 0), plain(asset, "kParamEnvRelease", 0));
            else synth.setImportedMediaEnvelope(oscillator, -1, -1);
            continue;
        }
        const auto* table = child(group, "WTOsc" + std::to_string(oscillator));
        const auto* embedded = child(table, "embeddedWTData");
        devices::WavetableLoadResult loaded;
        if (embedded != nullptr && embedded->array() != nullptr)
        {
            const auto channels = number(child(table, "numChannels"), 1);
            if (channels < 1 || channels > 2 || std::floor(channels) < channels)
            { warnings.push_back("Unsupported embedded wavetable channel count"); continue; }
            std::vector<float> samples;
            samples.reserve(embedded->array()->size());
            bool valid = true;
            for (const auto& sample : *embedded->array())
            {
                const auto value = sample.number();
                if (! value || std::abs(*value) > 32) { valid = false; break; }
                samples.push_back(static_cast<float>(*value));
            }
            if (! valid) { warnings.push_back("Invalid embedded wavetable samples"); continue; }
            // Embedded tables are not peak-normalised; interpolateAfterLoad (absent in most states) selects the 256-cell crossfade.
            const auto* interpolate = child(table, "interpolateAfterLoad");
            const bool crossfade = interpolate != nullptr
                && (interpolate->boolean().value_or(false) || interpolate->number().value_or(0) >= .5);
            loaded = devices::WavetableData::fromSamples(samples, static_cast<int>(channels), 2048,
                number(child(table, "sampleRate"), 44100), text(child(table, "tableDisplayName"), "Imported Table"),
                devices::WavetableData::PlaybackProfile::Octave16Approx,
                {crossfade ? devices::WavetableFrameLayout::Interpolated256 : devices::WavetableFrameLayout::Steps, serumTableGain, false});
        }
        else
        {
            const auto reference = text(child(table, "pathToWT"), text(child(table, "relativePathToWT"), "S2 Tables/Default Shapes.wav"));
            const auto file = resolveAsset(content, reference);
            if (! file.existsAsFile()) { warnings.push_back("Wavetable not found: " + reference); continue; }
            const auto path = file.getFullPathName().toStdString();
            const auto found = cache.find(path);
            if (found != cache.end()) loaded.data = found->second;
            else
            {
                // A file without the Serum chunk keeps the crossfade (unmeasured).
                const auto flag = serumTableFlag(file);
                loaded = devices::WavetableData::fromFile(file, 2048, devices::WavetableData::PlaybackProfile::Octave16Approx,
                    {flag == 0 ? devices::WavetableFrameLayout::Steps : devices::WavetableFrameLayout::Interpolated256, serumTableGain, true});
                if (loaded) cache[path] = loaded.data;
            }
        }
        if (! loaded) { warnings.push_back(loaded.error); continue; }
        synth.setCustomTable(oscillator, loaded.data);
    }
    const auto* noiseGroup = root.find("Oscillator3");
    const auto* noiseAsset = child(noiseGroup, "NoiseOsc3");
    const auto noiseReference = text(child(noiseAsset, "relativePathToNoiseSample"), "Organics/AC hum1.wav");
    const bool generatedNoise = ! choice(noiseAsset, "kParamNoiseType", "").empty(); // White/Pink/Brown/Geiger play no sample
    const auto noiseFile = generatedNoise ? juce::File() : resolveAsset(content, noiseReference, "Noises");
    if (generatedNoise) synth.setNoiseSampleExpected(false);
    else if (noiseFile.existsAsFile())
    {
        const auto noise = devices::OscillatorSample::fromFile(noiseFile);
        if (noise) synth.setNoiseSample(noise.data);
        else { synth.setParameter(W::NoiseOn, 0); warnings.push_back("Could not prepare Noise sample: " + noise.error); }
    }
    else if (! generatedNoise && plain(noiseGroup, "kParamEnable", 0) >= .5)
    {
        synth.setParameter(W::NoiseOn, 0);
        warnings.push_back("Noise sample not found: " + noiseReference);
    }
    synth.setImportedModulationDomain(W::NoiseAmplitude, W::ImportedModulationDomain::SquaredLevel);
    // The stored wheel position plays until the host sends CC 1 (measured: a preset at 50 sounds as wheel 0.5).
    synth.setImportedModWheel(static_cast<float>(plain(root.find("Global0"), "kParamModWheel", 0)));
    synth.setImportedVoiceAmp(static_cast<float>(plain(root.find("Global0"), "kParamVoiceAmp", 0.5)));
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        synth.setImportedDetuneScale(oscillator, static_cast<float>(plain(root.find("Oscillator" + std::to_string(oscillator)), "kParamUnisonRange", 2) / 2));
    for (int filter = 0; filter < W::kFilters; ++filter)
        synth.setImportedFilterCutoffPosition(filter, static_cast<float>(plain(root.find("VoiceFilter" + std::to_string(filter)), "kParamFreq", 0.5)));
    std::vector<W::LfoPointAssignment> pointAssignments;
    if (const auto* list = root.find("lfoPointModAssignments"); list && list->array())
        for (const auto& entry : *list->array())
        {
            // lfoType 0: the LFO curve (1: its path, not implemented).
            if (number(child(&entry, "lfoType"), 0) != 0.0) { warnings.push_back("LFO path point modulation is not implemented"); continue; }
            pointAssignments.push_back({static_cast<int>(number(child(&entry, "busID"), -1)), static_cast<int>(number(child(&entry, "lfoID"), -1)),
                                        static_cast<int>(number(child(&entry, "pointID"), -1)), static_cast<int>(number(child(&entry, "target"), -1))});
        }
    synth.setImportedLfoPointAssignments(pointAssignments);
    for (int lfo = 0; lfo < W::kLfos; ++lfo)
    {
        const auto* lfoGroup = root.find("LFO" + std::to_string(lfo));
        synth.setImportedLfoSyncDivisions(lfo, plain(lfoGroup, "kParamDotted", 0) >= 0.5, plain(lfoGroup, "kParamTriplets", 0) >= 0.5);
        const auto* data = child(root.find("LFO" + std::to_string(lfo)), "curveData");
        if (emptyMap(data)) continue;
        dsp::CurveGraph graph;
        if (! lfoCurve(data, graph) || ! synth.setLfoCurve(lfo, graph))
            warnings.push_back("Unsupported imported LFO curve " + std::to_string(lfo + 1));
    }
    std::map<std::string, int> fxTargets; // one prepared table per routed effect control, shared by its slots
    synth.clearImportedFxTargets();
    for (int slot = 0; slot < W::kModSlots; ++slot)
    {
        for (const bool via : {false, true})
            synth.setModulationCurve(slot, via, dsp::CurveGraph::preset(3), false, W::CurveLaw::ImportedExponential);
        synth.setImportedFxSlot(slot, -1);
        synth.setAuxiliaryInverted(slot, false);
        int source = 0, auxiliary = 0; ModTarget target;
        const auto* node = root.find("ModSlot" + std::to_string(slot));
        if (modulationRoute(node, source, auxiliary, target))
        {
            synth.setImportedModulationDomain(target.destination, target.domain);
            if (target.destination == W::FxParameter)
            {
                const auto module = text(child(node, "destModuleTypeString")), field = text(child(node, "destModuleParamName"));
                const int address = static_cast<int>(number(child(node, "destModuleID"), -1));
                const auto key = module + "/" + std::to_string(address) + "/" + field;
                auto found = fxTargets.find(key);
                if (found == fxTargets.end())
                {
                    int prepared = -1;
                    if (const auto table = serumEffectModulation(root, address, module, field))
                    {
                        std::vector<W::FxModulationCurve> curves;
                        for (const auto& curve : table->curves) curves.push_back({curve.parameter, curve.values});
                        prepared = synth.addImportedFxTarget(table->module, table->base, std::move(curves));
                    }
                    if (prepared < 0)
                        warnings.push_back("Modulation slot " + std::to_string(slot + 1) + ": " + module + " " + field
                                           + " has no native control to follow; route kept in the source only");
                    found = fxTargets.emplace(key, prepared).first;
                }
                synth.setImportedFxSlot(slot, found->second);
            }
            for (const bool via : {false, true})
            {
                dsp::CurveGraph graph; bool enabled = false;
                if (slotCurve(node, via, graph, enabled))
                {
                    if (! via && ! enabled) enabled = slotBend(node, graph);
                    synth.setModulationCurve(slot, via, graph, enabled, W::CurveLaw::ImportedExponential);
                }
            }
            // Measured: Inverted flips the aux after its curve (1 - curve(x)); an inverted unset aux (0) passes the full route.
            synth.setAuxiliaryInverted(slot, plain(node, "kParamAuxInverted", 0) >= 0.5);
        }
    }
    const auto effects = parseSerumEffects(root, content);
    warnings.insert(warnings.end(), effects.warnings.begin(), effects.warnings.end());
    if (effects.hasRacks)
    {
        std::string error;
        if (! synth.setEffectsConfiguration(effects.configuration, error))
            warnings.push_back("Could not prepare imported effect graph: " + error);
    }
}

SerumPatchResult importSerumPatch(std::span<const uint8_t> bytes, const std::string& sourceDirectory,
                                std::span<const uint8_t> controller)
{
    SerumPatchResult result;
    auto decoded = decodeSerumState(bytes);
    std::string format = "serum2-xferjson";
    if (! decoded) { decoded = decodeSerumPreset(bytes); format = "serum2-preset"; }
    if (! decoded) { result.error = decoded.error; return result; }
    const auto& state = *decoded.state;
    const auto& root = state.root();
    if (text(root.find("product")) != "Serum2"
        || (format == "serum2-xferjson" && text(root.find("component")) != "processor"))
    { result.error = "Choose a Serum 2 instrument processor or instrument preset"; return result; }
    if (bytes.size() + controller.size() > model::NativeContent::maxBytes)
    { result.error = "Preset exceeds the native content size limit"; return result; }
    if (! controller.empty())
    {
        const auto control = decodeSerumState(controller);
        if (! control || text(control.state->root().find("product")) != "Serum2"
            || text(control.state->root().find("component")) != "controller")
        { result.error = "Invalid Serum 2 controller state"; return result; }
    }
    auto content = std::make_shared<model::NativeContent>();
    content->format = format; content->sourceDirectory = sourceDirectory;
    content->data.assign(bytes.begin(), bytes.end());
    if (! controller.empty()) content->controllerData.assign(controller.begin(), controller.end());
    result.device = devices::nativeDeviceModel("SonoraWavetable");
    const auto probe = devices::createNative(result.device);
    result.device.nativeContent = std::move(content); result.device.nativeContentEdited = true;
    result.device.displayName = text(root.find("presetName"), "Sonora Prism");
    const auto& infos = probe->parameters();
    const auto assign = [&](int index, double value)
    {
        const auto& info = infos[static_cast<size_t>(index)];
        if (value < info.minValue || value > info.maxValue)
            result.warnings.push_back("Native range differs for " + info.name);
        result.device.parameters[static_cast<size_t>(index)].value = std::clamp(value, static_cast<double>(info.minValue), static_cast<double>(info.maxValue));
    };
    // These initialized scalar defaults were measured from the 2.0.24 hosted instance, not inferred from "default".
    // Other builds need reference normalization before DSP/preset compatibility can be claimed.
    if (text(root.find("productVersion")) != "2.0.24")
        result.warnings.push_back("Vendor defaults require normalization for this Serum version");
    const auto* global = root.find("Global0");
    assign(W::SourceRoutingOn, 1);
    assign(W::DirectGain, 2 * plain(global, "kParamDirectVol", .5));
    std::array<int, W::kRoutingSources> sourceDestinations{};
    for (int source = 0; source < W::kRoutingSources; ++source)
    {
        const auto* routing = root.find("RoutingSlot" + std::to_string(source));
        sourceDestinations[static_cast<size_t>(source)] = sourceDestination(routing, source == 0 ? W::RouteToFilters : W::RouteToMain, result.warnings);
        assign(W::sourceRoutingParam(source, W::SourceDestination), sourceDestinations[static_cast<size_t>(source)]);
        assign(W::sourceRoutingParam(source, W::SourceFilterBalance), plain(routing, "kParamFilterBalance", -100) / 100);
        assign(W::sourceRoutingParam(source, W::SourceBus1), plain(routing, "kParamFXBus1Level", 0) / 100);
        assign(W::sourceRoutingParam(source, W::SourceBus2), plain(routing, "kParamFXBus2Level", 0) / 100);
        assign(W::sourceRoutingParam(source, W::SourceEnvelope), plain(routing, "kParamViaEnv1", 1));
    }
    if (sourceDestinations[5] == W::RouteToFilters && sourceDestinations[6] == W::RouteToFilters)
        result.warnings.push_back("Cyclic filter routing is uncalibrated; independent sends and source state retained");
    assign(W::voiceModeParam(W::MonoOn), plain(global, "kParamMonoToggle", 0));
    assign(W::PolyphonyParam, std::clamp(plain(global, "kParamPolyCount", 8), 1.0, static_cast<double>(W::kVoices)));
    assign(W::voiceModeParam(W::LegatoOn), plain(global, "kParamLegato", 0));
    assign(W::voiceModeParam(W::PortaAlways), plain(global, "kParamPortaAlways", 0));
    assign(W::voiceModeParam(W::PortaScaled), plain(global, "kParamPortaScaled", 0));
    assign(W::voiceModeParam(W::PortaTime), plain(global, "kParamPortamentoTime", 0));
    assign(W::voiceModeParam(W::PortaCurve), plain(global, "kParamPortamentoCurve", 0));
    const double master = plain(global, "kParamMasterVolume", 0.25);
    assign(W::Volume, master > 0 ? 20 * std::log10(master * std::sqrt(2.0)) : -60);
    assign(W::Transpose, plain(global, "kParamTranspose", 0));
    assign(W::BendUp, plain(global, "kParamBendRangeUp", 2)); assign(W::BendDown, -plain(global, "kParamBendRangeDn", -2));
    if (std::abs(plain(global, "kParamMonoToggle", 0)) > 0 || std::abs(plain(global, "kParamPortamentoTime", 0)) > 0)
        result.warnings.push_back("Imported mono/portamento timing and extreme curves require full reference calibration");
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
    {
        const auto* group = root.find("Oscillator" + std::to_string(oscillator));
        const auto* table = child(group, "WTOsc" + std::to_string(oscillator));
        const auto setOsc = [&](W::OscParam p, double value) { assign(W::oscParam(oscillator, p), value); };
        const int mode = oscillatorMode(group);
        const bool wavetable = mode == W::WavetableMode;
        setOsc(W::On, mode >= 0 ? plain(group, "kParamEnable", oscillator == 0 ? 1 : 0) : 0);
        assign(W::mediaParam(oscillator, W::MediaMode), std::max(0, mode));
        setOsc(W::Table, W::Custom); setOsc(W::Position, (plain(table, "kParamTablePos", 1) - 1) / 255);
        setOsc(W::Level, plain(group, "kParamVolume", 0.5625)); setOsc(W::OscPan, plain(group, "kParamPan", 0) / 50);
        setOsc(W::Octave, plain(group, "kParamOctave", 0)); setOsc(W::Semi, plain(group, "kParamPitch", 0));
        setOsc(W::Fine, plain(group, "kParamFine", 0)); setOsc(W::Unison, plain(group, "kParamUnison", 1));
        assign(W::coarseParam(oscillator), plain(group, "kParamCoarsePit", 0));
        setOsc(W::Blend, plain(group, "kParamDetuneWid", 75) / 100); setOsc(W::Width, plain(group, "kParamUnisonStereo", 100) / 100);
        setOsc(W::Phase, plain(table, "kParamInitialPhase", 180) / 360);
        setOsc(W::Random, plain(table, "kParamRandomPhase", 100) / 100);
        setOsc(W::Detune, 200 * std::pow(std::clamp(plain(group, "kParamDetune", 0.25), 0.0, 1.0), 1.25)); // measured: D = 200 detune^1.25 cents
        if (! wavetable)
        {
            assign(W::mediaParam(oscillator, W::MediaStartPosition), plain(group, "kParamStart", 0) / 100);
            const auto loop = choice(group, "kParamLoopMode", "kNoLoop");
            assign(W::mediaParam(oscillator, W::MediaLoopMode), loop == "kForward" ? 2 : loop == "kPingPong" ? 4 : 0);
            result.warnings.push_back("Oscillator " + std::to_string(oscillator + 1) + " media pitch/scan/loop/warp requires reference calibration");
        }
        if (plain(group, "kParamUnison", 1) > 1) result.warnings.push_back("Unison distribution requires reference calibration");
        // Sample/multisample/granular/spectral oscillators keep their warps in their own group.
        const auto* warpGroup = wavetable || mode < 0 ? table : oscillatorAsset(group, mode, oscillator);
        for (int warp = 0; warp < 2; ++warp)
        {
            const auto name = choice(warpGroup, warp == 0 ? "kParamWarpMenu" : "kParamWarpMenu2", "kNoWarp");
            const auto imported = importedWarp(name, plain(warpGroup, warp == 0 ? "kParamWarp" : "kParamWarp2", 0));
            setOsc(warp == 0 ? W::Warp1Amount : W::Warp2Amount, imported.amount);
            setOsc(warp == 0 ? W::Warp1Mode : W::Warp2Mode, imported.mode);
            if (imported.approximate) result.warnings.push_back("Warp approximated by a native shape warp (unmeasured): " + name);
            else if (name != "kNoWarp" && imported.mode == W::WarpOff) result.warnings.push_back("Warp requires reference calibration: " + name);
        }
    }
    assign(W::SubOn, plain(root.find("Oscillator4"), "kParamEnable", 0));
    assign(W::SubLevel, plain(root.find("Oscillator4"), "kParamVolume", .75));
    assign(W::SubOctave, plain(root.find("Oscillator4"), "kParamOctave", 0));
    assign(W::SubPan, 2 * plain(root.find("Oscillator4"), "kParamPan", 0) / 100);
    const auto* sub = child(root.find("Oscillator4"), "SubOsc4");
    assign(W::SubPhase, plain(sub, "kParamInitialPhase", 0) / 360);
    assign(W::SubContinuous, plain(sub, "kParamContiguousPhase", 0));
    const auto subShape = choice(sub, "kParamShape", "kSine");
    if (subShape == "kTriangle") assign(W::SubWave, 1);
    else if (subShape == "kSaw") assign(W::SubWave, 2);
    else if (subShape == "kSquare") assign(W::SubWave, 3);
    if (subShape != "kSine") result.warnings.push_back("Sub waveform needs native calibration: " + subShape);
    if (plain(sub, "kParamContiguousPhase", 0) >= .5)
        result.warnings.push_back("Sub phase continuity during envelope release requires calibration");
    const auto* noiseGroup = root.find("Oscillator3");
    const auto* noise = child(noiseGroup, "NoiseOsc3");
    assign(W::NoiseOn, plain(noiseGroup, "kParamEnable", 0));
    assign(W::NoiseLevel, plain(noiseGroup, "kParamVolume", .0625));
    assign(W::NoisePan, plain(noiseGroup, "kParamPan", 0) / 50);
    const double noiseColor = plain(noise, "kParamColor", .5);
    assign(W::NoisePitch, noiseColor < .5 ? noiseColor * 384 - 192 : noiseColor * 96 - 48);
    assign(W::NoiseFine, plain(noise, "kParamFine", 0) * 96);
    assign(W::NoiseKeytrack, plain(noiseGroup, "kParamPitchTrack", 0));
    assign(W::NoisePhase, plain(noise, "kParamInitialPhase", 0) / 100);
    assign(W::NoiseRandom, plain(noise, "kParamRandomPhase", 0) / 100);
    const auto noiseType = choice(noise, "kParamNoiseType", "");
    const int generated = noiseType == "White" ? W::NoiseWhite : noiseType == "Pink" ? W::NoisePink : noiseType == "Brown" ? W::NoiseBrown
                        : noiseType == "Geiger" ? W::NoiseGeiger : W::NoiseSampleType;
    assign(W::NoiseTypeParam, generated);
    if (generated != W::NoiseSampleType) { assign(W::NoiseColor, 0); result.warnings.push_back("Generated " + noiseType + " noise is Sonora's own generator (unmeasured)"); }
    else if (! noiseType.empty()) result.warnings.push_back("Unsupported noise type: " + noiseType);
    if (plain(noiseGroup, "kParamEnable", 0) > 0)
        result.warnings.push_back("Noise interpolation, phase span and extreme rates require calibration; source sample retained");
    for (int envelope = 0; envelope < W::kEnvelopes; ++envelope)
    {
        const auto* group = root.find("Env" + std::to_string(envelope));
        const auto setEnv = [&](W::EnvParam p, double value) { assign(W::envParam(envelope, p), value); };
        setEnv(W::Delay, 0); setEnv(W::Attack, plain(group, "kParamAttack", envelope == 0 ? 0.001 : 0.000515363186));
        setEnv(W::Hold, plain(group, "kParamHold", 0)); setEnv(W::Decay, plain(group, "kParamDecay", 1));
        setEnv(W::Sustain, plain(group, "kParamSustain", 1)); setEnv(W::Release, plain(group, "kParamRelease", 0.014700845523));
        // Sparse (absent) curve values are 60 in the reference (measured bit-exactly); the default patch stores 50 / 66.6 / 66.6 explicitly.
        setEnv(W::AttackCurve, (plain(group, "kParamCurve1", 60) - 50) / 50);
        setEnv(W::DecayCurve, (plain(group, "kParamCurve2", 60) - 50) / 50);
        setEnv(W::ReleaseCurve, (plain(group, "kParamCurve3", 60) - 50) / 50);
        if (envelope > 0)
        {
            assign(W::envLevelParam(envelope, false), plain(group, "kParamStart", 0));
            assign(W::envLevelParam(envelope, true), plain(group, "kParamEnd", 0));
        }
    }
    for (int filter = 0; filter < W::kFilters; ++filter)
    {
        const auto* group = root.find("VoiceFilter" + std::to_string(filter));
        const double enabled = plain(group, "kParamEnable", 0);
        assign(W::filterParam(filter, W::FilterOn), enabled);
        assign(W::filterParam(filter, W::Cutoff), W::observedFilterHertz(static_cast<float>(plain(group, "kParamFreq", 0.5))));
        assign(W::filterParam(filter, W::Resonance), plain(group, "kParamReso", 10) / 100);
        assign(W::filterParam(filter, W::FilterMix), plain(group, "kParamWet", 100) / 100);
        // A switch in the reference (factory presets store 0 or 1): full keytrack around W::kImportedKeytrackCenter.
        assign(W::filterParam(filter, W::FilterKeytrack), plain(group, "kParamKeyTrack", 0) >= .5 ? 1 : 0);
        const double level = plain(group, "kParamLevelOut", .5);
        if (level < 0 || level > 1) result.warnings.push_back("Imported filter output level is outside its measured range");
        assign(W::filterOutputParam(filter), 4 * std::clamp(level, 0.0, 1.0) * std::clamp(level, 0.0, 1.0));
        // Voice filter type (vendor default: MG Low 12). Implemented: the measured MG Low ladders (6/12/18/24 dB) and the
        // normal Low / High / Band / Notch family (dsp/MeasuredSvfFilter.h).
        const auto type = choice(group, "kParamType", "MgL12");
        static const std::map<std::string, int> measuredTypes{
            {"MgL6", W::MgLow6}, {"MgL12", W::MgLow12}, {"MgL18", W::MgLow18}, {"MgL24", W::MgLow24},
            {"L6", W::SerumLow6}, {"L12", W::SerumLow12}, {"L18", W::SerumLow18}, {"L24", W::SerumLow24},
            {"H6", W::SerumHigh6}, {"H12", W::SerumHigh12}, {"H18", W::SerumHigh18}, {"H24", W::SerumHigh24},
            {"B12", W::SerumBand12}, {"B24", W::SerumBand24}, {"N12", W::SerumNotch12}, {"N24", W::SerumNotch24},
            // Ladder MG (measured); the remaining types are approximated by their nearest implemented relative.
            {"LadderMg", W::SerumLadderMg}, {"DirtyMg", W::SerumDirtyMg}, {"LadderEMS", W::SerumLadderEms}, {"LadderAcid", W::SerumLadderAcid},
            // Peak / dual / morph types (dsp/MeasuredMultiFilter.h) and the comb / flanger / phaser family (dsp/MeasuredCombFilter.h),
            // in the reference's own order.
            {"P12", W::SerumPeak12}, {"P24", W::SerumPeak24}, {"LH6", W::SerumLH6}, {"LH12", W::SerumLH12}, {"LB12", W::SerumLB12},
            {"LP12", W::SerumLP12}, {"LN12", W::SerumLN12}, {"HB12", W::SerumHB12}, {"HP12", W::SerumHP12}, {"HN12", W::SerumHN12},
            {"BP12", W::SerumBP12}, {"BN12", W::SerumBN12}, {"PP12", W::SerumPP12}, {"PN12", W::SerumPN12}, {"NN12", W::SerumNN12},
            {"LBH12", W::SerumLBH12}, {"LBH24", W::SerumLBH24}, {"LPH12", W::SerumLPH12}, {"LPH24", W::SerumLPH24},
            {"LNH12", W::SerumLNH12}, {"LNH24", W::SerumLNH24}, {"BPN12", W::SerumBPN12}, {"BPN24", W::SerumBPN24},
            {"CombP", W::SerumCombP}, {"CombN", W::SerumCombN}, {"CombL6P", W::SerumCombL6P}, {"CombL6N", W::SerumCombL6N},
            {"CombH6P", W::SerumCombH6P}, {"CombH6N", W::SerumCombH6N}, {"CombHL6P", W::SerumCombHL6P}, {"CombHL6N", W::SerumCombHL6N},
            {"FlangeP", W::SerumFlangeP}, {"FlangeN", W::SerumFlangeN}, {"FlangeL6P", W::SerumFlangeL6P}, {"FlangeL6N", W::SerumFlangeL6N},
            {"FlangeH6P", W::SerumFlangeH6P}, {"FlangeH6N", W::SerumFlangeH6N}, {"FlangeHL6P", W::SerumFlangeHL6P}, {"FlangeHL6N", W::SerumFlangeHL6N},
            {"Phase12P", W::SerumPhase12P}, {"Phase12N", W::SerumPhase12N}, {"Phase24P", W::SerumPhase24P}, {"Phase24N", W::SerumPhase24N},
            {"Phase36P", W::SerumPhase36P}, {"Phase36N", W::SerumPhase36N}, {"Phase48P", W::SerumPhase48P}, {"Phase48N", W::SerumPhase48N},
            {"Phase48L6P", W::SerumPhase48L6P}, {"Phase48L6N", W::SerumPhase48L6N}, {"Phase48H6P", W::SerumPhase48H6P},
            {"Phase48H6N", W::SerumPhase48H6N}, {"Phase48HL6P", W::SerumPhase48HL6P}, {"Phase48HL6N", W::SerumPhase48HL6N},
            {"FlangePhase12HL6P", W::SerumFlangePhase12HL6P}, {"FlangePhase12HL6N", W::SerumFlangePhase12HL6N}};
        auto measured = measuredTypes.find(type);
        const int approximation = measured != measuredTypes.end() ? -1 : approximateFilterType(type);
        if (measured != measuredTypes.end() || approximation >= 0)
        {
            const int mode = approximation >= 0 ? approximation : measured->second;
            if (approximation >= 0 && enabled > 0)
                result.warnings.push_back("Filter type " + type + " is approximated by the native " + W::filterTypeName(mode) + " (unmeasured)");
            assign(W::filterParam(filter, W::FilterMode), mode);
            // The vendor Drive control (percent) is an input gain in front of a soft clipper: 0 -> 1x ... 100 -> 16x
            // (measured for the ladders and, with the same table, for the normal family).
            const double drive = std::clamp(plain(group, "kParamDrive", 0), 0.0, 100.0) / 100;
            assign(W::filterParam(filter, W::Drive), 20 * std::log10(dsp::ladderDriveGain(static_cast<float>(drive))));
            const double var = plain(group, "kParamVar", 0);
            // Normal family: Var (0..100 %) restores that fraction of the resonance level loss (measured; negative acts as 0).
            // Approximated (unmeasured) multi types are their first response without that compensation.
            // MG Low ladders: Var restores that fraction of the ladder's resonance level loss (measured, same law).
            // Peak / dual / morph / comb types: Var 0..100 % is the measured 0..1 control (dual second cutoff, morph position,
            // loop filter corners; the peak types use the normal family's law).
            if (approximation >= 0 && W::isMeasuredSvfFilter(mode)) assign(W::filterParam(filter, W::FilterMorph), 1);
            else if (W::isMeasuredSvfFilter(mode) || (mode >= W::MgLow6 && mode <= W::MgLow24) || W::isMeasuredMultiFilter(mode)
                     || W::isMeasuredCombFilter(mode))
                assign(W::filterParam(filter, W::FilterMorph), std::clamp(var / 100, 0.0, 1.0));
            else if (enabled > 0 && std::abs(var) > 1e-9 && mode != W::SerumLadderAcid) // Ladder Acid: Var has no effect (measured)
                result.warnings.push_back("Filter Var is not implemented for " + type);
            // These keep the measured comb / phaser topology, but their loop filters did not fit the reference closely (median
            // 2..10 dB off at some Var settings).
            static const std::set<int> approximateLoops{W::SerumFlangeHL6P, W::SerumFlangeHL6N, W::SerumPhase48L6P, W::SerumPhase48L6N,
                W::SerumPhase48H6P, W::SerumPhase48H6N, W::SerumPhase48HL6P, W::SerumPhase48HL6N, W::SerumFlangePhase12HL6P, W::SerumFlangePhase12HL6N};
            if (enabled > 0 && approximateLoops.contains(mode))
                result.warnings.push_back("Filter type " + type + " has an approximate loop filter");
            if (enabled > 0 && std::abs(plain(group, "kParamStereo", 50) - 50) > 1e-9)
                result.warnings.push_back("Filter Stereo is not implemented for " + type);
        }
        else if (enabled > 0)
            result.warnings.push_back("Filter type " + type + " is not implemented yet (the native default filter is used)");
    }
    for (int lfo = 0; lfo < W::kLfos; ++lfo)
    {
        const auto* group = root.find("LFO" + std::to_string(lfo));
        assign(W::lfoParam(lfo, W::Rate), plain(group, "kParamRate", 6.25));
        assign(W::lfoParam(lfo, W::SyncOn), plain(group, "kParamBeatSync", 1));
        assign(W::lfoAnchorParam(lfo), plain(group, "kParamAnchored", 1));
        assign(W::lfoRateScaleParam(lfo), plain(group, "kParamRate10x", 0));
        assign(W::lfoParam(lfo, W::Division), dsp::LfoTempoDivisions::fromStoredRate(plain(group, "kParamRate", 6.25),
            plain(group, "kParamDotted", 0) >= 0.5, plain(group, "kParamTriplets", 0) >= 0.5));
        assign(W::lfoParam(lfo, W::Shape), 2);
        assign(W::lfoParam(lfo, W::Bipolar), 0);
        const auto mode = choice(group, "kParamMode", "Trigger");
        assign(W::lfoParam(lfo, W::Mode), mode == "Free" ? W::Global : mode == "Envelope" ? W::OneShot : W::Retrigger);
        assign(W::lfoParam(lfo, W::LfoPhase), plain(group, "kParamPhase", 0) / 360);
        if (! emptyMap(child(group, "curveData"))) assign(W::lfoParam(lfo, W::Shape), W::CustomLfoShape);
        const auto type = choice(group, "kParamType", "");
        const int lfoType = type == "Lorenz" ? W::LfoLorenz : type == "Rossler" ? W::LfoRossler : type == "RandomSH" ? W::LfoRandomHold : W::LfoNormal;
        assign(W::lfoTypeParam(lfo), lfoType);
        if (lfoType != W::LfoNormal) result.warnings.push_back("LFO " + std::to_string(lfo + 1) + " " + type + " uses Sonora's own attractor scaling (unmeasured)");
        else if (! type.empty()) result.warnings.push_back("LFO " + std::to_string(lfo + 1) + " type " + type + " is not implemented; the curve is used");
    }
    for (int macro = 0; macro < W::kMacros; ++macro)
        assign(W::macroParam(macro), plain(root.find("Macro" + std::to_string(macro)), "kParamValue", 0) / 100);
    int effects = 0, modulation = 0, mapped = 0;
    for (int rack = 0; rack < 3; ++rack)
        if (const auto* stages = child(root.find("FXRack" + std::to_string(rack)), "FX"); stages != nullptr && stages->array() != nullptr)
            effects += static_cast<int>(stages->array()->size());
    for (int slot = 0; slot < W::kModSlots; ++slot)
    {
        const auto* node = root.find("ModSlot" + std::to_string(slot));
        if (! child(node, "source")) continue;
        ++modulation;
        int source = 0, auxiliary = 0; ModTarget target;
        if (! modulationRoute(node, source, auxiliary, target)) continue;
        assign(W::modParam(slot, W::ModSource), source);
        assign(W::modParam(slot, W::AuxSource), auxiliary);
        assign(W::modParam(slot, W::ModDestination), target.destination);
        assign(W::modParam(slot, W::ModAmount), plain(node, "kParamAmount", 0) / 100);
        assign(W::outParam(slot), plain(node, "kParamOut", 100) / 100);
        assign(W::modParam(slot, W::ModBipolar), plain(node, "kParamBipolar", 0));
        ++mapped;
    }
    if (effects > 0) result.warnings.push_back(std::to_string(effects) + " effect modules use the partial native graph adapter; routing and DSP calibration remain pending");
    if (modulation > mapped) result.warnings.push_back(std::to_string(modulation - mapped) + " modulation routes require native target/curve support");
    if (mapped > 0) result.warnings.push_back("Imported normalized modulation domains require complete timing/curve calibration");
    result.warnings.push_back("Native audio calibration is pending; source preset retained");
    result.warnings.push_back("Extreme envelope curves and nonlinear LFO lookup require further calibration");
    return result;
}
} // namespace sonora::plugins
