#include "devices/native/ClassicEffects.h"
namespace sonora::devices
{
Gate::Gate() : NativeDevice({makeParam("Threshold", "Threshold", 0.000001f, 1, 0.01f, "lin"),
    makeParam("Attack", "Attack", 0.01f, 1000, 3, "ms"), makeParam("Hold", "Hold", 0, 10000, 10, "ms"),
    makeParam("Release", "Release", 0.1f, 10000, 50, "ms"), makeParam("Return", "Return", 0, 24, 3, "dB"),
    makeParam("Gain", "Floor", -75, 0, -40, "dB"), makeParam("FlipMode", "Flip", 0, 1, 0, "", true),
    makeParam("SideListen", "Listen", 0, 1, 0, "", true), makeParam("LookAhead", "Lookahead", 0, 2, 0, "", false, {"Off", "1.5 ms", "10 ms"})}) {}
void Gate::prepare(double rate, int)
{
    sampleRate_ = rate;
    const int setting = (int) std::lround(value(Lookahead));
    latency_ = (int) std::lround(rate * (setting == 1 ? 0.0015 : setting == 2 ? 0.01 : 0));
    for (auto& delay : delay_) delay.prepare(latency_ + 1);
    reset();
}
void Gate::reset() { for (auto& delay : delay_) delay.reset(); resetState(); reduction_.store(0); }
void Gate::resetState()
{
    flipState_ = value(Flip) > 0.5f;
    gain_ = flipState_ ? 1.0f : 0.0f;
    detector_ = 0; hold_ = 0; open_ = false;
}
void Gate::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed() || audio.getNumChannels() < 2) { reduction_.store(0, std::memory_order_relaxed); return; }
    const float offset = dsp::dbToGain(kThresholdOffsetDb);
    const float openLevel = value(Threshold) * offset, closeLevel = openLevel * dsp::dbToGain(-value(Return));
    const float openPower = openLevel * openLevel, closePower = closeLevel * closeLevel;
    const float floor = value(Floor) <= -74.99f ? 0.0f : dsp::dbToGain(value(Floor));
    const float attack = dsp::timeToCoef(value(Attack), sampleRate_), release = dsp::timeToCoef(value(Release), sampleRate_);
    const float up = dsp::timeToCoef(kDetectorUpMs, sampleRate_), down = dsp::timeToCoef(kDetectorDownMs, sampleRate_);
    const int holdSamples = (int) std::lround(value(Hold) * (float) sampleRate_ * 0.001f);
    const bool flip = value(Flip) > 0.5f, listen = value(Listen) > 0.5f;
    if (flip != flipState_) resetState();
    auto* left = audio.getWritePointer(0); auto* right = audio.getWritePointer(1);
    const bool external = sidechain_ && sidechain_->getNumChannels() > 0 && sidechain_->getNumSamples() >= audio.getNumSamples();
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const float dl = external ? sidechain_->getSample(0, i) * sidechainGain_ : left[i];
        const float dr = external ? sidechain_->getSample(std::min(1, sidechain_->getNumChannels() - 1), i) * sidechainGain_ : right[i];
        // The reference suspends the gate during digital silence: it restarts closed, from gain 0, when sound returns.
        if (left[i] == 0.0f && right[i] == 0.0f && dl == 0.0f && dr == 0.0f)
        {
            if (silent_ >= kSilenceResetSamples) { resetState(); continue; }
            ++silent_;
        }
        else silent_ = 0;
        const float power = 0.5f * (dl * dl + dr * dr);
        detector_ += (power - detector_) * (power > detector_ ? up : down);
        if (detector_ >= openPower) { open_ = true; hold_ = holdSamples; }
        else if (detector_ < closePower) { if (hold_ > 0) --hold_; else open_ = false; }
        const float target = open_ != flip ? 1.0f : floor;
        const bool rising = target > gain_;
        gain_ += (target - gain_) * (rising != flip ? attack : release);
        delay_[0].write(left[i]); delay_[1].write(right[i]);
        left[i] = listen ? dl : delay_[0].read((float) latency_) * gain_;
        right[i] = listen ? dr : delay_[1].read((float) latency_) * gain_;
    }
    reduction_.store(dsp::gainToDb(std::max(gain_, 1e-9f)), std::memory_order_relaxed);
}
AutoPan::AutoPan() : NativeDevice({makeParam("Lfo/LfoAmount", "Amount", 0, 1, 0, "%"),
    makeParam("Lfo/Frequency", "Rate", 0.01f, 40, 1, "Hz"), makeParam("Lfo/RateType", "Sync", 0, 1, 0, "", true),
    makeParam("Lfo/BeatRate", "Beat", 0, 16, 8, "", false, {"16 Bars", "12 Bars", "8 Bars", "6 Bars", "4 Bars", "3 Bars", "2 Bars", "1.5 Bars", "1 Bar", "3/4", "1/2", "3/8", "1/4", "3/16", "1/8", "1/16", "1/32"}),
    makeParam("Lfo/Type", "Wave", 0, 5, 0, "", false, {"Sine", "Triangle", "Down", "Up", "Square", "Random"}),
    makeParam("Lfo/Phase", "Phase", 0, 360, 180, "deg"), makeParam("Lfo/Offset", "Offset", 0, 360, 0, "deg"),
    makeParam("Lfo/LfoInvert", "Invert", 0, 1, 0, "", true), makeParam("Lfo/IsOn", "LFO", 0, 1, 1, "", true)}) {}
float AutoPan::waveform(double phase, int shape)
{
    const double cycle = std::floor(phase); const float p = (float) (phase - cycle);
    switch (shape)
    {
        case 1: return 1 - 4 * std::abs(p - 0.5f);
        case 2: return 1 - 2 * p;
        case 3: return 2 * p - 1;
        case 4: return p < 0.5f ? 1 : -1;
        case 5:
        {
            uint32_t seed = (uint32_t) (int64_t) cycle + 0x721c35abu; seed ^= seed >> 16; seed *= 0x7feb352du; seed ^= seed >> 15;
            return (float) (seed >> 8) / 8388608.0f - 1;
        }
        default: return std::sin(2 * (float) dsp::kPi * p);
    }
}
void AutoPan::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed() || value(On) < 0.5f) return;
    static constexpr double beats[]{64, 48, 32, 24, 16, 12, 8, 6, 4, 3, 2, 1.5, 1, 0.75, 0.5, 0.25, 0.125};
    const double hz = value(Sync) > 0.5f ? tempo_ / 60 / beats[std::clamp((int) std::lround(value(Beat)), 0, 16)] : value(Rate);
    const double increment = hz / sampleRate_, offset = value(Offset) / 360.0, stereo = value(Phase) / 360.0;
    const float amount = value(Amount), sign = value(Invert) > 0.5f ? -1.0f : 1.0f;
    const int shape = (int) std::lround(value(Shape));
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        for (int ch = 0; ch < std::min(2, audio.getNumChannels()); ++ch)
            audio.getWritePointer(ch)[i] *= 1 - amount * (1 - sign * waveform(phase_ + offset + (ch == 1 ? stereo : 0), shape)) * 0.5f;
        phase_ += increment;
        // Keep random cycles deterministic without unbounded float phase growth over long sessions.
        if (phase_ >= 1048576) phase_ -= 1048576;
    }
}
}
