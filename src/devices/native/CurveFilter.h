#pragma once
// Stereo multi-mode filter used by the Sonora Curve LFO (DEV-16b). Independent implementation:
// TPT state-variable stages, one-pole stages, a four-pole ladder and a damped feedback comb.
// Coefficients are set at control rate with setup(); process() is allocation-free.

#include "dsp/Dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sonora::devices
{

class CurveFilter
{
public:
    enum Type
    {
        Low6, Low12, Low18, Low24, High6, High12, High18, High24,
        Band, Notch, Peak, Morph, DualLowHigh, DualLowBand, Ladder, Comb, TypeCount
    };
    static const std::vector<std::string>& typeNames()
    {
        static const std::vector<std::string> names{"Low 6", "Low 12", "Low 18", "Low 24", "High 6", "High 12", "High 18",
                                                    "High 24", "Band", "Notch", "Peak", "Morph LBH", "Dual LH", "Dual LB",
                                                    "Ladder 24", "Comb"};
        return names;
    }
    // Control position 0..1 -> 20 Hz .. 20 kHz.
    static float positionToHz(float position) { return 20.0f * std::pow(1000.0f, std::clamp(position, 0.0f, 1.0f)); }

    void prepare(double sampleRate)
    {
        sampleRate_ = std::max(8000.0, sampleRate);
        for (auto& c : channels_) c.delay.prepare((int) (sampleRate_ / 20.0) + 8);
        reset();
    }
    void reset()
    {
        for (auto& c : channels_)
        {
            c.svf = {}; c.svf2 = {}; c.dual = {}; c.pole = {}; c.ladder = {}; c.damp = 0;
            c.delay.reset();
        }
    }
    void setup(int type, float cutoffHz, float resonance, float var)
    {
        type_ = std::clamp(type, 0, (int) TypeCount - 1);
        cutoff_ = std::clamp(cutoffHz, 20.0f, (float) (sampleRate_ * 0.45));
        resonance_ = std::clamp(resonance, 0.0f, 1.0f);
        var_ = std::clamp(var, 0.0f, 1.0f);
        const float q = 0.55f + resonance_ * resonance_ * 14.0f;
        tpt_.set(sampleRate_, cutoff_, q);
        tpt2_.set(sampleRate_, cutoff_, 0.7071f);
        const float cutoff2 = positionToHz(var_);
        tptDual_.set(sampleRate_, std::min(cutoff2, (float) (sampleRate_ * 0.45)), 0.7071f);
        poleCoef_ = 1.0f - std::exp(-2.0f * (float) dsp::kPi * cutoff_ / (float) sampleRate_);
        ladderG_ = 1.0f - std::exp(-2.0f * (float) dsp::kPi * cutoff_ / (float) (sampleRate_ * 1.25));
        combDelay_ = (float) (sampleRate_ / cutoff_);
        combDamp_ = 1.0f - std::exp(-2.0f * (float) dsp::kPi * std::max(200.0f, positionToHz(1.0f - var_)) / (float) sampleRate_);
    }
    float process(int channel, float x)
    {
        auto& c = channels_[(size_t) std::clamp(channel, 0, 1)];
        float lp, bp, hp;
        switch (type_)
        {
            case Low6: return pole(c.pole[0], x);
            case Low12: tpt_.tick(c.svf, x, lp, bp, hp); return lp;
            case Low18: tpt_.tick(c.svf, x, lp, bp, hp); return pole(c.pole[0], lp);
            case Low24: tpt_.tick(c.svf, x, lp, bp, hp); tpt2_.tick(c.svf2, lp, lp, bp, hp); return lp;
            case High6: return x - pole(c.pole[0], x);
            case High12: tpt_.tick(c.svf, x, lp, bp, hp); return hp;
            case High18: tpt_.tick(c.svf, x, lp, bp, hp); return hp - pole(c.pole[0], hp);
            case High24: tpt_.tick(c.svf, x, lp, bp, hp); tpt2_.tick(c.svf2, hp, lp, bp, hp); return hp;
            case Band: tpt_.tick(c.svf, x, lp, bp, hp); return bp * tpt_.k;
            case Notch: tpt_.tick(c.svf, x, lp, bp, hp); return x - bp * tpt_.k;
            case Peak: tpt_.tick(c.svf, x, lp, bp, hp); return lp - hp;
            case Morph:
            {
                tpt_.tick(c.svf, x, lp, bp, hp);
                const float b = bp * tpt_.k;
                return var_ < 0.5f ? lp + (b - lp) * var_ * 2 : b + (hp - b) * (var_ - 0.5f) * 2;
            }
            case DualLowHigh:
            {
                tpt_.tick(c.svf, x, lp, bp, hp); float l2, b2, h2;
                tptDual_.tick(c.dual, lp, l2, b2, h2); return h2;
            }
            case DualLowBand:
            {
                tpt_.tick(c.svf, x, lp, bp, hp); float l2, b2, h2;
                tptDual_.tick(c.dual, lp, l2, b2, h2); return b2 * tptDual_.k;
            }
            case Ladder:
            {
                const float k = resonance_ * 3.9f;
                const float in = std::tanh(x - k * c.ladder[3]);
                c.ladder[0] += ladderG_ * (in - c.ladder[0]);
                for (size_t i = 1; i < 4; ++i) c.ladder[i] += ladderG_ * (c.ladder[i - 1] - c.ladder[i]);
                return c.ladder[3] * (1.0f + var_ * k * 0.5f);
            }
            default:
            {
                const float feedback = resonance_ * 0.95f;
                const float delayed = c.delay.read(std::max(1.0f, combDelay_ - 1.0f));
                c.damp += combDamp_ * (delayed - c.damp);
                const float y = x + feedback * c.damp;
                c.delay.write(std::isfinite(y) ? y : 0.0f);
                return y * (1.0f - feedback * 0.5f);
            }
        }
    }

private:
    struct SvfState { float ic1 = 0, ic2 = 0; };
    struct Tpt
    {
        float g = 0, k = 1, a1 = 0, a2 = 0, a3 = 0;
        void set(double sampleRate, float fc, float q)
        {
            g = std::tan((float) dsp::kPi * std::clamp(fc, 10.0f, (float) (sampleRate * 0.45)) / (float) sampleRate);
            k = 1.0f / std::max(0.05f, q);
            a1 = 1.0f / (1.0f + g * (g + k)); a2 = g * a1; a3 = g * a2;
        }
        void tick(SvfState& s, float x, float& lp, float& bp, float& hp) const
        {
            const float v3 = x - s.ic2, v1 = a1 * s.ic1 + a2 * v3, v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
            s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2;
            lp = v2; bp = v1; hp = x - k * v1 - v2;
        }
    };
    float pole(float& state, float x) const { state += poleCoef_ * (x - state); return state; }
    struct Channel
    {
        SvfState svf, svf2, dual;
        std::array<float, 1> pole{};
        std::array<float, 4> ladder{};
        float damp = 0;
        dsp::FractionalDelay delay;
    };
    double sampleRate_ = 44100;
    int type_ = Low12;
    float cutoff_ = 1000, resonance_ = 0, var_ = 0, poleCoef_ = 0, ladderG_ = 0, combDelay_ = 100, combDamp_ = 1;
    Tpt tpt_, tpt2_, tptDual_;
    std::array<Channel, 2> channels_;
};

} // namespace sonora::devices
