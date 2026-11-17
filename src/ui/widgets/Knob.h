#pragma once
// Live-style rotary ("poti"): label on top, arc + needle, value text below.
// Drag vertically to change (Shift = fine), double-click resets to default.
// UI-93: a click on the value text (or a number typed while the knob is selected) opens a field to type the value;
// drags, the wheel and the arrow keys move in the steps of the unit the value shows (ui/ValueEntry.h).

#include "ui/ValueEntry.h"
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
        ValueUnit unit = ValueUnit::None;    // the parameter's unit when the value text alone cannot tell (UI-93)
    };

    Knob(std::string label, Source source) : label_(std::move(label)), src_(std::move(source)) {}
    const Source& source() const { return src_; } // tests and owners that set the value directly

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent&) override;
    bool wantsKey(const KeyEvent&) const override;

    // UI-93. Where the value is printed (the field opens there); empty: no click opens it. Subclasses that draw the value
    // elsewhere override it.
    virtual Rect valueBounds() const;
    // The text of a value as this knob prints it (default: the source's format). The unit law reads it.
    virtual std::string formatValue(float value) const;
    bool typeable = true; // false: no value field (choosers that only look like knobs)
    // The unit law of this knob's value text, and the field (tests call it directly).
    ValueLaw valueLaw() const;
    void openValueEntry(uint32_t firstChar = 0);
    void applyTypedValue(const std::string& text);

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
    float deviceStroke = 3.0f; // Device: ring thickness in knob units (Auto Filter's large Freq knob is thinner: 2.45)
    // Device: the ring leaves a slot of +-markerGap degrees at the marker, and the triangle's width in knob units (DEV-30:
    // the reference's Channel EQ capture shows a 16-degree slot and a 9.5-unit triangle; the UI-42 faces keep 0 / 7).
    float markerGap = 0.0f, markerWidth = 7.0f;
    std::function<void()> onLabelClick; // Ring: a click on the label (e.g. Width <-> Mid/Side) instead of a drag
    void setLabel(std::string label) { label_ = std::move(label); }
    const std::string& label() const { return label_; }
    bool showLabel = true;
    bool showValue = true;
    std::function<bool()> isEnabled; // Compact / Device: drawn without the value arc when false (Device also ignores input)
    // Device: the parameter follows a rack macro (DEV-30, as the reference draws it): grey ring and needle, no value arc,
    // a light marker triangle and a green dot at the upper left; hand edits are ignored (the macro owns the value).
    std::function<bool()> mapped;
    std::function<void()> onRightClick;

private:
    float toNorm(float v) const;
    float fromNorm(float n) const;

    std::string label_;
    Source src_;
    bool canType() const;
    float current() const { return src_.get ? src_.get() : src_.defaultValue; }

    float dragStartNorm_ = 0, dragStartY_ = 0;
    bool labelPressed_ = false;
    bool pressInValue_ = false, moved_ = false; // a press on the value text that has not dragged yet opens the field
    ValueLaw dragLaw_;
    float wheelNorm_ = 0, wheelSet_ = 0;        // the wheel's unrounded position, kept while nothing else moves the value
    bool wheeling_ = false;
};

} // namespace sonora::ui
