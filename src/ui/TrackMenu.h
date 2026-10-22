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
std::vector<MenuAction> clipContextActions(UiContext&, model::ClipLocation);
void showControlContextMenu(Widget&, UiContext&, std::function<void()> reset = {});
void showTrackContextMenu(UiContext& ctx);
void showSessionSlotContextMenu(UiContext&, int track, int scene);
void showClipContextMenu(UiContext&, model::ClipLocation);

} // namespace sonora::ui
