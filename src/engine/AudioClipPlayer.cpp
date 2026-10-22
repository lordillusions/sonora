#include "engine/AudioClipPlayer.h"

#include "core/TimeUtils.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr int kMaxGrain = 8192;

const std::vector<double>& sliceList(const PreparedClip& clip, const SampleData& sample)
{
    return clip.transientResolution >= 0 && clip.transientResolution <= 5 && ! clip.gridSliceSeconds.empty()
               ? clip.gridSliceSeconds
               : sample.onsets;
}
} // namespace

void AudioPlayState::prepare(double sampleRate, int maxBlockSize)
{
    (void) sampleRate;
    (void) maxBlockSize;
    active_ = nullptr;

    // Largest grain ring: grain + a block of look-ahead, rounded to a power of two.
    int ringSize = 1;
    while (ringSize < kMaxGrain + 2)
        ringSize <<= 1;
    for (auto& r : grains_.ring)
        r.assign((size_t) ringSize, 0.0f);
    grains_.mask = ringSize - 1;
    grains_.window.assign((size_t) kMaxGrain, 0.0f);
}

double AudioPlayState::sourceSeconds(const PreparedClip& clip, double beat, double secPerBeat)
{
    const auto& loop = clip.loop;
    const double content = model::clipContentPosition(beat, clip.currentStart, loop.loopStart, loop.loopEnd,
                                                      loop.startRelative, loop.enabled);
    return warpBeatsToSeconds(clip.warpMarkers, content, secPerBeat);
}

float AudioPlayState::edgeGain(const PreparedClip& clip, double beat, double fadeBeats)
{
    // `fadeBeats` is the automatic edge fade; a clip's own fade replaces it when it is longer.
    const double inProgress = (beat - clip.currentStart) / std::max(fadeBeats, clip.fadeInBeats);
    const double outProgress = (clip.currentEnd - beat) / std::max(fadeBeats, clip.fadeOutBeats);
    const double inEdge = clip.fadeInBeats > fadeBeats ? model::clipFadeGain(inProgress, clip.fadeInCurve) : inProgress;
    const double outEdge = clip.fadeOutBeats > fadeBeats ? model::clipFadeGain(outProgress, clip.fadeOutCurve) : outProgress;
    return (float) std::clamp(std::min(inEdge, outEdge), 0.0, 1.0);
}

float AudioPlayState::readSample(const float* data, int len, double pos)
{
    const int i0 = (int) std::floor(pos);
    if (i0 < 0 || i0 + 1 >= len)
        return 0.0f;
    const float frac = (float) (pos - i0);
    return data[i0] + (data[i0 + 1] - data[i0]) * frac;
}

void AudioPlayState::render(const PreparedClip& clip, const BlockSegment& seg, double tempo,
                            juce::AudioBuffer<float>& out)
{
    const SampleData* sample = clip.sample != nullptr ? clip.sample->get() : nullptr;
    if (sample == nullptr || clip.currentEnd <= clip.currentStart)
        return;
    if (seg.endBeats() <= clip.currentStart || seg.startBeats >= clip.currentEnd)
        return;

    const bool stretched = clip.warped && clip.warpMode != model::WarpMode::RePitch;
    if (! stretched)
    {
        active_ = nullptr;
        renderAudioClip(clip, seg, tempo, out);
        return;
    }

    if (active_ != &clip || std::fabs(seg.startBeats - expectedBeat_) > 0.5 / seg.samplesPerBeat)
    {
        restart(clip);
    }

    if (clip.warpMode == model::WarpMode::Beats)
        renderBeats(clip, *sample, seg, tempo, out);
    else
        renderGrains(clip, *sample, seg, tempo, out);
    expectedBeat_ = seg.endBeats();
}

void AudioPlayState::restart(const PreparedClip& clip)
{
    active_ = &clip;
    for (auto& r : grains_.ring)
        std::fill(r.begin(), r.end(), 0.0f);
    current_ = {};
    previous_ = {};
    sliceIndex_ = -2;
    grains_.counter = 0;
    grains_.nextGrain = -1; // -1 == prefill on the first sample
    grains_.prevChosen = -1.0;
    grains_.rng = 12345u;
}

// ---------------------------------------------------------------------------------- Beats

void AudioPlayState::renderBeats(const PreparedClip& clip, const SampleData& sample, const BlockSegment& seg,
                                 double tempo, juce::AudioBuffer<float>& out)
{
    const double secPerBeat = 60.0 / tempo;
    const double outRate = seg.samplesPerBeat / secPerBeat;
    const double srcRate = sample.sampleRate;
    const double step = srcRate / outRate * clip.pitchRatio; // source samples per output sample
    const auto& slices = sliceList(clip, sample);
    const int len = sample.audio.getNumSamples();
    const float* srcL = sample.audio.getReadPointer(0);
    const float* srcR = sample.audio.getReadPointer(1);
    float* outL = out.getWritePointer(0);
    float* outR = out.getWritePointer(1);

    const float fade = 1.0f / std::max(16.0f, (float) (0.003 * outRate)); // 3 ms
    const double edgeBeats = std::max(16.0, 0.002 * outRate) / seg.samplesPerBeat;
    const double loopRegion = 0.025 * srcRate; // gap filling loops the last 25 ms of a slice
    const int loopMode = clip.transientLoopMode;

    auto startVoice = [&](Voice& v, double pos, double sliceStart, double sliceEnd) {
        v.active = true;
        v.pos = pos;
        v.sliceStart = sliceStart;
        v.sliceEnd = sliceEnd;
        v.loopFrom = std::max(sliceStart, sliceEnd - loopRegion);
        v.direction = 1;
        v.gain = 0.0f;
        v.fadingOut = false;
    };

    // Advances a voice by one output sample and returns its sample pair (already faded).
    auto tick = [&](Voice& v, float& l, float& r) {
        if (! v.active)
            return false;
        if (v.fadingOut)
            v.gain -= fade;
        else if (v.gain < 1.0f)
            v.gain = std::min(1.0f, v.gain + fade);
        if (v.gain <= 0.0f && v.fadingOut)
        {
            v.active = false;
            return false;
        }

        if (v.direction > 0 && v.pos >= v.sliceEnd)
        {
            if (loopMode == 0 || v.sliceEnd - v.loopFrom < 8.0)
            {
                v.fadingOut = true; // Loop Off: let the slice end, silence until the next one
                return false;
            }
            if (loopMode == 1)
                v.pos = v.loopFrom + std::fmod(v.pos - v.loopFrom, v.sliceEnd - v.loopFrom);
            else
            {
                v.direction = -1;
                v.pos = v.sliceEnd - 1.0;
            }
        }
        else if (v.direction < 0 && v.pos <= v.loopFrom)
        {
            v.direction = 1;
            v.pos = v.loopFrom;
        }

        l = readSample(srcL, len, v.pos) * v.gain;
        r = readSample(srcR, len, v.pos) * v.gain;
        v.pos += v.direction * step;
        return true;
    };

    for (int i = 0; i < seg.numSamples; ++i)
    {
        const double beat = seg.startBeats + i / seg.samplesPerBeat;
        if (beat < clip.currentStart || beat >= clip.currentEnd)
            continue;

        const double s = sourceSeconds(clip, beat, secPerBeat);
        // Slice containing s: last onset <= s.
        int idx = (int) (std::upper_bound(slices.begin(), slices.end(), s) - slices.begin()) - 1;
        idx = std::max(idx, 0);
        if (idx != sliceIndex_)
        {
            const double sliceStart = slices.empty() ? 0.0 : slices[(size_t) idx] * srcRate;
            const double sliceEnd = (size_t) idx + 1 < slices.size() ? slices[(size_t) idx + 1] * srcRate : (double) len;
            if (current_.active)
            {
                previous_ = current_;
                previous_.fadingOut = true;
            }
            startVoice(current_, s * srcRate, sliceStart, sliceEnd);
            sliceIndex_ = idx;
        }

        float l = 0.0f, r = 0.0f, pl = 0.0f, pr = 0.0f;
        tick(current_, l, r);
        if (previous_.active && tick(previous_, pl, pr))
        {
            l += pl;
            r += pr;
        }
        const float g = clip.gain * edgeGain(clip, beat, edgeBeats);
        outL[seg.sampleOffset + i] += l * g;
        outR[seg.sampleOffset + i] += r * g;
    }
}

// ---------------------------------------------------------------------------------- grains

void AudioPlayState::configureGrains(const PreparedClip& clip, double outRate)
{
    // Grain sizes in milliseconds per mode; larger grains keep tonal/complex material smoother.
    double ms = 40.0;
    switch (clip.warpMode)
    {
        case model::WarpMode::Tones: ms = 20.0 + clip.granularityTones * 0.4; break;   // 20..60 ms
        case model::WarpMode::Texture: ms = 40.0 + clip.granularityTexture * 1.0; break; // 40..140 ms
        case model::WarpMode::Complex: ms = 90.0; break;
        case model::WarpMode::ComplexPro: ms = 110.0; break;
        case model::WarpMode::Beats:
        case model::WarpMode::RePitch: break;
    }
    int n = 256;
    while (n < ms * 0.001 * outRate && n < kMaxGrain)
        n <<= 1;
    grains_.size = n;
    grains_.hop = n / 4;
    for (int i = 0; i < n; ++i) // periodic Hann: overlap-add at hop N/4 sums to exactly 2
        grains_.window[(size_t) i] = 0.5f - 0.5f * std::cos(2.0f * kPi * (float) i / (float) n);
}

void AudioPlayState::scheduleGrain(const SampleData& sample, long long startOut, double srcPos, double pitch,
                                   bool search, long long skipBefore, float jitter)
{
    const int len = sample.audio.getNumSamples();
    const float* srcL = sample.audio.getReadPointer(0);
    const float* srcR = sample.audio.getReadPointer(1);
    const int N = grains_.size;
    double chosen = srcPos;

    if (search && grains_.prevChosen >= 0.0)
    {
        // WSOLA: among start positions near the wanted one, pick the one whose beginning best
        // continues the previous grain (normalised cross-correlation on a mono mix).
        const double natural = grains_.prevChosen + (double) grains_.hop * pitch;
        const int radius = std::min(N / 8, 384);
        const int span = std::min(grains_.hop, 384);
        const int base = (int) std::floor(srcPos);
        const int nat = (int) std::floor(natural);
        if (nat >= 0 && nat + span < len)
        {
            double best = -1e30;
            int bestDelta = 0;
            for (int d = -radius; d <= radius; d += 2)
            {
                const int c0 = base + d;
                if (c0 < 0 || c0 + span >= len)
                    continue;
                double xy = 0.0, yy = 1e-9;
                for (int j = 0; j < span; j += 2)
                {
                    const double x = 0.5 * ((double) srcL[nat + j] + srcR[nat + j]);
                    const double y = 0.5 * ((double) srcL[c0 + j] + srcR[c0 + j]);
                    xy += x * y;
                    yy += y * y;
                }
                const double score = xy / std::sqrt(yy);
                if (score > best)
                {
                    best = score;
                    bestDelta = d;
                }
            }
            chosen = (double) (base + bestDelta);
        }
        chosen += jitter;
    }
    grains_.prevChosen = chosen;

    const int mask = grains_.mask;
    for (int j = 0; j < N; ++j)
    {
        const long long o = startOut + j;
        if (o < skipBefore)
            continue;
        const double p = chosen + j * pitch;
        const float w = grains_.window[(size_t) j];
        grains_.ring[0][(size_t) (o & mask)] += w * readSample(srcL, len, p);
        grains_.ring[1][(size_t) (o & mask)] += w * readSample(srcR, len, p);
    }
}

void AudioPlayState::renderGrains(const PreparedClip& clip, const SampleData& sample, const BlockSegment& seg,
                                  double tempo, juce::AudioBuffer<float>& out)
{
    const double secPerBeat = 60.0 / tempo;
    const double outRate = seg.samplesPerBeat / secPerBeat;
    const double srcRate = sample.sampleRate;
    const double pitch = clip.pitchRatio * srcRate / outRate; // source samples per grain sample
    float* outL = out.getWritePointer(0);
    float* outR = out.getWritePointer(1);
    const double edgeBeats = std::max(16.0, 0.002 * outRate) / seg.samplesPerBeat;
    const bool texture = clip.warpMode == model::WarpMode::Texture;

    if (grains_.nextGrain == -1)
        configureGrains(clip, outRate);
    const int hop = grains_.hop;
    const int mask = grains_.mask;

    for (int i = 0; i < seg.numSamples; ++i)
    {
        const double beat = seg.startBeats + i / seg.samplesPerBeat;
        if (beat < clip.currentStart || beat >= clip.currentEnd)
            continue;

        const double s = sourceSeconds(clip, beat, secPerBeat);
        if (grains_.nextGrain == -1)
        {
            // (Re)start: pre-fill the grains that would already be overlapping this sample so
            // the output does not fade in over a whole grain length.
            const double s2 = sourceSeconds(clip, beat + 1.0 / seg.samplesPerBeat, secPerBeat);
            const double perOut = (s2 - s) * srcRate; // source samples per output sample here
            const double slope = std::fabs(perOut) < 64.0 ? perOut : 0.0;
            for (int k = 3; k >= 1; --k)
                scheduleGrain(sample, grains_.counter - (long long) k * hop, s * srcRate - (double) k * hop * slope,
                              pitch, false, grains_.counter, 0.0f);
            grains_.nextGrain = grains_.counter;
        }

        if (grains_.counter >= grains_.nextGrain)
        {
            float jitter = 0.0f;
            if (texture)
            {
                grains_.rng = grains_.rng * 1664525u + 1013904223u;
                const float u = (float) (grains_.rng >> 8) / 16777216.0f - 0.5f;
                jitter = u * (float) grains_.size * 0.5f * (clip.fluctuationTexture / 100.0f);
            }
            scheduleGrain(sample, grains_.counter, s * srcRate, pitch, true, grains_.counter, jitter);
            grains_.nextGrain += hop;
        }

        const size_t slot = (size_t) (grains_.counter & mask);
        const float g = clip.gain * edgeGain(clip, beat, edgeBeats) * 0.5f; // Hann sums to 2 at 75 % overlap
        outL[seg.sampleOffset + i] += grains_.ring[0][slot] * g;
        outR[seg.sampleOffset + i] += grains_.ring[1][slot] * g;
        grains_.ring[0][slot] = 0.0f;
        grains_.ring[1][slot] = 0.0f;
        ++grains_.counter;
    }
}

} // namespace sonora::engine
