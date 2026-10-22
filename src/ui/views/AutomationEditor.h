#pragma once

#include "core/Model.h"
#include "ui/Widget.h"

namespace sonora::ui
{

// One arrangement automation lane with a target selector in its right-hand header (UI-5).
class AutomationEditor : public Widget
{
public:
    explicit AutomationEditor(int track) : track_(track) {}
    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;

private:
    struct Target
    {
        int id = -1;
        std::string name, eventType = "FloatEvent";
        double min = 0, max = 1, base = 0;
        bool stepped = false;
        std::string group;
    };
    std::vector<Target> targets() const;
    model::AutomationEnvelope envelope(const Target& target) const;
    Rect plot() const;
    float beatX(double beat) const;
    float valueY(double value, const Target& target) const;
    void commit();
    void paintCell(const MouseEvent& e);
    Rect chooserBounds(bool device) const;
    void showChooser(bool device);

    double lastCell_ = -1;
    bool drawing_ = false; // Draw Mode: dragging paints a stepped value per grid cell
    int track_, targetId_ = -1, selectedPoint_ = -1;
    bool dragging_ = false, changed_ = false;
    Target dragTarget_;
    model::AutomationEnvelope preview_;
};

} // namespace sonora::ui
