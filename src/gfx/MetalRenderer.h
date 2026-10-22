#pragma once
// GPU path: Skia (Ganesh) rendering into a CAMetalLayer-backed NSView — the same
// library + backend family Live 12 uses (see docs/adr/0002-skia.md).
// Implementation is Objective-C++ in MetalLayerView.mm.

#include <functional>
#include <memory>

class SkCanvas;

namespace sonora::gfx
{

class MetalRenderer
{
public:
    MetalRenderer();
    ~MetalRenderer();

    bool isValid() const;

    // The NSView* to embed (as void* so this header stays plain C++).
    void* nativeView() const;

    // Renders one frame. width/height are logical points; scale is the backing scale.
    bool render(int width, int height, float scale, const std::function<void(SkCanvas&)>& draw);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// `ui_snapshot --bench N --gpu`: renders `frames` frames of pw x ph device pixels into an off-screen Metal surface through
// the same Skia Ganesh backend as the app. cpuMs is what the message thread spends per frame (recording + flush, as in
// MetalRenderer::render, which does not wait for the GPU); gpuWaitMs is the extra time until the GPU finished.
struct GpuFrameTimes
{
    double cpuMs = 0, worstCpuMs = 0, gpuWaitMs = 0;
    int frames = 0;
    bool ok = false;
};
GpuFrameTimes benchmarkMetalOffscreen(int pw, int ph, int frames, const std::function<void(SkCanvas&, int)>& draw);

} // namespace sonora::gfx
