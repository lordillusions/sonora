#include "devices/native/SweepSwell.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sonora::devices::swell
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

struct Random
{
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed != 0 ? seed : 0x9e3779b9u) {}
    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float uniform() { return (float) (next() >> 8) / 16777216.0f; } // 0..1
};

int tailFrames(const juce::AudioBuffer<float>& dry, double rate, const Settings& s)
{
    return dry.getNumSamples() + (int) std::ceil(std::clamp(s.decaySeconds, 0.05, 60.0) * rate) + (int) (0.05 * rate);
}

const float* channel(const juce::AudioBuffer<float>& b, int ch) { return b.getReadPointer(std::min(ch, b.getNumChannels() - 1)); }

std::vector<float> monoOf(const juce::AudioBuffer<float>& dry)
{
    std::vector<float> mono((size_t) dry.getNumSamples(), 0.0f);
    const float scale = 1.0f / (float) std::max(1, dry.getNumChannels());
    for (int ch = 0; ch < dry.getNumChannels(); ++ch)
    {
        const float* in = dry.getReadPointer(ch);
        for (int s = 0; s < dry.getNumSamples(); ++s) mono[(size_t) s] += in[s] * scale;
    }
    return mono;
}

// The damping as a one-pole low-pass coefficient for a cutoff between 18 kHz (Damp 0) and 1.2 kHz (Damp 1).
float dampCoefficient(float damp, double rate)
{
    const double hz = 18000.0 * std::pow(1200.0 / 18000.0, std::clamp((double) damp, 0.0, 1.0));
    return (float) std::exp(-2.0 * kPi * std::min(hz, rate * 0.45) / rate);
}

void lowPass(juce::AudioBuffer<float>& b, float coefficient)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        float* d = b.getWritePointer(ch);
        float z = 0;
        for (int s = 0; s < b.getNumSamples(); ++s) d[s] = z = d[s] + (z - d[s]) * coefficient;
    }
}

// ---- Hall: 8 modulated delay lines, Householder feedback, a damping low-pass in each line, diffused stereo input.
juce::AudioBuffer<float> hall(const juce::AudioBuffer<float>& dry, double rate, double decay, float damp, int frames, uint32_t seed)
{
    constexpr int kLines = 8;
    static constexpr int kDelays[kLines] = {1031, 1327, 1523, 1801, 2053, 2357, 2671, 2999};
    static constexpr int kDiffusers[2][4] = {{142, 107, 379, 277}, {151, 113, 397, 263}};
    const double scale = rate / 44100.0;
    juce::AudioBuffer<float> out(2, frames);
    out.clear();
    // Input diffusion (four allpasses per side): the tail starts dense instead of as separate echoes.
    std::vector<float> diffused[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        diffused[ch].assign((size_t) frames, 0.0f);
        const float* in = channel(dry, ch);
        for (int s = 0; s < dry.getNumSamples(); ++s) diffused[ch][(size_t) s] = in[s];
        for (int a = 0; a < 4; ++a)
        {
            const int length = std::max(1, (int) (kDiffusers[ch][a] * scale));
            std::vector<float> line((size_t) length, 0.0f);
            int pos = 0;
            for (auto& x : diffused[ch])
            {
                const float buffered = line[(size_t) pos];
                const float v = x + 0.6f * buffered;
                line[(size_t) pos] = v;
                x = buffered - 0.6f * v;
                pos = (pos + 1) % length;
            }
        }
    }
    std::vector<std::vector<float>> lines(kLines);
    int length[kLines], write = 0;
    float gain[kLines], lows[kLines]{};
    double lfo[kLines], lfoStep[kLines];
    Random random(seed);
    const int size = (int) (3200 * scale) + 32;
    for (int i = 0; i < kLines; ++i)
    {
        length[i] = std::max(8, (int) (kDelays[i] * scale));
        lines[(size_t) i].assign((size_t) size, 0.0f);
        gain[i] = (float) std::pow(10.0, -3.0 * length[i] / (std::max(0.05, decay) * rate));
        lfo[i] = random.uniform() * 2.0 * kPi;
        lfoStep[i] = 2.0 * kPi * (0.13 + 0.07 * i) / rate;
    }
    const float lowCoefficient = 0.05f + 0.55f * std::clamp(damp, 0.0f, 1.0f);
    const float depth = (float) (5.0 * scale);
    float* left = out.getWritePointer(0);
    float* right = out.getWritePointer(1);
    float taps[kLines];
    for (int s = 0; s < frames; ++s)
    {
        float sum = 0;
        for (int i = 0; i < kLines; ++i)
        {
            const double delay = length[i] + depth * std::sin(lfo[i]);
            lfo[i] += lfoStep[i];
            double read = write - delay;
            while (read < 0) read += size;
            const int i0 = (int) read % size, i1 = (i0 + 1) % size;
            const float frac = (float) (read - std::floor(read));
            const auto& line = lines[(size_t) i];
            float y = line[(size_t) i0] + (line[(size_t) i1] - line[(size_t) i0]) * frac;
            lows[i] = y + (lows[i] - y) * lowCoefficient;
            taps[i] = lows[i] * gain[i];
            sum += taps[i];
        }
        const float house = sum * (2.0f / kLines);
        const float inL = diffused[0][(size_t) s] * 0.35f, inR = diffused[1][(size_t) s] * 0.35f;
        for (int i = 0; i < kLines; ++i) lines[(size_t) i][(size_t) write] = taps[i] - house + ((i & 1) == 0 ? inL : inR);
        left[s] = taps[0] - taps[2] + taps[4] - taps[6];
        right[s] = taps[1] - taps[3] + taps[5] - taps[7];
        write = (write + 1) % size;
    }
    return out;
}

// One octave up by two rotating read taps (crossfaded with sin^2 / cos^2) over a 60 ms window.
juce::AudioBuffer<float> octaveUp(const juce::AudioBuffer<float>& in, double rate)
{
    juce::AudioBuffer<float> out(in.getNumChannels(), in.getNumSamples());
    const double window = 0.06 * rate;
    for (int ch = 0; ch < in.getNumChannels(); ++ch)
    {
        const float* x = in.getReadPointer(ch);
        float* y = out.getWritePointer(ch);
        for (int n = 0; n < in.getNumSamples(); ++n)
        {
            float sum = 0;
            for (int k = 0; k < 2; ++k)
            {
                const double d = std::fmod(window - std::fmod((double) n, window) + k * window * 0.5, window);
                const double read = n - d; // advances two samples per sample: an octave up
                const float w = (float) std::pow(std::sin(kPi * d / window), 2.0);
                if (read < 0) continue;
                const int i0 = (int) read;
                const int i1 = std::min(i0 + 1, in.getNumSamples() - 1);
                const float frac = (float) (read - i0);
                sum += w * (x[i0] + (x[i1] - x[i0]) * frac);
            }
            y[n] = sum;
        }
    }
    return out;
}

juce::AudioBuffer<float> shimmer(const juce::AudioBuffer<float>& dry, double rate, const Settings& s, int frames)
{
    auto first = hall(dry, rate, s.decaySeconds, s.damp * 0.6f, frames, s.seed);
    auto up = octaveUp(first, rate);
    up.applyGain(0.8f);
    auto second = hall(up, rate, s.decaySeconds * 1.15, s.damp * 0.4f, frames, s.seed ^ 0x51u);
    for (int ch = 0; ch < 2; ++ch) second.addFrom(ch, 0, first, ch, 0, frames, 0.45f);
    return second;
}

// ---- Bloom: STFT magnitudes held per bin, decaying at the tail's rate (faster up high with Damp), random phases.
juce::AudioBuffer<float> bloom(const juce::AudioBuffer<float>& dry, double rate, const Settings& s, int frames)
{
    const int order = rate > 60000.0 ? 13 : 12;
    const int n = 1 << order, hop = n / 4, bins = n / 2 + 1;
    juce::dsp::FFT fft(order);
    std::vector<float> window((size_t) n);
    for (int i = 0; i < n; ++i) window[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * kPi * i / n));
    std::vector<float> decay((size_t) bins);
    const double t60 = std::max(0.05, s.decaySeconds);
    for (int k = 0; k < bins; ++k)
    {
        const double above = std::clamp((double) k / (bins - 1), 0.0, 1.0);
        const double t = t60 * std::max(0.08, 1.0 - std::clamp((double) s.damp, 0.0, 1.0) * 0.9 * std::sqrt(above));
        decay[(size_t) k] = (float) std::pow(10.0, -3.0 * hop / (t * rate));
    }
    juce::AudioBuffer<float> out(2, frames);
    out.clear();
    std::vector<float> buffer((size_t) n * 2), held((size_t) bins), smooth((size_t) bins);
    Random random(s.seed ^ 0xb100u);
    const int frameCount = frames / hop + 2;
    for (int ch = 0; ch < 2; ++ch)
    {
        std::fill(held.begin(), held.end(), 0.0f);
        const float* in = channel(dry, ch);
        float* o = out.getWritePointer(ch);
        for (int f = 0; f < frameCount; ++f)
        {
            const int start = f * hop - n / 2;
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            for (int i = 0; i < n; ++i)
            {
                const int at = start + i;
                if (at >= 0 && at < dry.getNumSamples()) buffer[(size_t) i] = in[at] * window[(size_t) i];
            }
            fft.performRealOnlyForwardTransform(buffer.data(), true);
            for (int k = 0; k < bins; ++k)
            {
                const float re = buffer[(size_t) (2 * k)], im = buffer[(size_t) (2 * k + 1)];
                held[(size_t) k] = std::max(std::sqrt(re * re + im * im), held[(size_t) k] * decay[(size_t) k]);
            }
            for (int k = 0; k < bins; ++k) // a little blur across bins
                smooth[(size_t) k] = 0.5f * held[(size_t) k] + 0.25f * (held[(size_t) std::max(0, k - 1)] + held[(size_t) std::min(bins - 1, k + 1)]);
            for (int k = 0; k < bins; ++k)
            {
                const float phase = (float) (2.0 * kPi) * random.uniform();
                buffer[(size_t) (2 * k)] = smooth[(size_t) k] * std::cos(phase);
                buffer[(size_t) (2 * k + 1)] = smooth[(size_t) k] * std::sin(phase);
            }
            buffer[1] = 0.0f; // DC has no phase
            fft.performRealOnlyInverseTransform(buffer.data());
            for (int i = 0; i < n; ++i)
            {
                const int at = start + i;
                if (at >= 0 && at < frames) o[at] += buffer[(size_t) i] * window[(size_t) i] / 1.5f;
            }
        }
    }
    return out;
}

// ---- Grain: a decaying cloud of 30-120 ms grains from the sound, mostly at pitch, some an octave up or down.
juce::AudioBuffer<float> grain(const juce::AudioBuffer<float>& dry, double rate, const Settings& s, int frames)
{
    const auto mono = monoOf(dry);
    juce::AudioBuffer<float> out(2, frames);
    out.clear();
    if (mono.empty()) return out;
    Random random(s.seed ^ 0x6a41u);
    const double t60 = std::max(0.05, s.decaySeconds);
    const int count = std::max(8, (int) (90.0 * frames / rate));
    const int source = (int) mono.size();
    float* left = out.getWritePointer(0);
    float* right = out.getWritePointer(1);
    for (int g = 0; g < count; ++g)
    {
        const int length = (int) ((0.03 + 0.09 * random.uniform()) * rate);
        const int onset = (int) (random.uniform() * (float) std::max(1, frames - length));
        const float which = random.uniform();
        double ratio = std::pow(2.0, (random.uniform() - 0.5) * 0.25 / 12.0); // +-12 cents
        if (which > 0.85f) ratio *= 2.0;
        else if (which > 0.72f) ratio *= 0.5;
        // Grains read from where the sound is at their time (while it plays), then from anywhere in it.
        const double span = std::max(1.0, source - length * ratio);
        const double from = onset < source ? std::min((double) onset, span) * (0.7 + 0.3 * random.uniform())
                                           : random.uniform() * span;
        const float amp = (float) std::pow(10.0, -3.0 * std::max(0, onset - source / 2) / (t60 * rate));
        const float pan = random.uniform() * (float) (kPi * 0.5);
        const float gl = amp * std::cos(pan), gr = amp * std::sin(pan);
        for (int i = 0; i < length && onset + i < frames; ++i)
        {
            const double read = from + i * ratio;
            const int i0 = (int) read;
            if (i0 + 1 >= source) break;
            const float frac = (float) (read - i0);
            const float v = mono[(size_t) i0] + (mono[(size_t) i0 + 1] - mono[(size_t) i0]) * frac;
            const float w = 0.5f - 0.5f * (float) std::cos(2.0 * kPi * i / length);
            left[onset + i] += v * w * gl;
            right[onset + i] += v * w * gr;
        }
    }
    lowPass(out, dampCoefficient(s.damp, rate));
    return out;
}

float rmsOf(const juce::AudioBuffer<float>& b)
{
    double sum = 0;
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        const float* d = b.getReadPointer(ch);
        for (int s = 0; s < b.getNumSamples(); ++s) sum += (double) d[s] * d[s];
    }
    const double count = (double) std::max(1, b.getNumChannels() * b.getNumSamples());
    return (float) std::sqrt(sum / count);
}
} // namespace

std::array<float, kCharacters> weights(float x, float y)
{
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    std::array<float, kCharacters> w{};
    w[Hall] = (1 - x) * (1 - y);
    w[Shimmer] = (1 - x) * y;
    w[Bloom] = x * y;
    w[Grain] = x * (1 - y);
    return w;
}

juce::AudioBuffer<float> renderCharacter(Character which, const juce::AudioBuffer<float>& dry, double rate, const Settings& s)
{
    const int frames = tailFrames(dry, rate, s);
    if (dry.getNumSamples() <= 0 || dry.getNumChannels() <= 0 || rate <= 0)
    {
        juce::AudioBuffer<float> silent(2, std::max(1, frames));
        silent.clear();
        return silent;
    }
    switch (which)
    {
        case Shimmer: return shimmer(dry, rate, s, frames);
        case Bloom: return bloom(dry, rate, s, frames);
        case Grain: return grain(dry, rate, s, frames);
        case Hall:
        case kCharacters:
        default: return hall(dry, rate, s.decaySeconds, s.damp, frames, s.seed);
    }
}

juce::AudioBuffer<float> renderTail(const juce::AudioBuffer<float>& dry, double rate, const Settings& s)
{
    const int frames = tailFrames(dry, rate, s);
    juce::AudioBuffer<float> out(2, std::max(1, frames));
    out.clear();
    if (dry.getNumSamples() <= 0 || dry.getNumChannels() <= 0 || rate <= 0) return out;
    const auto w = weights(s.x, s.y);
    for (int c = 0; c < kCharacters; ++c)
    {
        if (w[(size_t) c] < 0.005f) continue;
        auto part = renderCharacter((Character) c, dry, rate, s);
        const float rms = rmsOf(part);
        if (rms <= 1.0e-9f) continue;
        for (int ch = 0; ch < 2; ++ch) out.addFrom(ch, 0, part, ch, 0, std::min(frames, part.getNumSamples()), w[(size_t) c] / rms);
    }
    // A short fade at the very end (the start of the reversed swell) and the level of the dry peak.
    const int fade = std::min(frames, (int) (0.05 * rate));
    for (int ch = 0; ch < 2; ++ch) out.applyGainRamp(ch, frames - fade, fade, 1.0f, 0.0f);
    float dryPeak = 0;
    for (int ch = 0; ch < dry.getNumChannels(); ++ch) dryPeak = std::max(dryPeak, dry.getMagnitude(ch, 0, dry.getNumSamples()));
    const float peak = std::max(out.getMagnitude(0, 0, frames), out.getMagnitude(1, 0, frames));
    if (peak > 1.0e-9f) out.applyGain((dryPeak > 1.0e-6f ? dryPeak : 0.5f) / peak);
    return out;
}

double decayTimeOf(const juce::AudioBuffer<float>& audio, double rate, double dropDb)
{
    const int window = std::max(1, (int) (0.05 * rate));
    const int blocks = audio.getNumSamples() / window;
    if (blocks <= 0) return 0.0;
    std::vector<float> level((size_t) blocks, 0.0f);
    float peak = 0;
    for (int b = 0; b < blocks; ++b)
    {
        double sum = 0;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const float* d = audio.getReadPointer(ch, b * window);
            for (int i = 0; i < window; ++i) sum += (double) d[i] * d[i];
        }
        level[(size_t) b] = (float) std::sqrt(sum / (window * std::max(1, audio.getNumChannels())));
        peak = std::max(peak, level[(size_t) b]);
    }
    const float floor = peak * (float) std::pow(10.0, -dropDb / 20.0);
    for (int b = blocks - 1; b >= 0; --b)
        if (level[(size_t) b] > floor) return (double) (b + 1) * window / rate;
    return 0.0;
}
} // namespace sonora::devices::swell
