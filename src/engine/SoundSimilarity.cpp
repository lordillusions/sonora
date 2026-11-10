#include "engine/SoundSimilarity.h"

#include "engine/AudioFileFormats.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace sonora::engine
{

namespace
{
constexpr const char* kHeader = "SonoraSoundFeatures";

std::string key(const juce::File& file) { return file.getFullPathName().toStdString(); }
int64_t modified(const juce::File& file) { return file.getLastModificationTime().toMilliseconds(); }
} // namespace

void SoundFeatureCache::loadLocked()
{
    if (loaded_) return;
    loaded_ = true;
    if (! file_.existsAsFile()) return;
    juce::StringArray lines;
    file_.readLines(lines);
    if (lines.isEmpty() || lines[0] != juce::String(kHeader) + " " + juce::String(dsp::SoundFeatures::kVersion)) return;
    for (int i = 1; i < lines.size(); ++i)
    {
        // bytes \t modified \t valid \t v0 v1 ... \t path
        const auto fields = juce::StringArray::fromTokens(lines[i], "\t", "");
        if (fields.size() != 5) continue;
        Entry entry;
        entry.bytes = fields[0].getLargeIntValue();
        entry.modified = fields[1].getLargeIntValue();
        entry.features.valid = fields[2] == "1";
        const auto values = juce::StringArray::fromTokens(fields[3], " ", "");
        if (values.size() != dsp::SoundFeatures::Count) continue;
        for (int v = 0; v < values.size(); ++v) entry.features.values[(size_t) v] = values[v].getFloatValue();
        entries_[fields[4].toStdString()] = entry;
    }
}

std::optional<dsp::SoundFeatures> SoundFeatureCache::find(const juce::File& file)
{
    const std::lock_guard lock(mutex_);
    loadLocked();
    const auto found = entries_.find(key(file));
    if (found == entries_.end() || found->second.bytes != file.getSize() || found->second.modified != modified(file)) return std::nullopt;
    return found->second.features;
}

void SoundFeatureCache::store(const juce::File& file, const dsp::SoundFeatures& features)
{
    const std::lock_guard lock(mutex_);
    loadLocked();
    entries_[key(file)] = {file.getSize(), modified(file), features};
    dirty_ = true;
}

size_t SoundFeatureCache::size()
{
    const std::lock_guard lock(mutex_);
    loadLocked();
    return entries_.size();
}

bool SoundFeatureCache::save()
{
    const std::lock_guard lock(mutex_);
    if (! dirty_) return true;
    juce::MemoryOutputStream text;
    text << kHeader << " " << dsp::SoundFeatures::kVersion << "\n";
    for (const auto& [path, entry] : entries_)
    {
        if (! juce::File(juce::String::fromUTF8(path.c_str())).existsAsFile()) continue; // forget deleted files
        text << juce::String(entry.bytes) << "\t" << juce::String(entry.modified) << "\t" << (entry.features.valid ? "1" : "0") << "\t";
        for (size_t v = 0; v < entry.features.values.size(); ++v)
            text << (v > 0 ? " " : "") << juce::String(entry.features.values[v], 5);
        text << "\t" << juce::String::fromUTF8(path.c_str()) << "\n";
    }
    if (! file_.getParentDirectory().createDirectory()) return false;
    juce::TemporaryFile temporary(file_);
    if (! temporary.getFile().replaceWithData(text.getData(), text.getDataSize()) || ! temporary.overwriteTargetFileWithTemporary())
        return false;
    dirty_ = false;
    return true;
}

std::optional<dsp::SoundFeatures> describeSoundFile(juce::AudioFormatManager& formats, const juce::File& file, double maxSeconds)
{
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (! reader || reader->sampleRate <= 0 || reader->lengthInSamples <= 0 || reader->numChannels == 0) return std::nullopt;
    const double seconds = (double) reader->lengthInSamples / reader->sampleRate;
    if (seconds > maxSeconds) return std::nullopt;
    const auto frames = (int) std::min<juce::int64>(reader->lengthInSamples, (juce::int64) std::ceil(dsp::kSoundFeatureSeconds * reader->sampleRate));
    const int channels = (int) std::min(reader->numChannels, 2u);
    juce::AudioBuffer<float> audio(channels, frames);
    if (! reader->read(&audio, 0, frames, 0, true, channels > 1)) return std::nullopt;
    std::vector<float> mono((size_t) frames, 0.0f);
    for (int c = 0; c < channels; ++c)
    {
        const float* data = audio.getReadPointer(c);
        for (int i = 0; i < frames; ++i) mono[(size_t) i] += data[i] / (float) channels;
    }
    return dsp::extractSoundFeatures(mono.data(), frames, reader->sampleRate, seconds);
}

std::vector<SimilarMatch> rankBySimilarity(const dsp::SoundFeatures& reference,
                                           const std::vector<std::pair<std::string, dsp::SoundFeatures>>& candidates, size_t limit)
{
    std::vector<SimilarMatch> result;
    for (const auto& [path, features] : candidates)
    {
        const float distance = dsp::soundDistance(reference, features);
        if (std::isfinite(distance)) result.push_back({path, distance});
    }
    std::sort(result.begin(), result.end(), [](const SimilarMatch& a, const SimilarMatch& b) {
        if (a.distance < b.distance) return true;
        if (b.distance < a.distance) return false;
        return a.path < b.path;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

std::vector<std::string> similarCandidates(const SimilarSearch& search, const std::function<bool()>& cancelled)
{
    std::vector<std::string> result;
    std::set<std::string> seen{search.reference};
    for (const auto& root : search.roots)
    {
        const juce::File folder(juce::String::fromUTF8(root.c_str()));
        if (! folder.isDirectory()) continue;
        for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                               juce::File::FollowSymlinks::noCycles))
        {
            if (cancelled && cancelled()) return {};
            const auto& file = entry.getFile();
            if (! isAudioFile(file)) continue;
            auto path = file.getFullPathName().toStdString();
            if (! seen.insert(path).second) continue;
            result.push_back(std::move(path));
            if (result.size() >= search.maxCandidates) return result;
        }
    }
    return result;
}

std::vector<SimilarMatch> findSimilarSounds(const SimilarSearch& search, SoundFeatureCache& cache, const std::function<bool()>& cancelled,
                                            const std::function<void(int, int)>& progress, std::string* error)
{
    auto fail = [&](const char* why) { if (error) *error = why; return std::vector<SimilarMatch>{}; };
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    auto describe = [&](const juce::File& file) -> std::optional<dsp::SoundFeatures> {
        if (auto cached = cache.find(file)) return cached;
        auto features = describeSoundFile(formats, file, search.maxSeconds);
        // Unreadable / too long files are cached as invalid, so the next search skips them quickly as well.
        cache.store(file, features ? *features : dsp::SoundFeatures{});
        return features;
    };
    const juce::File referenceFile(juce::String::fromUTF8(search.reference.c_str()));
    // The reference is described even when it is long: it is what the user asked about.
    std::optional<dsp::SoundFeatures> reference = cache.find(referenceFile);
    if (! reference || ! reference->valid)
    {
        reference = describeSoundFile(formats, referenceFile, 1e9);
        if (reference) cache.store(referenceFile, *reference);
    }
    if (! reference) return fail("the sample could not be read");
    if (! reference->valid) return fail("the sample is silent");
    const auto candidates = similarCandidates(search, cancelled);
    if (cancelled && cancelled()) return {};
    std::vector<std::pair<std::string, dsp::SoundFeatures>> described;
    described.reserve(candidates.size());
    const int total = (int) candidates.size();
    if (progress) progress(0, total);
    for (int i = 0; i < total; ++i)
    {
        if (cancelled && cancelled()) { cache.save(); return {}; }
        if (auto features = describe(juce::File(juce::String::fromUTF8(candidates[(size_t) i].c_str()))); features && features->valid)
            described.emplace_back(candidates[(size_t) i], *features);
        if (progress && ((i + 1) % 16 == 0 || i + 1 == total)) progress(i + 1, total);
    }
    cache.save();
    auto ranked = rankBySimilarity(*reference, described, search.maxResults);
    if (ranked.empty()) return fail("no other samples were found near it");
    return ranked;
}

} // namespace sonora::engine
