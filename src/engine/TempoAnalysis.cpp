#include "engine/TempoAnalysis.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace sonora::engine
{
TempoEstimate estimateSampleTempo(std::span<const double> input, double preferredBpm)
{
    if (! std::isfinite(preferredBpm) || preferredBpm < 20 || preferredBpm > 999) preferredBpm = 120;
    std::vector<double> onsets;
    for (double time : input)
    {
        if (! std::isfinite(time) || time < 0) return {};
        if (! onsets.empty() && time <= onsets.back()) continue;
        onsets.push_back(time);
        if (onsets.size() == 512 || time - onsets.front() >= 90) break;
    }
    if (onsets.size() < 4 || onsets.back() - onsets.front() < 0.7) return {};
    std::set<int> candidates;
    for (size_t i = 0; i < onsets.size(); ++i)
        for (size_t distance = 1; distance <= 8 && i + distance < onsets.size(); ++distance)
        {
            const double interval = onsets[i + distance] - onsets[i];
            if (interval < 0.08 || interval > 8) continue;
            for (int beats = 1; beats <= 8; ++beats)
            {
                const double bpm = 30 * beats / interval;
                if (bpm >= 55 && bpm <= 220) candidates.insert((int) std::lround(bpm * 10));
            }
        }
    TempoEstimate best;
    double bestScore = 0;
    for (int candidate : candidates)
    {
        const double bpm = candidate / 10.0;
        double fit = 0;
        for (double time : onsets)
        {
            const double beat = (time - onsets.front()) * bpm / 60;
            const double whole = std::abs(beat - std::round(beat));
            const double half = std::abs(beat * 2 - std::round(beat * 2));
            fit += 0.7 * std::exp(-whole * whole / 0.008) + 0.3 * std::exp(-half * half / 0.016);
        }
        fit /= (double) onsets.size();
        double pulse = 0;
        for (size_t i = 1; i < onsets.size(); ++i)
        {
            const double interval = (onsets[i] - onsets[i - 1]) * bpm / 60;
            const double distance = std::log2(interval);
            pulse += std::exp(-distance * distance / 0.5);
        }
        pulse /= (double) (onsets.size() - 1);
        // Prefer observed beat pulses over counting every attack as two beats; use the set
        // tempo only as a weak tie-breaker, not as a substitute for source analysis.
        const double score = fit * (0.85 + 0.1 * pulse + 0.05 * std::exp(-std::abs(std::log2(bpm / preferredBpm))));
        if (score > bestScore) { bestScore = score; best = {bpm, fit}; }
    }
    return best.confidence >= 0.55 ? best : TempoEstimate{};
}
}
