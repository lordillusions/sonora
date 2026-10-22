#include "engine/ResynthEdits.h"
#include "engine/ResynthAnalysis.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsNewSet.h"
#include <cmath>

namespace sonora::engine
{
model::EditCommand resynthTrack(const model::Song& song, const model::ResynthModel& analysis, model::Device instrument,
                               int afterTrack, double start, int scene)
{
    std::string validation;
    if (! model::validateResynth(analysis, validation) || instrument.type != "SonoraResynth"
        || ! std::isfinite(start) || start < 0 || scene < -1 || afterTrack < 0 || (size_t) afterTrack >= song.tracks.size()
        || (scene >= 0 && (size_t) scene >= song.scenes.size())) return {};
    int pointeeEnd = 0;
    const auto* source = model::trackAt(song, afterTrack);
    if (! source) return {};
    auto track = io::createMidiTrack(song, analysis.name + " - Resynth", source->colorIndex, pointeeEnd);
    track.groupId = source->groupId;
    track.outputRouting = source->groupId >= 0 ? "AudioOut/GroupTrack" : "AudioOut/Main"; track.outputRoutingEdited = true;
    auto allocator = song; allocator.nextPointeeId = std::max(allocator.nextPointeeId, pointeeEnd);
    if (! allocator.midiClipTemplate) allocator.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    model::Clip clip;
    const double beats = analysis.duration() * song.tempo / 60;
    if (! model::makeEmptyMidiClip(allocator, scene >= 0 ? 0 : start, beats, clip)) return {};
    clip.uid = model::allocateClipUid(); clip.notes = resynthPhrase(analysis, song.tempo / 60);
    clip.name = analysis.name; clip.colorIndex = track.colorIndex; clip.loop.enabled = false;
    // A phrase can repeat one pitch with different attacks/dynamics. CC119 chooses its exact analyzed
    // example before each note-on, at a sample offset, rather than relying on per-block parameter automation.
    constexpr int exampleController = 119 + 2;
    for (auto& id : track.midiControllerTargets) if (id < 0) id = allocator.nextPointeeId++;
    track.midiControllersEdited = true;
    model::AutomationEnvelope examples; examples.midiController = exampleController;
    examples.targetId = track.midiControllerTargets[(size_t) exampleController]; examples.points.push_back({0, 0});
    double previous = 0;
    for (size_t i = 0; i < analysis.notes.size(); ++i)
    {
        const double at = analysis.notes[i].start * song.tempo / 60;
        examples.points.push_back({at, previous}); previous = (double) (i + 1); examples.points.push_back({at, previous});
    }
    const double end = analysis.notes.back().end * song.tempo / 60;
    examples.points.push_back({end, previous}); examples.points.push_back({end, 0});
    clip.envelopes.push_back(std::move(examples));
    if (scene >= 0)
    {
        if ((size_t) scene >= track.slots.size()) track.slots.resize((size_t) scene + 1);
        track.slots[(size_t) scene].clip = clip;
    }
    else track.arrangementClips.push_back(std::move(clip));
    for (auto& parameter : instrument.parameters)
    {
        if (parameter.path == "PitchMotion" || parameter.path == "Sustain") parameter.value = 1;
    }
    instrument.id = -1; instrument.enabledTargetId = -1; instrument.sourceXml.reset();
    for (auto& parameter : instrument.parameters) { parameter.automationTargetId = -1; parameter.midi = {}; parameter.macroIndex = -1; }
    model::allocateDeviceIds(allocator, instrument);
    instrument.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(instrument));
    track.devices.push_back(std::move(instrument));
    return model::insertTracks({std::move(track)}, allocator.nextPointeeId, "Resynthesize to New MIDI Track", afterTrack + 1);
}
}
