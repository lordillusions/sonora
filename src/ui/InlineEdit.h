#pragma once
// UI-58: in-place text editing. Cmd+R (Edit > Rename, the track context menu's Rename) turns the
// track's name, where it is already written, into a text field: the whole name starts selected so typing replaces it,
// Return commits, Escape cancels, a click elsewhere commits. The root routes every key to it while it is open.

#include "ui/Rect.h"

#include "include/core/SkColor.h"

#include <algorithm>
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
    size_t caret = 0;  // byte offset, always on a code point boundary
    size_t anchor = 0; // the other end of the selection (== caret: nothing selected). UI-78.

    void start(std::string initial); // the whole text starts selected: typing or Backspace replaces it
    bool hasSelection() const { return anchor != caret; }
    bool allSelected() const { return ! text.empty() && selectionStart() == 0 && selectionEnd() == text.size(); }
    size_t selectionStart() const { return std::min(anchor, caret); }
    size_t selectionEnd() const { return std::max(anchor, caret); }
    void selectAll() { anchor = 0; caret = text.size(); }
    void clear() { text.clear(); caret = anchor = 0; }
    // Cmd+A select all, Cmd+C copy, Cmd+X cut, Cmd+V paste, Left/Right/Home/End (Shift extends the selection, Alt moves
    // by word, Cmd to the ends), Backspace/Delete (Alt: word, Cmd: to the start), typed characters replace the selection.
    // Every other key is swallowed: shortcuts and the computer MIDI keyboard never fire while a field is open.
    Result key(const KeyEvent&);
    void insert(const std::string& s); // replaces the selection

    // Mouse (UI-78): `offsetAt` maps a point to a text offset for a field drawn over `r` by paint/paintText;
    // `click` places the caret (Shift extends), a double click selects the word, a triple click everything;
    // `dragTo` extends the selection while the button is held.
    size_t offsetAt(float x, const Rect& r, float fontSize, bool bold = false) const;
    void click(size_t offset, int clicks, bool shift);
    void dragTo(size_t offset) { caret = std::min(offset, text.size()); }

    // Draws the field over `r`: text-field background and frame, the selection highlighted and the caret (scrolled so the
    // caret stays visible).
    void paint(SkCanvas&, const Rect& r, SkColor back, SkColor ink, SkColor selectionBack, SkColor selectionInk, float fontSize,
               bool bold = false) const;
    // The same without background and frame; `caretShown` false hides the caret (blinking).
    void paintText(SkCanvas&, const Rect& r, SkColor ink, SkColor selectionBack, SkColor selectionInk, float fontSize, bool bold,
                   bool caretShown) const;
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
