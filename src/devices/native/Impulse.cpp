#include "devices/native/Impulse.h"

namespace sonora::devices
{
namespace
{
std::vector<ParameterInfo> impulseParameters()
{
    std::vector<ParameterInfo> p{makeParam("Volume", "Volume", -60, 12, -12, "dB")};
    for (int slot = 0; slot < 8; ++slot)
    {
        const auto path = "Slot" + std::to_string(slot + 1) + "/";
        p.push_back(makeParam(path + "Start", "Start", 0, 1000, 0, "ms"));
        p.push_back(makeParam(path + "Tune", "Transpose", -48, 48, 0, "st"));
        p.push_back(makeParam(path + "Decay", "Decay", 5, 10000, 1000, "ms"));
        p.push_back(makeParam(path + "Pan", "Pan", -1, 1, 0));
        p.push_back(makeParam(path + "Level", "Volume", -60, 12, 0, "dB"));
        p.push_back(makeParam(path + "FilterOn", "Filter", 0, 1, 0, "", true));
        p.push_back(makeParam(path + "Cutoff", "Frequency", 20, 20000, 12000, "Hz"));
        p.push_back(makeParam(path + "Resonance", "Resonance", 0, 1, 0.1f, "%"));
        p.push_back(makeParam(path + "Drive", "Drive", 0, 24, 0, "dB"));
    }
    return p;
}
}
Impulse::Impulse(std::vector<Simpler::Zone> zones) : NativeDevice(impulseParameters()), zones_(std::move(zones)) {}
const Simpler::Zone* Impulse::zone(int slot) const
{
    if (slot < 0 || slot >= 8) return nullptr;
    for (const auto& z : zones_) if (z.part.rootKey == notes[(size_t) slot]) return &z;
    return nullptr;
}
void Impulse::prepare(double rate, int) { sampleRate_ = std::max(1.0, rate); reset(); }
void Impulse::reset() { voices_ = {}; audition_.store(-1); activePads_.store(0); age_ = 0; }
void Impulse::start(int slot, int channel, float velocity)
{
    const auto* z = zone(slot);
    if (! z || ! z->slot || ! z->slot->get()) return;
    auto it = std::find_if(voices_.begin(), voices_.end(), [](const auto& v) { return v.slot < 0; });
    if (it == voices_.end()) it = std::min_element(voices_.begin(), voices_.end(), [](const auto& a, const auto& b) { return a.age < b.age; });
    *it = Voice{}; it->slot = slot; it->channel = channel; it->velocity = velocity; it->envelope = 1; it->age = ++age_;
    it->position = (double) z->part.sampleStart + value(slotParam(slot, Start)) * z->slot->get()->sampleRate * 0.001;
}
void Impulse::render(juce::AudioBuffer<float>& audio, int from, int to)
{
    const float master = dsp::dbToGain(value(Volume));
    for (auto& voice : voices_)
    {
        if (voice.slot < 0) continue;
        const int slot = voice.slot;
        const auto* z = zone(slot);
        const auto* sample = z && z->slot ? z->slot->get() : nullptr;
        if (! sample || sample->audio.getNumSamples() < 2) { voice = Voice{}; continue; }
        const int end = z->part.sampleEnd > 0 ? (int) std::min<int64_t>(z->part.sampleEnd, sample->audio.getNumSamples()) : sample->audio.getNumSamples();
        const double step = sample->sampleRate / sampleRate_ * std::exp2((value(slotParam(slot, Tune)) + (float) z->part.detuneCents / 100) / 12);
        const float decay = std::exp(-6.907755f / (value(slotParam(slot, Decay)) * 0.001f * (float) sampleRate_));
        const float pan = std::clamp(value(slotParam(slot, Pan)) + (float) z->part.pan, -1.0f, 1.0f);
        const float gain = master * dsp::dbToGain(value(slotParam(slot, Level))) * (float) z->part.volume;
        const float drive = dsp::dbToGain(value(slotParam(slot, Drive)));
        const bool filtered = value(slotParam(slot, FilterOn)) > 0.5f;
        for (auto& filter : voice.filter) filter.setup(sampleRate_, value(slotParam(slot, Cutoff)), 0.707f + 8 * value(slotParam(slot, Resonance)));
        for (int i = from; i < to; ++i)
        {
            if (voice.position < 0 || voice.position >= end - 1 || voice.envelope < 0.00001f) { voice = Voice{}; break; }
            const int frame = (int) voice.position;
            const float fraction = (float) (voice.position - frame);
            for (int ch = 0; ch < std::min(2, audio.getNumChannels()); ++ch)
            {
                const float* data = sample->audio.getReadPointer(std::min(ch, sample->audio.getNumChannels() - 1));
                float output = data[frame] + fraction * (data[frame + 1] - data[frame]);
                if (drive > 1.0001f) output = std::tanh(output * drive);
                if (filtered) output = voice.filter[ch].process(output, dsp::Svf::Mode::LowPass);
                audio.addSample(ch, i, output * voice.envelope * voice.velocity * gain * std::min(1.0f, 1 + (ch == 0 ? -pan : pan)));
            }
            voice.position += step; voice.envelope *= decay;
        }
    }
}
void Impulse::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    const int preview = audition_.exchange(-1, std::memory_order_acquire);
    if (preview >= 0 && preview < 8) start(preview, 0, 0.8f);
    int position = 0;
    for (const auto metadata : midi)
    {
        const int offset = std::clamp(metadata.samplePosition, position, audio.getNumSamples());
        render(audio, position, offset); position = offset;
        if (metadata.numBytes < 3) continue;
        const auto* data = metadata.data;
        const int channel = data[0] & 15, kind = data[0] & 0xf0;
        if (kind == 0x90 && data[2] > 0)
            for (int slot = 0; slot < 8; ++slot) if (data[1] == notes[(size_t) slot]) start(slot, channel, (float) data[2] / 127);
        if (kind == 0xb0 && (data[1] == 120 || data[1] == 123))
            for (auto& v : voices_) if (v.channel == channel) v = Voice{};
    }
    render(audio, position, audio.getNumSamples());
    unsigned mask = 0;
    for (const auto& v : voices_) if (v.slot >= 0) mask |= 1u << (unsigned) v.slot;
    activePads_.store(mask, std::memory_order_relaxed);
}
}
