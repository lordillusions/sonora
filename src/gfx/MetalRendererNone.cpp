// COM-7: Windows/Linux have no Metal. The renderer reports itself invalid, so SkiaCanvasComponent
// uses its raster path. TODO(COM-7): Skia Ganesh GL (Linux) / D3D (Windows) renderer.
#include "gfx/MetalRenderer.h"

namespace sonora::gfx
{
struct MetalRenderer::Impl
{
};
MetalRenderer::MetalRenderer() = default;
MetalRenderer::~MetalRenderer() = default;
bool MetalRenderer::isValid() const { return false; }
void* MetalRenderer::nativeView() const { return nullptr; }
bool MetalRenderer::render(int, int, float, const std::function<void(SkCanvas&)>&) { return false; }
GpuFrameTimes benchmarkMetalOffscreen(int, int, int, const std::function<void(SkCanvas&, int)>&) { return {}; } // ok == false
} // namespace sonora::gfx
