#pragma once
// Compressor (.als "Compressor2") and Glue Compressor ("GlueCompressor") — ROADMAP DEV-7 / DEV-12.
//
// Both are feed-forward compressors built on one gain computer: linked stereo detector (peak or
// RMS), soft knee, attack/release smoothing of the gain reduction in the dB domain, make-up gain
// and dry/wet. An external sidechain (another track's tapped audio, ALS-12) drives the detector when
// the engine provides one. Not modelled: the sidechain EQ, the Glue Compressor's peak clipper.
// Compressor's laws are measured against the reference (DEV-22b): detector (|L|+|R|)/2 (Peak,
// Expand) or a 24 ms mean of (L^2+R^2)/2 (RMS); knee spans threshold +-Knee; Log envelope smooths the gain
// reduction in dB, Lin smooths the detector level; one-pole attack/release, coef 1 - exp(-1 / (t fs)).
// Compressor's look-ahead delays the audio and reports it as latency (plugin delay compensation).

#include "devices/NativeDevice.h"
#include "dsp/Dsp.h"
#include "engine/DelayLine.h"

#include <limits>

namespace sonora::devices
{

// Static gain computer: returns the output level (dB) for an input level (dB).
inline float compressorCurveDb(float inDb, float thresholdDb, float ratio, float kneeDb)
{
    const float over = inDb - thresholdDb;
    if (kneeDb > 0.0f && 2.0f * std::fabs(over) <= kneeDb)
    {
        const float x = over + kneeDb * 0.5f;
        return inDb + (1.0f / ratio - 1.0f) * x * x / (2.0f * kneeDb);
    }
    return over <= 0.0f ? inDb : thresholdDb + over / ratio;
}

// "Makeup" (GainCompensation), measured against the reference (DEV-22b):
// 0.7 x -threshold x (1 - 1/ratio) dB, independent of the knee (0 dB at a 0 dB threshold).
inline float compressorAutoMakeupDb(float thresholdDb, float ratio)
{
    return -0.7f * thresholdDb * (1.0f - 1.0f / ratio);
}

class Compressor final : public NativeDevice
{
public:
    enum Param
    {
        Threshold, // linear amplitude (0.0316 == -30 dB)
        Ratio,
        Attack,    // ms
        Release,   // ms
        AutoRelease,
        Gain,      // make-up, dB
        GainCompensation, // automatic make-up
        DryWet,
        Model,     // 0 Peak, 1 RMS, 2 Expand (treated as RMS)
        Knee,      // dB
        LookAhead, // 0 off, 1 = 1 ms, 2 = 10 ms
        ExpansionRatio, // Expand model: gain above threshold = (in - threshold) (1 - 1 / ratio)
        LogEnvelope,    // true: envelope in dB (default), false: linear detector envelope
        NumParams
    };

    Compressor();
    std::string name() const override { return "Compressor"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    int latencySamples() const override { return lookaheadSamples_; }
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override { sidechain_ = audio; sidechainGain_ = gain; }
    // Current gain reduction in dB (<= 0), for a meter. Any thread.
    float gainReductionDb() const { return grMeter_.load(std::memory_order_relaxed); }
    // Peak of the last block before / after the device in dBFS (the face's activity view, UI-42). Any thread.
    float inputPeakDb() const { return inMeter_.load(std::memory_order_relaxed); }
    float outputPeakDb() const { return outMeter_.load(std::memory_order_relaxed); }

private:
    double sampleRate_ = 44100.0;
    float grDb_ = 0.0f, rmsState_ = 0.0f, levelEnv_ = 0.0f;
    int lookaheadSamples_ = 0;
    engine::DelayLine delay_;     // look-ahead: audio is delayed, the gain is computed from the undelayed signal
    std::vector<float> gains_;    // per-sample linear gain of the current block
    std::atomic<float> grMeter_{0.0f}, inMeter_{-120.0f}, outMeter_{-120.0f};
    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1.0f;
};

// Glue Compressor static curve, fitted to the reference (DEV-22b): gain reduction
// = slope * w * ln(1 + exp((in - threshold + shift) / w)) per ratio (2:1, 4:1, 10:1). Not the exact law: the
// reference's knee and threshold reference move with the ratio (like a feedback design); fits are within 0.1 dB.
float glueReductionDb(float inDb, float thresholdDb, int ratioIndex);
// Glue Compressor's soft clipper (fitted): linear up to 0.97, saturating towards 1.18.
float glueSoftClip(float x);

class GlueCompressor final : public NativeDevice
{
public:
    enum Param
    {
        Threshold, // dB
        Range,     // dB, maximum reduction
        Makeup,    // dB
        Attack,    // index into 0.01 0.1 0.3 1 3 10 30 ms
        Ratio,     // index into 2 4 10
        Release,   // index into 0.2 0.4 0.6 0.8 1.2 s, Auto
        DryWet,
        PeakClipIn,
        NumParams
    };
    static float attackMs(int idx);
    static float ratioValue(int idx);
    static float releaseMs(int idx); // < 0 == Auto

    GlueCompressor();
    std::string name() const override { return "Glue Compressor"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override;
    void reset() override;
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override { sidechain_ = audio; sidechainGain_ = gain; }
    float gainReductionDb() const { return grMeter_.load(std::memory_order_relaxed); }

private:
    double sampleRate_ = 44100.0;
    float grDb_ = 0.0f;
    std::atomic<float> grMeter_{0.0f};
    const juce::AudioBuffer<float>* sidechain_ = nullptr;
    float sidechainGain_ = 1.0f;
};

} // namespace sonora::devices
