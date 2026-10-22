#pragma once
// MIDI clip editor (ROADMAP UI-10): keys on the left, beat ruler on top, notes in the middle and a
// velocity lane at the bottom. Edits go through UiContext::applyEdit as undoable `setNotes` commands
// (one per gesture), so the engine hears them immediately and the set can be saved.
//
// Mouse: click a note to select (Shift adds), drag empty space to select the notes inside the rectangle (Shift adds),
// drag its body to move (time + pitch), drag its right
// edge to resize, double-click empty space to add a note, drag in the velocity lane to set velocity.
// Keys: Delete removes, Up/Down transposes (Shift: octave), Left/Right nudges by the grid,
// Cmd+A selects all, Cmd+U quantizes the selection to the grid. Alt disables snapping.
// Cmd+wheel (or a trackpad pinch) zooms time around the pointer, Alt+wheel zooms the rows, the wheel scrolls
// vertically without limits (all 128 pitches are reachable), Shift+wheel scrolls in time. Dragging notes near an edge
// scrolls the view. Right-click opens the grid menu; Cmd+1..5 are Narrow/Widen/Triplet/Snap/Fixed grid, + and - zoom.
// Click Vel/Prob beside the note lane to switch between velocity and probability.

#include "ui/UiContext.h"
#include "ui/MidiClipEdits.h"
#include "ui/Widget.h"

#include <set>

namespace sonora::ui
{

class PianoRoll : public Widget
{
public:
    ~PianoRoll() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void paintOverChildren(SkCanvas& c, const Theme& t) override; // COL-5: others in the same clip
    void mouseMove(const MouseEvent& e) override;
    void mouseExit() override;
    void mouseDown(const MouseEvent& e) override;
    Cursor cursorAt(float x, float y) const override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool mouseMagnify(const MouseEvent& e, float factor) override;
    bool keyPressed(const KeyEvent& k) override;
    void focusLost() override;
    void tick() override;

    // Pure geometry helpers (public for tests).
    struct View
    {
        double pixelsPerBeat = 40.0;
        float rowHeight = 11.0f;
        double scrollBeats = 0.0; // content beat at the left edge of the note area
        double topPitch = 84.0;   // pitch of the row at the top edge; fractional while scrolling smoothly
    };
    View view;
    bool runMidiTool(midiedit::ToolSettings);
    enum class HeaderControl { Fold, Scale, Highlight };
    Rect headerControlBounds(HeaderControl) const;
    Rect rulerBounds() const { return rulerArea(); }
    Rect noteAreaBounds() const { return notesArea(); }
    const std::set<int>& selectedNotes() const { return selected_; }
    // Where the selected clip is playing, in its content beats (the notes' time base); -1 when it is not playing.
    double playheadBeat() const;


private:
    enum class Mode { None, Move, Resize, Velocity, Marquee, Draw };

    const model::Clip* clip() const;
    Rect keysArea() const;
    Rect rulerArea() const;
    Rect notesArea() const;
    Rect velocityArea() const;
    void audition(int pitch);
    void stopAudition();
    std::shared_ptr<engine::ControlBank> auditionBank_;
    int auditionTrack_ = -1, auditionUid_ = -1;
    uint64_t lastZoomRequest_ = 0;
    float velocityHeight() const;
    bool resizeLane_ = false;
    float grabY_ = 0, startVelocityHeight_ = 44;
    double xToBeat(float x) const { return view.scrollBeats + (x - notesArea().x) / view.pixelsPerBeat; }
    float beatToX(double beat) const { return notesArea().x + (float) ((beat - view.scrollBeats) * view.pixelsPerBeat); }
    int yToPitch(float y) const;
    float pitchToY(int pitch) const;
    int pitchRow(int pitch) const;
    void syncPitchRows();
    std::vector<int> pitches_; // ascending visible pitches; Fold to Scale also retains out-of-scale notes
    double grid() const;                    // grid size in beats at the current zoom (note length of new notes, nudges)
    double snapUnit(const MouseEvent& e) const; // 0 when snapping is off or overridden with Cmd/Alt
    double beatsPerBar() const;
    void clampVertical();                   // keep the view inside pitches 0..127
    void zoomTime(float anchorX, double factor);
    void revealSelection();                 // scroll so the selected notes are visible
    void autoScroll(const MouseEvent& e);   // while dragging near an edge
    int noteAt(float x, float y) const; // index in the working notes, -1 for none
    void commit(const char* label);
    void selectOnly(int index);
    bool isSelected(int index) const { return selected_.count(index) > 0; }
    const std::vector<model::MidiNote>& notes() const;

    // The notes being edited: a copy of the clip's notes while a gesture is in progress.
    std::vector<model::MidiNote> working_;
    std::vector<model::MidiNote> original_;
    model::ClipLocation editingLocation_;
    bool editing_ = false;
    std::set<int> selected_;
    // Selection rectangle (drag on empty space): corners in content beats and continuous pitch rows, so it survives scrolling.
    double marqueeBeat0_ = 0, marqueeBeat1_ = 0, marqueeRow0_ = 0, marqueeRow1_ = 0;
    std::set<int> marqueeBase_; // selection before the drag (Shift adds to it)
    bool marqueeActive_ = false;
    bool copying_ = false; // Option+drag of notes: the copies move, the originals stay
    // Draw Mode: the last grid cell painted (notes are added per cell while dragging)
    double drawCell_ = -1;
    int drawPitch_ = -1;
    void paintNoteAt(double cellStart, int pitch);
    void updateMarquee(const MouseEvent& e);
    double yToRow(float y) const { return view.topPitch - (y - notesArea().y) / view.rowHeight; }
    Mode mode_ = Mode::None;
    double grabBeat_ = 0;
    int grabPitch_ = 0;
    int grabbedNote_ = -1;
    bool changed_ = false;
    int lastUid_ = -1;
    double lastPlayhead_ = -1; // tick() repaints when the playhead moves half a pixel, appears or goes away
    bool probabilityLane_ = false;
    bool timeRange_ = false, selectingRuler_ = false;
    double rangeStart_ = 0, rangeEnd_ = 0, rangeGrab_ = 0;
    bool velocityLine_ = false;
    float velocityGrabX_ = 0, velocityLastX_ = 0, velocityLastY_ = 0;
};

} // namespace sonora::ui
