#pragma once
// The Sonora Kickstract VST3's window (PLG-17): a slim header above the shared KickstractFace on a Skia canvas.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class KickstractFace; }

namespace sonora::plugin
{
class KickstractPlugin;

// Root of the plug-in window. The root never receives input itself (RootWidget hands events to children), so the face
// is a child that fills the window below the header.
class KickstractView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 24.0f, kWidth = 1110.0f, kHeight = 330.0f;
    KickstractView(ui::UiContext&, KickstractPlugin&);
    ui::KickstractFace& face() { return *face_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;
    const char* version = "";

private:
    KickstractPlugin& plugin_;
    ui::KickstractFace* face_ = nullptr;
};

class KickstractPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit KickstractPluginEditor(KickstractPlugin&);
    ~KickstractPluginEditor() override;
    void resized() override;
    KickstractView& view() { return view_; }

private:
    ui::UiContext& connect();

    ui::UiContext context_;
    KickstractView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
