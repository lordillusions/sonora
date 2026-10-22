#pragma once
// Bridges JUCE (windowing, input) and Skia (drawing).
//
// Rendering paths:
//   1. Metal (default on macOS): MetalRenderer's NSView is embedded as a child; Skia draws on the GPU.
//   2. Raster fallback: Skia draws into a juce::Image's pixels, painted in paint().
//      Used when Metal is unavailable, SONORA_RASTER=1 is set, and always on Windows/Linux (COM-7;
//      MetalRendererNone.cpp reports no GPU there).
//
// Input: JUCE mouse/key events are converted and sent to the ui::RootWidget.
// Frame pacing: a VBlankAttachment ticks the widget tree (~30 Hz animation) and renders
// only when something is dirty.

#include "ui/RootWidget.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

class SkCanvas;

namespace sonora::gfx
{

class MetalRenderer;

class SkiaCanvasComponent : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit SkiaCanvasComponent(ui::RootWidget& root);
    ~SkiaCanvasComponent() override;

    bool usingGpu() const;
    void requestFrame() { root_.markDirty(); }

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    void mouseMagnify(const juce::MouseEvent& e, float scaleFactor) override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(FocusChangeType) override;

    // juce::FileDragAndDropTarget: Finder drags of audio files (and a single .als), see RootWidget::dispatchFileDrop.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override { fileDragMove(files, x, y); }
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void onVBlank();
    void updateCursor(); // the widget's cursor, or the animated busy cursor while UiContext::waitingForBusy()
    void drawTree(SkCanvas& canvas);
    float backingScale() const;
    float zoom() const; // UiContext::uiZoom, clamped

    ui::RootWidget& root_;
    std::unique_ptr<MetalRenderer> metal_;
#if JUCE_MAC
    juce::NSViewComponent viewHolder_;
#endif
    juce::VBlankAttachment vblank_;
    juce::Image raster_;
    double lastTick_ = 0;
    float lastZoom_ = 1.0f;
    int64_t framesRendered_ = 0;
    std::vector<juce::MouseCursor> busyCursors_; // rendered on first use
    int busyFrame_ = -1;                         // shown busy frame, -1 when the normal cursor is set
};

// Renders a widget tree off-screen (CPU) and writes a PNG. Used by tools/ui_snapshot
// and tests. Returns false on failure.
bool renderToPng(ui::RootWidget& root, int width, int height, float scale, const std::string& path);

// Renders `frames` frames off-screen (CPU) and returns the time of each phase in milliseconds, summed over the frames.
// `beforeFrame(i)` runs before every frame (change zoom / scroll there). Used by `ui_snapshot --bench`.
struct FrameTimes
{
    double tickMs = 0, layoutMs = 0, paintMs = 0, worstFrameMs = 0;
    int frames = 0;
};
FrameTimes benchmarkFrames(ui::RootWidget& root, int width, int height, float scale, int frames, const std::function<void(int)>& beforeFrame);

} // namespace sonora::gfx
