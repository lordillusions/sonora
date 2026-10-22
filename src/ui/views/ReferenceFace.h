#pragma once
// UI-42: device faces measured from captures of the reference's Live 12 devices, and the kit they are built from.
//
// A face is written in capture pixels: x from the device's left edge, y from the top of its body, in a capture whose
// body is kBodyHeight pixels tall. The face fills the device box under its title band and scales uniformly with the
// body height, so geometry, text and knobs keep the measured proportions at any detail-view height. New native devices
// derive from ReferenceFace and match their reference from the start (docs/NATIVE_DEVICE_FACES.md "Face kit").

#include "core/Model.h"
#include "ui/Painter.h"
#include "ui/Widget.h"

#include <functional>

namespace sonora::devices { class DeviceProcessor; }
namespace sonora::ui
{
class Knob;
class TextButton;

class ReferenceFace : public Widget
{
public:
    static constexpr float kBodyHeight = 239.0f;   // capture px of a device body (title band excluded)
    static constexpr float kText = 14.4f;          // regular label / value text, capture px (Utility's 9.9 of 164)
    static constexpr float kHeader = 15.6f;        // bold section labels ("Mode", "Routing")
    static constexpr float kDefaultBody = 176.0f;  // body height of the default detail view, before the first layout

    using Tag = std::function<void(Widget*, int)>;
    using Format = std::function<std::string(float)>;
    using IconPainter = std::function<void(SkCanvas&, const Theme&, Rect, int value, SkColor ink)>;
    // Knob law between a parameter's plain value and the knob's 0..1 travel.
    struct Law
    {
        std::function<float(float)> toNorm, fromNorm;
        static Law linear(float low, float high);
        static Law logarithmic(float low, float high);
        static Law power(float low, float high, float exponent); // norm = ((v - low) / (high - low))^exponent
    };

    // `originX` / `originY`: capture position of the device's left edge and of the top of its body.
    ReferenceFace(devices::DeviceProcessor* processor, float captureWidth, float originX, float originY, Tag tag);
    devices::DeviceProcessor* processor() const { return processor_; }
    float preferredWidth(float bodyHeight) const; // width of the face for a body of that height
    virtual Rect graphBounds() const { return {}; } // the face's main display, local coordinates
    void layout() override;
    void tick() override;

    // Canvas units per capture pixel, and a capture box (absolute capture coordinates) in local coordinates.
    float unit() const { return (height() > 0 ? height() : kDefaultBody) / kBodyHeight; }
    Rect at(float x0, float y0, float x1, float y1) const;
    float column(float x) const; // capture x -> local x, after any folded columns
    float text() const { return kText * unit(); }
    float header() const { return kHeader * unit(); }

    // Default value text: three significant digits with the parameter's unit ("80.0 Hz", "1.00 kHz", "30.0 ms").
    static std::string significant(float value);
    std::string formatted(int parameter) const;

protected:
    // ---- controls (all bound to real processor parameters; `x0..y1` are capture boxes)
    Knob* knob(int parameter, std::string label, float x0, float y0, float x1, float y1, Law law, Format format = {});
    // A knob whose parameter follows a selection (EQ Eight's band); the caller re-tags it when the selection changes.
    Knob* knob(std::function<int()> parameter, std::string label, float x0, float y0, float x1, float y1, Law law, Format format = {});
    Knob* inertKnob(std::string label, float norm, std::string value, float x0, float y0, float x1, float y1);
    TextButton* toggle(int parameter, std::string label, float x0, float y0, float x1, float y1);
    TextButton* option(int parameter, float value, std::string label, float x0, float y0, float x1, float y1);
    // A face button not bound to one parameter (EQ Eight's Edit L/R); set isOn / isEnabled on the result as needed.
    TextButton* button(std::string label, float x0, float y0, float x1, float y1, std::function<void()> onClick);
    TextButton* inertButton(std::string label, bool lit, float x0, float y0, float x1, float y1, std::string why);
    Widget* slider(int parameter, float x0, float y0, float x1, float y1, Law law, Format format = {});
    Widget* chooser(int parameter, float x0, float y0, float x1, float y1, IconPainter icon = {});
    Widget* chooser(std::function<int()> parameter, float x0, float y0, float x1, float y1, IconPainter icon = {});
    // A label and a cyan value on a dark display ("Thresh 0.00 dB"); drag or wheel to change, choices open a menu.
    Widget* displayValue(int parameter, std::string label, float x0, float y0, float x1, float y1, Law law, Format format = {},
                         bool menuMark = false);
    // Shows a placed widget only while `when` is true (re-evaluated every layout).
    void showWhen(Widget* widget, std::function<bool()> when);
    void place(Widget* widget, float x0, float y0, float x1, float y1);
    void tag(Widget* widget, int parameter) { if (tag_) tag_(widget, parameter); }
    void relayoutChain(); // the face changed its width: the device chain lays out again
    // Folds the capture columns [from, to) away (Compressor's sidechain section): later columns move left and
    // controls inside are hidden.
    void collapseColumns(float from, float to, bool collapsed);
    bool collapsed(float x) const { return collapsed_ && x >= collapseFrom_ && x < collapseTo_; }

    // ---- painting (capture boxes)
    void fillBody(SkCanvas&, const Theme&, bool gutters) const; // whole face: body colour, or the title colour for gutters
    void panel(SkCanvas&, const Theme&, float x0, float y0, float x1, float y1) const;
    void displayPanel(SkCanvas&, const Theme&, float x0, float y0, float x1, float y1) const;
    void label(SkCanvas&, const Theme&, std::string_view text, float x0, float y0, float x1, float y1, Align align = Align::Centre,
               bool bold = false, SkColor ink = 0) const;
    void led(SkCanvas&, const Theme&, float cx, float cy, float radius, bool lit) const;

    devices::DeviceProcessor* processor_;
    float captureWidth_, originX_, originY_;
    float collapseFrom_ = 0, collapseTo_ = 0;
    bool collapsed_ = false;
    bool animated_ = false; // repaint on every tick (meters, histories)

private:
    struct Item { Widget* widget; float x0, y0, x1, y1; std::function<bool()> shown; };
    std::vector<Item> items_;
    Tag tag_;
};

// Live 12 filter / band glyphs as drawn in the reference's EQ Eight band choosers: shelves branch into two levels,
// a bell is a diamond on the line, cuts fall away from a flat line (steeper for 48 dB).
// Types follow devices::EqEight::FilterType (0 Low Cut 48 ... 7 High Cut 48).
void eqBandIcon(SkCanvas&, Rect, int type, SkColor ink, float stroke);
// Title-bar circle icons of every device (hot-swap, save preset, menu), centred at (cx, cy).
enum class TitleIcon { HotSwap, Save, Menu };
void titleIcon(SkCanvas&, TitleIcon, float cx, float cy, float diameter, SkColor ink);

// The reference-matched face for this processor, added to `parent`, or nullptr when the device has none.
ReferenceFace* addReferenceFace(Widget& parent, const model::Device&, devices::DeviceProcessor*, ReferenceFace::Tag tag);
} // namespace sonora::ui
