// LIB-3 / LIB-4 / LIB-5: the sample file under an audio clip. Show Similar Files ranks samples near the clip's file by sound
// (engine/SoundSimilarity, on similarPool_ with an on-disk feature cache); Replace Sample... / browser drops / swaps put
// another file into the clip, or into every clip of the track or set that uses the same file, in one undo step
// (core/SampleReplace). The engine picks the new file up like any clip edit that changes the sample (Crop, Reverse).
#include "app/AppController.h"
#include "core/SampleReplace.h"
#include "engine/AudioFileFormats.h"
#include "io/als/AlsReader.h"
#include "platform/Paths.h"
#include "ui/SampleFiles.h"

namespace sonora::app
{

namespace
{
bool sameClip(const model::ClipLocation& a, const model::ClipLocation& b)
{
    return a.kind == b.kind && a.track == b.track && a.uid == b.uid && a.scene == b.scene && a.laneId == b.laneId;
}

void addRoot(std::vector<std::string>& roots, const juce::File& folder)
{
    if (! folder.isDirectory()) return;
    const auto path = folder.getFullPathName().toStdString();
    for (const auto& root : roots)
        if (path == root || folder.isAChildOf(juce::File(juce::String::fromUTF8(root.c_str())))) return; // already covered
    roots.push_back(path);
}
} // namespace

void AppController::setUpSampleFiles()
{
    soundFeatures_ = std::make_shared<engine::SoundFeatureCache>(platform::appDataFolder().getChildFile("SoundFeatures.cache"));
    ui_.findSimilarSamples = [this](const std::string& path, const model::ClipLocation& clip) { findSimilarSamples(path, clip); };
    ui_.cancelSimilarSamples = [this] { ++*similarGeneration_; };
    ui_.replaceClipSample = [this](const model::ClipLocation& location, const std::string& path) {
        replaceClipSample(location, path, ui_.sampleReplaceScope);
    };
    ui_.chooseClipSample = [this](const model::ClipLocation& location) { chooseClipSample(location); };
    ui_.swapSimilarSample = [this](const model::ClipLocation& location, int direction) { swapSimilarSample(location, direction); };
}

// The folders a similar search looks through, nearest first: the file's own folder, two levels above it (not the home or
// a volume root), the set's folder, the browser's Places, installed packs and the User Library. At most 5000 files.
void AppController::findSimilarSamples(const std::string& path, const model::ClipLocation& clip, int pendingSwap)
{
    const int generation = ++*similarGeneration_;
    auto& similar = ui_.similarSamples;
    const auto revision = similar.revision;
    similar = {};
    similar.active = similar.running = true;
    similar.reference = path;
    similar.clip = clip;
    similar.revision = revision + 1;
    engine::SimilarSearch search;
    search.reference = path;
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    auto folder = file.getParentDirectory();
    addRoot(search.roots, folder);
    for (int level = 0; level < 2; ++level)
    {
        const auto up = folder.getParentDirectory();
        if (up == folder || up == home || up.getParentDirectory() == up || up.getParentDirectory().getParentDirectory() == up.getParentDirectory()
            || ! file.isAChildOf(up))
            break;
        folder = up;
    }
    addRoot(search.roots, folder);
    if (song_) addRoot(search.roots, juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory());
    for (const auto& place : ui_.browserPlaces) addRoot(search.roots, juce::File(juce::String::fromUTF8(place.c_str())));
    if (! ui_.samplePacksRoot.empty()) addRoot(search.roots, juce::File(juce::String::fromUTF8(ui_.samplePacksRoot.c_str())));
    if (! ui_.userLibraryPath.empty()) addRoot(search.roots, juce::File(juce::String::fromUTF8(ui_.userLibraryPath.c_str())).getChildFile("Samples"));
    ui_.statusText = "Looking for samples that sound like " + file.getFileName().toStdString() + "...";
    if (ui_.requestRepaint) ui_.requestRepaint();
    similarPool_.addJob([this, search, generation, pendingSwap, clip, current = similarGeneration_, cache = soundFeatures_,
                         alive = std::weak_ptr<bool>(alive_)] {
        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        const auto cancelled = [&] { return current->load() != generation || (job && job->shouldExit()); };
        auto lastReport = juce::Time::getMillisecondCounter();
        const auto progress = [&](int done, int total) {
            const auto now = juce::Time::getMillisecondCounter();
            if (done != 0 && done != total && now - lastReport < 100) return;
            lastReport = now;
            juce::MessageManager::callAsync([this, alive, current, generation, done, total] {
                if (alive.expired() || current->load() != generation) return;
                ui_.similarSamples.done = done;
                ui_.similarSamples.total = total;
                if (ui_.requestRepaint) ui_.requestRepaint();
            });
        };
        std::string error;
        auto found = engine::findSimilarSounds(search, *cache, cancelled, progress, &error);
        if (cancelled()) return;
        juce::MessageManager::callAsync([this, alive, current, generation, pendingSwap, clip, error, matches = std::move(found)]() mutable {
            if (alive.expired() || current->load() != generation) return;
            auto& state = ui_.similarSamples;
            state.running = false;
            state.results.clear();
            for (auto& match : matches) state.results.emplace_back(std::move(match.path), match.distance);
            state.message = error.empty() ? std::string{} : "Nothing found: " + error;
            ++state.revision;
            ui_.statusText = error.empty() ? std::to_string(state.results.size()) + " similar samples" : state.message;
            if (ui_.requestRepaint) ui_.requestRepaint();
            if (pendingSwap != 0 && ! state.results.empty()) swapSimilarSample(clip, pendingSwap);
        });
    });
}

// LIB-4/5: steps through the similar list of the clip's reference file. The list stays anchored on the file the search
// started from, so Next walks further away from it and Previous comes back (to the reference itself at the top).
void AppController::swapSimilarSample(const model::ClipLocation& location, int direction)
{
    const auto current = ui::clipSamplePath(ui_, location);
    if (current.empty()) { ui_.statusText = "The sample file is missing"; return; }
    auto& similar = ui_.similarSamples;
    int index = -2; // not in the list
    if (similar.active && ! similar.running && sameClip(similar.clip, location) && ! similar.results.empty())
    {
        if (similar.reference == current) index = -1;
        for (size_t i = 0; i < similar.results.size(); ++i)
            if (similar.results[i].first == current) index = (int) i;
    }
    if (index == -2)
    {
        if (similar.running && sameClip(similar.clip, location) && similar.reference == current) return; // already searching
        findSimilarSamples(current, location, direction);
        return;
    }
    const int next = index + (direction < 0 ? -1 : 1);
    if (next >= (int) similar.results.size()) { ui_.statusText = "That was the last similar sample"; return; }
    if (next < -1) { ui_.statusText = "This is the sample the similar list started from"; return; }
    replaceClipSample(location, next < 0 ? similar.reference : similar.results[(size_t) next].first, ui_.sampleReplaceScope);
}

void AppController::replaceClipSample(const model::ClipLocation& location, const std::string& path, model::SampleScope scope)
{
    if (! song_ || exporting_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested) return;
    if (collabGuest() && inCollabSession()) { ui_.statusText = "Only the host can replace samples in a shared session"; return; }
    const auto* clip = model::findClip(*song_, location);
    if (clip == nullptr || clip->kind != model::Clip::Kind::Audio) return;
    if (const auto* track = model::trackAt(*song_, location.track); track && track->frozen)
    {
        ui_.statusText = "Unfreeze the track to replace its samples";
        return;
    }
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    if (! engine::isAudioFile(file) || ! file.existsAsFile()) { ui_.statusText = "Cannot use " + file.getFileName().toStdString() + " as a sample"; return; }
    const int generation = documentGeneration_, uid = clip->uid;
    const auto alive = std::weak_ptr<bool>(alive_);
    ui_.statusText = "Reading " + file.getFileName().toStdString() + "...";
    pool_.addJob([this, alive, generation, uid, location, file, scope] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        const double rate = reader ? reader->sampleRate : 0.0;
        const int64_t frames = reader ? (int64_t) reader->lengthInSamples : 0;
        juce::MessageManager::callAsync([this, alive, generation, uid, location, file, scope, rate, frames] {
            if (alive.expired() || generation != documentGeneration_ || ! song_) return;
            if (exporting_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested) return;
            const auto* current = model::findClip(*song_, location);
            if (current == nullptr || current->uid != uid) { ui_.statusText = "The clip changed; nothing was replaced"; return; }
            if (rate <= 0 || frames <= 0) { ui_.statusText = "Could not read " + file.getFileName().toStdString(); return; }
            model::SampleFile replacement;
            replacement.ref.absolutePath = file.getFullPathName().toStdString();
            const auto project = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory();
            if (file.isAChildOf(project))
            {
                replacement.ref.relativePath = file.getRelativePathFrom(project).replaceCharacter('\\', '/').toStdString();
                replacement.ref.relativePathType = 3;
                replacement.ref.sourceDirectory = project.getFullPathName().toStdString();
            }
            replacement.sampleRate = rate;
            replacement.frames = frames;
            int replaced = 0;
            auto command = model::replaceClipSample(*song_, location, replacement, scope, &replaced);
            if (! command.apply) { ui_.statusText = "The clip already uses " + file.getFileName().toStdString(); return; }
            if (! applyEdit(std::move(command)).ok) return;
            ui_.statusText = "Replaced the sample " + std::string(replaced == 1 ? "of 1 clip" : "of " + std::to_string(replaced) + " clips")
                           + " with " + file.getFileName().toStdString();
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
}

void AppController::chooseClipSample(const model::ClipLocation& location)
{
    if (! song_) return;
    const auto current = ui::clipSamplePath(ui_, location);
    const auto start = current.empty() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                       : juce::File(juce::String::fromUTF8(current.c_str()));
    const auto scope = ui_.sampleReplaceScope;
    chooser_ = std::make_unique<juce::FileChooser>(juce::String("Replace Sample (") + model::sampleScopeLabel(scope) + ")", start,
                                                   engine::audioFileWildcard());
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, location, scope, generation = documentGeneration_](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file.existsAsFile() && generation == documentGeneration_)
                replaceClipSample(location, file.getFullPathName().toStdString(), scope);
        });
}

} // namespace sonora::app
