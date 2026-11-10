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
#include <cctype>
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

// Word characters for Alt+arrows and double-click: letters, digits and any non-ASCII code point.
bool wordByte(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || std::isalnum(u) != 0 || c == '_';
}

size_t wordStart(const std::string& s, size_t i)
{
    while (i > 0 && ! wordByte(s[i - 1])) --i;
    while (i > 0 && wordByte(s[i - 1])) --i;
    return i;
}

size_t wordEnd(const std::string& s, size_t i)
{
    while (i < s.size() && ! wordByte(s[i])) ++i;
    while (i < s.size() && wordByte(s[i])) ++i;
    return i;
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
    selectAll();
}

void InlineTextEdit::insert(const std::string& s)
{
    const auto from = selectionStart();
    text.erase(from, selectionEnd() - from);
    const auto clean = singleLine(s);
    text.insert(from, clean);
    caret = anchor = from + clean.size();
}

InlineTextEdit::Result InlineTextEdit::key(const KeyEvent& k)
{
    using KP = juce::KeyPress;
    const auto ch = k.textChar;
    caret = std::min(caret, text.size());
    anchor = std::min(anchor, text.size());
    if (k.keyCode == KP::escapeKey) return Result::Cancel;
    if (k.keyCode == KP::returnKey) return Result::Commit;
    if (k.cmd && ! k.alt && (ch == 'a' || ch == 'A')) { selectAll(); return Result::Edited; }
    if (k.cmd && ! k.alt && (ch == 'c' || ch == 'C' || ch == 'x' || ch == 'X'))
    {
        if (hasSelection())
        {
            const auto from = selectionStart();
            juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(text.substr(from, selectionEnd() - from).c_str()));
            if (ch == 'x' || ch == 'X') insert({});
        }
        return Result::Edited;
    }
    if (k.cmd && ! k.alt && (ch == 'v' || ch == 'V'))
    {
        insert(juce::SystemClipboard::getTextFromClipboard().toStdString());
        return Result::Edited;
    }
    const bool home = k.keyCode == KP::homeKey || k.keyCode == KP::upKey, end = k.keyCode == KP::endKey || k.keyCode == KP::downKey;
    if (k.keyCode == KP::leftKey || k.keyCode == KP::rightKey || home || end)
    {
        const bool left = k.keyCode == KP::leftKey;
        size_t to = caret;
        if (home) to = 0;
        else if (end) to = text.size();
        else if (hasSelection() && ! k.shift) to = left ? selectionStart() : selectionEnd(); // the arrow collapses the selection
        else if (k.cmd) to = left ? 0 : text.size();
        else if (k.alt) to = left ? wordStart(text, caret) : wordEnd(text, caret);
        else to = left ? previousBoundary(text, caret) : nextBoundary(text, caret);
        caret = to;
        if (! k.shift) anchor = caret;
        return Result::Edited;
    }
    if (k.keyCode == KP::backspaceKey || k.keyCode == KP::deleteKey)
    {
        if (hasSelection()) { insert({}); return Result::Edited; }
        if (k.keyCode == KP::backspaceKey && caret > 0)
        {
            const auto from = k.cmd ? 0 : k.alt ? wordStart(text, caret) : previousBoundary(text, caret);
            text.erase(from, caret - from);
            caret = from;
        }
        else if (k.keyCode == KP::deleteKey && caret < text.size())
            text.erase(caret, (k.alt ? wordEnd(text, caret) : nextBoundary(text, caret)) - caret);
        anchor = caret;
        return Result::Edited;
    }
    if (ch >= 32 && ch != 127 && ! k.cmd && ! k.ctrl)
        insert(juce::String::charToString((juce::juce_wchar) ch).toStdString());
    return Result::Edited;
}

namespace
{
Rect innerOf(const Rect& r) { return {r.x + 3, r.y + 1, r.w - 6, r.h - 2}; }

// Horizontal scroll that keeps the caret inside the field (the same for drawing and hit-testing).
float scrollFor(const SkFont& font, const std::string& text, size_t caret, float innerWidth)
{
    const float caretX = font.measureText(text.data(), std::min(caret, text.size()), SkTextEncoding::kUTF8);
    return std::max(0.0f, caretX - innerWidth + 2);
}
} // namespace

size_t InlineTextEdit::offsetAt(float x, const Rect& r, float fontSize, bool bold) const
{
    const auto inner = innerOf(r);
    const auto font = gfx::uiFont(fontSize, bold);
    const float local = x - inner.x + scrollFor(font, text, caret, inner.w);
    size_t best = 0;
    float bestDistance = std::fabs(local);
    for (size_t i = nextBoundary(text, 0); i <= text.size() && ! text.empty(); i = nextBoundary(text, i))
    {
        const float distance = std::fabs(local - font.measureText(text.data(), i, SkTextEncoding::kUTF8));
        if (distance < bestDistance) { bestDistance = distance; best = i; }
        if (i == text.size()) break;
    }
    return best;
}

void InlineTextEdit::click(size_t offset, int clicks, bool shift)
{
    offset = std::min(offset, text.size());
    if (clicks >= 3) { selectAll(); return; }
    if (clicks == 2)
    {
        // The word under the pointer (or the run of spaces/punctuation between words).
        size_t from = offset, to = offset;
        const bool word = (offset < text.size() && wordByte(text[offset])) || (offset > 0 && wordByte(text[offset - 1]));
        auto inside = [&](size_t i) { return wordByte(text[i]) == word; };
        while (from > 0 && inside(from - 1)) --from;
        while (to < text.size() && inside(to)) ++to;
        anchor = from;
        caret = to;
        return;
    }
    caret = offset;
    if (! shift) anchor = offset;
}

void InlineTextEdit::paint(SkCanvas& c, const Rect& r, SkColor back, SkColor ink, SkColor selectionBack, SkColor selectionInk,
                           float fontSize, bool bold) const
{
    paint::fill(c, r, back);
    paint::stroke(c, r, ink, 1.0f);
    paintText(c, r, ink, selectionBack, selectionInk, fontSize, bold, true);
}

void InlineTextEdit::paintText(SkCanvas& c, const Rect& r, SkColor ink, SkColor selectionBack, SkColor selectionInk, float fontSize,
                               bool bold, bool caretShown) const
{
    const Rect inner = innerOf(r);
    if (inner.w <= 2) return;
    const auto font = gfx::uiFont(fontSize, bold);
    const auto widthOf = [&](size_t bytes) { return font.measureText(text.data(), std::min(bytes, text.size()), SkTextEncoding::kUTF8); };
    const float scroll = scrollFor(font, text, caret, inner.w);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float baseline = std::round(inner.centreY() - (metrics.fAscent + metrics.fDescent) * 0.5f);
    c.save();
    c.clipRect(inner.sk());
    const float x = inner.x - scroll;
    SkPaint pen;
    pen.setAntiAlias(true);
    pen.setColor(ink);
    c.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, pen);
    if (hasSelection())
    {
        const float from = x + widthOf(selectionStart()), to = x + widthOf(selectionEnd());
        const Rect band{from, inner.y + 1, to - from, inner.h - 2};
        paint::fill(c, band, selectionBack);
        c.save();
        c.clipRect(band.sk());
        pen.setColor(selectionInk);
        c.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, x, baseline, font, pen);
        c.restore();
    }
    else if (caretShown)
        paint::fill(c, {std::round(x + widthOf(caret)), inner.y + 1, 1, inner.h - 2}, ink);
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
