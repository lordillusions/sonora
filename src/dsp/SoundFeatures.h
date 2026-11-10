#pragma once
// LIB-4: a small description of how a sample sounds, for "Show Similar Files" and the similar-sample swap.
//
// Own, clean-room design (the reference's similarity search is a neural model; this is classic audio description):
// from the first seconds of a file, short-time spectra give the brightness (spectral centroid, spread, 85 % roll-off),
// noisiness (flatness, zero crossings) and a 16-band log-frequency spectrum shape; the loudness envelope gives the attack
// time, the decay at fixed times after the peak and the energy's centre in time; spectral-flux onsets per second tell a
// one-shot from a loop; the file's duration class is the last ingredient. Every value is scaled to roughly 0..1 and the
// distance is a weighted Euclidean one, so a kick ranks other kicks first, a closed hat other hats, a pad other pads.
// Not for the audio thread (allocates).

#include <array>
#include <cstdint>

namespace sonora::dsp
{

struct SoundFeatures
{
    static constexpr int kVersion = 1; // bump when the extraction changes: cached vectors are recomputed
    static constexpr int kBands = 16, kEnvelopePoints = 8;
    enum Index : int
    {
        Duration, Centroid, Spread, Flatness, Rolloff, CentroidVariation, ZeroCrossings, Attack, TemporalCentre, Onsets,
        Loudness, FirstBand, FirstEnvelope = FirstBand + kBands, Count = FirstEnvelope + kEnvelopePoints
    };
    std::array<float, Count> values{};
    bool valid = false; // false: silent or unreadable audio
};

// `mono` holds the analysed start of the file (callers pass ~3 s); `fileSeconds` is the whole file's duration.
SoundFeatures extractSoundFeatures(const float* mono, int64_t frames, double sampleRate, double fileSeconds);

// Weighted distance between two descriptions; invalid ones are infinitely far.
float soundDistance(const SoundFeatures& a, const SoundFeatures& b);

// How many seconds of a file extractSoundFeatures looks at.
inline constexpr double kSoundFeatureSeconds = 3.0;

} // namespace sonora::dsp
