#pragma once
// Arrangement view: bar ruler with loop brace, one lane per track with its clips (MIDI
// notes / audio placeholder), playhead, and Live-style track headers on the RIGHT.
//
// Input: click ruler = set position, drag the loop brace to move / resize the loop, click lane = select
// track, wheel = scroll, Cmd+wheel = time zoom, Option+wheel = track height, Shift+wheel = horizontal scroll.
// Clip editing (UI-3): click selects, drag the title bar to move (also to another track of the same kind),
// drag the body/empty lane to select time, Cmd+D duplicates that range including its gaps,
// drag an edge to resize / trim, Alt disables snapping; Delete removes, Cmd+D duplicates,
// double-click on an empty MIDI lane creates a 4-beat clip, Cmd+Shift+M inserts one at the playhead.
// Every change is an undoable model::EditCommand applied through UiContext::applyEdit.
// A toggles automation lanes (UI-5). Audio clips use cached waveforms (UI-4).
// TODO(UI-3b): multi-selection, split/consolidate and locator editing.

#include "ui/ClipEditing.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"
#include "ui/GridMenu.h"

namespace sonora::ui
{

// Width of the track header's name column (the rest holds I/O, mixer and meter), shared by automation lane headers (UI-46).
float arrangementTitleWidth(const UiContext& ctx);

class ArrangementView : public Widget
{
public:
    std::optional<TrackDropTarget> trackDropTarget(const MouseEvent&) const override;
    void layout() override;
    std::vector<MenuAction> gridContextActions(const MouseEvent&);
    void zoomTimeSelection(bool back);
    void paint(SkCanvas& c, const Theme& t) override;
    void paintOverChildren(SkCanvas& c, const Theme& t) override;
    void tick() override;
    void mouseDown(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseExit() override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool mouseMagnify(const MouseEvent& e, float factor) override;
    bool keyPressed(const KeyEvent& k) override;
    bool wantsTextInput() const override { return renaming_ || editingWaveformZoom_; }
    void focusLost() override { editingWaveformZoom_ = false; repaint(); }
    Cursor cursorAt(float x, float y) const override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;

    // Lane order: tracks (skipping folded groups' children), returns, main.
    struct Lane
    {
        int controlIndex;
        float y, h;
        int takeLaneId = -1; // stable lane ID; -1 is the main lane
    };
    const std::vector<Lane>& lanes() const { return lanes_; }

private:
    void rebuild();
    void computeLanes();
    float beatToX(double beat) const;
    double xToBeat(float x) const;
    Rect timelineArea() const;
    int laneEdgeAt(float x, float y) const; // control index whose lane bottom edge is under the point, -1 for none
    // UI-45: which part of the loop brace (bottom strip of the ruler) is under the point.
    enum class LoopDrag { None, Move, Left, Right };
    LoopDrag loopZoneAt(float x, float y) const;
    // UI-45: clip edges resize/trim (and Shift/Alt stretch) only in the clip's title bar, like Live; the body selects time.
    ui::DragMode clipEdgeAt(const Lane* lane, float x, float y, float x0, float x1) const;
    Rect overviewRect() const;
    double overviewBeats() const;
    void paintOverview(SkCanvas& c, const Theme& t, const Rect& r);
    void paintCollaborators(SkCanvas& c, const Theme& t); // COL-5
    void reportPointer(const MouseEvent& e);
    void scrollToOverview(float x);
    const model::Clip* clipAt(float x, float y, model::ClipLocation& location, float& clipX0, float& clipX1) const;
    int laneTrackAt(float y) const; // track index of the lane under y, -1 for none / returns / main
    double gridBeats() const;
    double beatsPerBar() const;
    double snapUnit(const MouseEvent& e) const; // grid to snap to, 0 when snapping is off or overridden with Cmd/Alt
    void zoomAt(float anchorX, double factor);
    void zoomTrackHeight(double factor, float anchorY);
    void scrub(const MouseEvent& e, bool start);
    void showContextMenu(const MouseEvent& e);
    void insertMidiClip(int track, double beat);
    void optimizeHeight();
    void optimizeWidth();
    Rect waveformZoomBounds() const;
    void showWaveformZoomMenu();
    void commitWaveformZoom();
    bool waveformDecibels_ = false, editingWaveformZoom_ = false, waveformTextSelected_ = false;
    std::string waveformZoomText_;
    bool selectionZoomed_ = false;
    double selectionZoomBefore_ = 14, selectionScrollBefore_ = 0;
    bool heightOptimized_ = false, widthOptimized_ = false, draggingWaveformZoom_ = false;
    float waveformGrabY_ = 0, waveformBefore_ = 1, savedTrackScroll_ = 0;
    double savedPixelsPerBeat_ = 14, savedScrollBeats_ = 0;
    std::vector<float> savedLaneHeights_;
    Widget* waveformButton_ = nullptr;
    Widget* heightButton_ = nullptr;
    Widget* widthButton_ = nullptr;

    // A clip being dragged: the preview is drawn instead of the original until the mouse is released.
    struct Drag
    {
        bool active = false, moved = false;
        bool copy = false; // Option+drag of a clip body: the original stays and a copy is dropped at the preview
        int track = -1, uid = -1, targetTrack = -1;
        ui::DragMode mode = ui::DragMode::Move;
        int laneId = -1;
        model::Clip original, preview;
        float startX = 0, startY = 0;
    } drag_;
    struct RegionDrag
    {
        bool active = false, moved = false, valid = false, copy = false;
        UiContext::TimeSelection original;
        const model::Song* song = nullptr;
        uint64_t structure = 0;
        double grabBeat = 0, at = 0;
        float startX = 0, startY = 0;
        int grabTrack = -1, trackOffset = 0;
        std::vector<std::pair<int, model::Clip>> clips; // immutable full clips, clipped to the range while painting
    } regionDrag_;
    bool inTimeSelection(float x, float y) const;
    model::ClipLocation selectionAnchor_;
    const model::Song* selectionAnchorSong_ = nullptr;
    // Loop brace editing.
    LoopDrag loopDrag_ = LoopDrag::None;
    double loopStartBefore_ = 0, loopLengthBefore_ = 0, loopGrabBeat_ = 0;

    struct TakeRange
    {
        bool active = false, selecting = false;
        int track = -1, laneId = -1;
        double grab = 0, start = 0, end = 0;
    } takeRange_;
    bool renaming_ = false, renameSelection_ = false;
    int renameTrack_ = -1, renameLane_ = -1;
    std::string renameText_;
    const Lane* laneAt(float y) const;
    const std::vector<model::Clip>* laneClips(const Lane&) const;
    const model::Song* builtFor_ = nullptr;
    uint64_t builtStructure_ = 0; // UiContext::trackStructure() the widgets were built for
    std::vector<Lane> lanes_;
    Widget* laneContainer_ = nullptr; // clips headers/editors below the ruler
    std::vector<Widget*> headers_;
    std::vector<Widget*> automationEditors_;
    double lastPlayhead_ = -1;
    int hoverUid_ = -1, hoverTrack_ = -1; // clip under the pointer: its fade handles show
    // Fade handle of an audio clip under (x, y), if any: FadeIn / FadeOut.
    bool fadeHandleAt(const model::Clip& clip, const model::ClipLocation& location, float clipX0, float clipX1, float x, float y, ui::DragMode& mode) const;
    bool draggingRuler_ = false, draggingOverview_ = false;
    bool selectingTime_ = false;
    double selectionGrab_ = 0;
    int selectionTrack_ = -1;
    int laneResize_ = -1;
    float laneGrabY_ = 0, laneStartHeight_ = 0;
    Widget* headerSplit_ = nullptr;
};

} // namespace sonora::ui
