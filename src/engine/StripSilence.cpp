#include "engine/StripSilence.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

std::vector<std::pair<int, int>> findSoundRegions(const juce::AudioBuffer<float>& audio, double sampleRate,
                                                  const StripSilenceOptions& options)
{
    std::vector<std::pair<int, int>> regions;
    const int n = audio.getNumSamples();
    if (n <= 0 || sampleRate <= 0.0 || audio.getNumChannels() <= 0) return regions;
    const int window = std::max(1, (int) std::lround(sampleRate * 0.01));
    const float threshold = (float) std::pow(10.0, options.thresholdDb / 20.0);

    // Loud windows, merged into runs.
    for (int start = 0; start < n; start += window)
    {
        const int count = std::min(window, n - start);
        float peak = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch) peak = std::max(peak, audio.getMagnitude(ch, start, count));
        if (peak < threshold) continue;
        if (! regions.empty() && regions.back().second == start) regions.back().second = start + count;
        else regions.push_back({start, start + count});
    }

    // Bridge short silences, then drop short blips.
    const int minSilence = (int) std::lround(options.minSilenceSeconds * sampleRate);
    std::vector<std::pair<int, int>> bridged;
    for (const auto& region : regions)
        if (! bridged.empty() && region.first - bridged.back().second < minSilence) bridged.back().second = region.second;
        else bridged.push_back(region);
    const int minRegion = (int) std::lround(options.minRegionSeconds * sampleRate);
    std::erase_if(bridged, [minRegion](const auto& r) { return r.second - r.first < minRegion; });

    // Pad and merge whatever now touches.
    const int pre = (int) std::lround(options.preRollSeconds * sampleRate), post = (int) std::lround(options.postRollSeconds * sampleRate);
    regions.clear();
    for (auto region : bridged)
    {
        region.first = std::max(0, region.first - pre);
        region.second = std::min(n, region.second + post);
        if (! regions.empty() && region.first <= regions.back().second) regions.back().second = std::max(regions.back().second, region.second);
        else regions.push_back(region);
    }
    return regions;
}

std::pair<int, int> latestPassage(const juce::AudioBuffer<float>& audio, double sampleRate, double thresholdDb, double gapSeconds)
{
    StripSilenceOptions options;
    options.thresholdDb = thresholdDb; options.minSilenceSeconds = gapSeconds; options.minRegionSeconds = 0.05;
    options.preRollSeconds = 0.05; options.postRollSeconds = 0.3;
    const auto regions = findSoundRegions(audio, sampleRate, options);
    return regions.empty() ? std::pair<int, int>{0, 0} : regions.back();
}

double stepToMarker(const std::vector<double>& ascending, double from, int direction, double epsilon)
{
    if (direction > 0)
    {
        const auto it = std::upper_bound(ascending.begin(), ascending.end(), from + epsilon);
        return it == ascending.end() ? from : *it;
    }
    const auto it = std::lower_bound(ascending.begin(), ascending.end(), from - epsilon);
    return it == ascending.begin() ? from : *std::prev(it);
}

} // namespace sonora::engine
