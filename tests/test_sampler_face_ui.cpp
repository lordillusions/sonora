// DEV-14b / UI-43k: the Sampler's reference face (SamplerFace.cpp) and its zone editor (SamplerZoneEditor.cpp): measured
// width, the .als paths bound to the processor, tab switching saved in ViewSettings/SelectedPage, every control inside the
// face on every tab, gestures through real input dispatch (live parameters, model values as one undo step), and zone
// moves / resizes / fades / selector in the zone editor.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/Sampler.h"
#include "devices/native/Simpler.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/MainLayout.h"
#include "ui/views/SamplerFace.h"

#include <juce_core/juce_core.h>

#include <map>
#include <set>

using namespace sonora;
using S = devices::Simpler;

namespace
{
ui::MouseEvent point(float x, float y)
{
    ui::MouseEvent e;
    e.x = x;
    e.y = y;
    return e;
}
ui::MouseEvent centre(const ui::Widget* w)
{
    const auto r = w->boundsInRoot();
    return point(r.x + r.w * 0.5f, r.y + r.h * 0.5f);
}
void click(ui::RootWidget& root, ui::MouseEvent e)
{
    root.dispatchMouseDown(e);
    root.dispatchMouseUp(e);
}
void drag(ui::RootWidget& root, ui::MouseEvent from, float dx, float dy)
{
    root.dispatchMouseDown(from);
    auto to = from;
    to.x += dx;
    to.y += dy;
    root.dispatchMouseDrag(to);
    root.dispatchMouseUp(to);
}
ui::Widget* withTooltip(ui::Widget& parent, const std::string& tooltip)
{
    for (const auto& child : parent.children())
    {
        if (child->tooltip == tooltip) return child.get();
        if (auto* found = withTooltip(*child, tooltip)) return found;
    }
    return nullptr;
}
// The visible child of `face` under a capture point (the face is laid out at the capture's scale: body 342, origin 18 / 44).
ui::Widget* under(ui::Widget& face, float captureX, float captureY)
{
    const float x = captureX - 18, y = captureY - 44;
    for (const auto& child : face.children())
        if (child->visible && child->bounds().contains(x, y)) return child.get();
    return nullptr;
}

// A Sampler with three velocity layers like the reference's "Lovely Keys" (generated: no reference preset is copied).
struct Rig
{
    model::Song song;
    std::unique_ptr<devices::DeviceProcessor> processor;
    ui::UiContext context;
    std::unique_ptr<ui::RootWidget> root;
    ui::ReferenceFace* face = nullptr;
    std::map<int, ui::Widget*> tagged;
    int edits = 0;
    std::vector<std::string> labels;
    std::function<void(int)> pick;

    Rig()
    {
        auto sampler = devices::nativeDeviceModel("MultiSampler");
        sampler.id = 7;
        const int vel[][2] = {{1, 74}, {75, 99}, {100, 127}};
        for (int i = 0; i < 3; ++i)
        {
            model::SamplePart part;
            part.name = "Layer " + std::to_string(i + 1);
            part.velMin = vel[i][0];
            part.velMax = vel[i][1];
            part.rootKey = 53;
            part.sampleRate = 48000;
            part.lengthSamples = part.sampleEnd = 96000;
            part.loopStart = 8000;
            part.loopEnd = 24000;
            sampler.sampleParts.push_back(part);
        }
        sampler.samplePartsEdited = true;
        model::Track track;
        track.name = "Keys";
        track.devices = {sampler};
        song.tracks.push_back(track);
        devices::BuildContext build;
        processor = devices::createDevice(song.tracks[0].devices[0], build);
        context.song = &song;
        context.selectedTrack = 0;
        context.showChoices = [this](const auto&, const auto&, auto chosen) { pick = chosen; };
        context.editDevice = [this](int t, int d, const std::function<void(model::Device&)>& fn, const std::string& label) {
            fn(song.tracks[(size_t) t].devices[(size_t) d]);
            ++edits;
            labels.push_back(label);
        };
        context.setDeviceViewValue = [this](int t, int d, int, int, const std::string& key, double value) {
            song.tracks[(size_t) t].devices[(size_t) d].plain[key] = value;
        };
        root = std::make_unique<ui::RootWidget>(context);
        face = ui::addSamplerFace(*root, song.tracks[0].devices[0], processor.get(), {0, 0, -1, -1},
                                  [this](ui::Widget* w, int parameter) { tagged.emplace(parameter, w); });
    }
    model::Device& device() { return song.tracks[0].devices[0]; }
    void layOut()
    {
        const float width = face->preferredWidth(342);
        root->setBounds({0, 0, width, 18 + 342});
        face->setBounds({0, 18, width, 342});
        face->layout();
    }
    int index(const std::string& id) const
    {
        const auto& infos = processor->parameters();
        for (size_t i = 0; i < infos.size(); ++i)
            if (infos[i].id == id) return (int) i;
        return -1;
    }
};
} // namespace

TEST_CASE("Sampler face: measured width, bound paths, tabs saved, every control inside the face, PNGs")
{
    Rig rig;
    REQUIRE(rig.face != nullptr);
    CHECK_NEAR(rig.face->preferredWidth(342), 1412, 0.01); // the capture's device box: x 18..1430 for a 342-px body
    rig.layOut();

    // Every Sampler path the processor knows and the reference's face shows is a live, mappable control.
    const char* live[] = {"VolumeAndPan/Volume", "VolumeAndPan/VolumeVelScale", "VolumeAndPan/Panorama", "VolumeAndPan/PanoramaRnd",
                          "VolumeAndPan/Envelope/AttackTime", "VolumeAndPan/Envelope/DecayTime", "VolumeAndPan/Envelope/SustainLevel",
                          "VolumeAndPan/Envelope/ReleaseTime", "VolumeAndPan/Envelope/AttackSlope", "VolumeAndPan/Envelope/DecaySlope",
                          "VolumeAndPan/Envelope/ReleaseSlope", "VolumeAndPan/Envelope/LoopMode", "VolumeAndPan/Envelope/LoopTime",
                          "Filter/IsOn", "Filter/Slot/Value/SimplerFilter/Type", "Filter/Slot/Value/SimplerFilter/Freq",
                          "Filter/Slot/Value/SimplerFilter/Res", "Filter/Slot/Value/SimplerFilter/Slope", "Filter/Slot/Value/SimplerFilter/Drive",
                          "Filter/Slot/Value/SimplerFilter/ModByPitch", "Filter/Slot/Value/SimplerFilter/ModByVelocity",
                          "Filter/Slot/Value/SimplerFilter/ModByLfo", "Filter/Slot/Value/SimplerFilter/CircuitLpHp",
                          "Filter/Slot/Value/SimplerFilter/CircuitBpNoMo", "Filter/Slot/Value/SimplerFilter/X",
                          "Filter/Slot/Value/SimplerFilter/Envelope/IsOn", "Filter/Slot/Value/SimplerFilter/Envelope/Amount",
                          "Filter/Slot/Value/SimplerFilter/Envelope/AttackTime", "Filter/Slot/Value/SimplerFilter/Envelope/DecayTime",
                          "Filter/Slot/Value/SimplerFilter/Envelope/SustainLevel", "Filter/Slot/Value/SimplerFilter/Envelope/ReleaseTime",
                          "Pitch/TransposeKey", "Pitch/TransposeFine", "Pitch/PitchLfoAmount", "Pitch/Envelope/IsOn",
                          "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/Amount", "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/AttackTime",
                          "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/DecayTime", "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/SustainLevel",
                          "Pitch/Envelope/Slot/Value/SimplerPitchEnvelope/ReleaseTime", "Lfo/IsOn", "Lfo/Slot/Value/SimplerLfo/Type",
                          "Lfo/Slot/Value/SimplerLfo/Frequency", "Lfo/Slot/Value/SimplerLfo/RateType", "Lfo/Slot/Value/SimplerLfo/BeatRate",
                          "Lfo/Slot/Value/SimplerLfo/Attack", "Lfo/Slot/Value/SimplerLfo/Retrigger", "Lfo/Slot/Value/SimplerLfo/Offset",
                          "Lfo/Slot/Value/SimplerLfo/FrequencyKeyScale", "VolumeAndPan/VolumeLfoAmount", "VolumeAndPan/PanoramaLfoAmount",
                          "Globals/KeyZoneShift", "Globals/SpreadAmount", "Globals/PortamentoMode", "Globals/PortamentoTime", "Player/Snap",
                          "Player/InterpolationMode"};
    for (const auto* id : live)
    {
        const int i = rig.index(id);
        if (i < 0 || ! rig.tagged.contains(i)) { std::printf("    not live: %s\n", id); CHECK(false); }
    }

    // Every Sampler-only engine parameter is a live control of the face, except what the reference's face does not show
    // there: legacy filter values, LFO smoothing / width, LFO 1's stereo settings, the volume envelope's End (dragged on its
    // display), the attack switch of the envelope time scaling, and the zone editor's own controls (live there).
    const std::set<std::string> elsewhere{"VolumeAndPan/Envelope/ReleaseLevel", "Filter/Slot/Value/SimplerFilter/LegacyType",
                                          "Filter/Slot/Value/SimplerFilter/LegacyQ", "Lfo/Slot/Value/SimplerLfo/StereoMode",
                                          "Lfo/Slot/Value/SimplerLfo/Spin", "Lfo/Slot/Value/SimplerLfo/Phase", "Lfo/Slot/Value/SimplerLfo/Smooth",
                                          "Lfo/Slot/Value/SimplerLfo/Width", "AuxLfos.0/Slot/Value/SimplerAuxLfo/Smooth",
                                          "AuxLfos.0/Slot/Value/SimplerAuxLfo/Width", "AuxLfos.1/Slot/Value/SimplerAuxLfo/Smooth",
                                          "AuxLfos.1/Slot/Value/SimplerAuxLfo/Width", "Player/SampleSelector", "Player/MultiSampleMap/LayerCrossfade",
                                          "Player/MultiSampleMap/RoundRobin", "Player/MultiSampleMap/RoundRobinMode",
                                          "Player/MultiSampleMap/RoundRobinResetPeriod", "Globals/EnvScale/EnvTimeIncludeAttack"};
    for (size_t i = (size_t) S::NumParams; i < rig.processor->parameters().size(); ++i)
    {
        const auto& id = rig.processor->parameters()[i].id;
        if (! rig.tagged.contains((int) i) && ! elsewhere.contains(id)) { std::printf("    not live on the face: %s\n", id.c_str()); CHECK(false); }
    }

    // Tabs: the set's page opens; a click on a title tab shows it and writes ViewSettings/SelectedPage (no undo step).
    CHECK(ui::samplerFacePage(*rig.face) == 0);
    const char* tabs[] = {"Sample", "Pitch/Osc", "Filter/Global", "Modulation", "MIDI"};
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    folder.createDirectory();
    for (int page = 4; page >= 0; --page)
    {
        auto* tab = withTooltip(*rig.root, std::string("Show the ") + tabs[page] + " tab");
        REQUIRE(tab != nullptr);
        click(*rig.root, centre(tab));
        CHECK(ui::samplerFacePage(*rig.face) == page);
        CHECK_NEAR(rig.device().plain["ViewSettings/SelectedPage"], page, 0);
        rig.face->layout();
        int shown = 0;
        for (const auto& child : rig.face->children())
        {
            if (! child->visible) continue;
            ++shown;
            const auto b = child->bounds();
            CHECK(b.w > 0);
            CHECK(b.h > 0);
            CHECK(b.x >= 0);
            CHECK(b.y >= 0);
            CHECK(b.right() <= rig.face->width() + 0.5f);
            CHECK(b.bottom() <= rig.face->height() + 0.5f);
        }
        CHECK(shown >= (page == 4 ? 38 : 20));
        REQUIRE(gfx::renderToPng(*rig.root, (int) rig.root->width(), (int) rig.root->height(), 1,
                                 folder.getChildFile(juce::String("Sampler-") + juce::String(page) + ".png").getFullPathName().toStdString()));
    }
    CHECK(rig.edits == 0); // tab switches are view settings, never undo steps

    // A face built for a set saved on the MIDI tab opens on it.
    rig.device().plain["ViewSettings/SelectedPage"] = 4;
    ui::RootWidget other(rig.context);
    auto* reopened = ui::addSamplerFace(other, rig.device(), rig.processor.get(), {0, 0, -1, -1}, {});
    CHECK(ui::samplerFacePage(*reopened) == 4);
}

TEST_CASE("Sampler face: live parameters drag in place, model values and layer values are one undo step each")
{
    Rig rig;
    rig.layOut();
    auto& p = *rig.processor;

    // Filter / Global: Volume (a processor parameter) follows the drag, no model edit.
    ui::selectSamplerFacePage(*rig.face, 2);
    rig.face->layout();
    const float before = p.getParameter(S::Volume);
    auto* volume = rig.tagged.at(S::Volume);
    REQUIRE(volume->visible);
    drag(*rig.root, centre(volume), 0, -40);
    CHECK(p.getParameter(S::Volume) > before + 5);
    CHECK(rig.edits == 0);

    // The filter display drags cutoff and resonance once the filter is on.
    p.setParameter(S::FilterOn, 1);
    p.setParameter(S::FilterFreq, 22000);
    auto* display = withTooltip(*rig.face, "Drag: cutoff (left / right) and resonance (up / down)");
    REQUIRE(display != nullptr);
    drag(*rig.root, centre(display), -60, -30);
    CHECK(p.getParameter(S::FilterFreq) < 2000);
    CHECK(p.getParameter(S::FilterRes) > 0.2f);

    // Voices (a plain Globals leaf the processor holds) picks from the chooser.
    auto* voices = under(*rig.face, 1350, 363);
    REQUIRE(voices != nullptr);
    click(*rig.root, centre(voices));
    REQUIRE(rig.pick);
    rig.pick(9);
    CHECK_NEAR(p.getParameter(S::Voices), 9, 0);

    // Sample tab: Root belongs to the selected layer: one drag = one model edit of that layer.
    ui::selectSamplerFacePage(*rig.face, 0);
    rig.face->layout();
    auto* root = under(*rig.face, 162, 363);
    REQUIRE(root != nullptr);
    drag(*rig.root, centre(root), 0, -40); // 4 px per semitone
    CHECK(rig.edits == 1);
    CHECK(rig.device().sampleParts[0].rootKey == 63);
    CHECK(rig.device().samplePartsEdited);
    CHECK(rig.labels.back() == "Change Sampler RootKey");

    // The Sample chooser selects another layer; its values show and edit that layer.
    auto* layer = withTooltip(*rig.face, "Sample layer shown on this tab");
    REQUIRE(layer != nullptr);
    click(*rig.root, centre(layer));
    REQUIRE(rig.pick);
    rig.pick(2);
    CHECK(ui::sampler::selectedPart(rig.context, rig.device()) == 2);
    rig.face->layout();
    drag(*rig.root, centre(under(*rig.face, 162, 363)), 0, 20);
    CHECK(rig.edits == 2);
    CHECK(rig.device().sampleParts[2].rootKey == 48);
    CHECK(rig.device().sampleParts[0].rootKey == 63);

    // Sustain Mode (a layer value) and Scale (kept in the layer's XML).
    click(*rig.root, centre(under(*rig.face, 693, 307)));
    CHECK(rig.device().sampleParts[2].loopMode == 1);
    drag(*rig.root, centre(under(*rig.face, 330, 363)), 0, 40);
    CHECK_NEAR(ui::sampler::partValue(rig.device().sampleParts[2], "TuneScale", 100), 90, 0);
    CHECK(rig.edits == 4);

    // The Sampler engine's own controls (Osc, matrix) are live too: no model edit, no undo step while dragging.
    ui::selectSamplerFacePage(*rig.face, 1);
    rig.face->layout();
    const int edits = rig.edits;
    click(*rig.root, centre(under(*rig.face, 75, 69))); // Osc on
    CHECK_NEAR(p.getParameter(devices::Sampler::OscOn), 1, 0);
    rig.face->layout();
    auto* osc = under(*rig.face, 1229, 109);
    REQUIRE(osc != nullptr);
    CHECK(rig.tagged.contains(devices::Sampler::OscVolume));
    const float oscBefore = p.getParameter(devices::Sampler::OscVolume);
    drag(*rig.root, centre(osc), 0, 30);
    CHECK(p.getParameter(devices::Sampler::OscVolume) < oscBefore);

    // MIDI tab: a destination chooser sets the matrix connection.
    ui::selectSamplerFacePage(*rig.face, 4);
    rig.face->layout();
    click(*rig.root, centre(under(*rig.face, 295, 71)));
    REQUIRE(rig.pick);
    rig.pick(4);
    CHECK_NEAR(p.getParameter(devices::Sampler::KeyMod + devices::Sampler::ModDst0), 4, 0);
    CHECK(rig.edits == edits);
}

TEST_CASE("Sampler zone editor: Key / Vel / Sel tabs, zone move, resize and fade drags, layer select, solo, selector")
{
    Rig rig;
    rig.device().plain["ViewSettings/ZoneEditorVisible"] = 1;
    auto target = ui::sampler::zoneEditorTarget(rig.context);
    REQUIRE(target.device == &rig.device());
    ui::RootWidget root(rig.context);
    auto* zones = root.add<ui::SamplerZoneEditor>();
    root.setBounds({0, 0, 1200, ui::SamplerZoneEditor::kHeight});
    zones->setBounds({0, 0, 1200, ui::SamplerZoneEditor::kHeight});
    const auto at = [&](ui::Rect r, float fx = 0.5f) { return point(r.x + r.w * fx, r.y + r.h * 0.5f); };

    // Key tab: every layer spans the keyboard.
    CHECK_NEAR(zones->zoneBar(0).w, 513, 0.01);
    click(root, at(zones->tabBounds(1)));
    CHECK(rig.context.samplerViews[7].zoneTab == 1);
    const float cell = zones->valueX(1) - zones->valueX(0);

    // Move layer 2 (75..99) five velocities up: one undo step.
    drag(root, at(zones->zoneBar(1)), 5 * cell, 0);
    CHECK(rig.edits == 1);
    CHECK(rig.device().sampleParts[1].velMin == 80);
    CHECK(rig.device().sampleParts[1].velMax == 104);
    CHECK(rig.labels.back() == "Change Velocity Zone");
    // Resize layer 3's top end down by three.
    const auto top = zones->zoneBar(2);
    drag(root, point(top.right() - 1, top.centreY()), -3 * cell, 0);
    CHECK(rig.device().sampleParts[2].velMax == 124);
    CHECK(rig.device().sampleParts[2].velMin == 100);
    // Fade layer 1 in over ten velocities (the crossfade bar's left end).
    const auto fade = zones->fadeBar(0);
    drag(root, point(fade.x + 1, fade.centreY()), 10 * cell, 0);
    const auto range = ui::sampler::zoneRange(rig.device().sampleParts[0], ui::sampler::Axis::Velocity);
    CHECK(range.min == 1);
    CHECK(range.fadeMin == 11);
    CHECK(range.fadeMax == 74);
    CHECK(rig.edits == 3);
    // A click that does not move changes nothing.
    click(root, at(zones->zoneBar(1)));
    CHECK(rig.edits == 3);

    // Selecting a layer in the list selects it for the Sample tab; S solos it alone.
    click(root, point(100, zones->rowBounds(1).centreY()));
    CHECK(ui::sampler::selectedPart(rig.context, rig.device()) == 1);
    const auto row = zones->rowBounds(2);
    click(root, point(zones->valueX(0) - 13, row.centreY()));
    CHECK_NEAR(ui::sampler::partValue(rig.device().sampleParts[2], "Solo", 0), 1, 0);
    CHECK_NEAR(ui::sampler::partValue(rig.device().sampleParts[0], "Solo", 0), 0, 0);

    // Sel tab: the selector line drags Player/SampleSelector; a selector zone edit keeps the other axes.
    click(root, at(zones->tabBounds(2)));
    drag(root, point(zones->valueX(10) + 1, 10), 30 * cell, 0);
    CHECK_NEAR(ui::sampler::deviceValue(rig.device(), "Player/SampleSelector", 0), 40, 1);
    drag(root, point(zones->zoneBar(0).right() - 1, zones->zoneBar(0).centreY()), -64 * cell, 0);
    const auto selector = ui::sampler::zoneRange(rig.device().sampleParts[0], ui::sampler::Axis::Selector);
    CHECK(selector.max == 63);
    CHECK(ui::sampler::zoneRange(rig.device().sampleParts[0], ui::sampler::Axis::Velocity).fadeMin == 11);
    REQUIRE(gfx::renderToPng(root, 1200, (int) ui::SamplerZoneEditor::kHeight, 1,
                             juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces/Sampler-zones.png")
                                 .getFullPathName().toStdString()));

    // The main layout shows the panel above the device view while the Zone tab is open, and hides it with the tab.
    ui::MainLayout main(rig.context);
    main.setBounds({0, 0, 1300, 800});
    main.layout();
    ui::SamplerZoneEditor* shown = nullptr;
    for (const auto& child : main.children())
        if (auto* z = dynamic_cast<ui::SamplerZoneEditor*>(child.get())) shown = z;
    REQUIRE(shown != nullptr);
    CHECK(shown->visible);
    CHECK_NEAR(shown->height(), ui::SamplerZoneEditor::kHeight, 0.01);
    rig.device().plain["ViewSettings/ZoneEditorVisible"] = 0;
    main.layout();
    CHECK(! shown->visible);
}
