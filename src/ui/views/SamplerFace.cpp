// DEV-14b / UI-43k: the Sampler (`MultiSampler`) face, matched to 1500 x 440 retina crops of the reference's Sampler
// ("Lovely Keys", Live 12.4.6, theme Default Light Cool High; /private/tmp/Sonora Parity/SamplerCaptures, outside the
// repository). The device box spans x 18..1430; its title band is y 10..44 (34 px) and the body y 44..386 (342 px,
// `captureBody`). Every box below is in those captures' pixels; boxes, displays and colours were measured with a pixel
// scan of the frames (#323540, 2 px) and dark displays.
//
// Bindings: each control names its .als path (see SamplerFace.h). Paths the processor exposes are live; the others are
// model values written back in one undo step when a gesture ends. The view leaves (ViewSettings/...) are saved with the
// set through UiContext::setDeviceViewValue, without an undo step.

#include "ui/views/SamplerFace.h"

#include "devices/DeviceProcessor.h"
#include "devices/native/Sampler.h"
#include "devices/native/Simpler.h"
#include "engine/PeakCache.h"
#include "engine/SampleCache.h"
#include "gfx/Brand.h"
#include "gfx/Fonts.h"
#include "ui/Controls.h"
#include "ui/UiContext.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <cstdio>
#include <deque>
#include <map>
#include <tuple>

namespace sonora::ui
{
namespace
{
using P = devices::DeviceProcessor;
constexpr float kOriginX = 18, kOriginY = 44, kWidth = 1412, kBody = 342;
constexpr float kRight = 1430, kTitleTop = 10, kTitleHeight = 34;
constexpr float kStagePx = 114;      // envelope displays: capture px per unit of a stage's time law
constexpr float kSustainEnd = 243;   // ... the sustain segment ends here (display x) unless the stages run longer
constexpr float kMinSustain = 32;
constexpr float kGainLaw = 4.2f;      // level boxes / displays of gain envelopes: norm = gain^(1 / kGainLaw)

std::string printed(double value, const char* format)
{
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}
double clean(double v, double tiny) { return std::abs(v) < tiny ? 0.0 : v; }

// ---------------------------------------------------------------- value specs
enum class Unit
{
    Ms,          // "0.29 ms", "397 ms", "30.0 s"
    Gain,        // linear gain shown in dB: "-8.1 dB", "-52 dB", "-inf dB"
    Db1,         // plain dB, one decimal: "-12.0 dB"
    Db2,         // plain dB, two decimals: "0.00 dB"
    Percent,     // 0..1: "36 %", "0.0 %"
    PercentInt,  // 0..1: "0 %"
    Percent2,    // 0..1: "0.00 %"
    PlainPct,    // 0..100: "0 %", "100 %"
    Semis,       // "0 st"
    SemisSigned, // "+5 st"
    SemisBare,   // "0"
    Cents,       // "-29 ct", "+11 ct"
    Hz,          // "1.00 Hz", "22.0 kHz"
    Degrees,     // "0.00°"
    Two,         // "0.00"
    Int,         // "3"
    Pan,         // "C", "50L"
    Note,        // "F2"
    Choice,
    Toggle, // "On" / "Off"
    Frames
};
enum class LawKind { Linear, Log, Gain };

struct Spec
{
    double min = 0, max = 1, def = 0;
    Unit unit = Unit::Two;
    LawKind law = LawKind::Linear;
    std::vector<std::string> choices;
    double logLow = 0; // Log: the law's low end (values below it map to 0)
};

Spec linear(double min, double max, double def, Unit unit) { return {min, max, def, unit, LawKind::Linear, {}, 0}; }
Spec logarithmic(double low, double max, double def, Unit unit, double min = -1) { return {min < 0 ? low : min, max, def, unit, LawKind::Log, {}, low}; }
Spec gain(double def) { return {0, 1, def, Unit::Gain, LawKind::Gain, {}, 0}; }
Spec choice(std::vector<std::string> choices, double def = 0)
{
    Spec s{0, (double) std::max<size_t>(1, choices.size()) - 1, def, Unit::Choice, LawKind::Linear, std::move(choices), 0};
    return s;
}
Spec toggleSpec(double def = 0) { return {0, 1, def, Unit::Toggle, LawKind::Linear, {}, 0}; }

double toNorm(const Spec& s, double v)
{
    switch (s.law)
    {
        case LawKind::Log: return v <= s.logLow ? 0.0 : std::log(v / s.logLow) / std::log(s.max / s.logLow);
        case LawKind::Gain: return v <= 0.000317 ? 0.0 : std::pow(std::clamp(v, 0.0, 1.0), 1.0 / kGainLaw);
        case LawKind::Linear: break;
    }
    return s.max > s.min ? (v - s.min) / (s.max - s.min) : 0.0;
}
double fromNorm(const Spec& s, double n)
{
    n = std::clamp(n, 0.0, 1.0);
    switch (s.law)
    {
        case LawKind::Log: return n <= 0 ? s.min : s.logLow * std::pow(s.max / s.logLow, n);
        case LawKind::Gain: return std::pow(n, (double) kGainLaw);
        case LawKind::Linear: break;
    }
    return s.min + n * (s.max - s.min);
}
bool stepped(const Spec& s)
{
    switch (s.unit)
    {
        case Unit::Int: case Unit::Note: case Unit::Choice: case Unit::Toggle: case Unit::Frames: case Unit::Cents: case Unit::Semis:
        case Unit::SemisSigned: case Unit::PlainPct:
            return true;
        case Unit::Ms: case Unit::Gain: case Unit::Db1: case Unit::Db2: case Unit::Percent: case Unit::PercentInt: case Unit::Percent2:
        case Unit::SemisBare: case Unit::Hz: case Unit::Degrees: case Unit::Two: case Unit::Pan:
            break;
    }
    return false;
}

std::string significant(double v) { return ReferenceFace::significant((float) v); }
std::string noteName(int key)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    key = std::clamp(key, 0, 127);
    return std::string(names[key % 12]) + std::to_string(key / 12 - 2);
}

std::string format(const Spec& s, double v)
{
    switch (s.unit)
    {
        case Unit::Ms: { const double ms = clean(v, 0.005); return ms < 999.5 ? significant(ms) + " ms" : significant(ms / 1000) + " s"; }
        case Unit::Gain:
        {
            if (v <= 0.000317) return "-\xE2\x88\x9E dB";
            const double db = clean(20 * std::log10(v), 0.05);
            return printed(db, std::abs(db) >= 9.95 ? "%.0f dB" : "%.1f dB");
        }
        case Unit::Db1: return printed(clean(v, 0.05), "%.1f dB");
        case Unit::Db2: return printed(clean(v, 0.005), "%.2f dB");
        case Unit::Percent: { const double p = clean(v * 100, 0.05); return printed(p, std::abs(p) >= 9.95 ? "%.0f %%" : "%.1f %%"); }
        case Unit::PercentInt: return printed(clean(v * 100, 0.5), "%.0f %%");
        case Unit::Percent2: return printed(clean(v * 100, 0.005), "%.2f %%");
        case Unit::PlainPct: return printed(clean(v, 0.5), "%.0f %%");
        case Unit::Semis: return printed(clean(std::round(v), 0.5), "%.0f st");
        case Unit::SemisSigned: return v > 0.5 ? printed(std::round(v), "+%.0f st") : printed(clean(std::round(v), 0.5), "%.0f st");
        case Unit::SemisBare: return std::abs(v - std::round(v)) < 0.05 ? printed(clean(std::round(v), 0.5), "%.0f") : printed(v, "%.1f");
        case Unit::Cents: return v > 0.5 ? printed(std::round(v), "+%.0f ct") : printed(clean(std::round(v), 0.5), "%.0f ct");
        case Unit::Hz: return v >= 999.5 ? significant(v / 1000) + " kHz" : significant(v) + " Hz";
        case Unit::Degrees: return printed(clean(v, 0.005), "%.2f") + "\xC2\xB0";
        case Unit::Two: return printed(clean(v, 0.005), "%.2f");
        case Unit::Int: return printed(clean(std::round(v), 0.5), "%.0f");
        case Unit::Pan:
        {
            const int amount = (int) std::lround(v * 50);
            return amount == 0 ? "C" : std::to_string(std::abs(amount)) + (amount < 0 ? "L" : "R");
        }
        case Unit::Note: return noteName((int) std::lround(v));
        case Unit::Choice:
        {
            const int i = std::clamp((int) std::lround(v), 0, std::max(0, (int) s.choices.size() - 1));
            return s.choices.empty() ? std::string() : s.choices[(size_t) i];
        }
        case Unit::Toggle: return v > 0.5 ? "On" : "Off";
        case Unit::Frames: return printed(std::round(std::max(0.0, v)), "%.0f");
    }
    return {};
}

// ---------------------------------------------------------------- reference lists
// Modulation destinations (ModConnection/Connection), in the reference's enum order. Index 4 (Loop Length) and 18
// (Volume) are confirmed by the Lovely Keys capture; the order between them follows the reference's own name table.
// TODO(DEV-14b): confirm the rest with a capture of the open chooser.
const std::vector<std::string>& destinations()
{
    static const std::vector<std::string> list{
        "Off", "Sample Selector", "Sample Offset", "Loop Start", "Loop Length", "Release Loop", "Pitch", "Pitch Sample Only",
        "Pitch Osc Only", "Pitch Envelope Amount", "Volume Osc", "Shaper Amt", "Filter Freq", "Filter Res", "Filter Q (Legacy)",
        "Filter Morph", "Filter Drive", "Filter Envelope Amount", "Volume", "Panorama", "Time", "LFO 1 Rate", "LFO 1 Amount Vol",
        "LFO 1 Amount Pan", "LFO 1 Amount Filter", "LFO 1 Amount Pitch", "LFO 2 Rate", "LFO 2 Amount A", "LFO 2 Amount B",
        "LFO 3 Rate", "LFO 3 Amount A", "LFO 3 Amount B"};
    return list;
}
const std::vector<std::string> kLfoShapes{"Sine", "Square", "Triangle", "Saw Down", "Saw Up", "Random"};
const std::vector<std::string> kBeatRates{"1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "5/16", "1/3", "3/8",
                                          "1/2", "3/4", "1", "1.5"};
const std::vector<std::string> kLoopModes{"None", "Loop", "Beat", "Sync", "Trigger"};
const std::vector<std::string> kVoices{"1", "2", "3", "4", "5", "6", "7", "8", "10", "12", "14", "16", "24", "32"};
// TODO(DEV-14b): the Osc type list and the Shaper types are from the reference's manual, not a capture of the chooser.
const std::vector<std::string> kOscTypes{"Sine", "Sine 4 Bit", "Sine 8 Bit", "Saw 3", "Saw 4", "Saw 6", "Saw 8", "Saw 16", "Saw 32",
                                         "Saw 64", "Saw D", "Square 3", "Square 4", "Square 6", "Square 8", "Square 16", "Square 32",
                                         "Square 64", "Square D", "Noise Looped", "Noise White"};
const std::vector<std::string> kShaperTypes{"Soft", "Hard", "Sine", "4 Bit"};

// ---------------------------------------------------------------- painting helpers
void stroke(SkCanvas& c, SkPathBuilder& path, SkColor ink, float width)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(width);
    pen.setStrokeJoin(SkPaint::kRound_Join);
    pen.setStrokeCap(SkPaint::kRound_Cap);
    pen.setColor(ink);
    c.drawPath(path.detach(), pen);
}
void fillPath(SkCanvas& c, SkPathBuilder& path, SkColor ink)
{
    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setColor(ink);
    c.drawPath(path.detach(), fill);
}
// The reference's framed controls: a 2-capture-px dark frame around a fill.
void framedBox(SkCanvas& c, const Theme& t, Rect r, float frame, SkColor fill)
{
    paint::fill(c, r, t.color("ControlContrastFrame"));
    paint::fill(c, r.reduced(frame), fill);
}
SkColor offDot(const Theme& t) { return paint::mix(t.color("ControlForeground"), t.color("SurfaceBackground"), 0.32f); }

void choose(Widget* owner, const std::vector<std::string>& labels, int current, std::function<void(int)> pick)
{
    std::vector<bool> ticks(labels.size());
    if (current >= 0 && (size_t) current < ticks.size()) ticks[(size_t) current] = true;
    auto safe = [owner, chosen = std::move(pick)](int index) { if (Widget::isAlive(owner) && index >= 0) chosen(index); };
    if (owner->ctx().showChoices) { owner->ctx().showChoices(labels, ticks, std::move(safe)); return; }
    juce::PopupMenu menu;
    for (size_t i = 0; i < labels.size(); ++i) menu.addItem((int) i + 1, labels[i], true, ticks[i]);
    menu.showMenuAsync(juce::PopupMenu::Options(), [select = std::move(safe)](int c) { if (c > 0) select(c - 1); });
}

// Slope curves of the envelope stages (devices::Simpler's measured shape): fraction of the stage at progress x.
double slopeCurve(double x, double slope)
{
    return slope >= 0 ? 1.0 - ((1.0 - slope) * (1.0 - x) + slope * std::exp(-9.115 * x))
                      : (1.0 + slope) * x + (-slope) * std::exp(-9.115 * (1.0 - x));
}

// LFO waveform value at phase (cycles).
float lfoWave(int shape, double phase)
{
    const double ph = phase - std::floor(phase);
    switch (shape)
    {
        case 0: return (float) std::sin(2 * M_PI * ph);
        case 1: return ph < 0.5 ? 1.0f : -1.0f;
        case 2: return (float) (1 - 4 * std::abs(ph - 0.5));
        case 3: return (float) (1 - 2 * ph);
        case 4: return (float) (2 * ph - 1);
        default:
        {
            const auto step = (unsigned) (std::floor(phase) + 1000) * 2654435761u;
            return (float) ((step >> 8) & 0xffff) / 32767.5f - 1;
        }
    }
}
} // namespace

// ================================================================ model values (namespace sampler)
namespace sampler
{
namespace
{
const juce::XmlElement* parsed(const std::shared_ptr<const std::string>& xml)
{
    // Parsed device / part XML, cached by the shared string (immutable while any model refers to it).
    static std::map<const std::string*, std::pair<std::weak_ptr<const std::string>, std::unique_ptr<juce::XmlElement>>> cache;
    if (! xml) return nullptr;
    for (auto it = cache.begin(); it != cache.end();) it = it->second.first.expired() ? cache.erase(it) : std::next(it);
    auto& entry = cache[xml.get()];
    if (! entry.second)
    {
        entry.first = xml;
        entry.second = juce::parseXML(juce::String::fromUTF8(xml->c_str()));
    }
    return entry.second.get();
}
const juce::XmlElement* element(const juce::XmlElement* root, const std::string& path)
{
    const auto* e = root;
    for (const auto& name : juce::StringArray::fromTokens(juce::String(path), "/", ""))
    {
        if (e == nullptr) return nullptr;
        e = e->getChildByName(name);
    }
    return e;
}
std::optional<double> leafValue(const juce::XmlElement* e)
{
    if (e == nullptr) return std::nullopt;
    if (const auto* manual = e->getChildByName("Manual")) e = manual;
    if (! e->hasAttribute("Value")) return std::nullopt;
    const auto v = e->getStringAttribute("Value");
    return v.equalsIgnoreCase("true") ? 1.0 : v.equalsIgnoreCase("false") ? 0.0 : v.getDoubleValue();
}
const char* axisTag(Axis axis) { return axis == Axis::Key ? "KeyRange" : axis == Axis::Velocity ? "VelocityRange" : "SelectorRange"; }
} // namespace

double deviceValue(const model::Device& device, const std::string& path, double fallback)
{
    if (const auto it = device.plain.find(path); it != device.plain.end()) return it->second;
    for (const auto& p : device.parameters)
        if (p.path == path) return p.value;
    if (const auto v = leafValue(element(parsed(device.sourceXml), path))) return *v;
    return fallback;
}

namespace
{
// The <MultiSamplePart> values the model holds as fields (DEV-14b, model::SamplePart): read and written there, so the
// engine plays them and the writer saves them. Anything else (only <Selection> today) is read from the part's XML.
struct PartField
{
    const char* path;
    double (*get)(const model::SamplePart&);
    void (*set)(model::SamplePart&, double);
};
int64_t whole(double v) { return (int64_t) std::llround(v); }
int integer(double v) { return (int) std::lround(v); }
const std::vector<PartField>& partFields()
{
    using SP = model::SamplePart;
    static const std::vector<PartField> fields{
        {"KeyRange/Min", [](const SP& p) { return (double) p.keyMin; }, [](SP& p, double v) { p.keyMin = integer(v); }},
        {"KeyRange/Max", [](const SP& p) { return (double) p.keyMax; }, [](SP& p, double v) { p.keyMax = integer(v); }},
        {"KeyRange/CrossfadeMin", [](const SP& p) { return (double) p.keyFadeMin; }, [](SP& p, double v) { p.keyFadeMin = integer(v); }},
        {"KeyRange/CrossfadeMax", [](const SP& p) { return (double) p.keyFadeMax; }, [](SP& p, double v) { p.keyFadeMax = integer(v); }},
        {"VelocityRange/Min", [](const SP& p) { return (double) p.velMin; }, [](SP& p, double v) { p.velMin = integer(v); }},
        {"VelocityRange/Max", [](const SP& p) { return (double) p.velMax; }, [](SP& p, double v) { p.velMax = integer(v); }},
        {"VelocityRange/CrossfadeMin", [](const SP& p) { return (double) p.velFadeMin; }, [](SP& p, double v) { p.velFadeMin = integer(v); }},
        {"VelocityRange/CrossfadeMax", [](const SP& p) { return (double) p.velFadeMax; }, [](SP& p, double v) { p.velFadeMax = integer(v); }},
        {"SelectorRange/Min", [](const SP& p) { return (double) p.selMin; }, [](SP& p, double v) { p.selMin = integer(v); }},
        {"SelectorRange/Max", [](const SP& p) { return (double) p.selMax; }, [](SP& p, double v) { p.selMax = integer(v); }},
        {"SelectorRange/CrossfadeMin", [](const SP& p) { return (double) p.selFadeMin; }, [](SP& p, double v) { p.selFadeMin = integer(v); }},
        {"SelectorRange/CrossfadeMax", [](const SP& p) { return (double) p.selFadeMax; }, [](SP& p, double v) { p.selFadeMax = integer(v); }},
        {"RootKey", [](const SP& p) { return (double) p.rootKey; }, [](SP& p, double v) { p.rootKey = integer(v); }},
        {"Detune", [](const SP& p) { return p.detuneCents; }, [](SP& p, double v) { p.detuneCents = v; }},
        {"TuneScale", [](const SP& p) { return p.tuneScale; }, [](SP& p, double v) { p.tuneScale = v; }},
        {"Panorama", [](const SP& p) { return p.pan; }, [](SP& p, double v) { p.pan = v; }},
        {"Volume", [](const SP& p) { return p.volume; }, [](SP& p, double v) { p.volume = v; }},
        {"Link", [](const SP& p) { return p.link ? 1.0 : 0.0; }, [](SP& p, double v) { p.link = v > 0.5; }},
        {"IsActive", [](const SP& p) { return p.active ? 1.0 : 0.0; }, [](SP& p, double v) { p.active = v > 0.5; }},
        {"Solo", [](const SP& p) { return p.solo ? 1.0 : 0.0; }, [](SP& p, double v) { p.solo = v > 0.5; }},
        {"SampleStart", [](const SP& p) { return (double) p.sampleStart; }, [](SP& p, double v) { p.sampleStart = whole(v); }},
        {"SampleEnd", [](const SP& p) { return (double) (p.sampleEnd > 0 ? p.sampleEnd : p.lengthSamples); }, [](SP& p, double v) { p.sampleEnd = whole(v); }},
        {"SustainLoop/Start", [](const SP& p) { return (double) p.loopStart; }, [](SP& p, double v) { p.loopStart = whole(v); }},
        {"SustainLoop/End", [](const SP& p) { return (double) p.loopEnd; }, [](SP& p, double v) { p.loopEnd = whole(v); }},
        {"SustainLoop/Mode", [](const SP& p) { return (double) p.loopMode; }, [](SP& p, double v) { p.loopMode = integer(v); }},
        {"SustainLoop/Crossfade", [](const SP& p) { return (double) p.loopCrossfade; }, [](SP& p, double v) { p.loopCrossfade = whole(v); }},
        {"SustainLoop/Detune", [](const SP& p) { return p.loopDetune; }, [](SP& p, double v) { p.loopDetune = v; }},
        {"ReleaseLoop/Start", [](const SP& p) { return (double) p.releaseStart; }, [](SP& p, double v) { p.releaseStart = whole(v); }},
        {"ReleaseLoop/End", [](const SP& p) { return (double) p.releaseEnd; }, [](SP& p, double v) { p.releaseEnd = whole(v); }},
        {"ReleaseLoop/Mode", [](const SP& p) { return (double) p.releaseMode; }, [](SP& p, double v) { p.releaseMode = integer(v); }},
        {"ReleaseLoop/Crossfade", [](const SP& p) { return (double) p.releaseCrossfade; }, [](SP& p, double v) { p.releaseCrossfade = whole(v); }},
        {"ReleaseLoop/Detune", [](const SP& p) { return p.releaseDetune; }, [](SP& p, double v) { p.releaseDetune = v; }},
    };
    return fields;
}
const PartField* partField(const std::string& path)
{
    for (const auto& f : partFields())
        if (path == f.path) return &f;
    return nullptr;
}
} // namespace

double partValue(const model::SamplePart& part, const std::string& path, double fallback)
{
    if (const auto* field = partField(path)) return field->get(part);
    if (const auto v = leafValue(element(parsed(part.sourceXml), path))) return *v;
    return fallback;
}

void setPartValue(model::SamplePart& part, const std::string& path, double value)
{
    if (const auto* field = partField(path)) { field->set(part, value); return; }
    // A value the model has no field for: patch a copy of the part's retained XML (the writer keeps it).
    const auto* source = parsed(part.sourceXml);
    if (source == nullptr) return; // a fresh part has nothing else to keep
    auto xml = std::make_unique<juce::XmlElement>(*source);
    auto* e = xml.get();
    for (const auto& name : juce::StringArray::fromTokens(juce::String(path), "/", ""))
    {
        auto* next = e->getChildByName(name);
        e = next != nullptr ? next : e->createNewChildElement(name);
    }
    const auto old = e->getStringAttribute("Value");
    if (old.equalsIgnoreCase("true") || old.equalsIgnoreCase("false")) e->setAttribute("Value", value >= 0.5 ? "true" : "false");
    else if (std::abs(value - std::round(value)) < 1e-9) e->setAttribute("Value", juce::String((int64_t) std::llround(value)));
    else e->setAttribute("Value", juce::String(value, 10));
    part.sourceXml = std::make_shared<const std::string>(xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader()).toStdString());
}

ZoneRange zoneRange(const model::SamplePart& part, Axis axis)
{
    const std::string tag = axisTag(axis);
    ZoneRange r;
    const double low = axis == Axis::Velocity ? 1 : 0;
    r.min = (int) std::lround(partValue(part, tag + "/Min", low));
    r.max = (int) std::lround(partValue(part, tag + "/Max", 127));
    r.fadeMin = std::clamp((int) std::lround(partValue(part, tag + "/CrossfadeMin", r.min)), r.min, r.max);
    r.fadeMax = std::clamp((int) std::lround(partValue(part, tag + "/CrossfadeMax", r.max)), r.fadeMin, r.max);
    return r;
}

void setZoneRange(model::SamplePart& part, Axis axis, ZoneRange r)
{
    const std::string tag = axisTag(axis);
    const int low = axis == Axis::Velocity ? 1 : 0;
    r.min = std::clamp(r.min, low, 127);
    r.max = std::clamp(r.max, r.min, 127);
    r.fadeMin = std::clamp(r.fadeMin, r.min, r.max);
    r.fadeMax = std::clamp(r.fadeMax, r.fadeMin, r.max);
    setPartValue(part, tag + "/Min", r.min);
    setPartValue(part, tag + "/Max", r.max);
    setPartValue(part, tag + "/CrossfadeMin", r.fadeMin);
    setPartValue(part, tag + "/CrossfadeMax", r.fadeMax);
}

int selectedPart(const UiContext& ctx, const model::Device& device)
{
    const int count = (int) device.sampleParts.size();
    if (count == 0) return -1;
    if (const auto it = ctx.samplerViews.find(device.id); it != ctx.samplerViews.end() && it->second.part >= 0)
        return std::min(it->second.part, count - 1);
    for (int i = 0; i < count; ++i)
        if (partValue(device.sampleParts[(size_t) i], "Selection", 0) > 0.5) return i;
    return 0;
}

ZoneTarget zoneEditorTarget(const UiContext& ctx)
{
    ZoneTarget found;
    const auto* track = ctx.trackAt(ctx.selectedTrack);
    if (track == nullptr || ctx.detailTab != DetailTab::Devices || ! ctx.detailVisible) return found;
    const auto open = [](const model::Device& d) {
        return d.type == "MultiSampler" && deviceValue(d, "ViewSettings/ZoneEditorVisible", 0) > 0.5;
    };
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& d = track->devices[i];
        if (open(d)) return {{ctx.selectedTrack, (int) i, -1, -1}, &d};
        if (d.id == ctx.selectedRackId && ctx.selectedRackChain >= 0 && (size_t) ctx.selectedRackChain < d.chains.size())
        {
            const auto& children = d.chains[(size_t) ctx.selectedRackChain].devices;
            for (size_t c = 0; c < children.size(); ++c)
                if (open(children[c])) return {{ctx.selectedTrack, (int) i, ctx.selectedRackChain, (int) c}, &children[c]};
        }
    }
    return found;
}
} // namespace sampler

// ================================================================ the face
namespace
{
struct Param
{
    std::string path;     // device path (empty for a layer value)
    std::string partPath; // path inside the selected MultiSamplePart
    int index = -1;       // the processor's parameter with this id, -1: a model value
    bool leaf = false;    // a plain <X Value=""/> leaf (else a <Manual>)
    bool view = false;    // a ViewSettings leaf: saved without an undo step
    bool ownChoices = false; // the face's names win over the processor's (Interpolation)
    Spec spec;
};

} // namespace

namespace
{
class Face final : public ReferenceFace
{
public:
    Face(Widget& box, const model::Device& model, P* processor, DeviceLocation where, Tag tagger);

    // ---- values
    double get(const Param& p) const
    {
        if (const auto it = pending_.find(&p); it != pending_.end()) return it->second;
        if (p.index >= 0) return processor_->getParameter(p.index);
        if (! p.partPath.empty())
        {
            const int part = selectedPart();
            return part < 0 ? p.spec.def : sampler::partValue(model_.sampleParts[(size_t) part], p.partPath, p.spec.def);
        }
        if (p.view)
            if (const auto it = views_.find(p.path); it != views_.end()) return it->second;
        return sampler::deviceValue(model_, p.path, p.spec.def);
    }
    double norm(const Param& p) const { return std::clamp(toNorm(p.spec, get(p)), 0.0, 1.0); }
    std::string text(const Param& p) const { return format(p.spec, get(p)); }
    // Live value during a gesture; `commit` ends it (model values: one undo step).
    void set(Param& p, double v)
    {
        v = std::clamp(v, p.spec.min, p.spec.max);
        if (stepped(p.spec)) v = std::round(v);
        if (p.index >= 0)
        {
            processor_->setParameter(p.index, (float) v);
            repaint();
            return;
        }
        if (p.view) { views_[p.path] = v; viewChanged(p.path, v); return; }
        pending_[&p] = v;
        repaint();
    }
    void commit(Param& p)
    {
        const auto it = pending_.find(&p);
        if (it == pending_.end()) return;
        const double v = it->second;
        const auto path = p.path, partPath = p.partPath;
        const bool leaf = p.leaf, boolean = p.spec.unit == Unit::Toggle;
        const int part = selectedPart();
        if (! ctx().editDevice) return; // the pending value stays shown (tests without an app)
        pending_.erase(it);
        edit([=](model::Device& d) {
            if (! partPath.empty())
            {
                if (part < 0 || (size_t) part >= d.sampleParts.size()) return;
                sampler::setPartValue(d.sampleParts[(size_t) part], partPath, v);
                d.samplePartsEdited = true;
                return;
            }
            for (auto& q : d.parameters)
                if (q.path == path) { q.value = v; return; }
            if (leaf) { d.plain[path] = v; return; }
            model::Parameter q;
            q.path = path;
            q.value = v;
            q.booleanValue = boolean;
            d.parameters.push_back(std::move(q));
        }, "Change Sampler " + (partPath.empty() ? path.substr(path.find_last_of('/') + 1) : partPath));
    }
    void setNow(Param& p, double v) { set(p, v); commit(p); }
    void discard(const Param& p) { pending_.erase(&p); }

    // ---- view state
    int page() const { return std::clamp((int) std::lround(get(*selectedPage_)), 0, 4); }
    void selectPage(int page) { set(*selectedPage_, page); layout(); repaint(); }
    bool zoneEditor() const { return get(*zoneVisible_) > 0.5; }
    int selectedPart() const { return sampler::selectedPart(ctx(), model_); }
    void selectPart(int part)
    {
        ctx().samplerViews[model_.id].part = part;
        layout();
        repaint();
    }
    void viewChanged(const std::string& key, double v)
    {
        if (ctx().setDeviceViewValue) ctx().setDeviceViewValue(where_.track, where_.device, where_.chain, where_.child, key, v);
        if (auto* root = rootWidget()) root->layout();
        repaint();
    }
    Widget* rootWidget()
    {
        Widget* w = this;
        while (w->parent() != nullptr) w = w->parent();
        return w;
    }
    void edit(const std::function<void(model::Device&)>& operation, const std::string& label)
    {
        if (! ctx().editDevice) return;
        const int chain = where_.chain, child = where_.child;
        ctx().editDevice(where_.track, where_.device, [operation, chain, child](model::Device& root) {
            if (chain < 0) operation(root);
            else if ((size_t) chain < root.chains.size() && child >= 0 && (size_t) child < root.chains[(size_t) chain].devices.size())
            {
                root.chainsEdited = true;
                operation(root.chains[(size_t) chain].devices[(size_t) child]);
            }
        }, label);
    }

    // ---- geometry
    float titleUnit() const { return 18.0f / kTitleHeight; }
    std::function<float()> unitFn() { return [this] { return unit(); }; }
    std::function<float()> sizeFn() { return [this] { return text(); }; }
    Rect local(float x0, float y0, float x1, float y1) const { return at(x0, y0, x1, y1); }
    using ReferenceFace::text;

    const model::Device& model() const { return model_; }
    P* processor() const { return processor_; }
    const engine::SampleData* sample(int part) const
    {
        // devices::Sampler keeps one zone per <MultiSamplePart>, in order.
        const std::vector<devices::Sampler::Zone>* zones = nullptr;
        if (const auto* s = dynamic_cast<const devices::Sampler*>(processor_)) zones = &s->zones();
        else if (const auto* simpler = dynamic_cast<const devices::Simpler*>(processor_)) zones = &simpler->zones();
        if (zones && part >= 0 && (size_t) part < zones->size() && (*zones)[(size_t) part].slot) return (*zones)[(size_t) part].slot->get();
        return nullptr;
    }

    // A sample dropped on the face replaces the selected layer (its zone ranges and root key stay), as the old face did.
    bool browserDrop(const BrowserDrag& payload, const MouseEvent&) override
    {
        if (payload.kind != BrowserDrag::Kind::Sample || ! ctx().dropDeviceItem) return false;
        ctx().dropDeviceItem(payload, where_.track, where_.device, where_.chain, std::max(0, selectedPart()), where_.child);
        return true;
    }

    // ---- painting
    void layout() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void tick() override { if (! sample(selectedPart()) && page() == 0) repaint(); }

    Param* param(const std::string& path, Spec spec, bool leaf = false, bool ownChoices = false);
    Param* partParam(const std::string& path, Spec spec);
    Param* viewParam(const std::string& path, Spec spec);

    // Section switches (the reference keeps the boxes of a switched-off section empty).
    bool on(const Param* p) const { return p == nullptr || get(*p) > 0.5; }

private:
    struct Label { std::string text; float x0, y0, x1, y1; int page; std::function<bool()> shown; Align align; };
    struct Dot { float x, y; int tab; std::function<bool()> lit; };

    void buildTitle(Widget& box);
    void buildSample();
    void buildPitch();
    void buildFilterGlobal();
    void buildModulation();
    void buildMidi();
    struct EnvPaths
    {
        std::string base;
        Spec level;      // Initial / Peak / Sustain / End law
        double initial, peak, sustain, end;
    };
    struct EnvParams
    {
        Param *attack, *decay, *release, *initial, *peak, *sustain, *end, *attackSlope, *decaySlope, *releaseSlope, *loop, *loopTime, *repeat,
            *timeVel, *overlay;
        Spec level;
    };
    EnvParams envelope(const EnvPaths& paths);
    void envelopeControls(const EnvParams& env, float x, float yLabel1, float yBox1, float ySquare, float yBox2, int page, std::function<bool()> active,
                          bool hasEnd, float loopX, int levelKind = 0);
    Widget* box(Param* p, int kind, float x0, float y0, float x1, float y1, int page, std::function<bool()> active = {}, std::function<bool()> shown = {});
    Widget* chooserBox(Param* p, float x0, float y0, float x1, float y1, int page, std::function<bool()> active = {},
                       std::function<void(SkCanvas&, Rect, int, SkColor)> glyph = {}, std::function<bool()> shown = {});
    Widget* button(std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> lit, std::function<void()> click,
                   std::function<bool()> active = {}, std::function<void(SkCanvas&, Rect, SkColor)> glyph = {}, std::function<bool()> shown = {});
    Widget* toggleButton(Param* p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> active = {});
    Widget* optionButton(Param* p, double v, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> active = {},
                         std::function<void(SkCanvas&, Rect, SkColor)> glyph = {});
    void text(std::string s, float x0, float y0, float x1, float y1, int page, std::function<bool()> shown = {}, Align align = Align::Left)
    {
        if (align == Align::Left) x1 = std::max(x1, x0 + 160); // labels never shorten ("Time<Key" touches the next box)
        labels_.push_back({std::move(s), x0, y0, x1, y1, page, std::move(shown), align});
    }
    template <typename W> W* placed(W* widget, float x0, float y0, float x1, float y1, int page, std::function<bool()> shown = {})
    {
        place(widget, x0, y0, x1, y1);
        showWhen(widget, [this, page, shown] { return (page < 0 || this->page() == page) && (! shown || shown()); });
        return widget;
    }
    void tagIf(Widget* w, const Param* p) { if (p != nullptr && p->index >= 0) tag(w, p->index); }
    void paintMidiLogo(SkCanvas&, const Theme&);

    const model::Device& model_;
    DeviceLocation where_;
    std::deque<Param> params_;
    std::map<const Param*, double> pending_;
    std::map<std::string, double> views_;
    std::vector<Label> labels_;
    std::vector<std::tuple<Widget*, float, float, float, float>> titleWidgets_;
    Param *selectedPage_ = nullptr, *zoneVisible_ = nullptr;

public:
    // Section switches, read by displays and title dots.
    Param *oscOn = nullptr, *pitchEnvOn = nullptr, *filterOn = nullptr, *filterEnvOn = nullptr, *shaperOn = nullptr, *auxOn = nullptr,
          *lfoOn[3] = {nullptr, nullptr, nullptr};
    Param *sustainMode = nullptr, *releaseMode = nullptr, *loopStart = nullptr, *loopEnd = nullptr, *loopFade = nullptr, *sampleStart = nullptr,
          *sampleEnd = nullptr, *channel = nullptr, *sampleZoom = nullptr, *releaseStart = nullptr;
    Param *filterType = nullptr, *filterFreq = nullptr, *filterRes = nullptr, *filterSlope = nullptr, *shaperType = nullptr, *shaperAmount = nullptr;
    Param *lfoType[3] = {nullptr, nullptr, nullptr}, *lfoRateType[3] = {nullptr, nullptr, nullptr}, *lfoFreq[3] = {nullptr, nullptr, nullptr},
          *lfoBeat[3] = {nullptr, nullptr, nullptr};
    std::vector<Param*> midiConnections;
};

// ---------------------------------------------------------------- widgets
// A framed number box. Kind 0: slider (cyan fill up to the value), 1: modulation amount (triangle at the right, cyan
// while not neutral), 2: plain. Inactive: the reference's empty box (section off) keeps only the frame.
class NumberBox final : public Widget
{
public:
    NumberBox(Face& f, Param* p, int kind) : f_(f), p_(p), kind_(kind) {}
    std::function<bool()> active, dim;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const auto r = localBounds();
        const float u = f_.unit(), frame = 2 * u;
        const bool live = ! active || active();
        const bool dimmed = live && dim && dim();
        framedBox(c, t, r, frame, ! live || dimmed ? t.color("SurfaceBackground") : t.color("ControlTextBack"));
        const auto inner = r.reduced(frame);
        if (kind_ == 1)
        {
            const bool moved = std::abs(f_.get(*p_) - neutral()) > 1e-6;
            const SkColor ink = live && moved ? t.color("RangeDefault") : dimmed ? t.color("ControlTextBack") : t.color("ControlFillHandle");
            SkPathBuilder tri;
            const float tw = 12 * u, right = inner.right();
            tri.moveTo(right, inner.y + 1 * u).lineTo(right, inner.bottom() - 1 * u).lineTo(right - tw, inner.centreY()).close();
            fillPath(c, tri, ink);
        }
        if (! live) return;
        if (kind_ == 0 && ! dimmed && f_.norm(*p_) > 0)
            paint::fill(c, {inner.x, inner.y, inner.w * (float) f_.norm(*p_), inner.h}, t.color("RangeEditField"));
        const auto s = f_.text(*p_);
        const float size = f_.text();
        paint::text(c, s, {0, 0, std::max(r.w, gfx::measureText(s, size) + 2), r.h}, t.color("ControlForeground"), size, Align::Centre);
    }
    void mouseDown(const MouseEvent& e) override
    {
        pressed_ = ! e.right && (! active || active());
        startY_ = e.y;
        start_ = f_.norm(*p_);
        startValue_ = f_.get(*p_);
        moved_ = false;
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_ || p_->spec.unit == Unit::Toggle) return;
        moved_ = moved_ || std::abs(e.y - startY_) > 2;
        if (p_->spec.unit == Unit::Frames || (stepped(p_->spec) && p_->spec.max - p_->spec.min > 200))
            f_.set(*p_, startValue_ + (double) (startY_ - e.y) * (p_->spec.max - p_->spec.min) / (e.shift ? 20000.0 : 400.0));
        else if (stepped(p_->spec) && p_->spec.max - p_->spec.min <= 200)
            f_.set(*p_, startValue_ + std::round((double) (startY_ - e.y) / (e.shift ? 12.0 : 4.0)));
        else
            f_.set(*p_, fromNorm(p_->spec, start_ + (double) (startY_ - e.y) / (e.shift ? 1000.0 : 200.0)));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (! pressed_) return;
        pressed_ = false;
        if (p_->spec.unit == Unit::Toggle && ! moved_ && localBounds().contains(e.x, e.y)) { f_.setNow(*p_, f_.get(*p_) > 0.5 ? 0 : 1); return; }
        f_.commit(*p_);
    }
    void mouseDoubleClick(const MouseEvent& e) override
    {
        if (e.right || (active && ! active()) || p_->spec.unit == Unit::Toggle) return;
        f_.setNow(*p_, p_->spec.def);
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || (active && ! active()) || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        const double direction = e.wheelDeltaY > 0 ? 1.0 : -1.0;
        if (stepped(p_->spec) && p_->spec.unit != Unit::Frames) f_.setNow(*p_, f_.get(*p_) + direction);
        else f_.setNow(*p_, fromNorm(p_->spec, f_.norm(*p_) + direction * (e.shift ? 0.002 : 0.02)));
        return true;
    }

private:
    double neutral() const { return p_->spec.def; } // the triangle turns cyan once the amount leaves its default
    Face& f_;
    Param* p_;
    int kind_;
    float startY_ = 0;
    double start_ = 0, startValue_ = 0;
    bool pressed_ = false, moved_ = false;
};

// The light drop-down field (text 6 px in, a 13 x 9 arrow 11 px from the right edge); empty while inactive.
class ChoiceBox final : public Widget
{
public:
    ChoiceBox(Face& f, Param* p) : f_(f), p_(p) {}
    std::function<bool()> active;
    std::function<void(SkCanvas&, Rect, int, SkColor)> glyph;
    std::function<std::vector<std::string>()> labels; // a chooser not bound to a parameter (the Sample layer)
    std::function<int()> current;
    std::function<void(int)> pick;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit();
        const bool live = ! active || active();
        framedBox(c, t, localBounds(), 2 * u, live ? t.color("ControlTextBack") : t.color("SurfaceBackground"));
        const auto ink = t.color("ControlForeground");
        if (live)
        {
            const int v = value();
            const auto list = choices();
            if (glyph) glyph(c, {6 * u, height() * 0.5f - 8 * u, 32 * u, 16 * u}, v, ink);
            else if (v >= 0 && (size_t) v < list.size())
            {
                // The reference clips a long choice under the arrow instead of shortening it ("Sound Objects-F3 G").
                c.save();
                c.clipRect(SkRect::MakeXYWH(2 * u, 0, width() - 22 * u, height()));
                paint::text(c, list[(size_t) v], {5 * u, 0, gfx::measureText(list[(size_t) v], f_.text()) + 10, height()}, ink, f_.text());
                c.restore();
            }
        }
        ctl::triangle(c, width() - 11 * u, height() * 0.5f, 13 * u, 9 * u, ctl::Dir::Down, ink);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y) || (active && ! active())) return;
        choose(this, choices(), value(), [this](int v) {
            if (pick) pick(v);
            else f_.setNow(*p_, v);
            repaint();
        });
    }
    bool mouseWheel(const MouseEvent& e) override
    {
        if (e.right || p_ == nullptr || (active && ! active()) || std::abs(e.wheelDeltaX) > std::abs(e.wheelDeltaY)) return false;
        f_.setNow(*p_, std::round(f_.get(*p_)) + (e.wheelDeltaY > 0 ? -1.0 : 1.0));
        return true;
    }

private:
    std::vector<std::string> choices() const { return labels ? labels() : p_ != nullptr ? p_->spec.choices : std::vector<std::string>{}; }
    int value() const { return current ? current() : p_ != nullptr ? (int) std::lround(f_.get(*p_)) : 0; }
    Face& f_;
    Param* p_;
};

// Framed text / glyph button: amber while lit. Inactive (section off): the lit one turns white and the others grey.
class Button final : public Widget
{
public:
    Button(Face& f, std::string label) : f_(f), label_(std::move(label)) {}
    std::function<bool()> lit, active;
    std::function<void()> click;
    std::function<void(SkCanvas&, Rect, SkColor)> glyph;
    SkColor litColour = 0; // 0: the theme's ChosenDefault
    bool quietOff = false;  // B / M / L / R: unlit keys are grey with grey text
    bool bare = false;      // an icon without a frame (hot-swap)
    float textScale = 1;    // OFF: small bold letters
    bool blankInactive = false; // its value lives in a switched-off section: drawn unlit while inactive
    bool bold = false;
    void paint(SkCanvas& c, const Theme& t) override
    {
        const bool live = ! active || active(), on = lit && lit() && (live || ! blankInactive);
        SkColor fill = t.color("ControlTextBack"), ink = t.color("ControlForeground");
        if (on && live) fill = litColour != 0 ? litColour : t.color("ChosenDefault");
        else if (on) fill = t.color("ControlDisabled");
        else if (! live || quietOff) { fill = t.color("ControlFillHandle"); ink = paint::mix(t.color("TextDisabled"), fill, 0.25f); }
        if (! bare) framedBox(c, t, localBounds(), 2 * f_.unit(), fill);
        if (glyph) glyph(c, localBounds(), ink);
        else paint::text(c, label_, localBounds(), ink, f_.text() * textScale, Align::Centre, bold);
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y) || ! click || (active && ! active())) return;
        click();
        if (parent()) parent()->layout();
        repaint();
    }

private:
    Face& f_;
    std::string label_;
};

// The small square before "Attack": shows the stage slopes in the time row (CurrentOverlay).
class OverlaySquare final : public Widget
{
public:
    OverlaySquare(Face& f, Param* p) : f_(f), p_(p) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit();
        const Rect r{0, (height() - 10 * u) * 0.5f, 10 * u, 10 * u};
        framedBox(c, t, r, 1.6f * u, f_.get(*p_) > 0.5 ? t.color("ChosenDefault") : t.color("ControlTextBack"));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        f_.setNow(*p_, f_.get(*p_) > 0.5 ? 0 : 1);
        if (parent()) parent()->layout();
    }

private:
    Face& f_;
    Param* p_;
};

// An envelope display: initial level, attack to the peak, decay to the sustain level, a sustain segment, release to
// the end level. Amber squares at the stage ends, pink diamonds at the curves' midpoints (slopes); drag any of them.
class EnvelopeDisplay final : public Widget
{
public:
    enum class Levels { Gain, Unipolar, Bipolar };
    EnvelopeDisplay(Face& f, std::array<Param*, 10> p, Levels levels, std::function<bool()> active)
        : f_(f), p_(p), levels_(levels), active_(std::move(active)) {}
    // p: attack, decay, release, initial, peak, sustain, end, attack slope, decay slope, release slope
    enum Handle { None = -1, Initial, AttackEnd, DecayEnd, ReleaseEnd, AttackMid, DecayMid, ReleaseMid };
    void paint(SkCanvas& c, const Theme& t) override
    {
        paint::fillRounded(c, localBounds(), 4 * f_.unit(), t.color("RetroDisplayBackground"));
        if (! active_()) return;
        const auto g = geometry();
        const float u = f_.unit();
        SkPathBuilder path;
        path.moveTo(g.x[0] * u, g.y[0] * u);
        for (int stage = 0; stage < 4; ++stage)
        {
            const float x0 = g.x[stage], x1 = g.x[stage + 1], y0 = g.y[stage], y1 = g.y[stage + 1];
            const double slope = stage == 2 ? 0.0 : f_.get(*p_[(size_t) (stage == 3 ? 9 : 7 + stage)]);
            for (int i = 1; i <= 24; ++i)
            {
                const double x = i / 24.0, k = stage == 2 ? x : slopeCurve(x, slope);
                path.lineTo((x0 + (x1 - x0) * (float) x) * u, (y0 + (y1 - y0) * (float) k) * u);
            }
        }
        stroke(c, path, t.color("RangeDefault"), 3 * u);
        const auto pink = SkColorSetRGB(0xe8, 0x7d, 0x83);
        for (int m = 0; m < 3; ++m)
        {
            const auto [mx, my] = mid(g, m);
            SkPathBuilder diamond;
            const float r = 5 * u;
            diamond.moveTo(mx * u, my * u - r).lineTo(mx * u + r, my * u).lineTo(mx * u, my * u + r).lineTo(mx * u - r, my * u).close();
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(2 * u);
            pen.setColor(pink);
            c.drawPath(diamond.detach(), pen);
        }
        for (int h = 0; h < 5; ++h)
        {
            if (h == 3) continue; // the sustain end has no handle
            const Rect r{(g.x[h] - 8) * u, (g.y[h] - 8) * u, 16 * u, 16 * u};
            paint::stroke(c, r.reduced(1 * u), t.color("ChosenDefault"), 2 * u);
        }
    }
    void mouseDown(const MouseEvent& e) override
    {
        drag_ = None;
        if (e.right || ! active_()) return;
        const auto g = geometry();
        const float u = f_.unit(), x = e.x / u, y = e.y / u;
        float best = 14;
        const auto consider = [&](Handle h, float hx, float hy) {
            const float d = std::hypot(x - hx, y - hy);
            if (d < best) { best = d; drag_ = h; }
        };
        consider(Initial, g.x[0], g.y[0]);
        consider(AttackEnd, g.x[1], g.y[1]);
        consider(DecayEnd, g.x[2], g.y[2]);
        consider(ReleaseEnd, g.x[4], g.y[4]);
        for (int m = 0; m < 3; ++m) { const auto [mx, my] = mid(g, m); consider((Handle) (AttackMid + m), mx, my); }
        startX_ = x;
        startY_ = y;
        for (size_t i = 0; i < p_.size(); ++i) start_[i] = f_.get(*p_[i]);
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (drag_ == None) return;
        const float u = f_.unit(), x = e.x / u, y = e.y / u;
        const auto g = geometry();
        const auto time = [&](int index, float from) {
            f_.set(*p_[(size_t) index], fromNorm(p_[(size_t) index]->spec, std::max(0.0f, x - from) / kStagePx));
        };
        switch (drag_)
        {
            case Initial: f_.set(*p_[3], levelAt(y)); break;
            case AttackEnd: time(0, g.x[0]); f_.set(*p_[4], levelAt(y)); break;
            case DecayEnd: time(1, g.x[1]); f_.set(*p_[5], levelAt(y)); break;
            case ReleaseEnd: time(2, g.x[3]); f_.set(*p_[6], levelAt(y)); break;
            case AttackMid: case DecayMid: case ReleaseMid:
            {
                const auto index = (size_t) (7 + (drag_ - AttackMid));
                const double rising = drag_ == AttackMid ? 1 : -1;
                f_.set(*p_[index], std::clamp(start_[index] + rising * (double) (startY_ - y) / 60.0, -1.0, 1.0));
                break;
            }
            case None: break;
        }
        repaint();
    }
    void mouseUp(const MouseEvent&) override
    {
        if (drag_ == None) return;
        drag_ = None;
        // One undo step for what the gesture changed; values it only re-set stay as they were.
        for (size_t i = 0; i < p_.size(); ++i)
            if (std::abs(f_.get(*p_[i]) - start_[i]) > 1e-9) f_.commit(*p_[i]);
            else f_.discard(*p_[i]);
    }
    // For tests: a handle's centre in local coordinates.
    std::pair<float, float> handle(Handle h) const
    {
        const auto g = geometry();
        const float u = f_.unit();
        if (h >= AttackMid) { const auto [mx, my] = mid(g, h - AttackMid); return {mx * u, my * u}; }
        const int i = h == ReleaseEnd ? 4 : (int) h;
        return {g.x[i] * u, g.y[i] * u};
    }

private:
    struct Geometry { float x[5], y[5]; };
    float h() const { return height() / f_.unit(); }
    float top() const { return 16; }
    float bottom() const { return h() * 0.88f; }
    float levelY(double v) const
    {
        switch (levels_)
        {
            case Levels::Gain: return top() + (bottom() - top()) * (1 - (float) std::pow(std::clamp(v, 0.0, 1.0), 1.0 / kGainLaw));
            case Levels::Unipolar: return top() + (bottom() - top()) * (1 - (float) std::clamp(v, 0.0, 1.0));
            case Levels::Bipolar: break;
        }
        const float middle = (top() + bottom()) * 0.5f;
        return middle - (middle - top()) * (float) std::clamp(v, -1.0, 1.0);
    }
    double levelAt(float y) const
    {
        const double f = std::clamp((double) ((y - top()) / (bottom() - top())), 0.0, 1.0);
        switch (levels_)
        {
            case Levels::Gain: return std::pow(1 - f, (double) kGainLaw);
            case Levels::Unipolar: return 1 - f;
            case Levels::Bipolar: break;
        }
        return 1 - 2 * f;
    }
    Geometry geometry() const
    {
        Geometry g{};
        const auto stage = [&](int i) { return kStagePx * (float) f_.norm(*p_[(size_t) i]); };
        g.x[0] = 16;
        g.x[1] = g.x[0] + stage(0);
        g.x[2] = g.x[1] + stage(1);
        g.x[3] = std::max(kSustainEnd, g.x[2] + kMinSustain);
        g.x[4] = g.x[3] + stage(2);
        g.y[0] = levelY(f_.get(*p_[3]));
        g.y[1] = levelY(f_.get(*p_[4]));
        g.y[2] = g.y[3] = levelY(f_.get(*p_[5]));
        g.y[4] = levelY(f_.get(*p_[6]));
        return g;
    }
    std::pair<float, float> mid(const Geometry& g, int m) const
    {
        const int from = m == 2 ? 3 : m;
        const double slope = f_.get(*p_[(size_t) (7 + m)]), k = slopeCurve(0.5, slope);
        return {(g.x[from] + g.x[from + 1]) * 0.5f, g.y[from] + (g.y[from + 1] - g.y[from]) * (float) k};
    }
    Face& f_;
    std::array<Param*, 10> p_;
    Levels levels_;
    std::function<bool()> active_;
    Handle drag_ = None;
    float startX_ = 0, startY_ = 0;
    std::array<double, 10> start_{};
};

// The filter display: the log frequency grid (30 Hz..23.7 kHz, 96 px per decade, brighter decades) with 100 / 1k / 10k,
// the response of the type and slope, and the amber cutoff / resonance handle. Drag anywhere: x cutoff, y resonance.
class FilterDisplay final : public Widget
{
public:
    explicit FilterDisplay(Face& f) : f_(f) {}
    static float xOf(double hz) { return 146 + 96 * (float) std::log10(hz / 1000.0); } // display px (capture x - 128)
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit(), w = width() / u, hgt = height() / u;
        paint::fillRounded(c, localBounds(), 4 * u, t.color("RetroDisplayBackground"));
        for (double decade = 10; decade <= 10000; decade *= 10)
            for (int k = 1; k <= 9; ++k)
            {
                const float x = xOf(decade * k);
                if (x < 0 || x > w) continue;
                paint::fill(c, {(x - 1) * u, 0, 2 * u, hgt * u}, k == 1 ? SkColorSetRGB(0x31, 0x33, 0x3c) : SkColorSetRGB(0x26, 0x28, 0x31));
            }
        for (int k = 0; k < 7; ++k) // every 18 px from 14; the 0 dB line (68) is brighter
            paint::fill(c, {0, (14 + 18.0f * (float) k) * u, w * u, 2 * u}, k == 3 ? SkColorSetRGB(0x31, 0x33, 0x3c) : SkColorSetRGB(0x1d, 0x20, 0x28));
        const auto scale = SkColorSetRGB(0x55, 0x57, 0x5d);
        const char* names[] = {"100", "1k", "10k"};
        for (int i = 0; i < 3; ++i)
        {
            const float x = xOf(100 * std::pow(10.0, i));
            paint::text(c, names[i], {(x - 40) * u, (hgt - 32) * u, 80 * u, 28 * u}, scale, f_.text() * 1.05f, Align::Centre, true);
        }
        if (! f_.on(f_.filterOn)) return;
        const double freq = f_.get(*f_.filterFreq), res = f_.get(*f_.filterRes);
        const int type = (int) std::lround(f_.get(*f_.filterType));
        const bool steep = f_.get(*f_.filterSlope) > 0.5;
        const double q = 0.707 + res * 6;
        // The reference keeps the handle (and the cutoff it draws) inside the display: 22 kHz sits 13 px from the edge.
        const float cutoffX = std::min(xOf(freq), w - 13);
        SkPathBuilder path;
        for (float x = 0; x <= w; x += 1)
        {
            // The display shows the digital filter at 44.1 kHz (bilinear): it falls away towards Nyquist, as the reference's does.
            const double hz = 1000 * std::pow(10.0, (x - 146) / 96.0), fc = 1000 * std::pow(10.0, (cutoffX - 146) / 96.0);
            const double r = hz >= 22049 ? 1e3 : std::tan(M_PI * hz / 44100.0) / std::tan(M_PI * std::min(fc, 21000.0) / 44100.0);
            const double denom = std::sqrt((1 - r * r) * (1 - r * r) + (r / q) * (r / q));
            const double plain = std::sqrt((1 - r * r) * (1 - r * r) + (r / 0.707) * (r / 0.707)); // the 24 dB slope's second stage
            double mag = 1;
            switch (type)
            {
                case 1: mag = r * r / denom * (steep ? r * r / plain : 1); break;
                case 2: mag = (r / q) / denom; break;
                case 3: mag = std::abs(1 - r * r) / denom; break;
                default: mag = 1 / denom * (steep ? 1 / plain : 1); break;
            }
            const double db = 20 * std::log10(std::max(mag, 1e-6));
            const float y = std::clamp(68 - (float) db * 2.2f, 4.0f, hgt + 10);
            if (x <= 0) path.moveTo(x * u, y * u); else path.lineTo(x * u, y * u);
        }
        c.save();
        c.clipRect(localBounds().sk());
        stroke(c, path, t.color("RangeDefault"), 3 * u);
        SkPaint ring;
        ring.setAntiAlias(true);
        ring.setStyle(SkPaint::kStroke_Style);
        ring.setStrokeWidth(3 * u);
        ring.setColor(t.color("ChosenDefault"));
        c.drawCircle(cutoffX * u, handleY(res) * u, 10 * u, ring);
        c.restore();
    }
    float handleY(double res) const { return (height() / f_.unit() - 12) - (float) (res / 1.25) * (height() / f_.unit() - 30); }
    void mouseDown(const MouseEvent& e) override { dragging_ = ! e.right && f_.on(f_.filterOn); mouseDrag(e); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! dragging_) return;
        const float u = f_.unit(), hgt = height() / u;
        f_.set(*f_.filterFreq, 1000 * std::pow(10.0, (e.x / u - 146) / 96.0));
        f_.set(*f_.filterRes, std::clamp((double) ((hgt - 12 - e.y / u) / (hgt - 30)), 0.0, 1.0) * 1.25);
        repaint();
    }
    void mouseUp(const MouseEvent&) override
    {
        if (! dragging_) return;
        dragging_ = false;
        f_.commit(*f_.filterFreq);
        f_.commit(*f_.filterRes);
    }

private:
    Face& f_;
    bool dragging_ = false;
};

// An LFO display: three seconds of the waveform (Hz or synced at the song tempo); the stereo LFOs mark L / R.
class LfoDisplay final : public Widget
{
public:
    LfoDisplay(Face& f, int lfo) : f_(f), lfo_(lfo) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit();
        paint::fillRounded(c, localBounds(), 4 * u, t.color("RetroDisplayBackground"));
        if (! f_.on(f_.lfoOn[lfo_])) return;
        double hz = f_.get(*f_.lfoFreq[lfo_]);
        if (f_.get(*f_.lfoRateType[lfo_]) > 0.5)
        {
            const int i = std::max(0, (int) std::lround(f_.get(*f_.lfoBeat[lfo_])));
            const double tempo = ctx().song != nullptr && ctx().song->tempo > 0 ? ctx().song->tempo : 120.0;
            hz = tempo / 60.0 / std::max(1e-3, devices::Sampler::lfoBeatQuarters(i));
        }
        const int shape = (int) std::lround(f_.get(*f_.lfoType[lfo_]));
        const float w = width() / u, hgt = height() / u;
        SkPathBuilder path;
        for (float x = 6; x <= w - 6; x += 1)
        {
            const float y = hgt * 0.5f - lfoWave(shape, hz * 3.0 * (x - 6) / (w - 12)) * (hgt * 0.5f - 12);
            if (x <= 6) path.moveTo(x * u, y * u); else path.lineTo(x * u, y * u);
        }
        stroke(c, path, t.color("RangeDefault"), 3 * u);
        if (lfo_ > 0)
        {
            paint::text(c, "L", {8 * u, hgt * u - 26 * u, 20 * u, 22 * u}, t.color("RangeDefault"), f_.text(), Align::Left, true);
            paint::text(c, "R", {w * u - 26 * u, hgt * u - 26 * u, 20 * u, 22 * u}, t.color("ChosenDefault"), f_.text(), Align::Right, true);
        }
    }

private:
    Face& f_;
    int lfo_;
};

// The Shaper's transfer curve.
class ShaperDisplay final : public Widget
{
public:
    explicit ShaperDisplay(Face& f) : f_(f) {}
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit();
        paint::fillRounded(c, localBounds(), 4 * u, t.color("RetroDisplayBackground"));
        if (! f_.on(f_.shaperOn)) return;
        const int type = (int) std::lround(f_.get(*f_.shaperType));
        const double drive = 1 + 3 * std::max(0.0, f_.get(*f_.shaperAmount));
        const float w = width() / u, hgt = height() / u;
        SkPathBuilder path;
        for (float x = 6; x <= w - 6; x += 1)
        {
            const double in = (x - w * 0.5) / (w * 0.5 - 12) * 1.6 * drive;
            double out = 0;
            switch (type)
            {
                case 1: out = std::clamp(in, -1.0, 1.0); break;
                case 2: out = std::sin(std::clamp(in, -M_PI / 2, M_PI / 2)); break;
                case 3: out = std::round(std::clamp(in, -1.0, 1.0) * 4) / 4; break;
                default: out = std::tanh(in); break;
            }
            const float y = hgt * 0.5f - (float) out * (hgt * 0.5f - 10);
            if (x <= 6) path.moveTo(x * u, y * u); else path.lineTo(x * u, y * u);
        }
        stroke(c, path, t.color("RangeDefault"), 3 * u);
    }

private:
    Face& f_;
};

// The sample display of the Sample tab: the selected layer's waveform (B: both channels stacked, M: their sum, L, R),
// the seconds ruler, the Sample Start / End flags, the sustain loop (teal, crossfade ramps, the loop bar with its flags).
// Drag a flag to move its point.
class WaveDisplay final : public Widget
{
public:
    explicit WaveDisplay(Face& f) : f_(f) {}
    enum Flag { None = -1, Start, End, LoopStart, LoopEnd };
    float frameX(double frame, double length) const { const float u = f_.unit(); return (4 + (float) (frame / std::max(1.0, length)) * (width() / u - 8)) * u; }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit(), w = width(), hgt = height();
        paint::fill(c, localBounds(), SkColorSetRGB(0x58, 0x5b, 0x66));
        paint::fill(c, localBounds().reduced(2 * u), t.color("RetroDisplayBackground"));
        const int part = f_.selectedPart();
        const auto* data = f_.sample(part);
        if (part < 0) return;
        const auto& model = f_.model().sampleParts[(size_t) part];
        const double length = data ? (double) data->audio.getNumSamples() : (double) std::max<int64_t>(1, model.lengthSamples);
        const double rate = data ? data->sampleRate : model.sampleRate > 0 ? model.sampleRate : 44100.0;
        const auto inner = localBounds().reduced(2 * u);
        c.save();
        c.clipRect(inner.sk());
        // Loop region first (the waveform draws over it).
        const int mode = (int) std::lround(f_.get(*f_.sustainMode));
        if (mode > 0)
        {
            const float x0 = frameX(f_.get(*f_.loopStart), length), x1 = frameX(f_.get(*f_.loopEnd), length);
            const float fade = (float) (f_.get(*f_.loopFade) / length) * (w - 8 * u);
            const auto teal = t.color("SampleDisplayBackground");
            paint::fill(c, {x0, inner.y, x1 - x0, inner.h}, teal);
            const auto ramp = paint::mix(teal, t.color("RetroDisplayBackground"), 0.45f);
            SkPathBuilder left, right;
            left.moveTo(x0, inner.y).lineTo(x0, inner.bottom()).lineTo(x0 - fade, inner.bottom()).close();
            fillPath(c, left, ramp);
            right.moveTo(x1, inner.y).lineTo(x1, inner.bottom()).lineTo(x1 - fade, inner.y).close();
            fillPath(c, right, ramp);
            // Loop bar: a light bracket with flags.
            const auto light = t.color("ControlFillHandle");
            paint::fill(c, {x0, inner.y, x1 - x0, 2 * u}, light);
            paint::fill(c, {x0, inner.y + 20 * u, x1 - x0, 2 * u}, light);
            ctl::triangle(c, x0 + 9 * u, inner.y + 11 * u, 18 * u, 16 * u, ctl::Dir::Right, light);
            ctl::triangle(c, x1 - 9 * u, inner.y + 11 * u, 18 * u, 16 * u, ctl::Dir::Left, light);
            paint::fill(c, {x0 - 1 * u, inner.y, 2 * u, inner.h}, SkColorSetRGB(0x0a, 0x0c, 0x15));
            paint::fill(c, {x1 - 1 * u, inner.y, 2 * u, inner.h}, SkColorSetRGB(0x0a, 0x0c, 0x15));
        }
        // Ruler: seconds at the bottom, a label every two seconds and a tick every half second.
        const auto scale = SkColorSetRGB(0x55, 0x57, 0x5d);
        const double seconds = length / rate;
        const double labelStep = seconds > 40 ? 10 : seconds > 16 ? 4 : 2;
        for (double s = 0; s <= seconds; s += labelStep / 4)
        {
            const float x = frameX(s * rate, length);
            const bool major = std::fmod(s + 1e-9, labelStep) < 1e-6;
            paint::fill(c, {x - 1 * u, hgt - (major ? 30 : 12) * u, 2 * u, (major ? 28 : 10) * u}, scale);
            if (major)
            {
                const int whole = (int) std::lround(s);
                paint::text(c, std::to_string(whole / 60) + ":" + printed(whole % 60, "%02.0f"), {x + 4 * u, hgt - 38 * u, 90 * u, 30 * u}, scale,
                            f_.text() * 1.12f);
            }
        }
        // Waveform.
        if (data != nullptr)
        {
            if (peaksFor_ != data)
            {
                peaksFor_ = data;
                if (ctx().peaks)
                {
                    model::Clip clip;
                    clip.kind = model::Clip::Kind::Audio;
                    clip.sample = model.sample;
                    peaks_ = ctx().peaks(clip);
                }
                if (! peaks_) peaks_ = engine::computePeaks(data->audio, data->sampleRate);
            }
            const int view = (int) std::lround(f_.get(*f_.channel));
            // The reference scales the drawing by the layer's volume (Vol).
            const double zoom = std::max(1.0, f_.get(*f_.sampleZoom)) * std::clamp(model.volume, 0.0, 2.0);
            const int lanes = view == 0 ? 2 : 1;
            const float laneH = inner.h / (float) lanes;
            const auto amber = t.color("RetroDisplayForeground");
            for (int lane = 0; lane < lanes; ++lane)
            {
                const int channel = view == 0 ? lane : view == 3 ? 1 : 0;
                const float centre = inner.y + laneH * ((float) lane + 0.5f), amp = laneH * 0.46f * (float) zoom;
                SkPathBuilder shape;
                std::vector<std::pair<float, float>> lows;
                for (float x = inner.x; x <= inner.right(); x += 1)
                {
                    const double f0 = (x / u - 4) / (w / u - 8) * length, f1 = f0 + length / (double) (w / u - 8);
                    float lo = 0, hi = 0;
                    if (peaks_) peaks_->range(std::max(0.0, f0) / rate, std::max(0.0, f1) / rate, lo, hi, channel);
                    if (view == 1 && peaks_)
                    {
                        float lo2 = 0, hi2 = 0;
                        peaks_->range(std::max(0.0, f0) / rate, std::max(0.0, f1) / rate, lo2, hi2, 1);
                        lo = (lo + lo2) * 0.5f;
                        hi = (hi + hi2) * 0.5f;
                    }
                    const float top = centre - std::max(hi * amp, 1.5f * u), bottom = centre - std::min(lo * amp, -1.5f * u);
                    if (x <= inner.x) shape.moveTo(x, top); else shape.lineTo(x, top);
                    lows.push_back({x, bottom});
                }
                for (auto it = lows.rbegin(); it != lows.rend(); ++it) shape.lineTo(it->first, it->second);
                shape.close();
                fillPath(c, shape, amber);
            }
        }
        // Sample Start / End flags.
        const auto flag = t.color("ControlFillHandle");
        const float xs = frameX(f_.get(*f_.sampleStart), length), xe = frameX(f_.get(*f_.sampleEnd), length);
        paint::fill(c, {xs - 1 * u, inner.y, 2 * u, inner.h}, flag);
        ctl::triangle(c, xs + 9 * u, inner.y + 30 * u, 18 * u, 20 * u, ctl::Dir::Right, flag);
        paint::fill(c, {xe - 1 * u, inner.y, 2 * u, inner.h}, flag);
        ctl::triangle(c, xe - 9 * u, inner.y + 30 * u, 18 * u, 20 * u, ctl::Dir::Left, flag);
        c.restore();
    }
    void mouseDown(const MouseEvent& e) override
    {
        drag_ = None;
        const int part = f_.selectedPart();
        if (e.right || part < 0) return;
        const float u = f_.unit();
        const double length = this->length();
        const auto near = [&](Param* p, float slack) { return std::abs(e.x - frameX(f_.get(*p), length)) < slack * u; };
        const bool loop = f_.get(*f_.sustainMode) > 0.5;
        if (loop && e.y < 26 * u && near(f_.loopStart, 22)) drag_ = LoopStart;
        else if (loop && e.y < 26 * u && near(f_.loopEnd, 22)) drag_ = LoopEnd;
        else if (near(f_.sampleStart, 20)) drag_ = Start;
        else if (near(f_.sampleEnd, 20)) drag_ = End;
    }
    void mouseDrag(const MouseEvent& e) override
    {
        if (drag_ == None) return;
        const float u = f_.unit();
        const double frame = std::clamp((double) ((e.x / u - 4) / (width() / u - 8)), 0.0, 1.0) * length();
        f_.set(*target(), std::round(frame));
        repaint();
    }
    void mouseUp(const MouseEvent&) override
    {
        if (drag_ == None) return;
        f_.commit(*target());
        drag_ = None;
    }

private:
    Param* target() const { return drag_ == Start ? f_.sampleStart : drag_ == End ? f_.sampleEnd : drag_ == LoopStart ? f_.loopStart : f_.loopEnd; }
    double length() const
    {
        const int part = f_.selectedPart();
        if (const auto* data = f_.sample(part)) return data->audio.getNumSamples();
        return part < 0 ? 1.0 : (double) std::max<int64_t>(1, f_.model().sampleParts[(size_t) part].lengthSamples);
    }
    Face& f_;
    Flag drag_ = None;
    const engine::SampleData* peaksFor_ = nullptr;
    std::shared_ptr<const engine::Peaks> peaks_;
};

// The vertical zoom slider right of the sample display: a black track with an outlined pointer (VerticalSampleZoom).
class ZoomSlider final : public Widget
{
public:
    explicit ZoomSlider(Face& f) : f_(f) {}
    float position() const { return 1 - (float) std::clamp(std::log(std::max(1.0, f_.get(*f_.sampleZoom))) / std::log(16.0), 0.0, 1.0); }
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float u = f_.unit();
        paint::fill(c, {0, 0, 4 * u, 72 * u}, t.color("ControlForeground"));
        const float y = position() * 72 * u;
        SkPathBuilder tri;
        tri.moveTo(4 * u, y).lineTo(24 * u, y - 10 * u).lineTo(24 * u, y + 10 * u).close();
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(2.5f * u);
        pen.setColor(t.color("ControlContrastFrame"));
        SkPaint fill;
        fill.setAntiAlias(true);
        fill.setColor(t.color("ControlTextBack"));
        const auto path = tri.detach();
        c.drawPath(path, fill);
        c.drawPath(path, pen);
    }
    void mouseDown(const MouseEvent& e) override { pressed_ = ! e.right; mouseDrag(e); }
    void mouseDrag(const MouseEvent& e) override
    {
        if (! pressed_) return;
        const double n = 1 - std::clamp((double) (e.y / (72 * f_.unit())), 0.0, 1.0);
        f_.set(*f_.sampleZoom, std::pow(16.0, n));
    }
    void mouseUp(const MouseEvent&) override { pressed_ = false; }

private:
    Face& f_;
    bool pressed_ = false;
};

// A tab of the device title (in the box's title band): unselected tabs are panel grey boxes, the shown one runs down
// into the body. Dots before / after the name mark the sections in use (amber on, dark grey off).
class TitleTab final : public Widget
{
public:
    std::string label;
    std::function<bool()> selected;
    std::function<void()> onSelect;
    std::function<float()> unit;
    std::vector<std::pair<float, std::function<bool()>>> dots; // x in capture px from the tab's left edge
    float textX = 0;                                           // capture px from the left; 0 centres the name
    std::function<int()> arrow;                                // Zone: 0 none, 1 left (closed), 2 up (open)
    void paint(SkCanvas& c, const Theme& t) override
    {
        const float k = unit(), tab = 26 * k;
        const auto fill = t.color("SurfaceBackground");
        if (selected && selected())
        {
            paint::fillRounded(c, {0, 0, width(), height()}, 4 * k, fill);
            paint::fill(c, {0, height() - 6 * k, width(), 6 * k}, fill);
        }
        else paint::fillRounded(c, {0, 0, width(), tab}, 4 * k, fill);
        SkPaint dot;
        dot.setAntiAlias(true);
        for (const auto& [x, lit] : dots)
        {
            dot.setColor(lit() ? t.color("ChosenDefault") : offDot(t));
            c.drawCircle(x * k, tab * 0.5f, 5 * k, dot);
        }
        const float size = 21.5f * k, w = gfx::measureText(label, size);
        const float x = textX > 0 ? textX * k : (width() - w) * 0.5f;
        paint::text(c, label, {x, 0, w + 4 * k, tab}, t.color("ControlForeground"), size, Align::Left);
        if (arrow && arrow() > 0)
            ctl::triangle(c, x + w + 14 * k, tab * 0.5f, arrow() == 1 ? 12 * k : 15 * k, arrow() == 1 ? 15 * k : 11 * k,
                          arrow() == 1 ? ctl::Dir::Left : ctl::Dir::Up, t.color("ControlForeground"));
    }
    void mouseUp(const MouseEvent& e) override
    {
        if (e.right || ! localBounds().contains(e.x, e.y)) return;
        onSelect();
    }
};

// Glyphs.
void loopGlyph(SkCanvas& c, Rect r, SkColor ink, int kind, float u)
{
    // 0: play to the end (an arrow into a bar), 1: forward loop (two arrows right), 2: back and forth.
    const float cx = r.centreX(), cy = r.centreY(), w = 20 * u, h = 6 * u;
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(2 * u);
    pen.setColor(ink);
    if (kind == 0)
    {
        c.drawLine(cx - w * 0.5f, cy, cx + w * 0.4f, cy, pen);
        c.drawLine(cx + w * 0.5f, cy - 7 * u, cx + w * 0.5f, cy + 7 * u, pen);
        ctl::triangle(c, cx + w * 0.3f, cy, 8 * u, 9 * u, ctl::Dir::Right, ink);
        return;
    }
    c.drawLine(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.35f, cy - h * 0.5f, pen);
    c.drawLine(cx - w * 0.35f, cy + h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, pen);
    ctl::triangle(c, cx + w * 0.4f, cy - h * 0.5f, 7 * u, 8 * u, ctl::Dir::Right, ink);
    if (kind == 1) ctl::triangle(c, cx + w * 0.45f, cy + h * 0.5f, 7 * u, 8 * u, ctl::Dir::Right, ink);
    else ctl::triangle(c, cx - w * 0.45f, cy + h * 0.5f, 7 * u, 8 * u, ctl::Dir::Left, ink);
}
void filterGlyph(SkCanvas& c, Rect r, SkColor ink, int type, float u)
{
    SkPathBuilder p;
    const float x = r.x + 7 * u, y = r.y + 7 * u, w = r.w - 14 * u, h = r.h - 12 * u;
    switch (type)
    {
        case 0: p.moveTo(x, y).lineTo(x + w * 0.62f, y).cubicTo(x + w * 0.95f, y, x + w, y + h * 0.3f, x + w, y + h); break;
        case 1: p.moveTo(x, y + h).cubicTo(x, y + h * 0.3f, x + w * 0.05f, y, x + w * 0.38f, y).lineTo(x + w, y); break;
        case 2:
            p.moveTo(x, y + h).cubicTo(x, y + h * 0.2f, x + w * 0.1f, y, x + w * 0.35f, y).lineTo(x + w * 0.65f, y)
                .cubicTo(x + w * 0.9f, y, x + w, y + h * 0.2f, x + w, y + h);
            break;
        case 3:
            p.moveTo(x, y).lineTo(x + w * 0.25f, y).cubicTo(x + w * 0.45f, y, x + w * 0.42f, y + h, x + w * 0.5f, y + h)
                .cubicTo(x + w * 0.58f, y + h, x + w * 0.55f, y, x + w * 0.75f, y).lineTo(x + w, y);
            break;
        default: // Morph: a dotted line into a low-pass bend
            for (int i = 0; i < 3; ++i) p.moveTo(x + (float) i * 5 * u, y).lineTo(x + (float) i * 5 * u + 2 * u, y);
            p.moveTo(x + w * 0.45f, y).lineTo(x + w * 0.62f, y).cubicTo(x + w * 0.9f, y, x + w, y + h * 0.3f, x + w, y + h);
            p.moveTo(x + w * 0.62f, y + h * 0.5f).lineTo(x + w * 0.62f, y + h * 0.62f);
            p.moveTo(x + w * 0.62f, y + h * 0.85f).lineTo(x + w * 0.62f, y + h);
            break;
    }
    stroke(c, p, ink, 2.5f * u);
}
void shapeGlyph(SkCanvas& c, Rect r, int shape, SkColor ink, float u)
{
    SkPathBuilder p;
    for (int i = 0; i <= 30; ++i)
    {
        const float f = (float) i / 30.0f, y = r.centreY() - lfoWave(shape, f * 1.0 - 0.0) * r.h * 0.42f;
        if (i == 0) p.moveTo(r.x + r.w * f, y); else p.lineTo(r.x + r.w * f, y);
    }
    stroke(c, p, ink, 2 * u);
}
void metronomeGlyph(SkCanvas& c, Rect r, SkColor ink, float u)
{
    const float x = r.centreX() - 9 * u, y = r.centreY() - 9 * u, w = 18 * u, h = 18 * u;
    SkPathBuilder p;
    p.moveTo(x + w * 0.35f, y).lineTo(x + w * 0.65f, y).lineTo(x + w, y + h).lineTo(x, y + h).close();
    p.moveTo(x + w * 0.5f, y + h * 0.75f).lineTo(x + w * 0.85f, y + h * 0.1f);
    stroke(c, p, ink, 2 * u);
}
void phaseGlyph(SkCanvas& c, Rect r, SkColor ink, bool spin, float u)
{
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setStyle(SkPaint::kStroke_Style);
    pen.setStrokeWidth(2 * u);
    pen.setColor(ink);
    const float cx = r.centreX(), cy = r.centreY();
    if (spin)
    {
        c.drawCircle(cx, cy, 8 * u, pen);
        c.drawCircle(cx + 2 * u, cy + 1 * u, 3 * u, pen);
        return;
    }
    c.drawOval(SkRect::MakeLTRB(cx - 6 * u, cy - 5 * u, cx + 6 * u, cy + 5 * u), pen);
    c.drawLine(cx + 3 * u, cy - 10 * u, cx - 3 * u, cy + 10 * u, pen);
}

// ---------------------------------------------------------------- Face: construction
Face::Face(Widget& box, const model::Device& model, P* processor, DeviceLocation where, Tag tagger)
    : ReferenceFace(processor, kWidth, kOriginX, kOriginY, std::move(tagger), kBody), model_(model), where_(where)
{
    selectedPage_ = viewParam("ViewSettings/SelectedPage", choice({"Sample", "Pitch/Osc", "Filter/Global", "Modulation", "MIDI"}));
    zoneVisible_ = viewParam("ViewSettings/ZoneEditorVisible", toggleSpec());
    buildSample();
    buildPitch();
    buildFilterGlobal();
    buildModulation();
    buildMidi();
    buildTitle(box);
}

Param* Face::param(const std::string& path, Spec spec, bool leaf, bool ownChoices)
{
    for (auto& p : params_)
        if (p.path == path && p.partPath.empty()) return &p;
    Param p;
    p.path = path;
    p.leaf = leaf;
    p.ownChoices = ownChoices;
    p.spec = std::move(spec);
    const auto& infos = processor_->parameters();
    for (size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == path)
        {
            // A live parameter: the processor's default (the neutral of a modulation box) and its choice names.
            p.index = (int) i;
            p.spec.def = infos[i].defaultValue;
            if (p.spec.unit == Unit::Choice && ! infos[i].choices.empty() && ! p.ownChoices)
            {
                p.spec.choices = infos[i].choices;
                p.spec.max = (double) infos[i].choices.size() - 1;
            }
            break;
        }
    params_.push_back(std::move(p));
    return &params_.back();
}
Param* Face::partParam(const std::string& path, Spec spec)
{
    for (auto& p : params_)
        if (p.partPath == path) return &p;
    Param p;
    p.partPath = path;
    p.spec = std::move(spec);
    params_.push_back(std::move(p));
    return &params_.back();
}
Param* Face::viewParam(const std::string& path, Spec spec)
{
    auto* p = param(path, std::move(spec), true);
    p->view = true;
    p->index = -1;
    return p;
}

Widget* Face::box(Param* p, int kind, float x0, float y0, float x1, float y1, int page, std::function<bool()> active, std::function<bool()> shown)
{
    auto* w = add<NumberBox>(*this, p, kind);
    w->active = std::move(active);
    tagIf(w, p);
    return placed(w, x0, y0, x1, y1, page, std::move(shown));
}
Widget* Face::chooserBox(Param* p, float x0, float y0, float x1, float y1, int page, std::function<bool()> active,
                         std::function<void(SkCanvas&, Rect, int, SkColor)> glyph, std::function<bool()> shown)
{
    auto* w = add<ChoiceBox>(*this, p);
    w->active = std::move(active);
    w->glyph = std::move(glyph);
    tagIf(w, p);
    return placed(w, x0, y0, x1, y1, page, std::move(shown));
}
Widget* Face::button(std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> lit, std::function<void()> click,
                     std::function<bool()> active, std::function<void(SkCanvas&, Rect, SkColor)> glyph, std::function<bool()> shown)
{
    auto* w = add<Button>(*this, std::move(label));
    w->lit = std::move(lit);
    w->click = std::move(click);
    w->active = std::move(active);
    w->glyph = std::move(glyph);
    return placed(w, x0, y0, x1, y1, page, std::move(shown));
}
Widget* Face::toggleButton(Param* p, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> active)
{
    auto* w = button(std::move(label), x0, y0, x1, y1, page, [this, p] { return get(*p) > 0.5; }, [this, p] { setNow(*p, get(*p) > 0.5 ? 0 : 1); },
                     std::move(active));
    tagIf(w, p);
    return w;
}
Widget* Face::optionButton(Param* p, double v, std::string label, float x0, float y0, float x1, float y1, int page, std::function<bool()> active,
                           std::function<void(SkCanvas&, Rect, SkColor)> glyph)
{
    auto* w = button(std::move(label), x0, y0, x1, y1, page, [this, p, v] { return std::abs(get(*p) - v) < 0.5; }, [this, p, v] { setNow(*p, v); },
                     std::move(active), std::move(glyph));
    tagIf(w, p);
    return w;
}

Face::EnvParams Face::envelope(const EnvPaths& e)
{
    const auto& b = e.base;
    auto level = [&](const char* name, double def) { auto s = e.level; s.def = def; return param(b + name, s); };
    EnvParams env{};
    env.attack = param(b + "AttackTime", logarithmic(0.1, 20000, 0.1, Unit::Ms, 0));
    env.decay = param(b + "DecayTime", logarithmic(1, 60000, 600, Unit::Ms));
    env.release = param(b + "ReleaseTime", logarithmic(1, 60000, 50, Unit::Ms));
    env.initial = level("AttackLevel", e.initial);
    env.peak = level("DecayLevel", e.peak);
    env.sustain = level("SustainLevel", e.sustain);
    env.end = level("ReleaseLevel", e.end);
    env.attackSlope = param(b + "AttackSlope", linear(-1, 1, 0, Unit::Two));
    env.decaySlope = param(b + "DecaySlope", linear(-1, 1, 1, Unit::Two));
    env.releaseSlope = param(b + "ReleaseSlope", linear(-1, 1, 1, Unit::Two));
    env.loop = param(b + "LoopMode", choice(kLoopModes));
    env.loopTime = param(b + "LoopTime", logarithmic(0.2, 20000, 100, Unit::Ms));
    env.repeat = param(b + "RepeatTime", choice({"1/64", "1/48", "1/32", "1/16", "1/12", "1/8", "1/6", "3/16", "1/4", "1/3", "3/8", "1/2", "3/4", "1", "2"}, 3));
    env.timeVel = param(b + "TimeVelScale", linear(-100, 100, 0, Unit::PlainPct));
    env.overlay = param(b + "CurrentOverlay", toggleSpec(), true);
    env.overlay->view = true; // a view leaf: the slope overlay switch is not an undo step
    env.level = e.level;
    return env;
}

// The two rows of an envelope's boxes: Initial / Peak / Sustain / End / Loop over Attack / Decay / Release / Time<Vel / Time
// (the slope overlay square swaps Attack / Decay / Release for their slopes).
void Face::envelopeControls(const EnvParams& env, float x, float yLabel1, float yBox1, float ySquare, float yBox2, int page,
                            std::function<bool()> active, bool hasEnd, float loopX, int levelKind)
{
    const auto overlay = [this, p = env.overlay] { return get(*p) > 0.5; };
    const auto times = [overlay] { return ! overlay(); };
    const char* top[] = {"Initial", "Peak", "Sustain", "End"};
    Param* levels[] = {env.initial, env.peak, env.sustain, env.end};
    for (int i = 0; i < (hasEnd ? 4 : 3); ++i)
    {
        const float x0 = x + 96.0f * (float) i;
        text(top[i], x0, yLabel1, x0 + 96, yBox1 - 2, page);
        box(levels[i], levelKind, x0, yBox1, x0 + 86, yBox1 + 26, page, active);
    }
    text("Loop", loopX, yLabel1, loopX + 96, yBox1 - 2, page);
    chooserBox(env.loop, loopX, yBox1, loopX + 86, yBox1 + 26, page, active);
    auto* square = add<OverlaySquare>(*this, env.overlay);
    placed(square, x, ySquare - 4, x + 12, ySquare + 14, page);
    square->tooltip = "Show the stage slopes";
    text("Attack", x + 16, yBox2 - 26, x + 96, yBox2 - 2, page, times);
    text("A.Slope", x + 16, yBox2 - 26, x + 96, yBox2 - 2, page, overlay);
    text("Decay", x + 96, yBox2 - 26, x + 192, yBox2 - 2, page, times);
    text("D.Slope", x + 96, yBox2 - 26, x + 192, yBox2 - 2, page, overlay);
    text("Release", x + 192, yBox2 - 26, x + 288, yBox2 - 2, page, times);
    text("R.Slope", x + 192, yBox2 - 26, x + 288, yBox2 - 2, page, overlay);
    Param* stage[] = {env.attack, env.decay, env.release}, *slope[] = {env.attackSlope, env.decaySlope, env.releaseSlope};
    for (int i = 0; i < 3; ++i)
    {
        const float x0 = x + 96.0f * (float) i;
        box(stage[i], 0, x0, yBox2, x0 + 86, yBox2 + 26, page, active, times);
        box(slope[i], 1, x0, yBox2, x0 + 86, yBox2 + 26, page, active, overlay);
    }
    const float xv = hasEnd ? x + 288 : loopX - 96;
    text("Time<Vel", xv, yBox2 - 26, xv + 96, yBox2 - 2, page);
    box(env.timeVel, levelKind, xv, yBox2, xv + 86, yBox2 + 26, page, active);
    text("Time", loopX, yBox2 - 26, loopX + 96, yBox2 - 2, page);
    // The loop time is greyed while the envelope does not loop (Loop: None).
    // Beat / Sync repeat on a note division (RepeatTime) instead of a time.
    const auto beats = [this, loop = env.loop] { const auto m = std::lround(get(*loop)); return m == 2 || m == 3; };
    const auto timed = [beats] { return ! beats(); };
    static_cast<NumberBox*>(box(env.loopTime, 2, loopX, yBox2, loopX + 86, yBox2 + 26, page, active, timed))->dim = [this, loop = env.loop] {
        return get(*loop) < 0.5;
    };
    chooserBox(env.repeat, loopX, yBox2, loopX + 86, yBox2 + 26, page, active, {}, beats);
}

// ---------------------------------------------------------------- Sample tab
void Face::buildSample()
{
    constexpr int pg = 0;
    auto* wave = add<WaveDisplay>(*this);
    placed(wave, 30, 52, 1382, 264, pg);
    sampleZoom = viewParam("ViewSettings/VerticalSampleZoom", linear(1, 16, 1, Unit::Two));
    channel = viewParam("ViewSettings/SelectedSampleChannel", choice({"B", "M", "L", "R"}));
    auto* slider = add<ZoomSlider>(*this);
    slider->tooltip = "Vertical zoom of the sample display";
    placed(slider, 1388, 60, 1416, 144, pg);
    const char* channels[] = {"B", "M", "L", "R"};
    for (int i = 0; i < 4; ++i)
    {
        const float y0 = 148 + 32.0f * (float) i;
        auto* b = button(channels[i], 1390, y0, 1416, y0 + 26, pg, [this, i] { return std::lround(get(*channel)) == i; },
                         [this, i] { set(*channel, i); }, {});
        static_cast<Button*>(b)->litColour = SkColorSetRGB(0xf3, 0xf4, 0xf5);
        static_cast<Button*>(b)->quietOff = true;
    }

    sampleStart = partParam("SampleStart", linear(0, 1e9, 0, Unit::Frames));
    sampleEnd = partParam("SampleEnd", linear(0, 1e9, 0, Unit::Frames));
    sustainMode = partParam("SustainLoop/Mode", choice({"Off", "Forward", "Alternate"}));
    releaseMode = partParam("ReleaseLoop/Mode", linear(0, 3, 0, Unit::Int));
    loopStart = partParam("SustainLoop/Start", linear(0, 1e9, 0, Unit::Frames));
    loopEnd = partParam("SustainLoop/End", linear(0, 1e9, 0, Unit::Frames));
    loopFade = partParam("SustainLoop/Crossfade", linear(0, 1e9, 0, Unit::Frames));
    releaseStart = partParam("ReleaseLoop/Start", linear(0, 1e9, 0, Unit::Frames));
    const auto sustainLoop = [this] { return get(*sustainMode) > 0.5; };
    // ReleaseLoop/Mode: 0 play to the end (->|), 1 forward, 2 back and forth, 3 off (the sustain loop continues).
    // TODO(DEV-14b): the reference's own enum order of ReleaseLoop/Mode is inferred from its labels, not measured.
    const auto releaseLoop = [this, sustainLoop] { return sustainLoop() && std::lround(get(*releaseMode)) != 3; };

    text("Reverse", 32, 270, 120, 292, pg);
    auto* reverse = param("Player/Reverse", toggleSpec());
    box(reverse, 2, 32, 294, 108, 320, pg);
    text("Snap", 32, 326, 120, 348, pg);
    box(param("Player/Snap", toggleSpec()), 2, 32, 350, 108, 376, pg);

    text("Sample", 124, 270, 334, 292, pg);
    auto* layer = add<ChoiceBox>(*this, nullptr);
    layer->labels = [this] {
        std::vector<std::string> names;
        for (const auto& part : model_.sampleParts) names.push_back(part.name);
        return names;
    };
    layer->current = [this] { return selectedPart(); };
    layer->pick = [this](int part) { selectPart(part); };
    layer->tooltip = "Sample layer shown on this tab";
    placed(layer, 124, 294, 334, 320, pg);
    auto* swap = button("", 340, 293, 370, 321, pg, {}, [this] { ctx().statusText = "Hot-swap of Sampler layers is not available yet (UI-42b)"; }, {},
                        [](SkCanvas& c, Rect r, SkColor ink) { titleIcon(c, TitleIcon::HotSwap, r.centreX(), r.centreY(), r.h * 0.9f, ink); });
    swap->tooltip = "Hot-swap the sample (UI-42b)";
    static_cast<Button*>(swap)->bare = true;
    text("Root", 124, 326, 200, 348, pg);
    box(partParam("RootKey", linear(0, 127, 60, Unit::Note)), 0, 124, 350, 200, 376, pg);
    text("Detune", 206, 326, 290, 348, pg);
    box(partParam("Detune", linear(-50, 50, 0, Unit::Cents)), 1, 206, 350, 286, 376, pg);
    text("Scale", 292, 326, 372, 348, pg);
    box(partParam("TuneScale", linear(0, 100, 100, Unit::PlainPct)), 2, 292, 350, 372, 376, pg);
    text("Vol", 384, 270, 470, 292, pg);
    {
        Spec vol = gain(1);
        vol.law = LawKind::Gain;
        box(partParam("Volume", vol), 1, 384, 294, 470, 320, pg);
    }
    text("Pan", 384, 326, 470, 348, pg);
    box(partParam("Panorama", linear(-1, 1, 0, Unit::Pan)), 2, 384, 350, 470, 376, pg);
    text("Sample Start", 486, 270, 620, 292, pg);
    box(sampleStart, 2, 486, 294, 608, 320, pg);
    text("Sample End", 486, 326, 620, 348, pg);
    box(sampleEnd, 2, 486, 350, 608, 376, pg);

    text("Sustain Mode", 624, 270, 830, 292, pg);
    for (int i = 0; i < 3; ++i)
    {
        const float x0 = 624 + 48.0f * (float) i;
        optionButton(sustainMode, i, "", x0, 294, x0 + 42, 320, pg, {}, [this, i](SkCanvas& c, Rect r, SkColor ink) { loopGlyph(c, r, ink, i, unit()); })
            ->tooltip = i == 0 ? "No sustain loop" : i == 1 ? "Sustain loop forward" : "Sustain loop back and forth";
    }
    toggleButton(partParam("Link", toggleSpec()), "Link", 776, 294, 826, 320, pg)->tooltip = "Link the sustain loop's start to Sample Start";
    text("Release Mode", 624, 326, 830, 348, pg);
    {
        auto* off = static_cast<Button*>(button("OFF", 624, 350, 666, 376, pg, [this] { return std::lround(get(*releaseMode)) == 3; },
                                                 [this] { setNow(*releaseMode, 3); }, sustainLoop, {}));
        off->textScale = 0.72f;
        off->bold = true;
        off->tooltip = "No release loop: the sustain loop continues";
        for (int i = 0; i < 3; ++i)
        {
            const float x0 = 672 + 48.0f * (float) i;
            optionButton(releaseMode, i, "", x0, 350, x0 + 42, 376, pg, sustainLoop,
                         [this, i](SkCanvas& c, Rect r, SkColor ink) { loopGlyph(c, r, ink, i, unit()); });
        }
    }
    const auto dimUnless = [](Widget* w, std::function<bool()> when) { static_cast<NumberBox*>(w)->dim = [when] { return ! when(); }; };
    text("Loop Start", 840, 270, 970, 292, pg);
    dimUnless(box(loopStart, 2, 840, 294, 962, 320, pg), sustainLoop);
    text("Loop End", 972, 270, 1100, 292, pg);
    dimUnless(box(loopEnd, 2, 972, 294, 1094, 320, pg), sustainLoop);
    text("Crossfade", 1104, 270, 1230, 292, pg);
    dimUnless(box(loopFade, 2, 1104, 294, 1226, 320, pg), sustainLoop);
    text("Detune", 1236, 270, 1330, 292, pg);
    dimUnless(box(partParam("SustainLoop/Detune", linear(-50, 50, 0, Unit::Cents)), 1, 1236, 294, 1326, 320, pg), sustainLoop);
    text("Release Loop", 840, 326, 1000, 348, pg);
    dimUnless(box(releaseStart, 2, 840, 350, 962, 376, pg), releaseLoop);
    text("Crossfade", 1104, 326, 1230, 348, pg);
    dimUnless(box(partParam("ReleaseLoop/Crossfade", linear(0, 1e9, 0, Unit::Frames)), 2, 1104, 350, 1226, 376, pg), releaseLoop);
    text("Detune", 1236, 326, 1330, 348, pg);
    dimUnless(box(partParam("ReleaseLoop/Detune", linear(-50, 50, 0, Unit::Cents)), 1, 1236, 350, 1326, 376, pg), releaseLoop);
    text("Interpol", 1340, 270, 1430, 292, pg);
    // TODO(DEV-14b): the names of InterpolationMode 0, 1 and 3 are from the manual; 2 = "Good" is the capture's.
    chooserBox(param("Player/InterpolationMode", choice({"No", "Normal", "Good", "Best"}, 2), true, true), 1340, 294, 1416, 320, pg);
    toggleButton(param("Player/MultiSampleMap/LoadInRam", toggleSpec(), true), "RAM", 1340, 350, 1416, 376, pg)->tooltip =
        "Load the samples into memory";
}

// ---------------------------------------------------------------- Pitch / Osc tab
void Face::buildPitch()
{
    constexpr int pg = 1;
    const std::string osc = "Player/SubOsc/Slot/Value/SimplerSubOsc/";
    oscOn = param("Player/SubOsc/IsOn", toggleSpec());
    const auto oscActive = [this] { return on(oscOn); };
    toggleButton(oscOn, "Osc", 32, 56, 118, 82, pg);
    auto* mode = param(osc + "Mode", choice({"FM", "AM"}));
    optionButton(mode, 0, "FM", 32, 178, 70, 204, pg, oscActive);
    optionButton(mode, 1, "AM", 76, 178, 114, 204, pg, oscActive);
    Spec oscLevel = gain(0);
    const auto oscEnv = envelope({osc + "Envelope/", oscLevel, 0.000316, 1, 1, 0.000316});
    auto* oscDisplay = add<EnvelopeDisplay>(*this, std::array<Param*, 10>{oscEnv.attack, oscEnv.decay, oscEnv.release, oscEnv.initial, oscEnv.peak,
                                                                           oscEnv.sustain, oscEnv.end, oscEnv.attackSlope, oscEnv.decaySlope,
                                                                           oscEnv.releaseSlope},
                                            EnvelopeDisplay::Levels::Gain, oscActive);
    placed(oscDisplay, 128, 56, 502, 206, pg);
    envelopeControls(oscEnv, 534, 72, 96, 137, 152, pg, oscActive, true, 918);
    text("Type", 1088, 72, 1180, 94, pg);
    chooserBox(param(osc + "Type", choice(kOscTypes)), 1088, 96, 1174, 122, pg, oscActive);
    text("Volume", 1186, 72, 1280, 94, pg);
    {
        Spec vol = gain(0.0316);
        box(param(osc + "Volume", vol), 0, 1186, 96, 1272, 122, pg, oscActive);
    }
    text("Vol<Vel", 1282, 72, 1380, 94, pg);
    box(param(osc + "VolumeVelScale", linear(0, 1, 0, Unit::Percent)), 0, 1282, 96, 1368, 122, pg, oscActive);
    auto* fixed = param(osc + "IsFixedFreq", toggleSpec());
    const auto isFixed = [this, fixed] { return get(*fixed) > 0.5; };
    const auto ratio = [isFixed] { return ! isFixed(); };
    text("Fixed", 1088, 128, 1180, 150, pg);
    box(fixed, 2, 1088, 152, 1174, 178, pg, oscActive);
    text("Coarse", 1186, 128, 1280, 150, pg, ratio);
    box(param(osc + "TuneCoarse", linear(-2, 48, 1, Unit::Int)), 0, 1186, 152, 1272, 178, pg, oscActive, ratio);
    text("Fine", 1282, 128, 1380, 150, pg, ratio);
    box(param(osc + "TuneFine", linear(0, 1000, 0, Unit::Int)), 0, 1282, 152, 1368, 178, pg, oscActive, ratio);
    // TODO(DEV-14b): the fixed-frequency boxes are not in the captures; their laws are assumed.
    text("Freq", 1186, 128, 1280, 150, pg, isFixed);
    box(param(osc + "FreqFixed", logarithmic(0.1, 10000, 100, Unit::Hz)), 0, 1186, 152, 1272, 178, pg, oscActive, isFixed);
    text("Multi", 1282, 128, 1380, 150, pg, isFixed);
    chooserBox(param(osc + "FreqFixedMul", choice({"0.001", "0.01", "0.1", "1", "10", "100", "1000"}, 3)), 1282, 152, 1368, 178, pg, oscActive, {}, isFixed);

    const std::string pe = "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/";
    pitchEnvOn = param("Pitch/Envelope/IsOn", toggleSpec());
    const auto penvActive = [this] { return on(pitchEnvOn); };
    toggleButton(pitchEnvOn, "P.Env", 32, 226, 118, 252, pg);
    text("Amount", 32, 326, 128, 348, pg);
    box(param(pe + "Amount", linear(-48, 48, 0, Unit::Semis)), 1, 32, 350, 118, 376, pg, penvActive);
    const auto penv = envelope({pe, linear(-1, 1, 0, Unit::Percent), 0, 1, 0, 0});
    auto* penvDisplay = add<EnvelopeDisplay>(*this, std::array<Param*, 10>{penv.attack, penv.decay, penv.release, penv.initial, penv.peak, penv.sustain,
                                                                            penv.end, penv.attackSlope, penv.decaySlope, penv.releaseSlope},
                                             EnvelopeDisplay::Levels::Bipolar, penvActive);
    placed(penvDisplay, 128, 226, 502, 376, pg);
    envelopeControls(penv, 534, 244, 268, 309, 324, pg, penvActive, true, 918, 1); // bipolar levels: modulation boxes

    text("Spread", 1088, 244, 1180, 266, pg);
    box(param("Globals/SpreadAmount", linear(0, 100, 0, Unit::PlainPct)), 0, 1088, 268, 1174, 294, pg);
    text("Transp", 1184, 244, 1280, 266, pg);
    box(param("Pitch/TransposeKey", linear(-48, 48, 0, Unit::Semis)), 1, 1184, 268, 1270, 294, pg);
    text("Detune", 1280, 244, 1380, 266, pg);
    box(param("Pitch/TransposeFine", linear(-50, 50, 0, Unit::Cents)), 1, 1280, 268, 1366, 294, pg);
    text("Zn Shft", 1088, 300, 1180, 322, pg);
    box(param("Globals/KeyZoneShift", linear(-48, 48, 0, Unit::Semis)), 1, 1088, 324, 1174, 350, pg);
    text("Glide", 1184, 300, 1280, 322, pg);
    auto* glide = param("Globals/PortamentoMode", choice({"Off", "Glide", "Portamento"}));
    chooserBox(glide, 1184, 324, 1270, 350, pg);
    text("Time", 1280, 302, 1380, 324, pg);
    // Greyed while Glide is off, as in the reference.
    static_cast<NumberBox*>(box(param("Globals/PortamentoTime", logarithmic(0.1, 10000, 50, Unit::Ms)), 2, 1280, 324, 1366, 350, pg))->dim = [this, glide] {
        return get(*glide) < 0.5;
    };
}

// ---------------------------------------------------------------- Filter / Global tab
void Face::buildFilterGlobal()
{
    constexpr int pg = 2;
    const std::string f = "Filter/Slot/Value/SimplerFilter/";
    filterOn = param("Filter/IsOn", toggleSpec());
    const auto filterActive = [this] { return on(filterOn); };
    toggleButton(filterOn, "Filter", 32, 56, 118, 82, pg);
    filterType = param(f + "Type", choice({"Lowpass", "Highpass", "Bandpass", "Notch", "Morph"}));
    const float typeBoxes[][2] = {{32, 90}, {78, 90}, {32, 122}, {78, 122}, {32, 154}};
    // The reference hides the type and slope keys while the filter is off.
    const auto shownWhenOn = [this, filterActive] { return page() == 2 && filterActive(); };
    for (int i = 0; i < 5; ++i)
    {
        auto* key = optionButton(filterType, i, "", typeBoxes[i][0], typeBoxes[i][1], typeBoxes[i][0] + 40, typeBoxes[i][1] + 26, pg, filterActive,
                                 [this, i](SkCanvas& c, Rect r, SkColor ink) { filterGlyph(c, r, ink, i, unit()); });
        key->tooltip = filterType->spec.choices[(size_t) i];
        showWhen(key, shownWhenOn);
    }
    filterFreq = param(f + "Freq", logarithmic(30, 22000, 22000, Unit::Hz));
    filterRes = param(f + "Res", linear(0, 1.25, 0, Unit::Percent));
    filterSlope = param(f + "Slope", toggleSpec(1));
    auto* display = add<FilterDisplay>(*this);
    display->tooltip = "Drag: cutoff (left / right) and resonance (up / down)";
    placed(display, 128, 56, 406, 192, pg);
    const auto lpHp = [this] { const auto t = std::lround(get(*filterType)); return t == 0 || t == 1; };
    const auto bpNotch = [lpHp] { return ! lpHp(); };
    text("Circuit", 32, 198, 128, 220, pg);
    chooserBox(param(f + "CircuitLpHp", choice({"Clean", "OSR", "MS2", "SMP", "PRD"})), 32, 222, 118, 248, pg, filterActive, {}, lpHp);
    chooserBox(param(f + "CircuitBpNoMo", choice({"Clean", "OSR"})), 32, 222, 118, 248, pg, filterActive, {}, bpNotch);
    text("Slope", 128, 198, 220, 220, pg);
    showWhen(optionButton(filterSlope, 0, "12", 128, 222, 168, 248, pg, filterActive), shownWhenOn);
    showWhen(optionButton(filterSlope, 1, "24", 174, 222, 214, 248, pg, filterActive), shownWhenOn);
    text("Freq", 224, 198, 320, 220, pg);
    box(filterFreq, 0, 224, 222, 314, 248, pg, filterActive);
    text("Res", 324, 198, 410, 220, pg);
    box(filterRes, 0, 324, 222, 404, 248, pg, filterActive);
    const auto morph = [this] { return std::lround(get(*filterType)) == 4; };
    const auto drive = [morph] { return ! morph(); };
    text("Drive", 32, 254, 168, 276, pg, drive);
    box(param(f + "Drive", linear(0, 24, 0, Unit::Db2)), 0, 32, 278, 168, 304, pg, filterActive, drive);
    // TODO(DEV-14b): no capture of the Morph type yet; its Morph amount takes the Drive box here.
    text("Morph", 32, 254, 168, 276, pg, morph);
    box(param(f + "X", linear(0, 1, 0, Unit::Percent)), 0, 32, 278, 168, 304, pg, filterActive, morph);
    text("Vel", 224, 254, 320, 276, pg);
    box(param(f + "ModByVelocity", linear(0, 1, 0, Unit::Percent)), 0, 224, 278, 314, 304, pg, filterActive);
    text("Key", 324, 254, 410, 276, pg);
    box(param(f + "ModByPitch", linear(0, 1, 1, Unit::Percent)), 0, 324, 278, 404, 304, pg, filterActive);

    const std::string sh = "Shaper/Slot/Value/SimplerShaper/";
    shaperOn = param("Shaper/IsOn", toggleSpec());
    const auto shaperActive = [this] { return on(shaperOn); };
    auto* structure = param(sh + "Structure", toggleSpec());
    auto* post = button("", 64, 312, 90, 338, pg, {}, [this, structure] { setNow(*structure, get(*structure) > 0.5 ? 0 : 1); }, {},
           [this, structure](SkCanvas& c, Rect r, SkColor ink) {
               ctl::triangle(c, r.centreX(), r.centreY(), 14 * unit(), 9 * unit(), get(*structure) > 0.5 ? ctl::Dir::Down : ctl::Dir::Up, ink);
           });
    post->tooltip = "Shaper before or after the filter";
    tagIf(post, structure);
    toggleButton(shaperOn, "Shaper", 32, 350, 118, 376, pg);
    shaperType = param(sh + "Type", choice(kShaperTypes));
    shaperAmount = param(sh + "Amount", linear(0, 100, 0, Unit::Two));
    placed(add<ShaperDisplay>(*this), 128, 326, 214, 376, pg);
    text("Type", 224, 326, 320, 348, pg);
    chooserBox(shaperType, 224, 350, 314, 376, pg, shaperActive);
    text("Amount", 324, 326, 420, 348, pg);
    box(shaperAmount, 2, 324, 350, 404, 376, pg, shaperActive);

    // Filter envelope.
    filterEnvOn = param(f + "Envelope/IsOn", toggleSpec());
    const auto fenvActive = [this, filterActive] { return filterActive() && on(filterEnvOn); };
    static_cast<Button*>(toggleButton(filterEnvOn, "F.Env", 438, 56, 524, 82, pg, filterActive))->blankInactive = true;
    text("Amount", 438, 142, 534, 164, pg);
    box(param(f + "Envelope/Amount", linear(-72, 72, 0, Unit::SemisBare)), 1, 438, 166, 524, 192, pg, fenvActive);
    const auto fenv = envelope({f + "Envelope/", linear(0, 1, 0, Unit::Percent), 0, 1, 0, 0});
    auto* fenvDisplay = add<EnvelopeDisplay>(*this, std::array<Param*, 10>{fenv.attack, fenv.decay, fenv.release, fenv.initial, fenv.peak, fenv.sustain,
                                                                            fenv.end, fenv.attackSlope, fenv.decaySlope, fenv.releaseSlope},
                                             EnvelopeDisplay::Levels::Unipolar, fenvActive);
    placed(fenvDisplay, 534, 56, 908, 192, pg);
    envelopeControls(fenv, 438, 198, 222, 263, 278, pg, fenvActive, true, 822);

    // Global: volume and its envelope, pan, envelope time scaling, voices.
    text("Volume", 946, 54, 1040, 76, pg);
    box(param("VolumeAndPan/Volume", linear(-36, 36, -12, Unit::Db1)), 0, 946, 78, 1032, 104, pg);
    text("Vol<Vel", 946, 142, 1040, 164, pg);
    box(param("VolumeAndPan/VolumeVelScale", linear(0, 1, 0.35, Unit::Percent)), 0, 946, 166, 1032, 192, pg);
    const auto venv = envelope({"VolumeAndPan/Envelope/", gain(1), 1, 1, 1, 0.000316});
    auto* venvDisplay = add<EnvelopeDisplay>(*this, std::array<Param*, 10>{venv.attack, venv.decay, venv.release, venv.initial, venv.peak, venv.sustain,
                                                                            venv.end, venv.attackSlope, venv.decaySlope, venv.releaseSlope},
                                             EnvelopeDisplay::Levels::Gain, [] { return true; });
    placed(venvDisplay, 1042, 56, 1416, 192, pg);
    envelopeControls(venv, 946, 198, 222, 265, 278, pg, {}, false, 1330);
    text("Pan", 946, 326, 1040, 348, pg);
    box(param("VolumeAndPan/Panorama", linear(-1, 1, 0, Unit::Pan)), 2, 946, 350, 1032, 376, pg);
    text("Pan<Rnd", 1042, 326, 1136, 348, pg);
    box(param("VolumeAndPan/PanoramaRnd", linear(0, 1, 0, Unit::Percent)), 0, 1042, 350, 1128, 376, pg);
    text("Time", 1138, 326, 1232, 348, pg);
    box(param("Globals/EnvScale/EnvTime", linear(-100, 100, 0, Unit::PlainPct)), 1, 1138, 350, 1224, 376, pg);
    text("Time<Key", 1234, 326, 1330, 348, pg);
    box(param("Globals/EnvScale/EnvTimeKeyScale", linear(-100, 100, 0, Unit::PlainPct)), 1, 1234, 350, 1320, 376, pg);
    text("Voices", 1330, 326, 1430, 348, pg);
    chooserBox(param("Globals/NumVoices", choice(kVoices, 5), true), 1330, 350, 1386, 376, pg);
    toggleButton(param("Globals/RetriggerMode", toggleSpec(1), true), "R", 1392, 350, 1416, 376, pg)->tooltip = "Retrigger";
}

// ---------------------------------------------------------------- Modulation tab
void Face::buildModulation()
{
    constexpr int pg = 3;
    const auto destination = [this](const std::string& base, int slot) {
        const auto b = base + "ModDst/ModConnections." + std::to_string(slot) + "/";
        return std::make_pair(param(b + "Connection", choice(destinations()), true), param(b + "Amount", linear(-100, 100, 0, Unit::Two), true));
    };
    const auto destinationRows = [&](const std::string& base, float label, float x0, float x1, float a0, std::function<bool()> active) {
        for (int slot = 0; slot < 2; ++slot)
        {
            const float y0 = 316 + 34.0f * (float) slot;
            const auto [target, amount] = destination(base, slot);
            text(slot == 0 ? "A" : "B", label, y0, x0, y0 + 26, pg);
            chooserBox(target, x0, y0, x1, y0 + 26, pg, active);
            static_cast<NumberBox*>(box(amount, 1, a0, y0, a0 + 86, y0 + 26, pg, active))->dim = [this, target] { return std::lround(get(*target)) == 0; };
        }
    };

    const std::string aux = "AuxEnv/Slot/Value/SimplerAuxEnvelope/";
    auxOn = param("AuxEnv/IsOn", toggleSpec());
    const auto auxActive = [this] { return on(auxOn); };
    toggleButton(auxOn, "Aux", 32, 56, 118, 82, pg);
    const auto aenv = envelope({aux, linear(0, 1, 0, Unit::Percent), 0, 1, 0, 0});
    auto* auxDisplay = add<EnvelopeDisplay>(*this, std::array<Param*, 10>{aenv.attack, aenv.decay, aenv.release, aenv.initial, aenv.peak, aenv.sustain,
                                                                           aenv.end, aenv.attackSlope, aenv.decaySlope, aenv.releaseSlope},
                                            EnvelopeDisplay::Levels::Unipolar, auxActive);
    placed(auxDisplay, 128, 56, 502, 192, pg);
    envelopeControls(aenv, 32, 198, 222, 263, 278, pg, auxActive, true, 416);
    destinationRows(aux, 32, 62, 406, 416, auxActive);

    const std::string bases[] = {"Lfo/Slot/Value/SimplerLfo/", "AuxLfos.0/Slot/Value/SimplerAuxLfo/", "AuxLfos.1/Slot/Value/SimplerAuxLfo/"};
    const std::string switches[] = {"Lfo/IsOn", "AuxLfos.0/IsOn", "AuxLfos.1/IsOn"};
    for (int l = 0; l < 3; ++l)
    {
        const float x = 530 + 304.0f * (float) l, dx = x - 530;
        const auto& b = bases[l];
        lfoOn[l] = param(switches[l], toggleSpec());
        const auto active = [this, l] { return on(lfoOn[l]); };
        toggleButton(lfoOn[l], "LFO " + std::to_string(l + 1), x, 56, x + 86, 82, pg);
        placed(add<LfoDisplay>(*this, l), 624 + dx, 56, 810 + dx, 134, pg);
        lfoType[l] = param(b + "Type", choice(kLfoShapes));
        lfoRateType[l] = param(b + "RateType", choice({"Hz", "Sync"}));
        lfoFreq[l] = param(b + "Frequency", logarithmic(0.01, 30, 1, Unit::Hz));
        lfoBeat[l] = param(b + "BeatRate", choice(kBeatRates, 4));
        const auto hz = [this, l] { return get(*lfoRateType[l]) < 0.5; };
        const auto synced = [hz] { return ! hz(); };
        text("Type", x, 142, x + 96, 164, pg);
        chooserBox(lfoType[l], x, 166, x + 86, 192, pg, active,
                   [this](SkCanvas& c, Rect r, int shape, SkColor ink) { shapeGlyph(c, r, shape, ink, unit()); });
        text("Rate", 626 + dx, 142, 720 + dx, 164, pg);
        optionButton(lfoRateType[l], 0, "Hz", 626 + dx, 166, 664 + dx, 192, pg, active);
        optionButton(lfoRateType[l], 1, "", 670 + dx, 166, 708 + dx, 192, pg, active,
                     [this](SkCanvas& c, Rect r, SkColor ink) { metronomeGlyph(c, r, ink, unit()); })
            ->tooltip = "Rate synced to the tempo";
        text("Freq", 722 + dx, 142, 816 + dx, 164, pg, hz);
        box(lfoFreq[l], 0, 722 + dx, 166, 808 + dx, 192, pg, active, hz);
        text("Beats", 722 + dx, 142, 816 + dx, 164, pg, synced);
        box(lfoBeat[l], 0, 722 + dx, 166, 808 + dx, 192, pg, active, synced);
        text("Attack", x, 198, x + 96, 220, pg);
        box(param(b + "Attack", logarithmic(0.1, 20000, 0.1, Unit::Ms, 0)), 0, x, 222, x + 86, 248, pg, active);
        text("Retrig", 626 + dx, 198, 720 + dx, 220, pg);
        auto* retrig = param(b + "Retrigger", toggleSpec(1));
        auto* retrigKey = static_cast<Button*>(button("", 626 + dx, 222, 712 + dx, 248, pg, [this, retrig] { return get(*retrig) > 0.5; },
                                                       [this, retrig] { setNow(*retrig, get(*retrig) > 0.5 ? 0 : 1); }, active,
                                                       [this, retrig, active](SkCanvas& c, Rect r, SkColor ink) {
                                                           paint::text(c, active() && get(*retrig) > 0.5 ? "On" : "Off", r, ink, text(), Align::Centre);
                                                       }));
        retrigKey->blankInactive = true;
        tagIf(retrigKey, retrig);
        text("Offset", 722 + dx, 198, 816 + dx, 220, pg);
        box(param(b + "Offset", linear(0, 360, 0, Unit::Degrees)), 0, 722 + dx, 222, 808 + dx, 248, pg, active);
        text("Key", x, 254, x + 96, 276, pg);
        box(param(b + "FrequencyKeyScale", linear(0, 1, 0, Unit::Percent)), 0, x, 278, x + 86, 304, pg, active);
        if (l == 0)
        {
            const std::pair<const char*, Param*> amounts[] = {
                {"Vol", param("VolumeAndPan/VolumeLfoAmount", linear(0, 1, 0, Unit::Percent2))},
                {"Pan", param("VolumeAndPan/PanoramaLfoAmount", linear(0, 1, 0, Unit::Percent2))},
                {"Filter", param("Filter/Slot/Value/SimplerFilter/ModByLfo", linear(0, 24, 0, Unit::Two))},
                {"Pitch", param("Pitch/PitchLfoAmount", linear(0, 1, 0, Unit::Percent2))}};
            for (int i = 0; i < 4; ++i)
            {
                const float y0 = 316 + 34.0f * (float) (i % 2), lx = i < 2 ? 530.0f : 676.0f, bx = i < 2 ? 580.0f : 730.0f;
                text(amounts[i].first, lx, y0, bx, y0 + 26, pg);
                auto* amountBox = static_cast<NumberBox*>(
                    box(amounts[i].second, 0, bx, y0, bx + 78, y0 + 26, pg, i == 2 ? std::function<bool()>([this] { return on(filterOn); }) : std::function<bool()>{}));
                amountBox->dim = [active] { return ! active(); }; // greyed while LFO 1 is off
            }
            continue;
        }
        text("Stereo", 626 + dx, 254, 720 + dx, 276, pg);
        auto* stereo = param(b + "StereoMode", choice({"Phase", "Spin"}));
        optionButton(stereo, 0, "", 626 + dx, 278, 664 + dx, 304, pg, active, [this](SkCanvas& c, Rect r, SkColor ink) { phaseGlyph(c, r, ink, false, unit()); })
            ->tooltip = "Stereo phase offset";
        optionButton(stereo, 1, "", 670 + dx, 278, 708 + dx, 304, pg, active, [this](SkCanvas& c, Rect r, SkColor ink) { phaseGlyph(c, r, ink, true, unit()); })
            ->tooltip = "Stereo spin";
        const auto phase = [this, stereo] { return get(*stereo) < 0.5; };
        const auto spin = [phase] { return ! phase(); };
        text("Phase", 722 + dx, 254, 816 + dx, 276, pg, phase);
        box(param(b + "Phase", linear(0, 360, 0, Unit::Degrees)), 0, 722 + dx, 278, 808 + dx, 304, pg, active, phase);
        text("Spin", 722 + dx, 254, 816 + dx, 276, pg, spin);
        box(param(b + "Spin", linear(0, 1, 0, Unit::Percent)), 0, 722 + dx, 278, 808 + dx, 304, pg, active, spin);
        destinationRows(b, x, x + 30, x + 182, x + 192, active);
    }
}

// ---------------------------------------------------------------- MIDI tab
void Face::buildMidi()
{
    constexpr int pg = 4;
    const char* names[] = {"Key", "Velocity", "Off Vel", "Mod Wheel", "Foot Ctrl", "Pitch Bend", "Note PB", "Slide", "Pressure"};
    const char* sources[] = {"KeyDst", "VelDst", "RelVelDst", "MidiCtrl.0", "MidiCtrl.1", "MidiCtrl.2", "MidiCtrl.3", "MidiCtrl.4", "MidiCtrl.5"};
    for (int row = 0; row < 9; ++row)
    {
        const float y0 = 58 + 36.0f * (float) row;
        text(names[row], 64, y0, 184, y0 + 26, pg);
        for (int slot = 0; slot < 2; ++slot)
        {
            const auto b = std::string(sources[row]) + "/ModConnections." + std::to_string(slot) + "/";
            auto* target = param(b + "Connection", choice(destinations()), true);
            auto* amount = param(b + "Amount", linear(-100, 100, 0, Unit::Two), true);
            midiConnections.push_back(target);
            const float x0 = slot == 0 ? 184.0f : 512.0f;
            chooserBox(target, x0, y0, x0 + 222, y0 + 26, pg);
            static_cast<NumberBox*>(box(amount, 1, x0 + 232, y0, x0 + 318, y0 + 26, pg))->dim = [this, target] { return std::lround(get(*target)) == 0; };
        }
    }
    text("Range", 840, 238, 910, 264, pg);
    box(param("Globals/PitchBendRange", linear(0, 48, 5, Unit::SemisSigned), true), 0, 912, 238, 998, 264, pg);
    text("Range", 840, 274, 910, 300, pg);
    box(param("Globals/MpePitchBendRange", linear(0, 96, 48, Unit::SemisSigned), true), 0, 912, 274, 998, 300, pg);
}

// ---------------------------------------------------------------- title
void Face::buildTitle(Widget& box)
{
    struct TabSpec { const char* name; float x0, x1, textX; std::vector<std::pair<float, std::function<bool()>>> dots; };
    const auto lit = [this](Param* p) { return std::function<bool()>([this, p] { return on(p); }); };
    const auto midiUsed = [this] {
        for (auto* p : midiConnections) if (std::lround(get(*p)) != 0) return true;
        return false;
    };
    std::vector<TabSpec> tabs{
        {"Sample", 666, 756, 0, {}},
        {"Pitch/Osc", 760, 896, 20, {{8, lit(pitchEnvOn)}, {127, lit(oscOn)}}},
        {"Filter/Global", 900, 1066, 34, {{8, lit(filterOn)}, {22, lit(shaperOn)}}},
        {"Modulation", 1070, 1248, 20, {{9, lit(auxOn)}, {141, lit(lfoOn[0])}, {154, lit(lfoOn[1])}, {168, lit(lfoOn[2])}}},
        {"MIDI", 1252, 1326, 20, {{8, midiUsed}}},
    };
    for (size_t i = 0; i < tabs.size(); ++i)
    {
        auto* t = box.add<TitleTab>();
        t->label = tabs[i].name;
        t->unit = [this] { return titleUnit(); };
        t->dots = tabs[i].dots;
        t->textX = tabs[i].textX;
        const int page = (int) i;
        t->selected = [this, page] { return this->page() == page; };
        t->onSelect = [this, page] { selectPage(page); };
        t->tooltip = std::string("Show the ") + tabs[i].name + " tab";
        titleWidgets_.push_back({t, tabs[i].x0, 14, tabs[i].x1, 44});
    }
    auto* zone = box.add<TitleTab>();
    zone->label = "Zone";
    zone->unit = [this] { return titleUnit(); };
    zone->textX = 24;
    zone->dots = {{11, [this] { return model_.sampleParts.size() > 1; }}};
    zone->arrow = [this] { return zoneEditor() ? 2 : 1; };
    zone->onSelect = [this] { set(*zoneVisible_, zoneEditor() ? 0 : 1); };
    zone->tooltip = "Show or hide the zone editor";
    titleWidgets_.push_back({zone, 566, 14, 662, 40});
}

// ---------------------------------------------------------------- layout and painting
void Face::layout()
{
    ReferenceFace::layout();
    const float k = titleUnit(), right = bounds().right();
    for (auto& [widget, x0, y0, x1, y1] : titleWidgets_)
        widget->setBounds({right - (kRight - x0) * k, (y0 - kTitleTop) * k, (x1 - x0) * k, (y1 - y0) * k});
}

void Face::paint(SkCanvas& c, const Theme& t)
{
    fillBody(c, t, false);
    const int pg = page();
    const auto rule = SkColorSetRGB(0x58, 0x5b, 0x66);
    const auto hline = [&](float x0, float x1, float y) { const auto r = at(x0, y, x1, y + 2); paint::fill(c, r, rule); };
    const auto vline = [&](float x, float y0, float y1) { const auto r = at(x, y0, x + 2, y1); paint::fill(c, r, rule); };
    if (pg == 1) { hline(32, 1414, 214); vline(1046, 246, 350); }
    if (pg == 2)
    {
        hline(32, 58, 324);
        hline(96, 912, 324);
        vline(926, 60, 376);
        const auto mark = at(921, 116, 931, 128);
        ctl::triangle(c, mark.centreX(), mark.centreY(), mark.w, mark.h, ctl::Dir::Right, paint::mix(t.color("TextDisabled"), t.color("SurfaceBackground"), 0.3f));
    }
    if (pg == 3) { vline(516, 60, 376); vline(820, 60, 376); vline(1124, 60, 376); }
    if (pg == 4)
    {
        for (int row = 3; row < 9; ++row) led(c, t, 45, 71 + 36.0f * (float) row, 9, false);
        paintMidiLogo(c, t);
    }
    for (const auto& l : labels_)
        if ((l.page < 0 || l.page == pg) && (! l.shown || l.shown())) label(c, t, l.text, l.x0, l.y0, l.x1, l.y1, l.align);
}

// The reference shows its product logo here; Sonora draws its own wordmark and the device's name instead.
void Face::paintMidiLogo(SkCanvas& c, const Theme&)
{
    const auto r = at(1006, 62, 1410, 368);
    paint::fillRounded(c, r, 6 * unit(), SkColorSetRGB(0x0a, 0x0c, 0x15));
    // Measured from the reference panel: the mark-and-name line is 300 px wide with 32-px capitals, centred.
    const float cap = 30 * unit(), size = cap * 1.38f, gap = 16 * unit();
    float wordmark = 0;
    gfx::brand::wordmarkStrokes(wordmark);
    wordmark *= cap;
    const float total = wordmark + gap + gfx::measureText("Sampler", size), x = r.centreX() - total * 0.5f;
    const auto white = SkColorSetRGB(0xf4, 0xf4, 0xf4);
    gfx::brand::drawWordmark(c, x, r.centreY() + cap * 0.5f, cap, white);
    paint::text(c, "Sampler", {x + wordmark + gap, r.centreY() - 30 * unit(), total, 60 * unit()}, white, size);
}
} // namespace

// ---------------------------------------------------------------- public
ReferenceFace* addSamplerFace(Widget& box, const model::Device& model, devices::DeviceProcessor* processor, DeviceLocation where,
                              ReferenceFace::Tag tag)
{
    if (model.type != "MultiSampler" || processor == nullptr) return nullptr;
    return box.add<Face>(box, model, processor, where, std::move(tag));
}

int samplerFacePage(const Widget& face)
{
    if (const auto* f = dynamic_cast<const Face*>(&face)) return f->page();
    return -1;
}

void selectSamplerFacePage(Widget& face, int page)
{
    if (auto* f = dynamic_cast<Face*>(&face)) f->selectPage(page);
}
} // namespace sonora::ui
