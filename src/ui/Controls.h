#pragma once
// Live 12-style control drawing: dropdown fields, value boxes, toggles, flat control-bar items.
// Pure drawing helpers (no state) so views and widgets share one look. Colours come from Theme tokens.

#include "ui/Painter.h"
#include "ui/Theme.h"

namespace sonora::ui::ctl
{

enum class Dir
{
    Up,
    Down,
    Left,
    Right
};

// Filled triangle centred on (cx, cy); `w` is the base, `h` the height.
void triangle(SkCanvas& c, float cx, float cy, float w, float h, Dir dir, SkColor color);
void loopBrace(SkCanvas&, const Theme&, const Rect&, bool enabled);
void midiMeter(SkCanvas&, const Theme&, const Rect&, bool active);

// Rectangle with a 1 px dark frame (the base of every Live control).
void framed(SkCanvas& c, const Theme& t, const Rect& r, SkColor fill);

// Light popup-menu field with a down arrow on the right ("All Ins", "Main"...).
// `size` 0 == the theme's small font size (also for valueBox and toggle).
void dropdown(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, bool enabled = true, float size = 0);

// Display/value box: light fill, frame, text aligned inside.
void valueBox(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align = Align::Centre,
              bool enabled = true, float size = 0);

// Empty/disabled text slot (e.g. the unused field below an output chooser).
void emptyBox(SkCanvas& c, const Theme& t, const Rect& r);

// Small framed toggle (In/Auto/Off, S, track number...). `onColour` is the lit fill.
void toggle(SkCanvas& c, const Theme& t, const Rect& r, std::string_view label, bool on, SkColor onColour,
            bool enabled = true, bool bold = false, float size = 0);

// Control-bar item: flat fill, no frame (tempo, signature, position display...).
void barBox(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align = Align::Centre, bool on = false,
            SkColor onColour = 0);

// Bold section caption ("MIDI From", "Sends").
void caption(SkCanvas& c, const Theme& t, const Rect& r, std::string_view text, Align align = Align::Left);

} // namespace sonora::ui::ctl
