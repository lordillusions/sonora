#pragma once
// Four-stage zero-delay-feedback ladder low-pass with a selectable tap ("MG Low 6/12/18/24" in the Serum 2 import).
//
// Own implementation of behaviour measured on the reference:
//  - the resonance feedback always comes from the fourth stage, k = 4 * resonance, solved instantaneously (no unit delay);
//    the 6/12/18 dB variants only tap stage 1/2/3, the loop is the same;
//  - the input first passes a cubic soft clipper (gain from the Drive control; y = z - z^3 / (3 knee^2), saturating at
//    2/3 knee), and the signal entering the first stage is hard limited at 1.1107 knee; everything else is linear, so small
//    signals see the plain TPT cascade. Without resonance nothing but the input clipper is nonlinear.
//  - the knee is 1.4959 for the Serum 2 FX filter module (plugin level units, saturating at +-1.0) and 4.966 for the voice filters
//    (the voice runs 8.1 dB hotter than the output before the main volume).
// All state and coefficient maths is allocation free and safe on the audio thread.

#include <algorithm>
#include <cmath>

namespace sonora::dsp
{

class LadderFilter
{
public:
    static constexpr float kVoiceKnee = 4.96598f;   // voice filters (engine internal units, before the main volume)
    static constexpr float kEffectKnee = 1.4959f;   // FX filter module (plugin level units)
    static constexpr float kLoopRatio = 1.1107f;    // loop limiter / knee, identical in both paths
    static constexpr float kCutoffTuning = 1.003f;  // measured: voice poles sit 0.3 % above the displayed cutoff (FX: 0.1 %)

    void setKnee(float knee) { knee_ = knee; limit_ = knee * kLoopRatio; }

    // `taps`: 1..4 poles at the output. `resonance` 0..1 (feedback 0..4). Keeps the state, so it can be called per control tick.
    void setup(double sampleRate, float cutoffHz, float resonance, int taps, float tuning = kCutoffTuning)
    {
        const double fc = std::clamp((double) cutoffHz * tuning, 5.0, sampleRate * 0.45);
        const double g = std::tan(3.14159265358979323846 * fc / sampleRate);
        g_ = (float) (g / (1.0 + g));
        k_ = 4.0f * std::clamp(resonance, 0.0f, 1.0f);
        const float g2 = g_ * g_;
        g4_ = g2 * g2;
        taps_ = std::clamp(taps, 1, 4) - 1;
    }
    void reset() { s_[0] = s_[1] = s_[2] = s_[3] = 0.0f; }

    // Soft clipper applied to the (already driven) input.
    static float inputClip(float z, float knee = kVoiceKnee)
    {
        z = std::clamp(z, -knee, knee);
        return z - z * z * z / (3.0f * knee * knee);
    }

    // `drivenInput` is input * drive gain.
    float process(float drivenInput)
    {
        const float x = inputClip(drivenInput, knee_);
        const float one = 1.0f - g_;
        const float s1 = one * s_[0], s2 = one * s_[1], s3 = one * s_[2], s4 = one * s_[3];
        const float sigma = g_ * g_ * g_ * s1 + g_ * g_ * s2 + g_ * s3 + s4;
        float u = (x - k_ * sigma) / (1.0f + k_ * g4_);
        u = std::clamp(u, -limit_, limit_);
        const float y1 = g_ * u + s1;
        const float y2 = g_ * y1 + s2;
        const float y3 = g_ * y2 + s3;
        const float y4 = g_ * y3 + s4;
        s_[0] = 2.0f * y1 - s_[0];
        s_[1] = 2.0f * y2 - s_[1];
        s_[2] = 2.0f * y3 - s_[2];
        s_[3] = 2.0f * y4 - s_[3];
        const float out[4] = {y1, y2, y3, y4};
        return out[taps_];
    }

private:
    float g_ = 0.1f, k_ = 0, g4_ = 0, knee_ = kVoiceKnee, limit_ = kVoiceKnee * kLoopRatio;
    int taps_ = 3;
    float s_[4] = {0, 0, 0, 0};
};

// Vendor drive law: input gain (linear) for a normalized 0..1 drive, measured from the reference (0 -> 1x, 1 -> ~16x = +24 dB).
inline float ladderDriveGain(float drive)
{
    static const float table[13] = {1.0f, 1.2152f, 1.4637f, 2.0731f, 2.8553f, 3.8405f, 5.0610f, 6.5514f, 8.3488f, 10.4926f, 13.0242f, 15.9874f, 15.9874f};
    static const float position[13] = {0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 2.0f};
    const float d = std::clamp(drive, 0.0f, 1.0f);
    for (int i = 1; i < 13; ++i)
        if (d <= position[i])
        {
            const float t = (d - position[i - 1]) / (position[i] - position[i - 1]);
            return table[i - 1] + (table[i] - table[i - 1]) * t;
        }
    return table[11];
}
// Serum 2 "Ladder EMS" (measured black-box, DEV-18i): four one-pole stages at 4.786 / 2.811 / 1.014 / 0.0733 x (0.762 x cutoff)
// with feedback k = 33.7 r around all four, output x 1.121 (1 + k) (DC gain constant). Zero-delay feedback, TPT stages.
// Measured linear: the reference keeps the output proportional to the input up to 24 dB of drive and output peaks of 2.6 at
// resonance 0.47 (no input or loop saturation). The loop is stable up to r = 1 (critical k = 34.06); kSafetyLimit only
// bounds a runaway under fast modulation and sits far above anything the reference was measured at.
class StaggeredLadder
{
public:
    void setup(double sampleRate, float cutoffHz, float resonance)
    {
        static constexpr double ratios[4]{4.78587, 2.81082, 1.01439, 0.0732825};
        for (int i = 0; i < 4; ++i)
        {
            const double fc = std::clamp(static_cast<double>(cutoffHz) * 0.762 * ratios[i], 2.0, sampleRate * 0.45);
            const double g = std::tan(3.14159265358979323846 * fc / sampleRate);
            G_[i] = g / (1 + g);
        }
        k_ = 33.7 * std::clamp(static_cast<double>(resonance), 0.0, 1.0);
        gain_ = 1.121 * (1 + k_);
    }
    void reset() { s_[0] = s_[1] = s_[2] = s_[3] = 0; }
    float process(float x)
    {
        // y_i = G_i (x_i - s_i) + s_i: the cascade output is y4 = A x4 + B; solve u = c x - k y4 for the feedback. The level
        // compensation c = 1.121 (1 + k) sits at the input.
        double a = 1, b = 0;
        for (int i = 0; i < 4; ++i) { b = G_[i] * (b - s_[i]) + s_[i]; a *= G_[i]; }
        const double driven = gain_ * x;
        const double y4 = (a * driven + b) / (1 + k_ * a);
        double input = std::clamp(driven - k_ * y4, -kSafetyLimit, kSafetyLimit);
        for (int i = 0; i < 4; ++i)
        {
            const double v = G_[i] * (input - s_[i]), y = v + s_[i];
            s_[i] = y + v; input = y;
        }
        const float out = static_cast<float>(input);
        return std::isfinite(out) ? out : (reset(), 0.0f);
    }

private:
    static constexpr double kSafetyLimit = 1.0e6;
    double G_[4]{}, s_[4]{}, k_ = 0, gain_ = 1;
};

// Inverse of ladderDriveGain (the table is strictly increasing on 0..1): the drive whose gain is `gain`.
inline float ladderDriveForGain(float gain)
{
    static const float table[12] = {1.0f, 1.2152f, 1.4637f, 2.0731f, 2.8553f, 3.8405f, 5.0610f, 6.5514f, 8.3488f, 10.4926f, 13.0242f, 15.9874f};
    static const float position[12] = {0.0f, 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
    if (! (gain > table[0])) return 0;
    for (int i = 1; i < 12; ++i)
        if (gain <= table[i]) return position[i - 1] + (position[i] - position[i - 1]) * (gain - table[i - 1]) / (table[i] - table[i - 1]);
    return 1;
}

} // namespace sonora::dsp
