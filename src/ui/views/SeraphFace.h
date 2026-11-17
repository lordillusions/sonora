#pragma once
// Face of Sonora Seraph (PLG-27), on the ProFace kit: the device chain (compact), its own window and the VST3 editor
// (full). Seraph is Sonora's own device (no reference device), so AGENTS rule 7 does not apply. Accent gold.
//
// Full layout (1160 x 750): the wordmark, the preset bar and what is singing; CHOIR (sections, the stalls: one dot per
// sounding singer placed by pan, sized by voice type, glowing with its level; singers a note, Men Octave, Fold, Poly or
// Legato); MOUTH (the vowel strip A E I O U with the marker and the motion range, Hum, the live spectrum: the formant
// curve of the vowel with the harmonics of the lowest sounding note, Vowel Steps, Onset, Motion); VOICE; HUMAN;
// ENVELOPE; OUT; CHANT (on, the phrase drawn as steps with the one singing lit, key, scale, rate, organum, ison); GATE
// (on, the pattern as a step lane with the play head, pattern chooser, rate, before/after the space, length, smooth,
// depth, swing); SPACE; the help line.
//
// Compact layout (760 x 170): sections, the vowel strip, gate on and pattern, the stalls, Singers, Breath, Vibrato, Space,
// Output, Attack, Poly / Legato. (The chain's title bar has the window button.)
//
// The gate lane is a StepLane and the preset row a PresetBar (ProFaceParts, PLG-25a).
//
// Real voices (PLG-27b): Voices / Real in the header; with Real, the mouth's display lists the loaded recordings (note,
// vowel, how far the last note was from its recording), Load... and Clear, or, empty, what to load. Audio files dropped on
// the face (Finder, Sonora's Browser) are loaded as Real voices.

#include "devices/native/Seraph.h"
#include "ui/views/ProFace.h"
#include "ui/views/ProFaceParts.h"

#include <functional>
#include <memory>

#include <string>
#include <vector>

namespace sonora::ui
{
class SeraphFace final : public ProFace
{
public:
    static constexpr SkColor kAccent = 0xFFE7C26B;
    static constexpr Size kFull{1160, 750}, kCompact{760, 170};

    explicit SeraphFace(devices::Seraph& device);
    devices::Seraph& seraph() { return seraph_; }

    std::string headline() const; // "Mixed choir · 5 singers a note · 15 singing"
    StepLane& gateLane() { return gateLane_; }
    PresetBar& presetBar() { return presetBar_; }

    // Real voices: files dropped on the face or chosen with Load... (the app: an undoable device edit; the VST3 loads them
    // directly), Clear. Unset: the face loads into the device itself.
    std::function<void(const std::vector<std::string>& paths)> onVoicesDropped;
    std::function<void()> onChooseVoices, onClearVoices;
    // PLG-27h, the Seraph Voices pack: where packs are installed (the app's Packs folder; empty: the default), and how to get
    // it (the app downloads it through the Browser's pack store; unset in the VST3: the face says where to get it).
    std::string packsRoot;
    std::function<void()> onGetPack;
    std::string packCredit() const { return pack_.credit.empty() ? std::string(devices::Seraph::kPackCredit) : pack_.credit; }
    std::string realMessage; // the last refusal or load note, shown in the Real panel
    std::weak_ptr<int> lifetime() const { return lifetime_; } // for callbacks that may outlive the face (file choosers)
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    void loadVoices(const std::vector<std::string>& paths); // what a drop does
    // The vowel strip's x for a vowel (design grid, current layout).
    float vowelX(float vowel) const;
    // The rectangles of the choosers (design grid, current layout; empty when not shown).
    Rect phraseRect() const { return phrase_; }
    Rect patternRect() const { return pattern_; }
    Rect scaleRect() const { return scale_; }

protected:
    void build(Layout) override;
    void paintBack(SkCanvas&, Layout) override;
    void paintFront(SkCanvas&, Layout) override;
    std::string valueText(int param, float value) const override;
    std::string idleHelp() const override;
    bool animate() override;

private:
    struct Slider
    {
        Rect r;
        int param;
        std::string label;
    };
    void addSlider(const Rect&, int param, std::string label, std::string tip);
    void addVowelStrip(const Rect&);
    void addPatternChooser(const Rect&);
    void selectPattern(int pattern);
    void paintReal(SkCanvas&, const Rect&);
    void editGateStep(int step, float level);
    void paintStalls(SkCanvas&, const Rect&, bool compact);
    void paintSpectrum(SkCanvas&, const Rect&);
    void paintVowelStrip(SkCanvas&, const Rect&, bool compact);
    void paintPhrase(SkCanvas&, const Rect&);
    void paintPattern(SkCanvas&, const Rect&, bool compact);
    void paintSlider(SkCanvas&, const Slider&);
    int spectrumType() const;

    devices::Seraph& seraph_;
    StepLane gateLane_;
    PresetBar presetBar_;
    std::vector<Slider> sliders_;
    Rect stalls_, vowel_, spectrum_, phrase_, pattern_, scale_;
    size_t lastRealCount_ = 0;
    int lastSource_ = -1, packScanTicks_ = 0;
    devices::Seraph::PackVoices pack_;
    std::string packSignature_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    float lastLevels_[devices::Seraph::kMaxSingers]{};
    int lastGateStep_ = -2, lastChantStep_ = -2, lastActive_ = -1, lastSteps_ = -1;
};
} // namespace sonora::ui
