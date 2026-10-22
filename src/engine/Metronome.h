#pragma once
// Metronome click (ROADMAP ENG-10): a short decaying sine on every beat, higher on the first
// beat of a bar. Mixed into the output after the main bus (like Live's metronome it does not go
// through the tracks). Audio thread safe: all state is plain members, no allocation.

#include "engine/ClipPlayers.h"

#include <cmath>

namespace sonora::engine
{

class Metronome
{
public:
    void prepare(double sampleRate) { sampleRate_ = sampleRate; remaining_ = 0; }

    // Adds the clicks that start inside `segs` (and the tail of a click that started earlier).
    // `l` / `r` are the output channels of the whole block.
    void render(const BlockSegment* segs, int numSegs, double beatsPerBar, int numSamples, float* l, float* r, double tickBeats = 1)
    {
        // 1) clicks that start in this block, in order of their sample position
        int starts[8];
        bool accents[8];
        int numStarts = 0;
        for (int s = 0; s < numSegs && numStarts < 8; ++s)
        {
            const auto& seg = segs[s];
            for (double b = std::ceil(seg.startBeats / tickBeats - 1e-9) * tickBeats; b < seg.endBeats() - 1e-9 && numStarts < 8; b += tickBeats)
            {
                const int offset = seg.sampleOffset + std::clamp((int) std::lround((b - seg.startBeats) * seg.samplesPerBeat), 0, seg.numSamples - 1);
                starts[numStarts] = offset;
                accents[numStarts] = beatsPerBar > 0 && std::abs(std::remainder(b, beatsPerBar)) < 1e-8;
                ++numStarts;
            }
        }

        // 2) synthesise, restarting the click at each start
        int next = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            while (next < numStarts && starts[next] == i)
            {
                remaining_ = (int) (kLength * sampleRate_);
                age_ = 0;
                freq_ = accents[next] ? kAccentHz : kBeatHz;
                level_ = accents[next] ? 0.5f : 0.35f;
                ++next;
            }
            if (remaining_ <= 0)
                continue;
            const double t = (double) age_ / sampleRate_;
            const float v = level_ * (float) (std::sin(2.0 * 3.14159265358979 * freq_ * t) * std::exp(-t / 0.006));
            l[i] += v;
            r[i] += v;
            --remaining_;
            ++age_;
        }
    }

private:
    static constexpr double kLength = 0.03; // seconds
    static constexpr double kBeatHz = 880.0, kAccentHz = 1320.0;
    double sampleRate_ = 44100.0;
    int remaining_ = 0, age_ = 0;
    double freq_ = kBeatHz;
    float level_ = 0.35f;
};

} // namespace sonora::engine
