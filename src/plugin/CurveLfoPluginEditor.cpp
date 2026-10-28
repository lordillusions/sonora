#include "plugin/CurveLfoPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/CurveLfoPlugin.h"
#include "ui/Painter.h"
#include "ui/views/CurveLfoEditor.h"

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
}

void CurveLfoPluginView::layout()
{
    editor_->setBounds({6, kHeader, std::max(0.0f, width() - 12), std::max(0.0f, height() - kHeader - 6)});
}

void CurveLfoPluginView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, "Sonora Curve LFO", {8, 0, 240, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, ui::Align::Left, true);
    const std::string note = plugin_.buildLog.empty() ? std::string{} : plugin_.buildLog.back();
    ui::paint::text(c, note, {256, 0, std::max(0.0f, r.w - 264), kHeader}, t.color("ControlForeground"), t.metrics.fontSize,
                    ui::Align::Right);
}

CurveLfoPluginEditor::CurveLfoPluginEditor(CurveLfoPlugin& plugin)
    : AudioProcessorEditor(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, false);
    setResizeLimits(720, 200, 4000, 3000);
    setSize(juce::roundToInt(kWidth), juce::roundToInt(kHeight));
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
