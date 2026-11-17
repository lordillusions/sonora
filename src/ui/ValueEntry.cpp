#include "ui/ValueEntry.h"

#include "gfx/Fonts.h"
#include "ui/Theme.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

namespace sonora::ui
{

namespace
{
constexpr double kInf = std::numeric_limits<double>::infinity();

std::string lower(std::string_view s)
{
    std::string out(s);
    for (auto& c : out) c = (char) std::tolower((unsigned char) c);
    return out;
}

std::string_view trim(std::string_view s)
{
    while (! s.empty() && std::isspace((unsigned char) s.front())) s.remove_prefix(1);
    while (! s.empty() && std::isspace((unsigned char) s.back())) s.remove_suffix(1);
    return s;
}

bool isWord(std::string_view s)
{
    if (s.empty()) return false;
    for (const char c : s)
        if (! (std::isalpha((unsigned char) c) || c == ' ' || c == '-' || c == '_' || (unsigned char) c >= 0x80)) return false;
    return true;
}

// The unit a suffix names, with its multiplier to the unit's base. `context` settles "s" (seconds or Side) and "m".
bool unitOf(std::string_view suffix, ValueUnit context, ValueUnit& unit, double& multiplier, double& sign)
{
    multiplier = 1;
    sign = 1;
    const auto s = lower(suffix);
    const bool pan = context == ValueUnit::Pan;
    // Utility's Mid/Side readout prints "40M" / "40S"; a time dial reads a typed "S" as seconds.
    if ((suffix == "M" || suffix == "S") && context != ValueUnit::Milliseconds)
    {
        unit = ValueUnit::Pan;
        sign = suffix == "M" ? -1 : 1;
        return true;
    }
    if (s == "ms") { unit = ValueUnit::Milliseconds; return true; }
    if (s == "s" && ! pan) { unit = ValueUnit::Milliseconds; multiplier = 1000; return true; }
    if (s == "sec" || s == "secs" || s == "seconds") { unit = ValueUnit::Milliseconds; multiplier = 1000; return true; }
    if (s == "us" || s == "\xc2\xb5s") { unit = ValueUnit::Milliseconds; multiplier = 0.001; return true; }
    if (s == "hz") { unit = ValueUnit::Hertz; return true; }
    if (s == "khz" || (s == "k" && context != ValueUnit::Milliseconds)) { unit = ValueUnit::Hertz; multiplier = 1000; return true; }
    if (s == "db") { unit = ValueUnit::Decibels; return true; }
    if (s == "%") { unit = ValueUnit::Percent; return true; }
    if (s == "st" || s == "semi" || s == "semis" || s == "semitone" || s == "semitones") { unit = ValueUnit::Semitones; return true; }
    if (s == "ct" || s == "cent" || s == "cents") { unit = ValueUnit::Cents; return true; }
    if (s == "bpm") { unit = ValueUnit::Bpm; return true; }
    if (s == "\xc2\xb0" || s == "deg" || s == "degrees") { unit = ValueUnit::Degrees; return true; }
    if (s == "l" || (s == "m" && pan)) { unit = ValueUnit::Pan; sign = -1; return true; }
    if (s == "r" || (s == "s" && pan)) { unit = ValueUnit::Pan; return true; }
    if (isWord(suffix)) { unit = ValueUnit::Other; return true; }
    return false;
}
} // namespace

ValueUnit valueUnitFromName(std::string_view name)
{
    ValueUnit unit = ValueUnit::None;
    double multiplier = 1, sign = 1;
    if (name.empty() || ! unitOf(name, ValueUnit::None, unit, multiplier, sign) || unit == ValueUnit::Other) return ValueUnit::None;
    return unit;
}

std::optional<ValueReading> readValue(std::string_view text, ValueUnit context, double contextMultiplier)
{
    text = trim(text);
    if (text.empty()) return std::nullopt;
    const auto low = lower(text);
    if (low == "c" || low == "center" || low == "centre")
        return ValueReading{0, ValueUnit::Pan, true, 1, 1};
    size_t i = 0;
    double sign = 1;
    if (text[i] == '+' || text[i] == '-') { sign = text[i] == '-' ? -1 : 1; ++i; }
    while (i < text.size() && text[i] == ' ') ++i;
    ValueReading reading;
    double magnitude = 0;
    int decimals = 0;
    if (low.compare(i, 3, "inf") == 0)
    {
        magnitude = kInf;
        i += 3;
    }
    else
    {
        bool digits = false, point = false;
        for (; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c >= '0' && c <= '9')
            {
                digits = true;
                if (point) { ++decimals; magnitude += (c - '0') * std::pow(10.0, -decimals); }
                else magnitude = magnitude * 10 + (c - '0');
            }
            else if ((c == '.' || c == ',') && ! point) point = true;
            else break;
        }
        if (! digits) return std::nullopt;
    }
    const auto suffix = trim(text.substr(i));
    double multiplier = 1, suffixSign = 1;
    if (suffix.empty())
    {
        reading.unit = context;
        multiplier = contextMultiplier > 0 ? contextMultiplier : 1;
    }
    else
    {
        if (! unitOf(suffix, context, reading.unit, multiplier, suffixSign)) return std::nullopt;
        reading.unitWritten = true;
    }
    reading.multiplier = multiplier;
    reading.value = sign * suffixSign * magnitude * multiplier;
    reading.resolution = std::pow(10.0, -decimals) * multiplier;
    if (! (std::abs(reading.value) > 0)) reading.value = 0.0; // no -0
    return reading;
}

// ------------------------------------------------------------------------------------------------ ValueLaw

float ValueLaw::valueAtNorm(float n) const
{
    n = std::clamp(n, 0.0f, 1.0f);
    if (range_.fromNorm) return range_.fromNorm(n);
    const float lo = range_.minValue, hi = range_.maxValue;
    if (range_.logarithmic && lo > 0 && hi > lo) return lo * std::pow(hi / lo, n);
    return lo + n * (hi - lo);
}

float ValueLaw::clampValue(float v) const
{
    return std::clamp(v, std::min(range_.minValue, range_.maxValue), std::max(range_.minValue, range_.maxValue));
}

std::optional<double> ValueLaw::shownAtNorm(float n) const
{
    if (! range_.format) return std::nullopt;
    const auto r = readValue(range_.format(valueAtNorm(n)), unit_);
    if (! r || (r->unit != unit_ && ! (unit_ == ValueUnit::None && ! r->unitWritten))) return std::nullopt;
    return r->value;
}

ValueLaw ValueLaw::detect(const DialRange& range)
{
    ValueLaw law;
    law.range_ = range;
    if (! range.format) return law;
    constexpr int kSamples = 17;
    std::array<float, kSamples> values {};
    std::array<std::optional<ValueReading>, kSamples> readings;
    std::map<ValueUnit, int> units;
    for (int i = 0; i < kSamples; ++i)
    {
        values[(size_t) i] = law.valueAtNorm((float) i / (kSamples - 1));
        readings[(size_t) i] = readValue(range.format(values[(size_t) i]), range.hint);
        if (readings[(size_t) i]) ++units[readings[(size_t) i]->unit];
    }
    if (units.empty()) return law;
    // The unit most positions show ("Off" at one end does not count).
    law.unit_ = std::max_element(units.begin(), units.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    std::vector<int> used;
    for (int i = 0; i < kSamples; ++i)
        if (readings[(size_t) i] && readings[(size_t) i]->unit == law.unit_) used.push_back(i);
    if ((int) used.size() < kSamples - 4) return law;
    // Monotonic, and not constant (a formatter that ignores its argument shows one value everywhere).
    bool up = true, down = true;
    double first = kInf, last = kInf, lowest = kInf, highest = -kInf;
    for (size_t k = 0; k < used.size(); ++k)
    {
        const double v = readings[(size_t) used[k]]->value;
        if (k > 0)
        {
            const double previous = readings[(size_t) used[k - 1]]->value;
            up &= v >= previous;
            down &= v <= previous;
        }
        if (! std::isfinite(v)) continue;
        if (! std::isfinite(first)) first = v;
        last = v;
        lowest = std::min(lowest, v);
        highest = std::max(highest, v);
    }
    if (! (up || down) || ! std::isfinite(first) || ! (highest > lowest)) return law;
    law.numeric_ = true;
    law.increasing_ = last > first;
    law.shownMin_ = lowest;
    law.shownMax_ = highest;
    law.integral_ = std::all_of(used.begin(), used.end(), [&](int i) {
        const auto& r = *readings[(size_t) i];
        return ! std::isfinite(r.value) || r.resolution >= r.multiplier * 0.999;
    });

    // A straight line through the first and last finite readings, snapped to a power of ten when it is one within the
    // printing's rounding (ms shown for seconds, % for 0..1), then checked against every reading.
    int a = -1, b = -1;
    for (const int i : used)
        if (std::isfinite(readings[(size_t) i]->value)) { if (a < 0) a = i; b = i; }
    const double va = values[(size_t) a], vb = values[(size_t) b];
    if (a == b || ! (std::abs(vb - va) > 0)) return law;
    double scale = (readings[(size_t) b]->value - readings[(size_t) a]->value) / (vb - va);
    if (std::abs(scale) > 0)
    {
        const double decade = std::pow(10.0, std::round(std::log10(std::abs(scale))));
        if (std::abs(std::abs(scale) / decade - 1) < 0.01) scale = std::copysign(decade, scale);
    }
    double offset = 0;
    for (const int i : used)
        if (std::isfinite(readings[(size_t) i]->value)) offset += readings[(size_t) i]->value - scale * values[(size_t) i];
    int finite = 0;
    for (const int i : used) finite += std::isfinite(readings[(size_t) i]->value) ? 1 : 0;
    offset /= finite;
    if (std::abs(offset) < 1e-4 * std::max(std::abs(highest), std::abs(lowest))) offset = 0;
    bool fits = std::abs(scale) > 0;
    for (const int i : used)
    {
        const auto& r = *readings[(size_t) i];
        if (! std::isfinite(r.value)) continue;
        const double predicted = scale * values[(size_t) i] + offset;
        fits &= std::abs(predicted - r.value) <= r.resolution * 0.51 + 1e-6 * std::abs(r.value) + 1e-9;
    }
    if (fits)
    {
        law.linear_ = true;
        law.scale_ = scale;
        law.offset_ = offset;
    }
    return law;
}

double ValueLaw::displayOf(float value) const
{
    if (! numeric_) return std::numeric_limits<double>::quiet_NaN();
    if (linear_) return scale_ * value + offset_;
    const auto r = range_.format ? readValue(range_.format(value), unit_) : std::nullopt;
    return r && r->unit == unit_ ? r->value : std::numeric_limits<double>::quiet_NaN();
}

double ValueLaw::step(double shown, bool fine) const
{
    if (! numeric_ || ! std::isfinite(shown)) return 0;
    const double a = std::abs(shown), span = shownMax_ - shownMin_;
    double coarse = 0, small = 0;
    switch (unit_)
    {
        case ValueUnit::Semitones:
        case ValueUnit::Cents:
        case ValueUnit::Pan: coarse = small = 1; break;
        case ValueUnit::Bpm:
        case ValueUnit::Degrees: coarse = 1; small = 0.1; break;
        case ValueUnit::Milliseconds:
            coarse = a < 1 ? 0.01 : a < 10 ? 0.1 : a < 1000 ? 1 : 10;
            small = std::max(0.001, coarse / 10);
            break;
        case ValueUnit::Hertz: // three significant digits, as the dials print them (0.50 Hz, 50.0 Hz, 500 Hz, 5.00 kHz)
            coarse = std::max(0.01, std::pow(10.0, std::floor(std::log10(std::max(a, 1e-6))) - 2));
            small = std::max(0.001, coarse / 10);
            break;
        case ValueUnit::Decibels: coarse = 0.1; small = 0.01; break;
        case ValueUnit::Percent:
            coarse = span >= 20 ? 1 : 0.1;
            small = coarse / 10;
            break;
        case ValueUnit::None:
        case ValueUnit::Other:
            if (integral_) coarse = small = 1;
            break;
    }
    if (coarse <= 0 || span / coarse < 3) return 0; // a range of a few steps stays continuous
    return fine ? small : coarse;
}

float ValueLaw::valueFor(double shown) const
{
    if (! numeric_) return clampValue(range_.minValue);
    if (std::isinf(shown))
        return valueAtNorm((shown < 0) == increasing_ ? 0.0f : 1.0f);
    if (linear_)
    {
        // The line holds where the dial was sampled; check the answer against the printed text (Utility's Gain turns to
        // linear amplitude below -33 dB) and search the text when it disagrees.
        const float v = clampValue((float) ((shown - offset_) / scale_));
        const auto back = readValue(range_.format(v), unit_);
        if (back && back->unit == unit_ && std::abs(back->value - shown) <= back->resolution * 0.51 + 1e-6 * std::abs(shown))
            return v;
    }
    // Search the printed text: the middle of the positions that print `shown` (its plateau), or the nearest edge.
    const double target = increasing_ ? shown : -shown;
    auto at = [&](float n) {
        const auto v = shownAtNorm(n);
        if (! v) return n < 0.5f ? -kInf : kInf;
        return increasing_ ? *v : -*v;
    };
    auto firstAtLeast = [&](double t, bool strictly) {
        float lo = 0, hi = 1;
        if (strictly ? at(1) <= t : at(1) < t) return 1.0f;
        for (int k = 0; k < 30; ++k)
        {
            const float mid = (lo + hi) * 0.5f;
            if (strictly ? at(mid) > t : at(mid) >= t) hi = mid;
            else lo = mid;
        }
        return hi;
    };
    const float from = firstAtLeast(target, false);
    const double found = at(from);
    if (std::abs(found - target) <= 1e-9 * std::max(1.0, std::abs(target)))
    {
        const float to = firstAtLeast(target, true);
        return clampValue(valueAtNorm((from + to) * 0.5f));
    }
    return clampValue(valueAtNorm(from));
}

float ValueLaw::quantize(float value, bool fine) const
{
    const double shown = displayOf(value);
    const double s = step(shown, fine);
    // Outside what the samples saw (Utility's Gain under -33 dB, before "-inf"), the value moves freely.
    if (! (s > 0) || shown < shownMin_ || shown > shownMax_) return value;
    const double target = std::clamp(std::round(shown / s) * s, shownMin_, shownMax_);
    if (linear_ && std::abs(target - shown) < 1e-9 * std::max(1.0, std::abs(shown))) return value;
    return valueFor(target);
}

float ValueLaw::nudge(float value, int direction, bool fine) const
{
    const double shown = displayOf(value);
    const double s = step(shown, fine);
    if (! (s > 0)) return std::numeric_limits<float>::quiet_NaN();
    const double towards = increasing_ ? direction : -direction;
    // Going down across a size boundary (10 ms -> 9.9 ms) takes the smaller step of the side it lands on.
    const double below = step(shown + towards * s * 0.5, fine);
    const double by = below > 0 ? std::min(s, below) : s;
    return valueFor(std::round(shown / by) * by + towards * by); // past an end: valueFor stops at it
}

std::optional<float> ValueLaw::fromText(std::string_view typed, float current) const
{
    typed = trim(typed);
    if (typed.empty() || ! range_.format) return std::nullopt;
    if (numeric_)
    {
        double context = 1;
        if (const auto now = readValue(range_.format(current), unit_); now && now->unit == unit_) context = now->multiplier;
        if (auto r = readValue(typed, unit_, context))
        {
            const bool fitsUnit = r->unit == unit_ || unit_ == ValueUnit::None || unit_ == ValueUnit::Other
                                  || (unit_ == ValueUnit::Pan && r->unit == ValueUnit::None);
            if (fitsUnit)
            {
                double v = r->value;
                if (std::isfinite(v) && (unit_ == ValueUnit::Semitones || unit_ == ValueUnit::Cents || unit_ == ValueUnit::Pan
                                         || ((unit_ == ValueUnit::None || unit_ == ValueUnit::Other) && integral_)))
                    v = std::round(v);
                return valueFor(v); // out of range: the nearest end
            }
        }
    }
    // Words and anything else: the position whose text is the typed one ("Off", "1/16", "Sine"), or starts with it.
    const auto wanted = lower(typed);
    const float lo = std::min(range_.minValue, range_.maxValue), hi = std::max(range_.minValue, range_.maxValue);
    const bool whole = ! range_.fromNorm && std::abs(lo - std::round(lo)) < 1e-6f && std::abs(hi - std::round(hi)) < 1e-6f && hi - lo <= 512;
    const int count = whole ? (int) std::lround(hi - lo) + 1 : 257;
    std::optional<float> prefix;
    for (int i = 0; i < count; ++i)
    {
        const float v = whole ? lo + (float) i : valueAtNorm((float) i / (float) (count - 1));
        const auto shown = lower(trim(range_.format(v)));
        if (shown == wanted) return v;
        if (! prefix && shown.rfind(wanted, 0) == 0) prefix = v;
    }
    return prefix;
}

// ------------------------------------------------------------------------------------------------ the field

bool ValueEntry::active() const { return owner != nullptr && Widget::isAlive(owner); }

Rect ValueEntry::boundsInRoot() const
{
    if (! active()) return {};
    const auto origin = owner->boundsInRoot();
    const float size = fontSize > 0 ? fontSize : 10.0f;
    const float h = std::max(local.h, size + 5);
    const float w = std::max({local.w, gfx::measureText(edit.text, size) + 10, size * 4});
    return {origin.x + local.centreX() - w * 0.5f, origin.y + local.centreY() - h * 0.5f, w, h};
}

void beginValueEntry(Widget& owner, const Rect& local, const std::string& initial, std::function<void(const std::string&)> commit,
                     uint32_t firstChar, float fontSize)
{
    auto& ctx = owner.ctx();
    if (ctx.valueEntry.active()) commitValueEntry(ctx);
    ctx.valueEntry.owner = &owner;
    ctx.valueEntry.local = local;
    ctx.valueEntry.commit = std::move(commit);
    ctx.valueEntry.fontSize = fontSize > 0 ? fontSize : ctx.theme.metrics.smallFontSize;
    ctx.valueEntry.edit.start(initial);
    ctx.valueEntry.initial = firstChar != 0 ? std::string() : initial;
    if (firstChar != 0)
    {
        ctx.valueEntry.edit.clear();
        ctx.valueEntry.edit.insert(juce::String::charToString((juce::juce_wchar) firstChar).toStdString());
    }
    if (ctx.requestRepaint) ctx.requestRepaint();
}

bool valueEntryKey(UiContext& ctx, const KeyEvent& k)
{
    if (! ctx.valueEntry.active()) { cancelValueEntry(ctx); return true; }
    switch (ctx.valueEntry.edit.key(k))
    {
        case InlineTextEdit::Result::Commit: commitValueEntry(ctx); break;
        case InlineTextEdit::Result::Cancel: cancelValueEntry(ctx); break;
        case InlineTextEdit::Result::Edited: break;
    }
    if (ctx.requestRepaint) ctx.requestRepaint();
    return true;
}

void commitValueEntry(UiContext& ctx)
{
    const bool alive = ctx.valueEntry.active();
    auto commit = std::move(ctx.valueEntry.commit);
    const auto text = std::move(ctx.valueEntry.edit.text);
    const bool unchanged = text == ctx.valueEntry.initial;
    ctx.valueEntry = {};
    if (alive && commit && ! unchanged) commit(text);
    if (ctx.requestRepaint) ctx.requestRepaint();
}

void cancelValueEntry(UiContext& ctx)
{
    ctx.valueEntry = {};
    if (ctx.requestRepaint) ctx.requestRepaint();
}

void paintValueEntry(SkCanvas& c, const UiContext& ctx, const Theme& t)
{
    if (! ctx.valueEntry.active()) return;
    const auto r = ctx.valueEntry.boundsInRoot();
    ctx.valueEntry.edit.paint(c, r, t.color("ControlTextBack"), t.color("ControlForeground"), t.color("RangeEditField"),
                              t.color("ControlForeground"), ctx.valueEntry.fontSize);
}

bool startsValueEntry(const KeyEvent& k)
{
    if (k.cmd || k.ctrl || k.alt) return false;
    const auto c = k.textChar;
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == ',';
}

} // namespace sonora::ui
