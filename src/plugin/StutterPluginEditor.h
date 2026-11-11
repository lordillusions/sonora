#pragma once
// The Sonora Stutter VST3's window (PLG-18): a slim header above the shared StutterFace on a Skia canvas.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class StutterFace; }

namespace sonora::plugin
{
class StutterPlugin;

// Root of the plug-in window. The root never receives input itself (RootWidget hands events to children), so the face
// is a child that fills the window below the header.
class StutterView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 24.0f, kWidth = 980.0f, kHeight = 250.0f;
    StutterView(ui::UiContext&, StutterPlugin&);
    ui::StutterFace& face() { return *face_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;
    const char* version = "";

private:
    StutterPlugin& plugin_;
    ui::StutterFace* face_ = nullptr;
};

class StutterPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit StutterPluginEditor(StutterPlugin&);
    ~StutterPluginEditor() override;
    void resized() override;
    StutterView& view() { return view_; }

private:
    ui::UiContext& connect();

    ui::UiContext context_;
    StutterView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
