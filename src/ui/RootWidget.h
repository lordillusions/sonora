#pragma once
// Top of the widget tree. Routes input (with mouse capture during drags) and tracks
// whether a redraw is needed.

#include "ui/UiContext.h"
#include "ui/Widget.h"

namespace sonora::ui
{

class RootWidget : public Widget
{
public:
    explicit RootWidget(UiContext& ctx) : ctx_(ctx) {}

    UiContext& context() const { return ctx_; }

    // Only sets a flag: the host (SkiaCanvasComponent) polls it every vblank.
    // UiContext::requestRepaint is the *external* entry point and ends up here.
    void markDirty() { dirty_ = true; }
    bool consumeDirty()
    {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    // ---- input from the host component (root coordinates)
    void dispatchMouseDown(MouseEvent e);
    void dispatchMouseDrag(MouseEvent e);
    void dispatchMouseUp(MouseEvent e);
    void dispatchMouseMove(MouseEvent e);
    void dispatchMouseWheel(MouseEvent e);
    void dispatchMouseMagnify(MouseEvent e, float factor);
    void dispatchMouseExit();
    // External (Finder) file drags, root coordinates. Audio files travel as a Sample BrowserDrag that is shown and
    // dropped like a browser drag (Cmd at the drop: one track per file); a lone .als drop opens that set.
    // Return false when the paths hold nothing Sonora accepts.
    static std::optional<BrowserDrag> fileDragPayload(const std::vector<std::string>& paths);
    static bool acceptsFileDrag(const std::vector<std::string>& paths);
    bool dispatchFileDragMove(float x, float y, const std::vector<std::string>& paths);
    void dispatchFileDragExit();
    bool dispatchFileDrop(MouseEvent e, const std::vector<std::string>& paths);
    void setKeyboardFocus(Widget* widget);
    Widget* keyboardFocus() const { return isAlive(keyboardFocus_) ? keyboardFocus_ : nullptr; }
    // Pointer shape for the widget under the mouse (or the one holding the mouse during a drag).
    Cursor cursor() const { return cursor_; }
    bool dispatchKey(const KeyEvent& k);
    // REQ-3a: physical key presses from the window. User-rebound menu shortcuts run first (never while typing, in Settings,
    // export or a MIDI/key map mode, or for plain keys the Computer MIDI Keyboard plays); then dispatchKey. Synthetic keys
    // the menus send themselves go straight to dispatchKey, so a released default key still works from its menu item.
    bool dispatchUserKey(const KeyEvent& k);
    bool dispatchKeyUp(int keyCode);
    void releaseMappedKeys();
    std::vector<int> heldLaunchKeys() const;
    void paintOverChildren(SkCanvas&, const Theme&) override;
    void tick() override;

private:
    void updateTrackDrag(const MouseEvent&);
    void dropBrowserPayload(BrowserDrag payload, const MouseEvent& e);
    bool externalDrag_ = false; // ctx_.browserDrag shows a Finder drag
    float downX_ = 0, downY_ = 0;
    bool movingTracks_ = false;
    std::optional<TrackDropTarget> trackDrop_;
    Widget* captured_ = nullptr;
    Widget* keyboardFocus_ = nullptr;
    Widget* hovered_ = nullptr;
    int hoverTicks_ = 0;
    Rect capturedOrigin_;
    bool capturedRight_ = false;
    UiContext& ctx_;
    bool dirty_ = true;
    Cursor cursor_ = Cursor::Default;
    std::vector<midi::Mapping> heldKeys_; // message-thread targets retained through key release
    struct HeldNote { int keyCode; int note; };
    std::vector<HeldNote> heldNotes_; // Computer MIDI Keyboard notes sounding until their key is released
    bool handleComputerKeyboard(const KeyEvent& k);
};

} // namespace sonora::ui
