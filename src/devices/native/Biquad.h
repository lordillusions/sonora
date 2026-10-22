#pragma once
// Allocation-free RBJ-cookbook biquad. Safe to recompute coefficients on the audio thread.
// Reference: Robert Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients".

#include <cmath>

namespace sonora::devices
{

struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; // normalised (a0 == 1)

    enum class Type
    {
        LowPass,
        HighPass,
        Peak,
        LowShelf,
        HighShelf,
        Notch,
        BandPass // constant 0 dB peak gain
    };

    static BiquadCoeffs make(Type type, double sampleRate, double freq, double q, double gainDb)
    {
        freq = std::fmin(std::fmax(freq, 10.0), sampleRate * 0.49);
        q = std::fmax(q, 0.05);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * M_PI * freq / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type)
        {
            case Type::LowPass:
                b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::HighPass:
                b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::Peak:
                b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
                a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
                break;
            case Type::BandPass:
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::Notch:
                b0 = 1; b1 = -2 * cw; b2 = 1;
                a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
                break;
            case Type::LowShelf:
            {
                const double s = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) - (A - 1) * cw + s);
                b1 = 2 * A * ((A - 1) - (A + 1) * cw);
                b2 = A * ((A + 1) - (A - 1) * cw - s);
                a0 = (A + 1) + (A - 1) * cw + s;
                a1 = -2 * ((A - 1) + (A + 1) * cw);
                a2 = (A + 1) + (A - 1) * cw - s;
                break;
            }
            case Type::HighShelf:
            {
                const double s = 2 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1) + (A - 1) * cw + s);
                b1 = -2 * A * ((A - 1) + (A + 1) * cw);
                b2 = A * ((A + 1) + (A - 1) * cw - s);
                a0 = (A + 1) - (A - 1) * cw + s;
                a1 = 2 * ((A - 1) - (A + 1) * cw);
                a2 = (A + 1) - (A - 1) * cw - s;
                break;
            }
        }
        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    // Magnitude response at `freq`, used by the EQ display and tests.
    double magnitudeAt(double freq, double sampleRate) const
    {
        const double w = 2.0 * M_PI * freq / sampleRate;
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
    }
};

struct BiquadState
{
    double z1 = 0, z2 = 0;

    float process(const BiquadCoeffs& c, float x)
    {
        // Transposed direct form II
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }

    void reset() { z1 = z2 = 0; }
};

// The same second-order filter as a trapezoidal (TPT) state-variable filter: identical response to BiquadState for fixed
// coefficients, but its integrator states stay continuous when the coefficients jump. A transposed direct form keeps
// coefficient-weighted signals in its state, so a large cutoff step on a loud signal (automation steps, modulation)
// produces a transient that can exceed full scale; use this form wherever the coefficients move while audio plays.
// The analog prototype comes from the inverse bilinear transform p = (1 - z^-1) / (1 + z^-1) of the biquad:
// H(p) = (N2 p^2 + N1 p + N0) / (A2 p^2 + A1 p + A0), realised as a mix of the SVF's high / band / low outputs.
struct SvfCoeffs
{
    double a1 = 1, a2 = 0, a3 = 0, k = 2; // tick coefficients (Simper's formulation), damping k
    double mHigh = 0, mBand = 0, mLow = 1;

    static SvfCoeffs fromBiquad(const BiquadCoeffs& c)
    {
        // Stable biquads have A0, A1, A2 > 0 (the stability triangle); clamp only against poles exactly on z = +-1.
        const double A2 = std::fmax(1.0 - c.a1 + c.a2, 1e-12), A1 = 2.0 * (1.0 - c.a2), A0 = std::fmax(1.0 + c.a1 + c.a2, 1e-12);
        const double N2 = c.b0 - c.b1 + c.b2, N1 = 2.0 * (c.b0 - c.b2), N0 = c.b0 + c.b1 + c.b2;
        SvfCoeffs s;
        const double g = std::sqrt(A0 / A2);
        s.k = g * A1 / A0;
        s.a1 = 1.0 / (1.0 + g * (g + s.k));
        s.a2 = g * s.a1;
        s.a3 = g * s.a2;
        s.mHigh = N2 * g * g / A0;
        s.mBand = N1 * g / A0;
        s.mLow = N0 / A0;
        return s;
    }
};

struct SvfState
{
    double ic1 = 0, ic2 = 0;

    float process(const SvfCoeffs& c, float x)
    {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        const double high = x - c.k * v1 - v2;
        return (float) (c.mHigh * high + c.mBand * v1 + c.mLow * v2);
    }

    void reset() { ic1 = ic2 = 0; }
};

} // namespace sonora::devices
