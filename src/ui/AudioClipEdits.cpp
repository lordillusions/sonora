#include "ui/AudioClipEdits.h"

#include "engine/ClipPlayers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

namespace
{
// Splits on the separators, collapsing runs ("1. 1. 1" has three parts); false for any other character.
bool splitParts(std::string_view text, std::string_view separators, std::vector<std::string>& parts)
{
    std::string current;
    for (char ch : text)
    {
        if (separators.find(ch) != std::string_view::npos)
        {
            if (! current.empty()) parts.push_back(std::move(current));
            current.clear();
        }
        else if ((ch >= '0' && ch <= '9') || ch == '.') current += ch;
        else return false;
    }
    if (! current.empty()) parts.push_back(std::move(current));
    return true;
}

std::string_view trimmed(std::string_view text)
{
    while (! text.empty() && text.front() == ' ') text.remove_prefix(1);
    while (! text.empty() && text.back() == ' ') text.remove_suffix(1);
    return text;
}

std::optional<double> plainNumber(std::string_view text)
{
    text = trimmed(text);
    if (text.empty() || text.size() > 30) return std::nullopt;
    std::string copy(text);
    std::replace(copy.begin(), copy.end(), ',', '.');
    char* end = nullptr;
    const double v = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || ! std::isfinite(v)) return std::nullopt;
    return v;
}
} // namespace

std::optional<double> parseBeats(std::string_view text, int beatsPerBar, bool oneBased)
{
    text = trimmed(text);
    const bool negative = ! text.empty() && text.front() == '-';
    if (negative) text.remove_prefix(1);
    std::vector<std::string> parts;
    if (! splitParts(text, ".,: ", parts) || parts.empty() || parts.size() > 3) return std::nullopt;
    const double units[] = {(double) std::max(1, beatsPerBar), 1.0, 0.25};
    double beats = 0;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const auto v = plainNumber(parts[i]);
        if (! v) return std::nullopt;
        const double part = *v - (oneBased ? 1.0 : 0.0);
        if (part < 0) return std::nullopt;
        beats += part * units[i];
    }
    return negative ? -beats : beats;
}

std::optional<double> parseSeconds(std::string_view text)
{
    text = trimmed(text);
    if (const auto colon = text.find(':'); colon != std::string_view::npos)
    {
        const auto minutes = plainNumber(text.substr(0, colon)), seconds = plainNumber(text.substr(colon + 1));
        if (! minutes || ! seconds || *minutes < 0 || *seconds < 0) return std::nullopt;
        return *minutes * 60.0 + *seconds;
    }
    return plainNumber(text);
}

std::optional<double> parseNumber(std::string_view text)
{
    text = trimmed(text);
    if (! text.empty() && text.back() == '%') text.remove_suffix(1);
    if (const auto slash = text.find('/'); slash != std::string_view::npos)
    {
        const auto a = plainNumber(text.substr(0, slash)), b = plainNumber(text.substr(slash + 1));
        if (! a || ! b || std::abs(*b) < 1e-12) return std::nullopt;
        return *a / *b;
    }
    return plainNumber(text);
}

} // namespace sonora::ui::audioedit
