#pragma once
// The Sonora Sweep VST3's window (PLG-19): a slim header above the shared SweepFace on a Skia canvas.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class SweepFace; }

namespace sonora::plugin
{
class SweepPlugin;

// Root of the plug-in window. The root never receives input itself (RootWidget hands events to children), so the face
// is a child that fills the window below the header.
class SweepView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 24.0f, kWidth = 1040.0f, kHeight = 250.0f;
    SweepView(ui::UiContext&, SweepPlugin&);
    ui::SweepFace& face() { return *face_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;
    const char* version = "";

private:
    SweepPlugin& plugin_;
    ui::SweepFace* face_ = nullptr;
};

class SweepPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit SweepPluginEditor(SweepPlugin&);
    ~SweepPluginEditor() override;
    void resized() override;
    SweepView& view() { return view_; }

private:
    ui::UiContext& connect();

    SweepPlugin& plugin_;
    ui::UiContext context_;
    SweepView view_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
