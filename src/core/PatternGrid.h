#pragma once
// PLG-25a: the time grid of the pattern generators (Sonora Phrase, Pulse, the bass line command): the rate list, swing
// and bar / cycle arithmetic, in beats (quarter notes). One table so the devices, their faces and the clip commands agree.
//
// Header only, no JUCE, constexpr, usable on the audio thread.

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace sonora::model
{
struct GridRate
{
    std::string_view name;
    double beats; // length of one step in quarter notes
};

// Slowest last. T = triplet (2/3 of the straight value), D = dotted (3/2). Stored as indices: append, never reorder.
inline constexpr std::array<GridRate, 13> kGridRates{{
    {"1/32T", 1.0 / 12}, {"1/32", 1.0 / 8}, {"1/16T", 1.0 / 6}, {"1/16", 1.0 / 4}, {"1/16D", 3.0 / 8}, {"1/8T", 1.0 / 3},
    {"1/8", 1.0 / 2}, {"1/8D", 3.0 / 4}, {"1/4T", 2.0 / 3}, {"1/4", 1.0}, {"1/4D", 3.0 / 2}, {"1/2", 2.0}, {"1/1", 4.0},
}};
inline constexpr int kRate1_32T = 0, kRate1_32 = 1, kRate1_16T = 2, kRate1_16 = 3, kRate1_16D = 4, kRate1_8T = 5, kRate1_8 = 6,
                     kRate1_8D = 7, kRate1_4T = 8, kRate1_4 = 9, kRate1_4D = 10, kRate1_2 = 11, kRate1_1 = 12;

namespace grid
{
inline constexpr double kEpsilon = 1e-9; // positions this close to a boundary count as on it

// Step length in beats of rate index `index` (clamped to the table).
constexpr double rateBeats(int index) noexcept
{
    return kGridRates[(size_t) (index < 0 ? 0 : index >= (int) kGridRates.size() ? (int) kGridRates.size() - 1 : index)].beats;
}
// The rate whose name is `name` ("1/16T"), or -1.
constexpr int rateIndex(std::string_view name) noexcept
{
    for (size_t i = 0; i < kGridRates.size(); ++i)
        if (kGridRates[i].name == name) return (int) i;
    return -1;
}
constexpr bool isTriplet(int index) noexcept
{
    return index >= 0 && index < (int) kGridRates.size() && kGridRates[(size_t) index].name.back() == 'T';
}

constexpr double beatsPerBar(int numerator = 4, int denominator = 4) noexcept
{
    return (numerator > 0 ? numerator : 4) * 4.0 / (denominator > 0 ? denominator : 4);
}

// floor(beat / length) with positions within kEpsilon of a boundary counted on it (so 3.9999999999 is in step 4).
inline int64_t indexAt(double beat, double length) noexcept
{
    return length > 0 ? (int64_t) std::floor(beat / length + kEpsilon) : 0;
}
// Start of the step / bar / cycle of `length` beats that contains `beat`.
inline double startOf(double beat, double length) noexcept { return (double) indexAt(beat, length) * length; }
// Position of `beat` inside its cycle of `length` beats: [0, length).
inline double positionIn(double beat, double length) noexcept
{
    if (length <= 0) return 0.0;
    const double p = beat - startOf(beat, length);
    return p < 0 ? 0.0 : p;
}

// Swing: the straight position `beat` on a grid of `stepBeats` mapped to its swung position. Every second step (odd
// index) is late by amount x stepBeats / 3 (amount 0..1; 1 = a triplet feel); positions in between are stretched
// linearly, so the map is monotonic and continuous, steps on even indices stay put and 0 is straight. Same law as the
// Arpeggiator's groove.
inline double swingWarp(double beat, double stepBeats, double amount) noexcept
{
    if (amount <= 0 || stepBeats <= 0) return beat;
    const double s = (amount > 1 ? 1.0 : amount) / 3.0;
    const double pair = 2.0 * stepBeats;
    const double base = startOf(beat, pair);
    const double f = beat - base < 0 ? 0.0 : beat - base;
    return base + (f < stepBeats ? f * (1.0 + s) : stepBeats * (1.0 + s) + (f - stepBeats) * (1.0 - s));
}
// The swung position of step `index` (onset of step index x stepBeats).
inline double stepBeat(int64_t index, double stepBeats, double swing = 0.0) noexcept
{
    const double straight = (double) index * stepBeats;
    return swing > 0 && (index & 1) != 0 ? straight + stepBeats * (swing > 1 ? 1.0 : swing) / 3.0 : straight;
}

// The steps whose swung onset lies in [from, to): first and one past the last index. Use with stepBeat():
//   for (auto i = r.first; i < r.end; ++i) if (const double b = stepBeat(i, step, swing); b >= from && b < to) ...
// (the range is widened by one step on each side because swing moves onsets by up to a third of a step).
struct StepRange { int64_t first = 0, end = 0; };
inline StepRange stepsIn(double from, double to, double stepBeats) noexcept
{
    if (stepBeats <= 0 || to <= from) return {};
    return {indexAt(from, stepBeats) - 1, indexAt(to, stepBeats) + 2};
}
} // namespace grid
} // namespace sonora::model
