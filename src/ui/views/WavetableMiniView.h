#pragma once
// UI-83: Sonora Prism folded in the device chain, its default while the full dashboard floats in its own window. Read
// only: one row per oscillator (A, B, C) with its current waveform at the live table position (matrix modulation of the
// sounding voice applied, as the dashboard draws it), the position marker, the table name, whether it is on, and the
// output level. Everything comes from the processor's atomics and immutable tables (message thread); it repaints only
// when what it shows changed. A click calls onClick (the chain opens the floating editor).
#include "ui/Widget.h"
#include "devices/native/WavetableSynth.h"

#include <algorithm>
#include <array>
#include <functional>

namespace sonora::ui
{
class WavetableMiniView final : public Widget
{
public:
    static constexpr float kWidth = 236;
    explicit WavetableMiniView(devices::WavetableSynth* processor) : processor_(processor) {}
    std::function<void()> onClick;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override {}
    void mouseUp(const MouseEvent&) override;
    void tick() override;
    // Geometry and live values (local coordinates); exposed for tests.
    Rect oscillatorRow(int oscillator) const;
    Rect oscillatorPlot(int oscillator) const;
    Rect meterBar(int channel) const;
    // Where the position marker sits across the oscillator's table or sample, 0..1 (live modulation applied).
    float markerPosition(int oscillator) const;
    float meterLevel(int channel) const { return meter_[(size_t) std::clamp(channel, 0, 1)]; }

private:
    float liveAmount(int destination) const;
    float livePosition(int oscillator) const;
    float signature() const;
    devices::WavetableSynth* processor_;
    devices::WavetableSynth::LiveState live_{};
    std::array<float, 2> meter_{};
    float signature_ = -1;
};
} // namespace sonora::ui
