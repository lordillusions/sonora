#include "plugin/SweepPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/SweepPlugin.h"
#include "ui/Painter.h"
#include "ui/views/SweepFace.h"

#include <algorithm>

namespace sonora::plugin
{
SweepView::SweepView(ui::UiContext& context, SweepPlugin& plugin) : RootWidget(context), plugin_(plugin)
{
    face_ = add<ui::SweepFace>(&plugin_.device());
#ifdef JucePlugin_VersionString
    version = JucePlugin_VersionString;
#endif
}

void SweepView::layout()
{
    face_->setBounds({8, kHeader + 4, std::max(0.0f, width() - 16), std::max(0.0f, height() - kHeader - 10)});
}

void SweepView::paint(SkCanvas& c, const ui::Theme& t)
{
    const auto r = localBounds();
    ui::paint::fill(c, r, t.color("SurfaceBackground"));
    ui::paint::fill(c, {0, 0, r.w, kHeader}, t.color("SurfaceHighlight"));
    ui::paint::text(c, "Sonora Sweep", {10, 0, 200, kHeader}, t.color("ControlForeground"), t.metrics.fontSize, ui::Align::Left, true);
    // Logo: a rising ramp of bars.
    for (int i = 0; i < 7; ++i)
    {
        const float h = 3.0f + (float) i * 2.0f;
        ui::paint::fillRounded(c, {116.0f + (float) i * 5.0f, kHeader * 0.5f + 7 - h, 3.5f, h}, 1.0f,
                               ui::paint::mix(ui::SweepFace::kPitch, ui::SweepFace::kCutoff, (float) i / 6.0f));
    }
    std::string note = plugin_.buildLog.empty() ? std::string("v") + version : plugin_.buildLog.back();
    ui::paint::text(c, note, {200, 0, std::max(0.0f, r.w - 210), kHeader}, t.color("TextDisabled"), t.metrics.smallFontSize,
                    ui::Align::Right);
}

SweepPluginEditor::SweepPluginEditor(SweepPlugin& plugin)
    : AudioProcessorEditor(plugin), plugin_(plugin), view_(connect(), plugin)
{
    view_.face().onLoadSample = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load a sample into Sonora Sweep", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
            if (const auto file = fc.getResult(); file != juce::File()) plugin_.loadSample(file);
        });
    };
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    setResizable(true, true);
    setResizeLimits(820, 200, 3000, 1200);
    setSize(juce::roundToInt(SweepView::kWidth), juce::roundToInt(SweepView::kHeight));
}

SweepPluginEditor::~SweepPluginEditor()
{
    chooser_.reset();
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void SweepPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
}

ui::UiContext& SweepPluginEditor::connect()
{
    // Runs before view_ is built: the face reads the context while it builds.
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
