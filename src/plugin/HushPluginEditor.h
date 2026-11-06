#pragma once
// The Sonora Hush VST3's window (PLG-14): the shared Hush face (ui/views/HushFace) on a Skia canvas, over the plug-in's
// session. The built-in device shows the same face in the device chain (PLG-14g).

#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/HushFace.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }

namespace sonora::plugin
{
class HushPlugin;

// Root of the plug-in window. The root never receives its own input (RootWidget hands events to children), so the face
// is a full-size child.
class HushView final : public ui::RootWidget
{
public:
    static constexpr float kWidth = ui::HushFace::kWidth, kHeight = ui::HushFace::kHeight;
    HushView(ui::UiContext&, HushPlugin&);
    ui::HushFace& face() { return *face_; }
    void layout() override;

private:
    ui::HushFace* face_ = nullptr;
};

class HushPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HushPluginEditor(HushPlugin&);
    ~HushPluginEditor() override;
    void resized() override;
    HushView& view() { return view_; }

private:
    ui::UiContext& connect();

    HushPlugin& plugin_;
    bool sized_ = false; // resized() keeps editorScale only once the constructor has set the size
    ui::UiContext context_;
    HushView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
