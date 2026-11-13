#pragma once
// Face of Sonora Stutter (PLG-18, PLG-23), on the ProFace kit. Shared by the device chain (compact), its own window and
// the VST3 editor (full).
//
// Full layout: the wordmark and what it is doing now (mode, speed, grains) on top; the display (INPUT: the last seconds
// recorded with the capture window, the read head and where the grains read; OUTPUT: what comes out with every grain
// drawn as its envelope, odd grains left and even ones right); the AUTO panel (AUTO, the voice light, the eight presets,
// Follow, Amount, Sensitivity and the preset's cycle as a timeline of coloured moves with the song's position); then the
// HOLD pad and Latch, the MOTION knob with the seven modes and what Slow and Fast do in the selected one, the GRAIN knobs
// (linked to Motion while Link is on, driven by a preset's move while AUTO plays) and the OUT knobs; a help line at the
// bottom explains the control under the pointer.
//
// Compact layout (the chain): the hold pad, AUTO, the preset (click: the next one), the display with the preset's
// timeline under it, Motion, the modes and Speed/Mix.

#include "devices/native/Stutter.h"
#include "ui/views/ProFace.h"

namespace sonora::ui
{
class StutterFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFFFF4F8B, kAccent2 = 0xFF8F7BFF;
    static constexpr Size kFull{1160, 758}, kCompact{700, 170};

    explicit StutterFace(devices::Stutter& device);
    devices::Stutter& stutter() { return stutter_; }
    const devices::Stutter::View& view() const { return view_; }

    // Words for the settings (shared with tests).
    static std::string speedWords(float speed);
    static std::string slowLine(int mode);
    static std::string fastLine(int mode);
    std::string statusLine() const;
    std::string autoLine() const; // AUTO: the preset, the bar, the move playing or the next one
    static SkColor modeColour(int mode);

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    void willEdit(int param) override;
    bool animate() override;

private:
    bool driven(int param) const;
    float shownValue(int param) const;
    void addGrainKnob(const Rect&, int param, std::string label, std::string tip);
    void paintDisplay(SkCanvas&, const Rect& input, const Rect& output, bool compact);
    void paintTimeline(SkCanvas&, const Rect&, bool compact);

    devices::Stutter& stutter_;
    devices::Stutter::View view_;
    Rect display_, input_, output_, timeline_;
};
} // namespace sonora::ui
