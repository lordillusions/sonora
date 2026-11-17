#include "ui/views/ProFaceParts.h"

#include "ui/views/ProFace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
SkColor mix(SkColor a, SkColor b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto channel = [t](unsigned x, unsigned y) { return (unsigned) std::lround((float) x + ((float) y - (float) x) * t); };
    return SkColorSetARGB(channel(SkColorGetA(a), SkColorGetA(b)), channel(SkColorGetR(a), SkColorGetR(b)),
                          channel(SkColorGetG(a), SkColorGetG(b)), channel(SkColorGetB(a), SkColorGetB(b)));
}

void hatch(SkCanvas& c, const Rect& r, SkColor colour)
{
    c.save();
    c.clipRect(r.sk());
    for (float x = r.x - r.h; x < r.right(); x += 6.0f) c.drawLine(x, r.bottom(), x + r.h, r.y, pro::stroke(colour, 1.0f));
    c.restore();
}

void arrow(SkCanvas& c, const Rect& r, bool left, SkColor colour)
{
    SkPathBuilder p;
    const float cx = r.centreX(), cy = r.centreY(), s = std::min(r.w, r.h) * 0.22f;
    if (left)
    {
        p.moveTo(cx + s * 0.6f, cy - s);
        p.lineTo(cx - s * 0.6f, cy);
        p.lineTo(cx + s * 0.6f, cy + s);
    }
    else
    {
        p.moveTo(cx - s * 0.6f, cy - s);
        p.lineTo(cx + s * 0.6f, cy);
        p.lineTo(cx - s * 0.6f, cy + s);
    }
    c.drawPath(p.detach(), pro::stroke(colour, 2.0f));
}

void dice(SkCanvas& c, const Rect& r, SkColor colour)
{
    const float s = std::min(r.w, r.h) * 0.62f;
    const Rect face{r.centreX() - s * 0.5f, r.centreY() - s * 0.5f, s, s};
    pro::roundedStroke(c, face, s * 0.2f, colour, 1.6f);
    const float d = s * 0.25f, dot = std::max(1.4f, s * 0.08f);
    for (const auto& [dx, dy] : {std::pair<float, float>{-d, -d}, {d, -d}, {0, 0}, {-d, d}, {d, d}})
        c.drawCircle(face.centreX() + dx, face.centreY() + dy, dot, pro::fill(colour));
}
} // namespace

// ------------------------------------------------------------------------------------------------------------ StepLane
Rect StepLane::cellRect(int step) const
{
    const float x0 = bounds.x + labelWidth, width = std::max(1.0f, bounds.w - labelWidth);
    const int n = std::max(1, steps);
    const float cell = width / (float) n;
    const float gap = cell > 10 ? 2.0f : 1.0f;
    return {x0 + cell * (float) step + gap * 0.5f, bounds.y + 1, cell - gap, bounds.h - 2};
}

int StepLane::stepAt(float x) const
{
    const float x0 = bounds.x + labelWidth, width = bounds.w - labelWidth;
    if (x < x0 || x >= x0 + width || width <= 0) return -1;
    return std::clamp((int) ((x - x0) / width * (float) std::max(1, steps)), 0, std::max(1, steps) - 1);
}

float StepLane::clampValue(float v) const
{
    switch (kind)
    {
        case Kind::Toggle: return v >= 0.5f ? 1.0f : 0.0f;
        case Kind::Choice: return std::clamp(std::round(v), 0.0f, (float) std::max(0, (int) choices.size() - 1));
        case Kind::Bar: break;
    }
    v = std::clamp(v, std::min(minValue, maxValue), std::max(minValue, maxValue));
    return integral ? std::round(v) : v;
}

float StepLane::barValueAt(float y) const
{
    const Rect cell = cellRect(0);
    const float pad = std::min(3.0f, cell.h * 0.1f); // the top and bottom few pixels are the extremes
    const float t = 1.0f - (y - cell.y - pad) / std::max(1.0f, cell.h - 2 * pad);
    return clampValue(minValue + std::clamp(t, 0.0f, 1.0f) * (maxValue - minValue));
}

void StepLane::apply(int step, float value)
{
    if (step < 0 || step >= steps || ! set) return;
    value = clampValue(value);
    if (get && std::abs(get(step) - value) < 1e-6f) return;
    set(step, value);
    notify();
}

void StepLane::mouseDown(const MouseEvent& e)
{
    const int step = stepAt(e.x);
    dragging_ = step >= 0;
    lastStep_ = step;
    if (step < 0) return;
    if (e.alt)
    {
        paintValue_ = clampValue(defaultValue);
        apply(step, paintValue_);
        return;
    }
    const float now = get ? get(step) : 0.0f;
    switch (kind)
    {
        case Kind::Toggle: paintValue_ = now >= 0.5f ? 0.0f : 1.0f; break;
        case Kind::Bar: paintValue_ = barValueAt(e.y); break;
        case Kind::Choice:
        {
            const int n = std::max(1, (int) choices.size());
            paintValue_ = (float) ((((int) std::lround(now) + (e.shift ? -1 : 1)) % n + n) % n);
            break;
        }
    }
    apply(step, paintValue_);
}

void StepLane::mouseDrag(const MouseEvent& e)
{
    if (! dragging_) return;
    const int step = std::clamp(stepAt(std::clamp(e.x, bounds.x + labelWidth, bounds.right() - 0.01f)), 0, steps - 1);
    const float value = kind == Kind::Bar && ! e.alt ? barValueAt(e.y) : paintValue_;
    // Paint every cell between the last one and this one (a fast drag skips cells).
    const int from = lastStep_ < 0 ? step : lastStep_;
    const int dir = step >= from ? 1 : -1;
    for (int s = from;; s += dir)
    {
        apply(s, value);
        if (s == step) break;
    }
    lastStep_ = step;
}

void StepLane::mouseUp(const MouseEvent&)
{
    dragging_ = false;
    lastStep_ = -1;
}

void StepLane::paint(SkCanvas& c)
{
    pro::well(c, bounds, 4.0f);
    if (labelWidth > 0 && ! label.empty())
        pro::text(c, label, {bounds.x + 8, bounds.y, labelWidth - 10, bounds.h}, pro::kDim, 10.5f, Align::Left, true);
    const int playing = playStep ? playStep() : -1;
    for (int s = 0; s < steps; ++s)
    {
        const Rect r = cellRect(s);
        const bool strong = beatEvery > 0 && s % beatEvery == 0;
        pro::rounded(c, r, 2.5f, strong ? 0xFF262C35 : 0xFF1D2229);
        const float v = get ? get(s) : 0.0f;
        switch (kind)
        {
            case Kind::Toggle:
                if (v >= 0.5f) pro::rounded(c, r.reduced(1.5f), 2.0f, accent);
                break;
            case Kind::Bar:
            {
                const float span = maxValue - minValue;
                const float t = std::abs(span) > 1e-9f ? std::clamp((v - minValue) / span, 0.0f, 1.0f) : 0.0f;
                if (bipolar)
                {
                    const float mid = r.y + r.h * 0.5f, top = r.y + r.h * (1.0f - t);
                    const Rect bar{r.x + 1, std::min(mid, top), r.w - 2, std::max(1.5f, std::abs(mid - top))};
                    pro::rounded(c, bar, 1.5f, accent);
                    c.drawLine(r.x, mid, r.right(), mid, pro::stroke(0x22FFFFFF, 1.0f));
                }
                else if (t > 0.0f)
                {
                    const float h = std::max(2.0f, (r.h - 2) * t);
                    pro::rounded(c, {r.x + 1, r.bottom() - 1 - h, r.w - 2, h}, 1.5f, mix(paint::withAlpha(accent, 150), accent, t));
                }
                break;
            }
            case Kind::Choice:
            {
                const int index = std::clamp((int) std::lround(v), 0, std::max(0, (int) choices.size() - 1));
                if (index > 0) pro::rounded(c, r.reduced(1.5f), 2.0f, paint::withAlpha(accent, 70));
                if (! choices.empty())
                    pro::text(c, choices[(size_t) index], r, index > 0 ? pro::kText : pro::kFaint, std::min(11.0f, r.h * 0.5f), Align::Centre, true);
                break;
            }
        }
        if (disabled && disabled(s)) hatch(c, r, kind == Kind::Toggle && v >= 0.5f ? 0x50000000 : 0x30FFFFFF); // over the value
        if (s == playing) pro::roundedStroke(c, r, 2.5f, pro::kText, 1.5f);
    }
}

// ------------------------------------------------------------------------------------------------------------ NoteRoll
std::pair<int, int> NoteRoll::pitchRange() const
{
    int low = lowPitch, high = highPitch;
    if (low < 0 || high < 0)
    {
        int lo = 127, hi = 0;
        for (const auto& n : notes)
        {
            lo = std::min(lo, n.pitch);
            hi = std::max(hi, n.pitch);
        }
        if (notes.empty()) { lo = 48; hi = 72; }
        if (hi - lo < 12)
        {
            const int pad = (12 - (hi - lo) + 1) / 2;
            lo -= pad;
            hi += pad;
        }
        if (low < 0) low = std::max(0, lo - 1);
        if (high < 0) high = std::min(127, hi + 1);
    }
    return {low, std::max(low + 1, high)};
}

void NoteRoll::paint(SkCanvas& c)
{
    pro::well(c, bounds, 6.0f);
    const Rect area = bounds.reduced(6.0f, 5.0f);
    if (lengthBeats <= 0 || area.w <= 0 || area.h <= 0) return;
    const auto [low, high] = pitchRange();
    const float pxPerBeat = area.w / (float) lengthBeats;
    const float row = area.h / (float) (high - low + 1);
    // Rows of the black keys, faintly.
    for (int p = low; p <= high; ++p)
    {
        const int pc = p % 12;
        if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
            c.drawRect(SkRect::MakeXYWH(area.x, area.bottom() - row * (float) (p - low + 1), area.w, row), pro::fill(0x0AFFFFFF));
    }
    for (double b = 0; b < lengthBeats + 1e-9; b += 1.0)
    {
        const bool bar = beatsPerBar > 0 && std::fmod(b + 1e-9, beatsPerBar) < 1e-6;
        const float x = area.x + (float) b * pxPerBeat;
        c.drawLine(x, area.y, x, area.bottom(), pro::stroke(bar ? 0x30FFFFFF : 0x12FFFFFF, 1.0f));
    }
    if (notes.empty()) pro::text(c, emptyText, area, pro::kFaint, 12.0f, Align::Centre);
    for (const auto& n : notes)
    {
        if (n.pitch < low || n.pitch > high) continue;
        const float x = area.x + (float) n.start * pxPerBeat;
        const float w = std::max(2.0f, (float) n.length * pxPerBeat - 1.0f);
        const float y = area.bottom() - row * (float) (n.pitch - low + 1);
        const SkColor colour = colourOf ? colourOf(n) : mix(paint::withAlpha(accent, 110), accent, n.velocity / 127.0f);
        pro::rounded(c, {x, y + 0.5f, std::min(w, area.right() - x), std::max(1.5f, row - 1.0f)}, std::min(2.0f, row * 0.3f), colour);
    }
    if (playBeat)
        if (const double beat = playBeat(); beat >= 0 && beat < lengthBeats)
        {
            const float x = area.x + (float) beat * pxPerBeat;
            c.drawLine(x, bounds.y + 2, x, bounds.bottom() - 2, pro::stroke(pro::kText, 1.5f));
        }
}

// ----------------------------------------------------------------------------------------------------------- PresetBar
Rect PresetBar::previousRect() const { return {bounds.x, bounds.y, bounds.h, bounds.h}; }
Rect PresetBar::nextRect() const { return {bounds.x + bounds.h + 2, bounds.y, bounds.h, bounds.h}; }
Rect PresetBar::diceRect() const { return dice ? Rect{bounds.right() - bounds.h, bounds.y, bounds.h, bounds.h} : Rect{bounds.right(), bounds.y, 0, 0}; }
Rect PresetBar::nameRect() const
{
    const float left = nextRect().right() + 4;
    const float right = (dice ? diceRect().x - 4 : bounds.right());
    return {left, bounds.y, std::max(10.0f, right - left), bounds.h};
}

std::vector<std::string> PresetBar::categories() const
{
    std::vector<std::string> out;
    for (const auto& p : presets)
        if (std::find(out.begin(), out.end(), p.category) == out.end()) out.push_back(p.category);
    return out;
}

std::vector<int> PresetBar::presetsIn(int category) const
{
    std::vector<int> out;
    const auto all = categories();
    if (category < 0 || (size_t) category >= all.size()) return out;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].category == all[(size_t) category]) out.push_back((int) i);
    return out;
}

static constexpr float kMenuRow = 22.0f, kCategoryWidth = 130.0f, kPresetWidth = 220.0f;

Rect PresetBar::menuRect() const
{
    const auto name = nameRect();
    const int rows = std::max((int) categories().size(), (int) presetsIn(category_).size());
    const float width = kCategoryWidth + kPresetWidth;
    return {menuToLeft ? name.right() - width : name.x, name.bottom() + 4, width, kMenuRow * (float) std::max(1, rows) + 8};
}

Rect PresetBar::categoryRow(int index) const
{
    const auto m = menuRect();
    return {m.x + 4, m.y + 4 + kMenuRow * (float) index, kCategoryWidth - 6, kMenuRow};
}

Rect PresetBar::presetRow(int row) const
{
    const auto m = menuRect();
    return {m.x + kCategoryWidth + 2, m.y + 4 + kMenuRow * (float) row, kPresetWidth - 6, kMenuRow};
}

void PresetBar::step(int delta)
{
    if (presets.empty() || ! select) return;
    const int n = (int) presets.size();
    const int now = current ? current() : -1;
    const int next = now < 0 ? (delta > 0 ? 0 : n - 1) : ((now + delta) % n + n) % n;
    select(next);
    notify();
}

void PresetBar::mouseDown(const MouseEvent& e)
{
    if (open_)
    {
        const auto all = categories();
        for (int i = 0; i < (int) all.size(); ++i)
            if (categoryRow(i).contains(e.x, e.y))
            {
                category_ = i;
                return;
            }
        const auto list = presetsIn(category_);
        for (int row = 0; row < (int) list.size(); ++row)
            if (presetRow(row).contains(e.x, e.y))
            {
                open_ = false;
                if (select) select(list[(size_t) row]);
                notify();
                return;
            }
        open_ = false; // a click anywhere else closes the menu
        return;
    }
    if (previousRect().contains(e.x, e.y)) step(-1);
    else if (nextRect().contains(e.x, e.y)) step(1);
    else if (dice && diceRect().contains(e.x, e.y))
    {
        dice();
        notify();
    }
    else if (nameRect().contains(e.x, e.y) && ! presets.empty())
    {
        open_ = true;
        const int now = current ? current() : -1;
        const auto all = categories();
        category_ = 0;
        if (now >= 0)
            for (int i = 0; i < (int) all.size(); ++i)
                if (all[(size_t) i] == presets[(size_t) now].category) category_ = i;
    }
}

void PresetBar::paint(SkCanvas& c)
{
    for (const auto& r : {previousRect(), nextRect()}) pro::rounded(c, r, 5.0f, pro::kRaised);
    arrow(c, previousRect(), true, pro::kDim);
    arrow(c, nextRect(), false, pro::kDim);
    const auto name = nameRect();
    pro::well(c, name, 5.0f);
    const int now = current ? current() : -1;
    const bool valid = now >= 0 && (size_t) now < presets.size();
    const std::string text = valid ? presets[(size_t) now].name : "(edited)";
    pro::text(c, text, {name.x + 10, name.y, name.w - 34, name.h}, valid ? pro::kText : pro::kDim, 13.0f, Align::Left, true);
    if (valid && showCategory)
        pro::text(c, presets[(size_t) now].category, {name.x, name.y, name.w - 22, name.h}, pro::kFaint, 10.0f, Align::Right, true);
    // The menu's chevron.
    const float cx = name.right() - 12, cy = name.centreY();
    SkPathBuilder chevron;
    chevron.moveTo(cx - 4, cy - 2);
    chevron.lineTo(cx, cy + 2);
    chevron.lineTo(cx + 4, cy - 2);
    c.drawPath(chevron.detach(), pro::stroke(open_ ? accent : pro::kDim, 1.6f));
    if (dice)
    {
        pro::rounded(c, diceRect(), 5.0f, pro::kRaised);
        ui::dice(c, diceRect(), accent);
    }
}

void PresetBar::paintOverlay(SkCanvas& c)
{
    const auto m = menuRect();
    c.drawRRect(SkRRect::MakeRectXY(m.translated(0, 3).sk(), 7, 7), pro::fill(0x80000000));
    pro::rounded(c, m, 7.0f, 0xFF20242C);
    pro::roundedStroke(c, m, 7.0f, pro::kLine, 1.0f);
    c.drawLine(m.x + kCategoryWidth, m.y + 6, m.x + kCategoryWidth, m.bottom() - 6, pro::stroke(pro::kLine, 1.0f));
    const auto all = categories();
    const int now = current ? current() : -1;
    for (int i = 0; i < (int) all.size(); ++i)
    {
        const auto r = categoryRow(i);
        if (i == category_) pro::rounded(c, r, 4.0f, paint::withAlpha(accent, 60));
        pro::text(c, all[(size_t) i], r.reduced(8, 0), i == category_ ? pro::kText : pro::kDim, 12.0f, Align::Left, true);
    }
    const auto list = presetsIn(category_);
    for (int row = 0; row < (int) list.size(); ++row)
    {
        const auto r = presetRow(row);
        const bool on = list[(size_t) row] == now;
        if (on) pro::rounded(c, r, 4.0f, paint::withAlpha(accent, 90));
        pro::text(c, presets[(size_t) list[(size_t) row]].name, r.reduced(8, 0), on ? pro::kText : 0xFFC9CED8, 12.0f, Align::Left);
    }
}

// ---------------------------------------------------------------------------------------------------------- ChordStrip
static constexpr float kChipGap = 4.0f, kMinChip = 42.0f;

double ChordStrip::pixelsPerBeat() const
{
    const int n = count ? count() : 0;
    double total = 0;
    for (int i = 0; i < n; ++i) total += std::max(0.0, chip ? chip(i).beats : 0.0);
    const float available = bounds.w - (add ? bounds.h + kChipGap : 0.0f) - kChipGap * (float) std::max(0, n - 1);
    return total > 0 ? std::max(1.0, (double) available / total) : 1.0;
}

Rect ChordStrip::chipRect(int index) const
{
    const int n = count ? count() : 0;
    if (index < 0 || index >= n) return {};
    const double perBeat = pixelsPerBeat();
    // Widths by length, every chip at least kMinChip wide; when that overflows, everything is squeezed to fit.
    std::vector<float> widths((size_t) n);
    float total = 0;
    for (int i = 0; i < n; ++i)
    {
        widths[(size_t) i] = std::max(kMinChip, (float) (std::max(0.0, chip(i).beats) * perBeat));
        total += widths[(size_t) i];
    }
    const float available = bounds.w - (add && n < maxChips ? bounds.h + kChipGap : 0.0f) - kChipGap * (float) std::max(0, n - 1);
    const float squeeze = total > available && total > 0 ? available / total : 1.0f;
    float x = bounds.x;
    for (int i = 0; i < index; ++i) x += widths[(size_t) i] * squeeze + kChipGap;
    return {x, bounds.y, widths[(size_t) index] * squeeze, bounds.h};
}

Rect ChordStrip::addRect() const
{
    const int n = count ? count() : 0;
    if (! add || n >= maxChips) return {};
    const float x = n > 0 ? chipRect(n - 1).right() + kChipGap : bounds.x;
    return {x, bounds.y, bounds.h, bounds.h};
}

std::string ChordStrip::lengthText(double beats) const
{
    const double bars = beatsPerBar > 0 ? beats / beatsPerBar : beats;
    char buffer[32];
    if (bars < 1.0 - 1e-9)
    {
        if (std::abs(bars - 0.5) < 1e-9) return "1/2 bar";
        std::snprintf(buffer, sizeof buffer, "%g beats", beats);
    }
    else std::snprintf(buffer, sizeof buffer, std::abs(bars - 1.0) < 1e-9 ? "%g bar" : "%g bars", bars);
    return buffer;
}

void ChordStrip::mouseDown(const MouseEvent& e)
{
    resizing_ = -1;
    if (add && addRect().contains(e.x, e.y))
    {
        add();
        notify();
        return;
    }
    const int n = count ? count() : 0;
    for (int i = 0; i < n; ++i)
    {
        const auto r = chipRect(i);
        // The chip, or the gap after it (its right edge is grabbed there too).
        if (! (e.x >= r.x && e.x < r.right() + kChipGap && e.y >= r.y && e.y < r.bottom())) continue;
        if (resize && e.x >= r.right() - 7)
        {
            resizing_ = i;
            resizeStartX_ = e.x;
            resizeStartBeats_ = chip(i).beats;
            resizePerBeat_ = pixelsPerBeat(); // the scale at the start: the strip re-scales while the chip grows
            return;
        }
        if (select) select(i);
        notify();
        return;
    }
}

void ChordStrip::mouseDrag(const MouseEvent& e)
{
    if (resizing_ < 0 || ! resize || lengths.empty()) return;
    const double wanted = resizeStartBeats_ + (e.x - resizeStartX_) / resizePerBeat_;
    double best = lengths.front();
    for (double l : lengths)
        if (std::abs(l - wanted) < std::abs(best - wanted)) best = l;
    if (std::abs(best - chip(resizing_).beats) > 1e-9)
    {
        resize(resizing_, best);
        notify();
    }
}

void ChordStrip::mouseUp(const MouseEvent&) { resizing_ = -1; }

void ChordStrip::paint(SkCanvas& c)
{
    const int n = count ? count() : 0;
    const int lit = playing ? playing() : -1, chosen = selected ? selected() : -1;
    for (int i = 0; i < n; ++i)
    {
        const auto r = chipRect(i);
        const auto data = chip(i);
        pro::rounded(c, r, 6.0f, i == lit ? mix(pro::kRaised, accent, 0.45f) : pro::kRaised);
        if (i == chosen) pro::roundedStroke(c, r, 6.0f, accent, 2.0f);
        else pro::roundedStroke(c, r, 6.0f, pro::kLine, 1.0f);
        const float nameSize = std::min(15.0f, r.h * 0.36f);
        pro::text(c, data.name, {r.x + 4, r.y + 3, r.w - 8, r.h * 0.6f}, pro::kText, nameSize, Align::Centre, true);
        pro::text(c, lengthText(data.beats), {r.x + 4, r.y + r.h * 0.56f, r.w - 8, r.h * 0.38f}, i == lit ? pro::kText : pro::kDim,
                  std::min(10.5f, r.h * 0.26f), Align::Centre);
        if (resize) c.drawLine(r.right() - 3, r.y + r.h * 0.3f, r.right() - 3, r.bottom() - r.h * 0.3f, pro::stroke(0x40FFFFFF, 1.5f));
    }
    if (add && n < maxChips)
    {
        const auto r = addRect();
        pro::roundedStroke(c, r, 6.0f, pro::kLine, 1.0f);
        const float s = r.h * 0.22f;
        c.drawLine(r.centreX() - s, r.centreY(), r.centreX() + s, r.centreY(), pro::stroke(pro::kDim, 2.0f));
        c.drawLine(r.centreX(), r.centreY() - s, r.centreX(), r.centreY() + s, pro::stroke(pro::kDim, 2.0f));
    }
    if (n == 0 && ! add) pro::text(c, "No chords", bounds, pro::kFaint, 12.0f, Align::Centre);
}
} // namespace sonora::ui
