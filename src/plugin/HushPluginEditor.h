#pragma once
// The Sonora Hush VST3's window (PLG-14), drawn with Skia like the app: a left panel (mode, depth, detail, attack,
// release), a collapsible side panel (stereo, tilt, scale), the toolbar (sidechain, presets, undo/redo, A/B, help,
// settings, size), the reduction graph with the depth-curve band nodes and the selected band's controls, and the footer
// (bypass, delta, mix, out, quality, low latency).
//
// Layout and palette follow the reference look (light blue panels, dark knobs, white curve over a blue
// reduction area); the wordmark, icons and every drawing are Sonora's own. Everything is laid out on a 900 x 532 design
// grid and scaled to the window.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::gfx { class SkiaCanvasComponent; }

namespace sonora::plugin
{
class HushPlugin;

class HushView final : public ui::RootWidget
{
public:
    static constexpr float kWidth = 900.0f, kHeight = 532.0f;

    HushView(ui::UiContext&, HushPlugin&);

    // The root never receives its own input (RootWidget hands events to children), so one full-size child takes every
    // mouse and key event and passes it to the view's handlers below.
    class InputLayer;

    // Where menus open (the plug-in window); null in tests, which then skip menus.
    std::function<juce::Component*()> menuHost;

    void layout() override;
    void paint(SkCanvas&, const ui::Theme&) override;
    void mouseDown(const ui::MouseEvent&) override;
    void mouseDrag(const ui::MouseEvent&) override;
    void mouseUp(const ui::MouseEvent&) override;
    void mouseMove(const ui::MouseEvent&) override;
    void mouseExit() override;
    void mouseDoubleClick(const ui::MouseEvent&) override;
    bool mouseWheel(const ui::MouseEvent&) override;
    bool keyPressed(const ui::KeyEvent&) override;
    bool wantsTextInput() const override { return editing_ >= 0; }
    void tick() override;

    // ---- tests and the editor
    // What the last menu showed (tests run without a window, so menus are recorded instead of opened).
    std::vector<std::string> lastMenu;
    std::function<void(int)> lastMenuResult;
    int selectedBand() const { return selected_; }
    void selectBand(int band);
    bool sidePanelOpen() const;
    float freqToX(float hz) const;  // design-grid x of a frequency on the graph
    float xToFreq(float x) const;
    float zeroY() const;            // design-grid y of 0 dB on the graph
    float nodeY(int band) const;
    int bandAt(float x, float y) const; // design-grid point -> band index or -1
    int createBand(float x, float y, int shape); // first unused band; -1 when all eight are in use
    ui::Rect graphArea() const { return graph_; }
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
        ui::Rect r;
        int param = -1;
        std::function<void(const ui::MouseEvent&)> click; // buttons
        std::string id;
    };

    float scale() const;
    ui::MouseEvent toDesign(const ui::MouseEvent&) const;
    void buildControls();
    const Control* hit(float x, float y) const;
    void beginEdit(int param);
    void endEdit();
    void setValue(int param, float v);
    float value(int param) const;
    void dragValue(int param, float dy, bool fine);
    void resetValue(int param);
    void commitText();
    void showMenu(juce::PopupMenu menu, const ui::MouseEvent& at, std::function<void(int)> done);
    void presetMenu(const ui::MouseEvent&);
    void nodeMenu(int band, const ui::MouseEvent&);
    void shapeMenu(const ui::MouseEvent&, float x, float y);

    void paintLeft(SkCanvas&);
    void paintSide(SkCanvas&);
    void paintToolbar(SkCanvas&);
    void paintGraph(SkCanvas&);
    void paintBandPanel(SkCanvas&);
    void paintFooter(SkCanvas&);
    void paintKnob(SkCanvas&, float cx, float cy, float radius, int param);
    void paintBox(SkCanvas&, const ui::Rect&, int param, float fontSize = 13.0f);
    void paintFocus(SkCanvas&, const ui::Rect&, int param);
    void paintTilt(SkCanvas&, const ui::Rect&, int param);

    HushPlugin& plugin_;
    ui::Rect left_, strip_, side_, toolbar_, graph_, footer_, bandPanel_;
    std::vector<Control> controls_;
    int selected_ = -1, hoverBand_ = -1, hoverControl_ = -1;
    int dragParam_ = -1, dragBand_ = -1;
    float lastY_ = 0, lastX_ = 0;
    bool dragListen_ = false;
    int editing_ = -1;
    std::string editText_;
    unsigned lastRevision_ = 0;
    int ticks_ = 0;
    InputLayer* input_ = nullptr;
    float mouseX_ = -1, mouseY_ = -1; // design grid, for tooltips and the frequency readout
    void paintOverlay(SkCanvas&);
    std::string tooltipFor(float x, float y) const;
};

class HushPluginEditor final : public juce::AudioProcessorEditor
{
public:
    explicit HushPluginEditor(HushPlugin&);
    ~HushPluginEditor() override;
    void resized() override;
    HushView& view() { return view_; }

private:
    ui::UiContext& connect();

    HushPlugin& plugin_;
    bool sized_ = false; // resized() keeps editorScale only once the constructor has set the size
    ui::UiContext context_;
    HushView view_;
    std::unique_ptr<gfx::SkiaCanvasComponent> canvas_;
};
} // namespace sonora::plugin
