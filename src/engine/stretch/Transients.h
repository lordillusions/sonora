#pragma once
// Onset (transient) detection for Beats-mode warping (ROADMAP ENG-5).
// Live stores its own analysis in .asd files we cannot read, so we detect onsets ourselves:
// a short-time energy envelope on the high-passed signal, positive log-energy flux, adaptive
// threshold, minimum spacing. Cheap (O(n)); runs once per sample file on the loader thread.

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

namespace sonora::engine
{

// Returns onset times in seconds, ascending. Always includes 0.0 as the first slice start.
std::vector<double> detectTransients(const juce::AudioBuffer<float>& audio, double sampleRate);

} // namespace sonora::engine
