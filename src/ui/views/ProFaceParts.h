#pragma once
// PLG-25a: face parts of the pattern generators (Sonora Phrase, Pulse, and Seraph's gate), on the ProFace kit's design
// grid. A part is owned by its face (a member that lives as long as the face), placed with ProFace::addPart(part, rect,
// tip) inside build(), and gets the mouse in design-grid coordinates. It is drawn with the controls (in their order); an
// open overlay (the preset menu) is drawn last and takes every click until it closes.
//
// Values that are not host parameters (step grids, progressions) go through get / set callbacks: the face reads its
// PatternSlot's working copy and publishes after `set`. Every part calls `changed` (when set) after an edit.
//
//   StepLane   N cells of one per-step value. Toggle (on/off), Bar (a level from the cell's height), Choice (a label per
//              value). Click sets, drag paints across cells (a Bar follows the pointer's height, a Toggle paints the value
//              of the first cell, a Choice paints the value the first click chose), Alt-click resets a cell to its
//              default, Shift-click on a Choice steps backwards. The cell playing is outlined; cells `disabled` says are
//              hatched (Bass's kick guard) and still editable.
//   NoteRoll   Read-only mini piano roll of the generated notes: bar and beat lines, notes coloured by `colourOf`
//              (velocity as brightness by default), the play head.
//   PresetBar  Previous / next arrows, the preset's name (a click opens a two-column menu: categories, then presets) and
//              the dice. "(edited)" when the settings match no preset.
//   ChordStrip The progression as chips (name and length), widths by length; the playing chip lit, the selected one
//              outlined; drag a chip's right edge to change its length (snapped to `lengths`); "+" adds a slot.
//
// These are Sonora's own faces (no reference device), so AGENTS rule 7 does not apply.

#include "ui/Painter.h"
#include "ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

class SkCanvas;

namespace sonora::ui
{
class FacePart
{
public:
    virtual ~FacePart() = default;
    std::string name;             // the Area's label (tests find the part by it)
    Rect bounds;                  // design grid (set by ProFace::addPart)
    SkColor accent = 0xFF35C2B0;  // the device's accent
    std::function<void()> changed;

    virtual void paint(SkCanvas&) = 0;
    virtual void mouseDown(const MouseEvent&) {} // design-grid coordinates
    virtual void mouseDrag(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual bool overlayOpen() const { return false; }
    virtual void paintOverlay(SkCanvas&) {}

protected:
    void notify() { if (changed) changed(); }
};

class StepLane final : public FacePart
{
public:
    enum class Kind { Toggle, Bar, Choice };
    Kind kind = Kind::Bar;
    int steps = 16;                     // cells
    float minValue = 0, maxValue = 1;   // Bar range (Choice: 0 .. choices - 1; Toggle: 0 / 1)
    float defaultValue = 0;             // Alt-click
    bool integral = false;              // Bar: whole numbers
    bool bipolar = false;               // Bar: drawn from the middle (an octave or scale-step lane)
    std::vector<std::string> choices;   // Choice: the label of each value
    std::string label;                  // drawn in the lane's label column at the left (labelWidth design pixels)
    float labelWidth = 0;
    int beatEvery = 4;                  // a stronger separator every n cells
    std::function<float(int step)> get;
    std::function<void(int step, float value)> set;
    std::function<int()> playStep;           // the cell playing, -1 none
    std::function<bool(int step)> disabled;  // drawn hatched

    // Geometry (design grid): the cell under x (-1 outside), a cell's rectangle, the value a y gives on a Bar.
    int stepAt(float x) const;
    Rect cellRect(int step) const;
    float barValueAt(float y) const;

    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;

private:
    void apply(int step, float value);
    float clampValue(float) const;
    int lastStep_ = -1;
    float paintValue_ = 0;
    bool dragging_ = false;
};

class NoteRoll final : public FacePart
{
public:
    struct Note
    {
        double start = 0, length = 0.25; // beats from the start of the shown span
        int pitch = 60;
        float velocity = 100;
        int tint = 0;                    // free for the face (bursts, fit), passed to colourOf
    };
    std::vector<Note> notes;
    double lengthBeats = 16;             // the span shown
    double beatsPerBar = 4;
    int lowPitch = -1, highPitch = -1;   // -1: from the notes (at least an octave)
    std::function<double()> playBeat;    // position in [0, lengthBeats); < 0 hides the play head
    std::function<SkColor(const Note&)> colourOf;
    std::string emptyText = "No notes";

    // The pitch range drawn now (auto range included).
    std::pair<int, int> pitchRange() const;
    void paint(SkCanvas&) override;
};

class PresetBar final : public FacePart
{
public:
    struct Entry
    {
        std::string category, name;
    };
    std::vector<Entry> presets;
    std::function<int()> current;         // the preset the settings match, -1: "(edited)"
    std::function<void(int index)> select;
    std::function<void()> dice;            // unset: no dice button
    std::string tipName = "Preset";
    bool menuToLeft = false;
    bool showCategory = true;              // the category at the name's right (off for a narrow bar)               // the menu opens leftwards from the name's right edge (a bar near the face's edge)

    // Hit zones (design grid) and the menu's rows, for tests and painting.
    Rect previousRect() const;
    Rect nextRect() const;
    Rect nameRect() const;
    Rect diceRect() const;
    std::vector<std::string> categories() const;
    Rect menuRect() const;
    Rect categoryRow(int index) const;
    Rect presetRow(int row) const;          // row of the open category's presets
    bool menuOpen() const { return open_; }
    int openCategory() const { return category_; }

    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
    bool overlayOpen() const override { return open_; }
    void paintOverlay(SkCanvas&) override;

private:
    std::vector<int> presetsIn(int category) const;
    void step(int delta);
    bool open_ = false;
    int category_ = 0;
};

class ChordStrip final : public FacePart
{
public:
    struct Chip
    {
        std::string name;   // "Am", "F/A"
        double beats = 8;
    };
    std::function<int()> count;
    std::function<Chip(int index)> chip;
    std::function<int()> playing;              // lit, -1 none
    std::function<int()> selected;             // outlined, -1 none
    std::function<void(int index)> select;
    std::function<void(int index, double beats)> resize;
    std::function<void()> add;                 // unset: no "+" (also hidden at 16 chips)
    std::vector<double> lengths{2, 4, 8, 16, 32};
    double beatsPerBar = 4;
    int maxChips = 16;

    Rect chipRect(int index) const;
    Rect addRect() const;
    std::string lengthText(double beats) const; // "2 bars", "1/2 bar"

    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;

private:
    double pixelsPerBeat() const;
    int resizing_ = -1;
    float resizeStartX_ = 0;
    double resizeStartBeats_ = 0, resizePerBeat_ = 1;
};
} // namespace sonora::ui
