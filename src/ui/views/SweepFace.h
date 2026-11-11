#pragma once
// Face of Sonora Sweep (PLG-19), shared by the device chain and the VST3 window. Sonora's own device (no reference face):
//
//   SOURCE (Noise / Tone / Sample, Up / Down, Length, Load Sample... and the sample's name) | SWEEP (the filter cutoff,
//   pitch and level over the sweep, gate marks, the play head while it sounds) | FILTER (Low, High, Resonance) | MOTION
//   (Pitch, Curve, Swell, Width) | SOUND (Detune, Rhythm, Tail, Reverse, Output)

#include "devices/native/Sweep.h"
#include "ui/Widget.h"

#include <functional>

namespace sonora::ui
{
class Knob;
class TextButton;

class SweepFace final : public Widget
{
public:
    static constexpr float kPreferredWidth = 1000, kPreferredHeight = 176;
    static constexpr SkColor kCutoff = 0xff3dd2ff, kPitch = 0xffffb03d, kLevel = 0xffeef1f4;

    explicit SweepFace(devices::Sweep* processor, std::function<void(Widget*, int)> tag = {});
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

    // "Load Sample...": whoever shows the face opens a file chooser and loads it (the app through an undoable device edit, the VST3
    // directly). Unset: the button is disabled.
    std::function<void()> onLoadSample;
    Rect curveArea() const;
    TextButton* loadButton() const { return load_; }

private:
    struct Column { float x = 0, w = 0; };
    Knob* knob(int parameter);
    TextButton* choice(const char* label, int parameter, int value);

    devices::Sweep* processor_;
    std::function<void(Widget*, int)> tag_;
    Column source_, curve_, filter_, motion_, sound_;
    Knob *length_ = nullptr, *low_ = nullptr, *high_ = nullptr, *resonance_ = nullptr, *pitch_ = nullptr, *curveKnob_ = nullptr,
         *swell_ = nullptr, *width_ = nullptr, *detune_ = nullptr, *rhythm_ = nullptr, *tail_ = nullptr, *output_ = nullptr;
    TextButton *sources_[3]{}, *directions_[2]{}, *reverse_ = nullptr, *load_ = nullptr;
    float progress_ = -1;
    std::string sampleShown_;
};
} // namespace sonora::ui
