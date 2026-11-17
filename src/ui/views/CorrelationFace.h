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
//
// Measure Kick x Bass (PLG-24c) swaps the stereo panels for the kick and the bass: the KICK x BASS reading (the value
// big, its meaning in a word, the verdict with what to do: flip the bass, shift it, or nothing), ONE BEAT from the kick's
// onset (both low ends drawn over each other, where they meet coloured by whether they add up or cancel, the 16th grid),
// WHERE THEY MEET (shared share, the sum there, the bass inverted, the best shift, levels, Kick From), the HISTORY, the LOW
// BANDS with their values, and Integration with Sync (a note value at the song's tempo). Listen: Bass / Bass inverted (the
// kick from the sidechain) or Stereo / Mono / Mono with the bass inverted (Kick From Left, the pan trick on a group).

#include "devices/native/Correlation.h"
#include "ui/views/ProFace.h"

#include "include/core/SkPoint.h"

#include <array>
#include <string>
#include <utility>
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
    static constexpr int kBeatRead = 720;   // points of the beat view
    static constexpr float kBeatPreMs = 10.0f;

    explicit CorrelationFace(devices::Correlation& device);
    devices::Correlation& meter() { return meter_; }

    struct Verdict
    {
        std::string title, detail;
        SkColor colour = 0;
    };
    Verdict verdict() const;
    // Kick x Bass: the reading in one word ("in phase", "out of phase", "unrelated", "take turns"...), empty in Stereo.
    std::string pairWord() const;
    bool kickBass() const { return pair_; }
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
    int beatPoints() const { return beatPoints_; }
    // The bands drawn now: the first and how many (Kick x Bass shows the low ones, 25 - 250 Hz).
    std::pair<int, int> bandSpan() const;

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
    Verdict pairVerdict() const;
    void paintScope(SkCanvas&, const Rect&, bool compact);
    void paintBeat(SkCanvas&, const Rect&, bool compact);
    void paintMeter(SkCanvas&, const Rect& track, bool labels);
    void paintBands(SkCanvas&, const Rect&, bool compact);
    void paintBandCallout(SkCanvas&);
    void paintHistory(SkCanvas&, const Rect&);
    void paintStereo(SkCanvas&, const Rect&);
    void paintMeet(SkCanvas&, const Rect&);
    void paintListenWarning(SkCanvas&, const Rect&, float textSize);
    void paintFullStereo(SkCanvas&, const Verdict&);
    void paintFullPair(SkCanvas&, const Verdict&);
    void addListen(const Rect&, const std::vector<std::pair<std::string, int>>& cells, bool vertical, float gap, const std::string& tip);
    void addIntegration(const Rect& knob, const Rect& sync, bool compact);

    devices::Correlation& meter_;
    devices::Correlation::Reading reading_;
    std::array<devices::Correlation::Band, C::kBands> bands_{};
    std::vector<float> x_, y_;
    std::vector<SkPoint> pts_;
    int points_ = 0;
    uint64_t seen_ = ~0ull;
    float autoZoomDb_ = 0;
    bool bypassed_ = false;
    // Kick x Bass
    bool pair_ = false, kickLeft_ = false, sync_ = false; // what the layout was built for
    std::vector<float> beatKick_, beatBass_, beatWeight_;
    int beatPoints_ = 0;
    float beatMs_ = 1.0f;
    bool beatFromOnset_ = false;
};
} // namespace sonora::ui
