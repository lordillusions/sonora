#include "ui/views/TopBar.h"
#include "ui/views/CollabPanel.h"

#include "engine/Transport.h"
#include "ui/Controls.h"
#include "ui/Painter.h"
#include "ui/RootWidget.h"
#include "ui/WheelScale.h"
#include "ui/UiContext.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>

namespace sonora::ui
{

namespace
{
// "bar. beat. sixteenth" like Live's arrangement position display (1-based).
std::string formatPosition(double beats, int num, int den)
{
    const double beatsPerBar = num * 4.0 / den;
    const double qn = 4.0 / den; // quarter notes per beat
    const int bar = (int) std::floor(beats / beatsPerBar);
    const double inBar = beats - bar * beatsPerBar;
    const int beat = (int) std::floor(inBar / qn);
    const int six = (int) std::floor((inBar - beat * qn) / 0.25);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d. %d. %d", bar < 0 ? bar : bar + 1, beat + 1, six + 1);
    return buf;
}

// "bars. beats. sixteenths" of a length (zero based, like Live's loop length field).
std::string formatLength(double beats, int num, int den)
{
    const double beatsPerBar = num * 4.0 / den, qn = 4.0 / den;
    const int bars = (int) std::floor(beats / beatsPerBar + 1e-9);
    const double inBar = beats - bars * beatsPerBar;
    const int whole = (int) std::floor(inBar / qn + 1e-9);
    const int six = (int) std::floor((inBar - whole * qn) / 0.25 + 1e-6);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d. %d. %d", bars, whole, six);
    return buf;
}

void displayBox(SkCanvas& c, const Theme& t, const Rect& r, const std::string& text, bool accent = false)
{
    ctl::barBox(c, t, r, text, Align::Centre, accent, t.color("ChosenAlternative"));
}
} // namespace

TopBar::TopBar()
{
    play_ = add<IconButton>(IconButton::Icon::Play);
    play_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TransportPlay, 0, 0, 0, {}};
    play_->isOn = [this] { return ctx().transport && (ctx().transport->isPlaying() || ctx().transport->isCountingIn()); };
    play_->onClick = [this] { if (ctx().transport) ctx().transport->play(); };

    stop_ = add<IconButton>(IconButton::Icon::Stop);
    stop_->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::TransportStop, 0, 0, 0, {}};
    stop_->onClick = [this] {
        if (auto* tr = ctx().transport)
        {
            // Live: first click stops, second click returns to the start.
            if (! tr->isPlaying() && ! tr->isCountingIn())
                tr->setPosition(0.0);
            tr->stop();
        }
    };

    record_ = add<IconButton>(IconButton::Icon::Record);
    record_->isOn = [this] {
        if (ctx().transport && ctx().transport->isCountingIn())
        {
            const double tick = 4.0 / std::max(1, ctx().transport->timeSigDenominator());
            return std::fmod(-ctx().transport->countInPosition(), tick) < tick * 0.5;
        }
        const int phase = ctx().sessionRecordingPhase ? ctx().sessionRecordingPhase(-1) : 0;
        if (ctx().recording && (phase == 1 || phase == 3)) return (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
        return ctx().recording || ctx().recordingRequested;
    };
    record_->onClick = [this] { if (ctx().toggleRecording) ctx().toggleRecording(); repaint(); };

    loop_ = add<IconButton>(IconButton::Icon::Loop);
    loop_->isOn = [this] { return ctx().transport && ctx().transport->loopEnabled(); };
    loop_->onClick = [this] {
        if (auto* tr = ctx().transport)
        {
            if (ctx().song && ctx().applyEdit) ctx().applyEdit(model::setLoop(*ctx().song, ! tr->loopEnabled(), tr->loopStart(), tr->loopLength()));
            else tr->setLoopEnabled(! tr->loopEnabled());
        }
    };

    metronome_ = add<IconButton>(IconButton::Icon::Metronome);
    metronome_->isOn = [this] { return ctx().transport && ctx().transport->metronomeOn(); };
    metronome_->onClick = [this] {
        if (auto* tr = ctx().transport)
            tr->setMetronome(! tr->metronomeOn());
    };

    countIn_ = add<TextButton>("Count: —");
    countIn_->barStyle = true;
    countIn_->isOn = [this] { return ctx().countInBars > 0; };
    countIn_->onClick = [this] {
        auto& x = ctx();
        const int bars = x.countInBars == 0 ? 1 : x.countInBars == 1 ? 2 : x.countInBars == 2 ? 4 : 0;
        x.countInBars = bars;
        if (x.setCountInBars) x.setCountInBars(bars);
        tick(); repaint();
    };
    punchIn_ = add<IconButton>(IconButton::Icon::PunchIn);
    punchOut_ = add<IconButton>(IconButton::Icon::PunchOut);
    punchIn_->isOn = [this] { return ctx().song && ctx().song->punchIn; };
    punchOut_->isOn = [this] { return ctx().song && ctx().song->punchOut; };
    punchIn_->onClick = [this] {
        auto& x = ctx();
        if (x.song && x.applyEdit) x.applyEdit(model::setPunch(*x.song, ! x.song->punchIn, x.song->punchOut));
        repaint();
    };
    punchOut_->onClick = [this] {
        auto& x = ctx();
        if (x.song && x.applyEdit) x.applyEdit(model::setPunch(*x.song, x.song->punchIn, ! x.song->punchOut));
        repaint();
    };

    browserToggle_ = add<IconButton>(IconButton::Icon::Browser);
    browserToggle_->isOn = [this] { return ctx().browserVisible; };
    browserToggle_->onClick = [this] { ctx().browserVisible = ! ctx().browserVisible; };

    // ---- Live's tools
    overdub_ = add<IconButton>(IconButton::Icon::Overdub);
    overdub_->isOn = [this] { return ctx().arrangementOverdub; };
    overdub_->onClick = [this] { ctx().arrangementOverdub = ! ctx().arrangementOverdub; if (ctx().layoutChanged) ctx().layoutChanged(); ctx().statusText = ctx().arrangementOverdub ? "MIDI Arrangement Overdub on: new MIDI takes merge into the clips they overlap" : "MIDI Arrangement Overdub off: new takes replace what they overlap"; };
    automationArm_ = add<IconButton>(IconButton::Icon::AutomationArm);
    automationArm_->isOn = [this] { return ctx().automationArm; };
    automationArm_->onClick = [this] { ctx().automationArm = ! ctx().automationArm; if (ctx().layoutChanged) ctx().layoutChanged(); ctx().statusText = ctx().automationArm ? "Automation Arm on: moving controls while recording writes automation" : "Automation Arm off"; };
    reEnable_ = add<IconButton>(IconButton::Icon::ReEnable);
    reEnable_->isOn = [this] { return ctx().automationOverridden && ctx().automationOverridden(); };
    reEnable_->isEnabled = [this] { return ctx().automationOverridden && ctx().automationOverridden(); };
    reEnable_->onClick = [this] { if (ctx().reEnableAutomation) ctx().reEnableAutomation(); ctx().statusText = "Automation re-enabled"; repaint(); };
    capture_ = add<IconButton>(IconButton::Icon::Capture);
    // Waiting MIDI wins; otherwise the button captures the armed audio inputs (TRK-4).
    capture_->isEnabled = [this] {
        return (ctx().captureMidiAvailable && ctx().captureMidiAvailable()) || (ctx().captureAudioAvailable && ctx().captureAudioAvailable());
    };
    capture_->onClick = [this] {
        if (ctx().captureMidiAvailable && ctx().captureMidiAvailable()) { if (ctx().captureMidi) ctx().captureMidi(); }
        else if (ctx().captureAudio) ctx().captureAudio();
        repaint();
    };
    sessionRecord_ = add<IconButton>(IconButton::Icon::SessionRecord);
    sessionRecord_->isOn = [this] { return (ctx().recording || ctx().recordingRequested) && ctx().recordingIsSession; };
    sessionRecord_->onClick = [this] { if (ctx().toggleSessionRecording) ctx().toggleSessionRecording(); repaint(); };
    drawMode_ = add<IconButton>(IconButton::Icon::Draw);
    drawMode_->isOn = [this] { return ctx().drawMode; };
    drawMode_->onClick = [this] { ctx().drawMode = ! ctx().drawMode; ctx().statusText = ctx().drawMode ? "Draw Mode on (B): drag in the note editor or an envelope to paint" : "Draw Mode off"; };
    keyboard_ = add<IconButton>(IconButton::Icon::Keyboard);
    keyboard_->isOn = [this] { return ctx().computerKeyboard; };
    keyboard_->onClick = [this] {
        auto& x = ctx();
        x.computerKeyboard = ! x.computerKeyboard;
        if (x.computerKeyboard) x.keyMapMode = false; // letters are either notes or Session clip keys
        x.statusText = x.computerKeyboard ? "Computer MIDI Keyboard on: A W S E D F T G Y H U J K play notes, Z/X octave, C/V velocity" : "Computer MIDI Keyboard off";
    };
    keyMap_ = add<TextButton>("Key");
    keyMap_->barStyle = true;
    keyMap_->isOn = [this] { return ctx().keyMapMode; };
    keyMap_->onClick = [this] { pressKey('K'); };
    keyMap_->bypassMapMode = keyboard_->bypassMapMode = true;
    struct ClickArea : Widget
    {
        std::function<void()> onClick;
        void mouseDown(const MouseEvent&) override { if (onClick) onClick(); }
    };
    auto* area = add<ClickArea>();
    area->bypassMapMode = true;
    area->onClick = [this] { pressKey('M'); };
    midiHit_ = area;
    scale_ = add<TextButton>("♭#");
    scale_->barStyle = true;
    scale_->tooltip = "Highlight Scale";
    scale_->isOn = [this] { return ctx().scaleHighlight; };
    scale_->onClick = [this] { ctx().scaleHighlight = ! ctx().scaleHighlight; repaint(); };
    collab_ = add<CollabBadge>(); // last: tests address the other children by creation order
}

void TopBar::pressKey(char letter)
{
    // The Key / MIDI map buttons act like Cmd+K / Cmd+M so there is a single implementation of the modes.
    Widget* top = this;
    while (top->parent() != nullptr) top = top->parent();
    if (auto* root = dynamic_cast<RootWidget*>(top))
        root->dispatchKey(KeyEvent{.keyCode = letter, .textChar = (uint32_t) letter, .cmd = true});
}

void TopBar::layout()
{
    if (lastCountInBars_ != ctx().countInBars)
    {
        lastCountInBars_ = ctx().countInBars;
        countIn_->setLabel(ctx().countInBars == 0 ? "Count: —" : "Count: " + std::to_string(ctx().countInBars));
    }
    // Items are 18 px tall, vertically centred in the 30 px above the gap to the panels.
    const auto& m = ctx().theme.metrics;
    Rect r{m.outerMargin, 6, width() - 2 * m.outerMargin - 54, 18};
    const float h = r.h;
    // Narrow windows drop the least important items (tap tempo, count-in, CPU/MIDI readouts, then the tool groups).
    const bool compact = width() < 1100;
    const bool tight = width() < 1000;
    const float boxW = tight ? 64.0f : 78.0f;
    browserToggle_->setBounds(r.removeFromLeft(26));
    r.removeFromLeft(10);
    tapBox_ = compact ? Rect{} : r.removeFromLeft(36);
    if (! compact)
        r.removeFromLeft(2);
    tempoBox_ = r.removeFromLeft(54);
    r.removeFromLeft(2);
    sigBox_ = r.removeFromLeft(46);
    r.removeFromLeft(2);
    metronome_->setBounds(r.removeFromLeft(34));
    r.removeFromLeft(2);
    countIn_->visible = ! compact;
    if (! compact)
        countIn_->setBounds(r.removeFromLeft(78));
    scale_->visible = width() >= 1200;
    scaleRootBox_ = scaleModeBox_ = {};
    if (scale_->visible)
    {
        r.removeFromLeft(6);
        scale_->setBounds(r.removeFromLeft(22)); r.removeFromLeft(2);
        scaleRootBox_ = r.removeFromLeft(38); r.removeFromLeft(2);
        scaleModeBox_ = r.removeFromLeft(68);
    }

    // Right side: view switch | cpu | midi | people, then the tools and the loop group to their left.
    midiBox_ = compact ? Rect{} : r.removeFromRight(40);
    midiHit_->visible = ! compact;
    if (! compact)
        midiHit_->setBounds(midiBox_);
    if (! compact)
        r.removeFromRight(2);
    cpuBox_ = compact ? Rect{} : r.removeFromRight(50);
    r.removeFromRight(8);
    lastCollabWidth_ = collab_->preferredWidth();
    collab_->setBounds(r.removeFromRight(lastCollabWidth_));
    r.removeFromRight(10);
    // draw mode | computer keyboard | key map (Live puts them right of the loop group)
    drawMode_->visible = keyboard_->visible = keyMap_->visible = ! tight;
    if (! tight)
    {
        keyMap_->setBounds(r.removeFromRight(32));
        r.removeFromRight(2);
        keyboard_->setBounds(r.removeFromRight(26));
        r.removeFromRight(2);
        drawMode_->setBounds(r.removeFromRight(26));
        r.removeFromRight(8);
    }
    r.removeFromRight(6);
    loopLenBox_ = r.removeFromRight(boxW);
    r.removeFromRight(2);
    punchOut_->setBounds(r.removeFromRight(28));
    r.removeFromRight(2);
    loop_->setBounds(r.removeFromRight(h + 8));
    r.removeFromRight(2);
    punchIn_->setBounds(r.removeFromRight(28));
    r.removeFromRight(2);
    loopStartBox_ = r.removeFromRight(boxW);

    // Transport + the five tools (+, automation arm, re-enable, capture, session record) between the left controls and the loop group.
    const float tw = 24, toolW = 22;
    const int tools = tight ? 2 : 5;
    const float positionW = tight ? 76.0f : 88.0f;
    const float centreW = positionW + 10 + 3 * (tw + 4) + 8 + (float) tools * (toolW + 3);
    const float left = r.x + 6, right = std::max(left, r.right() - centreW - 6);
    Rect centre{std::clamp(localBounds().centreX() - centreW * 0.5f, left, right), r.y, centreW, h};
    positionBox_ = centre.removeFromLeft(positionW);
    centre.removeFromLeft(10);
    play_->setBounds(centre.removeFromLeft(tw));
    centre.removeFromLeft(4);
    stop_->setBounds(centre.removeFromLeft(tw));
    centre.removeFromLeft(4);
    record_->setBounds(centre.removeFromLeft(tw));
    centre.removeFromLeft(8);
    overdub_->visible = automationArm_->visible = reEnable_->visible = ! tight;
    if (! tight)
    {
        overdub_->setBounds(centre.removeFromLeft(toolW)); centre.removeFromLeft(3);
        automationArm_->setBounds(centre.removeFromLeft(toolW)); centre.removeFromLeft(3);
        reEnable_->setBounds(centre.removeFromLeft(toolW)); centre.removeFromLeft(3);
    }
    capture_->setBounds(centre.removeFromLeft(toolW)); centre.removeFromLeft(3);
    sessionRecord_->setBounds(centre.removeFromLeft(toolW));
}

void TopBar::paint(SkCanvas& c, const Theme& t)
{
    // The control bar has no surface of its own: items sit on the Desktop colour.
    auto& x = ctx();
    static const char* roots[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (! scaleRootBox_.isEmpty())
    {
        ctl::dropdown(c, t, scaleRootBox_, roots[x.scaleRoot()]);
        ctl::dropdown(c, t, scaleModeBox_, std::string(model::scaleAt(x.scaleName()).name));
    }
    ctl::barBox(c, t, tapBox_, "Tap");
    const double tempo = x.transport ? x.transport->tempo() : (x.song ? x.song->tempo : 120.0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", tempo);
    if (editingTempo_)
        displayBox(c, t, tempoBox_, tempoText_.empty() ? std::string("_") : tempoText_ + "_", true);
    else
        displayBox(c, t, tempoBox_, buf);

    const int num = x.song ? x.song->timeSigNumerator : 4, den = x.song ? x.song->timeSigDenominator : 4;
    std::snprintf(buf, sizeof(buf), "%d / %d", draggingSignature_ ? signaturePreviewNum_ : num, draggingSignature_ ? signaturePreviewDen_ : den);
    std::string signatureDisplay = buf;
    if (editingSignature_)
        signatureDisplay = signatureText_.find_first_of("/,. ") != std::string::npos ? signatureText_ + "|"
                           : signatureDenominator_ ? std::to_string(num) + " / " + signatureText_ + "|"
                                                   : signatureText_ + "| / " + std::to_string(den);
    displayBox(c, t, sigBox_, signatureDisplay, editingSignature_);

    const bool counting = x.transport && x.transport->isCountingIn();
    const double pos = x.transport ? (counting ? x.transport->countInPosition() : x.transport->positionBeats()) : 0.0;
    displayBox(c, t, positionBox_, formatPosition(pos, num, den), counting);

    const double ls = x.transport ? x.transport->loopStart() : (x.song ? x.song->loopStart : 0.0);
    const double ll = x.transport ? x.transport->loopLength() : (x.song ? x.song->loopLength : 16.0);
    displayBox(c, t, loopStartBox_, formatPosition(ls, num, den));
    displayBox(c, t, loopLenBox_, formatLength(ll, num, den));

    std::snprintf(buf, sizeof(buf), "%d %%", (int) std::lround(x.cpuUsage * 100.0));
    ctl::barBox(c, t, cpuBox_, buf);
    ctl::barBox(c, t, midiBox_, "MIDI", Align::Centre, x.midiMapMode || x.midiActivityFrames > 0,
                x.midiMapMode ? t.color("ChosenAlternative") : t.color("ChosenDefault"));
}

void TopBar::tick()
{
    auto& x = ctx();
    if (std::abs(collab_->preferredWidth() - lastCollabWidth_) > 0.5f || (int) x.collab.state != lastCollabState_ || x.collab.participants.size() != lastCollabPeople_)
    {
        lastCollabState_ = (int) x.collab.state;
        lastCollabPeople_ = x.collab.participants.size();
        layout();
        repaint();
    }
    if (lastCountInBars_ != x.countInBars)
    {
        lastCountInBars_ = x.countInBars;
        countIn_->setLabel(x.countInBars == 0 ? "Count: —" : "Count: " + std::to_string(x.countInBars));
        repaint();
    }
    if (x.midiActivityFrames > 0)
    {
        --x.midiActivityFrames;
        repaint();
    }
    const bool automationOverride = x.automationOverridden && x.automationOverridden();
    const bool canCapture = (x.captureMidiAvailable && x.captureMidiAvailable()) || (x.captureAudioAvailable && x.captureAudioAvailable());
    if (automationOverride != lastOverridden_ || canCapture != lastCapture_)
    {
        lastOverridden_ = automationOverride;
        lastCapture_ = canCapture;
        repaint();
    }
    const double pos = x.transport ? (x.transport->isCountingIn() ? x.transport->countInPosition() : x.transport->positionBeats()) : 0.0;
    const int cpu = (int) std::lround(x.cpuUsage * 100.0);
    if (std::abs(pos - lastPosition_) > 0.01 || cpu != lastCpu_)
    {
        lastPosition_ = pos;
        lastCpu_ = cpu;
        repaint();
    }
}

void TopBar::mouseUp(const MouseEvent&)
{
    auto& x = ctx();
    if (draggingSignature_)
    {
        draggingSignature_ = false;
        if (ctx().song == signatureSong_) commitSignature(signaturePreviewNum_, signaturePreviewDen_);
        repaint(); return;
    }
    if (loopField_ != LoopField::None)
    {
        // The transport already follows the drag; commit it as one undoable edit.
        if (loopDragged_ && x.transport != nullptr && x.song != nullptr && x.applyEdit)
            x.applyEdit(model::setLoop(*x.song, x.transport->loopEnabled(), x.transport->loopStart(), x.transport->loopLength()));
        loopField_ = LoopField::None;
        loopDragged_ = false;
        return;
    }
    // Commit a tempo drag as one undoable edit (the transport already follows it live).
    if (draggedTempo_ && x.transport != nullptr && x.song != nullptr && x.applyEdit
        && std::abs(x.transport->tempo() - x.song->tempo) > 1e-9)
        x.applyEdit(model::setTempo(*x.song, x.transport->tempo()));
    draggedTempo_ = false;
}

void TopBar::mouseDown(const MouseEvent& e)
{
    if (e.right) return;
    if (sigBox_.contains(e.x, e.y) && ctx().song && ! ctx().recording && ! ctx().recordingRequested)
    {
        editingSignature_ = false; draggingSignature_ = true;
        signatureSong_ = ctx().song;
        signatureDenominator_ = e.x >= sigBox_.centreX(); dragStartY_ = e.y;
        signatureBeforeNum_ = signaturePreviewNum_ = ctx().song->timeSigNumerator;
        signatureBeforeDen_ = signaturePreviewDen_ = ctx().song->timeSigDenominator;
        return;
    }
    editingSignature_ = false;
    if (scaleRootBox_.contains(e.x, e.y) || scaleModeBox_.contains(e.x, e.y))
    {
        const bool root = scaleRootBox_.contains(e.x, e.y);
        std::vector<std::string> choices = root ? std::vector<std::string>{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}
                                                : std::vector<std::string>{};
        if (! root) for (const auto& scale : model::kScales) choices.emplace_back(scale.name);
        const int current = root ? ctx().scaleRoot() : std::clamp(ctx().scaleName(), 0, (int) model::kScales.size() - 1);
        auto pick = [this, root, count = (int) choices.size()](int choice) {
            if (! Widget::isAlive(this) || choice < 0 || choice >= count) return;
            if (root) ctx().setScale(choice, ctx().scaleName()); else ctx().setScale(ctx().scaleRoot(), choice);
            repaint();
        };
        std::vector<bool> ticked(choices.size(), false);
        ticked[(size_t) current] = true;
        if (ctx().showChoices) ctx().showChoices(choices, ticked, pick);
        else
        {
            juce::PopupMenu menu;
            for (size_t i = 0; i < choices.size(); ++i) menu.addItem((int) i + 1, choices[i], true, (int) i == current);
            menu.showMenuAsync(juce::PopupMenu::Options(), [pick](int choice) { if (choice > 0) pick(choice - 1); });
        }
        return;
    }
    if (tapBox_.contains(e.x, e.y) && ctx().transport != nullptr)
    {
        // Tap tempo: average the last few taps, restarting after a pause of more than two seconds.
        const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        if (! taps_.empty() && now - taps_.back() > 2.0)
            taps_.clear();
        taps_.push_back(now);
        if (taps_.size() > 5)
            taps_.erase(taps_.begin());
        if (taps_.size() >= 2)
        {
            const double bpm = 60.0 * (double) (taps_.size() - 1) / (taps_.back() - taps_.front());
            if (bpm >= 20.0 && bpm <= 999.0)
            {
                ctx().transport->setTempo(std::round(bpm * 100.0) / 100.0);
                draggedTempo_ = true;
            }
        }
        repaint();
        return;
    }
    loopField_ = LoopField::None;
    if ((loopStartBox_.contains(e.x, e.y) || loopLenBox_.contains(e.x, e.y)) && ctx().transport != nullptr && ! ctx().recording)
    {
        // Live's three-part fields: the left third drags bars, the middle beats, the right sixteenths.
        const bool start = loopStartBox_.contains(e.x, e.y);
        const Rect& box = start ? loopStartBox_ : loopLenBox_;
        const float third = (e.x - box.x) / box.w * 3.0f;
        const int num = ctx().song ? ctx().song->timeSigNumerator : 4, den = ctx().song ? ctx().song->timeSigDenominator : 4;
        loopUnit_ = third < 1.0f ? num * 4.0 / den : third < 2.0f ? 4.0 / den : 0.25;
        loopField_ = start ? LoopField::Start : LoopField::Length;
        loopStartBefore_ = ctx().transport->loopStart();
        loopLengthBefore_ = ctx().transport->loopLength();
        dragStartY_ = e.y;
        loopDragged_ = false;
        return;
    }
    dragStartY_ = e.y;
    draggedTempo_ = false;
    dragStartTempo_ = ctx().transport ? ctx().transport->tempo() : 120.0;
}

void TopBar::mouseDrag(const MouseEvent& e)
{
    if (draggingSignature_)
    {
        const int steps = (int) std::round((dragStartY_ - e.y) / 6);
        if (signatureDenominator_)
            signaturePreviewDen_ = 1 << std::clamp((int) std::lround(std::log2(signatureBeforeDen_)) + steps, 0, 4);
        else signaturePreviewNum_ = std::clamp(signatureBeforeNum_ + steps, 1, 99);
        repaint(); return;
    }
    if (loopField_ != LoopField::None && ctx().transport != nullptr)
    {
        // 6 px per step; up increases
        const double steps = std::round((dragStartY_ - e.y) / 6.0) * loopUnit_;
        auto* tr = ctx().transport;
        double start = loopStartBefore_, length = loopLengthBefore_;
        if (loopField_ == LoopField::Start)
            start = std::max(0.0, loopStartBefore_ + steps);
        else
            length = std::max(0.25, loopLengthBefore_ + steps);
        if (std::fabs(start - tr->loopStart()) > 1e-9 || std::fabs(length - tr->loopLength()) > 1e-9)
        {
            tr->setLoop(tr->loopEnabled(), start, length);
            loopDragged_ = true;
            repaint();
        }
        return;
    }
    // Dragging the tempo box changes BPM (1 BPM per 2 px, Shift = 0.01 steps).
    if (! tempoBox_.contains(e.x, dragStartY_) || ctx().transport == nullptr)
        return;
    const double delta = (dragStartY_ - e.y) * (e.shift ? 0.01 : 0.5);
    ctx().transport->setTempo(std::round((dragStartTempo_ + delta) * 100.0) / 100.0);
    draggedTempo_ = true;
    repaint();
}

void TopBar::commitTempo(double bpm)
{
    auto& x = ctx();
    bpm = std::clamp(std::round(bpm * 100.0) / 100.0, 20.0, 999.0);
    if (x.transport != nullptr)
        x.transport->setTempo(bpm);
    if (x.song != nullptr && x.applyEdit && std::abs(x.song->tempo - bpm) > 1e-9)
        x.applyEdit(model::setTempo(*x.song, bpm));
}

void TopBar::commitSignature(int numerator, int denominator)
{
    auto& x = ctx();
    if (! x.song || ! x.applyEdit || x.recording || x.recordingRequested) return;
    auto command = model::setTimeSignature(*x.song, numerator, denominator);
    if (! command.apply) return;
    x.applyEdit(std::move(command));
    if (x.transport) x.transport->setTimeSignature(x.song->timeSigNumerator, x.song->timeSigDenominator);
    if (x.modelChanged) x.modelChanged();
    repaint();
}

void TopBar::mouseDoubleClick(const MouseEvent& e)
{
    if (e.right) return;
    if (sigBox_.contains(e.x, e.y) && ctx().song)
    {
        editingTempo_ = draggingSignature_ = false; editingSignature_ = signatureSelected_ = true;
        signatureSong_ = ctx().song;
        signatureDenominator_ = e.x >= sigBox_.centreX();
        signatureText_ = std::to_string(signatureDenominator_ ? ctx().song->timeSigDenominator : ctx().song->timeSigNumerator);
        repaint(); return;
    }
    if (tempoBox_.contains(e.x, e.y) && ctx().song != nullptr)
    {
        editingSignature_ = false;
        editingTempo_ = true;
        tempoText_.clear(); // typing replaces the value
        repaint();
        return;
    }
    if (positionBox_.contains(e.x, e.y) && ctx().transport)
        ctx().transport->setPosition(0.0);
    repaint();
}

bool TopBar::mouseWheel(const MouseEvent& e)
{
    if (sigBox_.contains(e.x, e.y) && ctx().song && std::abs(e.wheelDeltaY) > 1e-8f)
    {
        const int step = e.wheelDeltaY > 0 ? 1 : -1;
        const int numerator = ctx().song->timeSigNumerator, denominator = ctx().song->timeSigDenominator;
        if (e.x < sigBox_.centreX()) commitSignature(std::clamp(numerator + step, 1, 99), denominator);
        else commitSignature(numerator, 1 << std::clamp((int) std::lround(std::log2(denominator)) + step, 0, 4));
        return true;
    }
    if (! tempoBox_.contains(e.x, e.y) || ctx().transport == nullptr)
        return false;
    // about 1 BPM per mouse notch, finer with Shift
    const double step = (e.shift ? 0.01 : 0.2) * (double) e.wheelDeltaY * kWheelPixels * 0.1;
    commitTempo(ctx().transport->tempo() + step);
    repaint();
    return true;
}

bool TopBar::keyPressed(const KeyEvent& k)
{
    if (editingSignature_)
    {
        if (ctx().song != signatureSong_) { editingSignature_ = false; return false; }
        if (k.keyCode == juce::KeyPress::escapeKey) editingSignature_ = false;
        else if (k.keyCode == juce::KeyPress::returnKey)
        {
            editingSignature_ = false;
            int numerator = 0, denominator = 0; char extra = 0;
            auto text = signatureText_;
            for (auto& ch : text) if (ch == '/' || ch == ',' || ch == '.') ch = ' ';
            const int count = std::sscanf(text.c_str(), "%d %d %c", &numerator, &denominator, &extra);
            if (ctx().song && count == 1)
                commitSignature(signatureDenominator_ ? ctx().song->timeSigNumerator : numerator,
                                signatureDenominator_ ? numerator : ctx().song->timeSigDenominator);
            else if (count == 2) commitSignature(numerator, denominator);
        }
        else if (k.cmd && (k.textChar == 'a' || k.textChar == 'A')) signatureSelected_ = true;
        else if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey)
        { if (signatureSelected_) signatureText_.clear(); else paint::popLastCodePoint(signatureText_); signatureSelected_ = false; }
        else if (! k.cmd && ! k.alt && ((k.textChar >= '0' && k.textChar <= '9') || k.textChar == '/' || k.textChar == ' ' || k.textChar == ',' || k.textChar == '.'))
        { if (signatureSelected_) signatureText_.clear(); signatureSelected_ = false; if (signatureText_.size() < 8) signatureText_ += (char) k.textChar; }
        repaint(); return true;
    }
    if (! editingTempo_)
        return false;
    if (k.keyCode == juce::KeyPress::escapeKey) { cancelTempoEdit(); return true; }
    if (k.keyCode == juce::KeyPress::returnKey)
    {
        editingTempo_ = false;
        if (! tempoText_.empty())
            commitTempo(juce::String(tempoText_).getDoubleValue());
        repaint();
        return true;
    }
    if (k.keyCode == juce::KeyPress::backspaceKey || k.keyCode == juce::KeyPress::deleteKey)
        paint::popLastCodePoint(tempoText_);
    else if (! k.cmd && ((k.textChar >= '0' && k.textChar <= '9') || ((k.textChar == '.' || k.textChar == ',') && tempoText_.find('.') == std::string::npos)))
    {
        if (tempoText_.size() < 7) tempoText_ += k.textChar == ',' ? '.' : (char) k.textChar;
    }
    else
        return true; // other keys do nothing while typing a value
    repaint();
    return true;
}

} // namespace sonora::ui
