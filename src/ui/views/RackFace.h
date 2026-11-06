#pragma once
// UI-66: the rack face (Instrument / Audio Effect / Drum Rack) matched to a Live 12 capture of an Instrument Rack
// ("Ac Piano Upright", 996 x 394 retina crop, body 338 px from y 46): the left strip of round view buttons, the macro
// boxes (coloured name header, knob, value in the mapped parameter's own units) and the chain list (chain colour and
// name, volume, pan, activator, solo, meter) with the Key / Vel / Chain zone editors. Drum Racks keep their pad grid
// (NativeDevicePanel) where the chain list goes. Rand and Map sit in the device title (DetailView's DeviceBox).

#include "ui/views/ReferenceFace.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sonora::devices { class RackProcessor; }
namespace sonora::ui
{
class NativeDevicePanel;
class RackMacroBox;

// The value text of macro `macro`: the mapped parameter's own display ("22.0 kHz", "600 ms", "14 %") when exactly one
// continuous parameter of the rack's chains follows the macro; otherwise the macro's own 0..127 value ("72"), as the
// reference shows a macro with several (or no) mappings. On/off switches mapped alongside do not count.
std::string macroValueText(const model::Device& rack, const devices::RackProcessor& processor, int macro);
// Any device parameter's value text as Sonora's face of that device shows it.
std::string deviceParameterText(devices::DeviceProcessor*, int parameter);

class RackFace final : public ReferenceFace
{
public:
    enum class Strip { Macros, MoreMacros, FewerMacros, Variations, Devices, ChainList };
    enum class Zones { Hide, Key, Velocity, Chain };

    RackFace(const model::Device& model, devices::RackProcessor* rack, int track, int device, int chain, int child, Tag tag);

    const model::Device& model() const { return model_; }
    devices::RackProcessor* rack() const { return rack_; }
    bool drums() const { return model_.type == "DrumGroupDevice"; }

    // View switches (UiContext::rackViews; Devices is "this rack's chain is shown in the device chain").
    bool macrosShown() const;
    bool chainsShown() const;
    bool devicesShown() const;
    Zones zones() const;
    bool mapping() const; // Map mode on for this rack
    int visibleMacros() const;
    int selectedChain() const;

    // Title actions (DeviceBox's Rand / Map buttons). `seed` makes Rand reproducible in tests; 0 = random.
    void randomizeMacros(uint32_t seed = 0);
    void toggleMapping();

    // Geometry, local coordinates (tests drive the face through these).
    Rect stripBounds(Strip) const;
    Rect macroBounds(int macro) const;
    Rect zoneTabBounds(Zones) const;
    Rect autoSelectBounds() const;
    Rect chainListBounds() const;
    Rect chainRowBounds(int chain) const;
    Rect chainNameBounds(int chain) const;
    Rect volumeBounds(int chain) const;
    Rect panBounds(int chain) const;
    Rect speakerBounds(int chain) const;
    Rect soloBounds(int chain) const;
    Rect hotSwapBounds(int chain) const;
    Rect meterBounds(int chain) const;
    Rect zoneRulerBounds() const;           // the zone editor's ruler (Chain: the selector's track)
    Rect zoneBarBounds(int chain) const;    // a chain's zone, as drawn
    Rect titleButtonsCapture() const;       // where Rand / Map go in the title, local x (y is the title's)
    NativeDevicePanel* drumPanel() const { return drumPanel_; }

    float macroText() const { return 22.0f * unit(); } // macro names / values and chain-list text (capture px 22)
    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    void mouseDoubleClick(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override;

    // Capture x of the chain area's left edge and the face's right edge for the current switches.
    float chainLeft() const;
    float captureRight() const;

private:
    friend class RackMacroBox;
    enum class Drag { None, Volume, Pan, ZoneMin, ZoneMax, ZoneMove, Selector };
    void edit(const std::function<void(model::Device&)>& operation, const std::string& label);
    void updateWidth();
    void setVolume(int chain, float gain);
    void setPan(int chain, float pan);
    float chainVolume(int chain) const;
    float chainPan(int chain) const;
    bool chainActive(int chain) const;
    bool chainSolo(int chain) const;
    std::pair<int, int> zoneOf(int chain) const; // min, max of the shown zone (Key, Velocity or Chain selector range)
    float zoneX(float value) const;               // zone value 0..127 -> local x on the ruler
    float zoneValueAt(float x) const;
    int zoneSteps(float x) const; // zone steps the pointer moved since the press
    int rowAt(float y) const;
    void paintStrip(SkCanvas&, const Theme&) const;
    void paintChains(SkCanvas&, const Theme&) const;
    void paintZones(SkCanvas&, const Theme&) const;
    void selectChain(int chain);

    const model::Device& model_;
    devices::RackProcessor* rack_;
    int track_, device_, chain_, child_;
    std::vector<RackMacroBox*> macros_;
    NativeDevicePanel* drumPanel_ = nullptr;
    // Gesture in progress
    Drag drag_ = Drag::None;
    int dragChain_ = -1;
    float dragStartX_ = 0, dragStartY_ = 0, dragStartValue_ = 0;
    int previewMin_ = 0, previewMax_ = 127, dragStartHi_ = 127; // zone preview while dragging; committed on release
    std::vector<std::array<float, 2>> meterPeak_, meterRms_;
};
} // namespace sonora::ui
