#include "ui/views/KickstractTimeline.h"

#include "devices/RackProcessor.h"
#include "engine/EngineGraph.h"
#include "ui/Painter.h"
#include "ui/views/KickstractFace.h"
#include "ui/widgets/Buttons.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using K = devices::Kickstract;
constexpr float kHeader = 30, kAxis = 20, kLabel = 74, kGap = 6;
constexpr SkColor kScreen = 0xff15181c, kGrid = 0xff272c33, kScale = 0xff7d8590, kInk = 0xffeef1f4;
constexpr SkColor kLaneColour[K::kTimelineLanes] = {0xffc8d0da, KickstractFace::kBandColour[0], KickstractFace::kBandColour[2]};
constexpr const char* kLaneName[K::kTimelineLanes] = {"INPUT", "KICK", "REMOVED"};
constexpr double kSpanChoices[3] = {2.0, 5.0, 10.0};
constexpr int kMinSpan = 80; // 0.2 s

int binsFor(double seconds) { return (int) std::lround(seconds / K::kTimelineBinSeconds); }
} // namespace

KickstractTimeline::KickstractTimeline(devices::Kickstract* processor) : processor_(processor)
{
    freeze_ = add<TextButton>("Freeze", TextButton::Style::Toggle);
    freeze_->isOn = [this] { return frozen_; };
    freeze_->onClick = [this] { setFrozen(! frozen_); };
    freeze_->tooltip = "Freeze: stop the scrolling to study what was captured (F). The sound keeps playing.";
    live_ = add<TextButton>("Now", TextButton::Style::Plain);
    live_->onClick = [this] { offset_ = 0; repaint(); };
    live_->tooltip = "Jump back to the newest moment";
    for (int i = 0; i < 3; ++i)
    {
        char label[8];
        std::snprintf(label, sizeof label, "%.0f s", kSpanChoices[i]);
        spans_[i] = add<TextButton>(label);
        spans_[i]->isOn = [this, i] { return span_ == binsFor(kSpanChoices[i]); };
        spans_[i]->onClick = [this, i] { setSpanSeconds(kSpanChoices[i]); };
        spans_[i]->tooltip = "Show this much time (the wheel zooms freely)";
    }
}

void KickstractTimeline::setProcessor(devices::Kickstract* processor)
{
    if (processor == processor_) return;
    processor_ = processor;
    pulled_ = 0;
    if (! frozen_) bins_.clear();
    repaint();
}

void KickstractTimeline::setFrozen(bool frozen)
{
    if (frozen == frozen_) return;
    frozen_ = frozen;
    offset_ = 0;
    if (! frozen_) pull();
    repaint();
}

void KickstractTimeline::setSpanSeconds(double seconds)
{
    span_ = std::clamp(binsFor(seconds), kMinSpan, K::kTimelineBins);
    clampOffset();
    repaint();
}

double KickstractTimeline::spanSeconds() const { return span_ * K::kTimelineBinSeconds; }

void KickstractTimeline::clampOffset()
{
    offset_ = std::clamp(offset_, 0, std::max(0, (int) bins_.size() - span_));
}

void KickstractTimeline::pull()
{
    if (processor_ == nullptr) return;
    const uint64_t written = processor_->timelineWritten();
    if (written == pulled_ && ! bins_.empty()) return;
    const uint64_t count = std::min<uint64_t>(written, (uint64_t) K::kTimelineBins);
    bins_.resize((size_t) count);
    for (uint64_t i = 0; i < count; ++i) bins_[(size_t) i] = processor_->timelineBin(written - count + i);
    pulled_ = written;
}

void KickstractTimeline::layout()
{
    const float y = (kHeader - 20) * 0.5f;
    freeze_->setBounds({kLabel + 112, y, 64, 20});
    live_->setBounds({kLabel + 180, y, 44, 20});
    for (int i = 0; i < 3; ++i) spans_[i]->setBounds({width() - 3 * 46.0f + (float) i * 46, y, 42, 20});
}

Rect KickstractTimeline::laneArea(int lane) const
{
    const float top = kHeader, bottom = std::max(top + 30, height() - kAxis);
    constexpr float lanes = (float) K::kTimelineLanes;
    const float laneHeight = (bottom - top - (lanes - 1) * kGap) / lanes;
    return {kLabel, top + (float) lane * (laneHeight + kGap), std::max(10.0f, width() - kLabel - 2), laneHeight};
}

double KickstractTimeline::secondsAt(float x) const
{
    const auto area = laneArea(0);
    const double fraction = std::clamp((double) ((x - area.x) / area.w), 0.0, 1.0);
    return -((1.0 - fraction) * span_ + offset_) * K::kTimelineBinSeconds;
}

int KickstractTimeline::hitsShown() const
{
    const int end = (int) bins_.size() - offset_, begin = std::max(0, end - span_);
    int hits = 0;
    for (int i = begin; i < end; ++i) hits += bins_[(size_t) i].hit ? 1 : 0;
    return hits;
}

void KickstractTimeline::tick()
{
    if (frozen_ || processor_ == nullptr) return;
    const auto before = pulled_;
    pull();
    if (pulled_ != before) repaint();
}

void KickstractTimeline::mouseDown(const MouseEvent& e)
{
    dragX_ = e.x;
    dragOffset_ = offset_;
    // Grabbing the picture while it scrolls freezes it: what you clicked stays put.
    if (! frozen_ && laneArea(0).y <= e.y && e.y <= height() - kAxis) setFrozen(true);
}

void KickstractTimeline::mouseDrag(const MouseEvent& e)
{
    if (! frozen_) return;
    const float binsPerPixel = (float) span_ / laneArea(0).w;
    offset_ = dragOffset_ + (int) std::lround((e.x - dragX_) * binsPerPixel);
    clampOffset();
    hoverX_ = e.x;
    repaint();
}

void KickstractTimeline::mouseMove(const MouseEvent& e)
{
    hoverX_ = e.x >= kLabel ? e.x : -1;
    repaint();
}

void KickstractTimeline::mouseExit()
{
    hoverX_ = -1;
    repaint();
}

void KickstractTimeline::mouseDoubleClick(const MouseEvent&) { setFrozen(! frozen_); }

bool KickstractTimeline::mouseWheel(const MouseEvent& e)
{
    if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY) && frozen_)
    {
        offset_ += (int) std::lround(e.wheelDeltaX * (float) span_ * 0.5f);
        clampOffset();
        repaint();
        return true;
    }
    if (std::abs(e.wheelDeltaY) < 1e-6f) return false;
    // Zoom around the pointer: the moment under it stays under it.
    const auto area = laneArea(0);
    const float fraction = std::clamp((e.x - area.x) / area.w, 0.0f, 1.0f);
    const double anchor = (1.0 - fraction) * span_ + offset_;
    const int span = std::clamp((int) std::lround(span_ * std::pow(2.0, -e.wheelDeltaY * 2.0)), kMinSpan, K::kTimelineBins);
    if (frozen_) offset_ = (int) std::lround(anchor - (1.0 - fraction) * span);
    span_ = span;
    clampOffset();
    repaint();
    return true;
}

bool KickstractTimeline::keyPressed(const KeyEvent& k)
{
    if (k.cmd || k.alt || k.ctrl) return false;
    if (k.textChar == 'f' || k.textChar == 'F')
    {
        setFrozen(! frozen_);
        return true;
    }
    return false;
}

void KickstractTimeline::paint(SkCanvas& c, const Theme& t)
{
    const auto fg = t.color("ControlForeground"), dim = t.color("TextDisabled");
    paint::text(c, "TIMELINE", {2, 0, 100, kHeader}, fg, t.metrics.fontSize, Align::Left, true);
    paint::text(c, frozen_ ? "frozen: drag to move, wheel to zoom" : "live", {kLabel + 230, 0, 260, kHeader},
                frozen_ ? KickstractFace::kBandColour[1] : dim, t.metrics.smallFontSize, Align::Left);
    if (hoverX_ >= 0)
    {
        char at[32];
        std::snprintf(at, sizeof at, "%.3f s", secondsAt(hoverX_));
        paint::text(c, at, {width() - 3 * 46.0f - 130, 0, 120, kHeader}, fg, t.metrics.smallFontSize, Align::Right);
    }
    for (int lane = 0; lane < K::kTimelineLanes; ++lane) paintLane(c, t, lane);

    // Time axis: seconds back from the newest moment shown.
    const auto first = laneArea(0), last = laneArea(K::kTimelineLanes - 1);
    const double spanS = spanSeconds();
    const double step = spanS > 6 ? 1.0 : spanS > 2.5 ? 0.5 : spanS > 1 ? 0.25 : 0.1;
    const double newest = -offset_ * K::kTimelineBinSeconds, oldest = newest - spanS;
    for (double s = std::ceil(oldest / step) * step; s <= newest + 1e-9; s += step)
    {
        const float x = first.x + (float) ((s - oldest) / spanS) * first.w;
        char label[16];
        std::snprintf(label, sizeof label, step < 0.25 ? "%.1f" : "%.2f", s);
        const float left = std::clamp(x - 30, first.x - 30, first.x + first.w - 60); // the edge labels stay inside
        paint::text(c, label, {left, last.y + last.h + 2, 60, kAxis - 4}, kScale, t.metrics.smallFontSize - 1,
                    left > x - 30 ? Align::Left : left < x - 30 ? Align::Right : Align::Centre);
    }
    if (processor_ == nullptr)
        paint::text(c, "Device unavailable", first, dim, t.metrics.fontSize, Align::Centre);
}

void KickstractTimeline::paintLane(SkCanvas& c, const Theme& t, int lane)
{
    const auto r = laneArea(lane);
    const SkColor colour = kLaneColour[lane];
    paint::fillRounded(c, r, 3, kScreen);
    paint::fill(c, {0, r.y, 3, r.h}, colour);
    paint::text(c, kLaneName[lane], {8, r.y, kLabel - 10, r.h}, t.color("ControlForeground"), t.metrics.smallFontSize,
                Align::Left, true);

    const int end = (int) bins_.size() - offset_, begin = end - span_;
    const double spanS = spanSeconds();
    const double step = spanS > 6 ? 1.0 : spanS > 2.5 ? 0.5 : spanS > 1 ? 0.25 : 0.1;
    const double newest = -offset_ * K::kTimelineBinSeconds, oldest = newest - spanS;
    for (double s = std::ceil(oldest / step) * step; s <= newest + 1e-9; s += step)
    {
        const float x = r.x + (float) ((s - oldest) / spanS) * r.w;
        paint::line(c, x, r.y, x, r.y + r.h, kGrid);
    }
    const float mid = r.y + r.h * 0.5f, half = r.h * 0.5f - 2;
    paint::line(c, r.x, mid, r.x + r.w, mid, kGrid);
    // -6 dB guides (half scale).
    paint::line(c, r.x, mid - half * 0.5f, r.x + r.w, mid - half * 0.5f, paint::withAlpha(kGrid, 140));
    paint::line(c, r.x, mid + half * 0.5f, r.x + r.w, mid + half * 0.5f, paint::withAlpha(kGrid, 140));

    c.save();
    c.clipRect(SkRect::MakeXYWH(r.x, r.y, r.w, r.h));
    // Hits: one line across all lanes where the gates opened.
    for (int i = std::max(0, begin); i < end; ++i)
        if (bins_[(size_t) i].hit)
        {
            const float x = r.x + (float) (i - begin) / (float) span_ * r.w;
            paint::line(c, x, r.y, x, r.y + r.h, paint::withAlpha(KickstractFace::kBandColour[0], 120));
        }
    // The waveform: one min/max column per pixel.
    SkPathBuilder path;
    const int columns = std::max(1, (int) r.w);
    for (int px = 0; px < columns; ++px)
    {
        const int b0 = begin + (int) std::floor((double) px / columns * span_);
        const int b1 = std::max(b0 + 1, begin + (int) std::floor((double) (px + 1) / columns * span_));
        float lo = 0, hi = 0;
        bool any = false;
        for (int i = std::max(0, b0); i < std::min(end, b1); ++i)
        {
            const auto& bin = bins_[(size_t) i];
            lo = any ? std::min(lo, bin.min[lane]) : bin.min[lane];
            hi = any ? std::max(hi, bin.max[lane]) : bin.max[lane];
            any = true;
        }
        if (! any) continue;
        const float yTop = mid - std::clamp(hi, -1.0f, 1.0f) * half, yBottom = mid - std::clamp(lo, -1.0f, 1.0f) * half;
        path.addRect(SkRect::MakeLTRB(r.x + (float) px, yTop, r.x + (float) px + 1.0f, std::max(yBottom, yTop + 1.0f)));
    }
    SkPaint brush;
    brush.setAntiAlias(false);
    brush.setColor(colour);
    c.drawPath(path.detach(), brush);
    if (hoverX_ >= r.x && hoverX_ <= r.x + r.w) paint::line(c, hoverX_, r.y, hoverX_, r.y + r.h, paint::withAlpha(kInk, 150));
    c.restore();
    if (frozen_ && lane == 0) paint::text(c, "FROZEN", {r.x + r.w - 70, r.y + 2, 64, 14}, KickstractFace::kBandColour[1],
                                          t.metrics.smallFontSize - 1, Align::Right, true);
}

// ================================================================== the app's window
std::optional<KickstractTimelineWindowView::Site> KickstractTimelineWindowView::find(const UiContext& x,
                                                                                     const UiContext::WavetableKey& key)
{
    for (int index = 0; index < x.numControlTracks(); ++index)
    {
        const auto* track = x.trackAt(index);
        if (track == nullptr || track->id != key.trackId) continue;
        for (size_t d = 0; d < track->devices.size(); ++d)
        {
            const auto& top = track->devices[d];
            if (top.id != key.deviceId) continue;
            Site site;
            site.track = index;
            devices::DeviceProcessor* processor = nullptr;
            if (x.graph != nullptr && (size_t) index < x.graph->nodes.size()
                && d < x.graph->nodes[(size_t) index].processorForModelDevice.size())
                processor = x.graph->nodes[(size_t) index].processorForModelDevice[d];
            if (key.chain < 0) site.model = &top;
            else
            {
                if ((size_t) key.chain >= top.chains.size()) return std::nullopt;
                const auto& chain = top.chains[(size_t) key.chain].devices;
                size_t child = chain.size();
                for (size_t c = 0; c < chain.size(); ++c)
                    if (chain[c].id == key.childId) { child = c; site.model = &chain[c]; }
                if (site.model == nullptr) return std::nullopt;
                auto* rack = dynamic_cast<devices::RackProcessor*>(processor);
                processor = nullptr;
                if (rack && (size_t) key.chain < rack->chains().size())
                {
                    const auto& processors = rack->chains()[(size_t) key.chain].processorForModelDevice;
                    if (child < processors.size()) processor = processors[child];
                }
            }
            if (site.model->type != "SonoraKickstract") return std::nullopt;
            site.processor = dynamic_cast<devices::Kickstract*>(processor);
            return site;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

KickstractTimelineWindowView::KickstractTimelineWindowView(UiContext& context, UiContext::WavetableKey key)
    : RootWidget(context), key_(key)
{
}

void KickstractTimelineWindowView::setKey(const UiContext::WavetableKey& key)
{
    if (key == key_) return;
    key_ = key;
    built_ = false;
}

bool KickstractTimelineWindowView::stale() const
{
    const auto& x = context();
    return ! built_ || builtSong_ != x.song || builtGraph_ != x.graph || builtStructure_ != x.trackStructure();
}

void KickstractTimelineWindowView::sync()
{
    if (stale()) rebuild();
    layout();
    markDirty();
}

void KickstractTimelineWindowView::rebuild()
{
    auto& x = context();
    built_ = true;
    builtSong_ = x.song;
    builtGraph_ = x.graph;
    builtStructure_ = x.trackStructure();
    if (x.song == nullptr)
    {
        if (timeline_) timeline_->setProcessor(nullptr);
        return;
    }
    const auto site = find(x, key_);
    if (! site)
    {
        if (timeline_) timeline_->setProcessor(nullptr);
        if (! goneSent_ && onGone) { goneSent_ = true; onGone(); }
        return;
    }
    std::string title = (site->model->displayName.empty() ? std::string("Sonora Kickstract") : site->model->displayName) + " Timeline";
    if (const auto* track = x.trackAt(site->track); track && ! track->name.empty()) title += "/" + track->name;
    if (title != title_)
    {
        title_ = title;
        if (onTitleChanged) onTitleChanged(title_);
    }
    // The timeline survives rebuilds (a frozen picture stays); only its processor changes.
    if (timeline_ == nullptr) timeline_ = add<KickstractTimeline>(site->processor);
    else timeline_->setProcessor(site->processor);
}

void KickstractTimelineWindowView::layout()
{
    if (stale()) rebuild();
    if (timeline_ != nullptr) timeline_->setBounds(localBounds().reduced(8));
}

void KickstractTimelineWindowView::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("SurfaceBackground"));
}

void KickstractTimelineWindowView::tick()
{
    if (stale())
    {
        layout();
        markDirty();
    }
    RootWidget::tick();
}
} // namespace sonora::ui
