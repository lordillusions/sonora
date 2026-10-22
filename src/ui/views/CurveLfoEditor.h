#pragma once
// Device face of the Sonora Curve LFO (DEV-16b, UI-43): LFO / Filter / MIDI control pages on the left, the graph display
// in the middle (twelve graph tabs with thumbnails, shape and speed menus, copy/paste, the audio scope behind the curve)
// and the five destination routes on the right.
// Graph display:
//   Double-click: add a point, or remove the one under the cursor (Cmd-click also removes).
//   Drag a point; drag the hollow handle on a slanted segment to bend it (Alt: bend every segment).
//   Alt-drag snaps to the grid. Shift-click adds a flat step at the grid size.
#include "dsp/CurveGraph.h"
#include "ui/Widget.h"
#include <functional>
#include <vector>

namespace sonora::devices { class CurveLfo; }
namespace sonora::ui
{
class Knob;
class TextButton;
class CurveLfoEditor final : public Widget
{
public:
    static constexpr float kPreferredWidth = 1130;
    enum Page { LfoPage, FilterPage, MidiPage };
    explicit CurveLfoEditor(devices::CurveLfo* processor, std::function<void(Widget*, int)> tag = {});
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseMove(const MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    void tick() override;

    void selectPage(int page);
    int page() const { return page_; }
    // Graph display geometry (local coordinates); exposed for tests.
    Rect display() const;   // header + plot
    Rect graphCell(int graph) const;
    Rect shapeCell() const;
    Rect warpCell() const;
    Rect copyCell() const;
    Rect pasteCell() const;
    Rect plot() const;
    Rect routeRow(int destination) const; // right-hand destination row
    float pointX(float phase) const;
    float pointY(float level) const;
    // Destinations in the order the routing column lists them.
    static constexpr int kRouteOrder[] = {2, 3, 0, 1, 4}; // Vol, Pan, Cut, Res, Var

private:
    enum class Drag { None, Point, Curve };
    struct Control { Widget* widget; int page; };
    int selectedGraph() const;
    bool headerClick(const MouseEvent&);
    int pointAt(const dsp::CurveGraph&, float x, float y) const;
    int handleAt(const dsp::CurveGraph&, float x, float y) const;
    void insertPoint(dsp::CurveGraph&, float phase, float level) const;
    float phaseAt(float x, bool snap) const;
    float levelAt(float y, bool snap) const;
    void chooseShape();
    void chooseWarp();
    void paintDisplay(SkCanvas&, const Theme&);
    void paintRoutes(SkCanvas&, const Theme&);
    Knob* knob(int parameter, int page, const std::string& label = {});
    Knob* attach(Knob*, int parameter, int page, const std::string& tip);
    TextButton* toggle(const std::string& label, int parameter, int page);
    TextButton* choice(const std::string& label, int parameter, int value, int page);
    Rect leftArea() const;
    Rect routeArea() const;

    devices::CurveLfo* processor_;
    std::function<void(Widget*, int)> tag_;
    std::vector<Control> controls_;
    std::vector<TextButton*> tabs_;
    std::vector<Knob*> routeGraphs_, routeAmounts_;
    Knob *rate_ = nullptr, *hertz_ = nullptr, *swing_ = nullptr, *phase_ = nullptr, *pwm_ = nullptr, *smooth_ = nullptr,
         *offset_ = nullptr, *depth_ = nullptr, *filterType_ = nullptr, *cutoff_ = nullptr, *resonance_ = nullptr,
         *filterVar_ = nullptr, *filterMix_ = nullptr, *ccOut_ = nullptr, *ccDepth_ = nullptr;
    TextButton *sync_ = nullptr, *dotted_ = nullptr, *triplet_ = nullptr, *anchor_ = nullptr, *filterOn_ = nullptr;
    std::vector<TextButton*> retrig_, midiToggles_;
    Drag drag_ = Drag::None;
    int dragIndex_ = -1, shapeIndex_ = 0, hoverPoint_ = -1, hoverHandle_ = -1, page_ = LfoPage;
    bool dragAll_ = false, syncShown_ = true;
    unsigned revision_ = 0;
};
} // namespace sonora::ui
