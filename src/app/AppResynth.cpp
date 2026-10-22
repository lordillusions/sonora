#include "app/AppController.h"
#include "app/PromptWindow.h"
#include "core/ResynthModel.h"
#include "devices/DeviceFactory.h"
#include "engine/ResynthAnalysis.h"
#include "engine/ResynthEdits.h"
#include "engine/ClipRender.h"
#include "io/als/AlsTemplates.h"
#include <cmath>
#include <cstdio>

namespace sonora::app
{
namespace
{
model::Device* target(model::Device& root, int chain, int child)
{
    if (chain < 0) return root.type == "SonoraResynth" ? &root : nullptr;
    if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return nullptr;
    auto& device = root.chains[(size_t) chain].devices[(size_t) child];
    return device.type == "SonoraResynth" ? &device : nullptr;
}


std::string seconds(double value)
{
    char text[32]; std::snprintf(text, sizeof(text), "%.1f s", value); return text;
}

// Too-long sources: say why and offer the leading part that fits; Cancel leaves everything unchanged.
void offerCut(std::string title, std::string message, std::string accept, std::function<void()> proceed)
{
    PromptOptions options;
    options.state.title = std::move(title);
    options.state.message = std::move(message);
    options.state.buttons = {{std::move(accept), true, false}, {"Cancel", false, false}};
    options.defaultButton = 0; options.cancelButton = 1; options.shortcuts = {0, 0};
    PromptWindow::show(std::move(options), [next = std::move(proceed)](int button) { if (button == 0) next(); });
}

// Rendering rounds up to whole samples; stay this far inside the analysis limit.
constexpr double cutMargin = 0.05;

// The last bar line within the analysis limit (clip and song bars start at beat 0), or the limit itself when the
// clip starts within that bar.
double resynthCutBeat(const model::Song& song, double start, double end)
{
    const double beatsPerBar = song.timeSigNumerator * 4.0 / std::max(1, song.timeSigDenominator);
    const double limit = start + (model::ResynthModel::maxSeconds - cutMargin) * song.tempo / 60;
    double cut = std::floor(limit / beatsPerBar) * beatsPerBar;
    if (cut - start < beatsPerBar) cut = limit;
    return std::min(cut, end);
}

std::string barsText(double beats, const model::Song& song)
{
    const double beatsPerBar = song.timeSigNumerator * 4.0 / std::max(1, song.timeSigDenominator);
    const double bars = beats / beatsPerBar;
    if (std::abs(bars - std::round(bars)) > 1e-6) return seconds(beats * 60 / song.tempo);
    const int whole = (int) std::lround(bars);
    return std::to_string(whole) + (whole == 1 ? " bar" : " bars");
}
}

void AppController::importResynth(int trackIndex, int deviceIndex, const std::string& path, int chain, int child, double limitSeconds)
{
    if (! song_ || trackIndex < 0 || deviceIndex < 0) return;
    if (limitSeconds <= 0)
    {
        const juce::File sourceFile(juce::String::fromUTF8(path.c_str()));
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sourceFile));
        const double length = reader && reader->sampleRate > 0 ? (double) reader->lengthInSamples / reader->sampleRate : 0;
        if (length > model::ResynthModel::maxSeconds)
        {
            const auto name = sourceFile.getFileName().toStdString();
            std::weak_ptr<bool> alive = alive_;
            offerCut("Audio is too long for Resynth",
                     "\u201C" + name + "\u201D is " + seconds(length) + ". Resynth analyzes up to "
                         + seconds(model::ResynthModel::maxSeconds) + " (64 notes) per instrument. Analyze the first "
                         + seconds(model::ResynthModel::maxSeconds) + "? To use a different part, trim the file or "
                         "place it on an audio track and use Resynthesize to New MIDI Track on a shorter clip.",
                     "Use First " + seconds(model::ResynthModel::maxSeconds),
                     [this, alive, trackIndex, deviceIndex, path, chain, child] {
                         if (! alive.expired()) importResynth(trackIndex, deviceIndex, path, chain, child, model::ResynthModel::maxSeconds);
                     });
            return;
        }
    }
    runMediaEdit("Analyze Resynth", false, [this, trackIndex, deviceIndex, path, chain, child, limitSeconds](const model::Song& song) {
        MediaEditResult result;
        const auto* track = model::trackAt(song, trackIndex);
        if (! track || (size_t) deviceIndex >= track->devices.size()) { result.error = "Resynth device is no longer available"; return result; }
        if (track->frozen) { result.error = "Unfreeze the track before replacing its Resynth model"; return result; }
        auto replacement = track->devices[(size_t) deviceIndex]; auto* device = target(replacement, chain, child);
        if (! device) { result.error = "Select a Sonora Resynth device"; return result; }
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        const juce::File file(juce::String::fromUTF8(path.c_str()));
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (! reader) { result.error = "Could not read \u201C" + file.getFileName().toStdString() + "\u201D as audio"; return result; }
        const double rate = reader->sampleRate;
        auto frames = reader->lengthInSamples;
        if (limitSeconds > 0 && std::isfinite(rate)) frames = std::min(frames, (juce::int64) (limitSeconds * rate));
        if (! std::isfinite(rate) || rate < 8000 || rate > 96000)
        { result.error = "Resynth needs audio at 8\u201396 kHz; this file is " + std::to_string((int) rate) + " Hz"; return result; }
        if (reader->numChannels < 1 || reader->numChannels > 2)
        { result.error = "Resynth needs mono or stereo audio; this file has " + std::to_string(reader->numChannels) + " channels"; return result; }
        if (frames < (juce::int64) (rate * 0.08)) { result.error = "The audio is too short for Resynth (at least 0.08 s)"; return result; }
        if ((double) frames > rate * model::ResynthModel::maxSeconds)
        { result.error = "Resynth analyzes up to " + seconds(model::ResynthModel::maxSeconds) + " of audio"; return result; }
        juce::AudioBuffer<float> audio((int) reader->numChannels, (int) frames);
        if (! reader->read(&audio, 0, audio.getNumSamples(), 0, true, true)) { result.error = "Could not read the Resynth source"; return result; }
        const auto analysis = engine::analyzeResynth(audio, rate, file.getFileNameWithoutExtension().toStdString(), exportCancel_, exportProgress_);
        if (! analysis.model)
        {
            result.error = analysis.error;
            if (analysis.fitSeconds > 0)
            {
                std::weak_ptr<bool> alive = alive_;
                result.offer = [this, alive, trackIndex, deviceIndex, path, chain, child, fit = analysis.fitSeconds, message = analysis.error] {
                    if (alive.expired()) return;
                    offerCut("Too many notes for Resynth", message + ". Analyze up to the 64th note (first " + seconds(fit) + ")?",
                             "Use First 64 Notes", [this, alive, trackIndex, deviceIndex, path, chain, child, fit] {
                                 if (! alive.expired()) importResynth(trackIndex, deviceIndex, path, chain, child, fit);
                             });
                };
            }
            return result;
        }
        device->nativeContent = model::encodeResynth(*analysis.model, result.error); device->nativeContentEdited = true;
        if (! device->nativeContent) return result;
        device->displayName = "Sonora Resynth";
        if (chain >= 0) replacement.chainsEdited = true;
        replacement.id = -1;
        if (chain >= 0) device->id = -1;
        auto allocator = song; model::allocateDeviceIds(allocator, replacement);
        result.edit = model::replaceDevice(song, trackIndex, deviceIndex, std::move(replacement), "Analyze Resynth");
        const auto apply = result.edit.apply;
        result.edit.apply = [apply, deviceEnd = allocator.nextDeviceId, pointeeEnd = allocator.nextPointeeId](model::Song& destination) {
            destination.nextDeviceId = std::max(destination.nextDeviceId, deviceEnd);
            destination.nextPointeeId = std::max(destination.nextPointeeId, pointeeEnd); destination.pointeeIdsAllocated = true;
            apply(destination);
        };
        result.selectTrack = trackIndex; return result;
    });
}

void AppController::resynthesizeClip(const model::ClipLocation& location, double endBeat)
{
    if (! song_ || song_->tempo <= 0) return;
    const auto* source = model::findClip(*song_, location);
    if (! source || source->kind != model::Clip::Kind::Audio) return;
    const auto clip = *source;
    const double end = endBeat > clip.currentStart ? std::min(endBeat, clip.currentEnd) : clip.currentEnd;
    const double length = (end - clip.currentStart) * 60 / song_->tempo;
    std::weak_ptr<bool> alive = alive_;
    if (length > model::ResynthModel::maxSeconds)
    {
        const double cut = resynthCutBeat(*song_, clip.currentStart, end);
        const auto part = barsText(cut - clip.currentStart, *song_);
        const bool arrangement = location.kind != model::ClipLocation::Kind::Session;
        offerCut("Clip is too long for Resynth",
                 "\u201C" + clip.name + "\u201D is " + seconds(length) + ". Resynth analyzes up to "
                     + seconds(model::ResynthModel::maxSeconds) + " (64 notes) per instrument. Resynthesize the first " + part
                     + (part.back() == 's' && part.find("bar") == std::string::npos ? "" : " (" + seconds((cut - clip.currentStart) * 60 / song_->tempo) + ")")
                     + "? "
                     + (arrangement ? "To keep the rest, split the clip (\u2318E) and resynthesize each part."
                                    : "To use another part, shorten the clip's start/end first."),
                 "Use First " + part,
                 [this, alive, location, cut] { if (! alive.expired()) resynthesizeClip(location, cut); });
        return;
    }
    runMediaEdit("Resynthesize", false, [this, alive, location, clip, end](const model::Song& song) {
        MediaEditResult result;
        auto rendered = engine::renderClipRange(song, {clip}, clip.currentStart, end, engine_.sampleRate(), engine_.sampleCache(), exportCancel_);
        if (! rendered.sample) { result.error = rendered.error; return result; }
        const auto analysis = engine::analyzeResynth(rendered.sample->audio, rendered.sample->sampleRate, clip.name, exportCancel_, exportProgress_);
        if (! analysis.model)
        {
            result.error = analysis.error;
            if (analysis.fitSeconds > 0)
            {
                const double cut = clip.currentStart + analysis.fitSeconds * song.tempo / 60;
                result.offer = [this, alive, location, cut, fit = analysis.fitSeconds, message = analysis.error] {
                    if (alive.expired()) return;
                    offerCut("Too many notes for Resynth", message + ". Resynthesize up to the 64th note (first " + seconds(fit) + ")?",
                             "Use First 64 Notes", [this, alive, location, cut] { if (! alive.expired()) resynthesizeClip(location, cut); });
                };
            }
            return result;
        }
        auto instrument = devices::nativeDeviceModel("SonoraResynth");
        instrument.nativeContent = model::encodeResynth(*analysis.model, result.error); instrument.nativeContentEdited = true;
        if (! instrument.nativeContent) return result;
        result.selectTrack = location.track + 1;
        result.edit = engine::resynthTrack(song, *analysis.model, std::move(instrument), location.track,
            location.kind == model::ClipLocation::Kind::Session ? 0 : clip.currentStart,
            location.kind == model::ClipLocation::Kind::Session ? location.scene : -1);
        return result;
    });
}

void AppController::createResynthPhrase(int trackIndex, int deviceIndex, int chain, int child)
{
    if (! song_ || exporting_ || ui_.recording || ui_.recordingRequested) return;
    syncModelFromEngine();
    const auto* track = model::trackAt(*song_, trackIndex);
    if (! track || deviceIndex < 0 || (size_t) deviceIndex >= track->devices.size()) return;
    auto root = track->devices[(size_t) deviceIndex]; const auto* device = target(root, chain, child);
    if (! device || ! device->nativeContent) return;
    std::string error; auto analysis = model::decodeResynth(*device->nativeContent, error);
    if (! analysis) { ui_.statusText = error; return; }
    auto command = engine::resynthTrack(*song_, *analysis, *device, trackIndex, engine_.transport().positionBeats());
    if (command.apply && applyEdit(std::move(command)).ok) ui_.selectTrack(trackIndex + 1, false, false);
}
} // namespace sonora::app
