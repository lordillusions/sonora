#include "plugin/KickstractPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/KickstractPlugin.h"
#include "ui/Painter.h"
#include "ui/views/KickstractFace.h"

#include <algorithm>

namespace sonora::plugin
{
KickstractView::KickstractView(ui::UiContext& context, KickstractPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    face_ = add<ui::KickstractFace>(&plugin_.device());
#ifdef JucePlugin_VersionString
    version = JucePlugin_VersionString;
#endif
}

void KickstractView::layout()
{
    face_->setBounds({8, kHeader + 4, std::max(0.0f, width() - 16), std::max(0.0f, height() - kHeader - 10)});
}

void KickstractView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, "Sonora Kickstract", {10, 0, 200, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, ui::Align::Left, true);
    // The band colours as a small logo strip next to the name.
    for (int b = 0; b < devices::Kickstract::kBands; ++b)
        ui::paint::fillRounded(c, {140.0f + (float) b * 12, kHeader * 0.5f - 4, 9, 8}, 2, ui::KickstractFace::kBandColour[b]);
    std::string note = plugin_.buildLog.empty() ? std::string("v") + version : plugin_.buildLog.back();
    ui::paint::text(c, note, {200, 0, std::max(0.0f, r.w - 210), kHeader}, t.color("TextDisabled"), t.metrics.smallFontSize,
                    ui::Align::Right);
}

KickstractPluginEditor::KickstractPluginEditor(KickstractPlugin& plugin)
    : AudioProcessorEditor(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, true);
    setResizeLimits(900, 220, 3000, 1400);
    setSize(juce::roundToInt(KickstractView::kWidth), juce::roundToInt(KickstractView::kHeight));
}

KickstractPluginEditor::~KickstractPluginEditor()
{
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void KickstractPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
}

ui::UiContext& KickstractPluginEditor::connect()
{
    // Runs before view_ is built: the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
