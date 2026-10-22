#pragma once
// UI-41 / UI-47b: bounded worker-thread tempo estimation for a decoded sample (never on the audio thread).
//
// An onset-strength envelope (log spectral flux) is scored by a comb tempogram over its autocorrelation, refined by a
// beat-grid fit, and octave errors are folded towards the file length (loops are a whole number of beats, usually a
// power of two) and a 70-180 BPM range. Long files (stems, songs) analyse at most kMaxAnalysisSeconds from the middle.
#include <juce_audio_basics/juce_audio_basics.h>

namespace sonora::engine
{
struct TempoEstimate
{
    double bpm = 0;        // 0: no steady tempo (one-shots, noise); callers keep the set's tempo and say so
    double confidence = 0; // 0..1
    double firstBeat = 0;  // seconds: where the beat grid starts in the file (the first beat at or after 0)
    bool fromLength = false; // the file length is a whole number of beats at bpm (the tempo was refined to it)
};

constexpr double kMaxAnalysisSeconds = 90;

// preferredBpm (the set's tempo) only breaks ties between equally likely octaves.
TempoEstimate estimateSampleTempo(const juce::AudioBuffer<float>& audio, double sampleRate, double preferredBpm);
}
