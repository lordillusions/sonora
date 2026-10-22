#pragma once
// Sonora brand drawing (UI-42): the "S" mark, the wordmark, the app icon, the launch splash and the
// busy indicator / cursor. Everything is grayscale and drawn from the geometry in Brand.cpp, so the
// app, tools/brand_render (icon PNGs, SVGs) and the website share one source. No font files are
// involved: the wordmark letters are stroked paths.

#include "include/core/SkColor.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"

#include <juce_graphics/juce_graphics.h>

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

// Activity indicator: four bars swinging like a level meter, centred in `box`.
void drawActivity(SkCanvas& canvas, const SkRect& box, double timeSeconds, SkColor color);

// Busy cursor: an arrow with a small activity badge. Logical size busyCursorSize, hot spot busyCursorHotSpot.
constexpr int busyCursorSize = 32;
constexpr int busyCursorHotSpot = 4;
void drawBusyCursor(SkCanvas& canvas, double timeSeconds);

// SVG documents for the website and the repository (resources/brand).
std::string markSvg(bool onIcon);
std::string wordmarkSvg(SkColor color);

// Renders with Skia (CPU) into a JUCE ARGB image of pixelWidth x pixelHeight, the canvas pre-scaled by `scale`.
juce::Image renderImage(int pixelWidth, int pixelHeight, float scale, const std::function<void(SkCanvas&)>& draw);

} // namespace sonora::gfx::brand
