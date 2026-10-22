#include "devices/native/LfoShaper.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace sonora::devices
{
namespace
{
constexpr double periods[] = {16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 0.0625, 1.5, 0.75, 2.0 / 3, 1.0 / 3};
std::vector<ParameterInfo> infos()
{
    std::vector<ParameterInfo> p{
        makeParam("Shape", "Shape", 0, 6, 0, "", false, {"Pump", "Sine", "Triangle", "Rise", "Fall", "Gate", "Custom"}),
        makeParam("Sync", "Sync", 0, 1, 1, "", true),
        makeParam("Rate", "Rate", 0, 12, 4, "", false,
                  {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4 dot", "1/8 dot", "1/4 trip", "1/8 trip"}),
        makeParam("Hertz", "Free Rate", 0.01f, 40, 2, "Hz"), makeParam("Depth", "Volume", 0, 1, 1, "%"),
        makeParam("Phase", "Phase", 0, 1, 0, "%"), makeParam("StereoPhase", "Stereo", 0, 1, 0, "%"),
        makeParam("PanAmount", "Pan Mod", 0, 1, 0, "%"), makeParam("Smooth", "Smooth", 0, 100, 2, "ms"),
        makeParam("Mix", "Dry/Wet", 0, 1, 1, "%"),
        makeParam("Trigger", "Trigger", 0, 3, 0, "", false, {"Transport", "Free", "Note Loop", "Note One-shot"})};
    for (int i = 0; i < LfoShaper::kPoints; ++i)
    {
        const float x = (float) i / (float) (LfoShaper::kPoints - 1);
        p.push_back(makeParam("Points." + std::to_string(i), "Point " + std::to_string(i + 1), 0, 1,
                              std::min(1.0f, x * 2)));
    }
    return p;
}
}
LfoShaper::LfoShaper() : NativeDevice(infos()) {}
void LfoShaper::prepare(double sampleRate, int) { sampleRate_ = std::max(1.0, sampleRate); reset(); }
void LfoShaper::reset() { phase_ = 0; smoothedL_ = smoothedR_ = 1; triggered_ = false; }
std::vector<int> LfoShaper::displayParameters() const
{
    std::vector<int> out;
    for (int i = 0; i < PointStart; ++i) out.push_back(i);
    return out;
}
float LfoShaper::levelAt(double phase) const
{
    phase = std::clamp(phase, 0.0, 1.0);
    switch ((int) std::lround(value(Shape)))
    {
        case 0: return (float) (1 - std::pow(1 - std::min(1.0, phase * 1.8), 3));
        case 1: return (float) (0.5 - 0.5 * std::cos(phase * 2 * std::numbers::pi));
        case 2: return (float) (1 - std::abs(phase * 2 - 1));
        case 3: return (float) phase;
        case 4: return (float) (1 - phase);
        case 5: return phase < 0.5 ? 1.0f : 0.0f;
        default:
        {
            const double x = phase * (kPoints - 1);
            const int index = std::min(kPoints - 2, (int) x);
            const float a = value(PointStart + index), b = value(PointStart + index + 1);
            return a + (b - a) * (float) (x - index);
        }
    }
}
void LfoShaper::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    const int trigger = (int) std::lround(value(Trigger));
    const double tempo = std::isfinite(host_.tempo) && host_.tempo > 0 ? host_.tempo : 120;
    const double period = periods[std::clamp((int) std::lround(value(Rate)), 0, 12)];
    const double step = value(Sync) >= 0.5f ? tempo / (60 * sampleRate_ * period) : value(Hertz) / sampleRate_;
    if (trigger == 0 && value(Sync) >= 0.5f && std::isfinite(host_.beats))
    { phase_ = host_.beats / period; phase_ -= std::floor(phase_); }
    const float depth = value(Depth), mix = value(Mix), pan = value(PanAmount);
    const double offset = value(Phase), stereo = value(StereoPhase);
    const float smoothing = value(Smooth) <= 0 ? 1.0f : (float) (1 - std::exp(-1000 / (sampleRate_ * value(Smooth))));
    auto events = midi.begin();
    const auto end = midi.end();
    for (int s = 0; s < audio.getNumSamples(); ++s)
    {
        while (events != end && (*events).samplePosition <= s)
        {
            const auto meta = *events;
            if (trigger >= 2 && meta.numBytes <= 3 && meta.getMessage().isNoteOn()) { phase_ = 0; triggered_ = true; }
            ++events;
        }
        const bool enabled = trigger < 2 || triggered_;
        auto sampleCurve = [&](double phase) { phase -= std::floor(phase); return levelAt(phase); };
        const float left = enabled ? sampleCurve(phase_ + offset) : 1;
        const float right = enabled ? sampleCurve(phase_ + offset + stereo) : 1;
        const float position = enabled ? (left * 2 - 1) * pan : 0;
        const float gl = (1 - depth + depth * left) * std::min(1.0f, 1 - position);
        const float gr = (1 - depth + depth * right) * std::min(1.0f, 1 + position);
        smoothedL_ += (gl - smoothedL_) * smoothing; smoothedR_ += (gr - smoothedR_) * smoothing;
        audio.setSample(0, s, audio.getSample(0, s) * (1 - mix + mix * smoothedL_));
        audio.setSample(1, s, audio.getSample(1, s) * (1 - mix + mix * smoothedR_));
        if ((trigger != 0 || host_.playing) && enabled)
        {
            phase_ += step;
            if (phase_ >= 1)
            {
                if (trigger == 3) { triggered_ = false; phase_ = 0; }
                else phase_ -= std::floor(phase_);
            }
        }
    }
}
} // namespace sonora::devices
