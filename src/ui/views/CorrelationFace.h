#pragma once
// Face of Sonora Correlation (PLG-24), on the ProFace kit: the device chain (compact), its own window and the VST3 editor
// (full).
//
// Full layout: the wordmark, Listen (the check modes light amber: the output is not the input), Freeze and Reset on top;
// the VERDICT in words (mono, mono-compatible, very wide, low end out of phase, out of phase...); the CORRELATION meter
// (the value big and coloured from red through amber to green, a needle, the held extremes); the SCOPE (Lissajous: mono
// is vertical, inverted is horizontal; or Polar: the same folded into a half disc), with auto or manual zoom; the BANDS
// (correlation of 31 third-octave bands over their level, the low end marked, held minimums; the pointer reads a band);
// the HISTORY of the last 8 s (all and low end); STEREO (balance, width, the mono sum, levels); the METER knobs.
//
// Compact layout: the scope, the value with the verdict, the meter bar and the bands, Listen, Integration, Hold, Freeze,
// Reset and the scope mode.

#include "devices/native/Correlation.h"
#include "ui/views/ProFace.h"

#include "include/core/SkPoint.h"

#include <array>
#include <string>
#include <vector>

namespace sonora::ui
{
class CorrelationFace final : public ProFace
{
public:
    using C = devices::Correlation;
    static constexpr SkColor kAccent = 0xFFA88BFF, kGood = 0xFF5BE38C, kWarn = 0xFFFFC14D, kBad = 0xFFFF6B6B, kLow = 0xFFFF9F43;
    static constexpr Size kFull{1120, 720}, kCompact{760, 170};
    static constexpr int kScopeRead = 4096; // newest samples drawn by the scope (four ages, the oldest faintest)

    explicit CorrelationFace(devices::Correlation& device);
    devices::Correlation& meter() { return meter_; }

    struct Verdict
    {
        std::string title, detail;
        SkColor colour = 0;
    };
    Verdict verdict() const;
    // -1 red (inverted) .. 0 amber (unrelated) .. +0.5 and up green.
    static SkColor colourFor(float correlation);
    float autoZoomDb() const { return autoZoomDb_; }
    float zoomDb() const; // the zoom the scope draws with now
    // The loud band (within 40 dB of the loudest) with the lowest correlation; -1 when none has signal.
    int worstBand(bool lowEndOnly = false) const;
    float correlationAbove(float hz) const; // energy-weighted correlation of the bands above hz (1 when none has signal)
    std::string bandReadout(int band) const;
    int bandAt(float designX, float designY) const; // the band under a design-grid point, -1 when none
    int scopePoints() const { return points_; }

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    void willEdit(int param) override;
    bool animate() override;

private:
    Rect bandsPlot() const;
    void paintScope(SkCanvas&, const Rect&, bool compact);
    void paintMeter(SkCanvas&, const Rect& track, bool labels);
    void paintBands(SkCanvas&, const Rect&, bool compact);
    void paintHistory(SkCanvas&, const Rect&);
    void paintStereo(SkCanvas&, const Rect&);
    void paintListenWarning(SkCanvas&, const Rect&, float textSize);
    void addListen(const Rect&, const std::vector<std::string>& labels, bool vertical, float gap, const std::string& tip);

    devices::Correlation& meter_;
    devices::Correlation::Reading reading_;
    std::array<devices::Correlation::Band, C::kBands> bands_{};
    std::vector<float> x_, y_;
    std::vector<SkPoint> pts_;
    int points_ = 0;
    uint64_t seen_ = ~0ull;
    float autoZoomDb_ = 0;
    bool bypassed_ = false;
};
} // namespace sonora::ui
