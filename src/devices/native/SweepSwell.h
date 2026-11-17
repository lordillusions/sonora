#pragma once
// Sonora Sweep's swell engine (PLG-19d): the "reverse reverb" technique. A sound's reverb tail is rendered on its own
// (wet only), reversed, and played so that it rises out of silence and ends exactly where the dry sound starts: the
// sound seems to breathe in before it hits. Offline, on the message thread (the device prepares it when the sample or a
// swell setting changes).
//
// Four characters, blended by an XY position (bilinear weights, each normalised to the same loudness first):
//   Hall    (x 0, y 0): an 8-line feedback delay network (Householder matrix, damped lines, diffused input).
//   Shimmer (x 0, y 1): the hall, its tail an octave up through a second hall: a bright, rising, choir-like tail.
//   Bloom   (x 1, y 1): a spectral smear: per-bin magnitudes held and decaying, random phases: a smooth frozen wash.
//   Grain   (x 1, y 0): a cloud of short grains picked from the sound, scattered over the tail, some an octave away.
// Decay is the tail's RT60 in seconds; Damp darkens it (high frequencies decay sooner).
//
// Clean-room: only the general, long-known technique (reverse reverb / pre-verb swells) is used; no code, asset or
// parameter law of any product.

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>

namespace sonora::devices::swell
{
enum Character { Hall, Shimmer, Bloom, Grain, kCharacters };
inline constexpr const char* kCharacterNames[kCharacters] = {"Hall", "Shimmer", "Bloom", "Grain"};

struct Settings
{
    float x = 0.0f, y = 0.0f;   // character blend: Hall (0,0), Shimmer (0,1), Bloom (1,1), Grain (1,0)
    double decaySeconds = 3.0;  // RT60 of the tail
    float damp = 0.35f;         // 0 bright .. 1 dark
    uint32_t seed = 0x5eed1234u;
};

// Bilinear weights of the four characters for an XY position (they sum to 1).
std::array<float, kCharacters> weights(float x, float y);

// The wet tail of `dry` (any channel count, `rate`): stereo, as long as the dry sound plus the decay, with the onset at
// sample 0. Peak-normalised to the dry peak. Characters with a weight under 0.5 % are not rendered.
juce::AudioBuffer<float> renderTail(const juce::AudioBuffer<float>& dry, double rate, const Settings&);
// One character alone, not normalised (tests, measurements).
juce::AudioBuffer<float> renderCharacter(Character, const juce::AudioBuffer<float>& dry, double rate, const Settings&);
// The time (seconds from sample 0) at which a buffer's level, smoothed over 50 ms, falls 60 dB under its peak for good.
double decayTimeOf(const juce::AudioBuffer<float>& audio, double rate, double dropDb = 60.0);
} // namespace sonora::devices::swell
