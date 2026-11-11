// LIB-4 similar samples (feature extraction, ranking, disk cache, search) and LIB-5 sample replacement (scopes, warp
// markers, undo, save/reload).
#include "TestFramework.h"

#include "core/SampleReplace.h"
#include "dsp/SoundFeatures.h"
#include "engine/SoundSimilarity.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/SampleDrop.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <random>
#include <set>

using namespace sonora;

namespace
{
constexpr double kRate = 44100.0;
constexpr double kTwoPi = 6.283185307179586;

// A kick: a sine sweeping down from `from` to `to` Hz with an exponential decay, plus a short click.
std::vector<float> kick(double from, double to, double decay, double seconds = 0.6)
{
    std::vector<float> out((size_t) (seconds * kRate));
    double phase = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = (double) i / kRate;
        const double hz = to + (from - to) * std::exp(-t / 0.04);
        phase += kTwoPi * hz / kRate;
        const double click = t < 0.003 ? (1.0 - t / 0.003) * 0.3 * std::sin(kTwoPi * 3000 * t) : 0.0;
        out[i] = (float) (0.9 * std::sin(phase) * std::exp(-t / decay) + click);
    }
    return out;
}

// A closed hat: high-passed noise with a very short decay.
std::vector<float> hat(unsigned seed, double decay, double seconds = 0.25)
{
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> out((size_t) (seconds * kRate));
    float previous = 0;
    for (size_t i = 0; i < out.size(); ++i)
    {
        const float n = noise(random), high = n - previous; // first difference: a crude high-pass
        previous = n;
        out[i] = (float) (0.5 * high * std::exp(-(double) i / kRate / decay));
    }
    return out;
}

// A pad: a few detuned harmonics with a slow attack, sustained.
std::vector<float> pad(double hz, double attack, double seconds = 4.0)
{
    std::vector<float> out((size_t) (seconds * kRate));
    for (size_t i = 0; i < out.size(); ++i)
    {
        const double t = (double) i / kRate;
        double s = 0;
        for (int h = 1; h <= 6; ++h)
            s += std::sin(kTwoPi * hz * h * t) / h + std::sin(kTwoPi * hz * 1.004 * h * t) / h;
        out[i] = (float) (0.15 * s * std::min(1.0, t / attack));
    }
    return out;
}

dsp::SoundFeatures describe(const std::vector<float>& audio)
{
    return dsp::extractSoundFeatures(audio.data(), (int64_t) audio.size(), kRate, (double) audio.size() / kRate);
}

void writeWav(const juce::File& file, const std::vector<float>& audio)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    REQUIRE(stream != nullptr);
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    const float* channels[] = {audio.data()};
    REQUIRE(writer->writeFromFloatArrays(channels, 1, (int) audio.size()));
}

juce::File scratchFolder(const char* name)
{
    auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    folder.deleteRecursively();
    folder.createDirectory();
    return folder;
}

int audioTrackIndex(const model::Song& song)
{
    for (size_t i = 0; i < song.tracks.size(); ++i)
        if (song.tracks[i].kind == model::TrackKind::Audio) return (int) i;
    return -1;
}
} // namespace

TEST_CASE("sound features: a kick's nearest neighbour is the other kick, a hat's the other hat, a pad's the other pad")
{
    const std::vector<std::pair<std::string, dsp::SoundFeatures>> sounds{
        {"kick a", describe(kick(160, 50, 0.18))}, {"kick b", describe(kick(130, 45, 0.25))},
        {"hat a", describe(hat(1, 0.03))},        {"hat b", describe(hat(2, 0.05))},
        {"pad a", describe(pad(220, 0.6))},       {"pad b", describe(pad(165, 0.9))},
    };
    for (const auto& [name, features] : sounds) CHECK(features.valid);
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        std::vector<std::pair<std::string, dsp::SoundFeatures>> others;
        for (size_t j = 0; j < sounds.size(); ++j) if (j != i) others.push_back(sounds[j]);
        const auto ranked = engine::rankBySimilarity(sounds[i].second, others, 10);
        REQUIRE(ranked.size() == 5);
        const auto& name = sounds[i].first;
        CHECK(ranked.front().path.substr(0, 3) == name.substr(0, 3));
        CHECK(ranked.front().distance <= ranked.back().distance);
    }
    // Brightness and envelope separate the families: a kick is darker than a hat and decays, a pad sustains.
    const auto& k = sounds[0].second.values;
    const auto& h = sounds[2].second.values;
    const auto& p = sounds[4].second.values;
    CHECK(k[dsp::SoundFeatures::Centroid] < h[dsp::SoundFeatures::Centroid]);
    CHECK(h[dsp::SoundFeatures::Flatness] > k[dsp::SoundFeatures::Flatness]);
    CHECK(p[dsp::SoundFeatures::Attack] > k[dsp::SoundFeatures::Attack]);
    CHECK(p[dsp::SoundFeatures::FirstEnvelope + 5] > k[dsp::SoundFeatures::FirstEnvelope + 5]);
    // Silence and nothing are not descriptions.
    const std::vector<float> silence(44100, 0.0f);
    CHECK(! describe(silence).valid);
    CHECK(! dsp::extractSoundFeatures(nullptr, 0, kRate, 0).valid);
    CHECK(std::isinf(dsp::soundDistance(describe(silence), sounds[0].second)));
}

TEST_CASE("similar sounds: files are ranked through an on-disk cache keyed by size and modification time")
{
    const auto folder = scratchFolder("sonora_similar_search");
    writeWav(folder.getChildFile("Kicks/Kick 1.wav"), kick(160, 50, 0.18));
    writeWav(folder.getChildFile("Kicks/Kick 2.wav"), kick(130, 45, 0.25));
    writeWav(folder.getChildFile("Hats/Hat 1.wav"), hat(1, 0.03));
    writeWav(folder.getChildFile("Hats/Hat 2.wav"), hat(2, 0.05));
    writeWav(folder.getChildFile("Pads/Pad 1.wav"), pad(220, 0.6));
    folder.getChildFile("notes.txt").replaceWithText("not audio");
    const auto cacheFile = folder.getChildFile("cache/SoundFeatures.cache");
    engine::SimilarSearch search;
    search.reference = folder.getChildFile("Kicks/Kick 1.wav").getFullPathName().toStdString();
    search.roots = {folder.getChildFile("Kicks").getFullPathName().toStdString(), folder.getFullPathName().toStdString()};
    const auto candidates = engine::similarCandidates(search, {});
    CHECK(candidates.size() == 4); // each file once, not the reference, no text file
    int lastDone = -1, lastTotal = -1;
    {
        engine::SoundFeatureCache cache(cacheFile);
        std::string error;
        const auto ranked = engine::findSimilarSounds(search, cache, {}, [&](int done, int total) { lastDone = done; lastTotal = total; }, &error);
        REQUIRE(ranked.size() == 4);
        CHECK(juce::File(ranked.front().path).getFileName() == "Kick 2.wav");
        CHECK(lastDone == 4); CHECK(lastTotal == 4);
        CHECK(cacheFile.existsAsFile());
    }
    {
        // A fresh cache object reads the file: everything is found without decoding again.
        engine::SoundFeatureCache cache(cacheFile);
        CHECK(cache.size() == 5);
        CHECK(cache.find(folder.getChildFile("Hats/Hat 1.wav")).has_value());
        // A changed file (new size) is described again.
        writeWav(folder.getChildFile("Hats/Hat 1.wav"), hat(3, 0.04, 0.3));
        CHECK(! cache.find(folder.getChildFile("Hats/Hat 1.wav")).has_value());
    }
    {
        // Cancelled before it starts: nothing.
        engine::SoundFeatureCache cache(cacheFile);
        CHECK(engine::findSimilarSounds(search, cache, [] { return true; }, {}).empty());
        std::string error;
        auto missing = search; missing.reference = folder.getChildFile("missing.wav").getFullPathName().toStdString();
        CHECK(engine::findSimilarSounds(missing, cache, {}, {}, &error).empty());
        CHECK(! error.empty());
    }
    folder.deleteRecursively();
}

TEST_CASE("library sound index (LIB-4c): set folders and excluded folders are left out, copies are listed once")
{
    const auto library = scratchFolder("sonora_sound_library");
    writeWav(library.getChildFile("Packs/Kicks/Kick A.wav"), kick(160, 50, 0.18));
    writeWav(library.getChildFile("Packs/Kicks/Kick B.wav"), kick(130, 45, 0.25));
    writeWav(library.getChildFile("Packs/Hats/Hat A.wav"), hat(1, 0.03));
    // A copy of Kick B elsewhere in the library: listed once.
    library.getChildFile("Other").createDirectory();
    REQUIRE(library.getChildFile("Packs/Kicks/Kick B.wav").copyFileTo(library.getChildFile("Other/Kick B.wav")));
    // A set's project folder inside the library (renders, recordings, imported copies): left out.
    library.getChildFile("My Song Project/Ableton Project Info").createDirectory();
    writeWav(library.getChildFile("My Song Project/Samples/Processed/Freeze Kick.wav"), kick(150, 48, 0.2));
    // The open set's own folder (passed as `exclude`).
    writeWav(library.getChildFile("Open Set/Samples/Kick Here.wav"), kick(158, 50, 0.18));
    // The reference lives in the set being edited, outside the library.
    const auto set = scratchFolder("sonora_sound_library_set");
    writeWav(set.getChildFile("Samples/Imported/Kick A.wav"), kick(160, 50, 0.18)); // a copy of Kick A

    engine::SimilarSearch search;
    search.roots = {library.getFullPathName().toStdString()};
    search.exclude = {library.getChildFile("Open Set").getFullPathName().toStdString()};
    search.reference = set.getChildFile("Samples/Imported/Kick A.wav").getFullPathName().toStdString();
    const auto candidates = engine::similarCandidates(search, {});
    std::set<std::string> names;
    for (const auto& path : candidates) names.insert(juce::File(path).getFileName().toStdString());
    CHECK(candidates.size() == 4); // Kick A, Kick B, Hat A, Other/Kick B
    CHECK(! names.contains("Freeze Kick.wav"));
    CHECK(! names.contains("Kick Here.wav"));

    const auto cacheFile = set.getChildFile("SoundFeatures.cache");
    {
        // The background index describes the library once; a search afterwards only reads the cache.
        engine::SoundFeatureCache cache(cacheFile);
        int lastDone = 0, lastTotal = 0, pauses = 0;
        const int indexed = engine::indexSoundLibrary(search, cache, {}, [&](int done, int total) { lastDone = done; lastTotal = total; },
                                                      [&] { ++pauses; });
        CHECK(indexed == 4);
        CHECK(lastDone == 4); CHECK(lastTotal == 4);
        CHECK(pauses == 4);
        CHECK(cache.size() == 4);
        // A second pass finds everything described already.
        pauses = 0;
        CHECK(engine::indexSoundLibrary(search, cache, {}, {}, [&] { ++pauses; }) == 4);
        CHECK(pauses == 0);
    }
    {
        engine::SoundFeatureCache cache(cacheFile);
        std::string error;
        const auto ranked = engine::findSimilarSounds(search, cache, {}, {}, &error);
        // The library's Kick A is the reference's own copy (same name and size): left out. Kick B once, then the hat.
        REQUIRE(ranked.size() == 2);
        CHECK(juce::File(ranked[0].path).getFileName() == "Kick B.wav");
        CHECK(juce::File(ranked[1].path).getFileName() == "Hat A.wav");
    }
    library.deleteRecursively();
    set.deleteRecursively();
}

TEST_CASE("replace sample: scopes, warp markers and names follow the reference, one undo step, saved and reloaded")
{
    const auto folder = scratchFolder("sonora_replace_sample");
    const auto setFile = folder.getChildFile("Replace.als");
    io::NewSetOptions options; options.scenes = 2; options.tempo = 120.0;
    std::string writeError;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), setFile, writeError));
    auto song = io::loadAls(setFile).song;
    REQUIRE(song != nullptr);
    const int audio = audioTrackIndex(*song);
    REQUIRE(audio >= 0);
    // A second audio track (insert a sample with "one track per file" on a fresh track)...
    const auto kickPath = folder.getChildFile("Kick.wav").getFullPathName().toStdString();
    const auto snarePath = folder.getChildFile("Snare.wav").getFullPathName().toStdString();
    auto place = [&](const std::vector<io::DroppedSample>& samples, io::SampleDropTarget target) {
        auto plan = io::placeSamples(*song, samples, target);
        REQUIRE(plan.command.apply != nullptr);
        plan.command.apply(*song);
    };
    // Three kick clips and a snare on the audio track, a kick in its first Session slot, a kick on a new track.
    place({{kickPath, 48000.0, 48000}, {snarePath, 48000.0, 24000}, {kickPath, 48000.0, 48000}, {kickPath, 48000.0, 48000}}, {audio, 0.0, -1, false});
    place({{kickPath, 48000.0, 48000}}, {audio, 0.0, 0, false});
    place({{kickPath, 48000.0, 48000}}, {-1, 0.0, -1, false});
    REQUIRE(song->tracks[(size_t) audio].arrangementClips.size() == 4);
    const int other = (int) song->tracks.size() - 1;
    REQUIRE(other != audio);
    const auto& clips = song->tracks[(size_t) audio].arrangementClips;
    const auto first = model::ClipLocation::arrangement(audio, clips[0].uid);
    CHECK(model::clipsUsingSample(*song, first, model::SampleScope::Clip).size() == 1);
    CHECK(model::clipsUsingSample(*song, first, model::SampleScope::Track).size() == 4); // 3 Arrangement kicks + the slot
    CHECK(model::clipsUsingSample(*song, first, model::SampleScope::Set).size() == 5);
    const auto snare = model::ClipLocation::arrangement(audio, clips[1].uid);
    CHECK(model::clipsUsingSample(*song, snare, model::SampleScope::Track).size() == 1);

    // A clip with edited settings keeps them; a longer file resets the warp markers at the clip's tempo.
    {
        auto edited = clips[0];
        edited.gain = 0.5; edited.pitchCoarse = 3; edited.name = "My Kick";
        edited.warpMarkers = {{0.0, 0.0}, {0.5, 1.0}, {1.0, 2.0}};
        model::EditHistory scratch;
        scratch.perform(*song, model::modifyClip(*song, first, edited, "Edit"));
    }
    const auto newKick = folder.getChildFile("Samples/Kick 2.wav");
    model::SampleFile replacement;
    replacement.ref.absolutePath = newKick.getFullPathName().toStdString();
    replacement.ref.relativePath = "Samples/Kick 2.wav";
    replacement.ref.relativePathType = 3;
    replacement.sampleRate = 44100.0;
    replacement.frames = 88200; // 2 s
    int replaced = 0;
    auto command = model::replaceClipSample(*song, first, replacement, model::SampleScope::Track, &replaced);
    REQUIRE(command.apply != nullptr);
    CHECK(replaced == 4);
    CHECK(command.label == "Replace Sample in 4 Clips");
    CHECK(command.tracks == std::vector<int>{audio});
    model::EditHistory history;
    history.perform(*song, std::move(command));
    {
        const auto& after = song->tracks[(size_t) audio];
        CHECK(after.arrangementClips[0].sample.absolutePath == replacement.ref.absolutePath);
        CHECK(after.arrangementClips[0].gain == 0.5);
        CHECK(after.arrangementClips[0].pitchCoarse == 3);
        CHECK(after.arrangementClips[0].name == "My Kick"); // renamed by the user: kept
        CHECK(after.arrangementClips[2].name == "Kick 2");  // named after its file: follows the file
        REQUIRE(after.arrangementClips[0].warpMarkers.size() == 2);
        CHECK(std::abs(after.arrangementClips[0].warpMarkers[1].seconds - 2.0) < 1e-9);
        CHECK(std::abs(after.arrangementClips[0].warpMarkers[1].beats - 4.0) < 1e-9); // 2 beats/s, the clip's tempo
        CHECK(after.arrangementClips[0].lengthSamples == 88200);
        CHECK(after.arrangementClips[1].sample.absolutePath == snarePath); // another file: untouched
        REQUIRE(after.slots[0].clip.has_value());
        CHECK(after.slots[0].clip->sample.absolutePath == replacement.ref.absolutePath);
        CHECK(song->tracks[(size_t) other].arrangementClips[0].sample.absolutePath == kickPath); // another track
    }
    // The same file again: nothing to do.
    CHECK(model::replaceClipSample(*song, first, replacement, model::SampleScope::Clip).apply == nullptr);
    history.undo(*song);
    CHECK(song->tracks[(size_t) audio].arrangementClips[0].sample.absolutePath == kickPath);
    CHECK(song->tracks[(size_t) audio].arrangementClips[0].warpMarkers.size() == 3);
    CHECK(song->tracks[(size_t) audio].slots[0].clip->sample.absolutePath == kickPath);
    history.redo(*song);
    CHECK(song->tracks[(size_t) audio].arrangementClips[3].sample.absolutePath == replacement.ref.absolutePath);

    // A file of the same length keeps the warp markers.
    {
        const auto& clip = song->tracks[(size_t) audio].arrangementClips[0];
        auto same = replacement;
        same.ref = {};
        same.ref.absolutePath = folder.getChildFile("Kick 3.wav").getFullPathName().toStdString();
        const auto changed = model::withReplacedSample(clip, same);
        CHECK(changed.warpMarkers.size() == clip.warpMarkers.size());
    }

    // Set scope reaches the other track; then save and reload keep the new references.
    replacement.frames = 44100;
    const auto otherKick = model::ClipLocation::arrangement(other, song->tracks[(size_t) other].arrangementClips[0].uid);
    auto everywhere = model::replaceClipSample(*song, otherKick, replacement, model::SampleScope::Set, &replaced);
    REQUIRE(everywhere.apply != nullptr);
    CHECK(replaced == 1); // the other four already use it
    history.perform(*song, std::move(everywhere));
    newKick.getParentDirectory().createDirectory();
    writeWav(newKick, kick(130, 45, 0.25, 1.0));
    const auto saved = io::saveAls(*song, setFile);
    if (! saved.ok) std::printf("save: %s\n", saved.error.c_str());
    REQUIRE(saved.ok);
    const auto loaded = io::loadAls(setFile);
    REQUIRE(loaded.song != nullptr);
    const auto& reloaded = loaded.song->tracks[(size_t) audio];
    REQUIRE(reloaded.arrangementClips.size() == 4);
    CHECK(io::resolveSamplePath(*loaded.song, reloaded.arrangementClips[0].sample) == newKick.getFullPathName().toStdString());
    CHECK(reloaded.arrangementClips[1].sample.absolutePath == snarePath); // missing on disk: kept as it was
    CHECK(io::resolveSamplePath(*loaded.song, loaded.song->tracks[(size_t) other].arrangementClips[0].sample) == newKick.getFullPathName().toStdString());
    REQUIRE(reloaded.slots[0].clip.has_value());
    CHECK(io::resolveSamplePath(*loaded.song, reloaded.slots[0].clip->sample) == newKick.getFullPathName().toStdString());
    CHECK(reloaded.arrangementClips[0].lengthSamples == 88200);
    CHECK(reloaded.arrangementClips[0].sample.relativePath == "Samples/Kick 2.wav"); // project-relative, like Collect All
    CHECK(reloaded.arrangementClips[0].sample.relativePathType == 3);
    folder.deleteRecursively();
}
