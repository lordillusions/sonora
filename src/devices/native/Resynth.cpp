#include "devices/native/Resynth.h"
#include "dsp/ResynthNoise.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace sonora::devices
{
namespace
{
std::vector<ParameterInfo> makeResynthParameters()
{
    std::vector<ParameterInfo> infos{makeParam("Mode", "Output mode", 0, 2, 0, {}, false, {"Synth", "Hybrid", "Reference"}),
            makeParam("Example", "Example", 0, 64, 0, "int"),
            makeParam("RootOffset", "Root correction", -24, 24, 0, "st"),
            makeParam("Attack", "Attack", 0.1f, 2000, 2, "ms"),
            makeParam("Release", "Release", 2, 5000, 150, "ms"),
            makeParam("Tone", "Cutoff", 20, 20000, 20000, "Hz"),
            makeParam("Noise", "Noise", 0, 2, 0, "%"),
            makeParam("Movement", "Scan speed", 0.1f, 4, 1, "x"),
            makeParam("Sustain", "Hold position", 0.1f, 1, 0.75f, "%"),
            makeParam("Glide", "Glide", 0, 1000, 0, "ms"),
            makeParam("Velocity", "Velocity", 0, 1, 1, "%"),
            makeParam("Volume", "Output", -60, 12, 0, "dB"),
            makeParam("PitchMotion", "Pitch contour", 0, 1, 0, {}, true),
            makeParam("HybridAmount", "Sampled attack", 0, 1, 1, "%"),
            makeParam("Fundamental", "Fundamental", 0, 2, 1, "%"),
            makeParam("OddLevel", "Odd harmonics", 0, 2, 1, "%"),
            makeParam("EvenLevel", "Even harmonics", 0, 2, 1, "%"),
            makeParam("SpectralTilt", "Spectral tilt", -3, 3, 0, "dB/oct"),
            makeParam("PartialCount", "Partials", 1, 96, 96, "int"),
            makeParam("SubLevel", "Sub oscillator", 0, 1, 0, "%"),
            makeParam("AmpDecay", "Decay", 1, 5000, 250, "ms"),
            makeParam("AmpSustain", "Sustain", 0, 1, 1, "%"),
            makeParam("FilterOn", "Filter", 0, 1, 1, {}, true),
            makeParam("FilterType", "Filter type", 0, 3, 0, {}, false, {"Low-pass", "High-pass", "Band-pass", "Notch"}),
            makeParam("Resonance", "Resonance", 0, 1, 0, "%"),
            makeParam("FilterEnv", "Env to cutoff", -48, 48, 0, "st"),
            makeParam("FilterAttack", "Attack", 0.1f, 5000, 10, "ms"),
            makeParam("FilterDecay", "Decay", 1, 5000, 250, "ms"),
            makeParam("FilterSustain", "Sustain", 0, 1, 0, "%"),
            makeParam("FilterRelease", "Release", 2, 5000, 150, "ms"),
            makeParam("Drive", "Drive", 0, 24, 0, "dB"),
            makeParam("LfoRate", "LFO rate", 0.05f, 20, 1, "Hz"),
            makeParam("LfoDepth", "LFO depth", 0, 1, 0, "%"),
            makeParam("LfoShape", "LFO shape", 0, 5, 0, {}, false, {"Sine", "Triangle", "Ramp down", "Ramp up", "Square", "Random"}),
            makeParam("LfoDestination", "LFO destination", 0, 4, 0, {}, false, {"Pitch", "Timbre scan", "Cutoff", "Amplitude", "Pan"}),
            makeParam("LfoSync", "Tempo sync", 0, 1, 0, {}, true),
            makeParam("PlayMode", "Voices", 0, 2, 0, {}, false, {"Mono", "Legato", "Poly 8"}),
            makeParam("BendRange", "Bend range", 0, 24, 2, "st"),
            makeParam("NoiseLow", "Low noise", 0, 2, 1, "%"),
            makeParam("NoiseMid", "Mid noise", 0, 2, 1, "%"),
            makeParam("NoiseHigh", "Presence noise", 0, 2, 1, "%"),
            makeParam("NoiseAir", "Air noise", 0, 2, 1, "%"),
            makeParam("TimbreStart", "Scan start", 0, 0.95f, 0, "%"),
            makeParam("ScanMode", "Scan mode", 0, 2, 0, {}, false, {"Evolve and hold", "Loop", "Freeze"})};
    for (int h = 0; h < model::ResynthFrame::harmonics; ++h)
        infos.push_back(makeParam("Harmonic." + std::to_string(h + 1), "Harmonic " + std::to_string(h + 1), 0, 2, 1, "%"));
    return infos;
}
constexpr int tableSize = 512;
double hz(double pitch) { return 440 * std::exp2((pitch - 69) / 12); }
}

Resynth::Resynth(std::shared_ptr<const model::ResynthModel> model) : NativeDevice(makeResynthParameters()), model_(std::move(model))
{
    std::string error;
    if (model_ && ! model::validateResynth(*model_, error)) model_.reset();
    if (! model_) return;
    preparedBytes_ = model_->source.size() * sizeof(float);
    juce::dsp::FFT fft(9);
    std::array<std::complex<float>, tableSize> spectrum{}, output{};
    for (const auto& note : model_->notes)
    {
        std::vector<float> samples(note.frames.size() * tableSize);
        for (size_t f = 0; f < note.frames.size(); ++f)
        {
            spectrum.fill({}); const auto& frame = note.frames[f];
            for (int h = 1; h <= model::ResynthFrame::harmonics; ++h)
            {
                const std::complex<float> coefficient{frame.cosine[(size_t) h - 1] * 0.5f, -frame.sine[(size_t) h - 1] * 0.5f};
                spectrum[(size_t) h] = coefficient; spectrum[(size_t) tableSize - (size_t) h] = std::conj(coefficient);
            }
            fft.perform(spectrum.data(), output.data(), true);
            for (int i = 0; i < tableSize; ++i) samples[f * tableSize + (size_t) i] = output[(size_t) i].real() * tableSize;
        }
        auto table = WavetableData::fromSamples(samples, 1, tableSize, model_->sampleRate, model_->name);
        if (! table) { prepared_.clear(); model_.reset(); preparedBytes_ = 0; return; }
        preparedBytes_ += table.data->storageBytes() + note.frames.size() * sizeof(model::ResynthFrame);
        if (preparedBytes_ > 128 * 1024 * 1024) { prepared_.clear(); model_.reset(); preparedBytes_ = 0; return; }
        prepared_.push_back({std::move(table.data)});
    }
    reset();
}

void Resynth::prepare(double sampleRate, int)
{
    rate_ = std::isfinite(sampleRate) && sampleRate >= 8000 ? sampleRate : 48000;
    noiseAlpha_ = dsp::resynthNoiseAlpha(rate_); noiseNormalization_ = dsp::resynthNoiseNormalization(rate_); reset();
}
void Resynth::reset()
{
    voices_.fill({}); bends_.fill(0); pedals_.fill(false); exampleOverride_.fill(-1);
    for (auto& channel : held_) channel.fill(false);
    for (auto& channel : velocities_) channel.fill(0);
    age_ = 0; audition_.store(-2); activeExample_.store(-1); playhead_.store(0); lfo_ = dsp::Lfo{};
}
void Resynth::audition(int note) { audition_.store(std::clamp(note, -1, 127), std::memory_order_release); }
void Resynth::releaseVoice(Voice& voice)
{
    if (voice.releasing) return;
    voice.releasing = true; voice.held = false;
    voice.releaseStep = voice.envelope / std::max(1.0f, value(Release) * 0.001f * (float) rate_);
    voice.filterReleaseStep = voice.filterEnvelope / std::max(1.0f, value(FilterRelease) * 0.001f * (float) rate_);
}
float Resynth::harmonicGain(int harmonic) const
{
    if (harmonic < 1 || harmonic > (int) std::lround(value(PartialCount)) || harmonic > model::ResynthFrame::harmonics) return 0;
    const float level = harmonic == 1 ? value(Fundamental) : harmonic % 2 == 0 ? value(EvenLevel) : value(OddLevel);
    return value(Harmonic0 + harmonic - 1) * level * dsp::dbToGain(value(SpectralTilt) * (float) std::log2(harmonic));
}
void Resynth::event(const uint8_t* data, int size)
{
    if (size < 1 || data[0] < 0x80 || data[0] >= 0xf0) return;
    const int kind = data[0] & 0xf0, channel = data[0] & 15;
    if (size < 3 || data[1] > 127 || data[2] > 127) return;
    if (kind == 0x90 && data[2] > 0 && ready())
    {
        held_[(size_t) channel][data[1]] = true; velocities_[(size_t) channel][data[1]] = data[2] / 127.0f;
        double previousHz = hz(data[1]);
        const int playMode = std::clamp((int) std::lround(value(PlayMode)), 0, 2);
        Voice* legato = nullptr;
        for (auto& voice : voices_) if (voice.note >= 0 && ! voice.releasing)
        {
            previousHz = voice.frequency;
            if (playMode == 1 && voice.held && voice.channel == channel) legato = &voice;
            else if (playMode != 2) releaseVoice(voice);
        }
        int example = exampleOverride_[(size_t) channel];
        if (example < 0) example = std::clamp((int) std::lround(value(Example)) - 1, -1, (int) model_->notes.size() - 1);
        if (example < 0)
        {
            float distance = 1000;
            for (size_t i = 0; i < model_->notes.size(); ++i)
            {
                const float candidate = std::abs(model_->notes[i].root + value(RootOffset) - data[1]);
                if (candidate < distance) { distance = candidate; example = (int) i; }
            }
        }
        if (legato)
        {
            legato->note = data[1]; legato->velocity = data[2] / 127.0f; legato->held = true;
            activeExample_.store(legato->example, std::memory_order_relaxed); return;
        }
        auto voice = std::find_if(voices_.begin(), voices_.end(), [](const auto& v) { return v.note < 0; });
        if (voice == voices_.end()) voice = std::min_element(voices_.begin(), voices_.end(), [](const auto& a, const auto& b) { return a.age < b.age; });
        *voice = Voice{}; voice->note = data[1]; voice->channel = channel; voice->held = true; voice->example = example;
        voice->velocity = data[2] / 127.0f; voice->age = ++age_; voice->random = (uint32_t) (voice->age * 7919 + 1);
        voice->frequency = value(Glide) > 0 ? previousHz : hz(data[1]); activeExample_.store(example, std::memory_order_relaxed);
    }
    else if (kind == 0x80 || (kind == 0x90 && data[2] == 0))
    {
        held_[(size_t) channel][data[1]] = false;
        bool activeReleased = false;
        for (auto& voice : voices_) if (voice.note == data[1] && voice.channel == channel && ! voice.releasing)
        {
            voice.held = false;
            if (! pedals_[(size_t) channel])
            {
                if (value(PlayMode) > 0.5f && value(PlayMode) < 1.5f)
                    for (int key = 127; key >= 0; --key) if (held_[(size_t) channel][(size_t) key])
                    {
                        voice.note = key; voice.velocity = velocities_[(size_t) channel][(size_t) key];
                        voice.held = true; break;
                    }
                if (! voice.held) { releaseVoice(voice); activeReleased = true; }
            }
        }
        if (activeReleased && value(PlayMode) < 0.5f)
            for (int key = 127; key >= 0; --key) if (held_[(size_t) channel][(size_t) key])
            {
                const uint8_t fallback[] = {(uint8_t) (0x90 | channel), (uint8_t) key,
                    (uint8_t) std::clamp((int) std::lround(velocities_[(size_t) channel][(size_t) key] * 127), 1, 127)};
                event(fallback, 3); break;
            }
    }
    else if (kind == 0xe0) bends_[(size_t) channel] = (data[1] + 128 * data[2] - 8192) / 8192.0f;
    else if (kind == 0xb0)
    {
        if (data[1] == 119)
            exampleOverride_[(size_t) channel] = data[2] > 0 && model_ && (size_t) data[2] <= model_->notes.size() ? data[2] - 1 : -1;
        if (data[1] == 64 || data[1] == 121)
        {
            pedals_[(size_t) channel] = data[1] == 64 && data[2] >= 64;
            if (data[1] == 121) { bends_[(size_t) channel] = 0; exampleOverride_[(size_t) channel] = -1; }
            if (! pedals_[(size_t) channel]) for (auto& voice : voices_)
                if (voice.channel == channel && ! voice.held) releaseVoice(voice);
        }
        if (data[1] == 120 || data[1] == 123)
        {
            held_[(size_t) channel].fill(false);
            for (auto& voice : voices_) if (voice.channel == channel)
            { if (data[1] == 120) voice = Voice{}; else releaseVoice(voice); }
        }
    }
}

void Resynth::render(juce::AudioBuffer<float>& audio, int begin, int end)
{
    if (! ready() || begin >= end) return;
    std::array<float, NumParams> p{};
    for (int i = 0; i < NumParams; ++i) p[(size_t) i] = value(i);
    const int mode = model_->source.empty() ? 0 : std::clamp((int) std::lround(p[Mode]), 0, 2);
    const float gain = dsp::dbToGain(p[Volume]);
    const float attackStep = 1 / std::max(1.0f, p[Attack] * 0.001f * (float) rate_);
    const float decayStep = (1 - p[AmpSustain]) / std::max(1.0f, p[AmpDecay] * 0.001f * (float) rate_);
    const float filterAttack = 1 / std::max(1.0f, p[FilterAttack] * 0.001f * (float) rate_);
    const float filterDecay = (1 - p[FilterSustain]) / std::max(1.0f, p[FilterDecay] * 0.001f * (float) rate_);
    const double glideAlpha = p[Glide] <= 0 ? 1 : 1 - std::exp(-1 / (p[Glide] * 0.001 * rate_));
    const int destination = std::clamp((int) std::lround(p[LfoDestination]), 0, 4);
    const auto shape = (dsp::Lfo::Shape) std::clamp((int) std::lround(p[LfoShape]), 0, 5);
    const double lfoRate = p[LfoSync] > 0.5f ? p[LfoRate] * tempo_ / 60 : p[LfoRate];
    std::array<float, model::ResynthFrame::harmonics> harmonicGains{};
    bool editedSpectrum = false;
    for (int h = 0; h < model::ResynthFrame::harmonics; ++h)
    {
        harmonicGains[(size_t) h] = harmonicGain(h + 1);
        editedSpectrum |= std::abs(harmonicGains[(size_t) h] - 1) > 1e-6f;
    }
    const bool filterEnabled = p[FilterOn] > 0.5f && (p[Tone] < 19999 || p[FilterType] > 0.5f
        || p[Resonance] > 0 || std::abs(p[FilterEnv]) > 0.001f || (destination == 2 && p[LfoDepth] > 0));
    const float drive = dsp::dbToGain(p[Drive]);
    double displayPosition = 0;
    for (int sample = begin; sample < end; ++sample)
    {
        const float modulation = lfo_.next(lfoRate / rate_, shape) * p[LfoDepth];
        float left = 0, right = 0;
        for (auto& voice : voices_)
        {
            if (voice.note < 0) continue;
            if (voice.releasing)
            {
                voice.envelope -= voice.releaseStep;
                voice.filterEnvelope = std::max(0.0f, voice.filterEnvelope - voice.filterReleaseStep);
                if (voice.envelope <= 0) { voice = Voice{}; continue; }
            }
            else
            {
                if (voice.ampStage == 1)
                { voice.envelope = std::min(1.0f, voice.envelope + attackStep); if (voice.envelope >= 1) voice.ampStage = 2; }
                else if (voice.ampStage == 2)
                { voice.envelope = std::max(p[AmpSustain], voice.envelope - decayStep); if (voice.envelope <= p[AmpSustain]) voice.ampStage = 3; }
                else voice.envelope = p[AmpSustain];
                if (voice.filterStage == 1)
                { voice.filterEnvelope = std::min(1.0f, voice.filterEnvelope + filterAttack); if (voice.filterEnvelope >= 1) voice.filterStage = 2; }
                else if (voice.filterStage == 2)
                { voice.filterEnvelope = std::max(p[FilterSustain], voice.filterEnvelope - filterDecay); if (voice.filterEnvelope <= p[FilterSustain]) voice.filterStage = 3; }
                else voice.filterEnvelope = p[FilterSustain];
            }
            const auto& note = model_->notes[(size_t) voice.example];
            const double length = note.end - note.start;
            const double start = std::min((double) p[TimbreStart], (double) p[Sustain]);
            const double extent = std::max(0.0001, (double) p[Sustain] - start);
            double position = start + voice.time * p[Movement] / length;
            if (p[ScanMode] > 1.5f) position = start;
            else if (p[ScanMode] > 0.5f) position = start + std::fmod(std::max(0.0, position - start), extent);
            else position = std::min(position, (double) p[Sustain]);
            if (destination == 1) position += modulation * 0.5;
            position = std::clamp(position, 0.0, 1.0); displayPosition = position;
            const double framePosition = position * (double) (note.frames.size() - 1);
            const size_t frame = (size_t) framePosition, next = std::min(frame + 1, note.frames.size() - 1);
            const float mix = (float) (framePosition - (double) frame);
            const auto& first = note.frames[frame]; const auto& second = note.frames[next];
            const double motion = p[PitchMotion] > 0.5f ? std::lerp(first.pitch, second.pitch, mix) - std::round(note.root) : 0;
            const double pitchMod = destination == 0 ? modulation * 12 : 0;
            const double target = hz((double) voice.note - p[RootOffset] + p[BendRange] * bends_[(size_t) voice.channel] + motion + pitchMod);
            voice.frequency += glideAlpha * (target - voice.frequency);
            const double increment = std::min(0.49, voice.frequency / rate_);
            float synth = 0;
            if (! editedSpectrum)
            {
                const auto* table = prepared_[(size_t) voice.example].table.get();
                synth = table->sample((float) position, voice.phase, table->mipForIncrement(increment));
            }
            else
            {
                // Direct additive path exposes all captured partials. One fundamental sin/cos pair and
                // a harmonic recurrence; no 96-sine calls, allocation or table rebuilding on the callback.
                const double angle = 2 * std::numbers::pi * voice.phase;
                const double c = std::cos(angle), s = std::sin(angle); double hc = c, hs = s;
                const int count = std::min(model::ResynthFrame::harmonics, (int) (0.5 / std::max(1e-9, increment)));
                double sum = 0;
                for (int h = 0; h < count; ++h)
                {
                    const double weight = harmonicGains[(size_t) h] * std::clamp((0.5 - (h + 1) * increment) / 0.02, 0.0, 1.0);
                    sum += weight * (std::lerp(first.cosine[(size_t) h], second.cosine[(size_t) h], mix) * hc
                                   + std::lerp(first.sine[(size_t) h], second.sine[(size_t) h], mix) * hs);
                    const double nextCosine = hc * c - hs * s; hs = hs * c + hc * s; hc = nextCosine;
                }
                synth = (float) sum;
            }
            voice.phase += increment; voice.phase -= std::floor(voice.phase);
            if (p[SubLevel] > 0)
                synth += (float) std::sin(2 * std::numbers::pi * voice.subPhase) * p[SubLevel] * 0.5f;
            voice.subPhase += increment * 0.5; voice.subPhase -= std::floor(voice.subPhase);
            // Independent excitation avoids coherent crossover-band summing: the captured band
            // values describe RMS energy, so their powers must add rather than their amplitudes.
            if (p[Noise] > 0) for (size_t i = 0; i < first.noise.size(); ++i)
            {
                voice.random ^= voice.random << 13; voice.random ^= voice.random >> 17; voice.random ^= voice.random << 5;
                const float white = (float) (voice.random >> 8) / 8388608.0f - 1;
                const auto bands = dsp::resynthNoiseBands(white, voice.noiseFilter[i], noiseAlpha_);
                synth += bands[i] * noiseNormalization_[i] * std::lerp(first.noise[i], second.noise[i], mix) * p[Noise] * p[(size_t) NoiseLow + i];
            }
            float output = synth;
            if (mode > 0)
            {
                const double sourceFrame = note.start * model_->sampleRate + voice.sourcePosition;
                const size_t a = std::min((size_t) sourceFrame, model_->source.size() - 1), b = std::min(a + 1, model_->source.size() - 1);
                const float original = sourceFrame < note.end * model_->sampleRate
                    ? std::lerp(model_->source[a], model_->source[b], (float) (sourceFrame - (double) a)) : 0;
                const float hybrid = p[HybridAmount] * (float) std::clamp(1 - voice.time / 0.06, 0.0, 1.0);
                output = mode == 2 ? original : std::lerp(synth, original, hybrid);
                voice.sourcePosition += voice.frequency / hz(note.root) * model_->sampleRate / rate_;
                if (mode == 2 && sourceFrame >= note.end * model_->sampleRate) releaseVoice(voice);
            }
            if (mode != 2)
            {
                if (p[Drive] > 0.001f) output = std::tanh(output * drive) / std::sqrt(drive);
                if (filterEnabled)
                {
                    if (voice.modulationClock == 0)
                    {
                        const float cutoff = p[Tone] * std::exp2((p[FilterEnv] * voice.filterEnvelope + (destination == 2 ? modulation * 48 : 0)) / 12);
                        voice.filter.setup(rate_, cutoff, 0.707f + 9 * p[Resonance]);
                    }
                    output = voice.filter.process(output, (dsp::Svf::Mode) std::clamp((int) std::lround(p[FilterType]), 0, 3));
                }
            }
            voice.modulationClock = (voice.modulationClock + 1) % 16;
            float amplitude = voice.envelope * std::lerp(1.0f, voice.velocity, p[Velocity]) * gain;
            if (destination == 3) amplitude *= std::max(0.0f, 1 + modulation);
            const float pan = destination == 4 ? modulation : 0;
            left += output * amplitude * std::min(1.0f, 1 - pan);
            right += output * amplitude * std::min(1.0f, 1 + pan);
            voice.time += 1 / rate_;
        }
        if (audio.getNumChannels() > 0) audio.addSample(0, sample, audio.getNumChannels() == 1 ? (left + right) * 0.5f : left);
        if (audio.getNumChannels() > 1) audio.addSample(1, sample, right);
    }
    playhead_.store((float) displayPosition, std::memory_order_relaxed);
}

void Resynth::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) { reset(); return; }
    const int audition = audition_.exchange(-2, std::memory_order_acq_rel);
    if (audition >= -1)
    {
        for (auto& voice : voices_) if (voice.channel == 15) releaseVoice(voice);
        if (audition >= 0) { const uint8_t message[] = {0x9f, (uint8_t) audition, 127}; event(message, 3); }
    }
    int position = 0;
    for (const auto metadata : midi)
    {
        const int offset = std::clamp(metadata.samplePosition, position, audio.getNumSamples());
        render(audio, position, offset); event(metadata.data, metadata.numBytes); position = offset;
    }
    render(audio, position, audio.getNumSamples());
}
} // namespace sonora::devices
