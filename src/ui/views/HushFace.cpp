#include "ui/views/HushFace.h"

#include "devices/RackProcessor.h"
#include "devices/native/HushSession.h"
#include "engine/EngineGraph.h"
#include "gfx/Fonts.h"
#include "ui/DeviceMenu.h"
#include "ui/Painter.h"

#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkGradientShader.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui
{
namespace
{
using H = devices::Hush;
using HushSettings = devices::HushSettings;
using HushPreset = devices::HushPreset;
using HushSession = devices::HushSession;

// Palette measured from reference screenshots.
constexpr SkColor kPanel = 0xFFA0B8D9, kPanelDark = 0xFF97B1D4, kStrip = 0xFF9CB5D5, kToolbar = 0xFFA1BAD8,
                  kField = 0xFFC5D4E8, kBox = 0xFFD1DCEF, kGraphTop = 0xFFB1C6DF, kGraphMid = 0xFFB8CCE0,
                  kCutLight = 0xFFB1C6E1, kReduction = 0xFF8094C8, kReductionLow = 0xFF889ED0, kFooter = 0xFF98B3D5,
                  kKnob = 0xFF45464F, kKnobLight = 0xFF5A5C68, kArcValue = 0xFFF3F3F7, kArcRest = 0xFF7D8FC0,
                  kText = 0xFF2E3140, kLabel = 0xFF3A3F55, kLogo = 0xFF4F5FA6, kCurve = 0xFFF4F5FC,
                  kCurveRight = 0xFF5E6E9E, kDarkButton = 0xFF44464C, kGrid = 0x40FFFFFF, kMaxCut = 0x405A6070;
constexpr SkColor kBandColours[H::kBands] = {0xFFDE6D83, 0xFF54A4CB, 0xFF479D84, 0xFFA066C8,
                                             0xFFE29A55, 0xFF4FB3B0, 0xFFD9B84A, 0xFFD36BB0};
constexpr float kCurvePx = 15.0f; // design pixels per curve dB above the 0 dB line

SkPaint fillPaint(SkColor c)
{
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(c);
    return p;
}
SkPaint strokePaint(SkColor c, float width)
{
    SkPaint p = fillPaint(c);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(width);
    p.setStrokeCap(SkPaint::kRound_Cap);
    p.setStrokeJoin(SkPaint::kRound_Join);
    return p;
}
void rounded(SkCanvas& c, const Rect& r, float radius, SkColor colour)
{
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(r.x, r.y, r.w, r.h), radius, radius), fillPaint(colour));
}
void text(SkCanvas& c, const std::string& s, const Rect& r, SkColor colour, float size, ui::Align align = ui::Align::Centre,
          bool bold = false)
{
    paint::text(c, s, r, colour, size, align, bold);
}
bool contains(const Rect& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// A circle button with a little drawn icon.
void circleButton(SkCanvas& c, const Rect& r, bool lit, SkColor background = kField)
{
    const float d = std::min(r.w, r.h);
    c.drawCircle(r.x + r.w / 2, r.y + r.h / 2, d / 2, fillPaint(lit ? kDarkButton : background));
}
void headphones(SkCanvas& c, float cx, float cy, float s, SkColor colour)
{
    auto pen = strokePaint(colour, 1.6f);
    c.drawArc(SkRect::MakeXYWH(cx - s, cy - s * 0.9f, 2 * s, 2 * s), 180, 180, false, pen);
    c.drawRoundRect(SkRect::MakeXYWH(cx - s - 1, cy, s * 0.55f, s * 0.8f), 1.5f, 1.5f, fillPaint(colour));
    c.drawRoundRect(SkRect::MakeXYWH(cx + s * 0.45f + 1, cy, s * 0.55f, s * 0.8f), 1.5f, 1.5f, fillPaint(colour));
}
void powerIcon(SkCanvas& c, float cx, float cy, float s, SkColor colour)
{
    auto pen = strokePaint(colour, 1.6f);
    c.drawArc(SkRect::MakeXYWH(cx - s, cy - s, 2 * s, 2 * s), -60, 300, false, pen);
    c.drawLine(cx, cy - s - 1, cx, cy - 1, pen);
}
void chevron(SkCanvas& c, float cx, float cy, float s, bool right, SkColor colour)
{
    SkPathBuilder p;
    p.moveTo(cx + (right ? -s / 2 : s / 2), cy - s);
    p.lineTo(cx + (right ? s / 2 : -s / 2), cy);
    p.lineTo(cx + (right ? -s / 2 : s / 2), cy + s);
    c.drawPath(p.detach(), strokePaint(colour, 1.6f));
}
void triangleDown(SkCanvas& c, float cx, float cy, float s, SkColor colour)
{
    SkPathBuilder p;
    p.moveTo(cx - s, cy - s / 2);
    p.lineTo(cx + s, cy - s / 2);
    p.lineTo(cx, cy + s / 2);
    p.close();
    c.drawPath(p.detach(), fillPaint(colour));
}
void lockIcon(SkCanvas& c, float cx, float cy, bool locked, SkColor colour)
{
    c.drawRoundRect(SkRect::MakeXYWH(cx - 5, cy - 1, 10, 8), 1.5f, 1.5f, fillPaint(colour));
    auto pen = strokePaint(colour, 1.5f);
    if (locked) c.drawArc(SkRect::MakeXYWH(cx - 3.5f, cy - 7, 7, 9), 180, 180, false, pen);
    else c.drawArc(SkRect::MakeXYWH(cx - 0.5f, cy - 8, 7, 9), 180, 180, false, pen);
}
// The eight band shapes as small strokes.
void shapeIcon(SkCanvas& c, int shape, const Rect& r, SkColor colour)
{
    SkPathBuilder p;
    const float x0 = r.x + 4, x1 = r.x + r.w - 4, y0 = r.y + 4, y1 = r.y + r.h - 4, xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
    switch ((devices::hush::Shape) shape)
    {
        case devices::hush::Shape::LowCut: p.moveTo(x0 + 2, y1); p.quadTo(x0 + 3, y0, xm, y0); p.lineTo(x1, y0); break;
        case devices::hush::Shape::LowShelf: p.moveTo(x0, ym - 3); p.lineTo(xm - 2, ym - 3); p.quadTo(xm + 2, ym + 4, x1, ym + 4); p.moveTo(x0, ym + 4); p.lineTo(xm - 2, ym + 4); break;
        case devices::hush::Shape::HighShelf: p.moveTo(x0, ym + 4); p.quadTo(xm - 2, ym + 4, xm + 2, ym - 3); p.lineTo(x1, ym - 3); p.moveTo(xm + 2, ym + 4); p.lineTo(x1, ym + 4); break;
        case devices::hush::Shape::HighCut: p.moveTo(x0, y0); p.lineTo(xm, y0); p.quadTo(x1 - 3, y0, x1 - 2, y1); break;
        case devices::hush::Shape::Bell: p.moveTo(x0, y1); p.cubicTo(xm - 3, y1, xm - 4, y0, xm, y0); p.cubicTo(xm + 4, y0, xm + 3, y1, x1, y1); break;
        case devices::hush::Shape::BandPass: p.moveTo(x0 + 1, y1); p.quadTo(x0 + 2, y0, xm - 2, y0); p.lineTo(xm + 2, y0); p.quadTo(x1 - 2, y0, x1 - 1, y1); break;
        case devices::hush::Shape::Notch: p.moveTo(x0 + 2, y0); p.quadTo(xm - 3, y0, xm - 2, y1); p.moveTo(xm + 2, y1); p.quadTo(xm + 3, y0, x1 - 2, y0); break;
        case devices::hush::Shape::Tilt: p.moveTo(x0, y1); p.cubicTo(xm, y1, xm, y0, x1, y0); break;
    }
    c.drawPath(p.detach(), strokePaint(colour, 2.0f));
}
std::string shapeLabel(int shape) { return devices::hush::shapeName((devices::hush::Shape) shape); }
} // namespace

// ================================================================== face
HushFace::HushFace(devices::HushSession& session) : session_(session)
{
    layout();
}

float HushFace::scale() const { return width() > 0 ? width() / kWidth : 1.0f; }

ui::MouseEvent HushFace::toDesign(const ui::MouseEvent& e) const
{
    ui::MouseEvent d = e;
    d.x = e.x / scale();
    d.y = e.y / scale();
    return d;
}

bool HushFace::sidePanelOpen() const { return session_.sidePanelOpen; }

std::string HushFace::versionText() const
{
    if (! version.empty()) return version;
    if (auto* app = juce::JUCEApplicationBase::getInstance()) return app->getApplicationVersion().toStdString();
    return {};
}

std::optional<midi::ControlTarget> HushFace::mappingTargetAt(float x, float y) const
{
    const float s = scale();
    const int param = controlAt(x / s, y / s);
    if (param < 0 || ! mappingTarget) return midiTarget;
    return mappingTarget(param);
}

void HushFace::layout()
{
    left_ = {0, 0, 173, kHeight};
    toolbar_ = {173, 0, kWidth - 173, 34};
    footer_ = {173, kHeight - 35, kWidth - 173, 35};
    strip_ = {173, 34, 30, footer_.y - 34};
    side_ = session_.sidePanelOpen ? Rect{203, 34, 112, footer_.y - 34} : Rect{203, 34, 0, footer_.y - 34};
    graph_ = {side_.x + side_.w, 34, kWidth - (side_.x + side_.w), footer_.y - 34};
    bandPanel_ = {graph_.x + (graph_.w - 450) / 2, graph_.y + graph_.h - 74, 450, 64};
    buildControls();
}

float HushFace::freqToX(float hz) const
{
    // The reference's axis: log10(f + 150), 20 Hz .. 20 kHz across the graph (fitted to its labels within a pixel).
    const float lo = std::log10(170.0f), hi = std::log10(20150.0f);
    return graph_.x + graph_.w * (std::log10(std::clamp(hz, 20.0f, 20000.0f) + 150.0f) - lo) / (hi - lo);
}

float HushFace::xToFreq(float x) const
{
    const float lo = std::log10(170.0f), hi = std::log10(20150.0f);
    const float t = std::clamp((x - graph_.x) / std::max(1.0f, graph_.w), 0.0f, 1.0f);
    return std::pow(10.0f, lo + t * (hi - lo)) - 150.0f;
}

float HushFace::zeroY() const { return graph_.y + 0.475f * graph_.h; }

float HushFace::nodeY(int band) const
{
    const auto shape = (devices::hush::Shape) std::lround(value(H::bandParam(band, H::BandShape)));
    const float depth = value(H::bandParam(band, H::BandDepth));
    if (shape == devices::hush::Shape::LowCut || shape == devices::hush::Shape::HighCut) return zeroY() - depth * kCurvePx * 0.5f;
    return zeroY() - depth * kCurvePx;
}

int HushFace::bandAt(float x, float y) const
{
    int best = -1;
    float bestDistance = 12.0f;
    for (int b = 0; b < H::kBands; ++b)
    {
        if (value(H::bandParam(b, H::BandUsed)) < 0.5f) continue;
        const float dx = x - freqToX(value(H::bandParam(b, H::BandFrequency))), dy = y - nodeY(b);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestDistance) { bestDistance = d; best = b; }
    }
    return best;
}

int HushFace::createBand(float x, float y, int shape)
{
    for (int b = 0; b < H::kBands; ++b)
        if (value(H::bandParam(b, H::BandUsed)) < 0.5f)
        {
            session_.pushUndo();
            devices::hush::Band band;
            band.used = true;
            band.shape = (devices::hush::Shape) shape;
            band.frequency = std::clamp(xToFreq(x), 20.0f, 20000.0f);
            const bool cut = band.shape == devices::hush::Shape::LowCut || band.shape == devices::hush::Shape::HighCut;
            band.depth = cut ? 0.0f : std::clamp((zeroY() - y) / kCurvePx, -10.0f, 10.0f);
            const int base = H::bandParam(b, 0);
            const float values[H::kBandFields] = {1.0f, 1.0f, (float) shape, band.frequency, band.depth, band.q, band.slope, band.focus};
            for (int f = 0; f < H::kBandFields; ++f) session_.setFromEditor(base + f, values[f]);
            selectBand(b);
            return b;
        }
    return -1;
}

void HushFace::selectBand(int band)
{
    selected_ = band;
    buildControls();
    repaint();
}

float HushFace::value(int param) const { return session_.hush().getParameter(param); }

void HushFace::setValue(int param, float v)
{
    session_.setFromEditor(param, v);
    repaint();
}

void HushFace::beginEdit(int param)
{
    session_.pushUndo();
    session_.beginGesture(param);
    dragParam_ = param;
}

void HushFace::endEdit()
{
    session_.endGesture(dragParam_);
    dragParam_ = -1;
}

void HushFace::dragValue(int param, float dy, bool fine)
{
    const auto& info = session_.hush().parameters()[(size_t) param];
    const float amount = -dy / (fine ? 1500.0f : 180.0f);
    const bool logarithmic = HushSession::isLogarithmic(info);
    float v = value(param);
    if (logarithmic) v = info.minValue * std::pow(info.maxValue / info.minValue,
                                                   std::clamp(std::log(v / info.minValue) / std::log(info.maxValue / info.minValue) + amount, 0.0f, 1.0f));
    else v = std::clamp(v + amount * (info.maxValue - info.minValue), info.minValue, info.maxValue);
    setValue(param, v);
}

void HushFace::resetValue(int param)
{
    beginEdit(param);
    setValue(param, session_.hush().parameters()[(size_t) param].defaultValue);
    endEdit();
}

void HushFace::commitText()
{
    if (editing_ < 0) return;
    const auto& info = session_.hush().parameters()[(size_t) editing_];
    beginEdit(editing_);
    setValue(editing_, HushSession::parseValue(info, juce::String::fromUTF8(editText_.c_str()), value(editing_)));
    endEdit();
    editing_ = -1;
    editText_.clear();
}

// ------------------------------------------------------------------ controls
void HushFace::buildControls()
{
    controls_.clear();
    const auto add = [this](Kind k, Rect r, int param, std::string id = {}, std::function<void(const ui::MouseEvent&)> click = {}) {
        controls_.push_back({k, r, param, std::move(click), std::move(id)});
    };
    // Left panel.
    add(Kind::Choice, {48, 92, 78, 24}, H::Mode);
    add(Kind::Knob, {35, 144, 104, 104}, H::Depth);
    add(Kind::Box, {67, 266, 40, 22}, H::Depth);
    add(Kind::Knob, {60, 318, 54, 54}, H::Detail);
    add(Kind::Box, {67, 382, 40, 22}, H::Detail);
    add(Kind::Knob, {21, 428, 54, 54}, H::Attack);
    add(Kind::Box, {28, 490, 40, 22}, H::Attack);
    add(Kind::Knob, {99, 428, 54, 54}, H::Release);
    add(Kind::Box, {106, 490, 40, 22}, H::Release);
    // Side strip and panel.
    add(Kind::Button, strip_, -1, "side", [this](const ui::MouseEvent&) {
        session_.sidePanelOpen = ! session_.sidePanelOpen;
        layout();
        repaint();
    });
    if (side_.w > 0)
    {
        const float x = side_.x + 8, w = side_.w - 16;
        add(Kind::Choice, {x + 6, side_.y + 30, w - 12, 22}, H::StereoMode);
        add(Kind::Box, {x, side_.y + 76, w, 20}, H::Link);
        add(Kind::FocusPair, {x, side_.y + 118, w, 10}, H::Focus);
        add(Kind::Box, {x, side_.y + 132, w, 20}, H::Focus);
        const int tilts[3][2] = {{H::DetailTiltLow, H::DetailTiltHigh}, {H::AttackTiltLow, H::AttackTiltHigh}, {H::ReleaseTiltLow, H::ReleaseTiltHigh}};
        for (int i = 0; i < 3; ++i)
        {
            const float y = side_.y + 200 + (float) i * 44;
            add(Kind::Tilt, {x, y, w / 2 - 2, 20}, tilts[i][0]);
            add(Kind::Tilt, {x + w / 2 + 2, y, w / 2 - 2, 20}, tilts[i][1]);
        }
        add(Kind::Box, {x, side_.y + 368, w, 20}, H::MaxCut);
        add(Kind::Box, {x, side_.y + 412, w, 20}, H::WetTrim);
    }
    // Toolbar.
    add(Kind::Toggle, {186, 5, 26, 24}, H::Sidechain);
    add(Kind::Toggle, {216, 5, 26, 24}, H::SidechainListen);
    add(Kind::Button, {256, 5, 18, 24}, -1, "prev", [this](const ui::MouseEvent&) { session_.stepPreset(-1); repaint(); });
    add(Kind::Button, {276, 5, 18, 24}, -1, "next", [this](const ui::MouseEvent&) { session_.stepPreset(1); repaint(); });
    add(Kind::Button, {296, 5, 336, 24}, -1, "preset", [this](const ui::MouseEvent& e) { presetMenu(e); });
    add(Kind::Button, {650, 5, 24, 24}, -1, "undo", [this](const ui::MouseEvent&) { session_.undo(); repaint(); });
    add(Kind::Button, {678, 5, 24, 24}, -1, "redo", [this](const ui::MouseEvent&) { session_.redo(); repaint(); });
    add(Kind::Button, {712, 5, 24, 24}, -1, "a", [this](const ui::MouseEvent&) { session_.selectSlot(0); repaint(); });
    add(Kind::Button, {736, 5, 24, 24}, -1, "b", [this](const ui::MouseEvent&) { session_.selectSlot(1); repaint(); });
    add(Kind::Button, {766, 5, 48, 24}, -1, "copy", [this](const ui::MouseEvent&) { session_.copyToOtherSlot(); repaint(); });
    add(Kind::Button, {826, 5, 24, 24}, -1, "help", [this](const ui::MouseEvent& e) {
        juce::PopupMenu m;
        m.addSectionHeader(juce::String("Sonora Hush ") + juce::String(versionText()));
        m.addItem(1, "Open the Hush page");
        m.addItem(2, "Copy system information");
        m.addSeparator();
        m.addItem(10, "Double-click the graph: add a band (edges: cuts, Cmd: band pass)", false);
        m.addItem(11, "Drag a node: frequency and depth; wheel: Q or slope; Cmd+wheel: focus", false);
        m.addItem(12, "Shift: fine; Option-drag: band listen; double-click a node: on/off", false);
        m.addItem(13, "Option-click a value: default; double-click a value: type it", false);
        showMenu(m, e, [this](int r) {
            if (r == 1) juce::URL("https://sonoradaw.com/download.html#hush").launchInDefaultBrowser(); // public page; docs/HUSH.md is private
            if (r == 2)
                juce::SystemClipboard::copyTextToClipboard(
                    "Sonora Hush " + juce::String(versionText()) + ", " + juce::SystemStats::getOperatingSystemName() + ", "
                    + juce::SystemStats::getCpuModel() + ", host " + juce::String::fromUTF8(session_.hostName.c_str()) + ", "
                    + juce::String(session_.hush().sampleRate()) + " Hz, latency " + juce::String(session_.hush().currentLatency()));
        });
    });
    add(Kind::Button, {852, 5, 24, 24}, -1, "settings", [this](const ui::MouseEvent& e) {
        auto& settings = HushSettings::get();
        juce::PopupMenu m;
        m.addSectionHeader("Settings (all instances)");
        m.addItem(1, "Show tooltips", true, settings.showTooltips.load());
        m.addItem(2, "Always render on ultra", true, settings.renderUltra.load());
        m.addItem(3, "Low latency for all instances", true, settings.lowLatencyAll.load());
        m.addItem(4, "Band listen: on node drag", true, settings.listenOnDrag.load());
        m.addItem(5, "Band listen: use delta", true, settings.listenDelta.load());
        m.addItem(6, "Show frequency near cursor", true, settings.frequencyNearCursor.load());
        showMenu(m, e, [this](int r) {
            auto& set = HushSettings::get();
            std::atomic<bool>* flags[] = {&set.showTooltips, &set.renderUltra, &set.lowLatencyAll, &set.listenOnDrag,
                                          &set.listenDelta, &set.frequencyNearCursor};
            if (r < 1 || r > 6) return;
            flags[r - 1]->store(! flags[r - 1]->load());
            set.save();
            repaint();
        });
    });
    add(Kind::Button, {878, 5, 18, 24}, -1, "size", [this](const ui::MouseEvent& e) {
        juce::PopupMenu m;
        constexpr float m1 = 1050.0f / kWidth; // "M" is the reference's own size
        const float sizes[4] = {0.8f * m1, m1, 1.25f * m1, 1.5f * m1};
        const char* names[4] = {"S", "M", "L", "XL"};
        if (onOpenWindow) m.addItem(10, "Open in a window");
        if (onScaleChosen)
            for (int i = 0; i < 4; ++i) m.addItem(i + 1, names[i], true, std::abs(session_.editorScale - sizes[i]) < 0.01f);
        showMenu(m, e, [this, sizes](int r) {
            if (r == 10 && onOpenWindow) { onOpenWindow(); return; }
            if (r < 1 || r > 4) return;
            session_.editorScale = sizes[r - 1];
            if (onScaleChosen) onScaleChosen(session_.editorScale);
        });
    });
    // Graph range.
    add(Kind::Button, {graph_.x + graph_.w - 42, graph_.y + graph_.h - 28, 36, 20}, -1, "range", [this](const ui::MouseEvent& e) {
        juce::PopupMenu m;
        const float ranges[6] = {3, 6, 9, 12, 18, 24};
        for (int i = 0; i < 6; ++i) m.addItem(i + 1, "-" + juce::String((int) ranges[i]), true, std::abs(session_.graphRange - ranges[i]) < 0.1f);
        showMenu(m, e, [this, ranges](int r) { if (r > 0) { session_.graphRange = ranges[r - 1]; repaint(); } });
    });
    // Selected band.
    if (selected_ >= 0)
    {
        const auto& p = bandPanel_;
        const int b = selected_;
        add(Kind::Toggle, {p.x + 12, p.y + 23, 18, 18}, H::bandParam(b, H::BandEnabled));
        add(Kind::Button, {p.x + 12, p.y + 43, 18, 18}, -1, "listen", [this, b](const ui::MouseEvent&) {
            const bool on = std::lround(value(H::BandListen)) == b + 1;
            beginEdit(H::BandListen);
            setValue(H::BandListen, on ? 0.0f : (float) (b + 1));
            endEdit();
        });
        for (int s = 0; s < devices::hush::kShapes; ++s)
        {
            // The reference's order: low cut, low shelf, high shelf, high cut / bell, band pass, band reject, tilt.
            static constexpr int kShapeOrder[8] = {0, 1, 6, 7, 2, 3, 4, 5};
            const int shape = kShapeOrder[s];
            add(Kind::Button, {p.x + 40 + (float) (s % 4) * 27, p.y + 19 + (float) (s / 4) * 22, 25, 20}, -1, "shape",
                [this, b, shape](const ui::MouseEvent&) {
                    beginEdit(H::bandParam(b, H::BandShape));
                    setValue(H::bandParam(b, H::BandShape), (float) shape);
                    endEdit();
                    buildControls();
                });
        }
        const auto shape = (devices::hush::Shape) std::lround(value(H::bandParam(b, H::BandShape)));
        const int qOrSlope = devices::hush::usesSlope(shape) ? H::bandParam(b, H::BandSlope) : H::bandParam(b, H::BandQ);
        const int knobs[3] = {H::bandParam(b, H::BandFrequency), H::bandParam(b, H::BandDepth), qOrSlope};
        for (int i = 0; i < 3; ++i)
        {
            const float cx = p.x + 190 + (float) i * 64;
            add(Kind::Knob, {cx - 13, p.y + 18, 26, 26}, knobs[i]);
            add(Kind::Box, {cx - 27, p.y + 45, 54, 17}, knobs[i]);
        }
        add(Kind::FocusPair, {p.x + 360, p.y + 26, 78, 9}, H::bandParam(b, H::BandFocus));
        add(Kind::Box, {p.x + 360, p.y + 45, 78, 17}, H::bandParam(b, H::BandFocus));
        add(Kind::Button, {p.x + p.w - 8, p.y + p.h - 20, 22, 22}, -1, "delete", [this, b](const ui::MouseEvent&) {
            session_.pushUndo();
            session_.setFromEditor(H::bandParam(b, H::BandUsed), 0.0f);
            if (std::lround(value(H::BandListen)) == b + 1) session_.setFromEditor(H::BandListen, 0.0f);
            selectBand(-1);
        });
    }
    // Footer.
    add(Kind::Toggle, {181, footer_.y + 6, 78, 24}, H::Bypass);
    add(Kind::Toggle, {264, footer_.y + 6, 64, 24}, H::Delta);
    add(Kind::Box, {372, footer_.y + 6, 70, 24}, H::Mix);
    add(Kind::Box, {478, footer_.y + 6, 74, 24}, H::OutTrim);
    add(Kind::Button, {598, footer_.y + 6, 18, 24}, -1, "qlock", [this](const ui::MouseEvent&) { session_.qualityLocked = ! session_.qualityLocked; repaint(); });
    add(Kind::Button, {672, footer_.y + 6, 96, 24}, -1, "quality", [this](const ui::MouseEvent& e) {
        juce::PopupMenu m;
        const int q = (int) std::lround(value(H::Quality));
        const bool linear = value(H::LinearPhase) >= 0.5f;
        m.addItem(1, "normal", true, q == 0);
        m.addItem(2, "high", true, q == 1);
        m.addItem(3, "ultra", true, q == 2);
        m.addSeparator();
        m.addItem(4, "linear phase", true, linear);
        showMenu(m, e, [this, linear](int r) {
            if (r >= 1 && r <= 3) { beginEdit(H::Quality); setValue(H::Quality, (float) (r - 1)); endEdit(); }
            if (r == 4) { beginEdit(H::LinearPhase); setValue(H::LinearPhase, linear ? 0.0f : 1.0f); endEdit(); }
        });
    });
    add(Kind::Button, {776, footer_.y + 6, 16, 24}, -1, "llock", [this](const ui::MouseEvent&) { session_.lowLatencyLocked = ! session_.lowLatencyLocked; repaint(); });
    add(Kind::Toggle, {794, footer_.y + 6, 100, 24}, H::LowLatency);
}

const HushFace::Control* HushFace::hit(float x, float y) const
{
    for (auto it = controls_.rbegin(); it != controls_.rend(); ++it)
        if (contains(it->r, x, y)) return &*it;
    return nullptr;
}

int HushFace::controlAt(float x, float y) const
{
    const auto* c = hit(x, y);
    return c != nullptr ? c->param : -1;
}

// ------------------------------------------------------------------ menus
void HushFace::showMenu(juce::PopupMenu menu, const ui::MouseEvent& at, std::function<void(int)> done)
{
    lastMenu.clear();
    for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
        lastMenu.push_back(it.getItem().text.toStdString());
    lastMenuResult = done;
    auto* host = menuHost ? menuHost() : nullptr;
    if (host == nullptr) return;
    auto options = juce::PopupMenu::Options().withTargetScreenArea(
        juce::Rectangle<int>(juce::roundToInt(at.screenX), juce::roundToInt(at.screenY), 1, 1));
    juce::Component::SafePointer<juce::Component> safe(host);
    menu.showMenuAsync(options, [safe, callback = std::move(done)](int result) {
        if (safe != nullptr) callback(result);
    });
}

void HushFace::presetMenu(const ui::MouseEvent& e)
{
    juce::PopupMenu m;
    const auto presets = session_.allPresets();
    std::vector<std::string> categories;
    for (const auto& p : presets)
        if (std::find(categories.begin(), categories.end(), p.category) == categories.end()) categories.push_back(p.category);
    for (const auto& category : categories)
    {
        juce::PopupMenu sub;
        for (size_t i = 0; i < presets.size(); ++i)
            if (presets[i].category == category)
                sub.addItem((int) i + 1, juce::String::fromUTF8(presets[i].name.c_str()), true, presets[i].name == session_.presetName());
        m.addSubMenu(juce::String::fromUTF8(category.c_str()), sub);
    }
    m.addSeparator();
    const bool userPreset = std::any_of(presets.begin(), presets.end(), [this](const HushPreset& p) {
        return p.name == session_.presetName() && p.file.isAChildOf(HushSession::userPresetFolder());
    });
    m.addItem(10004, "Save", userPreset && session_.presetModified());
    m.addItem(10001, "Save as...");
    m.addItem(10005, "Set as default", true, HushSettings::get().defaultPreset == session_.presetName());
    m.addItem(10002, "Open preset file...");
    m.addItem(10003, "Show user presets in Finder");
    showMenu(m, e, [this, presets](int r) {
        if (r >= 1 && r <= (int) presets.size()) { session_.loadPreset(presets[(size_t) r - 1]); repaint(); }
        if (r == 10001)
        {
            auto* window = new juce::AlertWindow("Save preset", "Name and category", juce::MessageBoxIconType::NoIcon);
            window->addTextEditor("name", juce::String::fromUTF8(session_.presetName().c_str()), "name");
            window->addTextEditor("category", "user", "category");
            window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
            window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            window->enterModalState(true, juce::ModalCallbackFunction::create([this, window](int result) {
                if (result == 1)
                    session_.savePreset(window->getTextEditorContents("name"), window->getTextEditorContents("category"), true);
                repaint();
            }), true);
        }
        if (r == 10002)
        {
            auto chooser = std::make_shared<juce::FileChooser>("Open a preset file", HushSession::userPresetFolder(), "*.json;*.preset");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                 [this, chooser](const juce::FileChooser& fc) {
                                     HushPreset p;
                                     if (fc.getResult().existsAsFile() && HushSession::readPresetFile(fc.getResult(), p))
                                     {
                                         session_.loadPreset(p);
                                         repaint();
                                     }
                                 });
        }
        if (r == 10004) { session_.savePreset(juce::String::fromUTF8(session_.presetName().c_str()), juce::String::fromUTF8(session_.presetCategory().c_str()), true); repaint(); }
        if (r == 10005)
        {
            auto& set = HushSettings::get();
            set.defaultPreset = set.defaultPreset == session_.presetName() ? std::string() : session_.presetName();
            set.save();
        }
        if (r == 10003)
        {
            HushSession::userPresetFolder().createDirectory();
            HushSession::userPresetFolder().revealToUser();
        }
    });
}

void HushFace::nodeMenu(int band, const ui::MouseEvent& e)
{
    juce::PopupMenu m;
    m.addSectionHeader("Band " + juce::String(band + 1));
    m.addItem(1, "enabled", true, value(H::bandParam(band, H::BandEnabled)) >= 0.5f);
    m.addItem(2, "listen", true, std::lround(value(H::BandListen)) == band + 1);
    m.addSeparator();
    const int shape = (int) std::lround(value(H::bandParam(band, H::BandShape)));
    for (int s = 0; s < devices::hush::kShapes; ++s) m.addItem(10 + s, juce::String(shapeLabel(s)), true, s == shape);
    m.addSeparator();
    m.addItem(3, "delete");
    showMenu(m, e, [this, band](int r) {
        session_.pushUndo();
        if (r == 1) session_.setFromEditor(H::bandParam(band, H::BandEnabled), value(H::bandParam(band, H::BandEnabled)) >= 0.5f ? 0.0f : 1.0f);
        if (r == 2) session_.setFromEditor(H::BandListen, std::lround(value(H::BandListen)) == band + 1 ? 0.0f : (float) (band + 1));
        if (r >= 10 && r < 10 + devices::hush::kShapes) session_.setFromEditor(H::bandParam(band, H::BandShape), (float) (r - 10));
        if (r == 3) { session_.setFromEditor(H::bandParam(band, H::BandUsed), 0.0f); selectBand(-1); }
        buildControls();
        repaint();
    });
}

void HushFace::shapeMenu(const ui::MouseEvent& e, float x, float y)
{
    juce::PopupMenu m;
    m.addSectionHeader("New band");
    for (int s = 0; s < devices::hush::kShapes; ++s) m.addItem(1 + s, juce::String(shapeLabel(s)));
    showMenu(m, e, [this, x, y](int r) { if (r > 0) createBand(x, y, r - 1); });
}

// ------------------------------------------------------------------ input
void HushFace::mouseDown(const ui::MouseEvent& raw)
{
    const auto e = toDesign(raw);
    if (editing_ >= 0) commitText();
    lastX_ = e.x;
    lastY_ = e.y;
    if (const auto* c = hit(e.x, e.y))
    {
        if (c->kind == Kind::Button)
        {
            if (! e.right && c->click) c->click(raw);
            return;
        }
        if (e.alt && (c->kind == Kind::Box || c->kind == Kind::Knob)) { resetValue(c->param); return; }
        if (c->kind == Kind::Toggle)
        {
            if (c->param == H::Sidechain && onSidechainToggle && onSidechainToggle()) return;
            beginEdit(c->param);
            setValue(c->param, value(c->param) >= 0.5f ? 0.0f : 1.0f);
            endEdit();
            return;
        }
        if (c->kind == Kind::Choice)
        {
            const auto& info = session_.hush().parameters()[(size_t) c->param];
            const int count = (int) info.choices.size();
            const int index = std::clamp((int) ((e.x - c->r.x) / c->r.w * (float) count), 0, count - 1);
            beginEdit(c->param);
            setValue(c->param, (float) index);
            endEdit();
            return;
        }
        if (c->kind == Kind::FocusPair)
        {
            beginEdit(c->param);
            setValue(c->param, std::clamp((e.x - c->r.x) / c->r.w, 0.0f, 1.0f));
            return;
        }
        beginEdit(c->param); // knob, box, tilt: vertical drag
        return;
    }
    if (contains(graph_, e.x, e.y))
    {
        const int b = bandAt(e.x, e.y);
        if (e.right)
        {
            if (b >= 0) { selectBand(b); nodeMenu(b, raw); }
            else shapeMenu(raw, e.x, e.y);
            return;
        }
        selectBand(b);
        if (b >= 0)
        {
            session_.pushUndo();
            dragBand_ = b;
            session_.beginGesture(H::bandParam(b, H::BandFrequency));
            session_.beginGesture(H::bandParam(b, H::BandDepth));
            if (e.alt || HushSettings::get().listenOnDrag.load())
            {
                dragListen_ = true;
                session_.setFromEditor(H::BandListen, (float) (b + 1));
            }
        }
    }
}

void HushFace::mouseDrag(const ui::MouseEvent& raw)
{
    const auto e = toDesign(raw);
    const float dy = e.y - lastY_, dx = e.x - lastX_;
    mouseX_ = e.x;
    mouseY_ = e.y;
    lastX_ = e.x;
    lastY_ = e.y;
    if (dragBand_ >= 0)
    {
        const int b = dragBand_;
        const bool fine = e.shift;
        const auto shape = (devices::hush::Shape) std::lround(value(H::bandParam(b, H::BandShape)));
        const bool cut = shape == devices::hush::Shape::LowCut || shape == devices::hush::Shape::HighCut;
        const float scaleMove = fine ? 0.1f : 1.0f;
        const float x = freqToX(value(H::bandParam(b, H::BandFrequency))) + dx * scaleMove;
        setValue(H::bandParam(b, H::BandFrequency), std::clamp(xToFreq(x), 20.0f, 20000.0f));
        const float perPx = (cut ? 2.0f : 1.0f) / kCurvePx;
        setValue(H::bandParam(b, H::BandDepth), std::clamp(value(H::bandParam(b, H::BandDepth)) - dy * perPx * scaleMove, -10.0f, 10.0f));
        return;
    }
    if (dragParam_ < 0) return;
    for (const auto& control : controls_)
        if (control.param == dragParam_ && control.kind == Kind::FocusPair)
        {
            setValue(dragParam_, std::clamp((e.x - control.r.x) / control.r.w, 0.0f, 1.0f));
            return;
        }
    dragValue(dragParam_, dy, e.shift);
}

void HushFace::mouseUp(const ui::MouseEvent&)
{
    if (dragBand_ >= 0)
    {
        session_.endGesture(H::bandParam(dragBand_, H::BandFrequency));
        session_.endGesture(H::bandParam(dragBand_, H::BandDepth));
        if (dragListen_) session_.setFromEditor(H::BandListen, 0.0f);
        dragListen_ = false;
        dragBand_ = -1;
    }
    endEdit();
}

void HushFace::mouseDoubleClick(const ui::MouseEvent& raw)
{
    const auto e = toDesign(raw);
    if (const auto* c = hit(e.x, e.y))
    {
        if (c->kind == Kind::Box)
        {
            endEdit();
            editing_ = c->param;
            editText_.clear();
                    repaint();
            return;
        }
        if (c->kind == Kind::Knob) { resetValue(c->param); return; }
        mouseDown(raw);
        return;
    }
    if (! contains(graph_, e.x, e.y)) return;
    const int b = bandAt(e.x, e.y);
    if (b >= 0)
    {
        endEdit();
        mouseUp(raw);
        session_.pushUndo();
        session_.setFromEditor(H::bandParam(b, H::BandEnabled), value(H::bandParam(b, H::BandEnabled)) >= 0.5f ? 0.0f : 1.0f);
        repaint();
        return;
    }
    // New band: cuts near the edges, a band pass with Cmd, otherwise a bell.
    const float t = (e.x - graph_.x) / graph_.w;
    const int shape = e.cmd ? (int) devices::hush::Shape::BandPass
                    : t < 0.08f ? (int) devices::hush::Shape::LowCut
                    : t > 0.92f ? (int) devices::hush::Shape::HighCut
                                : (int) devices::hush::Shape::Bell;
    createBand(e.x, e.y, shape);
}

bool HushFace::mouseWheel(const ui::MouseEvent& raw)
{
    const auto e = toDesign(raw);
    const float delta = e.wheelDeltaY * (e.shift ? 0.15f : 1.0f);
    int param = -1;
    if (const auto* c = hit(e.x, e.y); c != nullptr && c->kind != Kind::Button && c->kind != Kind::Toggle) param = c->param;
    else if (contains(graph_, e.x, e.y))
    {
        const int b = bandAt(e.x, e.y) >= 0 ? bandAt(e.x, e.y) : selected_;
        if (b < 0) return false;
        const auto shape = (devices::hush::Shape) std::lround(value(H::bandParam(b, H::BandShape)));
        param = e.cmd ? H::bandParam(b, H::BandFocus) : devices::hush::usesSlope(shape) ? H::bandParam(b, H::BandSlope) : H::bandParam(b, H::BandQ);
    }
    if (param < 0) return false;
    beginEdit(param);
    dragValue(param, -delta * 60.0f, false);
    endEdit();
    return true;
}

void HushFace::mouseMove(const ui::MouseEvent& raw)
{
    const auto e = toDesign(raw);
    mouseX_ = e.x;
    mouseY_ = e.y;
    if (HushSettings::get().showTooltips.load() || HushSettings::get().frequencyNearCursor.load()) repaint();
    const int band = contains(graph_, e.x, e.y) ? bandAt(e.x, e.y) : -1;
    const auto* c = hit(e.x, e.y);
    const int control = c != nullptr ? (int) (c - controls_.data()) : -1;
    if (band != hoverBand_ || control != hoverControl_)
    {
        hoverBand_ = band;
        hoverControl_ = control;
        repaint();
    }
}

void HushFace::mouseExit()
{
    mouseX_ = mouseY_ = -1;
    hoverBand_ = hoverControl_ = -1;
    repaint();
}

bool HushFace::keyPressed(const ui::KeyEvent& k)
{
    if (editing_ >= 0)
    {
        if (k.keyCode == juce::KeyPress::returnKey) { commitText(); repaint(); return true; }
        if (k.keyCode == juce::KeyPress::escapeKey) { editing_ = -1; editText_.clear(); repaint(); return true; }
        if (k.keyCode == juce::KeyPress::backspaceKey) { paint::popLastCodePoint(editText_); repaint(); return true; }
        if (k.textChar >= 32 && k.textChar < 127 && editText_.size() < 16)
        {
            editText_.push_back((char) k.textChar);
            repaint();
        }
        return true;
    }
    if (k.cmd && (k.textChar == 'z' || k.textChar == 'Z'))
    {
        if (k.shift) session_.redo();
        else session_.undo();
        repaint();
        return true;
    }
    if ((k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey) && selected_ >= 0)
    {
        session_.pushUndo();
        session_.setFromEditor(H::bandParam(selected_, H::BandUsed), 0.0f);
        selectBand(-1);
        return true;
    }
    return false;
}

void HushFace::tick()
{
    const unsigned revision = session_.hush().displayRevision();
    if (revision != lastRevision_)
    {
        lastRevision_ = revision;
        repaint();
    }
    if (++ticks_ % 15 == 0)
    {
        session_.applySettings(); // the settings menu is shared by every instance
        repaint();                // preset name / modified star, host automation
    }
}

// ------------------------------------------------------------------ painting
void HushFace::paint(SkCanvas& c, const ui::Theme&)
{
    c.save();
    c.scale(scale(), scale());
    paintGraph(c);
    paintLeft(c);
    paintSide(c);
    paintToolbar(c);
    paintFooter(c);
    paintOverlay(c);
    c.restore();
}

std::string HushFace::tooltipFor(float x, float y) const
{
    const auto* c = hit(x, y);
    if (c == nullptr)
    {
        if (contains(graph_, x, y))
            return bandAt(x, y) >= 0 ? "Drag: frequency and depth. Double-click: on/off. Right-click: shape and options"
                                     : "Double-click to add a band; right-click to pick its shape";
        return {};
    }
    if (c->param >= 0)
    {
        const auto& info = session_.hush().parameters()[(size_t) c->param];
        return info.name + ": " + HushSession::formatValue(info, value(c->param)).toStdString();
    }
    static const std::pair<const char*, const char*> kButtons[] = {
        {"side", "Stereo, tilt and scale controls"}, {"prev", "Previous preset"}, {"next", "Next preset"},
        {"preset", "Presets: load, save, set as default"}, {"undo", "Undo"}, {"redo", "Redo"}, {"a", "Settings A"},
        {"b", "Settings B"}, {"copy", "Copy these settings to the other slot"}, {"help", "Help"}, {"settings", "Settings"},
        {"size", "Window size"}, {"range", "Reduction graph range"}, {"listen", "Band listen"}, {"shape", "Band shape"},
        {"delete", "Delete band"}, {"qlock", "Keep quality when presets change"}, {"quality", "Quality and linear phase"},
        {"llock", "Keep low latency when presets change"}};
    for (const auto& [id, text] : kButtons)
        if (c->id == id) return text;
    return {};
}

void HushFace::paintOverlay(SkCanvas& c)
{
    if (mouseX_ < 0 || dragParam_ >= 0) return;
    const auto& settings = HushSettings::get();
    if ((settings.frequencyNearCursor.load() || dragBand_ >= 0) && contains(graph_, mouseX_, mouseY_))
    {
        const float hz = dragBand_ >= 0 ? value(H::bandParam(dragBand_, H::BandFrequency)) : xToFreq(mouseX_);
        const std::string label = hz < 1000 ? std::to_string((int) std::lround(hz)) + " Hz"
                                            : juce::String(hz / 1000.0f, 2).toStdString() + " kHz";
        const Rect r{mouseX_ + 12, mouseY_ - 24, gfx::measureText(label, 11.0f) + 10, 18};
        rounded(c, r, 4.0f, 0xE0FFFFFF);
        text(c, label, r, kText, 11.0f);
    }
    if (! settings.showTooltips.load() || dragBand_ >= 0) return;
    const auto tip = tooltipFor(mouseX_, mouseY_);
    if (tip.empty()) return;
    const float w = gfx::measureText(tip, 11.0f) + 14;
    Rect r{std::min(mouseX_ + 14, kWidth - w - 4), mouseY_ + 18, w, 20};
    if (r.y + r.h > kHeight - 2) r.y = mouseY_ - 26;
    rounded(c, r, 4.0f, 0xF02E3140);
    text(c, tip, r, 0xFFFFFFFF, 11.0f);
}

void HushFace::paintKnob(SkCanvas& c, float cx, float cy, float radius, int param)
{
    const auto& info = session_.hush().parameters()[(size_t) param];
    const bool logarithmic = info.unit == "Hz" || juce::String(info.name).endsWith(" q");
    const float v = value(param);
    float t = logarithmic ? std::log(v / info.minValue) / std::log(info.maxValue / info.minValue)
                          : (v - info.minValue) / std::max(1.0e-6f, info.maxValue - info.minValue);
    t = std::clamp(t, 0.0f, 1.0f);
    const float ring = std::max(3.0f, radius * 0.16f);
    const SkRect arc = SkRect::MakeXYWH(cx - radius, cy - radius, 2 * radius, 2 * radius);
    c.drawArc(arc, 135.0f, 270.0f, false, strokePaint(kArcRest, ring));
    if (t > 0.001f) c.drawArc(arc, 135.0f, 270.0f * t, false, strokePaint(kArcValue, ring));
    // Body: dark disc with a soft top light and a drop shadow.
    const float body = radius - ring * 0.9f;
    c.drawCircle(cx, cy + 2.5f, body, fillPaint(0x30000000));
    SkPaint shade = fillPaint(kKnob);
    const SkPoint ends[2] = {{cx, cy - body}, {cx, cy + body}};
    const SkColor colours[2] = {kKnobLight, kKnob};
    shade.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
    c.drawCircle(cx, cy, body, shade);
}

void HushFace::paintBox(SkCanvas& c, const Rect& r, int param, float fontSize)
{
    rounded(c, r, 4.0f, kBox);
    std::string s;
    if (editing_ == param) s = editText_ + "|";
    else s = HushSession::formatValue(session_.hush().parameters()[(size_t) param], value(param)).toStdString();
    text(c, s, r, kText, fontSize);
}

void HushFace::paintFocus(SkCanvas& c, const Rect& r, int param)
{
    const float f = value(param);
    const float left = devices::hush::focusAmount(f, 0), right = devices::hush::focusAmount(f, 1);
    const float half = r.w / 2 - 1.5f;
    rounded(c, {r.x, r.y, half, r.h}, r.h / 2, paint::withAlpha(kCurve, 110));
    rounded(c, {r.x + half + 3, r.y, half, r.h}, r.h / 2, paint::withAlpha(kCurve, 110));
    rounded(c, {r.x + half * (1 - left), r.y, half * left, r.h}, r.h / 2, kCurve);
    rounded(c, {r.x + half + 3, r.y, half * right, r.h}, r.h / 2, kCurve);
}

void HushFace::paintTilt(SkCanvas& c, const Rect& r, int param)
{
    rounded(c, r, 4.0f, kBox);
    // A line tilted by the multiplier: flat at x1, up for slower/narrower, down for faster/wider.
    const float t = std::log10(std::clamp(value(param), 0.1f, 10.0f)); // -1..1
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, half = r.w * 0.32f;
    c.drawLine(cx - half, cy + t * 6.0f, cx + half, cy - t * 6.0f, strokePaint(kText, 1.8f));
    if (hoverControl_ >= 0 && hoverControl_ < (int) controls_.size() && controls_[(size_t) hoverControl_].param == param)
        text(c, HushSession::formatValue(session_.hush().parameters()[(size_t) param], value(param)).toStdString(),
             {r.x, r.y - 15, r.w, 14}, kText, 10.0f);
}

void HushFace::paintLeft(SkCanvas& c)
{
    SkPaint bg;
    const SkPoint ends[2] = {{0, 0}, {0, kHeight}};
    const SkColor colours[2] = {kPanel, kPanelDark};
    bg.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
    c.drawRect(SkRect::MakeXYWH(left_.x, left_.y, left_.w, left_.h), bg);
    // Wordmark: Sonora's own, not the reference's logo.
    text(c, "hush", {14, 18, 120, 44}, kLogo, 40.0f, ui::Align::Left, true);
    text(c, "sonora", {16, 62, 80, 16}, kLogo, 13.0f, ui::Align::Left, true);
    text(c, versionText(), {90, 62, 70, 16}, kLogo, 12.0f, ui::Align::Right);
    // Mode toggle.
    const Rect mode{48, 92, 78, 24};
    rounded(c, mode, 12.0f, kField);
    const bool hard = value(H::Mode) >= 0.5f;
    rounded(c, {mode.x + (hard ? 39.0f : 0.0f), mode.y, 39, 24}, 12.0f, kDarkButton);
    text(c, "soft", {mode.x, mode.y, 39, 24}, hard ? kText : 0xFFFFFFFF, 13.0f);
    text(c, "hard", {mode.x + 39, mode.y, 39, 24}, hard ? 0xFFFFFFFF : kText, 13.0f);
    text(c, "depth", {37, 124, 100, 18}, kLabel, 14.0f);
    paintKnob(c, 87, 196, 52, H::Depth);
    paintBox(c, {67, 266, 40, 22}, H::Depth, 14.0f);
    text(c, "detail", {37, 298, 100, 18}, kLabel, 14.0f);
    paintKnob(c, 87, 345, 27, H::Detail);
    paintBox(c, {67, 382, 40, 22}, H::Detail, 14.0f);
    text(c, "attack", {0, 408, 96, 18}, kLabel, 14.0f);
    text(c, "release", {78, 408, 96, 18}, kLabel, 14.0f);
    paintKnob(c, 48, 455, 27, H::Attack);
    paintKnob(c, 126, 455, 27, H::Release);
    paintBox(c, {28, 490, 40, 22}, H::Attack, 14.0f);
    paintBox(c, {106, 490, 40, 22}, H::Release, 14.0f);
}

void HushFace::paintSide(SkCanvas& c)
{
    c.drawRect(SkRect::MakeXYWH(strip_.x, strip_.y, strip_.w + side_.w, strip_.h), fillPaint(kStrip));
    c.drawLine(strip_.x + strip_.w, strip_.y, strip_.x + strip_.w, strip_.y + strip_.h, strokePaint(0x30FFFFFF, 1.0f));
    // Indicator lights: a section differs from its defaults.
    const auto& infos = session_.hush().parameters();
    const auto differs = [&](std::initializer_list<int> params) {
        for (int p : params)
            if (std::abs(value(p) - infos[(size_t) p].defaultValue) > 1.0e-4f) return true;
        return false;
    };
    const bool lit[3] = {differs({H::StereoMode, H::Link, H::Focus}),
                         differs({H::DetailTiltLow, H::DetailTiltHigh, H::AttackTiltLow, H::AttackTiltHigh, H::ReleaseTiltLow, H::ReleaseTiltHigh}),
                         differs({H::MaxCut, H::WetTrim})};
    const char* names[3] = {"stereo", "tilt", "scale"};
    const float ys[3] = {strip_.y + 50, strip_.y + 260, strip_.y + 400};
    for (int i = 0; i < 3; ++i)
    {
        c.drawCircle(strip_.x + strip_.w / 2, ys[i] - 42, 4.5f, fillPaint(lit[i] ? 0xFFF4F5FC : 0xFF8197C4));
        c.save();
        c.translate(strip_.x + strip_.w / 2, ys[i]);
        c.rotate(-90);
        text(c, names[i], {-34, -9, 68, 18}, kLabel, 13.0f);
        c.restore();
    }
    if (side_.w <= 0) return;
    const float x = side_.x + 8, w = side_.w - 16;
    const auto header = [&](const char* name, float y) {
        auto pen = strokePaint(kLogo, 1.2f);
        const float tw = gfx::measureText(name, 13.0f, true);
        c.drawLine(x, y + 9, x + w / 2 - tw / 2 - 4, y + 9, pen);
        c.drawLine(x + w / 2 + tw / 2 + 4, y + 9, x + w, y + 9, pen);
        text(c, name, {x, y, w, 18}, kLogo, 13.0f, ui::Align::Centre, true);
    };
    header("stereo", side_.y + 8);
    const Rect st{x + 6, side_.y + 30, w - 12, 22};
    rounded(c, st, 11.0f, kField);
    const bool ms = value(H::StereoMode) >= 0.5f;
    rounded(c, {st.x + (ms ? st.w / 2 : 0.0f), st.y, st.w / 2, st.h}, 11.0f, kDarkButton);
    text(c, "L/R", {st.x, st.y, st.w / 2, st.h}, ms ? kText : 0xFFFFFFFF, 12.0f);
    text(c, "M/S", {st.x + st.w / 2, st.y, st.w / 2, st.h}, ms ? 0xFFFFFFFF : kText, 12.0f);
    text(c, "link", {x, side_.y + 58, w, 16}, kLabel, 13.0f);
    paintBox(c, {x, side_.y + 76, w, 20}, H::Link, 12.0f);
    text(c, "focus", {x, side_.y + 100, w, 16}, kLabel, 13.0f);
    paintFocus(c, {x, side_.y + 118, w, 10}, H::Focus);
    paintBox(c, {x, side_.y + 132, w, 20}, H::Focus, 12.0f);
    header("tilt", side_.y + 162);
    const char* tilts[3] = {"detail", "attack", "release"};
    const int params[3][2] = {{H::DetailTiltLow, H::DetailTiltHigh}, {H::AttackTiltLow, H::AttackTiltHigh}, {H::ReleaseTiltLow, H::ReleaseTiltHigh}};
    for (int i = 0; i < 3; ++i)
    {
        const float y = side_.y + 200 + (float) i * 44;
        text(c, tilts[i], {x, y - 18, w, 16}, kLabel, 13.0f);
        paintTilt(c, {x, y, w / 2 - 2, 20}, params[i][0]);
        paintTilt(c, {x + w / 2 + 2, y, w / 2 - 2, 20}, params[i][1]);
    }
    header("scale", side_.y + 330);
    text(c, "max cut", {x, side_.y + 350, w, 16}, kLabel, 13.0f);
    paintBox(c, {x, side_.y + 368, w, 20}, H::MaxCut, 12.0f);
    text(c, "wet trim", {x, side_.y + 394, w, 16}, kLabel, 13.0f);
    paintBox(c, {x, side_.y + 412, w, 20}, H::WetTrim, 12.0f);
}

void HushFace::paintToolbar(SkCanvas& c)
{
    c.drawRect(SkRect::MakeXYWH(toolbar_.x, toolbar_.y, toolbar_.w, toolbar_.h), fillPaint(kToolbar));
    rounded(c, {177, 6, 4, 22}, 2.0f, 0xFF7890C0);
    const bool sc = value(H::Sidechain) >= 0.5f, scListen = value(H::SidechainListen) >= 0.5f;
    circleButton(c, {186, 5, 26, 24}, sc);
    text(c, "sc", {186, 5, 26, 24}, sc ? 0xFFFFFFFF : kText, 13.0f, ui::Align::Centre, true);
    circleButton(c, {216, 5, 26, 24}, scListen);
    headphones(c, 229, 17, 6.0f, scListen ? 0xFFFFFFFF : kText);
    // Preset field.
    const Rect field{252, 5, 384, 24};
    rounded(c, field, 6.0f, kField);
    chevron(c, 265, 17, 7, false, 0xFF8090B0);
    chevron(c, 285, 17, 7, true, kText);
    std::string name = session_.presetName();
    if (session_.presetModified()) name += " *";
    text(c, name, {302, 5, 260, 24}, kText, 14.0f, ui::Align::Left);
    const std::string category = session_.presetCategory();
    const float cw = gfx::measureText(category, 11.0f) + 12.0f;
    const Rect tag{field.x + field.w - cw - 6, 9, cw, 16};
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(tag.x, tag.y, tag.w, tag.h), 3, 3), strokePaint(kLogo, 1.0f));
    text(c, category, tag, kLogo, 11.0f);
    // Undo / redo arrows.
    const auto arrow = [&](float cx, bool redo, bool enabled) {
        auto pen = strokePaint(enabled ? kText : 0xFF8A9AC0, 1.8f);
        c.drawArc(SkRect::MakeXYWH(cx - 7, 10, 14, 14), redo ? 200.0f : -20.0f, redo ? -260.0f : 260.0f, false, pen);
        const float tx = redo ? cx + 6 : cx - 6;
        c.drawLine(tx, 11, tx, 16, pen);
        c.drawLine(tx, 16, tx + (redo ? -5.0f : 5.0f), 16, pen);
    };
    circleButton(c, {650, 5, 24, 24}, false, kField);
    arrow(662, false, session_.canUndo());
    circleButton(c, {678, 5, 24, 24}, false, kField);
    arrow(690, true, session_.canRedo());
    // A/B.
    rounded(c, {710, 5, 52, 24}, 12.0f, kField);
    const int slot = session_.activeSlot();
    c.drawCircle(slot == 0 ? 723.0f : 749.0f, 17, 11, fillPaint(kDarkButton));
    text(c, "a", {712, 5, 24, 24}, slot == 0 ? 0xFFFFFFFF : kText, 14.0f);
    text(c, "b", {738, 5, 24, 24}, slot == 1 ? 0xFFFFFFFF : kText, 14.0f);
    rounded(c, {766, 5, 48, 24}, 12.0f, kField);
    text(c, slot == 0 ? "a" : "b", {768, 5, 14, 24}, kText, 13.0f);
    text(c, slot == 0 ? "b" : "a", {798, 5, 14, 24}, kText, 13.0f);
    {
        auto pen = strokePaint(kText, 1.3f);
        c.drawLine(784, 17, 795, 17, pen);
        c.drawLine(792, 14, 795, 17, pen);
        c.drawLine(792, 20, 795, 17, pen);
    }
    circleButton(c, {826, 5, 24, 24}, false, kField);
    text(c, "?", {826, 5, 24, 24}, kText, 15.0f, ui::Align::Centre, true);
    // Settings cog.
    {
        const float cx = 864, cy = 17;
        SkPathBuilder cog;
        for (int i = 0; i < 16; ++i)
        {
            const float a = (float) i * juce::MathConstants<float>::pi / 8.0f, rr = i % 2 == 0 ? 9.0f : 6.8f;
            if (i == 0) cog.moveTo(cx + rr * std::cos(a), cy + rr * std::sin(a));
            else cog.lineTo(cx + rr * std::cos(a), cy + rr * std::sin(a));
        }
        cog.close();
        c.drawPath(cog.detach(), fillPaint(kText));
        c.drawCircle(cx, cy, 3.0f, fillPaint(kToolbar));
    }
    // Resize arrows.
    {
        auto pen = strokePaint(kText, 1.6f);
        c.drawLine(880, 9, 893, 25, pen);
        c.drawLine(880, 9, 886, 9, pen);
        c.drawLine(880, 9, 880, 15, pen);
        c.drawLine(893, 25, 887, 25, pen);
        c.drawLine(893, 25, 893, 19, pen);
    }
}

void HushFace::paintGraph(SkCanvas& c)
{
    const Rect& g = graph_;
    const float z = zeroY(), bottom = g.y + g.h;
    const float range = std::max(1.0f, session_.graphRange);
    const float perDb = (bottom - 14.0f - z) / range;
    c.save();
    c.clipRect(SkRect::MakeXYWH(g.x, g.y, g.w, g.h));
    {
        SkPaint top;
        const SkPoint ends[2] = {{0, g.y}, {0, z}};
        const SkColor colours[2] = {kGraphTop, kGraphMid};
        top.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
        c.drawRect(SkRect::MakeLTRB(g.x, g.y, g.x + g.w, z), top);
        c.drawRect(SkRect::MakeLTRB(g.x, z, g.x + g.w, bottom), fillPaint(kCutLight));
    }
    // Reduction: the blue area rises from the bottom to (0 dB - reduction); unlinked channels draw two layers.
    const auto& hush = session_.hush();
    bool split = false;
    for (int p = 0; p < H::kDisplayPoints && ! split; p += 4)
        split = std::abs(hush.displayReduction(0, p) - hush.displayReduction(1, p)) > 0.05f;
    const auto reductionPath = [&](int channel) {
        SkPathBuilder path;
        path.moveTo(g.x, bottom);
        for (int p = 0; p < H::kDisplayPoints; ++p)
        {
            const float x = freqToX(H::displayFrequency(p));
            const float r = channel < 0 ? std::max(hush.displayReduction(0, p), hush.displayReduction(1, p)) : hush.displayReduction(channel, p);
            path.lineTo(x, std::min(bottom, z + std::min(r, range * 1.2f) * perDb));
        }
        path.lineTo(g.x + g.w, bottom);
        path.close();
        return path.detach();
    };
    {
        SkPaint area;
        const SkPoint ends[2] = {{0, z}, {0, bottom}};
        const SkColor colours[2] = {kReduction, kReductionLow};
        area.setAntiAlias(true);
        area.setShader(SkGradientShader::MakeLinear(ends, colours, nullptr, 2, SkTileMode::kClamp));
        if (! split) c.drawPath(reductionPath(-1), area);
        else
        {
            c.drawPath(reductionPath(1), fillPaint(0xFF9DB1DD));     // right/side: lighter
            c.drawPath(reductionPath(0), fillPaint(0xFF58679F));     // left/mid: darker
            SkPaint both = area;
            SkPathBuilder common;
            common.moveTo(g.x, bottom);
            for (int p = 0; p < H::kDisplayPoints; ++p)
                common.lineTo(freqToX(H::displayFrequency(p)),
                              std::min(bottom, z + std::min(std::max(hush.displayReduction(0, p), hush.displayReduction(1, p)), range * 1.2f) * perDb));
            common.lineTo(g.x + g.w, bottom);
            common.close();
            c.drawPath(common.detach(), both);
        }
    }
    // PLG-22: what the detector hears (above the 0 dB line): the sidechain in orange while it drives the reduction, the
    // input in light grey otherwise; the label says which, or why the sidechain is not heard.
    {
        const bool sidechain = hush.detectingSidechain();
        float loudest = -200.0f;
        SkPathBuilder spectrum;
        for (int p = 0; p < H::kDisplayPoints; ++p)
        {
            const float db = hush.displayDetector(p);
            loudest = std::max(loudest, db);
            const float y = z - std::clamp((db + 90.0f) / 90.0f, 0.0f, 1.0f) * (z - g.y - 24.0f);
            if (p == 0) spectrum.moveTo(freqToX(H::displayFrequency(p)), y);
            else spectrum.lineTo(freqToX(H::displayFrequency(p)), y);
        }
        c.drawPath(spectrum.detach(), strokePaint(sidechain ? 0xFFFF8A3D : 0x99FFFFFF, sidechain ? 1.8f : 1.2f));
        std::string state;
        SkColor colour = kText;
        if (value(H::Sidechain) >= 0.5f)
        {
            if (! hush.sidechainConnected()) { state = "sidechain: no track routed (choose one: device menu > Sidechain)"; colour = 0xFFB0413E; }
            else if (loudest < -85.0f) { state = "sidechain: the routed track is silent"; colour = 0xFFB0413E; }
            else { state = "detecting on the SIDECHAIN"; colour = 0xFFD9661F; }
        }
        else state = hush.sidechainConnected() ? "detecting on the input (sc off)" : "detecting on the input";
        text(c, state, {g.x + 10, g.y + 22, g.w - 60, 16}, colour, 12.0f, ui::Align::Left);
    }
    // Max cut limit.
    const float maxCut = value(H::MaxCut);
    if (maxCut < H::kMaxCutOff && maxCut < range)
        c.drawRect(SkRect::MakeLTRB(g.x, z + maxCut * perDb, g.x + g.w, bottom), fillPaint(kMaxCut));
    // Grid and labels.
    const float labels[8] = {100, 250, 500, 1000, 2000, 4000, 8000, 16000};
    const char* names[8] = {"100", "250", "500", "1k", "2k", "4k", "8k", "16k"};
    for (int i = 0; i < 8; ++i)
    {
        const float x = freqToX(labels[i]);
        c.drawLine(x, g.y + 22, x, bottom, strokePaint(kGrid, 1.0f));
        text(c, names[i], {x - 20, g.y + 3, 40, 16}, kText, 12.0f);
    }
    const float step = range <= 6 ? 1.0f : range <= 12 ? 3.0f : range <= 18 ? 6.0f : 12.0f;
    for (float db = step; db < range + 0.01f; db += step)
    {
        const float y = z + db * perDb;
        c.drawLine(g.x, y, g.x + g.w, y, strokePaint(kGrid, 1.0f));
        if (y < bottom - 30) text(c, "-" + std::to_string((int) db), {g.x + g.w - 34, y - 9, 30, 18}, kText, 12.0f, ui::Align::Right);
    }
    // Depth curve(s): white for left/mid, dark for right/side when the band focus splits them.
    const auto bands = hush.bands();
    bool curveSplit = std::abs(value(H::Focus) - 0.5f) > 0.001f;
    for (const auto& b : bands) curveSplit |= b.used && b.enabled && std::abs(b.focus - 0.5f) > 0.001f;
    for (int channel = curveSplit ? 1 : 0; channel >= 0; --channel)
    {
        SkPathBuilder curve;
        constexpr int steps = 300;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = g.x + g.w * (float) i / steps;
            const auto point = devices::hush::evaluate(bands, xToFreq(x), channel);
            const float focus = std::max(1.0e-3f, devices::hush::focusAmount(value(H::Focus), channel));
            const float y = std::clamp(z - (point.drawn() + 20.0f * std::log10(focus)) * kCurvePx, g.y - 10, bottom + 10);
            if (i == 0) curve.moveTo(x, y);
            else curve.lineTo(x, y);
        }
        c.drawPath(curve.detach(), strokePaint(channel == 0 ? kCurve : kCurveRight, 2.0f));
    }
    // Band nodes: filled when enabled, hollow when bypassed; cuts are upright pills.
    const int listening = (int) std::lround(value(H::BandListen)) - 1;
    for (int b = 0; b < H::kBands; ++b)
    {
        if (! bands[(size_t) b].used) continue;
        const float x = freqToX(bands[(size_t) b].frequency), y = nodeY(b);
        const SkColor colour = kBandColours[b];
        if (b == selected_ || b == hoverBand_) c.drawCircle(x, y, 16, fillPaint(0x40FFFFFF));
        const bool cut = bands[(size_t) b].shape == devices::hush::Shape::LowCut || bands[(size_t) b].shape == devices::hush::Shape::HighCut;
        if (cut)
        {
            const SkRect pill = SkRect::MakeXYWH(x - 6, y - 12, 12, 24);
            c.drawRRect(SkRRect::MakeRectXY(pill, 6, 6), fillPaint(0xFFFFFFFF));
            const SkRect inner = SkRect::MakeXYWH(x - 3.5f, y - 9.5f, 7, 19);
            c.drawRRect(SkRRect::MakeRectXY(inner, 3.5f, 3.5f), bands[(size_t) b].enabled ? fillPaint(colour) : strokePaint(colour, 1.5f));
        }
        else
        {
            c.drawCircle(x, y, 9.5f, fillPaint(0xFFFFFFFF));
            if (bands[(size_t) b].enabled) c.drawCircle(x, y, 7, fillPaint(colour));
            else c.drawCircle(x, y, 6, strokePaint(colour, 1.5f));
        }
        if (b == listening) headphones(c, x, y - 22, 5.0f, kText);
    }
    // Range selector.
    const Rect rangeBox{g.x + g.w - 42, bottom - 28, 36, 20};
    rounded(c, rangeBox, 4.0f, kBox);
    text(c, "-" + std::to_string((int) range), {rangeBox.x + 2, rangeBox.y, 20, 20}, kText, 12.0f);
    triangleDown(c, rangeBox.x + 28, rangeBox.y + 10, 4.0f, kText);
    if (selected_ >= 0 && bands[(size_t) selected_].used) paintBandPanel(c);
    c.restore();
}

void HushFace::paintBandPanel(SkCanvas& c)
{
    const Rect& p = bandPanel_;
    const int b = selected_;
    const SkColor colour = kBandColours[b];
    c.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(p.x, p.y + 3, p.w, p.h), 10, 10), fillPaint(0x30000000));
    rounded(c, p, 10.0f, 0xF0A9BEDF);
    rounded(c, {p.x + 2, p.y + 6, 5, p.h - 12}, 2.5f, colour);
    rounded(c, {p.x + p.w - 7, p.y + 6, 5, p.h - 12}, 2.5f, colour);
    c.drawCircle(p.x + 21, p.y + 12, 9, fillPaint(kLogo));
    text(c, std::to_string(b + 1), {p.x + 12, p.y + 3, 18, 18}, 0xFFFFFFFF, 12.0f, ui::Align::Centre, true);
    const bool enabled = value(H::bandParam(b, H::BandEnabled)) >= 0.5f;
    circleButton(c, {p.x + 12, p.y + 23, 18, 18}, enabled);
    powerIcon(c, p.x + 21, p.y + 32, 4.5f, enabled ? 0xFFFFFFFF : kText);
    const bool listening = std::lround(value(H::BandListen)) == b + 1;
    circleButton(c, {p.x + 12, p.y + 43, 18, 18}, listening);
    headphones(c, p.x + 21, p.y + 52, 4.5f, listening ? 0xFFFFFFFF : kText);
    text(c, "shape", {p.x + 40, p.y + 1, 106, 16}, kLabel, 13.0f);
    const int shape = (int) std::lround(value(H::bandParam(b, H::BandShape)));
    static constexpr int kShapeOrder[8] = {0, 1, 6, 7, 2, 3, 4, 5};
    for (int s = 0; s < 8; ++s)
    {
        const Rect r{p.x + 40 + (float) (s % 4) * 27, p.y + 19 + (float) (s / 4) * 22, 25, 20};
        if (kShapeOrder[s] == shape) rounded(c, r, 4.0f, kDarkButton);
        shapeIcon(c, kShapeOrder[s], r, kShapeOrder[s] == shape ? 0xFFFFFFFF : kText);
    }
    const auto sh = (devices::hush::Shape) shape;
    const char* labels[3] = {"freq", "depth", devices::hush::usesSlope(sh) ? "slope" : "q"};
    const int params[3] = {H::bandParam(b, H::BandFrequency), H::bandParam(b, H::BandDepth),
                           devices::hush::usesSlope(sh) ? H::bandParam(b, H::BandSlope) : H::bandParam(b, H::BandQ)};
    for (int i = 0; i < 3; ++i)
    {
        const float cx = p.x + 190 + (float) i * 64;
        text(c, labels[i], {cx - 30, p.y + 1, 60, 16}, kLabel, 13.0f);
        paintKnob(c, cx, p.y + 31, 13, params[i]);
        paintBox(c, {cx - 27, p.y + 45, 54, 17}, params[i], 11.5f);
    }
    text(c, "focus", {p.x + 360, p.y + 1, 78, 16}, kLabel, 13.0f);
    paintFocus(c, {p.x + 360, p.y + 26, 78, 9}, H::bandParam(b, H::BandFocus));
    paintBox(c, {p.x + 360, p.y + 45, 78, 17}, H::bandParam(b, H::BandFocus), 11.5f);
    // Delete.
    const float tx = p.x + p.w + 3, ty = p.y + p.h - 9;
    c.drawCircle(tx, ty, 11, fillPaint(kDarkButton));
    auto pen = strokePaint(0xFFFFFFFF, 1.4f);
    c.drawLine(tx - 5, ty - 4, tx + 5, ty - 4, pen);
    c.drawRect(SkRect::MakeXYWH(tx - 3.5f, ty - 3, 7, 8), pen);
    c.drawLine(tx - 1.5f, ty - 6, tx + 1.5f, ty - 6, pen);
}

void HushFace::paintFooter(SkCanvas& c)
{
    c.drawRect(SkRect::MakeXYWH(footer_.x, footer_.y, footer_.w, footer_.h), fillPaint(kFooter));
    const float y = footer_.y + 6;
    const auto radio = [&](const Rect& r, const char* label, bool on) {
        rounded(c, r, 12.0f, kField);
        c.drawCircle(r.x + 13, r.y + 12, 6.5f, on ? fillPaint(kDarkButton) : strokePaint(kText, 1.4f));
        text(c, label, {r.x + 24, r.y, r.w - 25, r.h}, kText, 13.5f, ui::Align::Left);
    };
    radio({181, y, 78, 24}, "bypass", value(H::Bypass) >= 0.5f);
    radio({264, y, 64, 24}, "delta", value(H::Delta) >= 0.5f);
    text(c, "mix", {338, y, 30, 24}, kLabel, 14.0f, ui::Align::Right);
    paintBox(c, {372, y, 70, 24}, H::Mix, 14.0f);
    text(c, "out", {446, y, 28, 24}, kLabel, 14.0f, ui::Align::Right);
    paintBox(c, {478, y, 74, 24}, H::OutTrim, 14.0f);
    lockIcon(c, 607, y + 13, session_.qualityLocked, kText);
    text(c, "quality", {618, y, 50, 24}, kLabel, 14.0f, ui::Align::Left);
    rounded(c, {672, y, 96, 24}, 4.0f, kBox);
    const int q = (int) std::lround(value(H::Quality));
    const char* qualities[3] = {"normal", "high", "ultra"};
    text(c, value(H::LinearPhase) >= 0.5f ? std::string(qualities[q]) + " lin" : qualities[q], {672, y, 80, 24}, kText, 13.5f);
    triangleDown(c, 758, y + 12, 4.0f, kText);
    lockIcon(c, 784, y + 13, session_.lowLatencyLocked, kText);
    radio({794, y, 100, 24}, "low latency", value(H::LowLatency) >= 0.5f);
}

// ================================================================== window (built-in device)
std::optional<HushWindowView::Site> HushWindowView::find(const UiContext& x, const UiContext::WavetableKey& key)
{
    for (int index = 0; index < x.numControlTracks(); ++index)
    {
        const auto* track = x.trackAt(index);
        if (track == nullptr || track->id != key.trackId) continue;
        for (size_t d = 0; d < track->devices.size(); ++d)
        {
            const auto& top = track->devices[d];
            if (top.id != key.deviceId) continue;
            Site site;
            site.track = index;
            devices::DeviceProcessor* processor = nullptr;
            if (x.graph != nullptr && (size_t) index < x.graph->nodes.size() && d < x.graph->nodes[(size_t) index].processorForModelDevice.size())
                processor = x.graph->nodes[(size_t) index].processorForModelDevice[d];
            if (key.chain < 0) site.model = &top;
            else
            {
                if ((size_t) key.chain >= top.chains.size()) return std::nullopt;
                const auto& chain = top.chains[(size_t) key.chain].devices;
                size_t child = chain.size();
                for (size_t c = 0; c < chain.size(); ++c)
                    if (chain[c].id == key.childId) { child = c; site.model = &chain[c]; }
                if (site.model == nullptr) return std::nullopt;
                auto* rack = dynamic_cast<devices::RackProcessor*>(processor);
                processor = nullptr;
                if (rack && (size_t) key.chain < rack->chains().size())
                {
                    const auto& processors = rack->chains()[(size_t) key.chain].processorForModelDevice;
                    if (child < processors.size()) processor = processors[child];
                }
            }
            if (site.model->type != "SonoraHush") return std::nullopt;
            site.processor = dynamic_cast<devices::HushDevice*>(processor);
            return site;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

HushWindowView::HushWindowView(UiContext& context, UiContext::WavetableKey key) : RootWidget(context), key_(key)
{
}

void HushWindowView::setKey(const UiContext::WavetableKey& key)
{
    if (key == key_) return;
    key_ = key;
    built_ = false;
}

bool HushWindowView::stale() const
{
    const auto& x = context();
    return ! built_ || builtSong_ != x.song || builtGraph_ != x.graph || builtStructure_ != x.trackStructure();
}

void HushWindowView::sync()
{
    if (stale()) rebuild();
    layout();
    markDirty();
}

void HushWindowView::rebuild()
{
    auto& x = context();
    removeAllChildren();
    face_ = nullptr;
    built_ = true;
    builtSong_ = x.song;
    builtGraph_ = x.graph;
    builtStructure_ = x.trackStructure();
    if (x.song == nullptr) return; // a device edit is in progress: the song comes back with the new graph
    const auto site = find(x, key_);
    if (! site)
    {
        if (! goneSent_ && onGone) { goneSent_ = true; onGone(); } // deleted, like a plug-in window whose device went away
        return;
    }
    std::string title = site->model->displayName.empty() ? "Sonora Hush" : site->model->displayName;
    if (const auto* track = x.trackAt(site->track); track && ! track->name.empty()) title += "/" + track->name;
    if (title != title_)
    {
        title_ = title;
        if (onTitleChanged) onTitleChanged(title_);
    }
    if (site->processor == nullptr) return;
    face_ = add<HushFace>(site->processor->session());
    face_->menuHost = menuHost;
    face_->onScaleChosen = onScaleChosen;
    const int track = site->track;
    const int device = [&] {
        const auto* t = x.trackAt(track);
        for (size_t d = 0; t != nullptr && d < t->devices.size(); ++d)
            if (t->devices[d].id == key_.deviceId) return (int) d;
        return -1;
    }();
    if (key_.chain < 0 && device >= 0)
    {
        face_->mappingTarget = [this, track, device](int parameter) -> std::optional<midi::ControlTarget> {
            if (face_ == nullptr) return std::nullopt;
            return midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, track, device, 0,
                                       face_->session().hush().parameters()[(size_t) parameter].id};
        };
        // PLG-14i: no source routed yet -> the Sidechain switch opens the track chooser (as in the device chain).
        face_->onSidechainToggle = [this, track, device] {
            auto& c = context();
            const auto* t = c.trackAt(track);
            if (t == nullptr || device >= (int) t->devices.size() || t->devices[(size_t) device].sidechain.routed()) return false;
            showSidechainMenu(c, track, device);
            return true;
        };
    }
}

void HushWindowView::layout()
{
    if (stale()) rebuild();
    // The face scales with its width, so it must never be taller than the window: fit both sides and centre it (a window
    // whose content came out shorter than the aspect ratio, e.g. clamped to the screen, cut the bottom buttons off).
    if (face_ == nullptr) return;
    const auto r = localBounds();
    const float s = std::max(0.01f, std::min(r.w / HushFace::kWidth, r.h / HushFace::kHeight));
    const float w = HushFace::kWidth * s, h = HushFace::kHeight * s;
    face_->setBounds({std::floor((r.w - w) * 0.5f), std::floor((r.h - h) * 0.5f), w, h});
}

void HushWindowView::paint(SkCanvas& c, const Theme& t)
{
    const auto r = localBounds();
    paint::fill(c, r, t.color("SurfaceBackground"));
    if (face_ == nullptr && context().song != nullptr)
        paint::text(c, "Device unavailable", {8, 8, r.w - 16, 20}, t.color("TextDisabled"), t.metrics.fontSize);
}

void HushWindowView::tick()
{
    // Children tick after this: never let the face tick against a graph that was replaced.
    if (stale())
    {
        layout();
        markDirty();
    }
    RootWidget::tick();
}
} // namespace sonora::ui
