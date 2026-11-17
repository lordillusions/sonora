#pragma once
// PLG-25a: the deterministic randomness of the pattern generators (Sonora Phrase, Pulse, and every later generator).
//
// A generator never keeps a running random generator. Every decision is a pure function of the seed and of where it is
// made: hash(seed, a, b, c), where a, b, c are what identifies the decision (the cycle index, the step inside it, and a
// "purpose" constant so that two decisions on the same step do not share their value). The same bar therefore always
// plays the same notes, a loop repeats exactly, a bounce equals playback and "To Clip" writes what was heard
// (TRANCE_SUITE.md "Decisions" item 3). One implementation here, so the devices, the clip commands and the tests agree.
//
// Header only, no JUCE, constexpr, usable on the audio thread.

#include <cstdint>

namespace sonora::model
{
struct PatternRandom
{
    // 32 well-mixed bits for (seed, a, b, c). Negative positions are fine (they are mixed as two's complement).
    static constexpr uint32_t hash(uint32_t seed, int64_t a, int64_t b = 0, int64_t c = 0) noexcept
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t) seed * 0xD6E8FEB86659FD93ull);
        h = mix(h ^ (uint64_t) a);
        h = mix(h ^ ((uint64_t) b + 0x632BE59BD9B4E019ull));
        h = mix(h ^ ((uint64_t) c + 0x85157AF5ull));
        return (uint32_t) (h >> 32);
    }
    // Uniform in [0, 1).
    static constexpr double unit(uint32_t seed, int64_t a, int64_t b = 0, int64_t c = 0) noexcept
    {
        return (double) hash(seed, a, b, c) * (1.0 / 4294967296.0);
    }
    // True with probability p (0..1): p <= 0 is never, p >= 1 always.
    static constexpr bool chance(double p, uint32_t seed, int64_t a, int64_t b = 0, int64_t c = 0) noexcept
    {
        return p >= 1.0 || (p > 0.0 && unit(seed, a, b, c) < p);
    }
    // An integer in [lo, hi] (inclusive); lo when hi < lo.
    static constexpr int range(int lo, int hi, uint32_t seed, int64_t a, int64_t b = 0, int64_t c = 0) noexcept
    {
        if (hi <= lo) return lo;
        return lo + (int) (unit(seed, a, b, c) * (double) (hi - lo + 1));
    }
    // Uniform in [-1, 1).
    static constexpr double bipolar(uint32_t seed, int64_t a, int64_t b = 0, int64_t c = 0) noexcept
    {
        return unit(seed, a, b, c) * 2.0 - 1.0;
    }

private:
    static constexpr uint64_t mix(uint64_t x) noexcept // splitmix64 finaliser
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return x;
    }
};
} // namespace sonora::model
