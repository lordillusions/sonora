#pragma once
// UI-3b: Arrangement locator workflows shared by the ruler, the Set / previous / next buttons, the menus and the
// keyboard. Every edit is a model::Op built into its command by model::opToCommand, the same path the control API
// (agents, MCP) uses, so both get the same validation, undo step and collab intent.

#include "core/Op.h"
#include "ui/GridMenu.h"

#include <functional>

namespace sonora::ui
{
struct UiContext;

// Applies `op` through UiContext::applyEdit. A rejected op (OpError) leaves the song as it was and puts the reason in
// the status bar. Returns true when an edit was applied.
bool applyLocatorOp(UiContext&, const model::Op&);

// End of the Arrangement (last clip end, at least the loop end): where Next Locator goes after the last locator.
double arrangementEndBeat(const model::Song&);
// The previous (direction < 0) or next locator before/after `from`, or the Arrangement start/end past the first/last.
double locatorJumpTarget(const model::Song&, double from, int direction);
// Where the Set button adds a locator: while playing, the play position rounded to the launch quantisation; while
// stopped, the start of the time selection, else the insert marker, else the play position.
double locatorSetBeat(const UiContext&);
// The Set button: deletes the selected locator ("Del"); otherwise adds one at locatorSetBeat, or selects the locator
// already there.
void setOrDeleteLocator(UiContext&);
// Moves the play position to `beat`. While playing the jump waits for the next launch quantisation boundary.
void jumpTo(UiContext&, double beat);
void jumpToAdjacentLocator(UiContext&, int direction);
// Starts playback; from stopped, at the song start locator when one is set ("Set Song Start Time Here").
void startPlayback(UiContext&);

// The locator's context menu (Rename and Edit Info Text open the ruler's text editor through the callbacks).
std::vector<MenuAction> locatorContextActions(UiContext&, int locatorId, std::function<void()> rename, std::function<void()> editInfo);
void showLocatorContextMenu(UiContext&, int locatorId, std::function<void()> rename, std::function<void()> editInfo);
// The scrub area's context menu at `beat`: Add Locator, Insert Time Signature Change (ENG-11b, not implemented).
std::vector<MenuAction> scrubAreaContextActions(UiContext&, double beat);
void showScrubAreaContextMenu(UiContext&, double beat);

} // namespace sonora::ui
