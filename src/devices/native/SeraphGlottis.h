#pragma once
// Sonora Seraph (PLG-27): the glottal source. A family of band-limited pulses (the glottal flow derivative, i.e. after
// lip radiation) from pressed to breathy, one wavetable per octave of the fundamental so nothing aliases, built once
// per sample rate (message thread) and shared by every Seraph. Also the loudness table that keeps a singer at the same
// level across vowels, voice types, pitches and tension. docs/SERAPH.md "Source".

#include "devices/native/SeraphFormants.h"

#include <memory>
#include <vector>

namespace sonora::devices::seraph
{
constexpr int kPulseShapes = 8;     // open quotient 0.4 (pressed) .. 0.8 (breathy)
constexpr int kPulseOctaves = 7;    // tables for fundamentals up to 40, 80, ... 2560 Hz
constexpr int kTableSize = 2048;    // one period
constexpr int kTableMask = kTableSize - 1;
constexpr float kLowestTableHz = 40.0f;
constexpr int kMaxHarmonics = 1000;

// Open quotient of shape s (0 .. kPulseShapes-1): 0.4 .. 0.8.
inline float openQuotient(float shape) { return 0.4f + 0.4f * shape / (float) (kPulseShapes - 1); }

class GlottalTables
{
public:
    // The tables for a sample rate (built on first use, then shared). Message thread.
    static std::shared_ptr<const GlottalTables> forRate(double sampleRate);

    explicit GlottalTables(double sampleRate);

    // One period of shape s for octave o (kTableSize + 1 samples: the first repeated at the end for interpolation).
    const float* table(int shape, int octave) const { return data_.data() + (size_t) (shape * kPulseOctaves + octave) * (kTableSize + 1); }
    // The octave table for a fundamental.
    static int octaveFor(float hz)
    {
        int o = 0;
        float top = kLowestTableHz;
        while (o < kPulseOctaves - 1 && hz > top) { top *= 2.0f; ++o; }
        return o;
    }
    // Harmonic amplitudes (k = 1..kMaxHarmonics, index k-1) of shape s, as in the tables (same normalisation).
    static const std::vector<float>& harmonics(int shape);
    double sampleRate() const { return sampleRate_; }

private:
    double sampleRate_;
    std::vector<float> data_;
};

// The gain that brings a singer of `type` (kVoiceTypes = Boys) to the reference loudness for a vowel, Hum, pulse shape
// (0..kPulseShapes-1, continuous) and fundamental. Computed once from the harmonic series through the formant filters.
float loudnessGain(int type, float vowel, float hum, float shape, float hz);
} // namespace sonora::devices::seraph
