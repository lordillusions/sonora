#pragma once
// DEV-18d: seven source routes and immutable FX return topology, entirely message-thread bound.
#include "devices/DeviceProcessor.h"
#include "ui/Widget.h"
#include <array>

namespace sonora::ui
{
class WavetableRoutingEditor final : public Widget
{
public:
    enum class Field { Destination, FilterBalance, Bus1, Bus2, Envelope, Level, Count };
    struct ParameterBinding
    {
        int index = -1;
        devices::ParameterInfo info;
        std::optional<midi::ControlTarget> target;
        bool quadraticGain = false;
    };
    struct Binding
    {
        std::function<ParameterBinding(int source, Field)> parameter;
        std::function<ParameterBinding(bool direct)> headerParameter;
        std::function<float(int)> get;
        std::function<void(int, float)> set;
        std::function<bool(int source, int choice)> destinationAllowed;
        std::function<bool(int source)> levelActive;
        std::function<std::array<bool, 2>()> returns;
        std::function<void(int bus, bool beforeMain)> commitReturn;
        std::function<void(int parameter)> parameterContext;
        std::function<void(int page)> onPageChange;
    };
    explicit WavetableRoutingEditor(Binding);
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    void focusLost() override;
    std::optional<midi::ControlTarget> mappingTargetAt(float x, float y) const override;
    Rect parameterCell(int source, Field) const;
    Rect activeCell() const;
    Rect directCell() const;
    Rect returnCell(int bus) const;
    Rect pageCell(int direction) const;
    std::vector<int> visibleSources() const;
    int page() const { return page_; }
    int pageCount() const { return (7 + rows_ - 1) / rows_; }
    void restorePage(int page);
private:
    ParameterBinding parameter(int source, Field) const;
    std::pair<int, Field> fieldAt(float x, float y) const;
    float value(const ParameterBinding&) const;
    void setValue(const ParameterBinding&, float);
    void chooseDestination(int source, int step = 0);
    void chooseReturn(int bus);
    Binding binding_;
    std::array<Widget*, 2> pageButtons_{}, returnButtons_{};
    int page_ = 0, rows_ = 1, pressedSource_ = -1;
    Field pressedField_ = Field::Count;
    bool dragging_ = false, headerPressed_ = false, ignoreUp_ = false;
    float dragY_ = 0, dragNormal_ = 0;
};
} // namespace sonora::ui
