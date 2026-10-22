#pragma once

#include "ui/Widget.h"

namespace sonora::devices { class DeviceProcessor; }

namespace sonora::ui
{
// Dedicated compact Delay/Reverb faces. Controls bind to the existing processors.
class EffectPanel : public Widget
{
public:
    EffectPanel(devices::DeviceProcessor* processor, int track, int device, bool nested);
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;
    float preferredWidth() const;

private:
    struct Item { Widget* widget; Rect rect; };
    Widget* control(int param, std::string label, Rect rect, bool knob = false, bool display = false, int choice = -1);
    void delayControls();
    void reverbControls();
    devices::DeviceProcessor* processor_;
    int track_, device_;
    bool nested_, delay_;
    std::vector<Item> items_;
};
} // namespace sonora::ui
