#pragma once
#include "ui/Widget.h"

namespace sonora::ui
{
class DetailView;
class StatusBar : public Widget
{
public:
    DetailView* detailView = nullptr;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    void tick() override;
private:
    Rect navigationBounds() const;
    Rect overviewBounds() const;
    Rect mixerBounds() const;
    void navigate(float x);
    int logIndex_ = -1;
    bool dragging_ = false;
};
} // namespace sonora::ui
