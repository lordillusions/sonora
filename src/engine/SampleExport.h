#pragma once
// Writing processed copies of samples (Clip > Reverse). Message/worker thread only.

#include <juce_audio_formats/juce_audio_formats.h>

#include <string>

namespace sonora::engine
{

// Writes the audio with its frames in reverse order as a 24-bit WAV at `target` (parent folders are created).
// Returns an empty string on success, otherwise a description of what failed.
std::string writeReversedWav(const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& target);

// Writes frames [startFrame, startFrame + numFrames) of `audio` as a 24-bit WAV at `target` (Clip > Crop Sample).
std::string writeCroppedWav(const juce::AudioBuffer<float>& audio, double sampleRate, int startFrame, int numFrames, const juce::File& target);

} // namespace sonora::engine
