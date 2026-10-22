#include "ui/AudioClipEdits.h"

#include "engine/ClipPlayers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui::audioedit
{

double clipBpm(const model::Clip& clip, double songTempo)
{
    if (clip.warpMarkers.size() >= 2)
    {
        const auto& a = clip.warpMarkers.front();
        const auto& b = clip.warpMarkers.back();
        const double seconds = b.seconds - a.seconds, beats = b.beats - a.beats;
        if (seconds > 1e-9 && beats > 1e-9)
            return 60.0 * beats / seconds;
    }
    return songTempo;
}

model::Clip toggleWarp(const model::Clip& before, double songTempo)
{
    auto working = before;
    const double secondsPerBeat = songTempo > 0 ? 60.0 / songTempo : 0.5;
    auto toSeconds = [&](double beats) { return engine::warpBeatsToSeconds(before.warpMarkers, beats, secondsPerBeat); };
    auto toBeats = [&](double seconds) {
        std::vector<model::WarpMarker> inverse;
        for (const auto& marker : before.warpMarkers) inverse.push_back({marker.beats, marker.seconds});
        return engine::warpBeatsToSeconds(inverse, seconds, 1 / secondsPerBeat);
    };
    const double start = before.loop.loopStart + before.loop.startRelative;
    working.loop.loopStart = before.warped ? toSeconds(before.loop.loopStart) : toBeats(before.loop.loopStart);
    working.loop.loopEnd = before.warped ? toSeconds(before.loop.loopEnd) : toBeats(before.loop.loopEnd);
    working.loop.startRelative = (before.warped ? toSeconds(start) : toBeats(start)) - working.loop.loopStart;
    working.warped = ! before.warped;
    if (working.warped && working.warpMarkers.empty())
    {
        const double duration = before.sampleRate > 0 ? (double) before.lengthSamples / before.sampleRate : before.loop.loopEnd;
        working.warpMarkers = {{0, 0}, {std::max(0.001, duration), std::max(0.001, duration) / secondsPerBeat}};
    }
    working.resetSourceMarkers = true;
    return working;
}

model::Clip enableWarpWithTempo(const model::Clip& before, double songTempo, double detectedBpm)
{
    if (before.warped || before.tempoAnalyzed || before.warpMarkers.size() > 2) return toggleWarp(before, songTempo);
    auto grid = before;
    const double bpm = std::isfinite(detectedBpm) && detectedBpm >= 20 && detectedBpm <= 999 ? detectedBpm : songTempo;
    const double duration = before.sampleRate > 0 ? (double) before.lengthSamples / before.sampleRate : before.loop.loopEnd;
    grid.warpMarkers = {{0, 0}, {std::max(0.001, duration), std::max(0.001, duration) * bpm / 60}};
    auto result = toggleWarp(grid, songTempo);
    result.tempoAnalyzed = true;
    result.currentEnd = result.currentStart + (before.currentEnd - before.currentStart) * bpm / std::max(20.0, songTempo);
    return result;
}

model::Clip scaleTempo(const model::Clip& before, double factor)
{
    auto c = before;
    if (! before.warped || before.warpMarkers.size() < 2 || ! std::isfinite(factor) || factor <= 0)
        return c;
    c.tempoAnalyzed = true;
    if (std::fabs(factor - 1.0) < 1e-12) return c;
    for (auto& marker : c.warpMarkers) marker.beats *= factor;
    c.loop.loopStart *= factor;
    c.loop.loopEnd *= factor;
    c.loop.startRelative *= factor;
    c.currentEnd = c.currentStart + (c.currentEnd - c.currentStart) * factor;
    c.resetSourceMarkers = true;
    return c;
}

model::Clip mirrorForReverse(const model::Clip& before, double sampleSeconds)
{
    auto c = before;
    if (before.warped && before.warpMarkers.size() >= 2)
    {
        const double pivot = before.warpMarkers.front().beats + before.warpMarkers.back().beats;
        c.warpMarkers.clear();
        for (auto it = before.warpMarkers.rbegin(); it != before.warpMarkers.rend(); ++it)
            c.warpMarkers.push_back({sampleSeconds - it->seconds, pivot - it->beats});
        c.loop.loopStart = pivot - before.loop.loopEnd;
        c.loop.loopEnd = pivot - before.loop.loopStart;
    }
    else if (! before.warped)
    {
        c.loop.loopStart = sampleSeconds - before.loop.loopEnd;
        c.loop.loopEnd = sampleSeconds - before.loop.loopStart;
        c.warpMarkers.clear();
        for (auto it = before.warpMarkers.rbegin(); it != before.warpMarkers.rend(); ++it)
            c.warpMarkers.push_back({sampleSeconds - it->seconds, it->beats});
    }
    c.loop.startRelative = 0.0;
    c.resetSourceMarkers = true;
    return c;
}

SourceRange cropRange(const model::Clip& clip, double songTempo)
{
    const double secondsPerBeat = songTempo > 0 ? 60.0 / songTempo : 0.5;
    auto toSeconds = [&](double v) { return clip.warped ? engine::warpBeatsToSeconds(clip.warpMarkers, v, secondsPerBeat) : v; };
    const double begin = toSeconds(clip.loop.loopStart), start = toSeconds(clip.loop.loopStart + clip.loop.startRelative);
    const double end = toSeconds(clip.loop.loopEnd);
    return {std::min(begin, start), std::max(end, start)};
}

model::Clip cropped(const model::Clip& clip, double startSeconds)
{
    auto c = clip;
    for (auto& marker : c.warpMarkers)
        marker.seconds -= startSeconds;
    if (! clip.warped)
    {
        c.loop.loopStart -= startSeconds;
        c.loop.loopEnd -= startSeconds;
    }
    c.resetSourceMarkers = true;
    return c;
}

std::string formatBeats(double beats, int beatsPerBar, bool oneBased)
{
    const int perBar = std::max(1, beatsPerBar);
    const bool negative = beats < 0;
    const double value = std::fabs(beats);
    const int bar = (int) std::floor(value / perBar + 1e-9);
    const double inBar = value - bar * perBar;
    const int beat = (int) std::floor(inBar + 1e-9);
    const int sixteenth = (int) std::floor((inBar - beat) * 4.0 + 1e-6);
    char text[48];
    std::snprintf(text, sizeof(text), "%s%d. %d. %d", negative ? "-" : "", bar + (oneBased ? 1 : 0), beat + (oneBased ? 1 : 0),
                  sixteenth + (oneBased ? 1 : 0));
    return text;
}

std::string formatSeconds(double seconds)
{
    const int minutes = (int) std::floor(std::max(0.0, seconds) / 60.0);
    const double rest = std::max(0.0, seconds) - minutes * 60.0;
    char text[32];
    std::snprintf(text, sizeof(text), "%d: %d.%03d", minutes, (int) std::floor(rest), (int) std::lround((rest - std::floor(rest)) * 1000.0) % 1000);
    return text;
}

} // namespace sonora::ui::audioedit
