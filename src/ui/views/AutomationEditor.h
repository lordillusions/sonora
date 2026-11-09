#pragma once
#include "core/AutomationLaw.h"

#include "core/Model.h"
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
    bool keyPressed(const KeyEvent& k) override;

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

    int lane() const { return lane_; }
    int shownTarget() const { return targetId_; }

private:
    std::vector<Target> targets() const { return targets(ctx(), track_); }
    model::AutomationEnvelope envelope(const Target& target) const;
    Rect plot() const;
    float beatX(double beat) const;
    float valueY(double value, const Target& target) const;
    double currentValue(const Target& target) const; // the control's value now (UI-75)
    void commit();
    void paintCell(const MouseEvent& e);
    Rect chooserBounds(bool device) const;
    Rect laneButton(bool add) const;
    void showChooser(bool device);
    void syncTarget(const std::vector<Target>& ts); // reads this lane's target from the context (or picks one)
    void setTarget(int id);
    void addLane(bool allAutomated);
    void removeLane(bool andBelow);

    double lastCell_ = -1;
    bool drawing_ = false; // Draw Mode: dragging paints a stepped value per grid cell
    int track_, lane_, targetId_ = -1, selectedPoint_ = -1;
    bool dragging_ = false, changed_ = false;
    Target dragTarget_;
    model::AutomationEnvelope preview_;
};

} // namespace sonora::ui
