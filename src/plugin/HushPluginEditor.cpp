#include "plugin/HushPluginEditor.h"

#include "gfx/SkiaCanvasComponent.h"
#include "plugin/HushPlugin.h"

namespace sonora::plugin
{
namespace
{
// The plug-in's version: juce_add_plugin's VERSION, which is the app's (docs/INSTALLERS.md "Release checks").
// tests/CMakeLists.txt defines it for the test build, which has no plug-in configuration.
constexpr const char* kVersion = JucePlugin_VersionString;
} // namespace

HushView::HushView(ui::UiContext& context, HushPlugin& plugin) : RootWidget(context)
{
    face_ = add<ui::HushFace>(plugin.session());
    face_->version = kVersion;
}

void HushView::layout()
{
    face_->setBounds(localBounds());
}

HushPluginEditor::HushPluginEditor(HushPlugin& plugin)
    : AudioProcessorEditor(plugin), plugin_(plugin), view_(connect(), plugin)
{
    canvas_ = std::make_unique<gfx::SkiaCanvasComponent>(view_);
    addAndMakeVisible(*canvas_);
    auto& face = view_.face();
    face.menuHost = [this]() -> juce::Component* { return canvas_.get(); };
    face.onScaleChosen = [this](float scale) {
        setSize(juce::roundToInt(HushView::kWidth * scale), juce::roundToInt(HushView::kHeight * scale));
    };
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(HushView::kWidth * 0.6f), juce::roundToInt(HushView::kHeight * 0.6f),
                    juce::roundToInt(HushView::kWidth * 2.5f), juce::roundToInt(HushView::kHeight * 2.5f));
    if (auto* constrainer = getConstrainer()) constrainer->setFixedAspectRatio(HushView::kWidth / HushView::kHeight);
    const float scale = plugin.session().editorScale;
    setSize(juce::roundToInt(HushView::kWidth * scale), juce::roundToInt(HushView::kHeight * scale));
    sized_ = true;
}

HushPluginEditor::~HushPluginEditor()
{
    canvas_.reset(); // it references view_
    view_.removeAllChildren();
}

void HushPluginEditor::resized()
{
    if (canvas_ != nullptr) canvas_->setBounds(getLocalBounds());
    if (sized_) plugin_.session().editorScale = (float) getWidth() / HushView::kWidth;
}

ui::UiContext& HushPluginEditor::connect()
{
    context_.theme.useBuiltin(false);
    context_.requestRepaint = [this] { if (canvas_) canvas_->requestFrame(); };
    return context_;
}
} // namespace sonora::plugin
