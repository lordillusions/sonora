#include "devices/native/FmSynth.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace sonora::devices
{
namespace
{
std::vector<ParameterInfo> infos()
{
    std::vector<ParameterInfo> p{
        makeParam("Algorithm", "Algorithm", 0, 7, 0, "", false,
                  {"D>C>B>A", "(B+C+D)>A", "B>A + D>C", "C>B>A + D", "D>(B+C)>A", "D>C + B + A", "D>(A+B+C)", "A+B+C+D"}),
        makeParam("Volume", "Volume", -60, 12, -12, "dB"), makeParam("Transpose", "Transpose", -48, 48, 0, "st"),
        makeParam("BendRange", "Bend Range", 0, 48, 2, "st"), makeParam("Feedback", "Feedback", 0, 1, 0, "%"),
        makeParam("Pan", "Pan", -1, 1, 0)};
    for (int i = 0; i < 4; ++i)
    {
        const auto path = "Osc." + std::to_string(i) + "/";
        const auto label = std::string(1, (char) ('A' + i)) + " ";
        p.push_back(makeParam(path + "Ratio", label + "Ratio", 0.25f, 32, 1));
        p.push_back(makeParam(path + "Fine", label + "Fine", -100, 100, 0, "ct"));
        p.push_back(makeParam(path + "Level", label + "Level", 0, 1, i == 0 ? 1.0f : 0.0f, "%"));
        p.push_back(makeParam(path + "Wave", label + "Wave", 0, 3, 0, "", false, {"Sine", "Triangle", "Saw", "Square"}));
        p.push_back(makeParam(path + "Attack", label + "Attack", 0.1f, 20000, 5, "ms"));
        p.push_back(makeParam(path + "Decay", label + "Decay", 1, 20000, 500, "ms"));
        p.push_back(makeParam(path + "Sustain", label + "Sustain", 0, 1, 0.7f, "%"));
        p.push_back(makeParam(path + "Release", label + "Release", 1, 20000, 200, "ms"));
    }
    return p;
}
float wave(double phase, int kind)
{
    phase -= std::floor(phase);
    const double angle = phase * 2 * std::numbers::pi;
    // Finite harmonic sums keep discontinuous waveforms bounded. FM sidebands can still alias.
    if (kind == 0) return (float) std::sin(angle);
    double result = 0;
    for (int h = 1; h <= 9; ++h)
    {
        if (kind != 2 && h % 2 == 0) continue;
        double amplitude = kind == 1 ? (h % 4 == 1 ? 1.0 : -1.0) / (h * h) : 1.0 / h;
        result += std::sin(angle * h) * amplitude;
    }
    return (float) (result * (kind == 1 ? 0.81 : kind == 2 ? 0.5 : 0.9));
}
}

FmSynth::FmSynth() : NativeDevice(infos()) {}
void FmSynth::prepare(double sampleRate, int) { sampleRate_ = std::max(1.0, sampleRate); reset(); }
void FmSynth::reset() { voices_ = {}; bends_ = {}; pedals_ = {}; age_ = 0; }

std::vector<int> FmSynth::displayParameters() const
{
    std::vector<int> result;
    for (int i = 0; i < (int) parameters().size(); ++i) result.push_back(i);
    return result;
}

void FmSynth::event(const juce::MidiMessage& m)
{
    const auto ch = (size_t) std::clamp(m.getChannel() - 1, 0, 15);
    if (m.isNoteOn())
    {
        auto it = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.note < 0; });
        if (it == voices_.end()) it = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) { return a.age < b.age; });
        *it = Voice{};
        it->note = m.getNoteNumber(); it->channel = (int) ch; it->keyDown = true;
        it->velocity = m.getFloatVelocity(); it->age = ++age_;
        for (auto& e : it->env) e.stage = 1;
    }
    else if (m.isNoteOff())
    {
        // Pair repeated pitches in arrival order, independently for each MIDI channel.
        Voice* found = nullptr;
        for (auto& v : voices_)
            if (v.note == m.getNoteNumber() && v.channel == (int) ch && v.keyDown && (! found || v.age < found->age)) found = &v;
        if (found) { found->keyDown = false; found->released = ! pedals_[ch]; }
    }
    else if (m.isPitchWheel()) bends_[ch] = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
    else if (m.isAllSoundOff()) { for (auto& v : voices_) if (v.channel == (int) ch) v = Voice{}; }
    else if (m.isAllNotesOff())
    { for (auto& v : voices_) if (v.channel == (int) ch) { v.keyDown = false; v.released = true; } }
    else if (m.isController() && m.getControllerNumber() == 64)
    {
        pedals_[ch] = m.getControllerValue() >= 64;
        if (! pedals_[ch]) for (auto& v : voices_) if (v.channel == (int) ch && ! v.keyDown) v.released = true;
    }
    else if (m.isResetAllControllers())
    {
        bends_[ch] = 0; pedals_[ch] = false;
        for (auto& v : voices_) if (v.channel == (int) ch && ! v.keyDown) v.released = true;
    }
}

void FmSynth::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = std::clamp(meta.samplePosition, pos, audio.getNumSamples());
        render(audio, pos, at); pos = at;
        if (meta.numBytes <= 3) event(meta.getMessage());
    }
    render(audio, pos, audio.getNumSamples());
}

void FmSynth::render(juce::AudioBuffer<float>& audio, int from, int to)
{
    if (from >= to) return;
    const int algorithm = (int) std::lround(value(Algorithm));
    const float gain = std::pow(10.0f, value(Volume) / 20.0f);
    const float pan = value(Pan), feedback = value(Feedback);
    std::array<float, 4> levels{}, attack{}, decay{}, sustain{}, release{};
    std::array<double, 4> ratios{};
    std::array<int, 4> waves{};
    for (int o = 0; o < 4; ++o)
    {
        const auto i = (size_t) o;
        ratios[i] = value(oscParam(o, Ratio)) * std::exp2(value(oscParam(o, Fine)) / 1200.0);
        levels[i] = value(oscParam(o, Level)); waves[i] = (int) std::lround(value(oscParam(o, Wave)));
        attack[i] = (float) (1000 / (sampleRate_ * value(oscParam(o, Attack))));
        decay[i] = (float) (1000 / (sampleRate_ * value(oscParam(o, Decay))));
        sustain[i] = value(oscParam(o, Sustain)); release[i] = (float) (1000 / (sampleRate_ * value(oscParam(o, Release))));
    }
    for (auto& v : voices_)
    {
        if (v.note < 0) continue;
        const double frequency = 440 * std::exp2(((double) v.note - 69 + value(Transpose) + bends_[(size_t) v.channel] * value(BendRange)) / 12.0);
        std::array<double, 4> steps{};
        for (size_t o = 0; o < 4; ++o) steps[o] = std::min(0.45, frequency * ratios[o] / sampleRate_);
        for (int s = from; s < to; ++s)
        {
            std::array<float, 4> amplitude{}, out{};
            bool active = false;
            for (size_t o = 0; o < 4; ++o)
            {
                auto& e = v.env[o];
                if (v.released && e.stage > 0 && e.stage < 4) { e.stage = 4; e.releaseStep = e.level * release[o]; }
                if (e.stage == 1) { e.level = std::min(1.0f, e.level + attack[o]); if (e.level >= 1) e.stage = 2; }
                else if (e.stage == 2) { e.level = std::max(sustain[o], e.level - decay[o] * (1 - sustain[o])); if (e.level <= sustain[o]) e.stage = 3; }
                else if (e.stage == 3) e.level = sustain[o];
                else if (e.stage == 4) { e.level = std::max(0.0f, e.level - e.releaseStep); if (e.level <= 0) e.stage = 0; }
                active |= e.stage != 0;
                amplitude[o] = levels[o] * e.level;
            }
            if (! active) { v.note = -1; break; }
            auto op = [&](size_t o, float mod) { return wave(v.phase[o] + mod, waves[o]) * amplitude[o]; };
            out[3] = op(3, v.feedback * feedback); v.feedback = out[3];
            float result = 0;
            switch (algorithm)
            {
                case 0: out[2] = op(2, out[3]); out[1] = op(1, out[2]); result = op(0, out[1]); break;
                case 1: result = op(0, op(1, 0) + op(2, 0) + out[3]); break;
                case 2: result = (op(0, op(1, 0)) + op(2, out[3])) * 0.5f; break;
                case 3: result = (op(0, op(1, op(2, 0))) + out[3]) * 0.5f; break;
                case 4: result = op(0, op(1, out[3]) + op(2, out[3])); break;
                case 5: result = (op(0, 0) + op(1, 0) + op(2, out[3])) / 3; break;
                case 6: result = (op(0, out[3]) + op(1, out[3]) + op(2, out[3])) / 3; break;
                default: result = (op(0, 0) + op(1, 0) + op(2, 0) + out[3]) * 0.25f; break;
            }
            result *= gain * v.velocity;
            audio.addSample(0, s, result * std::min(1.0f, 1 - pan));
            audio.addSample(1, s, result * std::min(1.0f, 1 + pan));
            for (size_t o = 0; o < 4; ++o) { v.phase[o] += steps[o]; v.phase[o] -= std::floor(v.phase[o]); }
        }
    }
}
} // namespace sonora::devices
