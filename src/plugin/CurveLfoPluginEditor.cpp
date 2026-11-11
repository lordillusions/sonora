#include "plugin/CurveLfoPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/CurveLfoPlugin.h"
#include "ui/Painter.h"
#include "ui/views/CurveLfoEditor.h"
#include "ui/widgets/Buttons.h"

#include <algorithm>

namespace sonora::plugin
{
namespace
{
constexpr float kWidth = ui::CurveLfoEditor::kPreferredWidth + 12, kHeight = 280 + CurveLfoPluginView::kHeader + 6;
}

CurveLfoPluginView::CurveLfoPluginView(ui::UiContext& context, CurveLfoPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    editor_ = add<ui::CurveLfoEditor>(&plugin_.lfo());
    mini_ = add<ui::CurveLfoMini>(&plugin_.lfo());
    mini_->onClick = [this] { setMini(false); };
    fold_ = add<ui::IconButton>(ui::IconButton::Icon::FoldOpen);
    fold_->onClick = [this] { setMini(! showsMini()); };
    editor_->visible = ! plugin_.editorMini;
    mini_->visible = plugin_.editorMini;
}

bool CurveLfoPluginView::showsMini() const { return mini_->visible; }

void CurveLfoPluginView::setMini(bool mini)
{
    plugin_.editorMini = mini;
    editor_->visible = ! mini;
    mini_->visible = mini;
    layout();
    repaint();
    if (onMiniChanged) onMiniChanged(mini); // the window resizes; layout runs again from there
}

void CurveLfoPluginView::layout()
{
    const ui::Rect body{6, kHeader, std::max(0.0f, width() - 12), std::max(0.0f, height() - kHeader - 6)};
    editor_->setBounds(body);
    mini_->setBounds(body);
    fold_->setIcon(showsMini() ? ui::IconButton::Icon::FoldClosed : ui::IconButton::Icon::FoldOpen);
    fold_->tooltip = showsMini() ? "Show the whole device" : "Show only the graph";
    fold_->setBounds({width() - 25, 3, 17, 16});
}

void CurveLfoPluginView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    const float titleEnd = fold_->bounds().x - 6;
    ui::paint::text(c, "Sonora Curve LFO", {8, 0, std::min(240.0f, titleEnd - 8), kHeader}, t.color("ControlForeground"), t.metrics.fontSize,
                    ui::Align::Left, true);
    const std::string note = plugin_.buildLog.empty() || showsMini() ? std::string{} : plugin_.buildLog.back();
    ui::paint::text(c, note, {256, 0, std::max(0.0f, titleEnd - 256), kHeader}, t.color("ControlForeground"), t.metrics.fontSize,
                    ui::Align::Right);
}

CurveLfoPluginEditor::CurveLfoPluginEditor(CurveLfoPlugin& plugin)
    : AudioProcessorEditor(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, false);
    view_.onMiniChanged = [this](bool mini) { applySize(mini); };
    applySize(plugin.editorMini);
}

void CurveLfoPluginEditor::applySize(bool mini)
{
    // UI-83: folded, the window shrinks to the graph; unfolded it returns to the full face's size.
    if (mini) { setResizeLimits(200, 150, 4000, 3000); setSize(kMiniWidth, kMiniHeight); }
    else { setResizeLimits(720, 200, 4000, 3000); setSize(juce::roundToInt(kWidth), juce::roundToInt(kHeight)); }
}

CurveLfoPluginEditor::~CurveLfoPluginEditor()
{
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void CurveLfoPluginEditor::resized()
{
    canvas_->setBounds(getLocalBounds());
}

ui::UiContext& CurveLfoPluginEditor::connect()
{
    // Runs before view_ is built (from its constructor's argument list): the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
