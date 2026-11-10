#include "core/SampleReplace.h"

#include <cmath>

namespace sonora::model
{

namespace
{
std::string fileStem(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (const auto dot = name.find_last_of('.'); dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

double seconds(double sampleRate, int64_t frames) { return sampleRate > 0 ? (double) frames / sampleRate : 0.0; }
} // namespace

const char* sampleScopeLabel(SampleScope scope)
{
    switch (scope)
    {
        case SampleScope::Clip: return "This Clip";
        case SampleScope::Track: return "All Clips on This Track";
        case SampleScope::Set: return "All Clips in the Set";
    }
    return "";
}

bool sameSampleFile(const FileRef& a, const FileRef& b)
{
    if (! a.absolutePath.empty() && a.absolutePath == b.absolutePath) return true;
    return a.relativePathType == 3 && b.relativePathType == 3 && ! a.relativePath.empty() && a.relativePath == b.relativePath;
}

std::vector<ClipLocation> clipsUsingSample(const Song& song, ClipLocation location, SampleScope scope)
{
    const auto* origin = findClip(song, location);
    if (origin == nullptr || origin->kind != Clip::Kind::Audio) return {};
    std::vector<ClipLocation> result{location};
    if (scope == SampleScope::Clip) return result;
    const auto ref = origin->sample;
    auto visit = [&](int index) {
        const auto& track = song.tracks[(size_t) index];
        if (track.frozen) return;
        auto add = [&](const Clip& clip, ClipLocation at) {
            if (clip.kind != Clip::Kind::Audio || ! sameSampleFile(clip.sample, ref)) return;
            if (at.kind == location.kind && at.track == location.track && at.uid == location.uid && at.scene == location.scene
                && at.laneId == location.laneId)
                return; // the origin, already first
            result.push_back(at);
        };
        for (const auto& clip : track.arrangementClips) add(clip, ClipLocation::arrangement(index, clip.uid));
        for (size_t scene = 0; scene < track.slots.size(); ++scene)
            if (track.slots[scene].clip) add(*track.slots[scene].clip, ClipLocation::session(index, track.slots[scene].clip->uid, (int) scene));
        for (const auto& lane : track.takeLanes)
            for (const auto& clip : lane.clips) add(clip, ClipLocation::take(index, clip.uid, lane.id));
    };
    if (scope == SampleScope::Track)
    {
        if (location.track >= 0 && (size_t) location.track < song.tracks.size()) visit(location.track);
    }
    else
        for (int index = 0; index < (int) song.tracks.size(); ++index) visit(index);
    return result;
}

Clip withReplacedSample(const Clip& clip, const SampleFile& replacement)
{
    Clip changed = clip;
    const auto oldStem = fileStem(clip.sample.absolutePath.empty() ? clip.sample.relativePath : clip.sample.absolutePath);
    changed.sample = replacement.ref;
    changed.sampleRate = replacement.sampleRate;
    changed.lengthSamples = replacement.frames;
    if (! oldStem.empty() && clip.name == oldStem) changed.name = fileStem(replacement.ref.absolutePath);
    const double before = seconds(clip.sampleRate, clip.lengthSamples), after = seconds(replacement.sampleRate, replacement.frames);
    // The reference keeps warp markers only for a file of the same length; anything else gets two anchors at the clip's
    // current tempo, so a warped kick keeps playing on the same grid.
    const bool sameLength = std::fabs(before - after) < 0.0005;
    if (! sameLength && after > 0)
    {
        double beatsPerSecond = 0;
        if (clip.warpMarkers.size() >= 2)
        {
            const auto& first = clip.warpMarkers.front();
            const auto& last = clip.warpMarkers.back();
            if (last.seconds - first.seconds > 1e-9) beatsPerSecond = (last.beats - first.beats) / (last.seconds - first.seconds);
        }
        if (! (beatsPerSecond > 0) || ! std::isfinite(beatsPerSecond)) beatsPerSecond = 2.0; // 120 BPM, only for empty maps
        changed.warpMarkers = {{0.0, 0.0}, {after, after * beatsPerSecond}};
        changed.tempoAnalyzed = false;
    }
    changed.resetSourceMarkers = true;
    return changed;
}

EditCommand replaceClipSample(const Song& song, ClipLocation location, const SampleFile& replacement, SampleScope scope, int* replaced)
{
    if (replaced) *replaced = 0;
    if (replacement.frames <= 0 || replacement.sampleRate <= 0 || replacement.ref.absolutePath.empty()) return {};
    std::vector<EditCommand> parts;
    for (const auto& at : clipsUsingSample(song, location, scope))
    {
        const auto* clip = findClip(song, at);
        if (clip == nullptr || sameSampleFile(clip->sample, replacement.ref)) continue;
        parts.push_back(modifyClip(song, at, withReplacedSample(*clip, replacement), "Replace Sample"));
    }
    if (parts.empty()) return {};
    if (replaced) *replaced = (int) parts.size();
    const auto count = parts.size();
    return compose(count == 1 ? "Replace Sample" : "Replace Sample in " + std::to_string(count) + " Clips", std::move(parts));
}

} // namespace sonora::model
