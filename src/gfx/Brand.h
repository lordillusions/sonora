#pragma once
// Sonora brand drawing (UI-51): the "S" mark, the wordmark, the app icon, the launch splash and the
// busy indicator / cursor. Everything is grayscale and drawn from the geometry in Brand.cpp, so the
// app, tools/brand_render (icon PNGs, SVGs) and the website share one source. No font files are
// involved: the wordmark letters are stroked paths.

#include "include/core/SkColor.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"

#include <juce_graphics/juce_graphics.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

class SkCanvas;
class SkPaint;

namespace sonora::gfx::brand
{

// One stroked piece of the mark or wordmark: a centerline and its stroke width (butt caps).
struct Stroke
{
    SkPath centerline;
    float width = 0;
};

// The "S" ribbon: three bars joined by half circles, in a box of height 1 (width markAspect), y down.
constexpr float markAspect = 0.76f;
std::vector<Stroke> markStrokes(const SkRect& box);

// "Sonora", cap height 1, baseline at y = 0 (y down), starting at x = 0. `width` receives the advance.
std::vector<Stroke> wordmarkStrokes(float& width);

// Filled outlines of strokes (for SVG export and hit-free filling).
SkPath outline(const Stroke& stroke);
void drawStrokes(SkCanvas& canvas, const std::vector<Stroke>& strokes, SkColor color);
void drawStrokes(SkCanvas& canvas, const std::vector<Stroke>& strokes, const SkPaint& paint);

// The macOS app icon on a size x size canvas (Big Sur grid: 824/1024 body, soft shadow).
void drawAppIcon(SkCanvas& canvas, float size);

// The wordmark at `capHeight` (its S is the mark); returns the drawn width.
float drawWordmark(SkCanvas& canvas, float x, float baseline, float capHeight, SkColor color);

struct SplashState
{
    std::string version;   // "Version 1.0 (42)"
    std::string status;    // "Starting plug-ins 3 of 12 - Serum 2"
    float progress = -1;   // 0..1, or < 0 for an indeterminate sweep
    double timeSeconds = 0; // drives the sweep animation
};
constexpr float splashWidth = 600, splashHeight = 360;
void drawSplash(SkCanvas& canvas, float width, float height, const SplashState& state);

// App prompt (UI-44, e.g. "Save changes?"): the splash's panel with the app icon, a bold title, a wrapped message and
// buttons laid out like a macOS alert: the primary button rightmost, then the others leftwards, a `leading` button
// (Don't Save) at the left under the text.
struct PromptButton
{
    std::string label;
    bool primary = false;
    bool leading = false;
};
struct PromptState
{
    std::string title;
    std::string message;
    std::vector<PromptButton> buttons; // in the caller's index order
    int hovered = -1, pressed = -1;    // button indices, -1 for none
    int focused = -1;                  // the default button (Return), drawn with a ring when it is not primary
};
constexpr float promptWidth = 460;
float promptHeight(const PromptState& state);
std::vector<SkRect> promptButtonRects(const PromptState& state, float width, float height); // one per button
void drawPrompt(SkCanvas& canvas, float width, float height, const PromptState& state);

// The panel look shared by the splash, the prompts, About and the Settings panel (UI-63): palette and metrics.
namespace panel
{
constexpr float cornerRadius = 12;                    // the window's corners
constexpr float buttonHeight = 28, buttonRadius = 7;  // a button's corners are a quarter of its height, at most buttonRadius
constexpr float titleSize = 15, textSize = 12.5f;     // prompt title, prompt message and button labels
constexpr SkColor backgroundTop = SkColorSetRGB(0x22, 0x22, 0x22), backgroundBottom = SkColorSetRGB(0x0b, 0x0b, 0x0b);
constexpr SkColor border = SkColorSetARGB(40, 255, 255, 255);
constexpr SkColor hairline = SkColorSetARGB(46, 255, 255, 255); // button edges and separators
constexpr SkColor title = SkColorSetRGB(0xf4, 0xf4, 0xf4);
constexpr SkColor text = SkColorSetRGB(0xd8, 0xd8, 0xd8);
constexpr SkColor message = SkColorSetRGB(0xa4, 0xa4, 0xa4);  // secondary text
constexpr SkColor dim = SkColorSetRGB(0x6e, 0x6e, 0x6e);      // hints and disabled text
constexpr SkColor alert = SkColorSetRGB(0xff, 0xa0, 0x5a);    // the one warm colour: failures, "press the new key"
constexpr SkColor button = SkColorSetRGB(0x2c, 0x2c, 0x2c), buttonHot = SkColorSetRGB(0x3a, 0x3a, 0x3a),
                  buttonDown = SkColorSetRGB(0x1c, 0x1c, 0x1c), buttonText = SkColorSetRGB(0xe8, 0xe8, 0xe8);
constexpr SkColor primary = SkColorSetRGB(0xec, 0xec, 0xec), primaryHot = SK_ColorWHITE,
                  primaryDown = SkColorSetRGB(0xc4, 0xc4, 0xc4), primaryText = SkColorSetRGB(0x11, 0x11, 0x11);
constexpr SkColor focusRing = SkColorSetARGB(150, 255, 255, 255);
constexpr SkColor well = SkColorSetARGB(110, 0, 0, 0);        // text fields
constexpr SkColor sidebar = SkColorSetARGB(64, 0, 0, 0);
constexpr SkColor separator = SkColorSetARGB(24, 255, 255, 255);
constexpr SkColor selection = SkColorSetARGB(30, 255, 255, 255); // the chosen sidebar row
constexpr SkColor scrollbar = SkColorSetARGB(70, 255, 255, 255);
} // namespace panel

// The panel background (black, lit slightly from the top-left, rounded like a macOS window) and its hairline border,
// drawn last. `bounds` may sit anywhere on the canvas.
void drawPanel(SkCanvas& canvas, const SkRect& bounds);
void drawPanelBorder(SkCanvas& canvas, const SkRect& bounds);
// The splash's ghost S in `box`, fading from topAlpha at its top to bottomAlpha at fadeBottom (white).
void drawGhostMark(SkCanvas& canvas, const SkRect& box, float fadeBottom, uint8_t topAlpha, uint8_t bottomAlpha);

// Panel controls: the primary button is white on black, the others dark with a hairline; hover lifts, press sinks.
struct ButtonLook
{
    bool primary = false, hot = false, down = false, focused = false, enabled = true;
};
void drawButtonFrame(SkCanvas& canvas, const SkRect& rect, const ButtonLook& look);
SkColor buttonTextColor(const ButtonLook& look);
void drawButton(SkCanvas& canvas, const SkRect& rect, const std::string& label, const ButtonLook& look,
                float textSize = panel::textSize);
// An on/off switch filling `rect` (a pill; the knob slides right and the track turns white when on).
void drawSwitch(SkCanvas& canvas, const SkRect& rect, bool on, bool hot, bool enabled = true);
// A text field's well.
void drawField(SkCanvas& canvas, const SkRect& rect);

// About Sonora (COM-8): the splash's panel with the wordmark, version, licence, the author's credit, an update row (COM-4),
// the supporters by tier (or an invitation when there are none), and four buttons at the bottom.
enum AboutButton { AboutWebsite, AboutGitHub, AboutClose, AboutSupport, AboutUpdates, aboutButtonCount };
inline constexpr std::array<const char*, aboutButtonCount> aboutButtonLabels{"Website", "GitHub", "Close", "Support on Ko-fi",
                                                                             "Check for Updates..."};
// The update row under the credit: the app's updater fills it (app/AboutWindow.cpp). The status wraps onto two lines at most.
struct AboutUpdateRow
{
    std::string label = aboutButtonLabels[AboutUpdates]; // or "Install and Restart..." when an update waits
    std::string status;                                  // "Checks daily · Stable channel", or why updates are off
    bool enabled = false;
};
struct AboutState
{
    std::string version; // "Version 0.1.2"
    std::string credit;  // "Created by ..."
    AboutUpdateRow updates;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups; // tier title, names
    std::string emptyText;           // shown when there are no supporters
    int hovered = -1, pressed = -1;  // AboutButton, -1 for none
};
constexpr float aboutWidth = 540;
float aboutHeight(const AboutState& state);
std::array<SkRect, aboutButtonCount> aboutButtonRects(const AboutState& state, float width, float height);
void drawAbout(SkCanvas& canvas, float width, float height, const AboutState& state);

// Activity indicator: four bars swinging like a level meter, centred in `box`.
void drawActivity(SkCanvas& canvas, const SkRect& box, double timeSeconds, SkColor color);

// Busy cursor: an arrow with a small activity badge. Logical size busyCursorSize, hot spot busyCursorHotSpot.
constexpr int busyCursorSize = 32;
constexpr int busyCursorHotSpot = 4;
void drawBusyCursor(SkCanvas& canvas, double timeSeconds);

// Scrub cursor (the Arrangement ruler's play-from-here area): a small speaker, black with a white rim, sound waves to
// its right. Logical size scrubCursorSize; the hot spot is the speaker's centre, so the click plays from under it.
constexpr int scrubCursorSize = 24;
constexpr int scrubCursorHotSpotX = 8, scrubCursorHotSpotY = 12;
void drawScrubCursor(SkCanvas& canvas);
// The Arrangement beat-time ruler's zoom cursor (drag up/down zooms, left/right scrolls): a magnifying glass, lens centre
// at the hot spot.
constexpr int zoomCursorSize = 24;
constexpr int zoomCursorHotSpotX = 9, zoomCursorHotSpotY = 9;
void drawZoomCursor(SkCanvas& canvas);

// SVG documents for the website and the repository (resources/brand).
std::string markSvg(bool onIcon);
std::string wordmarkSvg(SkColor color);

// Renders with Skia (CPU) into a JUCE ARGB image of pixelWidth x pixelHeight, the canvas pre-scaled by `scale`.
juce::Image renderImage(int pixelWidth, int pixelHeight, float scale, const std::function<void(SkCanvas&)>& draw);

} // namespace sonora::gfx::brand
