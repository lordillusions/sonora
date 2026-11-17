#pragma once
// Face of Sonora Phrase (PLG-25, docs/PHRASE.md "Face"), on the ProFace kit and its PLG-25a parts. Accent teal. Sonora's
// own device (no reference device), so AGENTS rule 7 does not apply.
//
// Full layout (its own window, the VST3 editor; 1160 x 780): header (wordmark, preset bar, Part, To Clip / Drag MIDI,
// Keep Chords); HARMONY (Key, Scale, Chords source, Link channel, the chord strip with the palette under it, the factory
// progressions, the playing chord and the link status); PATTERN (slot tabs A-H, form, the pattern's rate / length /
// register, the live Pattern choice, the tone grid and the step lanes; Pad: its controls and a voicing view); DICE (the
// part's style, Dice / Mutate / Answer); ROLL (the form's notes over the progression in fit colours, play head); PERFORM
// (Density and the performance knobs); the help line.
//
// Compact layout (the device chain, 760 x 170): Part, the slot pads A-H (lit: playing; a click plays that slot, a click
// on the lit one returns to the form), Density, Swing, Gate, Octave, Dice, the chord playing, a small roll.

#include "devices/native/Phrase.h"
#include "ui/views/ProFace.h"
#include "ui/views/ProFaceParts.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::ui
{
namespace phrase
{
// A small value box for state that is not a host parameter (pattern settings, generator styles): the label above, the
// value inside; drag up / down or use the wheel to change it, a click on a choice steps it (Shift back), Alt-click resets.
class ValueBox final : public FacePart
{
public:
    std::string label;
    float minValue = 0, maxValue = 1, defaultValue = 0, step = 1;
    std::vector<std::string> choices;          // a choice box when set
    std::function<std::string(float)> format;  // number text (default: the integer)
    std::function<float()> get;
    std::function<void(float)> set;

    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;

private:
    void apply(float);
    float startValue_ = 0, startY_ = 0;
    bool dragging_ = false;
};

// A button that hands its click's modifiers on (Dice: Shift keeps the rhythm, Alt keeps the pitches).
class PushButton final : public FacePart
{
public:
    std::string label;
    bool primary = false;
    std::function<void(const MouseEvent&)> press;
    std::function<bool()> lit;
    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent& e) override
    {
        if (press) press(e);
        notify();
    }
};

// The pattern's tone grid: rows are chord tones (Arp: 1-8; Bass: root, fifth, octave; Stabs: the top tone), columns are
// steps. Click a cell to place a note there (a click on the note removes it), drag to paint along the row under the
// pointer; Alt-drag a note up or down to add scale steps (shown as +1 / -2). Ties extend their note.
class ToneGrid final : public FacePart
{
public:
    int rows = 8;
    bool topLane = false;   // Stabs: the rows are the top tone
    bool kickHatch = false; // Bass: the kick's sixteenth hatched
    std::function<model::PhrasePattern&()> pattern;
    std::function<int()> playStep;
    std::function<void()> edited; // after a change (publish)

    int stepAt(float x) const;
    int rowAt(float y) const;
    Rect cellRect(int step, int row) const;
    void paint(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;

private:
    int toneOf(const model::PhraseStep&) const;
    void place(int step, int row);
    bool adding_ = true, alt_ = false, active_ = false;
    int altStep_ = -1, altStart_ = 0;
    float altY_ = 0;
};

// The chord palette (an overlay under the chord strip): every chord of the palette's key and mode, one column per degree,
// rows Triad, 7th, 9th, Add9, Sus2, Sus4. A filled dot: every note in the instance's scale; hollow: borrowed. The arrows
// move the palette's key round the circle of fifths, the mode button steps through the parallel modes, Home returns to
// the instance's key and scale. A click auditions a chord, a double-click puts it into the selected slot. Footer:
// the slot's inversion, Remove, Close.
class ChordPalette final : public FacePart
{
public:
    static constexpr int kRows = 6;
    int key = 9, scale = 1;                       // the palette's own key and mode
    std::function<int()> homeKey, homeScale;      // the instance's
    std::function<void(const model::ChordTones&, model::ChordExtension, model::ChordQuality)> audition, assign;
    std::function<int()> inversion;               // of the selected slot
    std::function<void(int)> setInversion;
    std::function<void()> remove, close;
    bool open = false;

    static model::ChordExtension rowExtension(int row);
    static model::ChordQuality rowQuality(int row);
    model::ChordTones chordAt(int degree, int row) const;
    Rect cellRect(int degree, int row) const;
    Rect leftRect() const;
    Rect rightRect() const;
    Rect modeRect() const;
    Rect homeRect() const;
    Rect inversionRect(int index) const; // 0 Auto, 1 Root, 2 1st, 3 2nd, 4 3rd
    Rect removeRect() const;
    Rect closeRect() const;

    bool overlayOpen() const override { return open; }
    void paint(SkCanvas&) override {}
    void paintOverlay(SkCanvas&) override;
    void mouseDown(const MouseEvent&) override;
};
} // namespace phrase

class PhraseFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFF35C2B0;
    static constexpr Size kFull{1160, 780}, kCompact{760, 170};

    explicit PhraseFace(devices::Phrase& device);
    devices::Phrase& phrase() { return phrase_; }

    // The VST3: drag the part out as a .mid file (shown instead of To Clip when onToClip is unset).
    std::function<void()> onDragMidi;

    int selectedSlot() const { return slot_; }
    void selectSlot(int slot);
    int selectedChord() const { return chord_; }
    const NoteRoll& roll() const { return roll_; }
    std::string chordNow() const; // the chord playing ("Am"), or what the link / MIDI In situation is
    void refreshRoll();           // renders the roll now (tests; the face does it when settings change)

    // Parts (tests drive them by their geometry).
    PresetBar& presetBar() { return presetBar_; }
    PresetBar& progressionBar() { return progressionBar_; }
    ChordStrip& chordStrip() { return chordStrip_; }
    phrase::ChordPalette& palette() { return palette_; }
    phrase::ToneGrid& toneGrid() { return toneGrid_; }
    StepLane& lane(const std::string& name);
    phrase::ValueBox& box(const std::string& name);
    phrase::PushButton& button(const std::string& name);

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string idleHelp() const override;
    bool animate() override;

private:
    model::PhrasePart part() const { return phrase_.part(); }
    model::PhrasePattern& pattern(); // the selected slot of the current part (working copy)
    void edited();                   // publish the working copy and redraw the roll
    void buildHeader(Layout);
    void buildHarmony();
    void buildPattern();
    void buildDice();
    void buildPerform();
    phrase::ValueBox& addBox(const std::string& name, const Rect&, std::string tip);
    StepLane& addLane(const std::string& name, const Rect&, std::string tip);
    phrase::PushButton& addPush(const std::string& name, const Rect&, std::string tip);
    uint64_t signature() const;
    SkColor fitColour(const NoteRoll::Note&) const;

    devices::Phrase& phrase_;
    int slot_ = 0, chord_ = -1;
    int answerKind_ = 0;
    PresetBar presetBar_, progressionBar_;
    ChordStrip chordStrip_;
    phrase::ChordPalette palette_;
    phrase::ToneGrid toneGrid_;
    NoteRoll roll_, smallRoll_;
    std::vector<std::unique_ptr<StepLane>> lanes_;
    std::vector<std::unique_ptr<phrase::ValueBox>> boxes_;
    std::vector<std::unique_ptr<phrase::PushButton>> buttons_;
    uint64_t rollSignature_ = 0;
    model::PhrasePart builtPart_ = model::PhrasePart::Count;
    double rollBeats_ = 16;
};
} // namespace sonora::ui
