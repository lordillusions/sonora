#include "plugin/PhaseAlignPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/PhaseAlignPlugin.h"
#include "ui/Painter.h"
#include "ui/views/PhaseAlignFace.h"

#include <algorithm>
#include <cmath>

namespace sonora::plugin
{
PhaseAlignView::PhaseAlignView(ui::UiContext& context, PhaseAlignPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    face_ = add<ui::PhaseAlignFace>(&plugin_.device());
#ifdef JucePlugin_VersionString
    version = JucePlugin_VersionString;
#endif
}

void PhaseAlignView::layout()
{
    face_->setBounds({8, kHeader + 4, std::max(0.0f, width() - 16), std::max(0.0f, height() - kHeader - 10)});
}

void PhaseAlignView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, "Sonora Phase Align", {10, 0, 200, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, ui::Align::Left, true);
    // Logo: a kick and a bass wave meeting in phase.
    const SkColor colours[2] = {ui::PhaseAlignFace::kKick, ui::PhaseAlignFace::kAfter};
    for (int w = 0; w < 2; ++w)
        for (int i = 0; i < 12; ++i)
        {
            const float x = 150.0f + (float) i * 3.0f, y = kHeader * 0.5f - std::sin((float) i * 0.55f) * (w == 0 ? 5.0f : 3.5f);
            ui::paint::fillRounded(c, {x, y - 1, 2.4f, 2.4f}, 1.2f, colours[w]);
        }
    std::string note = plugin_.buildLog.empty() ? std::string("v") + version : plugin_.buildLog.back();
    ui::paint::text(c, note, {200, 0, std::max(0.0f, r.w - 210), kHeader}, t.color("TextDisabled"), t.metrics.smallFontSize,
                    ui::Align::Right);
}

PhaseAlignPluginEditor::PhaseAlignPluginEditor(PhaseAlignPlugin& plugin)
    : AudioProcessorEditor(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, true);
    setResizeLimits(820, 200, 3000, 1200);
    setSize(juce::roundToInt(PhaseAlignView::kWidth), juce::roundToInt(PhaseAlignView::kHeight));
}

PhaseAlignPluginEditor::~PhaseAlignPluginEditor()
{
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void PhaseAlignPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
}

ui::UiContext& PhaseAlignPluginEditor::connect()
{
    // Runs before view_ is built: the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
