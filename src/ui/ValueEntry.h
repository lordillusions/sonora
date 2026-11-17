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
#include "ui/ValueLaw.h"

#include <cstdint>
#include <functional>
#include <string>

class SkCanvas;

namespace sonora::ui
{
class Widget;
struct KeyEvent;
class Theme;
struct UiContext;

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
