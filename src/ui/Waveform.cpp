#include "ui/Waveform.h"

#include "core/TimeUtils.h"
#include "engine/ClipPlayers.h"
#include "include/core/SkVertices.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonora::ui
{

void paintWaveform(SkCanvas& c, const engine::Peaks& peaks, const model::Clip& clip, const Rect& body, float origin,
                   float pixelsPerBeat, double tempo, float visibleX0, float visibleX1, SkColor color, float displayGain)
{
    if (pixelsPerBeat <= 0 || tempo <= 0 || body.h < 4)
        return;
    const double secPerBeat = 60.0 / tempo;
    const double clipLen = clip.currentEnd - clip.currentStart;
    const double pitchRatio = std::pow(2.0, ((double) clip.pitchCoarse + clip.pitchFine / 100.0) / 12.0);
    const auto& loop = clip.loop;

    // Source seconds at a beat offset from the clip start (same mapping as the engine's AudioClipPlayer).
    auto seconds = [&](double offsetBeats) {
        const double beat = clip.currentStart + offsetBeats;
        if (clip.warped)
            return engine::warpBeatsToSeconds(clip.warpMarkers,
                                              model::clipContentPosition(beat, clip.currentStart, loop.loopStart, loop.loopEnd,
                                                                         loop.startRelative, loop.enabled),
                                              secPerBeat);
        return model::clipContentPosition(offsetBeats * secPerBeat * pitchRatio, 0.0, loop.loopStart, loop.loopEnd,
                                          loop.startRelative, loop.enabled);
    };

    const bool stereo = body.h >= 48.0f;
    const int displays = stereo ? 2 : 1;
    const float x0 = std::max({visibleX0, body.x, origin});
    const float x1 = std::min({visibleX1, body.right(), origin + (float) (clipLen * pixelsPerBeat)});
    // One column per device pixel, drawn as a triangle strip (UI-4b). The anti-aliased path + round-joined outline this
    // replaces fell back to Skia's CPU path rasterizer on the GPU backend: 17 ms per frame for seven long clips, which kept
    // the message thread ~75 % busy during playback. At device-pixel spacing the strip looks the same.
    const float deviceScale = std::max(0.25f, c.getTotalMatrix().getScaleX());
    const float step = std::clamp(1.0f / deviceScale, 0.25f, 1.0f);
    const float minThickness = 1.0f / std::max(0.25f, c.getTotalMatrix().getScaleY()); // keeps silence visible
    constexpr float outlineHalf = 0.4f;
    static thread_local std::vector<SkPoint> strip; // UI thread; reused across frames
    SkPaint ink;
    ink.setColor(color);
    for (int channel = 0; channel < displays; ++channel)
    {
        const float channelHeight = body.h / (float) displays;
        const float half = (channelHeight - 2.0f) * 0.5f;
        const float mid = body.y + channelHeight * ((float) channel + 0.5f);
        strip.clear();
        auto flush = [&] {
            if (strip.size() >= 4)
                c.drawVertices(SkVertices::MakeCopy(SkVertices::kTriangleStrip_VertexMode, (int) strip.size(), strip.data(), nullptr, nullptr),
                               SkBlendMode::kModulate, ink);
            strip.clear();
        };
        double previousSeconds = -1.0;
        for (float x = x0; x <= x1; x += step)
        {
            const double b0 = std::clamp((double) (x - origin) / pixelsPerBeat, 0.0, clipLen);
            const double b1 = std::min(clipLen, b0 + step / pixelsPerBeat);
            const double t0 = seconds(b0), t1 = seconds(b1);
            if (t1 < t0 || (previousSeconds >= 0 && t0 < previousSeconds)) flush(); // loop wrap
            previousSeconds = t0;
            float lo = 0, hi = 0;
            if (! peaks.range(t0, std::max(t0, t1), lo, hi, stereo ? channel : -1)) { flush(); continue; }
            const float gain = (float) clip.gain * displayGain;
            lo = std::clamp(lo * gain, -1.0f, 1.0f);
            hi = std::clamp(hi * gain, -1.0f, 1.0f);
            // Padded by half of the 0.8 px outline the old path had, so quiet tails stay as visible as before.
            float top = mid - hi * half - outlineHalf, bot = std::max(mid - lo * half, top + outlineHalf + 0.35f) + outlineHalf;
            if (bot - top < minThickness)
            {
                const float centre = 0.5f * (top + bot);
                top = centre - 0.5f * minThickness;
                bot = centre + 0.5f * minThickness;
            }
            // Each column spans [x, x + step): the strip's last pair closes the final column.
            strip.push_back(SkPoint::Make(x, top));
            strip.push_back(SkPoint::Make(x, bot));
            strip.push_back(SkPoint::Make(x + step, top));
            strip.push_back(SkPoint::Make(x + step, bot));
        }
        flush();
    }
}

} // namespace sonora::ui
