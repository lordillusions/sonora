#pragma once
// TRK-3: Strip Silence and transient navigation helpers (offline, message/worker thread only).
//
// `findSoundRegions` scans a rendered clip (song time, see renderClipRange) for audible passages: 10 ms peak windows above the
// threshold form regions; gaps shorter than `minSilenceSeconds` are bridged, regions shorter than `minRegionSeconds` are
// dropped, and the survivors are padded by pre/post roll so attacks and tails are not clipped.

#include <juce_audio_basics/juce_audio_basics.h>

#include <utility>
#include <vector>

namespace sonora::engine
{

struct StripSilenceOptions
{
    double thresholdDb = -40.0;
    double minSilenceSeconds = 0.25;
    double minRegionSeconds = 0.02;
    double preRollSeconds = 0.005;
    double postRollSeconds = 0.05;
};

// Audible regions as [first, end) sample ranges, ascending and non-overlapping.
std::vector<std::pair<int, int>> findSoundRegions(const juce::AudioBuffer<float>& audio, double sampleRate,
                                                  const StripSilenceOptions& options = {});

// TRK-4 Capture Audio: the most recent passage of a capture buffer. A silence of `gapSeconds` ends a passage; returns an empty
// range ({0, 0}) when nothing rises above `thresholdDb`.
std::pair<int, int> latestPassage(const juce::AudioBuffer<float>& audio, double sampleRate, double thresholdDb = -50.0, double gapSeconds = 8.0);

// The first of `ascending` strictly after (direction > 0) or before (direction < 0) `from` beyond `epsilon`, or `from`
// when there is none.
double stepToMarker(const std::vector<double>& ascending, double from, int direction, double epsilon = 1e-6);

} // namespace sonora::engine
