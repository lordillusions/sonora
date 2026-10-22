#include "ui/TrackMenu.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"
#include "ui/MenuLookAndFeel.h"
#include "midi/MidiManager.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{
void toggleTrackSolo(UiContext& ctx, int index, bool additive)
{
    if (! ctx.controls || index < 0 || index >= ctx.numTracks() + ctx.numReturns()
        || (size_t) index >= ctx.controls->size()) return;
    auto& selected = (*ctx.controls)[(size_t) index].solo;
    const bool wasOn = selected.load();
    bool otherOn = false;
    if (! additive)
        for (size_t i = 0; i < ctx.controls->size(); ++i)
            if ((int) i != index) otherOn |= (*ctx.controls)[i].solo.exchange(false);
    selected.store(additive ? ! wasOn : ! wasOn || otherOn);
    if (ctx.requestRepaint) ctx.requestRepaint();
}

void splitArrangementSelection(UiContext& ctx)
{
    if (! ctx.song || ! ctx.applyEdit || ctx.mainView != MainView::Arrangement || ctx.clipEditorFocused
        || ctx.recording || ctx.recordingRequested) return;
    const auto& range = ctx.arrangementSelection;
    const bool selection = range.active && range.end > range.start;
    const int first = selection ? range.firstTrack : ctx.selectedTrack, last = selection ? range.lastTrack : first;
    const double position = selection ? range.start : ctx.arrangementCursor >= 0 ? ctx.arrangementCursor
                                                       : ctx.transport ? ctx.transport->positionBeats() : -1;
    auto edit = model::splitArrangementClips(*ctx.song, first, last, position, selection ? range.end : -1);
    if (edit.apply) ctx.applyEdit(std::move(edit));
}

namespace
{
void appendActions(juce::PopupMenu& menu, const std::vector<MenuAction>& actions)
{
    for (const auto& action : actions)
    {
        if (action.label.empty()) { menu.addSeparator(); continue; }
        juce::PopupMenu::Item item(juce::String(action.label));
        item.isEnabled = action.enabled;
        item.shortcutKeyDescription = juce::String(action.shortcut);
        item.action = action.run;
        menu.addItem(std::move(item));
    }
}

juce::PopupMenu colorsMenu(int selected, std::function<void(int)> pick)
{
    juce::PopupMenu menu;
    for (int index = 0; index < 70; ++index)
    {
        juce::PopupMenu::Item item("Color " + juce::String(index + 1));
        juce::Image swatch(juce::Image::ARGB, 14, 14, true);
        juce::Graphics graphics(swatch);
        graphics.fillAll(juce::Colour(Theme::paletteColor(index)));
        item.image = std::make_unique<juce::DrawableImage>(swatch);
        item.isTicked = index == selected;
        item.action = [pick, index] { pick(index); };
        if ((index + 1) % 14 == 0) item.shouldBreakAfter = true;
        menu.addItem(std::move(item));
    }
    return menu;
}

class TrackColorGrid : public juce::PopupMenu::CustomComponent
{
public:
    TrackColorGrid(int selected, float zoom, std::function<void(int)> pick)
        : CustomComponent(false), selected_(selected), zoom_(zoom), pick_(std::move(pick)) {}
    void getIdealSize(int& w, int& h) override { w = (int) std::ceil(250 * zoom_); h = (int) std::ceil(91 * zoom_); }
    void paint(juce::Graphics& g) override
    {
        for (int i = 0; i < 70; ++i)
        {
            const auto r = cell(i);
            g.setColour(juce::Colour(Theme::paletteColor(i))); g.fillRect(r);
            if (i == selected_ || i == hovered_) { g.setColour(juce::Colour(0xff313541)); g.drawRect(r.expanded(0.5f * zoom_), zoom_); }
        }
    }
    void mouseMove(const juce::MouseEvent& e) override { hovered_ = hit(e.position); repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = -1; repaint(); }
    void mouseUp(const juce::MouseEvent& e) override
    {
        const int index = hit(e.position);
        if (index >= 0) { pick_(index); triggerMenuItem(); }
    }
private:
    juce::Rectangle<float> cell(int i) const { return {(8.0f + (float) (i % 14) * 17) * zoom_, (6.0f + (float) (i / 14) * 17) * zoom_, 11 * zoom_, 11 * zoom_}; }
    int hit(juce::Point<float> p) const { for (int i = 0; i < 70; ++i) if (cell(i).contains(p)) return i; return -1; }
    int selected_ = -1, hovered_ = -1;
    float zoom_ = 1;
    std::function<void(int)> pick_;
};
} // namespace

void showControlContextMenu(Widget& owner, UiContext& ctx, std::function<void()> reset)
{
    juce::PopupMenu menu;
    auto* widget = &owner;
    const auto* song = ctx.song;
    if (reset)
    {
        juce::PopupMenu::Item item("Return to Default");
        item.shortcutKeyDescription = juce::String::fromUTF8("⌫");
        item.action = [widget, &ctx, song, reset] { if (Widget::isAlive(widget) && ctx.song == song) reset(); };
        menu.addItem(std::move(item));
        menu.addSeparator();
    }
    for (const auto* label : {"Show Automation", "Show Automation in New Lane", "Delete Automation"})
        menu.addItem(juce::PopupMenu::Item(label).setEnabled(false));
    if (owner.midiTarget)
    {
        const auto target = *owner.midiTarget;
        juce::PopupMenu::Item map("Edit MIDI Map");
        map.shortcutKeyDescription = juce::String::fromUTF8("⌘M");
        map.isEnabled = ctx.midi != nullptr;
        map.action = [widget, &ctx, song, target] {
            if (! Widget::isAlive(widget) || ctx.song != song || ! ctx.midi) return;
            ctx.midiMapMode = true; ctx.keyMapMode = false; ctx.mapSelection = target; ctx.midi->beginLearn(target);
            if (ctx.requestRepaint) ctx.requestRepaint();
        };
        menu.addSeparator(); menu.addItem(std::move(map));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

std::vector<MenuAction> trackContextActions(UiContext& ctx)
{
    const auto* track = ctx.trackAt(ctx.selectedTrack);
    const bool editable = ctx.song && ! ctx.recording && ! ctx.recordingRequested;
    const bool normal = track && track->kind != model::TrackKind::Main;
    const auto* song = ctx.song;
    const auto id = track ? track->id : -1;
    const int index = ctx.selectedTrack;
    auto valid = [&ctx, song, id, index] {
        const auto* current = ctx.trackAt(index);
        return ctx.song == song && current && current->id == id;
    };
    std::vector<int> members;
    const bool group = editable && ctx.groupSelectedTracks && model::groupableSelection(*ctx.song, ctx.selectedTrackIndices(), members);
    const bool duplicate = editable && ctx.duplicateSelectedTracks && model::groupableSelection(*ctx.song, ctx.selectedTrackIndices(), members);
    std::vector<model::TrackId> deleting;
    for (int selected : ctx.selectedChannelIndices()) if (const auto* t = ctx.trackAt(selected)) deleting.push_back(t->id);
    std::vector<MenuAction> result{
        {"Cut", {}, false, "⌘X"}, {"Copy", {}, false, "⌘C"},
        {"Duplicate", [&ctx, valid] { if (valid() && ctx.duplicateSelectedTracks) ctx.duplicateSelectedTracks(); }, duplicate, "⌘D"},
        {"Delete", [&ctx, valid, deleting] {
            if (! valid()) return;
            std::set<int> indices;
            for (int i = 0; i < ctx.numTracks() + ctx.numReturns(); ++i)
                if (const auto* t = ctx.trackAt(i); t && std::find(deleting.begin(), deleting.end(), t->id) != deleting.end()) indices.insert(i);
            if (indices.size() != deleting.size() || indices.empty()) return;
            ctx.trackSelection = indices; ctx.selectedTrack = *indices.begin(); deleteTrackSelection(ctx);
        }, editable && normal && ! deleting.empty() && (ctx.deleteSelectedTracks || ctx.applyEdit), "Del"}, {"", {}, false},
        {"Rename", [&ctx, valid, index] {
            if (! valid() || ! ctx.renameSelection) return;
            ctx.selectedTrack = index;
            ctx.selectedClipTrack = ctx.selectedClipUid = ctx.selectedClipScene = ctx.selectedTakeLane = -1;
            ctx.detailTab = DetailTab::Devices; ctx.renameSelection();
        }, editable && normal && static_cast<bool>(ctx.renameSelection), "⌘R"},
        {"Edit Info Text", {}, false}, {"", {}, false},
    };
    if (track && (track->kind == model::TrackKind::Audio || track->kind == model::TrackKind::Midi))
        result.push_back({track->frozen ? "Unfreeze Track" : "Freeze Track", [&ctx, valid, index] {
            if (valid() && ctx.renderTrack) ctx.renderTrack(index, true, false);
        }, editable && static_cast<bool>(ctx.renderTrack), "⌥⇧⌘F"});
    if (normal)
    {
        result.push_back({track->deactivated ? "Activate Track" : "Deactivate Track", [&ctx, valid] {
            if (valid()) toggleSelectedTracksDeactivated(ctx);
        }, editable && static_cast<bool>(ctx.applyEdit)});
        result.push_back({track->hidden ? "Show Track" : "Hide Track", [&ctx, valid] {
            if (valid()) toggleSelectedTracksHidden(ctx);
        }, editable && static_cast<bool>(ctx.applyEdit)});
    }
    result.push_back({track && track->kind == model::TrackKind::Group ? "Bounce Group in Place" : "Bounce Track in Place", [&ctx, valid, index] {
        if (valid() && ctx.renderTrack) ctx.renderTrack(index, false, false);
    }, editable && index >= 0 && index < ctx.numTracks() && static_cast<bool>(ctx.renderTrack)});
    result.push_back({"", {}, false});
    result.push_back({"Insert Audio Track", [&ctx, valid] { if (valid() && ctx.insertTrack) ctx.insertTrack(false); }, editable && static_cast<bool>(ctx.insertTrack), "⌘T"});
    result.push_back({"Insert MIDI Track", [&ctx, valid] { if (valid() && ctx.insertTrack) ctx.insertTrack(true); }, editable && static_cast<bool>(ctx.insertTrack), "⇧⌘T"});
    result.push_back({"Insert Return Track", [&ctx, valid] { if (valid() && ctx.insertReturnTrack) ctx.insertReturnTrack(); }, editable && ctx.song->returns.size() < model::maxReturnTracks && static_cast<bool>(ctx.insertReturnTrack), "⌥⌘T"});
    result.push_back({"Group Tracks", [&ctx, valid] { if (valid() && ctx.groupSelectedTracks) ctx.groupSelectedTracks(); }, group, "⌘G"});
    if (track && track->kind == model::TrackKind::Group)
        result.push_back({"Ungroup Tracks", [&ctx, valid] { if (valid() && ctx.ungroupSelectedTracks) ctx.ungroupSelectedTracks(); }, editable && static_cast<bool>(ctx.ungroupSelectedTracks), "⇧⌘G"});
    result.push_back({"", {}, false});
    const bool content = track && ! track->arrangementClips.empty();
    result.push_back({"Select Track Content", [&ctx, valid, index] {
        if (! valid()) return;
        double first = 1e18, last = -1e18;
        int firstTrack = index, lastTrack = index;
        for (int selected : ctx.selectedTrackIndices())
            if (const auto* current = ctx.trackAt(selected))
                for (const auto& clip : current->arrangementClips)
                { first = std::min(first, clip.currentStart); last = std::max(last, clip.currentEnd); firstTrack = std::min(firstTrack, selected); lastTrack = std::max(lastTrack, selected); }
        if (last <= first) return;
        ctx.arrangementSelection = {true, first, last, firstTrack, lastTrack};
        ctx.mainView = MainView::Arrangement; ctx.sessionTrackSelection = false;
        ctx.selectedClipTrack = ctx.selectedClipUid = ctx.selectedClipScene = ctx.selectedTakeLane = -1;
        if (ctx.modelChanged) ctx.modelChanged();
        if (ctx.requestRepaint) ctx.requestRepaint();
    }, content});
    const bool regular = track && (track->kind == model::TrackKind::Audio || track->kind == model::TrackKind::Midi);
    if (regular)
    {
        result.push_back({"", {}, false});
        result.push_back({"Insert Take Lane", [&ctx, valid] { if (valid()) insertEmptyTakeLane(ctx); }, editable && static_cast<bool>(ctx.applyEdit), "⌥⇧T"});
        result.push_back({"Show Take Lanes", [&ctx, valid, index] {
            if (! valid()) return;
            auto& unfolded = ctx.unfoldedTakeTracks;
            const auto found = std::find(unfolded.begin(), unfolded.end(), index);
            if (found == unfolded.end()) { unfolded.push_back(index); ctx.automationVisible = false; } else unfolded.erase(found);
            ctx.mainView = MainView::Arrangement;
            if (ctx.modelChanged) ctx.modelChanged();
            if (ctx.requestRepaint) ctx.requestRepaint();
        }, true, "⌥⌘U"});
        result.push_back({"Delete All Take Lanes", {}, false});
        result.push_back({"Delete Unused Take Lanes", {}, false});
    }
    result.push_back({"", {}, false});
    result.push_back({"Link Tracks", {}, false}); result.push_back({"Unlink Track(s)", {}, false});
    if (regular)
    {
        result.push_back({"", {}, false});
        result.push_back({track->kind == model::TrackKind::Midi ? "Save as Default MIDI Track" : "Save as Default Audio Track", {}, false});
    }
    return result;
}

namespace
{
void applyTrackFlag(UiContext& ctx, bool hidden)
{
    const auto* track = ctx.trackAt(ctx.selectedTrack);
    if (! ctx.song || ! ctx.applyEdit || ! track || track->kind == model::TrackKind::Main || ctx.recording || ctx.recordingRequested) return;
    const auto selection = ctx.selectedChannelIndices();
    const bool value = hidden ? ! track->hidden : ! track->deactivated;
    auto command = hidden ? model::setTracksHidden(*ctx.song, selection, value) : model::setTracksDeactivated(*ctx.song, selection, value);
    if (! command.apply) return;
    ctx.applyEdit(std::move(command));
    if (hidden && value && ! ctx.showHiddenTracks)
        ctx.statusText = selection.size() == 1 ? "Track hidden: View > Show Hidden Tracks lists it again"
                                               : std::to_string(selection.size()) + " tracks hidden: View > Show Hidden Tracks lists them again";
    if (ctx.modelChanged) ctx.modelChanged();
    if (ctx.requestRepaint) ctx.requestRepaint();
}
} // namespace

void toggleSelectedTracksHidden(UiContext& ctx) { applyTrackFlag(ctx, true); }
void toggleSelectedTracksDeactivated(UiContext& ctx) { applyTrackFlag(ctx, false); }

bool trackHiddenInViews(const UiContext& ctx, int index)
{
    if (ctx.showHiddenTracks || ! ctx.song) return false;
    const auto* track = ctx.trackAt(index);
    for (int guard = 0; track && guard < 256; ++guard)
    {
        if (track->hidden) return true;
        const auto parent = track->groupId;
        if (parent == -1) return false;
        const auto found = std::find_if(ctx.song->tracks.begin(), ctx.song->tracks.end(), [parent](const auto& t) { return t.id == parent; });
        track = found == ctx.song->tracks.end() ? nullptr : &*found;
    }
    return false;
}

void deleteTrackSelection(UiContext& ctx)
{
    if (! ctx.song || ctx.recording || ctx.recordingRequested) return;
    if (ctx.deleteSelectedTracks) { ctx.deleteSelectedTracks(); return; }
    if (! ctx.applyEdit) return;
    auto command = model::deleteTracks(*ctx.song, ctx.selectedChannelIndices());
    if (! command.apply) return;
    ctx.applyEdit(std::move(command));
    ctx.selectedTrack = std::clamp(ctx.selectedTrack, 0, ctx.numTracks() + ctx.numReturns());
    ctx.trackSelection.clear(); ctx.trackSelectionAnchor = ctx.selectedTrack;
    ctx.selectedClipTrack = ctx.selectedClipUid = ctx.selectedClipScene = ctx.selectedTakeLane = -1;
    if (ctx.modelChanged) ctx.modelChanged();
    if (ctx.requestRepaint) ctx.requestRepaint();
}

void insertEmptyTakeLane(UiContext& ctx)
{
    const int index = ctx.selectedTrack;
    const auto* track = ctx.trackAt(index);
    if (! ctx.song || ! track || index < 0 || index >= ctx.numTracks() || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested
        || (track->kind != model::TrackKind::Audio && track->kind != model::TrackKind::Midi)) return;
    const auto id = track->id;
    const auto count = track->takeLanes.size();
    model::TakeLane lane; lane.name = "Take " + std::to_string(track->nextTakeLaneId + 1);
    auto command = model::appendTakeLanes(*ctx.song, index, {std::move(lane)}); command.label = "Insert Take Lane";
    ctx.applyEdit(std::move(command));
    track = ctx.trackAt(index);
    if (! track || track->id != id || track->takeLanes.size() != count + 1) return;
    if (std::find(ctx.unfoldedTakeTracks.begin(), ctx.unfoldedTakeTracks.end(), index) == ctx.unfoldedTakeTracks.end()) ctx.unfoldedTakeTracks.push_back(index);
    ctx.automationVisible = false; ctx.mainView = MainView::Arrangement; ctx.sessionTrackSelection = false;
    ctx.selectedClipTrack = index; ctx.selectedClipUid = ctx.selectedClipScene = -1; ctx.selectedTakeLane = track->takeLanes.back().id;
    if (ctx.modelChanged) ctx.modelChanged();
    if (ctx.requestRepaint) ctx.requestRepaint();
}

std::vector<MenuAction> clipContextActions(UiContext& ctx, model::ClipLocation location)
{
    const auto* song = ctx.song;
    const auto* clip = song ? model::findClip(*song, location) : nullptr;
    const bool editable = clip && ctx.applyEdit && ! ctx.recording && ! ctx.recordingRequested;
    auto valid = [&ctx, song, location] { return ctx.song == song && song && model::findClip(*song, location); };
    std::vector<MenuAction> result{
        {"Cut", {}, false, "⌘X"}, {"Copy", {}, false, "⌘C"}, {"Paste", {}, false, "⌘V"},
        {"Duplicate", [&ctx, valid, location] { if (valid() && ctx.applyEdit) ctx.applyEdit(model::duplicateClip(*ctx.song, location)); }, editable, "⌘D"},
        {"Duplicate as Linked", [&ctx, valid, location] { // TRK-5
            if (valid() && ctx.applyEdit) ctx.applyEdit(model::duplicateClipLinked(*ctx.song, location.track, location.uid));
        }, editable && location.kind == model::ClipLocation::Kind::Arrangement && clip->kind == model::Clip::Kind::Midi},
        {"Unlink Clip", [&ctx, valid, location] { if (valid() && ctx.applyEdit) ctx.applyEdit(model::unlinkClip(*ctx.song, location)); },
         editable && clip->linkGroup >= 0},
        {"Delete", [&ctx, valid, location] { if (valid() && ctx.applyEdit) ctx.applyEdit(model::deleteClip(*ctx.song, location)); }, editable, "⌫"},
        {"Rename", [&ctx, valid, location] {
            if (! valid() || ! ctx.renameSelection) return;
            ctx.selectedClipTrack = location.track; ctx.selectedClipUid = location.uid;
            ctx.selectedClipScene = location.scene; ctx.selectedTakeLane = location.laneId; ctx.renameSelection();
        }, editable && static_cast<bool>(ctx.renameSelection), "⌘R"},
        {"Edit Info Text", {}, false}, {"", {}, false},
        {clip && clip->disabled ? "Activate Clip" : "Deactivate Clip", [&ctx, valid, location] {
            if (! valid() || ! ctx.applyEdit) return;
            auto changed = *model::findClip(*ctx.song, location); changed.disabled = ! changed.disabled;
            ctx.applyEdit(model::modifyClip(*ctx.song, location, std::move(changed), "Activate/Deactivate Clip"));
        }, editable, "0"},
        {"Extract Groove(s)", {}, false},
    };
    if (location.kind == model::ClipLocation::Kind::Arrangement)
    {
        result.push_back({"Split", [&ctx, valid, location] {
            if (! valid()) return;
            ctx.selectedTrack = location.track; ctx.clipEditorFocused = false;
            splitArrangementSelection(ctx);
        }, editable && ! ctx.trackAt(location.track)->frozen, "⌘E"});
        result.push_back({"Consolidate", [&ctx, valid, location] {
            if (! valid() || ! ctx.consolidateSelection) return;
            ctx.selectedClipTrack = location.track; ctx.selectedClipUid = location.uid; ctx.selectedClipScene = -1;
            ctx.consolidateSelection();
        }, editable && static_cast<bool>(ctx.consolidateSelection), "⌘J"});
    }
    result.push_back({"", {}, false});
    const auto* track = ctx.trackAt(location.track);
    result.push_back({track && track->frozen ? "Unfreeze Track" : "Freeze Track", [&ctx, valid, location] {
        if (valid() && ctx.renderTrack) ctx.renderTrack(location.track, true, false);
    }, editable && static_cast<bool>(ctx.renderTrack), "⌥⇧⌘F"});
    result.push_back({"Bounce Track in Place", [&ctx, valid, location] {
        if (valid() && ctx.renderTrack) ctx.renderTrack(location.track, false, false);
    }, editable && static_cast<bool>(ctx.renderTrack)});
    if (clip && clip->kind == model::Clip::Kind::Audio)
    {
        result.push_back({"", {}, false});
        if (location.kind == model::ClipLocation::Kind::Arrangement)
            result.push_back({"Strip Silence", [&ctx, valid, location] {
                if (valid() && ctx.stripSilence) ctx.stripSilence(location);
            }, editable && ! ctx.trackAt(location.track)->frozen && static_cast<bool>(ctx.stripSilence)});
        result.push_back({"Resynthesize to New MIDI Track", [&ctx, valid, location] {
            if (valid() && ctx.resynthesizeClip) ctx.resynthesizeClip(location);
        }, editable && static_cast<bool>(ctx.resynthesizeClip)});
        const char* labels[] = {"Convert Melody to New MIDI Track", "Convert Harmony to New MIDI Track", "Convert Drums to New MIDI Track", "Slice to New MIDI Track"};
        for (int mode = 0; mode < 4; ++mode)
            result.push_back({labels[mode], [&ctx, valid, location, mode] {
                if (valid() && ctx.convertAudioToMidi) ctx.convertAudioToMidi(location, mode);
            }, editable && static_cast<bool>(ctx.convertAudioToMidi)});
    }
    return result;
}

juce::PopupMenu createTrackContextMenu(UiContext& ctx)
{
    juce::PopupMenu menu;
    appendActions(menu, trackContextActions(ctx));
    const auto* track = ctx.trackAt(ctx.selectedTrack);
    const auto* song = ctx.song;
    auto indices = ctx.selectedTrackIndices();
    if (indices.empty() && track && track->kind == model::TrackKind::Return) indices.push_back(ctx.selectedTrack);
    std::vector<model::TrackId> ids;
    for (int index : indices) if (const auto* current = ctx.trackAt(index)) ids.push_back(current->id);
    auto currentIndices = [&ctx, song, ids] {
        std::vector<int> current;
        if (ctx.song != song || ! song) return current;
        for (int index = 0; index < ctx.numTracks() + ctx.numReturns(); ++index)
            if (const auto* candidate = ctx.trackAt(index); candidate && std::find(ids.begin(), ids.end(), candidate->id) != ids.end()) current.push_back(index);
        return current;
    };
    if (track && track->kind != model::TrackKind::Main)
    {
        menu.addSeparator();
        menu.addCustomItem(10000, std::make_unique<TrackColorGrid>(track->colorIndex, ctx.uiZoom, [&ctx, song, currentIndices](int color) {
            if (ctx.song == song && ctx.applyEdit && ! ctx.recording && ! ctx.recordingRequested)
                ctx.applyEdit(model::setTrackColor(*song, currentIndices(), color));
        }), nullptr, "Track Colors");
        menu.addSeparator();
        appendActions(menu, {{"Assign Track Color to Clips", [&ctx, song, currentIndices] {
            if (ctx.song != song || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested) return;
            std::vector<model::EditCommand> edits;
            for (int index : currentIndices())
                if (const auto* current = ctx.trackAt(index))
                {
                    auto colorClip = [&](const model::Clip& clip, model::ClipLocation location) {
                        if (clip.colorIndex == current->colorIndex) return;
                        auto changed = clip; changed.colorIndex = current->colorIndex;
                        edits.push_back(model::modifyClip(*song, location, std::move(changed), "Change Clip Color"));
                    };
                    for (const auto& clip : current->arrangementClips) colorClip(clip, model::ClipLocation::arrangement(index, clip.uid));
                    for (size_t scene = 0; scene < current->slots.size(); ++scene)
                        if (current->slots[scene].clip) colorClip(*current->slots[scene].clip, model::ClipLocation::session(index, current->slots[scene].clip->uid, (int) scene));
                    for (const auto& lane : current->takeLanes)
                        for (const auto& clip : lane.clips) colorClip(clip, model::ClipLocation::take(index, clip.uid, lane.id));
                }
            if (! edits.empty()) ctx.applyEdit(model::compose("Assign Track Color to Clips", std::move(edits)));
        }, ! ctx.recording && ! ctx.recordingRequested && static_cast<bool>(ctx.applyEdit)}});
    }
    return menu;
}

void showTrackContextMenu(UiContext& ctx)
{
    createTrackContextMenu(ctx).showMenuAsync(juce::PopupMenu::Options());
}

juce::Image renderTrackMenuPreview(UiContext& ctx, float scale)
{
    MenuLookAndFeel look; look.configure(ctx.theme, ctx.uiZoom);
    auto menu = createTrackContextMenu(ctx);
    int width = 0, height = 4;
    juce::PopupMenu::MenuItemIterator sizes(menu);
    while (sizes.next())
    {
        int w = 0, h = 0;
        if (sizes.getItem().customComponent) sizes.getItem().customComponent->getIdealSize(w, h);
        else look.getIdealPopupMenuItemSize(sizes.getItem().text, sizes.getItem().isSeparator, 0, w, h);
        width = std::max(width, w); height += h;
    }
    juce::Image image(juce::Image::ARGB, (int) std::ceil((float) width * scale), (int) std::ceil((float) height * scale), true);
    juce::Graphics graphics(image); graphics.addTransform(juce::AffineTransform::scale(scale));
    look.drawPopupMenuBackground(graphics, width, height);
    juce::PopupMenu::MenuItemIterator rows(menu);
    int y = 2;
    while (rows.next())
    {
        const auto& item = rows.getItem();
        int w = 0, h = 0;
        if (item.customComponent)
        {
            item.customComponent->getIdealSize(w, h);
            item.customComponent->setBounds(0, y, width, h);
            const juce::Graphics::ScopedSaveState saved(graphics);
            graphics.setOrigin(0, y); item.customComponent->paint(graphics);
        }
        else
        {
            look.getIdealPopupMenuItemSize(item.text, item.isSeparator, 0, w, h);
            look.drawPopupMenuItem(graphics, {0, y, width, h}, item.isSeparator, item.isEnabled, false, item.isTicked, item.subMenu != nullptr,
                                  item.text, item.shortcutKeyDescription, item.image.get(), nullptr);
        }
        y += h;
    }
    return image;
}

void showSessionSlotContextMenu(UiContext& ctx, int track, int scene)
{
    const auto* song = ctx.song;
    const auto* target = ctx.trackAt(track);
    if (! song || ! target || track >= ctx.numTracks() || scene < 0 || (size_t) scene >= song->scenes.size()) return;
    if ((size_t) scene < target->slots.size() && target->slots[(size_t) scene].clip)
    { showClipContextMenu(ctx, model::ClipLocation::session(track, target->slots[(size_t) scene].clip->uid, scene)); return; }
    const auto id = target->id;
    juce::PopupMenu menu;
    appendActions(menu, {
        {"Cut", {}, false, "⌘X"}, {"Copy", {}, false, "⌘C"}, {"Paste", {}, false, "⌘V"}, {"", {}, false},
        {"Insert MIDI Clip", [&ctx, song, track, scene, id] {
            const auto* current = ctx.trackAt(track);
            if (ctx.song != song || ! current || current->id != id || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested
                || (size_t) scene >= song->scenes.size() || ((size_t) scene < current->slots.size() && current->slots[(size_t) scene].clip)) return;
            model::Clip fresh;
            if (model::makeEmptyMidiClip(*song, 0, song->timeSigNumerator * 4.0 / song->timeSigDenominator, fresh))
            { fresh.colorIndex = current->colorIndex; ctx.applyEdit(model::setSessionClip(*song, track, scene, std::move(fresh))); }
        }, target->kind == model::TrackKind::Midi && ! ctx.recording && ! ctx.recordingRequested && static_cast<bool>(ctx.applyEdit), "⇧⌘M"},
        {"Remove Stop Button", {}, false, "⌘E"},
    });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void showClipContextMenu(UiContext& ctx, model::ClipLocation location)
{
    juce::PopupMenu menu;
    appendActions(menu, clipContextActions(ctx, location));
    const auto* song = ctx.song;
    if (const auto* clip = song ? model::findClip(*song, location) : nullptr)
        menu.addSubMenu("Colors", colorsMenu(clip->colorIndex, [&ctx, song, location](int color) {
            if (ctx.song != song || ! ctx.applyEdit) return;
            if (const auto* current = model::findClip(*song, location))
            {
                auto changed = *current; changed.colorIndex = color;
                ctx.applyEdit(model::modifyClip(*song, location, std::move(changed), "Change Clip Color"));
            }
        }), ! ctx.recording && ! ctx.recordingRequested && static_cast<bool>(ctx.applyEdit));
    menu.showMenuAsync(juce::PopupMenu::Options());
}
} // namespace sonora::ui
