#include "ui/RootWidget.h"

#include <cstring>
#include <cmath>

#include "engine/AudioFileFormats.h"
#include "midi/MidiManager.h"
#include "ui/Painter.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{

namespace
{
MouseEvent localise(MouseEvent e, const Rect& originInRoot)
{
    e.x -= originInRoot.x;
    e.y -= originInRoot.y;
    return e;
}
} // namespace

void RootWidget::dispatchMouseDown(MouseEvent e)
{
    hoverTicks_ = 0;
    ctx_.browserDrag.reset();
    ctx_.trackDrag.reset(); trackDrop_.reset(); movingTracks_ = false;
    downX_ = e.x; downY_ = e.y;
    capturedRight_ = e.right;
    float lx = 0, ly = 0;
    const Widget* tool = (ctx_.midiMapMode || ctx_.keyMapMode) ? findAt(e.x, e.y, lx, ly) : nullptr;
    if ((ctx_.midiMapMode || ctx_.keyMapMode) && ! (tool != nullptr && tool->bypassMapMode))
    {
        // MIDI map mode: clicks select a mappable control instead of operating it.
        captured_ = nullptr;
        Widget* w = findAt(e.x, e.y, lx, ly);
        std::optional<midi::ControlTarget> target;
        for (; w; w = w->parent())
        {
            const auto origin = w->boundsInRoot();
            target = w->mappingTargetAt(e.x - origin.x, e.y - origin.y);
            if (target) break;
        }
        if (ctx_.keyMapMode && target && target->kind != midi::ControlTarget::Kind::SessionClip) target.reset();
        if (target && ctx_.midi != nullptr)
        {
            setKeyboardFocus(w);
            ctx_.mapSelection = target;
            if (e.right)
                ctx_.midi->removeMapping(*target, ctx_.keyMapMode);
            else if (! ctx_.keyMapMode)
                ctx_.midi->beginLearn(*target);
        }
        else
        {
            ctx_.mapSelection.reset();
            if (ctx_.midi != nullptr)
                ctx_.midi->cancelLearn();
        }
        markDirty();
        return;
    }
    captured_ = findAt(e.x, e.y, lx, ly);
    setKeyboardFocus(captured_);
    if (captured_ == nullptr || captured_ == this)
        return;
    for (auto* a = captured_; a != nullptr && a != this; a = a->parent())
        a->descendantPressed(captured_);
    if (! isAlive(captured_)) { captured_ = nullptr; return; }
    capturedOrigin_ = captured_->boundsInRoot();
    auto local = localise(e, capturedOrigin_);
    if (e.clicks >= 2)
        captured_->mouseDoubleClick(local);
    else
        captured_->mouseDown(local);
}

void RootWidget::updateTrackDrag(const MouseEvent& e)
{
    trackDrop_.reset();
    if (! ctx_.trackDrag) return;
    const auto& drag = *ctx_.trackDrag;
    if (drag.song != ctx_.song || drag.structure != ctx_.trackStructure() || ctx_.recording || ctx_.recordingRequested)
    { ctx_.trackDrag.reset(); movingTracks_ = false; captured_ = nullptr; markDirty(); return; }
    movingTracks_ |= std::abs(e.x - downX_) > 4 || std::abs(e.y - downY_) > 4;
    if (! movingTracks_) return;
    float lx = 0, ly = 0;
    for (auto* w = findAt(e.x, e.y, lx, ly); w && w != this; w = w->parent())
        if (auto target = w->trackDropTarget(localise(e, w->boundsInRoot())))
        {
            if (model::moveTracks(*ctx_.song, drag.tracks, target->before, target->group).apply)
            {
                const auto origin = w->boundsInRoot();
                target->indicator.x += origin.x; target->indicator.y += origin.y;
                trackDrop_ = target;
            }
            break;
        }
    cursor_ = Cursor::Move;
    ctx_.statusText = trackDrop_ ? (trackDrop_->group >= 0 ? "Move tracks into group" : "Reorder tracks") : "Cannot move tracks here";
    markDirty();
}

void RootWidget::dispatchMouseDrag(MouseEvent e)
{
    e.right = e.right || capturedRight_;
    if (ctx_.trackDrag) { updateTrackDrag(e); return; }
    if (! isAlive(captured_))
        captured_ = nullptr;
    if (captured_ != nullptr && captured_ != this)
        captured_->mouseDrag(localise(e, capturedOrigin_));
    if (ctx_.browserDrag)
    {
        ctx_.browserDragX = e.x;
        ctx_.browserDragY = e.y;
        markDirty();
    }
}

void RootWidget::dispatchMouseUp(MouseEvent e)
{
    e.right = e.right || capturedRight_; // host release events can omit the button that began the gesture
    capturedRight_ = false;
    if (ctx_.trackDrag)
    {
        updateTrackDrag(e);
        const auto drag = ctx_.trackDrag; const auto target = trackDrop_;
        ctx_.trackDrag.reset(); trackDrop_.reset(); captured_ = nullptr; movingTracks_ = false; cursor_ = Cursor::Default;
        if (drag && target && ctx_.applyEdit)
            ctx_.applyEdit(model::moveTracks(*ctx_.song, drag->tracks, target->before, target->group));
        markDirty(); return;
    }
    if (ctx_.browserDrag)
    {
        const auto payload = *ctx_.browserDrag;
        ctx_.browserDrag.reset();
        captured_ = nullptr;
        dropBrowserPayload(payload, e);
        return;
    }
    if (! isAlive(captured_))
        captured_ = nullptr;
    if (captured_ != nullptr && captured_ != this)
        captured_->mouseUp(localise(e, capturedOrigin_));
    captured_ = nullptr;
}

void RootWidget::dropBrowserPayload(BrowserDrag payload, const MouseEvent& e)
{
    payload.oneTrackPerFile = e.cmd && payload.files.size() > 1;
    float lx = 0, ly = 0;
    for (auto* w = findAt(e.x, e.y, lx, ly); w && w != this; w = w->parent())
        if (w->browserDrop(payload, localise(e, w->boundsInRoot()))) break;
    markDirty();
}

std::optional<BrowserDrag> RootWidget::fileDragPayload(const std::vector<std::string>& paths)
{
    BrowserDrag payload;
    for (const auto& path : paths)
        if (engine::isAudioFile(juce::File(juce::String::fromUTF8(path.c_str()))))
            payload.files.push_back(path);
    if (payload.files.empty()) return std::nullopt;
    payload.value = payload.files.front();
    payload.label = payload.files.size() == 1 ? juce::File(juce::String::fromUTF8(payload.value.c_str())).getFileName().toStdString()
                                              : std::to_string(payload.files.size()) + " files";
    return payload;
}

bool RootWidget::acceptsFileDrag(const std::vector<std::string>& paths)
{
    return fileDragPayload(paths).has_value() || (paths.size() == 1 && juce::File(juce::String::fromUTF8(paths[0].c_str())).hasFileExtension("als"));
}

bool RootWidget::dispatchFileDragMove(float x, float y, const std::vector<std::string>& paths)
{
    if (ctx_.exportOpen) { dispatchFileDragExit(); return false; }
    if (ctx_.browserDrag && ! externalDrag_) return false; // an in-app browser drag owns the overlay
    auto payload = fileDragPayload(paths);
    if (! payload || ctx_.recording) { if (externalDrag_) dispatchFileDragExit(); return acceptsFileDrag(paths); }
    ctx_.browserDrag = std::move(payload);
    ctx_.browserDragX = x;
    ctx_.browserDragY = y;
    externalDrag_ = true;
    markDirty();
    return true;
}

void RootWidget::dispatchFileDragExit()
{
    if (! externalDrag_) return;
    externalDrag_ = false;
    ctx_.browserDrag.reset();
    markDirty();
}

bool RootWidget::dispatchFileDrop(MouseEvent e, const std::vector<std::string>& paths)
{
    dispatchFileDragExit();
    if (ctx_.exportOpen) { ctx_.statusText = "Finish audio processing before dropping files"; markDirty(); return false; }
    if (ctx_.recording || ctx_.recordingRequested) { ctx_.statusText = "Finish recording before dropping files"; markDirty(); return false; }
    if (auto payload = fileDragPayload(paths))
    {
        dropBrowserPayload(std::move(*payload), e);
        return true;
    }
    if (paths.size() == 1 && juce::File(juce::String::fromUTF8(paths[0].c_str())).hasFileExtension("als") && ctx_.openFile)
    {
        ctx_.openFile(paths[0]);
        markDirty();
        return true;
    }
    return false;
}

void RootWidget::dispatchMouseMove(MouseEvent e)
{
    float lx = 0, ly = 0;
    auto* w = findAt(e.x, e.y, lx, ly);
    if (! isAlive(hovered_))
        hovered_ = nullptr;
    if (w != hovered_)
    {
        hoverTicks_ = 0; markDirty();
        if (hovered_ != nullptr) hovered_->mouseExit();
    }
    hovered_ = w;
    if (w != nullptr && w != this)
    {
        const auto local = localise(e, w->boundsInRoot());
        cursor_ = w->cursorAt(local.x, local.y);
        w->mouseMove(local);
    }
    else
        cursor_ = Cursor::Default;
}

void RootWidget::dispatchMouseExit()
{
    cursor_ = Cursor::Default;
    if (isAlive(hovered_))
        hovered_->mouseExit();
    hovered_ = nullptr;
    hoverTicks_ = 0; markDirty();
}

void RootWidget::dispatchMouseWheel(MouseEvent e)
{
    float lx = 0, ly = 0;
    for (auto* w = findAt(e.x, e.y, lx, ly); w != nullptr && w != this; w = w->parent())
        if (w->mouseWheel(localise(e, w->boundsInRoot())))
            return;
}

void RootWidget::dispatchMouseMagnify(MouseEvent e, float factor)
{
    float lx = 0, ly = 0;
    for (auto* w = findAt(e.x, e.y, lx, ly); w != nullptr && w != this; w = w->parent())
        if (w->mouseMagnify(localise(e, w->boundsInRoot()), factor))
            return;
}

void RootWidget::setKeyboardFocus(Widget* widget)
{
    if (keyboardFocus_ != widget && isAlive(keyboardFocus_)) keyboardFocus_->focusLost();
    keyboardFocus_ = widget;
}

bool RootWidget::dispatchUserKey(const KeyEvent& k)
{
    bool typing = false;
    if (isAlive(keyboardFocus_))
        for (auto* w = keyboardFocus_; w != nullptr; w = w->parent()) typing |= w->wantsTextInput();
    const bool plain = ! k.cmd && ! k.alt && ! k.ctrl;
    if (ctx_.userShortcut && ! typing && ! ctx_.preferencesOpen && ! ctx_.exportOpen && ! ctx_.collabOpen && ! ctx_.midiMapMode && ! ctx_.keyMapMode
        && ! ctx_.trackDrag && ! (ctx_.computerKeyboard && plain) && ctx_.userShortcut(k))
    {
        markDirty();
        return true;
    }
    return dispatchKey(k);
}

bool RootWidget::dispatchKey(const KeyEvent& k)
{
    if (ctx_.trackDrag && k.keyCode == juce::KeyPress::escapeKey)
    { ctx_.trackDrag.reset(); trackDrop_.reset(); captured_ = nullptr; movingTracks_ = false; cursor_ = Cursor::Default; markDirty(); return true; }
    if (ctx_.preferencesOpen || ctx_.exportOpen || ctx_.collabOpen) return keyPressed(k);
    if (ctx_.computerKeyboard)
    {
        bool typing = false;
        if (isAlive(keyboardFocus_))
            for (auto* w = keyboardFocus_; w != nullptr; w = w->parent()) typing |= w->wantsTextInput();
        if (! typing && handleComputerKeyboard(k))
            return true;
    }
    const int code = midi::normaliseLaunchKey(k.keyCode);
    if (ctx_.keyMapMode && ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && code != 0)
    {
        if (ctx_.mapSelection && ctx_.midi && ctx_.mapSelection->kind == midi::ControlTarget::Kind::SessionClip)
        {
            midi::Mapping mapping; mapping.keyCode = code; mapping.target = *ctx_.mapSelection;
            ctx_.midi->addMapping(mapping);
            markDirty();
        }
        return true;
    }
    if (code != 0 && ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift
        && std::any_of(heldKeys_.begin(), heldKeys_.end(), [&](const auto& m) { return m.keyCode == code; }))
        return true; // OS auto-repeat must not retrigger Toggle/Trigger
    if (ctx_.browserDrag && k.keyCode == juce::KeyPress::escapeKey)
    {
        ctx_.browserDrag.reset();
        captured_ = nullptr;
        markDirty();
        return true;
    }
    // Modal preferences and MIDI mapping own input before an editor beneath the overlay.
    if (ctx_.preferencesOpen || ctx_.exportOpen || ctx_.collabOpen || ctx_.midiMapMode || ctx_.keyMapMode) return keyPressed(k);
    if (isAlive(keyboardFocus_))
    {
        bool shown = true;
        for (auto* w = keyboardFocus_; w; w = w->parent()) shown &= w->visible;
        if (shown)
        {
            // Editors claim local actions first (e.g. Delete notes rather than the whole clip).
            // Ancestors then get global shortcuts. A handled key may rebuild/delete the focus widget.
            for (auto* w = keyboardFocus_; w; w = w->parent())
                if (w->keyPressed(k)) return true;
        }
        else setKeyboardFocus(nullptr);
    }
    if (ctx_.mainView == MainView::Session && code != 0 && ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift
        && ctx_.midi && ctx_.mappedControl)
        for (const auto& mapping : ctx_.midi->mappings())
            if (mapping.keyCode == code && mapping.target.kind == midi::ControlTarget::Kind::SessionClip)
            {
                heldKeys_.push_back(mapping);
                ctx_.mappedControl(mapping.target, {1.0f, false, 0, true});
                markDirty();
                return true;
            }
    if (isAlive(keyboardFocus_)) return false; // the focused chain already tried the root; never edit a different view
    // Without explicit focus, the visible editor handles local actions before global shortcuts.
    std::function<bool(Widget*)> visit = [&](Widget* w) {
        if (! w->visible)
            return false;
        for (auto& c : w->children())
            if (visit(c.get()))
                return true;
        return w->keyPressed(k); // root last: global shortcuts (MainLayout)
    };
    return visit(this);
}

// Computer MIDI Keyboard (Live's layout): A W S E D F T G Y H U J K O L P ; ' are the keys of two octaves starting at the
// octave's C; Z / X move the octave, C / V the velocity. The keys never reach the shortcuts while it is on.
bool RootWidget::handleComputerKeyboard(const KeyEvent& k)
{
    if (k.cmd || k.alt || k.ctrl || k.shift)
        return false;
    int ch = k.textChar != 0 ? (int) k.textChar : k.keyCode;
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    static const char layout[] = "awsedftgyhujkolp;'";
    const char* at = ch > 0 && ch < 128 ? std::strchr(layout, ch) : nullptr;
    if (at != nullptr && *at != '\0')
    {
        const int note = std::clamp((ctx_.computerKeyboardOctave + 2) * 12 + (int) (at - layout), 0, 127);
        const int key = k.keyCode != 0 ? k.keyCode : ch;
        if (std::any_of(heldNotes_.begin(), heldNotes_.end(), [&](const HeldNote& h) { return h.keyCode == key; }))
            return true; // OS auto-repeat
        heldNotes_.push_back({key, note});
        if (ctx_.computerKeyboardNote) ctx_.computerKeyboardNote(note, ctx_.computerKeyboardVelocity, true);
        return true;
    }
    switch (ch)
    {
        case 'z': ctx_.computerKeyboardOctave = std::max(-2, ctx_.computerKeyboardOctave - 1); break;
        case 'x': ctx_.computerKeyboardOctave = std::min(8, ctx_.computerKeyboardOctave + 1); break;
        case 'c': ctx_.computerKeyboardVelocity = std::max(1, ctx_.computerKeyboardVelocity - 20); break;
        case 'v': ctx_.computerKeyboardVelocity = std::min(127, ctx_.computerKeyboardVelocity + 20); break;
        default: return false;
    }
    ctx_.statusText = "Computer MIDI Keyboard: octave C" + std::to_string(ctx_.computerKeyboardOctave) + ", velocity "
                      + std::to_string(ctx_.computerKeyboardVelocity);
    markDirty();
    return true;
}

bool RootWidget::dispatchKeyUp(int keyCode)
{
    if (const auto note = std::find_if(heldNotes_.begin(), heldNotes_.end(), [&](const HeldNote& h) { return h.keyCode == keyCode; });
        note != heldNotes_.end())
    {
        const int value = note->note;
        heldNotes_.erase(note);
        if (ctx_.computerKeyboardNote) ctx_.computerKeyboardNote(value, 0, false);
        return true;
    }
    const int code = midi::normaliseLaunchKey(keyCode);
    const auto found = std::find_if(heldKeys_.begin(), heldKeys_.end(), [&](const auto& m) { return m.keyCode == code; });
    if (found == heldKeys_.end()) return false;
    const auto target = found->target;
    heldKeys_.erase(found);
    if (ctx_.mappedControl) ctx_.mappedControl(target, {0.0f, false, 0, false, true});
    markDirty();
    return true;
}

void RootWidget::releaseMappedKeys()
{
    ctx_.trackDrag.reset(); trackDrop_.reset(); movingTracks_ = false; cursor_ = Cursor::Default; markDirty();
    if (isAlive(keyboardFocus_)) keyboardFocus_->focusLost();
    while (! heldNotes_.empty()) dispatchKeyUp(heldNotes_.back().keyCode);
    while (! heldKeys_.empty()) dispatchKeyUp(heldKeys_.back().keyCode);
}

std::vector<int> RootWidget::heldLaunchKeys() const
{
    std::vector<int> keys;
    for (const auto& held : heldNotes_) keys.push_back(held.keyCode);
    for (const auto& held : heldKeys_) keys.push_back(held.keyCode);
    return keys;
}

void RootWidget::paintOverChildren(SkCanvas& canvas, const Theme& theme)
{
    if (movingTracks_ && trackDrop_)
    {
        const auto& r = trackDrop_->indicator;
        paint::fill(canvas, r, paint::withAlpha(theme.color("SelectionBackground"), 100));
        paint::stroke(canvas, r, theme.color("SelectionFrame"), 2);
    }
    if (hoverTicks_ >= 15 && isAlive(hovered_) && ! hovered_->tooltip.empty() && ! captured_ && ! ctx_.browserDrag)
    {
        const auto anchor = hovered_->boundsInRoot();
        const float w = std::min(width(), std::max(80.0f, (float) hovered_->tooltip.size() * theme.metrics.fontSize * 0.55f + 12));
        const Rect tip{std::clamp(anchor.right() - w, 0.0f, std::max(0.0f, width() - w)),
                       std::min(height() - 20, anchor.bottom() + 5), w, 18};
        paint::fill(canvas, tip, theme.color("ControlTextBack"));
        paint::stroke(canvas, tip, theme.color("ControlContrastFrame"));
        paint::text(canvas, hovered_->tooltip, tip.reduced(5, 0), theme.color("ControlForeground"), theme.metrics.fontSize);
    }
    if (! ctx_.browserDrag) return;
    const Rect box{ctx_.browserDragX + 10, ctx_.browserDragY + 12, 150, 22};
    paint::fill(canvas, box, theme.color("SelectionBackground"));
    paint::stroke(canvas, box, theme.color("SelectionFrame"));
    paint::text(canvas, ctx_.browserDrag->label, box.reduced(5, 0), theme.color("SelectionForeground"), theme.metrics.fontSize);
}

void RootWidget::tick()
{
    if (isAlive(hovered_) && ! hovered_->tooltip.empty() && hoverTicks_ < 15)
        if (++hoverTicks_ == 15) markDirty();
}

} // namespace sonora::ui
