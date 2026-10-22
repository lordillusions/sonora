// Skia-on-Metal renderer. Compiled with ARC (-fobjc-arc, see src/CMakeLists.txt).

#include "gfx/MetalRenderer.h"

#include <algorithm>
#include <chrono>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

// Layer-hosting view that never takes mouse events: JUCE's peer view underneath receives
// them and forwards them to SkiaCanvasComponent. The VST3 build (PLG-11) gives it its own name, since the app and the
// plug-in can share a process.
#ifndef SONORA_METAL_VIEW_CLASS
#define SONORA_METAL_VIEW_CLASS SonoraMetalView
#endif
@interface SONORA_METAL_VIEW_CLASS : NSView
@end

@implementation SONORA_METAL_VIEW_CLASS
- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self)
        self.wantsLayer = YES;
    return self;
}
- (CALayer*)makeBackingLayer
{
    return [CAMetalLayer layer];
}
- (BOOL)wantsUpdateLayer
{
    return YES;
}
- (NSView*)hitTest:(NSPoint)point
{
    (void) point;
    return nil;
}
- (BOOL)isOpaque
{
    return YES;
}
@end

namespace sonora::gfx
{

struct MetalRenderer::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    SONORA_METAL_VIEW_CLASS* view = nil;
    sk_sp<GrDirectContext> context;
};

MetalRenderer::MetalRenderer() : impl_(std::make_unique<Impl>())
{
    impl_->device = MTLCreateSystemDefaultDevice();
    if (impl_->device == nil)
        return;
    impl_->queue = [impl_->device newCommandQueue];

    GrMtlBackendContext backend;
    backend.fDevice.retain((__bridge GrMTLHandle) impl_->device);
    backend.fQueue.retain((__bridge GrMTLHandle) impl_->queue);
    impl_->context = GrDirectContexts::MakeMetal(backend);
    if (impl_->context == nullptr)
        return;

    impl_->view = [[SONORA_METAL_VIEW_CLASS alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    auto* layer = (CAMetalLayer*) impl_->view.layer;
    layer.device = impl_->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.opaque = YES;
    layer.displaySyncEnabled = YES;
}

MetalRenderer::~MetalRenderer()
{
    if (impl_->context)
        impl_->context->abandonContext();
}

bool MetalRenderer::isValid() const
{
    return impl_->context != nullptr && impl_->view != nil;
}

void* MetalRenderer::nativeView() const
{
    return (__bridge void*) impl_->view;
}

bool MetalRenderer::render(int width, int height, float scale, const std::function<void(SkCanvas&)>& draw)
{
    if (! isValid() || width <= 0 || height <= 0)
        return false;

    @autoreleasepool
    {
        auto* layer = (CAMetalLayer*) impl_->view.layer;
        const int pw = (int) std::lround((float) width * scale), ph = (int) std::lround((float) height * scale);
        layer.contentsScale = scale;
        layer.drawableSize = CGSizeMake(pw, ph);

        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (drawable == nil)
            return false;

        GrMtlTextureInfo info;
        info.fTexture.retain((__bridge GrMTLHandle) drawable.texture);
        const auto target = GrBackendRenderTargets::MakeMtl(pw, ph, info);

        SkSurfaceProps props(0, kRGB_H_SkPixelGeometry);
        auto surface = SkSurfaces::WrapBackendRenderTarget(impl_->context.get(), target, kTopLeft_GrSurfaceOrigin,
                                                           kBGRA_8888_SkColorType, SkColorSpace::MakeSRGB(), &props);
        if (surface == nullptr)
            return false;

        auto* canvas = surface->getCanvas();
        canvas->save();
        canvas->scale(scale, scale);
        draw(*canvas);
        canvas->restore();

        impl_->context->flushAndSubmit(surface.get());

        id<MTLCommandBuffer> commands = [impl_->queue commandBuffer];
        [commands presentDrawable:drawable];
        [commands commit];
    }
    return true;
}

GpuFrameTimes benchmarkMetalOffscreen(int pw, int ph, int frames, const std::function<void(SkCanvas&, int)>& draw)
{
    GpuFrameTimes times;
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            return times;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        GrMtlBackendContext backend;
        backend.fDevice.retain((__bridge GrMTLHandle) device);
        backend.fQueue.retain((__bridge GrMTLHandle) queue);
        auto context = GrDirectContexts::MakeMetal(backend);
        if (context == nullptr)
            return times;
        SkSurfaceProps props(0, kRGB_H_SkPixelGeometry);
        auto surface = SkSurfaces::RenderTarget(context.get(), skgpu::Budgeted::kNo,
                                                SkImageInfo::Make(pw, ph, kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB()),
                                                0, kTopLeft_GrSurfaceOrigin, &props);
        if (surface == nullptr)
            return times;
        auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        for (int i = 0; i < frames; ++i)
        {
            @autoreleasepool
            {
                const double t0 = now();
                auto* canvas = surface->getCanvas();
                canvas->save();
                draw(*canvas, i);
                canvas->restore();
                context->flushAndSubmit(surface.get(), GrSyncCpu::kNo);
                const double t1 = now();
                context->submit(GrSyncCpu::kYes);
                const double t2 = now();
                times.cpuMs += t1 - t0;
                times.worstCpuMs = std::max(times.worstCpuMs, t1 - t0);
                times.gpuWaitMs += t2 - t1;
                ++times.frames;
            }
        }
        context->abandonContext();
        times.ok = true;
    }
    return times;
}

} // namespace sonora::gfx
