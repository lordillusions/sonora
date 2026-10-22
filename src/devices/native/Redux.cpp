#include "devices/native/Redux.h"

#include <cmath>

namespace sonora::devices
{
Redux::Redux() : NativeDevice({
    makeParam("SampleRate", "Rate", 20, 40000, 40000, "Hz"),
    makeParam("Jitter", "Jitter", 0, 1, 0, "%"),
    makeParam("BitDepth", "Bits", 1, 16, 16),
    makeParam("QuantizerShape", "Shape", 0, 1, 0, "%"),
    makeParam("QuantizerDcShift", "DC Shift", 0, 1, 0, "", true),
    makeParam("EnablePreFilter", "Pre", 0, 1, 0, "", true),
    makeParam("EnablePostFilter", "Post", 0, 1, 0, "", true),
    makeParam("PostFilterValue", "Post Oct", -4, 4, 0, "oct"),
    makeParam("DryWet", "Dry/Wet", 0, 1, 1, "%")}) {}

const std::array<float, (2 * Redux::kHalfKernel) * Redux::kPhases + 1>& Redux::residualTable()
{
    // B(x) = integral of a Blackman-windowed sinc (cutoff 0.46 fs) over +-kHalfKernel; residual = B(x) - step(x).
    static const auto table = [] {
        std::array<float, (2 * kHalfKernel) * kPhases + 1> t{};
        constexpr int fine = 16, n = (2 * kHalfKernel) * kPhases * fine;
        std::vector<double> h((size_t) n + 1);
        double sum = 0;
        for (int i = 0; i <= n; ++i)
        {
            const double x = -kHalfKernel + 2.0 * kHalfKernel * i / n, c = 0.92;
            const double sinc = std::abs(x) < 1e-12 ? c : std::sin(M_PI * c * x) / (M_PI * x);
            const double w = 0.42 + 0.5 * std::cos(M_PI * x / kHalfKernel) + 0.08 * std::cos(2 * M_PI * x / kHalfKernel);
            h[(size_t) i] = sinc * w;
            sum += h[(size_t) i];
        }
        double acc = 0;
        for (int i = 0; i <= n; ++i)
        {
            acc += h[(size_t) i] / sum;
            if (i % fine == 0)
            {
                const int k = i / fine;
                const double x = -kHalfKernel + (double) k / kPhases;
                t[(size_t) k] = (float) (acc - (x >= 0 ? 1.0 : 0.0));
            }
        }
        t.back() = 0.0f;
        return t;
    }();
    return table;
}

float Redux::quantize(float x, float bits, float shape, bool dcShift)
{
    // mu-law companding (mu = 2^(8 Shape) - 1: Shape 0 linear, Shape 1 = mu 255), uniform steps of 1 / 2^(Bits - 1) in
    // the companded domain; DC Shift = mid-rise levels. Measured: Shape 1 / 0.5 level sets of the reference.
    const float levels = std::pow(2.0f, bits - 1), mu = std::exp2(8.0f * shape) - 1.0f;
    const float a = std::min(std::abs(x), 1.0f);
    const float c = mu > 1e-4f ? std::log1p(mu * a) / std::log1p(mu) : a;
    const float signedC = std::copysign(c, x);
    const float q = dcShift ? (std::ceil(signedC * levels) - 0.5f) / levels : std::round(signedC * levels) / levels; // mid-rise: 0 -> lower level
    const float qa = std::min(std::abs(q), 1.0f);
    const float back = mu > 1e-4f ? std::expm1(qa * std::log1p(mu)) / mu : qa;
    return std::copysign(back, q);
}

void Redux::prepare(double rate, int)
{
    sampleRate_ = std::max(1.0, rate); lastRate_ = -1; lastPost_ = -100; reset();
    (void) residualTable(); // ENG-14d: built here (allocates, ~2 ms) instead of in the first audio callback
}
void Redux::reset()
{
    channels_ = {}; channels_[0].random = 0x31415926u; channels_[1].random = 0x27182818u;
}

void Redux::setupFilters()
{
    const float rate = value(Rate), post = value(PostOctave);
    if (std::abs(rate - lastRate_) < 1e-6f && std::abs(post - lastPost_) < 1e-6f) return;
    lastRate_ = rate; lastPost_ = post;
    const double nyquistLimit = 0.49 * sampleRate_;
    const double preHz = std::min(0.5 * rate, nyquistLimit), postHz = std::min(0.5 * rate * std::exp2(post), nyquistLimit);
    for (int k = 0; k < kStages; ++k)
    {
        const double q = 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * M_PI / (4.0 * kStages))); // Butterworth order 2 kStages
        preCoeffs_[(size_t) k] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate_, preHz, q, 0.0));
        postCoeffs_[(size_t) k] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate_, postHz, q, 0.0));
    }
}

void Redux::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed()) return;
    setupFilters();
    const auto& table = residualTable();
    const double period = sampleRate_ / std::max(1.0f, value(Rate));
    const float bits = value(Bits), shape = value(Shape), wet = value(DryWet), jitter = value(Jitter);
    const bool dc = value(Dc) > 0.5f, pre = value(PreFilter) > 0.5f, post = value(PostFilter) > 0.5f;
    const float dcPole = (float) std::exp(-1.0 / (0.0318 * sampleRate_));
    // Grid lock: the first instant at or after history[1] (= block start - 2 samples) on the song-time grid.
    double gridStart = -1;
    if (jitter <= 0.0f && playing_.load(std::memory_order_relaxed))
    {
        const double songSample = beats_.load(std::memory_order_relaxed) * 60.0 / std::max(1.0, tempo_.load(std::memory_order_relaxed)) * sampleRate_;
        const double origin = kGridOffsetSeconds * sampleRate_, from = songSample - 2.0;
        gridStart = origin + std::ceil((from - origin) / period - 1e-9) * period - from;
    }
    // A step at fraction t after history[1] reaches the output (delayed by kLatency) kLatency - 1 + (1 - t) samples
    // from now; its residual covers +-kHalfKernel around that point.
    constexpr int firstJ = kLatency - 1 - kHalfKernel;
    for (int ch = 0; ch < std::min(2, audio.getNumChannels()); ++ch)
    {
        auto& s = channels_[(size_t) ch];
        auto* out = audio.getWritePointer(ch);
        if (gridStart >= 0.0 && std::abs(gridStart - s.nextSample) > 1e-3 && std::abs(gridStart + period - s.nextSample) > 1e-3)
            s.nextSample = gridStart;
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const float dry = out[i];
            if (dry == 0.0f && std::abs(s.dcY) < 1e-4f && std::abs(s.held - quantize(0.0f, bits, shape, dc)) < 1e-6f)
                ++s.silent;
            else if (dry != 0.0f)
            {
                if (s.silent >= 512) // wake: the held value jumps to the quantised silence level at once
                    s.held = quantize(0.0f, bits, shape, dc);
                s.silent = 0;
            }
            if (s.silent >= 512)
            {
                // Suspended (as the reference does with silent devices): output nothing, keep the grid phase.
                const double next = s.nextSample;
                const uint32_t random = s.random;
                s = Channel{};
                s.nextSample = next - 1.0; s.random = random;
                while (s.nextSample <= 0.0) s.nextSample += period;
                out[i] = 0.0f;
                continue;
            }
            float x = dry;
            if (pre)
                for (int k = 0; k < kStages; ++k) x = s.pre[(size_t) k].process(preCoeffs_[(size_t) k], x);
            s.history = {s.history[1], s.history[2], s.history[3], x};
            // Sampling instants inside [history[1], history[2]] (one sample behind the input): cubic interpolation.
            while (s.nextSample <= 1.0)
            {
                const auto t = (float) s.nextSample;
                const float p0 = s.history[0], p1 = s.history[1], p2 = s.history[2], p3 = s.history[3];
                const float c1 = 0.5f * (p2 - p0), c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3, c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
                const float value = quantize(((c3 * t + c2) * t + c1) * t + p1, bits, shape, dc);
                const float step = value - s.held;
                s.held = value;
                if (step != 0.0f)
                {
                    const double frac = 1.0 - t;
                    for (int j = 0; j <= 2 * kHalfKernel; ++j)
                    {
                        const double index = (j + frac) * kPhases;
                        const int k = (int) index;
                        if (k >= (int) table.size() - 1) break;
                        const auto f = (float) (index - k);
                        s.residual[(size_t) ((s.pos + firstJ + j) % kRing)] += step * (table[(size_t) k] + f * (table[(size_t) k + 1] - table[(size_t) k]));
                    }
                }
                s.random = s.random * 1664525u + 1013904223u;
                const double noise = (double) (s.random >> 8) / 8388608.0 - 1;
                s.nextSample += std::max(0.05, period * (1 + jitter * noise * 0.9));
            }
            s.nextSample -= 1.0;
            // heldDelay written d samples ago holds the held value at (now - d - 1): read d = kLatency - 1; dry: kLatency.
            s.heldDelay[(size_t) s.pos] = s.held;
            s.dryDelay[(size_t) s.pos] = dry;
            float y = s.heldDelay[(size_t) ((s.pos + kRing - (kLatency - 1)) % kRing)] + s.residual[(size_t) s.pos];
            s.residual[(size_t) s.pos] = 0.0f;
            if (post)
                for (int k = 0; k < kStages; ++k) y = s.post[(size_t) k].process(postCoeffs_[(size_t) k], y);
            const float blocked = y - s.dcX + dcPole * s.dcY; // DC blocker (the reference removes the DC shift's offset)
            s.dcX = y; s.dcY = blocked;
            const float delayedDry = s.dryDelay[(size_t) ((s.pos + kRing - kLatency) % kRing)];
            out[i] = delayedDry + wet * (blocked - delayedDry);
            s.pos = (s.pos + 1) % kRing;
        }
    }
}
}
