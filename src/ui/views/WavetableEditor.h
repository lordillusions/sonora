#pragma once
// Native synthesis dashboard (DEV-18d): independent vector graphics, simultaneous voice
// modules, persistent modulation workspace and a complete parameter inspector for compact heights.
#include "ui/Widget.h"
#include "dsp/CurveGraph.h"
#include "devices/native/WavetableEffects.h"
#include "devices/native/WavetableSynth.h"
#include <array>

namespace sonora::devices { class WavetableSynth; class OscillatorSample; }
namespace sonora::ui
{
class TextButton;
class WavetableModulationEditor;
class WavetableRoutingEditor;
class WavetablePresetBrowser;
class WavetableEditor final : public Widget
{
public:
    enum class Section { Oscillators, Filters, Envelopes, Lfos, Matrix, Macros, Main, Media, Effects, Routing, Noise, Count };
    enum class Page { Osc, Mix, Fx, Matrix, Global, Count };
    static constexpr float kPreferredWidth = 1080.0f;
    WavetableEditor(devices::WavetableSynth*, int track, int device, int chain = -1, int child = -1);
    ~WavetableEditor() override;
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void paintOverChildren(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void focusLost() override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;
    void tick() override;
    Rect sectionCell(Section) const;
    Rect selectorCell(int direction) const;
    Rect pageCell(int direction) const;
    Rect parameterCell(int index) const;
    Rect oscillatorPlot(int oscillator) const;
    Rect subPlot() const;
    Rect subRoutingCell() const;
    Rect voiceQualityCell() const;
    Rect noisePlot() const;
    Rect noiseAssetCell() const;
    Rect noiseImportCell() const;
    Rect noiseMoreCell() const;
    Rect workflowCell(Page) const;
    Rect inspectorCell() const;
    Rect presetCell(int direction) const;      // -1 "<", 0 the preset name (opens the browser), 1 ">"
    Rect presetGroupCell(int direction) const; // -1 "<<", 1 ">>": previous / next group of the preset's bank (UI-28i)
    Rect presetMoreCell() const;               // "...": built-in presets, the library as menus, Import Preset...
    Rect matrixBankCell(int direction) const;
    Rect envelopeCell(int envelope) const;
    Rect lfoCell(int lfo) const;
    Rect envelopePlot() const;
    // Draggable envelope graph (UI-28e): 0 attack end, 1 hold end, 2 decay end / sustain, 3 release end,
    // 4..6 attack / decay / release curve handles. Empty when not shown.
    Rect envelopePoint(int point) const;
    Rect lfoPlot() const;
    Rect lfoModeCell(int mode) const; // 0 FREE (Global), 1 RETRIG, 2 ENVELOPE (One Shot)
    Rect lfoRateCell() const;         // the RATE knob shown for the selected LFO (Hz rate or tempo division)
    // Footer (UI-28): slim keyboard C1..C7 played straight into this device (no arm needed), wheels and the ARP view.
    static constexpr int kLowestKey = 24, kHighestKey = 96;
    Rect keyboardCell(int note) const;
    Rect pitchBendCell() const;
    Rect modWheelCell() const;
    Rect keyboardViewCell(bool arp) const;
    bool arpView() const { return arpView_; }
    // Drag-to-modulate (UI-28): ENV/LFO tabs and macros carry source handles; dropping one on a modulatable control
    // creates (or reuses) a matrix route drawn as a ring around that control.
    Rect modulationSourceCell(int source) const;
    Rect modulationRingCell(int parameter, int ring = 0) const; // a grab point on the control's ring (editor coordinates)
    int modulationDragSource() const { return modDragSource_; }
    const devices::WavetableSynth* synth() const { return processor_; }
    int modulationTargetAt(float x, float y) const;            // modulatable control parameter under (x, y), -1 none
    void beginModulationDrag(int source, float x, float y);
    void moveModulationDrag(float x, float y);
    void endModulationDrag(float x, float y);
    // Route readout shown while a ring is hovered or dragged ("Env 1→Filter 1 Cutoff: 57%", "Range: 45 Hz → 3.99 kHz").
    struct RingTip { int parameter = -1, slot = -1; };
    void setRingTip(RingTip tip) { ringTip_ = tip; repaint(); }
    RingTip ringTip() const { return ringTip_; }
    std::pair<std::string, std::string> ringReadout(int parameter, int slot) const;
    // VOICE / VELO / NOTE panel (UI-28d): tab 1/2 edits the Velocity / Note source curve.
    Rect voiceTabCell(int tab) const;
    int voiceTab() const { return voiceTab_; }
    Rect curveSourcePlot() const;
    Rect curveSourcePoint(int point) const; // 0 start, 1 middle, 2 end (editor coordinates, 6 px square)
    // Installed Serum 2 library (runtime compatibility import; see plugins/SerumLibrary.h).
    void loadLibraryTable(int oscillator, const std::string& path);
    void loadLibraryPreset(const std::string& path);
    bool stepLibraryTable(int oscillator, int direction);  // ◀ ▶ through the library when the table came from it
    bool stepLibraryPreset(int direction);  // < > inside the loaded library preset's bank, wrapping at its ends
    bool stepLibraryGroup(int direction);   // << >> to the first preset of the neighbouring group of that bank
    bool libraryGroupsAvailable() const;    // the loaded library preset's bank has at least two groups
    std::string libraryPresetPath() const;  // the loaded library preset, "" when the patch did not come from the library
    // Preset browser (UI-28i), drawn over the dashboard; its state survives the editor rebuild a preset load causes.
    void openPresetBrowser();
    void savePreset(); // UI-50: "Save Preset..." writes a .prism file (Prism library User bank by default)
    void closePresetBrowser();
    bool presetBrowserOpen() const;
    WavetablePresetBrowser* presetBrowser() const { return presetBrowser_; }
    Rect fxBusCell(int bus) const;
    Rect fxModuleCell(devices::WavetableEffects::ModuleId) const;
    Rect fxAddCell() const;
    Rect fxRemoveCell() const;
    Rect fxMoveCell(int direction) const;
    Rect fxBandCell(int band) const;
    Rect fxChainPageCell(int direction) const;
    Rect fxControlsPageCell(int direction) const;
    Rect fxImpulseCell() const;
    Rect fxPanelCell(devices::WavetableEffects::ModuleId) const; // the module's panel in the FX stack, empty when scrolled away
    // Rack list icons (UI-28g, Serum's list): hide/show the module's panel, delete the module. The power switch is the
    // module's On parameter (parameterCell(fxEnabledParameterIndex(id))).
    Rect fxRowHideCell(devices::WavetableEffects::ModuleId) const;
    Rect fxRowRemoveCell(devices::WavetableEffects::ModuleId) const;
    bool fxCollapsed(devices::WavetableEffects::ModuleId) const;
    Rect fxPanelMoreCell(devices::WavetableEffects::ModuleId) const; // ••• : the module's full CONTROLS page
    WavetableModulationEditor* modulationEditor() const { return modulationEditor_; }
    WavetableRoutingEditor* routingEditor() const { return routingEditor_; }
    Rect mixPageCell(bool sources) const;
    int fxBus() const { return fxBus_; }
    devices::WavetableEffects::ModuleId selectedFxModule() const { return fxSelected_; }
    bool inspectorOpen() const { return inspector_; }
    bool dashboardVisible() const { return ! inspector_; }
    Page workflow() const { return workflow_; }
    Section section() const { return section_; }
    int selectedInstance() const { return instances_[(size_t) section_]; }
    int instanceCount() const;
    int pageCount() const { return (int) pages_.size(); }
    int page() const { return page_; }
    std::vector<int> visibleParameters() const;
    // Controller publishes immutable content after edits; no live table replacement here.
    std::function<void(int oscillator)> onImportTable;
    std::function<void(int oscillator)> onImportMedia;
    std::function<void(int lfo, const dsp::CurveGraph&)> onEditLfoCurve;
    std::function<void(std::function<void(devices::WavetableEffects::Type)>)> onChooseEffect;
    std::function<void(std::function<void(std::string)>)> onChooseNoiseFile;
private:
    using Effects = devices::WavetableEffects;
    struct FxNode
    {
        Effects::ModuleId id = 0;
        Effects::Type type = Effects::Type::Utility;
        int bus = 0;
        std::vector<int> path;
        TextButton* select = nullptr;
    };
    struct Control
    {
        int parameter = 0, instance = 0, span = 1;
        Section section = Section::Main;
        Widget* widget = nullptr;
    };
    struct Placement { size_t control; int row, column; };
    void buildControls();
    void buildEffects();
    void buildModulation();
    void restoreModulationView();
    void buildRouting();
    bool routingDestinationAllowed(int source, int choice) const;
    void restoreFxView();
    void storeFxView();
    int fxViewKey() const;
    void selectFxBus(int);
    void selectFxModule(Effects::ModuleId);
    void showFxTypeMenu();
    void insertEffect(Effects::Type);
    void removeEffect();
    void moveEffect(int direction);
    void publishEffects(const Effects::Configuration&);
    void importImpulse();
    const FxNode* selectedFxNode() const;
    std::vector<const FxNode*> currentFxChain() const;
    int fxChainPageCount() const;
    void layoutEffects();
    void layoutMixer();
    void paintEffects(SkCanvas&);
    void paintMixer(SkCanvas&);
    void importOscillator(int oscillator, bool media);
    void tag(Widget*, int parameter);
    void showControl(int parameter, Rect bounds, bool compact = false);
    void selectSection(Section);
    void selectInstance(int);
    void selectWorkflow(Page);
    void instanceMenu();
    void presetMenu();
    void voiceQualityMenu();
    void noiseAssetMenu();
    void chooseNoiseFile();
    bool importNoiseFile(const std::string&);
    void commitNoiseSample(std::shared_ptr<const devices::OscillatorSample>);
    bool generatedNoise() const;
    void prepareNoisePreview();
    void paintNoisePreview(SkCanvas&, Rect);
    void loadPreset(int);
    void releaseNote();
    void playNote(int note, float y);
    void sendWheel(bool bend, float y);
    int noteAt(float x, float y) const;
    std::string instanceLabel(int) const;
    void layoutInspector();
    void layoutDashboard();
    struct OscillatorLayout { Rect table, pitch, display, phase, base, knobs, warps; };
    static OscillatorLayout oscillatorLayout(Rect panel);
    Rect filterPlot(int filter) const;
    void layoutOscillator(int oscillator, Rect);
    void layoutSub();
    void layoutNoise();
    void layoutFilter(int filter, Rect);
    void layoutModulation();
    void paintModules(SkCanvas&);
    void paintModulation(SkCanvas&);
    void paintInspector(SkCanvas&);
    void paintKeyboard(SkCanvas&);
    void paintWaveform(SkCanvas&, int oscillator, Rect);
    void paintTable3D(SkCanvas&, int oscillator, Rect);
    void paintCurveSource(SkCanvas&);
    void paintFilter(SkCanvas&, int filter, Rect);
    void paintEnvelope(SkCanvas&, Rect);
    void paintLfo(SkCanvas&, Rect);
    devices::WavetableSynth* processor_;
    int track_, device_, chain_, child_;
    Page workflow_ = Page::Osc;
    Section section_ = Section::Oscillators;
    std::array<int, (size_t) Section::Count> instances_{};
    std::array<TextButton*, (size_t) Section::Count> sections_{};
    std::array<TextButton*, (size_t) Page::Count> workflowButtons_{};
    std::array<TextButton*, 4> envelopeButtons_{};
    std::array<TextButton*, 10> lfoButtons_{};
    std::vector<Widget*> modHandles_;
    devices::WavetableSynth::LiveState live_{}; // refreshed every paint: animated knobs, curves and playheads (UI-28c)
    std::array<float, 2> meter_{};
    float liveAmount(int destination) const;     // Σ amount × live route value into the destination, 0 when idle // index = source (null where a source has no handle)
    TextButton *keysButton_ = nullptr, *arpButton_ = nullptr;
    std::array<TextButton*, 3> voiceTabs_{};
    std::array<TextButton*, 3> lfoModes_{};          // FREE / RETRIG / ENVELOPE (the LFO Trigger parameter)
    std::array<Widget*, 10> lfoDivisionKnobs_{};     // RATE in BPM mode turns the tempo division
    int voiceTab_ = 0, curveDrag_ = -1;
    struct EnvelopeShape { float delay = 0, attack = 0, hold = 0, decay = 0, release = 0, sustain = 0, start = 0, end = 0, sustaining = 0, total = 1; bool imported = false; };
    EnvelopeShape envelopeShape() const;
    std::pair<float, float> envelopeAt(const EnvelopeShape&, float seconds, float level) const; // plot coordinates
    int envelopeDrag_ = -1;
    float envelopeFrozenTotal_ = 0, envelopeFrozenSustaining_ = 0, envelopeDragX_ = 0, envelopeDragY_ = 0, envelopeDragValue_ = 0, envelopeCurveSign_ = 1;
    RingTip ringTip_;
    bool arpView_ = false;
    int modDragSource_ = -1, modDragTarget_ = -1, wheelDrag_ = -1; // wheelDrag_: 0 pitch bend, 1 mod wheel
    float modDragX_ = 0, modDragY_ = 0;
    std::array<TextButton*, 3> oscillatorMore_{};
    std::array<TextButton*, 3> mediaImport_{};
    std::array<int, 3> mediaModes_{};
    std::array<TextButton*, 2> filterMore_{};
    std::array<TextButton*, 3> fxBusButtons_{}, fxBandButtons_{}, mixOpenButtons_{};
    TextButton *fxAdd_ = nullptr, *fxRemove_ = nullptr, *fxUp_ = nullptr, *fxDown_ = nullptr;
    TextButton *fxChainPrevious_ = nullptr, *fxChainNext_ = nullptr, *fxControlsPrevious_ = nullptr, *fxControlsNext_ = nullptr;
    TextButton *fxImpulse_ = nullptr, *fxEnableBuses_ = nullptr;
    Effects::Configuration fxConfiguration_;
    std::vector<FxNode> fxNodes_;
    std::vector<Effects::ModuleId> fxInspectorIds_;
    Effects::ModuleId fxSelected_ = 0;
    int fxBus_ = 0, fxBand_ = -1, fxChainPage_ = 0, fxControlsPage_ = 0, fxRows_ = 1, fxControlsPages_ = 1;
    bool fxViewLoaded_ = false;
    WavetableModulationEditor* modulationEditor_ = nullptr;
    bool modulationViewLoaded_ = false, matrixCompact_ = true;
    WavetableRoutingEditor* routingEditor_ = nullptr;
    TextButton *mixSourcesButton_ = nullptr, *mixBusesButton_ = nullptr;
    bool mixSources_ = true, routingViewLoaded_ = false, mixCompact_ = true;
    Rect fxChain_, fxDetail_, fxControlsArea_, fxGraphic_;
    // Serum-style rack (UI-28f): every module of the bus as a stacked horizontal panel; fxControlsPage_ is the first panel shown.
    struct FxPanel { Effects::ModuleId id = 0; Rect bounds, graphic, mode, more; bool collapsed = false; };
    struct FxRow { Effects::ModuleId id = 0; Rect hide, remove; };
    std::vector<FxRow> fxRows;
    void toggleFxCollapsed(Effects::ModuleId);
    void deleteFxModule(Effects::ModuleId);
    void paintFxGraph(SkCanvas&, const FxNode&, Rect);
    std::vector<FxPanel> fxPanels_;
    Effects::ModuleId fxScrolledFor_ = 0;
    TextButton *previous_ = nullptr, *selector_ = nullptr, *next_ = nullptr;
    TextButton *pagePrevious_ = nullptr, *pageNext_ = nullptr, *inspectButton_ = nullptr;
    TextButton *presetPrevious_ = nullptr, *presetNext_ = nullptr, *presetButton_ = nullptr;
    TextButton *presetGroupPrevious_ = nullptr, *presetGroupNext_ = nullptr, *presetMore_ = nullptr;
    WavetablePresetBrowser* presetBrowser_ = nullptr;
    // False once this editor is destroyed. A preset load can rebuild (delete) the editor from inside the browser's
    // callback; an address check on the old browser can be fooled when the allocator reuses it for the new one.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    bool presetBrowserRestored_ = false, libraryGroups_ = false;
    void storePresetBrowser();
    void layoutPresetBrowser();
    TextButton *envelopeMore_ = nullptr, *lfoMore_ = nullptr, *macroMore_ = nullptr, *subRouting_ = nullptr, *voiceQuality_ = nullptr;
    TextButton *noiseAsset_ = nullptr, *noiseImport_ = nullptr, *noiseMore_ = nullptr;
    Widget* master_ = nullptr;
    std::vector<Control> controls_;
    std::vector<std::vector<Placement>> pages_;
    Rect body_, controlsArea_, upper_, lower_, footer_, sub_, subPlot_, noise_, noisePlot_, macroPanel_, envelopePanel_, lfoPanel_, voicePanel_;
    struct NoisePeak { std::array<float, 2> minimum{}, maximum{}; };
    std::array<NoisePeak, 256> noisePeaks_{};
    float noisePreviewGain_ = 1;
    std::array<Rect, 3> oscillatorPanels_{};
    std::array<Rect, 2> filterPanels_{};
    int page_ = 0, columns_ = 1, preset_ = 0, auditionNote_ = -1, dragOscillator_ = -1;
    float rowHeight_ = 60;
    bool inspector_ = false, forcedCompact_ = false, presetEdited_ = false;
    std::vector<float> presetValues_;
    std::string presetLabel_ = "Init";
    dsp::CurveGraph lfoDraft_;
    int dragLfoPoint_ = -1;
    bool dragLfoCurve_ = false;
};
} // namespace sonora::ui
