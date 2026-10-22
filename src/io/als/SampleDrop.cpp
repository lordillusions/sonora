#include "io/als/SampleDrop.h"

#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"

#include <juce_core/juce_core.h>

#include <algorithm>

#include <cmath>

namespace sonora::io
{

model::Clip makeSampleClip(const model::Song& song, const DroppedSample& sample, int colorIndex, double start)
{
    model::Clip clip;
    clip.kind = model::Clip::Kind::Audio;
    clip.resetSourceMarkers = true;
    clip.name = juce::File(juce::String::fromUTF8(sample.path.c_str())).getFileNameWithoutExtension().toStdString();
    clip.colorIndex = colorIndex;
    clip.sample.absolutePath = sample.path;
    clip.sampleRate = sample.sampleRate;
    clip.lengthSamples = sample.frames;
    // Live's defaults for a new clip: warped (Beats mode) at the sample's own tempo when one was detected (UI-47), so a
    // 128 BPM loop dropped into a 120 BPM set lasts its true number of beats and follows the set tempo from then on; with
    // no steady tempo (one-shots) the set's tempo is assumed, so the clip plays at its original speed. One anchor at
    // each end; loop and start/end are in beats.
    const double seconds = (double) sample.frames / sample.sampleRate;
    const bool detected = std::isfinite(sample.detectedBpm) && sample.detectedBpm >= 20 && sample.detectedBpm <= 999;
    const double beats = seconds * (detected ? sample.detectedBpm : song.tempo) / 60;
    clip.tempoAnalyzed = detected;
    clip.warped = true;
    clip.warpMode = model::WarpMode::Beats;
    clip.warpMarkers = {{0.0, 0.0}, {seconds, beats}};
    clip.loop = {0, beats, 0, false};
    clip.time = clip.currentStart = start;
    clip.currentEnd = clip.currentStart + beats;
    // Use a set's full audio format when available, retaining an immutable copy for undo.
    for (const auto& source : song.tracks)
    {
        for (const auto& candidate : source.arrangementClips)
            if (candidate.kind == clip.kind && candidate.sourceXml) { clip.sourceXml = candidate.sourceXml; break; }
        if (! clip.sourceXml)
            for (const auto& slot : source.slots)
                if (slot.clip && slot.clip->kind == clip.kind && slot.clip->sourceXml) { clip.sourceXml = slot.clip->sourceXml; break; }
        if (clip.sourceXml) break;
    }
    if (! clip.sourceXml) clip.sourceXml = std::make_shared<const std::string>(audioClipTemplate(clip));
    return clip;
}

SampleDropPlan placeSamples(const model::Song& song, const std::vector<DroppedSample>& samples, SampleDropTarget target)
{
    SampleDropPlan plan;
    std::vector<const DroppedSample*> readable;
    for (const auto& sample : samples)
        if (sample.sampleRate > 0 && sample.frames > 0) readable.push_back(&sample);
    if (readable.empty()) { plan.error = samples.size() == 1 ? "Could not read the audio file" : "Could not read the audio files"; return plan; }
    if (target.track >= (int) song.tracks.size())
    { plan.error = "Drop samples on an audio track"; return plan; }

    // Every part is built against, and applied to, a scratch copy, so later parts see the tracks and scenes earlier
    // parts add; the composed command then replays the same sequence on the real song.
    model::Song scratch = song;
    std::vector<model::EditCommand> parts;
    const auto run = [&](model::EditCommand command) { command.apply(scratch); parts.push_back(std::move(command)); };
    const auto newTrack = [&](int position, model::TrackId group) {
        int pointeeEnd = 0;
        auto track = createAudioTrack(scratch, "Audio", 18, -1, -1, pointeeEnd);
        if (group >= 0) { track.groupId = group; track.outputRouting = "AudioOut/GroupTrack"; track.outputRoutingEdited = true; }
        run(model::insertTracks({std::move(track)}, pointeeEnd, "Insert Audio Track", position));
        return position;
    };

    // A MIDI or group track cannot hold audio: like dropping below the tracks, a new audio track takes the files, right
    // after the target and in its group (a group target's new track goes inside it).
    int track = target.track;
    if (track < 0)
        track = newTrack((int) scratch.tracks.size(), -1);
    else if (const auto& t = scratch.tracks[(size_t) track]; t.kind != model::TrackKind::Audio)
    {
        plan.newTrackForKind = true;
        track = newTrack(track + 1, t.kind == model::TrackKind::Group ? t.id : t.groupId);
    }
    plan.firstTrack = track;
    const auto group = scratch.tracks[(size_t) track].groupId;
    const double start = std::max(0.0, target.beat);
    double beat = start;
    int scene = target.scene;
    for (size_t i = 0; i < readable.size(); ++i)
    {
        if (i > 0 && target.oneTrackPerFile) track = newTrack(track + 1, group);
        auto clip = makeSampleClip(scratch, *readable[i], scratch.tracks[(size_t) track].colorIndex, target.scene >= 0 ? 0.0 : beat);
        if (target.scene >= 0)
        {
            run(model::setSessionClip(scratch, track, scene, std::move(clip)));
            if (! target.oneTrackPerFile) ++scene;
        }
        else
        {
            const double end = clip.currentEnd;
            run(model::insertClip(track, std::move(clip), "Insert Sample"));
            if (! target.oneTrackPerFile) beat = end;
        }
    }
    plan.placed = (int) readable.size();
    plan.command = model::compose(plan.placed == 1 ? "Insert Sample" : "Insert " + std::to_string(plan.placed) + " Samples", std::move(parts));
    return plan;
}

} // namespace sonora::io
