#pragma once
// Live-style rotary ("poti"): label on top, arc + needle, value text below.
// Drag vertically to change (Shift = fine), double-click resets to default.

#include "ui/Widget.h"

namespace sonora::ui
{

class Knob : public Widget
{
public:
    struct Source
    {
        std::function<float()> get;          // plain value
        std::function<void(float)> set;      // plain value
        float minValue = 0, maxValue = 1, defaultValue = 0;
        bool bipolar = false;                // arc drawn from the centre (pan, gain in dB)
        std::function<std::string(float)> format; // value text; default "%.2f"
        bool logarithmic = false;            // frequency knobs
    };

    Knob(std::string label, Source source) : label_(std::move(label)), src_(std::move(source)) {}

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent&) override;

    // Arc: device-panel knob (label on top, value text below). Poti: Live's mixer pan/send ring with
    // a pointer; the label (A, B) sits inside the open bottom of the ring. Compact: Simpler-style knob with
    // the label on top (left aligned), a small dial and the value to the right of the dial. Ring: the reference's
    // Utility knob (label, a triangle at `marker`, a thick dark ring and needle, the cyan value arc from the marker,
    // value text); proportions follow its 51-unit-tall layout. Device: the reference's standard device knob as its
    // EQ / dynamics faces show it (UI-42): 36-unit ring, 3 thick, label row above and value row below at 24.5 / 25
    // units from the ring centre in a 63.5-unit-tall layout (71.5 with the `markerTriangle` above the ring).
    enum class Style { Arc, Poti, Compact, Ring, Device };
    Style style = Style::Arc;
    float fontSize = 0; // Ring: label / value size (0 == the theme's small font size)
    float marker = -1; // Ring / Device: normalized origin of the value arc, marked by a triangle; < 0 = arc from the start
    bool markerTriangle = true; // Device: draw the triangle above the ring (the reference's EQ gain), or only start the arc there
    std::function<void()> onLabelClick; // Ring: a click on the label (e.g. Width <-> Mid/Side) instead of a drag
    void setLabel(std::string label) { label_ = std::move(label); }
    bool showLabel = true;
    bool showValue = true;
    std::function<bool()> isEnabled; // Compact / Device: drawn without the value arc when false (Device also ignores input)
    std::function<void()> onRightClick;

private:
    float toNorm(float v) const;
    float fromNorm(float n) const;

    std::string label_;
    Source src_;
    float dragStartNorm_ = 0, dragStartY_ = 0;
    bool labelPressed_ = false;
};

} // namespace sonora::ui
