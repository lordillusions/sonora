#pragma once

#include "ui/Widget.h"
namespace sonora::devices { class DeviceProcessor; }
namespace sonora::ui
{
class PluginPanel : public Widget
{
public:
    PluginPanel(devices::DeviceProcessor* processor, int track, int deviceId, int parentIndex = -1, int chain = -1);
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
private:
    Rect pad() const;
    std::string assignmentKey() const;
    devices::DeviceProcessor* processor_;
    int track_, deviceId_, parentIndex_, chain_;
    bool dragging_ = false;
    float x_ = 0, y_ = 0;
};
} // namespace sonora::ui
