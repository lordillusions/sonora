#pragma once
// Face of Sonora Phase Align (PLG-21, PLG-23), on the ProFace kit: the device chain (compact), its own window and the VST3
// editor (full).
//
// Full layout: the wordmark and the kick's source on top; the VERDICT in words (how much low end it adds on every kick, or
// why there is nothing to align and the bass passes untouched); the PHASE dial (applied angle, target, coherence) with
// Auto / Hold / Manual; the LAST HIT (kick and bass, each scaled to be seen, and the kick + bass sum as it was against
// aligned); HITS (per hit the gain the best angle could give and the one it gets); the ALIGN, DETECT and OUT knobs.
//
// Compact layout: the dial, the verdict and the last hit, the modes, Listen and four knobs.

#include "devices/native/PhaseAlign.h"
#include "ui/views/ProFace.h"

#include <functional>
#include <string>

namespace sonora::ui
{
class PhaseAlignFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFF3DD2FF, kKick = 0xFFFF8A3D, kBefore = 0xFF7D8590, kGood = 0xFF5BE38C,
                             kWarn = 0xFFFFC14D, kBad = 0xFFFF6B6B;
    static constexpr Size kFull{1100, 600}, kCompact{760, 170};

    explicit PhaseAlignFace(devices::PhaseAlign& device);
    devices::PhaseAlign& phase() { return phase_; }

    // The sidechain source shown on top ("" = none routed) and what a click on it does (unset: the host routes it, VST3).
    std::function<std::string()> sidechainName;
    std::function<void()> onChooseSidechain;

    std::string verdictTitle() const;
    std::string verdictDetail() const;
    SkColor verdictColour() const;
    const devices::PhaseAlign::Shape& shape() const { return shape_; }

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    bool animate() override;

private:
    std::string kickLabel() const;
    void paintDial(SkCanvas&, float cx, float cy, float radius, bool compact);
    void paintHit(SkCanvas&, const Rect& waves, const Rect& sum, bool compact);
    void paintHistory(SkCanvas&, const Rect&);

    devices::PhaseAlign& phase_;
    devices::PhaseAlign::Shape shape_;
    uint32_t hits_ = 0;
    float angle_ = 0, target_ = 0, mix_ = 0;
    int verdict_ = -1;
};
} // namespace sonora::ui
