#include "ui/views/PhraseFace.h"

#include "core/PatternRandom.h"
#include "core/Scales.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
using devices::Phrase;
using model::PhrasePart;

namespace
{
constexpr SkColor kMajor = 0xFFE9B35F, kMinorColour = 0xFF35C2B0, kOther = 0xFF9AA1AF;
constexpr SkColor kFitChord = 0xFF8FE3D6, kFitScale = 0xFF7D8BA0, kFitOutside = 0xFFFF9F43;

std::string noteName(int pitch)
{
    return std::string(model::pitchClassName(pitch % 12)) + std::to_string(pitch / 12 - 1);
}

SkColor qualityColour(const model::ChordTones& t)
{
    if (t.count >= 3 && t.third && t.intervals[1] == 4) return kMajor;
    if (t.count >= 3 && t.third && t.intervals[1] == 3 && t.intervals[2] == 7) return kMinorColour;
    return kOther;
}

std::vector<std::string> rateNames()
{
    std::vector<std::string> names;
    for (const auto& r : model::kGridRates) names.emplace_back(r.name);
    return names;
}

const std::array<int, 10> kModes{1, 5, 2, 13, 16, 0, 3, 4, 6, 23}; // the palette's parallel modes
} // namespace

// =========================================================================================================== parts
namespace phrase
{
void ValueBox::apply(float v)
{
    if (! set) return;
    v = std::clamp(v, minValue, maxValue);
    if (step > 0) v = minValue + std::round((v - minValue) / step) * step;
    if (get && std::abs(get() - v) < 1e-6f) return;
    set(v);
    notify();
}

void ValueBox::mouseDown(const MouseEvent& e)
{
    dragging_ = true;
    startY_ = e.y;
    startValue_ = get ? get() : 0.0f;
    if (e.alt) { apply(defaultValue); dragging_ = false; return; }
    if (! choices.empty())
    {
        const int n = (int) choices.size();
        const int now = (int) std::lround(startValue_ - minValue);
        apply(minValue + (float) (((now + (e.shift ? -1 : 1)) % n + n) % n));
        dragging_ = false;
    }
}

void ValueBox::mouseDrag(const MouseEvent& e)
{
    if (! dragging_) return;
    const float steps = std::round((startY_ - e.y) / 6.0f);
    apply(startValue_ + steps * (step > 0 ? step : 1.0f));
}

void ValueBox::paint(SkCanvas& c)
{
    pro::well(c, bounds, 5.0f);
    pro::text(c, label, {bounds.x + 6, bounds.y + 2, bounds.w - 12, 12}, pro::kFaint, 9.5f, Align::Left, true);
    const float v = get ? get() : 0.0f;
    std::string text;
    if (! choices.empty())
        text = choices[(size_t) std::clamp((int) std::lround(v - minValue), 0, (int) choices.size() - 1)];
    else if (format) text = format(v);
    else text = std::to_string((int) std::lround(v));
    pro::text(c, text, {bounds.x + 6, bounds.y + 12, bounds.w - 12, bounds.h - 14}, pro::kText, 12.5f, Align::Left, true);
}

void PushButton::paint(SkCanvas& c)
{
    const bool on = lit && lit();
    pro::rounded(c, bounds, 6.0f, primary ? accent : (on ? paint::withAlpha(accent, 90) : pro::kRaised));
    if (! primary) pro::roundedStroke(c, bounds, 6.0f, on ? accent : pro::kLine, 1.0f);
    pro::text(c, label, bounds, primary ? 0xFF0B1514 : pro::kText, 12.0f, Align::Centre, true);
}

// ---------------------------------------------------------------------------------------------------- ToneGrid
int ToneGrid::stepAt(float x) const
{
    if (! pattern) return -1;
    const int n = std::clamp((int) pattern().length, 1, model::kPhraseSteps);
    if (x < bounds.x || x >= bounds.right()) return -1;
    return std::clamp((int) ((x - bounds.x) / bounds.w * (float) n), 0, n - 1);
}

int ToneGrid::rowAt(float y) const
{
    if (y < bounds.y || y >= bounds.bottom()) return -1;
    const int fromTop = (int) ((y - bounds.y) / bounds.h * (float) rows);
    return std::clamp(rows - 1 - fromTop, 0, rows - 1);
}

Rect ToneGrid::cellRect(int step, int row) const
{
    const int n = pattern ? std::clamp((int) pattern().length, 1, model::kPhraseSteps) : 16;
    const float w = bounds.w / (float) n, h = bounds.h / (float) rows;
    return {bounds.x + w * (float) step + 0.5f, bounds.y + h * (float) (rows - 1 - row) + 0.5f, w - 1.0f, h - 1.0f};
}

int ToneGrid::toneOf(const model::PhraseStep& st) const { return topLane ? st.top : st.tone; }

void ToneGrid::place(int step, int row)
{
    auto& st = pattern().steps[(size_t) step];
    if (adding_)
    {
        if (st.state == model::StepState::Note && toneOf(st) == row) return;
        if (st.state != model::StepState::Note)
        {
            st.velocity = st.velocity > 0 ? st.velocity : (uint8_t) 90;
            if (st.gate < 5) st.gate = 50;
            if (st.chance == 0) st.chance = 100;
            st.scaleStep = 0;
            st.rank = 200; // comes in late with Density until the face re-ranks
        }
        st.state = model::StepState::Note;
        if (topLane) st.top = (int8_t) row;
        else st.tone = (int8_t) row;
    }
    else
    {
        if (st.state != model::StepState::Note) return;
        st.state = model::StepState::Rest;
    }
    if (edited) edited();
    notify();
}

void ToneGrid::mouseDown(const MouseEvent& e)
{
    const int step = stepAt(e.x), row = rowAt(e.y);
    active_ = step >= 0 && row >= 0;
    if (! active_) return;
    auto& st = pattern().steps[(size_t) step];
    alt_ = e.alt && st.state == model::StepState::Note && ! topLane;
    if (alt_)
    {
        altStep_ = step;
        altStart_ = st.scaleStep;
        altY_ = e.y;
        return;
    }
    adding_ = ! (st.state == model::StepState::Note && toneOf(st) == row);
    place(step, row);
}

void ToneGrid::mouseDrag(const MouseEvent& e)
{
    if (! active_) return;
    if (alt_)
    {
        auto& st = pattern().steps[(size_t) altStep_];
        const int v = std::clamp(altStart_ + (int) std::lround((altY_ - e.y) / 10.0f), -7, 7);
        if (v != st.scaleStep)
        {
            st.scaleStep = (int8_t) v;
            if (edited) edited();
            notify();
        }
        return;
    }
    const int step = stepAt(std::clamp(e.x, bounds.x, bounds.right() - 0.01f));
    const int row = rowAt(std::clamp(e.y, bounds.y, bounds.bottom() - 0.01f));
    if (step >= 0 && row >= 0) place(step, row);
}

void ToneGrid::mouseUp(const MouseEvent&) { active_ = alt_ = false; }

void ToneGrid::paint(SkCanvas& c)
{
    pro::well(c, bounds, 5.0f);
    if (! pattern) return;
    const auto& pat = pattern();
    const int n = std::clamp((int) pat.length, 1, model::kPhraseSteps);
    const double sb = pat.stepBeats();
    const int playing = playStep ? playStep() : -1;
    for (int s = 0; s < n; ++s)
    {
        const bool beat = model::grid::positionIn(s * sb, 1.0) < 1e-9;
        const bool bar = model::grid::positionIn(s * sb, 4.0) < 1e-9;
        for (int r = 0; r < rows; ++r)
        {
            const auto cell = cellRect(s, r);
            pro::rounded(c, cell, 2.0f, bar ? 0xFF222831 : beat ? 0xFF1C2129 : 0xFF171B21);
            if (kickHatch && model::grid::positionIn(s * sb, 1.0) < 0.25 - 1e-9 && r == 0)
            {
                c.save();
                c.clipRect(cell.sk());
                for (float x = cell.x - cell.h; x < cell.right(); x += 6.0f) c.drawLine(x, cell.bottom(), x + cell.h, cell.y, pro::stroke(0x30FFFFFF, 1.0f));
                c.restore();
            }
        }
    }
    // Notes, ties as bars from their note.
    for (int s = 0; s < n; ++s)
    {
        const auto& st = pat.steps[(size_t) s];
        if (st.state != model::StepState::Note) continue;
        const int row = std::clamp(toneOf(st), 0, rows - 1);
        int ties = 0;
        while (s + 1 + ties < n && pat.steps[(size_t) (s + 1 + ties)].state == model::StepState::Tie) ++ties;
        auto cell = cellRect(s, row);
        if (ties > 0) cell.w = cellRect(s + ties, row).right() - cell.x;
        const float alpha = 0.45f + 0.55f * st.velocity / 127.0f;
        pro::rounded(c, cell.reduced(1.0f), 2.5f, paint::withAlpha(st.accent ? 0xFFEAFBF8 : accent, (uint8_t) std::lround(255 * alpha)));
        if (st.scaleStep != 0 && cell.w > 10)
        {
            char text[8];
            std::snprintf(text, sizeof text, "%+d", (int) st.scaleStep);
            pro::text(c, text, cell, 0xFF0B1514, std::min(10.0f, cell.h * 0.6f), Align::Centre, true);
        }
        if (st.octave != 0) c.drawCircle(cell.right() - 3, cell.y + 3, 2.0f, pro::fill(st.octave > 0 ? 0xFFFFFFFF : 0xFF0B1514));
    }
    if (playing >= 0 && playing < n)
    {
        const auto top = cellRect(playing, rows - 1), bottom = cellRect(playing, 0);
        pro::roundedStroke(c, {top.x, top.y, top.w, bottom.bottom() - top.y}, 2.0f, pro::kText, 1.2f);
    }
}

// ------------------------------------------------------------------------------------------------- ChordPalette
static constexpr Rect kPaletteRect{380, 140, 760, 340};

model::ChordExtension ChordPalette::rowExtension(int row)
{
    switch (row)
    {
        case 1: return model::ChordExtension::Seventh;
        case 2: return model::ChordExtension::Ninth;
        case 3: return model::ChordExtension::Add9;
        default: return model::ChordExtension::None;
    }
}

model::ChordQuality ChordPalette::rowQuality(int row)
{
    if (row == 4) return model::ChordQuality::Sus2;
    if (row == 5) return model::ChordQuality::Sus4;
    return model::ChordQuality::Auto;
}

model::ChordTones ChordPalette::chordAt(int degree, int row) const
{
    return model::chordTones(model::Chord::degree(degree, rowQuality(row), rowExtension(row)), key, scale);
}

Rect ChordPalette::leftRect() const { return {kPaletteRect.x + 12, kPaletteRect.y + 10, 30, 26}; }
Rect ChordPalette::rightRect() const { return {kPaletteRect.x + 110, kPaletteRect.y + 10, 30, 26}; }
Rect ChordPalette::modeRect() const { return {kPaletteRect.x + 150, kPaletteRect.y + 10, 170, 26}; }
Rect ChordPalette::homeRect() const { return {kPaletteRect.x + 328, kPaletteRect.y + 10, 70, 26}; }
Rect ChordPalette::cellRect(int degree, int row) const
{
    const float x0 = kPaletteRect.x + 70, w = (kPaletteRect.w - 82) / 7.0f;
    return {x0 + w * (float) degree, kPaletteRect.y + 46 + 40.0f * (float) row, w - 6, 34};
}
Rect ChordPalette::inversionRect(int index) const { return {kPaletteRect.x + 12 + 62.0f * (float) index, kPaletteRect.bottom() - 46, 58, 26}; }
Rect ChordPalette::removeRect() const { return {kPaletteRect.right() - 230, kPaletteRect.bottom() - 46, 100, 26}; }
Rect ChordPalette::closeRect() const { return {kPaletteRect.right() - 120, kPaletteRect.bottom() - 46, 100, 26}; }

void ChordPalette::mouseDown(const MouseEvent& e)
{
    if (! kPaletteRect.contains(e.x, e.y))
    {
        open = false;
        if (close) close();
        return;
    }
    if (leftRect().contains(e.x, e.y)) { key = (key + 5) % 12; return; }  // a fifth down round the circle
    if (rightRect().contains(e.x, e.y)) { key = (key + 7) % 12; return; } // a fifth up
    if (modeRect().contains(e.x, e.y))
    {
        auto it = std::find(kModes.begin(), kModes.end(), scale);
        const int index = it == kModes.end() ? 0 : (int) (it - kModes.begin());
        scale = kModes[(size_t) ((index + (e.shift ? (int) kModes.size() - 1 : 1)) % (int) kModes.size())];
        return;
    }
    if (homeRect().contains(e.x, e.y))
    {
        if (homeKey) key = homeKey();
        if (homeScale) scale = homeScale();
        return;
    }
    for (int i = 0; i < 5; ++i)
        if (inversionRect(i).contains(e.x, e.y))
        {
            if (setInversion) setInversion(i - 1);
            notify();
            return;
        }
    if (removeRect().contains(e.x, e.y)) { if (remove) remove(); open = false; notify(); return; }
    if (closeRect().contains(e.x, e.y)) { open = false; if (close) close(); return; }
    const int degrees = model::scaleAt(scale).size;
    for (int d = 0; d < degrees && d < 7; ++d)
        for (int r = 0; r < kRows; ++r)
            if (cellRect(d, r).contains(e.x, e.y))
            {
                const auto tones = chordAt(d, r);
                if (e.clicks >= 2) { if (assign) assign(tones, rowExtension(r), rowQuality(r)); notify(); }
                else if (audition) audition(tones, rowExtension(r), rowQuality(r));
                return;
            }
}

void ChordPalette::paintOverlay(SkCanvas& c)
{
    const auto& r = kPaletteRect;
    c.drawRRect(SkRRect::MakeRectXY(r.translated(0, 4).sk(), 10, 10), pro::fill(0x90000000));
    pro::rounded(c, r, 10.0f, 0xFF1F232B);
    pro::roundedStroke(c, r, 10.0f, pro::kLine, 1.0f);
    for (const auto& b : {leftRect(), rightRect(), modeRect(), homeRect()}) pro::rounded(c, b, 5.0f, pro::kRaised);
    pro::text(c, "<", leftRect(), pro::kText, 14, Align::Centre, true);
    pro::text(c, ">", rightRect(), pro::kText, 14, Align::Centre, true);
    pro::text(c, model::pitchClassName(key), {leftRect().right(), r.y + 10, rightRect().x - leftRect().right(), 26}, pro::kText, 15, Align::Centre, true);
    pro::text(c, std::string(model::scaleAt(scale).name), modeRect(), pro::kText, 12, Align::Centre, true);
    pro::text(c, "Home", homeRect(), pro::kDim, 12, Align::Centre, true);
    pro::text(c, "click: listen   double-click: use", {r.right() - 300, r.y + 12, 288, 22}, pro::kFaint, 11, Align::Right);
    static const char* rowNames[kRows] = {"Triad", "7th", "9th", "Add9", "Sus2", "Sus4"};
    const int hk = homeKey ? homeKey() : key, hs = homeScale ? homeScale() : scale;
    const int degrees = std::min(7, model::scaleAt(scale).size);
    for (int row = 0; row < kRows; ++row)
    {
        pro::text(c, rowNames[row], {r.x + 12, cellRect(0, row).y, 56, 34}, pro::kDim, 11, Align::Left, true);
        for (int d = 0; d < degrees; ++d)
        {
            const auto cell = cellRect(d, row);
            const auto tones = chordAt(d, row);
            const auto colour = qualityColour(tones);
            pro::rounded(c, cell, 6.0f, paint::withAlpha(colour, 40));
            pro::roundedStroke(c, cell, 6.0f, paint::withAlpha(colour, 140), 1.0f);
            pro::text(c, model::chordName(tones), {cell.x + 6, cell.y, cell.w - 18, cell.h}, pro::kText, 12.5f, Align::Left, true);
            bool inside = true;
            for (int i = 0; i < tones.count; ++i) inside &= model::inScale(tones.root + tones.intervals[(size_t) i], hk, hs);
            const float cx = cell.right() - 9, cy = cell.centreY();
            if (inside) c.drawCircle(cx, cy, 3.5f, pro::fill(colour));
            else c.drawCircle(cx, cy, 3.5f, pro::stroke(colour, 1.4f));
        }
    }
    static const char* inversions[5] = {"Auto", "Root", "1st", "2nd", "3rd"};
    const int now = inversion ? inversion() : -1;
    for (int i = 0; i < 5; ++i)
    {
        const auto b = inversionRect(i);
        pro::rounded(c, b, 5.0f, now == i - 1 ? paint::withAlpha(accent, 110) : pro::kRaised);
        pro::text(c, inversions[i], b, pro::kText, 11.5f, Align::Centre, true);
    }
    pro::rounded(c, removeRect(), 5.0f, pro::kRaised);
    pro::text(c, "Remove", removeRect(), 0xFFFF8A80, 12, Align::Centre, true);
    pro::rounded(c, closeRect(), 5.0f, pro::kRaised);
    pro::text(c, "Close", closeRect(), pro::kText, 12, Align::Centre, true);
}
} // namespace phrase

// ============================================================================================================ face
PhraseFace::PhraseFace(Phrase& device) : ProFace(device, kFull, kCompact, kAccent), phrase_(device)
{
    for (FacePart* p : std::initializer_list<FacePart*>{&presetBar_, &progressionBar_, &chordStrip_, &palette_, &toneGrid_, &roll_, &smallRoll_})
        p->accent = kAccent;
    presetBar_.name = "Presets";
    for (const auto& p : Phrase::presets()) presetBar_.presets.push_back({p.category, p.name});
    presetBar_.current = [this] { return phrase_.matchingPreset(); };
    presetBar_.select = [this](int i) { phrase_.applyPreset(i); rebuild(); refreshRoll(); };
    presetBar_.dice = [this] { phrase_.dice(slot_); refreshRoll(); };

    progressionBar_.name = "Progressions";
    progressionBar_.tipName = "Progression";
    progressionBar_.menuToLeft = true;
    progressionBar_.showCategory = false;
    for (int i = 0; i < model::factoryProgressionCount(); ++i)
    {
        const auto full = model::factoryProgressionName(i);
        const auto comma = full.find(", ");
        // The menu groups by move; the bar shows the move compactly with its bars ("i-bVI-bVII-i, 2 bars each").
        std::string move = full.substr(0, comma), compact;
        for (char ch : move)
            if (ch != ' ') compact += ch;
        progressionBar_.presets.push_back({move, compact + (comma == std::string::npos ? "" : ", " + full.substr(comma + 2))});
    }
    progressionBar_.current = [this] {
        for (int i = 0; i < model::factoryProgressionCount(); ++i)
            if (model::factoryProgression(i) == phrase_.state().progression) return i;
        return -1;
    };
    progressionBar_.select = [this](int i) {
        phrase_.editState().progression = model::factoryProgression(i);
        phrase_.editState().keepChords = true;
        chord_ = -1;
        edited();
    };

    chordStrip_.name = "Chords";
    chordStrip_.count = [this] { return phrase_.progressionInUse().count; };
    chordStrip_.chip = [this](int i) {
        const auto p = phrase_.progressionInUse();
        const auto& slot = p.slots[(size_t) std::clamp(i, 0, model::Progression::kMaxSlots - 1)];
        return ChordStrip::Chip{model::chordName(model::chordTones(slot.chord, phrase_.keyInUse(), phrase_.scaleInUse())), slot.beats};
    };
    chordStrip_.playing = [this]() -> int {
        if (! phrase_.transportPlaying()) return -1;
        return phrase_.progressionInUse().at(phrase_.playBeat()).index;
    };
    chordStrip_.selected = [this] { return chord_; };
    auto ownChords = [this] { return phrase_.linkStatus() != Phrase::LinkStatus::Following && phrase_.linkStatus() != Phrase::LinkStatus::NoLeader; };
    chordStrip_.select = [this, ownChords](int i) {
        if (! ownChords()) return;
        chord_ = i;
        palette_.open = true;
        palette_.key = phrase_.keyInUse();
        palette_.scale = phrase_.scaleInUse();
    };
    chordStrip_.resize = [this, ownChords](int i, double beats) {
        if (! ownChords()) return;
        phrase_.editState().progression.slots[(size_t) i].beats = beats;
        phrase_.editState().keepChords = true;
        edited();
    };
    chordStrip_.add = [this, ownChords] {
        auto& p = phrase_.editState().progression;
        if (! ownChords() || p.count >= model::Progression::kMaxSlots) return;
        p.slots[(size_t) p.count] = {model::Chord::degree(0), 8.0};
        chord_ = p.count++;
        phrase_.editState().keepChords = true;
        palette_.open = true;
        palette_.key = phrase_.keyInUse();
        palette_.scale = phrase_.scaleInUse();
        edited();
    };

    palette_.name = "Palette";
    palette_.homeKey = [this] { return phrase_.keyInUse(); };
    palette_.homeScale = [this] { return phrase_.scaleInUse(); };
    palette_.audition = [this](const model::ChordTones& tones, model::ChordExtension, model::ChordQuality) {
        model::VoicingRequest rq;
        rq.voices = std::min(4, std::max(3, tones.count));
        rq.centre = 62;
        const auto v = model::voice(tones, rq);
        std::array<int, 8> pitches{};
        for (int i = 0; i < v.count && i < 8; ++i) pitches[(size_t) i] = v.pitches[(size_t) i];
        phrase_.audition(pitches.data(), v.count);
    };
    palette_.assign = [this](const model::ChordTones& tones, model::ChordExtension extension, model::ChordQuality quality) {
        if (chord_ < 0) return;
        const int key = phrase_.keyInUse(), scale = phrase_.scaleInUse();
        model::Chord chord;
        const int degree = model::scaleDegreeOf(tones.root, key, scale);
        const auto native = degree >= 0 ? model::chordTones(model::Chord::degree(degree, quality, extension), key, scale) : model::ChordTones{};
        if (degree >= 0 && native.intervals == tones.intervals && native.count == tones.count)
            chord = model::Chord::degree(degree, quality, extension);
        else
        {
            // Borrowed: the palette chord's shape as an explicit quality on a chromatic root.
            auto q = quality;
            if (q == model::ChordQuality::Auto)
            {
                const int third = tones.intervals[1], fifth = tones.count > 2 ? tones.intervals[2] : 7;
                q = third == 4 ? (fifth == 8 ? model::ChordQuality::Augmented : model::ChordQuality::Major)
                               : (fifth == 6 ? model::ChordQuality::Diminished : model::ChordQuality::Minor);
            }
            chord = model::Chord::borrowed(tones.root - key, q, extension);
        }
        auto& slot = phrase_.editState().progression.slots[(size_t) chord_];
        chord.inversion = slot.chord.inversion;
        slot.chord = chord;
        phrase_.editState().keepChords = true;
        edited();
    };
    palette_.inversion = [this] { return chord_ >= 0 ? (int) phrase_.state().progression.slots[(size_t) chord_].chord.inversion : -1; };
    palette_.setInversion = [this](int inversion) {
        if (chord_ < 0) return;
        phrase_.editState().progression.slots[(size_t) chord_].chord.inversion = (int8_t) inversion;
        edited();
    };
    palette_.remove = [this] {
        auto& p = phrase_.editState().progression;
        if (chord_ < 0 || p.count <= 1) return;
        for (int i = chord_; i + 1 < p.count; ++i) p.slots[(size_t) i] = p.slots[(size_t) i + 1];
        --p.count;
        chord_ = -1;
        edited();
    };
    palette_.close = [this] { chord_ = -1; };

    toneGrid_.name = "Tones";
    toneGrid_.pattern = [this]() -> model::PhrasePattern& { return pattern(); };
    toneGrid_.edited = [this] { edited(); };
    toneGrid_.playStep = [this]() -> int {
        if (! phrase_.transportPlaying()) return -1;
        const auto seg = model::phraseSegmentAt(phrase_.state().part(part()), (int) std::lround(value(Phrase::Pattern)), phrase_.playBeat());
        if (seg.slot != slot_) return -1;
        return (int) ((phrase_.playBeat() - seg.start) / std::max(1e-6, pattern().stepBeats()));
    };
    roll_.name = "Roll";
    smallRoll_.name = "Roll";
    for (NoteRoll* r : {&roll_, &smallRoll_})
    {
        r->colourOf = [this](const NoteRoll::Note& n) { return fitColour(n); };
        r->playBeat = [this]() -> double {
            if (! phrase_.transportPlaying() || rollBeats_ <= 0) return -1.0;
            return model::grid::positionIn(phrase_.playBeat(), rollBeats_);
        };
    }
}

model::PhrasePattern& PhraseFace::pattern() { return phrase_.editState().part(part()).slots[(size_t) std::clamp(slot_, 0, 7)]; }

void PhraseFace::edited()
{
    phrase_.publishState();
    refreshRoll();
    repaint();
}

void PhraseFace::selectSlot(int slot)
{
    slot_ = std::clamp(slot, 0, model::kPhraseSlots - 1);
    repaint();
}

StepLane& PhraseFace::lane(const std::string& name)
{
    for (auto& l : lanes_)
        if (l->name == name) return *l;
    return *lanes_.front();
}
phrase::ValueBox& PhraseFace::box(const std::string& name)
{
    for (auto& b : boxes_)
        if (b->name == name) return *b;
    return *boxes_.front();
}
phrase::PushButton& PhraseFace::button(const std::string& name)
{
    for (auto& b : buttons_)
        if (b->name == name) return *b;
    return *buttons_.front();
}

phrase::ValueBox& PhraseFace::addBox(const std::string& name, const Rect& r, std::string tip)
{
    auto box = std::make_unique<phrase::ValueBox>();
    box->name = name;
    box->label = name;
    box->accent = kAccent;
    box->changed = [this] { edited(); };
    boxes_.push_back(std::move(box));
    addPart(*boxes_.back(), r, std::move(tip));
    return *boxes_.back();
}

StepLane& PhraseFace::addLane(const std::string& name, const Rect& r, std::string tip)
{
    auto lane = std::make_unique<StepLane>();
    lane->name = name;
    lane->label = name;
    lane->labelWidth = 74;
    lane->accent = kAccent;
    lane->changed = [this] { edited(); };
    lane->steps = pattern().length;
    lane->beatEvery = std::max(1, (int) std::lround(1.0 / pattern().stepBeats()));
    lane->playStep = toneGrid_.playStep;
    lanes_.push_back(std::move(lane));
    addPart(*lanes_.back(), r, std::move(tip));
    return *lanes_.back();
}

phrase::PushButton& PhraseFace::addPush(const std::string& name, const Rect& r, std::string tip)
{
    auto b = std::make_unique<phrase::PushButton>();
    b->name = name;
    b->label = name;
    b->accent = kAccent;
    buttons_.push_back(std::move(b));
    addPart(*buttons_.back(), r, std::move(tip));
    return *buttons_.back();
}

// --------------------------------------------------------------------------------------------------------- build
void PhraseFace::build(Layout layout)
{
    lanes_.clear();
    boxes_.clear();
    buttons_.clear();
    builtPart_ = part();
    palette_.open = palette_.open && layout == Layout::Full;
    if (layout == Layout::Compact)
    {
        addSegments({10, 10, 250, 28}, Phrase::Part, {}, false, 4, "Part: what this instance plays.");
        for (int s = 0; s < model::kPhraseSlots; ++s)
        {
            auto& pad = addButton({10 + 31.5f * (float) s, 46, 28, 28}, std::string(1, (char) ('A' + s)), [this, s] {
                setValue(Phrase::Pattern, std::lround(value(Phrase::Pattern)) == s + 1 ? 0.0f : (float) (s + 1));
            }, "Slot pads: play this slot (lit: the slot playing). Click the lit pad to play the form again.");
            pad.lit = [this, s] {
                const auto seg = model::phraseSegmentAt(phrase_.state().part(part()), (int) std::lround(value(Phrase::Pattern)), phrase_.playBeat());
                return seg.slot == s;
            };
        }
        addKnob({270, 6, 82, 92}, Phrase::Density, "DENSITY", "Density: steps come in by rank; 0 % is silence.");
        addKnob({356, 6, 62, 72}, Phrase::Swing, "SWING", "Swing on the off steps.");
        addKnob({420, 6, 62, 72}, Phrase::Gate, "GATE", "Gate: scales every note length.");
        addKnob({484, 6, 62, 72}, Phrase::Octave, "OCTAVE", "Octave of the whole part.");
        addButton({356, 100, 120, 28}, "Dice", [this] { phrase_.dice(slot_); refreshRoll(); }, "Dice: a new pattern in the selected slot.");
        addPart(smallRoll_, {556, 8, 196, 154}, "The part's notes over the progression; the line is the play head.");
        refreshRoll();
        return;
    }
    buildHeader(layout);
    buildHarmony();
    buildPattern();
    buildDice();
    buildPerform();
    addPart(roll_, {24, 598, 1112, 78}, "The whole form over the progression: root, chord tones, scale, outside in their colours.");
    // The palette is an overlay: no area of its own while closed (it would cover the pattern), every click while open.
    addPart(palette_, {0, 0, 0, 0}, "The chord palette: click to listen, double-click to put the chord into the selected slot.");
    refreshRoll();
}

void PhraseFace::buildHeader(Layout)
{
    addPart(presetBar_, {196, 16, 400, 34}, "Presets: previous, next, the menu by category, the dice (a new pattern).");
    addSegments({612, 16, 290, 34}, Phrase::Part, {}, false, 4, "Part: Arp, question / answer Stabs, Pad or a rolling Bass.");
    const bool app = static_cast<bool>(onToClip);
    addButton({916, 16, 108, 34}, app ? "To Clip" : "Drag MIDI", [this] {
        if (onToClip) onToClip();
        else if (onDragMidi) onDragMidi();
    }, app ? "To Clip: the part as notes in the selected MIDI clip (or a new clip at the loop); the device turns off."
           : "Drag MIDI: drag the part as a .mid file into your host.");
    auto& keep = addButton({1032, 16, 110, 34}, "Keep Chords", [this] {
        phrase_.editState().keepChords = ! phrase_.state().keepChords;
        edited();
    }, "Keep Chords: presets leave the progression alone.");
    keep.lit = [this] { return phrase_.state().keepChords; };
}

void PhraseFace::buildHarmony()
{
    addKnob({20, 78, 64, 60}, Phrase::Key, "KEY", "Key: the tonic.").small = true;
    addKnob({88, 78, 64, 60}, Phrase::Scale, "SCALE", "Scale: Minor and Phrygian are home in this style.").small = true;
    addSegments({160, 80, 210, 24}, Phrase::Chords, {}, false, 3,
                "Chords from: the progression here, the held notes (MIDI In), or another Phrase (Link).");
    addSegments({160, 110, 210, 24}, Phrase::LinkChannel, {}, false, 3,
                "Link channel: with Progression this instance leads the channel; with Link it follows it.");
    addPart(chordStrip_, {380, 80, 496, 54}, "The progression: click a chord for the palette, drag its right edge to change its length, + adds one.");
    addPart(progressionBar_, {886, 80, 254, 26}, "Factory progressions (moves of the style at 2, 4 and 8 bars per chord).");
}

void PhraseFace::buildPattern()
{
    const auto p = part();
    for (int s = 0; s < model::kPhraseSlots; ++s)
    {
        auto& tab = addButton({100 + 34.0f * (float) s, 166, 30, 24}, std::string(1, (char) ('A' + s)), [this, s] { selectSlot(s); },
                              "Pattern slots A-H: the one you edit.");
        tab.lit = [this, s] { return slot_ == s; };
    }
    static const std::array<const char*, 5> forms{"A", "A B", "A A A B", "A B A C", "A A B A"};
    for (int f = 0; f < (int) forms.size(); ++f)
    {
        auto& b = addButton({390 + 82.0f * (float) f, 166, 78, 24}, forms[(size_t) f], [this, f] {
            auto& partState = phrase_.editState().part(part());
            const std::string text = forms[(size_t) f];
            partState.formLength = 0;
            for (char c : text)
                if (c >= 'A' && c <= 'H') partState.form[(size_t) partState.formLength++] = (uint8_t) (c - 'A');
            edited();
        }, "Form: which slot plays when (question A, answers B, C).");
        b.textSize = 11;
        b.lit = [this, f] {
            const auto& partState = phrase_.state().part(part());
            std::string text;
            for (int i = 0; i < partState.formLength; ++i) text += std::string(text.empty() ? "" : " ") + (char) ('A' + partState.form[(size_t) i]);
            return text == forms[(size_t) f];
        };
    }
    addSegments({300, 196, 560, 24}, Phrase::Pattern, {}, false, 2, "Pattern: play the form, or hold one slot (automate it to launch variations).");
    auto& rate = addBox("Rate", {24, 196, 80, 30}, "Rate: the step length.");
    rate.choices = rateNames();
    rate.maxValue = (float) model::kGridRates.size() - 1;
    rate.defaultValue = model::kRate1_16;
    rate.get = [this] { return (float) pattern().rate; };
    rate.set = [this](float v) { pattern().rate = (uint8_t) std::lround(v); };
    auto& length = addBox("Length", {108, 196, 80, 30}, "Length in steps (1-64).");
    length.minValue = 1;
    length.maxValue = 64;
    length.defaultValue = 16;
    length.get = [this] { return (float) pattern().length; };
    length.set = [this](float v) { pattern().length = (uint8_t) std::lround(v); };
    if (p != PhrasePart::Pad)
    {
        auto& octave = addBox("Register", {192, 196, 100, 30}, "Register: the octave the chords are placed in.");
        octave.minValue = 0;
        octave.maxValue = 7;
        octave.defaultValue = p == PhrasePart::Bass ? 1 : 4;
        octave.format = [this](float v) { return noteName(model::pitchOf(phrase_.keyInUse(), (int) std::lround(v))); };
        octave.get = [this] { return (float) pattern().octave; };
        octave.set = [this](float v) { pattern().octave = (int8_t) std::lround(v); };
    }

    if (p == PhrasePart::Pad)
    {
        auto choiceNames = [](auto fn, int count) {
            std::vector<std::string> names;
            for (int i = 0; i < count; ++i) names.emplace_back(fn(i));
            return names;
        };
        const auto voicings = choiceNames([](int i) { return model::voicingStyleName((model::VoicingStyle) i); }, (int) model::VoicingStyle::Count);
        const auto motions = choiceNames([](int i) { return model::padMotionName((model::PadMotion) i); }, (int) model::PadMotion::Count);
        const auto basses = choiceNames([](int i) { return model::padBassName((model::PadBass) i); }, (int) model::PadBass::Count);
        auto& voices = addBox("Voices", {24, 236, 100, 34}, "Voices of the pad (3-7).");
        voices.minValue = 3; voices.maxValue = 7; voices.defaultValue = 5;
        voices.get = [this] { return (float) pattern().padVoices; };
        voices.set = [this](float v) { pattern().padVoices = (uint8_t) std::lround(v); };
        auto& voicing = addBox("Voicing", {130, 236, 110, 34}, "Voicing: Close, Open, Drop 2, Wide (root and fifth low), Fifths.");
        voicing.choices = voicings; voicing.maxValue = (float) voicings.size() - 1;
        voicing.get = [this] { return (float) pattern().padVoicing; };
        voicing.set = [this](float v) { pattern().padVoicing = (model::VoicingStyle) std::lround(v); };
        auto& centre = addBox("Centre", {246, 236, 100, 34}, "Register: the voicing's centre note.");
        centre.minValue = 48; centre.maxValue = 76; centre.defaultValue = 62;
        centre.format = [](float v) { return noteName((int) std::lround(v)); };
        centre.get = [this] { return (float) pattern().padCentre; };
        centre.set = [this](float v) { pattern().padCentre = (uint8_t) std::lround(v); };
        auto& motion = addBox("Motion", {352, 236, 110, 34}, "Motion: Hold keeps common tones, Breathe moves one voice, Pulse strikes again.");
        motion.choices = motions; motion.maxValue = (float) motions.size() - 1;
        motion.get = [this] { return (float) pattern().motion; };
        motion.set = [this](float v) { pattern().motion = (model::PadMotion) std::lround(v); };
        auto& every = addBox("Every", {468, 236, 90, 34}, "How many bars between motions.");
        every.minValue = 1; every.maxValue = 8; every.defaultValue = 2;
        every.format = [](float v) { return std::to_string((int) std::lround(v)) + (std::lround(v) == 1 ? " bar" : " bars"); };
        every.get = [this] { return (float) pattern().motionBars; };
        every.set = [this](float v) { pattern().motionBars = (uint8_t) std::lround(v); };
        auto& bass = addBox("Bass", {564, 236, 120, 34}, "Bass under the pad: off, the root, or root and fifth.");
        bass.choices = basses; bass.maxValue = (float) basses.size() - 1;
        bass.get = [this] { return (float) pattern().padBass; };
        bass.set = [this](float v) { pattern().padBass = (model::PadBass) std::lround(v); };
        using PatternByte = decltype(&model::PhrasePattern::colour); // a uint8_t member; spelled this way for MSVC
        const std::array<std::tuple<const char*, int, int, int, PatternByte, const char*>, 5> percents{{
            {"Colour", 0, 100, 30, &model::PhrasePattern::colour, "Colour: the chance per chord of an add9, sus2, 7th or 11th."},
            {"Overlap", 0, 100, 25, &model::PhrasePattern::overlap, "Overlap: legato into the next chord, in % of a beat."},
            {"Strum ms", 0, 250, 0, &model::PhrasePattern::padStrumMs, "Strum: the notes of a chord spread over this many ms."},
            {"Shimmer", 0, 100, 0, &model::PhrasePattern::shimmer, "Shimmer: sparse quiet octave-up chord tones on the off eighths."},
            {"Level", 1, 127, 80, &model::PhrasePattern::padVelocity, "Level: the pad's velocity."},
        }};
        float x = 24;
        for (const auto& [name, lo, hi, def, member, tip] : percents)
        {
            auto& b = addBox(name, {x, 276, 100, 34}, tip);
            b.minValue = (float) lo; b.maxValue = (float) hi; b.defaultValue = (float) def;
            const auto m = member;
            b.get = [this, m] { return (float) (pattern().*m); };
            b.set = [this, m](float v) { pattern().*m = (uint8_t) std::lround(v); };
            x += 106;
        }
        return;
    }
    if (p == PhrasePart::Stabs)
    {
        auto& voices = addBox("Voices", {24, 236, 80, 30}, "Voices of each stab (3-6).");
        voices.minValue = 3; voices.maxValue = 6; voices.defaultValue = 4;
        voices.get = [this] { return (float) pattern().voices; };
        voices.set = [this](float v) { pattern().voices = (uint8_t) std::lround(v); };
        std::vector<std::string> names;
        for (int i = 0; i < (int) model::VoicingStyle::Count; ++i) names.emplace_back(model::voicingStyleName((model::VoicingStyle) i));
        auto& voicing = addBox("Voicing", {108, 236, 90, 30}, "Voicing of the stabs (least motion from the last stab, no thirds below C3).");
        voicing.choices = names; voicing.maxValue = (float) names.size() - 1;
        voicing.get = [this] { return (float) pattern().voicing; };
        voicing.set = [this](float v) { pattern().voicing = (model::VoicingStyle) std::lround(v); };
        auto& strum = addBox("Strum", {202, 236, 90, 30}, "Strum: the stab's notes spread over this many ms, low to high.");
        strum.maxValue = 60;
        strum.format = [](float v) { return std::to_string((int) std::lround(v)) + " ms"; };
        strum.get = [this] { return (float) pattern().strumMs; };
        strum.set = [this](float v) { pattern().strumMs = (uint8_t) std::lround(v); };
    }
    const float gridY = p == PhrasePart::Stabs ? 272.0f : 236.0f;
    toneGrid_.rows = p == PhrasePart::Bass ? 3 : p == PhrasePart::Stabs ? 7 : 8;
    toneGrid_.topLane = p == PhrasePart::Stabs;
    toneGrid_.kickHatch = p == PhrasePart::Bass;
    addPart(toneGrid_, {100, gridY, 760, 378 - gridY},
            p == PhrasePart::Stabs ? "The top line: which chord tone is the highest note of each stab. Click to place, drag to paint."
                                   : "Tones: click to place a note on a chord tone, drag to paint, Alt-drag a note for scale steps.");
    auto lanePart = [&](const std::string& name, float y, StepLane::Kind kind, float lo, float hi, float def, auto getter, auto setter, std::string tip) -> StepLane& {
        auto& l = addLane(name, {26, y, 834, 24}, std::move(tip));
        l.kind = kind;
        l.minValue = lo;
        l.maxValue = hi;
        l.defaultValue = def;
        l.integral = true;
        l.get = [this, getter](int s) { return getter(pattern().steps[(size_t) s]); };
        l.set = [this, setter](int s, float v) { setter(pattern().steps[(size_t) s], v); };
        if (part() == PhrasePart::Bass) l.disabled = [this](int s) { return model::grid::positionIn(s * pattern().stepBeats(), 1.0) < 0.25 - 1e-9; };
        return l;
    };
    using St = model::PhraseStep;
    lanePart("Octave", 384, StepLane::Kind::Bar, -2, 2, 0, [](const St& st) { return (float) st.octave; },
             [](St& st, float v) { st.octave = (int8_t) std::lround(v); }, "Octave per step.").bipolar = true;
    lanePart("Velocity", 412, StepLane::Kind::Bar, 1, 127, 90, [](const St& st) { return (float) st.velocity; },
             [](St& st, float v) { st.velocity = (uint8_t) std::lround(v); }, "Velocity per step.");
    lanePart("Gate", 440, StepLane::Kind::Bar, 5, 400, 50, [](const St& st) { return (float) st.gate; },
             [](St& st, float v) { st.gate = (uint16_t) std::lround(v); }, "Gate per step, in % of the step (over 100: legato).");
    lanePart("Chance", 468, StepLane::Kind::Bar, 0, 100, 100, [](const St& st) { return (float) st.chance; },
             [](St& st, float v) { st.chance = (uint8_t) std::lround(v); }, "Chance that the step plays (from the seed: the same every time).");
    lanePart("Accent", 496, StepLane::Kind::Toggle, 0, 1, 0, [](const St& st) { return st.accent ? 1.0f : 0.0f; },
             [](St& st, float v) { st.accent = v >= 0.5f; }, "Accent: adds the Accent knob's velocity.");
    lanePart("Slide", 524, StepLane::Kind::Toggle, 0, 1, 0, [](const St& st) { return st.slide ? 1.0f : 0.0f; },
             [](St& st, float v) { st.slide = v >= 0.5f; }, "Slide: the note overlaps the next one (glide on a mono synth).");
    lanePart("Tie", 552, StepLane::Kind::Toggle, 0, 1, 0, [](const St& st) { return st.state == model::StepState::Tie ? 1.0f : 0.0f; },
             [](St& st, float v) {
                 if (v >= 0.5f) st.state = model::StepState::Tie;
                 else if (st.state == model::StepState::Tie) st.state = model::StepState::Rest;
             }, "Tie: the step extends the note before it.");
}

void PhraseFace::buildDice()
{
    const auto p = part();
    const float x = 884, w = 124;
    auto pos = [&](int i) { return Rect{x + (float) (i % 2) * (w + 8), 186 + 40.0f * (float) (i / 2), w, 34}; };
    auto styleBox = [&](int i, const std::string& name, std::vector<std::string> choices, float lo, float hi, auto getter, auto setter, std::string tip) {
        auto& b = addBox(name, pos(i), std::move(tip));
        b.choices = std::move(choices);
        b.minValue = lo;
        b.maxValue = b.choices.empty() ? hi : (float) b.choices.size() - 1;
        b.defaultValue = lo;
        b.get = [this, getter] { return getter(phrase_.state().part(part())); };
        b.set = [this, setter](float v) { setter(phrase_.editState().part(part()), v); };
        return &b;
    };
    auto names = [](auto fn, int count) {
        std::vector<std::string> list;
        for (int i = 0; i < count; ++i) list.emplace_back(fn(i));
        return list;
    };
    using PS = model::PhrasePartState;
    int i = 0;
    if (p == PhrasePart::Arp)
    {
        styleBox(i++, "Shape", names([](int k) { return model::arpShapeName((model::ArpShape) k); }, (int) model::ArpShape::Count), 0, 0,
                 [](const PS& s) { return (float) s.arp.shape; }, [](PS& s, float v) { s.arp.shape = (model::ArpShape) std::lround(v); },
                 "Shape: the order the chord tones are played in (Pedal Low: the root between moving notes).");
        styleBox(i++, "Rhythm", names([](int k) { return model::arpRhythmName((model::ArpRhythm) k); }, (int) model::ArpRhythm::Count), 0, 0,
                 [](const PS& s) { return (float) s.arp.rhythm; }, [](PS& s, float v) { s.arp.rhythm = (model::ArpRhythm) std::lround(v); },
                 "Rhythm: Offbeats skips the kick's sixteenth.");
        styleBox(i++, "Accents", names([](int k) { return model::arpAccentsName((model::ArpAccents) k); }, (int) model::ArpAccents::Count), 0, 0,
                 [](const PS& s) { return (float) s.arp.accents; }, [](PS& s, float v) { s.arp.accents = (model::ArpAccents) std::lround(v); },
                 "Accents: Off Beat puts them on steps 3, 7, 11 and 14.");
        styleBox(i++, "Glide", {"Off", "On"}, 0, 1, [](const PS& s) { return s.arp.accentGlide ? 1.0f : 0.0f; },
                 [](PS& s, float v) { s.arp.accentGlide = v >= 0.5f; }, "Accent Glide: long accented notes that slide, short others.");
        using StyleByte = decltype(&model::ArpStyle::tension); // a uint8_t member; spelled this way for MSVC
        for (const auto& [name, member, hi, tip] : std::array<std::tuple<const char*, StyleByte, float, const char*>, 6>{{
                 {"Tension", &model::ArpStyle::tension, 100, "Tension: chance of an accented 2nd on step 7 or 15 that falls to the root."},
                 {"Range", &model::ArpStyle::range, 3, "Range in octaves."},
                 {"Passing", &model::ArpStyle::passing, 100, "Passing: scale-step neighbours on the weak steps."},
                 {"Variation", &model::ArpStyle::variation, 100, "Variation: how much bars 2 and 4 turn."},
                 {"Bars", &model::ArpStyle::bars, 8, "Bars: 1, 2, 4; 8 writes four bars and their answer in the next slot."},
                 {"Hits", &model::ArpStyle::euclidHits, 16, "Hits per 16 steps (Euclid)."}}})
        {
            const auto m = member;
            auto* b = styleBox(i++, name, {}, name == std::string("Range") || name == std::string("Bars") || name == std::string("Hits") ? 1.0f : 0.0f, hi,
                               [m](const PS& s) { return (float) (s.arp.*m); }, [m](PS& s, float v) { s.arp.*m = (uint8_t) std::lround(v); }, tip);
            if (name == std::string("Tension") || name == std::string("Passing") || name == std::string("Variation"))
            {
                b->step = 5;
                b->format = [](float v) { return std::to_string((int) std::lround(v)) + " %"; };
            }
        }
    }
    else if (p == PhrasePart::Stabs)
    {
        styleBox(i++, "Rhythm", names([](int k) { return model::stabRhythmName((model::StabRhythm) k); }, (int) model::StabRhythm::Count), 0, 0,
                 [](const PS& s) { return (float) s.stabs.rhythm; }, [](PS& s, float v) { s.stabs.rhythm = (model::StabRhythm) std::lround(v); },
                 "Rhythm of the stabs: off-beats, 3-3-2, dotted, sparse, pushes before the beat.");
        styleBox(i++, "Hits", {}, 2, 8, [](const PS& s) { return (float) s.stabs.hits; }, [](PS& s, float v) { s.stabs.hits = (uint8_t) std::lround(v); },
                 "Hits per bar.");
        styleBox(i++, "Contour", names([](int k) { return model::stabContourName((model::StabContour) k); }, (int) model::StabContour::Count), 0, 0,
                 [](const PS& s) { return (float) s.stabs.contour; }, [](PS& s, float v) { s.stabs.contour = (model::StabContour) std::lround(v); },
                 "Contour of the top line.");
        styleBox(i++, "Bars", {}, 1, 4, [](const PS& s) { return (float) s.stabs.bars; }, [](PS& s, float v) { s.stabs.bars = (uint8_t) std::lround(v); },
                 "Bars of the question.");
    }
    else if (p == PhrasePart::Bass)
    {
        styleBox(i++, "Rhythm", names([](int k) { return model::bassRhythmName((model::BassRhythm) k); }, (int) model::BassRhythm::Count), 0, 0,
                 [](const PS& s) { return (float) s.bass.rhythm; }, [](PS& s, float v) { s.bass.rhythm = (model::BassRhythm) std::lround(v); },
                 "Rhythm: KBBB is the three sixteenths between the kicks.");
        styleBox(i++, "Jumps", {}, 0, 100, [](const PS& s) { return (float) s.bass.octaveJumps; }, [](PS& s, float v) { s.bass.octaveJumps = (uint8_t) std::lround(v); },
                 "Octave jumps: chance per bar of one in its last beat.")->step = 5;
        styleBox(i++, "Fifths", {}, 0, 100, [](const PS& s) { return (float) s.bass.fifths; }, [](PS& s, float v) { s.bass.fifths = (uint8_t) std::lround(v); },
                 "Fifths: chance of the fifth on the phrase end.")->step = 5;
        styleBox(i++, "Bars", {}, 1, 4, [](const PS& s) { return (float) s.bass.bars; }, [](PS& s, float v) { s.bass.bars = (uint8_t) std::lround(v); },
                 "Bars of the pattern.");
    }
    const float y = 186 + 40.0f * (float) ((i + 1) / 2) + 8;
    auto& dice = addPush("Dice", {x, y, 256, 34}, "Dice: a new pattern from the style (Shift: keep the rhythm; Alt: keep the pitches).");
    dice.primary = true;
    dice.press = [this](const MouseEvent& e) {
        phrase_.dice(slot_, e.shift ? model::DiceKeep::Rhythm : e.alt ? model::DiceKeep::Pitches : model::DiceKeep::None);
        refreshRoll();
    };
    if (p == PhrasePart::Pad) return; // a pad has no steps to mutate or answer
    auto& mutate = addPush("Mutate", {x, y + 40, 124, 30}, "Mutate: changes about one step in eight.");
    mutate.press = [this](const MouseEvent&) { phrase_.mutate(slot_); refreshRoll(); };
    auto& kind = addBox("Answer as", {x + 132, y + 40, 124, 30}, "How the answer is written (Answer closes on the root).");
    for (int k = 0; k < (int) model::AnswerKind::Count; ++k) kind.choices.emplace_back(model::answerKindName((model::AnswerKind) k));
    kind.maxValue = (float) kind.choices.size() - 1;
    kind.get = [this] { return (float) answerKind_; };
    kind.set = [this](float v) { answerKind_ = (int) std::lround(v); };
    kind.changed = [this] { repaint(); };
    auto& answer = addPush("Answer", {x, y + 76, 256, 30}, "Answer: writes the next slot from this one and selects it.");
    answer.press = [this](const MouseEvent&) {
        const int to = std::min(model::kPhraseSlots - 1, slot_ + 1);
        phrase_.answer(slot_, to, (model::AnswerKind) answerKind_);
        selectSlot(to);
        refreshRoll();
    };
}

void PhraseFace::buildPerform()
{
    auto& density = addKnob({20, 684, 88, 76}, Phrase::Density, "DENSITY", "Density: steps come in by rank. Automate it to build the part up.");
    density.caption = [this] {
        const float d = value(Phrase::Density);
        return d <= 0.001f ? std::string("silent") : d >= 0.999f ? std::string("full") : std::string("by rank");
    };
    const std::array<std::tuple<int, const char*, const char*>, 9> knobs{{
        {Phrase::Swing, "SWING", "Swing: the off steps late by up to a third."},
        {Phrase::Gate, "GATE", "Gate: scales every length."},
        {Phrase::Velocity, "VELOCITY", "Velocity: scales every velocity."},
        {Phrase::Accent, "ACCENT", "Accent: velocity added to accented steps."},
        {Phrase::Octave, "OCTAVE", "Octave of the part."},
        {Phrase::Shift, "SHIFT", "Shift: moves the part inside the scale."},
        {Phrase::Humanize, "HUMANIZE", "Humanize: timing and velocity, the same every time (from the seed)."},
        {Phrase::KickGuard, "KICK GUARD", "Kick Guard: quieter on the kick's sixteenth (100 % mutes it)."},
        {Phrase::Seed, "SEED", "Seed: chance and humanize."},
    }};
    float x = 120;
    for (const auto& [param, label, tip] : knobs)
    {
        addKnob({x, 690, 76, 68}, param, label, tip).small = true;
        x += 82;
    }
    addToggle({x + 10, 694, 100, 26}, Phrase::Latch, "Latch", "Latch: MIDI In keeps the chord after the keys are released.");
    addToggle({x + 10, 726, 100, 26}, Phrase::Audition, "Audition", "Audition: the VST3's own voice plays the part (off in Sonora).");
}

// --------------------------------------------------------------------------------------------------------- state
uint64_t PhraseFace::signature() const
{
    uint64_t h = phrase_.stateVersion();
    for (int i = 0; i < Phrase::NumParams; ++i)
        if (i != Phrase::Audition) h = model::PatternRandom::hash((uint32_t) h, (int64_t) std::lround(device_.getParameter(i) * 10000.0f), i);
    h = h * 1000003u + (uint64_t) phrase_.linkStatus();
    const auto held = phrase_.heldChord();
    h = model::PatternRandom::hash((uint32_t) h, held.root, held.count, held.intervals[1]);
    const auto progression = phrase_.progressionInUse();
    for (int i = 0; i < progression.count; ++i)
        h = model::PatternRandom::hash((uint32_t) h, progression.slots[(size_t) i].chord.root, (int64_t) progression.slots[(size_t) i].beats);
    return h;
}

void PhraseFace::refreshRoll()
{
    rollSignature_ = signature();
    const auto& s = phrase_.state();
    const int pattern = (int) std::lround(value(Phrase::Pattern));
    double beats = pattern >= 1 ? s.part(part()).slots[(size_t) (pattern - 1)].lengthBeats() : s.part(part()).formBeats();
    const bool midiIn = std::lround(value(Phrase::Chords)) == Phrase::FromMidi;
    if (! midiIn) beats = std::max(beats, phrase_.progressionInUse().lengthBeats());
    rollBeats_ = std::clamp(beats, 4.0, 64.0);
    std::vector<model::MidiNote> input;
    if (midiIn)
    {
        const auto held = phrase_.heldChord();
        for (int i = 0; i < held.count; ++i)
        {
            model::MidiNote n;
            n.start = 0;
            n.duration = rollBeats_;
            n.pitch = model::pitchOf(held.root, 4) + held.interval(i);
            input.push_back(n);
        }
    }
    const auto notes = phrase_.renderNotes(input, 0, rollBeats_, 138.0, 24000.0, 1024);
    for (NoteRoll* r : {&roll_, &smallRoll_})
    {
        r->notes.clear();
        for (const auto& n : notes) r->notes.push_back({n.start, n.duration, n.pitch, n.velocity, 0});
        r->lengthBeats = rollBeats_;
        r->emptyText = midiIn ? "Hold a chord (MIDI In)" : phrase_.linkStatus() == Phrase::LinkStatus::NoLeader ? "No leader on this link channel" : "Silent (Density?)";
    }
}

SkColor PhraseFace::fitColour(const NoteRoll::Note& n) const
{
    const int key = phrase_.keyInUse(), scale = phrase_.scaleInUse();
    model::ChordTones tones;
    if (std::lround(value(Phrase::Chords)) == Phrase::FromMidi) tones = phrase_.heldChord();
    else
    {
        const auto p = phrase_.progressionInUse();
        tones = model::chordTones(p.chordAt(n.start + 1e-9), key, scale);
    }
    SkColor colour = kFitScale;
    switch (model::noteFit(n.pitch, tones, key, scale))
    {
        case model::NoteFit::Root: colour = kAccent; break;
        case model::NoteFit::ChordTone: colour = kFitChord; break;
        case model::NoteFit::InScale: colour = kFitScale; break;
        case model::NoteFit::Outside: colour = kFitOutside; break;
    }
    return paint::withAlpha(colour, (uint8_t) std::lround(110 + 145 * std::clamp(n.velocity / 127.0f, 0.0f, 1.0f)));
}

std::string PhraseFace::chordNow() const
{
    if (phrase_.linkStatus() == Phrase::LinkStatus::NoLeader) return "no leader";
    if (std::lround(device_.getParameter(Phrase::Chords)) == Phrase::FromMidi)
    {
        const auto held = phrase_.heldChord();
        return held.count > 0 ? model::chordName(held) : "hold a chord";
    }
    const auto p = phrase_.progressionInUse();
    const double beat = phrase_.transportPlaying() ? phrase_.playBeat() : 0.0;
    return model::chordName(model::chordTones(p.chordAt(beat), phrase_.keyInUse(), phrase_.scaleInUse()));
}

bool PhraseFace::animate()
{
    if (part() != builtPart_) { rebuild(); return true; }
    if (signature() != rollSignature_) refreshRoll();
    for (auto& l : lanes_)
    {
        l->steps = pattern().length;
        l->beatEvery = std::max(1, (int) std::lround(1.0 / pattern().stepBeats()));
    }
    return phrase_.transportPlaying() || palette_.open;
}

std::string PhraseFace::idleHelp() const
{
    std::string text = std::string(model::phrasePartName(phrase_.part())) + " over " + chordNow();
    switch (phrase_.linkStatus())
    {
        case Phrase::LinkStatus::Leading: text += " · leading link " + std::string(1, (char) ('A' + std::lround(device_.getParameter(Phrase::LinkChannel)) - 1)); break;
        case Phrase::LinkStatus::Following: text += " · following the link"; break;
        case Phrase::LinkStatus::NoLeader: text += " · nobody leads this link channel: resting"; break;
        case Phrase::LinkStatus::Off: break;
    }
    return text + ". Click a chord for the palette; Dice writes a new pattern into the selected slot.";
}

// --------------------------------------------------------------------------------------------------------- paint
void PhraseFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        pro::panel(c, {4, 4, 548, 162}, 6);
        pro::text(c, chordNow(), {486, 100, 64, 28}, pro::kText, 15, Align::Left, true);
        pro::text(c, "now", {486, 126, 64, 14}, pro::kFaint, 9.5f, Align::Left, true);
        return;
    }
    pro::wordmark(c, "phrase", 24, 10, 28, kAccent, version);
    pro::panel(c, {14, 60, 1132, 82});
    pro::sectionTitle(c, "HARMONY", 24, 60, kAccent);
    pro::text(c, chordNow(), {886, 110, 254, 26}, pro::kText, 16, Align::Left, true);
    pro::panel(c, {14, 148, 856, 438});
    pro::sectionTitle(c, "PATTERN", 24, 148, kAccent);
    pro::text(c, "SLOT", {24, 170, 70, 18}, pro::kFaint, 10, Align::Left, true);
    pro::panel(c, {876, 148, 270, 438});
    pro::sectionTitle(c, "DICE", 884, 156, kAccent);
    pro::panel(c, {14, 592, 1132, 88});
    pro::panel(c, {14, 684, 1132, 78});
    if (part() != PhrasePart::Pad)
    {
        // The tone grid's row labels.
        const char* bassRows[3] = {"Root", "5th", "Oct"};
        for (int r = 0; r < toneGrid_.rows; ++r)
        {
            const auto cell = toneGrid_.cellRect(0, r);
            const std::string label = part() == PhrasePart::Bass ? bassRows[r] : (part() == PhrasePart::Stabs ? "top " : "") + std::to_string(r + 1);
            pro::text(c, label, {24, cell.y, 72, cell.h}, pro::kDim, std::min(11.0f, cell.h * 0.7f), Align::Left, true);
        }
    }
    else
    {
        // The pad's voicings, one keyboard strip per chord of the progression.
        const Rect area{24, 318, 836, 262};
        pro::well(c, area, 6);
        const auto p = phrase_.progressionInUse();
        const int n = std::max(1, p.count);
        const float w = (area.w - 16) / (float) n;
        model::Voicing previous;
        const auto& pat = phrase_.state().part(PhrasePart::Pad).slots[0];
        for (int i = 0; i < p.count; ++i)
        {
            const auto tones = model::chordTones(p.slots[(size_t) i].chord, phrase_.keyInUse(), phrase_.scaleInUse());
            model::VoicingRequest rq;
            rq.style = pat.padVoicing;
            rq.voices = pat.padVoices;
            rq.centre = pat.padCentre;
            const auto v = model::voice(tones, rq, previous.count > 0 ? &previous : nullptr);
            previous = v;
            const Rect column{area.x + 8 + w * (float) i, area.y + 8, w - 6, area.h - 16};
            pro::text(c, model::chordName(tones), {column.x, column.y, column.w, 18}, pro::kText, 12, Align::Centre, true);
            for (int pitch = 36; pitch < 88; ++pitch)
            {
                const float y = column.bottom() - (float) (pitch - 36 + 1) * (column.h - 24) / 52.0f;
                const bool black = (pitch % 12 == 1 || pitch % 12 == 3 || pitch % 12 == 6 || pitch % 12 == 8 || pitch % 12 == 10);
                c.drawRect(SkRect::MakeXYWH(column.x, y, column.w, (column.h - 24) / 52.0f - 0.5f), pro::fill(black ? 0xFF14171C : 0xFF1C2027));
                for (int k = 0; k < v.count; ++k)
                    if (v.pitches[(size_t) k] == pitch)
                        c.drawRect(SkRect::MakeXYWH(column.x + 2, y, column.w - 4, (column.h - 24) / 52.0f - 0.5f), pro::fill(kAccent));
            }
        }
    }
}

void PhraseFace::paintFront(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Full) paintHelp(c, {20, 764, 1120, 14});
}
} // namespace sonora::ui
