#include "engine/SampleCache.h"

#include "engine/stretch/Transients.h"

#include <algorithm>

namespace sonora::engine
{

SampleCache::SampleCache()
{
    formats_.registerBasicFormats(); // see engine/AudioFileFormats.h for the extensions this reads
}

std::shared_ptr<const SampleData> SampleCache::load(const std::string& path)
{
    {
        std::lock_guard lock(mutex_);
        if (auto it = cache_.find(path); it != cache_.end())
            if (auto alive = it->second.lock())
                return alive;
    }

    // Decode outside the lock so several files can load in parallel.
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(juce::File(path)));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto data = std::make_shared<SampleData>();
    data->sampleRate = reader->sampleRate;
    data->fileChannels = (int) reader->numChannels;
    data->fileBits = (int) reader->bitsPerSample;
    data->fileFloat = reader->usesFloatingPointData;
    const int len = (int) juce::jmin<juce::int64>(reader->lengthInSamples, std::numeric_limits<int>::max());
    data->audio.setSize(2, len);
    reader->read(&data->audio, 0, len, 0, true, true);
    if (reader->numChannels == 1)
        data->audio.copyFrom(1, 0, data->audio, 0, 0, len);
    data->onsets = detectTransients(data->audio, data->sampleRate);

    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(path); it != cache_.end())
        if (auto alive = it->second.lock())
            return alive; // another thread finished first
    cache_[path] = data;
    return data;
}

std::shared_ptr<SampleSlot> SampleCache::request(const std::string& path)
{
    auto slot = std::make_shared<SampleSlot>();

    auto finish = [this, path](const std::shared_ptr<SampleSlot>& s) {
        if (auto data = load(path))
        {
            s->publish(std::move(data));
        }
        else
        {
            s->markFailed();
            std::lock_guard lock(mutex_);
            if (std::find(failed_.begin(), failed_.end(), path) == failed_.end())
                failed_.push_back(path);
        }
    };

    if (pool_ == nullptr)
    {
        finish(slot);
        return slot;
    }

    {
        std::lock_guard lock(mutex_);
        if (auto it = cache_.find(path); it != cache_.end())
            if (auto alive = it->second.lock())
            {
                slot->publish(std::move(alive));
                return slot;
            }
    }

    ++pending_;
    const int gen = generation_.load();
    pool_->addJob([this, slot, finish, gen] {
        if (gen == generation_.load())
            finish(slot);
        --pending_;
    });
    return slot;
}

std::vector<std::string> SampleCache::failedFiles() const
{
    std::lock_guard lock(mutex_);
    return failed_;
}

bool SampleCache::waitForLoads(int timeoutMs) const
{
    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (pending_.load() > 0)
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;
        juce::Thread::sleep(5);
    }
    return true;
}

void SampleCache::clearUnused()
{
    std::lock_guard lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end();)
        it = it->second.expired() ? cache_.erase(it) : std::next(it);
}

} // namespace sonora::engine
