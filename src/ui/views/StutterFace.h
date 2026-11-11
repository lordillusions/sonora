#pragma once
// Face of Sonora Stutter (PLG-18), shared by the device chain and the VST3 window. Stutter is Sonora's own device (no
// reference device to match, AGENTS rule 7 does not apply), drawn in the chain's dark display style:
//
//   TRIGGER (five mode pads, momentary: hold one to stutter; Latch keeps the chosen mode on = the Hold control)
//   | SLICE display (the captured slice, the play head, the repeat count) over the 16-step PATTERN strip
//   | SLICE knobs (Rate or Length, Shrink, Spacing, Sync, Quantize, Smooth) | PITCH (Pitch, Ramp, Decay, Wide, Chance)
//   | OUT (Tone, Mix, Output)
//
// Pattern cells: a click sets the step to the selected mode (or back to Off), dragging paints, Alt/right-click clears.

#include "devices/native/Stutter.h"
#include "ui/Widget.h"

#include <functional>

namespace sonora::ui
{
class Knob;
class TextButton;

class StutterFace final : public Widget
{
public:
    static constexpr float kPreferredWidth = 960, kPreferredHeight = 176;
    static constexpr SkColor kModeColour[devices::Stutter::kModes] = {0xff3dd2ff, 0xffff6a9a, 0xff9b8cff, 0xffffb03d, 0xff5cff7a};

    explicit StutterFace(devices::Stutter* processor, std::function<void(Widget*, int)> tag = {});
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void tick() override;

    // Geometry (local coordinates), exposed for tests.
    Rect padArea(int index) const; // 0..kModes-1 the mode pads, kModes = Latch
    Rect displayArea() const;
    Rect stepArea(int step) const;
    const devices::Stutter::View& view() const { return view_; }

private:
    struct Column { float x = 0, w = 0; };
    Knob* knob(int parameter);
    void setParameter(int parameter, float value);
    int padAt(float x, float y) const;
    int stepAt(float x, float y) const;
    void paintPads(SkCanvas&, const Theme&);
    void paintDisplay(SkCanvas&, const Theme&);
    void paintPattern(SkCanvas&, const Theme&);

    devices::Stutter* processor_;
    std::function<void(Widget*, int)> tag_;
    Column pads_, display_, slice_, pitch_, out_;
    Knob *rate_ = nullptr, *length_ = nullptr, *shrink_ = nullptr, *spacing_ = nullptr, *quantize_ = nullptr, *smooth_ = nullptr;
    Knob *pitchKnob_ = nullptr, *ramp_ = nullptr, *decay_ = nullptr, *wide_ = nullptr, *chance_ = nullptr;
    Knob *tone_ = nullptr, *mix_ = nullptr, *output_ = nullptr;
    TextButton *sync_ = nullptr, *pattern_ = nullptr;
    devices::Stutter::View view_;
    int pressedPad_ = -1, paintValue_ = -1, lastPainted_ = -1;
    bool lastEngaged_ = false;
};
} // namespace sonora::ui
