#include "devices/native/Compressor.h"

namespace sonora::devices
{

namespace
{
std::vector<ParameterInfo> compressorInfos()
{
    using C = Compressor;
    std::vector<ParameterInfo> p(C::NumParams);
    p[C::Threshold] = makeParam("Threshold", "Threshold", 0.000316f, 1.0f, 0.5f, "lin");
    p[C::Ratio] = makeParam("Ratio", "Ratio", 1.0f, 100.0f, 2.0f);
    p[C::Attack] = makeParam("Attack", "Attack", 0.01f, 1000.0f, 0.1f, "ms");
    p[C::Release] = makeParam("Release", "Release", 1.0f, 3000.0f, 30.0f, "ms");
    p[C::AutoRelease] = makeParam("AutoReleaseControlOnOff", "Auto", 0.0f, 1.0f, 0.0f, "", true);
    p[C::Gain] = makeParam("Gain", "Makeup", -36.0f, 36.0f, 0.0f, "dB");
    p[C::GainCompensation] = makeParam("GainCompensation", "Makeup Auto", 0.0f, 1.0f, 0.0f, "", true);
    p[C::DryWet] = makeParam("DryWet", "Dry/Wet", 0.0f, 1.0f, 1.0f, "%");
    p[C::Model] = makeParam("Model", "Model", 0.0f, 2.0f, 0.0f, "", false, {"Peak", "RMS", "Expand"});
    p[C::Knee] = makeParam("Knee", "Knee", 0.0f, 18.0f, 6.0f, "dB");
    p[C::LookAhead] = makeParam("LookAhead", "Lookahead", 0.0f, 2.0f, 0.0f, "", false, {"0 ms", "1 ms", "10 ms"});
    p[C::ExpansionRatio] = makeParam("ExpansionRatio", "Expansion", 1.0f, 2.0f, 1.15f);
    p[C::LogEnvelope] = makeParam("LogEnvelope", "Log Env", 0.0f, 1.0f, 1.0f, "", true);
    return p;
}
} // namespace

Compressor::Compressor() : NativeDevice(compressorInfos()) {}

void Compressor::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    gains_.assign((size_t) maxBlockSize, 1.0f);
    // The look-ahead setting is read here (latency is static, like in Live).
    const int idx = (int) std::lround(value(LookAhead));
    lookaheadSamples_ = idx == 1 ? (int) (0.001 * sampleRate) : idx == 2 ? (int) (0.010 * sampleRate) : 0;
    delay_.prepare(lookaheadSamples_);
    reset();
}

void Compressor::reset()
{
    grDb_ = rmsState_ = levelEnv_ = 0.0f;
    delay_.reset();
}

void Compressor::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = std::min(audio.getNumSamples(), (int) gains_.size());
    float* l = audio.getWritePointer(0);
    float* r = audio.getWritePointer(1);

    const float thresholdDb = dsp::gainToDb(value(Threshold));
    // The reference stores an infinite ratio as FLT_MAX; the parameter clamps it to its 100:1 maximum, which is "inf".
    const float ratio = value(Ratio) >= 99.99f ? std::numeric_limits<float>::infinity() : std::max(1.0f, value(Ratio));
    const int model = (int) std::lround(value(Model));
    const bool rms = model == 1, expand = model == 2;
    // Expand: upward expansion above the threshold, as a curve with slope 2 - 1/ratio (same knee formula).
    const float curveRatio = expand ? 1.0f / (2.0f - 1.0f / std::max(1.0f, value(ExpansionRatio))) : ratio;
    const float knee = 2.0f * value(Knee); // the reference's knee spans threshold +- Knee
    const float attackCoef = dsp::timeToCoef(value(Attack), sampleRate_);
    // Auto release: measured as program dependent (~45 ms at deep reduction, ~62 ms near 0 dB); 50 ms fits best.
    const float relCoef = dsp::timeToCoef(value(AutoRelease) > 0.5f ? 50.0f : value(Release), sampleRate_);
    const bool logEnvelope = value(LogEnvelope) > 0.5f;
    const float rmsCoef = dsp::timeToCoef(24.0f, sampleRate_);
    const float autoMakeup = value(GainCompensation) > 0.5f && ! expand ? compressorAutoMakeupDb(thresholdDb, ratio) : 0.0f;
    const float makeup = dsp::dbToGain(value(Gain) + autoMakeup);
    const float wet = value(DryWet);

    // 1) gain per sample from the undelayed signal (or the sidechain input when one is routed)
    const bool external = sidechain_ != nullptr && sidechain_->getNumChannels() >= 2 && sidechain_->getNumSamples() >= n;
    const float* dl = external ? sidechain_->getReadPointer(0) : l;
    const float* dr = external ? sidechain_->getReadPointer(1) : r;
    const float dg = external ? sidechainGain_ : 1.0f;
    float minGr = 0.0f, inPeak = 0.0f, outPeak = 0.0f;
    for (int i = 0; i < n; ++i)
        inPeak = std::max(inPeak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    for (int i = 0; i < n; ++i)
    {
        const float sl = dl[i] * dg, sr = dr[i] * dg;
        float level;
        if (rms)
        {
            rmsState_ += (0.5f * (sl * sl + sr * sr) - rmsState_) * rmsCoef;
            level = std::sqrt(std::max(rmsState_, 0.0f));
        }
        else
        {
            level = 0.5f * (std::fabs(sl) + std::fabs(sr));
        }
        if (logEnvelope)
        {
            const float inDb = dsp::gainToDb(level);
            const float targetGr = compressorCurveDb(inDb, thresholdDb, curveRatio, knee) - inDb; // < 0 compresses, > 0 expands
            // Attack while the change grows (more reduction, or more expansion), release towards 0 dB.
            const bool growing = expand ? targetGr > grDb_ : targetGr < grDb_;
            grDb_ += (targetGr - grDb_) * (growing ? attackCoef : relCoef);
        }
        else
        {
            levelEnv_ += (level - levelEnv_) * (level > levelEnv_ ? attackCoef : relCoef);
            const float inDb = dsp::gainToDb(levelEnv_);
            grDb_ = compressorCurveDb(inDb, thresholdDb, curveRatio, knee) - inDb;
        }
        minGr = std::min(minGr, grDb_);
        gains_[(size_t) i] = dsp::dbToGain(grDb_) * makeup;
    }
    grMeter_.store(minGr, std::memory_order_relaxed);

    // 2) delay the audio (look-ahead) and apply the gain; dry/wet blends the delayed dry signal
    delay_.processInPlace(audio, n);
    for (int i = 0; i < n; ++i)
    {
        const float g = 1.0f + (gains_[(size_t) i] - 1.0f) * wet;
        l[i] *= g;
        r[i] *= g;
        outPeak = std::max(outPeak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    }
    inMeter_.store(dsp::gainToDb(inPeak), std::memory_order_relaxed);
    outMeter_.store(dsp::gainToDb(outPeak), std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------- Glue

float glueReductionDb(float inDb, float thresholdDb, int ratioIndex)
{
    struct Fit { float shift, width, slope; };
    static constexpr Fit fits[] = {{3.85f, 7.28f, 0.63f}, {-0.3f, 1.2f, 0.72f}, {-3.61f, 0.15f, 0.897f}};
    const auto& f = fits[std::clamp(ratioIndex, 0, 2)];
    const float x = (inDb - thresholdDb + f.shift) / f.width;
    return -f.slope * f.width * (x > 30.0f ? x : std::log1p(std::exp(x)));
}

float glueSoftClip(float x)
{
    constexpr float knee = 0.97f, ceiling = 1.18f;
    const float a = std::fabs(x);
    if (a <= knee) return x;
    return std::copysign(knee + (ceiling - knee) * std::tanh((a - knee) / (ceiling - knee)), x);
}

float GlueCompressor::attackMs(int idx)
{
    static constexpr float v[] = {0.01f, 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f};
    return v[std::clamp(idx, 0, 6)];
}
float GlueCompressor::ratioValue(int idx)
{
    static constexpr float v[] = {2.0f, 4.0f, 10.0f};
    return v[std::clamp(idx, 0, 2)];
}
float GlueCompressor::releaseMs(int idx)
{
    static constexpr float v[] = {200.0f, 400.0f, 600.0f, 800.0f, 1200.0f, -1.0f};
    return v[std::clamp(idx, 0, 5)];
}

GlueCompressor::GlueCompressor() : NativeDevice([] {
    using G = GlueCompressor;
    std::vector<ParameterInfo> p(G::NumParams);
    p[G::Threshold] = makeParam("Threshold", "Threshold", -40.0f, 0.0f, 0.0f, "dB");
    p[G::Range] = makeParam("Range", "Range", 0.0f, 70.0f, 70.0f, "dB");
    p[G::Makeup] = makeParam("Makeup", "Makeup", 0.0f, 20.0f, 0.0f, "dB");
    p[G::Attack] = makeParam("Attack", "Attack", 0.0f, 6.0f, 0.0f, "", false,
                             {"0.01 ms", "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"});
    p[G::Ratio] = makeParam("Ratio", "Ratio", 0.0f, 2.0f, 0.0f, "", false, {"2:1", "4:1", "10:1"});
    p[G::Release] = makeParam("Release", "Release", 0.0f, 5.0f, 2.0f, "", false, {"0.2 s", "0.4 s", "0.6 s", "0.8 s", "1.2 s", "Auto"});
    p[G::DryWet] = makeParam("DryWet", "Dry/Wet", 0.0f, 1.0f, 1.0f, "%");
    p[G::PeakClipIn] = makeParam("PeakClipIn", "Clip", 0.0f, 1.0f, 0.0f, "", true);
    return p;
}())
{
}

void GlueCompressor::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate;
    reset();
}

void GlueCompressor::reset()
{
    grDb_ = 0.0f;
}

void GlueCompressor::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = audio.getNumSamples();
    float* l = audio.getWritePointer(0);
    float* r = audio.getWritePointer(1);

    const float threshold = value(Threshold);
    const float attackCoef = dsp::timeToCoef(attackMs((int) std::lround(value(Attack))), sampleRate_);
    const float relMs = releaseMs((int) std::lround(value(Release)));
    // Measured: the reference releases ~2.5x faster than the nominal times (fit 0.4 x), attack is nominal.
    const float relCoef = dsp::timeToCoef(0.4f * (relMs < 0.0f ? 300.0f : relMs), sampleRate_);
    const int ratioIndex = (int) std::lround(value(Ratio));
    const bool clip = value(PeakClipIn) > 0.5f;
    const float range = value(Range);
    const float makeup = dsp::dbToGain(value(Makeup));
    const float wet = value(DryWet);

    const bool external = sidechain_ != nullptr && sidechain_->getNumChannels() >= 2 && sidechain_->getNumSamples() >= n;
    const float* dl = external ? sidechain_->getReadPointer(0) : l;
    const float* dr = external ? sidechain_->getReadPointer(1) : r;
    const float dg = external ? sidechainGain_ : 1.0f;
    float minGr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float inL = l[i], inR = r[i];
        const float inDb = dsp::gainToDb(0.5f * (std::fabs(dl[i] * dg) + std::fabs(dr[i] * dg))); // channel mean, as Compressor
        float targetGr = glueReductionDb(inDb, threshold, ratioIndex);
        targetGr = std::max(targetGr, -range);
        grDb_ += (targetGr - grDb_) * (targetGr < grDb_ ? attackCoef : relCoef);
        minGr = std::min(minGr, grDb_);
        const float g = dsp::dbToGain(grDb_) * makeup;
        float outL = inL * g, outR = inR * g;
        if (clip)
        {
            outL = glueSoftClip(outL);
            outR = glueSoftClip(outR);
        }
        l[i] = inL + (outL - inL) * wet;
        r[i] = inR + (outR - inR) * wet;
    }
    grMeter_.store(minGr, std::memory_order_relaxed);
}

} // namespace sonora::devices
