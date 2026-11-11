#pragma once
// The Sonora Phase Align VST3's window (PLG-21): a slim header above the shared PhaseAlignFace on a Skia canvas.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class PhaseAlignFace; }

namespace sonora::plugin
{
class PhaseAlignPlugin;

// Root of the plug-in window. The root never receives input itself (RootWidget hands events to children), so the face
// is a child that fills the window below the header.
class PhaseAlignView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 24.0f, kWidth = 940.0f, kHeight = 260.0f;
    PhaseAlignView(ui::UiContext&, PhaseAlignPlugin&);
    ui::PhaseAlignFace& face() { return *face_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;
    const char* version = "";

private:
    PhaseAlignPlugin& plugin_;
    ui::PhaseAlignFace* face_ = nullptr;
};

class PhaseAlignPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit PhaseAlignPluginEditor(PhaseAlignPlugin&);
    ~PhaseAlignPluginEditor() override;
    void resized() override;
    PhaseAlignView& view() { return view_; }

private:
    ui::UiContext& connect();

    ui::UiContext context_;
    PhaseAlignView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
