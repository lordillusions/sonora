#pragma once
// Native equivalent of the Serum 2 FX Distortion, built from black-box measurements of the reference.
// Audio-thread safe: no allocation, no locks.
//
// Signal path (48 kHz reference): [pre filter] -> 2x oversampled waveshaper y = 0.25 * F_mode,drive(2 * x) -> [post filter].
// Tube is analytic, F(u) = clip(u) / ((1 - d) + d |clip(u)|) with d = 0.9886 drive / 100. The other measured shapers are tables of F sampled
// on a cubic grid and interpolated across drive.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "devices/native/SerumDistortionTables.h"

namespace sonora::devices::serum_distortion
{

// Mode indices follow the reference enumeration (kTube = 0 ... kSoftSat = 17).
inline constexpr int modeCount = 18;

inline const int16_t* tableFor(int mode) noexcept
{
    switch (mode)
    {
        case 1: return tables::mode1;
        case 2: return tables::mode2;
        case 3: return tables::mode3;
        case 4: return tables::mode4;
        case 7: return tables::mode7;
        case 9: return tables::mode9;
        case 10: return tables::mode10;
        case 14: return tables::mode14;
        case 15: return tables::mode15;
        case 17: return tables::mode17;
        default: return nullptr;
    }
}
// True for the modes with a measured implementation (Tube, the table modes, Downsample - a sample-and-hold - and the Overdrive chain). The folds
// and the X/Sine shapers stay unsupported (the folds oscillate faster than the table grid at drive > 10 %).
inline bool isSupported(int mode) noexcept { return mode == 0 || mode == 8 || mode == 16 || tableFor(mode) != nullptr; }

// F(x) for an input sample x (already scaled so that +-1 is full scale). Returns the reference's output divided by 0.25.
inline float shaper(int mode, float drive01, float x) noexcept
{
    drive01 = std::clamp(drive01, 0.0f, 1.0f);
    if (mode == 0)
    {
        // Measured: the effective knee amount is 0.9886 of the drive fraction (fitted to the harmonics of 1 kHz sines at three levels, 0.1-0.3 %).
        const float amount = 0.9886f * drive01;
        const float u = 2.0f * std::clamp(x, -0.5f, 0.5f);
        return u / ((1.0f - amount) + amount * std::abs(u) + 1e-12f);
    }
    const int16_t* table = tableFor(mode);
    if (table == nullptr) return 2.0f * x;
    const float a = std::clamp(x, -1.0f, 1.0f);
    const float t = std::cbrt(a) * 64.0f + 64.0f; // grid position, 0..128
    const int column = std::min(127, static_cast<int>(t));
    const float cf = t - static_cast<float>(column);
    const float rowPosition = drive01 * 20.0f;
    const int row = std::min(19, static_cast<int>(rowPosition));
    const float rf = rowPosition - static_cast<float>(row);
    const auto at = [&](int r, int c) { return static_cast<float>(table[r * tables::columns + c]); };
    const float low = at(row, column) + (at(row, column + 1) - at(row, column)) * cf;
    const float high = at(row + 1, column) + (at(row + 1, column + 1) - at(row + 1, column)) * cf;
    return (low + (high - low) * rf) * (1.0f / 8192.0f);
}

// 4x oversampling around the shaper: two cascaded polyphase all-pass IIR half-band stages up and the mirror image down (structure as in the
// reference; the four first-order all-pass coefficients were fitted to its measured linear response, 300 Hz - 23.9 kHz, rms 2e-4:
// 0.15841 and 0.73327 form one branch, 0.03880 and 0.37481 the other). One stage handles one channel; the up stage turns one sample into two
// (branch outputs, 0.03880/0.37481 first), the down stage turns two into one (branch outputs averaged, first sample into the 0.15841 branch).
// The whole path has a low-frequency delay of about 6.4 samples and a gentle roll-off (0.72 at 23.9 kHz).
class HalfbandStage
{
public:
    void reset() noexcept { *this = HalfbandStage(); }
    void up(float x, float& first, float& second) noexcept
    {
        const float newA0 = history_[0] + (x - a_[0]) * kCoefficient[0];
        const float newA1 = history_[1] + (x - a_[1]) * kCoefficient[1];
        const float newB0 = delayed_[0] + (a_[0] - b_[0]) * kCoefficient[2];
        const float newB1 = delayed_[1] + (a_[1] - b_[1]) * kCoefficient[3];
        delayed_ = a_;
        history_ = {x, x};
        a_ = {newA0, newA1};
        b_ = {newB0, newB1};
        first = newB1;
        second = newB0;
    }
    float down(float first, float second) noexcept
    {
        const float newB0 = delayed_[0] + (a_[0] - b_[0]) * kCoefficient[2];
        const float newB1 = delayed_[1] + (a_[1] - b_[1]) * kCoefficient[3];
        const float newA0 = history_[0] + (first - a_[0]) * kCoefficient[0];
        const float newA1 = history_[1] + (second - a_[1]) * kCoefficient[1];
        delayed_ = a_;
        history_ = {first, second};
        a_ = {newA0, newA1};
        b_ = {newB0, newB1};
        return 0.5f * (newB0 + newB1);
    }

private:
    static constexpr std::array<float, 4> kCoefficient{0.15841f, 0.03880f, 0.73327f, 0.37481f};
    std::array<float, 2> history_{}, a_{}, delayed_{}, b_{};
};

// One channel's 4x path: x -> up, up -> four samples -> shaper -> down, down.
class Oversampler
{
public:
    void reset() noexcept
    {
        up1_.reset(); up2_.reset(); down1_.reset(); down2_.reset();
    }
    template <class Shaper>
    float process(float x, Shaper&& shaper) noexcept
    {
        float a1, a2, b1, b2, b3, b4;
        up1_.up(x, a1, a2);
        up2_.up(a1, b1, b2);
        up2_.up(a2, b3, b4);
        const float e1 = down1_.down(shaper(b1), shaper(b2));
        const float e2 = down1_.down(shaper(b3), shaper(b4));
        return down2_.down(e1, e2);
    }

private:
    HalfbandStage up1_, up2_, down1_, down2_;
};

// The reference's pre / post filter: a nonlinear Chamberlin state-variable filter run as two sub-steps per sample, with the LP -> BP -> HP morph
// (LPHP 0..100 %: [0, 50] crossfades low-pass and band-pass, [50, 100] band-pass and high-pass) smoothed per sample. Structure as in the
// reference; the laws below were confirmed against hosted renders (log-magnitude error < 1e-3 over the whole band):
//   ratio = 2^(10.667 min(1.06875 Freq, 1.06875) - 5.75) 440 / fs (clamped to 1),  F = 2 sin(pi ratio / 2)  (so fc is about 0.44 fs/2 ... per sub-step),
//   k = min(2 / F - F / 2, 2, 2 - 2 f3^(1/4)),  f3 = clamp(BW / 7.5, 0.01, 0.99),  band-pass is unnormalised.
// The input is clipped at +-1.5 and soft-limited with x - 4/27 x^3; the band state is limited to +-0.8333 per step and loses F/2 b^3.
class MorphFilter
{
public:
    void setup(double sampleRate, float freq01, float bandwidth, float morph01) noexcept
    {
        const double u = std::min(std::max<double>(freq01, 0.0) * 1.06875, 1.06875);
        const double ratio = std::min(std::exp2(u * 10.666666666666666 - 5.75) * 440.0 / sampleRate, 1.0);
        const double f = 2.0 * std::sin(ratio * 1.5707963267948966);
        const double f3 = std::clamp<double>(bandwidth / 7.5, 0.01, 0.99);
        const double damping = std::min({std::max(2.0 / f - f * 0.5, 0.0), 2.0, 2.0 - 2.0 * std::sqrt(std::sqrt(f3))});
        f_ = static_cast<float>(f);
        k_ = static_cast<float>(damping);
        const float m = std::clamp(morph01, 0.0f, 1.0f) * 2.0f;
        target_ = {std::max(1.0f - m, 0.0f), m < 1.0f ? m : 2.0f - m, std::max(m - 1.0f, 0.0f)};
        smoothing_ = static_cast<float>(sampleRate <= 43999.0 ? 1.0 - std::exp(-1.5707964) : 1.0 - std::exp(-(20.0 / sampleRate) * 1.5707964));
    }
    void reset() noexcept
    {
        low_ = band_ = high_ = 0.0f;
        weight_ = {1.0f, 0.0f, 0.0f};
    }
    float process(float input) noexcept
    {
        for (size_t i = 0; i < 3; ++i) weight_[i] += (target_[i] - weight_[i]) * smoothing_;
        float x = std::clamp(input, -1.5f, 1.5f);
        x = x + x * -0.14814815f * x * x;
        const float low1 = low_ + band_ * f_;
        const float high1 = x - (low1 + k_ * band_);
        float band1 = std::clamp(high1 * f_, -0.8333333f, 0.8333333f) + band_;
        band1 = band1 - f_ * 0.5f * band1 * band1 * band1;
        const float low2 = low1 + f_ * band1;
        const float high2 = x - (low2 + k_ * band1);
        float band2 = std::clamp(high2 * f_, -0.8333333f, 0.8333333f) + band1;
        band2 = band2 - f_ * 0.5f * band2 * band2 * band2;
        const float out = (low_ * weight_[0] + band_ * weight_[1] + high_ * weight_[2]) * 0.25f
                          + (low1 * weight_[0] + high1 * weight_[2] + band1 * weight_[1]) * 0.5f
                          + (low2 * weight_[0] + high2 * weight_[2] + band2 * weight_[1]) * 0.25f;
        low_ = low2;
        band_ = band2;
        high_ = high2;
        return out;
    }

private:
    float f_ = 0.01f, k_ = 1.368f, low_ = 0, band_ = 0, high_ = 0, smoothing_ = 6.5e-4f;
    std::array<float, 3> target_{1.0f, 0.0f, 0.0f}, weight_{1.0f, 0.0f, 0.0f};
};

// Overdrive (mode 16): up to 16 cascaded stages, each a 10 Hz and a 15 Hz Butterworth high-pass per channel, a linked peak envelope
// (attack 100 /s, release 3.33 /s rates) that compresses by (env / thr)^-0.9 above thr = 10^(-1.7 d - 0.05), a gain of 10^(1.95 d + 0.05)
// and a tanh saturator above 0.9 (0.9 + 0.1 tanh(10 |v| - 9)), then 0.9886; the chain output is scaled by 1.33325. Structure as in the
// reference; every constant was confirmed against hosted renders. `d` is the drive fraction after a 0.0111 per-sample smoother.
class Overdrive
{
public:
    static constexpr int maxStages = 16;
    void prepare(double sampleRate) noexcept
    {
        const auto design = [&](double hz, double& gain, double& a1, double& a2) {
            const double t = std::tan(2.0 * 3.14159265358979323846 * hz / sampleRate), t2 = t * t;
            gain = 1.0 / (t * 1.4142135623730951 + 1.0 + t2);
            a1 = (t2 + t2 - 2.0) * gain;
            a2 = ((1.0 - t * 1.4142135623730951) + t2) * gain;
        };
        design(10.0, gain_[0], a1_[0], a2_[0]);
        design(15.0, gain_[1], a1_[1], a2_[1]);
        attack_ = 1.0 - std::exp(-100.0 / sampleRate);
        release_ = 1.0 - std::exp(-3.3333333333333335 / sampleRate);
        reset();
    }
    void reset() noexcept { state_ = {}; envelope_ = {}; smoothedDrive_ = 0.0f; }
    // Processes one stereo frame in place; `drive01` is the drive fraction, `stages` 1..16.
    void process(float& left, float& right, float drive01, int stages) noexcept
    {
        smoothedDrive_ += 0.01109f * (drive01 - smoothedDrive_);
        const double p = smoothedDrive_;
        const float threshold = static_cast<float>(std::pow(10.0, p * -1.7000000000000002 - 0.05));
        const float gain = static_cast<float>(std::pow(10.0, p * 1.9500000000000002 + 0.05));
        float channel[2] = {left, right};
        for (int stage = 0; stage < std::clamp(stages, 1, maxStages); ++stage)
        {
            auto& st = state_[static_cast<size_t>(stage)];
            double filtered[2];
            for (int c = 0; c < 2; ++c)
            {
                auto& s = st[static_cast<size_t>(c)];
                double u = gain_[0] * channel[c];
                double v = u + s[0];
                s[0] = (s[1] - (u + u)) - a1_[0] * v;
                s[1] = u - v * a2_[0];
                double w = gain_[1] * v;
                double z = w + s[2];
                s[2] = (s[3] - (w + w)) - a1_[1] * z;
                s[3] = w - z * a2_[1];
                filtered[c] = z;
            }
            const float peak = std::max(std::abs(static_cast<float>(filtered[0])), std::abs(static_cast<float>(filtered[1])));
            double& env = envelope_[static_cast<size_t>(stage)];
            env += ((peak <= env) ? release_ : attack_) * (static_cast<double>(peak) - env);
            const float reduction = threshold <= env ? std::pow(static_cast<float>(env * (1.0 / threshold)), -0.9f) : 1.0f;
            for (int c = 0; c < 2; ++c)
            {
                const float v = reduction * gain * static_cast<float>(filtered[c]);
                float out = v;
                if (std::abs(v) >= 0.9f)
                {
                    const float magnitude = std::tanh(std::abs(v) * 10.0f - 9.0f) * 0.1f + 0.9f;
                    out = v > 0.0f ? magnitude : -magnitude;
                }
                channel[c] = out * 0.9886f;
            }
        }
        left = channel[0] * 1.33325f;
        right = channel[1] * 1.33325f;
    }

private:
    std::array<double, 2> gain_{}, a1_{}, a2_{};
    double attack_ = 0, release_ = 0;
    std::array<std::array<std::array<double, 4>, 2>, maxStages> state_{};
    std::array<double, maxStages> envelope_{};
    float smoothedDrive_ = 0.0f;
};

} // namespace sonora::devices::serum_distortion
