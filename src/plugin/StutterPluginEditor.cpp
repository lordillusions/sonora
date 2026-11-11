#include "plugin/StutterPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/StutterPlugin.h"
#include "ui/Painter.h"
#include "ui/views/StutterFace.h"

#include <algorithm>

namespace sonora::plugin
{
StutterView::StutterView(ui::UiContext& context, StutterPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    face_ = add<ui::StutterFace>(&plugin_.device());
#ifdef JucePlugin_VersionString
    version = JucePlugin_VersionString;
#endif
}

void StutterView::layout()
{
    face_->setBounds({8, kHeader + 4, std::max(0.0f, width() - 16), std::max(0.0f, height() - kHeader - 10)});
}

void StutterView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, "Sonora Stutter", {10, 0, 200, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, ui::Align::Left, true);
    // The mode colours as a small logo strip next to the name, shrinking like a roll.
    float x = 116;
    for (int m = 0; m < devices::Stutter::kModes; ++m)
    {
        const float w = 10.0f - (float) m * 1.5f;
        ui::paint::fillRounded(c, {x, kHeader * 0.5f - 4, w, 8}, 2, ui::StutterFace::kModeColour[m]);
        x += w + 3;
    }
    std::string note = plugin_.buildLog.empty() ? std::string("v") + version : plugin_.buildLog.back();
    ui::paint::text(c, note, {200, 0, std::max(0.0f, r.w - 210), kHeader}, t.color("TextDisabled"), t.metrics.smallFontSize,
                    ui::Align::Right);
}

StutterPluginEditor::StutterPluginEditor(StutterPlugin& plugin)
    : AudioProcessorEditor(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, true);
    setResizeLimits(820, 200, 3000, 1200);
    setSize(juce::roundToInt(StutterView::kWidth), juce::roundToInt(StutterView::kHeight));
}

StutterPluginEditor::~StutterPluginEditor()
{
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void StutterPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
}

ui::UiContext& StutterPluginEditor::connect()
{
    // Runs before view_ is built: the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
