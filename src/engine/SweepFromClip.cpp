#include "engine/SweepFromClip.h"

#include "devices/native/Sweep.h"
#include "engine/SampleExport.h"
#include "io/als/AlsReader.h"
#include "io/als/SampleDrop.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{
SweepFromClipResult sweepFromClip(const model::Song& song, const model::ClipLocation& location, bool up, double beats,
                                  const juce::AudioBuffer<float>& audio, double sampleRate, const std::string& stemName,
                                  const juce::File& folder)
{
    SweepFromClipResult result;
    const auto* source = model::findClip(song, location);
    if (source == nullptr || source->kind != model::Clip::Kind::Audio || location.kind != model::ClipLocation::Kind::Arrangement)
    {
        result.error = "Make Riser / Downlifter works on audio clips in the arrangement";
        return result;
    }
    if (up && source->currentStart < beats - 1e-6)
    {
        result.error = "Not enough room before the clip for a riser that long: move the clip later or pick a shorter one";
        return result;
    }
    if (sampleRate <= 0 || audio.getNumSamples() == 0)
    {
        result.error = "The clip's sample is empty";
        return result;
    }
    using W = devices::Sweep;
    int lengthIndex = -1;
    for (int i = 0; i < W::kLengths; ++i)
        if (std::abs(W::kLengthBeats[i] - beats) < 1e-6) lengthIndex = i;
    if (lengthIndex < 0)
    {
        result.error = "Unsupported sweep length";
        return result;
    }
    juce::AudioBuffer<float> clipped(2, std::min(audio.getNumSamples(), (int) (20.0 * sampleRate)));
    for (int ch = 0; ch < 2; ++ch) clipped.copyFrom(ch, 0, audio, std::min(ch, audio.getNumChannels() - 1), 0, clipped.getNumSamples());
    std::vector<float> values(W::NumParams);
    {
        W defaults;
        for (int i = 0; i < W::NumParams; ++i) values[(size_t) i] = defaults.getParameter(i);
    }
    values[W::Direction] = up ? (float) W::Up : (float) W::Down;
    values[W::LengthParam] = (float) lengthIndex;
    values[W::LowHz] = 400.0f;
    values[W::HighHz] = 20000.0f;
    values[W::Swell] = 0.7f;
    values[W::Output] = 0.0f;
    const auto rendered = W::renderOffline(values, &clipped, sampleRate, song.tempo);

    if (! folder.createDirectory())
    {
        result.error = "Cannot create " + folder.getFullPathName().toStdString();
        return result;
    }
    result.file = folder.getNonexistentChildFile(juce::String::fromUTF8(stemName.c_str()) + (up ? " [riser]" : " [downlifter]"), ".wav", false);
    if (const auto error = writeCroppedWav(rendered, sampleRate, 0, rendered.getNumSamples(), result.file); ! error.empty())
    {
        result.error = "Could not write the sweep: " + error;
        return result;
    }

    int trackId = 0;
    for (const auto& t : song.tracks) trackId = std::max(trackId, t.id + 1);
    for (const auto& t : song.returns) trackId = std::max(trackId, t.id + 1);
    int pointeeEnd = std::max(1, song.nextPointeeId);
    const int colour = (source->colorIndex + 5) % 70;
    auto track = io::createAudioTrack(song, up ? "Riser" : "Downlifter", colour, trackId, pointeeEnd, pointeeEnd);
    track.groupId = song.tracks[(size_t) location.track].groupId;
    const double start = up ? source->currentStart - beats : source->currentStart;
    auto clip = io::makeSampleClip(song, {result.file.getFullPathName().toStdString(), sampleRate, rendered.getNumSamples()}, colour, start);
    const auto project = juce::File(juce::String::fromUTF8(song.sourcePath.c_str())).getParentDirectory();
    if (! song.sourcePath.empty() && result.file.isAChildOf(project))
    {
        clip.sample.relativePath = result.file.getRelativePathFrom(project).replaceCharacter('\\', '/').toStdString();
        clip.sample.relativePathType = 3;
        clip.sample.sourceDirectory = project.getFullPathName().toStdString();
    }
    clip.name = stemName + (up ? " riser" : " downlifter");
    const std::string label = up ? "Make Riser from Clip" : "Make Downlifter from Clip";
    result.newTrack = location.track + 1;
    std::vector<model::EditCommand> parts;
    parts.push_back(model::insertTracks({std::move(track)}, pointeeEnd, label, result.newTrack));
    parts.push_back(model::insertClip(result.newTrack, std::move(clip), label));
    result.edit = model::compose(label, std::move(parts));
    return result;
}
} // namespace sonora::engine
