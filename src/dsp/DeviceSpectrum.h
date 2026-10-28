#pragma once
// Message-thread analysis for a device face, kept separate from the realtime capture storage.
#include "dsp/Analyzers.h"
#include "dsp/AudioDisplayTap.h"

namespace sonora::dsp
{
class DeviceSpectrum
{
public:
    DeviceSpectrum() { curve_.fill(SpectrumAnalyzer::kFloorDb); }
    // Caller limits this to 30 Hz. Each stereo channel is analyzed separately so antiphase audio
    // remains visible. The envelope shows the stronger channel at each frequency.
    bool update(AudioDisplayTap& tap, double seconds, bool spectrum = true)
    {
        const auto oldLevels = levels_, oldHolds = holds_;
        const auto oldCurve = curve_;
        const auto changed = [&] { return oldLevels != levels_ || oldHolds != holds_ || oldCurve != curve_; };
        if (generation_ != tap.generation() || std::abs(left_.sampleRate() - tap.sampleRate()) > 0.0 || seconds > 0.25)
        {
            generation_ = tap.generation();
            left_.prepare(tap.sampleRate()); right_.prepare(tap.sampleRate());
            tap.discard(); levels_.fill(0); holds_.fill(0); ages_.fill(0);
            curve_.fill(SpectrumAnalyzer::kFloorDb);
        }
        seconds = std::clamp(seconds, 0.0, 0.25);
        tap.request(spectrum);
        std::array<float, 2> peaks{};
        const int n = tap.read(samplesL_, samplesR_, peaks);
        for (size_t ch = 0; ch < 2; ++ch)
        {
            levels_[ch] = std::max(peaks[ch], levels_[ch] * (float) std::pow(0.85, seconds * 30));
            if (levels_[ch] < 0.00001f) levels_[ch] = 0;
            if (levels_[ch] >= holds_[ch]) { holds_[ch] = levels_[ch]; ages_[ch] = 0; }
            else if ((ages_[ch] += seconds) > 0.75)
                holds_[ch] = std::max(levels_[ch], holds_[ch] * (float) std::pow(0.1, seconds));
            if (holds_[ch] < 0.00001f) holds_[ch] = 0;
        }
        if (! spectrum)
        {
            left_.reset(); right_.reset(); curve_.fill(SpectrumAnalyzer::kFloorDb);
            return changed();
        }
        if (peaks[0] <= 0 && peaks[1] <= 0 && std::all_of(curve_.begin(), curve_.end(), [](float db) { return db <= SpectrumAnalyzer::kFloorDb; }))
            return changed(); // a silent/idle EQ needs no FFT and does not invalidate the root
        if (n > 0)
        {
            left_.push(samplesL_.data(), nullptr, n); right_.push(samplesR_.data(), nullptr, n);
        }
        else // stopped transport / bypass / disconnected graph: never freeze the previous FFT
        {
            samplesL_.fill(0);
            left_.push(samplesL_.data(), nullptr, SpectrumAnalyzer::kFftSize);
            right_.push(samplesL_.data(), nullptr, SpectrumAnalyzer::kFftSize);
        }
        left_.analyze(seconds, true); right_.analyze(seconds, true);
        for (size_t i = 0; i < curve_.size(); ++i) curve_[i] = std::max(left_.curve()[i], right_.curve()[i]);
        return changed();
    }
    const auto& curve() const { return curve_; }
    const auto& levels() const { return levels_; }
    const auto& holds() const { return holds_; }

private:
    SpectrumAnalyzer left_, right_;
    AudioDisplayTap::Samples samplesL_{}, samplesR_{};
    std::array<float, SpectrumAnalyzer::kCurvePoints> curve_{};
    std::array<float, 2> levels_{}, holds_{};
    std::array<double, 2> ages_{};
    uint32_t generation_ = 0;
};
} // namespace sonora::dsp
