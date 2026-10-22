#include "ui/GridModel.h"

#include <algorithm>
#include <cstdio>

namespace sonora::ui
{

namespace
{
constexpr double kEps = 1e-7;

double minGridPixels(GridSettings::Density density)
{
    switch (density)
    {
        case GridSettings::Density::Narrowest: return 3.0;
        case GridSettings::Density::Narrow: return 6.0;
        case GridSettings::Density::Medium: return 10.0;
        case GridSettings::Density::Wide: return 20.0;
        case GridSettings::Density::Widest: return 40.0;
    }
    return 10.0;
}

// All grid sizes in beats, ascending: 1/64 ... 1/2, then bar multiples (1, 2, 4, 8 ... bars).
std::vector<double> candidateSizes(double beatsPerBar)
{
    std::vector<double> sizes;
    for (double s = 0.0625; s < beatsPerBar - kEps && s < 4.0; s *= 2.0)
        sizes.push_back(s);
    for (double bars = 1.0; bars <= 4096.0; bars *= 2.0)
        sizes.push_back(beatsPerBar * bars);
    return sizes;
}

bool nearInteger(double v) { return std::fabs(v - std::round(v)) < 1e-6; }
} // namespace

double fixedGridBeats(int index, double beatsPerBar)
{
    static constexpr double fractions[] = {0.0, 0.0, 0.0, 0.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625};
    index = std::clamp(index, 0, kFixedGridCount - 1);
    if (index <= 3)
        return beatsPerBar * (double) (8 >> index);
    return fractions[index];
}

double gridBeats(const GridSettings& settings, double pixelsPerBeat, double beatsPerBar)
{
    const double bar = std::max(0.25, beatsPerBar);
    double size;
    if (settings.fixed)
        size = fixedGridBeats(settings.fixedIndex, bar);
    else
    {
        const double minPixels = minGridPixels(settings.density);
        size = 0.0;
        for (double s : candidateSizes(bar))
        {
            // A triplet grid uses 2/3 of every sub-bar size (1/8T, 1/16T ...).
            const double effective = settings.triplet && s < bar - kEps ? s * 2.0 / 3.0 : s;
            if (effective * pixelsPerBeat >= minPixels) { size = effective; break; }
        }
        if (size <= 0.0)
            size = bar;
        return size;
    }
    return settings.triplet && size < bar - kEps ? size * 2.0 / 3.0 : size;
}

std::string gridLabel(double grid, double beatsPerBar)
{
    const double bar = std::max(0.25, beatsPerBar);
    char text[24];
    if (grid >= bar - kEps)
    {
        const double bars = grid / bar;
        if (std::fabs(bars - 1.0) < 1e-6)
            return bar == 4.0 ? "1/1" : "1 Bar";
        std::snprintf(text, sizeof(text), "%d Bars", (int) std::lround(bars));
        return text;
    }
    // sizes as fractions of a whole note (4 beats); triplets are 2/3 of a power of two
    const double whole = grid / 4.0;
    for (int n = 1; n <= 128; n *= 2)
    {
        if (std::fabs(whole - 1.0 / n) < 1e-9) { std::snprintf(text, sizeof(text), "1/%d", n); return text; }
        if (std::fabs(whole - (2.0 / 3.0) / n) < 1e-9) { std::snprintf(text, sizeof(text), "1/%dT", n); return text; }
    }
    std::snprintf(text, sizeof(text), "%.3g", grid);
    return text;
}

int encodeGrid(const GridSettings& s)
{
    const int density = (int) s.density;
    return (s.fixed ? 1 : 0) | (s.triplet ? 2 : 0) | (s.snap ? 4 : 0)
           | ((density & 3) << 3) | (s.fixedIndex << 5) | (density == 4 ? 1 << 10 : 0);
}

GridSettings decodeGrid(int v)
{
    GridSettings s;
    s.fixed = (v & 1) != 0;
    s.triplet = (v & 2) != 0;
    s.snap = (v & 4) != 0;
    s.density = (v & (1 << 10)) ? GridSettings::Density::Widest : (GridSettings::Density) ((v >> 3) & 3);
    s.fixedIndex = std::clamp((v >> 5) & 31, 0, kFixedGridCount - 1);
    return s;
}

void narrowGrid(GridSettings& s)
{
    if (s.fixed)
        s.fixedIndex = std::min(kFixedGridCount - 1, s.fixedIndex + 1);
    else
        switch (s.density)
        {
            case GridSettings::Density::Widest: s.density = GridSettings::Density::Wide; break;
            case GridSettings::Density::Wide: s.density = GridSettings::Density::Medium; break;
            case GridSettings::Density::Medium: s.density = GridSettings::Density::Narrow; break;
            case GridSettings::Density::Narrow:
            case GridSettings::Density::Narrowest: s.density = GridSettings::Density::Narrowest; break;
        }
}

void widenGrid(GridSettings& s)
{
    if (s.fixed)
        s.fixedIndex = std::max(0, s.fixedIndex - 1);
    else
        switch (s.density)
        {
            case GridSettings::Density::Narrowest: s.density = GridSettings::Density::Narrow; break;
            case GridSettings::Density::Narrow: s.density = GridSettings::Density::Medium; break;
            case GridSettings::Density::Medium: s.density = GridSettings::Density::Wide; break;
            case GridSettings::Density::Wide:
            case GridSettings::Density::Widest: s.density = GridSettings::Density::Widest; break;
        }
}

std::string rulerText(double beat, double beatsPerBar, double beatUnit)
{
    const double bar = std::max(0.25, beatsPerBar), unit = std::max(0.0625, beatUnit);
    const long long barIndex = (long long) std::floor(beat / bar + 1e-9);
    double rest = beat - (double) barIndex * bar;
    const long long beatIndex = (long long) std::floor(rest / unit + 1e-9);
    rest -= (double) beatIndex * unit;
    const double sixteenth = unit / 4.0;
    const long long sixteenthIndex = (long long) std::floor(rest / sixteenth + 1e-9);
    rest -= (double) sixteenthIndex * sixteenth;
    const long long fineIndex = (long long) std::floor(rest / (sixteenth / 4.0) + 1e-9);

    char text[48];
    if (beatIndex == 0 && sixteenthIndex == 0 && fineIndex == 0)
        std::snprintf(text, sizeof(text), "%lld", barIndex + 1);
    else if (sixteenthIndex == 0 && fineIndex == 0)
        std::snprintf(text, sizeof(text), "%lld.%lld", barIndex + 1, beatIndex + 1);
    else if (fineIndex == 0)
        std::snprintf(text, sizeof(text), "%lld.%lld.%lld", barIndex + 1, beatIndex + 1, sixteenthIndex + 1);
    else
        std::snprintf(text, sizeof(text), "%lld.%lld.%lld.%lld", barIndex + 1, beatIndex + 1, sixteenthIndex + 1, fineIndex + 1);
    return text;
}

std::vector<RulerMark> rulerMarks(double firstBeat, double lastBeat, double ppb, double beatsPerBar, double beatUnit,
                                  float charWidth, float minTickPixels)
{
    std::vector<RulerMark> marks;
    if (ppb <= 0.0 || lastBeat <= firstBeat)
        return marks;
    const double bar = std::max(0.25, beatsPerBar), unit = std::max(0.0625, beatUnit);
    firstBeat = std::max(0.0, firstBeat);

    // Levels from coarse to fine: bar multiples (descending), bar, beat, then halves of a beat.
    struct Level { double step; int level; int chars; };
    std::vector<Level> levels;
    for (double bars = 1024.0; bars > 1.0; bars /= 2.0)
        levels.push_back({bar * bars, 0, 2});
    levels.push_back({bar, 0, 2});
    levels.push_back({unit, 1, 3});
    levels.push_back({unit / 2.0, 2, 5});   // 1.1.3
    levels.push_back({unit / 4.0, 2, 5});   // 1.1.2
    levels.push_back({unit / 8.0, 3, 7});
    levels.push_back({unit / 16.0, 3, 7});
    levels.push_back({unit / 32.0, 3, 7});

    // The finest level whose label still fits (label width + padding <= spacing).
    size_t labelLevel = 0;
    for (size_t i = 0; i < levels.size(); ++i)
        if (levels[i].step * ppb >= (double) levels[i].chars * charWidth + 15.0)
            labelLevel = i;
    // Ticks: the finest level still at least minTickPixels apart.
    size_t tickLevel = labelLevel;
    for (size_t i = labelLevel; i < levels.size(); ++i)
        if (levels[i].step * ppb >= minTickPixels)
            tickLevel = i;

    const double tickStep = levels[tickLevel].step, labelStep = levels[labelLevel].step;
    const long long first = (long long) std::floor(firstBeat / tickStep - kEps);
    for (long long i = first; (double) i * tickStep <= lastBeat + tickStep; ++i)
    {
        const double beat = (double) i * tickStep;
        if (beat < -kEps)
            continue;
        RulerMark m;
        m.beat = beat;
        const double inBar = std::fmod(beat, bar);
        const bool onBar = inBar < kEps || bar - inBar < kEps;
        const double inBeat = std::fmod(beat, unit);
        const bool onBeat = inBeat < kEps || unit - inBeat < kEps;
        m.level = onBar ? 0 : onBeat ? 1 : 2;
        const double ratio = beat / labelStep;
        m.labelled = nearInteger(ratio);
        if (m.labelled)
            m.text = rulerText(beat, bar, unit);
        marks.push_back(std::move(m));
    }
    return marks;
}

std::vector<GridLine> gridLines(double firstBeat, double lastBeat, double ppb, double beatsPerBar, double beatUnit, double grid)
{
    std::vector<GridLine> lines;
    if (ppb <= 0.0 || lastBeat <= firstBeat || grid <= 0.0)
        return lines;
    const double bar = std::max(0.25, beatsPerBar), unit = std::max(0.0625, beatUnit);
    firstBeat = std::max(0.0, firstBeat);
    // Never draw lines closer than 3 px; very coarse zoom still shows bars.
    double step = grid;
    while (step * ppb < 3.0)
        step *= 2.0;
    const long long first = (long long) std::floor(firstBeat / step - kEps);
    for (long long i = first; (double) i * step <= lastBeat + step; ++i)
    {
        const double beat = (double) i * step;
        if (beat < -kEps)
            continue;
        const double inBar = std::fmod(beat, bar);
        const double inBeat = std::fmod(beat, unit);
        const int strength = (inBar < kEps || bar - inBar < kEps) ? 2 : (inBeat < kEps || unit - inBeat < kEps) ? 1 : 0;
        lines.push_back({beat, strength});
    }
    // Bar lines stay visible even when the grid is coarser than a bar's worth of lines (fixed 8-bar grid, say).
    if (grid > bar + kEps)
    {
        std::vector<GridLine> withBars;
        const long long firstBar = (long long) std::floor(firstBeat / bar - kEps);
        for (long long i = firstBar; (double) i * bar <= lastBeat + bar; ++i)
            if ((double) i * bar >= -kEps)
                withBars.push_back({(double) i * bar, 2});
        return withBars;
    }
    return lines;
}

} // namespace sonora::ui
