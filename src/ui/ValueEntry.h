#pragma once
// UI-93: dials know their unit and take typed values.
//
// ValueLaw reads a dial's value text (the formatter it already has) at a few points of its range and works out what it
// shows: semitones, cents, ms (s), Hz (kHz), dB, %, BPM, degrees, pan (L/C/R, M/S) or a plain number, and how the shown
// number follows the dial's own value (linear when it can prove it, otherwise by searching the printed text). With that:
//   - quantize(): drags, the wheel and the arrow keys move in steps that make sense for the unit (whole semitones and
//     cents, whole BPM, ms in 0.01 / 0.1 / 1 / 10 ms by size, Hz with three significant digits, dB in 0.1, % in 1);
//     Shift is finer where the unit allows it (never for st / ct);
//   - fromText(): a typed value ("250", "1.5 s", "2k", "-6", "-inf", "20L", "C", "7", "1/16") becomes the dial's value.
//     A number without a unit is read in the unit the dial shows now (a dial showing "1.20 kHz" reads "2" as 2 kHz).
//
// ValueEntry is the field itself: a dial (or a face that draws dials) opens it over the value it prints; the root sends it
// every key while it is open, draws it over everything and commits it on a click elsewhere, as the UI-58 rename does.

#include "ui/InlineEdit.h"
#include "ui/Rect.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

class SkCanvas;

namespace sonora::ui
{
class Widget;
struct KeyEvent;
class Theme;
struct UiContext;

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
struct ValueEntry
{
    Widget* owner = nullptr;  // the dial or face that opened it; the field closes if it is deleted
    Rect local;               // where the value is printed, in the owner's coordinates
    InlineTextEdit edit;
    std::string initial;      // the text it opened with: committing it unchanged leaves the value alone
    std::function<void(const std::string&)> commit;
    float fontSize = 0;       // 0: the theme's small size

    bool active() const;
    Rect boundsInRoot() const; // the field as drawn: at least wide enough for the text
};

// Opens the field over `local` (owner coordinates) with `initial` selected, or, when `firstChar` is set, with that
// character typed (a digit typed on a selected dial). Commits a field that was already open.
void beginValueEntry(Widget& owner, const Rect& local, const std::string& initial, std::function<void(const std::string&)> commit,
                     uint32_t firstChar = 0, float fontSize = 0);
// The root's key handler while a field is open. Always consumes the key.
bool valueEntryKey(UiContext&, const KeyEvent&);
void commitValueEntry(UiContext&);
void cancelValueEntry(UiContext&);
void paintValueEntry(SkCanvas&, const UiContext&, const Theme&);
// A plain key that opens the field on a selected dial: digits, '-', '+', '.', ','.
bool startsValueEntry(const KeyEvent&);

} // namespace sonora::ui
