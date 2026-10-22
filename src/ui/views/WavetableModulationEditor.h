#pragma once
// DEV-18d: native modulation rows, stable targets and immutable source/via curve gestures.
// Binding callbacks run on the message thread; commits may synchronously destroy this widget.
#include "devices/DeviceProcessor.h"
#include "dsp/CurveGraph.h"
#include "ui/Widget.h"
#include <array>

namespace sonora::ui
{
class WavetableModulationEditor final : public Widget
{
public:
    enum class Field { Source, Via, Destination, Amount, Bipolar, Out, Count };
    enum class CurveLaw { Native, Imported };
    struct ParameterBinding
    {
        int index = -1;
        devices::ParameterInfo info;
        std::optional<midi::ControlTarget> target;
    };
    struct CurveSnapshot
    {
        dsp::CurveGraph graph = dsp::CurveGraph::preset(3);
        bool enabled = false;
        CurveLaw law = CurveLaw::Native;
    };
    struct Target { std::string id, label, group; };
    struct ViewState
    {
        int page = 0, selectedSlot = 0;
        bool auxiliary = false, curveOpen = false;
    };
    struct Binding
    {
        int slots = 64;
        std::function<ParameterBinding(int slot, Field)> parameter;
        std::function<float(int index)> getParameter;
        std::function<void(int index, float value)> setParameter;
        std::function<CurveSnapshot(int slot, bool auxiliary)> curve;
        std::function<std::string(int slot)> destination;
        std::function<std::vector<Target>()> targets;
        std::function<void(int slot, bool auxiliary, const dsp::CurveGraph&, bool enabled)> commitCurve;
        std::function<void(int slot, const std::string& id)> commitDestination;
        std::function<void(int index)> parameterContext;
        std::function<void(const ViewState&)> onViewChange;
    };

    explicit WavetableModulationEditor(Binding);
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    void focusLost() override;
    bool mouseWheel(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;
    std::optional<midi::ControlTarget> mappingTargetAt(float x, float y) const override;
    void tick() override { repaint(); }
    Rect parameterCell(int slot, Field) const;
    Rect curveCell(int slot, bool auxiliary) const;
    Rect pageCell(int direction) const;
    Rect curveSelectorCell(bool auxiliary) const;
    Rect curveEnableCell(bool auxiliary) const;
    Rect closeCell() const;
    Rect resetCell() const;
    Rect slotCell(int direction) const;
    Rect plot() const;
    int pageCount() const;
    int rowsPerPage() const { return rows_; }
    std::vector<int> visibleSlots() const;
    const ViewState& viewState() const { return view_; }
    void restoreViewState(ViewState);
    // The caller can supply a target chooser; the default is a grouped native popup.
    std::function<void(const std::vector<Target>&, const std::string& current,
                       std::function<void(std::string)>)> onChooseDestination;

private:
    enum class Drag { None, Parameter, Point, Curve };
    struct CurveButtons { Widget* primary = nullptr; Widget* auxiliary = nullptr; };
    ParameterBinding parameter(int slot, Field) const;
    CurveSnapshot curve(int slot, bool auxiliary) const;
    Rect rowCell(int slot) const;
    std::pair<int, Field> fieldAt(float x, float y) const;
    float value(const ParameterBinding&) const;
    void setValue(const ParameterBinding&, float);
    void stepChoice(int slot, Field, int direction);
    void chooseField(int slot, Field);
    void chooseDestination(int slot);
    void openCurve(int slot, bool auxiliary);
    void toggleCurve(bool auxiliary);
    void resetCurve();
    void commitCurve(const CurveSnapshot&);
    void storeView();
    void paintCurve(SkCanvas&, Rect, const CurveSnapshot&, bool handles) const;
    float phaseAt(float x, bool snap) const;
    float levelAt(float y, bool snap) const;
    int pointAt(const dsp::CurveGraph&, float x, float y) const;
    int handleAt(const CurveSnapshot&, float x, float y) const;
    void insertPoint(float phase, float level);

    Binding binding_;
    ViewState view_;
    std::vector<CurveButtons> curveButtons_;
    std::array<Widget*, 2> pages_{}, selectors_{}, enables_{}, slots_{};
    Widget *close_ = nullptr, *reset_ = nullptr;
    int rows_ = 1, dragIndex_ = -1, pressedSlot_ = -1;
    Field pressedField_ = Field::Count;
    Drag drag_ = Drag::None;
    float dragY_ = 0, dragValue_ = 0;
    bool ignoreUp_ = false;
    CurveSnapshot draft_;
};
} // namespace sonora::ui
