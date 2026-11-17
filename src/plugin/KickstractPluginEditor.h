#pragma once
// The Sonora Kickstract VST3's window (PLG-17): the shared KickstractFace's full layout on a Skia canvas, letterboxed on
// the ProFace background like Sonora's other plug-ins (PLG-17f).

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class KickstractFace; class KickstractTimeline; }

namespace sonora::plugin
{
class KickstractPlugin;

// Root of the plug-in window. The root never receives input itself (RootWidget hands events to children), so the face
// is a child, scaled to fit and centred.
class KickstractView final : public ui::RootWidget
{
public:
    static constexpr float kWidth = 1003.0f, kHeight = 484.5f; // the full face at 85 %
    KickstractView(ui::UiContext&, KickstractPlugin&);
    ui::KickstractFace& face() { return *face_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;

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
    // PLG-17e: the timeline (Input / Kick / Removed over time, Freeze) in its own window; null while closed.
    void openTimeline();
    void closeTimeline();
    ui::KickstractTimeline* timeline() const;

private:
    class TimelineWindow;
    ui::UiContext& connect();

    KickstractPlugin& plugin_;
    ui::UiContext context_;
    KickstractView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
    std::unique_ptr<TimelineWindow> timeline_;
};
} // namespace sonora::plugin
