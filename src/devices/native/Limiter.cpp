#include "devices/native/Limiter.h"

namespace sonora::devices
{

namespace
{
std::vector<ParameterInfo> infos()
{
    using L = Limiter;
    std::vector<ParameterInfo> p(L::NumParams);
    p[L::Gain] = makeParam("Gain", "Gain", -24.0f, 24.0f, 0.0f, "dB");
    p[L::Ceiling] = makeParam("Ceiling", "Ceiling", -12.0f, 0.0f, -0.3f, "dB");
    p[L::Release] = makeParam("Release", "Release", 1.0f, 3000.0f, 300.0f, "ms");
    p[L::AutoRelease] = makeParam("AutoRelease", "Auto", 0.0f, 1.0f, 1.0f, "", true);
    p[L::LinkChannels] = makeParam("LinkChannels", "Link", 0.0f, 1.0f, 1.0f, "", true);
    p[L::Lookahead] = makeParam("Lookahead", "Lookahead", 0.0f, 2.0f, 1.0f, "", false, {"1.5 ms", "3 ms", "6 ms"});
    return p;
}
} // namespace

Limiter::Limiter() : NativeDevice(infos())
{
    for (auto& meter : outChannel_) meter.store(-120.0f, std::memory_order_relaxed);
}

void Limiter::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    gains_[0].assign((size_t) maxBlockSize, 1.0f);
    gains_[1].assign((size_t) maxBlockSize, 1.0f);
    const int idx = (int) std::lround(value(Lookahead));
    const double ms = idx == 0 ? 1.5 : idx == 1 ? 3.0 : 6.0;
    look_ = std::max(1, (int) std::lround(ms * 0.001 * sampleRate));
    delay_.prepare(look_);
    for (auto& c : ch_)
    {
        c.window.assign((size_t) look_ + 1, 1.0f);
        c.average.assign((size_t) look_, 1.0f);
        c.sum = look_;
        c.wpos = c.apos = 0;
        c.releaseGain = 1.0f;
    }
}

void Limiter::reset()
{
    delay_.reset();
    for (auto& c : ch_)
    {
        std::fill(c.window.begin(), c.window.end(), 1.0f);
        std::fill(c.average.begin(), c.average.end(), 1.0f);
        c.sum = look_;
        c.releaseGain = 1.0f;
    }
}

void Limiter::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    if (isBypassed())
        return;
    const int n = std::min(audio.getNumSamples(), (int) gains_[0].size());
    float* data[2] = {audio.getWritePointer(0), audio.getWritePointer(1)};

    const float inGain = dsp::dbToGain(value(Gain));
    const float ceiling = dsp::dbToGain(value(Ceiling));
    const bool link = value(LinkChannels) > 0.5f;
    const float relMs = value(AutoRelease) > 0.5f ? 150.0f : value(Release);
    const float relCoef = dsp::timeToCoef(relMs, sampleRate_);

    // Gain required per sample (before the look-ahead smoothing), computed from the undelayed input.
    float minGain = 1.0f;
    float channelGain[2] = {1.0f, 1.0f}, outPeak[2] = {0.0f, 0.0f};
    for (int i = 0; i < n; ++i)
    {
        data[0][i] *= inGain;
        data[1][i] *= inGain;
        float req[2];
        for (int c = 0; c < 2; ++c)
        {
            const float peak = std::fabs(data[c][i]);
            req[c] = peak > ceiling ? ceiling / peak : 1.0f;
        }
        if (link)
            req[0] = req[1] = std::min(req[0], req[1]);

        for (int c = 0; c < 2; ++c)
        {
            auto& ch = ch_[(size_t) c];
            ch.window[(size_t) ch.wpos] = req[c];
            ch.wpos = (ch.wpos + 1) % (int) ch.window.size();
            // minimum of the last look_+1 required gains (the peak that arrives look_ samples from now is in it)
            float m = 1.0f;
            for (float v : ch.window)
                m = std::min(m, v);
            // box-filter the minimum over look_ samples so the gain ramps down before the peak
            ch.sum += (double) m - (double) ch.average[(size_t) ch.apos];
            ch.average[(size_t) ch.apos] = m;
            ch.apos = (ch.apos + 1) % (int) ch.average.size();
            const float smoothed = (float) (ch.sum / (double) ch.average.size());
            // release: the gain may only rise as fast as the release time allows
            ch.releaseGain = smoothed < ch.releaseGain ? smoothed : ch.releaseGain + (smoothed - ch.releaseGain) * relCoef;
            gains_[c][(size_t) i] = ch.releaseGain;
            minGain = std::min(minGain, ch.releaseGain);
            channelGain[c] = std::min(channelGain[c], ch.releaseGain);
        }
    }

    delay_.processInPlace(audio, n);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c)
        {
            float v = data[c][i] * gains_[c][(size_t) i];
            data[c][i] = std::clamp(v, -ceiling, ceiling); // safety net for rounding
            outPeak[c] = std::max(outPeak[c], std::fabs(data[c][i]));
        }
    grMeter_.store(dsp::gainToDb(minGain), std::memory_order_relaxed);
    for (size_t c = 0; c < 2; ++c)
    {
        grChannel_[c].store(dsp::gainToDb(channelGain[c]), std::memory_order_relaxed);
        outChannel_[c].store(dsp::gainToDb(outPeak[c]), std::memory_order_relaxed);
    }
}

} // namespace sonora::devices
