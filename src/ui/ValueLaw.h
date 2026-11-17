#pragma once
// UI-93: what a dial's value text means. ValueLaw reads a dial's own text at a few points of its range and works out
// the unit it shows (semitones, cents, ms (s), Hz (kHz), dB, %, BPM, degrees, pan, a plain number) and how the shown
// number follows the dial's value; then it rounds drags to the unit's steps and turns typed text into a value.
// Pure C++ (no JUCE, no Skia): tests build it for x86_64 too, where the arithmetic must land on the same values.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sonora::ui
{

enum class ValueUnit
{
    None,         // a plain number ("4", "0.35")
    Semitones,    // st
    Cents,        // ct
    Milliseconds, // ms; seconds are read as 1000 ms
    Hertz,        // Hz; kHz is read as 1000 Hz
    Decibels,     // dB, "-inf dB"
    Percent,      // %
    Bpm,          // BPM
    Degrees,      // ° / deg
    Pan,          // 20L / C / 20R and 20M / C / 20S (left / mid negative)
    Other         // another word after the number ("x", "oct", "Voices")
};

// One number as a dial prints it or as someone types it.
struct ValueReading
{
    double value = 0;       // in the unit's base (ms, Hz); +-infinity for "inf"
    ValueUnit unit = ValueUnit::None;
    bool unitWritten = false; // the text named its unit (typed "500" names none)
    double multiplier = 1;  // what the written unit scales by (s -> 1000, kHz -> 1000)
    double resolution = 1;  // the last printed digit, in base units ("1.20 s" -> 10 ms)
};

// `context` (and its multiplier) says how a number without a unit is read, and settles "s" (seconds or Side) and "m".
// Accepts a comma as the decimal separator. Nullopt when the text is not one number with an optional unit.
std::optional<ValueReading> readValue(std::string_view text, ValueUnit context = ValueUnit::None, double contextMultiplier = 1);
// "st", "ct", "ms", "s", "Hz", "dB", "%", "BPM", "deg" (a ParameterInfo unit) -> the unit; unknown -> None.
ValueUnit valueUnitFromName(std::string_view);

struct DialRange
{
    std::function<std::string(float)> format; // the dial's value text for a value
    float minValue = 0, maxValue = 1;
    bool logarithmic = false;                 // frequency dials (positions are log-spaced)
    std::function<float(float)> fromNorm;     // optional: the dial's own position -> value law (overrides logarithmic)
    ValueUnit hint = ValueUnit::None;         // the parameter's declared unit, when the text alone cannot tell
};

class ValueLaw
{
public:
    // Reads the dial's text at 17 positions. Cheap enough to run whenever an edit starts.
    static ValueLaw detect(const DialRange&);

    bool numeric() const { return numeric_; }
    bool linear() const { return linear_; }
    ValueUnit unit() const { return unit_; }
    double displayOf(float value) const; // the number shown for a value (base units); NaN when not numeric
    // The step the unit moves by around a shown number (0: continuous).
    double step(double shown, bool fine) const;
    // The value whose shown number is `shown`, clamped to the range.
    float valueFor(double shown) const;
    // Rounds a value to the unit's step (a drag's raw value in, the value to set out).
    float quantize(float value, bool fine) const;
    // Moves one step up or down from `value` (arrow keys); at least one step even where quantize would round back.
    float nudge(float value, int direction, bool fine) const;
    // A typed text -> the value to set; nullopt when it cannot be read (the dial keeps its value).
    std::optional<float> fromText(std::string_view typed, float current) const;

private:
    float clampValue(float) const;
    float valueAtNorm(float) const;
    std::optional<double> shownAtNorm(float) const;

    DialRange range_;
    bool numeric_ = false, linear_ = false, integral_ = false, increasing_ = true;
    ValueUnit unit_ = ValueUnit::None;
    double scale_ = 1, offset_ = 0;
    double shownMin_ = 0, shownMax_ = 0; // finite ends of what the dial shows
};

// The value field (one per UiContext).

} // namespace sonora::ui
