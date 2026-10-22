#include "engine/AudioToMidi.h"
#include "engine/stretch/Transients.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sonora::engine
{
namespace
{
constexpr double analysisRate = 11025;
constexpr int window = 2048, hop = 256;
int pitchAt(const std::vector<float>& samples, int begin)
{
    std::array<double, 370> difference{};
    double cumulative = 0;
    int best = -1; double bestScore = 1;
    for (int lag = 1; lag < (int) difference.size(); ++lag)
    {
        double sum = 0;
        for (int i = 0; i < window / 2; ++i)
        {
            const double d = samples[(size_t) (begin + i)] - samples[(size_t) (begin + i + lag)]; sum += d * d;
        }
        cumulative += sum;
        difference[(size_t) lag] = cumulative > 1e-15 ? sum * lag / cumulative : 1;
        if (lag > 8 && difference[(size_t) lag] < bestScore) { bestScore = difference[(size_t) lag]; best = lag; }
        if (lag > 10 && difference[(size_t) lag - 1] < 0.15 && difference[(size_t) lag] > difference[(size_t) lag - 1])
        { best = lag - 1; bestScore = difference[(size_t) best]; break; }
    }
    if (best < 1 || bestScore > 0.3) return -1;
    const double a = difference[(size_t) std::max(1, best - 1)], b = difference[(size_t) best], c = difference[(size_t) std::min(best + 1, 369)];
    const double denominator = a - 2 * b + c;
    const double refined = best + (std::abs(denominator) > 1e-12 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0);
    const int pitch = (int) std::round(69 + 12 * std::log2(analysisRate / refined / 440));
    return pitch >= 24 && pitch <= 108 ? pitch : -1;
}
}

AudioToMidiResult audioToMidi(const juce::AudioBuffer<float>& audio, double rate, double tempo, AudioToMidiMode mode,
                             const std::atomic<bool>& cancel, std::atomic<float>& progress)
{
    AudioToMidiResult result;
    if (rate <= 0 || tempo <= 0 || audio.getNumChannels() < 1 || audio.getNumSamples() == 0) return result;
    const double seconds = audio.getNumSamples() / rate, beatsPerSecond = tempo / 60;
    auto mono = [&](int frame) {
        frame = std::clamp(frame, 0, audio.getNumSamples() - 1);
        return audio.getNumChannels() > 1 ? 0.5f * (audio.getSample(0, frame) + audio.getSample(1, frame)) : audio.getSample(0, frame);
    };
    if (mode == AudioToMidiMode::Drums || mode == AudioToMidiMode::Slices)
    {
        auto onsets = detectTransients(audio, rate);
        // Include quiet low-frequency attacks, which a high-pass transient detector can miss.
        double lastEnergy = 0, lastHit = -1;
        const int block = std::max(1, (int) (rate * 0.005));
        for (int pos = 0; pos < audio.getNumSamples(); pos += block)
        {
            if (cancel.load()) { result.cancelled = true; return result; }
            double energy = 0;
            for (int j = pos; j < std::min(pos + block, audio.getNumSamples()); ++j) energy += mono(j) * mono(j);
            energy /= block;
            if (energy > 0.0001 && energy > lastEnergy * 3 + 0.0001 && pos / rate - lastHit > 0.06)
            { onsets.push_back(pos / rate); lastHit = pos / rate; }
            lastEnergy = energy;
        }
        std::sort(onsets.begin(), onsets.end());
        for (double at : onsets)
        {
            if (! result.slices.empty() && at - result.slices.back() < 0.06) continue;
            const int first = (int) (at * rate), last = std::min(audio.getNumSamples(), first + (int) (0.07 * rate));
            double energy = 0, lowEnergy = 0, filtered = 0; int crossings = 0; float previous = 0;
            const double alpha = 1 - std::exp(-2 * std::numbers::pi * 350 / rate);
            for (int i = first; i < last; ++i)
            {
                const float value = mono(i); filtered += alpha * (value - filtered);
                energy += value * value; lowEnergy += filtered * filtered;
                if ((value >= 0) != (previous >= 0)) ++crossings;
                previous = value;
            }
            if (energy < 1e-6 || last <= first) continue;
            const int pitch = mode == AudioToMidiMode::Slices ? 36 + (int) result.slices.size()
                : lowEnergy / energy > 0.55 ? 36 : (double) crossings / (last - first) > 0.22 ? 42 : 38;
            if (pitch > 127) break;
            model::MidiNote note; note.pitch = pitch; note.start = at * beatsPerSecond;
            note.duration = std::min(0.125, (seconds - at) * beatsPerSecond);
            note.velocity = (float) std::clamp(45 + 100 * std::sqrt(energy / (last - first)), 30.0, 127.0);
            result.notes.push_back(note); result.slices.push_back(at);
        }
        progress.store(1); return result;
    }
    const int count = (int) std::ceil(seconds * analysisRate);
    std::vector<float> samples((size_t) count + window, 0);
    float peak = 0;
    for (int i = 0; i < count; ++i)
    {
        const double source = i * rate / analysisRate; const int frame = (int) source;
        samples[(size_t) i] = std::lerp(mono(frame), mono(frame + 1), (float) (source - frame));
        peak = std::max(peak, std::abs(samples[(size_t) i]));
    }
    if (peak < 1e-5f) return result;
    std::array<int, 128> active; active.fill(-1);
    juce::dsp::FFT fft(12);
    std::vector<float> spectrum(8192);
    auto close = [&](int pitch, double at) {
        const int index = active[(size_t) pitch]; if (index < 0) return;
        auto& note = result.notes[(size_t) index]; note.duration = std::max(0.0, at * beatsPerSecond - note.start); active[(size_t) pitch] = -1;
    };
    for (int pos = 0; pos < count; pos += hop)
    {
        if (cancel.load()) { result.cancelled = true; return result; }
        progress.store((float) pos / (float) std::max(1, count));
        double energy = 0;
        for (int i = 0; i < window / 2; ++i) energy += samples[(size_t) pos + (size_t) i] * samples[(size_t) pos + (size_t) i];
        std::array<bool, 128> found{};
        if (std::sqrt(energy / (window / 2)) > std::max(0.001, (double) peak * 0.035))
        {
            if (mode == AudioToMidiMode::Melody)
            {
                const int pitch = pitchAt(samples, pos); if (pitch >= 0) found[(size_t) pitch] = true;
            }
            else
            {
                std::fill(spectrum.begin(), spectrum.end(), 0);
                for (int i = 0; i < 4096; ++i)
                {
                    const int source = pos + i - 1024;
                    if (source >= 0 && source < count) spectrum[(size_t) i] = samples[(size_t) source] * (float) (0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / 4095));
                }
                fft.performFrequencyOnlyForwardTransform(spectrum.data());
                const float maximum = *std::max_element(spectrum.begin() + 10, spectrum.begin() + 1800);
                std::vector<std::pair<double, float>> accepted;
                for (int bin = 10; bin < 1800 && accepted.size() < 8; ++bin)
                {
                    const float magnitude = spectrum[(size_t) bin];
                    if (magnitude < maximum * 0.16f || magnitude <= spectrum[(size_t) bin - 1] || magnitude < spectrum[(size_t) bin + 1]) continue;
                    const double denominator = spectrum[(size_t) bin - 1] - 2 * magnitude + spectrum[(size_t) bin + 1];
                    const double delta = std::abs(denominator) > 1e-12 ? 0.5 * (spectrum[(size_t) bin - 1] - spectrum[(size_t) bin + 1]) / denominator : 0;
                    const double frequency = (bin + delta) * analysisRate / 4096;
                    bool harmonic = false;
                    for (const auto& [fundamental, strength] : accepted)
                    {
                        const double ratio = frequency / fundamental;
                        if (ratio > 1.8 && std::abs(ratio - std::round(ratio)) < 0.035 && magnitude < strength * 0.8f) harmonic = true;
                    }
                    const double midi = 69 + 12 * std::log2(frequency / 440);
                    const int pitch = (int) std::round(midi);
                    if (! harmonic && pitch >= 24 && pitch <= 108 && std::abs(midi - pitch) < 0.4)
                    { found[(size_t) pitch] = true; accepted.emplace_back(frequency, magnitude); }
                }
            }
        }
        // Centred spectral windows are compensated; no grid quantisation is imposed on detected notes.
        const double at = std::max(0.0, std::min(seconds, pos / analysisRate));
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            if (! found[(size_t) pitch]) close(pitch, at);
            else if (active[(size_t) pitch] < 0)
            {
                model::MidiNote note; note.pitch = pitch; note.start = at * beatsPerSecond;
                note.velocity = (float) std::clamp(55 + 100 * std::sqrt(energy / (window / 2)), 35.0, 127.0);
                active[(size_t) pitch] = (int) result.notes.size(); result.notes.push_back(note);
            }
        }
    }
    for (int pitch = 0; pitch < 128; ++pitch) close(pitch, seconds);
    result.notes.erase(std::remove_if(result.notes.begin(), result.notes.end(), [&](const auto& n) { return n.duration < 0.045 * beatsPerSecond; }), result.notes.end());
    progress.store(1); return result;
}
}
