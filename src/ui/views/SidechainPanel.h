#pragma once
// PLG-22: the Sidechain section the reference shows on the left of every device that can listen to another track
// (plug-ins with a sidechain input, Sonora Hush, Sonora Phase Align): the source track, where it is tapped (Pre FX /
// Post FX / Post Mixer), Gain, Mix and Mute. The device box's small arrow shows and hides it.
//
// Source, tap and Mute are device edits (one undo step each, the device is rebuilt: a plug-in's sidechain bus is only
// enabled while a track is routed). Gain and Mix change the running device live (DeviceProcessor::setSidechainLevels)
// and the model (UiContext::setDeviceSidechainLevels), like the reference's knobs.

#include "ui/Widget.h"

namespace sonora::devices { class DeviceProcessor; }
namespace sonora::model { struct Sidechain; }

namespace sonora::ui
{
class Knob;

class SidechainPanel final : public Widget
{
public:
    static constexpr float kWidth = 122, kArrowWidth = 11;
    SidechainPanel(devices::DeviceProcessor* processor, int track, int device);
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void tick() override;
    Rect meterBox() const { return {4, 57, width() - 8, 5}; }
    float meterLevel() const { return meter_; } // linear peak, decaying

    Rect sourceBox() const { return {4, 18, width() - 8, 16}; }
    Rect tapBox() const { return {4, 38, width() - 8, 16}; }
    Rect muteBox() const { return {4, std::max(60.0f, height() - 20), width() - 8, 16}; }
    std::string sourceLabel() const;
    std::string tapLabel() const;
    // The menus' choices, as the panel offers them (tests run them without a popup).
    struct Choice { std::string label; std::function<void()> run; bool ticked = false; };
    std::vector<Choice> sourceChoices();
    std::vector<Choice> tapChoices();
    void toggleMute();

private:
    const model::Sidechain* sidechain() const;
    void edit(std::function<void(model::Sidechain&)> change, std::string label);
    void setLevels(float gain, float mix);
    static void show(std::vector<Choice> choices);

    devices::DeviceProcessor* processor_;
    int track_, device_;
    Knob *gain_ = nullptr, *mix_ = nullptr;
    float meter_ = 0;
    double lastTick_ = 0;
};
} // namespace sonora::ui
