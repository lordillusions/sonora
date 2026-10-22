#include "devices/native/VectorSynth.h"

namespace sonora::devices
{
namespace
{
std::vector<ParameterInfo> vectorParameters()
{
    return {
        makeParam("Osc1/Position", "Position", 0, 1, 0.35f, "%"), makeParam("Osc2/Position", "Position", 0, 1, 0.7f, "%"),
        makeParam("Osc1/Level", "Level", 0, 1, 0.8f, "%"), makeParam("Osc2/Level", "Level", 0, 1, 0, "%"),
        makeParam("Osc1/Tune", "Semi", -48, 48, 0, "st"), makeParam("Osc2/Tune", "Semi", -48, 48, 0, "st"),
        makeParam("Osc1/Detune", "Detune", -100, 100, 0, "ct"), makeParam("Osc2/Detune", "Detune", -100, 100, 7, "ct"),
        makeParam("Filter/On", "Filter", 0, 1, 1, "", true), makeParam("Filter/Frequency", "Frequency", 20, 20000, 12000, "Hz"),
        makeParam("Filter/Resonance", "Resonance", 0, 1, 0.1f, "%"),
        makeParam("Filter/Type", "Type", 0, 3, 0, "", false, {"Low-pass", "High-pass", "Band-pass", "Notch"}),
        makeParam("Amp/Attack", "Attack", 0.1f, 20000, 5, "ms"), makeParam("Amp/Decay", "Decay", 1, 20000, 500, "ms"),
        makeParam("Amp/Sustain", "Sustain", 0, 1, 0.7f, "%"), makeParam("Amp/Release", "Release", 1, 20000, 250, "ms"),
        makeParam("Lfo/Rate", "Rate", 0.01f, 20, 1, "Hz"), makeParam("Lfo/Position", "Position Amt", 0, 1, 0, "%"),
        makeParam("Volume", "Volume", -60, 6, -12, "dB"), makeParam("Pan", "Pan", -1, 1, 0),
        makeParam("Sub/Level", "Sub", 0, 1, 0, "%"), makeParam("Transpose", "Transpose", -48, 48, 0, "st")};
}
}
VectorSynth::VectorSynth() : NativeDevice(vectorParameters())
{
    constexpr int length = WavetableData::nativeFrameLength;
    std::vector<float> samples((size_t) length * 4);
    for (int i = 0; i < length; ++i)
    {
        const double phase = (double) i / length;
        samples[(size_t) i] = (float) std::sin(2 * dsp::kPi * phase);
        samples[(size_t) (length + i)] = (float) (1 - 4 * std::abs(phase - 0.5));
        samples[(size_t) (2 * length + i)] = (float) (2 * phase - 1);
        samples[(size_t) (3 * length + i)] = phase < 0.5 ? 1.0f : -1.0f;
    }
    table_ = WavetableData::fromSamples(samples, 1, length, 44100, "Basic Shapes").data;
}
void VectorSynth::prepare(double rate, int) { sampleRate_ = std::max(1.0, rate); reset(); }
void VectorSynth::reset() { voices_ = {}; bends_ = {}; pedals_ = {}; lfoPhase_ = 0; age_ = 0; }
void VectorSynth::event(const uint8_t* data, int size)
{
    if (size < 1 || data[0] < 0x80 || data[0] >= 0xf0) return;
    const auto channel = (size_t) (data[0] & 15);
    const int kind = data[0] & 0xf0;
    if (size >= 3 && kind == 0x90 && data[2] > 0)
    {
        auto it = std::find_if(voices_.begin(), voices_.end(), [](const auto& v) { return v.note < 0; });
        if (it == voices_.end()) it = std::min_element(voices_.begin(), voices_.end(), [](const auto& a, const auto& b) { return a.age < b.age; });
        *it = Voice{}; it->note = data[1]; it->channel = (int) channel; it->held = true;
        it->velocity = (float) data[2] / 127; it->age = ++age_; it->stage = 1;
    }
    else if (size >= 3 && (kind == 0x80 || kind == 0x90))
    {
        Voice* found = nullptr;
        for (auto& v : voices_)
            if (v.note == data[1] && v.channel == (int) channel && v.held && (! found || v.age < found->age)) found = &v;
        if (found) { found->held = false; if (! pedals_[channel]) found->stage = 4; }
    }
    else if (size >= 3 && kind == 0xe0) bends_[channel] = (float) ((int) data[1] + 128 * data[2] - 8192) / 8192;
    else if (size >= 3 && kind == 0xb0)
    {
        if (data[1] == 64 || data[1] == 121)
        {
            pedals_[channel] = data[1] == 64 && data[2] >= 64;
            if (data[1] == 121) bends_[channel] = 0;
            if (! pedals_[channel]) for (auto& v : voices_) if (v.channel == (int) channel && ! v.held) v.stage = 4;
        }
        if (data[1] == 120 || data[1] == 123)
            for (auto& v : voices_) if (v.channel == (int) channel)
            { if (data[1] == 120) v = Voice{}; else { v.held = false; v.stage = 4; } }
    }
}
void VectorSynth::render(juce::AudioBuffer<float>& audio, int start, int end)
{
    std::array<float, NumParams> p;
    for (int i = 0; i < NumParams; ++i) p[(size_t) i] = value(i);
    const float attackStep = 1 / (p[Attack] * 0.001f * (float) sampleRate_);
    const float decayStep = (1 - p[Sustain]) / (p[Decay] * 0.001f * (float) sampleRate_);
    const float gain = dsp::dbToGain(p[Volume]);
    const float left = std::min(1.0f, 1 - p[Pan]), right = std::min(1.0f, 1 + p[Pan]);
    for (auto& v : voices_) if (v.note >= 0)
        v.filter.setup(sampleRate_, p[Cutoff], 0.707f + 8 * p[Resonance]);
    for (int sample = start; sample < end; ++sample)
    {
        const float mod = (float) std::sin(2 * dsp::kPi * lfoPhase_) * p[LfoAmount];
        lfoPhase_ += p[LfoRate] / sampleRate_; lfoPhase_ -= std::floor(lfoPhase_);
        float sum = 0;
        for (auto& v : voices_)
        {
            if (v.note < 0) continue;
            if (v.stage == 1) { v.envelope = std::min(1.0f, v.envelope + attackStep); if (v.envelope >= 1) v.stage = 2; }
            else if (v.stage == 2) { v.envelope = std::max(p[Sustain], v.envelope - decayStep); if (v.envelope <= p[Sustain]) v.stage = 3; }
            else if (v.stage == 3) v.envelope = p[Sustain];
            else if (v.stage == 4)
            {
                if (v.releaseStep <= 0) v.releaseStep = v.envelope / (p[Release] * 0.001f * (float) sampleRate_);
                v.envelope -= v.releaseStep;
                if (v.envelope <= 0) { v = Voice{}; continue; }
            }
            const double base = 440 * std::exp2(((double) v.note - 69 + p[Transpose] + 2 * bends_[(size_t) v.channel]) / 12) / sampleRate_;
            float output = 0;
            for (int osc = 0; osc < 2; ++osc)
            {
                const double inc = std::min(0.49, base * std::exp2((p[(size_t) Tune1 + (size_t) osc] + p[(size_t) Detune1 + (size_t) osc] / 100) / 12));
                output += table_->sample(std::clamp(p[(size_t) Position1 + (size_t) osc] + mod, 0.0f, 1.0f), v.phase[osc], table_->mipForIncrement(inc)) * p[(size_t) Level1 + (size_t) osc];
                v.phase[osc] += inc; v.phase[osc] -= std::floor(v.phase[osc]);
            }
            output += (float) std::sin(2 * dsp::kPi * v.subPhase) * p[SubLevel];
            v.subPhase += base * 0.5; v.subPhase -= std::floor(v.subPhase);
            if (p[FilterOn] > 0.5f) output = v.filter.process(output, (dsp::Svf::Mode) std::clamp((int) std::lround(p[FilterType]), 0, 3));
            sum += output * v.envelope * v.velocity * gain * 0.5f;
        }
        for (int ch = 0; ch < std::min(2, audio.getNumChannels()); ++ch) audio.addSample(ch, sample, sum * (ch == 0 ? left : right));
    }
}
void VectorSynth::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    int position = 0;
    for (const auto metadata : midi)
    {
        const int offset = std::clamp(metadata.samplePosition, position, audio.getNumSamples());
        render(audio, position, offset); event(metadata.data, metadata.numBytes); position = offset;
    }
    render(audio, position, audio.getNumSamples());
}
}
