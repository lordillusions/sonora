#include "ui/ClipAutomation.h"

#include "ui/UiContext.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui
{

void withClipAutomation(UiContext& ctx, int fromTrack, double start, double end, bool copy, std::function<void(bool)> place)
{
    if (! ctx.song || ctx.clipAutomationPolicy == 2 || ! model::trackAutomationMovesIn(*ctx.song, fromTrack, start, end))
    {
        place(false);
        return;
    }
    if (ctx.clipAutomationPolicy == 1 || ! ctx.askChoice)
    {
        place(true); // the reference's default (Lock Envelopes off)
        return;
    }
    const auto* song = ctx.song;
    const auto structure = ctx.trackStructure();
    auto* context = &ctx;
    ctx.askChoice(copy ? "Copy the automation too?" : "Move the automation too?",
                  std::string("The track has automation under this clip. ") + (copy ? "Copy" : "Move")
                      + " it with the clip, or leave it where it is? Options > Clip Automation sets this for every clip.",
                  {"With Automation", "Clip Only", "Always", "Cancel"}, 0, 3,
                  [context, song, structure, then = std::move(place)](int button) {
                      auto& c = *context;
                      if (c.song != song || c.trackStructure() != structure || button == 3 || button < 0) return;
                      if (button == 2)
                      {
                          c.clipAutomationPolicy = 1;
                          if (c.setClipAutomationPolicy) c.setClipAutomationPolicy(1);
                      }
                      then(button != 1);
                      if (c.requestRepaint) c.requestRepaint();
                  });
}

model::EditCommand withTrackAutomation(const model::Song& song, model::EditCommand clip, int fromTrack, double start, double end,
                                       int toTrack, double at, bool copy)
{
    auto automation = model::transferClipAutomation(song, fromTrack, start, end, toTrack, at, copy);
    if (! clip.apply || ! automation.apply) return clip;
    auto label = clip.label;
    std::vector<model::EditCommand> parts;
    parts.push_back(std::move(clip));
    parts.push_back(std::move(automation));
    return model::compose(std::move(label), std::move(parts));
}

void duplicateClipWithAutomation(UiContext& ctx, model::ClipLocation location)
{
    if (! ctx.song || ! ctx.applyEdit) return;
    const auto* clip = model::findClip(*ctx.song, location);
    if (clip == nullptr) return;
    const double start = clip->currentStart, end = clip->currentEnd;
    const int track = location.track, uid = location.uid;
    const bool arrangement = location.kind == model::ClipLocation::Kind::Arrangement;
    auto run = [&ctx, location, start, end, track, uid, arrangement](bool withAutomation) {
        if (! ctx.song || ! ctx.applyEdit || ! model::findClip(*ctx.song, location)) return;
        auto command = model::duplicateClip(*ctx.song, location);
        if (withAutomation && arrangement) command = withTrackAutomation(*ctx.song, std::move(command), track, start, end, track, end, true);
        ctx.applyEdit(std::move(command));
        if (! arrangement) return;
        // Select the copy (the clip that now starts where the original ended and is not the original).
        for (const auto& c : ctx.song->tracks[(size_t) track].arrangementClips)
            if (c.uid != uid && std::abs(c.currentStart - end) < 1e-9) { ctx.selectedClipUid = c.uid; break; }
    };
    if (! arrangement) { run(false); return; }
    withClipAutomation(ctx, track, start, end, true, run);
}

void duplicateTracksAsking(UiContext& ctx, std::function<void(bool)> duplicate)
{
    bool automated = false;
    if (ctx.song)
        for (int index : ctx.selectedTrackIndices())
            if (const auto* t = ctx.trackAt(index))
                for (const auto& lane : t->automation)
                    automated |= lane.targetId >= 0 && std::any_of(lane.points.begin(), lane.points.end(), [](const auto& p) { return p.time >= 0; });
    if (! automated || ! ctx.askChoice) { duplicate(true); return; }
    const auto* song = ctx.song;
    auto* context = &ctx;
    ctx.askChoice("Duplicate the automation too?",
                  "The selected tracks have automation. Should the copies get it as well?",
                  {"With Automation", "Without Automation", "Cancel"}, 0, 2,
                  [context, song, then = std::move(duplicate)](int button) {
                      if (context->song != song || button < 0 || button == 2) return;
                      then(button == 0);
                  });
}

} // namespace sonora::ui
