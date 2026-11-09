#pragma once
// Face of Sonora Kickstract (PLG-17), shared by the device chain and the VST3 window. Kickstract is Sonora's own device
// (no reference device to match), so the face is its own design in the chain's dark display style:
//
//   DETECT | SPECTRUM (input grey, result per band colour, draggable split lines) | LAST KICK (the captured hit: input,
//   each band after its gate, the result's waveform, each band's gate envelope with draggable Length / Fade handles)
//   | LOW / MID / HIGH strips (On, Solo, Length, Fade, Gain) | FINISH (Punch, Drive, Output, Listen)
//
// Band regions glow with their gate level while kicks play. Double-click a split line or handle: back to the default.

#include "dsp/DeviceSpectrum.h"
#include "devices/native/Kickstract.h"
#include "ui/Widget.h"

#include <functional>
#include <vector>

namespace sonora::ui
{
class Knob;
class TextButton;

class KickstractFace final : public Widget
{
public:
    static constexpr float kPreferredWidth = 1090, kPreferredHeight = 176;
    static constexpr SkColor kBandColour[devices::Kickstract::kBands] = {0xffff6a3d, 0xffffc53d, 0xff3dd2ff};

    explicit KickstractFace(devices::Kickstract* processor, std::function<void(Widget*, int)> tag = {});
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseMove(const MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const MouseEvent&) override;
    Cursor cursorAt(float x, float y) const override;
    void tick() override;

    // Geometry (local coordinates), exposed for tests.
    Rect spectrumArea() const;
    Rect kickArea() const;
    float xForHz(float hz) const;
    float hzForX(float x) const;
    // Handle under (x, y): 0 / 1 = the low / high split line, 10 + band = Length end, 20 + band = Fade end; -1 none.
    int handleAt(float x, float y) const;
    float xForMs(float ms) const; // kick display time axis
    const devices::Kickstract::KickShape& kick() const { return kick_; }

private:
    struct Column { float x = 0, w = 0; };
    Knob* knob(int parameter, const std::string& label = {});
    TextButton* toggle(const std::string& label, int parameter, int style);
    void setParameter(int parameter, float value);
    float windowMs() const;
    float gateLevelAt(int band, float ms) const;
    void paintSpectrum(SkCanvas&, const Theme&);
    void paintKick(SkCanvas&, const Theme&);
    void paintDetector(SkCanvas&, const Theme&);
    void paintColumnTitles(SkCanvas&, const Theme&);

    devices::Kickstract* processor_;
    std::function<void(Widget*, int)> tag_;
    Column detect_, spectrum_, kickColumn_, bands_[devices::Kickstract::kBands], finish_;
    Knob *threshold_ = nullptr, *retrigger_ = nullptr, *input_ = nullptr, *punch_ = nullptr, *drive_ = nullptr, *output_ = nullptr;
    TextButton* chop_ = nullptr;
    Knob* bandKnobs_[devices::Kickstract::kBands][3]{};
    TextButton *bandOn_[devices::Kickstract::kBands]{}, *bandSolo_[devices::Kickstract::kBands]{};
    TextButton* listen_[3]{};

    dsp::DeviceSpectrum inSpectrum_, outSpectrum_;
    devices::Kickstract::KickShape kick_;
    float meter_ = 0, bandLevels_[devices::Kickstract::kBands]{};
    uint32_t triggers_ = 0;
    double lastTick_ = 0, flashUntil_ = 0;
    int drag_ = -1, hover_ = -1;
};
} // namespace sonora::ui
