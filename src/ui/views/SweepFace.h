#pragma once
// Face of Sonora Sweep (PLG-19, PLG-23), on the ProFace kit: the device chain (compact), its own window and the VST3
// editor (full).
//
// Full layout (PLG-19d): what it makes (riser or downlifter, length, seconds at the tempo), Save WAV... and Preview on
// top; TYPE (ready made sweeps in two groups, SWELL and SYNTH, one click) and SAMPLE (the loaded file, its waveform with
// the Region's two handles; drop one from the Browser or the Finder anywhere on the face) on the left; the display (the
// sweep itself, rendered, with the filter, pitch and level curves over the bars and the drop marked; drag it out to get
// the WAV); SOURCE and Up/Down; LENGTH; SWELL (the character pad: Hall / Shimmer / Bloom / Grain, Decay, Damp, Hit,
// Reverb Swell / Reversed Sample, Fit, Reverse) and FILTER; PITCH, SHAPE and OUT; the help line.
//
// Compact layout: the display, the sources, Up/Down, Preview, Length, Swell, High and Level.

#include "devices/native/Sweep.h"
#include "ui/views/ProFace.h"

#include <functional>
#include <string>
#include <vector>

namespace sonora::ui
{
class SweepFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFFFFB13B, kCutoff = 0xFF4FD1FF, kPitch = 0xFFB59BFF, kLevel = 0xFFEEF1F4;
    static constexpr Size kFull{1180, 760}, kCompact{720, 170};

    explicit SweepFace(devices::Sweep& device);
    devices::Sweep& sweep() { return sweep_; }

    // A sample was dropped on the face (the app: an undoable device edit; the VST3 loads it directly).
    std::function<void(const std::string& path)> onSampleDropped;
    // "Choose..." in the SAMPLE panel: whoever shows the face opens a file chooser. Unset: the button is hidden.
    std::function<void()> onChooseSample;
    // "Save WAV..." (unset: hidden) and dragging the display out (the app: a Browser-style sample drag onto a track; the
    // VST3: a file drag to the host). Both render with Sweep::renderToFile.
    std::function<void()> onSaveWav, onDragOut;

    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    std::string headline() const; // "Riser · 4 Bars · 6.9 s at 139 BPM"
    // The rendered sweep shown in the display (min/max bins); renders now when the settings changed.
    const std::vector<float>& renderedMax();
    void renderNow();

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    bool animate() override;

private:
    uint64_t signature() const;
    void addSourceSegments(const Rect& row1, const Rect& row2, bool compact);
    void paintDisplay(SkCanvas&, const Rect&, bool compact);
    void paintCharacterPad(SkCanvas&);
    void paintRegion(SkCanvas&);
    void setCharacterFrom(float x, float y);
    void setRegionFrom(float x, bool start);

    devices::Sweep& sweep_;
    std::vector<float> renderLo_, renderHi_;
    uint64_t renderedSignature_ = 0, pendingSignature_ = 0;
    int pendingTicks_ = 0;
    float progress_ = -1;
    Rect display_;
    // PLG-19d: the character pad and the Region strip (design grid), the source overview for the strip, a drag-out
    static constexpr Rect kPad{248, 404, 168, 160}, kRegion{28, 642, 182, 34};
    std::vector<std::pair<float, float>> overview_;
    const void* overviewOf_ = nullptr;
    bool regionStart_ = true, draggingOut_ = false;
    float downX_ = 0, downY_ = 0;
};
} // namespace sonora::ui
