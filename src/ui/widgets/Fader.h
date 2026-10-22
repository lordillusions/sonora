#pragma once
// Session-mixer volume fader with a stereo peak meter next to it (Live draws the volume
// handle as a small triangle on the meter scale).

#include "ui/Widget.h"

namespace sonora::ui
{

class Fader : public Widget
{
public:
    std::function<float()> getGain;      // linear
    std::function<void(float)> setGain;  // linear
    std::function<float()> getPeakL, getPeakR;

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    void tick() override;
    bool keyPressed(const KeyEvent&) override;
    Rect meterRect() const;

    // Mapping used for both handle and meter (tested in tests/test_ui.cpp).
    static float gainToPosition(float gain); // 0 (bottom) .. 1 (top, +6 dB)
    static float positionToGain(float pos);

private:
    float dragStartPos_ = 0, dragStartY_ = 0;
    float shownL_ = 0, shownR_ = 0; // decaying meter values
};

} // namespace sonora::ui
