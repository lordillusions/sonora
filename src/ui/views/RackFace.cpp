#include "ui/views/RackFace.h"

#include "core/FaderCurve.h"
#include "core/LiveColors.h"
#include "devices/RackProcessor.h"
#include "devices/native/Delay.h"
#include "devices/native/Reverb.h"
#include "gfx/Fonts.h"
#include "ui/MeterBallistics.h"
#include "ui/UiContext.h"
#include "ui/views/DeviceFace.h"
#include "ui/views/EffectPanels.h"
#include "ui/views/NativeEditors.h"
#include "ui/widgets/Fader.h"

#include "include/core/SkPaint.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "include/core/SkPathBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace sonora::ui
{
namespace
{
// Capture geometry (UI-66): the device's left edge is x 6, its body starts at y 46 and is 342 px tall with the bottom
// frame (the 4-px title-coloured frame around the body is part of the face).
constexpr float kOriginX = 6, kOriginY = 46, kCaptureBody = 342;
constexpr float kStripX = 26.5f, kStripD = 26.0f;
constexpr float kStripY[6] = {66.5f, 101.0f, 131.0f, 164.5f, 198.5f, 232.5f};
constexpr float kMacroX = 48, kMacroY = 54, kMacroW = 100, kMacroH = 160, kMacroPitchX = 104, kMacroPitchY = 164;
constexpr float kHeaderH = 56; // name header under the box's 2-px border
constexpr float kListW = 488, kZonesW = 340, kDrumW = 1040, kRowTop = 108, kRowH = 36, kRowPitch = 38;
constexpr float kListTop = 106, kListBottom = 378, kTabTop = 66, kTabBottom = 92;
constexpr float kStartDeg = 135.0f, kSweepDeg = 270.0f; // the device knob's travel

std::string printed(double value, const char* format)
{
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

float gainDb(float gain) { return 20.0f * std::log10(std::max(gain, 1.0e-9f)); }

std::string volumeText(float gain)
{
    return gain <= 0.000317f ? std::string("-inf dB") : printed(gainDb(gain), "%.1f dB");
}

std::string panText(float pan)
{
    const int amount = (int) std::lround(std::abs(pan) * 50.0f);
    return amount == 0 ? std::string("C") : std::to_string(amount) + (pan < 0 ? "L" : "R");
}

// Word wrap into at most two lines that fit `width` (the reference's macro headers: "Filter / Cutoff").
std::vector<std::string> wrapped(const std::string& text, float width, float size)
{
    if (gfx::measureText(text, size) <= width) return {text};
    std::vector<std::string> words;
    for (size_t start = 0; start < text.size();)
    {
        const auto space = text.find(' ', start);
        words.push_back(text.substr(start, space == std::string::npos ? std::string::npos : space - start));
        if (space == std::string::npos) break;
        start = space + 1;
    }
    if (words.size() < 2) return {text};
    // The first line takes as many words as fit; the rest goes to the second (truncated by paint::text if too long).
    std::string first = words[0];
    size_t next = 1;
    while (next < words.size() && gfx::measureText(first + " " + words[next], size) <= width) first += " " + words[next++];
    std::string second;
    for (size_t i = next; i < words.size(); ++i) second += (second.empty() ? "" : " ") + words[i];
    return second.empty() ? std::vector<std::string>{first} : std::vector<std::string>{first, second};
}

SkColor paletteColour(int index)
{
    const auto rgb = model::livePaletteColour(index);
    return SkColorSetRGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

bool isDark(SkColor c) { return 0.299f * SkColorGetR(c) + 0.587f * SkColorGetG(c) + 0.114f * SkColorGetB(c) < 110.0f; }

SkColor macroAssignment(const Theme& t) { return t.color("MacroAssignment"); }

std::string dropHint(const model::Device& d)
{
    if (d.type == "AudioEffectGroupDevice") return "Drop Audio Effects Here";
    if (d.type == "MidiEffectGroupDevice") return "Drop MIDI Effects Here";
    return "Drop an Instrument or Sample Here";
}

void ring(SkCanvas& c, float cx, float cy, float radius, float stroke, SkColor colour)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(stroke);
    pen.setColor(colour);
    c.drawCircle(cx, cy, radius, pen);
}

void disc(SkCanvas& c, float cx, float cy, float radius, SkColor colour)
{
    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setColor(colour);
    c.drawCircle(cx, cy, radius, fill);
}
} // namespace

// ---------------------------------------------------------------- value texts
std::string deviceParameterText(devices::DeviceProcessor* p, int index)
{
    if (p == nullptr || index < 0 || (size_t) index >= p->parameters().size()) return {};
    if (DeviceFace::supports(p)) return DeviceFace::valueText(p, index);
    if (dynamic_cast<devices::Reverb*>(p) || dynamic_cast<devices::Delay*>(p)) return effectParameterText(p, index);
    const auto& info = p->parameters()[(size_t) index];
    const float v = p->getParameter(index);
    if (! info.choices.empty()) return info.choices[(size_t) std::clamp((int) std::lround(v), 0, (int) info.choices.size() - 1)];
    if (info.isToggle) return v > 0.5f ? "On" : "Off";
    if (info.unit == "Hz") return v >= 1000 ? ReferenceFace::significant(v / 1000) + " kHz" : ReferenceFace::significant(v) + " Hz";
    if (info.unit == "ms") return v >= 1000 ? ReferenceFace::significant(v / 1000) + " s" : ReferenceFace::significant(v) + " ms";
    if (info.unit == "dB") return printed(v, "%.1f dB");
    if (info.unit == "lin") return v <= 0.0000317f ? std::string("-inf dB") : printed(gainDb(v), "%.1f dB");
    if (info.unit == "%") return printed(v * 100.0, "%.0f %%");
    if (info.unit == "pct") return printed(v, "%.0f %%");
    return ReferenceFace::significant(v);
}

std::string macroValueText(const model::Device& rack, const devices::RackProcessor& processor, int macro)
{
    // Mappings in the model: the reference counts every mapped parameter, whether or not Sonora runs its device.
    int count = 0, chainOf = -1, deviceOf = -1;
    std::string path;
    for (size_t ci = 0; ci < rack.chains.size(); ++ci)
        for (size_t di = 0; di < rack.chains[ci].devices.size(); ++di)
            for (const auto& p : rack.chains[ci].devices[di].parameters)
                if (p.macroIndex == macro && ! p.booleanValue && p.path != "On")
                {
                    ++count;
                    chainOf = (int) ci;
                    deviceOf = (int) di;
                    path = p.path;
                }
    if (count == 1 && (size_t) chainOf < processor.chains().size())
    {
        const auto& processors = processor.chains()[(size_t) chainOf].processorForModelDevice;
        if (auto* target = (size_t) deviceOf < processors.size() ? processors[(size_t) deviceOf] : nullptr)
        {
            const auto& infos = target->parameters();
            for (size_t i = 0; i < infos.size(); ++i)
                if (infos[i].id == path) return deviceParameterText(target, (int) i);
        }
    }
    return printed(processor.getParameter(macro), "%.0f");
}

// ---------------------------------------------------------------- macro box
class RackMacroBox final : public Widget
{
public:
    RackMacroBox(RackFace& face, int macro) : face_(face), macro_(macro) {}
    int macro() const { return macro_; }

    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = face_.unit();
        const auto r = localBounds();
        const float corner = 4 * u;
        paint::fillRounded(c, r, corner, t.color("SurfaceArea")); // the 2-px border
        const Rect inner = r.reduced(2 * u, 2 * u);
        paint::fillRounded(c, inner, corner * 0.6f, t.color("SurfaceBackground"));
        const int colourIndex = (size_t) macro_ < face_.model().macroColors.size() ? face_.model().macroColors[(size_t) macro_] : -1;
        SkColor ink = t.color("ControlForeground");
        if (colourIndex >= 0)
        {
            const auto header = paletteColour(colourIndex);
            paint::fill(c, {inner.x, inner.y, inner.w, kHeaderH * u}, header);
            if (isDark(header)) ink = SkColorSetRGB(0xff, 0xff, 0xff);
        }
        const float size = face_.macroText();
        const auto lines = wrapped(name(), inner.w - 6 * u, size);
        for (size_t i = 0; i < lines.size(); ++i)
            paint::text(c, lines[i], {inner.x, inner.y + (5.5f + 21.0f * (float) i) * u, inner.w, 24 * u}, ink, size, Align::Centre);

        // Knob: 54 px across outside, 4.5 thick, centred 99 px below the box top; the value 135 px below.
        const auto* rack = face_.rack();
        const float value = rack ? rack->getParameter(macro_) : 0.0f, norm = std::clamp(value / 127.0f, 0.0f, 1.0f);
        const float cx = r.centreX() + 1 * u, cy = 99 * u, stroke = 4.5f * u, d = (54 - 4.5f) * u;
        const SkRect oval = SkRect::MakeXYWH(cx - d / 2, cy - d / 2, d, d);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(stroke);
        pen.setStrokeCap(SkPaint::kButt_Cap);
        pen.setColor(t.color("ControlForeground"));
        c.drawArc(oval, kStartDeg, kSweepDeg, false, pen);
        if (norm > 0.001f)
        {
            pen.setColor(t.color("RangeDefault"));
            c.drawArc(oval, kStartDeg, kSweepDeg * norm, false, pen);
        }
        const float angle = (kStartDeg + kSweepDeg * norm) * (float) M_PI / 180.0f;
        pen.setColor(t.color("ControlForeground"));
        pen.setStrokeCap(SkPaint::kSquare_Cap);
        const float reach = d / 2 + stroke * 0.5f;
        c.drawLine(cx, cy, cx + std::cos(angle) * reach, cy + std::sin(angle) * reach, pen);
        const auto text = rack ? macroValueText(face_.model(), *rack, macro_) : std::string("0");
        paint::text(c, text, {r.x, 123 * u, r.w, 24 * u}, t.color("ControlForeground"), size, Align::Centre);

        if (face_.mapping()) // Map mode: every macro is a mapping target, the chosen one framed
        {
            const auto assign = macroAssignment(t);
            paint::fillRounded(c, inner, corner * 0.6f, paint::withAlpha(assign, 60));
            if (ctx().rackMacroToMap == macro_) paint::stroke(c, r.reduced(1.5f * u, 1.5f * u), assign, 3 * u);
        }
    }

    void mouseDown(const MouseEvent& e) override
    {
        if (face_.mapping() || e.right)
        {
            ctx().rackMacroToMap = macro_;
            ctx().statusText = "Map: click a parameter in the chain to map it to " + name();
            repaint();
            return;
        }
        startY_ = e.y;
        start_ = face_.rack() ? face_.rack()->getParameter(macro_) : 0.0f;
        dragging_ = face_.rack() != nullptr;
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! dragging_) return;
        const float pixels = (e.shift ? 1000.0f : 200.0f) * face_.unit() / 0.52f; // full range, like the device knobs
        set(start_ + (startY_ - e.y) / pixels * 127.0f);
    }
    void mouseUp(const MouseEvent&) override { dragging_ = false; }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right || face_.mapping() || ! face_.rack()) { mouseDown(e); return; }
        set(face_.rack()->parameters()[(size_t) macro_].defaultValue);
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (! face_.rack() || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        set(face_.rack()->getParameter(macro_) + (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.1f : 1.0f));
        return true;
    }

private:
    std::string name() const
    {
        const auto& names = face_.model().macroNames;
        return (size_t) macro_ < names.size() && ! names[(size_t) macro_].empty() ? names[(size_t) macro_] : "Macro " + std::to_string(macro_ + 1);
    }
    void set(float value)
    {
        face_.rack()->setParameter(macro_, std::clamp(value, 0.0f, 127.0f));
        repaint();
    }
    RackFace& face_;
    int macro_;
    float startY_ = 0, start_ = 0;
    bool dragging_ = false;
};

// ---------------------------------------------------------------- face
RackFace::RackFace(const model::Device& model, devices::RackProcessor* rack, int track, int device, int chain, int child, Tag tag)
    : ReferenceFace(rack, 972, kOriginX, kOriginY, std::move(tag), kCaptureBody), model_(model), rack_(rack), track_(track),
      device_(device), chain_(chain), child_(child)
{
    for (int i = 0; i < devices::RackProcessor::kNumMacros; ++i)
    {
        auto* box = add<RackMacroBox>(*this, i);
        macros_.push_back(box);
        if (rack_) ReferenceFace::tag(box, i); // MIDI-mappable to MacroControls.i
    }
    if (drums()) drumPanel_ = add<NativeDevicePanel>(model, track, device, chain, child);
    meterPeak_.resize(model.chains.size());
    meterRms_.resize(model.chains.size());
    captureWidth_ = captureRight() - kOriginX;
}

// ---- view switches
namespace
{
UiContext::RackView& viewOf(UiContext& x, const model::Device& d)
{
    auto [it, fresh] = x.rackViews.try_emplace(d.id);
    if (fresh)
    {
        it->second.macros = d.macrosVisible;
        it->second.chains = d.type == "DrumGroupDevice" ? true : d.chainListVisible;
        it->second.zones = d.zonesVisible ? 1 : 0;
    }
    return it->second;
}
} // namespace

bool RackFace::macrosShown() const { return parent() == nullptr ? model_.macrosVisible : viewOf(ctx(), model_).macros; }
bool RackFace::chainsShown() const
{
    return parent() == nullptr ? (drums() || model_.chainListVisible) : viewOf(ctx(), model_).chains;
}
bool RackFace::devicesShown() const { return parent() != nullptr && ctx().selectedRackId == model_.id; }
RackFace::Zones RackFace::zones() const
{
    if (drums() || ! chainsShown()) return Zones::Hide;
    return (Zones) std::clamp(parent() == nullptr ? (model_.zonesVisible ? 1 : 0) : viewOf(ctx(), model_).zones, 0, 3);
}
bool RackFace::mapping() const { return parent() != nullptr && ctx().rackMapId == model_.id; }
int RackFace::visibleMacros() const { return std::clamp(model_.numVisibleMacros, 1, devices::RackProcessor::kNumMacros); }
int RackFace::selectedChain() const
{
    if (model_.chains.empty()) return -1;
    return parent() != nullptr && ctx().selectedRackId == model_.id ? std::clamp(ctx().selectedRackChain, 0, (int) model_.chains.size() - 1) : 0;
}

// ---- geometry (capture px)
namespace
{
int macroColumns(int visible) { return visible <= 1 ? 1 : (visible + 1) / 2; }
} // namespace

float RackFace::chainLeft() const
{
    if (! macrosShown()) return 50;
    return kMacroX + (float) macroColumns(visibleMacros()) * kMacroPitchX - 4 + 12;
}

float RackFace::captureRight() const
{
    if (chainsShown())
        return chainLeft() + (drums() ? kDrumW : kListW + (zones() != Zones::Hide ? kZonesW : 0.0f)) + 18;
    if (macrosShown()) return chainLeft() - 12 + 18;
    return 60;
}

Rect RackFace::stripBounds(Strip s) const
{
    const float y = kStripY[(int) s];
    return at(kStripX - kStripD / 2, y - kStripD / 2, kStripX + kStripD / 2, y + kStripD / 2);
}

Rect RackFace::macroBounds(int macro) const
{
    const int cols = macroColumns(visibleMacros());
    const float x0 = kMacroX + (float) (macro % cols) * kMacroPitchX, y0 = kMacroY + (float) (macro / cols) * kMacroPitchY;
    return at(x0, y0, x0 + kMacroW, y0 + kMacroH);
}

Rect RackFace::autoSelectBounds() const { const float l = chainLeft(); return at(l + 6, kTabTop, l + 42, kTabBottom); }

Rect RackFace::zoneTabBounds(Zones z) const
{
    const float right = chainLeft() + kListW, x0 = right - 274 + (float) (z == Zones::Hide ? 3 : (int) z - 1) * 66;
    return at(x0, kTabTop, x0 + 60, kTabBottom);
}

Rect RackFace::chainListBounds() const
{
    const float l = chainLeft();
    return at(l, kListTop, l + kListW + (zones() != Zones::Hide ? kZonesW : 0.0f), kListBottom);
}

namespace
{
float rowY(int row) { return kRowTop + (float) row * kRowPitch; }
} // namespace

Rect RackFace::chainRowBounds(int chain) const
{
    const float l = chainLeft(), y = rowY(chain);
    return at(l + 2, y, l + kListW + (zones() != Zones::Hide ? kZonesW : 0.0f) - 2, y + kRowH);
}
Rect RackFace::chainNameBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 22, y, l + 174, y + kRowH); }
Rect RackFace::volumeBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 194, y + 4, l + 270, y + 30); }
Rect RackFace::panBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 278, y + 4, l + 354, y + 30); }
Rect RackFace::speakerBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 362, y + 4, l + 394, y + 30); }
Rect RackFace::soloBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 402, y + 4, l + 434, y + 30); }
Rect RackFace::hotSwapBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 442, y + 4.5f, l + 468, y + 30.5f); }
Rect RackFace::meterBounds(int chain) const { const float l = chainLeft(), y = rowY(chain); return at(l + 474, y + 2, l + 484, y + 32); }
Rect RackFace::zoneRulerBounds() const { const float r = chainLeft() + kListW; return at(r + 6, kTabTop, r + kZonesW - 6, kTabBottom); }

float RackFace::zoneX(float value) const
{
    const auto ruler = zoneRulerBounds();
    return ruler.x + ruler.w * std::clamp(value, 0.0f, 128.0f) / 128.0f;
}
int RackFace::zoneSteps(float x) const
{
    return (int) std::lround((x - dragStartX_) / std::max(1.0f, zoneRulerBounds().w) * 128.0f);
}
float RackFace::zoneValueAt(float x) const
{
    const auto ruler = zoneRulerBounds();
    return std::clamp((x - ruler.x) / std::max(1.0f, ruler.w) * 128.0f, 0.0f, 127.0f);
}

std::pair<int, int> RackFace::zoneOf(int chain) const
{
    const auto& c = model_.chains[(size_t) chain];
    if (drag_ >= Drag::ZoneMin && drag_ <= Drag::ZoneMove && dragChain_ == chain) return {previewMin_, previewMax_};
    switch (zones())
    {
        case Zones::Velocity: return {c.velMin, c.velMax};
        case Zones::Chain: return {c.selMin, c.selMax};
        case Zones::Hide:
        case Zones::Key: break;
    }
    return {c.keyMin, c.keyMax};
}

Rect RackFace::zoneBarBounds(int chain) const
{
    const auto [lo, hi] = zoneOf(chain);
    const auto row = chainRowBounds(chain);
    const float x0 = zoneX((float) lo), x1 = zoneX((float) hi + 1.0f);
    return {x0, row.y + 8 * unit(), std::max(2.0f, x1 - x0), 20 * unit()};
}

Rect RackFace::titleButtonsCapture() const
{
    // Rand at 413..472 and Map at 482..532 in the capture's title; a narrower rack keeps them left of the title icons.
    const float right = std::min(532.0f, captureRight() - 112); // the title icons take the last ~100 capture px
    return at(right - 119, 0, right, 0);
}

int RackFace::rowAt(float y) const
{
    const float capture = y / unit() + kOriginY;
    if (capture < kRowTop) return -1;
    return (int) std::floor((capture - kRowTop) / kRowPitch);
}

// ---- live chain mixer
float RackFace::chainVolume(int chain) const
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) return mix->volume.load();
    return (float) model_.chains[(size_t) chain].volume;
}
float RackFace::chainPan(int chain) const
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) return mix->pan.load();
    return (float) model_.chains[(size_t) chain].pan;
}
bool RackFace::chainActive(int chain) const
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) return mix->active.load();
    return model_.chains[(size_t) chain].speakerOn;
}
bool RackFace::chainSolo(int chain) const
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) return mix->solo.load();
    return model_.chains[(size_t) chain].solo;
}
void RackFace::setVolume(int chain, float gain)
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) mix->volume.store(std::clamp(gain, 0.0f, model::kFaderMaxGain));
    repaint();
}
void RackFace::setPan(int chain, float pan)
{
    if (auto* mix = rack_ ? rack_->chainMix(chain) : nullptr) mix->pan.store(std::clamp(pan, -1.0f, 1.0f));
    repaint();
}

void RackFace::edit(const std::function<void(model::Device&)>& operation, const std::string& label)
{
    const int chain = chain_, child = child_;
    if (ctx().editDevice)
        ctx().editDevice(track_, device_, [operation, chain, child](model::Device& root) {
            if (chain >= 0)
            {
                if ((size_t) chain >= root.chains.size() || child < 0 || (size_t) child >= root.chains[(size_t) chain].devices.size()) return;
                root.chainsEdited = true;
                operation(root.chains[(size_t) chain].devices[(size_t) child]);
            }
            else operation(root);
        }, label);
}

void RackFace::randomizeMacros(uint32_t seed)
{
    std::mt19937 random(seed != 0 ? seed : std::random_device{}());
    std::uniform_real_distribution<double> range(0.0, 127.0);
    std::array<double, devices::RackProcessor::kNumMacros> values{};
    for (auto& v : values) v = range(random);
    const int visible = visibleMacros();
    edit([values, visible](model::Device& d) {
        for (int i = 0; i < visible; ++i)
        {
            if (d.macroNoRandom[(size_t) i]) continue;
            const auto path = "MacroControls." + std::to_string(i);
            auto found = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == path; });
            if (found == d.parameters.end())
            {
                model::Parameter p;
                p.path = path;
                d.parameters.push_back(p);
                found = d.parameters.end() - 1;
            }
            found->value = values[(size_t) i];
        }
    }, "Randomize Macros");
}

void RackFace::toggleMapping()
{
    auto& x = ctx();
    if (mapping()) { x.rackMapId = -1; x.statusText = "Ready"; }
    else
    {
        x.rackMapId = model_.id;
        x.rackMacroToMap = std::clamp(x.rackMacroToMap, 0, visibleMacros() - 1);
        if (chain_ < 0 && ! model_.chains.empty()) selectChain(selectedChain()); // the chain's devices hold the targets
        x.statusText = "Map: choose a macro, then click a parameter in the chain";
    }
    if (x.requestRepaint) x.requestRepaint();
    repaint();
}

void RackFace::selectChain(int chain)
{
    auto& x = ctx();
    if (chain < 0 || (size_t) chain >= model_.chains.size()) return;
    if (chain_ < 0) x.selectedRackId = model_.id; // only top-level racks open their chain in the device chain
    x.selectedRackChain = chain;
    repaint();
}

void RackFace::updateWidth()
{
    const float width = captureRight() - kOriginX;
    if (std::abs(width - captureWidth_) < 0.01f) { layout(); repaint(); return; }
    captureWidth_ = width;
    relayoutChain();
}

// ---- layout / paint
void RackFace::layout()
{
    ReferenceFace::layout();
    if (std::abs(captureRight() - kOriginX - captureWidth_) > 0.01f) captureWidth_ = captureRight() - kOriginX; // switches seeded from the set
    const int visible = visibleMacros();
    for (auto* box : macros_)
    {
        box->visible = macrosShown() && box->macro() < visible;
        if (box->visible) box->setBounds(macroBounds(box->macro()));
    }
    if (drumPanel_)
    {
        drumPanel_->visible = chainsShown();
        const float l = chainLeft();
        drumPanel_->setBounds(at(l, kMacroY, l + kDrumW, kListBottom));
    }
}

void RackFace::tick()
{
    if (! rack_) return;
    bool changed = false;
    const size_t count = std::min(model_.chains.size(), meterPeak_.size());
    for (size_t ci = 0; ci < count; ++ci)
        if (auto* mix = rack_->chainMix((int) ci))
        {
            const auto peak = mix->meter.takePeak(), rms = mix->meter.takeRms();
            for (size_t ch = 0; ch < 2; ++ch)
            {
                const float p = meterDecay(peak[ch], meterPeak_[ci][ch]), r = meterDecay(rms[ch], meterRms_[ci][ch]);
                changed = changed || std::abs(p - meterPeak_[ci][ch]) > 0.0f || std::abs(r - meterRms_[ci][ch]) > 0.0f;
                meterPeak_[ci][ch] = p;
                meterRms_[ci][ch] = r;
            }
        }
    if (changed && chainsShown() && ! drums()) repaint();
}

void RackFace::paint(SkCanvas& c, const Theme& t)
{
    // The body sits in a 4-px frame of the title colour (capture: x 6..9 and the bottom rows).
    paint::fill(c, localBounds(), t.color("SurfaceHighlight"));
    const float right = captureRight();
    paint::fill(c, at(10, kOriginY, right - 4, kOriginY + kCaptureBody - 4), t.color("SurfaceBackground"));
    paintStrip(c, t);
    if (chainsShown() && ! drums()) paintChains(c, t);
}

void RackFace::paintStrip(SkCanvas& c, const Theme& t) const
{
    const float u = unit();
    const auto cut = t.color("SurfaceBackground"), amber = t.color("ChosenDefault"), dark = t.color("ControlForeground");
    for (int i = 0; i < 6; ++i)
    {
        const auto s = (Strip) i;
        const auto b = stripBounds(s);
        const float cx = b.centreX(), cy = b.centreY(), radius = b.w * 0.5f;
        const bool on = s == Strip::Macros ? macrosShown() : s == Strip::Devices ? devicesShown() : s == Strip::ChainList ? chainsShown() : false;
        if (s == Strip::MoreMacros || s == Strip::FewerMacros)
        {
            const bool enabled = s == Strip::MoreMacros ? visibleMacros() < devices::RackProcessor::kNumMacros : visibleMacros() > 1;
            disc(c, cx, cy, radius - 1 * u, t.color("ControlBackground"));
            ring(c, cx, cy, radius - 1.5f * u, 2.5f * u, t.color("ControlContrastFrame"));
            const auto ink = enabled ? dark : t.color("TextDisabled");
            paint::fill(c, {cx - 7 * u, cy - 1.5f * u, 14 * u, 3 * u}, ink);
            if (s == Strip::MoreMacros) paint::fill(c, {cx - 1.5f * u, cy - 7 * u, 3 * u, 14 * u}, ink);
            continue;
        }
        disc(c, cx, cy, radius, on ? amber : dark);
        switch (s)
        {
            case Strip::Macros: // a knob: open ring and a needle
            {
                SkPaint pen;
                pen.setAntiAlias(true);
                pen.setStyle(SkPaint::kStroke_Style);
                pen.setStrokeWidth(2.6f * u);
                pen.setColor(cut);
                const float d = 15 * u;
                c.drawArc(SkRect::MakeXYWH(cx - d / 2, cy - d / 2, d, d), 100, 300, false, pen);
                c.drawLine(cx, cy, cx + 4.5f * u, cy + 4.5f * u, pen);
                break;
            }
            case Strip::Variations: // a camera with a plus
                paint::fill(c, {cx - 7 * u, cy - 4 * u, 14 * u, 10 * u}, cut);
                paint::fill(c, {cx - 4 * u, cy - 6.5f * u, 5 * u, 3 * u}, cut);
                paint::fill(c, {cx - 3 * u, cy + 0.2f * u, 6 * u, 1.6f * u}, dark);
                paint::fill(c, {cx - 0.8f * u, cy - 2 * u, 1.6f * u, 6 * u}, dark);
                break;
            case Strip::Devices: // a device: framed box with a title bar
                paint::stroke(c, {cx - 8 * u, cy - 5 * u, 16 * u, 10 * u}, cut, 1.6f * u);
                paint::fill(c, {cx - 8 * u, cy - 5 * u, 16 * u, 3 * u}, cut);
                break;
            case Strip::ChainList: // a list: three bullets and lines
                for (int row = 0; row < 3; ++row)
                {
                    const float y = cy - 5 * u + (float) row * 4.2f * u;
                    paint::fill(c, {cx - 7.5f * u, y, 2.6f * u, 2.6f * u}, cut);
                    paint::fill(c, {cx - 3.5f * u, y, 11 * u, 2.6f * u}, cut);
                }
                break;
            case Strip::MoreMacros:
            case Strip::FewerMacros: break;
        }
    }
}

void RackFace::paintChains(SkCanvas& c, const Theme& t) const
{
    const float u = unit(), size = macroText();
    const auto frame = t.color("ControlContrastFrame"), ink = t.color("ControlForeground");
    auto box = [&](const Rect& r, SkColor fill) {
        paint::fill(c, r, frame);
        paint::fill(c, r.reduced(2 * u, 2 * u), fill);
    };
    // Auto-select (follows played notes) is not implemented: drawn disabled (UI-66b).
    const auto auto_ = autoSelectBounds();
    box(auto_, t.color("ControlBackground"));
    {
        const auto glyph = t.color("TextDisabled");
        const float cx = auto_.centreX(), cy = auto_.centreY();
        paint::fill(c, {cx - 7 * u, cy - 4 * u, 2 * u, 8 * u}, glyph);
        paint::fill(c, {cx - 3.5f * u, cy - 4 * u, 2 * u, 8 * u}, glyph);
        SkPathBuilder tri;
        tri.moveTo(cx, cy - 6 * u).lineTo(cx + 7 * u, cy).lineTo(cx, cy + 6 * u).close();
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(glyph);
        c.drawPath(tri.detach(), fill);
    }
    const char* tabs[4] = {"Hide", "Key", "Vel", "Chain"};
    for (int z = 0; z < 4; ++z)
    {
        const auto r = zoneTabBounds((Zones) z);
        box(r, zones() == (Zones) z ? t.color("RangeDefault") : t.color("ControlBackground"));
        paint::text(c, tabs[z], r, ink, size, Align::Centre);
    }

    // The list: 2-px frame, one 36-px row per chain with a 2-px separator.
    const auto list = chainListBounds();
    paint::fill(c, list, t.color("SurfaceArea"));
    paint::fill(c, list.reduced(2 * u, 2 * u), t.color("SurfaceBackground"));
    c.save();
    c.clipRect(list.reduced(2 * u, 2 * u).sk());
    const int selected = selectedChain();
    for (int i = 0; i < (int) model_.chains.size(); ++i)
    {
        const auto row = chainRowBounds(i);
        if (row.y > list.bottom()) break;
        const auto& chain = model_.chains[(size_t) i];
        paint::fill(c, row, i == selected ? t.color("StandbySelectionBackground") : t.color("SurfaceBackground"));
        paint::fill(c, {row.x, row.bottom(), row.w, 2 * u}, t.color("SurfaceArea"));
        const auto nameBox = chainNameBounds(i);
        const auto colour = chain.color >= 0 ? paletteColour(chain.color) : t.color("SurfaceHighlight");
        paint::fill(c, nameBox, colour);
        {
            // The reference's font is narrower than the system one: shrink a name up to 25 % before truncating it.
            const auto name = chain.name.empty() ? "Chain " + std::to_string(i + 1) : chain.name;
            const auto room = nameBox.reduced(6 * u, 0);
            const float fit = std::max(size * 0.75f, std::min(size, size * room.w * 0.98f / std::max(1.0f, gfx::measureText(name, size))));
            paint::text(c, name, room, isDark(colour) ? SkColorSetRGB(0xff, 0xff, 0xff) : ink, fit);
        }
        // Drag handle: six dots.
        const float dotX = list.x + 183 * u;
        for (int dot = 0; dot < 6; ++dot) disc(c, dotX, row.y + (6 + (float) dot * 4.6f) * u, 1.9f * u, ink);
        // Volume: number box with the fader-law fill; pan: number box.
        const float gain = chainVolume(i);
        const auto vol = volumeBounds(i);
        box(vol, t.color("ControlTextBack"));
        const auto inner = vol.reduced(2 * u, 2 * u);
        paint::fill(c, inner.withWidth(inner.w * model::faderGainToPosition(gain)), paint::mix(t.color("RangeDefault"), ink, 0.17f));
        paint::text(c, volumeText(gain), vol, ink, size, Align::Centre);
        const auto pan = panBounds(i);
        box(pan, t.color("ControlTextBack"));
        if (const float p = chainPan(i); std::abs(p) > 0.005f)
        {
            const auto in = pan.reduced(2 * u, 2 * u);
            const float centre = in.centreX(), edge = centre + p * in.w * 0.5f;
            paint::fill(c, {std::min(centre, edge), in.y, std::abs(edge - centre), in.h}, paint::mix(t.color("RangeDefault"), ink, 0.17f));
        }
        paint::text(c, panText(chainPan(i)), pan, ink, size, Align::Centre);
        // Chain activator (speaker), solo, hot-swap.
        const auto speaker = speakerBounds(i);
        const bool active = chainActive(i);
        box(speaker, active ? t.color("ChosenDefault") : t.color("ControlBackground"));
        {
            const float cx = speaker.centreX(), cy = speaker.centreY();
            SkPathBuilder cone;
            cone.moveTo(cx - 6 * u, cy - 3 * u).lineTo(cx - 2.5f * u, cy - 3 * u).lineTo(cx + 3 * u, cy - 8 * u)
                .lineTo(cx + 3 * u, cy + 8 * u).lineTo(cx - 2.5f * u, cy + 3 * u).lineTo(cx - 6 * u, cy + 3 * u).close();
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(2 * u);
            pen.setColor(ink);
            c.drawPath(cone.detach(), pen);
        }
        const auto solo = soloBounds(i);
        box(solo, chainSolo(i) ? t.color("ChosenPreListen") : t.color("ControlBackground"));
        paint::text(c, "S", solo, ink, size, Align::Centre);
        const auto swap = hotSwapBounds(i);
        titleIcon(c, TitleIcon::HotSwap, swap.centreX(), swap.centreY(), swap.w, ink); // a click says hot-swap is not there yet (UI-42b)
        // Meter: two rails, post-fader chain output.
        const auto m = meterBounds(i);
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const Rect rail{m.x + (float) ch * 6 * u, m.y, 4 * u, m.h};
            paint::fill(c, rail, t.color("MeterBackground"));
            if ((size_t) i < meterRms_.size())
                if (const float h = rail.h * Fader::gainToPosition(meterRms_[(size_t) i][ch]); h > 0)
                    paint::fill(c, {rail.x, rail.bottom() - h, rail.w, h}, Fader::meterColour(meterRms_[(size_t) i][ch], ! active));
        }
    }
    // Free space under the rows: where devices are dropped to make a chain.
    const float rowsBottom = chainRowBounds((int) model_.chains.size()).y;
    if (rowsBottom < list.bottom() - 30 * u)
    {
        const Rect free{list.x, rowsBottom, at(chainLeft(), 0, chainLeft() + kListW, 0).w, list.bottom() - rowsBottom};
        const auto lines = wrapped(dropHint(model_), 320 * u, size);
        const float lineH = 30 * u, top = free.centreY() - lineH * (float) lines.size() * 0.5f;
        for (size_t i = 0; i < lines.size(); ++i)
            paint::text(c, lines[i], {free.x, top + lineH * (float) i, free.w, lineH}, ink, size, Align::Centre);
    }
    c.restore();
    if (zones() != Zones::Hide) paintZones(c, t);
}

void RackFace::paintZones(SkCanvas& c, const Theme& t) const
{
    const float u = unit(), size = text();
    const auto ink = t.color("ControlForeground");
    const auto ruler = zoneRulerBounds();
    paint::fill(c, ruler, t.color("ControlContrastFrame"));
    paint::fill(c, ruler.reduced(2 * u, 2 * u), t.color("ControlTextBack"));
    const auto mode = zones();
    // Ruler: octaves for keys (C-2 .. C8), quarters of 0..127 otherwise.
    if (mode == Zones::Key)
    {
        for (int note = 0; note <= 120; note += 12)
        {
            const float x = zoneX((float) note);
            paint::line(c, x, ruler.bottom() - 6 * u, x, ruler.bottom() - 2 * u, ink, 1.2f * u);
            if (note % 36 == 0) paint::text(c, "C" + std::to_string(note / 12 - 2), {x + 2 * u, ruler.y, 40 * u, ruler.h - 4 * u}, ink, size);
        }
    }
    else
    {
        for (int v = 0; v <= 128; v += 32)
        {
            const float x = zoneX((float) v);
            paint::line(c, x, ruler.bottom() - 6 * u, x, ruler.bottom() - 2 * u, ink, 1.2f * u);
            if (v < 128) paint::text(c, std::to_string(v == 0 && mode == Zones::Velocity ? 1 : v), {x + 2 * u, ruler.y, 40 * u, ruler.h - 4 * u}, ink, size);
        }
    }
    const auto list = chainListBounds();
    c.save();
    c.clipRect(list.reduced(2 * u, 2 * u).sk());
    const int selected = selectedChain();
    const auto fill = t.color("RangeDefault");
    for (int i = 0; i < (int) model_.chains.size(); ++i)
    {
        const auto bar = zoneBarBounds(i);
        if (bar.y > list.bottom()) break;
        paint::fill(c, bar, i == selected ? fill : paint::mix(fill, t.color("SurfaceBackground"), 0.45f));
        paint::stroke(c, bar, t.color("ControlContrastFrame"), 1.2f * u);
    }
    if (mode == Zones::Chain && rack_) // the chain selector: an amber line through the rows, dragged on the ruler
    {
        const float x = zoneX(rack_->getParameter(devices::RackProcessor::kChainSelector) + 0.5f);
        paint::line(c, x, ruler.bottom(), x, list.bottom(), t.color("ChosenDefault"), 2 * u);
    }
    c.restore();
    if (mode == Zones::Chain && rack_)
    {
        const float x = zoneX(rack_->getParameter(devices::RackProcessor::kChainSelector) + 0.5f);
        SkPathBuilder handle;
        handle.moveTo(x - 6 * u, ruler.y + 2 * u).lineTo(x + 6 * u, ruler.y + 2 * u).lineTo(x, ruler.bottom() - 2 * u).close();
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setColor(t.color("ChosenDefault"));
        c.drawPath(handle.detach(), pen);
    }
}

// ---- input
void RackFace::mouseDown(const MouseEvent& e)
{
    drag_ = Drag::None;
    dragChain_ = -1;
    auto& x = ctx();
    for (int i = 0; i < 6; ++i)
        if (stripBounds((Strip) i).contains(e.x, e.y))
        {
            if (e.right) return;
            auto& view = viewOf(x, model_);
            switch ((Strip) i)
            {
                case Strip::Macros: view.macros = ! view.macros; break;
                case Strip::MoreMacros:
                case Strip::FewerMacros:
                {
                    const int next = std::clamp(visibleMacros() + ((Strip) i == Strip::MoreMacros ? 1 : -1), 1, devices::RackProcessor::kNumMacros);
                    if (next != visibleMacros())
                        edit([next](model::Device& d) { d.numVisibleMacros = next; }, (Strip) i == Strip::MoreMacros ? "Show More Macros" : "Show Fewer Macros");
                    return;
                }
                case Strip::Variations: x.statusText = "Macro variations are not available yet (UI-66c)"; repaint(); return;
                case Strip::Devices:
                    if (devicesShown()) x.selectedRackId = -1;
                    else if (! model_.chains.empty()) selectChain(selectedChain());
                    if (x.requestRepaint) x.requestRepaint();
                    repaint();
                    return;
                case Strip::ChainList: view.chains = ! view.chains; break;
            }
            updateWidth();
            return;
        }
    if (! chainsShown() || drums()) return;
    if (autoSelectBounds().contains(e.x, e.y)) { x.statusText = "Auto Select is not available yet (UI-66b)"; repaint(); return; }
    for (int z = 0; z < 4; ++z)
        if (zoneTabBounds((Zones) z).contains(e.x, e.y))
        {
            viewOf(x, model_).zones = z;
            updateWidth();
            return;
        }
    const auto mode = zones();
    if (mode == Zones::Chain && rack_ && zoneRulerBounds().contains(e.x, e.y))
    {
        drag_ = Drag::Selector;
        rack_->setParameter(devices::RackProcessor::kChainSelector, std::round(zoneValueAt(e.x)));
        repaint();
        return;
    }
    if (! chainListBounds().contains(e.x, e.y)) return;
    const int row = rowAt(e.y);
    const bool onRow = row >= 0 && row < (int) model_.chains.size() && chainRowBounds(row).contains(e.x, e.y);
    if (e.right)
    {
        std::vector<std::string> labels{"Create Chain"};
        if (onRow) labels.push_back("Delete");
        auto* self = this;
        // An async native menu may outlive a device-chain rebuild.
        auto pickFn = [self, row](int pick) {
            if (! Widget::isAlive(self)) return;
            if (pick == 0)
                self->edit([](model::Device& d) {
                    model::Device::Chain chain;
                    chain.name = "Chain " + std::to_string(d.chains.size() + 1);
                    d.chains.push_back(std::move(chain));
                    d.chainsEdited = true;
                }, "Create Chain");
            else if (pick == 1)
                self->edit([row](model::Device& d) {
                    if ((size_t) row < d.chains.size()) d.chains.erase(d.chains.begin() + row);
                    d.chainsEdited = true;
                }, "Delete Chain");
        };
        if (x.showChoices) x.showChoices(labels, std::vector<bool>(labels.size(), false), std::move(pickFn));
        else
        {
            juce::PopupMenu menu;
            for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i]);
            menu.showMenuAsync(juce::PopupMenu::Options(), [pickFn](int result) { if (result > 0) pickFn(result - 1); });
        }
        return;
    }
    if (! onRow) return;
    if (mode != Zones::Hide && e.x >= zoneRulerBounds().x - 2 * unit())
    {
        const auto bar = zoneBarBounds(row);
        const auto [lo, hi] = zoneOf(row);
        previewMin_ = lo;
        previewMax_ = hi;
        dragChain_ = row;
        dragStartX_ = e.x;
        dragStartValue_ = (float) lo;
        const float grab = 6 * unit();
        if (std::abs(e.x - bar.x) <= grab) drag_ = Drag::ZoneMin;
        else if (std::abs(e.x - bar.right()) <= grab) drag_ = Drag::ZoneMax;
        else if (bar.contains(e.x, e.y)) drag_ = Drag::ZoneMove;
        else
        {
            // A click beside the zone moves its nearer edge there, then the drag continues from it.
            drag_ = e.x < bar.x ? Drag::ZoneMin : Drag::ZoneMax;
            if (drag_ == Drag::ZoneMin) previewMin_ = std::min((int) std::floor(zoneValueAt(e.x)), previewMax_);
            else previewMax_ = std::max((int) std::floor(zoneValueAt(e.x)), previewMin_);
            repaint();
        }
        dragStartValue_ = (float) previewMin_;
        dragStartHi_ = previewMax_;
        selectChain(row);
        return;
    }
    if (volumeBounds(row).contains(e.x, e.y))
    {
        drag_ = Drag::Volume;
        dragChain_ = row;
        dragStartY_ = e.y;
        dragStartValue_ = model::faderGainToPosition(chainVolume(row));
        return;
    }
    if (panBounds(row).contains(e.x, e.y))
    {
        drag_ = Drag::Pan;
        dragChain_ = row;
        dragStartY_ = e.y;
        dragStartValue_ = chainPan(row);
        return;
    }
    if (speakerBounds(row).contains(e.x, e.y))
    {
        if (auto* mix = rack_ ? rack_->chainMix(row) : nullptr) mix->active.store(! mix->active.load());
        repaint();
        return;
    }
    if (soloBounds(row).contains(e.x, e.y))
    {
        if (rack_)
        {
            const bool next = ! chainSolo(row);
            for (int i = 0; i < (int) model_.chains.size(); ++i)
                if (auto* mix = rack_->chainMix(i))
                {
                    if (i == row) mix->solo.store(next);
                    else if (! e.cmd) mix->solo.store(false); // exclusive solo, Cmd adds to the soloed chains
                }
        }
        repaint();
        return;
    }
    if (hotSwapBounds(row).contains(e.x, e.y)) { x.statusText = "Hot-swapping presets is not available yet"; repaint(); return; }
    selectChain(row);
    if (x.requestRepaint) x.requestRepaint();
}

void RackFace::mouseDrag(const MouseEvent& e)
{
    const float pixels = (e.shift ? 1000.0f : 200.0f) * unit() / 0.52f;
    switch (drag_)
    {
        case Drag::Volume:
            setVolume(dragChain_, model::faderPositionToGain(std::clamp(dragStartValue_ + (dragStartY_ - e.y) / pixels, 0.0f, 1.0f)));
            break;
        case Drag::Pan: setPan(dragChain_, dragStartValue_ + (dragStartY_ - e.y) / pixels * 2.0f); break;
        case Drag::Selector:
            if (rack_) rack_->setParameter(devices::RackProcessor::kChainSelector, std::round(zoneValueAt(e.x)));
            repaint();
            break;
        // Edges move by the steps the pointer travelled, wherever on the edge the drag started.
        case Drag::ZoneMin: previewMin_ = std::clamp((int) dragStartValue_ + zoneSteps(e.x), 0, previewMax_); repaint(); break;
        case Drag::ZoneMax: previewMax_ = std::clamp(dragStartHi_ + zoneSteps(e.x), previewMin_, 127); repaint(); break;
        case Drag::ZoneMove:
        {
            const int span = previewMax_ - previewMin_;
            const int lo = std::clamp((int) dragStartValue_ + zoneSteps(e.x), 0, 127 - span);
            previewMin_ = lo;
            previewMax_ = lo + span;
            repaint();
            break;
        }
        case Drag::None: break;
    }
}

void RackFace::mouseUp(const MouseEvent&)
{
    const auto drag = drag_;
    drag_ = Drag::None;
    if (drag < Drag::ZoneMin || drag > Drag::ZoneMove || dragChain_ < 0) return;
    const int row = dragChain_, lo = previewMin_, hi = previewMax_;
    const auto mode = zones();
    const auto [oldLo, oldHi] = zoneOf(row);
    if (lo == oldLo && hi == oldHi) { repaint(); return; }
    edit([row, lo, hi, mode](model::Device& d) {
        if ((size_t) row >= d.chains.size()) return;
        auto& c = d.chains[(size_t) row];
        if (mode == Zones::Key) { c.keyMin = lo; c.keyMax = hi; }
        else if (mode == Zones::Velocity) { c.velMin = std::max(1, lo); c.velMax = std::max(std::max(1, lo), hi); }
        else { c.selMin = lo; c.selMax = hi; }
        d.chainsEdited = true;
    }, "Edit Chain Zone");
}

void RackFace::mouseDoubleClick(const MouseEvent& e)
{
    const int row = rowAt(e.y);
    if (! e.right && chainsShown() && ! drums() && row >= 0 && row < (int) model_.chains.size())
    {
        if (volumeBounds(row).contains(e.x, e.y)) { setVolume(row, 1.0f); return; }
        if (panBounds(row).contains(e.x, e.y)) { setPan(row, 0.0f); return; }
    }
    mouseDown(e);
}

bool RackFace::mouseWheel(const MouseEvent& e)
{
    if (std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY) || ! chainsShown() || drums()) return false;
    const int row = rowAt(e.y);
    if (row < 0 || row >= (int) model_.chains.size()) return false;
    const float step = (e.wheelDeltaY > 0 ? 1.0f : -1.0f) * (e.shift ? 0.1f : 1.0f);
    if (volumeBounds(row).contains(e.x, e.y))
    {
        setVolume(row, model::faderPositionToGain(std::clamp(model::faderGainToPosition(chainVolume(row)) + step * 0.01f, 0.0f, 1.0f)));
        return true;
    }
    if (panBounds(row).contains(e.x, e.y)) { setPan(row, chainPan(row) + step * 0.02f); return true; }
    return false;
}

bool RackFace::browserDrop(const BrowserDrag& payload, const MouseEvent& e)
{
    if (drums() || ! chainsShown() || ! chainListBounds().contains(e.x, e.y)) return false;
    int chain = rowAt(e.y);
    if (chain < 0 || chain >= (int) model_.chains.size()) chain = -1; // the free space: a new chain
    if (ctx().dropDeviceItem) ctx().dropDeviceItem(payload, track_, device_, chain, -1, child_);
    return true;
}
} // namespace sonora::ui
