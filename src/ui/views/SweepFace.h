#pragma once
// Face of Sonora Sweep (PLG-19, PLG-23), on the ProFace kit: the device chain (compact), its own window and the VST3
// editor (full).
//
// Full layout: what it makes (riser or downlifter, length, seconds at the tempo) and the Preview button on top; TYPE (ready
// made sweeps, one click) and SAMPLE (the loaded file; drop one from the Browser or the Finder anywhere on the face) on
// the left; the display (the sweep itself, rendered, with the filter, pitch and level curves over the bars and the drop
// marked); SOURCE and Up/Down; LENGTH; the FILTER, PITCH, SHAPE and OUT knobs; the help line.
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
    static constexpr Size kFull{1100, 700}, kCompact{720, 170};

    explicit SweepFace(devices::Sweep& device);
    devices::Sweep& sweep() { return sweep_; }

    // A sample was dropped on the face (the app: an undoable device edit; the VST3 loads it directly).
    std::function<void(const std::string& path)> onSampleDropped;
    // "Choose..." in the SAMPLE panel: whoever shows the face opens a file chooser. Unset: the button is hidden.
    std::function<void()> onChooseSample;

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

    devices::Sweep& sweep_;
    std::vector<float> renderLo_, renderHi_;
    uint64_t renderedSignature_ = 0, pendingSignature_ = 0;
    int pendingTicks_ = 0;
    float progress_ = -1;
    Rect display_;
};
} // namespace sonora::ui
