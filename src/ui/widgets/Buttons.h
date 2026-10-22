#pragma once
// Small rectangular buttons in Live's style (1px dark frame, flat fill, orange when on).

#include "ui/Widget.h"

namespace sonora::ui
{

class TextButton : public Widget
{
public:
    enum class Style
    {
        Toggle,    // on: ChosenDefault fill (orange)
        Activator, // track activator: on == ChosenDefault, off == dimmed
        Solo,      // on: ChosenPreListen (blue)
        Arm,       // on: ChosenRecord (red)
        Plain,     // momentary
    };

    TextButton(std::string label, Style style = Style::Toggle) : label_(std::move(label)), style_(style) {}

    std::function<bool()> isOn;    // queried every paint
    std::function<void()> onClick; // mouse up inside
    std::function<void(const MouseEvent&)> onClickWithModifiers;

    std::function<bool()> isEnabled;

    void setLabel(std::string l) { label_ = std::move(l); }
    const std::string& label() const { return label_; }

    std::function<bool()> isMidiArm; // MIDI arm uses a note glyph; audio arm uses a circle
    bool roundStyle = false;
    bool barStyle = false; // flat control-bar look instead of a framed toggle

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent&) override;
    void mouseUp(const MouseEvent& e) override;

private:
    std::string label_;
    Style style_;
    bool pressed_ = false;
};

// Transport-style icon button (play / stop / record / loop ...).
class IconButton : public Widget
{
public:
    enum class Icon
    {
        Play,
        Stop,
        Record,
        Loop,
        Metronome,
        Wrench,
        Browser, // view toggle: two vertical bars, orange when the browser is shown
        Overdub,        // "+": MIDI Arrangement Overdub
        AutomationArm,  // two linked breakpoints
        ReEnable,       // left arrow: Re-Enable Automation
        Capture,        // dashed square: Capture MIDI
        SessionRecord,  // open circle
        PunchIn,        // step down
        PunchOut,       // step up
        Draw,           // pencil
        Keyboard,       // piano keys
        FloatWindow,    // window with an outward arrow: open an editor in its own window
        SessionView,
        ArrangementView,
    };

    explicit IconButton(Icon icon) : icon_(icon) {}
    Icon icon() const { return icon_; }

    std::function<bool()> isOn;
    std::function<void()> onClick;
    std::function<void()> onDoubleClick;
    std::function<bool()> isEnabled; // greyed and inert when it returns false (unset == enabled)
    bool flat = false; // no background, title-text colour: the glyph sits in a ring its parent draws (device titles)

    void paint(SkCanvas& c, const Theme& t) override;
    void mouseDown(const MouseEvent&) override {}
    void mouseUp(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent&) override;

private:
    Icon icon_;
};

} // namespace sonora::ui
