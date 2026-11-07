#include "core/ClipGeometry.h"
#include "core/Edit.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace sonora::model
{

namespace
{
constexpr double kEps = 1e-9;

bool unwarpedAudio(const Clip& clip) { return clip.kind == Clip::Kind::Audio && ! clip.warped; }

// Content beats of a source time through the warp markers (piecewise linear, the outer segments extended), the inverse of
// engine::warpBeatsToSeconds. Kept here so core does not depend on the engine.
double warpSecondsToBeats(const std::vector<WarpMarker>& markers, double seconds, double secPerBeat)
{
    if (markers.empty()) return secPerBeat > 0 ? seconds / secPerBeat : seconds;
    if (markers.size() == 1) return markers[0].beats + (secPerBeat > 0 ? (seconds - markers[0].seconds) / secPerBeat : 0.0);
    size_t i = 1;
    while (i + 1 < markers.size() && markers[i].seconds < seconds) ++i;
    const auto& a = markers[i - 1];
    const auto& b = markers[i];
    const double span = b.seconds - a.seconds;
    if (std::fabs(span) < 1e-12) return a.beats;
    return a.beats + (seconds - a.seconds) * (b.beats - a.beats) / span;
}

// The loop position `pos` folded into [loopStart, loopEnd).
double wrapIntoLoop(double pos, double loopStart, double loopEnd)
{
    const double length = loopEnd - loopStart;
    if (length <= kEps) return loopStart;
    double offset = std::fmod(pos - loopStart, length);
    if (offset < 0) offset += length;
    if (offset > length - 1e-9) offset = 0.0;
    return loopStart + offset;
}
} // namespace

double contentUnitsPerBeat(const Clip& clip, double tempo)
{
    if (! unwarpedAudio(clip)) return 1.0;
    return tempo > 0.0 ? 60.0 / std::max(20.0, tempo) : 0.0;
}

ClipMarkers clipMarkers(const Clip& clip, bool arrangement, double tempo)
{
    ClipMarkers m;
    m.looping = clip.loop.enabled;
    m.loopStart = clip.loop.loopStart;
    m.loopEnd = clip.loop.loopEnd;
    m.start = clip.loop.loopStart + clip.loop.startRelative;
    m.end = clip.loop.loopEnd;
    const double units = contentUnitsPerBeat(clip, tempo);
    if (arrangement && ! clip.loop.enabled && units > 0.0)
        m.end = m.start + (clip.currentEnd - clip.currentStart) * units;
    return m;
}

std::pair<double, double> clipEditorRange(const Clip& clip, bool arrangement, double tempo)
{
    const auto m = clipMarkers(clip, arrangement, tempo);
    const double from = m.looping ? std::min(m.start, m.loopStart) : m.start;
    const double to = m.looping ? m.loopEnd : m.end;
    return {from, std::max(to, from + 0.0625)};
}

std::optional<std::pair<double, double>> savedEditorRange(const Clip& clip, bool arrangement, double tempo)
{
    if (! clip.sourceXml || clip.resetSourceMarkers) return std::nullopt;
    const std::string& xml = *clip.sourceXml;
    const auto block = xml.find("<ScrollerTimePreserver");
    if (block == std::string::npos) return std::nullopt;
    const auto blockEnd = xml.find("</ScrollerTimePreserver>", block);
    auto valueOf = [&](const char* tag, double& out) {
        const auto at = xml.find(tag, block);
        if (at == std::string::npos || (blockEnd != std::string::npos && at > blockEnd)) return false;
        const auto quote = xml.find("Value=\"", at);
        if (quote == std::string::npos) return false;
        char* end = nullptr;
        out = std::strtod(xml.c_str() + quote + 7, &end);
        return end != xml.c_str() + quote + 7 && std::isfinite(out);
    };
    double left = 0, right = 0;
    if (! valueOf("<LeftTime", left) || ! valueOf("<RightTime", right) || right <= left + 1e-6) return std::nullopt;
    const auto region = clipEditorRange(clip, arrangement, tempo);
    if (right <= region.first || left >= region.second) return std::nullopt; // stale after an edit moved the content
    return std::make_pair(left, right);
}

std::pair<double, double> openingEditorRange(const Clip& clip, bool arrangement, double tempo)
{
    if (auto saved = savedEditorRange(clip, arrangement, tempo)) return *saved;
    return clipEditorRange(clip, arrangement, tempo);
}

void normaliseUnloopedMarkers(Clip& clip, double tempo)
{
    const double units = contentUnitsPerBeat(clip, tempo);
    if (clip.loop.enabled || units <= 0.0) return;
    clip.loop.loopStart += clip.loop.startRelative;
    clip.loop.startRelative = 0.0;
    clip.loop.loopEnd = clip.loop.loopStart + (clip.currentEnd - clip.currentStart) * units;
}

Clip withMarkers(Clip clip, const ClipMarkers& markers, bool arrangement, double tempo)
{
    const double units = contentUnitsPerBeat(clip, tempo);
    const double minimum = unwarpedAudio(clip) ? 0.001 : 1.0 / 64.0;
    clip.loop.enabled = markers.looping;
    if (! markers.looping)
    {
        const double end = std::max(markers.end, markers.start + minimum);
        clip.loop.loopStart = markers.start;
        clip.loop.startRelative = 0.0;
        clip.loop.loopEnd = end;
        if (arrangement && units > 0.0)
            clip.currentEnd = clip.currentStart + (end - markers.start) / units;
    }
    else
    {
        clip.loop.loopStart = markers.loopStart;
        clip.loop.loopEnd = std::max(markers.loopEnd, markers.loopStart + minimum);
        double start = markers.start;
        if (start >= clip.loop.loopEnd - kEps) start = wrapIntoLoop(start, clip.loop.loopStart, clip.loop.loopEnd);
        clip.loop.startRelative = start - clip.loop.loopStart;
    }
    clip.resetSourceMarkers = true;
    return clip;
}

bool sampleContentRange(const Clip& clip, double tempo, double& first, double& last)
{
    if (clip.kind != Clip::Kind::Audio || clip.sampleRate <= 0.0 || clip.lengthSamples <= 0) return false;
    const double seconds = (double) clip.lengthSamples / clip.sampleRate;
    if (! clip.warped)
    {
        first = 0.0; last = seconds;
        return true;
    }
    const double secPerBeat = tempo > 0.0 ? 60.0 / tempo : 0.5;
    first = warpSecondsToBeats(clip.warpMarkers, 0.0, secPerBeat);
    last = warpSecondsToBeats(clip.warpMarkers, seconds, secPerBeat);
    return last > first;
}

Clip resizedEnd(const Clip& original, Beats newEnd, double tempo, Beats minLength)
{
    Clip clip = original;
    const double units = contentUnitsPerBeat(clip, tempo);
    newEnd = std::max(clip.currentStart + std::max(1.0 / 64.0, minLength), newEnd);
    if (! clip.loop.enabled && units > 0.0)
    {
        const double contentStart = clip.loop.loopStart + clip.loop.startRelative;
        double first = 0, last = 0;
        // An unlooped audio clip ends at the end of its sample at the latest (Live cannot reveal audio that is not there).
        if (sampleContentRange(clip, tempo, first, last) && newEnd > clip.currentEnd)
            newEnd = std::max(clip.currentEnd, std::min(newEnd, clip.currentStart + (last - contentStart) / units));
        clip.currentEnd = newEnd;
        normaliseUnloopedMarkers(clip, tempo);
        return clip;
    }
    clip.currentEnd = newEnd; // looped: the loop repeats up to the new end
    return clip;
}

Clip trimmedStart(const Clip& original, Beats newStart, double tempo, Beats minLength)
{
    Clip clip = original;
    const double units = contentUnitsPerBeat(clip, tempo);
    newStart = std::clamp(newStart, 0.0, clip.currentEnd - std::max(1.0 / 64.0, minLength));
    if (units <= 0.0) return withStart(clip, newStart, tempo);
    const double oldStart = clip.loop.loopStart + clip.loop.startRelative;
    double start = oldStart + (newStart - clip.currentStart) * units;
    double first = 0, last = 0;
    const bool sample = sampleContentRange(clip, tempo, first, last);
    if (! clip.loop.enabled)
    {
        // The content stays put on the timeline: an earlier left edge reveals what came before the start marker, down to
        // the sample's first frame for audio.
        if (sample && start < first && newStart < clip.currentStart)
        {
            start = std::min(oldStart, first);
            newStart = clip.currentStart + (start - oldStart) / units;
        }
        clip.currentStart = clip.time = newStart;
        clip.loop.loopStart = start;
        clip.loop.startRelative = 0.0;
        clip.loop.loopEnd = start + (clip.currentEnd - newStart) * units;
        return clip;
    }
    // Looped: the start marker moves through the loop and wraps around it (earlier repeats appear to the left).
    if (start >= clip.loop.loopEnd - kEps || (start < clip.loop.loopStart - kEps && start < oldStart - kEps))
        start = wrapIntoLoop(start, clip.loop.loopStart, clip.loop.loopEnd);
    clip.currentStart = clip.time = newStart;
    clip.loop.startRelative = start - clip.loop.loopStart;
    return clip;
}

double snapWithTargets(double position, double grid, const std::vector<double>& targets)
{
    if (grid <= 0.0) return position;
    double best = std::round(position / grid) * grid;
    for (double target : targets)
        if (std::isfinite(target) && std::fabs(target - position) < std::fabs(best - position) - 1e-12) best = target;
    return best;
}

double snapClipStart(double rawStart, double length, double grid, const std::vector<double>& targets, bool alignEnd)
{
    if (grid <= 0.0) return std::max(0.0, rawStart);
    double best = alignEnd ? std::round((rawStart + length) / grid) * grid - length : std::round(rawStart / grid) * grid;
    double distance = std::fabs(best - rawStart);
    for (double target : targets)
    {
        if (! std::isfinite(target)) continue;
        for (double candidate : {target, target - length})
            if (std::fabs(candidate - rawStart) < distance - 1e-12)
            {
                best = candidate;
                distance = std::fabs(candidate - rawStart);
            }
    }
    return std::max(0.0, best);
}

} // namespace sonora::model
