#pragma once
// UI-58: in-place text editing. Cmd+R (Edit > Rename, the track context menu's Rename) turns the
// track's name, where it is already written, into a text field: the whole name starts selected so typing replaces it,
// Return commits, Escape cancels, a click elsewhere commits. The root routes every key to it while it is open.

#include "ui/Rect.h"

#include "include/core/SkColor.h"

#include <cstddef>
#include <string>

class SkCanvas;

namespace sonora::ui
{
struct KeyEvent;
struct UiContext;

struct InlineTextEdit
{
    enum class Result
    {
        Edited,  // text, caret or selection changed (or the key was swallowed)
        Commit,  // Return / Enter
        Cancel   // Escape
    };

    std::string text;
    size_t caret = 0;        // byte offset, always on a code point boundary
    bool allSelected = true; // the whole text is selected: typing or Backspace replaces it

    void start(std::string initial);
    // Cmd+A select all, Cmd+C copy, Cmd+X cut, Cmd+V paste, Left/Right/Home/End, Backspace/Delete, typed characters.
    // Every other key is swallowed: shortcuts and the computer MIDI keyboard never fire while a field is open.
    Result key(const KeyEvent&);
    void insert(const std::string& s);

    // Draws the field over `r`: text-field background and frame, the selected text highlighted, or the text with a
    // caret (scrolled so the caret stays visible).
    void paint(SkCanvas&, const Rect& r, SkColor back, SkColor ink, SkColor selectionBack, SkColor selectionInk, float fontSize,
               bool bold = false) const;
};

// The track whose name is being edited in place (Arrangement header and Session title draw the field).
struct TrackRename
{
    int trackId = -1; // -1: not editing
    InlineTextEdit edit;
    Rect bounds;      // where the field was last drawn, root coordinates: a click elsewhere commits

    bool active() const { return trackId >= 0; }
};

// Opens the field on the track at control index `index` (not Main). False when it cannot be renamed.
bool beginTrackRename(UiContext&, int index);
// The root's key handler while a rename is open. Always consumes the key.
bool trackRenameKey(UiContext&, const KeyEvent&);
// Applies the typed name (an empty or unchanged name leaves the track as it was) and closes the field.
void commitTrackRename(UiContext&);
void cancelTrackRename(UiContext&);
// The text a rename starts with: the stored name, or "#" + the automatic suffix for automatically numbered tracks.
std::string editableTrackName(const UiContext&, int index);

} // namespace sonora::ui
