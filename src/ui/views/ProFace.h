#pragma once
// The face kit of Sonora's own creative devices (PLG-23): Stutter, Sweep and Phase Align. A dark studio look (graphite
// panels, one accent colour per device), big knobs that show their value inside and explain it in words underneath,
// segmented choices, pads, and a help line that tells what the control under the pointer does.
//
// A face is laid out on a design grid and scaled to its widget. It has two layouts: Full (its own window and the VST3
// editor) and Compact (the device chain, a short and wide grid that stays readable at the chain's height). Whoever hosts
// the face picks the size; the face picks the layout from its height (compact below kCompactBelow pixels).
//
// Controls edit the device's parameters directly (DeviceProcessor::setParameter); the app saves them from the running
// device and the VST3 wrappers report them to the host. MIDI mapping works through mappingTarget like the other faces.
//
// These devices have no reference device (they are Sonora's own), so AGENTS rule 7 does not apply.

#include "devices/DeviceProcessor.h"
#include "ui/Painter.h"
#include "ui/Widget.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class SkPath;

namespace sonora::ui
{
namespace pro
{
constexpr SkColor kBackground = 0xFF111317, kPanel = 0xFF191C22, kRaised = 0xFF21252D, kWell = 0xFF0B0D10,
                  kLine = 0xFF2A2F38, kLineSoft = 0xFF20242B, kText = 0xFFEAEDF3, kDim = 0xFF9AA1AF, kFaint = 0xFF606775,
                  kKnobTop = 0xFF454B57, kKnobBottom = 0xFF20242B, kTrack = 0xFF2C313B;

SkPaint fill(SkColor);
SkPaint stroke(SkColor, float width);
void rounded(SkCanvas&, const Rect&, float radius, SkColor);
void roundedStroke(SkCanvas&, const Rect&, float radius, SkColor, float width);
void text(SkCanvas&, std::string_view, const Rect&, SkColor, float size, Align = Align::Centre, bool bold = false);
float textWidth(std::string_view, float size, bool bold = false);
// A panel: raised graphite with a hairline border.
void panel(SkCanvas&, const Rect&, float radius = 8.0f);
// A display well: near black, inset.
void well(SkCanvas&, const Rect&, float radius = 6.0f);
// Section title: spaced capitals in the dim colour with a short accent tick.
void sectionTitle(SkCanvas&, std::string_view, float x, float y, SkColor accent);
// A knob: track arc, value arc (from the minimum or, bipolar, from the centre), shaded body, pointer.
void knob(SkCanvas&, float cx, float cy, float radius, float normalised, SkColor accent, bool bipolar, bool hot, bool driven);
// The device's name as a wordmark: the name big and light, "sonora" small under it.
void wordmark(SkCanvas&, std::string_view name, float x, float y, float size, SkColor accent, std::string_view version);
// Min/max waveform bins as a filled shape centred on y (values -1..1 times halfHeight).
void waveform(SkCanvas&, const float* lo, const float* hi, int bins, const Rect&, SkColor, float gain = 1.0f);
} // namespace pro

class ProFace : public Widget
{
public:
    enum class Layout { Full, Compact };
    struct Size { float w = 0, h = 0; };
    static constexpr float kCompactBelow = 330.0f;

    ProFace(devices::DeviceProcessor& device, Size full, Size compact, SkColor accent);

    Layout layoutMode() const { return mode_; }
    Size designSize() const { return mode_ == Layout::Full ? full_ : compact_; }
    Size fullSize() const { return full_; }
    Size compactSize() const { return compact_; }
    float scale() const; // widget pixels per design pixel
    // The width the compact layout takes at a given height (the device chain keeps the aspect ratio).
    float compactWidthFor(float height) const { return height * compact_.w / compact_.h; }
    SkColor accent() const { return accent_; }
    devices::DeviceProcessor& device() const { return device_; }

    // ---- owners
    std::function<std::optional<midi::ControlTarget>(int parameter)> mappingTarget; // MIDI/key map target of a parameter
    std::function<void()> onOpenWindow;  // compact layout: the "open the big editor" button; unset hides it
    std::string version;                 // shown next to the wordmark (the VST3 passes its own)

    // ---- Widget
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseMove(const MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    std::optional<midi::ControlTarget> mappingTargetAt(float x, float y) const override;
    void tick() override;

    // ---- tests
    // The control at a design-grid point: its parameter (-1 for buttons and areas) and label.
    int parameterAt(float designX, float designY) const;
    // Design-grid centre of the first control with this label in the current layout (nullopt when none).
    std::optional<std::pair<float, float>> controlCentre(const std::string& label) const;
    std::string helpLine() const; // the help text shown now (hovered control or the face's own)
    void hoverDesign(float x, float y); // tests: moves the pointer to a design-grid point
    MouseEvent designToLocal(float x, float y) const;

protected:
    enum class Kind { Knob, Toggle, Segment, Button, Pad, Area };
    struct Control
    {
        Kind kind = Kind::Knob;
        Rect r;          // design grid
        int param = -1;  // -1: Button/Pad/Area
        std::string label, tip;
        std::function<std::string()> caption; // knobs: the value's meaning in words, under the value
        float value = 0; // Segment: the value this cell sets
        bool bipolar = false, small = false;
        std::function<float(float)> toNorm, fromNorm; // knob response; default from the parameter (log for ms and Hz)
        std::function<bool()> driven;                  // drawn as driven by a macro (accent ring, dimmed arc)
        std::function<float()> shown;                  // the value drawn instead of the parameter (a driven control)
        std::function<bool()> lit;                     // Button/Pad: drawn on
        std::function<void()> click;                   // Button
        std::function<void(bool down)> press;          // Pad: down / up
        std::function<void(const MouseEvent&)> down, drag, up; // Area (design-grid events)
        SkColor colour = 0;                            // 0 = the face's accent
        float textSize = 0;                            // Button/Segment label size (0 = default)
    };

    // Adds the layout's controls (design grid). Called whenever the layout changes or rebuild() asks for it.
    virtual void build(Layout) = 0;
    virtual void paintBack(SkCanvas&, Layout) {}  // under the controls: panels, displays
    virtual void paintFront(SkCanvas&, Layout) {} // over them: overlays
    // The text a parameter value shows (default: choices, %, dB, Hz, ms, st).
    virtual std::string valueText(int param, float value) const;
    // The help line when no control is hovered.
    virtual std::string idleHelp() const { return {}; }
    // A knob is about to be edited by hand (Stutter turns its macro off and keeps the driven values).
    virtual void willEdit(int /*param*/) {}
    // Called ~30x/s; return true to repaint.
    virtual bool animate() { return false; }

    void rebuild();
    Control& addKnob(const Rect&, int param, std::string label, std::string tip = {});
    Control& addToggle(const Rect&, int param, std::string label, std::string tip = {});
    Control& addSegment(const Rect&, int param, float value, std::string label, std::string tip = {});
    // One cell per choice of `param` across r (or down it when vertical).
    void addSegments(const Rect&, int param, std::vector<std::string> labels = {}, bool vertical = false, float gap = 4.0f,
                     const std::string& tip = {});
    Control& addButton(const Rect&, std::string label, std::function<void()> click, std::string tip = {});
    Control& addPad(const Rect&, std::string label, std::function<void(bool)> press, std::string tip = {});
    Control& addArea(const Rect&, std::string tip = {});

    float value(int param) const;
    void setValue(int param, float value); // clamped to the parameter's range
    float normalised(const Control&) const;
    void setNormalised(const Control&, float);
    const devices::ParameterInfo& info(int param) const { return device_.parameters()[(size_t) param]; }

    // Design-grid paint helpers for faces.
    void paintKnob(SkCanvas&, const Control&);
    void paintSegment(SkCanvas&, const Control&);
    void paintButton(SkCanvas&, const Control&);
    void paintToggle(SkCanvas&, const Control&);
    void paintHelp(SkCanvas&, const Rect&); // the help line
    void paintWindowButton(SkCanvas&, const Rect&);

    const std::vector<Control>& controls() const { return controls_; }
    int hovered() const { return hover_; }
    float designMouseX() const { return mouseX_; }
    float designMouseY() const { return mouseY_; }
    int ticks() const { return ticks_; }

    devices::DeviceProcessor& device_;

private:
    MouseEvent toDesign(const MouseEvent&) const;
    int hit(float x, float y) const;
    void paintControl(SkCanvas&, const Control&, int index);

    Size full_, compact_;
    SkColor accent_;
    Layout mode_ = Layout::Full;
    bool built_ = false;
    std::vector<Control> controls_;
    int hover_ = -1, active_ = -1, pressedPad_ = -1;
    float lastY_ = 0, mouseX_ = -1, mouseY_ = -1;
    float dragNorm_ = 0;
    int ticks_ = 0;
    std::vector<float> lastValues_;
};
} // namespace sonora::ui
