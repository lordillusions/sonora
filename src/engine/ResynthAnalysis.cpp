#include "engine/ResynthAnalysis.h"
#include "dsp/ResynthNoise.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sonora::engine
{
namespace
{
constexpr double tau = 2 * std::numbers::pi, pitchRate = 8000;
constexpr int pitchHop = 80, pitchWindow = 768, maxLag = 320;
struct Point { double time = 0, hz = 0; float confidence = 0, rms = 0; };
float midiPitch(double hz) { return (float) (69 + 12 * std::log2(hz / 440)); }
double frequency(float pitch) { return 440 * std::exp2((pitch - 69.0) / 12); }

Point detect(const std::vector<float>& mono, int center)
{
    std::array<double, maxLag + 1> difference{};
    const auto sample = [&](int index) { return index >= 0 && index < (int) mono.size() ? mono[(size_t) index] : 0.0f; };
    Point point; point.time = center / pitchRate;
    double energy = 0;
    for (int i = -40; i < 40; ++i) { const float value = sample(center + i); energy += value * value; }
    point.rms = (float) std::sqrt(energy / 80);
    double cumulative = 0;
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double sum = 0;
        for (int i = 0; i < pitchWindow; ++i)
        {
            const double delta = sample(center - pitchWindow / 2 + i - lag / 2)
                               - sample(center - pitchWindow / 2 + i + (lag + 1) / 2);
            sum += delta * delta;
        }
        cumulative += sum;
        difference[(size_t) lag] = cumulative > 1e-15 ? sum * lag / cumulative : 1;
    }
    int best = 0;
    for (int lag = 8; lag < maxLag; ++lag)
        if (difference[(size_t) lag] < 0.18 && difference[(size_t) lag] <= difference[(size_t) lag - 1]
            && difference[(size_t) lag] < difference[(size_t) lag + 1]) { best = lag; break; }
    if (! best)
    {
        best = (int) (std::min_element(difference.begin() + 8, difference.end()) - difference.begin());
        if (best >= maxLag || difference[(size_t) best] > 0.35) return point;
    }
    const double a = difference[(size_t) best - 1], b = difference[(size_t) best], c = difference[(size_t) best + 1];
    const double denominator = a - 2 * b + c;
    const double refined = best + (std::abs(denominator) > 1e-12 ? std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5) : 0);
    point.hz = pitchRate / refined; point.confidence = (float) std::clamp(1 - b, 0.0, 1.0);
    return point;
}
}

ResynthAnalysisResult analyzeResynth(const juce::AudioBuffer<float>& audio, double rate, const std::string& name,
                                    const std::atomic<bool>& cancel, std::atomic<float>& progress)
{
    ResynthAnalysisResult result; progress.store(0);
    const auto cancelled = [&] { if (! cancel.load()) return false; result.cancelled = true; result.error = "Resynth analysis cancelled"; return true; };
    if (cancelled()) return result;
    if (! std::isfinite(rate) || rate < 8000 || rate > 96000 || audio.getNumChannels() < 1 || audio.getNumChannels() > 2
        || audio.getNumSamples() < (int) (rate * 0.08) || audio.getNumSamples() > rate * model::ResynthModel::maxSeconds)
    { result.error = "Resynth needs 0.08–30 seconds of mono/stereo audio at 8–96 kHz"; return result; }
    auto model = std::make_shared<model::ResynthModel>(); model->sampleRate = rate;
    model->name = juce::String::fromUTF8(name.c_str()).substring(0, 256).toStdString();
    double bestEnergy = -1;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
    {
        double energy = 0;
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const float sample = audio.getSample(channel, i);
            if (! std::isfinite(sample) || std::abs(sample) > 16) { result.error = "Resynth source contains invalid or excessive sample values"; return result; }
            energy += sample * sample;
        }
        if (energy > bestEnergy) { bestEnergy = energy; model->analysisChannel = channel; }
    }
    if (bestEnergy < audio.getNumSamples() * 1e-10) { result.error = "No audible signal to analyze"; return result; }
    model->source.assign(audio.getReadPointer(model->analysisChannel), audio.getReadPointer(model->analysisChannel) + audio.getNumSamples());
    model->sourceFrames = audio.getNumSamples();
    double mean = 0;
    for (float sample : model->source) mean += sample;
    mean /= (double) model->source.size();
    for (auto& sample : model->source) sample -= (float) mean;
    // Four causal low-pass sections before pitch decimation; retain original-bandwidth source for spectral analysis.
    std::vector<float> lowpass(model->source.size());
    std::array<double, 4> filter{};
    const double alpha = 1 - std::exp(-tau * 2400 / rate);
    for (size_t i = 0; i < model->source.size(); ++i)
    {
        double value = model->source[i];
        for (auto& state : filter) { state += alpha * (value - state); value = state; }
        lowpass[i] = (float) value;
    }
    const int count = (int) std::ceil(audio.getNumSamples() * pitchRate / rate);
    std::vector<float> mono((size_t) count);
    for (int i = 0; i < count; ++i)
    {
        const double at = i * rate / pitchRate; const int a = std::min((int) at, audio.getNumSamples() - 1);
        mono[(size_t) i] = std::lerp(lowpass[(size_t) a], lowpass[(size_t) std::min(a + 1, audio.getNumSamples() - 1)], (float) (at - a));
    }
    std::vector<Point> points;
    for (int center = 0; center < count; center += pitchHop)
    {
        if (cancelled()) return result;
        points.push_back(detect(mono, center)); progress.store(0.35f * (float) center / (float) count);
    }
    float maximum = 0;
    for (const auto& point : points) maximum = std::max(maximum, point.rms);
    const float gate = std::max(0.00005f, maximum * 0.025f);
    const auto voiced = [&](size_t i) { return points[i].hz >= 25 && points[i].hz <= 1000 && points[i].confidence >= 0.65f && points[i].rms > gate; };
    // Bridge brief unvoiced transitions; do not invent a pitch for long silence/noise.
    for (size_t i = 1; i + 2 < points.size(); ++i)
        if (! voiced(i) && voiced(i - 1) && voiced(i + 1) && points[i].rms > gate)
        { points[i].hz = std::sqrt(points[i - 1].hz * points[i + 1].hz); points[i].confidence = 0.66f; }
    size_t begin = points.size(), lastVoiced = 0;
    const auto close = [&](size_t end) {
        if (begin >= points.size() || end <= begin + 5) return;
        model::ResynthNote note;
        note.start = std::max(model->notes.empty() ? 0.0f : model->notes.back().end,
                              (float) std::max(0.0, points[begin].time - 0.005));
        note.end = (float) std::min(audio.getNumSamples() / rate, points[std::min(end, points.size() - 1)].time);
        std::vector<float> pitches;
        double confidence = 0;
        for (size_t i = begin; i < end && i < points.size(); ++i) if (voiced(i))
        { pitches.push_back(midiPitch(points[i].hz)); confidence += points[i].confidence; }
        if (pitches.empty() || note.end - note.start < 0.06f) return;
        std::sort(pitches.begin(), pitches.end()); note.root = pitches[pitches.size() / 2];
        note.confidence = (float) (confidence / (double) pitches.size()); note.sustain = (note.end - note.start) * 0.75f;
        model->notes.push_back(std::move(note));
    };
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (! voiced(i))
        {
            if (begin < points.size() && i > lastVoiced + 2) { close(lastVoiced + 1); begin = points.size(); }
            continue;
        }
        if (begin >= points.size()) begin = i;
        else if (i > begin + 8 && i + 3 < points.size())
        {
            bool pitchStep = voiced(i - 3) && points[i].confidence > 0.9f && points[i - 3].confidence > 0.9f
                && std::abs(midiPitch(points[i].hz) - midiPitch(points[i - 3].hz)) > 0.8f
                && voiced(i + 2) && std::abs(midiPitch(points[i + 2].hz) - midiPitch(points[i].hz)) < 0.3f;
            for (size_t j = i; j < i + 6; ++j)
                if (j >= points.size() || ! voiced(j) || std::abs(midiPitch(points[j].hz) - midiPitch(points[i].hz)) > 0.3f) pitchStep = false;
            if (pitchStep) { close(i); begin = i; }
        }
        lastVoiced = i;
    }
    if (begin < points.size()) close(points.size() - 1);
    if (model->notes.empty()) { result.error = "No confident bass/melody notes found. Try a dry, isolated single-note stem"; return result; }
    if (model->notes.size() > model::ResynthModel::maxNotes)
    {
        // Suggest the cut between the last note that fits and the next one, so no kept note is truncated.
        const auto& last = model->notes[(size_t) model::ResynthModel::maxNotes - 1];
        result.fitSeconds = (last.end + model->notes[(size_t) model::ResynthModel::maxNotes].start) * 0.5;
        result.error = "Found " + std::to_string(model->notes.size()) + " notes; one Resynth instrument holds up to "
            + std::to_string(model::ResynthModel::maxNotes) + ". Select a shorter phrase";
        return result;
    }
    // A continuous analysis phase makes harmonic coefficients compatible across frames, including vibrato/slides.
    std::vector<double> phase(model->source.size());
    double cycles = 0;
    for (size_t i = 0; i < phase.size(); ++i)
    {
        const double point = (double) i / rate * pitchRate / pitchHop;
        const size_t a = std::min((size_t) point, points.size() - 1), b = std::min(a + 1, points.size() - 1);
        const double hzA = voiced(a) ? points[a].hz : voiced(b) ? points[b].hz : frequency(model->notes.front().root);
        const double hzB = voiced(b) ? points[b].hz : hzA;
        phase[i] = cycles; cycles += std::lerp(hzA, hzB, std::clamp(point - (double) a, 0.0, 1.0)) / rate;
    }
    const double duration = (double) model->source.size() / rate;
    const double frameStep = std::max(0.012, duration / 1200);
    const double noiseRate = rate / std::max(1, (int) (rate / 24000));
    const auto noiseAlpha = dsp::resynthNoiseAlpha(noiseRate);
    for (size_t n = 0; n < model->notes.size(); ++n)
    {
        auto& note = model->notes[n]; const double length = note.end - note.start;
        const int frames = std::clamp((int) std::ceil(length / frameStep) + 1, 3, 96);
        note.frames.resize((size_t) frames);
        const size_t first = std::min((size_t) (note.start * rate), phase.size() - 1);
        for (int f = 0; f < frames; ++f)
        {
            if (cancelled()) return result;
            auto& frame = note.frames[(size_t) f]; frame.time = (float) (length * f / (frames - 1));
            const double time = note.start + frame.time;
            const size_t point = std::min((size_t) std::lround(time * pitchRate / pitchHop), points.size() - 1);
            const double hz = voiced(point) ? points[point].hz : frequency(note.root);
            frame.pitch = midiPitch(hz); frame.confidence = points[point].confidence;
            const int center = (int) std::lround(time * rate);
            const int radius = std::clamp((int) std::lround(2.0 * rate / hz), (int) (0.012 * rate), (int) (0.08 * rate));
            std::array<double, model::ResynthFrame::harmonics> cosines{}, sines{};
            double weight = 0, validWeight = 0;
            std::array<double, 4> bandEnergy{};
            std::array<float, 3> bandFilter{};
            // Limit analysis rate to approximately 24 kHz for bounded high-rate work.
            const int stride = std::max(1, (int) (rate / 24000));
            const int harmonics = std::min(model::ResynthFrame::harmonics, (int) (rate / stride * 0.45 / hz));
            for (int j = -radius; j <= radius; j += stride)
            {
                const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * j / radius); weight += w;
                const int index = center + j;
                if (index < (int) first || index >= (int) (note.end * rate) || index >= audio.getNumSamples()) continue;
                validWeight += w;
                const double sample = model->source[(size_t) index];
                const auto bands = dsp::resynthNoiseBands((float) sample, bandFilter, noiseAlpha);
                for (size_t band = 0; band < bands.size(); ++band) bandEnergy[band] += w * bands[band] * bands[band];
                const double angle = tau * std::remainder(phase[(size_t) index] - phase[first], 1.0);
                const double c = std::cos(angle), s = std::sin(angle); double hc = c, hs = s;
                for (int h = 0; h < harmonics; ++h)
                {
                    cosines[(size_t) h] += w * sample * hc; sines[(size_t) h] += w * sample * hs;
                    const double next = hc * c - hs * s; hs = hs * c + hc * s; hc = next;
                }
            }
            for (int h = 0; h < harmonics; ++h)
            {
                frame.cosine[(size_t) h] = (float) (2 * cosines[(size_t) h] / std::max(1.0, weight));
                frame.sine[(size_t) h] = (float) (2 * sines[(size_t) h] / std::max(1.0, weight));
            }
            // Measure the actual residual. Subtracting unrelated band powers treated pitch/window
            // leakage and filter transients as fresh noise, especially on low notes and note edges.
            std::array<double, 4> residualEnergy{};
            bandFilter.fill(0);
            const double normalization = weight / std::max(1.0, validWeight);
            for (int j = -radius; j <= radius; j += stride)
            {
                const int index = center + j;
                if (index < (int) first || index >= (int) (note.end * rate) || index >= audio.getNumSamples()) continue;
                const double angle = tau * std::remainder(phase[(size_t) index] - phase[first], 1.0);
                const double c = std::cos(angle), s = std::sin(angle); double hc = c, hs = s, tonal = 0;
                for (int h = 0; h < harmonics; ++h)
                {
                    tonal += frame.cosine[(size_t) h] * hc + frame.sine[(size_t) h] * hs;
                    const double next = hc * c - hs * s; hs = hs * c + hc * s; hc = next;
                }
                const auto bands = dsp::resynthNoiseBands((float) (model->source[(size_t) index] - tonal * normalization), bandFilter, noiseAlpha);
                const double w = 0.5 + 0.5 * std::cos(std::numbers::pi * j / radius);
                for (size_t band = 0; band < bands.size(); ++band) residualEnergy[band] += w * bands[band] * bands[band];
            }
            for (size_t band = 0; band < frame.noise.size(); ++band)
                frame.noise[band] = (float) std::sqrt(std::max(0.0, std::min(bandEnergy[band], residualEnergy[band]) / std::max(1.0, weight)));
        }
        progress.store(0.35f + 0.65f * (float) (n + 1) / (float) model->notes.size());
    }
    if (! model::validateResynth(*model, result.error)) return result;
    result.model = std::move(model); progress.store(1); return result;
}

std::vector<model::MidiNote> resynthPhrase(const model::ResynthModel& model, double beatsPerSecond)
{
    std::vector<model::MidiNote> notes;
    if (! std::isfinite(beatsPerSecond) || beatsPerSecond <= 0) return notes;
    for (const auto& region : model.notes)
    {
        model::MidiNote note;
        note.pitch = std::clamp((int) std::lround(region.root), 0, 127); note.start = region.start * beatsPerSecond;
        note.duration = (region.end - region.start) * beatsPerSecond; note.velocity = 127;
        notes.push_back(note);
    }
    return notes;
}
} // namespace sonora::engine
