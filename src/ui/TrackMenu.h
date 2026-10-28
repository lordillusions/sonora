#pragma once
// Context commands shared by Session and Arrangement.

#include "ui/GridMenu.h"
#include "core/Edit.h"

namespace juce { class PopupMenu; class Image; }

namespace sonora::ui
{
struct UiContext;
class Widget;

// Lists what applies to the current track selection and runs the picked command through the UiContext callbacks.
std::vector<MenuAction> trackContextActions(UiContext&);
juce::PopupMenu createTrackContextMenu(UiContext&);
juce::Image renderTrackMenuPreview(UiContext&, float scale = 2);
void insertEmptyTakeLane(UiContext&);
void deleteTrackSelection(UiContext&);
void splitArrangementSelection(UiContext&);
void toggleTrackSolo(UiContext&, int track, bool additive = false);
// TRK-1: hide/show or deactivate/activate the selected tracks and returns; the selected track decides the direction.
void toggleSelectedTracksHidden(UiContext&);
void toggleSelectedTracksDeactivated(UiContext&);
// True when `index` (a control index) is hidden, directly or through a hidden group, and hidden tracks are not shown.
bool trackHiddenInViews(const UiContext&, int index);
// ENG-7: true when track `index` (a control index, tracks only) is silent in the mix — deactivated, switched off and not
// soloed, or left out by another track's solo — the same rule as engine::isTrackAudible. A switched-on activator of a
// track left out by a solo draws unlit (white), as in the reference.
bool trackSilencedInMix(const UiContext&, int index);
std::vector<MenuAction> clipContextActions(UiContext&, model::ClipLocation);
void showControlContextMenu(Widget&, UiContext&, std::function<void()> reset = {});
void showTrackContextMenu(UiContext& ctx);
void showSessionSlotContextMenu(UiContext&, int track, int scene);
void showClipContextMenu(UiContext&, model::ClipLocation);
// Adds the 14x5 palette grid the track menu uses (`selected` ticked, -1 for none) as a custom item of `menu`.
void addColorGrid(juce::PopupMenu& menu, int selected, float zoom, std::function<void(int)> pick, const char* title);

} // namespace sonora::ui
