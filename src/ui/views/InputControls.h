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
    static constexpr float compactPitch = 17.5f;  // Arrangement header row pitch (the mixer column uses the same rows)
    explicit InputControls(int track, bool captions = false) : track_(track), captions_(captions) {}
    // Arrangement header rows that fit the track's height (1..5), like the reference: 5 = input type, input channel,
    // monitor, output type, output channel; 4 = input channel (a grip opens the type), monitor, output type and channel;
    // 3 = input, monitor, output; 2 = input, output; 1 = an empty box. Ignored with captions (Session).
    void setRowCount(int rows) { rowCount_ = rows < 1 ? 1 : rows > 5 ? 5 : rows; }
    int rowCount() const { return rowCount_; }
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
private:
    struct Rows
    {
        Rect sourceCaption, source, sourceGrip, channel, monitorCaption, outputCaption, output, spare;
        float monitorY = -1; // < 0: no monitor row
    };
    Rows rows() const;
    Rect monitorButton(int mode) const;
    void applySelection(model::InputSelection selection, const std::string& displayName);
    int track_;
    bool captions_;
    int rowCount_ = 5;
};

// "Main", "Group", "No Output"... as the output chooser shows a track's output.
std::string trackOutputName(const model::Track& track);
} // namespace sonora::ui
