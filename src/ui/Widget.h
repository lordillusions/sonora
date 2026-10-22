#pragma once
// Sonora's retained-mode widget tree, drawn entirely with Skia (like Live, which draws
// its own controls instead of using native widgets).
//
// - Bounds are relative to the parent. paint() receives a canvas already translated to
//   the widget's top-left and clipped to its bounds.
// - The tree is owned by RootWidget, which is hosted by gfx::SkiaCanvasComponent.
// - Everything here runs on the message thread.

#include "midi/ControlTarget.h"
#include "ui/BrowserDrag.h"
#include "ui/Rect.h"
#include "ui/Theme.h"

#include "include/core/SkCanvas.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sonora::ui
{

struct UiContext;

enum class Cursor
{
    Default,
    ResizeHorizontal, // left/right arrows: a vertical divider
    ResizeVertical,   // up/down arrows: a horizontal divider
    Move              // a selected Arrangement region can be dragged as one block
};

struct MouseEvent
{
    float x = 0, y = 0; // local to the receiving widget
    float screenX = 0, screenY = 0;
    int clicks = 1;
    bool shift = false, cmd = false, alt = false, right = false;
    float wheelDeltaY = 0, wheelDeltaX = 0;
};

struct KeyEvent
{
    int keyCode = 0; // juce::KeyPress key code
    uint32_t textChar = 0;
    bool shift = false, cmd = false, alt = false;
    bool ctrl = false;
};

struct TrackDropTarget
{
    int before = -1, group = -1;
    Rect indicator; // local to the widget returning the target
};

class Widget
{
public:
    Widget();
    virtual ~Widget();
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // ---- tree
    template <typename T, typename... Args>
    T* add(Args&&... args)
    {
        auto w = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = w.get();
        raw->parent_ = this;
        children_.push_back(std::move(w));
        return raw;
    }
    void removeAllChildren() { children_.clear(); }
    Widget* parent() const { return parent_; }
    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }

    // ---- geometry
    void setBounds(const Rect& r);
    const Rect& bounds() const { return bounds_; }
    Rect localBounds() const { return {0, 0, bounds_.w, bounds_.h}; }
    float width() const { return bounds_.w; }
    float height() const { return bounds_.h; }

    bool visible = true;
    bool interceptsMouse = true;
    float cornerRadius = 0; // > 0: contents are clipped to a rounded rect (Live 12 panels)
    // Set on controls that MIDI can map; MIDI map mode (Cmd+M) highlights and learns them.
    std::optional<midi::ControlTarget> midiTarget;
    // Stays clickable in MIDI/Key map mode instead of being selected for mapping (the map-mode buttons themselves).
    bool bypassMapMode = false;
    std::string tooltip;

    // ---- overridables
    virtual std::optional<TrackDropTarget> trackDropTarget(const MouseEvent&) const { return {}; }
    virtual void layout() {}
    virtual void paint(SkCanvas&, const Theme&) {}
    virtual void paintOverChildren(SkCanvas&, const Theme&) {}
    virtual void mouseDown(const MouseEvent&) {}
    virtual void mouseDrag(const MouseEvent&) {}
    virtual void mouseUp(const MouseEvent&) {}
    virtual void mouseMove(const MouseEvent&) {}
    virtual void mouseExit() {}
    virtual void focusLost() {}
    virtual void mouseDoubleClick(const MouseEvent& e) { mouseDown(e); }
    virtual bool mouseWheel(const MouseEvent&) { return false; } // true == consumed
    // Trackpad pinch: factor > 1 zooms in, around the pointer. Returning false lets a parent handle it.
    virtual bool mouseMagnify(const MouseEvent&, float /*factor*/) { return false; }
    virtual bool keyPressed(const KeyEvent&) { return false; }   // true == consumed
    // True while the widget is collecting typed text (search field, rename box): letters are text then, never shortcuts or notes.
    virtual bool wantsTextInput() const { return false; }
    // A mouse press landed on `leaf` (this widget or one of its descendants); lets containers track "the thing the user is on".
    virtual void descendantPressed(Widget* /*leaf*/) {}
    virtual std::optional<midi::ControlTarget> mappingTargetAt(float /*x*/, float /*y*/) const { return midiTarget; }
    virtual bool browserDrop(const BrowserDrag&, const MouseEvent&) { return false; }
    // Pointer shape over local (x, y); views with internal dividers override it.
    virtual Cursor cursorAt(float /*x*/, float /*y*/) const { return Cursor::Default; }
    // Called ~30x/s by the root while something animates (playhead, meters).
    virtual void tick() {}

    // ---- services
    void repaint();
    UiContext& ctx() const; // walks up to the root
    Rect boundsInRoot() const;

    // True while `w` points to a live widget. Views rebuild their children freely, so the
    // root uses this to drop stale hover/capture pointers.
    static bool isAlive(const Widget* w);

    // Internal (used by RootWidget).
    void paintTree(SkCanvas& canvas, const Theme& theme);
    Widget* findAt(float x, float y, float& localX, float& localY);
    void tickTree();

protected:
    Widget* parent_ = nullptr;
    std::vector<std::unique_ptr<Widget>> children_;
    Rect bounds_;
};

} // namespace sonora::ui
