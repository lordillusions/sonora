#pragma once
// LIB-4 "Show Similar Files": ranks sample files by how they sound (dsp/SoundFeatures) against a reference file.
// The description of each file is cached on disk (keyed by path, size and modification time), so a repeated search over
// the same folders only decodes what changed. Worker threads only (decodes files); never the audio thread.

#include "dsp/SoundFeatures.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sonora::engine
{

class SoundFeatureCache
{
public:
    explicit SoundFeatureCache(juce::File file) : file_(std::move(file)) {}
    // The cached description of `file` when its size and modification time still match.
    std::optional<dsp::SoundFeatures> find(const juce::File& file);
    void store(const juce::File& file, const dsp::SoundFeatures& features);
    bool save(); // writes the cache file when something changed; true on success or nothing to do
    size_t size();

private:
    struct Entry
    {
        int64_t bytes = 0, modified = 0;
        dsp::SoundFeatures features;
    };
    void loadLocked();
    juce::File file_;
    std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    bool loaded_ = false, dirty_ = false;
};

// Decodes the first kSoundFeatureSeconds of `file`, mixes it to mono and describes it. Files longer than `maxSeconds`
// (the reference skips long files too) and unreadable ones give nullopt.
std::optional<dsp::SoundFeatures> describeSoundFile(juce::AudioFormatManager& formats, const juce::File& file, double maxSeconds = 60.0);

struct SimilarMatch
{
    std::string path;
    float distance = 0;
};

// The `limit` nearest candidates to `reference`, nearest first (ties by path); invalid descriptions are left out.
std::vector<SimilarMatch> rankBySimilarity(const dsp::SoundFeatures& reference,
                                           const std::vector<std::pair<std::string, dsp::SoundFeatures>>& candidates, size_t limit);

struct SimilarSearch
{
    std::string reference;            // absolute path of the file to match
    std::vector<std::string> roots;   // folders searched recursively, in order
    std::vector<std::string> exclude; // folders whose files are never candidates (the open set's own folder)
    bool skipProjects = true;         // LIB-4c: leave out set folders (renders, recordings and copies inside them)
    size_t maxCandidates = 5000;      // files looked at, at most
    size_t maxResults = 200;
    double maxSeconds = 60.0;
};

// Lists the audio files below the roots (each file once, hidden files and the reference itself left out), up to the cap.
// With `skipProjects`, a folder holding an "Ableton Project Info" folder (a set's project folder) and everything below it
// is left out, as are the folders in `exclude` and the Apple Music library.
std::vector<std::string> similarCandidates(const SimilarSearch&, const std::function<bool()>& cancelled);

// The cached description of `file`, or a fresh one stored in the cache (unreadable / too long files are stored invalid).
std::optional<dsp::SoundFeatures> describeCached(juce::AudioFormatManager&, SoundFeatureCache&, const juce::File&, double maxSeconds);

// LIB-4c: describes every candidate of `library` that the cache does not know yet (the library sound index Show Similar
// Files ranks against), saving the cache every few hundred files. `pause` is called between files (the caller sleeps
// there to stay in the background). Returns how many files the index holds (valid descriptions).
int indexSoundLibrary(const SimilarSearch& library, SoundFeatureCache&, const std::function<bool()>& cancelled,
                      const std::function<void(int done, int total)>& progress, const std::function<void()>& pause = {});

// Describes the reference and every candidate (through the cache), then ranks them. `progress(done, total)` is called
// from this thread now and then; `cancelled` is polled between files (an empty result is returned when it says so).
// `error` explains an empty result (the reference could not be read, ...).
std::vector<SimilarMatch> findSimilarSounds(const SimilarSearch&, SoundFeatureCache&, const std::function<bool()>& cancelled,
                                            const std::function<void(int done, int total)>& progress, std::string* error = nullptr);

} // namespace sonora::engine
