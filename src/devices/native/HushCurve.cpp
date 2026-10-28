#include "devices/native/HushCurve.h"

#include <algorithm>

namespace sonora::devices::hush
{
namespace
{
float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

// Bell width: the octave bandwidth of an analogue peaking filter with this Q, as a Gaussian's standard deviation.
float bellSigma(float q)
{
    const float bw = 2.0f * std::asinh(1.0f / (2.0f * clampf(q, 0.05f, 40.0f))) / std::log(2.0f);
    return std::max(0.02f, bw / 2.0f); // measured: a Q 0.707 bell still adds ~60 % one octave away
}

// Flat-topped band pass around fc (measured: full inside half an octave at Q 0.707, two thirds one octave out).
float passShape(float hz, float fc, float q)
{
    const float r = (hz / fc - fc / hz) * clampf(q, 0.05f, 40.0f);
    return 1.0f / std::sqrt(1.0f + r * r * r * r);
}

// Butterworth-like cut magnitude; slope 5 is about 19 dB per octave (measured), 0..10 spans 3..36.
float cutGate(float ratio, float slope)
{
    const float order = 0.5f + clampf(slope, 0.0f, 10.0f) * 0.54f;
    return 1.0f / std::sqrt(1.0f + std::pow(ratio, 2.0f * order));
}

float shelfSteepness(float slope) { return 1.0f + clampf(slope, 0.0f, 10.0f) * 0.9f; }
} // namespace

const char* shapeName(Shape s)
{
    switch (s)
    {
        case Shape::LowCut: return "low cut";
        case Shape::LowShelf: return "low shelf";
        case Shape::Bell: return "bell";
        case Shape::BandPass: return "band pass";
        case Shape::Notch: return "band reject";
        case Shape::Tilt: return "tilt";
        case Shape::HighShelf: return "high shelf";
        case Shape::HighCut: return "high cut";
    }
    return "bell";
}

bool usesSlope(Shape s)
{
    return s == Shape::LowCut || s == Shape::HighCut || s == Shape::LowShelf || s == Shape::HighShelf || s == Shape::Tilt;
}

BandResponse bandResponse(const Band& b, float hz)
{
    BandResponse out;
    const float fc = clampf(b.frequency, 10.0f, 30000.0f), f = std::max(hz, 1.0f);
    const float x = std::log2(f / fc);
    switch (b.shape)
    {
        case Shape::LowCut:
            out.gate = cutGate(fc / f, b.slope);
            out.curve = b.depth * out.gate;
            break;
        case Shape::HighCut:
            out.gate = cutGate(f / fc, b.slope);
            out.curve = b.depth * out.gate;
            break;
        case Shape::LowShelf:
            out.curve = b.depth / (1.0f + std::exp2(x * shelfSteepness(b.slope)));
            break;
        case Shape::HighShelf:
            out.curve = b.depth / (1.0f + std::exp2(-x * shelfSteepness(b.slope)));
            break;
        case Shape::Bell:
        {
            const float s = bellSigma(b.q);
            out.curve = b.depth * std::exp(-x * x / (2.0f * s * s));
            break;
        }
        case Shape::BandPass:
            out.pass = passShape(f, fc, b.q);
            out.curve = b.depth * out.pass;
            break;
        case Shape::Notch:
        {
            const float p = passShape(f, fc, b.q);
            out.gate = 1.0f - p;
            out.curve = b.depth * p;
            break;
        }
        case Shape::Tilt:
            // Positive depth: more processing above the frequency, less below. Slope narrows the transition.
            out.curve = b.depth * std::tanh(x * (1.3f + 0.15f * clampf(b.slope, 0.0f, 10.0f)));
            break;
    }
    return out;
}

CurvePoint evaluate(const std::array<Band, kBands>& bands, float hz, int channel)
{
    CurvePoint p;
    float pass = 0.0f;
    bool anyPass = false;
    for (const auto& b : bands)
    {
        if (! b.used || ! b.enabled) continue;
        const float amount = focusAmount(b.focus, channel);
        if (amount <= 0.0f) continue;
        const auto r = bandResponse(b, hz);
        p.curve += amount * r.curve;
        p.gate *= 1.0f - amount * (1.0f - r.gate);
        if (r.pass >= 0.0f)
        {
            anyPass = true;
            pass += amount * r.pass + (1.0f - amount); // an unfocused pass band lets everything through
        }
    }
    if (anyPass) p.gate *= std::min(1.0f, pass);
    p.curve = clampf(p.curve, -40.0f, 40.0f);
    return p;
}

float bandRegion(const Band& b, float hz)
{
    const float fc = clampf(b.frequency, 10.0f, 30000.0f), f = std::max(hz, 1.0f);
    const float x = std::log2(f / fc);
    switch (b.shape)
    {
        case Shape::LowCut: return 1.0f - cutGate(fc / f, b.slope);
        case Shape::HighCut: return 1.0f - cutGate(f / fc, b.slope);
        case Shape::LowShelf: return 1.0f / (1.0f + std::exp2(x * shelfSteepness(b.slope)));
        case Shape::HighShelf: return 1.0f / (1.0f + std::exp2(-x * shelfSteepness(b.slope)));
        case Shape::Tilt: return 0.5f + 0.5f * std::tanh(x * (b.depth < 0 ? -1.0f : 1.0f) * (1.3f + 0.15f * b.slope));
        case Shape::Bell:
        {
            const float s = bellSigma(b.q);
            return std::exp(-x * x / (2.0f * s * s));
        }
        case Shape::BandPass:
        case Shape::Notch: return passShape(f, fc, b.q);
    }
    return 0.0f;
}

} // namespace sonora::devices::hush
