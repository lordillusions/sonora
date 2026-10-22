#include "ui/views/MainLayout.h"

#include "engine/Transport.h"
#include "engine/EngineGraph.h"
#include "midi/MidiManager.h"
#include "ui/Painter.h"
#include "ui/TrackMenu.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/BrowserPanel.h"
#include "ui/views/DetailView.h"
#include "ui/views/ResynthEditor.h"
#include "ui/views/PreferencesPanel.h"
#include "ui/views/CollabPanel.h"
#include "ui/views/ExportPanel.h"
#include "ui/views/SessionView.h"
#include "ui/views/StatusBar.h"
#include "ui/views/TopBar.h"
#include "ui/views/PianoRoll.h"
#include "ui/views/SampleView.h"
#include "ui/widgets/Splitter.h"

#include "include/core/SkPaint.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace sonora::ui
{

MainLayout::MainLayout(UiContext& ctx) : RootWidget(ctx)
{
    inputSong_ = padSong_ = ctx.song;
    top_ = add<TopBar>();
    browser_ = add<BrowserPanel>();
    session_ = add<SessionView>();
    arrangement_ = add<ArrangementView>();
    detail_ = add<DetailView>();
    status_ = add<StatusBar>();
    status_->detailView = detail_;
    sessionButton_ = add<IconButton>(IconButton::Icon::SessionView);
    sessionButton_->tooltip = "Session View";
    sessionButton_->isOn = [this] { return context().mainView == MainView::Session; };
    sessionButton_->onClick = [this] { context().mainView = MainView::Session; layout(); setKeyboardFocus(session_); markDirty(); };
    arrangementButton_ = add<IconButton>(IconButton::Icon::ArrangementView);
    arrangementButton_->tooltip = "Arrangement View";
    arrangementButton_->isOn = [this] { return context().mainView == MainView::Arrangement; };
    arrangementButton_->onClick = [this] { context().mainView = MainView::Arrangement; layout(); setKeyboardFocus(arrangement_); markDirty(); };
    // Dividers between the panels (Live lets every boundary be dragged). Added after the panels so they win hit tests.
    browserSplit_ = add<Splitter>(Splitter::Orientation::Vertical);
    browserSplit_->getSize = [this] { return context().theme.metrics.browserWidth; };
    browserSplit_->setSize = [this](float w) {
        context().theme.metrics.browserWidth = std::clamp(w, 140.0f, std::max(140.0f, width() * 0.6f));
        layout();
        markDirty();
    };
    browserSplit_->finished = [this] { if (context().layoutChanged) context().layoutChanged(); };
    detailSplit_ = add<Splitter>(Splitter::Orientation::Horizontal);
    detailSplit_->growDirection = -1.0f;
    detailSplit_->getSize = [this] { return context().theme.metrics.detailHeight; };
    detailSplit_->setSize = [this](float h) {
        if (compactDevices() || hasResynthEditor()) return;
        const auto& m = context().theme.metrics;
        context().theme.metrics.detailHeight = std::clamp(h, 60.0f, std::max(60.0f, height() - m.controlBarHeight - m.statusBarHeight - 120.0f));
        layout();
        markDirty();
    };
    detailSplit_->finished = [this] { if (context().layoutChanged) context().layoutChanged(); };
    prefs_ = add<PreferencesPanel>();
    collab_ = add<CollabPanel>();
    export_ = add<ExportPanel>(); // last child: drawn and hit-tested on top
}

bool MainLayout::compactDevices()
{
    auto& x = context();
    if (x.detailTab != DetailTab::Devices) return false;
    const auto* track = x.trackAt(x.selectedTrack);
    if (! track) return true;
    // UI-27 / RESYN-2: docked Wavetable and Resynth use expanded instrument editors.
    const auto expanded = [&](const model::Device& device, int index, int chain = -1, int child = -1) {
        if (device.type == "SonoraResynth") return true;
        if (device.type != "SonoraWavetable") return false;
        const auto key = x.wavetableKey(x.selectedTrack, index, chain, child);
        return key && ! x.isWavetableFloating(*key);
    };
    for (size_t i = 0; i < track->devices.size(); ++i)
    {
        const auto& device = track->devices[i];
        if (expanded(device, (int) i)) return false;
        if (device.id == x.selectedRackId && x.selectedRackChain >= 0 && (size_t) x.selectedRackChain < device.chains.size())
        {
            const auto& children = device.chains[(size_t) x.selectedRackChain].devices;
            for (size_t child = 0; child < children.size(); ++child)
                if (expanded(children[child], (int) i, x.selectedRackChain, (int) child)) return false;
        }
    }
    return true;
}

bool MainLayout::hasResynthEditor()
{
    auto& x = context();
    const auto* selected = x.trackAt(x.selectedTrack);
    if (x.detailTab != DetailTab::Devices || ! selected) return false;
    for (const auto& device : selected->devices)
    {
        if (device.type == "SonoraResynth") return true;
        if (device.id == x.selectedRackId && x.selectedRackChain >= 0 && (size_t) x.selectedRackChain < device.chains.size())
            for (const auto& child : device.chains[(size_t) x.selectedRackChain].devices)
                if (child.type == "SonoraResynth") return true;
    }
    return false;
}

void MainLayout::layout()
{
    if (context().song && padSong_ != context().song)
    {
        context().pluginPadAssignments.clear();
        padSong_ = context().song;
    }
    if (inputSong_ != context().song)
    {
        releaseMappedKeys();
        context().mapSelection.reset();
        context().midiMapMode = context().keyMapMode = false;
        if (context().midi) context().midi->cancelLearn();
        inputSong_ = context().song;
    }
    if (context().preferencesOpen || context().exportOpen || context().collabOpen || lastView_ != context().mainView) releaseMappedKeys();
    if (context().preferencesOpen || context().exportOpen || context().collabOpen)
    {
        context().browserDrag.reset();
        context().keyMapMode = context().midiMapMode = false;
        context().mapSelection.reset();
        if (context().midi) context().midi->cancelLearn();
    }
    const auto& m = context().theme.metrics;
    auto r = localBounds();
    const Rect r0 = r;
    top_->setBounds(r.removeFromTop(m.controlBarHeight));
    status_->setBounds(r.removeFromBottom(m.statusBarHeight));
    r.removeFromLeft(m.outerMargin);
    r.removeFromRight(m.outerMargin);

    detail_->visible = context().detailVisible;
    if (detail_->visible)
    {
        const bool resynth = hasResynthEditor();
        const float desiredHeight = resynth ? ResynthEditor::preferredHeight + 36.0f : compactDevices() ? 200.0f : m.detailHeight;
        detail_->setBounds(r.removeFromBottom(std::clamp(desiredHeight, 60.0f, std::max(60.0f, resynth ? r.h : r.h - 120.0f))));
        detailSplit_->setBounds({detail_->bounds().x, detail_->bounds().y - m.panelGap - 1, detail_->bounds().w, m.panelGap + 2});
        r.removeFromBottom(m.panelGap);
    }
    detailSplit_->visible = detail_->visible && ! compactDevices() && ! hasResynthEditor();

    browser_->visible = context().browserVisible;
    if (browser_->visible)
    {
        browser_->setBounds(r.removeFromLeft(std::clamp(m.browserWidth, 140.0f, std::max(140.0f, r0.w * 0.6f))));
        browserSplit_->setBounds({browser_->bounds().right() - 1, browser_->bounds().y, m.panelGap + 2, browser_->bounds().h});
        r.removeFromLeft(m.panelGap);
    }
    browserSplit_->visible = browser_->visible;

    sessionButton_->setBounds({width() - m.outerMargin - 48, 6, 22, 18});
    arrangementButton_->setBounds({width() - m.outerMargin - 24, 6, 22, 18});

    const bool sessionMode = context().mainView == MainView::Session;
    session_->visible = sessionMode;
    arrangement_->visible = ! sessionMode;
    (sessionMode ? static_cast<Widget*>(session_) : arrangement_)->setBounds(r);
    mainBounds_ = r;
    for (Widget* panel : {static_cast<Widget*>(browser_), static_cast<Widget*>(session_), static_cast<Widget*>(arrangement_),
                          static_cast<Widget*>(detail_)})
        panel->cornerRadius = m.panelRadius;

    prefs_->visible = context().preferencesOpen;
    prefs_->setBounds(r0);
    export_->visible = context().exportOpen;
    export_->setBounds(r0);
    collab_->visible = context().collabOpen && ! context().exportOpen;
    collab_->setBounds(r0);
    lastPrefs_ = context().preferencesOpen;
    lastExport_ = context().exportOpen;
    lastCollab_ = context().collabOpen;
    lastView_ = context().mainView;
    lastBrowser_ = context().browserVisible;
    lastDetail_ = context().detailVisible;
    lastTab_ = context().detailTab;
    lastCompact_ = compactDevices();
    lastResynth_ = hasResynthEditor();
}

void MainLayout::paint(SkCanvas& c, const Theme& t)
{
    paint::fill(c, localBounds(), t.color("Desktop"));
}

namespace
{
std::string describeMapping(const midi::Mapping& m)
{
    return (m.isNote ? "Note " : "CC ") + std::to_string(m.number) + "  ch " + std::to_string(m.channel);
}
} // namespace

// MIDI map mode: every mappable control gets an outline (orange = free, blue = mapped, with
// the controller number), the selected one is filled.
void MainLayout::paintOverChildren(SkCanvas& c, const Theme& t)
{
    if (context().preferencesOpen || context().exportOpen || context().collabOpen) return;
    RootWidget::paintOverChildren(c, t);
    auto& x = context();
    {
        // Live marks the focused main view with a dark 2 px rounded frame.
        SkPaint frame;
        frame.setAntiAlias(true);
        frame.setStyle(SkPaint::kStroke_Style);
        frame.setStrokeWidth(2.0f);
        frame.setColor(t.color("MainViewFocusIndicator"));
        const auto inner = mainBounds_.reduced(1.0f);
        c.drawRoundRect(inner.sk(), t.metrics.panelRadius, t.metrics.panelRadius, frame);
    }
    if (! x.midiMapMode && ! x.keyMapMode)
        return;
    const auto mappings = x.midi != nullptr ? x.midi->mappings() : std::vector<midi::Mapping>{};
    const bool learning = x.midi != nullptr && x.midi->isLearning();

    std::function<void(Widget*)> visit = [&](Widget* w) {
        if (! w->visible)
            return;
        if (! x.keyMapMode && w->midiTarget && ! w->bounds().isEmpty())
        {
            const auto r = w->boundsInRoot();
            const midi::Mapping* mapped = nullptr;
            for (const auto& m : mappings)
                if (m.keyCode == 0 && m.target == *w->midiTarget)
                    mapped = &m;
            const bool selected = x.mapSelection && *x.mapSelection == *w->midiTarget;
            const SkColor base = mapped ? t.color("ChosenAlternative") : t.color("ChosenDefault");
            paint::fill(c, r, paint::withAlpha(base, selected ? 150 : 60));
            paint::stroke(c, r, base, selected ? 2.0f : 1.0f);
            if (mapped && r.w >= 40 && r.h >= 14)
                paint::text(c, describeMapping(*mapped), r.reduced(2, 0), paint::contrastText(base), t.metrics.smallFontSize,
                            Align::Centre);
            else if (selected && learning && r.w >= 30)
                paint::text(c, "...", r, paint::contrastText(base), t.metrics.fontSize, Align::Centre);
        }
        for (auto& ch : w->children())
            visit(ch.get());
    };
    visit(this);

    auto bar = localBounds().removeFromBottom(24);
    paint::fill(c, bar, paint::withAlpha(t.color("ChosenAlternative"), 230));
    paint::text(c, x.keyMapMode ? "Key Map Mode: click a Session clip, then press a letter or number. Right-click removes. Cmd+K or Esc to leave."
                            : learning ? "MIDI Map Mode: now move a knob or press a key on your controller"
                            : "MIDI Map Mode: click a control, then move a controller. Right-click removes a mapping. Cmd+M or Esc to leave.",
                bar.reduced(10, 0), paint::contrastText(t.color("ChosenAlternative")), t.metrics.fontSize);
}

void MainLayout::tick()
{
    RootWidget::tick();
    auto& x = context();
    if (compactDevices() != lastCompact_ || hasResynthEditor() != lastResynth_ || x.detailTab != lastTab_ || x.mainView != lastView_ || x.browserVisible != lastBrowser_ || x.detailVisible != lastDetail_
        || x.preferencesOpen != lastPrefs_ || x.exportOpen != lastExport_ || x.collabOpen != lastCollab_)
    {
        layout();
        markDirty();
    }
}

bool MainLayout::keyPressed(const KeyEvent& k)
{
    auto& x = context();
    if (x.exportOpen)
        return export_->keyPressed(k) ? (layout(), markDirty(), true) : false;
    if (x.preferencesOpen)
        return prefs_->keyPressed(k) ? (layout(), markDirty(), true) : false;
    if (x.collabOpen)
        return collab_->keyPressed(k) ? (layout(), markDirty(), true) : false;
    const auto ch = k.textChar >= 'A' && k.textChar <= 'Z' ? k.textChar + ('a' - 'A') : k.textChar;
    auto focusDetail = [this](DetailTab tab) {
        context().detailVisible = true; context().detailTab = tab; layout();
        Widget* target = detail_;
        std::function<void(Widget*)> find = [&](Widget* widget) {
            if (! widget->visible) return;
            if (dynamic_cast<PianoRoll*>(widget) || dynamic_cast<SampleView*>(widget)) target = widget;
            for (const auto& child : widget->children()) find(child.get());
        };
        if (tab == DetailTab::Clip) find(detail_);
        setKeyboardFocus(target);
    };
    if (k.cmd && ! k.ctrl && ! k.alt && ! k.shift && ch == 'e' && x.mainView == MainView::Arrangement && ! x.clipEditorFocused)
    { splitArrangementSelection(x); return true; }
    if (k.cmd && ! k.ctrl && ! k.alt && ! k.shift && ch == 'j' && x.mainView == MainView::Arrangement && ! x.clipEditorFocused)
    { if (x.consolidateSelection) x.consolidateSelection(); return true; }
    if (k.cmd && k.alt && k.shift && ! k.ctrl && ch == 'f')
    { if (x.renderTrack) x.renderTrack(x.selectedTrack, true, false); return true; }
    if (k.cmd && ! k.ctrl && ! k.alt && ! k.shift && ch == 'b' && ! x.clipEditorFocused)
    { if (x.renderTrack) x.renderTrack(x.selectedTrack, false, true); return true; }
    if (k.cmd && k.ctrl && ch == 'f')
    { if (x.toggleFullScreen) x.toggleFullScreen(); return true; }
    if (! k.cmd && ! k.ctrl && (k.keyCode == juce::KeyPress::F12Key || (k.keyCode == juce::KeyPress::tabKey && k.shift && ! k.alt)))
    {
        focusDetail(x.detailTab == DetailTab::Devices ? DetailTab::Clip : DetailTab::Devices);
        markDirty(); return true;
    }
    if (k.alt && ! k.ctrl && ! k.shift && k.textChar >= '0' && k.textChar <= '5')
    {
        if (k.cmd)
        {
            if (ch == '3' || ch == '4')
            {
                const auto tab = ch == '3' ? DetailTab::Clip : DetailTab::Devices;
                const bool hide = x.detailVisible && x.detailTab == tab;
                if (! hide) focusDetail(tab); else { x.detailVisible = false; setKeyboardFocus(nullptr); }
            }
            else if (ch == '5') x.browserVisible = ! x.browserVisible;
            else return false;
        }
        else
        {
            if (ch == '0') setKeyboardFocus(top_);
            else if (ch == '1') { x.mainView = MainView::Session; setKeyboardFocus(session_); }
            else if (ch == '2') { x.mainView = MainView::Arrangement; setKeyboardFocus(arrangement_); }
            else if (ch == '3' || ch == '4') focusDetail(ch == '3' ? DetailTab::Clip : DetailTab::Devices);
            else { x.browserVisible = true; browser_->focusContent(); setKeyboardFocus(browser_); }
        }
        layout(); markDirty(); return true;
    }
    if (k.cmd && ! k.alt && ! k.ctrl && (k.textChar == '+' || k.textChar == '=' || k.textChar == '-'))
    {
        const float zoom = std::clamp(x.uiZoom + (k.textChar == '-' ? -0.1f : 0.1f), 0.8f, 2.0f);
        if (x.setUiZoom) x.setUiZoom(zoom); else x.uiZoom = zoom;
        layout(); markDirty(); return true;
    }
    if (k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (ch == '0' || (ch >= '6' && ch <= '9')) && x.song && x.applyEdit)
    {
        const int quant = ch == '0' ? 0 : ch == '6' ? 11 : ch == '7' ? 9 : ch == '8' ? 7 : 4;
        x.applyEdit(model::setGlobalQuantisation(*x.song, quant)); return true;
    }
    if (k.cmd && k.alt && ! k.shift && (ch == 'm' || ch == 's'))
    {
        if (ch == 'm') x.mixerVisible = ! x.mixerVisible; else x.sendsVisible = ! x.sendsVisible;
        if (x.layoutChanged) x.layoutChanged();
        layout(); markDirty(); return true;
    }
    if (! k.cmd && k.alt && k.shift && (ch == '1' || ch == '2'))
    {
        x.clipEnvelopesVisible = ch == '2'; focusDetail(DetailTab::Clip); markDirty(); return true;
    }
    if (k.cmd && ! k.alt && k.shift && ch == 'm')
    { if (x.insertMidiClip) x.insertMidiClip(); return true; }
    if (! k.cmd && k.alt && k.shift && ! k.ctrl && ch == 't')
    { insertEmptyTakeLane(x); layout(); markDirty(); return true; }
    if (! k.cmd && ! k.alt && ! k.ctrl && (ch == 'm' || ch == 's' || ch == 'c'))
    {
        if (ch == 'm') { releaseMappedKeys(); x.computerKeyboard = ! x.computerKeyboard; }
        else if (x.controls && x.selectedTrack >= 0 && (size_t) x.selectedTrack < x.controls->size())
        {
            auto& controls = (*x.controls)[(size_t) x.selectedTrack];
            if (ch == 's') toggleTrackSolo(x, x.selectedTrack);
            else if (x.armTrack) x.armTrack(x.selectedTrack, ! controls.armed.load());
        }
        markDirty(); return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && k.textChar == '0' && x.song)
    {
        if (const auto* clip = x.selectedClip(); clip && x.applyEdit)
        { auto changed = *clip; changed.disabled = ! changed.disabled; x.applyEdit(model::modifyClip(*x.song, x.selectedClipLocation(), std::move(changed), "Activate/Deactivate Clip")); }
        else if (x.controls && x.selectedTrack >= 0 && (size_t) x.selectedTrack < x.controls->size())
        { auto& active = (*x.controls)[(size_t) x.selectedTrack].active; active.store(! active.load()); }
        markDirty(); return true;
    }
    if (k.cmd && k.alt && ! k.shift && ! k.ctrl && ch == 'u')
    {
        x.mainView = MainView::Arrangement;
        const bool handled = arrangement_->keyPressed(k); layout(); markDirty(); return handled;
    }
    if (k.cmd && k.shift && ! k.alt && ! k.ctrl && (k.textChar == 'r' || k.textChar == 'R') && x.song != nullptr && x.startExport)
    {
        x.exportOpen = true; // Export Audio, like Live's Shift+Cmd+R
        return true;
    }
    if (! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && x.sessionTrackSelection
        && (k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey))
    {
        for (auto* focus = keyboardFocus(); focus; focus = focus->parent())
            if (focus == detail_ || focus == browser_ || focus == top_) return false;
        deleteTrackSelection(x); return true;
    }
    if (k.cmd && ! k.alt && ! k.ctrl && ! k.shift && ch == 'd' && x.sessionTrackSelection && x.selectedClipUid < 0 && x.selectedTakeLane < 0)
    {
        for (auto* focus = keyboardFocus(); focus; focus = focus->parent())
            if (focus == detail_ || focus == browser_ || focus == top_) return false;
        if (x.duplicateSelectedTracks) x.duplicateSelectedTracks();
        return true;
    }
    if (k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 'z' || k.textChar == 'Z'))
    {
        if (k.shift ? x.redo != nullptr : x.undo != nullptr)
            (k.shift ? x.redo : x.undo)();
    }
    else if (k.cmd && (k.textChar == 'n' || k.textChar == 'N') && ! k.alt && ! k.ctrl && ! k.shift && x.newProject)
    {
        x.newProject();
    }
    else if (k.cmd && k.alt && ! k.shift && ! k.ctrl && (k.textChar == 't' || k.textChar == 'T' || k.keyCode == 'T' || k.keyCode == 't')
             && x.insertReturnTrack)
    {
        x.insertReturnTrack();
    }
    else if (k.cmd && ! k.alt && (k.textChar == 't' || k.textChar == 'T') && x.insertTrack)
    {
        x.insertTrack(k.shift);
    }
    else if (k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (k.textChar == 'r' || k.textChar == 'R') && x.renameSelection)
    {
        x.renameSelection();
    }
    else if (k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 's' || k.textChar == 'S') && x.saveFile)
    {
        if (k.shift && x.saveFileAs)
            x.saveFileAs();
        else
            x.saveFile();
    }
    else if (k.cmd && (k.textChar == 'm' || k.textChar == 'M') && ! k.alt && ! k.shift)
    {
        releaseMappedKeys();
        x.midiMapMode = ! x.midiMapMode;
        x.keyMapMode = false;
        x.mapSelection.reset();
        if (! x.midiMapMode && x.midi != nullptr)
            x.midi->cancelLearn();
    }
    else if (k.cmd && (k.textChar == 'k' || k.textChar == 'K') && ! k.alt && ! k.shift)
    {
        releaseMappedKeys();
        x.keyMapMode = ! x.keyMapMode;
        x.midiMapMode = false;
        x.mapSelection.reset();
        if (x.midi) x.midi->cancelLearn();
    }
    else if ((x.midiMapMode || x.keyMapMode) && k.keyCode == juce::KeyPress::escapeKey)
    {
        x.midiMapMode = x.keyMapMode = false;
        x.mapSelection.reset();
        if (x.midi != nullptr)
            x.midi->cancelLearn();
    }
    else if ((x.midiMapMode || x.keyMapMode) && (k.keyCode == juce::KeyPress::deleteKey || k.keyCode == juce::KeyPress::backspaceKey))
    {
        if (x.mapSelection && x.midi != nullptr)
            x.midi->removeMapping(*x.mapSelection, x.keyMapMode);
    }
    else if (k.cmd && ! k.alt && ! k.ctrl && ! k.shift && k.textChar == ',')
    {
        x.preferencesOpen = true;
    }
    else if (k.keyCode == juce::KeyPress::tabKey && ! k.cmd && ! k.alt && ! k.ctrl)
    {
        x.mainView = x.mainView == MainView::Session ? MainView::Arrangement : MainView::Session;
    }
    else if (k.cmd && ! k.alt && ! k.ctrl && ! k.shift && (k.textChar == 'f' || k.textChar == 'F'))
    {
        browser_->focusSearch();
    }
    else if (! k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 'b' || k.textChar == 'B') && (! x.computerKeyboard || k.shift))
    {
        x.drawMode = ! x.drawMode; // Draw Mode, like the pencil button in the control bar
    }
    else if (k.cmd && ! k.alt && ! k.shift && (k.textChar == 'l' || k.textChar == 'L'))
    {
        if (x.song && x.applyEdit && x.mainView == MainView::Arrangement && x.arrangementSelection.active)
        {
            x.applyEdit(model::setLoop(*x.song, true, x.arrangementSelection.start, x.arrangementSelection.end - x.arrangementSelection.start));
            return true;
        }
        // Loop Selection: the loop brace becomes the selected Arrangement clip, and looping is switched on.
        const auto* clip = x.song != nullptr && x.selectedClipScene < 0 ? x.selectedClip() : nullptr;
        if (clip != nullptr && x.applyEdit && clip->currentEnd > clip->currentStart)
            x.applyEdit(model::setLoop(*x.song, true, clip->currentStart, clip->currentEnd - clip->currentStart));
        else
            x.statusText = "Select a clip, notes or a take range first, then Loop Selection (Cmd+L)";
    }
    else if (k.cmd && ! k.alt && ! k.ctrl && (k.textChar == 'g' || k.textChar == 'G'))
    {
        for (auto* focus = keyboardFocus(); focus; focus = focus->parent())
            if (focus == detail_ || focus == browser_) return true;
        if (k.shift) { if (x.ungroupSelectedTracks) x.ungroupSelectedTracks(); }
        else if (x.groupSelectedTracks) x.groupSelectedTracks();
    }
    else if (k.keyCode == juce::KeyPress::F9Key)
    {
        if (k.cmd && k.shift && x.toggleSessionRecording) x.toggleSessionRecording();
        else if (! k.cmd && ! k.alt && ! k.shift && x.toggleRecording) x.toggleRecording();
    }
    else if (k.keyCode == juce::KeyPress::spaceKey)
    {
        if (x.transport)
        {
            if (k.shift && ! k.cmd && ! k.alt) x.transport->play();
            else if (! k.cmd && ! k.alt && ! k.ctrl) x.transport->togglePlay();
        }
    }
    else if (k.cmd && k.alt && (k.textChar == 'i' || k.textChar == 'I'))
    {
        x.ioVisible = ! x.ioVisible;
    }
    else if (k.cmd && k.alt && (k.textChar == 'b' || k.textChar == 'B'))
    {
        x.browserVisible = ! x.browserVisible;
    }
    else if (k.cmd && k.alt && (k.textChar == 'l' || k.textChar == 'L'))
    {
        x.detailVisible = ! x.detailVisible;
    }
    else if (k.keyCode == juce::KeyPress::homeKey)
    {
        if (x.transport)
            x.transport->setPosition(0.0);
    }
    else if (k.keyCode == juce::KeyPress::returnKey && ! k.cmd && ! k.alt && ! k.ctrl && ! k.shift && x.mainView == MainView::Arrangement)
    {
        // The loop brace is always present (drawn dimmed when the switch is off), so Return always goes to its start.
        if (x.transport)
            x.transport->setPosition(x.transport->loopStart());
    }
    else if (! k.cmd && ! k.alt && ! k.ctrl && (k.keyCode == juce::KeyPress::upKey || k.keyCode == juce::KeyPress::downKey))
    {
        if (x.selectedTakeLane >= 0) x.selectedTakeLane = x.selectedClipTrack = x.selectedClipUid = x.selectedClipScene = -1;
        const int dir = k.keyCode == juce::KeyPress::upKey ? -1 : 1;
        x.selectedTrack = std::clamp(x.selectedTrack + dir, 0, std::max(0, x.numControlTracks() - 1));
    }
    else
    {
        return false;
    }
    layout();
    markDirty();
    return true;
}

} // namespace sonora::ui
