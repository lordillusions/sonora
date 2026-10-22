#pragma once
// Live's control bar: tempo / time signature / metronome on the left, transport and
// song position in the middle, loop on the right side, CPU + MIDI indicators far right.

#include "ui/Widget.h"
#include "ui/widgets/Buttons.h"

#include <vector>

namespace sonora::model { struct Song; }

namespace sonora::ui
{

class TopBar : public Widget
{
public:
    TopBar();

    void layout() override;
    void paint(SkCanvas& c, const Theme& t) override;
    void tick() override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDrag(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseDoubleClick(const MouseEvent& e) override;
    bool mouseWheel(const MouseEvent& e) override;
    bool keyPressed(const KeyEvent& k) override;
    Rect timeSignatureBounds() const { return sigBox_; }
    bool wantsTextInput() const override { return editingTempo_ || editingSignature_; }
    void focusLost() override { cancelTempoEdit(); editingSignature_ = false; draggingSignature_ = false; repaint(); }

private:
    void pressKey(char letter);
    void commitTempo(double bpm);
    void commitSignature(int numerator, int denominator);
    bool editingSignature_ = false, signatureSelected_ = false, draggingSignature_ = false, signatureDenominator_ = false;
    std::string signatureText_;
    const model::Song* signatureSong_ = nullptr;
    int signatureBeforeNum_ = 4, signatureBeforeDen_ = 4, signaturePreviewNum_ = 4, signaturePreviewDen_ = 4;
    void cancelTempoEdit() { if (editingTempo_) { editingTempo_ = false; repaint(); } }
    // Double-click the tempo field to type a value (Enter commits, Escape cancels); wheel and drag change it too.
    bool editingTempo_ = false;
    std::string tempoText_;
    IconButton* browserToggle_ = nullptr;
    IconButton* play_ = nullptr;
    IconButton* stop_ = nullptr;
    IconButton* record_ = nullptr;
    IconButton* loop_ = nullptr;
    IconButton* metronome_ = nullptr;
    TextButton* countIn_ = nullptr;
    IconButton* punchIn_ = nullptr;
    IconButton* punchOut_ = nullptr;
    // Live's tools: "+", automation arm, re-enable, capture MIDI, session record | draw mode, computer keyboard, key map
    IconButton* overdub_ = nullptr;
    IconButton* automationArm_ = nullptr;
    IconButton* reEnable_ = nullptr;
    IconButton* capture_ = nullptr;
    IconButton* sessionRecord_ = nullptr;
    IconButton* drawMode_ = nullptr;
    IconButton* keyboard_ = nullptr;
    TextButton* keyMap_ = nullptr;
    TextButton* scale_ = nullptr;
    Rect scaleRootBox_, scaleModeBox_;
    Widget* midiHit_ = nullptr; // transparent click target over the MIDI box (MIDI map mode)
    class CollabBadge* collab_ = nullptr; // COL-5: people in the session, or "Share"
    float lastCollabWidth_ = -1;
    int lastCollabState_ = -1;
    size_t lastCollabPeople_ = 0;

    std::vector<double> taps_;
    Rect tapBox_, tempoBox_, sigBox_, positionBox_, loopStartBox_, loopLenBox_, cpuBox_, midiBox_;
    // Loop start / length boxes drag like Live's: the third of the box under the pointer picks bars, beats or sixteenths.
    enum class LoopField { None, Start, Length } loopField_ = LoopField::None;
    double loopUnit_ = 1.0, loopStartBefore_ = 0, loopLengthBefore_ = 4;
    bool loopDragged_ = false;
    float dragStartY_ = 0;
    double dragStartTempo_ = 120;
    bool draggedTempo_ = false;
    double lastPosition_ = -1;
    int lastCpu_ = -1;
    int lastCountInBars_ = -1;
    bool lastOverridden_ = false, lastCapture_ = false;
};

} // namespace sonora::ui
