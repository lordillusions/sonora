#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetableModulationEditor.h"
#include "ui/views/WavetableRoutingEditor.h"
#include "ui/views/WavetablePresetBrowser.h"
#include "ui/RootWidget.h"
#include "devices/native/WavetableSynth.h"
#include "engine/AudioFileFormats.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableModRouting.h"
#include "plugins/SerumLibrary.h"
#include "plugins/PrismPreset.h"
#include "devices/DeviceFactory.h"
#include "dsp/ExponentialCurveGraph.h"
#include "dsp/ExponentialEnvelopeCurve.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkDashPathEffect.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <set>
#include <cstdio>

namespace sonora::ui
{
namespace
{
using S = WavetableEditor::Section;
using W = devices::WavetableSynth;
constexpr float kHeader = 52, kTabs = 22, kNavigation = 22, kGap = 4;
constexpr SkColor kBackground = 0xff11171f, kPanel = 0xff202a35, kRaised = 0xff2d3946, kLine = 0xff3b4b5c;
constexpr SkColor kText = 0xffe5edf7, kDim = 0xff96a9bd, kCyan = 0xff43d9e7, kBlue = 0xff4f9eff, kPlot = 0xff111d29;
constexpr SkColor kBase = 0xff283543, kValueArc = 0xff4aa3ff, kValue = 0xff7fd6ff, kLfo = 0xff9a8cff, kMacro = 0xffe8b04b, kFrame = 0xffffd23f;
constexpr std::array<const char*, (size_t) S::Count> kSections = {
    "Oscillators", "Filters", "Envelopes", "LFOs", "Matrix", "Macros", "Main", "Media", "Effects", "Routing", "Noise"
};

std::string number(const char* format, double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}
bool quadraticGain(const std::string& id)
{
    return id == "FxRouting/DirectGain" || id == "FxRouting/Bus1ToMain" || id == "FxRouting/Bus2ToMain"
        || id == "Filter.0/OutputLevel" || id == "Filter.1/OutputLevel";
}

Knob::Source parameterSource(devices::DeviceProcessor* device, int parameter)
{
    const auto& info = device->parameters()[(size_t) parameter];
    Knob::Source source;
    source.get = [device, parameter] { return device->getParameter(parameter); };
    source.set = [device, parameter, integer = info.unit == "int"](float value) {
        device->setParameter(parameter, integer ? std::round(value) : value);
    };
    source.minValue = info.minValue;
    source.maxValue = info.maxValue;
    source.defaultValue = info.defaultValue;
    source.bipolar = info.minValue < 0 && info.maxValue > 0;
    source.logarithmic = (info.unit == "Hz" || info.unit == "s" || info.unit == "ms") && info.minValue > 0;
    source.format = [unit = info.unit](float value) {
        if (unit == "Hz") return value >= 1000 ? number("%.2f kHz", value / 1000.0) : number("%.2f Hz", value);
        if (unit == "ms") return value >= 1000 ? number("%.2f s", value / 1000.0) : number("%.1f ms", value);
        if (unit == "s") return value < 1 ? number("%.1f ms", value * 1000.0) : number("%.2f s", value);
        if (unit == "dB") return number("%.1f dB", value);
        if (unit == "%") return number("%.0f %%", value * 100.0);
        if (unit == "pct") return number("%.0f %%", value);
        if (unit == "int") return number("%.0f", value);
        if (unit == "st") return number("%+.1f st", value);
        if (unit == "ct") return number("%+.0f ct", value);
        if (unit == "oct") return number("%+.2f oct", value);
        if (unit == "deg") return number("%.0f deg", value);
        if (unit == "x") return number("%+.2fx", value);
        if (unit == "beats") return number("%.3f beats", value);
        return number("%.2f", value);
    };
    if (quadraticGain(info.id))
    {
        // Gesture coordinates use the measured quadratic taper; the device, saved model,
        // automation and MIDI mappings retain the ordinary 0..4 gain factor.
        source.get = [device, parameter] { return std::sqrt(std::clamp(device->getParameter(parameter) / 4, 0.0f, 1.0f)); };
        source.set = [device, parameter](float value) { value = std::clamp(value, 0.0f, 1.0f); device->setParameter(parameter, 4 * value * value); };
        source.defaultValue = std::sqrt(info.defaultValue / 4);
        source.minValue = 0; source.maxValue = 1;
        source.format = [](float value) {
            const float gain = 4 * value * value;
            return number(gain > 0 && gain < 0.0001f ? "%.2gx" : gain > 0 && gain < 0.01f ? "%.4fx" : "%.2fx", gain);
        };
    }
    else if (info.unit == "s" && info.minValue <= 0 && info.maxValue > 0)
    {
        // The observed fifth-power envelope scalar keeps millisecond edits practical while
        // retaining zero and the full seconds range. The engine/mapping target still stores seconds.
        const float maximum = info.maxValue;
        const auto plainFormat = source.format;
        source.get = [device, parameter, maximum] { return std::pow(std::clamp(device->getParameter(parameter) / maximum, 0.0f, 1.0f), 0.2f); };
        source.set = [device, parameter, maximum](float value) { device->setParameter(parameter, maximum * std::pow(value, 5.0f)); };
        source.defaultValue = std::pow(info.defaultValue / maximum, 0.2f);
        source.minValue = 0; source.maxValue = 1;
        source.format = [plainFormat, maximum](float value) { return plainFormat(maximum * std::pow(value, 5.0f)); };
    }
    return source;
}

Theme& darkTheme()
{
    static Theme theme;
    static const bool initialized = [] {
        for (const char* token : {"ControlForeground", "ControlOnForeground", "ControlOffForeground"}) theme.set(token, kText);
        for (const char* token : {"SurfaceBackground", "SurfaceHighlight", "ControlBackground", "ControlFillHandle", "ControlTextBack", "TransportOffBackground"}) theme.set(token, kRaised);
        theme.set("TransportOffForeground", kText);
        theme.set("ControlContrastFrame", kLine); theme.set("ChosenDefault", 0xff246980);
        theme.set("TextDisabled", kDim); theme.set("RangeDefault", kCyan);
        theme.metrics.fontSize = 10; theme.metrics.smallFontSize = 9;
        return true;
    }();
    (void) initialized;
    return theme;
}
class DarkButton final : public TextButton
{
public:
    using TextButton::TextButton;
    void paint(SkCanvas& canvas, const Theme&) override { TextButton::paint(canvas, darkTheme()); }
    void mouseUp(const MouseEvent& event) override
    {
        // Preset/media edits may replace the processor and this widget synchronously.
        const auto action = onClick; onClick = {};
        TextButton::mouseUp(event); onClick = action;
        if (localBounds().contains(event.x, event.y) && action && (! isEnabled || isEnabled())) action();
    }
};
// Nested popup of library items by folder; picking one runs `pick(path)` (the widget may be gone by then).
void addLibraryMenu(juce::PopupMenu& menu, const std::string& title, const std::vector<plugins::SerumLibraryItem>& items,
                    std::function<void(const std::string&)> pick)
{
    struct Node { std::map<std::string, Node> folders; std::vector<const plugins::SerumLibraryItem*> items; };
    Node root;
    for (const auto& item : items)
    {
        Node* node = &root;
        for (const auto& part : juce::StringArray::fromTokens(juce::String(item.folder), "/", ""))
            if (part.isNotEmpty()) node = &node->folders[part.toStdString()];
        node->items.push_back(&item);
    }
    std::function<juce::PopupMenu(const Node&)> build = [&](const Node& node) {
        juce::PopupMenu result;
        for (const auto& [name, child] : node.folders) result.addSubMenu(juce::String(name), build(child));
        for (const auto* item : node.items) result.addItem(juce::String(item->name), [pick, path = item->path] { pick(path); });
        return result;
    };
    if (items.empty()) menu.addItem(juce::PopupMenu::Item(juce::String(title) + " (not installed)").setEnabled(false));
    else menu.addSubMenu(juce::String(title), build(root));
}
// Each effect type keeps one colour across its name, knobs and graph (Serum colours its modules).
SkColor effectColor(devices::WavetableEffects::Type type)
{
    using T = devices::WavetableEffects::Type;
    switch (type)
    {
        case T::Distortion: return 0xffff7a45;
        case T::Flanger: return 0xff4fd1ff;
        case T::Phaser: return 0xffff5fa2;
        case T::Chorus: return 0xff6f8cff;
        case T::Delay: return 0xffa77bff;
        case T::Compressor: return 0xffff5b5b;
        case T::Reverb: return 0xff8be04e;
        case T::Eq: return 0xffffd23f;
        case T::Filter: return 0xff43d9e7;
        case T::HyperDimension: return 0xffe86bff;
        case T::FrequencyShift: return 0xfff0a35b;
        case T::Convolution: return 0xff7fdcb0;
        case T::Split3: return 0xffd0d8e0;
        case T::Utility: case T::Unknown: break;
    }
    return 0xffb0bccb;
}
// The controls a compact rack panel shows, in priority order (Serum's panels show a module's main controls; the rest are in
// the module's CONTROLS page, opened from the panel's ••• button).
std::vector<std::string_view> primaryEffectControls(devices::WavetableEffects::Type type)
{
    using T = devices::WavetableEffects::Type;
    switch (type)
    {
        case T::Delay: return {"Feedback", "Time", "RightTime", "FilterFreq", "FilterWidth", "LowCut", "HighCut"};
        case T::Reverb: return {"Size", "Decay", "Predelay", "Damping", "Diffusion", "Width"};
        case T::Distortion: return {"Drive", "Tone", "Bias", "Amount", "FilterFreq", "FilterWidth"};
        case T::Chorus: return {"Rate", "Depth", "Time", "Feedback", "Voices", "Width"};
        case T::Flanger: return {"Rate", "Depth", "Time", "Feedback", "Phase"};
        case T::Phaser: return {"Rate", "Center", "Depth", "Feedback", "Stages", "Phase"};
        case T::Compressor: return {"Threshold", "Ratio", "Attack", "Release", "Makeup", "Knee"};
        case T::Eq: return {"Band1Freq", "Band1Gain", "Band1Q", "Band2Freq", "Band2Gain", "Band2Q"};
        case T::Filter: return {"Cutoff", "Resonance", "Drive"};
        case T::HyperDimension: return {"Rate", "Detune", "Voices", "DimensionSize", "DimensionMix", "Width"};
        case T::FrequencyShift: return {"Shift", "Fine", "Feedback", "Width"};
        case T::Utility: return {"Width", "Pan", "HighPass", "LowPass", "Mono"};
        case T::Split3: return {"LowSplit", "HighSplit", "LowGain", "MidGain", "HighGain"};
        case T::Convolution: return {"Output"};
        case T::Unknown: break;
    }
    return {};
}
std::map<int, std::set<uint32_t>>& collapsedEffects() { static std::map<int, std::set<uint32_t>> collapsed; return collapsed; } // view state per instance
SkColor sourceColor(int source)
{
    if (source >= W::Env1 && source <= W::Env4) return kCyan;
    if (source >= W::Lfo1 && source <= W::Lfo10) return kLfo;
    if (source >= W::Macro1 && source <= W::Macro8) return kMacro;
    return 0xff6fdc8c;
}
// The instrument's own name, current or from before the rename (sets keep the name they were saved with).
bool isDefaultName(const std::string& name) { return name.empty() || name == "Sonora Prism" || name == "Sonora Wavetable"; }
std::string upper(std::string text)
{
    for (auto& c : text) c = (char) std::toupper((unsigned char) c);
    return text;
}
void showChoiceMenu(Widget& owner, const std::vector<std::string>& labels, std::function<void(int)> pick)
{
    if (const auto show = owner.ctx().showChoices) { show(labels, std::vector<bool>(labels.size(), false), std::move(pick)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, juce::String(labels[i]));
    auto* widget = &owner;
    menu.showMenuAsync(juce::PopupMenu::Options(), [widget, pick](int result) { if (result > 0 && Widget::isAlive(widget)) pick(result - 1); });
}
// Serum-style rotary: dark body, blue value arc on the bezel, label below (value while hovered/dragged) and one
// thin ring per matrix route into this control. Dragging a ring rescales that route; right-click removes routes.
class PanelKnob final : public Knob
{
public:
    PanelKnob(std::string label, Source source, devices::DeviceProcessor* processor, int parameter)
        : Knob(label, source), label_(std::move(label)), source_(std::move(source)), processor_(processor), parameter_(parameter) {}
    bool compact = false, inlineField = false; // inlineField: compact on one line, caption left and value right
    bool stepper = false;                      // Serum's UNISON box: the number with ▲▼, label below
    SkColor accent = 0;                        // value-arc colour override (FX modules use their own colour)
    std::string compactLabel;
    std::function<std::string(float)> valueText; // overrides the parameter's own format (e.g. degrees)
    std::function<void(int slot)> onRingHover;    // slot under the pointer / being dragged, -1 none
    float plainAt(float n) const
    {
        n = std::clamp(n, 0.0f, 1.0f);
        if (source_.logarithmic && source_.minValue > 0) return source_.minValue * std::pow(source_.maxValue / source_.minValue, n);
        return source_.minValue + n * (source_.maxValue - source_.minValue);
    }
    float currentNorm() const { return normalized(source_.get ? source_.get() : source_.defaultValue); }
    Rect stepperBox() const
    {
        const auto g = geometry(); const float w = std::min(width() - 4, 44.0f);
        return {g.cx - w * 0.5f, g.cy - 11, w, 22};
    }
    int ringAt(float x, float y) const
    {
        const auto routes = this->routes();
        if (routes.empty()) return -1;
        const auto g = geometry();
        const float distance = std::hypot(x - g.cx, y - g.cy) - g.radius - kRingGap;
        const int ring = (int) std::lround(distance / kRingStep);
        return distance > -2.0f && ring >= 0 && ring < (int) std::min(routes.size(), kRings) ? routes[(size_t) ring].slot : -1;
    }
    struct Geometry { float cx = 0, cy = 0, radius = 0; Rect label; };
    Geometry geometry() const
    {
        auto r = localBounds(); Geometry g; g.label = r.removeFromBottom(12);
        const float d = std::max(8.0f, std::min(r.w, r.h) - 2 * (kRingGap + kRingStep + 1.5f)); // room for two route rings
        g.cx = r.centreX(); g.cy = r.centreY(); g.radius = d * 0.5f; return g;
    }
    static constexpr float kRingGap = 5.5f, kRingStep = 3.0f;
    static constexpr size_t kRings = 2; // further routes stay listed in the right-click menu and the matrix
    std::vector<devices::wavetable_routing::Route> routes() const
    {
        if (compact || parameter_ >= W::ParameterCount) return {};
        return devices::wavetable_routing::routesForParameter(*processor_, parameter_);
    }
    // Local grab point on ring `ring` (top of the circle).
    std::pair<float, float> ringPoint(int ring) const
    { const auto g = geometry(); return {g.cx, g.cy - g.radius - kRingGap - kRingStep * (float) ring}; }
    float normalized(float value) const
    {
        float n = (value - source_.minValue) / (source_.maxValue - source_.minValue);
        if (source_.logarithmic && source_.minValue > 0) n = std::log(value / source_.minValue) / std::log(source_.maxValue / source_.minValue);
        return std::clamp(n, 0.0f, 1.0f);
    }
    std::string text(float value) const
    {
        return valueText ? valueText(value) : source_.format ? source_.format(value) : number("%.2f", value);
    }
    void paint(SkCanvas& canvas, const Theme&) override
    {
        auto r = localBounds();
        const float value = source_.get ? source_.get() : source_.defaultValue;
        const std::string caption = compactLabel.empty() ? label_ : compactLabel;
        if (compact && inlineField)
        {
            paint::fillRounded(canvas, r.reduced(1), 2, kBackground);
            paint::text(canvas, upper(caption), {5, 0, r.w * 0.5f, r.h}, kDim, 8, Align::Left, true);
            paint::text(canvas, text(value), {r.w * 0.35f, 0, r.w * 0.65f - 5, r.h}, kValue, 9, Align::Right);
            return;
        }
        if (compact)
        {
            paint::fillRounded(canvas, r.reduced(1), 2, kBackground);
            paint::text(canvas, upper(caption), {2, 1, r.w - 4, 10}, kDim, 8, Align::Centre, true);
            paint::text(canvas, text(value), {2, 11, r.w - 4, std::max(10.0f, r.h - 11)}, kValue, 9, Align::Centre);
            return;
        }
        const auto g = geometry();
        if (stepper)
        {
            const auto box = stepperBox();
            paint::fillRounded(canvas, box, 3, kBackground); paint::stroke(canvas, box, kLine);
            paint::text(canvas, number("%.0f", value), {box.x + 2, box.y, box.w - 15, box.h}, kValue, 11, Align::Centre, true);
            paint::line(canvas, box.right() - 13, box.y + 3, box.right() - 13, box.bottom() - 3, kLine);
            ctl::triangle(canvas, box.right() - 6.5f, box.y + box.h * 0.3f, 6, 4, ctl::Dir::Up, kDim);
            ctl::triangle(canvas, box.right() - 6.5f, box.y + box.h * 0.7f, 6, 4, ctl::Dir::Down, kDim);
            paint::text(canvas, upper(caption), g.label, kDim, std::clamp((g.label.w - 2) / (0.64f * (float) std::max<size_t>(1, caption.size())), 6.0f, 8.0f), Align::Centre, true);
            return;
        }
        const float cx = g.cx, cy = g.cy, radius = g.radius;
        SkPaint pen; pen.setAntiAlias(true);
        const auto routes = this->routes();
        const float bezel = radius + 2.6f;
        pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(2.6f); pen.setColor(0xff0a1017);
        canvas.drawArc(SkRect::MakeXYWH(cx - bezel, cy - bezel, bezel * 2, bezel * 2), 135, 270, false, pen);
        pen.setStyle(SkPaint::kFill_Style); pen.setColor(0xff070c12);
        canvas.drawCircle(cx, cy + 1, radius + 0.5f, pen);
        const SkColor colors[]{0xff4a5868, 0xff1b232d};
        const SkPoint points[]{{cx, cy - radius}, {cx, cy + radius}};
        pen.setShader(SkGradientShader::MakeLinear(points, colors, nullptr, 2, SkTileMode::kClamp));
        canvas.drawCircle(cx, cy, radius, pen); pen.setShader(nullptr);
        pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1); pen.setColor(0xff56677a);
        canvas.drawCircle(cx, cy, radius - 0.5f, pen);
        const float norm = normalized(value);
        const float origin = source_.bipolar ? -source_.minValue / (source_.maxValue - source_.minValue) : 0.0f;
        pen.setColor(accent != 0 ? accent : kValueArc); pen.setStrokeWidth(2.6f);
        const auto arc = SkRect::MakeXYWH(cx - bezel, cy - bezel, bezel * 2, bezel * 2);
        canvas.drawArc(arc, 135 + std::min(origin, norm) * 270, std::max(0.6f, std::abs(norm - origin) * 270), false, pen);
        const float angle = (135 + norm * 270) * 0.01745329252f;
        pen.setColor(kText); pen.setStrokeWidth(1.8f); pen.setStrokeCap(SkPaint::kRound_Cap);
        canvas.drawLine(cx + std::cos(angle) * radius * 0.3f, cy + std::sin(angle) * radius * 0.3f,
                        cx + std::cos(angle) * radius * 0.86f, cy + std::sin(angle) * radius * 0.86f, pen);
        pen.setStrokeCap(SkPaint::kButt_Cap);
        const float travel = devices::wavetable_routing::travelPerAmount(*processor_, parameter_);
        for (size_t i = 0; i < std::min(routes.size(), kRings); ++i)
        {
            const auto& route = routes[i];
            const float ring = radius + kRingGap + kRingStep * (float) i;
            const auto oval = SkRect::MakeXYWH(cx - ring, cy - ring, ring * 2, ring * 2);
            pen.setStrokeWidth(2.2f); pen.setColor(0xff0a1017); canvas.drawArc(oval, 135, 270, false, pen);
            const auto sweep = devices::wavetable_routing::span(*processor_, parameter_, route, norm);
            const float from = sweep.from, to = sweep.to;
            pen.setColor(sourceColor(route.source));
            if (route.slot == ringSlot_ || route.slot == hoverSlot_) pen.setStrokeWidth(3.0f);
            canvas.drawArc(oval, 135 + from * 270, std::max(1.5f, (to - from) * 270), false, pen);
        }
        if (const auto* synth = dynamic_cast<const W*>(processor_); synth != nullptr && ! routes.empty())
        {
            // Live position: where the modulated value is right now (Serum's moving marker on the ring).
            const auto live = synth->liveState();
            if (live.active)
            {
                float offset = 0;
                for (const auto& route : routes) offset += route.amount * live.route[(size_t) route.slot];
                const float at = std::clamp(norm + offset * travel, 0.0f, 1.0f), liveAngle = (135 + at * 270) * 0.01745329252f;
                pen.setStyle(SkPaint::kFill_Style); pen.setColor(sourceColor(routes.front().source));
                canvas.drawCircle(cx + std::cos(liveAngle) * bezel, cy + std::sin(liveAngle) * bezel, 3.2f, pen);
                pen.setColor(kText);
                canvas.drawCircle(cx + std::cos(liveAngle) * bezel, cy + std::sin(liveAngle) * bezel, 1.6f, pen);
            }
        }
        const bool showValue = hovered_ || dragging_;
        const std::string shown = showValue ? text(value) : upper(caption);
        // Shrink to fit narrow cells instead of running into the neighbouring label.
        const float size = std::clamp((g.label.w - 2) / (0.64f * (float) std::max<size_t>(1, shown.size())), 6.0f, 8.0f);
        paint::text(canvas, shown, g.label, showValue ? kText : kDim, size, Align::Centre, ! showValue);
    }
    void mouseMove(const MouseEvent& event) override
    {
        if (! hovered_) { hovered_ = true; repaint(); }
        const int slot = ringAt(event.x, event.y);
        if (slot != hoverSlot_ && onRingHover) { hoverSlot_ = slot; onRingHover(slot); }
    }
    void mouseExit() override
    {
        hovered_ = false; repaint();
        if (hoverSlot_ >= 0 && ringSlot_ < 0 && onRingHover) { hoverSlot_ = -1; onRingHover(-1); }
    }
    void mouseDown(const MouseEvent& event) override
    {
        if (stepper && ! event.right)
        {
            const auto box = stepperBox();
            if (box.contains(event.x, event.y) && event.x >= box.right() - 14 && source_.get && source_.set)
            {
                source_.set(std::clamp(std::round(source_.get()) + (event.y < box.centreY() ? 1.0f : -1.0f), source_.minValue, source_.maxValue));
                repaint(); return;
            }
        }
        const auto routes = this->routes();
        const int ring = ringAt(event.x, event.y);
        if (event.right && ring >= 0) { directionMenu(ring); return; }
        if (event.right && ! routes.empty()) { modulationMenu(routes); return; }
        ringSlot_ = -1;
        if (! event.right && ring >= 0)
        {
            ringSlot_ = ring; ringStartAmount_ = processor_->getParameter(W::modParam(ring, W::ModAmount)); ringStartY_ = event.y;
            dragging_ = true; if (onRingHover) onRingHover(ring); return;
        }
        dragging_ = ! event.right;
        Knob::mouseDown(event);
    }
    void mouseDrag(const MouseEvent& event) override
    {
        if (ringSlot_ < 0) { Knob::mouseDrag(event); return; }
        const float travel = devices::wavetable_routing::travelPerAmount(*processor_, parameter_);
        if (travel <= 0) return;
        const float delta = (ringStartY_ - event.y) / (event.shift ? 600.0f : 150.0f);
        devices::wavetable_routing::apply(*processor_, devices::wavetable_routing::setAmount(ringSlot_, ringStartAmount_ + delta / travel));
        repaint();
    }
    void mouseUp(const MouseEvent& event) override
    {
        const bool ring = ringSlot_ >= 0;
        ringSlot_ = -1; dragging_ = false; repaint();
        if (ring && onRingHover) { hoverSlot_ = ringAt(event.x, event.y); onRingHover(hoverSlot_); }
    }
    bool mouseWheel(const MouseEvent& event) override
    {
        if (std::abs(event.wheelDeltaX) > std::abs(event.wheelDeltaY)) return false;
        if (event.shift && quadraticGain(processor_->parameters()[(size_t) parameter_].id))
        {
            if (source_.get && source_.set) source_.set(std::clamp(source_.get() + event.wheelDeltaY * 0.005f, 0.0f, 1.0f));
            repaint(); return true;
        }
        return Knob::mouseWheel(event);
    }
    void mouseDoubleClick(const MouseEvent& event) override
    {
        if (event.right) { mouseDown(event); return; }
        namespace R = devices::wavetable_routing;
        if (const int ring = ringAt(event.x, event.y); ring >= 0)
        {
            // Double-click a ring: both sides <-> forward (Serum's bipolar toggle).
            R::apply(*processor_, R::setDirection(*processor_, ring, R::direction(*processor_, ring) == R::Direction::Both ? R::Direction::Forward : R::Direction::Both));
            repaint(); return;
        }
        processor_->setParameter(parameter_, processor_->parameters()[(size_t) parameter_].defaultValue); repaint();
    }
private:
    void directionMenu(int slot)
    {
        namespace R = devices::wavetable_routing;
        const auto current = R::direction(*processor_, slot);
        const std::vector<std::string> labels{std::string(current == R::Direction::Forward ? "✓ " : "") + "Forward (unipolar, up)",
                                              std::string(current == R::Direction::Backward ? "✓ " : "") + "Backward (unipolar, down)",
                                              std::string(current == R::Direction::Both ? "✓ " : "") + "Both Sides (bipolar)",
                                              "Remove " + std::string(W::sourceName((int) std::lround(processor_->getParameter(W::modParam(slot, W::ModSource))))) + " Modulation"};
        auto* processor = processor_;
        showChoiceMenu(*this, labels, [this, processor, slot](int choice) {
            namespace R2 = devices::wavetable_routing;
            if (choice >= 0 && choice < 3) R2::apply(*processor, R2::setDirection(*processor, slot, static_cast<R2::Direction>(choice)));
            if (choice == 3) R2::apply(*processor, R2::disconnect(slot));
            repaint();
        });
    }
    void modulationMenu(const std::vector<devices::wavetable_routing::Route>& routes)
    {
        std::vector<std::string> labels;
        const std::string target = W::destinationName(routes.front().destination);
        for (const auto& route : routes) labels.push_back("Remove " + std::string(W::sourceName(route.source)) + " → " + target);
        labels.push_back("Remove All Modulation");
        labels.push_back("Return to Default");
        auto* processor = processor_; const int parameter = parameter_;
        std::vector<int> slots; for (const auto& route : routes) slots.push_back(route.slot);
        showChoiceMenu(*this, labels, [this, processor, parameter, slots](int choice) {
            namespace R = devices::wavetable_routing;
            if (choice >= 0 && (size_t) choice < slots.size()) R::apply(*processor, R::disconnect(slots[(size_t) choice]));
            else if ((size_t) choice == slots.size()) for (const int slot : slots) R::apply(*processor, R::disconnect(slot));
            else if ((size_t) choice == slots.size() + 1) processor->setParameter(parameter, processor->parameters()[(size_t) parameter].defaultValue);
            repaint();
        });
    }
    std::string label_;
    Source source_;
    devices::DeviceProcessor* processor_;
    int parameter_;
    bool hovered_ = false, dragging_ = false;
    int ringSlot_ = -1, hoverSlot_ = -1;
    float ringStartAmount_ = 0, ringStartY_ = 0;
};
// Drag handle of a modulation source (ENV/LFO tab, macro): the gesture is forwarded to the editor, which owns the drop.
class ModSourceHandle final : public Widget
{
public:
    ModSourceHandle(WavetableEditor& editor, int source) : editor_(editor), source_(source) {}
    void paint(SkCanvas& canvas, const Theme&) override
    {
        const auto r = localBounds();
        const bool active = editor_.modulationDragSource() == source_;
        paint::fillRounded(canvas, r, 2, active ? paint::withAlpha(sourceColor(source_), 90) : 0xff17202a);
        // Activity: the source's current value fills the handle from the bottom while a note sounds (macros always).
        const auto* synth = editor_.synth();
        const auto live = synth->liveState();
        float level = 0;
        if (source_ >= W::Env1 && source_ <= W::Env4 && live.active) level = live.envelopeLevel[(size_t) (source_ - W::Env1)];
        else if (source_ >= W::Lfo1 && source_ <= W::Lfo10 && live.active)
        {
            const int lfo = source_ - W::Lfo1;
            const float value = live.lfoValue[(size_t) lfo];
            level = synth->getParameter(W::lfoParam(lfo, W::Bipolar)) >= 0.5f ? (value + 1) * 0.5f : value;
        }
        else if (source_ >= W::Macro1 && source_ <= W::Macro8) level = synth->getParameter(W::macroParam(source_ - W::Macro1));
        else if (source_ == W::Velocity && live.active) level = synth->curveSourceAt(W::VelocityCurve, live.velocity);
        else if (source_ == W::Keytrack && live.active) level = synth->curveSourceAt(W::NoteCurve, (float) live.note / 127);
        level = std::clamp(level, 0.0f, 1.0f);
        if (level > 0.001f) paint::fillRounded(canvas, {r.x, r.bottom() - r.h * level, r.w, r.h * level}, 2, paint::withAlpha(sourceColor(source_), 70));
        const float cx = r.centreX(), cy = r.centreY(), arm = std::min(r.w, r.h) * 0.32f;
        const SkColor color = sourceColor(source_);
        paint::line(canvas, cx - arm, cy, cx + arm, cy, color); paint::line(canvas, cx, cy - arm, cx, cy + arm, color);
        ctl::triangle(canvas, cx + arm + 0.5f, cy, 3, 3.5f, ctl::Dir::Right, color); ctl::triangle(canvas, cx - arm - 0.5f, cy, 3, 3.5f, ctl::Dir::Left, color);
        ctl::triangle(canvas, cx, cy - arm - 0.5f, 3.5f, 3, ctl::Dir::Up, color); ctl::triangle(canvas, cx, cy + arm + 0.5f, 3.5f, 3, ctl::Dir::Down, color);
        int routes = 0;
        for (int slot = 0; slot < W::kModSlots; ++slot)
            if (std::lround(synth->getParameter(W::modParam(slot, W::ModSource))) == source_
                && std::lround(synth->getParameter(W::modParam(slot, W::ModDestination))) != W::DestOff
                && std::abs(synth->getParameter(W::modParam(slot, W::ModAmount))) > 1e-7f) ++routes;
        if (routes > 0)
        {
            SkPaint badge; badge.setAntiAlias(true); badge.setColor(color);
            canvas.drawCircle(r.right() - 3, r.y + 3, 3.6f, badge);
            paint::text(canvas, std::to_string(routes), {r.right() - 7, r.y - 1, 8, 8}, kBackground, 6.5f, Align::Centre, true);
        }
    }
    void mouseDown(const MouseEvent& event) override { if (! event.right) editor_.beginModulationDrag(source_, bounds().x + event.x, bounds().y + event.y); }
    void mouseDrag(const MouseEvent& event) override { editor_.moveModulationDrag(bounds().x + event.x, bounds().y + event.y); }
    void mouseUp(const MouseEvent& event) override { editor_.endModulationDrag(bounds().x + event.x, bounds().y + event.y); }
private:
    WavetableEditor& editor_;
    int source_;
};
class ParameterToggle final : public Widget
{
public:
    ParameterToggle(devices::DeviceProcessor* processor, int parameter, std::string label = {})
        : processor_(processor), parameter_(parameter), label_(std::move(label)) {}
    std::function<std::string(bool on)> stateLabel; // e.g. BPM / HZ
    std::function<void()> onChange;                 // after a click changed the value (may relayout the owner)
    void paint(SkCanvas& canvas, const Theme&) override
    {
        const bool on = processor_->getParameter(parameter_) > 0.5f;
        if (width() < 22)
        {
            SkPaint pen; pen.setAntiAlias(true); pen.setColor(on ? kCyan : kDim); pen.setStrokeWidth(1.8f); pen.setStyle(SkPaint::kStroke_Style);
            canvas.drawArc(SkRect::MakeXYWH(2, 3, width() - 4, height() - 5), -50, 280, false, pen);
            canvas.drawLine(width() * 0.5f, 1, width() * 0.5f, height() * 0.5f, pen);
        }
        else
        {
            paint::fillRounded(canvas, localBounds(), 2, on ? 0xff246980 : kRaised);
            paint::stroke(canvas, localBounds(), on ? kCyan : kLine);
            const std::string text = stateLabel ? stateLabel(on) : label_.empty() ? processor_->parameters()[(size_t) parameter_].name : label_;
            paint::text(canvas, text, localBounds().reduced(3, 0), kText, 9, Align::Centre, (bool) stateLabel);
        }
    }
    void mouseUp(const MouseEvent& event) override
    {
        if (localBounds().contains(event.x, event.y))
        {
            processor_->setParameter(parameter_, processor_->getParameter(parameter_) > 0.5f ? 0.0f : 1.0f);
            if (const auto changed = onChange) { changed(); return; }
        }
        repaint();
    }
private:
    devices::DeviceProcessor* processor_;
    int parameter_;
    std::string label_;
};
class ParameterChoice final : public Widget
{
public:
    ParameterChoice(devices::DeviceProcessor* processor, int parameter) : processor_(processor), parameter_(parameter) {}
    bool compact = false, stacked = false; // stacked: the name above a pair of step buttons (Serum's warp slots)
    SkColor accent = 0;                    // stacked name colour override
    std::function<void()> onImport;
    std::function<void()> onChange;
    std::function<bool(int)> choiceAllowed;
    std::function<std::string(int)> choiceLabel;
    std::function<void(juce::PopupMenu&)> extendMenu;   // extra entries (library submenus) with their own actions
    std::function<bool(int direction)> stepOverride;    // true: the arrows were handled (e.g. a library neighbour)
    void paint(SkCanvas& canvas, const Theme&) override
    {
        const auto& info = processor_->parameters()[(size_t) parameter_];
        auto r = localBounds();
        if (stacked)
        {
            const auto name = r.removeFromTop(r.h - 14);
            paint::fillRounded(canvas, name.reduced(0, 1), 2, kBackground);
            const bool off = selected() == 0;
            paint::text(canvas, upper(choiceLabel ? choiceLabel(selected()) : info.choices[(size_t) selected()]), name.reduced(2, 0),
                        accent != 0 ? accent : off ? kDim : kValue, accent != 0 ? 11.0f : 8.5f, Align::Centre, true);
            for (int side = 0; side < 2; ++side)
            {
                const Rect button{r.x + (float) side * r.w * 0.5f + (side == 0 ? 0.0f : 1.0f), r.y, r.w * 0.5f - 1, r.h};
                paint::fillRounded(canvas, button, 2, kRaised);
                ctl::triangle(canvas, button.centreX(), button.centreY(), 4, 5, side == 0 ? ctl::Dir::Left : ctl::Dir::Right, kDim);
            }
            return;
        }
        if (! compact) paint::text(canvas, info.name, r.removeFromTop(12), kDim, 9, Align::Centre);
        r.y += std::max(0.0f, (r.h - 23) * 0.5f); r.h = std::min(23.0f, r.h);
        paint::fillRounded(canvas, r.reduced(12, 1), 2, kBackground);
        paint::stroke(canvas, r.reduced(12, 1), kLine);
        const bool narrow = width() < 90; // too narrow for arrows: the whole field is the name (click for the menu)
        paint::text(canvas, choiceLabel ? choiceLabel(selected()) : info.choices[(size_t) selected()], r.reduced(narrow ? 3.0f : 18.0f, 0), kText, narrow ? 8.5f : 9.0f, Align::Centre);
        if (narrow) return;
        ctl::triangle(canvas, 5, r.centreY(), 4, 6, ctl::Dir::Left, kDim);
        ctl::triangle(canvas, width() - 5, r.centreY(), 4, 6, ctl::Dir::Right, kDim);
    }
    void mouseUp(const MouseEvent& event) override
    {
        if (ignoreNextUp_) { ignoreNextUp_ = false; return; }
        if (! localBounds().contains(event.x, event.y)) return;
        if (stacked && event.y >= height() - 14) { step(event.x < width() * 0.5f ? -1 : 1); return; }
        if (! stacked && width() >= 90 && event.x < 12) { step(-1); return; }
        if (! stacked && width() >= 90 && event.x >= width() - 12) { step(1); return; }
        const auto& info = processor_->parameters()[(size_t) parameter_];
        juce::PopupMenu menu;
        for (size_t i = 0; i < info.choices.size(); ++i)
            menu.addItem((int) i + 1, juce::String(choiceLabel ? choiceLabel((int) i) : info.choices[i]), ! choiceAllowed || choiceAllowed((int) i), (int) i == selected());
        if (extendMenu) { menu.addSeparator(); extendMenu(menu); }
        if (onImport) { menu.addSeparator(); menu.addItem(1001, "Import Wavetable..."); }
        menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
            if (result == 1001 && Widget::isAlive(this)) { if (onImport) onImport(); return; }
            if (result > 0 && Widget::isAlive(this) && (! choiceAllowed || choiceAllowed(result - 1)))
            { processor_->setParameter(parameter_, (float) (result - 1)); repaint(); if (onChange) onChange(); }
        });
    }
    void mouseDoubleClick(const MouseEvent&) override
    {
        ignoreNextUp_ = true;
        processor_->setParameter(parameter_, processor_->parameters()[(size_t) parameter_].defaultValue); repaint();
        if (onChange) onChange();
    }
    bool mouseWheel(const MouseEvent& event) override
    {
        if (std::abs(event.wheelDeltaX) > std::abs(event.wheelDeltaY)) return false;
        if (std::abs(event.wheelDeltaY) > 1e-6f) step(event.wheelDeltaY > 0 ? 1 : -1);
        return true;
    }
    bool keyPressed(const KeyEvent& event) override
    {
        if (event.keyCode == juce::KeyPress::leftKey || event.keyCode == juce::KeyPress::downKey) { step(-1); return true; }
        if (event.keyCode == juce::KeyPress::rightKey || event.keyCode == juce::KeyPress::upKey) { step(1); return true; }
        return false;
    }
private:
    int selected() const
    {
        return std::clamp((int) std::lround(processor_->getParameter(parameter_)), 0,
                          (int) processor_->parameters()[(size_t) parameter_].choices.size() - 1);
    }
    void step(int delta)
    {
        if (stepOverride) { const auto handler = stepOverride; if (handler(delta)) return; } // may rebuild this widget
        const int count = (int) processor_->parameters()[(size_t) parameter_].choices.size(); int next = std::clamp(selected() + delta, 0, count - 1);
        while (choiceAllowed && ! choiceAllowed(next)) { next += delta; if (next < 0 || next >= count) return; }
        processor_->setParameter(parameter_, (float) next); repaint();
        if (onChange) onChange();
    }
    devices::DeviceProcessor* processor_;
    int parameter_;
    bool ignoreNextUp_ = false;
};
std::pair<S, int> sectionFor(int parameter)
{
    if (parameter >= W::ParameterCount) return {S::Effects, 0};
    if (parameter >= W::LfoTypeStart) return {S::Lfos, parameter - W::LfoTypeStart};
    if (parameter == W::NoiseTypeParam) return {S::Noise, 0};
    if (parameter >= W::ArpStart) return {S::Main, 0};
    if (parameter >= W::NoisePitch) return {S::Noise, 0};
    if (parameter == W::SubPhase || parameter == W::SubContinuous) return {S::Main, 0};
    if (parameter >= W::FilterOutputStart) return {S::Filters, parameter - W::FilterOutputStart};
    if (parameter >= W::DirectGain) return {S::Routing, W::kRoutingSources};
    if (parameter >= W::SourceRoutingStart) return {S::Routing, (parameter - W::SourceRoutingStart) / W::SourceRoutingParams};
    if (parameter >= W::VoiceModeStart) return {S::Main, 0};
    if (parameter >= W::LfoRateScaleStart) return {S::Lfos, parameter - W::LfoRateScaleStart};
    if (parameter >= W::LfoAnchorStart) return {S::Lfos, parameter - W::LfoAnchorStart};
    if (parameter >= W::ModOutStart) return {S::Matrix, parameter - W::ModOutStart};
    if (parameter >= W::EnvLevelStart) return {S::Envelopes, (parameter - W::EnvLevelStart) / 2 + 1};
    if (parameter >= W::CoarseStart) return {S::Oscillators, parameter - W::CoarseStart};
    if (parameter >= W::MediaStart) return {S::Media, (parameter - W::MediaStart) / W::MediaParams};
    if (parameter >= W::MacroStart) return {S::Macros, 0};
    if (parameter >= W::ModStart) return {S::Matrix, (parameter - W::ModStart) / W::ModParams};
    if (parameter >= W::LfoStart) return {S::Lfos, (parameter - W::LfoStart) / W::LfoParams};
    if (parameter >= W::EnvStart) return {S::Envelopes, (parameter - W::EnvStart) / W::EnvParams};
    if (parameter >= W::FilterStart) return {S::Filters, (parameter - W::FilterStart) / W::FilterParams};
    if (parameter >= W::OscStart) return {S::Oscillators, (parameter - W::OscStart) / W::OscParams};
    return {S::Main, 0};
}
void panel(SkCanvas& canvas, Rect bounds, std::string_view title, bool selected = false)
{
    if (bounds.isEmpty()) return;
    paint::fillRounded(canvas, bounds, 3, kPanel);
    paint::fill(canvas, {bounds.x, bounds.y, bounds.w, 25}, kRaised);
    paint::stroke(canvas, bounds, selected ? kCyan : kLine);
    paint::text(canvas, title, {bounds.x + 26, bounds.y, bounds.w - (bounds.w < 95 ? 30 : 49), 25}, kText, 10, Align::Left, true);
}
void chooseFile(Widget* owner, const std::string& title, const std::string& pattern, std::function<void(std::string)> action)
{
    auto chooser = std::make_shared<juce::FileChooser>(juce::String(title), juce::File(), juce::String(pattern));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                        [owner, chooser, completion = std::move(action)](const juce::FileChooser& result) {
        const auto file = result.getResult();
        if (Widget::isAlive(owner) && file.existsAsFile()) completion(file.getFullPathName().toStdString());
    });
}
constexpr std::array<const char*, 4> kPresets{"Init", "Pulse Stack", "Hollow Keys", "Slow Motion"};
using E = devices::WavetableEffects;
constexpr std::array<const char*, 3> kFxBuses{"MAIN", "AUX 1", "AUX 2"}, kFxBands{"LOW", "MID", "HIGH"};
std::vector<E::Module>* effectChain(E::Configuration& configuration, int bus, const std::vector<int>& path)
{
    if (bus < 0 || bus >= E::busCount) return nullptr;
    auto* chain = &configuration.buses[(size_t) bus];
    for (size_t i = 0; i + 2 < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= chain->size() || path[i + 1] < 0 || path[i + 1] > 2) return nullptr;
        chain = &(*chain)[(size_t) path[i]].bands[(size_t) path[i + 1]];
    }
    return chain;
}
E::Module* effectModule(E::Configuration& configuration, int bus, const std::vector<int>& path)
{
    auto* chain = effectChain(configuration, bus, path);
    return chain && ! path.empty() && path.back() >= 0 && (size_t) path.back() < chain->size() ? &(*chain)[(size_t) path.back()] : nullptr;
}
std::map<int, std::string>& libraryPresets() { static std::map<int, std::string> loaded; return loaded; } // view state per instance
std::map<int, WavetablePresetBrowser::State>& presetBrowserViews() { static std::map<int, WavetablePresetBrowser::State> views; return views; }
// The installed presets. Rescanned when the browser opens or an arrow steps, so files added while Sonora runs show up;
// rebuilt editors (every preset load) reuse the last scan.
std::shared_ptr<const std::vector<plugins::SerumLibraryItem>>& presetLibrary()
{
    static std::shared_ptr<const std::vector<plugins::SerumLibraryItem>> library;
    return library;
}
const std::vector<plugins::SerumLibraryItem>& rescanPresets()
{
    presetLibrary() = std::make_shared<const std::vector<plugins::SerumLibraryItem>>(plugins::serumLibrary("Presets"));
    return *presetLibrary();
}
const std::vector<plugins::SerumLibraryItem>& cachedPresets() { return presetLibrary() ? *presetLibrary() : rescanPresets(); }
bool overlaps(Rect a, Rect b) { return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(); }
} // namespace

WavetableEditor::WavetableEditor(W* processor, int track, int device, int chain, int child)
    : processor_(processor), track_(track), device_(device), chain_(chain), child_(child)
{
    bypassMapMode = true;
    auto button = [this](std::string name, std::function<void()> action) {
        auto* result = add<DarkButton>(std::move(name), TextButton::Style::Plain);
        result->bypassMapMode = true; result->onClick = std::move(action); return result;
    };
    constexpr std::array<const char*, 5> pages{"OSC", "MIX", "FX", "MATRIX", "GLOBAL"};
    for (size_t i = 0; i < workflowButtons_.size(); ++i)
    {
        workflowButtons_[i] = button(pages[i], [this, i] { selectWorkflow((Page) i); });
        workflowButtons_[i]->isOn = [this, i] { return (size_t) workflow_ == i && ! inspector_; };
    }
    inspectButton_ = button("CONTROLS", [this] {
        if (forcedCompact_)
        {
            auto* root = static_cast<Widget*>(this); while (root->parent()) root = root->parent();
            auto& context = ctx(); const auto& metrics = context.theme.metrics;
            context.theme.metrics.detailHeight = std::clamp(740.0f, 60.0f, std::max(60.0f, root->height() - metrics.controlBarHeight - metrics.statusBarHeight - 120));
            root->layout();
            if (! Widget::isAlive(this)) return;
            if (forcedCompact_) context.statusText = "Enlarge the window to show the full synth dashboard";
            const auto saved = context.layoutChanged;
            repaint(); if (saved) saved(); return;
        }
        inspector_ = ! inspector_; layout(); repaint();
    });
    inspectButton_->isOn = [this] { return inspector_; };
    presetPrevious_ = button("<", [this] { if (! stepLibraryPreset(-1)) loadPreset((preset_ + 3) % 4); });
    presetNext_ = button(">", [this] { if (! stepLibraryPreset(1)) loadPreset((preset_ + 1) % 4); });
    presetGroupPrevious_ = button("<<", [this] { stepLibraryGroup(-1); });
    presetGroupNext_ = button(">>", [this] { stepLibraryGroup(1); });
    for (auto* jump : {presetGroupPrevious_, presetGroupNext_}) jump->isEnabled = [this] { return libraryGroups_; };
    presetButton_ = button("Init", [this] { if (presetBrowserOpen()) closePresetBrowser(); else openPresetBrowser(); });
    presetButton_->isOn = [this] { return presetBrowserOpen(); };
    presetMore_ = button("...", [this] { presetMenu(); });
    for (size_t i = 0; i < sections_.size(); ++i)
    {
        sections_[i] = button(kSections[i], [this, i] { selectSection((Section) i); });
        sections_[i]->isOn = [this, i] { return (size_t) section_ == i; };
    }
    previous_ = button("<", [this] { selectInstance(selectedInstance() - 1); });
    selector_ = button("", [this] { instanceMenu(); });
    next_ = button(">", [this] { selectInstance(selectedInstance() + 1); });
    pagePrevious_ = button("<", [this] { page_ = (page_ + pageCount() - 1) % std::max(1, pageCount()); layout(); repaint(); });
    pageNext_ = button(">", [this] { page_ = (page_ + 1) % std::max(1, pageCount()); layout(); repaint(); });
    for (size_t i = 0; i < oscillatorMore_.size(); ++i)
    {
        oscillatorMore_[i] = button("...", [this, i] {
            const auto section = processor_->getParameter(W::mediaParam((int) i, W::MediaMode)) > 0.5f ? Section::Media : Section::Oscillators;
            instances_[(size_t) section] = (int) i; selectSection(section);
        });
        mediaImport_[i] = button("IMPORT SAMPLE...", [this, i] { importOscillator((int) i, true); });
    }
    for (size_t i = 0; i < filterMore_.size(); ++i)
        filterMore_[i] = button("...", [this, i] { instances_[(size_t) Section::Filters] = (int) i; selectSection(Section::Filters); });
    for (size_t i = 0; i < envelopeButtons_.size(); ++i)
    {
        envelopeButtons_[i] = button("ENV " + std::to_string(i + 1), [this, i] { instances_[(size_t) Section::Envelopes] = (int) i; layout(); repaint(); });
        envelopeButtons_[i]->isOn = [this, i] { return instances_[(size_t) Section::Envelopes] == (int) i; };
    }
    for (size_t i = 0; i < lfoButtons_.size(); ++i)
    {
        lfoButtons_[i] = button(std::to_string(i + 1), [this, i] { instances_[(size_t) Section::Lfos] = (int) i; layout(); repaint(); });
        lfoButtons_[i]->isOn = [this, i] { return instances_[(size_t) Section::Lfos] == (int) i; };
    }
    envelopeMore_ = button("...", [this] { selectSection(Section::Envelopes); });
    lfoMore_ = button("...", [this] { selectSection(Section::Lfos); });
    macroMore_ = button("...", [this] { selectSection(Section::Macros); });
    subRouting_ = button("Routing...", [this] {
        mixSources_ = true;
        const int key = fxViewKey(); if (key >= 0) ctx().wavetableRoutingViews[key].sources = true;
        selectWorkflow(Page::Mix);
        for (int page = 0; page < routingEditor_->pageCount() && routingEditor_->parameterCell(4, WavetableRoutingEditor::Field::Destination).isEmpty(); ++page)
            routingEditor_->restorePage((routingEditor_->page() + 1) % routingEditor_->pageCount());
        if (key >= 0) ctx().wavetableRoutingViews[key].page = routingEditor_->page();
        repaint();
    });
    voiceQuality_ = button("Voice Quality  1×", [this] { voiceQualityMenu(); });
    noiseAsset_ = button("Generated Noise", [this] { noiseAssetMenu(); });
    noiseImport_ = button("Import...", [this] { chooseNoiseFile(); });
    noiseMore_ = button("...", [this] { selectSection(Section::Noise); });
    buildEffects();
    buildControls();
    buildModulation();
    buildRouting();
    prepareNoisePreview();
    master_ = add<PanelKnob>("MAIN", parameterSource(processor_, W::Volume), processor_, W::Volume); tag(master_, W::Volume);
    keysButton_ = button("KEYS", [this] { arpView_ = false; layout(); repaint(); });
    keysButton_->isOn = [this] { return ! arpView_; };
    arpButton_ = button("ARP", [this] { arpView_ = true; layout(); repaint(); });
    arpButton_->isOn = [this] { return arpView_; };
    constexpr std::array<const char*, 3> voiceTabs{"VOICE", "VELO", "NOTE"};
    for (size_t tab = 0; tab < voiceTabs_.size(); ++tab)
    {
        voiceTabs_[tab] = button(voiceTabs[tab], [this, tab] { voiceTab_ = (int) tab; layout(); repaint(); });
        voiceTabs_[tab]->isOn = [this, tab] { return voiceTab_ == (int) tab; };
    }
    constexpr std::array<const char*, 3> lfoModes{"FREE", "RETRIG", "ENVELOPE"};
    for (size_t mode = 0; mode < lfoModes_.size(); ++mode)
    {
        lfoModes_[mode] = button(lfoModes[mode], [this, mode] {
            processor_->setParameter(W::lfoParam(instances_[(size_t) Section::Lfos], W::Mode), (float) mode); repaint();
        });
        lfoModes_[mode]->isOn = [this, mode] {
            return (int) std::lround(processor_->getParameter(W::lfoParam(instances_[(size_t) Section::Lfos], W::Mode))) == (int) mode;
        };
    }
    for (int lfo = 0; lfo < W::kLfos; ++lfo)
    {
        const int division = W::lfoParam(lfo, W::Division);
        Knob::Source source;
        source.get = [this, division] { return processor_->getParameter(division); };
        source.set = [this, division](float value) { processor_->setParameter(division, std::round(value)); };
        source.minValue = 0; source.maxValue = (float) (W::kLfoDivisions - 1); source.defaultValue = processor_->parameters()[(size_t) division].defaultValue;
        source.format = [choices = processor_->parameters()[(size_t) division].choices](float value) {
            return choices[(size_t) std::clamp((int) std::lround(value), 0, (int) choices.size() - 1)];
        };
        auto* knob = add<PanelKnob>("Rate", source, processor_, division);
        knob->visible = false; tag(knob, division); lfoDivisionKnobs_[(size_t) lfo] = knob;
    }
    // Source handles last: they sit above the tabs and macro knobs they belong to.
    modHandles_.assign((size_t) W::NumSources, nullptr);
    for (int source = W::Env1; source <= W::Lfo10; ++source) modHandles_[(size_t) source] = add<ModSourceHandle>(*this, source);
    for (int source = W::Macro1; source <= W::Macro8; ++source) modHandles_[(size_t) source] = add<ModSourceHandle>(*this, source);
    for (int source : {(int) W::Velocity, (int) W::Keytrack}) modHandles_[(size_t) source] = add<ModSourceHandle>(*this, source);
    presetValues_.reserve(processor_->parameters().size());
    for (size_t i = 0; i < processor_->parameters().size(); ++i)
    {
        const float value = processor_->getParameter((int) i); presetValues_.push_back(value);
        if (std::abs(value - processor_->parameters()[i].defaultValue) > 1e-6f) presetLabel_ = "Current Patch";
    }
    // The preset browser is the last child: it covers the dashboard (layoutPresetBrowser hides what lies beneath).
    presetBrowser_ = add<WavetablePresetBrowser>(); presetBrowser_->visible = false;
    presetBrowser_->onLoad = [this](const std::string& path) {
        // Copied to the stack first: a rebuild destroys this lambda (and its captures) along with the browser.
        auto* self = this; const auto alive = alive_; const auto* browser = presetBrowser_;
        self->storePresetBrowser(); self->loadLibraryPreset(path); // may rebuild this editor (and its browser)
        if (*alive && Widget::isAlive(browser)) self->presetBrowser_->setCurrent(self->libraryPresetPath());
    };
    presetBrowser_->onClose = [this] { closePresetBrowser(); };
}
WavetableEditor::~WavetableEditor() { *alive_ = false; releaseNote(); }
void WavetableEditor::buildControls()
{
    const auto& parameters = processor_->parameters();
    for (size_t p = 0; p < parameters.size(); ++p)
    {
        const auto& info = parameters[p]; auto [section, instance] = sectionFor((int) p);
        if (section == Section::Effects)
            for (size_t i = 0; i < fxInspectorIds_.size(); ++i)
            {
                const auto id = fxInspectorIds_[i];
                const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [id](const auto& node) { return node.id == id; });
                if (found == fxNodes_.end()) continue;
                bool belongs = processor_->fxEnabledParameterIndex(id) == (int) p;
                const auto descriptors = Effects::parameterDescriptors(found->type);
                for (size_t d = 0; d < descriptors.size(); ++d) belongs |= processor_->fxParameterIndex(id, (int) d) == (int) p;
                if (belongs) { instance = (int) i + 1; break; }
            }
        std::string toggleLabel = (int) p >= W::LfoRateScaleStart && (int) p < W::VoiceModeStart ? "10x" : "";
        if (section == Section::Oscillators && (int) p < W::FilterStart && ((int) p - W::OscStart) % W::OscParams == W::RouteFilter1) toggleLabel = "F1";
        if (section == Section::Oscillators && (int) p < W::FilterStart && ((int) p - W::OscStart) % W::OscParams == W::RouteFilter2) toggleLabel = "F2";
        if ((int) p == W::arpParam(W::ArpOn)) toggleLabel = "ON";
        Widget* control = info.isToggle ? static_cast<Widget*>(add<ParameterToggle>(processor_, (int) p, toggleLabel))
                          : ! info.choices.empty() ? static_cast<Widget*>(add<ParameterChoice>(processor_, (int) p))
                                                   : static_cast<Widget*>(add<PanelKnob>(info.name, parameterSource(processor_, (int) p), processor_, (int) p));
        tag(control, (int) p); controls_.push_back({(int) p, instance, info.choices.empty() ? 1 : 2, section, control});
        if (auto* knob = dynamic_cast<PanelKnob*>(control))
            knob->onRingHover = [this, parameter = (int) p](int slot) { setRingTip(slot >= 0 ? RingTip{parameter, slot} : RingTip{}); };
        if (auto* toggle = dynamic_cast<ParameterToggle*>(control); toggle && section == Section::Lfos && (int) p < W::ModStart
            && ((int) p - W::LfoStart) % W::LfoParams == W::SyncOn)
            toggle->onChange = [this] { layout(); repaint(); }; // BPM <-> HZ swaps the RATE knob
        if (auto* choice = dynamic_cast<ParameterChoice*>(control); choice && (int) p >= W::LfoTypeStart && (int) p < W::ParameterCount)
            choice->onChange = [this] { layout(); repaint(); }; // chaos types hide the curve Shape menu
        if (auto* choice = dynamic_cast<ParameterChoice*>(control); choice && section == Section::Media
            && ((int) p - W::MediaStart) % W::MediaParams == W::MediaMode)
        {
            choice->onChange = [this] { layout(); repaint(); };
            choice->choiceLabel = [choices = info.choices](int mode) { return upper(choices[(size_t) mode]); };
        }
        if (auto* choice = dynamic_cast<ParameterChoice*>(control); choice && section == Section::Oscillators
            && ((int) p - W::OscStart) % W::OscParams == W::Table)
        {
            choice->onImport = [this, instance] { importOscillator(instance, false); };
            choice->extendMenu = [this, instance](juce::PopupMenu& menu) {
                addLibraryMenu(menu, "Tables", plugins::serumLibrary("Tables"), [this, instance, alive = this](const std::string& path) {
                    if (Widget::isAlive(alive)) loadLibraryTable(instance, path);
                });
            };
            choice->stepOverride = [this, instance](int direction) { return stepLibraryTable(instance, direction); };
            choice->choiceLabel = [this, instance, choices = info.choices](int table) {
                // An imported or loaded table shows its own name rather than "Custom".
                if (table == W::Custom && processor_->customTable(instance) != nullptr) return processor_->customTableName(instance);
                return choices[(size_t) table];
            };
        }
        if (auto* choice = dynamic_cast<ParameterChoice*>(control); choice && section == Section::Oscillators && (int) p < W::FilterStart
            && (((int) p - W::OscStart) % W::OscParams == W::Warp1Mode || ((int) p - W::OscStart) % W::OscParams == W::Warp2Mode))
        {
            const int warp = ((int) p - W::OscStart) % W::OscParams == W::Warp1Mode ? 0 : 1;
            choice->choiceLabel = [this, instance, warp, choices = info.choices](int mode) {
                // A preset's own warp name: the native stand-in it maps to, or "NAME *" when there is none yet.
                const auto imported = processor_->importedWarpLabel(instance, warp);
                if (! imported.label.empty() && mode == imported.mode) return imported.label + (imported.mode == W::WarpOff ? " *" : "");
                return choices[(size_t) mode];
            };
        }
        if (auto* choice = dynamic_cast<ParameterChoice*>(control); choice && section == Section::Routing && instance >= 5 && instance < 7
            && (int) p == W::sourceRoutingParam(instance, W::SourceDestination))
        {
            choice->choiceAllowed = [this, instance](int destination) { return routingDestinationAllowed(instance, destination); };
            const auto choices = info.choices;
            choice->choiceLabel = [choices](int destination) { return destination == W::RouteToFilters ? "Other Filter" : choices[(size_t) destination]; };
        }
    }
}
void WavetableEditor::buildEffects()
{
    fxConfiguration_ = processor_->effectsConfiguration();
    auto button = [this](std::string label, std::function<void()> action) {
        auto* result = add<DarkButton>(std::move(label), TextButton::Style::Plain);
        result->bypassMapMode = true; result->onClick = std::move(action); return result;
    };
    std::function<void(const std::vector<Effects::Module>&, int, std::vector<int>)> visit;
    visit = [&](const auto& modules, int bus, std::vector<int> prefix) {
        for (size_t i = 0; i < modules.size(); ++i)
        {
            const auto& module = modules[i]; auto path = prefix; path.push_back((int) i);
            const auto id = module.id;
            std::string label = path.size() > 1 ? std::string(kFxBands[(size_t) path[path.size() - 2]]) + " / " : "";
            label += Effects::typeName(module.type);
            auto* select = button(label, [this, id] { selectFxModule(id); });
            select->isOn = [this, id] { return fxSelected_ == id; };
            fxNodes_.push_back({id, module.type, bus, path, select}); fxInspectorIds_.push_back(id);
            if (module.type == Effects::Type::Split3)
                for (int band = 0; band < 3; ++band)
                { auto nested = path; nested.push_back(band); visit(module.bands[(size_t) band], bus, std::move(nested)); }
        }
    };
    for (int bus = 0; bus < Effects::busCount; ++bus) visit(fxConfiguration_.buses[(size_t) bus], bus, {});
    std::sort(fxInspectorIds_.begin(), fxInspectorIds_.end());
    for (int bus = 0; bus < Effects::busCount; ++bus)
    {
        fxBusButtons_[(size_t) bus] = button(kFxBuses[(size_t) bus], [this, bus] { selectFxBus(bus); });
        fxBusButtons_[(size_t) bus]->isOn = [this, bus] { return fxBus_ == bus; };
        mixOpenButtons_[(size_t) bus] = button(kFxBuses[(size_t) bus], [this, bus] { selectFxBus(bus); selectWorkflow(Page::Fx); });
        fxBandButtons_[(size_t) bus] = button(std::string("+ ") + kFxBands[(size_t) bus], [this, bus] {
            fxBand_ = bus; storeFxView(); showFxTypeMenu();
        });
    }
    fxAdd_ = button("+ ADD EFFECT", [this] { fxBand_ = -1; showFxTypeMenu(); });
    fxRemove_ = button("REMOVE", [this] { removeEffect(); });
    fxUp_ = button("UP", [this] { moveEffect(-1); }); fxDown_ = button("DOWN", [this] { moveEffect(1); });
    fxChainPrevious_ = button("<", [this] { fxChainPage_ = (fxChainPage_ + fxChainPageCount() - 1) % fxChainPageCount(); storeFxView(); layout(); repaint(); });
    fxChainNext_ = button(">", [this] { fxChainPage_ = (fxChainPage_ + 1) % fxChainPageCount(); storeFxView(); layout(); repaint(); });
    fxControlsPrevious_ = button("<", [this] { fxControlsPage_ = (fxControlsPage_ + fxControlsPages_ - 1) % fxControlsPages_; storeFxView(); layout(); repaint(); });
    fxControlsNext_ = button(">", [this] { fxControlsPage_ = (fxControlsPage_ + 1) % fxControlsPages_; storeFxView(); layout(); repaint(); });
    fxImpulse_ = button("LOAD IMPULSE...", [this] { importImpulse(); });
    fxEnableBuses_ = button("ENABLE AUX BUSES", [this] { const auto configuration = processor_->effectsConfiguration(); publishEffects(configuration); });
}
void WavetableEditor::buildModulation()
{
    using M = WavetableModulationEditor;
    M::Binding binding;
    binding.parameter = [this](int slot, M::Field field) {
        const std::array<int, (size_t) M::Field::Count> indices{W::modParam(slot, W::ModSource), W::modParam(slot, W::AuxSource),
            W::modParam(slot, W::ModDestination), W::modParam(slot, W::ModAmount), W::modParam(slot, W::ModBipolar), W::outParam(slot)};
        M::ParameterBinding bound; bound.index = indices[(size_t) field]; bound.info = processor_->parameters()[(size_t) bound.index];
        if (chain_ < 0) bound.target = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, track_, device_, 0, bound.info.id};
        return bound;
    };
    binding.getParameter = [this](int index) { return processor_->getParameter(index); };
    binding.setParameter = [this](int index, float value) { processor_->setParameter(index, value); };
    binding.curve = [this](int slot, bool auxiliary) {
        const auto snapshot = processor_->modulationCurve(slot, auxiliary);
        M::CurveSnapshot result; result.graph = snapshot.graph; result.enabled = snapshot.enabled;
        result.law = snapshot.law == W::CurveLaw::ImportedExponential ? M::CurveLaw::Imported : M::CurveLaw::Native; return result;
    };
    binding.destination = [this](int slot) { return "native/" + std::to_string((int) std::lround(processor_->getParameter(W::modParam(slot, W::ModDestination)))); };
    binding.targets = [] {
        std::vector<M::Target> targets;
        for (int destination = 0; destination < W::NumDestinations; ++destination)
            targets.push_back({"native/" + std::to_string(destination), W::destinationName(destination), ""});
        return targets;
    };
    binding.commitDestination = [this](int slot, const std::string& id) {
        for (int destination = 0; destination < W::NumDestinations; ++destination)
            if (id == "native/" + std::to_string(destination)) { processor_->setParameter(W::modParam(slot, W::ModDestination), (float) destination); repaint(); return; }
    };
    binding.commitCurve = [this](int slot, bool auxiliary, const dsp::CurveGraph& graph, bool enabled) {
        const auto action = ctx().editWavetableModulationCurve;
        if (! action) { ctx().statusText = "Curve editing is unavailable"; repaint(); return; }
        const int track = track_, device = device_, chain = chain_, child = child_;
        action(track, device, slot, auxiliary, graph, enabled, chain, child);
    };
    binding.parameterContext = [this](int index) {
        if (index < 0 || (size_t) index >= controls_.size()) return;
        if (const auto* knob = dynamic_cast<Knob*>(controls_[(size_t) index].widget))
        { const auto action = knob->onRightClick; if (action) action(); }
    };
    binding.onViewChange = [this](const M::ViewState& view) {
        const int key = fxViewKey(); if (key < 0) return;
        auto& saved = ctx().wavetableModulationViews[key]; saved.page = view.page; saved.selectedSlot = view.selectedSlot;
        saved.auxiliary = view.auxiliary; saved.curveOpen = view.curveOpen;
    };
    modulationEditor_ = add<M>(std::move(binding)); modulationEditor_->visible = false;
}
bool WavetableEditor::routingDestinationAllowed(int source, int destination) const
{
    if (source < 5 || destination != W::RouteToFilters) return true;
    const int other = source == 5 ? 6 : 5;
    return processor_->getParameter(W::sourceRoutingParam(other, W::SourceDestination)) > 0.5f;
}
void WavetableEditor::buildRouting()
{
    using R = WavetableRoutingEditor;
    const auto boundParameter = [this](int index) {
        R::ParameterBinding bound; bound.index = index;
        if (index < 0) return bound;
        bound.info = processor_->parameters()[(size_t) index];
        bound.quadraticGain = quadraticGain(bound.info.id);
        if (chain_ < 0) bound.target = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, track_, device_, 0, bound.info.id};
        return bound;
    };
    R::Binding binding;
    binding.parameter = [boundParameter](int source, R::Field field) {
        const std::array<W::SourceRoutingParam, 5> fields{W::SourceDestination, W::SourceFilterBalance, W::SourceBus1, W::SourceBus2, W::SourceEnvelope};
        const int index = field == R::Field::Level ? (source >= 5 ? W::filterOutputParam(source - 5) : -1)
            : W::sourceRoutingParam(source, fields[(size_t) field]);
        auto bound = boundParameter(index);
        if (field == R::Field::Destination) bound.info.choices = {source >= 5 ? "Other Filter" : "Filters", "Main FX", "Direct", "Off"};
        return bound;
    };
    binding.headerParameter = [boundParameter](bool direct) { return boundParameter(direct ? W::DirectGain : W::SourceRoutingOn); };
    binding.get = [this](int index) { return processor_->getParameter(index); };
    binding.set = [this](int index, float value) { processor_->setParameter(index, value); };
    binding.destinationAllowed = [this](int source, int destination) { return routingDestinationAllowed(source, destination); };
    binding.levelActive = [this](int source) { return source >= 5 && processor_->getParameter(W::filterParam(source - 5, W::FilterOn)) > 0.5f; };
    binding.returns = [this] { return fxConfiguration_.returnBeforeMain; };
    binding.commitReturn = [this](int bus, bool before) {
        auto configuration = processor_->effectsConfiguration(); configuration.returnBeforeMain[(size_t) bus] = before;
        publishEffects(configuration);
    };
    binding.parameterContext = [this](int index) {
        if (index < 0 || (size_t) index >= controls_.size()) return;
        if (const auto* knob = dynamic_cast<Knob*>(controls_[(size_t) index].widget)) { const auto action = knob->onRightClick; if (action) action(); }
    };
    binding.onPageChange = [this](int page) { const int key = fxViewKey(); if (key >= 0) ctx().wavetableRoutingViews[key].page = page; };
    routingEditor_ = add<R>(std::move(binding)); routingEditor_->visible = false;
    const auto button = [this](const char* label, bool sources) {
        auto* result = add<DarkButton>(label, TextButton::Style::Plain); result->bypassMapMode = true;
        result->isOn = [this, sources] { return mixSources_ == sources; };
        result->onClick = [this, sources] { mixSources_ = sources; const int key = fxViewKey(); if (key >= 0) ctx().wavetableRoutingViews[key].sources = sources; layout(); repaint(); };
        return result;
    };
    mixSourcesButton_ = button("SOURCES", true); mixBusesButton_ = button("BUS FX", false);
}
void WavetableEditor::restoreModulationView()
{
    if (modulationViewLoaded_) return;
    modulationViewLoaded_ = true;
    if (const auto found = ctx().wavetableModulationViews.find(fxViewKey()); found != ctx().wavetableModulationViews.end())
    {
        const auto& saved = found->second;
        modulationEditor_->restoreViewState({saved.page, saved.selectedSlot, saved.auxiliary, saved.curveOpen});
    }
}
int WavetableEditor::fxViewKey() const
{
    const auto* track = ctx().trackAt(track_);
    if (! track || device_ < 0 || (size_t) device_ >= track->devices.size()) return -1;
    const auto* device = &track->devices[(size_t) device_];
    if (chain_ >= 0)
    {
        if ((size_t) chain_ >= device->chains.size() || child_ < 0 || (size_t) child_ >= device->chains[(size_t) chain_].devices.size()) return -1;
        device = &device->chains[(size_t) chain_].devices[(size_t) child_];
    }
    return device->id;
}
void WavetableEditor::restoreFxView()
{
    if (fxViewLoaded_) return;
    fxViewLoaded_ = true;
    if (const auto found = ctx().wavetableFxViews.find(fxViewKey()); found != ctx().wavetableFxViews.end())
    {
        const auto& view = found->second; fxBus_ = std::clamp(view.bus, 0, 2); fxSelected_ = view.module;
        fxBand_ = view.band; fxChainPage_ = std::max(0, view.chainPage); fxControlsPage_ = std::max(0, view.controlsPage);
        workflow_ = (Page) std::clamp(view.workflow, 0, (int) Page::Count - 1);
        if (workflow_ == Page::Fx) section_ = Section::Effects;
        if (workflow_ == Page::Mix || workflow_ == Page::Global) section_ = Section::Main;
        if (workflow_ == Page::Matrix) section_ = Section::Matrix;
        if (view.inspectorState)
        {
            inspector_ = view.inspector;
            section_ = (Section) std::clamp(view.section, 0, (int) Section::Count - 1);
            instances_[(size_t) section_] = std::clamp(view.instance, 0, instanceCount() - 1);
            page_ = std::max(0, view.page);
        }
    }
    const auto chain = currentFxChain();
    if (! selectedFxNode() && ! chain.empty()) fxSelected_ = chain.front()->id;
    if (selectedFxNode())
    {
        const auto found = std::find(fxInspectorIds_.begin(), fxInspectorIds_.end(), fxSelected_);
        instances_[(size_t) Section::Effects] = found == fxInspectorIds_.end() ? 0 : (int) (found - fxInspectorIds_.begin()) + 1;
    }
}
void WavetableEditor::storeFxView()
{
    const int key = fxViewKey(); if (key < 0) return;
    auto& view = ctx().wavetableFxViews[key]; view.bus = fxBus_; view.module = fxSelected_; view.band = fxBand_;
    view.chainPage = fxChainPage_; view.controlsPage = fxControlsPage_; view.workflow = (int) workflow_;
    if (view.inspectorState)
    { view.inspector = inspector_; view.section = (int) section_; view.instance = selectedInstance(); view.page = page_; }
}
const WavetableEditor::FxNode* WavetableEditor::selectedFxNode() const
{
    const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [this](const auto& node) { return node.id == fxSelected_ && node.bus == fxBus_; });
    return found == fxNodes_.end() ? nullptr : &*found;
}
std::vector<const WavetableEditor::FxNode*> WavetableEditor::currentFxChain() const
{
    std::vector<const FxNode*> result;
    for (const auto& node : fxNodes_) if (node.bus == fxBus_) result.push_back(&node);
    return result;
}
int WavetableEditor::fxChainPageCount() const { return std::max(1, ((int) currentFxChain().size() + fxRows_ - 1) / fxRows_); }
void WavetableEditor::selectFxBus(int bus)
{
    fxBus_ = std::clamp(bus, 0, 2); fxChainPage_ = fxControlsPage_ = 0; fxSelected_ = 0; fxBand_ = -1;
    const auto chain = currentFxChain(); if (! chain.empty()) fxSelected_ = chain.front()->id;
    const auto found = std::find(fxInspectorIds_.begin(), fxInspectorIds_.end(), fxSelected_);
    instances_[(size_t) Section::Effects] = found == fxInspectorIds_.end() ? 0 : (int) (found - fxInspectorIds_.begin()) + 1;
    storeFxView(); layout(); repaint();
}
void WavetableEditor::selectFxModule(Effects::ModuleId id)
{
    fxSelected_ = id; fxBand_ = -1; fxControlsPage_ = 0;
    const auto found = std::find(fxInspectorIds_.begin(), fxInspectorIds_.end(), id);
    instances_[(size_t) Section::Effects] = found == fxInspectorIds_.end() ? 0 : (int) (found - fxInspectorIds_.begin()) + 1;
    storeFxView(); layout(); repaint();
}
void WavetableEditor::showFxTypeMenu()
{
    const auto select = [this](Effects::Type type) { if (Widget::isAlive(this)) insertEffect(type); };
    if (onChooseEffect) { const auto chooser = onChooseEffect; chooser(select); return; }
    juce::PopupMenu menu;
    for (int code = 0; code <= (int) Effects::Type::Split3; ++code)
        if (code != (int) Effects::Type::Unknown) menu.addItem(code + 1, juce::String(Effects::typeName((Effects::Type) code).data()));
    menu.showMenuAsync(juce::PopupMenu::Options(), [select](int result) { if (result > 0) select((Effects::Type) (result - 1)); });
}
void WavetableEditor::publishEffects(const Effects::Configuration& configuration)
{
    const auto action = ctx().setWavetableEffects;
    if (! action) { ctx().statusText = "Effects editing is unavailable"; repaint(); return; }
    storeFxView(); const int track = track_, device = device_, chain = chain_, child = child_;
    action(track, device, configuration, chain, child);
}
void WavetableEditor::insertEffect(Effects::Type type)
{
    if (type == Effects::Type::Unknown) return;
    if (fxNodes_.size() >= (size_t) Effects::maxModules) { ctx().statusText = "The effects rack is full"; repaint(); return; }
    auto configuration = processor_->effectsConfiguration();
    auto* target = &configuration.buses[(size_t) fxBus_]; size_t position = target->size(); bool nested = false;
    if (const auto* selected = selectedFxNode())
    {
        if (selected->type == Effects::Type::Split3 && fxBand_ >= 0)
        {
            if ((selected->path.size() - 1) / 2 >= (size_t) Effects::maxDepth) { ctx().statusText = "This band has reached its nesting limit"; repaint(); return; }
            auto* split = effectModule(configuration, selected->bus, selected->path); if (! split) return;
            target = &split->bands[(size_t) fxBand_]; position = target->size(); nested = true;
        }
        else
        { target = effectChain(configuration, selected->bus, selected->path); position = (size_t) selected->path.back() + 1; nested = selected->path.size() > 1; }
    }
    if (! target) return;
    if (target->size() >= (size_t) (nested ? Effects::maxBandModules : Effects::maxBusModules))
    { ctx().statusText = "This effects chain is full"; repaint(); return; }
    Effects::Module module; module.type = type;
    module.id = 1;
    while (std::find(fxInspectorIds_.begin(), fxInspectorIds_.end(), module.id) != fxInspectorIds_.end()) ++module.id;
    for (const auto& descriptor : Effects::parameterDescriptors(type)) module.values.push_back(descriptor.initial);
    fxSelected_ = module.id; fxControlsPage_ = 0;
    target->insert(target->begin() + (ptrdiff_t) position, std::move(module)); publishEffects(configuration);
}
void WavetableEditor::removeEffect()
{
    const auto* selected = selectedFxNode(); if (! selected) return;
    auto configuration = processor_->effectsConfiguration(); auto* chain = effectChain(configuration, selected->bus, selected->path); if (! chain) return;
    const auto index = (size_t) selected->path.back(); chain->erase(chain->begin() + (ptrdiff_t) index);
    fxSelected_ = chain->empty() ? 0 : (*chain)[std::min(index, chain->size() - 1)].id; fxBand_ = -1; fxControlsPage_ = 0;
    publishEffects(configuration);
}
void WavetableEditor::moveEffect(int direction)
{
    const auto* selected = selectedFxNode(); if (! selected) return;
    auto configuration = processor_->effectsConfiguration(); auto* chain = effectChain(configuration, selected->bus, selected->path); if (! chain) return;
    const int index = selected->path.back(), next = index + direction;
    if (next < 0 || (size_t) next >= chain->size()) return;
    std::swap((*chain)[(size_t) index], (*chain)[(size_t) next]); publishEffects(configuration);
}
void WavetableEditor::importImpulse()
{
    const auto* selected = selectedFxNode(); if (! selected || selected->type != Effects::Type::Convolution) return;
    const auto action = ctx().importWavetableImpulseResponse; if (! action) return;
    storeFxView(); const auto id = selected->id; const int track = track_, device = device_, chain = chain_, child = child_;
    chooseFile(this, "Load Impulse Response", engine::audioFileWildcard().toStdString(),
               [action, track, device, id, chain, child](std::string path) { action(track, device, id, path, chain, child); });
}
void WavetableEditor::importOscillator(int oscillator, bool media)
{
    if (media && onImportMedia) { onImportMedia(oscillator); return; }
    if (! media && onImportTable) { onImportTable(oscillator); return; }
    const auto action = media ? ctx().importOscillatorMedia : ctx().importWavetable;
    if (! action) return;
    const int track = track_, device = device_, chain = chain_, child = child_;
    chooseFile(this, media ? "Import Oscillator Media" : "Import Wavetable",
               (engine::audioFileWildcard() + (media ? ";*.sfz" : "")).toStdString(),
               [action, track, device, oscillator, chain, child](std::string path) { action(track, device, oscillator, path, chain, child); });
}

void WavetableEditor::tag(Widget* widget, int parameter)
{
    const auto info = processor_->parameters()[(size_t) parameter];
    if (chain_ < 0)
    {
        widget->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, track_, device_, 0, info.id};
        return;
    }
    if (auto* knob = dynamic_cast<Knob*>(widget))
        knob->onRightClick = [this, info] {
            const int macro = ctx().rackMacroToMap, chain = chain_, child = child_;
            if (ctx().editDevice) ctx().editDevice(track_, device_, [macro, chain, child, info](auto& rack) {
                if ((size_t) chain >= rack.chains.size() || (size_t) child >= rack.chains[(size_t) chain].devices.size()) return;
                auto& target = rack.chains[(size_t) chain].devices[(size_t) child];
                if (std::none_of(target.parameters.begin(), target.parameters.end(), [&](const auto& p) { return p.path == info.id; }))
                {
                    model::Parameter p; p.path = info.id; p.value = info.defaultValue; p.booleanValue = info.isToggle;
                    target.parameters.push_back(std::move(p));
                }
                for (auto& p : target.parameters) if (p.path == info.id)
                {
                    p.macroIndex = macro; p.rangeMin = info.minValue; p.rangeMax = info.maxValue;
                    p.midi.channel = 16; p.midi.isNote = false; p.midi.number = macro; p.macroMappingEdited = true;
                }
                rack.chainsEdited = true;
            }, "Map Rack Macro");
        };
}

int WavetableEditor::instanceCount() const
{
    switch (section_)
    {
        case Section::Oscillators: case Section::Media: return W::kOscillators;
        case Section::Filters: return W::kFilters;
        case Section::Envelopes: return W::kEnvelopes;
        case Section::Lfos: return W::kLfos;
        case Section::Matrix: return W::kModSlots;
        case Section::Routing: return W::kRoutingSources + 1;
        case Section::Effects: return (int) fxInspectorIds_.size() + 1;
        case Section::Macros: case Section::Main: case Section::Noise: case Section::Count: return 1;
    }
    return 1;
}
std::string WavetableEditor::instanceLabel(int instance) const
{
    if (section_ == Section::Effects)
    {
        if (instance <= 0 || (size_t) instance > fxInspectorIds_.size()) return "Bus Routing";
        const auto id = fxInspectorIds_[(size_t) instance - 1];
        const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [id](const auto& node) { return node.id == id; });
        return found == fxNodes_.end() ? "Effect" : std::string(Effects::typeName(found->type)) + " #" + std::to_string(id);
    }
    if (section_ == Section::Routing)
    {
        constexpr std::array<const char*, 8> names{"Osc A", "Osc B", "Osc C", "Noise", "Sub", "Filter 1", "Filter 2", "Routing Global"};
        return names[(size_t) std::clamp(instance, 0, 7)];
    }
    if (section_ == Section::Noise) return "Noise";
    const char* label = section_ == Section::Oscillators || section_ == Section::Media ? "Oscillator" : section_ == Section::Filters ? "Filter"
                        : section_ == Section::Envelopes ? "Envelope" : section_ == Section::Lfos ? "LFO" : "Matrix Slot";
    return std::string(label) + " " + std::to_string(instance + 1);
}
void WavetableEditor::selectSection(Section section)
{
    inspector_ = true; matrixCompact_ = mixCompact_ = false; section_ = section; page_ = 0; layout(); repaint();
}
void WavetableEditor::selectInstance(int instance)
{
    instances_[(size_t) section_] = (instance + instanceCount()) % instanceCount(); page_ = 0;
    if (section_ == Section::Effects && selectedInstance() > 0)
    {
        fxSelected_ = fxInspectorIds_[(size_t) selectedInstance() - 1];
        const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [this](const auto& node) { return node.id == fxSelected_; });
        if (found != fxNodes_.end()) fxBus_ = found->bus;
        storeFxView();
    }
    else if (section_ == Section::Effects) { fxSelected_ = 0; fxBand_ = -1; storeFxView(); }
    layout(); repaint();
}
void WavetableEditor::selectWorkflow(Page page)
{
    releaseNote(); workflow_ = page; inspector_ = forcedCompact_;
    if (page == Page::Matrix) section_ = Section::Matrix;
    if (page == Page::Matrix) matrixCompact_ = true;
    if (page == Page::Mix) mixCompact_ = true;
    if (page == Page::Global || page == Page::Mix) section_ = Section::Main;
    if (page == Page::Fx) section_ = Section::Effects;
    if (page == Page::Osc) section_ = Section::Oscillators;
    storeFxView();
    layout(); repaint();
}
void WavetableEditor::instanceMenu()
{
    juce::PopupMenu menu;
    for (int i = 0; i < instanceCount(); ++i) menu.addItem(i + 1, juce::String(instanceLabel(i)), true, i == selectedInstance());
    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) { if (result > 0 && Widget::isAlive(this)) selectInstance(result - 1); });
}
void WavetableEditor::presetMenu()
{
    juce::PopupMenu menu;
    for (size_t i = 0; i < kPresets.size(); ++i) menu.addItem((int) i + 1, kPresets[i], true, (int) i == preset_);
    menu.addSeparator();
    addLibraryMenu(menu, "Presets", plugins::serumLibrary("Presets"), [this, alive = this](const std::string& path) {
        if (Widget::isAlive(alive)) loadLibraryPreset(path);
    });
    menu.addSeparator();
    menu.addItem(100, "Open Preset...");
    menu.addItem(101, "Save Preset...");
    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
        if (! Widget::isAlive(this)) return;
        if (result >= 1 && result <= 4) loadPreset(result - 1);
        if (result == 101) savePreset();
        if (result == 100 && ctx().importWavetablePreset)
        {
            const auto action = ctx().importWavetablePreset; const int track = track_, device = device_, chain = chain_, child = child_;
            chooseFile(this, "Open Preset", plugins::prismPresetPatterns,
                       [action, track, device, chain, child](std::string path) { action(track, device, path, chain, child); });
        }
    });
}
void WavetableEditor::savePreset()
{
    // The live controls (and effects) captured now, with the model's tables and media: what the instrument plays.
    const auto* track = ctx().trackAt(track_);
    if (! track || device_ < 0 || (size_t) device_ >= track->devices.size()) return;
    const model::Device* source = &track->devices[(size_t) device_];
    if (chain_ >= 0)
    {
        if ((size_t) chain_ >= source->chains.size() || child_ < 0 || (size_t) child_ >= source->chains[(size_t) chain_].devices.size()) return;
        source = &source->chains[(size_t) chain_].devices[(size_t) child_];
    }
    auto device = *source;
    devices::captureDeviceState(device, processor_);
    const auto folder = plugins::prismLibraryRoot().getChildFile("Presets").getChildFile("User");
    folder.createDirectory();
    const auto name = isDefaultName(presetLabel_) ? std::string("My Preset") : presetLabel_;
    auto chooser = std::make_shared<juce::FileChooser>("Save Preset", folder.getChildFile(juce::String::fromUTF8(name.c_str()) + plugins::prismPresetExtension), "*.prism");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, chooser, device](const juce::FileChooser& result) mutable {
        auto file = result.getResult();
        if (file == juce::File()) return;
        file = file.withFileExtension(plugins::prismPresetExtension);
        device.displayName = file.getFileNameWithoutExtension().toStdString();
        std::string error;
        const bool saved = plugins::savePrismPreset(device, file, error);
        if (! Widget::isAlive(this)) return;
        ctx().statusText = saved ? "Saved preset " + device.displayName : "Could not save preset: " + error;
        repaint();
    });
}
void WavetableEditor::voiceQualityMenu()
{
    const auto choose = [this](int selected) {
        if (! Widget::isAlive(this) || selected < 0 || selected > 1) return;
        const int factor = selected + 1; if (factor == processor_->requestedVoiceRateFactor()) return;
        auto& context = ctx(); const auto edit = context.editDevice;
        if (! edit) { context.statusText = "Voice quality editing is unavailable"; repaint(); return; }
        const auto* track = context.trackAt(track_);
        if (! track || device_ < 0 || (size_t) device_ >= track->devices.size()) return;
        const auto* target = &track->devices[(size_t) device_];
        if (chain_ >= 0)
        {
            if ((size_t) chain_ >= target->chains.size() || child_ < 0 || (size_t) child_ >= target->chains[(size_t) chain_].devices.size()) return;
            target = &target->chains[(size_t) chain_].devices[(size_t) child_];
        }
        auto validation = *target; std::string error;
        if (! devices::storeWavetableVoiceRate(validation, factor, error)) { context.statusText = error; repaint(); return; }
        const int key = fxViewKey();
        if (key >= 0) { context.wavetableFxViews[key].inspectorState = true; storeFxView(); }
        const int chain = chain_, child = child_; auto* sharedContext = &context;
        // editDevice captures live state and publishes a newly prepared processor. The old
        // processor's voice rate is immutable throughout this message-thread gesture.
        edit(track_, device_, [factor, chain, child, sharedContext](model::Device& root) {
            auto* edited = &root;
            if (chain >= 0)
            {
                if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
                edited = &root.chains[(size_t) chain].devices[(size_t) child];
            }
            std::string reason;
            if (! devices::storeWavetableVoiceRate(*edited, factor, reason)) { sharedContext->statusText = reason; return; }
            if (chain >= 0) root.chainsEdited = true;
        }, "Change Voice Quality");
        // The synchronized edit may have destroyed this editor.
    };
    const int selected = processor_->requestedVoiceRateFactor() - 1;
    if (ctx().showChoices)
    {
        const auto show = ctx().showChoices; show({"1×", "2×"}, {selected == 0, selected == 1}, choose); return;
    }
    juce::PopupMenu menu; menu.addItem(1, juce::String::fromUTF8("1×"), true, selected == 0); menu.addItem(2, juce::String::fromUTF8("2×"), true, selected == 1);
    menu.showMenuAsync(juce::PopupMenu::Options(), [choose](int result) { if (result > 0) choose(result - 1); });
}
bool WavetableEditor::generatedNoise() const
{
    return (int) std::lround(processor_->getParameter(W::NoiseTypeParam)) != W::NoiseSampleType
        || (processor_->noiseSample() == nullptr && ! processor_->expectsNoiseSample());
}
void WavetableEditor::noiseAssetMenu()
{
    // 0 Generated (legacy tilted noise), 1 Import Sample..., then Serum's generated colours White / Pink / Brown / Geiger.
    const auto choose = [this](int selected) {
        if (! Widget::isAlive(this)) return;
        if (selected >= 2 && selected <= 5)
        { processor_->setParameter(W::NoiseTypeParam, (float) (selected - 1)); layout(); repaint(); return; }
        processor_->setParameter(W::NoiseTypeParam, W::NoiseSampleType);
        if (selected == 0) commitNoiseSample(nullptr);
        if (selected == 1) chooseNoiseFile();
    };
    const int type = (int) std::lround(processor_->getParameter(W::NoiseTypeParam));
    const std::vector<std::string> labels{"Generated Noise", "Import Sample...", "White", "Pink", "Brown", "Geiger"};
    std::vector<bool> ticked{type == W::NoiseSampleType && generatedNoise(), false};
    for (int t = W::NoiseWhite; t <= W::NoiseGeiger; ++t) ticked.push_back(type == t);
    if (ctx().showChoices) { const auto show = ctx().showChoices; show(labels, ticked, choose); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) { if (i == 2) menu.addSeparator(); menu.addItem((int) i + 1, juce::String(labels[i]), true, ticked[i]); }
    menu.showMenuAsync(juce::PopupMenu::Options(), [choose](int result) { if (result > 0) choose(result - 1); });
}
void WavetableEditor::chooseNoiseFile()
{
    const auto complete = [this](std::string path) { if (Widget::isAlive(this) && ! path.empty()) importNoiseFile(path); };
    if (onChooseNoiseFile) { const auto choose = onChooseNoiseFile; choose(complete); return; }
    chooseFile(this, "Import Noise Sample", engine::audioFileWildcard().toStdString(), complete);
}
bool WavetableEditor::importNoiseFile(const std::string& path)
{
    const auto loaded = devices::OscillatorSample::fromFile(juce::File(juce::String(path)), false);
    if (! loaded) { ctx().statusText = loaded.error; repaint(); return false; }
    commitNoiseSample(loaded.data); return true;
}
void WavetableEditor::commitNoiseSample(std::shared_ptr<const devices::OscillatorSample> sample)
{
    if (sample == nullptr && generatedNoise()) return;
    auto& context = ctx(); const auto edit = context.editDevice;
    if (! edit) { context.statusText = "Noise sample editing is unavailable"; repaint(); return; }
    const auto* track = context.trackAt(track_);
    if (! track || device_ < 0 || (size_t) device_ >= track->devices.size()) return;
    const auto* target = &track->devices[(size_t) device_];
    if (chain_ >= 0)
    {
        if ((size_t) chain_ >= target->chains.size() || child_ < 0 || (size_t) child_ >= target->chains[(size_t) chain_].devices.size()) return;
        target = &target->chains[(size_t) chain_].devices[(size_t) child_];
    }
    auto validation = *target; std::string error;
    if (! devices::storeWavetableNoise(validation, sample, error, true)) { context.statusText = error; repaint(); return; }
    const int key = fxViewKey(); if (key >= 0) { context.wavetableFxViews[key].inspectorState = true; storeFxView(); }
    const int chain = chain_, child = child_; auto* sharedContext = &context;
    edit(track_, device_, [sample, chain, child, sharedContext](model::Device& root) {
        auto* edited = &root;
        if (chain >= 0)
        {
            if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
            edited = &root.chains[(size_t) chain].devices[(size_t) child];
        }
        std::string reason;
        if (! devices::storeWavetableNoise(*edited, sample, reason, true)) { sharedContext->statusText = reason; return; }
        if (chain >= 0) root.chainsEdited = true;
    }, sample != nullptr ? "Import Noise Sample" : "Use Generated Noise");
    // The synchronized edit may have destroyed this editor.
}
void WavetableEditor::prepareNoisePreview()
{
    const auto* sample = processor_->noiseSample(); if (sample == nullptr) return;
    const size_t channels = static_cast<size_t>(sample->channels()), frames = static_cast<size_t>(sample->sampleFrames());
    const auto& pcm = sample->originalSamples(); float peak = 1;
    for (size_t bin = 0; bin < noisePeaks_.size(); ++bin)
    {
        const size_t first = bin * frames / noisePeaks_.size(), end = std::max(first + 1, (bin + 1) * frames / noisePeaks_.size());
        auto& range = noisePeaks_[bin];
        for (size_t channel = 0; channel < 2; ++channel)
        {
            range.minimum[channel] = range.maximum[channel] = pcm[std::min(first, frames - 1) * channels + std::min(channel, channels - 1)];
            for (size_t frame = first; frame < std::min(end, frames); ++frame)
            {
                const float value = pcm[frame * channels + std::min(channel, channels - 1)];
                range.minimum[channel] = std::min(range.minimum[channel], value); range.maximum[channel] = std::max(range.maximum[channel], value);
                peak = std::max(peak, std::abs(value));
            }
        }
    }
    noisePreviewGain_ = 1 / peak;
}
void WavetableEditor::loadPreset(int preset)
{
    releaseNote();
    if (! ctx().editDevice) return;
    libraryPresets().erase(fxViewKey()); // a built-in patch: the arrows cycle the built-ins again
    preset_ = std::clamp(preset, 0, 3);
    const auto infos = processor_->parameters();
    std::vector<float> values;
    for (const auto& info : infos) values.push_back(info.defaultValue);
    const auto set = [&values](int parameter, float value) { values[(size_t) parameter] = value; };
    if (preset_ == 1)
    {
        set(W::oscParam(0, W::Position), 1);
        set(W::oscParam(0, W::Unison), 7);
        set(W::oscParam(0, W::Detune), 18);
        set(W::filterParam(0, W::Cutoff), 4600);
    }
    if (preset_ == 2)
    {
        set(W::oscParam(0, W::Table), W::Hollow);
        set(W::oscParam(0, W::Position), 0.36f);
        set(W::oscParam(1, W::On), 1);
        set(W::oscParam(1, W::Table), W::Harmonic);
        set(W::oscParam(1, W::Level), 0.16f);
        set(W::envParam(0, W::Sustain), 0.1f);
        set(W::envParam(0, W::Decay), 0.8f);
        set(W::filterParam(0, W::Cutoff), 6200);
    }
    if (preset_ == 3)
    {
        set(W::envParam(0, W::Attack), 1.8f);
        set(W::envParam(0, W::Release), 2.4f);
        set(W::oscParam(0, W::Unison), 5);
        set(W::modParam(0, W::ModSource), W::Lfo1);
        set(W::modParam(0, W::ModDestination), W::Osc1Position);
        set(W::modParam(0, W::ModAmount), 0.3f);
        set(W::lfoParam(0, W::Division), 1);
    }
    const std::string name = std::string("Sonora Prism / ") + kPresets[(size_t) preset_];
    const int chain = chain_, child = child_;
    const auto edit = ctx().editDevice;
    edit(track_, device_, [infos, values, name, chain, child](model::Device& root) {
        auto* target = &root;
        if (chain >= 0)
        {
            if ((size_t) chain >= root.chains.size() || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
            target = &root.chains[(size_t) chain].devices[(size_t) child];
            root.chainsEdited = true;
        }
        for (size_t i = 0; i < infos.size(); ++i)
        {
            auto found = std::find_if(target->parameters.begin(), target->parameters.end(), [&](const auto& p) { return p.path == infos[i].id; });
            if (found == target->parameters.end())
            {
                model::Parameter p; p.path = infos[i].id; p.booleanValue = infos[i].isToggle;
                target->parameters.push_back(std::move(p)); found = std::prev(target->parameters.end());
            }
            found->value = values[i];
        }
        target->displayName = name;
    }, "Load Wavetable Preset");
    // The synchronized edit may have rebuilt and destroyed this editor.
}
Rect WavetableEditor::workflowCell(Page page) const { return {140 + (float) page * 48, 5, 46, 42}; }
Rect WavetableEditor::inspectorCell() const { return {width() - 170, 8, 94, 36}; }
Rect WavetableEditor::presetCell(int direction) const
{
    if (direction < 0) return {410, 8, 22, 34};
    if (direction > 0) return {width() - 224, 8, 22, 34};
    return {434, 8, std::max(40.0f, width() - 692), 34};
}
Rect WavetableEditor::presetGroupCell(int direction) const
{
    return direction < 0 ? Rect{386, 8, 22, 34} : Rect{width() - 200, 8, 22, 34};
}
Rect WavetableEditor::presetMoreCell() const { return {width() - 254, 8, 28, 34}; }
Rect WavetableEditor::matrixBankCell(int direction) const
{
    if (! modulationEditor_->visible || modulationEditor_->viewState().curveOpen) return {};
    const auto area = modulationEditor_->pageCell(direction), bounds = modulationEditor_->bounds();
    return area.translated(bounds.x, bounds.y);
}
Rect WavetableEditor::sectionCell(Section section) const
{
    if (! inspector_) return {};
    const float cell = body_.w / (float) Section::Count;
    return {body_.x + (float) section * cell, body_.y, cell - 2, kTabs - 2};
}
Rect WavetableEditor::selectorCell(int direction) const
{
    if (! inspector_) return {};
    if (direction < 0) return {body_.x, body_.y + kTabs + 2, 22, kNavigation - 4};
    if (direction > 0) return {body_.x + 184, body_.y + kTabs + 2, 22, kNavigation - 4};
    return {body_.x + 24, body_.y + kTabs + 2, 158, kNavigation - 4};
}
Rect WavetableEditor::pageCell(int direction) const
{
    return {body_.right() - (direction < 0 ? 141 : 22), body_.y + kTabs + 2, 22, kNavigation - 4};
}
Rect WavetableEditor::parameterCell(int index) const
{
    if (routingEditor_->visible)
    {
        Rect area;
        if (index == W::DirectGain) area = routingEditor_->directCell();
        if (index == W::SourceRoutingOn) area = routingEditor_->activeCell();
        if (index >= W::SourceRoutingStart && index < W::DirectGain)
            area = routingEditor_->parameterCell((index - W::SourceRoutingStart) / W::SourceRoutingParams,
                (WavetableRoutingEditor::Field) ((index - W::SourceRoutingStart) % W::SourceRoutingParams));
        if (index >= W::FilterOutputStart && index < W::SubPhase)
            area = routingEditor_->parameterCell(5 + index - W::FilterOutputStart, WavetableRoutingEditor::Field::Level);
        if (! area.isEmpty()) return area.translated(routingEditor_->bounds().x, routingEditor_->bounds().y);
    }
    if (modulationEditor_->visible)
    {
        using F = WavetableModulationEditor::Field;
        const std::array<F, W::ModParams> fields{F::Source, F::Destination, F::Amount, F::Via, F::Bipolar};
        int slot = -1; F field = F::Out;
        if (index >= W::ModStart && index < W::MacroStart)
        { slot = (index - W::ModStart) / W::ModParams; field = fields[(size_t) ((index - W::ModStart) % W::ModParams)]; }
        if (index >= W::ModOutStart && index < W::LfoAnchorStart) slot = index - W::ModOutStart;
        if (slot >= 0)
        {
            const auto area = modulationEditor_->parameterCell(slot, field), bounds = modulationEditor_->bounds();
            if (! area.isEmpty()) return area.translated(bounds.x, bounds.y);
        }
    }
    for (const auto& control : controls_) if (control.parameter == index && control.widget->visible) return control.widget->bounds();
    return {};
}
Rect WavetableEditor::subPlot() const { return ! inspector_ && workflow_ == Page::Osc ? subPlot_ : Rect{}; }
Rect WavetableEditor::subRoutingCell() const { return subRouting_->visible ? subRouting_->bounds() : Rect{}; }
Rect WavetableEditor::voiceQualityCell() const { return voiceQuality_->visible ? voiceQuality_->bounds() : Rect{}; }
Rect WavetableEditor::noisePlot() const
{ return (! inspector_ && workflow_ == Page::Osc) || (inspector_ && section_ == Section::Noise) ? noisePlot_ : Rect{}; }
Rect WavetableEditor::noiseAssetCell() const { return noiseAsset_->visible ? noiseAsset_->bounds() : Rect{}; }
Rect WavetableEditor::noiseImportCell() const { return noiseImport_->visible ? noiseImport_->bounds() : Rect{}; }
Rect WavetableEditor::noiseMoreCell() const { return noiseMore_->visible ? noiseMore_->bounds() : Rect{}; }
std::vector<int> WavetableEditor::visibleParameters() const
{
    std::vector<int> result;
    for (const auto& control : controls_) if (control.widget->visible) result.push_back(control.parameter);
    if (routingEditor_->visible)
    {
        result.push_back(W::DirectGain); result.push_back(W::SourceRoutingOn);
        for (int source : routingEditor_->visibleSources())
        {
            for (int field = 0; field < W::SourceRoutingParams; ++field)
                if (! parameterCell(W::sourceRoutingParam(source, (W::SourceRoutingParam) field)).isEmpty()) result.push_back(W::sourceRoutingParam(source, (W::SourceRoutingParam) field));
            if (source >= 5) result.push_back(W::filterOutputParam(source - 5));
        }
    }
    if (modulationEditor_->visible)
        for (int slot : modulationEditor_->visibleSlots())
        {
            for (int field = 0; field < W::ModParams; ++field)
                if (! parameterCell(W::modParam(slot, (W::ModParam) field)).isEmpty()) result.push_back(W::modParam(slot, (W::ModParam) field));
            if (! parameterCell(W::outParam(slot)).isEmpty()) result.push_back(W::outParam(slot));
        }
    return result;
}
Rect WavetableEditor::oscillatorPlot(int oscillator) const
{
    if (oscillator < 0 || oscillator >= 3 || inspector_ || workflow_ != Page::Osc) return {};
    return oscillatorLayout(oscillatorPanels_[(size_t) oscillator]).display;
}
Rect WavetableEditor::fxBusCell(int bus) const
{
    if (bus < 0 || bus > 2) return {};
    if (inspector_) return section_ == Section::Effects && fxBusButtons_[(size_t) bus]->visible ? fxBusButtons_[(size_t) bus]->bounds() : Rect{};
    if (workflow_ == Page::Mix) return mixOpenButtons_[(size_t) bus]->bounds();
    return workflow_ == Page::Fx ? Rect{fxChain_.x + (float) bus * fxChain_.w / 3, fxChain_.y, fxChain_.w / 3 - 2, 23} : Rect{};
}
Rect WavetableEditor::fxModuleCell(Effects::ModuleId id) const
{
    const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [id](const auto& node) { return node.id == id; });
    return found != fxNodes_.end() && found->select->visible ? found->select->bounds() : Rect{};
}
Rect WavetableEditor::fxAddCell() const { return fxAdd_->visible ? fxAdd_->bounds() : Rect{}; }
Rect WavetableEditor::fxRemoveCell() const { return fxRemove_->visible ? fxRemove_->bounds() : Rect{}; }
Rect WavetableEditor::fxMoveCell(int direction) const
{
    auto* button = direction < 0 ? fxUp_ : fxDown_; return button->visible ? button->bounds() : Rect{};
}
Rect WavetableEditor::fxBandCell(int band) const
{
    return band >= 0 && band < 3 && fxBandButtons_[(size_t) band]->visible ? fxBandButtons_[(size_t) band]->bounds() : Rect{};
}
Rect WavetableEditor::fxChainPageCell(int direction) const
{
    auto* button = direction < 0 ? fxChainPrevious_ : fxChainNext_; return button->visible ? button->bounds() : Rect{};
}
Rect WavetableEditor::fxControlsPageCell(int direction) const
{
    auto* button = direction < 0 ? fxControlsPrevious_ : fxControlsNext_; return button->visible ? button->bounds() : Rect{};
}
Rect WavetableEditor::fxImpulseCell() const { return fxImpulse_->visible ? fxImpulse_->bounds() : Rect{}; }
Rect WavetableEditor::envelopeCell(int envelope) const
{
    if (inspector_) return {};
    const float cell = (envelopePanel_.w - 24) / 4;
    return {envelopePanel_.x + (float) envelope * cell, envelopePanel_.y, cell - 1, 22};
}
Rect WavetableEditor::lfoCell(int lfo) const
{
    if (inspector_) return {};
    const float cell = (lfoPanel_.w - 59) / 10;
    return {lfoPanel_.x + 34 + (float) lfo * cell, lfoPanel_.y, cell - 1, 22};
}
Rect WavetableEditor::envelopePlot() const
{
    if (inspector_) return {};
    const float top = instances_[(size_t) Section::Envelopes] > 0 ? 52.0f : 28.0f;
    return {envelopePanel_.x + 5, envelopePanel_.y + top, envelopePanel_.w - 10, std::max(30.0f, envelopePanel_.h - top - 63)};
}
Rect WavetableEditor::lfoPlot() const
{
    if (inspector_) return {};
    return {lfoPanel_.x + 100, lfoPanel_.y + 28, lfoPanel_.w - 106, std::max(30.0f, lfoPanel_.h - 91)};
}
Rect WavetableEditor::lfoModeCell(int mode) const
{
    if (inspector_ || mode < 0 || mode > 2) return {};
    return {lfoPanel_.x + 6, lfoPanel_.y + 56 + (float) mode * 24, 88, 20};
}
Rect WavetableEditor::lfoRateCell() const
{
    const int lfo = instances_[(size_t) Section::Lfos];
    const auto* knob = processor_->getParameter(W::lfoParam(lfo, W::SyncOn)) >= 0.5f ? lfoDivisionKnobs_[(size_t) lfo] : controls_[(size_t) W::lfoParam(lfo, W::Rate)].widget;
    return knob != nullptr && knob->visible ? knob->bounds() : Rect{};
}
void WavetableEditor::showControl(int parameter, Rect bounds, bool compact)
{
    if (parameter < 0 || (size_t) parameter >= controls_.size()) return;
    auto* widget = controls_[(size_t) parameter].widget;
    if (auto* knob = dynamic_cast<PanelKnob*>(widget)) { knob->compact = compact; knob->inlineField = knob->stepper = false; knob->accent = 0; knob->compactLabel.clear(); knob->valueText = {}; }
    if (auto* choice = dynamic_cast<ParameterChoice*>(widget)) { choice->compact = compact; choice->stacked = false; choice->accent = 0; }
    widget->setBounds(bounds); widget->visible = ! bounds.isEmpty();
}
void WavetableEditor::layout()
{
    restoreFxView();
    restoreModulationView();
    const bool wasCompact = forcedCompact_; forcedCompact_ = height() < 500;
    if (forcedCompact_) inspector_ = true; else if (wasCompact) inspector_ = false;
    body_ = {4, kHeader + 4, std::max(0.0f, width() - 8), std::max(0.0f, height() - kHeader - 8)};
    for (size_t i = 0; i < workflowButtons_.size(); ++i) workflowButtons_[i]->setBounds(workflowCell((Page) i));
    inspectButton_->setLabel(forcedCompact_ ? "EXPAND" : "CONTROLS");
    inspectButton_->setBounds(inspectorCell()); presetPrevious_->setBounds(presetCell(-1));
    presetNext_->setBounds(presetCell(1)); presetButton_->setBounds(presetCell(0));
    presetGroupPrevious_->setBounds(presetGroupCell(-1)); presetGroupNext_->setBounds(presetGroupCell(1)); presetMore_->setBounds(presetMoreCell());
    libraryGroups_ = libraryGroupsAvailable();
    std::string presetName = presetLabel_;
    if (const auto* track = ctx().trackAt(track_); track && (size_t) device_ < track->devices.size())
    {
        const model::Device* device = &track->devices[(size_t) device_];
        if (chain_ >= 0 && (size_t) chain_ < device->chains.size() && (size_t) child_ < device->chains[(size_t) chain_].devices.size())
            device = &device->chains[(size_t) chain_].devices[(size_t) child_];
        if (! isDefaultName(device->displayName)) presetName = device->displayName;
        for (size_t i = 0; i < kPresets.size(); ++i)
            if (device->displayName == std::string("Sonora Prism / ") + kPresets[i] || device->displayName == std::string("Sonora Wavetable / ") + kPresets[i])
            { preset_ = (int) i; presetName = kPresets[i]; }
    }
    presetLabel_ = presetName; presetButton_->setLabel(presetName + (presetEdited_ ? " *" : ""));
    master_->setBounds({width() - 72, 2, 54, 48});
    for (auto& control : controls_) control.widget->visible = false;
    modulationEditor_->visible = false;
    routingEditor_->visible = false; mixSourcesButton_->visible = mixBusesButton_->visible = false;
    for (auto* widget : {previous_, selector_, next_, pagePrevious_, pageNext_, envelopeMore_, lfoMore_, macroMore_, subRouting_, voiceQuality_, noiseAsset_, noiseImport_, noiseMore_}) widget->visible = false;
    {
        static constexpr std::array<const char*, W::NumNoiseTypes> types{"", "White Noise", "Pink Noise", "Brown Noise", "Geiger"};
        const int type = std::clamp((int) std::lround(processor_->getParameter(W::NoiseTypeParam)), 0, W::NumNoiseTypes - 1);
        noiseAsset_->setLabel(type != W::NoiseSampleType ? types[(size_t) type] : processor_->noiseSampleName());
    }
    std::string quality = "Voice Quality  " + std::to_string(processor_->requestedVoiceRateFactor()) + "×";
    if (processor_->voiceRateFactor() != processor_->requestedVoiceRateFactor()) quality += " (active " + std::to_string(processor_->voiceRateFactor()) + "×)";
    voiceQuality_->setLabel(quality);
    for (auto* widget : sections_) widget->visible = false;
    for (auto* widget : oscillatorMore_) widget->visible = false;
    for (auto* widget : mediaImport_) widget->visible = false;
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        mediaModes_[(size_t) oscillator] = (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode)));
    for (auto* widget : filterMore_) widget->visible = false;
    for (auto* widget : fxBusButtons_) widget->visible = false;
    for (auto* widget : fxBandButtons_) widget->visible = false;
    for (auto* widget : mixOpenButtons_) widget->visible = false;
    for (auto& node : fxNodes_) node.select->visible = false;
    for (auto* widget : {fxAdd_, fxRemove_, fxUp_, fxDown_, fxChainPrevious_, fxChainNext_, fxControlsPrevious_, fxControlsNext_, fxImpulse_, fxEnableBuses_}) widget->visible = false;
    for (auto* widget : envelopeButtons_) widget->visible = false;
    for (auto* widget : lfoButtons_) widget->visible = false;
    for (auto* widget : modHandles_) if (widget) widget->visible = false;
    keysButton_->visible = arpButton_->visible = false;
    for (auto* widget : voiceTabs_) widget->visible = false;
    for (auto* widget : lfoModes_) widget->visible = false;
    for (auto* widget : lfoDivisionKnobs_) widget->visible = false;
    if (inspector_ && forcedCompact_ && workflow_ == Page::Matrix && matrixCompact_)
    { modulationEditor_->visible = true; modulationEditor_->setBounds(body_); modulationEditor_->layout(); }
    else if (inspector_ && forcedCompact_ && workflow_ == Page::Mix && mixCompact_)
    { routingEditor_->visible = true; routingEditor_->setBounds(body_); routingEditor_->layout(); }
    else if (inspector_) layoutInspector(); else layoutDashboard();
    if (routingEditor_->visible && ! routingViewLoaded_)
    {
        routingViewLoaded_ = true;
        if (const auto found = ctx().wavetableRoutingViews.find(fxViewKey()); found != ctx().wavetableRoutingViews.end())
            routingEditor_->restorePage(found->second.page);
    }
    layoutPresetBrowser();
}
void WavetableEditor::layoutInspector()
{
    for (size_t i = 0; i < sections_.size(); ++i) { sections_[i]->visible = true; sections_[i]->setBounds(sectionCell((Section) i)); }
    const bool selector = instanceCount() > 1;
    previous_->visible = selector_->visible = next_->visible = selector;
    previous_->setBounds(selectorCell(-1)); selector_->setBounds(selectorCell(0)); next_->setBounds(selectorCell(1));
    selector_->setLabel(instanceLabel(selectedInstance()));
    if (section_ == Section::Main)
    { voiceQuality_->visible = true; voiceQuality_->setBounds({body_.x + 4, body_.y + kTabs + 2, 220, kNavigation - 4}); }
    if (section_ == Section::Effects)
    {
        const float y = body_.y + kTabs + 2;
        fxAdd_->visible = true; fxAdd_->setBounds({body_.x + 220, y, 102, 18});
        for (int bus = 0; bus < 3; ++bus)
        { auto* button = fxBusButtons_[(size_t) bus]; button->visible = true; button->setBounds({body_.x + 328 + (float) bus * 62, y, 59, 18}); }
        if (selectedInstance() > 0 && selectedFxNode())
        {
            fxUp_->visible = fxDown_->visible = fxRemove_->visible = true;
            fxUp_->setBounds({body_.x + 520, y, 40, 18}); fxDown_->setBounds({body_.x + 564, y, 45, 18}); fxRemove_->setBounds({body_.x + 613, y, 61, 18});
            if (selectedFxNode()->type == Effects::Type::Convolution)
            { fxImpulse_->visible = true; fxImpulse_->setLabel("LOAD IMPULSE..."); fxImpulse_->setBounds({body_.x + 680, y, 141, 18}); }
            if (selectedFxNode()->type == Effects::Type::Split3)
                for (int band = 0; band < 3; ++band)
                { auto* button = fxBandButtons_[(size_t) band]; button->visible = true; button->setBounds({body_.x + 680 + (float) band * 65, y, 61, 18}); }
        }
    }
    controlsArea_ = {body_.x, body_.y + kTabs + kNavigation + 3, body_.w, std::max(0.0f, body_.h - kTabs - kNavigation - 3)};
    if (section_ == Section::Noise)
    {
        noiseAsset_->visible = noiseImport_->visible = true;
        noiseAsset_->setBounds({body_.x + 4, body_.y + kTabs + 2, 370, kNavigation - 4});
        noiseImport_->setBounds({body_.x + 380, body_.y + kTabs + 2, 104, kNavigation - 4});
        if (controlsArea_.h >= 70)
        {
            noisePlot_ = {body_.x + 4, controlsArea_.y, body_.w - 8, std::clamp(controlsArea_.h * 0.45f, 28.0f, 110.0f)};
            controlsArea_.removeFromTop(noisePlot_.h + 6);
        }
        else noisePlot_ = {};
    }
    columns_ = std::clamp((int) (body_.w / 94), 2, 11);
    const int rows = std::max(1, (int) (controlsArea_.h / 60));
    rowHeight_ = std::clamp(controlsArea_.h / (float) rows, 32.0f, 66.0f);
    pages_.clear(); pages_.emplace_back(); int row = 0, column = 0;
    for (size_t i = 0; i < controls_.size(); ++i)
    {
        const auto& control = controls_[i];
        const bool noiseParameter = (control.parameter >= W::NoiseOn && control.parameter <= W::NoiseColor)
            || (control.parameter >= W::NoisePitch && control.parameter < W::ArpStart) || control.parameter == W::NoiseTypeParam;
        if (section_ == Section::Noise ? ! noiseParameter : control.section != section_ || control.instance != selectedInstance()) continue;
        if (control.parameter == W::NoiseColor && ! generatedNoise()) continue;
        if (column + control.span > columns_) { ++row; column = 0; }
        if (row >= rows) { pages_.emplace_back(); row = 0; column = 0; }
        pages_.back().push_back({i, row, column}); column += control.span;
    }
    page_ = std::clamp(page_, 0, pageCount() - 1);
    pagePrevious_->visible = pageNext_->visible = pageCount() > 1;
    pagePrevious_->setBounds(pageCell(-1)); pageNext_->setBounds(pageCell(1));
    const float cell = controlsArea_.w / (float) columns_;
    for (const auto& placement : pages_[(size_t) page_])
    {
        const auto& control = controls_[placement.control];
        Rect r{controlsArea_.x + (float) placement.column * cell + kGap, controlsArea_.y + (float) placement.row * rowHeight_,
               (float) control.span * cell - kGap * 2, rowHeight_ - 2};
        if (processor_->parameters()[(size_t) control.parameter].isToggle) r = {r.x, r.y + (r.h - 21) * 0.5f, r.w, 21};
        if (controlsArea_.h >= 30) showControl(control.parameter, r);
    }
}
WavetableEditor::OscillatorLayout WavetableEditor::oscillatorLayout(Rect r)
{
    // Serum-like stack: header, table, pitch fields, frame display, phase row, then two knob rows over a lighter base.
    OscillatorLayout l;
    l.table = {r.x + 3, r.y + 28, r.w - 6, 22};
    l.pitch = {r.x + 3, r.y + 53, r.w - 6, 24};
    const float knobRow = std::clamp((r.h - 150) * 0.3f, 44.0f, 60.0f);
    l.base = {r.x + 1, r.bottom() - 2 * knobRow - 16, r.w - 2, 2 * knobRow + 15};
    l.phase = {r.x + 3, l.base.y - 21, r.w - 6, 18};
    l.display = {r.x + 4, r.y + 80, r.w - 8, std::max(24.0f, l.phase.y - 3 - (r.y + 80))};
    l.knobs = {l.base.x + 3, l.base.y + 2, l.base.w - 6, knobRow};
    l.warps = {l.base.x + 3, l.base.y + 2 + knobRow, l.base.w - 6, knobRow};
    return l;
}
void WavetableEditor::layoutOscillator(int oscillator, Rect r)
{
    const int mode = (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode)));
    const auto l = oscillatorLayout(r);
    const auto knob = [&](int parameter, Rect bounds, const char* caption) {
        showControl(parameter, bounds);
        if (auto* widget = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget)) widget->compactLabel = caption;
    };
    const auto field = [&](int parameter, Rect bounds, const char* caption, std::function<std::string(float)> format = {}) {
        showControl(parameter, bounds, true);
        if (auto* widget = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget))
        { widget->compactLabel = caption; widget->inlineField = true; widget->valueText = std::move(format); }
    };
    showControl(W::oscParam(oscillator, W::On), {r.x + 6, r.y + 5, 14, 16});
    showControl(W::mediaParam(oscillator, W::MediaMode), {r.x + 60, r.y + 4, r.w - 134, 18}, true);
    showControl(W::oscParam(oscillator, W::RouteFilter1), {r.right() - 70, r.y + 4, 22, 18});
    showControl(W::oscParam(oscillator, W::RouteFilter2), {r.right() - 46, r.y + 4, 22, 18});
    oscillatorMore_[(size_t) oscillator]->visible = true;
    oscillatorMore_[(size_t) oscillator]->setBounds({r.right() - 22, r.y + 4, 18, 18});
    if (mode == W::WavetableMode) showControl(W::oscParam(oscillator, W::Table), l.table, true);
    else
    {
        auto* button = mediaImport_[(size_t) oscillator]; button->visible = true; button->setBounds(l.table);
    }
    const float pitch = l.pitch.w / 4;
    const std::array<std::pair<int, const char*>, 4> pitches{{{W::oscParam(oscillator, W::Octave), "OCT"}, {W::oscParam(oscillator, W::Semi), "SEM"},
                                                             {W::oscParam(oscillator, W::Fine), "FIN"}, {W::coarseParam(oscillator), "CRS"}}};
    for (size_t i = 0; i < pitches.size(); ++i)
        field(pitches[i].first, {l.pitch.x + (float) i * pitch, l.pitch.y, pitch - 3, l.pitch.h}, pitches[i].second, [i](float value) {
            if (i == 2) return number("%.0f", value);
            return std::abs(value) < 0.005f ? std::string("0") : number(i == 3 ? "%+.1f" : "%+.0f", value);
        });
    const float frameWidth = l.phase.w * 0.2f, phaseWidth = (l.phase.w - frameWidth) * 0.5f;
    field(W::oscParam(oscillator, W::Phase), {l.phase.x + frameWidth + 2, l.phase.y, phaseWidth - 3, l.phase.h}, "Φ",
          [](float value) { return number("%.0f°", value * 360.0); });
    field(W::oscParam(oscillator, W::Random), {l.phase.x + frameWidth + phaseWidth + 1, l.phase.y, phaseWidth - 1, l.phase.h}, "Φ RAND",
          [](float value) { return number("%.0f", value * 100.0); });
    // Two rows: four knobs (or media controls) and Pan; the warp pair and Level.
    const float side = l.knobs.w * 0.2f, cell = (l.knobs.w - side) / 4;
    const std::array<std::pair<int, const char*>, 4> wavetable{{{W::oscParam(oscillator, W::Position), "WT POS"}, {W::oscParam(oscillator, W::Unison), "UNISON"},
                                                               {W::oscParam(oscillator, W::Detune), "DETUNE"}, {W::oscParam(oscillator, W::Blend), "BLEND"}}};
    const std::array<std::pair<int, const char*>, 4> granular{{{W::mediaParam(oscillator, W::MediaStartPosition), "START"}, {W::mediaParam(oscillator, W::MediaGrainSize), "GRAIN"},
                                                              {W::mediaParam(oscillator, W::MediaDensity), "DENSITY"}, {W::mediaParam(oscillator, W::MediaScanRate), "SCAN"}}};
    const std::array<std::pair<int, const char*>, 4> sample{{{W::mediaParam(oscillator, W::MediaStartPosition), "START"}, {W::mediaParam(oscillator, W::MediaScanRate), "SCAN"},
                                                            {W::oscParam(oscillator, W::Unison), "UNISON"}, {W::oscParam(oscillator, W::Detune), "DETUNE"}}};
    const auto& row = mode == W::WavetableMode ? wavetable : mode == W::GranularMode ? granular : sample;
    for (size_t i = 0; i < row.size(); ++i) knob(row[i].first, {l.knobs.x + (float) i * cell, l.knobs.y, cell, l.knobs.h}, row[i].second);
    if (auto* unison = dynamic_cast<PanelKnob*>(controls_[(size_t) W::oscParam(oscillator, W::Unison)].widget); unison && unison->visible) unison->stepper = true;
    knob(W::oscParam(oscillator, W::OscPan), {l.knobs.right() - side, l.knobs.y, side, l.knobs.h}, "PAN");
    knob(W::oscParam(oscillator, W::Level), {l.warps.right() - side, l.warps.y, side, l.warps.h}, "LEVEL");
    const float warpKnob = cell * 0.82f, selector = (l.warps.w - side - 2 * warpKnob) * 0.5f;
    const float selectorY = l.warps.y + 3, selectorH = std::min(34.0f, l.warps.h - 16);
    if (mode == W::WavetableMode)
    {
        knob(W::oscParam(oscillator, W::Warp1Amount), {l.warps.x, l.warps.y, warpKnob, l.warps.h}, "1");
        for (int warp = 0; warp < 2; ++warp)
        {
            const int parameter = W::oscParam(oscillator, warp == 0 ? W::Warp1Mode : W::Warp2Mode);
            showControl(parameter, {l.warps.x + warpKnob + (float) warp * selector + 1, selectorY, selector - 2, selectorH}, true);
            if (auto* choice = dynamic_cast<ParameterChoice*>(controls_[(size_t) parameter].widget)) choice->stacked = true;
        }
        knob(W::oscParam(oscillator, W::Warp2Amount), {l.warps.x + warpKnob + 2 * selector, l.warps.y, warpKnob, l.warps.h}, "2");
    }
    else
    {
        const float half = (l.warps.w - side) * 0.5f;
        showControl(W::mediaParam(oscillator, W::MediaLoopMode), {l.warps.x, selectorY, half - 3, 22}, true);
        if (mode == W::GranularMode) showControl(W::mediaParam(oscillator, W::MediaWindow), {l.warps.x + half, selectorY, half - 3, 22}, true);
        else showControl(W::mediaParam(oscillator, W::MediaReverse), {l.warps.x + half, selectorY, half - 3, 20});
    }
}
Rect WavetableEditor::filterPlot(int filter) const
{
    const auto r = filterPanels_[(size_t) filter];
    const float knobRow = std::clamp((r.h - 100) * 0.45f, 44.0f, 62.0f);
    return {r.x + 4, r.y + 50, r.w - 8, std::max(20.0f, r.h - 56 - knobRow)};
}
void WavetableEditor::layoutFilter(int filter, Rect r)
{
    // Header (power, title, type), response plot, then Cutoff / Resonance / Drive / Mix on one row.
    showControl(W::filterParam(filter, W::FilterOn), {r.x + 6, r.y + 5, 14, 16});
    showControl(W::filterParam(filter, W::FilterMode), {r.x + 3, r.y + 27, r.w - 6, 21}, true);
    const auto plot = filterPlot(filter);
    const float cell = (r.w - 8) / 4, top = plot.bottom() + 2, h = r.bottom() - 2 - top;
    const std::array<std::pair<W::FilterParam, const char*>, 4> knobs{{{W::Cutoff, "CUTOFF"}, {W::Resonance, "RES"}, {W::Drive, "DRIVE"}, {W::FilterMix, "MIX"}}};
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const int parameter = W::filterParam(filter, knobs[i].first);
        showControl(parameter, {r.x + 4 + (float) i * cell, top, cell, h});
        if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget)) knob->compactLabel = knobs[i].second;
    }
    filterMore_[(size_t) filter]->visible = true; filterMore_[(size_t) filter]->setBounds({r.right() - 22, r.y + 4, 18, 18});
}
void WavetableEditor::layoutDashboard()
{
    auto r = body_; footer_ = r.removeFromBottom(34); r.removeFromBottom(6);
    lower_ = r.removeFromBottom(std::clamp(r.h * 0.42f, 164.0f, 238.0f)); r.removeFromBottom(6); upper_ = r;
    if (workflow_ == Page::Osc)
    {
        // SUB | OSC A | OSC B | OSC C | NOISE | FILTER 1 over FILTER 2: the oscillators get the width.
        auto row = upper_; sub_ = row.removeFromLeft(92); row.removeFromLeft(6);
        const float filterWidth = std::clamp(upper_.w * 0.16f, 150.0f, 210.0f);
        const float oscillatorWidth = (upper_.w - 92 - 100 - filterWidth - 30) / 3;
        for (int oscillator = 0; oscillator < 3; ++oscillator)
        {
            oscillatorPanels_[(size_t) oscillator] = row.removeFromLeft(oscillatorWidth); row.removeFromLeft(6);
            layoutOscillator(oscillator, oscillatorPanels_[(size_t) oscillator]);
        }
        noise_ = row.removeFromLeft(100); row.removeFromLeft(6);
        const float filterHeight = (row.h - 6) * 0.5f;
        for (int filter = 0; filter < 2; ++filter)
        {
            filterPanels_[(size_t) filter] = {row.x, row.y + (float) filter * (filterHeight + 6), row.w, filterHeight};
            layoutFilter(filter, filterPanels_[(size_t) filter]);
        }
        layoutSub();
        layoutNoise();
    }
    else if (workflow_ == Page::Matrix)
    {
        modulationEditor_->visible = true; modulationEditor_->setBounds(upper_); modulationEditor_->layout();
    }
    else if (workflow_ == Page::Mix)
    {
        for (int filter = 0; filter < 2; ++filter)
        {
            filterPanels_[(size_t) filter] = {upper_.x + (float) filter * 210, upper_.y, 202, upper_.h};
            layoutFilter(filter, filterPanels_[(size_t) filter]);
        }
        layoutMixer();
    }
    else if (workflow_ == Page::Fx) layoutEffects();
    else
    {
        voiceQuality_->visible = true; voiceQuality_->setBounds({upper_.right() - 226, upper_.y + 4, 220, 18});
        std::vector<int> parameters;
        for (int parameter = 0; parameter < W::OscStart; ++parameter)
            if (parameter != W::NoiseColor || generatedNoise()) parameters.push_back(parameter);
        for (int parameter = 0; parameter < W::VoiceModeParams; ++parameter) parameters.push_back(W::voiceModeParam((W::VoiceModeParam) parameter));
        parameters.push_back(W::SubPhase); parameters.push_back(W::SubContinuous);
        int count = 0;
        for (const int parameter : parameters)
        {
            const int span = processor_->parameters()[(size_t) parameter].choices.empty() ? 1 : 2;
            if (count % 10 + span > 10) count += 10 - count % 10;
            count += span;
        }
        const int rows = std::max(1, (count + 9) / 10);
        int position = 0;
        for (const int parameter : parameters)
        {
            const bool choice = ! processor_->parameters()[(size_t) parameter].choices.empty();
            const int span = choice ? 2 : 1;
            if (position % 10 + span > 10) position += 10 - position % 10;
            const float w = upper_.w / 10, h = std::min(66.0f, (upper_.h - 32) / (float) rows);
            Rect cell{upper_.x + (float) (position % 10) * w + 4, upper_.y + 32 + (float) (position / 10) * h, (float) span * w - 8, h - 2};
            if (processor_->parameters()[(size_t) parameter].isToggle) cell = {cell.x, cell.y + (cell.h - 21) * 0.5f, cell.w, 21};
            showControl(parameter, cell); position += span;
        }
    }
    layoutModulation();
    keysButton_->visible = arpButton_->visible = true;
    keysButton_->setBounds(keyboardViewCell(false)); arpButton_->setBounds(keyboardViewCell(true));
    if (arpView_)
    {
        const float x = footer_.x + 88, y = footer_.y + 3;
        showControl(W::arpParam(W::ArpOn), {x, y + 4, 34, 20});
        showControl(W::arpParam(W::ArpMode), {x + 38, y, 112, 28}, true);
        showControl(W::arpParam(W::ArpRate), {x + 154, y, 92, 28}, true);
        showControl(W::arpParam(W::ArpOctaves), {x + 250, y, 54, 28}, true);
        showControl(W::arpParam(W::ArpGate), {x + 308, y, 54, 28}, true);
    }
}
void WavetableEditor::layoutSub()
{
    showControl(W::SubOn, {sub_.x + 6, sub_.y + 5, 14, 16});
    showControl(W::SubWave, {sub_.x + 4, sub_.y + 28, sub_.w - 8, 22}, true);
    const float cell = (sub_.w - 12) * 0.5f;
    showControl(W::SubOctave, {sub_.x + 4, sub_.y + 54, cell, 30}, true);
    showControl(W::SubPhase, {sub_.x + 8 + cell, sub_.y + 54, cell, 30}, true);
    if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) W::SubOctave].widget)) knob->compactLabel = "Octave";
    if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) W::SubPhase].widget)) knob->compactLabel = "Phase";
    subPlot_ = {sub_.x + 5, sub_.y + 90, sub_.w - 10, std::clamp(sub_.h - 195, 27.0f, 58.0f)};
    const float y = subPlot_.bottom() + 5;
    if (sub_.h >= 335)
    {
        showControl(W::SubPan, {sub_.x + 5, y, sub_.w - 10, 55});
        showControl(W::SubLevel, {sub_.x + 5, y + 61, sub_.w - 10, 55});
    }
    else
    {
        const float h = std::min(66.0f, sub_.bottom() - 49 - y);
        showControl(W::SubPan, {sub_.x + 4, y, cell, h});
        showControl(W::SubLevel, {sub_.x + 8 + cell, y, cell, h});
    }
    showControl(W::SubContinuous, {sub_.x + 5, sub_.bottom() - 47, sub_.w - 10, 18});
    subRouting_->visible = true; subRouting_->setBounds({sub_.x + 5, sub_.bottom() - 23, sub_.w - 10, 18});
}
void WavetableEditor::layoutNoise()
{
    showControl(W::NoiseOn, {noise_.x + 6, noise_.y + 5, 14, 16});
    noiseMore_->visible = noiseAsset_->visible = noiseImport_->visible = true;
    noiseMore_->setBounds({noise_.right() - 22, noise_.y + 4, 18, 17});
    noiseAsset_->setBounds({noise_.x + 4, noise_.y + 28, noise_.w - 8, 22});
    noiseImport_->setBounds({noise_.x + 5, noise_.bottom() - 23, noise_.w - 10, 18});
    const bool large = noise_.h >= 335;
    noisePlot_ = {noise_.x + 5, noise_.y + 56, noise_.w - 10, large ? 86.0f : 38.0f};
    const float cell = (noise_.w - 12) * .5f;
    const auto field = [this, cell](int parameter, const char* caption, float y, bool right) {
        showControl(parameter, {noise_.x + 4 + (right ? cell + 4 : 0), noise_.y + y, cell, 28}, true);
        if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget)) knob->compactLabel = caption;
    };
    float y = large ? 148.0f : 100.0f;
    if (! generatedNoise())
    {
        field(W::NoisePitch, "Pitch", y, false); field(large ? W::NoiseFine : W::NoisePhase, large ? "Fine" : "Phase", y, true);
        if (large) { field(W::NoisePhase, "Phase", 184, false); field(W::NoiseRandom, "Rand", 184, true); y = 220; }
        else y = 132;
        showControl(W::NoiseKeytrack, {noise_.x + 5, noise_.bottom() - 47, noise_.w - 10, 18});
    }
    else
    {
        showControl(W::NoiseColor, {noise_.x + 5, noise_.bottom() - 53, noise_.w - 10, 26}, true);
        y = noisePlot_.bottom() - noise_.y + 5;
    }
    const float h = std::min(66.0f, noise_.h - 49 - y);
    showControl(W::NoisePan, {noise_.x + 4, noise_.y + y, cell, h});
    showControl(W::NoiseLevel, {noise_.x + 8 + cell, noise_.y + y, cell, h});
    if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) W::NoisePan].widget)) knob->compactLabel = "Pan";
    if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) W::NoiseLevel].widget)) knob->compactLabel = "Level";
}
void WavetableEditor::layoutEffects()
{
    fxChain_ = {upper_.x, upper_.y, 242, upper_.h};
    fxDetail_ = {fxChain_.right() + 6, upper_.y, upper_.w - fxChain_.w - 6, upper_.h};
    for (int bus = 0; bus < Effects::busCount; ++bus)
    { auto* button = fxBusButtons_[(size_t) bus]; button->visible = true; button->setBounds(fxBusCell(bus)); }
    fxAdd_->visible = true; fxAdd_->setBounds({fxChain_.x + 4, fxChain_.y + 28, fxChain_.w - 8, 24});
    fxRows.clear();
    fxRows_ = std::max(1, (int) ((fxChain_.h - 81) / 26));
    fxChainPage_ = std::clamp(fxChainPage_, 0, fxChainPageCount() - 1);
    const auto chain = currentFxChain();
    for (int row = 0; row < fxRows_; ++row)
    {
        const int index = fxChainPage_ * fxRows_ + row; if ((size_t) index >= chain.size()) break;
        const auto* node = chain[(size_t) index]; const float indent = (float) ((node->path.size() - 1) / 2) * 10;
        const float y = fxChain_.y + 58 + (float) row * 26;
        // Serum's list row: colour bar, name, then turn off / hide / delete.
        node->select->visible = true; node->select->setBounds({fxChain_.x + 10 + indent, y, fxChain_.w - 76 - indent, 24});
        showControl(processor_->fxEnabledParameterIndex(node->id), {fxChain_.right() - 62, y + 3, 18, 18});
        fxRows.push_back({node->id, {fxChain_.right() - 42, y + 3, 18, 18}, {fxChain_.right() - 22, y + 3, 18, 18}});
    }
    fxChainPrevious_->visible = fxChainNext_->visible = fxChainPageCount() > 1;
    fxChainPrevious_->setBounds({fxChain_.x + 4, fxChain_.bottom() - 22, 23, 18});
    fxChainNext_->setBounds({fxChain_.right() - 27, fxChain_.bottom() - 22, 23, 18});
    fxPanels_.clear(); fxGraphic_ = {};
    const auto* node = selectedFxNode(); if (! node) return;
    // Toolbar for the selected module: reorder/remove, plus its band or impulse buttons.
    fxUp_->visible = fxDown_->visible = fxRemove_->visible = true;
    fxUp_->setBounds({fxDetail_.right() - 164, fxDetail_.y + 4, 43, 19});
    fxDown_->setBounds({fxDetail_.right() - 118, fxDetail_.y + 4, 47, 19});
    fxRemove_->setBounds({fxDetail_.right() - 68, fxDetail_.y + 4, 63, 19});
    if (node->type == Effects::Type::Split3)
        for (int band = 0; band < 3; ++band)
        {
            auto* button = fxBandButtons_[(size_t) band]; button->visible = true;
            button->setBounds({fxDetail_.right() - 370 + (float) band * 66, fxDetail_.y + 4, 62, 19});
        }
    if (node->type == Effects::Type::Convolution)
    {
        auto copy = fxConfiguration_; const auto* module = effectModule(copy, node->bus, node->path);
        fxImpulse_->visible = true;
        fxImpulse_->setLabel(module && module->impulse ? module->impulse->name() : "LOAD IMPULSE RESPONSE...");
        fxImpulse_->setBounds({fxDetail_.right() - 370, fxDetail_.y + 4, 200, 19});
    }
    // The stack: one Serum-style panel per module of this bus (nested band chains included). A panel is a hide gutter, the
    // name block, a dark body with the mode selector, the graph and the knobs, then the MIX column.
    const Rect stack{fxDetail_.x + 4, fxDetail_.y + 28, fxDetail_.w - 8, std::max(40.0f, fxDetail_.bottom() - fxDetail_.y - 52)};
    fxControlsArea_ = stack;
    constexpr float gutter = 22, nameWidth = 112, mixWidth = 74, cellWidth = 58, rowHeight = 60, modeWidth = 104, graphWidth = 132;
    const int key = fxViewKey();
    const auto collapsed = [&](Effects::ModuleId id) { return collapsedEffects()[key].contains((uint32_t) id); };
    const auto modeDescriptor = [](Effects::Type type) {
        // The selector shown big in the body: Mode, then Shape, then Style.
        const auto descriptors = Effects::parameterDescriptors(type);
        for (const char* id : {"Mode", "Shape", "Style"})
            for (size_t d = 0; d < descriptors.size(); ++d) if (descriptors[d].id == id && ! descriptors[d].choices.empty()) return (int) d;
        return -1;
    };
    const auto hasGraph = [](Effects::Type type) {
        using T = Effects::Type;
        return type != T::Utility && type != T::FrequencyShift && type != T::Unknown;
    };
    // One compact row per module, like Serum's rack: 78 px open, 28 px hidden.
    const auto panelLayout = [&](const FxNode& item) {
        if (collapsed(item.id)) return std::pair{1, 28.0f};
        const float body = stack.w - gutter - nameWidth - mixWidth - 30 - (modeDescriptor(item.type) >= 0 ? modeWidth : 0.0f) - (hasGraph(item.type) ? graphWidth : 0.0f);
        return std::pair{std::max(1, (int) (body / cellWidth)), 78.0f};
    };
    fxControlsPages_ = std::max(1, (int) chain.size());
    int selectedIndex = 0;
    for (size_t i = 0; i < chain.size(); ++i) if (chain[i]->id == fxSelected_) selectedIndex = (int) i;
    fxControlsPage_ = std::clamp(fxControlsPage_, 0, fxControlsPages_ - 1);
    if (fxScrolledFor_ != fxSelected_)
    {
        // A newly selected module scrolls into view; manual scrolling is left alone afterwards.
        fxScrolledFor_ = fxSelected_;
        if (selectedIndex < fxControlsPage_) fxControlsPage_ = selectedIndex;
        while (fxControlsPage_ < selectedIndex)
        {
            float used = 0;
            for (int i = fxControlsPage_; i <= selectedIndex; ++i) used += panelLayout(*chain[(size_t) i]).second + 4;
            if (used <= stack.h + 4) break;
            ++fxControlsPage_;
        }
    }
    float y = stack.y;
    for (int i = fxControlsPage_; i < (int) chain.size(); ++i)
    {
        const auto& item = *chain[(size_t) i];
        const auto [columns, height] = panelLayout(item);
        if (y + height > stack.bottom() && i > fxControlsPage_) break;
        const float indent = (float) ((item.path.size() - 1) / 2) * 10;
        FxPanel panel{item.id, {stack.x + indent, y, stack.w - indent, std::min(height, stack.bottom() - y)}, {}, {}, {}, collapsed(item.id)};
        y += height + 4;
        if (panel.collapsed) { fxPanels_.push_back(panel); continue; }
        const SkColor accent = effectColor(item.type);
        float x = panel.bounds.x + gutter + nameWidth + 6;
        const float top = panel.bounds.y + 6, bodyHeight = panel.bounds.h - 12;
        if (const int mode = modeDescriptor(item.type); mode >= 0)
        {
            panel.mode = {x, top, modeWidth - 8, bodyHeight};
            const int parameter = processor_->fxParameterIndex(item.id, mode);
            showControl(parameter, {x + 6, top + bodyHeight * 0.5f - 24, modeWidth - 20, 40}, true);
            if (auto* choice = dynamic_cast<ParameterChoice*>(controls_[(size_t) parameter].widget)) { choice->stacked = true; choice->accent = accent; }
            x += modeWidth;
        }
        if (hasGraph(item.type)) { panel.graphic = {x, top + 2, graphWidth - 8, bodyHeight - 4}; x += graphWidth; }
        if (item.id == fxSelected_ && (item.type == Effects::Type::Split3 || item.type == Effects::Type::Convolution)) fxGraphic_ = panel.graphic;
        const int mix = Effects::parameterIndex(item.type, "Mix");
        if (const int parameter = mix >= 0 ? processor_->fxParameterIndex(item.id, mix) : -1; parameter >= 0)
        {
            showControl(parameter, {panel.bounds.right() - mixWidth + 6, panel.bounds.y + 6, mixWidth - 12, panel.bounds.h - 12});
            if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget)) { knob->accent = accent; knob->compactLabel = "MIX"; }
        }
        int placed = 0;
        for (const auto id : primaryEffectControls(item.type))
        {
            if (placed >= columns) break;
            const int descriptor = Effects::parameterIndex(item.type, id);
            const int parameter = descriptor >= 0 ? processor_->fxParameterIndex(item.id, descriptor) : -1;
            if (parameter < 0) continue;
            const Rect cell{x + (float) placed * cellWidth, top + (bodyHeight - rowHeight) * 0.5f, cellWidth - 4, rowHeight};
            const bool toggle = processor_->parameters()[(size_t) parameter].isToggle;
            showControl(parameter, toggle ? Rect{cell.x, cell.y + (cell.h - 21) * 0.5f, cell.w, 21} : cell);
            if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget)) knob->accent = accent;
            ++placed;
        }
        panel.more = {panel.bounds.right() - mixWidth - 26, panel.bounds.y + 8, 20, 16};
        fxPanels_.push_back(panel);
    }
    fxControlsPrevious_->visible = fxControlsNext_->visible = fxControlsPages_ > 1;
    fxControlsPrevious_->setBounds({fxDetail_.right() - 56, fxDetail_.bottom() - 21, 24, 17});
    fxControlsNext_->setBounds({fxDetail_.right() - 28, fxDetail_.bottom() - 21, 24, 17});
}
Rect WavetableEditor::fxPanelCell(Effects::ModuleId id) const
{
    for (const auto& panel : fxPanels_) if (panel.id == id) return panel.bounds;
    return {};
}
Rect WavetableEditor::fxPanelMoreCell(Effects::ModuleId id) const
{
    for (const auto& panel : fxPanels_) if (panel.id == id) return panel.more;
    return {};
}
Rect WavetableEditor::fxRowHideCell(Effects::ModuleId id) const
{
    for (const auto& row : fxRows) if (row.id == id) return row.hide;
    return {};
}
Rect WavetableEditor::fxRowRemoveCell(Effects::ModuleId id) const
{
    for (const auto& row : fxRows) if (row.id == id) return row.remove;
    return {};
}
bool WavetableEditor::fxCollapsed(Effects::ModuleId id) const
{
    const auto found = collapsedEffects().find(fxViewKey());
    return found != collapsedEffects().end() && found->second.contains((uint32_t) id);
}
void WavetableEditor::toggleFxCollapsed(Effects::ModuleId id)
{
    auto& set = collapsedEffects()[fxViewKey()];
    if (! set.erase((uint32_t) id)) set.insert((uint32_t) id);
    layout(); repaint();
}
void WavetableEditor::deleteFxModule(Effects::ModuleId id)
{
    selectFxModule(id);
    if (fxSelected_ == id) removeEffect(); // may rebuild this editor
}
void WavetableEditor::layoutMixer()
{
    const Rect rack{upper_.x + 428, upper_.y, upper_.w - 428, upper_.h};
    if (! routingViewLoaded_)
        if (const auto found = ctx().wavetableRoutingViews.find(fxViewKey()); found != ctx().wavetableRoutingViews.end()) mixSources_ = found->second.sources;
    mixSourcesButton_->visible = mixBusesButton_->visible = true;
    mixSourcesButton_->setBounds(mixPageCell(true)); mixBusesButton_->setBounds(mixPageCell(false));
    if (mixSources_)
    {
        routingEditor_->visible = true; routingEditor_->setBounds({rack.x, rack.y + 27, rack.w, std::max(100.0f, rack.h - 99)}); routingEditor_->layout();
        for (int bus = 0; bus < 3; ++bus)
        { auto* button = mixOpenButtons_[(size_t) bus]; button->visible = true; button->setBounds({rack.x + 214 + (float) bus * 64, rack.y + 3, 60, 21}); }
        showControl(W::FilterRouting, {rack.x + 410, rack.y + 3, rack.w - 414, 21}, true);
    }
    else
    {
    showControl(W::FilterRouting, {rack.x + 12, rack.y + 31, 250, 25}, true);
    const float width = (rack.w - 24) / 3;
    const float cardHeight = std::clamp(rack.h - 180, 40.0f, 92.0f);
    for (int bus = 0; bus < Effects::busCount; ++bus)
    {
        auto* button = mixOpenButtons_[(size_t) bus]; button->visible = true;
        button->setBounds({rack.x + 9 + (float) bus * width, rack.y + 74, width - 6, cardHeight < 64 ? 20.0f : 23.0f});
    }
    }
    if (! processor_->hasEffectsConfiguration())
    {
        fxEnableBuses_->visible = true; fxEnableBuses_->setBounds({rack.x + 12, rack.bottom() - 56, rack.w - 24, 30}); return;
    }
    const float knobWidth = (rack.w - 20) / (float) W::FxRoutingParams;
    for (int route = 0; route < W::FxRoutingParams; ++route)
        showControl(processor_->fxRoutingParameterIndex((W::FxRoutingParam) route),
                    {rack.x + 10 + (float) route * knobWidth, rack.bottom() - (mixSources_ ? 72.0f : 79.0f), knobWidth - 3, mixSources_ ? 58.0f : 65.0f});
}
Rect WavetableEditor::mixPageCell(bool sources) const
{
    if (inspector_ || workflow_ != Page::Mix) return {};
    return {upper_.x + 432 + (sources ? 0.0f : 105.0f), upper_.y + 3, 100, 21};
}
void WavetableEditor::layoutModulation()
{
    auto r = lower_; macroPanel_ = r.removeFromLeft(120); r.removeFromLeft(6);
    envelopePanel_ = r.removeFromLeft(300); r.removeFromLeft(6); voicePanel_ = r.removeFromRight(150); r.removeFromRight(6); lfoPanel_ = r;
    const float macroHeight = (macroPanel_.h - 27) / 4;
    for (int macro = 0; macro < W::kMacros; ++macro)
        showControl(W::macroParam(macro), {macroPanel_.x + 3 + (float) (macro / 4) * 57, macroPanel_.y + 26 + (float) (macro % 4) * macroHeight, 54, macroHeight - 1});
    macroMore_->visible = true; macroMore_->setBounds({macroPanel_.right() - 24, macroPanel_.y + 4, 19, 17});
    const auto handle = [this](int source, Rect bounds) { auto* widget = modHandles_[(size_t) source]; widget->visible = true; widget->setBounds(bounds); };
    for (size_t envelope = 0; envelope < envelopeButtons_.size(); ++envelope)
    {
        const auto cell = envelopeCell((int) envelope);
        envelopeButtons_[envelope]->visible = true; envelopeButtons_[envelope]->setBounds({cell.x, cell.y, cell.w - 17, cell.h});
        handle(W::Env1 + (int) envelope, {cell.right() - 16, cell.y + 3, 15, cell.h - 6});
    }
    for (int macro = 0; macro < W::kMacros; ++macro)
    {
        const auto knob = controls_[(size_t) W::macroParam(macro)].widget->bounds();
        handle(W::Macro1 + macro, {knob.x + 1, knob.y + 1, 11, 11});
    }
    envelopeMore_->visible = true; envelopeMore_->setBounds({envelopePanel_.right() - 21, envelopePanel_.y + 2, 19, 18});
    const W::EnvParam envelopeParams[]{W::Attack, W::Hold, W::Decay, W::Sustain, W::Release, W::Delay};
    for (int i = 0; i < 6; ++i)
        showControl(W::envParam(instances_[(size_t) Section::Envelopes], envelopeParams[i]),
                    {envelopePanel_.x + 3 + (float) i * 49, envelopePanel_.bottom() - 59, 48, 57});
    const int selectedEnvelope = instances_[(size_t) Section::Envelopes];
    if (selectedEnvelope > 0)
    {
        showControl(W::envLevelParam(selectedEnvelope, false), {envelopePanel_.x + 4, envelopePanel_.y + 25, 66, 24}, true);
        showControl(W::envLevelParam(selectedEnvelope, true), {envelopePanel_.right() - 70, envelopePanel_.y + 25, 66, 24}, true);
    }
    for (size_t lfo = 0; lfo < lfoButtons_.size(); ++lfo)
    {
        const auto cell = lfoCell((int) lfo);
        lfoButtons_[lfo]->visible = true; lfoButtons_[lfo]->setBounds({cell.x, cell.y, cell.w - 14, cell.h});
        handle(W::Lfo1 + (int) lfo, {cell.right() - 13, cell.y + 4, 12, cell.h - 8});
    }
    lfoMore_->visible = true; lfoMore_->setBounds({lfoPanel_.right() - 21, lfoPanel_.y + 2, 19, 18});
    // Serum's LFO strip: FREE / RETRIG / ENVELOPE beside the graph; BPM|HZ, RATE, RISE, PHASE, HOST, 10x and the shape below.
    const int lfo = instances_[(size_t) Section::Lfos]; const float y = lfoPanel_.bottom() - 59;
    for (int mode = 0; mode < 3; ++mode) { lfoModes_[(size_t) mode]->visible = true; lfoModes_[(size_t) mode]->setBounds(lfoModeCell(mode)); }
    showControl(W::lfoTypeParam(lfo), {lfoPanel_.x + 6, lfoPanel_.y + 29, 88, 22}, true); // Normal / Chaos: Lorenz / Chaos: Rossler / Random S&H
    const float scale = lfoPanel_.w / 514;
    auto control = [&](int parameter, float x, float w, bool toggle = false, const char* caption = nullptr) {
        showControl(parameter, {lfoPanel_.x + x * scale, y + (toggle ? 20 : 0), w * scale - 3, toggle ? 22.0f : 57.0f});
        if (auto* knob = dynamic_cast<PanelKnob*>(controls_[(size_t) parameter].widget); knob && caption) knob->compactLabel = caption;
    };
    control(W::lfoParam(lfo, W::SyncOn), 4, 44, true);
    if (auto* sync = dynamic_cast<ParameterToggle*>(controls_[(size_t) W::lfoParam(lfo, W::SyncOn)].widget))
        sync->stateLabel = [](bool on) { return std::string(on ? "BPM" : "HZ"); };
    const bool synced = processor_->getParameter(W::lfoParam(lfo, W::SyncOn)) >= 0.5f;
    if (synced)
    {
        auto* knob = lfoDivisionKnobs_[(size_t) lfo];
        knob->visible = true; knob->setBounds({lfoPanel_.x + 50 * scale, y, 56 * scale - 3, 57});
        if (auto* panelKnob = dynamic_cast<PanelKnob*>(knob)) panelKnob->compactLabel = "RATE";
    }
    else control(W::lfoParam(lfo, W::Rate), 50, 56, false, "RATE");
    control(W::lfoParam(lfo, W::Fade), 108, 50, false, "RISE");
    control(W::lfoParam(lfo, W::LfoPhase), 160, 50, false, "PHASE");
    control(W::lfoAnchorParam(lfo), 214, 50, true);
    control(W::lfoRateScaleParam(lfo), 268, 36, true);
    if ((int) std::lround(processor_->getParameter(W::lfoTypeParam(lfo))) == W::LfoNormal) control(W::lfoParam(lfo, W::Shape), 310, 120);
    if (auto* anchor = dynamic_cast<ParameterToggle*>(controls_[(size_t) W::lfoAnchorParam(lfo)].widget)) anchor->stateLabel = [](bool) { return std::string("HOST"); };
    for (int tab = 0; tab < 3; ++tab) { voiceTabs_[(size_t) tab]->visible = true; voiceTabs_[(size_t) tab]->setBounds(voiceTabCell(tab)); }
    for (int tab = 1; tab < 3; ++tab)
    {
        const auto cell = voiceTabCell(tab);
        voiceTabs_[(size_t) tab]->setBounds({cell.x, cell.y, cell.w - 15, cell.h});
        handle(tab == 1 ? W::Velocity : W::Keytrack, {cell.right() - 14, cell.y + 3, 13, cell.h - 6});
    }
    if (workflow_ == Page::Global || voiceTab_ != 0) return;
    const float expansion = std::clamp((voicePanel_.h - 164) / 74, 0.0f, 1.0f), column = (voicePanel_.w - 12) * 0.5f;
    const auto voiceControl = [&](W::VoiceModeParam parameter, int col, float yOffset, float h, bool compact = false, int span = 1) {
        showControl(W::voiceModeParam(parameter), {voicePanel_.x + 4 + (float) col * (column + 4), voicePanel_.y + yOffset, column * (float) span + 4 * (float) (span - 1), h}, compact);
    };
    voiceControl(W::MonoOn, 0, 28, 19); voiceControl(W::LegatoOn, 1, 28, 19);
    const float portaHeight = 24 + 8 * expansion;
    voiceControl(W::PortaTime, 0, 50 + expansion, portaHeight, true); voiceControl(W::PortaCurve, 1, 50 + expansion, portaHeight, true);
    voiceControl(W::PortaAlways, 0, 77 + 13 * expansion, 18); voiceControl(W::PortaScaled, 1, 77 + 13 * expansion, 18);
    voiceControl(W::NotePriority, 0, 99 + 14 * expansion, 22 + 3 * expansion, true);
    showControl(W::PolyphonyParam, {voicePanel_.x + 8 + column, voicePanel_.y + 96 + 14 * expansion, column, 34});
    if (auto* poly = dynamic_cast<PanelKnob*>(controls_[(size_t) W::PolyphonyParam].widget)) { poly->stepper = true; poly->compactLabel = "POLY"; }
}
void WavetableEditor::paintWaveform(SkCanvas& canvas, int oscillator, Rect plot)
{
    paint::fill(canvas, plot, kPlot);
    for (int i = 1; i < 4; ++i)
    {
        paint::line(canvas, plot.x + plot.w * (float) i / 4, plot.y, plot.x + plot.w * (float) i / 4, plot.bottom(), paint::withAlpha(kLine, 95));
        paint::line(canvas, plot.x, plot.y + plot.h * (float) i / 4, plot.right(), plot.y + plot.h * (float) i / 4, paint::withAlpha(kLine, 95));
    }
    const auto preview = processor_->wavePreview(oscillator);
    const bool enabled = processor_->getParameter(W::oscParam(oscillator, W::On)) > 0.5f;
    const auto color = enabled ? kCyan : paint::mix(kDim, kPlot, 0.45f);
    SkPathBuilder line, area; area.moveTo(plot.x, plot.centreY());
    for (size_t i = 0; i < preview.size(); ++i)
    {
        const float x = plot.x + (float) i / (float) (preview.size() - 1) * plot.w;
        const float y = plot.centreY() - preview[i] * plot.h * 0.43f;
        if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
        area.lineTo(x, y);
    }
    area.lineTo(plot.right(), plot.centreY()); area.close();
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(paint::withAlpha(color, enabled ? 29 : 12)); canvas.drawPath(area.detach(), pen);
    pen.setColor(color); pen.setStrokeWidth(1.7f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(line.detach(), pen);
    const auto mode = (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode)));
    const auto table = (int) std::lround(processor_->getParameter(W::oscParam(oscillator, W::Table)));
    std::string name = mode != W::WavetableMode ? processor_->oscillatorMediaName(oscillator)
                       : table == W::Custom ? processor_->customTableName(oscillator) : W::tableName(table);
    if (name.empty()) name = "Drop a wavetable";
    if (mode != W::WavetableMode && processor_->oscillatorMedia(oscillator) == nullptr) name = "Drop a sample";
    paint::text(canvas, name, {plot.x + 4, plot.y + 3, plot.w - 8, 12}, kDim, 8, Align::Left);
    const float position = processor_->getParameter(mode == W::WavetableMode ? W::oscParam(oscillator, W::Position)
                                                                           : W::mediaParam(oscillator, W::MediaStartPosition));
    if (mode != W::WavetableMode)
        paint::line(canvas, plot.x + position * plot.w, plot.y + 16, plot.x + position * plot.w, plot.bottom() - 15, kBlue);
    paint::text(canvas, number(mode == W::WavetableMode ? "%.0f %%" : "START %.0f %%", position * 100),
                {plot.x + 4, plot.bottom() - 14, plot.w - 8, 12}, kCyan, 8, Align::Right);
}
void WavetableEditor::paintTable3D(SkCanvas& canvas, int oscillator, Rect plot)
{
    // Stacked frames in a receding box: frame 1 at the front left, the last at the back right. Every frame is drawn
    // dimmer with depth; the frame the Position selects (with warps applied) is drawn in yellow at its depth.
    paint::fillRounded(canvas, plot, 2, kPlot);
    const bool enabled = processor_->getParameter(W::oscParam(oscillator, W::On)) > 0.5f;
    const int frames = std::max(1, processor_->tableFrameCount(oscillator));
    const float depthX = plot.w * 0.36f, depthY = plot.h * 0.44f;
    const float waveWidth = plot.w - depthX - 14, amplitude = std::max(4.0f, (plot.h - depthY - 14) * 0.4f);
    const float frontX = plot.x + 7, frontY = plot.bottom() - 7 - amplitude;
    const auto origin = [&](float depth) { return std::pair{frontX + depth * depthX, frontY - depth * depthY}; };
    canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h));
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1);
    pen.setColor(paint::withAlpha(kLine, 170));
    const auto [backX, backY] = origin(1);
    for (const float y : {-amplitude, amplitude})
    {
        canvas.drawLine(frontX, frontY + y, backX, backY + y, pen);
        canvas.drawLine(frontX + waveWidth, frontY + y, backX + waveWidth, backY + y, pen);
    }
    canvas.drawLine(backX, backY - amplitude, backX + waveWidth, backY - amplitude, pen);
    canvas.drawLine(backX + waveWidth, backY - amplitude, backX + waveWidth, backY + amplitude, pen);
    constexpr int kPoints = 96, kMaxFrames = 32;
    const int drawn = std::min(frames, kMaxFrames);
    const auto drawFrame = [&](float depth, const std::function<float(float)>& wave, SkColor line, SkColor fill, float width) {
        const auto [x0, y0] = origin(depth);
        SkPathBuilder curve, area; area.moveTo(x0, y0);
        for (int i = 0; i <= kPoints; ++i)
        {
            const float phase = (float) i / kPoints, x = x0 + phase * waveWidth;
            const float y = y0 - std::clamp(wave(phase), -1.2f, 1.2f) * amplitude;
            if (i == 0) curve.moveTo(x, y); else curve.lineTo(x, y);
            area.lineTo(x, y);
        }
        area.lineTo(x0 + waveWidth, y0); area.close();
        SkPaint brush; brush.setAntiAlias(true); brush.setColor(fill); canvas.drawPath(area.detach(), brush);
        brush.setColor(line); brush.setStyle(SkPaint::kStroke_Style); brush.setStrokeWidth(width); brush.setStrokeJoin(SkPaint::kRound_Join);
        canvas.drawPath(curve.detach(), brush);
    };
    // The highlighted frame and its warps follow the sounding voice's modulation.
    const float livePosition = processor_->getParameter(W::oscParam(oscillator, W::Position)) + liveAmount(W::Osc1Position + oscillator);
    const float warp1 = processor_->getParameter(W::oscParam(oscillator, W::Warp1Amount)) + liveAmount(W::Osc1Warp1 + oscillator);
    const float warp2 = processor_->getParameter(W::oscParam(oscillator, W::Warp2Amount)) + liveAmount(W::Osc1Warp2 + oscillator);
    const float position = frames > 1 ? processor_->tableFramePosition(oscillator, livePosition) / (float) (frames - 1) : 0;
    const auto highlight = [&] {
        drawFrame(position, [&](float phase) { return processor_->waveformAt(oscillator, phase, livePosition, warp1, warp2); },
                  enabled ? kFrame : paint::mix(kFrame, kPlot, 0.5f),
                  paint::withAlpha(kFrame, enabled ? 48 : 20), 1.9f);
    };
    bool highlighted = false;
    for (int k = drawn - 1; k >= 0; --k)
    {
        const int frame = drawn == 1 ? 0 : (int) std::lround((double) k * (frames - 1) / (drawn - 1));
        const float depth = frames == 1 ? 0 : (float) frame / (float) (frames - 1);
        if (! highlighted && depth < position - 1e-4f) { highlight(); highlighted = true; } // frames in front of it overlap it
        if (std::abs(depth - position) <= 1e-4f) continue;
        const auto alpha = (uint8_t) std::lround((enabled ? 215 : 110) - 135 * depth);
        drawFrame(depth, [&](float phase) { return processor_->tableFrameAt(oscillator, frame, phase); },
                  paint::withAlpha(kCyan, alpha), paint::withAlpha(kCyan, (uint8_t) std::lround(16 - 9 * depth)), 1.1f);
    }
    if (! highlighted) highlight();
    canvas.restore();
}
void WavetableEditor::paintFilter(SkCanvas& canvas, int filter, Rect plot)
{
    paint::fill(canvas, plot, kPlot);
    float cutoff = processor_->getParameter(W::filterParam(filter, W::Cutoff));
    float resonance = processor_->getParameter(W::filterParam(filter, W::Resonance));
    if (live_.active)
    {
        // The curve follows the voice: matrix routes (8 octaves per unit), the filter's own envelope amount and key tracking.
        const int envelope = std::clamp((int) std::lround(processor_->getParameter(W::filterParam(filter, W::EnvSource))), 0, W::kEnvelopes - 1);
        const float octaves = liveAmount(W::Filter1Cutoff + filter) * 8
            + processor_->getParameter(W::filterParam(filter, W::EnvAmount)) * live_.envelopeLevel[(size_t) envelope]
            + processor_->getParameter(W::filterParam(filter, W::FilterKeytrack)) * (float) (live_.note - 60) / 12;
        cutoff = std::clamp(cutoff * std::exp2(std::clamp(octaves, -16.0f, 16.0f)), 20.0f, 20000.0f);
        resonance = std::clamp(resonance + liveAmount(W::Filter1Resonance + filter), 0.0f, 1.0f);
    }
    const int mode = (int) std::lround(processor_->getParameter(W::filterParam(filter, W::FilterMode)));
    for (int i = 1; i < 4; ++i) paint::line(canvas, plot.x, plot.y + (float) i * plot.h / 4, plot.right(), plot.y + (float) i * plot.h / 4, paint::withAlpha(kLine, 90));
    SkPathBuilder path;
    for (int i = 0; i <= 100; ++i)
    {
        const float frequency = 20 * std::pow(1000.0f, (float) i / 100), ratio = frequency / cutoff;
        const float low = 1 / std::sqrt(1 + ratio * ratio * ratio * ratio), high = 1 - low;
        float response = mode <= W::Low24 ? low : mode <= W::High24 ? high : mode == W::BandPass ? 4 * low * high : mode == W::Notch ? 1 - 3.7f * low * high : low * 0.5f + high * 0.5f;
        if (mode == W::Low24 || mode == W::High24) response *= response;
        const float var = processor_->getParameter(W::filterParam(filter, W::FilterMorph));
        if (W::isMeasuredSvfFilter(mode))
            response = dsp::MeasuredSvfFilter::magnitude(mode - W::SerumLow6, frequency, cutoff, resonance, var);
        else if (W::isMeasuredMultiFilter(mode))
            response = dsp::MeasuredMultiFilter::magnitude(mode - W::SerumPeak12, frequency, cutoff, resonance, var);
        else if (mode == W::SerumLadderAcid) response = dsp::DiodeLadderFilter::magnitude(frequency, cutoff, resonance);
        else if (W::isMeasuredCombFilter(mode))
            response = dsp::MeasuredCombFilter::magnitude(mode - W::SerumCombP, frequency, cutoff, resonance, var);
        else if (W::isLadderFilter(mode))
        {
            // Ladder: N poles at the tap, feedback 4 * resonance around all four.
            const std::complex<float> pole = 1.0f / std::complex<float>(1.0f, mode == W::SerumDirtyMg ? ratio / 0.841f : ratio);
            const auto loop = pole * pole * pole * pole;
            std::complex<float> tap = 1.0f;
            for (int stage = 0; stage < (mode == W::SerumLadderMg || mode == W::SerumDirtyMg ? 4 : mode - W::MgLow6 + 1); ++stage) tap *= pole;
            const float feedback = 4.0f * (mode == W::SerumDirtyMg ? W::dirtyMgFeedback(resonance) : resonance);
            response = std::abs(tap / (1.0f + feedback * loop)) * (mode == W::SerumLadderMg ? 0.8007f * (1 + 4 * resonance)
                : mode == W::SerumDirtyMg ? 0.893f : 1 + 4 * resonance * processor_->getParameter(W::filterParam(filter, W::FilterMorph)));
        }
        else if (mode == W::FilterThrough) response = 1;
        else if (mode == W::SerumLadderEms)
        {
            std::complex<float> denominator = 1.0f;
            for (const float stage : {4.78587f, 2.81082f, 1.01439f, 0.0732825f}) denominator *= std::complex<float>(1.0f, ratio / (0.762f * stage));
            response = std::abs(1.121f * (1 + 33.7f * resonance) / (denominator + 33.7f * resonance));
        }
        else
        {
            const float distance = std::log(std::max(ratio, 0.001f)) * 4.0f;
            response += resonance * 0.24f * std::exp(-distance * distance);
        }
        const float x = plot.x + plot.w * (float) i / 100, y = plot.bottom() - 5 - std::clamp(response, 0.0f, 1.25f) * (plot.h - 12) * 0.75f;
        if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(kBlue); pen.setStrokeWidth(1.7f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(path.detach(), pen);
    paint::text(canvas, "20 Hz", {plot.x + 3, plot.bottom() - 12, plot.w * 0.5f, 10}, kDim, 7.5f);
    paint::text(canvas, "20 kHz", {plot.x, plot.bottom() - 12, plot.w - 3, 10}, kDim, 7.5f, Align::Right);
}
void WavetableEditor::paintEnvelope(SkCanvas& canvas, Rect plot)
{
    paint::fill(canvas, plot, kPlot);
    canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h));
    const int envelope = instances_[(size_t) Section::Envelopes];
    const auto geometry = envelopeShape();
    const float delay = geometry.delay, attack = geometry.attack, hold = geometry.hold, decay = geometry.decay, release = geometry.release;
    const bool imported = geometry.imported;
    const float sustain = geometry.sustain, startLevel = geometry.start, endLevel = geometry.end;
    const auto shape = [&](float t, W::EnvParam curve) { return processor_->envelopeCurveAt(envelope, curve, t); };
    const float sustaining = geometry.sustaining, total = geometry.total;
    for (int i = 1; i < 5; ++i) paint::line(canvas, plot.x + plot.w * (float) i / 5, plot.y, plot.x + plot.w * (float) i / 5, plot.bottom(), paint::withAlpha(kLine, 100));
    SkPathBuilder line, area; area.moveTo(plot.x, plot.bottom());
    for (int i = 0; i <= 240; ++i)
    {
        float time = total * (float) i / 240, level = startLevel;
        if (time >= delay)
        {
            time -= delay;
            if (time < attack && attack > 0) level = startLevel + (1 - startLevel) * shape(time / attack, W::AttackCurve);
            else if ((time -= attack) < hold) level = 1;
            else if ((time -= hold) < decay && decay > 0) level = 1 - (1 - sustain) * shape(time / decay, W::DecayCurve);
            else if ((time -= decay) < sustaining) level = sustain;
            else level = sustain + (endLevel - sustain) * shape(release > 0 ? (time - sustaining) / release : 1, W::ReleaseCurve);
        }
        if (! imported) level *= level;
        const float x = plot.x + plot.w * (float) i / 240, y = plot.bottom() - 2 - level * (plot.h - 7);
        if (i == 0) line.moveTo(x, y); else line.lineTo(x, y); area.lineTo(x, y);
    }
    area.lineTo(plot.right(), plot.bottom()); area.close();
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(paint::withAlpha(kBlue, 38)); canvas.drawPath(area.detach(), pen);
    pen.setColor(kBlue); pen.setStrokeWidth(1.8f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(line.detach(), pen);
    paint::text(canvas, number("%.2f s", total), {plot.x + 4, plot.bottom() - 13, plot.w - 8, 11}, kDim, 8, Align::Right);
    for (int point = 0; point < 7; ++point)
    {
        const auto cell = envelopePoint(point); if (cell.isEmpty()) continue;
        SkPaint dot; dot.setAntiAlias(true);
        const bool active = point == envelopeDrag_;
        if (point < 4) { dot.setColor(active ? kText : kCyan); canvas.drawCircle(cell.centreX(), cell.centreY(), active ? 4.0f : 3.0f, dot); }
        else { dot.setStyle(SkPaint::kStroke_Style); dot.setStrokeWidth(1.2f); dot.setColor(active ? kText : paint::withAlpha(kCyan, 190)); canvas.drawCircle(cell.centreX(), cell.centreY(), 3.2f, dot); }
    }
    canvas.restore();
    if (const int stage = live_.envelopeStage[(size_t) envelope]; live_.active && stage > 0)
    {
        // Playhead along the drawn segments: delay, attack, hold, decay, the drawn sustain stretch, release.
        const float elapsed = live_.envelopeElapsed[(size_t) envelope];
        const std::array<float, 6> segments{delay, attack, hold, decay, sustaining, release};
        float time = 0;
        for (int i = 0; i + 1 < std::min(stage, 7); ++i) time += segments[(size_t) i];
        if (stage <= 6) time += std::min(elapsed, segments[(size_t) stage - 1] * 0.999f);
        float level = live_.envelopeLevel[(size_t) envelope];
        if (! imported) level *= level;
        const float x = plot.x + plot.w * std::clamp(time / total, 0.0f, 1.0f), y = plot.bottom() - 2 - level * (plot.h - 7);
        paint::line(canvas, x, plot.y, x, plot.bottom(), paint::withAlpha(kCyan, 70));
        pen.setStyle(SkPaint::kFill_Style); pen.setColor(kCyan); canvas.drawCircle(x, y, 4, pen);
        pen.setColor(kText); canvas.drawCircle(x, y, 1.8f, pen);
    }
}
WavetableEditor::EnvelopeShape WavetableEditor::envelopeShape() const
{
    EnvelopeShape g;
    const int envelope = instances_[(size_t) Section::Envelopes];
    const auto value = [&](W::EnvParam p) { return processor_->getParameter(W::envParam(envelope, p)); };
    g.delay = value(W::Delay); g.attack = value(W::Attack); g.hold = value(W::Hold); g.decay = value(W::Decay); g.release = value(W::Release);
    g.imported = processor_->usesImportedEnvelopeProfile();
    g.sustain = g.imported ? value(W::Sustain) * value(W::Sustain) : value(W::Sustain);
    g.start = envelope > 0 ? processor_->getParameter(W::envLevelParam(envelope, false)) : 0;
    g.end = envelope > 0 ? processor_->getParameter(W::envLevelParam(envelope, true)) : 0;
    // While a point is dragged the time axis holds still, so the point follows the pointer.
    g.sustaining = envelopeDrag_ >= 0 ? envelopeFrozenSustaining_ : std::clamp((g.delay + g.attack + g.hold + g.decay + g.release) * 0.3f, 0.1f, 0.8f);
    g.total = envelopeDrag_ >= 0 ? envelopeFrozenTotal_ : std::max(0.01f, g.delay + g.attack + g.hold + g.decay + g.sustaining + g.release);
    return g;
}
std::pair<float, float> WavetableEditor::envelopeAt(const EnvelopeShape& g, float seconds, float level) const
{
    const auto plot = envelopePlot();
    const float shown = g.imported ? level : level * level;
    return {plot.x + plot.w * seconds / g.total, plot.bottom() - 2 - shown * (plot.h - 7)};
}
Rect WavetableEditor::envelopePoint(int point) const
{
    if (inspector_ || point < 0 || point > 6 || envelopePlot().isEmpty()) return {};
    const auto g = envelopeShape();
    const int envelope = instances_[(size_t) Section::Envelopes];
    const auto shape = [&](float t, W::EnvParam curve) { return processor_->envelopeCurveAt(envelope, curve, t); };
    const float attackEnd = g.delay + g.attack, holdEnd = attackEnd + g.hold, decayEnd = holdEnd + g.decay, releaseStart = decayEnd + g.sustaining;
    const auto level = [&](float seconds, float value) { return envelopeAt(g, seconds, value); };
    std::pair<float, float> at;
    switch (point)
    {
        case 0: at = level(attackEnd, 1); break;
        case 1: if (g.hold < 1e-3f) return {}; at = level(holdEnd, 1); break;
        case 2: at = level(decayEnd, g.sustain); break;
        case 3: at = level(releaseStart + g.release, g.end); break;
        case 4: if (g.attack < 1e-3f) return {}; at = level(g.delay + g.attack * 0.5f, g.start + (1 - g.start) * shape(0.5f, W::AttackCurve)); break;
        case 5: if (g.decay < 1e-3f) return {}; at = level(holdEnd + g.decay * 0.5f, 1 - (1 - g.sustain) * shape(0.5f, W::DecayCurve)); break;
        default: if (g.release < 1e-3f) return {}; at = level(releaseStart + g.release * 0.5f, g.sustain + (g.end - g.sustain) * shape(0.5f, W::ReleaseCurve)); break;
    }
    return {at.first - 5, at.second - 5, 10, 10};
}
void WavetableEditor::paintLfo(SkCanvas& canvas, Rect plot)
{
    paint::fill(canvas, plot, kPlot);
    const int shown = instances_[(size_t) Section::Lfos];
    const int lfoType = (int) std::lround(processor_->getParameter(W::lfoTypeParam(shown)));
    if (lfoType != W::LfoLorenz && lfoType != W::LfoRossler)
    {
        for (int i = 1; i < 8; ++i) paint::line(canvas, plot.x + plot.w * (float) i / 8, plot.y, plot.x + plot.w * (float) i / 8, plot.bottom(), paint::withAlpha(kLine, 110));
        for (int i = 1; i < 4; ++i) paint::line(canvas, plot.x, plot.y + plot.h * (float) i / 4, plot.right(), plot.y + plot.h * (float) i / 4, paint::withAlpha(kLine, 110));
    }
    const bool running = live_.active || (int) std::lround(processor_->getParameter(W::lfoParam(shown, W::Mode))) == W::Global;
    if (lfoType == W::LfoLorenz || lfoType == W::LfoRossler)
    {
        // Serum draws the attractor itself: a faint long trajectory with the current position on it.
        const auto& trajectory = W::chaosTrajectory(lfoType);
        SkPathBuilder orbit;
        for (size_t i = 0; i < trajectory.size(); ++i)
        {
            const float x = plot.x + 6 + trajectory[i][0] * (plot.w - 12), y = plot.bottom() - 6 - trajectory[i][1] * (plot.h - 12);
            if (i == 0) orbit.moveTo(x, y); else orbit.lineTo(x, y);
        }
        SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(0.8f); pen.setColor(paint::withAlpha(kCyan, 110));
        canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h)); canvas.drawPath(orbit.detach(), pen); canvas.restore();
        if (const auto point = live_.lfoPoint[(size_t) shown]; running && (point[0] > 0 || point[1] > 0))
        {
            const float x = plot.x + 6 + point[0] * (plot.w - 12), y = plot.bottom() - 6 - point[1] * (plot.h - 12);
            pen.setStyle(SkPaint::kFill_Style); pen.setColor(kLfo); canvas.drawCircle(x, y, 4.2f, pen); pen.setColor(kText); canvas.drawCircle(x, y, 1.8f, pen);
        }
        return;
    }
    if (lfoType == W::LfoRandomHold)
    {
        // Random steps, one per cycle, with the current held value.
        uint32_t seed = 0x51ed27u; SkPathBuilder steps;
        for (int i = 0; i < 16; ++i)
        {
            seed = seed * 1664525u + 1013904223u; const float level = (float) (seed >> 8) / 16777216.0f;
            const float x0 = plot.x + plot.w * (float) i / 16, x1 = plot.x + plot.w * (float) (i + 1) / 16, y = plot.bottom() - level * plot.h;
            if (i == 0) steps.moveTo(x0, y); else steps.lineTo(x0, y);
            steps.lineTo(x1, y);
        }
        SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.8f); pen.setColor(kBlue); canvas.drawPath(steps.detach(), pen);
        if (running)
        {
            const bool bipolar = processor_->getParameter(W::lfoParam(shown, W::Bipolar)) >= 0.5f;
            const float value = std::clamp(bipolar ? (live_.lfoValue[(size_t) shown] + 1) * 0.5f : live_.lfoValue[(size_t) shown], 0.0f, 1.0f);
            paint::line(canvas, plot.x, plot.bottom() - value * plot.h, plot.right(), plot.bottom() - value * plot.h, paint::withAlpha(kLfo, 180));
        }
        return;
    }
    const auto graph = dragLfoPoint_ >= 0 ? lfoDraft_ : processor_->lfoCurve(instances_[(size_t) Section::Lfos]);
    const bool imported = processor_->usesImportedLfoProfile();
    dsp::ExponentialCurveGraph exponential; if (imported) exponential.prepare(graph);
    const auto evaluate = [&](float phase) { return imported ? exponential.eval(phase) : graph.eval(phase); };
    SkPathBuilder line, area; area.moveTo(plot.x, plot.bottom());
    for (int i = 0; i <= 300; ++i)
    {
        const float phase = (float) i / 300, x = plot.x + phase * plot.w, y = plot.bottom() - evaluate(phase) * plot.h;
        if (i == 0) line.moveTo(x, y); else line.lineTo(x, y); area.lineTo(x, y);
    }
    area.lineTo(plot.right(), plot.bottom()); area.close();
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(paint::withAlpha(kBlue, 35)); canvas.drawPath(area.detach(), pen);
    pen.setColor(kBlue); pen.setStrokeWidth(1.8f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(line.detach(), pen);
    pen.setStyle(SkPaint::kFill_Style); pen.setColor(kCyan);
    for (int i = 0; i < graph.count; ++i) canvas.drawCircle(plot.x + graph.x[i] * plot.w, plot.bottom() - graph.y[i] * plot.h, 2.2f, pen);
    const int lfo = instances_[(size_t) Section::Lfos];
    const bool global = (int) std::lround(processor_->getParameter(W::lfoParam(lfo, W::Mode))) == W::Global;
    if ((live_.active || global) && dragLfoPoint_ < 0)
    {
        const float phase = std::clamp(live_.lfoPhase[(size_t) lfo], 0.0f, 1.0f);
        const float x = plot.x + phase * plot.w, y = plot.bottom() - evaluate(phase) * plot.h;
        paint::line(canvas, x, plot.y, x, plot.bottom(), paint::withAlpha(kLfo, 90));
        pen.setColor(kLfo); canvas.drawCircle(x, y, 4.2f, pen);
        pen.setColor(kText); canvas.drawCircle(x, y, 1.8f, pen);
        pen.setColor(kCyan);
    }
    pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1);
    for (int i = 0; i + 1 < graph.count; ++i)
        if (graph.x[i + 1] > graph.x[i] + 1e-4f && std::abs(graph.y[i + 1] - graph.y[i]) > 1e-4f)
        {
            const float middle = (graph.x[i] + graph.x[i + 1]) * 0.5f;
            canvas.drawCircle(plot.x + middle * plot.w, plot.bottom() - evaluate(middle) * plot.h, 3, pen);
        }
}
void WavetableEditor::paintNoisePreview(SkCanvas& canvas, Rect plot)
{
    if (plot.isEmpty()) return;
    paint::fill(canvas, plot, kPlot);
    const auto* sample = processor_->noiseSample();
    if (const int type = (int) std::lround(processor_->getParameter(W::NoiseTypeParam)); type != W::NoiseSampleType)
    {
        // A fixed excerpt of the selected colour, drawn with the same generators as the engine.
        uint32_t seed = 0x1234567u; std::array<float, 3> pink{}; float brown = 0, geiger = 0;
        SkPathBuilder path;
        for (int i = 0; i <= 256; ++i)
        {
            seed = seed * 1664525u + 1013904223u; float raw = (float) (seed >> 8) / 8388608.0f - 1;
            if (type == W::NoisePink)
            { pink[0] = 0.99765f * pink[0] + raw * 0.099f; pink[1] = 0.963f * pink[1] + raw * 0.2965f; pink[2] = 0.57f * pink[2] + raw * 1.0527f; raw = (pink[0] + pink[1] + pink[2] + raw * 0.1848f) * 0.25f; }
            else if (type == W::NoiseBrown) { brown = (brown + 0.12f * raw) / 1.02f; raw = brown * 1.5f; }
            else if (type == W::NoiseGeiger) { geiger *= 0.55f; if (i % 37 == 11) geiger = raw < 0 ? -1.0f : 1.0f; raw = geiger; }
            const float x = plot.x + plot.w * (float) i / 256, y = plot.centreY() - std::clamp(raw, -1.0f, 1.0f) * plot.h * 0.4f;
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        SkPaint pen; pen.setAntiAlias(true); pen.setColor(kCyan); pen.setStrokeWidth(1); pen.setStyle(SkPaint::kStroke_Style);
        canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h)); canvas.drawPath(path.detach(), pen); canvas.restore();
        return;
    }
    if (sample == nullptr)
    {
        paint::text(canvas, generatedNoise() ? "Generated" : "Sample unavailable", plot.reduced(3), kDim, 8, Align::Centre);
        return;
    }
    canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h));
    for (int channel = 0; channel < sample->channels(); ++channel)
    {
        const float h = plot.h / static_cast<float>(sample->channels()), center = plot.y + (static_cast<float>(channel) + .5f) * h;
        const SkColor color = channel == 0 ? kCyan : kBlue;
        paint::line(canvas, plot.x, center, plot.right(), center, paint::withAlpha(kLine, 90));
        for (size_t bin = 0; bin < noisePeaks_.size(); ++bin)
        {
            const float x = plot.x + static_cast<float>(bin) * plot.w / static_cast<float>(noisePeaks_.size());
            const auto& range = noisePeaks_[bin];
            paint::line(canvas, x, center - range.maximum[(size_t) channel] * h * .4f * noisePreviewGain_,
                        x, center - range.minimum[(size_t) channel] * h * .4f * noisePreviewGain_, paint::withAlpha(color, 75));
        }
        SkPathBuilder path;
        for (int i = 0; i <= 512; ++i)
        {
            const float position = static_cast<float>(i) / 512, x = plot.x + position * plot.w;
            const float y = center - processor_->noiseWaveformAt(position, channel) * h * .4f * noisePreviewGain_;
            if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        SkPaint pen; pen.setAntiAlias(true); pen.setColor(color); pen.setStrokeWidth(1.3f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(path.detach(), pen);
    }
    const float marker = plot.x + processor_->getParameter(W::NoisePhase) * plot.w;
    paint::line(canvas, marker, plot.y, marker, plot.bottom(), paint::withAlpha(kText, 110));
    canvas.restore();
}
void WavetableEditor::paintModules(SkCanvas& canvas)
{
    if (workflow_ == Page::Osc)
    {
        panel(canvas, sub_, "SUB"); panel(canvas, noise_, "NOISE");
        for (int oscillator = 0; oscillator < 3; ++oscillator)
        {
            const auto r = oscillatorPanels_[(size_t) oscillator];
            panel(canvas, r, "OSC " + std::string(1, (char) ('A' + oscillator)), instances_[(size_t) Section::Oscillators] == oscillator);
            const auto l = oscillatorLayout(r);
            const bool table = (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode))) == W::WavetableMode;
            if (table) paintTable3D(canvas, oscillator, l.display); else paintWaveform(canvas, oscillator, l.display);
            // Frame number box (Serum shows the selected frame beside the phase fields).
            const Rect frame{l.phase.x, l.phase.y, l.phase.w * 0.2f - 1, l.phase.h};
            paint::fillRounded(canvas, frame.reduced(0, 0), 2, kBackground);
            const float livePosition = processor_->getParameter(W::oscParam(oscillator, W::Position)) + liveAmount(W::Osc1Position + oscillator);
            const std::string index = table ? std::to_string((int) std::lround(processor_->tableFramePosition(oscillator, livePosition)) + 1)
                                            : number("%.0f%%", processor_->getParameter(W::mediaParam(oscillator, W::MediaStartPosition)) * 100);
            paint::text(canvas, index, frame, kValue, 9, Align::Centre, true);
            // The knob rows sit on a lighter base, split by a rule before Pan/Level.
            paint::fillRounded(canvas, l.base, 3, kBase);
            const float divider = l.knobs.right() - l.knobs.w * 0.2f;
            paint::line(canvas, divider, l.base.y + 6, divider, l.base.bottom() - 6, kLine);
            if (table)
            {
                const float cell = l.knobs.w * 0.8f / 4, y = l.base.bottom() - 9;
                paint::text(canvas, "WARP", {l.warps.x + cell, y - 5, l.warps.w * 0.8f - 2 * cell, 10}, kDim, 8, Align::Centre, true);
                paint::line(canvas, l.warps.x + cell * 0.6f, y, l.warps.x + l.warps.w * 0.4f - 16, y, kLine);
                paint::line(canvas, l.warps.x + l.warps.w * 0.4f + 16, y, l.warps.x + l.warps.w * 0.8f - cell * 0.6f, y, kLine);
            }
        }
        for (int filter = 0; filter < 2; ++filter)
        {
            const auto r = filterPanels_[(size_t) filter]; panel(canvas, r, "FILTER " + std::to_string(filter + 1));
            paintFilter(canvas, filter, filterPlot(filter));
        }
        const auto subPlot = subPlot_;
        paint::fill(canvas, subPlot, kPlot);
        SkPathBuilder subLine;
        for (int i = 0; i <= 128; ++i)
        {
            const float phase = (float) i / 128, x = subPlot.x + phase * subPlot.w;
            const float level = processor_->subWaveformAt(phase);
            const float y = subPlot.centreY() - level * subPlot.h * 0.4f;
            if (i == 0) subLine.moveTo(x, y); else subLine.lineTo(x, y);
        }
        SkPaint pen; pen.setAntiAlias(true); pen.setColor(kCyan); pen.setStrokeWidth(1.3f); pen.setStyle(SkPaint::kStroke_Style);
        canvas.save(); canvas.clipRect(SkRect::MakeXYWH(subPlot.x, subPlot.y, subPlot.w, subPlot.h)); canvas.drawPath(subLine.detach(), pen); canvas.restore();
        paintNoisePreview(canvas, noisePlot_);
    }
    else if (workflow_ == Page::Matrix)
    {
        // The bound helper draws rows and an immutable curve workspace in this panel.
    }
    else if (workflow_ == Page::Mix)
    {
        for (int filter = 0; filter < 2; ++filter)
        {
            const auto r = filterPanels_[(size_t) filter]; panel(canvas, r, "FILTER " + std::to_string(filter + 1));
            paintFilter(canvas, filter, filterPlot(filter));
        }
        paintMixer(canvas);
    }
    else if (workflow_ == Page::Fx) paintEffects(canvas);
    else
    {
        panel(canvas, upper_, "GLOBAL · OUTPUT / SUB / NOISE / VOICE");
    }
}
void WavetableEditor::paintFxGraph(SkCanvas& canvas, const FxNode& node, Rect plot)
{
    // Small Serum-like graphs from each module's own controls (drawn shapes, not measured responses).
    using T = Effects::Type;
    if (node.type == T::Split3 || node.type == T::Convolution)
    {
        if (node.id != fxSelected_) { paint::fill(canvas, plot, kPlot); paint::text(canvas, "select to edit", plot, kDim, 8, Align::Centre); }
        return; // the selected one is drawn by the band / impulse view below
    }
    const SkColor accent = effectColor(node.type);
    const auto value = [&](const char* id, float fallback) {
        const int index = processor_->fxParameterIndex(node.id, Effects::parameterIndex(node.type, id));
        return index >= 0 ? processor_->getParameter(index) : fallback;
    };
    const auto logX = [&](float hertz) { return plot.x + std::clamp(std::log(std::max(20.0f, hertz) / 20) / std::log(1000.0f), 0.0f, 1.0f) * plot.w; };
    canvas.save(); canvas.clipRect(SkRect::MakeXYWH(plot.x, plot.y, plot.w, plot.h));
    SkPathBuilder line, area; area.moveTo(plot.x, plot.bottom());
    const auto curve = [&](const std::function<float(float)>& level) { // level 0..1 over x 0..1
        for (int i = 0; i <= 96; ++i)
        {
            const float u = (float) i / 96, x = plot.x + u * plot.w, y = plot.bottom() - 3 - std::clamp(level(u), 0.0f, 1.0f) * (plot.h - 8);
            if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
            area.lineTo(x, y);
        }
    };
    const auto hertzAt = [](float u) { return 20 * std::pow(1000.0f, u); };
    const bool cutoffDot = node.type == T::Delay || node.type == T::Filter;
    float dotX = 0, dotY = 0;
    switch (node.type)
    {
        case T::Delay:
        {
            const float centre = value("FilterFreq", 849), width = std::max(0.3f, value("FilterWidth", 6.75f));
            curve([&](float u) { const float octaves = std::log2(hertzAt(u) / centre); return std::exp(-octaves * octaves / (2 * width * width / 9)); });
            dotX = logX(centre); dotY = plot.bottom() - 16; break;
        }
        case T::Reverb:
        {
            const float damping = value("Damping", 8000), decay = value("Decay", 3);
            curve([&](float u) { const float ratio = hertzAt(u) / damping; return 0.8f / std::sqrt(1 + ratio * ratio * ratio * ratio) * std::min(1.0f, 0.4f + decay / 10); });
            break;
        }
        case T::Filter:
        {
            const int mode = (int) std::lround(value("Mode", 0)); const float cutoff = value("Cutoff", 10000), res = value("Resonance", 0.1f);
            curve([&](float u) {
                const float ratio = hertzAt(u) / cutoff, low = 1 / std::sqrt(1 + ratio * ratio * ratio * ratio), high = 1 - low;
                const float peak = res * 0.5f * std::exp(-std::pow(std::log(ratio) * 3, 2.0f));
                const float response = mode == 1 ? high : mode == 2 ? 3.5f * low * high : mode == 3 ? 1 - 3.5f * low * high : low;
                return 0.7f * response + peak;
            });
            dotX = logX(cutoff); dotY = plot.bottom() - 16; break;
        }
        case T::Eq:
        {
            curve([&](float u) {
                float db = 0;
                for (int band = 1; band <= 2; ++band)
                {
                    const std::string prefix = "Band" + std::to_string(band);
                    const float f = value((prefix + "Freq").c_str(), band == 1 ? 210 : 2040), g = value((prefix + "Gain").c_str(), 0);
                    const int type = (int) std::lround(value((prefix + "Type").c_str(), band == 1 ? 0 : 2));
                    const float octaves = std::log2(hertzAt(u) / f);
                    db += type == 1 ? g * std::exp(-octaves * octaves * 2) : type == 0 ? g / (1 + std::exp(octaves * 3)) : type == 2 ? g / (1 + std::exp(-octaves * 3))
                        : type == 3 ? -24 / (1 + std::exp(octaves * 4)) : -24 / (1 + std::exp(-octaves * 4));
                }
                return 0.5f + db / 48;
            });
            paint::line(canvas, plot.x, plot.centreY(), plot.right(), plot.centreY(), paint::withAlpha(kLine, 150));
            break;
        }
        case T::Distortion:
        {
            const float drive = std::pow(10.0f, value("Drive", 6) / 20);
            curve([&](float u) { const float x = u * 2 - 1; return 0.5f + 0.5f * std::tanh(x * drive) / std::tanh(drive); });
            break;
        }
        case T::Compressor:
        {
            const float threshold = value("Threshold", -18), ratio = std::max(1.0f, value("Ratio", 4));
            curve([&](float u) { const float in = -60 + u * 60, out = in < threshold ? in : threshold + (in - threshold) / ratio; return (out + 60) / 60; });
            break;
        }
        case T::Flanger: case T::Phaser: case T::Chorus: case T::HyperDimension: case T::FrequencyShift:
        case T::Convolution: case T::Utility: case T::Unknown: case T::Split3:
        {
            // Modulation effects: the LFO they sweep with (cycles grow with the rate).
            const float rate = value("Rate", 0.5f), cycles = std::clamp(1.0f + rate, 1.0f, 6.0f), depth = 0.25f + std::min(0.2f, value("Depth", 2) / 30);
            curve([&](float u) { return 0.5f + depth * std::sin(u * cycles * 6.2831853f); });
            break;
        }
    }
    area.lineTo(plot.right(), plot.bottom()); area.close();
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(paint::withAlpha(accent, 60)); canvas.drawPath(area.detach(), pen);
    pen.setColor(accent); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.6f); canvas.drawPath(line.detach(), pen);
    if (cutoffDot) { pen.setColor(kText); pen.setStrokeWidth(1.4f); canvas.drawCircle(dotX, dotY, 4, pen); }
    canvas.restore();
}
void WavetableEditor::paintEffects(SkCanvas& canvas)
{
    paint::fillRounded(canvas, fxChain_, 3, kPanel); paint::stroke(canvas, fxChain_, kLine);
    const auto chain = currentFxChain();
    paint::text(canvas, std::to_string(chain.size()) + " EFFECTS · " + std::to_string(fxChainPage_ + 1) + " / " + std::to_string(fxChainPageCount()),
                {fxChain_.x + 29, fxChain_.bottom() - 23, fxChain_.w - 58, 19}, kDim, 8, Align::Centre);
    if (chain.empty()) paint::text(canvas, "Add an effect to this bus", {fxChain_.x + 8, fxChain_.y + 75, fxChain_.w - 16, 24}, kDim, 10, Align::Centre);
    const auto* node = selectedFxNode();
    if (! node)
    {
        panel(canvas, fxDetail_, "EFFECTS");
        paint::text(canvas, "Build your signal chain", {fxDetail_.x + 24, fxDetail_.centreY() - 19, fxDetail_.w - 48, 30}, kText, 18, Align::Centre);
        paint::text(canvas, "Choose an effect with + ADD EFFECT", {fxDetail_.x + 24, fxDetail_.centreY() + 13, fxDetail_.w - 48, 24}, kDim, 11, Align::Centre);
        return;
    }
    panel(canvas, fxDetail_, std::string(kFxBuses[(size_t) fxBus_]) + "  FX RACK", false);
    for (const auto& item : fxPanels_)
    {
        const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [&](const auto& n) { return n.id == item.id; });
        if (found == fxNodes_.end()) continue;
        const bool selected = item.id == fxSelected_;
        const bool on = processor_->getParameter(processor_->fxEnabledParameterIndex(item.id)) > 0.5f;
        const SkColor accent = on ? effectColor(found->type) : paint::mix(effectColor(found->type), kPanel, 0.55f);
        const auto r = item.bounds;
        paint::fillRounded(canvas, r, 4, 0xff1a222c);
        // Gutter: ▼ hides the panel (► when hidden).
        paint::fill(canvas, {r.x, r.y, 22, r.h}, 0xff121920);
        ctl::triangle(canvas, r.x + 11, r.y + std::min(r.h * 0.5f, 14.0f), 8, 6, item.collapsed ? ctl::Dir::Right : ctl::Dir::Down, accent);
        // Name block: the type in its colour between Serum-like dashed rules.
        const Rect logo{r.x + 22, r.y, 112, r.h};
        const std::string name = upper(std::string(Effects::typeName(found->type)));
        if (item.collapsed)
        {
            paint::text(canvas, name, {logo.x + 8, r.y, 200, r.h}, accent, 10, Align::Left, true);
        }
        else
        {
            for (int line = 0; line < 2; ++line)
                for (int dash = 0; dash < 5; ++dash)
                {
                    const float y = logo.centreY() + (line == 0 ? -20.0f : 16.0f), x = logo.x + 14 + (float) dash * 17;
                    paint::line(canvas, x, y, x + 10, y, paint::withAlpha(accent, (uint8_t) (dash % 2 == line ? 220 : 110)));
                }
            paint::text(canvas, name, {logo.x + 4, logo.centreY() - 10, logo.w - 8, 20}, accent, name.size() > 9 ? 10.0f : 13.0f, Align::Centre, true);
            std::string where = kFxBuses[(size_t) found->bus];
            for (size_t i = 1; i < found->path.size(); i += 2) where += std::string(" / ") + kFxBands[(size_t) found->path[i]];
            paint::text(canvas, where, {logo.x + 4, r.bottom() - 16, logo.w - 8, 12}, kDim, 7.5f, Align::Centre);
            // Body and MIX column.
            const Rect body{logo.right() + 2, r.y + 4, r.w - 22 - 112 - 2 - 78, r.h - 8};
            paint::fillRounded(canvas, body, 4, 0xff0f151c);
            paint::line(canvas, r.right() - 76, r.y + 8, r.right() - 76, r.bottom() - 8, kLine);
            const auto hatch = [&](float x) {
                for (float y = body.y + 4; y < body.bottom() - 6; y += 6) paint::line(canvas, x, y + 6, x + 6, y, paint::withAlpha(kLine, 160));
            };
            if (! item.mode.isEmpty()) hatch(item.mode.right());
            if (! item.graphic.isEmpty()) { hatch(item.graphic.right() + 1); paintFxGraph(canvas, *found, item.graphic); }
            if (! item.more.isEmpty())
                for (int dot = 0; dot < 3; ++dot)
                {
                    SkPaint pen; pen.setAntiAlias(true); pen.setColor(kDim);
                    canvas.drawCircle(item.more.x + 4 + (float) dot * 6, item.more.centreY(), 1.6f, pen);
                }
        }
        SkPaint frame; frame.setAntiAlias(true); frame.setStyle(SkPaint::kStroke_Style); frame.setStrokeWidth(selected ? 1.6f : 1.0f);
        frame.setColor(selected ? accent : kLine);
        canvas.drawRoundRect(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), 4, 4, frame);
    }
    // Rack list: colour bar per row and the hide / delete icons.
    for (const auto& row : fxRows)
    {
        const auto found = std::find_if(fxNodes_.begin(), fxNodes_.end(), [&](const auto& n) { return n.id == row.id; });
        if (found == fxNodes_.end() || ! found->select->visible) continue;
        const auto button = found->select->bounds();
        paint::fill(canvas, {button.x - 6, button.y + 4, 3, button.h - 8}, effectColor(found->type));
        const bool hidden = fxCollapsed(row.id);
        SkPaint eye; eye.setAntiAlias(true); eye.setStyle(SkPaint::kStroke_Style); eye.setStrokeWidth(1.3f); eye.setColor(hidden ? kDim : kText);
        canvas.drawOval(SkRect::MakeXYWH(row.hide.x + 2, row.hide.y + 5, row.hide.w - 4, row.hide.h - 10), eye);
        if (hidden) paint::line(canvas, row.hide.x + 2, row.hide.bottom() - 3, row.hide.right() - 2, row.hide.y + 3, kDim);
        else { eye.setStyle(SkPaint::kFill_Style); canvas.drawCircle(row.hide.centreX(), row.hide.centreY(), 2, eye); }
        paint::line(canvas, row.remove.x + 4, row.remove.y + 4, row.remove.right() - 4, row.remove.bottom() - 4, kDim);
        paint::line(canvas, row.remove.right() - 4, row.remove.y + 4, row.remove.x + 4, row.remove.bottom() - 4, kDim);
    }
    if (fxControlsPages_ > 1)
        paint::text(canvas, std::to_string(fxControlsPage_ + 1) + " / " + std::to_string(fxControlsPages_),
                    {fxDetail_.right() - 140, fxDetail_.bottom() - 21, 80, 17}, kDim, 8, Align::Right);
    if (fxGraphic_.isEmpty()) return;
    paint::fill(canvas, fxGraphic_, kPlot); paint::stroke(canvas, fxGraphic_, kLine);
    if (node->type == Effects::Type::Split3)
    {
        const int lowerIndex = processor_->fxParameterIndex(node->id, 2), upperIndex = processor_->fxParameterIndex(node->id, 3);
        const float low = lowerIndex >= 0 ? processor_->getParameter(lowerIndex) : 250;
        const float high = upperIndex >= 0 ? std::max(low * 1.05f, processor_->getParameter(upperIndex)) : 2500;
        const auto x = [&](float hertz) { return fxGraphic_.x + std::clamp(std::log(std::max(20.0f, hertz) / 20) / std::log(1000.0f), 0.0f, 1.0f) * fxGraphic_.w; };
        const std::array<float, 4> edges{fxGraphic_.x, x(low), x(high), fxGraphic_.right()};
        const std::array<SkColor, 3> colors{kBlue, kCyan, 0xffe1ac59};
        for (int band = 0; band < 3; ++band)
        {
            Rect region{edges[(size_t) band], fxGraphic_.y, std::max(0.0f, edges[(size_t) band + 1] - edges[(size_t) band]), fxGraphic_.h};
            paint::fill(canvas, region.reduced(1), paint::withAlpha(colors[(size_t) band], 28));
            paint::text(canvas, kFxBands[(size_t) band], region.reduced(3), colors[(size_t) band], 9, Align::Centre, true);
        }
        paint::line(canvas, x(low), fxGraphic_.y, x(low), fxGraphic_.bottom(), kBlue);
        paint::line(canvas, x(high), fxGraphic_.y, x(high), fxGraphic_.bottom(), kCyan);
    }
    if (node->type == Effects::Type::Convolution)
    {
        auto configuration = fxConfiguration_; const auto* module = effectModule(configuration, node->bus, node->path);
        if (! module || ! module->impulse)
        { paint::text(canvas, "Drop an impulse response here", fxGraphic_, kDim, 11, Align::Centre); return; }
        const auto& impulse = *module->impulse; const auto& samples = impulse.originalSamples();
        const size_t frames = static_cast<size_t>(impulse.frames()), channels = static_cast<size_t>(impulse.channels());
        for (size_t bin = 0; bin < 256; ++bin)
        {
            const size_t begin = bin * frames / 256, end = std::max(begin + 1, (bin + 1) * frames / 256);
            float peak = 0;
            for (size_t frame = begin; frame < std::min(end, frames); ++frame)
                for (size_t channel = 0; channel < channels; ++channel) peak = std::max(peak, std::abs(samples[frame * channels + channel]));
            const float x = fxGraphic_.x + (float) bin / 255 * fxGraphic_.w;
            const float amplitude = std::min(1.0f, peak) * (fxGraphic_.h - 12) * 0.5f;
            paint::line(canvas, x, fxGraphic_.centreY() - amplitude, x, fxGraphic_.centreY() + amplitude, kCyan);
        }
    }
}
void WavetableEditor::paintMixer(SkCanvas& canvas)
{
    const Rect rack{upper_.x + 428, upper_.y, upper_.w - 428, upper_.h}; panel(canvas, rack, "BUS MIXER");
    if (mixSources_) return;
    const float width = (rack.w - 24) / 3;
    const float cardHeight = std::clamp(rack.h - 180, 40.0f, 92.0f);
    for (int bus = 0; bus < Effects::busCount; ++bus)
    {
        const Rect card{rack.x + 9 + (float) bus * width, rack.y + 68, width - 6, cardHeight};
        paint::fillRounded(canvas, card, 3, kPlot); paint::stroke(canvas, card, bus == 0 ? kCyan : kLine);
        const auto count = std::count_if(fxNodes_.begin(), fxNodes_.end(), [bus](const auto& node) { return node.bus == bus; });
        paint::text(canvas, std::to_string(count) + " effects",
                    {card.x + 5, cardHeight < 74 ? card.bottom() - 15 : card.y + 34, card.w - 10, cardHeight < 74 ? 12.0f : 20.0f}, kDim, 10, Align::Centre);
        std::string summary;
        for (const auto& module : fxConfiguration_.buses[(size_t) bus])
        { if (! summary.empty()) summary += " → "; summary += Effects::typeName(module.type); }
        if (cardHeight >= 74) paint::text(canvas, summary.empty() ? "Direct" : summary, {card.x + 5, card.y + 59, card.w - 10, 20}, kText, 8, Align::Centre);
    }
    paint::text(canvas, "Filters feed Main; sends feed Aux 1 / Aux 2 and return to the output",
                {rack.x + 10, rack.y + 74 + cardHeight, rack.w - 20, 20}, kDim, 9, Align::Centre);
}
void WavetableEditor::paintModulation(SkCanvas& canvas)
{
    panel(canvas, macroPanel_, "MACROS");
    paint::fillRounded(canvas, envelopePanel_, 3, kPanel); paint::stroke(canvas, envelopePanel_, kLine);
    paint::fillRounded(canvas, lfoPanel_, 3, kPanel); paint::stroke(canvas, lfoPanel_, kLine);
    paint::text(canvas, "LFO", {lfoPanel_.x + 3, lfoPanel_.y, 31, 22}, kDim, 9, Align::Centre, true);
    paintEnvelope(canvas, envelopePlot()); paintLfo(canvas, lfoPlot());
    paint::fillRounded(canvas, voicePanel_, 3, kPanel); paint::stroke(canvas, voicePanel_, kLine);
    paint::fill(canvas, {voicePanel_.x, voicePanel_.y, voicePanel_.w, 25}, kRaised);
    if (voiceTab_ != 0) { paintCurveSource(canvas); return; }
    const bool mono = processor_->getParameter(W::voiceModeParam(W::MonoOn)) > 0.5f;
    const float expansion = std::clamp((voicePanel_.h - 164) / 74, 0.0f, 1.0f);
    const float meterY = workflow_ == Page::Global ? 36 : 125 + 26 * expansion;
    const float modeY = workflow_ == Page::Global ? 67 : 149 + 33 * expansion;
    // Serum counts oscillator voices: notes x (enabled oscillators' unison + Noise + Sub), against POLY x the same.
    int perNote = 0;
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        if (processor_->getParameter(W::oscParam(oscillator, W::On)) > 0.5f)
            perNote += (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode))) == W::WavetableMode
                ? std::max(1, (int) std::lround(processor_->getParameter(W::oscParam(oscillator, W::Unison)))) : 1;
    perNote += (processor_->getParameter(W::NoiseOn) > 0.5f ? 1 : 0) + (processor_->getParameter(W::SubOn) > 0.5f ? 1 : 0);
    perNote = std::max(1, perNote);
    const int poly = mono ? 1 : (int) std::lround(processor_->getParameter(W::PolyphonyParam));
    paint::text(canvas, std::to_string((int) processor_->activeVoiceCount() * perNote) + " / " + std::to_string(poly * perNote),
                {voicePanel_.x + 4, voicePanel_.y + meterY, voicePanel_.w - 8, 20 + 5 * expansion}, kCyan, 17 + 3 * expansion, Align::Centre, true);
    paint::text(canvas, mono ? "MONOPHONIC" : "POLYPHONIC", {voicePanel_.x + 4, voicePanel_.y + modeY, voicePanel_.w - 8, 12 + 2 * expansion}, kDim, 8, Align::Centre);
    if (voicePanel_.h >= 236)
    {
        paint::text(canvas, "4 ENV  ·  10 LFO", {voicePanel_.x + 4, voicePanel_.bottom() - 40, voicePanel_.w - 8, 15}, kDim, 8, Align::Centre);
        paint::text(canvas, "64 MOD ROUTES", {voicePanel_.x + 4, voicePanel_.bottom() - 24, voicePanel_.w - 8, 17}, kDim, 8, Align::Centre);
    }
    else if (voicePanel_.h >= 210)
        paint::text(canvas, "4 ENV · 10 LFO · 64 MOD", {voicePanel_.x + 4, voicePanel_.bottom() - 22, voicePanel_.w - 8, 15}, kDim, 7.5f, Align::Centre);
}
void WavetableEditor::paintInspector(SkCanvas& canvas)
{
    if (modulationEditor_->visible || routingEditor_->visible) return;
    paint::fillRounded(canvas, body_, 3, kPanel);
    if (section_ == Section::Noise) paintNoisePreview(canvas, noisePlot_);
    paint::line(canvas, body_.x, controlsArea_.y - 2, body_.right(), controlsArea_.y - 2, kLine);
    if (pageCount() > 1)
        paint::text(canvas, "Page " + std::to_string(page_ + 1) + " / " + std::to_string(pageCount()),
                    {body_.right() - 115, body_.y + kTabs + 2, 90, kNavigation - 4}, kDim, 9, Align::Centre);
    if (controlsArea_.h < 30)
        paint::text(canvas, "Raise the Detail View divider to edit", controlsArea_, kDim, 9, Align::Centre);
    else if (section_ == Section::Matrix)
        paint::text(canvas, "SOURCE → DESTINATION   /   VIA SCALES THE SOURCE", {body_.x + 215, body_.y + kTabs + 2, body_.w - 370, 18}, kDim, 8);
}
namespace
{
bool blackKey(int note) { const int pitch = note % 12; return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10; }
}
Rect WavetableEditor::keyboardViewCell(bool arp) const
{
    if (inspector_) return {};
    return {footer_.x + 4, footer_.y + (arp ? 18.0f : 2.0f), 42, 14};
}
Rect WavetableEditor::pitchBendCell() const { return inspector_ ? Rect{} : Rect{footer_.x + 50, footer_.y + 3, 15, footer_.h - 6}; }
Rect WavetableEditor::modWheelCell() const { return inspector_ ? Rect{} : Rect{footer_.x + 69, footer_.y + 3, 15, footer_.h - 6}; }
Rect WavetableEditor::keyboardCell(int note) const
{
    if (inspector_ || note < kLowestKey || note > kHighestKey) return {};
    int whites = 0, total = 0;
    for (int n = kLowestKey; n <= kHighestKey; ++n) if (! blackKey(n)) { if (n < note) ++whites; ++total; }
    const float start = footer_.x + (arpView_ ? 460.0f : 88.0f);
    const float keyWidth = (footer_.right() - 3 - start) / (float) total, top = footer_.y + 2, height = footer_.h - 4;
    if (blackKey(note)) return {start + (float) whites * keyWidth - keyWidth * 0.32f, top, keyWidth * 0.64f, height * 0.6f};
    return {start + (float) whites * keyWidth, top, keyWidth - 1, height};
}
int WavetableEditor::noteAt(float x, float y) const
{
    for (int note = kLowestKey; note <= kHighestKey; ++note) if (blackKey(note) && keyboardCell(note).contains(x, y)) return note;
    for (int note = kLowestKey; note <= kHighestKey; ++note) if (keyboardCell(note).contains(x, y)) return note;
    return -1;
}
void WavetableEditor::paintKeyboard(SkCanvas& canvas)
{
    paint::fillRounded(canvas, footer_, 3, kPanel); paint::stroke(canvas, footer_, kLine);
    // Wheels: a slot with a lit thumb; pitch bend springs back to the centre.
    for (int wheel = 0; wheel < 2; ++wheel)
    {
        const auto cell = wheel == 0 ? pitchBendCell() : modWheelCell();
        paint::fillRounded(canvas, cell, 3, 0xff0b1118); paint::stroke(canvas, cell, kLine);
        const float value = wheel == 0 ? (processor_->pitchBendDisplay() + 1) * 0.5f : processor_->modWheelDisplay();
        const float y = cell.bottom() - 4 - value * (cell.h - 8);
        if (wheel == 1) paint::fill(canvas, {cell.x + 3, y, cell.w - 6, cell.bottom() - 3 - y}, paint::withAlpha(kValueArc, 120));
        else paint::line(canvas, cell.x + 3, cell.centreY(), cell.right() - 3, cell.centreY(), kLine);
        paint::fillRounded(canvas, {cell.x + 2, y - 2, cell.w - 4, 4}, 1, kText);
        paint::text(canvas, wheel == 0 ? "PB" : "MW", {cell.x - 2, cell.y + 1, cell.w + 4, 9}, kDim, 6.5f, Align::Centre, true);
    }
    const int arpNote = processor_->arpNoteDisplay();
    for (int pass = 0; pass < 2; ++pass)
        for (int note = kLowestKey; note <= kHighestKey; ++note)
        {
            const bool black = blackKey(note);
            if (black != (pass == 1)) continue;
            const auto key = keyboardCell(note);
            const bool lit = note == auditionNote_ || processor_->noteLit(note);
            const SkColor fill = note == arpNote ? kFrame : lit ? kCyan : black ? 0xff151d28 : 0xffd6e0ea;
            paint::fill(canvas, key, fill);
            paint::stroke(canvas, key, 0xff080d13);
            if (note % 12 == 0 && key.w >= 12)
                paint::text(canvas, "C" + std::to_string(note / 12 - 2), {key.x, key.bottom() - 10, key.w, 9}, 0xff3c4d61, 6.5f, Align::Centre);
        }
}
void WavetableEditor::paintOverChildren(SkCanvas& canvas, const Theme&)
{
    if (presetBrowser_->visible) return; // veils, ring tips and drag lines belong to the dashboard it covers
    // Turned-off oscillators recede like Serum's: everything but the power switch is dimmed.
    // Every module that is switched off recedes like Serum's (oscillators, Sub, Noise, filters): all but its power switch.
    const auto veil = [&](Rect r, bool on) {
        if (on || r.isEmpty()) return;
        const SkColor shade = paint::withAlpha(kBackground, 150);
        paint::fill(canvas, {r.x + 24, r.y, r.w - 24, 25}, shade);
        paint::fill(canvas, {r.x, r.y + 25, r.w, r.h - 25}, shade);
    };
    if (! inspector_ && workflow_ == Page::Osc)
    {
        for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
            veil(oscillatorPanels_[(size_t) oscillator], processor_->getParameter(W::oscParam(oscillator, W::On)) > 0.5f);
        veil(sub_, processor_->getParameter(W::SubOn) > 0.5f);
        veil(noise_, processor_->getParameter(W::NoiseOn) > 0.5f);
    }
    if (! inspector_ && (workflow_ == Page::Osc || workflow_ == Page::Mix))
        for (int filter = 0; filter < W::kFilters; ++filter)
            veil(filterPanels_[(size_t) filter], processor_->getParameter(W::filterParam(filter, W::FilterOn)) > 0.5f);
    if (ringTip_.parameter >= 0 && ringTip_.slot >= 0 && (size_t) ringTip_.parameter < controls_.size()
        && controls_[(size_t) ringTip_.parameter].widget->visible)
    {
        const auto [first, second] = ringReadout(ringTip_.parameter, ringTip_.slot);
        if (! first.empty())
        {
            const auto knob = controls_[(size_t) ringTip_.parameter].widget->bounds();
            const float w = 236, h = 40;
            Rect box{std::clamp(knob.centreX() - w * 0.5f, 4.0f, width() - w - 4), knob.bottom() + 4, w, h};
            if (box.bottom() > height() - 4) box.y = knob.y - h - 4;
            paint::fillRounded(canvas, box, 5, paint::withAlpha(0xff0b1118, 238));
            SkPaint frame; frame.setAntiAlias(true); frame.setStyle(SkPaint::kStroke_Style); frame.setStrokeWidth(1.4f); frame.setColor(kValueArc);
            canvas.drawRoundRect(SkRect::MakeXYWH(box.x, box.y, box.w, box.h), 5, 5, frame);
            paint::text(canvas, first, {box.x + 8, box.y + 4, box.w - 16, 15}, kText, 9.5f, Align::Centre, true);
            paint::text(canvas, second, {box.x + 8, box.y + 21, box.w - 16, 15}, kValue, 9.5f, Align::Centre);
        }
    }
    if (modDragSource_ < 0) return;
    for (const auto& control : controls_)
    {
        if (! control.widget->visible || devices::wavetable_routing::destinationForParameter(control.parameter) == W::DestOff) continue;
        if (dynamic_cast<PanelKnob*>(control.widget) == nullptr) continue;
        const bool hovered = control.parameter == modDragTarget_;
        const auto bounds = control.widget->bounds().reduced(1);
        if (hovered) paint::fillRounded(canvas, bounds, 4, paint::withAlpha(sourceColor(modDragSource_), 50));
        SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(hovered ? 2.0f : 1.0f);
        pen.setColor(paint::withAlpha(sourceColor(modDragSource_), hovered ? 255 : 120));
        canvas.drawRoundRect(SkRect::MakeXYWH(bounds.x, bounds.y, bounds.w, bounds.h), 4, 4, pen);
    }
    const auto from = modulationSourceCell(modDragSource_);
    SkPaint pen; pen.setAntiAlias(true); pen.setStyle(SkPaint::kStroke_Style); pen.setStrokeWidth(1.6f); pen.setColor(sourceColor(modDragSource_));
    const SkScalar dashes[]{5, 3};
    pen.setPathEffect(SkDashPathEffect::Make(dashes, 0));
    canvas.drawLine(from.centreX(), from.centreY(), modDragX_, modDragY_, pen);
    pen.setPathEffect(nullptr); pen.setStyle(SkPaint::kFill_Style);
    canvas.drawCircle(modDragX_, modDragY_, 3.5f, pen);
    paint::text(canvas, W::sourceName(modDragSource_), {modDragX_ + 8, modDragY_ - 16, 80, 12}, sourceColor(modDragSource_), 9, Align::Left, true);
}
Rect WavetableEditor::modulationSourceCell(int source) const
{
    if (source < 0 || (size_t) source >= modHandles_.size() || modHandles_[(size_t) source] == nullptr || ! modHandles_[(size_t) source]->visible) return {};
    return modHandles_[(size_t) source]->bounds();
}
Rect WavetableEditor::modulationRingCell(int parameter, int ring) const
{
    if (parameter < 0 || (size_t) parameter >= controls_.size()) return {};
    const auto* knob = dynamic_cast<const PanelKnob*>(controls_[(size_t) parameter].widget);
    if (knob == nullptr || ! knob->visible || knob->compact) return {};
    const auto [x, y] = knob->ringPoint(ring);
    return {knob->bounds().x + x - 1, knob->bounds().y + y - 1, 2, 2};
}
int WavetableEditor::modulationTargetAt(float x, float y) const
{
    for (const auto& control : controls_)
        if (control.widget->visible && control.widget->bounds().contains(x, y) && dynamic_cast<PanelKnob*>(control.widget) != nullptr
            && devices::wavetable_routing::destinationForParameter(control.parameter) != W::DestOff)
            return control.parameter;
    if (master_->visible && master_->bounds().contains(x, y)) return W::Volume;
    return -1;
}
void WavetableEditor::beginModulationDrag(int source, float x, float y)
{
    if (! devices::wavetable_routing::isModulationSource(source)) return;
    releaseNote(); modDragSource_ = source; modDragX_ = x; modDragY_ = y; modDragTarget_ = -1; repaint();
}
void WavetableEditor::moveModulationDrag(float x, float y)
{
    if (modDragSource_ < 0) return;
    modDragX_ = x; modDragY_ = y; modDragTarget_ = modulationTargetAt(x, y); repaint();
}
void WavetableEditor::endModulationDrag(float x, float y)
{
    const int source = modDragSource_;
    modDragSource_ = modDragTarget_ = -1; repaint();
    if (source < 0) return;
    const int parameter = modulationTargetAt(x, y);
    if (parameter < 0) return;
    namespace R = devices::wavetable_routing;
    int slot = -1;
    const int existing = R::findSlot(*processor_, source, R::destinationForParameter(parameter));
    const float amount = existing >= 0 ? processor_->getParameter(W::modParam(existing, W::ModAmount)) : R::defaultAmount(*processor_, parameter);
    const auto writes = R::connect(*processor_, source, parameter, std::abs(amount) > 1e-6f ? amount : R::defaultAmount(*processor_, parameter), slot);
    if (slot < 0) { ctx().statusText = "All " + std::to_string(W::kModSlots) + " modulation slots are in use"; return; }
    R::apply(*processor_, writes);
    ctx().statusText = std::string(W::sourceName(source)) + " → " + W::destinationName(R::destinationForParameter(parameter))
        + " (matrix slot " + std::to_string(slot + 1) + ")";
    if (ctx().requestRepaint) ctx().requestRepaint();
}
float WavetableEditor::liveAmount(int destination) const
{
    if (! live_.active) return 0;
    float sum = 0;
    for (const auto& route : devices::wavetable_routing::routesTo(*processor_, destination)) sum += route.amount * live_.route[(size_t) route.slot];
    return sum;
}
void WavetableEditor::paint(SkCanvas& canvas, const Theme&)
{
    live_ = processor_->liveState();
    paint::fill(canvas, localBounds(), kBackground);
    paint::fill(canvas, {0, 0, width(), kHeader}, 0xff1c2530);
    paint::line(canvas, 0, kHeader - 1, width(), kHeader - 1, kLine);
    paint::text(canvas, "SONORA", {12, 5, 122, 25}, kText, 19, Align::Left, true);
    paint::text(canvas, "P R I S M", {13, 29, 122, 14}, kDim, 8.5f);
    if (inspector_) paintInspector(canvas);
    else { paintModules(canvas); paintModulation(canvas); paintKeyboard(canvas); }
    // Output meter beside MAIN: block peaks with a falling hold, -48..0 dB.
    for (size_t channel = 0; channel < 2; ++channel)
    {
        meter_[channel] = std::max(live_.outputPeak[channel], meter_[channel] * 0.82f);
        const Rect bar{width() - 13 + (float) channel * 5, 6, 3, kHeader - 14};
        paint::fill(canvas, bar, 0xff0b1118);
        const float db = 20 * std::log10(std::max(meter_[channel], 1e-5f));
        const float fill = std::clamp((db + 48) / 48, 0.0f, 1.0f) * bar.h;
        paint::fill(canvas, {bar.x, bar.bottom() - fill, bar.w, fill}, db > -1 ? 0xffff6b5b : kValueArc);
    }
}
void WavetableEditor::releaseNote()
{
    if (auditionNote_ >= 0) processor_->queueUiMidi(0x80, (uint8_t) auditionNote_, 0);
    auditionNote_ = -1;
}
void WavetableEditor::playNote(int note, float y)
{
    // Lower on the key plays louder, like a real key's travel.
    const auto key = keyboardCell(note);
    const float depth = key.h > 0 ? std::clamp((y - key.y) / key.h, 0.0f, 1.0f) : 0.8f;
    const auto velocity = (uint8_t) std::clamp((int) std::lround(30 + depth * 97), 1, 127);
    releaseNote(); auditionNote_ = note;
    processor_->queueUiMidi(0x90, (uint8_t) note, velocity); repaint();
}
void WavetableEditor::sendWheel(bool bend, float y)
{
    const auto cell = bend ? pitchBendCell() : modWheelCell();
    const float value = std::clamp((cell.bottom() - 4 - y) / std::max(1.0f, cell.h - 8), 0.0f, 1.0f);
    if (bend)
    {
        const int position = std::clamp((int) std::lround(value * 16383), 0, 16383);
        processor_->queueUiMidi(0xe0, (uint8_t) (position & 127), (uint8_t) (position >> 7));
    }
    else processor_->queueUiMidi(0xb0, 1, (uint8_t) std::lround(value * 127));
    repaint();
}
void WavetableEditor::mouseDown(const MouseEvent& event)
{
    if (inspector_) return;
    if (workflow_ == Page::Fx && ! event.right)
    {
        for (const auto& row : fxRows)
        {
            if (row.hide.contains(event.x, event.y)) { toggleFxCollapsed(row.id); return; }
            if (row.remove.contains(event.x, event.y)) { deleteFxModule(row.id); return; }
        }
        for (const auto& panel : fxPanels_)
        {
            if (! panel.bounds.contains(event.x, event.y)) continue;
            if (event.x < panel.bounds.x + 22) { toggleFxCollapsed(panel.id); return; } // the gutter hides / shows the panel
            if (panel.more.reduced(-3).contains(event.x, event.y))
            {
                // ••• opens every control of this module in the CONTROLS inspector.
                selectFxModule(panel.id);
                const auto at = std::find(fxInspectorIds_.begin(), fxInspectorIds_.end(), panel.id);
                if (at != fxInspectorIds_.end()) { instances_[(size_t) Section::Effects] = (int) (at - fxInspectorIds_.begin()) + 1; selectSection(Section::Effects); }
                return;
            }
            if (panel.id != fxSelected_) { selectFxModule(panel.id); return; }
        }
    }
    if (envelopePlot().reduced(-6).contains(event.x, event.y) && ! event.right)
    {
        // Nearest handle within reach: levels/times first, then the curve handles.
        int best = -1; float distance = 12;
        for (int point = 0; point < 7; ++point)
        {
            const auto cell = envelopePoint(point); if (cell.isEmpty()) continue;
            const float d = std::hypot(event.x - cell.centreX(), event.y - cell.centreY()) + (point >= 4 ? 2.0f : 0.0f);
            if (d < distance) { distance = d; best = point; }
        }
        if (best < 0) return;
        const auto g = envelopeShape();
        envelopeFrozenTotal_ = g.total; envelopeFrozenSustaining_ = g.sustaining;
        envelopeDrag_ = best; envelopeDragX_ = event.x; envelopeDragY_ = event.y;
        const int envelope = instances_[(size_t) Section::Envelopes];
        const W::EnvParam timed[]{W::Attack, W::Hold, W::Decay, W::Release, W::AttackCurve, W::DecayCurve, W::ReleaseCurve};
        envelopeDragValue_ = processor_->getParameter(W::envParam(envelope, timed[best]));
        if (best >= 4)
        {
            // Pick the tension direction that raises the handle when dragged up, whatever the curve law.
            const auto mid = [&](float tension) {
                return g.imported ? (float) dsp::ExponentialEnvelopeCurve::fromTension(tension).eval(0.5f) : dsp::CurveGraph::shape(0.5f, tension);
            };
            const float slope = mid(std::clamp(envelopeDragValue_ + 0.05f, -1.0f, 1.0f)) - mid(std::clamp(envelopeDragValue_ - 0.05f, -1.0f, 1.0f));
            const bool rising = best == 4; // attack rises with the shape value; decay and release fall
            envelopeCurveSign_ = (slope >= 0) == rising ? 1.0f : -1.0f;
        }
        repaint(); return;
    }
    if (curveSourcePlot().reduced(-6).contains(event.x, event.y) && ! event.right)
    {
        float best = 1e9f; curveDrag_ = -1;
        for (int point = 0; point < 3; ++point)
        {
            const auto cell = curveSourcePoint(point);
            const float distance = std::hypot(event.x - cell.centreX(), event.y - cell.centreY());
            if (distance < best) { best = distance; curveDrag_ = point; }
        }
        mouseDrag(event); return;
    }
    for (int oscillator = 0; oscillator < 3; ++oscillator)
        if (oscillatorPlot(oscillator).contains(event.x, event.y))
        {
            instances_[(size_t) Section::Oscillators] = oscillator; section_ = Section::Oscillators;
            dragOscillator_ = oscillator; repaint(); return;
        }
    if (lfoPlot().reduced(-4).contains(event.x, event.y)
        && (int) std::lround(processor_->getParameter(W::lfoTypeParam(instances_[(size_t) Section::Lfos]))) == W::LfoNormal)
    {
        lfoDraft_ = processor_->lfoCurve(instances_[(size_t) Section::Lfos]);
        dragLfoPoint_ = -1; dragLfoCurve_ = false;
        const auto plot = lfoPlot(); float distance = 64;
        for (int i = 0; i < lfoDraft_.count; ++i)
        {
            const float x = plot.x + lfoDraft_.x[i] * plot.w, y = plot.bottom() - lfoDraft_.y[i] * plot.h;
            const float d = (event.x - x) * (event.x - x) + (event.y - y) * (event.y - y);
            if (d < distance) { distance = d; dragLfoPoint_ = i; }
        }
        if (event.right && dragLfoPoint_ > 0 && dragLfoPoint_ + 1 < lfoDraft_.count)
        {
            for (int i = dragLfoPoint_; i + 1 < lfoDraft_.count; ++i)
            { lfoDraft_.x[i] = lfoDraft_.x[i + 1]; lfoDraft_.y[i] = lfoDraft_.y[i + 1]; lfoDraft_.curve[i] = lfoDraft_.curve[i + 1]; }
            --lfoDraft_.count; mouseUp(event); return;
        }
        if (event.right) { dragLfoPoint_ = -1; return; }
        if (dragLfoPoint_ < 0)
        {
            dsp::ExponentialCurveGraph exponential;
            const bool imported = processor_->usesImportedLfoProfile(); if (imported) exponential.prepare(lfoDraft_);
            for (int i = 0; i + 1 < lfoDraft_.count; ++i)
            {
                const float middle = (lfoDraft_.x[i] + lfoDraft_.x[i + 1]) * 0.5f;
                const float x = plot.x + middle * plot.w, y = plot.bottom() - (imported ? exponential.eval(middle) : lfoDraft_.eval(middle)) * plot.h;
                if (lfoDraft_.x[i + 1] > lfoDraft_.x[i] + 1e-4f && std::abs(lfoDraft_.y[i + 1] - lfoDraft_.y[i]) > 1e-4f
                    && (event.x - x) * (event.x - x) + (event.y - y) * (event.y - y) < 64)
                { dragLfoPoint_ = i; dragLfoCurve_ = true; break; }
            }
        }
        return;
    }
    if (! event.right && (pitchBendCell().contains(event.x, event.y) || modWheelCell().contains(event.x, event.y)))
    {
        wheelDrag_ = pitchBendCell().contains(event.x, event.y) ? 0 : 1; sendWheel(wheelDrag_ == 0, event.y); return;
    }
    if (const int note = noteAt(event.x, event.y); note >= 0 && ! event.right) playNote(note, event.y);
}
void WavetableEditor::mouseDoubleClick(const MouseEvent& event)
{
    if (inspector_) return;
    if (curveSourcePlot().reduced(-6).contains(event.x, event.y))
    {
        const int curve = voiceTab_ - 1; curveDrag_ = -1;
        for (int p = 0; p < W::CurveSourceParams; ++p)
        {
            const int parameter = W::curveSourceParam(curve, (W::CurveSourceParam) p);
            processor_->setParameter(parameter, processor_->parameters()[(size_t) parameter].defaultValue);
        }
        repaint(); return;
    }
    for (int oscillator = 0; oscillator < 3; ++oscillator)
        if (oscillatorPlot(oscillator).contains(event.x, event.y))
        {
            const auto section = processor_->getParameter(W::mediaParam(oscillator, W::MediaMode)) > 0.5f ? Section::Media : Section::Oscillators;
            instances_[(size_t) section] = oscillator; selectSection(section); return;
        }
    if (lfoPlot().reduced(-4).contains(event.x, event.y)
        && (int) std::lround(processor_->getParameter(W::lfoTypeParam(instances_[(size_t) Section::Lfos]))) == W::LfoNormal)
    {
        lfoDraft_ = processor_->lfoCurve(instances_[(size_t) Section::Lfos]);
        const auto plot = lfoPlot(); const float phase = std::clamp((event.x - plot.x) / plot.w, 0.0f, 1.0f), level = std::clamp((plot.bottom() - event.y) / plot.h, 0.0f, 1.0f);
        for (int i = 0; i < lfoDraft_.count; ++i)
        {
            const float x = plot.x + lfoDraft_.x[i] * plot.w, y = plot.bottom() - lfoDraft_.y[i] * plot.h;
            if ((event.x - x) * (event.x - x) + (event.y - y) * (event.y - y) > 36) continue;
            if (i == 0 || i + 1 == lfoDraft_.count) return;
            for (int point = i; point + 1 < lfoDraft_.count; ++point)
            { lfoDraft_.x[point] = lfoDraft_.x[point + 1]; lfoDraft_.y[point] = lfoDraft_.y[point + 1]; lfoDraft_.curve[point] = lfoDraft_.curve[point + 1]; }
            --lfoDraft_.count; dragLfoPoint_ = i; mouseUp(event); return;
        }
        if (lfoDraft_.count >= dsp::CurveGraph::kMaxPoints) return;
        int at = 1; while (at < lfoDraft_.count && lfoDraft_.x[at] <= phase) ++at;
        for (int i = lfoDraft_.count; i > at; --i) { lfoDraft_.x[i] = lfoDraft_.x[i - 1]; lfoDraft_.y[i] = lfoDraft_.y[i - 1]; lfoDraft_.curve[i] = lfoDraft_.curve[i - 1]; }
        lfoDraft_.x[at] = phase; lfoDraft_.y[at] = level; lfoDraft_.curve[at] = 0; ++lfoDraft_.count; dragLfoPoint_ = at;
        mouseUp(event); return;
    }
    mouseDown(event);
}
void WavetableEditor::mouseDrag(const MouseEvent& event)
{
    if (envelopeDrag_ >= 0)
    {
        const auto plot = envelopePlot(); const int envelope = instances_[(size_t) Section::Envelopes];
        if (plot.isEmpty()) { envelopeDrag_ = -1; return; }
        const W::EnvParam timed[]{W::Attack, W::Hold, W::Decay, W::Release, W::AttackCurve, W::DecayCurve, W::ReleaseCurve};
        const int parameter = W::envParam(envelope, timed[envelopeDrag_]);
        if (envelopeDrag_ < 4)
        {
            const float seconds = envelopeDragValue_ + (event.x - envelopeDragX_) * envelopeFrozenTotal_ / plot.w;
            processor_->setParameter(parameter, std::clamp(seconds, 0.0f, 32.0f));
        }
        else processor_->setParameter(parameter, std::clamp(envelopeDragValue_ + envelopeCurveSign_ * (envelopeDragY_ - event.y) / 60, -1.0f, 1.0f));
        if (envelopeDrag_ == 2)
        {
            // Sustain from height: both laws draw the level squared.
            const float shown = std::clamp((plot.bottom() - 2 - event.y) / (plot.h - 7), 0.0f, 1.0f);
            processor_->setParameter(W::envParam(envelope, W::Sustain), std::sqrt(shown));
        }
        repaint(); return;
    }
    if (curveDrag_ >= 0)
    {
        const auto plot = curveSourcePlot(); const int curve = voiceTab_ - 1;
        if (plot.isEmpty()) { curveDrag_ = -1; return; }
        const float x = std::clamp((event.x - plot.x) / plot.w, 0.02f, 0.98f), y = std::clamp((plot.bottom() - event.y) / plot.h, 0.0f, 1.0f);
        if (curveDrag_ == 1) processor_->setParameter(W::curveSourceParam(curve, W::CurveMidX), x);
        processor_->setParameter(W::curveSourceParam(curve, curveDrag_ == 0 ? W::CurveStart : curveDrag_ == 2 ? W::CurveEnd : W::CurveMidY), y);
        repaint(); return;
    }
    if (dragOscillator_ >= 0)
    {
        const auto plot = oscillatorPlot(dragOscillator_);
        const bool media = processor_->getParameter(W::mediaParam(dragOscillator_, W::MediaMode)) > 0.5f;
        processor_->setParameter(media ? W::mediaParam(dragOscillator_, W::MediaStartPosition) : W::oscParam(dragOscillator_, W::Position),
                                 std::clamp((event.x - plot.x) / plot.w, 0.0f, 1.0f)); repaint();
    }
    if (dragLfoPoint_ >= 0)
    {
        const auto plot = lfoPlot(); const int point = dragLfoPoint_;
        const float level = std::clamp((plot.bottom() - event.y) / plot.h, 0.0f, 1.0f);
        if (dragLfoCurve_ && point + 1 < lfoDraft_.count)
        {
            const float fraction = std::clamp((level - lfoDraft_.y[point]) / (lfoDraft_.y[point + 1] - lfoDraft_.y[point]), 0.02f, 0.98f);
            const float tension = processor_->usesImportedLfoProfile()
                ? 1 - 2 * (fraction < 0.5f ? std::cbrt(fraction / 4) : 1 - std::cbrt((1 - fraction) / 4))
                : fraction < 0.5f ? std::clamp((std::log(fraction) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f)
                                  : -std::clamp((std::log(1 - fraction) / std::log(0.5f) - 1) / 3, 0.0f, 1.0f);
            lfoDraft_.curve[point] = tension;
        }
        else
        {
            lfoDraft_.y[point] = event.alt ? std::round(level * 8) / 8 : level;
            if (point > 0 && point + 1 < lfoDraft_.count)
            {
                float phase = (event.x - plot.x) / plot.w;
                if (event.alt) phase = std::round(phase * 16) / 16;
                lfoDraft_.x[point] = std::clamp(phase, lfoDraft_.x[point - 1], lfoDraft_.x[point + 1]);
            }
        }
        repaint();
    }
    if (wheelDrag_ >= 0) sendWheel(wheelDrag_ == 0, event.y);
    if (auditionNote_ >= 0)
    {
        const int note = noteAt(event.x, event.y);
        if (note >= 0 && note != auditionNote_) playNote(note, event.y);
    }
}
void WavetableEditor::mouseUp(const MouseEvent&)
{
    curveDrag_ = -1;
    if (envelopeDrag_ >= 0) { envelopeDrag_ = -1; repaint(); }
    if (wheelDrag_ == 0) processor_->queueUiMidi(0xe0, 0, 64); // the bend springs back to the centre
    wheelDrag_ = -1;
    dragOscillator_ = -1; releaseNote();
    if (dragLfoPoint_ >= 0)
    {
        dragLfoPoint_ = -1; dragLfoCurve_ = false; const auto curve = lfoDraft_; const int lfo = instances_[(size_t) Section::Lfos];
        const auto local = onEditLfoCurve; const auto action = ctx().editWavetableLfo; const int track = track_, device = device_, chain = chain_, child = child_;
        repaint();
        if (local) local(lfo, curve); else if (action) action(track, device, lfo, curve, chain, child);
    }
}
void WavetableEditor::focusLost()
{
    if (wheelDrag_ == 0) processor_->queueUiMidi(0xe0, 0, 64);
    wheelDrag_ = -1; dragOscillator_ = -1; dragLfoPoint_ = -1; releaseNote();
}
bool WavetableEditor::browserDrop(const BrowserDrag& payload, const MouseEvent& event)
{
    if (payload.kind != BrowserDrag::Kind::Sample) return false;
    if (noisePlot().contains(event.x, event.y) || noiseAssetCell().contains(event.x, event.y)) return importNoiseFile(payload.value);
    if ((! inspector_ && workflow_ == Page::Fx) || (inspector_ && section_ == Section::Effects))
    {
        const FxNode* target = selectedFxNode();
        for (const auto& node : fxNodes_) if (fxModuleCell(node.id).contains(event.x, event.y)) target = &node;
        if (target && target->type == Effects::Type::Convolution
            && ((inspector_ ? body_.contains(event.x, event.y) : fxDetail_.contains(event.x, event.y)) || fxModuleCell(target->id).contains(event.x, event.y)))
        {
            const auto action = ctx().importWavetableImpulseResponse; if (! action) return false;
            fxSelected_ = target->id; storeFxView();
            const auto id = target->id; const int track = track_, device = device_, chain = chain_, child = child_;
            action(track, device, id, payload.value, chain, child); return true;
        }
    }
    for (int oscillator = 0; oscillator < 3; ++oscillator)
        if (oscillatorPlot(oscillator).contains(event.x, event.y))
        {
            const bool media = processor_->getParameter(W::mediaParam(oscillator, W::MediaMode)) > 0.5f;
            const auto action = media ? ctx().importOscillatorMedia : ctx().importWavetable;
            if (! action) return false;
            action(track_, device_, oscillator, payload.value, chain_, child_); return true;
        }
    return false;
}
void WavetableEditor::loadLibraryTable(int oscillator, const std::string& path)
{
    const auto action = ctx().importWavetable; if (! action) { ctx().statusText = "Wavetable import is unavailable"; return; }
    const int track = track_, device = device_, chain = chain_, child = child_;
    action(track, device, oscillator, path, chain, child); // may rebuild this editor
}
void WavetableEditor::loadLibraryPreset(const std::string& path)
{
    const auto action = ctx().importWavetablePreset; if (! action) { ctx().statusText = "Preset import is unavailable"; return; }
    if (const int key = fxViewKey(); key >= 0) libraryPresets()[key] = path;
    const int track = track_, device = device_, chain = chain_, child = child_;
    action(track, device, path, chain, child); // may rebuild this editor
}
bool WavetableEditor::stepLibraryTable(int oscillator, int direction)
{
    if ((int) std::lround(processor_->getParameter(W::oscParam(oscillator, W::Table))) != W::Custom || processor_->customTable(oscillator) == nullptr) return false;
    const auto items = plugins::serumLibrary("Tables");
    const int next = plugins::serumLibraryStep(items, processor_->customTableName(oscillator), direction);
    if (next < 0) return false;
    loadLibraryTable(oscillator, items[(size_t) next].path);
    return true;
}
bool WavetableEditor::stepLibraryPreset(int direction)
{
    const auto current = libraryPresetPath();
    if (current.empty()) return false;
    const auto& items = rescanPresets();
    const int next = plugins::serumLibraryStepInBank(items, current, direction);
    if (next < 0) return false;
    if (presetBrowserOpen())
    {
        auto state = presetBrowser_->state(); state.selected = next;
        presetBrowser_->setLibrary(presetLibrary(), plugins::serumLibraryBankFolders("Presets")); presetBrowser_->restore(state);
        storePresetBrowser();
    }
    loadLibraryPreset(items[(size_t) next].path); // may rebuild this editor
    return true;
}
bool WavetableEditor::stepLibraryGroup(int direction)
{
    const auto current = libraryPresetPath();
    if (current.empty()) return false;
    const auto& items = rescanPresets();
    const int next = plugins::serumLibraryStepGroup(items, current, direction);
    if (next < 0) return false;
    if (presetBrowserOpen())
    {
        auto state = presetBrowser_->state(); state.selected = next;
        presetBrowser_->setLibrary(presetLibrary(), plugins::serumLibraryBankFolders("Presets")); presetBrowser_->restore(state);
        storePresetBrowser();
    }
    loadLibraryPreset(items[(size_t) next].path); // may rebuild this editor
    return true;
}
std::string WavetableEditor::libraryPresetPath() const
{
    const int key = fxViewKey();
    if (const auto found = libraryPresets().find(key); key >= 0 && found != libraryPresets().end()) return found->second;
    // A reopened set or another view: the imported device carries the preset's name.
    const auto* track = ctx().trackAt(track_);
    if (! track || device_ < 0 || (size_t) device_ >= track->devices.size()) return {};
    const model::Device* device = &track->devices[(size_t) device_];
    if (chain_ >= 0 && (size_t) chain_ < device->chains.size() && child_ >= 0 && (size_t) child_ < device->chains[(size_t) chain_].devices.size())
        device = &device->chains[(size_t) chain_].devices[(size_t) child_];
    if (device->displayName.empty() || device->displayName.rfind("Sonora Prism", 0) == 0 || device->displayName.rfind("Sonora Wavetable", 0) == 0) return {};
    const auto& items = cachedPresets();
    const int index = plugins::serumLibraryFind(items, device->displayName);
    return index >= 0 ? items[(size_t) index].path : std::string();
}
bool WavetableEditor::libraryGroupsAvailable() const
{
    const auto current = libraryPresetPath();
    if (current.empty()) return false;
    const auto& items = cachedPresets();
    const int index = plugins::serumLibraryFind(items, current);
    return index >= 0 && plugins::serumLibraryGroups(items, plugins::serumLibraryBank(items[(size_t) index])).size() >= 2;
}
bool WavetableEditor::presetBrowserOpen() const { return presetBrowser_ != nullptr && presetBrowser_->state().open; }
void WavetableEditor::storePresetBrowser()
{
    if (const int key = fxViewKey(); key >= 0) presetBrowserViews()[key] = presetBrowser_->state();
}
void WavetableEditor::openPresetBrowser()
{
    releaseNote();
    rescanPresets();
    presetBrowser_->setLibrary(presetLibrary(), plugins::serumLibraryBankFolders("Presets"));
    presetBrowser_->setCurrent(libraryPresetPath());
    presetBrowser_->restore(presetBrowser_->initialState());
    presetBrowserRestored_ = true;
    storePresetBrowser();
    layout(); repaint();
}
void WavetableEditor::closePresetBrowser()
{
    auto state = presetBrowser_->state(); state.open = state.focused = false;
    presetBrowser_->restore(state);
    storePresetBrowser();
    layout(); repaint();
}
void WavetableEditor::layoutPresetBrowser()
{
    if (! presetBrowserRestored_)
        if (const int key = fxViewKey(); key >= 0)
        {
            presetBrowserRestored_ = true;
            const auto found = presetBrowserViews().find(key);
            if (found != presetBrowserViews().end() && found->second.open)
            {
                if (! presetLibrary()) rescanPresets();
                presetBrowser_->setLibrary(presetLibrary(), plugins::serumLibraryBankFolders("Presets"));
                presetBrowser_->restore(found->second);
            }
        }
    presetBrowser_->visible = presetBrowser_->state().open;
    if (! presetBrowser_->visible) return;
    presetBrowser_->setCurrent(libraryPresetPath());
    auto area = body_; // the keyboard footer stays playable below the dashboard view
    if (! inspector_ && ! footer_.isEmpty()) area.h = std::max(0.0f, footer_.y - 6 - area.y);
    presetBrowser_->setBounds(area); presetBrowser_->layout();
    for (const auto& child : children_)
        if (child.get() != presetBrowser_ && overlaps(child->bounds(), area)) child->visible = false;
    if (presetBrowser_->state().focused)
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent())
            if (auto* root = dynamic_cast<RootWidget*>(ancestor)) root->setKeyboardFocus(presetBrowser_);
}
std::pair<std::string, std::string> WavetableEditor::ringReadout(int parameter, int slot) const
{
    if (parameter < 0 || (size_t) parameter >= controls_.size() || slot < 0) return {};
    const auto* knob = dynamic_cast<const PanelKnob*>(controls_[(size_t) parameter].widget);
    if (knob == nullptr) return {};
    namespace R = devices::wavetable_routing;
    for (const auto& route : R::routesForParameter(*processor_, parameter))
    {
        if (route.slot != slot) continue;
        const auto sweep = R::span(*processor_, parameter, route, knob->currentNorm());
        const auto direction = R::direction(*processor_, slot);
        const char* mode = direction == R::Direction::Both ? "  ↔" : direction == R::Direction::Backward ? "  ↓" : "  ↑";
        return {std::string(W::sourceName(route.source)) + "→" + W::destinationName(route.destination) + ":  " + number("%.0f%%", route.amount * 100) + mode,
                "Range:  " + knob->text(knob->plainAt(sweep.from)) + "  →  " + knob->text(knob->plainAt(sweep.to))};
    }
    return {};
}
Rect WavetableEditor::voiceTabCell(int tab) const
{
    if (inspector_ || tab < 0 || tab > 2 || voicePanel_.isEmpty()) return {};
    const float first = 46, rest = (voicePanel_.w - first - 6) * 0.5f;
    if (tab == 0) return {voicePanel_.x + 2, voicePanel_.y + 2, first, 21};
    return {voicePanel_.x + 4 + first + (float) (tab - 1) * rest, voicePanel_.y + 2, rest - 1, 21};
}
Rect WavetableEditor::curveSourcePlot() const
{
    if (inspector_ || voiceTab_ == 0 || voicePanel_.isEmpty()) return {};
    return {voicePanel_.x + 10, voicePanel_.y + 34, voicePanel_.w - 20, voicePanel_.h - 62};
}
Rect WavetableEditor::curveSourcePoint(int point) const
{
    const auto plot = curveSourcePlot(); if (plot.isEmpty() || point < 0 || point > 2) return {};
    const int curve = voiceTab_ - 1;
    const float x = point == 0 ? 0 : point == 2 ? 1 : processor_->getParameter(W::curveSourceParam(curve, W::CurveMidX));
    const float y = processor_->getParameter(W::curveSourceParam(curve, point == 0 ? W::CurveStart : point == 2 ? W::CurveEnd : W::CurveMidY));
    return {plot.x + x * plot.w - 3, plot.bottom() - y * plot.h - 3, 6, 6};
}
void WavetableEditor::paintCurveSource(SkCanvas& canvas)
{
    const auto plot = curveSourcePlot(); if (plot.isEmpty()) return;
    const int curve = voiceTab_ - 1;
    paint::fill(canvas, plot, kPlot);
    for (int i = 1; i < 4; ++i)
    {
        paint::line(canvas, plot.x + plot.w * (float) i / 4, plot.y, plot.x + plot.w * (float) i / 4, plot.bottom(), paint::withAlpha(kLine, 110));
        paint::line(canvas, plot.x, plot.y + plot.h * (float) i / 4, plot.right(), plot.y + plot.h * (float) i / 4, paint::withAlpha(kLine, 110));
    }
    SkPathBuilder line, area; area.moveTo(plot.x, plot.bottom());
    for (int i = 0; i <= 64; ++i)
    {
        const float input = (float) i / 64, x = plot.x + input * plot.w, y = plot.bottom() - processor_->curveSourceAt(curve, input) * plot.h;
        if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
        area.lineTo(x, y);
    }
    area.lineTo(plot.right(), plot.bottom()); area.close();
    SkPaint pen; pen.setAntiAlias(true); pen.setColor(paint::withAlpha(kBlue, 40)); canvas.drawPath(area.detach(), pen);
    pen.setColor(kValueArc); pen.setStrokeWidth(1.8f); pen.setStyle(SkPaint::kStroke_Style); canvas.drawPath(line.detach(), pen);
    for (int point = 0; point < 3; ++point)
    {
        const auto cell = curveSourcePoint(point);
        pen.setStyle(point == 1 ? SkPaint::kStroke_Style : SkPaint::kFill_Style); pen.setStrokeWidth(1.4f); pen.setColor(point == curveDrag_ ? kText : kValueArc);
        canvas.drawCircle(cell.centreX(), cell.centreY(), point == 1 ? 3.5f : 2.5f, pen);
    }
    if (live_.active)
    {
        // The sounding note's input and output (Serum's green marker).
        const float input = curve == 0 ? live_.velocity : (float) live_.note / 127;
        const float x = plot.x + input * plot.w, y = plot.bottom() - processor_->curveSourceAt(curve, input) * plot.h;
        paint::line(canvas, x, plot.y, x, plot.bottom(), paint::withAlpha(0xff6fdc8c, 120));
        pen.setStyle(SkPaint::kFill_Style); pen.setColor(0xff6fdc8c); canvas.drawCircle(x, y, 3.5f, pen);
    }
    paint::text(canvas, curve == 0 ? "VELOCITY → OUTPUT" : "NOTE → OUTPUT", {plot.x, plot.bottom() + 6, plot.w, 12}, kDim, 7.5f, Align::Centre, true);
}
void WavetableEditor::tick()
{
    for (int oscillator = 0; oscillator < W::kOscillators; ++oscillator)
        if (mediaModes_[(size_t) oscillator] != (int) std::lround(processor_->getParameter(W::mediaParam(oscillator, W::MediaMode))))
        { layout(); break; }
    if (! presetEdited_)
        for (size_t i = 0; i < presetValues_.size(); ++i)
            if (std::abs(presetValues_[i] - processor_->getParameter((int) i)) > 1e-6f)
            { presetEdited_ = true; presetButton_->setLabel(presetLabel_ + " *"); break; }
    repaint();
}
} // namespace sonora::ui
