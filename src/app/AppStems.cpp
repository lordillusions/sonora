// Clip > Stems: separate an audio clip's sample into stems with an installed command-line tool and add them as new tracks.

#include "app/AppController.h"

#include "engine/StemSeparator.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "util/Log.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace sonora::app
{

void AppController::separateClipStems(const model::ClipLocation& location)
{
    if (stemsRunning_)
    {
        stemsCancel_ = true;
        ui_.statusText = "Cancelling the stem separation...";
        return;
    }
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording || exporting_)
        return;
    const auto* clip = model::findClip(*song_, location);
    if (clip == nullptr || clip->kind != model::Clip::Kind::Audio)
        return;
    const auto path = io::resolveSamplePath(*song_, clip->sample);
    if (path.empty())
    {
        ui_.statusText = "Cannot separate: the sample file was not found";
        return;
    }
    std::vector<juce::File> extra;
    if (const auto folder = settings().getValue("stemToolDirectory"); folder.isNotEmpty())
        extra.emplace_back(folder);
    const auto tools = engine::StemSeparator::detectTools(extra);
    if (tools.empty())
    {
        juce::String message = "Sonora separates stems with a command-line tool and none is installed.\n\n"
                               "Install one of these in Terminal, then try again:\n\n"
                               "   uv tool install \"audio-separator[cpu]\"\n   pipx install demucs\n\n";
        if (engine::StemSeparator::uvrModelFolder() != juce::File())
            message += "Ultimate Vocal Remover is installed: Sonora will reuse its MDX-Net model, so audio-separator needs no model download.";
        else
            message += "The first run downloads the model (a few hundred MB).";
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "No stem separation tool found", message);
        ui_.statusText = "Stems: no separation tool installed (audio-separator or demucs)";
        return;
    }
    const auto& tool = tools.front();

    const juce::File projectDirectory = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory();
    const juce::File input(juce::String::fromUTF8(path.c_str()));
    const auto work = projectDirectory.getChildFile("Samples/Processed/Stems/.work-" + juce::String(juce::Time::currentTimeMillis()));
    const auto finalFolder = projectDirectory.getChildFile("Samples/Processed/Stems");
    const int generation = documentGeneration_;
    const std::weak_ptr<bool> alive = alive_;
    const model::Clip original = *clip;
    stemsRunning_ = ui_.stemsRunning = true;
    stemsCancel_ = false;
    ui_.statusText = "Separating stems with " + tool.label + " (this takes a while)...";
    stemsBusy_ = ui_.beginBusy(ui_.statusText, true);

    pool_.addJob([this, alive, generation, location, original, tool, input, work, finalFolder, projectDirectory] {
        auto result = std::make_shared<engine::StemResult>(engine::StemSeparator::separate(
            tool, input, work, stemsCancel_, [this, alive](const std::string& line) {
                juce::MessageManager::callAsync([this, alive, line] {
                    if (! alive.expired() && stemsRunning_)
                    {
                        ui_.statusText = "Stems: " + line.substr(0, 90);
                        ui_.updateBusy(stemsBusy_, ui_.statusText);
                    }
                });
            }));
        // Move the results out of the work folder under readable names.
        std::vector<engine::StemFile> placed;
        if (result->ok)
        {
            const auto base = input.getFileNameWithoutExtension();
            for (const auto& stem : result->stems)
            {
                const auto target = finalFolder.getNonexistentChildFile(base + " [" + juce::String(stem.name) + "]", ".wav", false);
                if (stem.file.moveFileTo(target))
                    placed.push_back({stem.name, target});
            }
            if (placed.empty())
            {
                result->ok = false;
                result->error = "The stems could not be moved into the project";
            }
        }
        work.deleteRecursively();
        juce::MessageManager::callAsync([this, alive, generation, location, original, result, placed, projectDirectory] {
            if (alive.expired())
                return;
            stemsRunning_ = ui_.stemsRunning = false;
            ui_.endBusy(stemsBusy_);
            if (result->cancelled)
            {
                ui_.statusText = "Stem separation cancelled";
                return;
            }
            if (! result->ok)
            {
                ui_.statusText = "Stems failed: " + result->error;
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Stem separation failed", result->error);
                return;
            }
            if (generation != documentGeneration_ || ! song_ || editingDevices_ || syncingModel_ || ui_.recording)
            {
                ui_.statusText = "Stems were written to Samples/Processed/Stems, but the document changed, so no tracks were added";
                return;
            }
            const auto* current = model::findClip(*song_, location);
            if (current == nullptr || current->uid != original.uid)
            {
                ui_.statusText = "The clip changed while separating; the stems are in Samples/Processed/Stems";
                return;
            }
            addStemTracks(original, placed, projectDirectory);
        });
    });
}

void AppController::addStemTracks(const model::Clip& original, const std::vector<engine::StemFile>& stems, const juce::File& projectDirectory)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    int firstId = 0;
    for (const auto& t : song_->tracks) firstId = std::max(firstId, t.id + 1);
    for (const auto& t : song_->returns) firstId = std::max(firstId, t.id + 1);
    int pointee = std::max(1, song_->nextPointeeId), pointeeEnd = pointee;
    const int firstIndex = (int) song_->tracks.size();
    std::vector<model::Track> tracks;
    std::vector<model::Clip> clips;
    for (size_t i = 0; i < stems.size(); ++i)
    {
        const auto& stem = stems[i];
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(stem.file));
        if (reader == nullptr || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
            continue;
        const int colour = (original.colorIndex + 1 + (int) i * 3) % 70;
        tracks.push_back(io::createAudioTrack(*song_, stem.name, colour, firstId + (int) tracks.size(), pointee, pointeeEnd));
        pointee = pointeeEnd;
        auto clip = original; // warp markers, loop, gain and fades carry over: the stem has the same timing as the source
        clip.uid = -1;
        clip.name = original.name.empty() ? stem.name : original.name + " (" + stem.name + ")";
        clip.colorIndex = colour;
        clip.sample.absolutePath = stem.file.getFullPathName().toStdString();
        clip.sample.relativePath = stem.file.getRelativePathFrom(projectDirectory).replaceCharacter('\\', '/').toStdString();
        clip.sample.relativePathType = 3;
        clip.sample.sourceDirectory = projectDirectory.getFullPathName().toStdString();
        clip.sampleRate = reader->sampleRate;
        clip.lengthSamples = reader->lengthInSamples;
        clips.push_back(std::move(clip));
    }
    if (tracks.empty())
    {
        ui_.statusText = "Stems: the separator wrote no readable audio";
        return;
    }
    std::vector<model::EditCommand> parts;
    parts.push_back(model::insertTracks(tracks, pointeeEnd, "Insert Tracks"));
    for (size_t i = 0; i < clips.size(); ++i)
        parts.push_back(model::insertClip(firstIndex + (int) i, std::move(clips[i]), "Insert Stem"));
    applyEdit(model::compose("Separate Stems", std::move(parts)));
    ui_.selectedTrack = firstIndex;
    ui_.statusText = "Added " + std::to_string(tracks.size()) + " stem tracks; the original clip is unchanged";
    if (ui_.modelChanged) ui_.modelChanged();
}

} // namespace sonora::app
