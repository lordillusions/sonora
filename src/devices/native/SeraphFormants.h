#pragma once
// Sonora Seraph (PLG-27): the formant table of sung vowels and its interpolation. Header only, no JUCE.
//
// Frequency (Hz) / level (dB) / bandwidth (Hz) of F1..F5 for five vowels (A E I O U) of four voice types: published
// measurements of sung vowels (the Csound manual, appendix "Formant Values", after IRCAM's CHANT data). Values between
// vowels are interpolated in log frequency, dB and log bandwidth. docs/SERAPH.md "Formants".

#include <algorithm>
#include <array>
#include <cmath>

namespace sonora::devices::seraph
{
enum VoiceType { Bass, Tenor, Alto, Soprano, kVoiceTypes };
constexpr int kVowels = 5;   // A E I O U
constexpr int kFormants = 5; // F1..F5

struct FormantRow
{
    float hz[kFormants];
    float db[kFormants];
    float bw[kFormants];
};

// [type][vowel]. The men's F3-F5 close together between 2.2 and 3.3 kHz are the singer's formant: keep them as measured.
inline constexpr FormantRow kFormantTable[kVoiceTypes][kVowels] = {
    { // Bass
        {{600, 1040, 2250, 2450, 2750}, {0, -7, -9, -9, -20}, {60, 70, 110, 120, 130}},
        {{400, 1620, 2400, 2800, 3100}, {0, -12, -9, -12, -18}, {40, 80, 100, 120, 120}},
        {{250, 1750, 2600, 3050, 3340}, {0, -30, -16, -22, -28}, {60, 90, 100, 120, 120}},
        {{400, 750, 2400, 2600, 2900}, {0, -11, -21, -20, -40}, {40, 80, 100, 120, 120}},
        {{350, 600, 2400, 2675, 2950}, {0, -20, -32, -28, -36}, {40, 80, 100, 120, 120}},
    },
    { // Tenor
        {{650, 1080, 2650, 2900, 3250}, {0, -6, -7, -8, -22}, {80, 90, 120, 130, 140}},
        {{400, 1700, 2600, 3200, 3580}, {0, -14, -12, -14, -20}, {70, 80, 100, 120, 120}},
        {{290, 1870, 2800, 3250, 3540}, {0, -15, -18, -20, -30}, {40, 90, 100, 120, 120}},
        {{400, 800, 2600, 2800, 3000}, {0, -10, -12, -12, -26}, {70, 80, 100, 130, 135}},
        {{350, 600, 2700, 2900, 3300}, {0, -20, -17, -14, -26}, {40, 60, 100, 120, 120}},
    },
    { // Alto
        {{800, 1150, 2800, 3500, 4950}, {0, -4, -20, -36, -60}, {80, 90, 120, 130, 140}},
        {{400, 1600, 2700, 3300, 4950}, {0, -24, -30, -35, -60}, {60, 80, 120, 150, 200}},
        {{350, 1700, 2700, 3700, 4950}, {0, -20, -30, -36, -60}, {50, 100, 120, 150, 200}},
        {{450, 800, 2830, 3500, 4950}, {0, -9, -16, -28, -55}, {70, 80, 100, 130, 135}},
        {{325, 700, 2530, 3500, 4950}, {0, -12, -30, -40, -64}, {50, 60, 170, 180, 200}},
    },
    { // Soprano
        {{800, 1150, 2900, 3900, 4950}, {0, -6, -32, -20, -50}, {80, 90, 120, 130, 140}},
        {{350, 2000, 2800, 3600, 4950}, {0, -20, -15, -40, -56}, {60, 100, 120, 150, 200}},
        {{270, 2140, 2950, 3900, 4950}, {0, -12, -26, -26, -44}, {60, 90, 100, 120, 120}},
        {{450, 800, 2830, 3800, 4950}, {0, -11, -22, -22, -50}, {40, 80, 100, 120, 120}},
        {{325, 700, 2700, 3800, 4950}, {0, -16, -35, -40, -60}, {50, 60, 170, 180, 200}},
    },
};

// Sung ranges (MIDI notes, inclusive). Boys use the soprano table with formants +12 % over a treble range.
inline constexpr int kRangeLow[kVoiceTypes] = {40, 48, 55, 60};
inline constexpr int kRangeHigh[kVoiceTypes] = {64, 69, 74, 84};
constexpr int kBoysLow = 57, kBoysHigh = 81;
constexpr float kBoysFormantScale = 1.12f;

struct FormantSet
{
    float hz[kFormants];
    float gain[kFormants]; // linear
    float bw[kFormants];
};

// What shapes the formants of one singer at one moment.
struct FormantShape
{
    float vowel = 0;      // 0..4 continuous (A E I O U)
    float hum = 0;        // 0..1: towards a closed mouth (F1 to 250 Hz, F2-F5 down 24 dB)
    float scale = 1;      // all frequencies (Size, the singer's throat, Boys)
    float brightDb = 0;   // tilt of F3-F5
    float f0 = 0;         // the fundamental (Hz), for the rule F1 >= f0; 0 = off
    float f2Target = 0;   // onsets: F2 drawn towards this frequency (0 = none) by `f2Amount`
    float f2Amount = 0;
    float f3Scale = 1;    // onsets: L lowers F3
};

namespace detail
{
// Logs of the table, computed once (constexpr-friendly code kept simple: a function-local static).
struct LogTable
{
    float lnHz[kVoiceTypes][kVowels][kFormants];
    float lnBw[kVoiceTypes][kVowels][kFormants];
};
inline const LogTable& logTable()
{
    static const LogTable table = [] {
        LogTable t{};
        for (int v = 0; v < kVoiceTypes; ++v)
            for (int w = 0; w < kVowels; ++w)
                for (int f = 0; f < kFormants; ++f)
                {
                    t.lnHz[v][w][f] = std::log(kFormantTable[v][w].hz[f]);
                    t.lnBw[v][w][f] = std::log(kFormantTable[v][w].bw[f]);
                }
        return t;
    }();
    return table;
}
} // namespace detail

// The formants of `type` for a shape: vowel interpolation, Hum, scale, brightness, F1 >= f0 and the onset overrides.
inline FormantSet formants(int type, const FormantShape& s)
{
    const auto& logs = detail::logTable();
    type = std::clamp(type, 0, kVoiceTypes - 1);
    const float vowel = std::clamp(s.vowel, 0.0f, (float) (kVowels - 1));
    const int a = std::min((int) vowel, kVowels - 2);
    const float t = vowel - (float) a;
    const float hum = std::clamp(s.hum, 0.0f, 1.0f);
    FormantSet out{};
    for (int f = 0; f < kFormants; ++f)
    {
        float lnHz = logs.lnHz[type][a][f] + t * (logs.lnHz[type][a + 1][f] - logs.lnHz[type][a][f]);
        const float lnBw = logs.lnBw[type][a][f] + t * (logs.lnBw[type][a + 1][f] - logs.lnBw[type][a][f]);
        float db = kFormantTable[type][a].db[f] + t * (kFormantTable[type][a + 1].db[f] - kFormantTable[type][a].db[f]);
        if (f == 0) lnHz += hum * (5.5214609f - lnHz); // ln 250
        else db -= 24.0f * hum;
        if (f >= 2) db += s.brightDb * (0.4f + 0.3f * (float) (f - 2));
        float hz = std::exp(lnHz) * s.scale;
        if (f == 1 && s.f2Amount > 0.0f) hz = std::exp(std::log(hz) + s.f2Amount * (std::log(s.f2Target) - std::log(hz)));
        if (f == 2) hz *= s.f3Scale;
        out.hz[f] = hz;
        out.bw[f] = std::exp(lnBw) * (0.85f + 0.15f * s.scale); // wider resonances in a bigger throat, but not proportionally
        out.gain[f] = std::pow(10.0f, db / 20.0f);
    }
    // When the fundamental is above F1, F1 follows it (sopranos open the jaw on high notes); F2 keeps above F1.
    if (s.f0 > 0.0f)
    {
        const float floor1 = s.f0 * 1.04f;
        if (out.hz[0] < floor1) out.hz[0] = floor1;
        if (out.hz[1] < out.hz[0] * 1.25f) out.hz[1] = out.hz[0] * 1.25f;
    }
    // The upper formants stay ordered (Size and the onsets move them).
    for (int f = 2; f < kFormants; ++f) out.hz[f] = std::max(out.hz[f], out.hz[f - 1] * 1.05f);
    return out;
}

// The range of a type (Boys: the treble range).
inline void range(int type, bool boys, int& low, int& high)
{
    if (boys) { low = kBoysLow; high = kBoysHigh; return; }
    type = std::clamp(type, 0, kVoiceTypes - 1);
    low = kRangeLow[type];
    high = kRangeHigh[type];
}
} // namespace sonora::devices::seraph
