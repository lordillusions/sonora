#pragma once
// Trance gate (PLG-27, becomes the Sonora Gate audio effect in PLG-30): a step pattern of levels locked to the song
// grid. Each step opens for Length of its duration at its level, edges ramp over Smooth (no step in the gain), Depth
// mixes towards no gating, Swing delays every second step. Header only, no JUCE; allocation free after construction.
//
// The grid: step i starts at beat i * stepBeats (odd steps later by swing * stepBeats / 2), the pattern repeats every
// `steps` steps from beat 0, so the same bar always gates the same way. The caller passes the song position of every
// sample (or a free clock when the transport is stopped).

#include <algorithm>
#include <array>
#include <cmath>

namespace sonora::dsp
{
class TranceGate
{
public:
    static constexpr int kMaxSteps = 32;
    enum Rate { Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond, kRates };
    static constexpr const char* kRateNames[kRates] = {"1/8", "1/8T", "1/16", "1/16T", "1/32"};
    static constexpr double stepBeats(int rate)
    {
        constexpr double beats[kRates] = {0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
        return beats[(size_t) std::clamp(rate, 0, kRates - 1)];
    }

    struct Pattern
    {
        const char* name;
        const char* description;
        int steps;          // 12, 16, 24 or 32
        int rate;           // the rate it is written for (triplet patterns: a triplet rate); -1 = any
        float level[kMaxSteps];
    };
    static constexpr int kFactoryPatterns = 26;
    static const std::array<Pattern, kFactoryPatterns>& factory();

    void prepare(double sampleRate)
    {
        sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
        reset();
    }
    void reset()
    {
        gain_ = 1.0f;
        target_ = -1.0f;
        remaining_ = 0;
        increment_ = 0.0f;
    }
    void setPattern(const float* levels, int steps)
    {
        steps_ = std::clamp(steps, 1, kMaxSteps);
        for (int i = 0; i < kMaxSteps; ++i) levels_[(size_t) i] = i < steps_ ? std::clamp(levels[i], 0.0f, 1.0f) : 0.0f;
    }
    void setShape(int rate, float length, float smoothMs, float depth, float swing)
    {
        stepBeats_ = stepBeats(rate);
        length_ = std::clamp(length, 0.01f, 1.0f);
        smoothSamples_ = std::max(1, (int) std::lround(std::clamp(smoothMs, 0.0f, 500.0f) * 0.001 * sampleRate_));
        depth_ = std::clamp(depth, 0.0f, 1.0f);
        swing_ = std::clamp(swing, 0.0f, 1.0f);
    }

    // The gain the pattern asks for at `beat` (no smoothing).
    float target(double beat) const
    {
        const double pair = 2.0 * stepBeats_;
        const double inPair = beat - std::floor(beat / pair) * pair;
        const double oddStart = stepBeats_ * (1.0 + 0.5 * swing_);
        const bool odd = inPair >= oddStart;
        const double stepStart = odd ? oddStart : 0.0;
        const double stepLength = odd ? pair - oddStart : oddStart;
        const long long pairIndex = (long long) std::floor(beat / pair);
        const long long stepIndex = pairIndex * 2 + (odd ? 1 : 0);
        const int step = (int) (((stepIndex % steps_) + steps_) % steps_);
        const bool open = inPair - stepStart < stepLength * (double) length_;
        const float level = open ? levels_[(size_t) step] : 0.0f;
        return 1.0f - depth_ * (1.0f - level);
    }
    // Advances one sample at song position `beat` and returns the smoothed gain: when the target changes, the gain ramps
    // to it linearly over Smooth, starting at that very sample.
    float next(double beat)
    {
        const float t = target(beat);
        if (std::abs(t - target_) > 1.0e-6f)
        {
            if (target_ < 0.0f) gain_ = t; // first sample: no ramp from nowhere
            target_ = t;
            remaining_ = smoothSamples_;
            increment_ = (t - gain_) / (float) smoothSamples_;
        }
        if (remaining_ > 0)
        {
            gain_ += increment_;
            if (--remaining_ == 0) gain_ = target_;
        }
        return gain_;
    }
    int steps() const { return steps_; }
    float level(int step) const { return levels_[(size_t) std::clamp(step, 0, kMaxSteps - 1)]; }
    // The step playing at `beat` (for the face's play head).
    int stepAt(double beat) const
    {
        const double pair = 2.0 * stepBeats_;
        const double inPair = beat - std::floor(beat / pair) * pair;
        const bool odd = inPair >= stepBeats_ * (1.0 + 0.5 * swing_);
        const long long stepIndex = (long long) std::floor(beat / pair) * 2 + (odd ? 1 : 0);
        return (int) (((stepIndex % steps_) + steps_) % steps_);
    }

private:
    double sampleRate_ = 44100.0, stepBeats_ = 0.25;
    std::array<float, kMaxSteps> levels_{};
    int steps_ = 16, smoothSamples_ = 220, remaining_ = 0;
    float length_ = 0.65f, depth_ = 1.0f, swing_ = 0.0f, gain_ = 1.0f, target_ = -1.0f, increment_ = 0.0f;
};

// Factory patterns. Rhythms of the style (docs/TRANCE_SUITE.md "Style facts"): straight and offbeat sixteenths, the
// kick's sixteenth left alone, 3-3-2, gallops, dotted figures, triplets, two-bar patterns whose second bar varies, ramps.
inline const std::array<TranceGate::Pattern, TranceGate::kFactoryPatterns>& TranceGate::factory()
{
    constexpr int S16 = Sixteenth, T16 = SixteenthTriplet, T8 = EighthTriplet;
    static const std::array<Pattern, kFactoryPatterns> list = {{
        {"Straight 16", "Every sixteenth open: the classic gated pad.", 16, -1, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}},
        {"Eighths", "Every eighth: half the chop, more air.", 16, S16, {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0}},
        {"Offbeat 16", "Open on the three sixteenths after each beat: follows the rolling bass.", 16, S16,
         {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1}},
        {"Kick Space", "Closed on the beat, the kick's sixteenth left alone, the rest shaped.", 16, S16,
         {0, 0.8f, 1, 0.8f, 0, 0.8f, 1, 0.8f, 0, 0.8f, 1, 0.8f, 0, 0.8f, 1, 0.8f}},
        {"Offbeat 8", "Only the eighth between the beats.", 16, S16, {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0}},
        {"3-3-2", "Three, three, two: the tresillo over a bar of eighths.", 16, S16,
         {1, 0.3f, 0.3f, 1, 0.3f, 0.3f, 1, 0.3f, 1, 0.3f, 0.3f, 1, 0.3f, 0.3f, 1, 0.3f}},
        {"3-3-3-3-4", "Dotted eighths across the bar, reset by a quarter.", 16, S16,
         {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0}},
        {"Gallop", "An eighth and two sixteenths on every beat.", 16, S16, {1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1}},
        {"Reverse Gallop", "Two sixteenths and an eighth on every beat.", 16, S16, {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0}},
        {"Dotted", "Dotted eighths over two bars, the figure crossing the bar line.", 32, S16,
         {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0}},
        {"Two and Two", "Two open, two closed.", 16, S16, {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0}},
        {"Long Short", "Long-short pairs: a dotted eighth and a sixteenth.", 16, S16, {1, 1, 1, 0.6f, 1, 1, 1, 0.6f, 1, 1, 1, 0.6f, 1, 1, 1, 0.6f}},
        {"Syncopated", "Off the grid: pushes before the beats.", 16, S16, {1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0}},
        {"Accents", "Every sixteenth, the offbeat ones louder.", 16, S16,
         {0.45f, 0.7f, 1, 0.7f, 0.45f, 0.7f, 1, 0.7f, 0.45f, 0.7f, 1, 0.7f, 0.45f, 0.7f, 1, 0.7f}},
        {"Duck", "Every beat dips and swells back: a pumping sidechain feel.", 16, S16,
         {0.15f, 0.55f, 0.85f, 1, 0.15f, 0.55f, 0.85f, 1, 0.15f, 0.55f, 0.85f, 1, 0.15f, 0.55f, 0.85f, 1}},
        {"Quarter Pulse", "One open sixteenth per beat.", 16, S16, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}},
        {"Triplet Roll", "Sixteenth triplets with the first of each beat closed.", 24, T16,
         {0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1}},
        {"Triplet Eighths", "Eighth triplets, the middle one quieter.", 12, T8, {1, 0.5f, 1, 1, 0.5f, 1, 1, 0.5f, 1, 1, 0.5f, 1}},
        {"Shuffle Triplet", "Triplet shuffle: long-short on every beat.", 24, T16,
         {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1}},
        {"Turnaround", "Two bars: offbeat sixteenths, then the last beat opens into straight sixteenths.", 32, S16,
         {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}},
        {"Call and Answer", "Two bars: a busy bar, then a sparse one.", 32, S16,
         {1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0}},
        {"Bar Break", "Two bars: gallops, with the last half bar left closed for a breath.", 32, S16,
         {1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"Stutter End", "Two bars of eighths, the end chopped into sixteenths.", 32, S16,
         {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1}},
        {"Ramp Up", "Offbeat sixteenths rising in level over the bar.", 16, S16,
         {0, 0.3f, 0.35f, 0.4f, 0, 0.5f, 0.55f, 0.6f, 0, 0.7f, 0.75f, 0.8f, 0, 0.9f, 0.95f, 1}},
        {"Ramp Down", "Every sixteenth falling in level over the bar.", 16, S16,
         {1, 0.95f, 0.9f, 0.85f, 0.8f, 0.75f, 0.7f, 0.65f, 0.6f, 0.55f, 0.5f, 0.45f, 0.4f, 0.35f, 0.3f, 0.25f}},
        {"Long Ramp", "Two bars rising from a whisper to full.", 32, S16,
         {0.1f, 0.13f, 0.16f, 0.19f, 0.22f, 0.25f, 0.28f, 0.31f, 0.34f, 0.37f, 0.4f, 0.43f, 0.46f, 0.49f, 0.52f, 0.55f,
          0.58f, 0.61f, 0.64f, 0.67f, 0.7f, 0.73f, 0.76f, 0.79f, 0.82f, 0.85f, 0.88f, 0.91f, 0.94f, 0.97f, 1, 1}},
    }};
    return list;
}
} // namespace sonora::dsp
