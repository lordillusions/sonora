#pragma once
// Pure helpers for Live's encodings. Unit tested in tests/test_core.cpp.

#include <cmath>
#include <utility>

namespace sonora::model
{

// Live stores the time signature as one enum integer:
//   value = (numerator - 1) + 99 * log2(denominator)      e.g. 201 == 4/4
inline std::pair<int, int> decodeTimeSignature(int value)
{
    const int numerator = value % 99 + 1;
    const int denominator = 1 << (value / 99);
    return {numerator, denominator};
}

inline int encodeTimeSignature(int numerator, int denominator)
{
    int log2den = 0;
    while ((1 << log2den) < denominator)
        ++log2den;
    return (numerator - 1) + 99 * log2den;
}

inline double gainToDecibels(double gain)
{
    return gain <= 0.000316228 ? -70.0 : 20.0 * std::log10(gain);
}

inline double decibelsToGain(double db)
{
    return db <= -70.0 ? 0.0 : std::pow(10.0, db / 20.0);
}

// Smooth, concave fade shared by clip drawing and both audio playback paths.
// Fixed arithmetic keeps the per-sample callback bounded and allocation-free.
inline double clipFadeGain(double progress, double shape = 0.0)
{
    if (progress <= 0.0) return 0.0;
    if (progress >= 1.0) return 1.0;
    // Rational skew: monotone, exact endpoints, bounded work in the audio callback.
    const double bounded = std::fmax(-0.95, std::fmin(0.95, shape));
    const double factor = (1.0 + bounded) / (1.0 - bounded);
    progress = progress / (progress + factor * (1.0 - progress));
    const double inverse = 1.0 - progress;
    return 1.0 - inverse * inverse;
}

// Maps an arrangement position to a position inside the clip's content, following
// the loop region. Returns the content position in beats.
// Approximation of Live's semantics, see docs/ALS_FORMAT.md "Clip timing".
inline double clipContentPosition(double timelineBeats,
                                  double currentStart,
                                  double loopStart,
                                  double loopEnd,
                                  double startRelative,
                                  bool looping)
{
    const double elapsed = timelineBeats - currentStart;
    const double start = loopStart + startRelative;
    if (! looping || loopEnd <= loopStart)
        return start + elapsed;

    const double length = loopEnd - loopStart;
    double pos = start + elapsed;
    if (pos >= loopEnd)
        pos = loopStart + std::fmod(pos - loopStart, length);
    return pos;
}

} // namespace sonora::model
