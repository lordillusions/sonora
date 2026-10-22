#pragma once
// Live 12's Clip view panel for audio and MIDI clips, as boxes under the clip's title bar: the Clip box (start/end, loop,
// position/length, signature, groove, scale; no header), the Audio box (Warp / Follow, warp mode with its Preserve / grain /
// transient settings, BPM, /2 x2, Reverse / Stems, Fade / RAM / HiQ, gain fader and pitch) or the MIDI boxes (Pitch & Time,
// Transform, Generate), and the Launch box (mode, quantisation, legato, velocity). Boxes flow into further columns when the
// detail view is too short to stack them (columnsFor()); the panel scrolls vertically only when one box is taller than it.
// Numeric fields drag vertically (Shift: fine, double-click resets gain / pitch); menus use native popups.
// Every gesture is one undoable model::modifyClip.

#include "core/Edit.h"
#include "ui/MidiClipEdits.h"
#include "ui/Widget.h"

#include <vector>

namespace sonora::ui
{

class ClipControls : public Widget
{
public:
    static constexpr float preferredWidth = 170;

    enum class Id
    {
        Activator, ClipHeader, AudioHeader, NotesHeader, LaunchHeader, Start, End, Position, Length, StartSet, EndSet, PositionSet, LengthSet, Loop, SigNum, SigDen,
        ScaleIcon, ScaleRoot, ScaleMode, Warp, WarpMode, Preserve, LoopMode, Envelope, Grain, Flux, Formants, ProEnvelope,
        Bpm, Half, Double, Gain, PitchSt, PitchCt, Reverse, Stems, NoteRev, NoteInv, NoteLegato, NoteDuplicate, NoteDouble,
        NoteHalf, FitScale, Humanize, TransformHeader, GenerateHeader, TransformType, GenerateType, ToolApply, ToolReset,
        ToolRate, ToolAmount, ToolGate, ToolParts, ToolLow, ToolHigh, ToolDensity, LaunchMode, LaunchQuant, LaunchLegato, LaunchVelocity,
        Follow, Fade, Ram, HiQ, PitchStBox, Groove, Separator
    };

    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool wantsTextInput() const override { return editingBpm_; }
    void focusLost() override { editingBpm_ = false; repaint(); }

    bool toolPanelOpen() const { return transformOpen_ || generateOpen_; }
    void showTool(midiedit::Tool tool)
    {
        const bool generate = midiedit::isGenerator(tool);
        transformOpen_ = ! generate; generateOpen_ = generate; notesOpen_ = launchOpen_ = false;
        (generate ? generator_ : transform_).tool = tool;
        repaint();
    }
    bool keyPressed(const KeyEvent&) override;
    // Rectangle of a control in this panel's coordinates (tests, hit-testing); empty when it is not shown.
    Rect controlBounds(Id id) const;
    // The clip gain fader's taper: position 0 (bottom, -70 dB) .. 1 (top, +24 dB) and back (Live's curve).
    static double gainFaderPosition(double db);
    static double gainFaderDb(double position);
    // Width of one column of boxes, and how many columns the selected clip's boxes need at this panel height.
    float columnWidth() const;
    int columnsFor(float panelHeight);

private:
    struct Control
    {
        Id id;
        Rect r;
    };
    void applyTool(bool generate);
    void finishBpmEdit();
    bool editingBpm_ = false, bpmSelected_ = false;
    std::string bpmText_;
    const model::Song* bpmSong_ = nullptr;
    midiedit::ToolSettings transform_, generator_{.tool = midiedit::Tool::Rhythm};
    bool transformOpen_ = false, generateOpen_ = false;
    uint32_t toolSeed_ = 1;
    enum class Box { Clip, Audio, Notes, Launch, Transform, Generate };
    struct Section
    {
        Box box;
        Rect r;
        Id header; // Id::Separator: no header (the Clip box)
        bool open;
    };
    void layoutControls(const model::Clip& clip, float panelHeight = -1);
    const Control* controlAt(float x, float y) const;
    void commit(model::Clip changed, const char* label);
    void showMenu(Id id, const model::Clip& clip);
    model::Clip dragged(Id id, const model::Clip& base, float dy, bool fine) const;
    std::string valueText(Id id, const model::Clip& clip) const;

    std::vector<Control> controls_;
    std::vector<Section> sections_;
    int columns_ = 1;
    float contentHeight_ = 0, scrollY_ = 0;
    bool audioOpen_ = true, notesOpen_ = true, launchOpen_ = false;
    // Active drag
    bool dragging_ = false, dragChanged_ = false, toolDragging_ = false;
    midiedit::ToolSettings toolBefore_;
    Id dragId_ = Id::Start;
    float grabY_ = 0;
    model::Clip before_, working_;
    model::ClipLocation location_;
};

} // namespace sonora::ui
