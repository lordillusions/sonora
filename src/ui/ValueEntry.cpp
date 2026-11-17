#include "ui/ValueEntry.h"

#include "gfx/Fonts.h"
#include "ui/Theme.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

namespace sonora::ui
{

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
