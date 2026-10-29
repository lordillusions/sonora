#include "ui/InlineEdit.h"

#include "core/Op.h"
#include "gfx/Fonts.h"
#include "ui/Painter.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFontMetrics.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace sonora::ui
{

namespace
{
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

size_t previousBoundary(const std::string& s, size_t i)
{
    if (i == 0) return 0;
    do --i; while (i > 0 && continuation(s[i]));
    return i;
}

size_t nextBoundary(const std::string& s, size_t i)
{
    if (i >= s.size()) return s.size();
    do ++i; while (i < s.size() && continuation(s[i]));
    return i;
}

// Names are one line: pasted line breaks and tabs become spaces.
std::string singleLine(std::string s)
{
    std::replace_if(s.begin(), s.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    return s;
}

void repaint(UiContext& ctx) { if (ctx.requestRepaint) ctx.requestRepaint(); }

int controlIndexOf(const UiContext& ctx, int trackId)
{
    if (! ctx.song) return -1;
    for (int i = 0; i < ctx.numTracks() + ctx.numReturns(); ++i)
        if (const auto* track = ctx.trackAt(i); track && track->id == trackId) return i;
    return -1;
}
} // namespace

void InlineTextEdit::start(std::string initial)
{
    text = singleLine(std::move(initial));
    caret = text.size();
    allSelected = true;
}

void InlineTextEdit::insert(const std::string& s)
{
    if (allSelected) { text.clear(); caret = 0; allSelected = false; }
    const auto clean = singleLine(s);
    text.insert(caret, clean);
    caret += clean.size();
}

InlineTextEdit::Result InlineTextEdit::key(const KeyEvent& k)
{
    using KP = juce::KeyPress;
    const auto ch = k.textChar;
    if (k.keyCode == KP::escapeKey) return Result::Cancel;
    if (k.keyCode == KP::returnKey) return Result::Commit;
    if (k.cmd && ! k.alt && (ch == 'a' || ch == 'A')) { allSelected = true; caret = text.size(); return Result::Edited; }
    if (k.cmd && ! k.alt && (ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X'))
    {
        if (allSelected && ! text.empty())
        {
            juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(text.c_str()));
            if (ch == 'x' || ch == 'X') { text.clear(); caret = 0; allSelected = false; }
        }
        return Result::Edited;
    }
    if (k.cmd && ! k.alt && (ch == 'v' || ch == 'V'))
    {
        insert(juce::SystemClipboard::getTextFromClipboard().toStdString());
        return Result::Edited;
    }
    if (k.keyCode == KP::leftKey || k.keyCode == KP::rightKey)
    {
        const bool left = k.keyCode == KP::leftKey;
        if (allSelected) caret = left ? 0 : text.size(); // like a text field: the arrow collapses the selection to that side
        else if (k.cmd) caret = left ? 0 : text.size();
        else caret = left ? previousBoundary(text, caret) : nextBoundary(text, caret);
        allSelected = false;
        return Result::Edited;
    }
    if (k.keyCode == KP::homeKey || k.keyCode == KP::upKey) { caret = 0; allSelected = false; return Result::Edited; }
    if (k.keyCode == KP::endKey || k.keyCode == KP::downKey) { caret = text.size(); allSelected = false; return Result::Edited; }
    if (k.keyCode == KP::backspaceKey || k.keyCode == KP::deleteKey)
    {
        if (allSelected) { text.clear(); caret = 0; allSelected = false; return Result::Edited; }
        if (k.keyCode == KP::backspaceKey && caret > 0)
        {
            const auto from = k.cmd ? 0 : previousBoundary(text, caret);
            text.erase(from, caret - from);
            caret = from;
        }
        else if (k.keyCode == KP::deleteKey && caret < text.size())
            text.erase(caret, nextBoundary(text, caret) - caret);
        return Result::Edited;
    }
    if (ch >= 32 && ch != 127 && ! k.cmd && ! k.ctrl)
        insert(juce::String::charToString((juce::juce_wchar) ch).toStdString());
    return Result::Edited;
}

void InlineTextEdit::paint(SkCanvas& c, const Rect& r, SkColor back, SkColor ink, SkColor selectionBack, SkColor selectionInk,
                           float fontSize, bool bold) const
{
    paint::fill(c, r, back);
    paint::stroke(c, r, ink, 1.0f);
    const Rect inner{r.x + 3, r.y + 1, r.w - 6, r.h - 2};
    if (inner.w <= 2) return;
    const auto font = gfx::uiFont(fontSize, bold);
    const auto widthOf = [&](size_t bytes) { return font.measureText(text.data(), bytes, SkTextEncoding::kUTF8); };
    const float full = widthOf(text.size());
    const float caretX = widthOf(std::min(caret, text.size()));
    // Scroll so the caret (or, with everything selected, the end of the name) stays inside the field.
    const float scroll = std::max(0.0f, (allSelected ? full : caretX) - inner.w + 2);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float baseline = std::round(inner.centreY() - (metrics.fAscent + metrics.fDescent) * 0.5f);
    c.save();
    c.clipRect(inner.sk());
    const float x = inner.x - scroll;
    if (allSelected && ! text.empty())
        paint::fill(c, {x, inner.y + 1, full, inner.h - 2}, selectionBack);
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setColor(allSelected ? selectionInk : ink);
    c.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, pen);
    if (! allSelected || text.empty())
        paint::fill(c, {std::round(x + caretX), inner.y + 1, 1, inner.h - 2}, ink);
    c.restore();
}

std::string editableTrackName(const UiContext& ctx, int index)
{
    const auto* track = ctx.trackAt(index);
    if (! track) return {};
    if (track->nameIsAutomatic && index < ctx.numTracks())
    {
        const auto displayed = ctx.trackDisplayName(index);
        return "#" + displayed.substr(std::min(displayed.size(), std::to_string(index + 1).size()));
    }
    return track->name;
}

bool beginTrackRename(UiContext& ctx, int index)
{
    const auto* track = ctx.trackAt(index);
    if (! track || track->kind == model::TrackKind::Main || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested)
        return false;
    ctx.trackRename.trackId = track->id;
    ctx.trackRename.bounds = {};
    ctx.trackRename.edit.start(editableTrackName(ctx, index));
    repaint(ctx);
    return true;
}

void cancelTrackRename(UiContext& ctx)
{
    ctx.trackRename = {};
    repaint(ctx);
}

void commitTrackRename(UiContext& ctx)
{
    const int id = ctx.trackRename.trackId;
    auto name = juce::String::fromUTF8(ctx.trackRename.edit.text.c_str()).trim().toStdString();
    ctx.trackRename = {};
    repaint(ctx);
    const int index = controlIndexOf(ctx, id);
    if (index < 0 || name.empty() || name == editableTrackName(ctx, index) || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested)
        return;
    try
    {
        auto command = model::opToCommand(*ctx.song, model::Op{model::ops::RenameTrack{id, std::move(name)}});
        if (command.apply) ctx.applyEdit(std::move(command));
    }
    catch (const model::OpError& error)
    {
        ctx.statusText = error.what();
    }
}

bool trackRenameKey(UiContext& ctx, const KeyEvent& k)
{
    // The track went away under the field (undo, a collaborator, another set): close it quietly.
    if (controlIndexOf(ctx, ctx.trackRename.trackId) < 0) { cancelTrackRename(ctx); return true; }
    switch (ctx.trackRename.edit.key(k))
    {
        case InlineTextEdit::Result::Commit: commitTrackRename(ctx); break;
        case InlineTextEdit::Result::Cancel: cancelTrackRename(ctx); break;
        case InlineTextEdit::Result::Edited: repaint(ctx); break;
    }
    return true;
}

} // namespace sonora::ui
