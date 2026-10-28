#include "ui/Locators.h"
#include "ui/TrackMenu.h"
#include "ui/UiContext.h"
#include "engine/ClipLauncher.h"
#include "engine/Transport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace sonora::ui
{
namespace
{
bool editable(const UiContext& ctx) { return ctx.song && ctx.applyEdit && ! ctx.recording && ! ctx.recordingRequested; }

double quantum(const UiContext& ctx)
{
    const auto& song = *ctx.song;
    return engine::quantisationBeats(song.globalQuantisation, song.timeSigNumerator, song.timeSigDenominator);
}

void repaint(UiContext& ctx) { if (ctx.requestRepaint) ctx.requestRepaint(); }
} // namespace

bool applyLocatorOp(UiContext& ctx, const model::Op& op)
{
    if (! editable(ctx)) return false;
    try
    {
        auto command = model::opToCommand(*ctx.song, op);
        if (! command.apply) return false;
        ctx.applyEdit(std::move(command));
        return true;
    }
    catch (const model::OpError& error)
    {
        ctx.statusText = error.what();
        repaint(ctx);
        return false;
    }
}

double arrangementEndBeat(const model::Song& song)
{
    double end = song.loopOn ? song.loopStart + song.loopLength : 0.0;
    for (const auto& track : song.tracks)
        for (const auto& clip : track.arrangementClips) end = std::max(end, clip.currentEnd);
    return end;
}

double locatorJumpTarget(const model::Song& song, double from, int direction)
{
    constexpr double epsilon = 1e-6;
    if (direction < 0)
    {
        for (auto it = song.locators.rbegin(); it != song.locators.rend(); ++it)
            if (it->time < from - epsilon) return it->time;
        return 0.0;
    }
    for (const auto& locator : song.locators)
        if (locator.time > from + epsilon) return locator.time;
    return std::max(from, arrangementEndBeat(song));
}

double locatorSetBeat(const UiContext& ctx)
{
    const auto* transport = ctx.transport;
    if (transport && transport->isPlaying())
    {
        const double position = transport->positionBeats(), q = ctx.song ? quantum(ctx) : 0.0;
        return q > 0.0 ? std::max(0.0, std::round(position / q) * q) : position;
    }
    if (ctx.arrangementSelection.active && ctx.arrangementSelection.end > ctx.arrangementSelection.start)
        return ctx.arrangementSelection.start;
    if (ctx.arrangementCursor >= 0) return ctx.arrangementCursor;
    return transport ? transport->positionBeats() : 0.0;
}

void setOrDeleteLocator(UiContext& ctx)
{
    if (! editable(ctx)) return;
    if (model::findLocator(*ctx.song, ctx.selectedLocator))
    {
        if (applyLocatorOp(ctx, {model::ops::DeleteLocator{ctx.selectedLocator}})) ctx.selectedLocator = -1;
        return;
    }
    const double beat = locatorSetBeat(ctx);
    for (const auto& locator : ctx.song->locators)
        if (std::abs(locator.time - beat) < 1e-9) { ctx.selectedLocator = locator.id; repaint(ctx); return; }
    const int id = model::nextLocatorId(*ctx.song);
    if (applyLocatorOp(ctx, {model::ops::AddLocator{beat, std::nullopt, -1}})) ctx.selectedLocator = id;
}

void jumpTo(UiContext& ctx, double beat)
{
    auto* transport = ctx.transport;
    if (! transport) return;
    beat = std::max(0.0, beat);
    const double q = ctx.song ? quantum(ctx) : 0.0;
    if (transport->isPlaying() && q > 0.0 && ! transport->requestHook)
    {
        const double position = transport->positionBeats();
        transport->scheduleJump((std::floor(position / q + 1e-9) + 1.0) * q, beat);
        return;
    }
    ctx.arrangementCursor = beat;
    transport->setPosition(beat);
}

void jumpToAdjacentLocator(UiContext& ctx, int direction)
{
    if (! ctx.song || ! ctx.transport) return;
    const double pending = ctx.transport->scheduledJumpTarget();
    const double from = pending >= 0.0 ? pending : ctx.transport->isPlaying() || ctx.arrangementCursor < 0 ? ctx.transport->positionBeats()
                                                                                                           : ctx.arrangementCursor;
    jumpTo(ctx, locatorJumpTarget(*ctx.song, from, direction));
    repaint(ctx);
}

void startPlayback(UiContext& ctx)
{
    if (! ctx.transport) return;
    if (ctx.song && ! ctx.transport->isPlaying() && ! ctx.transport->isCountingIn())
        for (const auto& locator : ctx.song->locators)
            if (locator.isSongStart) { ctx.transport->setPosition(locator.time); break; }
    ctx.transport->play();
}

std::vector<MenuAction> locatorContextActions(UiContext& ctx, int id, std::function<void()> rename, std::function<void()> editInfo)
{
    const auto* song = ctx.song;
    const auto* locator = song ? model::findLocator(*song, id) : nullptr;
    const bool canEdit = locator && editable(ctx);
    auto valid = [&ctx, song, id] { return ctx.song == song && song && model::findLocator(*song, id) && editable(ctx); };
    double next = -1;
    if (locator)
        for (const auto& other : song->locators)
            if (other.time > locator->time + 1e-9) { next = other.time; break; }
    std::vector<MenuAction> actions;
    actions.push_back({"Loop to Next Locator", [&ctx, valid, id, next] {
        if (! valid()) return;
        const double start = model::findLocator(*ctx.song, id)->time;
        ctx.applyEdit(model::setLoop(*ctx.song, true, start, next - start));
        if (ctx.transport) ctx.transport->setLoop(true, start, next - start);
        repaint(ctx);
    }, canEdit && next > locator->time});
    actions.push_back({"Set Song Start Time Here", [&ctx, valid, id] {
        if (! valid()) return;
        model::ops::UpdateLocator op;
        op.locatorId = id;
        op.songStart = ! model::findLocator(*ctx.song, id)->isSongStart;
        applyLocatorOp(ctx, {op});
    }, canEdit, {}, locator && locator->isSongStart});
    actions.push_back({"", {}, false});
    actions.push_back({"Rename", [valid, rename] { if (valid() && rename) rename(); }, canEdit && static_cast<bool>(rename), "\xe2\x8c\x98R"});
    actions.push_back({"Edit Info Text", [valid, editInfo] { if (valid() && editInfo) editInfo(); }, canEdit && static_cast<bool>(editInfo)});
    actions.push_back({"", {}, false});
    actions.push_back({"Delete", [&ctx, valid, id] {
        if (valid() && applyLocatorOp(ctx, {model::ops::DeleteLocator{id}}) && ctx.selectedLocator == id) ctx.selectedLocator = -1;
    }, canEdit, "\xe2\x8c\xab"});
    return actions;
}

void showLocatorContextMenu(UiContext& ctx, int id, std::function<void()> rename, std::function<void()> editInfo)
{
    const auto* song = ctx.song;
    const auto* locator = song ? model::findLocator(*song, id) : nullptr;
    if (! locator) return;
    juce::PopupMenu menu;
    for (auto& action : locatorContextActions(ctx, id, std::move(rename), std::move(editInfo)))
    {
        if (action.label.empty()) { menu.addSeparator(); continue; }
        juce::PopupMenu::Item item(juce::String::fromUTF8(action.label.c_str()));
        item.isTicked = action.ticked;
        item.isEnabled = action.enabled;
        item.shortcutKeyDescription = juce::String::fromUTF8(action.shortcut.c_str());
        item.action = action.run;
        menu.addItem(std::move(item));
    }
    menu.addSeparator();
    // Sonora extension: the reference draws every locator in one colour; here each one can take a palette colour.
    juce::PopupMenu::Item none("Default Locator Color");
    none.isTicked = locator->colorIndex < 0;
    none.isEnabled = editable(ctx);
    none.action = [&ctx, song, id] {
        if (ctx.song != song || ! model::findLocator(*song, id)) return;
        model::ops::UpdateLocator op; op.locatorId = id; op.color = -1;
        applyLocatorOp(ctx, {op});
    };
    menu.addItem(std::move(none));
    addColorGrid(menu, locator->colorIndex, ctx.uiZoom, [&ctx, song, id](int colour) {
        if (ctx.song != song || ! model::findLocator(*song, id)) return;
        model::ops::UpdateLocator op; op.locatorId = id; op.color = colour;
        applyLocatorOp(ctx, {op});
    }, "Locator Colors");
    menu.showMenuAsync(juce::PopupMenu::Options());
}

std::vector<MenuAction> scrubAreaContextActions(UiContext& ctx, double beat)
{
    const auto* song = ctx.song;
    const bool taken = song && std::any_of(song->locators.begin(), song->locators.end(),
                                           [beat](const model::Locator& l) { return std::abs(l.time - beat) < 1e-9; });
    return {
        {"Add Locator", [&ctx, song, beat] {
            if (ctx.song != song || ! song) return;
            const int id = model::nextLocatorId(*song);
            if (applyLocatorOp(ctx, {model::ops::AddLocator{beat, std::nullopt, -1}})) ctx.selectedLocator = id;
        }, editable(ctx) && ! taken},
        // TODO(ENG-11b): time signature changes on the timeline.
        {"Insert Time Signature Change", {}, false},
    };
}

void showScrubAreaContextMenu(UiContext& ctx, double beat)
{
    juce::PopupMenu menu;
    for (auto& action : scrubAreaContextActions(ctx, beat))
    {
        juce::PopupMenu::Item item(juce::String::fromUTF8(action.label.c_str()));
        item.isEnabled = action.enabled;
        item.action = action.run;
        menu.addItem(std::move(item));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

} // namespace sonora::ui
