#include "plugins/SerumEffectsImport.h"

#include "dsp/LadderFilter.h"
#include "devices/native/SerumDistortion.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

namespace sonora::plugins
{
namespace
{
using E = devices::WavetableEffects;
using V = SerumValue;
constexpr std::array<std::string_view, 15> moduleKeys{
    "FXDistortion", "FXFlanger", "FXPhaser", "FXChorus", "FXDelay", "FXComp", "FXReverb", "FXEQ",
    "FXFilter", "FXHyperD", "FXBode", "FXConv", "FXUtils", "", "FXSplit3"};
constexpr size_t maxWarnings = 512;

const V* child(const V* value, std::string_view key) { return value ? value->find(key) : nullptr; }
std::optional<double> finiteNumber(const V* value)
{
    const auto number = value ? value->number() : std::nullopt;
    return number && std::isfinite(*number) ? number : std::nullopt;
}
bool sparse(const V* value)
{
    return value == nullptr || (value->text() && *value->text() == "default");
}

// These omissions were observed after fresh controlled writes/recapture. They apply only to
// this measured field/type pair, not to other controls with a similar name.
std::optional<double> observedDefault(E::Type type, std::string_view field)
{
    const bool tested = type == E::Type::Distortion || type == E::Type::Delay || type == E::Type::Eq
                        || type == E::Type::Filter || type == E::Type::Utility || type == E::Type::Compressor || type == E::Type::Reverb;
    if (tested && field == "kParamEnable") return 1;
    if (tested && field == "kParamLevelOut") return 0.5;
    if ((type == E::Type::Distortion || type == E::Type::Filter || type == E::Type::Utility || type == E::Type::Compressor) && field == "kParamWet") return 100;
    if (type == E::Type::Delay && field == "kParamWet") return 30;
    if (type == E::Type::Reverb && field == "kParamWet") return 20;
    if (type == E::Type::Phaser && field == "kParamWet") return 100;
    if (type == E::Type::HyperDimension && field == "kParamWet") return 100;
    if (type == E::Type::Compressor)
    {
        if (field == "kParamThresh") return 0.5;
        if (field == "kParamRatio") return 4;
        if (field == "kParamAttack") return 90.091;   // plain milliseconds; the smoothing time constant is half of it
        if (field == "kParamRelease") return 89.929;
        if (field == "kParamMakeup") return 0;
    }
    if (type == E::Type::Utility)
    {
        if (field == "kParamWidth") return 100;
        if (field == "kParamBalance" || field == "kParamHPF" || field == "kParamLFMono") return 0;
        if (field == "kParamLPF") return 20000; // the control tops out at 20 kHz (22050 behaves identically)
        if (field == "kParamLFXover") return 90;
    }
    if (type == E::Type::Eq)
    {
        if (field == "kParamGain1" || field == "kParamGain2" || field == "kParamType1" || field == "kParamType2") return 0;
        if (field == "kParamFreq1") return 209.61696906559536;
        if (field == "kParamFreq2") return 2040.5360638259378;
        if (field == "kParamReso1" || field == "kParamReso2") return 60;
    }
    if (type == E::Type::Filter)
    {
        if (field == "kParamFreq") return 0.5;
        if (field == "kParamReso" || field == "kParamDrive") return 0;
    }
    if (type == E::Type::Delay)
    {
        if (field == "kParamOffsetL" || field == "kParamOffsetR") return 1;
        if (field == "kParamTimeL" || field == "kParamTimeR") return 0.0773125;
        if (field == "kParamFeedback") return 41.9;
        if (field == "kParamFreq") return 849;
        if (field == "kParamBW") return 6.75;
        if (field == "kParamMode") return 0;
    }
    return std::nullopt;
}


// Measured normalized->plain laws of modulatable FX controls (hosted black-box: a Macro route at amount A renders
// sample-identical to the stored value law(u0 + A); endpoints from where the stored value stops clamping).
struct FieldLaw
{
    enum class Kind { Linear, Power, Log } kind = Kind::Linear;
    double low = 0, high = 1, exponent = 1;
    double plain(double u) const
    {
        u = std::clamp(u, 0.0, 1.0);
        if (kind == Kind::Log) return low * std::pow(high / low, u);
        return low + (high - low) * (kind == Kind::Power ? std::pow(u, exponent) : u);
    }
    double normalized(double value) const
    {
        value = std::clamp(value, std::min(low, high), std::max(low, high));
        if (kind == Kind::Log) return std::log(value / low) / std::log(high / low);
        const double u = (value - low) / (high - low);
        return kind == Kind::Power ? std::pow(std::max(u, 0.0), 1 / exponent) : u;
    }
};
std::optional<FieldLaw> fieldLaw(std::string_view key, std::string_view field)
{
    using K = FieldLaw::Kind;
    const bool mixed = key == "FXDistortion" || key == "FXDelay" || key == "FXFilter" || key == "FXUtils" || key == "FXComp"
                       || key == "FXPhaser" || key == "FXHyperD" || key == "FXReverb";
    if (field == "kParamWet" && mixed) return FieldLaw{K::Linear, 0, 100};
    if (field == "kParamLevelOut" && key.starts_with("FX")) return FieldLaw{K::Linear, 0, 1};
    if (key == "FXDistortion" && field == "kParamDrive") return FieldLaw{K::Linear, 0, 100};
    if (key == "FXFilter" && field == "kParamFreq") return FieldLaw{K::Linear, 0, 1};
    if (key == "FXComp" && field == "kParamMakeup") return FieldLaw{K::Power, 1, 31, 2};
    if (key == "FXReverb" && field == "kParamDelay") return FieldLaw{K::Power, 0, 250, 2};
    if (key == "FXUtils" && field == "kParamLPF") return FieldLaw{K::Log, 50, 20000};
    if (key == "FXEQ" && (field == "kParamGain1" || field == "kParamGain2")) return FieldLaw{K::Linear, -24, 24};
    if (key == "FXEQ" && (field == "kParamFreq1" || field == "kParamFreq2")) return FieldLaw{K::Log, 22050.0 / 1024, 20000};
    if (key == "FXDelay")
    {
        if (field == "kParamFeedback") return FieldLaw{K::Linear, 0, 100};
        if (field == "kParamFreq") return FieldLaw{K::Log, 40, 18000};
        if (field == "kParamBW") return FieldLaw{K::Linear, 0.75, 8.25};
        if (field == "kParamTimeL" || field == "kParamTimeR") return FieldLaw{K::Power, 0.001, 0.501, 4};
        if (field == "kParamOffsetL" || field == "kParamOffsetR") return FieldLaw{K::Linear, 0.5, 1.5};
    }
    return std::nullopt;
}

bool legacyPresetFormat(const SerumValue& root)
{
    const auto* fileType = root.find("fileType");
    const auto version = finiteNumber(root.find("version"));
    return fileType && fileType->text() && *fileType->text() == "SerumPreset" && version && *version <= 6;
}
struct Parser
{
    SerumEffectsImportResult result;
    std::string sourceDirectory;
    double sampleRate = 44100;
    size_t moduleCount = 0, impulseBytes = 0, suppressedWarnings = 0;
    std::string_view overrideField; // modulation tables: this field reads overrideValue instead of the stored value
    std::optional<double> overrideValue;
    // Preset files of format 3..6 (Serum 2.0.12 era) store FX Distortion Freq in an older domain: the hosted plugin
    // migrates it on load to 160/171 of the stored value (measured on load/save round trips; format 7+ unchanged).
    bool legacyDistortionFreq = false;
    std::map<std::string, std::shared_ptr<const devices::ConvolutionIR>, std::less<>> impulses;

    void warn(const std::string& location, const std::string& message)
    {
        if (result.warnings.size() < maxWarnings) result.warnings.push_back("Serum FX " + location + ": " + message);
        else ++suppressedWarnings;
    }
    static std::string location(int rack, size_t index)
    {
        return "rack " + std::to_string(rack) + " entry " + std::to_string(index);
    }
    std::optional<double> parameter(const V* params, E::Type type, std::string_view field, const std::string& where,
                                    std::set<std::string, std::less<>>& handled)
    {
        handled.emplace(field);
        if (overrideValue && field == overrideField) return overrideValue;
        if (! sparse(params) && ! params->map()) return std::nullopt;
        const auto* value = child(params, field);
        if (value == nullptr) return observedDefault(type, field);
        const auto numeric = finiteNumber(value);
        if (! numeric) warn(where, std::string(field) + " is not a finite numeric value; preserved without mapping");
        return numeric;
    }
    void assign(E::Module& module, std::string_view id, double plain, const std::string& where)
    {
        const auto descriptors = E::parameterDescriptors(module.type);
        const int index = E::parameterIndex(module.type, id);
        if (index < 0 || static_cast<size_t>(index) >= descriptors.size())
        {
            warn(where, "native " + std::string(id) + " control is unavailable; exact value is preserved in the source");
            return;
        }
        const auto& descriptor = descriptors[static_cast<size_t>(index)];
        if (! std::isfinite(plain)) { warn(where, std::string(id) + " conversion is nonfinite; skipped"); return; }
        const double bounded = std::clamp(plain, static_cast<double>(descriptor.minimum), static_cast<double>(descriptor.maximum));
        if (std::abs(plain - bounded) > 1e-9) warn(where, std::string(id) + " exceeds the native range; bounded native value is an approximation");
        module.values[static_cast<size_t>(index)] = static_cast<float>(bounded);
    }
    void controls(E::Module& module, const V* group, const std::string& where)
    {
        const auto descriptors = E::parameterDescriptors(module.type);
        for (const auto& descriptor : descriptors) module.values.push_back(descriptor.initial);
        const auto* params = child(group, "plainParams");
        if (! sparse(params) && ! params->map()) warn(where, "plainParams is neither a map nor the measured default token; native defaults retained");
        std::set<std::string, std::less<>> handled;
        const auto get = [&](std::string_view field) { return parameter(params, module.type, field, where, handled); };
        if (const auto enabled = get("kParamEnable"))
        {
            if (std::abs(*enabled) < 1e-9) module.enabled = false;
            else if (std::abs(*enabled - 1) < 1e-9) module.enabled = true;
            else warn(where, "kParamEnable is outside measured 0/1 values; native enable retained");
        }
        else warn(where, "Enable sparse default is unverified for this type; native enabled default retained");

        const bool measuredMix = module.type == E::Type::Distortion || module.type == E::Type::Delay
                                 || module.type == E::Type::Filter || module.type == E::Type::Utility || module.type == E::Type::Compressor || module.type == E::Type::Phaser || module.type == E::Type::HyperDimension || module.type == E::Type::Reverb;
        if (measuredMix)
        {
            if (const auto wet = get("kParamWet")) assign(module, "Mix", *wet / 100, where);
            else warn(where, "Wet sparse default is unverified; native full-wet default retained");
        }
        const bool measuredOutput = module.type == E::Type::Eq || module.type == E::Type::Utility || module.type == E::Type::Compressor
                                    || module.type == E::Type::Distortion || module.type == E::Type::Delay || module.type == E::Type::Phaser || module.type == E::Type::HyperDimension || module.type == E::Type::Reverb;
        if (measuredOutput)
        {
            if (const auto output = get("kParamLevelOut"))
            {
                if (*output < 0 || *output > 1) warn(where, "LevelOut is outside measured 0..1; gain bounded to measured endpoints");
                const double u = std::clamp(*output, 0.0, 1.0);
                assign(module, "Output", 0, where);
                assign(module, "OutputGain", 4 * u * u, where);
            }
        }
        if (module.type == E::Type::Utility)
        {
            if (const auto width = get("kParamWidth")) assign(module, "Width", *width / 100, where);
            if (const auto balance = get("kParamBalance")) assign(module, "Pan", std::clamp(*balance, -100.0, 100.0) / 100, where);
            // Measured: HPF = Linkwitz-Riley 4th order (0 = off, up to 400 Hz), LPF = Butterworth 2nd order (50 Hz .. 20 kHz), Low Mono = LR4 high-pass on the side signal.
            const double highPass = get("kParamHPF").value_or(0.0), lowPass = get("kParamLPF").value_or(20000.0);
            assign(module, "DcBlock", 1, where);
            assign(module, "HighPass", std::clamp(highPass, 0.0, 400.0), where);
            assign(module, "LowPass", std::clamp(lowPass, 50.0, 20000.0), where);
            if (const auto mono = get("kParamLFMono")) assign(module, "LowMono", *mono >= 0.5 ? 1 : 0, where);
            assign(module, "LowMonoFreq", std::clamp(get("kParamLFXover").value_or(90.0), 20.0, 400.0), where);
            if (const auto polarity = get("kParamPolarityL")) assign(module, "InvertLeft", *polarity >= 0.5 ? 1 : 0, where);
            if (const auto polarity = get("kParamPolarityR")) assign(module, "InvertRight", *polarity >= 0.5 ? 1 : 0, where);
        }
        if (module.type == E::Type::Eq)
        {
            // Measured: per band RBJ biquads at the host rate; band 1 is low shelf / bell / high pass,
            // band 2 is high shelf / bell / low pass. The type control is a stepped 3-way selector over 0..2 (thirds).
            for (int band = 1; band <= 2; ++band)
            {
                const auto suffix = std::to_string(band), prefix = "Band" + suffix;
                const auto frequency = get("kParamFreq" + suffix), gain = get("kParamGain" + suffix);
                const auto shape = get("kParamType" + suffix), reso = get("kParamReso" + suffix);
                if (frequency) assign(module, prefix + "Freq", std::clamp(*frequency, 20.0, 20000.0), where);
                if (gain) assign(module, prefix + "Gain", std::clamp(*gain, -24.0, 24.0), where);
                const int step = shape ? std::clamp(static_cast<int>(std::floor(*shape * 1.5 + 1e-9)), 0, 2) : 0;
                assign(module, prefix + "Type", step == 0 ? (band == 1 ? 0 : 2) : step == 1 ? 1 : (band == 1 ? 3 : 4), where);
                if (reso)
                {
                    const double u = std::clamp(*reso, 0.0, 100.0) / 100;
                    const double q = step == 0 ? 0.4 + 1.7416 * u * u * u : 0.1 + 18.889 * u * u * u * u;
                    assign(module, prefix + "Q", q, where);
                }
            }
        }
        if (module.type == E::Type::Distortion)
        {
            // Measured: 2x oversampled shaper y = 0.25 F(2x), optional LP/BP/HP morph filter before or after it.
            static constexpr std::array<std::string_view, 18> names{"kTube", "kSoftClip", "kHardClip", "kDiode1", "kDiode2", "kLinFold", "kSinFold", "kZeroSquare",
                                                                    "kDownsample", "kAsym", "kRectify", "kXShaper", "kXShaperAsym", "kSineShaper", "kStompBox",
                                                                    "kTapeSat", "kOverdrive", "kSoftSat"};
            handled.emplace("kParamMode");
            const auto* modeValue = child(params, "kParamMode");
            const std::string modeName = modeValue && modeValue->text() ? *modeValue->text() : std::string("kTube");
            int mode = -1;
            for (size_t i = 0; i < names.size(); ++i) if (names[i] == modeName) mode = static_cast<int>(i);
            const bool wellFormed = sparse(params) || params->map();
            const auto drive = get("kParamDrive"), freq = get("kParamFreq"), width = get("kParamBW"), morph = get("kParamLPHP"), placement = get("kParamPrePost");
            const auto stages = get("kParamNumStages"); // Overdrive: number of cascaded stages (1..16, default 1)
            if (wellFormed && mode >= 0 && devices::serum_distortion::isSupported(mode))
            {
                assign(module, "Style", 1, where);
                assign(module, "Mode", mode, where);
                assign(module, "Amount", std::clamp(drive.value_or(24.7), 0.0, 100.0), where);
                assign(module, "Filter", std::clamp(std::round(placement.value_or(0.0)), 0.0, 2.0), where);
                const bool migrate = legacyDistortionFreq && overrideField != "kParamFreq";
                assign(module, "FilterFreq", std::clamp(freq.value_or(0.5) * (migrate && freq ? 160.0 / 171 : 1.0), 0.0, 1.0), where);
                assign(module, "FilterWidth", std::clamp(width.value_or(2.0), 0.0, 10.0), where);
                assign(module, "FilterMorph", std::clamp(morph.value_or(0.0), 0.0, 100.0), where);
                assign(module, "Stages", std::clamp(std::round(stages.value_or(1.0)), 1.0, 16.0), where);
            }
            else if (wellFormed)
                warn(where, "distortion mode " + modeName + " has no measured native shaper yet; the native Soft shaper is retained");
        }
        if (module.type == E::Type::Compressor)
        {
            // Measured: single linked band, instantaneous peak level, hard knee, reduction smoothed in dB.
            assign(module, "Style", 1, where);
            const double thresh = std::clamp(get("kParamThresh").value_or(0.5), 0.0, 1.0);
            assign(module, "Threshold", std::max(60 * std::log10(std::max(1 - thresh, 1e-12)), -113.5), where);
            const double ratio = get("kParamRatio").value_or(4.0);
            assign(module, "Ratio", std::clamp(ratio, 1.0, 100.0), where);
            if (ratio >= 53.25)
                warn(where, "ratio >= 53 selects the reference's lookahead brickwall limiter (47 + 0.24 * attack-ms samples of latency); "
                            "native uses the same release and ceiling without lookahead");
            assign(module, "Attack", std::clamp(get("kParamAttack").value_or(90.091) / 2000, 0.0001, 1.0), where); // half the displayed ms
            assign(module, "Release", std::clamp(get("kParamRelease").value_or(89.929) / 1000, 0.001, 5.0), where);
            const double makeup = std::clamp(get("kParamMakeup").value_or(0.0), 1.0, 31.0);
            assign(module, "Makeup", 20 * std::log10(std::min(32.0, makeup * (1 + 0.0335 * (1 - std::pow(makeup, -1.15))))), where);
            for (const auto field : {"kParamRatioBelow", "kParamGain0", "kParamGain2", "kParamCompensatedWetDry"})
                get(field); // measured inert on the single-band path (no effect on level, mix or spectrum); preserved in the source state
        }
        if (module.type == E::Type::Filter)
        {
            if (const auto frequency = get("kParamFreq"))
            {
                if (*frequency < 0 || *frequency > 1) warn(where, "filter normalized cutoff outside measured 0..1; bounded");
                const double u = std::clamp(*frequency, 0.0, 1.0);
                // Verified constructor uses 440 Hz reference/44100 Hz, even during 48k renders.
                const double hz = std::min(22050.0, 440 * std::exp2(11.4 * u - 5.75));
                assign(module, "Cutoff", std::min(hz, sampleRate * 0.45), where);
            }
            handled.emplace("kParamType");
            // The FX filter is the same ladder family as the voice filters (measured). A missing type is MG Low 6 here.
            const auto* shape = child(params, "kParamType");
            const std::string type = shape && shape->text() ? *shape->text() : std::string("MgL6");
            const int ladderTap = type == "MgL6" ? 1 : type == "MgL12" ? 2 : type == "MgL18" ? 3 : type == "MgL24" ? 4 : 0;
            const bool wellFormed = sparse(params) || params->map(); // never infer defaults from malformed fields
            if (! wellFormed) {}
            else if (ladderTap > 0)
            {
                assign(module, "Mode", 3 + ladderTap, where);
                const auto resonance = get("kParamReso"), drive = get("kParamDrive");
                assign(module, "Resonance", std::clamp(resonance.value_or(0.0), 0.0, 100.0) / 100, where);
                assign(module, "Drive", 20 * std::log10(dsp::ladderDriveGain(static_cast<float>(std::clamp(drive.value_or(0.0), 0.0, 100.0) / 100))), where);
            }
            else if (wellFormed)
            {
                warn(where, "filter type " + type + " is not implemented yet; own low-pass design retained");
                for (const auto field : {"kParamReso", "kParamDrive"})
                    if (const auto value = get(field))
                    {
                        if (std::abs(*value) < 1e-9) assign(module, std::string_view(field) == "kParamReso" ? "Resonance" : "Drive", 0, where);
                        else warn(where, std::string(field) + " transfer curve is unverified; native value retained");
                    }
            }
        }
        if (module.type == E::Type::HyperDimension)
        {
            // Hyper (unison chorus) + Dimension: defaults are provisional until measured.
            assign(module, "Style", 1, where);
            assign(module, "Unison", std::clamp(get("kParamUnison").value_or(4.0), 0.0, 7.0), where);
            assign(module, "HyperRate", std::clamp(get("kParamRate").value_or(40.0), 0.0, 100.0) / 100, where);
            assign(module, "HyperDetune", std::clamp(get("kParamDetune").value_or(25.0), 0.0, 100.0) / 100, where);
            assign(module, "DimeSize", std::clamp(get("kParamDimESize").value_or(50.0), 0.0, 100.0) / 100, where);
            assign(module, "DimeWet", std::clamp(get("kParamDimEWet").value_or(0.0), 0.0, 100.0) / 100, where);
            assign(module, "DimeLevel", std::clamp(get("kParamDimELevelOut").value_or(0.5), 0.0, 1.0), where);
            get("kParamRetrig");
            if (const auto* lfo = child(group, "lfo")) if (const auto* rows = lfo->array(); rows && rows->size() >= 2)
                for (size_t row = 0; row < 2; ++row)
                    if (const auto* values = (*rows)[row].array())
                        for (size_t j = 0; j < 8 && j < values->size(); ++j)
                            if (const auto value = finiteNumber(&(*values)[j]))
                                assign(module, std::string(row == 0 ? "PhaseL" : "PhaseR") + std::to_string(j), std::clamp(*value, 0.0, 1.0), where);
        }
        if (module.type == E::Type::Reverb)
        {
            // Measured: a sparse type is kPlate. kHall and kPlate have native engines; the other types keep the generic
            // reverb. Shared plain controls: Size, Delay (Hall decay: plain = 250 x^2), Freq, FreqB, Feedback, Width (all percent), PreDelay (seconds).
            handled.emplace("kParamType");
            const auto* typeValue = child(params, "kParamType");
            const std::string typeName = typeValue && typeValue->text() ? *typeValue->text() : std::string("kPlate");
            const bool wellFormed = sparse(params) || params->map();
            const auto size = get("kParamSize"), decay = get("kParamDelay"), low = get("kParamFreq"), high = get("kParamFreqB");
            const auto rate = get("kParamFeedback"), depth = get("kParamWidth"), preDelay = get("kParamPreDelay"), sync = get("kParamPreDelayBeatSync");
            if (wellFormed && (typeName == "kHall" || typeName == "kPlate"))
            {
                assign(module, "Style", typeName == "kHall" ? 1 : 2, where);
                assign(module, "RvSize", std::clamp(size.value_or(35.0), 0.0, 100.0) / 100, where);
                assign(module, "RvDelay", std::sqrt(std::clamp(decay.value_or(30.625), 0.0, 250.0) / 250.0), where);
                assign(module, "RvFreq", std::clamp(low.value_or(0.0), 0.0, 100.0) / 100, where);
                assign(module, "RvFreqB", std::clamp(high.value_or(35.0), 0.0, 100.0) / 100, where);
                assign(module, "RvFeedback", std::clamp(rate.value_or(25.0), 0.0, 100.0) / 100, where);
                assign(module, "RvWidth", std::clamp(depth.value_or(20.0), 0.0, 100.0) / 100, where);
                assign(module, "Predelay", std::clamp(preDelay.value_or(0.0), 0.0, 2.5), where);
                if (sync && *sync >= 0.5) warn(where, "reverb pre-delay tempo sync is not decoded yet; the free-running seconds value is used");
            }
            else if (wellFormed)
                warn(where, "reverb type " + typeName + " has no measured native engine yet; the generic native reverb is retained");
            get("kParamFreqC"); get("kParamMode"); get("kParamVintageScale"); get("kParamVintageScaleB"); // other types' controls (preserved in the source)
        }
        if (module.type == E::Type::Phaser)
        {
            // Measured: 2*Poles first-order all-passes with spread poles, triangle-LFO coefficient modulation, mix (1-q^2) dry + q wet.
            assign(module, "Style", 1, where);
            assign(module, "Freq", std::clamp(get("kParamFreq").value_or(600.0), 20.0, 18000.0), where);
            assign(module, "Mod", std::clamp(get("kParamDepth").value_or(50.0), 0.0, 100.0) / 100, where);
            assign(module, "Spread", std::clamp(get("kParamDepth2").value_or(0.5), 0.0, 1.0), where);
            assign(module, "Feedback", std::clamp(get("kParamFeedback").value_or(80.0), 0.0, 100.0) / 100, where);
            assign(module, "Width", std::clamp(get("kParamWidth").value_or(180.0), 0.0, 360.0) / 360, where); // degrees of right-channel LFO offset
            assign(module, "Poles", std::clamp(get("kParamNumPoles").value_or(4.0), 1.0, 18.0), where);
            const auto rate = get("kParamRate"), sync = get("kParamBeatSync");
            assign(module, "Rate", std::clamp(rate.value_or(0.078125), 0.0, 20.0), where); // Rate = 20 u^4 Hz, default u = 0.25
            if (sync && *sync >= 0.5) warn(where, "phaser tempo-sync rate grid is not decoded yet; the free-running Rate value is used");
            if (const auto* phasor = child(group, "lfophasor")) if (const auto value = finiteNumber(phasor)) assign(module, "LfoPhase", std::clamp(*value, 0.0, 1.0), where);
        }
        if (module.type == E::Type::Delay)
        {
            // Measured: band-pass loop filter (Freq/BW, designed at 44.1 kHz), delay = offset * (time - 50 samples),
            // modes normal / ping-pong / mono-serial. BeatSync defaults to on; the stored time then picks a power-of-two note length.
            const auto sync = get("kParamBeatSync");
            const bool synced = ! sync || std::abs(*sync) >= 1e-9; // absent = synced by default
            const auto feedback = get("kParamFeedback"), freq = get("kParamFreq"), width = get("kParamBW"), mode = get("kParamMode");
            const auto left = get("kParamTimeL"), right = get("kParamTimeR"), offsetLeft = get("kParamOffsetL"), offsetRight = get("kParamOffsetR"), link = get("kParamLink");
            get("kParamHQ");
            {
                assign(module, "Style", 1, where);
                assign(module, "Sync", synced ? 1 : 0, where); // synced: the stored time selects a note length (power-of-two beats at the host tempo)
                assign(module, "Feedback", std::clamp(feedback.value_or(41.9), 0.0, 100.0) / 100, where);
                assign(module, "FilterFreq", std::clamp(freq.value_or(849.0), 20.0, 20000.0), where);
                assign(module, "FilterWidth", std::clamp(width.value_or(6.75), 0.75, 9.5), where);
                assign(module, "Time", left.value_or(0.0773125), where);
                assign(module, "RightTime", right.value_or(0.0773125), where);
                assign(module, "OffsetLeft", std::clamp(offsetLeft.value_or(1.0), 0.5, 1.5), where);
                assign(module, "OffsetRight", std::clamp(offsetRight.value_or(1.0), 0.5, 1.5), where);
                if (link) assign(module, "Link", *link >= 0.5 ? 1 : 0, where);
                const double m = mode.value_or(0.0);
                if (std::abs(m - std::round(m)) > 1e-9 || m < 0 || m > 2) warn(where, "delay mode outside the measured 0/1/2 values; native mode 0 retained");
                else assign(module, "Mode", m, where);
            }
        }
        if (module.type == E::Type::Split3)
            for (const auto field : {"kParamModuleCount1", "kParamModuleCount2", "kParamModuleCount3"}) handled.emplace(field);
        if (params && params->map())
            for (const auto& [field, value] : *params->map())
            {
                (void) value;
                if (! handled.contains(field)) warn(where, field + " scalar/enum/DSP mapping is unsupported; retained in original source");
            }
        if (module.type != E::Type::Utility && module.type != E::Type::Eq && module.type != E::Type::Filter && module.type != E::Type::Delay)
            warn(where, "unmeasured native defaults/controls remain; type presence does not establish reference behavior");
        warn(where, std::string(E::typeName(module.type)) + " audio algorithm remains independently authored; reference equivalence is not established");
    }

    juce::File resolveImpulse(const std::string& reference) const
    {
        const auto path = juce::String(reference).replaceCharacter('\\', '/');
        if (juce::File::isAbsolutePath(path))
        {
            const juce::File absolute(path);
            if (absolute.existsAsFile()) return absolute;
            return {};
        }
        juce::Array<juce::File> roots;
        if (! sourceDirectory.empty())
        {
            const auto origin = juce::String(sourceDirectory);
            const auto root = juce::File::isAbsolutePath(origin) ? juce::File(origin) : juce::File::getCurrentWorkingDirectory().getChildFile(origin);
            roots.add(root); roots.add(root.getChildFile("Impulses"));
        }
        roots.add(juce::File("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Impulses"));
        roots.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Audio/Presets/Xfer Records/Serum 2 Presets/Impulses"));
        roots.add(juce::File("/Library/Audio/Presets/Xfer Records/Serum Presets/Impulses"));
        for (const auto& root : roots)
        {
            const auto candidate = root.getChildFile(path);
            if (candidate.existsAsFile()) return candidate;
        }
        return {};
    }
    void impulse(E::Module& module, const V* group, const std::string& where)
    {
        const auto* reference = child(group, "relativePathToIR");
        if (! reference || ! reference->text() || reference->text()->empty() || reference->text()->size() > 4096
            || reference->text()->find('\0') != std::string::npos)
        {
            warn(where, "IR reference missing/malformed; native convolution bypassed"); module.enabled = false; return;
        }
        const auto file = resolveImpulse(*reference->text());
        if (! file.existsAsFile()) { warn(where, "IR file unavailable: " + *reference->text() + "; native convolution bypassed"); module.enabled = false; return; }
        const auto key = file.getFullPathName().toStdString();
        auto found = impulses.find(key);
        std::shared_ptr<const devices::ConvolutionIR> data;
        if (found != impulses.end()) data = found->second;
        else
        {
            const auto loaded = devices::ConvolutionIR::fromFile(file);
            if (! loaded) { warn(where, loaded.error + "; native convolution bypassed"); module.enabled = false; return; }
            const size_t bytes = loaded.data->originalSamples().size() * sizeof(float);
            if (bytes > model::NativeContent::maxBytes - impulseBytes)
            {
                warn(where, "IR PCM exceeds the 64 MiB import budget; native convolution bypassed"); module.enabled = false; return;
            }
            impulseBytes += bytes; data = loaded.data; impulses.emplace(key, data);
        }
        for (const auto field : {"numChannels", "numFrames", "sampleRate"})
        {
            const auto* declared = child(group, field);
            if (! declared) continue;
            const auto value = finiteNumber(declared);
            const double actual = std::string_view(field) == "numChannels" ? data->channels()
                                  : std::string_view(field) == "numFrames" ? data->frames() : data->sampleRate();
            if (! value || std::abs(*value - actual) > 1e-6)
            {
                warn(where, std::string(field) + " IR metadata mismatch; native convolution bypassed"); module.enabled = false; return;
            }
        }
        module.impulse = std::move(data);
        warn(where, "IR PCM loaded unchanged; trim/tone/decay/minimum-phase and vendor resampling are not reconstructed");
    }

    std::optional<E::Module> entry(const V::Array& flat, size_t& cursor, size_t end, int rack, int depth, bool retain)
    {
        const size_t index = cursor++;
        const auto where = location(rack, index);
        const auto& value = flat[index];
        const auto rawType = finiteNumber(child(&value, "type"));
        if (! rawType || *rawType < 0 || *rawType > 14 || std::floor(*rawType) < *rawType)
        { warn(where, "invalid/unknown type; entry skipped"); return std::nullopt; }
        const int code = static_cast<int>(*rawType);
        const auto type = E::typeFromCode(code);
        if (! type) { warn(where, "unsupported type13; entry retained in source and skipped natively"); return std::nullopt; }
        const auto key = moduleKeys[static_cast<size_t>(code)];
        const auto* group = child(&value, key);
        if (! group || ! group->map()) { warn(where, "type/module key mismatch or malformed " + std::string(key) + "; entry skipped"); return std::nullopt; }
        std::array<size_t, 3> counts{};
        if (*type == E::Type::Split3)
        {
            const auto* params = child(group, "plainParams");
            size_t total = 0;
            for (size_t band = 0; band < 3; ++band)
            {
                const auto count = finiteNumber(child(params, "kParamModuleCount" + std::to_string(band + 1)));
                if (! count || *count < 0 || *count > E::maxModules || std::floor(*count) < *count)
                { warn(where, "Split3 count missing/invalid; splitter skipped without guessing child membership"); return std::nullopt; }
                counts[band] = static_cast<size_t>(*count); total += counts[band];
            }
            if (total > end - cursor)
            { warn(where, "Split3 counts exceed containing flat span; splitter skipped without consuming ordered remainder"); return std::nullopt; }
            if (depth > 0) warn(where, "nested Split3 count semantics beyond the observed shallow graph are unverified; guarded flat-span interpretation used");
        }
        if (! retain || depth > E::maxDepth || moduleCount >= E::maxModules)
        {
            if (retain) warn(where, "native graph count/depth limit reached; subtree skipped");
            for (size_t count : counts) cursor += count;
            return std::nullopt;
        }
        E::Module module;
        module.id = static_cast<E::ModuleId>(rack * 256 + static_cast<int>(index) + 1);
        module.type = *type; ++moduleCount;
        controls(module, group, where);
        if (*type == E::Type::Convolution) impulse(module, group, where);
        const int vendorAddress = 100 * rack + static_cast<int>(index);
        result.addresses.push_back({rack, static_cast<int>(index), vendorAddress, module.id, *type});
        if (index >= 100) warn(where, "flat index>=100 is outside the verified address domain; modulation address mapping withheld");
        if (value.map())
            for (const auto& [field, ignored] : *value.map())
            {
                (void) ignored;
                if (field != "type" && field != key) warn(where, "entry field " + field + " is preserved without native interpretation");
            }
        for (const auto& [field, ignored] : *group->map())
        {
            (void) ignored;
            const bool irMetadata = *type == E::Type::Convolution
                                    && (field == "relativePathToIR" || field == "numChannels" || field == "numFrames" || field == "sampleRate");
            if (field != "plainParams" && ! irMetadata) warn(where, "module field " + field + " is preserved without native interpretation");
        }
        if (*type == E::Type::Split3)
            for (size_t band = 0; band < counts.size(); ++band)
            {
                const size_t bandEnd = cursor + counts[band];
                while (cursor < bandEnd)
                {
                    const bool keep = module.bands[band].size() < E::maxBandModules;
                    if (! keep) warn(where, "native band limit8 reached; remaining band entries skipped");
                    auto nested = entry(flat, cursor, bandEnd, rack, depth + 1, keep);
                    if (nested) module.bands[band].push_back(std::move(*nested));
                }
            }
        return module;
    }
    void run(const V& root)
    {
        legacyDistortionFreq = legacyPresetFormat(root);
        result.configuration.mixLaw = E::MixLaw::Shaped;
        result.configuration.returnBeforeMain = {true, true};
        const auto* global = child(root.find("Global0"), "plainParams");
        const auto* direct = child(global, "kParamDirectVol");
        const auto directVolume = direct ? finiteNumber(direct) : std::optional<double>(.5);
        if (directVolume && *directVolume >= 0 && *directVolume <= 2)
            result.configuration.routing.directGain = static_cast<float>(2 * *directVolume);
        else warn("Global0", "kParamDirectVol is outside its measured stored range; unity Direct gain retained");
        for (int bus = 0; bus < 2; ++bus)
        {
            const auto destinationName = "kParamFXBus" + std::to_string(bus + 1) + "Dest";
            const auto volumeName = "kParamFXBus" + std::to_string(bus + 1) + "Vol";
            const auto destination = child(global, destinationName), volume = child(global, volumeName);
            const auto d = destination ? finiteNumber(destination) : std::optional<double>(0);
            const auto v = volume ? finiteNumber(volume) : std::optional<double>(.5);
            if (d && *d >= 0 && *d <= 1 && std::floor(*d) >= *d)
                result.configuration.returnBeforeMain[static_cast<size_t>(bus)] = *d < .5;
            else warn("Global0", destinationName + " has an uncalibrated destination; default Main return retained");
            if (v && *v >= 0 && *v <= 2)
            {
                if (bus == 0) result.configuration.routing.bus1ToMain = static_cast<float>(2 * *v);
                else result.configuration.routing.bus2ToMain = static_cast<float>(2 * *v);
            }
            else warn("Global0", volumeName + " is outside its measured stored range; unity return retained");
        }
        for (int rack = 0; rack < E::busCount; ++rack)
        {
            const auto key = "FXRack" + std::to_string(rack);
            const auto* group = root.find(key);
            if (! group) continue;
            result.hasRacks = true;
            const auto* effects = child(group, "FX");
            if (! effects || ! effects->array()) { warn(key, "FX list missing/malformed; rack skipped"); continue; }
            const auto* params = child(group, "plainParams");
            if (! sparse(params) && (! params->map() || ! params->map()->empty()))
                warn(key, "rack scalar/routing controls are uncalibrated; native routing defaults retained");
            const auto& flat = *effects->array();
            const size_t end = std::min(flat.size(), static_cast<size_t>(E::maxModules));
            if (end < flat.size()) warn(key, "flat list exceeds128 inspected entries; remainder preserved only in source");
            size_t cursor = 0;
            auto& bus = result.configuration.buses[static_cast<size_t>(rack)];
            while (cursor < end)
            {
                const bool keep = bus.size() < E::maxBusModules;
                if (! keep) warn(key, "native root limit16 reached; remaining roots skipped");
                auto module = entry(flat, cursor, end, rack, 0, keep);
                if (module) bus.push_back(std::move(*module));
            }
        }
        if (suppressedWarnings > 0) result.warnings.push_back("Serum FX: " + std::to_string(suppressedWarnings) + " additional unsupported-field warnings suppressed by bounded diagnostic limit");
    }
};
} // namespace

std::optional<devices::WavetableEffects::ModuleId> SerumEffectsImportResult::nativeModuleId(int vendorAddress) const
{
    std::optional<devices::WavetableEffects::ModuleId> result;
    for (const auto& address : addresses)
        if (address.vendorAddress == vendorAddress)
        {
            if (address.flatIndex >= 100 || result) return std::nullopt;
            result = address.nativeId;
        }
    return result;
}

bool serumEffectFieldModulatable(std::string_view moduleKey, std::string_view field)
{
    return fieldLaw(moduleKey, field).has_value();
}

std::optional<SerumEffectModulation> serumEffectModulation(const SerumValue& root, int vendorAddress, std::string_view moduleKey,
                                                           std::string_view field, int points, double sampleRate)
{
    const auto law = fieldLaw(moduleKey, field);
    const int rack = vendorAddress / 100, index = vendorAddress % 100;
    if (! law || vendorAddress < 0 || rack >= E::busCount || points < 2 || points > 4096 || ! root.map()) return std::nullopt;
    const auto* flat = child(root.find("FXRack" + std::to_string(rack)), "FX");
    if (! flat || ! flat->array() || static_cast<size_t>(index) >= flat->array()->size()) return std::nullopt;
    const auto& value = (*flat->array())[static_cast<size_t>(index)];
    const auto rawType = finiteNumber(child(&value, "type"));
    if (! rawType || *rawType < 0 || *rawType > 14 || std::floor(*rawType) < *rawType) return std::nullopt;
    const int code = static_cast<int>(*rawType);
    const auto type = E::typeFromCode(code);
    const auto* group = child(&value, moduleKey);
    if (! type || moduleKeys[static_cast<size_t>(code)] != moduleKey || ! group || ! group->map()) return std::nullopt;
    try
    {
        Parser parser;
        parser.sampleRate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 44100;
        std::set<std::string, std::less<>> handled;
        auto stored = parser.parameter(child(group, "plainParams"), *type, field, "modulation", handled);
        if (! stored) return std::nullopt;
        if (moduleKey == "FXDistortion" && field == "kParamFreq" && legacyPresetFormat(root)) *stored *= 160.0 / 171;
        SerumEffectModulation result;
        result.module = static_cast<E::ModuleId>(rack * 256 + index + 1);
        result.base = static_cast<float>(law->normalized(*stored));
        std::vector<std::vector<float>> samples;
        parser.overrideField = field;
        for (int point = 0; point < points; ++point)
        {
            parser.overrideValue = law->plain(static_cast<double>(point) / (points - 1));
            E::Module module;
            module.type = *type;
            parser.controls(module, group, "modulation");
            samples.push_back(std::move(module.values));
        }
        const size_t count = samples.front().size();
        for (size_t parameter = 0; parameter < count; ++parameter)
        {
            SerumEffectModulation::Curve curve{static_cast<int>(parameter), {}};
            float low = samples.front()[parameter], high = low;
            for (const auto& row : samples)
            {
                const float v = parameter < row.size() ? row[parameter] : samples.front()[parameter];
                curve.values.push_back(v); low = std::min(low, v); high = std::max(high, v);
            }
            if (high - low > 1e-7f * std::max(1.0f, std::abs(high))) result.curves.push_back(std::move(curve));
        }
        if (result.curves.empty()) return std::nullopt;
        return result;
    }
    catch (const std::bad_alloc&) { return std::nullopt; }
    catch (const std::length_error&) { return std::nullopt; }
}

SerumEffectsImportResult parseSerumEffects(const SerumValue& root, const std::string& sourceDirectory, double sampleRate)
{
    Parser parser;
    parser.sourceDirectory = sourceDirectory;
    parser.sampleRate = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 44100;
    if (! root.map()) { parser.warn("root", "document root is not a map"); return std::move(parser.result); }
    try { parser.run(root); }
    catch (const std::bad_alloc&)
    {
        parser.result.configuration = {}; parser.result.configuration.mixLaw = E::MixLaw::Shaped;
        parser.result.addresses.clear(); parser.warn("load", "allocation failed; native graph skipped, original state retained");
    }
    catch (const std::length_error&)
    {
        parser.result.configuration = {}; parser.result.configuration.mixLaw = E::MixLaw::Shaped;
        parser.result.addresses.clear(); parser.warn("load", "allocation length exceeded; native graph skipped, original state retained");
    }
    return std::move(parser.result);
}
} // namespace sonora::plugins
