#include "ui/GeneratorClipUi.h"

#include "core/GeneratorClip.h"
#include "devices/MidiGenerator.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"
#include "ui/UiContext.h"

namespace sonora::ui
{
devices::MidiGenerator* generatorAt(const UiContext& ctx, int track, int device)
{
    if (ctx.graph == nullptr || ctx.song == nullptr || track < 0 || (size_t) track >= ctx.song->tracks.size()
        || (size_t) track >= ctx.graph->nodes.size() || device < 0)
        return nullptr;
    const auto& processors = ctx.graph->nodes[(size_t) track].processorForModelDevice;
    if ((size_t) device >= processors.size()) return nullptr;
    return dynamic_cast<devices::MidiGenerator*>(processors[(size_t) device]);
}

std::vector<int> generatorDevices(const UiContext& ctx, int track)
{
    std::vector<int> found;
    if (ctx.graph == nullptr || ctx.song == nullptr || track < 0 || (size_t) track >= ctx.song->tracks.size()
        || (size_t) track >= ctx.graph->nodes.size())
        return found;
    const auto& processors = ctx.graph->nodes[(size_t) track].processorForModelDevice;
    for (size_t i = 0; i < processors.size(); ++i)
        if (dynamic_cast<devices::MidiGenerator*>(processors[i]) != nullptr) found.push_back((int) i);
    return found;
}

bool applyGeneratorToClip(UiContext& ctx, int track, int device, std::optional<model::ClipLocation> clip)
{
    auto* generator = generatorAt(ctx, track, device);
    if (generator == nullptr || ! ctx.applyEdit || ctx.song == nullptr) return false;
    const double tempo = ctx.song->tempo;
    model::GeneratorClipSpec spec;
    spec.deviceName = generator->name();
    spec.usesInput = generator->listensToInput();
    spec.render = [generator, tempo](const std::vector<model::MidiNote>& input, double from, double to) {
        return generator->renderNotes(input, from, to, tempo);
    };
    std::string message;
    model::EditCommand command;
    if (clip)
        command = model::applyGeneratorToClip(*ctx.song, *clip, spec, message);
    else
    {
        const bool loopOn = ctx.transport != nullptr ? ctx.transport->loopEnabled() : ctx.song->loopOn;
        const double loopStart = ctx.transport != nullptr ? ctx.transport->loopStart() : ctx.song->loopStart;
        const double playhead = ctx.transport != nullptr ? ctx.transport->positionBeats() : ctx.song->currentTime;
        const double start = model::generatorClipStart(*ctx.song, playhead, loopOn, loopStart);
        command = model::insertGeneratorClip(*ctx.song, track, start, generator->clipLengthBeats(), spec, message);
    }
    ctx.statusText = message;
    if (! command.apply)
    {
        if (ctx.requestRepaint) ctx.requestRepaint();
        return false;
    }
    ctx.applyEdit(std::move(command));
    // Looked up again: applying an edit may hand the UI a new graph.
    if (auto* applied = generatorAt(ctx, track, device)) applied->setBypassed(true);
    ctx.statusText = message + "; " + spec.deviceName + " is now off so the part does not play twice";
    if (ctx.requestRepaint) ctx.requestRepaint();
    return true;
}

bool generatorToClipFromFace(UiContext& ctx, int track, int device)
{
    std::optional<model::ClipLocation> location;
    if (const auto* selected = ctx.selectedClip(); selected != nullptr && selected->kind == model::Clip::Kind::Midi
        && ctx.selectedClipTrack == track)
        location = ctx.selectedClipLocation();
    return applyGeneratorToClip(ctx, track, device, location);
}
} // namespace sonora::ui
