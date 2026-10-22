#pragma once
// DEV-18d: measured sync divisions. Preserve the original thirteen native enum indices.
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace sonora::dsp
{
struct LfoTempoDivisions
{
    static constexpr int count = 43;
    static constexpr std::array<int, 13> legacyBins{9, 12, 15, 18, 21, 24, 27, 30, 33, 26, 23, 19, 16};
    static constexpr std::array<int, count> makeBins()
    {
        std::array<int, count> result{};
        int index = 0;
        for (const int bin : legacyBins) result[static_cast<size_t>(index++)] = bin;
        for (int bin = 0; bin < count; ++bin)
            if (std::find(legacyBins.begin(), legacyBins.end(), bin) == legacyBins.end()) result[static_cast<size_t>(index++)] = bin;
        return result;
    }
    static int nativeIndex(int bin)
    {
        static constexpr auto bins = makeBins();
        const auto found = std::find(bins.begin(), bins.end(), std::clamp(bin, 0, count - 1));
        return static_cast<int>(found - bins.begin());
    }
    static int binFor(int native)
    {
        static constexpr auto bins = makeBins();
        return bins[static_cast<size_t>(std::clamp(native, 0, count - 1))];
    }
    static double beats(int native)
    {
        const int bin = binFor(native);
        const double base = std::ldexp(128.0, -(bin / 3));
        return bin % 3 == 0 ? base : bin % 3 == 1 ? base * 0.75 : base * (2.0 / 3);
    }
    static std::string_view label(int native)
    {
        static constexpr std::array<std::string_view, count> labels{
            "32 Bars", "24 Bars", "32 Bars Triplet", "16 Bars", "12 Bars", "16 Bars Triplet",
            "8 Bars", "6 Bars", "8 Bars Triplet", "4 Bars", "3 Bars", "4 Bars Triplet",
            "2 Bars", "1.5 Bars", "2 Bars Triplet", "1 Bar", "1/2 Dotted", "1 Bar Triplet",
            "1/2", "1/4 Dotted", "1/2 Triplet", "1/4", "1/8 Dotted", "1/4 Triplet",
            "1/8", "1/16 Dotted", "1/8 Triplet", "1/16", "1/32 Dotted", "1/16 Triplet",
            "1/32", "1/64 Dotted", "1/32 Triplet", "1/64", "1/128 Dotted", "1/64 Triplet",
            "1/128", "1/256 Dotted", "1/128 Triplet", "1/256", "1/512 Dotted", "1/256 Triplet", "Fast"};
        return labels[static_cast<size_t>(binFor(native))];
    }
    static int fromStoredRate(double rate, bool dotted, bool triplets)
    {
        const double normalized = std::isfinite(rate) ? std::sqrt(std::sqrt(std::clamp(rate / 100, 0.0, 1.0))) : 0;
        int bin = std::clamp(static_cast<int>(std::floor(42 * normalized + 0.5)), 0, count - 1);
        while (bin > 0 && ((bin % 3 == 1 && ! dotted) || (bin % 3 == 2 && ! triplets))) --bin;
        return nativeIndex(bin);
    }
};
} // namespace sonora::dsp
