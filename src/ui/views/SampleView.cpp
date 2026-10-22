#include "ui/views/SampleView.h"

#include "ui/WheelScale.h"

#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "engine/ClipPlayers.h"
#include "engine/SampleCache.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "core/TimeUtils.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace sonora::ui
{

namespace
{
// One channel of the waveform. Zoomed in far enough it draws the real samples as a smooth thick line; at
// intermediate zooms it scans the decoded audio for each pixel column's extremes; zoomed out it uses the
// min/max pyramid. The envelope is filled with anti-aliasing so it stays crisp at any size.
void drawChannel(SkCanvas& c, const engine::Peaks& peaks, int channel, const Rect& area, double scroll, double secondsPerPixel,
                 float gain, SkColor color)
{
    const float half = std::max(2.0f, area.h * 0.5f - 3.0f), mid = area.centreY();
    const double sr = peaks.sampleRate, framesPerPixel = secondsPerPixel * sr;
    const auto* source = peaks.source.get();
    const int frames = source != nullptr ? source->audio.getNumSamples() : 0;
    const float* data = source != nullptr ? source->audio.getReadPointer(std::min(channel, source->audio.getNumChannels() - 1)) : nullptr;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color);

    if (data != nullptr && framesPerPixel <= 1.0)
    {
        const int first = std::max(0, (int) std::floor(scroll * sr) - 1);
        const int last = std::min(frames - 1, (int) std::ceil((scroll + area.w * secondsPerPixel) * sr) + 1);
        SkPathBuilder path;
        for (int i = first; i <= last; ++i)
        {
            const float x = area.x + (float) (((double) i / sr - scroll) / secondsPerPixel);
            const float y = mid - std::clamp(data[i] * gain, -1.0f, 1.0f) * half;
            if (i == first) path.moveTo(x, y); else path.lineTo(x, y);
        }
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(2.0f);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        c.drawPath(path.detach(), paint);
        if (framesPerPixel < 1.0 / 8.0) // each sample is a dot once there is room for it
        {
            paint.setStyle(SkPaint::kFill_Style);
            for (int i = first; i <= last; ++i)
                c.drawCircle(area.x + (float) (((double) i / sr - scroll) / secondsPerPixel),
                             mid - std::clamp(data[i] * gain, -1.0f, 1.0f) * half, 2.2f, paint);
        }
        return;
    }

    const int columns = (int) std::ceil(area.w);
    std::vector<float> lo((size_t) columns, 0.0f), hi((size_t) columns, 0.0f);
    std::vector<char> valid((size_t) columns, 0);
    for (int x = 0; x < columns; ++x)
    {
        const double t0 = scroll + x * secondsPerPixel, t1 = t0 + secondsPerPixel;
        float a = 0, b = 0;
        if (data != nullptr && framesPerPixel <= 4096.0)
        {
            const int f0 = (int) std::floor(t0 * sr), f1 = (int) std::floor(t1 * sr);
            if (f1 < 0 || f0 >= frames) continue;
            a = b = data[std::clamp(f0, 0, frames - 1)];
            for (int f = std::max(0, f0); f <= std::min(frames - 1, f1); ++f) { a = std::min(a, data[f]); b = std::max(b, data[f]); }
        }
        else if (! peaks.range(t0, t1, a, b, channel))
            continue;
        lo[(size_t) x] = std::clamp(a * gain, -1.0f, 1.0f);
        hi[(size_t) x] = std::clamp(b * gain, -1.0f, 1.0f);
        valid[(size_t) x] = 1;
    }
    SkPathBuilder path;
    bool open = false;
    // Contiguous runs of valid columns become closed shapes: upper contour left to right, lower contour back.
    int runStart = 0;
    for (int x = 0; x <= columns; ++x)
    {
        if (x < columns && valid[(size_t) x])
        {
            if (! open) { runStart = x; open = true; path.moveTo(area.x + (float) x, mid - hi[(size_t) x] * half); }
            const float top = mid - hi[(size_t) x] * half, bottom = mid - lo[(size_t) x] * half;
            path.lineTo(area.x + (float) x + 0.5f, std::min(top, bottom - 0.35f));
        }
        else if (open)
        {
            // walk back along the bottom edge of this run
            for (int y = x - 1; y >= runStart; --y)
            {
                const float top = mid - hi[(size_t) y] * half, bottom = std::max(mid - lo[(size_t) y] * half, top + 0.35f);
                path.lineTo(area.x + (float) y + 0.5f, bottom);
            }
            path.close();
            open = false;
        }
    }
    paint.setStyle(SkPaint::kFill_Style);
    c.drawPath(path.detach(), paint);
}

std::vector<model::WarpMarker> inverseMarkers(const model::Clip& clip)
{
    std::vector<model::WarpMarker> inverse;
    for (const auto& marker : clip.warpMarkers) inverse.push_back({marker.beats, marker.seconds});
    return inverse;
}

std::string timeLabel(double seconds, double step)
{
    char text[32];
    const int minutes = (int) (seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    if (step >= 1.0)
        std::snprintf(text, sizeof(text), "%d:%02d", minutes, (int) std::lround(rest));
    else
        std::snprintf(text, sizeof(text), "%d:%02d.%03d", minutes, (int) rest, (int) std::lround((rest - std::floor(rest)) * 1000.0));
    return text;
}

// "44.1 kHz · 16-Bit · 2 Ch". Compressed files report what they decode to; like Live, they are shown as 16-bit.
std::string formatText(const engine::SampleData* source, double sampleRate, const std::string& path)
{
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%g kHz", std::round(sampleRate / 100.0) / 10.0);
    std::string text = rate;
    if (source == nullptr) return text;
    const auto extension = juce::File(juce::String::fromUTF8(path.c_str())).getFileExtension().toLowerCase();
    const bool lossy = extension == ".mp3" || extension == ".m4a" || extension == ".aac" || extension == ".mp4" || extension == ".ogg"
                       || extension == ".m4r" || extension == ".m4b" || extension == ".3gp" || extension == ".ac3" || extension == ".amr";
    const int bits = lossy ? 16 : source->fileBits;
    if (bits > 0) text += " \xC2\xB7 " + std::to_string(bits) + (source->fileFloat && ! lossy ? "-Bit Float" : "-Bit");
    text += " \xC2\xB7 " + std::to_string(source->fileChannels) + " Ch";
    return text;
}
} // namespace

Rect SampleView::waveArea() const
{
    const float top = warpRowY() + kWarpRowHeight + 1;
    return {kSide, top, std::max(1.0f, width() - 2 * kSide), std::max(1.0f, height() - top - 2)};
}

double SampleView::playheadSeconds(const model::Clip& clip) const
{
    const auto* transport = ctx().transport;
    if (! transport || ! transport->isPlaying() || ! ctx().song) return -1;
    const double secPerBeat = ctx().song->tempo > 0 ? 60.0 / ctx().song->tempo : 0.5;
    const auto& loop = clip.loop;
    if (const int scene = ctx().selectedClipScene; scene >= 0)
    {
        // Session clip (UI-17c): the engine publishes the playing slot and the beats since its launch; the clip's own
        // timeline starts at 0 there (engine::prepareSessionClip), and a one-shot ends after its start-to-end length.
        const auto& controls = ctx().controls;
        const int track = ctx().selectedClipTrack;
        if (! controls || track < 0 || (size_t) track >= controls->size()) return -1;
        const auto& c = (*controls)[(size_t) track];
        if (c.playingSlot.load(std::memory_order_relaxed) != scene) return -1;
        const double elapsed = std::max(0.0, c.sessionElapsed.load(std::memory_order_relaxed));
        if (! loop.enabled && elapsed >= std::max(loop.loopEnd, clip.currentEnd) - (loop.loopStart + loop.startRelative)) return -1;
        if (clip.warped)
            return engine::warpBeatsToSeconds(clip.warpMarkers,
                model::clipContentPosition(elapsed, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled), secPerBeat);
        return model::clipContentPosition(elapsed * secPerBeat, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
    }
    const double beat = transport->positionBeats();
    if (beat < clip.currentStart || beat >= clip.currentEnd) return -1;
    if (clip.warped)
        return engine::warpBeatsToSeconds(clip.warpMarkers,
            model::clipContentPosition(beat, clip.currentStart, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled), secPerBeat);
    // Unwarped: the loop is in source seconds; elapsed timeline beats become seconds at the set tempo.
    return model::clipContentPosition((beat - clip.currentStart) * secPerBeat, 0, loop.loopStart, loop.loopEnd, loop.startRelative, loop.enabled);
}

void SampleView::paint(SkCanvas& canvas, const Theme& theme)
{
    paint::fill(canvas, localBounds(), theme.color("SurfaceBackground"));
    const auto& context = ctx();
    const auto* selected = context.selectedClip();
    if (! selected || selected->kind != model::Clip::Kind::Audio) return;
    const auto* clip = (marker_ >= 0 || edge_ != 0) && selected->uid == original_.uid ? &working_ : selected;
    auto peaks = context.peaks ? context.peaks(*clip) : nullptr;
    double duration = peaks ? peaks->lengthSeconds() : clip->sampleRate > 0 ? (double) clip->lengthSamples / clip->sampleRate : 1;
    duration = std::max(0.001, duration);
    const Rect wave = waveArea();
    if (lastUid_ != clip->uid)
    {
        lastUid_ = clip->uid;
        scrollSeconds_ = 0;
        secondsPerPixel_ = duration / std::max(1.0f, wave.w);
    }
    const SkColor fg = theme.color("ControlForeground"), frame = theme.color("SurfaceArea");
    const double secPerBeat = context.song && context.song->tempo > 0 ? 60.0 / context.song->tempo : 0.5;
    const double playhead = playheadSeconds(*clip);
    if (context.sampleFollow && playhead >= 0 && (playhead < scrollSeconds_ || playhead > scrollSeconds_ + wave.w * secondsPerPixel_ * 0.9))
        scrollSeconds_ = std::max(0.0, playhead - wave.w * secondsPerPixel_ * 0.1); // page like Live's Follow

    // ---- Header: waveform icon, file name and format; the Sample tab (clip envelopes for audio are not implemented)
    const Rect icon{6, 3, 16, 12};
    ctl::framed(canvas, theme, icon, theme.color("ControlTextBack"));
    const float bars[] = {3, 6, 8, 5, 3};
    for (int i = 0; i < 5; ++i)
        paint::fill(canvas, {icon.x + 3.5f + (float) i * 2.0f, icon.centreY() - bars[i] * 0.5f, 1.0f, bars[i]}, fg);
    const std::string filePath = clip->sample.absolutePath;
    const auto fileName = juce::File(juce::String::fromUTF8(filePath.c_str())).getFileName().toStdString();
    const auto format = formatText(peaks ? peaks->source.get() : nullptr, peaks ? peaks->sampleRate : clip->sampleRate, filePath);
    paint::text(canvas, (fileName.empty() ? clip->name : fileName) + "  " + format, {icon.right() + 6, 0, width() - 180, kHeaderHeight}, fg,
                theme.metrics.fontSize + 0.5f, Align::Left);
    const Rect sampleTab{width() - 132, 1, 64, kHeaderHeight - 2}, envelopeTab{width() - 66, 1, 64, kHeaderHeight - 2};
    paint::fill(canvas, sampleTab, theme.color("SurfaceHighlight"));
    paint::text(canvas, "Sample", sampleTab, fg, theme.metrics.fontSize, Align::Centre);
    paint::text(canvas, "Envelopes", envelopeTab, theme.color("TextDisabled"), theme.metrics.fontSize, Align::Centre);
    paint::line(canvas, 0, kHeaderHeight - 0.5f, width(), kHeaderHeight - 0.5f, frame);

    // ---- Ruler and marker rows
    const Rect ruler{wave.x, kHeaderHeight, wave.w, kRulerHeight};
    const Rect startRow{wave.x, startRowY(), wave.w, kStartRowHeight}, loopRow{wave.x, loopRowY(), wave.w, kLoopRowHeight},
               warpRow{wave.x, warpRowY(), wave.w, kWarpRowHeight};
    paint::fill(canvas, {0, ruler.y, width(), ruler.h}, theme.color("DetailViewBackground"));
    paint::fill(canvas, {0, warpRow.y, width(), warpRow.h}, paint::mix(theme.color("SurfaceBackground"), theme.color("SurfaceArea"), 0.3f));
    for (float y : {startRow.y, loopRow.y, warpRow.y, wave.y - 1})
        paint::line(canvas, 0, y - 0.5f, width(), y - 0.5f, frame);

    // Grid: bars of the clip's beat time when warped (mapped through the warp markers), seconds otherwise.
    struct Tick { float x; std::string label; bool major; };
    std::vector<Tick> ticks;
    const double viewEnd = scrollSeconds_ + wave.w * secondsPerPixel_;
    if (clip->warped)
    {
        const int perBar = std::max(1, clip->timeSigNumerator * 4 / std::max(1, clip->timeSigDenominator));
        const double beatsPerPixel = secondsPerPixel_ / secPerBeat * std::max(0.05, clip->warpMarkers.size() >= 2
            ? (clip->warpMarkers.back().beats - clip->warpMarkers.front().beats) * secPerBeat
                  / std::max(1e-6, clip->warpMarkers.back().seconds - clip->warpMarkers.front().seconds)
            : 1.0);
        int barStep = 1;
        while (barStep * perBar / beatsPerPixel < 46 && barStep < 4096) barStep *= 2;
        const double firstBeat = secondsToUnits(*clip, scrollSeconds_), lastBeat = secondsToUnits(*clip, viewEnd);
        const double stepBeats = (double) barStep * perBar;
        const double minor = barStep >= 2 ? stepBeats / 2 : (perBar / beatsPerPixel > 60 ? 1.0 : stepBeats);
        for (double b = std::floor(firstBeat / minor) * minor; b <= lastBeat + minor; b += minor)
        {
            const float x = secondsToX(engine::warpBeatsToSeconds(clip->warpMarkers, b, secPerBeat));
            if (x < wave.x - 1 || x > wave.right() + 1) continue;
            const bool major = std::fabs(std::remainder(b, stepBeats)) < 1e-6;
            const int bar = (int) std::floor(b / perBar + 1e-9) + 1;
            ticks.push_back({x, major ? std::to_string(bar) : std::string{}, major});
        }
    }
    else
    {
        const double step = std::pow(10, std::ceil(std::log10(std::max(0.001, secondsPerPixel_ * 60))));
        for (double time = std::ceil(scrollSeconds_ / step) * step; time < viewEnd; time += step)
            ticks.push_back({secondsToX(time), timeLabel(time, step), true});
    }
    canvas.save();
    canvas.clipRect(Rect{wave.x, ruler.y, wave.w, height() - ruler.y}.sk());
    for (const auto& tick : ticks)
    {
        if (tick.major)
        {
            // Live's bar label: a short corner tick with the number beside it.
            paint::line(canvas, tick.x + 0.5f, ruler.bottom() - 7, tick.x + 0.5f, ruler.bottom(), fg);
            paint::line(canvas, tick.x, ruler.bottom() - 7.5f, tick.x + 3, ruler.bottom() - 7.5f, fg);
            paint::text(canvas, tick.label, {tick.x + 4, ruler.y + 1, 60, ruler.h - 4}, theme.color("GridLabel"), theme.metrics.fontSize, Align::Left);
        }
        else
            paint::fill(canvas, {tick.x - 0.5f, ruler.bottom() - 3, 1, 1}, fg);
        paint::line(canvas, tick.x, loopRow.y, tick.x, warpRow.bottom(), paint::withAlpha(frame, tick.major ? 140 : 70));
    }
    canvas.restore();

    // ---- Waveform
    const auto toSeconds = [&](double v) { return clip->warped ? engine::warpBeatsToSeconds(clip->warpMarkers, v, secPerBeat) : v; };
    const double startSeconds = toSeconds(clip->loop.loopStart + clip->loop.startRelative);
    const double endSeconds = toSeconds(clip->loop.loopEnd);
    const float xStart = secondsToX(startSeconds), xEnd = secondsToX(endSeconds);
    paint::fill(canvas, wave, theme.color("DetailViewBackground"));
    canvas.save();
    canvas.clipRect(wave.sk());
    for (const auto& tick : ticks)
        paint::line(canvas, tick.x, wave.y, tick.x, wave.bottom(), paint::withAlpha(theme.color("GridLineBase"), tick.major ? 70 : 35));
    canvas.restore();
    if (! peaks)
        paint::text(canvas, "Sample unavailable or waveform loading", wave, theme.color("TextDisabled"), theme.metrics.fontSize, Align::Centre);
    else
    {
        const float gain = (float) clip->gain * verticalZoom_;
        for (int channel = 0; channel < 2; ++channel)
        {
            const Rect area{wave.x, wave.y + wave.h * (float) channel * 0.5f, wave.w, wave.h * 0.5f};
            canvas.save();
            canvas.clipRect(area.sk());
            drawChannel(canvas, *peaks, channel, area, scrollSeconds_, secondsPerPixel_, gain, theme.color("WaveformColor"));
            canvas.restore();
        }
    }
    paint::line(canvas, wave.x, wave.centreY(), wave.right(), wave.centreY(), frame);
    canvas.save();
    canvas.clipRect(wave.sk());
    // Outside the clip's start / end the waveform is dimmed, like Live's sample editor.
    const SkColor shade = paint::withAlpha(theme.color("SurfaceArea"), 90);
    if (xStart > wave.x) paint::fill(canvas, {wave.x, wave.y, std::min(xStart, wave.right()) - wave.x, wave.h}, shade);
    if (xEnd < wave.right()) paint::fill(canvas, {std::max(xEnd, wave.x), wave.y, wave.right() - std::max(xEnd, wave.x), wave.h}, shade);
    canvas.restore();

    // ---- Markers (clipped to the editor's width)
    canvas.save();
    canvas.clipRect(Rect{0, startRow.y, width(), height() - startRow.y}.sk());
    const SkColor ink = theme.color("ControlContrastFrame");
    auto outlineTriangle = [&](float x, bool right, SkColor fill) {
        SkPathBuilder path;
        const float top = startRow.y + 1, bottom = startRow.bottom() - 1, tip = right ? x + 8 : x - 8;
        path.moveTo(x, top); path.lineTo(tip, startRow.centreY()); path.lineTo(x, bottom); path.close();
        SkPaint p; p.setAntiAlias(true); p.setColor(fill);
        auto shape = path.detach();
        canvas.drawPath(shape, p);
        p.setStyle(SkPaint::kStroke_Style); p.setColor(ink);
        canvas.drawPath(shape, p);
    };
    outlineTriangle(xStart, true, theme.color("ControlTextBack"));
    outlineTriangle(xEnd, false, theme.color("ControlTextBack"));
    // Loop row: the loop brace while looping, and the position (loop start) marker.
    const float xLoopStart = secondsToX(toSeconds(clip->loop.loopStart));
    if (clip->loop.enabled)
        ctl::loopBrace(canvas, theme, {xLoopStart, loopRow.y + 2, std::max(2.0f, xEnd - xLoopStart), loopRow.h - 4}, true);
    {
        SkPathBuilder path;
        path.moveTo(xLoopStart, loopRow.y + 1); path.lineTo(xLoopStart + 8, loopRow.centreY()); path.lineTo(xLoopStart, loopRow.bottom() - 1); path.close();
        SkPaint p; p.setAntiAlias(true); p.setColor(theme.color("RangeDefault"));
        auto shape = path.detach();
        canvas.drawPath(shape, p);
        p.setStyle(SkPaint::kStroke_Style); p.setColor(ink);
        canvas.drawPath(shape, p);
    }
    // Transients: small ticks at the bottom of the warp row, only once they are far enough apart to read.
    if (clip->warped && peaks && peaks->source != nullptr)
    {
        const auto& onsets = peaks->source->onsets;
        const double visible = std::max(1.0, (double) (std::upper_bound(onsets.begin(), onsets.end(), viewEnd)
                                                      - std::lower_bound(onsets.begin(), onsets.end(), scrollSeconds_)));
        if (wave.w / visible >= 6.0)
            for (double onset : onsets)
            {
                const float x = secondsToX(onset);
                if (x < wave.x || x > wave.right()) continue;
                paint::line(canvas, x, warpRow.bottom() - 4, x, warpRow.bottom() - 1, theme.color("TextDisabled"));
                paint::line(canvas, x, warpRow.bottom() - 4, x + 2, warpRow.bottom() - 4, theme.color("TextDisabled"));
            }
    }
    // Warp markers: yellow tabs in the warp row; the one being dragged also shows its line through the waveform.
    if (clip->warped)
        for (size_t i = 0; i < clip->warpMarkers.size(); ++i)
        {
            const float x = secondsToX(clip->warpMarkers[i].seconds);
            if (x < wave.x - 6 || x > wave.right() + 6) continue;
            const Rect tab{x, warpRow.y + 1, 6, warpRow.h - 2};
            paint::fill(canvas, tab, theme.color("ChosenDefault"));
            paint::stroke(canvas, tab, ink);
            if ((int) i == marker_) paint::line(canvas, x, wave.y, x, wave.bottom(), theme.color("ChosenDefault"));
        }
    if (playhead >= 0)
    {
        const float x = secondsToX(playhead);
        if (x >= wave.x && x <= wave.right()) paint::line(canvas, x, startRow.y, x, wave.bottom(), fg);
    }
    canvas.restore();
}

// Source seconds -> the clip's loop units (beats when warped, seconds otherwise).
double SampleView::secondsToUnits(const model::Clip& clip, double seconds) const
{
    if (! clip.warped)
        return seconds;
    const double tempo = ctx().song && ctx().song->tempo > 0 ? ctx().song->tempo : 120.0;
    return engine::warpBeatsToSeconds(inverseMarkers(clip), seconds, tempo / 60);
}

void SampleView::showContextMenu(const MouseEvent& event, const model::Clip& clip)
{
    juce::PopupMenu menu;
    const auto location = ctx().selectedClipLocation();
    std::vector<std::function<void()>> actions;
    auto item = [&](const char* label, bool enabled, std::function<void()> action) {
        actions.push_back(std::move(action));
        menu.addItem((int) actions.size(), label, enabled);
    };
    const double duration = clip.sampleRate > 0 ? (double) clip.lengthSamples / clip.sampleRate : 0.0;
    double seconds = std::clamp(xToSeconds(event.x), 0.0, std::max(0.0, duration));
    auto peaks = ctx().peaks ? ctx().peaks(clip) : nullptr;
    if (peaks && peaks->source)
        for (double onset : peaks->source->onsets)
            if (std::fabs(onset - seconds) < 8.0 * secondsPerPixel_) { seconds = onset; break; }
    int nearest = -1;
    double best = 12.0 * secondsPerPixel_;
    for (size_t i = 0; i < clip.warpMarkers.size(); ++i)
        if (std::fabs(clip.warpMarkers[i].seconds - seconds) < best) { best = std::fabs(clip.warpMarkers[i].seconds - seconds); nearest = (int) i; }
    const auto base = clip;
    const double tempo = ctx().song ? ctx().song->tempo : 120.0;
    auto edit = [this, base, location](std::function<void(model::Clip&)> change, const char* label) {
        return [this, base, location, change, label] {
            if (! Widget::isAlive(this) || ! ctx().song || ! ctx().applyEdit || ! model::findClip(*ctx().song, location)) return;
            auto c = base;
            change(c);
            c.resetSourceMarkers = true;
            ctx().applyEdit(model::modifyClip(*ctx().song, location, std::move(c), label));
        };
    };
    if (clip.warped)
    {
        item("Insert Warp Marker", true, edit([this, seconds](model::Clip& c) {
            for (const auto& m : c.warpMarkers) if (std::fabs(m.seconds - seconds) < 1e-6) return;
            const double beats = engine::warpBeatsToSeconds(inverseMarkers(c), seconds, ctx().song->tempo / 60);
            c.warpMarkers.push_back({seconds, beats});
            std::sort(c.warpMarkers.begin(), c.warpMarkers.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
        }, "Insert Warp Marker"));
        item("Delete Warp Marker", nearest >= 0 && clip.warpMarkers.size() > 2,
             edit([nearest](model::Clip& c) { c.warpMarkers.erase(c.warpMarkers.begin() + nearest); }, "Delete Warp Marker"));
        item("Set 1.1.1 Here", clip.warpMarkers.size() >= 2, edit([seconds](model::Clip& c) {
            // Shifts every marker so the point under the cursor becomes beat 0 (the clip's downbeat).
            const double here = engine::warpBeatsToSeconds(inverseMarkers(c), seconds, 1.0);
            for (auto& m : c.warpMarkers) m.beats -= here;
        }, "Set Downbeat"));
        item("Reset Warp Markers", true, edit([duration, tempo](model::Clip& c) {
            const double d = std::max(0.001, duration);
            c.warpMarkers = {{0.0, 0.0}, {d, d * tempo / 60.0}};
        }, "Reset Warp Markers"));
        menu.addSeparator();
    }
    item("Crop Sample", ctx().cropClip != nullptr && clip.lengthSamples > 0, [this, location] {
        if (Widget::isAlive(this) && ctx().cropClip) ctx().cropClip(location);
    });
    menu.addSeparator();
    item("Zoom to Fit", true, [this, duration] {
        if (! Widget::isAlive(this)) return;
        scrollSeconds_ = 0;
        secondsPerPixel_ = std::max(duration, 0.001) / std::max(1.0f, waveArea().w);
        verticalZoom_ = 1.0f;
        repaint();
    });
    menu.showMenuAsync(juce::PopupMenu::Options(), [callbacks = std::move(actions)](int result) {
        if (result >= 1 && (size_t) result <= callbacks.size()) callbacks[(size_t) result - 1]();
    });
}

void SampleView::mouseDown(const MouseEvent& event)
{
    marker_ = -1;
    edge_ = 0;
    changed_ = false;
    const auto* clip = ctx().selectedClip();
    if (clip && clip->kind == model::Clip::Kind::Audio && event.right && event.y >= kHeaderHeight && ctx().applyEdit && ctx().song)
    {
        showContextMenu(event, *clip);
        return;
    }
    if (! clip || ! ctx().applyEdit || ! ctx().song || event.y < startRowY()) return;
    original_ = working_ = *clip;
    location_ = ctx().selectedClipLocation();
    grabY_ = event.y;
    if (event.y < loopRowY())
    {
        // Start row: the clip start / end triangles drag in source time.
        const double secPerBeat = ctx().song->tempo > 0 ? 60.0 / ctx().song->tempo : 0.5;
        auto toSeconds = [&](double v) { return clip->warped ? engine::warpBeatsToSeconds(clip->warpMarkers, v, secPerBeat) : v; };
        const float xs = secondsToX(toSeconds(clip->loop.loopStart + clip->loop.startRelative));
        const float xe = secondsToX(toSeconds(clip->loop.loopEnd));
        if (std::fabs(xs - event.x) <= 9 && std::fabs(xs - event.x) <= std::fabs(xe - event.x)) edge_ = 1;
        else if (std::fabs(xe - event.x) <= 9) edge_ = 2;
        repaint();
        return;
    }
    if (event.y < warpRowY())
    {
        // Loop row (UI-17c): the position (loop start) marker, the brace's end, or the brace itself (moves the loop).
        const double secPerBeat = ctx().song->tempo > 0 ? 60.0 / ctx().song->tempo : 0.5;
        auto toSeconds = [&](double v) { return clip->warped ? engine::warpBeatsToSeconds(clip->warpMarkers, v, secPerBeat) : v; };
        const float xs = secondsToX(toSeconds(clip->loop.loopStart)), xe = secondsToX(toSeconds(clip->loop.loopEnd));
        grabX_ = event.x;
        if (clip->loop.enabled && std::fabs(xe - event.x) <= 6 && std::fabs(xe - event.x) < std::fabs(xs - event.x)) edge_ = 4;
        else if (std::fabs(xs - event.x) <= 9) edge_ = 3;
        else if (clip->loop.enabled && event.x > xs && event.x < xe) edge_ = 5;
        repaint();
        return;
    }
    if (! clip->warped) return;
    // Warp row and waveform: grab the nearest warp marker, double-click to add or remove one.
    double distance = 7;
    for (size_t i = 0; i < clip->warpMarkers.size(); ++i)
    {
        const double x = secondsToX(clip->warpMarkers[i].seconds) + 3; // the tab's centre
        if (std::fabs(x - event.x) < distance) { distance = std::fabs(x - event.x); marker_ = (int) i; }
    }
    if (marker_ >= 0 && event.clicks >= 2)
    {
        if (working_.warpMarkers.size() <= 2) { marker_ = -1; return; } // preserve valid warp anchors
        working_.warpMarkers.erase(working_.warpMarkers.begin() + marker_);
        changed_ = true;
        commit();
    }
    else if (marker_ < 0 && event.clicks >= 2)
    {
        const double duration = clip->sampleRate > 0 ? (double) clip->lengthSamples / clip->sampleRate : 1e9;
        double seconds = std::clamp(xToSeconds(event.x), 0.0, std::max(0.0, duration));
        // Snap to a detected transient when the click lands within a few pixels of one.
        if (auto peaks = ctx().peaks ? ctx().peaks(*clip) : nullptr; peaks && peaks->source)
            for (double onset : peaks->source->onsets)
                if (std::fabs(onset - seconds) < 6.0 * secondsPerPixel_) { seconds = onset; break; }
        if (std::any_of(clip->warpMarkers.begin(), clip->warpMarkers.end(), [&](const auto& marker) {
                return std::fabs(marker.seconds - seconds) < 1e-6;
            })) return;
        const double beats = secondsToUnits(*clip, seconds);
        working_.warpMarkers.push_back({seconds, beats});
        std::sort(working_.warpMarkers.begin(), working_.warpMarkers.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
        changed_ = true;
        commit();
    }
    repaint();
}

void SampleView::mouseDrag(const MouseEvent& event)
{
    if (edge_ != 0)
    {
        working_ = original_;
        const double duration = original_.sampleRate > 0 ? (double) original_.lengthSamples / original_.sampleRate : 1e9;
        const double seconds = std::clamp(xToSeconds(event.x), 0.0, duration);
        const double units = std::max(0.0, secondsToUnits(original_, seconds));
        const double epsilon = original_.warped ? 0.0625 : 0.001;
        if (edge_ == 1)
            working_.loop.startRelative = std::clamp(units, 0.0, std::max(0.0, original_.loop.loopEnd - epsilon)) - original_.loop.loopStart;
        else if (edge_ == 2 || edge_ == 4)
            working_.loop.loopEnd = std::max(original_.loop.loopStart + (edge_ == 2 ? original_.loop.startRelative : 0.0) + epsilon, units);
        else
        {
            // Position marker / whole brace: the clip start marker keeps its place in the sample.
            const double absoluteStart = original_.loop.loopStart + original_.loop.startRelative;
            const double length = original_.loop.loopEnd - original_.loop.loopStart;
            const double limit = std::max(0.0, secondsToUnits(original_, duration));
            double start = edge_ == 3 ? units
                                      : original_.loop.loopStart + units - std::max(0.0, secondsToUnits(original_, std::clamp(xToSeconds(grabX_), 0.0, duration)));
            if (original_.warped && ! event.alt) start = std::round(start * 4.0) / 4.0; // 1/16 notes, like the grid
            start = edge_ == 3 ? std::clamp(start, 0.0, std::max(0.0, original_.loop.loopEnd - epsilon))
                               : std::clamp(start, 0.0, std::max(0.0, limit - length));
            working_.loop.loopStart = start;
            if (edge_ == 5) working_.loop.loopEnd = start + length;
            working_.loop.startRelative = absoluteStart - start;
        }
        working_.resetSourceMarkers = true;
        changed_ = std::fabs(working_.loop.loopEnd - original_.loop.loopEnd) > 1e-9
                   || std::fabs(working_.loop.loopStart - original_.loop.loopStart) > 1e-9
                   || std::fabs(working_.loop.startRelative - original_.loop.startRelative) > 1e-9;
        repaint();
        return;
    }
    if (marker_ < 0 || (size_t) marker_ >= working_.warpMarkers.size()) return;
    working_ = original_;
    auto& marker = working_.warpMarkers[(size_t) marker_];
    const auto& markers = original_.warpMarkers;
    const double epsilon = 1e-6;
    if (event.shift)
    {
        double beats = marker.beats + (grabY_ - event.y) * 0.025;
        if (! event.alt) beats = std::round(beats * 4) / 4;
        const double lo = marker_ > 0 ? markers[(size_t) marker_ - 1].beats + epsilon : -1e9;
        const double hi = (size_t) marker_ + 1 < markers.size() ? markers[(size_t) marker_ + 1].beats - epsilon : 1e9;
        if (lo < hi) marker.beats = std::clamp(beats, lo, hi);
    }
    else
    {
        const double seconds = xToSeconds(event.x);
        const double duration = original_.sampleRate > 0 ? (double) original_.lengthSamples / original_.sampleRate : 1e9;
        const double lo = marker_ > 0 ? markers[(size_t) marker_ - 1].seconds + epsilon : 0;
        const double hi = (size_t) marker_ + 1 < markers.size() ? markers[(size_t) marker_ + 1].seconds - epsilon : duration;
        if (lo < hi) marker.seconds = std::clamp(seconds, lo, hi);
    }
    changed_ = std::fabs(marker.seconds - markers[(size_t) marker_].seconds) > 1e-9
               || std::fabs(marker.beats - markers[(size_t) marker_].beats) > 1e-9;
    repaint();
}

void SampleView::commit()
{
    if (changed_ && ctx().song && model::findClip(*ctx().song, location_))
        ctx().applyEdit(model::modifyClip(*ctx().song, location_, working_,
                                          edge_ == 3 ? "Move Loop Position" : edge_ == 4 ? "Change Loop Length"
                                          : edge_ == 5 ? "Move Loop" : edge_ != 0 ? "Move Clip Marker" : "Edit Warp Markers"));
    marker_ = -1;
    edge_ = 0;
    changed_ = false;
    repaint();
}

void SampleView::mouseUp(const MouseEvent&) { commit(); }

bool SampleView::mouseWheel(const MouseEvent& event)
{
    if (event.cmd && event.alt)
    {
        verticalZoom_ = std::clamp(verticalZoom_ * (float) wheelZoomFactor(event.wheelDeltaY), 1.0f, 32.0f);
        repaint();
        return true;
    }
    if (event.cmd)
    {
        const double anchor = xToSeconds(event.x);
        secondsPerPixel_ = std::clamp(secondsPerPixel_ * wheelZoomFactor(-event.wheelDeltaY), 1e-6, 60.0);
        scrollSeconds_ = std::max(0.0, anchor - (event.x - kSide) * secondsPerPixel_);
    }
    else
        scrollSeconds_ = std::max(0.0, scrollSeconds_ - (event.wheelDeltaX + event.wheelDeltaY) * kWheelPixels * secondsPerPixel_);
    repaint();
    return true;
}

} // namespace sonora::ui
