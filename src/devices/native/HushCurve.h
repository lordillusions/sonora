#pragma once
// Sonora Hush (PLG-14): the depth curve. Up to eight EQ-style bands shape where the resonance suppressor works: bells and
// shelves raise or lower the amount of reduction ("curve dB"), cuts and band passes gate it. The audio thread evaluates it
// per analysis bin and the editor draws it, so both use these functions. Independent design written from the public
// product manual's feature description; the shapes are Sonora's own.

#include <array>
#include <cmath>

namespace sonora::devices::hush
{

enum class Shape
{
    LowCut,
    LowShelf,
    Bell,
    BandPass,
    Notch,
    Tilt,
    HighShelf,
    HighCut
};
inline constexpr int kShapes = 8;
inline constexpr int kBands = 8;

const char* shapeName(Shape);
bool usesSlope(Shape); // cuts, shelves and tilt show "slope" instead of "q"

struct Band
{
    bool used = false, enabled = true;
    Shape shape = Shape::Bell;
    float frequency = 1000.0f; // Hz
    float depth = 0.0f;        // -10..10
    float q = 0.707f;          // 0.1..10
    float slope = 5.0f;        // 0..10
    float focus = 0.5f;        // 0 = left/mid only .. 0.5 even .. 1 = right/side only
};

// How much of a stereo-focus value reaches processing channel `channel` (0 = left/mid, 1 = right/side): 1 at "even".
inline float focusAmount(float focus, int channel)
{
    const float f = focus < 0.0f ? 0.0f : focus > 1.0f ? 1.0f : focus;
    const float v = channel == 0 ? 2.0f * (1.0f - f) : 2.0f * f;
    return v > 1.0f ? 1.0f : v;
}

// One band's effect at `hz`: `curve` is added (curve dB), `gate` (0..1) multiplies the processing. Pass bands report
// their pass value through `pass` instead, because several pass bands combine in parallel.
struct BandResponse
{
    float curve = 0.0f, gate = 1.0f, pass = -1.0f; // pass < 0: not a pass band
};
BandResponse bandResponse(const Band&, float hz);

// The whole curve at `hz` for processing channel `channel` (band focus applied).
struct CurvePoint
{
    float curve = 0.0f; // dB, the sum of the bands' boosts and cuts
    float gate = 1.0f;  // 0..1, cuts and pass bands
    // Multiplier of the reduction drive: boosts count 0.8 dB per curve dB, cuts 1.1 (fitted to measurements).
    float weight() const { return std::pow(10.0f, (curve > 0.0f ? 0.8f : 1.1f) * curve / 20.0f) * gate; }
    // What the editor draws: the curve with the gate shown as a drop (floored at -60).
    float drawn() const { return curve + 20.0f * std::log10(gate > 1.0e-3f ? gate : 1.0e-3f); }
};
CurvePoint evaluate(const std::array<Band, kBands>&, float hz, int channel);

// The band's own frequency region (0..1), for band listen: what the band covers on the spectrum.
float bandRegion(const Band&, float hz);

} // namespace sonora::devices::hush
