#pragma once
// UI-5b: linked MIDI controller envelopes, shared by Arrangement, Session and take clips.
#include "core/Edit.h"
#include "ui/Widget.h"
#include <map>
#include <set>

namespace sonora::ui
{
class ClipEnvelopeEditor : public Widget
{
public:
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool wantsTextInput() const override { return menuOpen_; }
    bool keyPressed(const KeyEvent&) override;
    void focusLost() override;
    double pixelsPerBeat = 40, scrollBeats = 0;
    Rect plot() const;
    Rect chooser() const;
    static std::string controllerName(int index);
private:
    model::AutomationEnvelope envelope() const;
    void syncClip();
    float beatX(double) const;
    double xBeat(float) const;
    float valueY(double) const;
    double yValue(float) const;
    double grid() const;
    Rect menu() const;
    int menuRows() const;
    std::vector<int> choices() const;
    void choose(int);
    void beginGesture();
    void drawCell(const MouseEvent&);
    void commit();
    void cancel();
    int pointAt(float, float) const;
    bool editing_ = false, drawing_ = false, changed_ = false, menuOpen_ = false;
    bool focused_ = false;
    int menuOffset_ = 0, menuSelection_ = 0;
    std::string search_;
    model::ClipLocation location_, lastLocation_;
    model::AutomationEnvelope original_, working_, observed_;
    std::set<int> selected_;
    std::map<double, double> cells_;
    double grabBeat_ = 0, grabValue_ = 0, lastCell_ = 0;
};
} // namespace sonora::ui
