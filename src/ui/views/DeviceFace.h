#pragma once

#include "core/Model.h"
#include "ui/Widget.h"

namespace sonora::devices { class DeviceProcessor; }
namespace sonora::engine { struct Peaks; struct SampleData; }
namespace sonora::ui
{
class NativeDevicePanel;
// UI-33: compact device-specific faces. All controls address real processor parameters.
class DeviceFace final : public Widget
{
public:
    enum class Kind { Fm, Simpler, Sampler, Vector, Impulse, Echo, Redux, Tuner, Gate, AutoPan, Meter, Utility };
    static bool supports(devices::DeviceProcessor*);
    DeviceFace(const model::Device&, devices::DeviceProcessor*, int track, int device, int chain, int child,
               std::function<void(Widget*, int)> tag);
    float preferredWidth() const;
    bool applies(const std::string& parameterId) const; // false: a shared processor parameter this face does not use
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    Kind kind() const { return kind_; }
    Rect graphBounds() const { return scaled(graph_); }
    int selectedPage() const { return page_; }
    void selectPage(int page) { page_ = page; layout(); repaint(); }
    int selectedSection() const { return selected_; }
    void selectSection(int section) { selected_ = section; layout(); repaint(); }
private:
    struct Control { Widget* widget; Rect bounds; int page; };
    Widget* control(int parameter, std::string label, Rect, bool knob = true, int page = -1, bool dark = false);
    void tab(std::string label, Rect, int value, bool selection = false);
    void sampleControl(std::string label, Rect, int field);
    Rect scaled(Rect) const;
    void envelope(SkCanvas&, const Theme&, Rect, int a, int d, int s, int r);
    void waveform(SkCanvas&, const Theme&, Rect);
    void filter(SkCanvas&, const Theme&, Rect, int frequency, int resonance, int type = -1);
    void routing(SkCanvas&, const Theme&, Rect);
    void meter(SkCanvas&, const Theme&);
    Widget* utilityKnob(int kind, Rect);
    void edit(const std::function<void(model::Device&)>&, const std::string&);
    const model::Device& model_;
    devices::DeviceProcessor* processor_;
    int track_, device_, chain_, child_;
    Kind kind_;
    int selected_ = 0, page_ = 0, dragHandle_ = -1;
    int64_t dragFrame_ = 0;
    Rect graph_;
    std::shared_ptr<const engine::Peaks> preview_;
    const engine::SampleData* previewSource_ = nullptr;
    std::array<float, 160> history_{};
    int historyWrite_ = 0;
    double strobePhase_ = 0;
    std::array<float, 2> meterLevel_{}, meterHold_{}; // Meter: shown bar heights 0..1 and peak-hold marks
    std::array<int, 2> meterHoldAge_{};
    NativeDevicePanel* zones_ = nullptr;
    std::vector<Control> controls_;
    std::function<void(Widget*, int)> tag_;
};
}
