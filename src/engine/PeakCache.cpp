#include "engine/PeakCache.h"

#include "engine/SampleCache.h"
#include "io/als/AlsReader.h"

#include <algorithm>
#include <cmath>

namespace sonora::engine
{

bool Peaks::range(double t0, double t1, float& lo, float& hi, int channel) const
{
    if (levels.empty() || levels[0].mins[0].empty() || sampleRate <= 0 || ! std::isfinite(t0) || ! std::isfinite(t1))
        return false;
    if (t0 > t1) std::swap(t0, t1);
    const double f0 = t0 * sampleRate, f1 = t1 * sampleRate;
    if (f1 < 0 || f0 >= (double) numFrames) return false;
    // At close zoom, query the actual samples rather than extending every peak
    // over a 64-frame bucket. Large spans retain the logarithmic pyramid path.
    if (source && f1 - f0 < kBucket * 4)
    {
        const auto& audio = source->audio;
        const int a = std::clamp((int) std::floor(std::max(0.0, f0)), 0, audio.getNumSamples() - 1);
        const int b = std::clamp((int) std::ceil(std::min(f1, (double) audio.getNumSamples() - 1)), a, audio.getNumSamples() - 1);
        lo = hi = channel < 0 ? 0.5f * (audio.getSample(0, a) + audio.getSample(audio.getNumChannels() > 1 ? 1 : 0, a))
                              : audio.getSample(std::min(channel, audio.getNumChannels() - 1), a);
        for (int i = a; i <= b; ++i)
        {
            const float value = channel < 0 ? 0.5f * (audio.getSample(0, i) + audio.getSample(audio.getNumChannels() > 1 ? 1 : 0, i))
                                            : audio.getSample(std::min(channel, audio.getNumChannels() - 1), i);
            lo = std::min(lo, value); hi = std::max(hi, value);
        }
        return true;
    }
    const size_t last = levels[0].mins[0].size() - 1;
    size_t begin = std::min(last, (size_t) (std::max(0.0, f0) / kBucket));
    const size_t end = std::min(last, (size_t) (std::max(0.0, std::min(f1, (double) numFrames - 1)) / kBucket)) + 1;
    const size_t ch = channel < 0 ? 0 : (size_t) std::clamp(channel + 1, 1, 2);
    lo = hi = 0;
    // Decompose into aligned power-of-two buckets. At most 2*log2(N) reads, with no extra
    // samples pulled from outside the queried base buckets (important at waveform transients).
    while (begin < end)
    {
        size_t level = 0, span = 1;
        while (level + 1 < levels.size() && begin % (span * 2) == 0 && begin + span * 2 <= end)
        { ++level; span *= 2; }
        lo = std::min(lo, levels[level].mins[ch][begin / span]);
        hi = std::max(hi, levels[level].maxs[ch][begin / span]);
        begin += span;
    }
    return true;
}

std::shared_ptr<const Peaks> computePeaks(const juce::AudioBuffer<float>& audio, double sampleRate,
                                          std::shared_ptr<const SampleData> source)
{
    auto p = std::make_shared<Peaks>();
    p->source = std::move(source);
    p->sampleRate = sampleRate;
    p->numFrames = audio.getNumSamples();
    const int buckets = (audio.getNumSamples() + Peaks::kBucket - 1) / Peaks::kBucket;
    if (audio.getNumChannels() == 0 || buckets == 0) return p;
    Peaks::Level base;
    for (size_t ch = 0; ch < 3; ++ch)
    {
        base.mins[ch].assign((size_t) buckets, 0);
        base.maxs[ch].assign((size_t) buckets, 0);
    }
    const float* l = audio.getReadPointer(0), *r = audio.getReadPointer(audio.getNumChannels() > 1 ? 1 : 0);
    for (int b = 0; b < buckets; ++b)
        for (int i = b * Peaks::kBucket; i < std::min(audio.getNumSamples(), (b + 1) * Peaks::kBucket); ++i)
        {
            const std::array<float, 3> values{0.5f * (l[i] + r[i]), l[i], r[i]};
            for (size_t ch = 0; ch < 3; ++ch)
            {
                base.mins[ch][(size_t) b] = std::min(base.mins[ch][(size_t) b], values[ch]);
                base.maxs[ch][(size_t) b] = std::max(base.maxs[ch][(size_t) b], values[ch]);
            }
        }
    p->levels.push_back(std::move(base));
    while (p->levels.back().mins[0].size() > 1)
    {
        const auto& previous = p->levels.back();
        const size_t count = (previous.mins[0].size() + 1) / 2;
        Peaks::Level level;
        for (size_t ch = 0; ch < 3; ++ch)
        {
            level.mins[ch].resize(count);
            level.maxs[ch].resize(count);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t a = i * 2, b = std::min(a + 1, previous.mins[ch].size() - 1);
                level.mins[ch][i] = std::min(previous.mins[ch][a], previous.mins[ch][b]);
                level.maxs[ch][i] = std::max(previous.maxs[ch][a], previous.maxs[ch][b]);
            }
        }
        p->levels.push_back(std::move(level));
    }
    return p;
}

std::shared_ptr<const Peaks> PeakCache::request(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = entries_[path];
        if (e.peaks != nullptr)
            return e.peaks;
        if (e.pending || e.failed)
            return nullptr;
        e.pending = true;
    }
    if (pool_ == nullptr)
    {
        compute(path);
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_[path].peaks;
    }
    pool_->addJob([this, path, gen = generation_, expected = generation_->load()] {
        if (gen->load() == expected)
            compute(path);
    });
    return nullptr;
}

std::shared_ptr<const Peaks> PeakCache::request(const model::Song& song, const model::Clip& clip)
{
    const std::string key = clip.sample.relativePath + "|" + clip.sample.absolutePath;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = resolved_.find(key);
        if (it != resolved_.end())
            path = it->second;
    }
    if (path.empty())
    {
        path = io::resolveSamplePath(song, clip.sample);
        std::lock_guard<std::mutex> lock(mutex_);
        resolved_[key] = path.empty() ? "?" : path; // "?" = not found, do not look again
    }
    if (path.empty() || path == "?")
        return nullptr;
    return request(path);
}

void PeakCache::compute(const std::string& path)
{
    std::shared_ptr<const Peaks> peaks;
    if (auto data = samples_.load(path))
        peaks = computePeaks(data->audio, data->sampleRate, data);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = entries_[path];
        e.pending = false;
        e.peaks = peaks;
        e.failed = peaks == nullptr;
    }
    if (peaks != nullptr && onReady)
        onReady();
}

void PeakCache::clear()
{
    ++*generation_;
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    resolved_.clear();
}

} // namespace sonora::engine
