#pragma once
// Face of Sonora Phase Align (PLG-21), shared by the device chain and the VST3 window. Sonora's own device (no reference
// face to match), in the chain's dark display style:
//
//   PHASE (a dial: the applied angle, the target, coherence as the ring, Auto / Hold / Manual) | LAST HIT (the kick, the
//   bass before and after over the analysis window, the low-end gain) | HITS (one dot per hit: the angle it asked for,
//   brighter when coherent; hollow = ignored) | ALIGN knobs (Angle, Speed, Crossover) | DETECT knobs (Threshold, Window,
//   Min. Coherence) | OUT (Output, Listen Bass / Sum / Orig.)
//
// The title row shows where the sidechain comes from; in the app a click opens the "Sidechain from" track chooser.

#include "devices/native/PhaseAlign.h"
#include "ui/Widget.h"

#include <functional>
#include <string>

namespace sonora::ui
{
class Knob;
class TextButton;

class PhaseAlignFace final : public Widget
{
public:
    static constexpr float kPreferredWidth = 900, kPreferredHeight = 176;
    static constexpr SkColor kKick = 0xffff8a3d, kBefore = 0xff7d8590, kAfter = 0xff3dd2ff;

    explicit PhaseAlignFace(devices::PhaseAlign* processor, std::function<void(Widget*, int)> tag = {});
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

    // The sidechain source shown in the title row ("" = none routed) and what a click on it does (unset: not clickable).
    std::function<std::string()> sidechainName;
    std::function<void()> onChooseSidechain;
    TextButton* sidechainButton() const { return sidechain_; }

    Rect dialArea() const;
    Rect hitArea() const;
    Rect historyArea() const;
    const devices::PhaseAlign::Shape& shape() const { return shape_; }

private:
    struct Column { float x = 0, w = 0; };
    Knob* knob(int parameter);
    void paintDial(SkCanvas&, const Theme&);
    void paintHit(SkCanvas&, const Theme&);
    void paintHistory(SkCanvas&, const Theme&);

    devices::PhaseAlign* processor_;
    std::function<void(Widget*, int)> tag_;
    Column phase_, hit_, history_, align_, detect_, out_;
    Knob *angle_ = nullptr, *speed_ = nullptr, *crossover_ = nullptr, *threshold_ = nullptr, *window_ = nullptr,
         *coherence_ = nullptr, *output_ = nullptr;
    TextButton* modes_[3]{};
    TextButton* listen_[3]{};
    TextButton* sidechain_ = nullptr;
    devices::PhaseAlign::Shape shape_;
    uint32_t hits_ = 0;
    float shownAngle_ = 0;
    std::string shownSource_;
};
} // namespace sonora::ui
