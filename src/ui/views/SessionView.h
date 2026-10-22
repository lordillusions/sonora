#pragma once
// Session view: track columns with clip slots, return tracks, Main column with scene
// launchers, and the mixer section (sends, pan, volume/meter, activator/solo/arm).
//
// Clip grid is painted directly (no widget per slot) for speed; mixer controls are real
// widgets rebuilt when the song changes.
// Clicking a clip slot launches it (quantised, ENG-6); an empty slot stops the track; the Main
// column launches scenes and has "Stop All Clips".

#include "ui/UiContext.h"
#include "ui/Widget.h"

namespace sonora::ui
{

class SessionView : public Widget
{
public:
    std::optional<TrackDropTarget> trackDropTarget(const MouseEvent&) const override;
    void layout() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void paintOverChildren(SkCanvas& c, const Theme& t) override; // COL-5: others' columns
    void tick() override;
    void mouseMove(const MouseEvent& e) override;
    void mouseExit() override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    Cursor cursorAt(float x, float y) const override;
    bool keyPressed(const KeyEvent&) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    std::optional<midi::ControlTarget> mappingTargetAt(float x, float y) const override;

    // Stop-clip button row under the slot grid, in this view's coordinates (tests and hit-testing).
    Rect stopRowBounds(int column) const;
    Rect groupToggleBounds(int column) const;

private:
    void rebuild();
    bool columnVisible(int column) const;
    bool descendantOf(int track, int group) const;
    uint64_t foldSignature() const;
    void updateVisibleColumns();
    void stopGroup(int column);
    float columnX(int column) const;
    float columnWidth(int column) const;
    int columnAt(float x) const;
    int numColumns() const;
    float mixerHeight() const;
    float gridBottom() const;
    float mainWidth() const;
    float tracksViewport() const;
    float widthOf(int from, int to) const;
    int resizeColumnAt(float x) const; // column whose width handle is under x, -1 for none
    bool nearMixerEdge(float y) const;

    int resizeColumn_ = -1;
    int gateTrack_ = -1, gateScene_ = -1;
    bool resizingMixer_ = false;
    float grabX_ = 0, grabY_ = 0, startSize_ = 0;
    const model::Song* builtFor_ = nullptr;
    uint64_t builtStructure_ = 0, builtFold_ = 0;
    std::vector<bool> visibleColumns_;
    float scrollX_ = 0;
    float scrollY_ = 0;
    std::vector<Widget*> mixerStrips_;
};

} // namespace sonora::ui
