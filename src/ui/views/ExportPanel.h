#pragma once
// Modal "Export Audio" dialog: what to render (Main, every track as a stem, one track), the range, tail, file format,
// bit depth, Normalize / Mono / Dither; then a progress view and a loudness summary. The render itself is driven by
// the app through UiContext::startExport / cancelExport; this widget only edits UiContext::exportSettings and shows
// UiContext::exportState.

#include "ui/Widget.h"

#include <functional>
#include <vector>

namespace sonora::ui
{

class ExportPanel : public Widget
{
public:
    void paint(SkCanvas& c, const Theme& t) override;
    void tick() override;
    void mouseDown(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;
    bool wantsTextInput() const override { return editing_ >= 0; }
    void focusLost() override { commitEdit(); }

private:
    struct Hit
    {
        Rect area;
        std::function<void()> action;
    };

    Rect panelRect() const;
    void button(SkCanvas& c, const Theme& t, const Rect& r, const std::string& label, bool on, std::function<void()> action, bool enabled = true);
    void dropdownField(SkCanvas& c, const Theme& t, const Rect& r, const std::string& text, std::function<void()> action, bool enabled = true);
    void numberField(SkCanvas& c, const Theme& t, const Rect& r, int index, double value, bool enabled);
    void choose(const std::vector<std::string>& labels, int current, std::function<void(int)> pick);
    void commitEdit();
    void close();
    std::string lengthText() const;

    std::vector<Hit> hits_;
    int editing_ = -1; // 0 start bar, 1 end bar
    std::string text_;
    float shownProgress_ = -1.0f;
    int shownPhase_ = -1;
};

} // namespace sonora::ui
