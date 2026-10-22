#pragma once
// Track input/channel/monitor selectors (ENG-9b), shared by Session and Arrangement.
#include "core/InputRouting.h"
#include "ui/Widget.h"

namespace sonora::ui
{
class InputControls : public Widget
{
public:
    static constexpr float preferredHeight = 80;  // compact (Arrangement header)
    static constexpr float sessionHeight = 115;   // with captions (Session mixer strip)
    explicit InputControls(int track, bool captions = false) : track_(track), captions_(captions) {}
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
private:
    struct Rows
    {
        Rect sourceCaption, source, channel, monitorCaption, outputCaption, output, spare;
        float monitorY = 0;
    };
    Rows rows() const;
    Rect monitorButton(int mode) const;
    void applySelection(model::InputSelection selection, const std::string& displayName);
    int track_;
    bool captions_;
};
} // namespace sonora::ui
