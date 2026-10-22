#pragma once
#include "core/Model.h"
#include "ui/Widget.h"

namespace sonora::devices { class LfoShaper; }
namespace sonora::ui
{
class LfoCurveEditor final : public Widget
{
public:
    explicit LfoCurveEditor(devices::LfoShaper* processor) : processor_(processor) {}
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
private:
    devices::LfoShaper* processor_;
    int lastPoint_ = -1;
    float lastValue_ = 0;
};

// Sample zones and rack pads/chains, including drag targets and editable numeric cells.
// Drag numeric cells vertically (or use the wheel); right-click a row to remove it.
class NativeDevicePanel final : public Widget
{
public:
    NativeDevicePanel(const model::Device& device, int track, int index, int chain = -1, int child = -1)
        : device_(device), track_(track), index_(index), chain_(chain), child_(child) {}
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    Rect padBounds(int note) const;
    Rect cellBounds(int row, int column) const;
private:
    bool rack() const { return device_.kind == model::DeviceKind::Rack; }
    bool drums() const { return device_.type == "DrumGroupDevice"; }
    void edit(const std::function<void(model::Device&)>& operation, const std::string& label);
    int rowAt(float y) const;
    int padAt(float x, float y) const;
    int selectedChain() const;
    int padBase() const;
    float tableTop() const;
    float tableLeft() const { return drums() ? 224.0f : 0.0f; }
    float tableWidth() const { return width() - tableLeft(); }
    void changeCell(int row, int column, int delta);
    const model::Device& device_;
    int track_, index_, chain_, child_;
    int scroll_ = 0;
    int dragRow_ = -1, dragColumn_ = -1, dragDelta_ = 0;
    float dragY_ = 0;
};
} // namespace sonora::ui
