#pragma once
// PLG-19: Clip > Make Riser / Make Downlifter from Clip. Renders an audio clip's sample through Sonora Sweep offline at the
// set's tempo and builds the edit that puts the result on a new audio track right under the clip's track (same group):
// Up = the sample reversed with a reverb tail, rising in pitch, ending exactly where the clip starts; Down = forwards,
// falling in pitch, starting with the clip. Background thread safe (no UI, no engine state).

#include "core/Edit.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <string>

namespace sonora::engine
{
struct SweepFromClipResult
{
    model::EditCommand edit;
    int newTrack = -1; // index of the inserted track once the edit is applied
    juce::File file;
    std::string error;
};

// `audio` / `sampleRate`: the clip's sample (only the first 20 s are used). `folder`: where the WAV goes (created).
SweepFromClipResult sweepFromClip(const model::Song& song, const model::ClipLocation& location, bool up, double beats,
                                  const juce::AudioBuffer<float>& audio, double sampleRate, const std::string& stemName,
                                  const juce::File& folder);
} // namespace sonora::engine
