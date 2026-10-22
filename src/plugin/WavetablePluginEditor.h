#pragma once
// The Sonora Prism VST3's window (PLG-11): the app's WavetableEditor on a Skia canvas, over a UiContext whose song is
// the plug-in's one-device model. The UiContext callbacks the dashboard uses (content edits, imports) are implemented
// here against WavetablePlugin instead of AppController; the rest of UiContext stays unset.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace sonora::gfx { class SkiaCanvasComponent; }
namespace sonora::ui { class WavetableEditor; }

namespace sonora::plugin
{
class WavetablePlugin;

// Root of the plug-in window: a slim header (name, last status line) above the dashboard.
class WavetablePluginView final : public ui::RootWidget
{
public:
    static constexpr float kHeader = 22.0f;
    WavetablePluginView(ui::UiContext&, WavetablePlugin&);
    void rebuild(); // after an instance swap: a new WavetableEditor over the running synth
    ui::WavetableEditor* editor() const { return editor_; }
    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;

private:
    WavetablePlugin& plugin_;
    ui::WavetableEditor* editor_ = nullptr;
};

class WavetablePluginEditor final : public juce::AudioProcessorEditor, private juce::AsyncUpdater
{
public:
    explicit WavetablePluginEditor(WavetablePlugin&);
    ~WavetablePluginEditor() override;
    void resized() override;
    WavetablePluginView& view() { return view_; }
    ui::UiContext& context() { return context_; }
    void rebuildNowIfPending() { handleUpdateNowIfNeeded(); } // tests: the rebuild normally waits for the message loop

private:
    void handleAsyncUpdate() override; // rebuilds the dashboard after a swap, outside the gesture that caused it
    void connect();
    void importTable(int oscillator, const std::string& path);
    void importMedia(int oscillator, const std::string& path);
    void importPreset(const std::string& path);
    void importImpulse(uint32_t moduleId, const std::string& path);
    void setEffects(const devices::WavetableEffects::Configuration&);

    WavetablePlugin& plugin_;
    ui::UiContext context_;
    WavetablePluginView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
