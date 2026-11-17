#pragma once
// Face of Sonora Kickstract (PLG-17), on the ProFace kit like Sonora's other devices (PLG-17f: the house look: wordmark,
// graphite panels, big knobs that show their value, the help line). The device chain shows the compact layout; the
// VST3 window and the app's own window the full one.
//
// Full: the wordmark, the bands in words and LISTEN (Kick / Removed / Input) and Timeline on top; DETECT (Threshold,
// Retrigger, Input, Chop and the detector meter with its hit lamp); SPECTRUM (input grey, result per band colour,
// draggable split lines); LAST KICK (the captured hit: input, each band after its gate, the result's waveform, each
// band's gate envelope with draggable Length / Fade handles); LOW / MID / HIGH (On, Solo, Length, Fade, Gain); FINISH
// (Punch, Drive, Output); the help line.
// Compact: the two displays, the three bands (On, Solo, Length, Fade, Gain), Listen, Punch and Output.
//
// Band regions glow with their gate level while kicks play. Double-click a split line or handle: back to the default.

#include "dsp/DeviceSpectrum.h"
#include "devices/native/Kickstract.h"
#include "ui/views/ProFace.h"

#include <functional>
#include <string>

namespace sonora::ui
{
class KickstractFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFFFF6A3D;
    static constexpr SkColor kBandColour[devices::Kickstract::kBands] = {0xffff6a3d, 0xffffc53d, 0xff3dd2ff};
    static constexpr Size kFull{1180, 570}, kCompact{1100, 170};

    explicit KickstractFace(devices::Kickstract& device);
    devices::Kickstract& kickstract() { return kickstract_; }

    // Geometry in the widget's local coordinates (tests drive the handles through the root).
    Rect spectrumArea() const;
    Rect kickArea() const;
    float xForHz(float hz) const;
    float hzForX(float x) const;
    float xForMs(float ms) const; // kick display time axis
    // Handle under (x, y): 0 / 1 = the low / high split line, 10 + band = Length end, 20 + band = Fade end; -1 none.
    int handleAt(float x, float y) const;
    const devices::Kickstract::KickShape& kick() const { return kick_; }
    // PLG-17e: opens the timeline window (Input / Kick / Removed over time). The button shows only when this is set.
    std::function<void()> onOpenTimeline;
    std::string headline() const;
    Cursor cursorAt(float x, float y) const override;

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    bool animate() override;

private:
    // Design-grid geometry of the current layout.
    Rect spectrumRect() const;
    Rect kickRect() const;
    float designXForHz(float hz) const;
    float designHzForX(float x) const;
    float designXForMs(float ms) const;
    float msForDesignX(float x) const;
    int designHandleAt(float x, float y) const;
    float handleY(int band) const;
    float windowMs() const;
    float gateLevelAt(int band, float ms) const;
    void dragHandle(float x);
    void resetHandle(int handle);
    void paintSpectrum(SkCanvas&, bool compact);
    void paintKick(SkCanvas&, bool compact);
    void paintMeter(SkCanvas&, const Rect&);

    devices::Kickstract& kickstract_;
    dsp::DeviceSpectrum inSpectrum_, outSpectrum_;
    devices::Kickstract::KickShape kick_;
    float meter_ = 0, bandLevels_[devices::Kickstract::kBands]{};
    uint32_t triggers_ = 0;
    double lastTick_ = 0, flashUntil_ = 0;
    int drag_ = -1, lastHandle_ = -1;
};
} // namespace sonora::ui
