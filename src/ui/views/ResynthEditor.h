#pragma once
#include "ui/Widget.h"
#include "devices/native/Resynth.h"

namespace sonora::ui
{
class TextButton;
class Knob;
class ResynthEditor final : public Widget
{
public:
    static constexpr float preferredWidth = 1080, preferredHeight = 368;
    ResynthEditor(devices::Resynth*, int track, int device, int chain, int child,
                  std::function<void(Widget*, int)> tag = {});
    ~ResynthEditor() override;
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    void tick() override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    Rect waveformBounds() const { return {640, 51, 292, 70}; }
    Rect spectrumBounds() const { return {8, 51, 620, 70}; }
    int selectedExample() const { return selected_; }
    void selectPage(int page) { page_ = std::clamp(page, 0, 2); layout(); repaint(); }
private:
    Knob* knob(int parameter, int page, Rect bounds, const std::string& label = {});
    TextButton* choice(const std::string&, int parameter, int value, int page, Rect bounds);
    TextButton* toggle(const std::string&, int parameter, int page, Rect bounds);
    void loadFile();
    void import(const std::string&);
    void editBoundary(float start, float end);
    void removeSource();
    void envelope(SkCanvas&, const Theme&, Rect, int attack, int decay, int sustain, int release);
    devices::Resynth* processor_;
    int track_, device_, chain_, child_, selected_ = 0, drag_ = -1, page_ = 0, harmonic_ = -1;
    float boundaryStart_ = 0, boundaryEnd_ = 0, harmonicStart_ = 1, dragY_ = 0;
    bool auditioning_ = false;
    std::array<float, 292> lowPeaks_{}, highPeaks_{};
    std::function<void(Widget*, int)> tag_;
    struct Control { Widget* widget; int page; Rect bounds; };
    std::vector<Control> controls_;
    TextButton *load_ = nullptr, *phrase_ = nullptr, *audition_ = nullptr, *remove_ = nullptr;
    std::vector<TextButton*> modes_, tabs_;
};
} // namespace sonora::ui
