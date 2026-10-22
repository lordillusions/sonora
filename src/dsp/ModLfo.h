#pragma once
// Modulation LFO with the seven waveforms Live's Delay offers (Sine, Triangle, Ramp Up, Ramp Down, Square,
// S&H, Wander) and a Morph amount. Allocation free; random numbers come from a per-instance LCG.
//
// Morph (Sonora's own definition, not measured against Live):
//   periodic waves  -> skews the phase around the midpoint (pulse width for Square, asymmetry for the others)
//   S&H             -> glide time between the held values (0 = steps, 1 = about half a cycle)
//   Wander          -> adds a second, 2.6x faster random component

#include <algorithm>
#include <cmath>

namespace sonora::dsp
{

class ModLfo
{
public:
    enum Wave { Sine, Triangle, RampUp, RampDown, Square, SampleHold, Wander, NumWaves };

    void reset(unsigned seed = 0x9e3779b9u)
    {
        rnd_ = seed;
        phase_ = 0.0;
        phaseFast_ = 0.0;
        for (auto& v : wander_)
            v = random();
        held_ = random();
        glide_ = held_;
        wander2_[0] = wander2_[1] = random();
    }

    void setPhase(double p) { phase_ = p - std::floor(p); }
    double phase() const { return phase_; }

    // Advances by `inc` cycles per sample and returns -1..1.
    float next(double inc, int wave, float morph)
    {
        phase_ += inc;
        bool wrapped = false;
        if (phase_ >= 1.0)
        {
            phase_ -= std::floor(phase_);
            wrapped = true;
        }
        const float p = (float) phase_;
        const float m = std::clamp(morph, 0.0f, 1.0f);
        switch (wave)
        {
            case SampleHold:
            {
                if (wrapped)
                    held_ = random();
                const float coef = m <= 0.0f ? 1.0f : std::min(1.0f, (float) inc / (m * 0.5f));
                glide_ += (held_ - glide_) * coef;
                return glide_;
            }
            case Wander:
            {
                if (wrapped)
                {
                    wander_[0] = wander_[1];
                    wander_[1] = random();
                }
                phaseFast_ += inc * 2.618;
                if (phaseFast_ >= 1.0)
                {
                    phaseFast_ -= std::floor(phaseFast_);
                    wander2_[0] = wander2_[1];
                    wander2_[1] = random();
                }
                const float slow = cosInterp(wander_[0], wander_[1], p);
                const float fast = cosInterp(wander2_[0], wander2_[1], (float) phaseFast_);
                return (slow + m * fast) / (1.0f + m);
            }
            default: break;
        }
        const float w = 0.5f + 0.49f * m;
        const float q = p < w ? 0.5f * p / w : 0.5f + 0.5f * (p - w) / (1.0f - w);
        switch (wave)
        {
            case Sine: return std::sin(6.28318530718f * q);
            case Triangle: return 1.0f - 4.0f * std::fabs(q - 0.5f);
            case RampUp: return 2.0f * q - 1.0f;
            case RampDown: return 1.0f - 2.0f * q;
            case Square: return q < 0.5f ? 1.0f : -1.0f;
        }
        return 0.0f;
    }

private:
    static float cosInterp(float a, float b, float t)
    {
        const float k = 0.5f - 0.5f * std::cos(3.14159265359f * t);
        return a + (b - a) * k;
    }
    float random()
    {
        rnd_ = rnd_ * 1664525u + 1013904223u;
        return (float) (rnd_ >> 8) / 8388608.0f - 1.0f;
    }

    double phase_ = 0.0, phaseFast_ = 0.0;
    unsigned rnd_ = 0x9e3779b9u;
    float held_ = 0.0f, glide_ = 0.0f;
    float wander_[2] = {0.0f, 0.0f};
    float wander2_[2] = {0.0f, 0.0f};
};

} // namespace sonora::dsp
