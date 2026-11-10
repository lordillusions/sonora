#pragma once
// UI-76: the track automation under an Arrangement clip that is moved, Option-copied or duplicated. The reference moves it
// with the clip unless Lock Envelopes is on; Sonora asks first (Options > Clip Automation: Ask / Always Move With Clips /
// Lock Envelopes) when the clip's time range actually holds automation.

#include "core/Edit.h"

#include <functional>

namespace sonora::ui
{
struct UiContext;

// Decides whether automation goes along and calls `place(withAutomation)`: right away when the policy or the lane content
// decides, else after the user answered (never when they cancel). `copy` only changes the question's wording.
void withClipAutomation(UiContext&, int fromTrack, double start, double end, bool copy, std::function<void(bool withAutomation)> place);
// A clip edit plus the automation under [start, end) of `fromTrack` going to `toTrack` at `at`, as one undo step labelled
// like the clip edit. `clip` alone when no automation moves.
model::EditCommand withTrackAutomation(const model::Song&, model::EditCommand clip, int fromTrack, double start, double end,
                                       int toTrack, double at, bool copy);
// Cmd+D on an Arrangement clip (or a take / Session clip, which have no track automation of their own) with the question
// above; selects the copy.
void duplicateClipWithAutomation(UiContext&, model::ClipLocation);
// Track > Duplicate: asks whether the copies keep the tracks' automation when the selection has any.
void duplicateTracksAsking(UiContext&, std::function<void(bool withAutomation)> duplicate);
} // namespace sonora::ui
