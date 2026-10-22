#include "gfx/SkiaCanvasComponent.h"

#include "gfx/Brand.h"
#include "gfx/MetalRenderer.h"
#include "util/Log.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sonora::gfx
{

namespace
{
ui::MouseEvent convert(const juce::MouseEvent& e, float zoom)
{
    ui::MouseEvent m;
    m.x = e.position.x / zoom;
    m.y = e.position.y / zoom;
    m.screenX = (float) e.getScreenX();
    m.screenY = (float) e.getScreenY();
    m.clicks = e.getNumberOfClicks();
    m.shift = e.mods.isShiftDown();
    m.cmd = e.mods.isCommandDown();
    m.alt = e.mods.isAltDown();
    m.right = e.mods.isPopupMenu();
    return m;
}
std::vector<std::string> toPaths(const juce::StringArray& files)
{
    std::vector<std::string> paths;
    for (const auto& file : files) paths.push_back(file.toStdString());
    return paths;
}
} // namespace

SkiaCanvasComponent::SkiaCanvasComponent(ui::RootWidget& root)
    : root_(root), vblank_(this, [this] { onVBlank(); })
{
    setWantsKeyboardFocus(true);
    setOpaque(true);
    root_.context().rootToScreen = [this](const ui::Rect& r) {
        const float z = zoom();
        const auto origin = localPointToGlobal(juce::Point<float>(r.x * z, r.y * z));
        return ui::Rect{origin.x, origin.y, r.w * z, r.h * z};
    };

    if (std::getenv("SONORA_RASTER") == nullptr)
    {
        metal_ = std::make_unique<MetalRenderer>();
        if (metal_->isValid())
        {
#if JUCE_MAC
            viewHolder_.setView(metal_->nativeView());
            viewHolder_.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(viewHolder_);
#endif
        }
        else
        {
            metal_.reset();
        }
    }
    logf("renderer: %s", metal_ ? "Skia/Metal (GPU)" : "Skia/raster (CPU)");
}

SkiaCanvasComponent::~SkiaCanvasComponent()
{
    root_.context().rootToScreen = nullptr;
#if JUCE_MAC
    viewHolder_.setView(nullptr);
#endif
}

bool SkiaCanvasComponent::usingGpu() const
{
    return metal_ != nullptr;
}

float SkiaCanvasComponent::backingScale() const
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds()))
        return (float) display->scale;
    return 2.0f;
}

void SkiaCanvasComponent::resized()
{
#if JUCE_MAC
    viewHolder_.setBounds(getLocalBounds());
#endif
    lastZoom_ = zoom();
    root_.setBounds({0, 0, (float) getWidth() / lastZoom_, (float) getHeight() / lastZoom_});
    root_.markDirty();
}

float SkiaCanvasComponent::zoom() const
{
    return std::clamp(root_.context().uiZoom, 0.5f, 3.0f);
}

void SkiaCanvasComponent::drawTree(SkCanvas& canvas)
{
    canvas.scale(lastZoom_, lastZoom_);
    root_.layout(); // cheap; guarantees views rebuilt for the current song before painting
    root_.paintTree(canvas, root_.context().theme);
}

void SkiaCanvasComponent::onVBlank()
{
    if (std::fabs(zoom() - lastZoom_) > 1e-4f)
        resized(); // zoom changed in the preferences
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastTick_ >= 33.0)
    {
        lastTick_ = now;
        root_.tickTree();
        if (busyFrame_ >= 0 || root_.context().waitingForBusy()) updateCursor();
    }
    if (! root_.consumeDirty())
        return;

    if (metal_ != nullptr)
    {
        const bool ok = metal_->render(getWidth(), getHeight(), backingScale(), [this](SkCanvas& c) { drawTree(c); });
        if (ok && ++framesRendered_ == 1)
            logf("first GPU frame rendered (%dx%d @%.1fx)", getWidth(), getHeight(), (double) backingScale());
    }
    else
        repaint(); // raster path draws in paint()
}

void SkiaCanvasComponent::paint(juce::Graphics& g)
{
    if (metal_ != nullptr)
        return;

    const float scale = backingScale();
    const int pw = juce::roundToInt((float) getWidth() * scale), ph = juce::roundToInt((float) getHeight() * scale);
    if (pw <= 0 || ph <= 0)
        return;
    if (! raster_.isValid() || raster_.getWidth() != pw || raster_.getHeight() != ph)
        raster_ = juce::Image(juce::Image::ARGB, pw, ph, false, juce::SoftwareImageType());

    {
        // JUCE ARGB images on macOS are BGRA premultiplied == kBGRA_8888 / premul.
        juce::Image::BitmapData data(raster_, juce::Image::BitmapData::writeOnly);
        const auto info = SkImageInfo::Make(pw, ph, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        if (auto surface = SkSurfaces::WrapPixels(info, data.data, (size_t) data.lineStride))
        {
            auto* canvas = surface->getCanvas();
            canvas->scale(scale, scale);
            drawTree(*canvas);
        }
    }
    g.drawImage(raster_, getLocalBounds().toFloat());
}

void SkiaCanvasComponent::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    // UI-16: JUCE includes the double-click count here. Its subsequent mouseDoubleClick
    // notification must not dispatch a second edit (add/remove markers and breakpoints).
    root_.dispatchMouseDown(convert(e, zoom()));
}

void SkiaCanvasComponent::mouseDrag(const juce::MouseEvent& e)
{
    root_.dispatchMouseDrag(convert(e, zoom()));
}

void SkiaCanvasComponent::mouseUp(const juce::MouseEvent& e)
{
    root_.dispatchMouseUp(convert(e, zoom()));
}

void SkiaCanvasComponent::mouseMove(const juce::MouseEvent& e)
{
    root_.dispatchMouseMove(convert(e, zoom()));
    updateCursor();
}

void SkiaCanvasComponent::updateCursor()
{
    if (root_.context().waitingForBusy())
    {
        // UI-51: an animated arrow + activity badge while the user waits (never the system's spinning wait cursor).
        constexpr int frames = 12;
        constexpr double period = 0.9; // seconds per loop of the cached frames
        if (busyCursors_.empty())
        {
            constexpr int px = gfx::brand::busyCursorSize * 2;
            for (int i = 0; i < frames; ++i)
            {
                const auto image = gfx::brand::renderImage(px, px, 2.0f, [i](SkCanvas& c) { gfx::brand::drawBusyCursor(c, period * i / frames); });
                busyCursors_.emplace_back(juce::ScaledImage(image, 2.0), juce::Point<int>(gfx::brand::busyCursorHotSpot, gfx::brand::busyCursorHotSpot));
            }
        }
        const auto frame = (size_t) (std::fmod(juce::Time::getMillisecondCounterHiRes() / 1000.0, period) / period * frames) % busyCursors_.size();
        if ((int) frame != busyFrame_)
        {
            busyFrame_ = (int) frame;
            setMouseCursor(busyCursors_[frame]);
            if (isMouseOver()) juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
        }
        return;
    }
    const bool wasBusy = busyFrame_ >= 0;
    busyFrame_ = -1;
    switch (root_.cursor())
    {
        case ui::Cursor::ResizeHorizontal: setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); break;
        case ui::Cursor::ResizeVertical: setMouseCursor(juce::MouseCursor::UpDownResizeCursor); break;
        case ui::Cursor::Move: setMouseCursor(juce::MouseCursor::DraggingHandCursor); break;
        case ui::Cursor::Default: setMouseCursor(juce::MouseCursor::NormalCursor); break;
    }
    if (wasBusy && isMouseOver()) juce::Desktop::getInstance().getMainMouseSource().forceMouseCursorUpdate();
}

void SkiaCanvasComponent::mouseExit(const juce::MouseEvent&)
{
    root_.dispatchMouseExit();
}

bool SkiaCanvasComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    return ui::RootWidget::acceptsFileDrag(toPaths(files));
}

void SkiaCanvasComponent::fileDragMove(const juce::StringArray& files, int x, int y)
{
    const float z = zoom();
    root_.dispatchFileDragMove((float) x / z, (float) y / z, toPaths(files));
}

void SkiaCanvasComponent::fileDragExit(const juce::StringArray&)
{
    root_.dispatchFileDragExit();
}

void SkiaCanvasComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    const float z = zoom();
    ui::MouseEvent m;
    m.x = (float) x / z;
    m.y = (float) y / z;
    const auto mods = juce::ModifierKeys::getCurrentModifiersRealtime();
    m.cmd = mods.isCommandDown();
    m.shift = mods.isShiftDown();
    m.alt = mods.isAltDown();
    root_.dispatchFileDrop(m, toPaths(files));
    grabKeyboardFocus();
}

void SkiaCanvasComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto m = convert(e, zoom());
    m.wheelDeltaX = w.deltaX;
    m.wheelDeltaY = w.deltaY;
    root_.dispatchMouseWheel(m);
}

void SkiaCanvasComponent::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    root_.dispatchMouseMagnify(convert(e, zoom()), scaleFactor);
}

bool SkiaCanvasComponent::keyPressed(const juce::KeyPress& key)
{
    ui::KeyEvent k;
    k.keyCode = key.getKeyCode();
    k.textChar = (uint32_t) key.getTextCharacter();
    k.shift = key.getModifiers().isShiftDown();
    k.cmd = key.getModifiers().isCommandDown();
    // Command clears textCharacter; Option may replace it with a symbol (e.g. Option+2 → ™).
    // Shortcut matching uses the physical character key for both modifiers.
    k.alt = key.getModifiers().isAltDown();
    if ((k.cmd || k.alt) && k.keyCode >= 32 && k.keyCode < 127)
        k.textChar = (uint32_t) k.keyCode;
#if JUCE_MAC
    k.ctrl = key.getModifiers().isCtrlDown();
#else
    k.ctrl = false; // Windows/Linux: Ctrl is JUCE's command modifier and already set k.cmd (COM-7)
#endif
    return root_.dispatchUserKey(k);
}

bool SkiaCanvasComponent::keyStateChanged(bool isKeyDown)
{
    bool handled = false;
    if (! isKeyDown)
        for (int code : root_.heldLaunchKeys())
            if (! juce::KeyPress::isKeyCurrentlyDown(code)) handled |= root_.dispatchKeyUp(code);
    return handled;
}

void SkiaCanvasComponent::focusLost(FocusChangeType)
{
    root_.releaseMappedKeys();
}

bool renderToPng(ui::RootWidget& root, int width, int height, float scale, const std::string& path)
{
    const int pw = (int) std::lround((float) width * scale), ph = (int) std::lround((float) height * scale);
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pw, ph));
    if (surface == nullptr)
        return false;

    const float zoom = std::clamp(root.context().uiZoom, 0.5f, 3.0f);
    const ui::Rect logical{0, 0, (float) width / zoom, (float) height / zoom};
    root.setBounds(logical);
    root.tickTree();
    root.setBounds(logical); // second pass: views rebuilt in tick
    auto* canvas = surface->getCanvas();
    canvas->scale(scale * zoom, scale * zoom);
    root.paintTree(*canvas, root.context().theme);

    SkPixmap pixmap;
    if (! surface->peekPixels(&pixmap))
        return false;
    SkFILEWStream out(path.c_str());
    return out.isValid() && SkPngEncoder::Encode(&out, pixmap, {});
}

FrameTimes benchmarkFrames(ui::RootWidget& root, int width, int height, float scale, int frames, const std::function<void(int)>& beforeFrame)
{
    FrameTimes times;
    const int pw = (int) std::lround((float) width * scale), ph = (int) std::lround((float) height * scale);
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pw, ph));
    if (surface == nullptr)
        return times;
    const float zoom = std::clamp(root.context().uiZoom, 0.5f, 3.0f);
    const ui::Rect logical{0, 0, (float) width / zoom, (float) height / zoom};
    auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };
    for (int i = 0; i < frames; ++i)
    {
        if (beforeFrame)
            beforeFrame(i);
        const double t0 = now();
        root.setBounds(logical);
        root.tickTree();
        const double t1 = now();
        root.setBounds(logical);
        const double t2 = now();
        auto* canvas = surface->getCanvas();
        canvas->save();
        canvas->scale(scale * zoom, scale * zoom);
        root.paintTree(*canvas, root.context().theme);
        canvas->restore();
        const double t3 = now();
        times.tickMs += t1 - t0;
        times.layoutMs += t2 - t1;
        times.paintMs += t3 - t2;
        times.worstFrameMs = std::max(times.worstFrameMs, t3 - t0);
        ++times.frames;
    }
    return times;
}

} // namespace sonora::gfx
