#pragma once
#include "core/AutomationEdits.h"
#include "core/AutomationLaw.h"

#include "core/Model.h"
#include "ui/GridMenu.h"
#include "ui/InlineEdit.h"
#include "midi/ControlTarget.h"
#include "ui/Widget.h"

namespace sonora::ui
{

struct UiContext;

// One arrangement automation lane with a target selector in its right-hand header (UI-5). A track can show several
// lanes, one target each (UI-65, ctx().automationLaneTargets); `lane` is this one's position below the track.
class AutomationEditor : public Widget
{
public:
    AutomationEditor(int track, int lane) : track_(track), lane_(lane) {}
    explicit AutomationEditor(int track) : AutomationEditor(track, 0) {}
    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseMove(const MouseEvent& e) override;
    void mouseExit() override;
    bool keyPressed(const KeyEvent& k) override;
    bool wantsTextInput() const override { return editingValue_; }
    void focusLost() override;
    Cursor cursorAt(float x, float y) const override;

    struct Target
    {
        int id = -1;
        std::string name, eventType = "FloatEvent";
        double min = 0, max = 1, base = 0;
        bool stepped = false;
        double position(double value) const { return model::automationPosition(law, value, min, max); }
        double valueAt(double position) const { return model::automationValue(law, position, min, max); }
        std::string group;
        // UI-65: a hosted plugin's parameter. Not listed by the set yet when id < -1: the first edit gives it a target.
        int pluginDevice = -1, hostId = -1, parameterIndex = -1;
        // UI-75: the control's override key (engine::automationOverrideKey; -1 none) and, for a native device parameter,
        // its device slot (parameterIndex then indexes the processor's parameters).
        int overrideKey = -1, nativeDevice = -1;
        model::AutomationLaw law = model::AutomationLaw::Linear; // UI-75: where a value sits on the lane
    };
    // Everything the lanes of `track` can show, in chooser order: mixer, song tempo, then each device (every parameter of a
    // loaded plugin), then lanes of the set that no control owns.
    static std::vector<Target> targets(const UiContext& ctx, int track);
    // Targets of `track` whose lane has breakpoints, in chooser order.
    static std::vector<int> automatedTargets(const UiContext& ctx, int track);
    static bool isAutomated(const UiContext& ctx, int track, int targetId);
    // The stand-in id of plugin parameter `index` on device `device` until the set lists it.
    static int pendingTargetId(int device, int index) { return -2 - device * 65536 - index; }
    // UI-65 (Live's Cmd-click on a lane's fold button, for any set of tracks): one lane per automated target on each of
    // `tracks` and automation shown for them; tracks without automation get none. False when none has automation.
    static bool showAutomatedLanes(UiContext& ctx, const std::vector<int>& tracks);

    // Shift+A / View > Show All Automation: every track's automated lanes, or hides them all when lanes show.
    static void toggleAllAutomation(UiContext& ctx);
    // The automation target a mixer or device control drives (pending id for an unlisted plugin parameter); -1 for none.
    static int targetFor(const UiContext& ctx, const midi::ControlTarget& control, int& track);
    // A control's context menu (Live): its envelope in the track's first lane, or in a new lane below the others.
    static void showTarget(UiContext& ctx, int track, int targetId, bool newLane);

    // UI-77 (Automation Write mode): the lane of `control` shows on its track (Live: clicking a control in Automation Mode),
    // and `writePoint` stores the control's value `value` at `beat` as a breakpoint (a lane that did not move yet becomes
    // flat at that value). Returns the track's control index, -1 when the control has no target.
    static int touchControl(UiContext& ctx, const midi::ControlTarget& control);
    static bool writePoint(UiContext& ctx, const midi::ControlTarget& control, double beat);
    // UI-76: the lane's right-click commands at (x, y), in menu order ("Insert Shape/<name>" go into a submenu).
    std::vector<MenuAction> contextActions(float x, float y);

    int lane() const { return lane_; }
    int shownTarget() const { return targetId_; }
    // The scale of a target's lane and whether it reads as pan / a switch (value readouts and Edit Value).
    static model::LaneScale scaleOf(const Target& t) { return {t.law, t.min, t.max}; }
    static std::string formatValue(const Target& t, double value);
    static model::AutomationEnvelope envelopeOf(const UiContext&, int track, const Target&);
    static double controlValue(const UiContext&, int track, const Target&); // the control's value now (UI-75)
    // Stores `env` as the target's lane (one undo step; an unlisted plugin parameter gets its target first) and gives the
    // control back to the lane. Returns the target id it was stored under, -1 when nothing was stored.
    static int store(UiContext&, int track, const Target&, model::AutomationEnvelope env);

private:
    std::vector<Target> targets() const { return targets(ctx(), track_); }
    model::AutomationEnvelope envelope(const Target& target) const;
    Rect plot() const;
    float beatX(double beat) const;
    float valueY(double value, const Target& target) const;
    double currentValue(const Target& target) const; // the control's value now (UI-75)
    void commit();
    void paintCell(const MouseEvent& e);
    const Target* current(const std::vector<Target>& ts);
    double beatAt(float x) const;
    double positionAt(float y) const;
    double snapped(double beat, bool bypass) const;
    int pointAt(const model::AutomationEnvelope& env, const Target& t, float x, float y) const;
    float lineDistance(const model::AutomationEnvelope& env, const Target& t, float x, float y) const;
    bool selectionHere() const;
    bool pointSelected(double time) const;
    void selectPoints(std::vector<double> times, bool range = false, double start = 0, double end = 0);
    void clearSelection();
    bool selectionRange(double& start, double& end) const; // the time selection, or the span of the selected points
    // Runs an envelope edit on a copy of this lane and stores it as one undo step.
    bool editLane(const std::function<void(model::AutomationEnvelope&, const Target&)>& edit, const char* label = "Edit Automation");
    void beginSegmentDrag(float x);
    void beginValueEdit(double time);
    void commitValueEdit();
    void showContextMenu(float x, float y);
    bool copySelection(bool cut);
    bool pasteClipboard();
    bool duplicateSelection();
    bool deleteSelection();
    Rect chooserBounds(bool device) const;
    Rect laneButton(bool add) const;
    void showChooser(bool device);
    void syncTarget(const std::vector<Target>& ts); // reads this lane's target from the context (or picks one)
    void setTarget(int id);
    void addLane(bool allAutomated);
    void removeLane(bool andBelow);

    double lastCell_ = -1;
    bool drawing_ = false; // Draw Mode: dragging paints a stepped value per grid cell
    int track_, lane_, targetId_ = -1;
    bool dragging_ = false, changed_ = false;
    Target dragTarget_;
    model::AutomationEnvelope preview_, original_;
    // UI-76: what a press started. LineClick becomes a segment drag when the pointer moves, else adds a breakpoint.
    enum class Gesture { None, Points, LineClick, SegmentNear, Segment, Curve, Range };
    Gesture gesture_ = Gesture::None;
    float downX_ = 0, downY_ = 0;
    bool moved_ = false, shift_ = false, bypassSnap_ = false;
    double pressedTime_ = 0, pressedPosition_ = 0, rangeAnchor_ = 0, curveStart_ = 0;
    int curveIndex_ = -1;
    bool curveRising_ = true, movesPreRoll_ = false;
    std::vector<double> movingTimes_, movedTimes_;
    float hoverX_ = -1, hoverY_ = -1;
    bool editingValue_ = false;
    double editTime_ = 0;
    InlineTextEdit valueEdit_;
};

} // namespace sonora::ui
