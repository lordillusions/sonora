#pragma once
// The Sonora Curve LFO VST3's window (PLG-12): the app's CurveLfoEditor on a Skia canvas. The face writes the plug-in's
// single CurveLfo directly, so only the theme and repaint parts of UiContext are set.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class CurveLfoEditor; class CurveLfoMini; class IconButton; }

namespace sonora::plugin
{
class CurveLfoPlugin;

// Root of the plug-in window: a slim header (name, last load note) above the device face. UI-83: the header's arrow
// folds the face to its graph (ui::CurveLfoMini) and back; onMiniChanged lets the window resize to it.
class CurveLfoPluginView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 22.0f;
    CurveLfoPluginView(ui::UiContext&, CurveLfoPlugin&);
    ui::CurveLfoEditor* editor() const { return editor_; }
    ui::CurveLfoMini* mini() const { return mini_; }
    ui::IconButton* foldButton() const { return fold_; }
    bool showsMini() const;
    void setMini(bool mini);
    std::function<void(bool mini)> onMiniChanged;
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;

private:
    CurveLfoPlugin& plugin_;
    ui::CurveLfoEditor* editor_ = nullptr;
    ui::CurveLfoMini* mini_ = nullptr;
    ui::IconButton* fold_ = nullptr;
};

class CurveLfoPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CurveLfoPluginEditor(CurveLfoPlugin&);
    ~CurveLfoPluginEditor() override;
    void resized() override;
    CurveLfoPluginView& view() { return view_; }
    static constexpr int kMiniWidth = 244, kMiniHeight = 200; // UI-83: the folded window (graph only)

private:
    void applySize(bool mini);
    ui::UiContext& connect();

    ui::UiContext context_;
    CurveLfoPluginView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
