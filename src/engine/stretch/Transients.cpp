#include "engine/stretch/Transients.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

std::vector<double> detectTransients(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    std::vector<double> onsets{0.0};
    const int n = audio.getNumSamples();
    const int hop = std::max(32, (int) (sampleRate * 0.003)); // ~3 ms
    const int win = hop * 2;
    if (n < win * 4 || sampleRate <= 0.0)
        return onsets;

    // Energy per frame of the first difference (emphasises attacks over sustained lows).
    const int frames = (n - win) / hop;
    std::vector<double> logE((size_t) frames);
    const float* l = audio.getReadPointer(0);
    const float* r = audio.getNumChannels() > 1 ? audio.getReadPointer(1) : l;
    for (int f = 0; f < frames; ++f)
    {
        double e = 0.0;
        const int s0 = f * hop;
        for (int i = 1; i < win; ++i)
        {
            const double d = 0.5 * ((double) l[s0 + i] + r[s0 + i]) - 0.5 * ((double) l[s0 + i - 1] + r[s0 + i - 1]);
            e += d * d;
        }
        logE[(size_t) f] = std::log10(e / win + 1e-10);
    }

    // Positive flux against the recent past (max of the previous ~12 ms) so decays do not retrigger.
    const int past = std::max(2, (int) (0.012 / (hop / sampleRate)));
    std::vector<double> flux((size_t) frames, 0.0);
    for (int f = 1; f < frames; ++f)
    {
        double ref = -1e9;
        for (int k = std::max(0, f - past); k < f; ++k)
            ref = std::max(ref, logE[(size_t) k]);
        flux[(size_t) f] = std::max(0.0, logE[(size_t) f] - ref);
    }

    // Adaptive threshold: local mean over ~0.25 s plus a floor (flux is in decades of energy).
    const int half = std::max(1, (int) (0.125 / (hop / sampleRate)));
    const int minGap = std::max(1, (int) (0.040 / (hop / sampleRate)));
    int lastFrame = -minGap;
    for (int f = 1; f < frames - 1; ++f)
    {
        double mean = 0.0;
        int cnt = 0;
        for (int k = std::max(0, f - half); k < std::min(frames, f + half); ++k, ++cnt)
            mean += flux[(size_t) k];
        mean /= std::max(1, cnt);
        const double threshold = std::max(0.35, mean * 2.5);
        if (flux[(size_t) f] < threshold || flux[(size_t) f] < flux[(size_t) f - 1] || flux[(size_t) f] < flux[(size_t) f + 1])
            continue;
        if (f - lastFrame < minGap)
            continue;

        // Back up to where the energy starts rising so the slice includes the whole attack.
        int start = f;
        while (start > 1 && logE[(size_t) start - 1] < logE[(size_t) start] - 0.05 && f - start < 4)
            --start;
        const double t = (double) (start * hop) / sampleRate;
        if (t > onsets.back() + 0.02)
            onsets.push_back(t);
        lastFrame = f;
    }
    return onsets;
}

} // namespace sonora::engine
