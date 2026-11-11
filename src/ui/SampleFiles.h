#pragma once
// LIB-3 / LIB-4 / LIB-5: reaching and replacing the sample file of an audio clip, shared by the clip context menu
// (Arrangement and Session), the clip's sample editor (header name, context menu, drops, swap arrows) and the browser.

#include "ui/GridMenu.h"
#include "core/SampleReplace.h"

#include <string>
#include <vector>

namespace juce { class PopupMenu; }

namespace sonora::ui
{
struct UiContext;

// The clip's sample file on disk (resolved like the engine does: project-relative first), empty when it is missing.
std::string clipSamplePath(const UiContext&, const model::ClipLocation&);

// Opens the browser on `path`'s folder and selects its row (UiContext::browserReveal). False when the file is missing.
bool showInBrowser(UiContext&, const std::string& path);

// Starts Show Similar Files for `path` (the browser lists the results); `clip` is the clip the search came from, if any.
bool showSimilarFiles(UiContext&, const std::string& path, const model::ClipLocation& clip = {});

// The browser's similar list ends (also stops a running search).
void leaveSimilarFiles(UiContext&);

// Replaces the clip's sample with `path` in the remembered scope (UiContext::sampleReplaceScope).
bool replaceClipSampleWith(UiContext&, const model::ClipLocation&, const std::string& path);

// LIB-5c "Replace Sample...": opens the browser with the clip as the replace target (UiContext::sampleSwap) and lists the
// samples that sound like its file. False when the clip cannot take another sample now (MIDI, frozen, recording).
bool beginSampleSwap(UiContext&, const model::ClipLocation&);
void endSampleSwap(UiContext&);
// The replace target's clip while the bar is up, nullptr when it is not or the clip went away (the browser then ends it).
const model::Clip* sampleSwapClip(const UiContext&);

// "Show in Browser", "Show in Finder", "Show Similar Files", "Replace Sample..." for an audio clip (empty for MIDI clips).
std::vector<MenuAction> sampleFileActions(UiContext&, model::ClipLocation);

// The "Replace In" submenu: the three scopes with how many clips each covers, the remembered one ticked.
void addSampleScopeMenu(juce::PopupMenu&, UiContext&, model::ClipLocation);

// Short label of a scope for buttons: "Clip", "Track", "Set".
const char* sampleScopeShortLabel(model::SampleScope);

} // namespace sonora::ui
