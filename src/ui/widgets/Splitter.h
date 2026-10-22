#pragma once
// A draggable divider between two panels (Live lets every panel boundary be resized).
// The owner supplies the current size; dragging reports the new size, clamped by the owner.

#include "ui/Widget.h"

#include <functional>

namespace sonora::ui
{

class Splitter : public Widget
{
public:
    enum class Orientation
    {
        Vertical,   // a vertical bar: dragging moves it left/right
        Horizontal  // a horizontal bar: dragging moves it up/down
    };

    explicit Splitter(Orientation o) : orientation_(o) {}

    // Size of the panel the splitter resizes, and the sign of the drag direction that grows it.
    std::function<float()> getSize;
    std::function<void(float)> setSize;
    std::function<void()> finished;
    float growDirection = 1.0f; // +1: dragging right/down grows the panel, -1: dragging left/up grows it

    Cursor cursorAt(float, float) const override
    {
        return orientation_ == Orientation::Vertical ? Cursor::ResizeHorizontal : Cursor::ResizeVertical;
    }
    void paint(SkCanvas&, const Theme&) override {}
    void mouseDown(const MouseEvent& e) override
    {
        start_ = orientation_ == Orientation::Vertical ? e.x : e.y;
        startSize_ = getSize ? getSize() : 0.0f;
    }
    void mouseDrag(const MouseEvent& e) override
    {
        const float delta = (orientation_ == Orientation::Vertical ? e.x : e.y) - start_;
        if (setSize)
            setSize(startSize_ + growDirection * delta);
    }
    void mouseUp(const MouseEvent&) override
    {
        if (finished)
            finished();
    }

private:
    Orientation orientation_;
    float start_ = 0, startSize_ = 0;
};

} // namespace sonora::ui
