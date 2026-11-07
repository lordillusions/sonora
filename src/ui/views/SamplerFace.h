#pragma once
// DEV-14b / UI-43k: the Sampler (`MultiSampler`) face and its zone editor, matched to retina captures of the reference's
// Sampler (Live 12.4.6, "Lovely Keys", theme Default Light Cool High): five tabs in the title band (Sample, Pitch/Osc,
// Filter/Global, Modulation, MIDI) and the Zone tab, which opens the zone editor above the device view.
//
// Every control binds to the parameter's .als path. A path the processor exposes (ParameterInfo::id) is edited live and can
// be mapped / automated; any other path is read from the device model (parameters, plain leaves, or the retained device
// XML) and written back to the model in one undo step when the gesture ends. Per-layer values (root key, loop points,
// zone ranges...) belong to the selected MultiSamplePart. See docs/NATIVE_DEVICE_FACES.md "Sonora Sampler".

#include "core/Model.h"
#include "ui/views/ReferenceFace.h"

#include <string>

namespace sonora::ui
{

// Where a device sits: a track-level device (`chain` < 0) or a child of a rack chain.
struct DeviceLocation
{
    int track = -1, device = -1, chain = -1, child = -1;
};

// The Sampler face for `model` (type MultiSampler with a processor), added to `box` together with its title tabs;
// nullptr for anything else.
ReferenceFace* addSamplerFace(Widget& box, const model::Device& model, devices::DeviceProcessor* processor, DeviceLocation where,
                              ReferenceFace::Tag tag);
// The shown tab of a Sampler face (0 Sample, 1 Pitch/Osc, 2 Filter/Global, 3 Modulation, 4 MIDI); -1 for another widget.
int samplerFacePage(const Widget& face);
void selectSamplerFacePage(Widget& face, int page);

namespace sampler
{
// A plain or Manual value of the device at an .als path ("ViewSettings/SelectedPage", "AuxLfos.1/Slot/Value/SimplerAuxLfo/Type"):
// the model's plain map, then its parameters, then the device XML the set was read from; `fallback` when none has it.
double deviceValue(const model::Device& device, const std::string& path, double fallback);
// A value of a sample layer at a path inside <MultiSamplePart> ("SelectorRange/CrossfadeMin", "TuneScale", "Solo").
// Values the model holds as fields (KeyRange/Min, RootKey, SustainLoop/Start ...) come from the fields.
double partValue(const model::SamplePart& part, const std::string& path, double fallback);
// Writes such a value: model fields where the model has them, the part's retained XML for the rest.
void setPartValue(model::SamplePart& part, const std::string& path, double value);

// A zone range of a layer: Min / Max and the crossfade points (CrossfadeMin >= Min fades in, CrossfadeMax <= Max fades out).
enum class Axis { Key, Velocity, Selector };
struct ZoneRange { int min = 0, max = 127, fadeMin = 0, fadeMax = 127; };
ZoneRange zoneRange(const model::SamplePart& part, Axis axis);
void setZoneRange(model::SamplePart& part, Axis axis, ZoneRange range);
// The layer shown by the Sample tab and highlighted in the zone editor (the view state, else the set's <Selection>).
int selectedPart(const UiContext& ctx, const model::Device& device);

// The Sampler on the selected track whose zone editor is open (ViewSettings/ZoneEditorVisible), if any.
struct ZoneTarget
{
    DeviceLocation where;
    const model::Device* device = nullptr;
};
ZoneTarget zoneEditorTarget(const UiContext& ctx);
} // namespace sampler

// The zone editor panel (MainLayout shows it above the device view while a Sampler's Zone tab is open): the sample layer
// list with speaker / solo, Key / Vel / Sel tabs with their rulers, a zone bar per layer with crossfade bars. Drag a zone to
// move it, its ends to resize, the crossfade bar's ends to fade; each gesture is one undo step.
class SamplerZoneEditor final : public Widget
{
public:
    static constexpr float kHeight = 245.0f; // points: the capture's 490-px panel at 2 px per point
    void paint(SkCanvas&, const Theme&) override;
    void mouseDown(const MouseEvent&) override;
    void mouseDrag(const MouseEvent&) override;
    void mouseUp(const MouseEvent&) override;
    bool mouseWheel(const MouseEvent&) override;
    bool browserDrop(const BrowserDrag&, const MouseEvent&) override; // a dropped sample becomes a new layer
    void layout() override;
    // Geometry for tests: the zone bar of `part` in local coordinates and the ruler's x of a value 0..127.
    Rect zoneBar(int part) const;
    Rect fadeBar(int part) const;
    float valueX(float value) const;
    Rect tabBounds(int tab) const;
    Rect rowBounds(int part) const;

private:
    struct Drag
    {
        enum class What { None, Move, Min, Max, FadeMin, FadeMax, Selector } what = What::None;
        int part = -1;
        float startX = 0;
        sampler::ZoneRange start;
        sampler::ZoneRange current;
        double selector = 0;
    };
    sampler::ZoneTarget target() const;
    // The engine's parameter at an .als path (track-level Samplers: the selector, round robin, crossfade law) for live
    // edits, else nullptr (the value is then edited in the model, one undo step).
    devices::DeviceProcessor* processor(int& index, const std::string& path) const;
    double value(const model::Device&, const std::string& path, double fallback) const;
    void setValue(const std::string& path, double value, const std::string& label);
    void edit(const std::function<void(model::Device&)>& operation, const std::string& label);
    void editPart(int part, const std::function<void(model::SamplePart&)>& operation, const std::string& label);
    sampler::Axis axis() const;
    sampler::ZoneRange shown(int part, const model::SamplePart&) const;
    float zoneLeft() const;
    int tab() const;
    void setTab(int tab);
    Drag drag_;
    double pendingSelector_ = -1;
};

} // namespace sonora::ui
