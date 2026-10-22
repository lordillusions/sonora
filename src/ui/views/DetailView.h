#pragma once
// Bottom "Device View": the device chain of the selected track, left to right.
// Each device gets a DeviceBox; the body depends on the device kind:
//   EQ Eight -> response curve + band selector + Freq/Gain/Q knobs
//   Utility, compact devices, meters -> DeviceFace (layouts measured from the reference)
//   Plugin   -> X/Y pad, assignable parameter fields and editor button
//   Rack     -> list of chains
//   anything not runnable -> greyed "not supported" box
// The Clip tab shows shared loop/start controls and the selected Arrangement/Session note or sample editor.
// Rebuilt whenever the selected track, the song, the engine graph or the tab changes.

#include "ui/Widget.h"

namespace sonora::ui
{

class DetailView : public Widget
{
public:
    void layout() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void paintOverChildren(SkCanvas&, const Theme&) override;
    void tick() override;
    bool mouseWheel(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    bool keyPressed(const KeyEvent& k) override;
    void paintOverview(SkCanvas&, const Theme&, const Rect&);
    void navigateOverview(float fraction);
    Rect noteTabBounds(int tab) const;
    // Device title drags forward here; only the parent commits the edit after release.
    void beginDeviceDrag(int deviceId, float x, float y);
    void updateDeviceDrag(float x, float y);
    void endDeviceDrag(float x, float y);

private:
    void rebuild();
    void updateDeviceDropTarget();
    void cancelDeviceDrag();

    const void* builtSong_ = nullptr;
    const void* builtGraph_ = nullptr;
    uint64_t builtStructure_ = 0;
    int builtTrack_ = -1;
    int builtTab_ = -1;
    int builtRack_ = -1, builtChain_ = -1;
    uint64_t builtPlacement_ = 0; // UiContext::wavetablePlacementRevision (UI-27)
    Rect tabRect(int tab) const;
    float sidebarWidth() const;
    int clipColumns_ = 1; // columns of the clip panel's boxes, from the last layout
    float notesTabsX() const;
    Widget* panelSplit_ = nullptr; // divider between the clip panel and the editor
    float scrollX_ = 0;
    int scrollTrack_ = -2, lastBoxCount_ = -1;
    bool draggingScrollbar_ = false;
    float scrollGrabX_ = 0, scrollGrabMouseX_ = 0;
    float instrumentSlotWidth() const;
    float instrumentSlotX() const;
    float chainWidth() const;
    float contentWidth() const;
    bool scrollbarVisible() const;
    Rect scrollbarThumb() const;
    std::vector<Widget*> boxes_;
    int dragDeviceId_ = -1, dragDestination_ = -1;
    float dragStartX_ = 0, dragStartY_ = 0, dragX_ = 0, dragY_ = 0, insertionX_ = 0;
    bool deviceDragging_ = false, deviceDropAllowed_ = false;
    std::string dragLabel_;
};

} // namespace sonora::ui
