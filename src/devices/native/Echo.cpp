#include "devices/native/Echo.h"

namespace sonora::devices
{
Echo::Echo() : NativeDevice({
    makeParam("Input", "Input", -24, 24, 0, "dB"),
    makeParam("TimeL", "Left", 1, 4000, 375, "ms"), makeParam("TimeR", "Right", 1, 4000, 375, "ms"),
    makeParam("SyncL", "Sync", 0, 1, 1, "", true), makeParam("SyncR", "Sync", 0, 1, 1, "", true),
    makeParam("DivisionL", "Left", 0, 7, 3, "", false, {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2"}),
    makeParam("DivisionR", "Right", 0, 7, 3, "", false, {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1", "2"}),
    makeParam("Feedback", "Feedback", 0, 0.98f, 0.4f, "%"), makeParam("Invert", "Invert", 0, 1, 0, "", true),
    makeParam("Mode", "Mode", 0, 2, 0, "", false, {"Stereo", "Ping Pong", "Mid/Side"}),
    makeParam("Filter/On", "Filter", 0, 1, 1, "", true), makeParam("Filter/HP", "HP", 20, 20000, 80, "Hz"),
    makeParam("Filter/LP", "LP", 20, 20000, 8000, "Hz"), makeParam("Modulation/Rate", "Rate", 0.01f, 20, 0.3f, "Hz"),
    makeParam("Modulation/Amount", "Modulation", 0, 1, 0, "%"), makeParam("Duck", "Ducking", 0, 1, 0, "%"),
    makeParam("Output", "Output", -24, 24, 0, "dB"), makeParam("DryWet", "Dry/Wet", 0, 1, 0.3f, "%")}) {}
double Echo::delayMilliseconds(int channel) const
{
    const bool right = channel != 0;
    return getParameter(right ? SyncR : SyncL) > 0.5f
        ? 60000 / tempo_ * std::exp2((double) std::clamp((int) std::lround(getParameter(right ? DivisionR : DivisionL)), 0, 7) - 4)
        : getParameter(right ? TimeR : TimeL);
}
void Echo::prepare(double rate, int)
{
    sampleRate_ = std::max(1.0, rate);
    for (auto& delay : delays_) delay.prepare((int) (sampleRate_ * 25));
    reset();
}
void Echo::reset()
{
    for (auto& delay : delays_) delay.reset();
    for (auto& f : high_) f.reset();
    for (auto& f : low_) f.reset();
    phase_ = 0; detector_ = 0;
    for (int ch = 0; ch < 2; ++ch) times_[(size_t) ch] = (float) (delayMilliseconds(ch) * sampleRate_ * 0.001);
}
void Echo::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed() || audio.getNumChannels() < 1) return;
    const float inputGain = dsp::dbToGain(value(Input)), outputGain = dsp::dbToGain(value(Output));
    const float mix = value(DryWet), feedback = value(Feedback) * (value(Invert) > 0.5f ? -1.0f : 1.0f);
    const int mode = (int) std::lround(value(Mode));
    const bool filtered = value(FilterOn) > 0.5f;
    const double rate = value(ModRate) / sampleRate_;
    const float amount = value(ModAmount), duck = value(Duck);
    const float smoothing = dsp::timeToCoef(25, sampleRate_), release = dsp::timeToCoef(100, sampleRate_);
    float targets[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        targets[ch] = (float) (delayMilliseconds(ch) * sampleRate_ * 0.001);
        high_[ch].setup(sampleRate_, value(HighPass), 0.707f); low_[ch].setup(sampleRate_, value(LowPass), 0.707f);
    }
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        float dry[2]{audio.getSample(0, i), audio.getSample(std::min(1, audio.getNumChannels() - 1), i)};
        const float peak = std::max(std::abs(dry[0]), std::abs(dry[1]));
        detector_ += (peak - detector_) * (peak > detector_ ? 0.1f : release);
        float input[2]{dry[0] * inputGain, dry[1] * inputGain};
        if (mode == 2) { input[0] = (dry[0] + dry[1]) * 0.5f * inputGain; input[1] = (dry[0] - dry[1]) * 0.5f * inputGain; }
        if (mode == 1) { input[0] = (input[0] + input[1]) * 0.5f; input[1] = 0; }
        float wet[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            times_[(size_t) ch] += smoothing * (targets[ch] - times_[(size_t) ch]);
            const float modulation = (float) std::sin(2 * dsp::kPi * (phase_ + ch * 0.25)) * amount * 0.01f * (float) sampleRate_;
            wet[ch] = delays_[ch].read(std::max(0.0f, times_[(size_t) ch] + modulation - 1));
            if (filtered) wet[ch] = low_[ch].process(high_[ch].process(wet[ch], dsp::Svf::Mode::HighPass), dsp::Svf::Mode::LowPass);
        }
        for (int ch = 0; ch < 2; ++ch)
        {
            const float sample = input[ch] + wet[mode == 1 ? 1 - ch : ch] * feedback;
            delays_[ch].write(std::clamp(sample, -16.0f, 16.0f));
        }
        if (mode == 2) { const float mid = wet[0]; wet[0] = mid + wet[1]; wet[1] = mid - wet[1]; }
        const float duckGain = 1 / (1 + 12 * duck * detector_);
        for (int ch = 0; ch < std::min(2, audio.getNumChannels()); ++ch)
            audio.setSample(ch, i, (dry[ch] * (1 - mix) + wet[ch] * mix * duckGain) * outputGain);
        phase_ += rate; phase_ -= std::floor(phase_);
    }
}
}
