#pragma once
// The Sonora Hush face (PLG-14, PLG-14g), drawn with Skia: a left panel (mode, depth, detail, attack, release), a
// collapsible side panel (stereo, tilt, scale), the toolbar (sidechain, presets, undo/redo, A/B, help, settings, size),
// the reduction graph with the depth-curve band nodes and the selected band's controls, and the footer (bypass, delta,
// mix, out, quality, low latency).
//
// The same face is the VST3's window (plugin/HushPluginEditor) and the built-in device's face in the device chain
// (DetailView) and its own window. It edits a devices::HushSession. Everything is laid out on a 900 x 532 design grid
// and scaled to the widget's width; owners keep the aspect ratio.
//
// Layout and palette follow the reference look (light blue panels, dark knobs, white curve over a blue reduction
// area); the wordmark, icons and every drawing are Sonora's own.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/Widget.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sonora::devices { class HushDevice; class HushSession; }

namespace sonora::ui
{
class HushFace final : public Widget
{
public:
    static constexpr float kWidth = 900.0f, kHeight = 532.0f;

    explicit HushFace(devices::HushSession&);
    devices::HushSession& session() { return session_; }

    // Where menus open; null in tests, which then only record them (lastMenu).
    std::function<juce::Component*()> menuHost;
    // Size button: S/M/L/XL when set (the VST3 resizes its window; the built-in device's window too).
    std::function<void(float scale)> onScaleChosen;
    // Size button: "Open in a window" when set (the built-in device in the device chain).
    std::function<void()> onOpenWindow;
    // PLG-14i, built-in Hush: the Sidechain switch was clicked; returns true when it handled the click (opened the source
    // chooser because no track is routed yet), false to toggle as usual. Unset in the VST3 (the host routes it).
    std::function<bool()> onSidechainToggle;
    // The MIDI map / key map target for a parameter (the device chain maps controls like any device's knobs).
    std::function<std::optional<midi::ControlTarget>(int parameter)> mappingTarget;
    std::string version; // shown under the wordmark; empty = the running application's version

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseMove(const MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool keyPressed(const KeyEvent&) override;
    bool wantsTextInput() const override { return editing_ >= 0; }
    std::optional<midi::ControlTarget> mappingTargetAt(float x, float y) const override;
    void tick() override;

    // ---- tests and owners
    // What the last menu showed (tests run without a window, so menus are recorded instead of opened).
    std::vector<std::string> lastMenu;
    std::function<void(int)> lastMenuResult;
    int selectedBand() const { return selected_; }
    void selectBand(int band);
    bool sidePanelOpen() const;
    float scale() const; // widget pixels per design-grid pixel
    float freqToX(float hz) const;  // design-grid x of a frequency on the graph
    float xToFreq(float x) const;
    float zeroY() const;            // design-grid y of 0 dB on the graph
    float nodeY(int band) const;
    int bandAt(float x, float y) const; // design-grid point -> band index or -1
    int createBand(float x, float y, int shape); // first unused band; -1 when all eight are in use
    Rect graphArea() const { return graph_; }
    int controlAt(float x, float y) const; // parameter index of the control under a design-grid point, -1 when none

private:
    enum class Kind
    {
        Knob,
        Box,
        Toggle,
        Choice,
        FocusPair,
        Tilt,
        Button
    };
    struct Control
    {
        Kind kind;
        Rect r;
        int param = -1;
        std::function<void(const MouseEvent&)> click; // buttons
        std::string id;
    };

    MouseEvent toDesign(const MouseEvent&) const;
    std::string versionText() const;
    void buildControls();
    const Control* hit(float x, float y) const;
    void beginEdit(int param);
    void endEdit();
    void setValue(int param, float v);
    float value(int param) const;
    void dragValue(int param, float dy, bool fine);
    void resetValue(int param);
    void commitText();
    void showMenu(juce::PopupMenu menu, const MouseEvent& at, std::function<void(int)> done);
    void presetMenu(const MouseEvent&);
    void nodeMenu(int band, const MouseEvent&);
    void shapeMenu(const MouseEvent&, float x, float y);

    void paintLeft(SkCanvas&);
    void paintSide(SkCanvas&);
    void paintToolbar(SkCanvas&);
    void paintGraph(SkCanvas&);
    void paintBandPanel(SkCanvas&);
    void paintFooter(SkCanvas&);
    void paintKnob(SkCanvas&, float cx, float cy, float radius, int param);
    void paintBox(SkCanvas&, const Rect&, int param, float fontSize = 13.0f);
    void paintFocus(SkCanvas&, const Rect&, int param);
    void paintTilt(SkCanvas&, const Rect&, int param);
    void paintOverlay(SkCanvas&);
    std::string tooltipFor(float x, float y) const;

    devices::HushSession& session_;
    Rect left_, strip_, side_, toolbar_, graph_, footer_, bandPanel_;
    std::vector<Control> controls_;
    int selected_ = -1, hoverBand_ = -1, hoverControl_ = -1;
    int dragParam_ = -1, dragBand_ = -1;
    float lastY_ = 0, lastX_ = 0;
    bool dragListen_ = false;
    int editing_ = -1;
    std::string editText_;
    unsigned lastRevision_ = 0;
    int ticks_ = 0;
    float mouseX_ = -1, mouseY_ = -1; // design grid, for tooltips and the frequency readout
};

// Root of the built-in Sonora Hush's own window (PLG-14g): the face at the window's size. It follows its instance through
// graph rebuilds by key, like the floating Prism (WavetableWindowView), and calls onGone once the device was deleted.
class HushWindowView final : public RootWidget
{
public:
    HushWindowView(UiContext&, UiContext::WavetableKey);
    const UiContext::WavetableKey& key() const { return key_; }
    void setKey(const UiContext::WavetableKey&);
    // Rebuilds the face after song/graph changes. Call synchronously when the model changes (pointers into the old graph
    // must go before it is deleted).
    void sync();
    HushFace* face() const { return face_; }
    const std::string& title() const { return title_; }
    std::function<void(const std::string&)> onTitleChanged; // the window name follows the device and track names
    std::function<void()> onGone;                           // asynchronous close; may be called from inside the window
    std::function<void(float scale)> onScaleChosen;         // Size menu: S/M/L/XL
    std::function<juce::Component*()> menuHost;

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

    // The built-in Hush at a key: control-track index, model device and its running processor (null while rebuilding).
    struct Site
    {
        int track = -1;
        const model::Device* model = nullptr;
        devices::HushDevice* processor = nullptr;
    };
    static std::optional<Site> find(const UiContext&, const UiContext::WavetableKey&);

private:
    bool stale() const;
    void rebuild();

    UiContext::WavetableKey key_;
    HushFace* face_ = nullptr;
    const void* builtSong_ = nullptr;
    const void* builtGraph_ = nullptr;
    uint64_t builtStructure_ = 0;
    bool built_ = false, goneSent_ = false;
    std::string title_;
};
} // namespace sonora::ui
