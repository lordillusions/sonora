#include "devices/native/Tuner.h"
namespace sonora::devices
{
Tuner::Tuner() : NativeDevice({makeParam("TuningFreq", "Reference", 410, 480, 440, "Hz"),
    makePlainParam(makeParam("DisplayPitchInHz", "Hz", 0, 1, 0, "", true)),
    makePlainParam(makeParam("ViewMode", "View", 0, 1, 0, "", false, {"Classic", "History"})),
    makePlainParam(makeParam("StrobeMode", "Strobe", 0, 1, 0, "", true)),
    makePlainParam(makeParam("AccidentalSpelling", "Spelling", 0, 2, 0, "", false, {"Sharps", "Flats", "Both"}))}) {}
void Tuner::prepare(double rate, int)
{
    decimation_ = std::max(1, (int) std::floor(rate / 8000));
    analysisRate_ = rate / decimation_;
    lowpass_.setup(rate, (float) analysisRate_ * 0.4f, 0.707f); reset();
}
void Tuner::reset()
{
    ring_.fill(0); differences_.fill(0); write_ = filled_ = hop_ = countdown_ = 0;
    lowpass_.reset(); frequency_.store(0); confidence_.store(0);
}
void Tuner::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed() || audio.getNumChannels() == 0) { frequency_.store(0); confidence_.store(0); return; }
    const auto* left = audio.getReadPointer(0);
    const auto* right = audio.getReadPointer(std::min(1, audio.getNumChannels() - 1));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float sample = lowpass_.process((left[i] + right[i]) * 0.5f, dsp::Svf::Mode::LowPass);
        if (++countdown_ < decimation_) continue;
        countdown_ = 0; ring_[(size_t) write_] = sample; write_ = (write_ + 1) % kWindow;
        filled_ = std::min(kWindow, filled_ + 1);
        if (++hop_ >= 256) { hop_ = 0; if (filled_ == kWindow) analyse(); }
    }
}
void Tuner::analyse()
{
    constexpr int span = 1024;
    // Use the newest 1536 samples; a silent input clears a previous result promptly.
    const int origin = (write_ + kWindow - span - kMaxLag) % kWindow;
    double energy = 0;
    for (int i = 0; i < span; ++i) { const float v = ring_[(size_t) ((origin + kMaxLag + i) % kWindow)]; energy += v * v; }
    if (energy / span < 1e-8) { frequency_.store(0); confidence_.store(0); return; }
    differences_[0] = 1;
    double sum = 0;
    for (int lag = 1; lag <= kMaxLag; ++lag)
    {
        double difference = 0;
        for (int i = 0; i < span; ++i)
        {
            const float delta = ring_[(size_t) ((origin + i) % kWindow)] - ring_[(size_t) ((origin + i + lag) % kWindow)];
            difference += delta * delta;
        }
        rawDifferences_[(size_t) lag] = (float) difference;
        sum += difference;
        differences_[(size_t) lag] = sum > 1e-15 ? (float) (difference * lag / sum) : 1;
    }
    const int minimum = std::max(2, (int) (analysisRate_ / 2000));
    int best = -1;
    for (int lag = minimum; lag < kMaxLag - 1; ++lag)
        if (differences_[(size_t) lag] < 0.15f)
        {
            while (lag + 1 < kMaxLag && differences_[(size_t) (lag + 1)] < differences_[(size_t) lag]) ++lag;
            best = lag; break;
        }
    if (best < 0 || best >= kMaxLag) { frequency_.store(0); confidence_.store(0); return; }
    const float a = rawDifferences_[(size_t) (best - 1)], b = rawDifferences_[(size_t) best], c = rawDifferences_[(size_t) (best + 1)];
    const float denominator = a - 2 * b + c;
    const float correction = std::abs(denominator) > 1e-9f ? std::clamp(0.5f * (a - c) / denominator, -0.5f, 0.5f) : 0;
    frequency_.store((float) analysisRate_ / ((float) best + correction), std::memory_order_relaxed);
    confidence_.store(1 - differences_[(size_t) best], std::memory_order_relaxed);
}
}
