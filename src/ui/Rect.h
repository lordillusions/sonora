#pragma once
// Float rectangle with the slicing helpers used for layout (JUCE-style removeFromTop...).

#include "include/core/SkRect.h"

#include <algorithm>

namespace sonora::ui
{

struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    float centreX() const { return x + w * 0.5f; }
    float centreY() const { return y + h * 0.5f; }
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool isEmpty() const { return w <= 0 || h <= 0; }

    SkRect sk() const { return SkRect::MakeXYWH(x, y, w, h); }

    Rect reduced(float d) const { return {x + d, y + d, std::max(0.0f, w - 2 * d), std::max(0.0f, h - 2 * d)}; }
    Rect reduced(float dx, float dy) const
    {
        return {x + dx, y + dy, std::max(0.0f, w - 2 * dx), std::max(0.0f, h - 2 * dy)};
    }
    Rect translated(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    Rect withHeight(float nh) const { return {x, y, w, nh}; }
    Rect withWidth(float nw) const { return {x, y, nw, h}; }

    Rect removeFromTop(float a)
    {
        a = std::min(a, h);
        Rect r{x, y, w, a};
        y += a;
        h -= a;
        return r;
    }
    Rect removeFromBottom(float a)
    {
        a = std::min(a, h);
        h -= a;
        return {x, y + h, w, a};
    }
    Rect removeFromLeft(float a)
    {
        a = std::min(a, w);
        Rect r{x, y, a, h};
        x += a;
        w -= a;
        return r;
    }
    Rect removeFromRight(float a)
    {
        a = std::min(a, w);
        w -= a;
        return {x + w, y, a, h};
    }
};

} // namespace sonora::ui
