#pragma once
// The device title's context menu (right-click on a device, or its menu circle): the device commands and, for Serum 2
// and LFOTool instances (installed or not), PLG-16f's "Replace with Sonora Prism / Curve LFO" for this instance or for
// every instance in the set. The swap is plugins::NativeReplacement's, the same as the prompt after a load.

#include "ui/GridMenu.h"

#include <string>
#include <vector>

namespace juce { class Image; }

namespace sonora::plugins { struct NativeReplacement; }

namespace sonora::ui
{
struct UiContext;

// `path`: top-level device index, then (chain, device) pairs into racks (plugins::NativeReplacement::path).
std::vector<MenuAction> deviceContextActions(UiContext&, int track, const std::vector<int>& path);
void showDeviceContextMenu(UiContext&, int track, const std::vector<int>& path);
juce::Image renderDeviceMenuPreview(UiContext&, int track, const std::vector<int>& path, float scale = 2);

// Swaps the instance at `path` (or, with `all`, every instance of the same plug-in in the set) for its Sonora device:
// the running plug-ins' states are captured first (UiContext::capturePluginStates) so edits since the load carry over,
// then one undoable edit. Returns how many were replaced; the notes go to the load notes and the status line.
int replaceWithNativeDevices(UiContext&, int track, const std::vector<int>& path, bool all);
// Puts a swap's notes in the load notes and its summary in the status line (the menu and the prompt after a load).
void reportNativeReplacements(UiContext&, const std::vector<plugins::NativeReplacement>& replacements);

} // namespace sonora::ui
