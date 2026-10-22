// UI-38: render/analysis jobs share the export modal, cancellation and document edit guard.
#include "app/AppController.h"
#include "core/ClipOperations.h"
#include "engine/AudioToMidi.h"
#include "engine/ClipRender.h"
#include "engine/SampleExport.h"
#include "engine/StripSilence.h"
#include "engine/stretch/Transients.h"
#include "ui/AudioClipEdits.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/SampleDrop.h"

namespace sonora::app
{
namespace
{
juce::File processedFolder(const model::Song& song, const char* name)
{
    return juce::File(juce::String::fromUTF8(song.sourcePath.c_str())).getParentDirectory()
        .getChildFile("Samples/Processed").getChildFile(name).getChildFile(juce::Uuid().toString());
}
model::Clip renderedClip(const model::Song& song, const juce::File& file, double rate, int64_t frames, int color, double start)
{
    auto clip = io::makeSampleClip(song, {file.getFullPathName().toStdString(), rate, frames}, color, start);
    const auto project = juce::File(juce::String::fromUTF8(song.sourcePath.c_str())).getParentDirectory();
    if (file.isAChildOf(project))
    {
        clip.sample.relativePath = file.getRelativePathFrom(project).replaceCharacter('\\', '/').toStdString();
        clip.sample.relativePathType = 3; clip.sample.sourceDirectory = project.getFullPathName().toStdString();
    }
    clip.uid = model::allocateClipUid();
    return clip;
}
}

void AppController::runMediaEdit(std::string label, bool offline, std::function<MediaEditResult(const model::Song&)> worker)
{
    if (! song_ || exporting_ || stemsRunning_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested) return;
    syncModelFromEngine();
    engine_.transport().stop();
    if (offline) { closePluginWindows(); closeWavetableWindows(); }
    if (offline && ! engine_.beginOffline()) { ui_.statusText = "The audio engine is not ready"; return; }
    auto snapshot = std::make_shared<const model::Song>(*song_);
    const auto generation = documentGeneration_;
    exporting_ = true; exportCancel_ = false; exportProgress_ = 0;
    ui_.exportOpen = true; ui_.exportState = {}; ui_.exportState.phase = ui::ExportState::Phase::Running;
    ui_.exportState.message = label; exportPoller_.startTimerHz(10);
    if (ui_.requestRepaint) ui_.requestRepaint();
    std::weak_ptr<bool> alive = alive_;
    pool_.addJob([this, alive, snapshot, generation, offline, label, operation = std::move(worker)] {
        auto result = std::make_shared<MediaEditResult>();
        try { *result = operation(*snapshot); }
        catch (const std::exception& error) { result->error = error.what(); }
        catch (...) { result->error = "The audio operation failed"; }
        juce::MessageManager::callAsync([this, alive, result, generation, offline, label] {
            if (alive.expired()) return;
            exportPoller_.stopTimer();
            if (offline) engine_.endOffline();
            exporting_ = false;
            const bool cancelled = exportCancel_.load();
            if (generation != documentGeneration_ || cancelled)
            { ui_.exportOpen = false; ui_.statusText = cancelled ? "Cancelled " + label : "The document changed"; }
            else if (! result->error.empty() && result->offer)
            { ui_.exportOpen = false; ui_.statusText = result->error; result->offer(); }
            else if (! result->error.empty() || ! result->edit.apply)
            {
                ui_.exportState.phase = ui::ExportState::Phase::Failed;
                ui_.exportState.message = result->error.empty() ? "No content to process" : result->error;
                ui_.statusText = ui_.exportState.message;
            }
            else
            {
                ui_.exportOpen = false;
                const auto applied = applyEdit(std::move(result->edit));
                if (applied.ok)
                {
                    if (result->selectTrack >= 0) ui_.selectTrack(result->selectTrack, false, false);
                    ui_.selectedClipTrack = ui_.selectedClipUid = ui_.selectedClipScene = -1;
                    ui_.statusText = label + " complete";
                }
            }
            if (ui_.modelChanged) ui_.modelChanged();
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
}

void AppController::consolidateSelection()
{
    if (! song_ || ui_.clipEditorFocused || ui_.mainView != ui::MainView::Arrangement) return;
    auto range = ui_.arrangementSelection;
    if (! range.active || range.end <= range.start)
    {
        const auto* clip = ui_.selectedClip();
        if (! clip || ui_.selectedClipScene >= 0 || ui_.selectedTakeLane >= 0) return;
        range = {true, clip->currentStart, clip->currentEnd, ui_.selectedClipTrack, ui_.selectedClipTrack};
    }
    if (range.firstTrack < 0 || range.lastTrack >= (int) song_->tracks.size()) return;
    for (int index = range.firstTrack; index <= range.lastTrack; ++index)
        if (song_->tracks[(size_t) index].frozen) { ui_.statusText = "Unfreeze the selected tracks before consolidating"; return; }
    runMediaEdit("Consolidate", false, [this, range](const model::Song& song) {
        MediaEditResult result; std::vector<model::EditCommand> commands;
        const auto folder = processedFolder(song, "Consolidate");
        for (int index = range.firstTrack; index <= range.lastTrack; ++index)
        {
            const auto& track = song.tracks[(size_t) index];
            const bool has = std::any_of(track.arrangementClips.begin(), track.arrangementClips.end(), [&](const auto& c) {
                return c.currentStart < range.end && c.currentEnd > range.start;
            });
            if (! has || track.kind == model::TrackKind::Group) continue;
            model::Clip clip;
            if (track.kind == model::TrackKind::Midi) clip = model::consolidatedMidi(song, index, range.start, range.end);
            else
            {
                auto rendered = engine::renderClipRange(song, track.arrangementClips, range.start, range.end, engine_.sampleRate(), engine_.sampleCache(), exportCancel_);
                if (! rendered.sample) { result.error = rendered.error; folder.deleteRecursively(); return result; }
                const auto file = folder.getChildFile(juce::String(index) + ".wav");
                result.error = engine::writeCroppedWav(rendered.sample->audio, rendered.sample->sampleRate, 0, rendered.sample->audio.getNumSamples(), file);
                if (! result.error.empty()) { folder.deleteRecursively(); return result; }
                clip = renderedClip(song, file, rendered.sample->sampleRate, rendered.sample->audio.getNumSamples(), track.colorIndex, range.start);
                clip.name = track.name;
                clip.envelopes = model::consolidatedEnvelopes(song, index, range.start, range.end);
                clip.warped = true; clip.warpMode = model::WarpMode::Complex;
                clip.warpMarkers = {{0, 0}, {(double) rendered.sample->audio.getNumSamples() / rendered.sample->sampleRate, range.end - range.start}};
                clip.loop = {0, range.end - range.start, 0, false};
            }
            commands.push_back(model::replaceArrangementRange(song, index, range.start, range.end, std::move(clip)));
            exportProgress_.store((float) (index - range.firstTrack + 1) / (float) (range.lastTrack - range.firstTrack + 1));
        }
        if (commands.empty()) result.error = "Select clips or a time range containing clips";
        else result.edit = model::compose("Consolidate", std::move(commands));
        return result;
    });
}

void AppController::renderTrack(int index, bool freeze, bool toNewTrack)
{
    if (! song_ || index < 0 || index >= (int) song_->tracks.size() || exporting_ || ui_.recording || ui_.recordingRequested) return;
    const auto& source = song_->tracks[(size_t) index];
    if (freeze && source.frozen)
    {
        auto restored = source; restored.frozen = false; restored.frozenArrangement.clear(); restored.frozenSlots.clear();
        applyEdit(model::replaceTrack(*song_, index, std::move(restored), "Unfreeze Track")); return;
    }
    if (freeze && source.kind == model::TrackKind::Group) return;
    const std::string label = freeze ? "Freeze Track" : toNewTrack ? "Bounce to New Track" : "Bounce Track in Place";
    // Bounce to New Track with an Arrangement time selection on this track (e.g. part of a clip) renders only that range,
    // at its place, like Live; without one it renders the track's whole span and its Session clips.
    const auto selection = toNewTrack ? ui_.bounceSelection(index) : std::nullopt;
    runMediaEdit(label, true, [this, index, freeze, toNewTrack, label, selection](const model::Song& song) {
        MediaEditResult result;
        const auto& original = song.tracks[(size_t) index];
        const auto folder = processedFolder(song, freeze ? "Freeze" : "Bounce");
        std::vector<model::Clip> arrangement;
        std::vector<model::ClipSlot> slots(song.scenes.size());
        auto render = [&](double start, double end, int scene) -> std::optional<model::Clip> {
            engine::ExportOptions options; options.target = engine::ExportTarget::SingleTrack; options.trackIndex = index;
            options.preMixer = true; options.sessionScene = scene; options.startBeat = start; options.endBeat = end;
            options.bitDepth = 32; options.dither = false;
            options.autoTail = scene < 0 && ! selection; // a selection renders exactly its range
            options.destination = folder.getChildFile(scene < 0 ? "Arrangement.wav" : "Scene " + juce::String(scene + 1) + ".wav");
            if (! folder.createDirectory()) { result.error = "Cannot create rendered audio folder"; return {}; }
            engine::Exporter exporter(engine_, song);
            const auto exported = exporter.run(options, exportProgress_, exportCancel_);
            if (! exported.ok) { result.error = exported.error; return {}; }
            auto clip = renderedClip(song, options.destination, engine_.sampleRate(), (int64_t) std::llround(exported.seconds * engine_.sampleRate()), original.colorIndex, start);
            clip.name = original.name;
            clip.warped = true; clip.warpMode = model::WarpMode::Complex; clip.warpMarkers = exported.warpMarkers;
            std::vector<model::WarpMarker> inverse;
            for (const auto& marker : clip.warpMarkers) inverse.push_back({marker.beats, marker.seconds});
            const double beats = engine::warpBeatsToSeconds(inverse, exported.seconds, song.tempo / 60);
            clip.currentEnd = clip.currentStart + beats; clip.loop = {0, beats, 0, false};
            auto envelopeSource = song;
            if (scene >= 0 && (size_t) scene < original.slots.size() && original.slots[(size_t) scene].clip)
            {
                auto sourceClip = model::movedClip(*original.slots[(size_t) scene].clip, 0);
                sourceClip.currentEnd = end;
                envelopeSource.tracks[(size_t) index].arrangementClips = {std::move(sourceClip)};
            }
            clip.envelopes = model::consolidatedEnvelopes(envelopeSource, index, start, end);
            const auto& mixer = original.mixer;
            clip.envelopes.erase(std::remove_if(clip.envelopes.begin(), clip.envelopes.end(), [&](const auto& envelope) {
                const int id = envelope.targetId;
                return id < 0 || (id != mixer.volumeTargetId && id != mixer.panTargetId && id != mixer.speakerTargetId
                    && std::find(mixer.sendTargetIds.begin(), mixer.sendTargetIds.end(), id) == mixer.sendTargetIds.end());
            }), clip.envelopes.end());
            return clip;
        };
        if (! engine_.sampleCache().waitForLoads(30000)) { result.error = "Wait for the samples to finish loading"; return result; }
        std::vector<int> members;
        // A group bounce includes every nested child; a track bounce retains its own arrangement span.
        for (const auto& track : song.tracks)
        {
            bool included = track.id == original.id;
            auto parent = track.groupId;
            for (size_t depth = 0; ! included && parent >= 0 && depth < song.tracks.size(); ++depth)
            {
                included = parent == original.id;
                const auto found = std::find_if(song.tracks.begin(), song.tracks.end(), [parent](const auto& t) { return t.id == parent; });
                parent = found == song.tracks.end() ? -1 : found->groupId;
            }
            if (included)
            {
                members.push_back((int) (&track - song.tracks.data()));
            }
        }
        if (const auto span = selection ? selection : model::arrangementClipSpan(song, members))
        {
            auto clip = render(span->first, span->second, -1); if (! clip) { folder.deleteRecursively(); return result; }
            arrangement.push_back(std::move(*clip));
        }
        for (size_t scene = 0; scene < slots.size() && ! selection; ++scene)
        {
            const model::Clip* sourceClip = nullptr;
            double length = 0;
            for (int member : members)
            {
                const auto& track = song.tracks[(size_t) member];
                if (scene >= track.slots.size() || ! track.slots[scene].clip) continue;
                const auto& candidate = *track.slots[scene].clip;
                const double candidateLength = candidate.warped || candidate.kind == model::Clip::Kind::Midi
                    ? candidate.loop.loopEnd - candidate.loop.loopStart
                    : (candidate.loop.loopEnd - candidate.loop.loopStart) * song.tempo / 60;
                if (! sourceClip || candidateLength > length) { sourceClip = &candidate; length = candidateLength; }
            }
            if (! sourceClip) continue;
            auto clip = render(0, std::max(0.0625, length), (int) scene);
            if (! clip) { folder.deleteRecursively(); return result; }
            clip->loop.enabled = sourceClip->loop.enabled; clip->name = sourceClip->name;
            clip->launchMode = sourceClip->launchMode; clip->launchQuantisation = sourceClip->launchQuantisation;
            clip->legato = sourceClip->legato;
            slots[scene].clip = std::move(*clip);
        }
        if (arrangement.empty() && std::none_of(slots.begin(), slots.end(), [](const auto& slot) { return slot.clip.has_value(); }))
        { result.error = "This track has no clips to render"; folder.deleteRecursively(); return result; }
        if (freeze)
        {
            auto frozen = original; frozen.frozen = true;
            frozen.frozenArrangement = std::move(arrangement); frozen.frozenSlots = std::move(slots);
            result.edit = model::replaceTrack(song, index, std::move(frozen), label);
        }
        else
        {
            int pointeeEnd = 0;
            auto track = io::createAudioTrack(song, original.name, original.colorIndex, toNewTrack ? -1 : original.id, -1, pointeeEnd);
            track.groupId = original.groupId; track.outputRouting = original.outputRouting; track.outputRoutingEdited = true;
            const auto freshMixer = track.mixer;
            track.mixer = original.mixer; track.trackDelayMs = original.trackDelayMs;
            if (toNewTrack)
            {
                track.mixer.volumeTargetId = freshMixer.volumeTargetId; track.mixer.panTargetId = freshMixer.panTargetId;
                track.mixer.speakerTargetId = freshMixer.speakerTargetId; track.mixer.sendTargetIds = freshMixer.sendTargetIds;
                track.mixer.sendSourceXml = freshMixer.sendSourceXml;
            }
            track.arrangementClips = std::move(arrangement); track.slots = std::move(slots);
            // Only mixer/send automation remains live; all device and clip processing is baked.
            auto playback = model::frozenPlaybackTrack([&] { auto t = original; t.frozen = true; return t; }());
            track.automation = std::move(playback.automation);
            auto remap = [&](std::vector<model::AutomationEnvelope>& envelopes) {
                for (auto& envelope : envelopes)
                {
                    if (envelope.targetId == original.mixer.volumeTargetId) envelope.targetId = freshMixer.volumeTargetId;
                    else if (envelope.targetId == original.mixer.panTargetId) envelope.targetId = freshMixer.panTargetId;
                    else if (envelope.targetId == original.mixer.speakerTargetId) envelope.targetId = freshMixer.speakerTargetId;
                    else for (size_t send = 0; send < original.mixer.sendTargetIds.size() && send < freshMixer.sendTargetIds.size(); ++send)
                        if (envelope.targetId == original.mixer.sendTargetIds[send]) { envelope.targetId = freshMixer.sendTargetIds[send]; break; }
                }
            };
            if (toNewTrack)
            {
                remap(track.automation);
                for (auto& clip : track.arrangementClips) remap(clip.envelopes);
                for (auto& slot : track.slots) if (slot.clip) remap(slot.clip->envelopes);
            }
            if (toNewTrack)
            {
                // Parallel bounce starts muted to avoid doubling the source; the original is untouched.
                track.name += " (Bounced)"; track.mixer.speakerOn = false;
                track.automation.erase(std::remove_if(track.automation.begin(), track.automation.end(), [&](const auto& e) { return e.targetId == track.mixer.speakerTargetId; }), track.automation.end());
                int before = index + 1;
                if (original.kind == model::TrackKind::Group && ! members.empty()) before = *std::max_element(members.begin(), members.end()) + 1;
                result.edit = model::insertTracks({std::move(track)}, pointeeEnd, label, before); result.selectTrack = before;
            }
            else
            {
                std::vector<model::EditCommand> edits;
                if (original.kind == model::TrackKind::Group)
                {
                    // Remove the group subtree, then insert the baked audio at the original position.
                    auto staged = song;
                    auto remove = model::deleteTracks(staged, {index}); remove.apply(staged); edits.push_back(std::move(remove));
                    edits.push_back(model::insertTracks({std::move(track)}, pointeeEnd, label, index));
                }
                else edits.push_back(model::replaceTrack(song, index, std::move(track), label, pointeeEnd));
                result.edit = model::compose(label, std::move(edits));
            }
        }
        return result;
    });
}

void AppController::convertAudioToMidi(const model::ClipLocation& location, int modeValue)
{
    if (! song_ || modeValue < 0 || modeValue > 3) return;
    const auto* selected = model::findClip(*song_, location);
    if (! selected || selected->kind != model::Clip::Kind::Audio) return;
    const auto clip = *selected;
    const auto mode = static_cast<engine::AudioToMidiMode>(modeValue);
    const std::string name = modeValue == 0 ? "Melody" : modeValue == 1 ? "Harmony" : modeValue == 2 ? "Drums" : "Slices";
    runMediaEdit("Convert " + name + " to MIDI", false, [this, location, clip, mode, name](const model::Song& song) {
        MediaEditResult result;
        auto rendered = engine::renderClipRange(song, {clip}, clip.currentStart, clip.currentEnd, engine_.sampleRate(), engine_.sampleCache(), exportCancel_);
        if (! rendered.sample) { result.error = rendered.error; return result; }
        const auto analysis = engine::audioToMidi(rendered.sample->audio, rendered.sample->sampleRate, song.tempo, mode, exportCancel_, exportProgress_);
        if (analysis.cancelled) { result.error = "Cancelled"; return result; }
        if (analysis.notes.empty()) { result.error = "No notes detected. Try an isolated, clearly pitched or percussive clip"; return result; }
        int pointeeEnd = 0;
        auto track = io::createMidiTrack(song, clip.name + " - " + name, clip.colorIndex, pointeeEnd);
        const auto& source = song.tracks[(size_t) location.track];
        track.groupId = source.groupId; track.outputRouting = source.groupId >= 0 ? "AudioOut/GroupTrack" : "AudioOut/Main"; track.outputRoutingEdited = true;
        model::Clip midi;
        model::makeEmptyMidiClip(song, location.kind == model::ClipLocation::Kind::Arrangement ? clip.currentStart : 0,
                                 clip.currentEnd - clip.currentStart, midi);
        midi.uid = model::allocateClipUid(); midi.notes = analysis.notes; midi.name = clip.name; midi.colorIndex = clip.colorIndex;
        midi.loop.enabled = clip.loop.enabled;
        if (location.kind == model::ClipLocation::Kind::Session) track.slots[(size_t) location.scene].clip = midi;
        else track.arrangementClips.push_back(midi);
        model::Device instrument; instrument.id = song.nextDeviceId; instrument.displayName = name;
        if (mode == engine::AudioToMidiMode::Drums || mode == engine::AudioToMidiMode::Slices)
        {
            instrument.type = "DrumGroupDevice";
            const auto folder = processedFolder(song, "Audio to MIDI"); const auto file = folder.getChildFile("Slices.wav");
            result.error = engine::writeCroppedWav(rendered.sample->audio, rendered.sample->sampleRate, 0, rendered.sample->audio.getNumSamples(), file);
            if (! result.error.empty()) { folder.deleteRecursively(); return result; }
            std::set<int> notes;
            int childId = song.nextDeviceId + 1;
            for (size_t i = 0; i < analysis.notes.size(); ++i)
            {
                const int pitch = analysis.notes[i].pitch; if (! notes.insert(pitch).second) continue;
                model::Device::Chain chain; chain.receivingNote = pitch; chain.sendingNote = 60;
                chain.name = mode == engine::AudioToMidiMode::Slices ? "Slice " + std::to_string(i + 1) : pitch == 36 ? "Kick" : pitch == 38 ? "Snare" : "Hi-hat";
                model::Device sampler; sampler.type = "OriginalSimpler"; sampler.id = childId++; sampler.displayName = chain.name;
                model::SamplePart part; part.name = chain.name; part.sample.absolutePath = file.getFullPathName().toStdString();
                const auto project = juce::File(juce::String::fromUTF8(song.sourcePath.c_str())).getParentDirectory();
                part.sample.relativePath = file.getRelativePathFrom(project).replaceCharacter('\\', '/').toStdString();
                part.sample.relativePathType = 3; part.sample.sourceDirectory = project.getFullPathName().toStdString();
                part.sampleRate = rendered.sample->sampleRate; part.lengthSamples = rendered.sample->audio.getNumSamples();
                part.sampleStart = (int64_t) std::round(analysis.slices[i] * part.sampleRate);
                part.sampleEnd = i + 1 < analysis.slices.size() ? (int64_t) std::round(analysis.slices[i + 1] * part.sampleRate) : part.lengthSamples;
                sampler.sampleParts.push_back(part); sampler.plain["Globals/PlaybackMode"] = 1;
                sampler.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(sampler));
                chain.devices.push_back(std::move(sampler)); instrument.chains.push_back(std::move(chain));
            }
        }
        else instrument.type = "SonoraFm";
        instrument.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(instrument));
        track.devices.push_back(std::move(instrument));
        result.edit = model::insertTracks({std::move(track)}, pointeeEnd, "Convert " + name + " to MIDI", location.track + 1);
        result.selectTrack = location.track + 1;
        return result;
    });
}

void AppController::stripSilence(const model::ClipLocation& location)
{
    if (! song_ || location.kind != model::ClipLocation::Kind::Arrangement) return;
    const auto* selected = model::findClip(*song_, location);
    if (! selected || selected->kind != model::Clip::Kind::Audio) return;
    if (const auto* track = model::trackAt(*song_, location.track); ! track || track->frozen)
    { ui_.statusText = "Unfreeze the track before stripping silence"; return; }
    const auto clip = *selected;
    runMediaEdit("Strip Silence", false, [this, location, clip](const model::Song& song) {
        MediaEditResult result;
        auto rendered = engine::renderClipRange(song, {clip}, clip.currentStart, clip.currentEnd, engine_.sampleRate(), engine_.sampleCache(), exportCancel_);
        if (! rendered.sample) { result.error = rendered.error; return result; }
        const double rate = rendered.sample->sampleRate, beatsPerSample = song.tempo / 60.0 / rate;
        std::vector<std::pair<model::Beats, model::Beats>> keep;
        for (const auto& [first, end] : engine::findSoundRegions(rendered.sample->audio, rate))
            keep.push_back({clip.currentStart + first * beatsPerSample, clip.currentStart + end * beatsPerSample});
        result.edit = model::stripArrangementClip(song, location.track, location.uid, keep);
        if (! result.edit.apply) result.error = "No silence below -40 dB to strip";
        return result;
    });
}

void AppController::jumpToTransient(int direction)
{
    const int track = ui_.selectedTrack;
    if (! song_ || track < 0 || track >= (int) song_->tracks.size() || direction == 0) return;
    const double cursor = ui_.arrangementCursor >= 0 ? ui_.arrangementCursor : engine_.transport().positionBeats();
    const double rate = engine_.sampleRate() > 0 ? engine_.sampleRate() : 44100.0;
    const std::atomic<bool> never{false};
    std::vector<const model::Clip*> clips;
    for (const auto& clip : song_->tracks[(size_t) track].arrangementClips)
        if (clip.kind == model::Clip::Kind::Audio && ! clip.disabled
            && (direction > 0 ? clip.currentEnd > cursor + 1e-6 : clip.currentStart < cursor - 1e-6)) clips.push_back(&clip);
    std::sort(clips.begin(), clips.end(), [direction](const auto* a, const auto* b) {
        return direction > 0 ? a->currentStart < b->currentStart : a->currentEnd > b->currentEnd;
    });
    for (const auto* clip : clips)
    {
        auto& cached = transientBeats_[clip->uid];
        if (cached.first != history_.version() || cached.second.empty())
        {
            cached = {history_.version(), {}};
            auto rendered = engine::renderClipRange(*song_, {*clip}, clip->currentStart, clip->currentEnd, rate, engine_.sampleCache(), never);
            if (! rendered.sample) continue;
            for (double seconds : engine::detectTransients(rendered.sample->audio, rate))
                cached.second.push_back(clip->currentStart + seconds * song_->tempo / 60.0);
            cached.second.push_back(clip->currentEnd);
        }
        const double next = engine::stepToMarker(cached.second, cursor, direction);
        if (std::abs(next - cursor) < 1e-9) continue;
        ui_.arrangementCursor = next;
        ui_.arrangementSelection.active = false;
        ui_.statusText = "Transient at " + ui::audioedit::formatBeats(next, song_->timeSigNumerator, true);
        if (ui_.requestRepaint) ui_.requestRepaint();
        return;
    }
    ui_.statusText = direction > 0 ? "No later transient on this track" : "No earlier transient on this track";
    if (ui_.requestRepaint) ui_.requestRepaint();
}
std::vector<int> AppController::audioCaptureTargets() const
{
    std::vector<int> armed, selected;
    const auto controls = engine_.controls();
    if (! song_) return armed;
    auto capturable = [this](const model::Track& track) {
        if (track.kind != model::TrackKind::Audio || track.frozen || track.deactivated) return false;
        const auto input = model::parseInput(track);
        return (input.kind == model::InputKind::AudioMono || input.kind == model::InputKind::AudioStereo)
            && engine_.audioCapture().keeps(input.channel);
    };
    for (int i = 0; i < (int) song_->tracks.size(); ++i)
        if (capturable(song_->tracks[(size_t) i]))
        {
            if (controls && (*controls)[(size_t) i].armed.load()) armed.push_back(i);
            else if (i == ui_.selectedTrack) selected.push_back(i);
        }
    return armed.empty() ? selected : armed;
}

// Capture Audio: the latest passage heard on each armed audio track's input (the selected audio track when none is armed)
// becomes a clip in its first empty Session slot, or in the Arrangement where it was played when the transport is running.
void AppController::captureAudio()
{
    if (! song_ || editingDevices_ || syncingModel_ || ui_.recording || ui_.recordingRequested || exporting_) return;
    const auto targets = audioCaptureTargets();
    if (targets.empty())
    {
        ui_.statusText = "Arm an audio track with an enabled input and play something, then press Capture Audio";
        return;
    }
    const double now = engine_.transport().positionBeats();
    const bool playing = engine_.transport().isPlaying();
    const double beatsPerBar = song_->timeSigNumerator * 4.0 / std::max(1, song_->timeSigDenominator);
    const auto folder = juce::File(juce::String::fromUTF8(song_->sourcePath.c_str())).getParentDirectory().getChildFile("Samples/Recorded");
    int created = 0;
    std::string where;
    for (int track : targets)
    {
        const auto input = model::parseInput(song_->tracks[(size_t) track]);
        juce::AudioBuffer<float> kept; double rate = 0;
        if (! engine_.audioCapture().read(input.channel, input.kind == model::InputKind::AudioStereo, kept, rate)) continue;
        const auto [first, end] = engine::latestPassage(kept, rate);
        if (end <= first) continue;
        const int channels = input.kind == model::InputKind::AudioStereo ? 2 : 1;
        juce::AudioBuffer<float> passage(channels, end - first);
        for (int ch = 0; ch < channels; ++ch) passage.copyFrom(ch, 0, kept, ch, first, end - first);
        const auto name = song_->tracks[(size_t) track].name + " Capture " + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S").toStdString();
        const auto file = folder.getNonexistentChildFile(juce::File::createLegalFileName(juce::String::fromUTF8(name.c_str())), ".wav");
        if (! folder.createDirectory() || ! engine::writeCroppedWav(passage, rate, 0, passage.getNumSamples(), file).empty())
        { ui_.statusText = "Capture Audio could not write " + file.getFullPathName().toStdString(); return; }
        auto& slots = song_->tracks[(size_t) track].slots;
        int scene = 0;
        while ((size_t) scene < slots.size() && slots[(size_t) scene].clip) ++scene;
        const bool inSession = (size_t) scene < slots.size();
        const double secondsAgo = (double) (kept.getNumSamples() - first) / rate;
        const double played = now - secondsAgo * song_->tempo / 60.0;
        const double at = inSession ? 0.0 : playing && played >= 0 ? played : std::floor(now / beatsPerBar) * beatsPerBar;
        auto clip = renderedClip(*song_, file, rate, passage.getNumSamples(), song_->tracks[(size_t) track].colorIndex, at);
        clip.name = song_->tracks[(size_t) track].name + " Capture";
        if (inSession) applyEdit(model::setSessionClip(*song_, track, scene, std::move(clip)));
        else applyEdit(model::insertClip(track, std::move(clip), "Capture Audio"));
        ++created;
        where = song_->tracks[(size_t) track].name + (inSession ? " (scene " + std::to_string(scene + 1) + ")" : " (Arrangement)");
    }
    ui_.statusText = created == 0 ? "Nothing to capture: no sound above -50 dB on the armed inputs in the last minute"
                   : "Captured audio on " + (created == 1 ? where : std::to_string(created) + " tracks");
    if (ui_.modelChanged) ui_.modelChanged();
}

}
