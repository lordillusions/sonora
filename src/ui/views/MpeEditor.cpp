#include "ui/views/MpeEditor.h"
#include "ui/UiContext.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "include/core/SkPaint.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sonora::ui
{
namespace
{
constexpr int controllers[] = {-2, 74, -1};
constexpr const char* labels[] = {"Pitch", "Slide", "Pressure", "Release Velocity"};
bool sameMidi(const model::Clip& a, const model::Clip& b)
{
    if (a.notes.size() != b.notes.size() || a.noteExpressions.size() != b.noteExpressions.size()) return false;
    for (size_t i = 0; i < a.notes.size(); ++i)
    {
        const auto& x = a.notes[i]; const auto& y = b.notes[i];
        if (x.noteId != y.noteId || x.pitch != y.pitch || x.enabled != y.enabled || std::abs(x.start - y.start) > 0.0
            || std::abs(x.duration - y.duration) > 0.0 || std::abs(x.velocity - y.velocity) > 0.0f
            || std::abs(x.releaseVelocity - y.releaseVelocity) > 0.0f || std::abs(x.probability - y.probability) > 0.0f
            || std::abs(x.velocityDeviation - y.velocityDeviation) > 0.0f) return false;
    }
    for (size_t i = 0; i < a.noteExpressions.size(); ++i)
    {
        const auto& x = a.noteExpressions[i]; const auto& y = b.noteExpressions[i];
        if (x.noteId != y.noteId || x.cc != y.cc || x.points.size() != y.points.size()) return false;
        for (size_t j = 0; j < x.points.size(); ++j)
        {
            const auto& p = x.points[j]; const auto& q = y.points[j];
            if (std::abs(p.time - q.time) > 0.0 || std::abs(p.value - q.value) > 0.0 || std::abs(p.x1 - q.x1) > 0.0
                || std::abs(p.y1 - q.y1) > 0.0 || std::abs(p.x2 - q.x2) > 0.0 || std::abs(p.y2 - q.y2) > 0.0) return false;
        }
    }
    return true;
}
}
Rect MpeEditor::plot() const { return {46, 50, std::max(1.0f, width() - 55), std::max(1.0f, height() - 72)}; }
Rect MpeEditor::dimensionBounds(int dimension) const { return {5 + (float) dimension * 100, 25, 96, 18}; }
void MpeEditor::sync()
{
    const auto* clip = ctx().selectedClip();
    if (! clip) { uid_ = -1; dragging_ = false; return; }
    if (uid_ != clip->uid || song_ != ctx().song) { uid_ = clip->uid; song_ = ctx().song; selected_ = 0; dragging_ = false; }
    selected_ = std::clamp(selected_, 0, std::max(0, (int) clip->notes.size() - 1));
}
const model::NoteExpression* MpeEditor::expression(const model::Clip& clip) const
{
    if (dimension_ > 2 || clip.notes.empty() || selected_ < 0 || (size_t) selected_ >= clip.notes.size()) return nullptr;
    const int id = clip.notes[(size_t) selected_].noteId;
    const auto found = std::find_if(clip.noteExpressions.begin(), clip.noteExpressions.end(), [&](const auto& lane) { return id >= 0 && lane.noteId == id && lane.cc == controllers[dimension_]; });
    return found == clip.noteExpressions.end() ? nullptr : &*found;
}
void MpeEditor::paint(SkCanvas& c, const Theme& t)
{
    sync(); paint::fill(c, localBounds(), t.color("DetailViewBackground"));
    const auto* clip = ctx().selectedClip();
    if (! clip || clip->notes.empty())
    { paint::text(c, "Add or select a MIDI note to edit its expression", localBounds(), t.color("TextDisabled"), t.metrics.fontSize, Align::Centre); return; }
    const auto& current = dragging_ ? working_ : *clip;
    const auto& note = current.notes[(size_t) selected_];
    char title[120]; std::snprintf(title, sizeof(title), "Note %d of %d   ·   MIDI %d   ·   %.3g beats", selected_ + 1, (int) current.notes.size(), note.pitch, note.duration);
    ctl::dropdown(c, t, {5, 3, std::min(400.0f, width() - 10), 18}, title);
    for (int dimension = 0; dimension < 4; ++dimension)
        ctl::toggle(c, t, dimensionBounds(dimension), labels[dimension], dimension == dimension_, t.color("SelectionBackground"));
    const auto area = plot();
    paint::fill(c, area, t.color("SurfaceBackground"));
    for (int i = 0; i <= 4; ++i)
    {
        const float y = area.y + area.h * (float) i / 4;
        paint::line(c, area.x, y, area.right(), y, t.color("GridLineBase"));
        const float x = area.x + area.w * (float) i / 4;
        paint::line(c, x, area.y, x, area.bottom(), t.color("GridLineBase"));
    }
    const double low = dimension_ == 0 ? -48 : 0, high = dimension_ == 0 ? 48 : 127;
    auto yFor = [&](double value) { return area.bottom() - (float) ((value - low) / (high - low)) * area.h; };
    paint::text(c, dimension_ == 0 ? "+48" : "127", {2, area.y - 6, 39, 14}, t.color("TextDisabled"), t.metrics.smallFontSize, Align::Right);
    paint::text(c, dimension_ == 0 ? "-48" : "0", {2, area.bottom() - 8, 39, 14}, t.color("TextDisabled"), t.metrics.smallFontSize, Align::Right);
    const auto* lane = expression(current);
    const auto ink = Theme::paletteColor(current.colorIndex);
    c.save(); c.clipRect(area.sk());
    if (! lane || lane->points.empty() || dimension_ == 3)
    {
        const double value = dimension_ == 3 ? note.releaseVelocity : dimension_ == 1 ? 64.0 : 0;
        paint::line(c, area.x, yFor(value), area.right(), yFor(value), ink, 2);
    }
    else
    {
        float previousX = area.x, previousY = yFor(lane->points.front().value);
        for (const auto& point : lane->points)
        {
            const float x = area.x + (float) (point.time / std::max(0.001, note.duration)) * area.w, y = yFor(point.value);
            paint::line(c, previousX, previousY, x, y, ink, 2);
            SkPaint dot; dot.setAntiAlias(true); dot.setColor(t.color("ControlForeground")); c.drawCircle(x, y, 3, dot);
            previousX = x; previousY = y;
        }
        paint::line(c, previousX, previousY, area.right(), previousY, ink, 2);
    }
    c.restore();
    paint::text(c, dimension_ == 0 ? "Note-relative pitch · ±48 semitones" : "Click to add, drag to shape; right-click deletes a point", {area.x, area.bottom() + 3, area.w, 16}, t.color("TextDisabled"), t.metrics.smallFontSize);
}
void MpeEditor::mouseDown(const MouseEvent& e)
{
    sync(); const auto* clip = ctx().selectedClip(); if (! clip || clip->notes.empty()) return;
    if (e.y < 23 && ! e.right)
    {
        juce::PopupMenu menu;
        for (size_t index = 0; index < clip->notes.size() && index < 4096; ++index)
        {
            const auto& note = clip->notes[index];
            menu.addItem((int) index + 1, "Note " + juce::String((int) index + 1) + " · MIDI " + juce::String(note.pitch) + " · " + juce::String(note.start, 3), true, (int) index == selected_);
        }
        const auto* source = ctx().song; const int uid = clip->uid;
        menu.showMenuAsync(juce::PopupMenu::Options(), [this, source, uid](int result) {
            if (! Widget::isAlive(this) || ctx().song != source || ! ctx().selectedClip() || ctx().selectedClip()->uid != uid || ctx().selectedClip()->notes.empty() || result <= 0) return;
            selected_ = std::clamp(result - 1, 0, (int) ctx().selectedClip()->notes.size() - 1); repaint();
        });
        return;
    }
    for (int dimension = 0; dimension < 4; ++dimension) if (dimensionBounds(dimension).contains(e.x, e.y))
    { dimension_ = dimension; repaint(); return; }
    if (! plot().contains(e.x, e.y) || ! ctx().applyEdit || ctx().recording || ctx().recordingRequested) return;
    location_ = ctx().selectedClipLocation(); original_ = working_ = *clip; changed_ = false; grabbed_ = -1;
    auto& note = working_.notes[(size_t) selected_];
    if (dimension_ < 3)
    {
        if (note.noteId < 0)
        {
            int next = 1;
            for (const auto& other : working_.notes) next = std::max(next, other.noteId + 1);
            note.noteId = next;
        }
        auto it = std::find_if(working_.noteExpressions.begin(), working_.noteExpressions.end(), [&](const auto& lane) { return lane.noteId == note.noteId && lane.cc == controllers[dimension_]; });
        if (it == working_.noteExpressions.end())
        { working_.noteExpressions.push_back({note.noteId, controllers[dimension_], {{0, dimension_ == 1 ? 64.0 : 0.0}}}); it = std::prev(working_.noteExpressions.end()); }
        const double time = (e.x - plot().x) / plot().w * note.duration;
        for (size_t i = 0; i < it->points.size(); ++i)
            if (std::abs(it->points[i].time - time) * plot().w / std::max(0.001, note.duration) < 6) { grabbed_ = (int) i; break; }
        working_.noteExpressionsEdited = true; it->edited = true;
        if (e.right || e.clicks >= 2)
        {
            if (grabbed_ >= 0) { it->points.erase(it->points.begin() + grabbed_); changed_ = true; }
            dragging_ = true; mouseUp(e); return;
        }
        if (grabbed_ < 0) { it->points.push_back({time, 0}); grabbed_ = (int) it->points.size() - 1; }
        working_.noteExpressionsEdited = true;
    }
    dragging_ = true; update(e); repaint();
}
void MpeEditor::update(const MouseEvent& e)
{
    if (! dragging_ || working_.notes.empty()) return;
    const auto area = plot(); const double normal = std::clamp((double) (area.bottom() - e.y) / area.h, 0.0, 1.0);
    auto& note = working_.notes[(size_t) selected_];
    if (dimension_ == 3) note.releaseVelocity = (float) std::round(normal * 127);
    else
    {
        auto it = std::find_if(working_.noteExpressions.begin(), working_.noteExpressions.end(), [&](const auto& lane) { return lane.noteId == note.noteId && lane.cc == controllers[dimension_]; });
        if (it == working_.noteExpressions.end() || grabbed_ < 0 || (size_t) grabbed_ >= it->points.size()) return;
        it->edited = true;
        auto& point = it->points[(size_t) grabbed_];
        point.time = std::clamp((double) (e.x - area.x) / area.w * note.duration, 0.0, note.duration);
        point.value = dimension_ == 0 ? normal * 96 - 48 : normal * 127;
        working_.noteExpressionsEdited = true;
    }
    changed_ = true; repaint();
}
void MpeEditor::mouseDrag(const MouseEvent& e) { update(e); }
void MpeEditor::mouseUp(const MouseEvent&)
{
    if (! dragging_) return;
    dragging_ = false;
    const auto* current = ctx().selectedClip();
    if (changed_ && current && current->uid == uid_ && song_ == ctx().song && sameMidi(*current, original_) && ctx().applyEdit && ! ctx().recording && ! ctx().recordingRequested)
    {
        for (auto& lane : working_.noteExpressions)
            std::stable_sort(lane.points.begin(), lane.points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
        auto updated = *current; updated.notes = working_.notes; updated.noteExpressions = working_.noteExpressions;
        updated.noteExpressionsEdited = working_.noteExpressionsEdited;
        ctx().applyEdit(model::modifyClip(*ctx().song, location_, std::move(updated), "Edit Note Expression"));
    }
    changed_ = false; repaint();
}
bool MpeEditor::keyPressed(const KeyEvent& e)
{
    if (e.keyCode != juce::KeyPress::escapeKey || ! dragging_) return false;
    focusLost(); return true;
}
void MpeEditor::focusLost() { dragging_ = changed_ = false; repaint(); }
} // namespace sonora::ui
