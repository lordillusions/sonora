// UI-66: the rack face (macro boxes, chain list, zone editors, Rand / Map) and the chain mixer behind it.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/MainLayout.h"
#include "ui/views/RackFace.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"

#include <juce_core/juce_core.h>

using namespace sonora;
namespace
{
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}
ui::MouseEvent at(ui::Widget& widget, ui::Rect local)
{
    ui::MouseEvent e;
    const auto origin = widget.boundsInRoot();
    e.x = origin.x + local.centreX();
    e.y = origin.y + local.centreY();
    return e;
}
void click(ui::Widget& root, ui::MouseEvent e)
{
    auto& r = static_cast<ui::RootWidget&>(root);
    r.dispatchMouseDown(e);
    r.dispatchMouseUp(e);
}
model::Parameter mapped(std::string path, int macro, double low, double high, double value)
{
    model::Parameter p;
    p.path = std::move(path);
    p.value = value;
    p.macroIndex = macro;
    p.rangeMin = low;
    p.rangeMax = high;
    p.midi.channel = 16;
    p.midi.number = macro;
    return p;
}
// Replaces the device's entry for that path (new native devices list their parameters) or adds it.
void put(model::Device& device, model::Parameter parameter)
{
    for (auto& p : device.parameters)
        if (p.path == parameter.path) { p = parameter; return; }
    device.parameters.push_back(std::move(parameter));
}
// The owner's "Ac Piano Upright" mappings: Simpler filter, LFO rate and release; two shelves on Tone;
// Reverb's dry/wet together with its On switch on Reverb.
model::Device pianoRack()
{
    auto rack = devices::nativeDeviceModel("InstrumentGroupDevice");
    rack.id = 77;
    rack.macroNames = {"Filter Cutoff", "Filter Reso", "Tremolo Rate", "Tremolo Amount", "Attack", "Release", "Tone", "Reverb"};
    rack.macroColors = {27, 27, 41, 41, 55, 55, 69, 69, -1, -1, -1, -1, -1, -1, -1, -1};
    rack.macroNoRandom[7] = true;
    rack.chainListVisible = true;
    const double values[8] = {127, 0, 100.645088, 0, 0, 93.3856735, 72, 17.7800007};
    rack.numVisibleMacros = 8; // a new rack shows 16
    for (int i = 0; i < 8; ++i)
    {
        const auto path = "MacroControls." + std::to_string(i);
        auto found = std::find_if(rack.parameters.begin(), rack.parameters.end(), [&](const auto& p) { return p.path == path; });
        if (found == rack.parameters.end())
        {
            rack.parameters.emplace_back();
            found = rack.parameters.end() - 1;
            found->path = path;
        }
        found->value = values[i];
    }
    auto simpler = devices::nativeDeviceModel("OriginalSimpler");
    put(simpler, mapped("Filter/Slot/Value/SimplerFilter/Freq", 0, 30, 22000, 22000));
    put(simpler, mapped("Lfo/Slot/Value/SimplerLfo/Frequency", 2, 2, 8, 6));
    put(simpler, mapped("VolumeAndPan/Envelope/ReleaseTime", 5, 1, 6000, 600));
    auto eq = devices::nativeDeviceModel("ChannelEq");
    put(eq, mapped("LowShelfGain", 6, 5.6, 0.18, 1));
    put(eq, mapped("HighShelfGain", 6, 0.18, 5.6, 1));
    auto reverb = devices::nativeDeviceModel("Reverb");
    auto on = mapped("On", 7, 1, 127, 1);
    on.booleanValue = true;
    put(reverb, on);
    put(reverb, mapped("MixDirect", 7, 0, 1, 0.14));
    model::Device::Chain chain;
    chain.name = "Piano Upright";
    chain.color = 19;
    chain.devices = {simpler, eq, reverb};
    rack.chains = {chain};
    model::Device::Chain second;
    second.name = "Pad";
    rack.chains.push_back(second);
    rack.chainsEdited = true;
    return rack;
}
} // namespace

TEST_CASE("rack face: macros show the mapped parameter's value text, several mappings show 0..127 (UI-66)")
{
    auto rack = pianoRack();
    engine::SampleCache samples;
    devices::BuildContext build;
    auto processor = devices::createDevice(rack, build);
    REQUIRE(processor);
    auto* r = dynamic_cast<devices::RackProcessor*>(processor.get());
    REQUIRE(r);
    r->prepare(48000, 256);
    CHECK(ui::macroValueText(rack, *r, 0) == "22.0 kHz"); // one mapping: the Simpler's own display
    CHECK(ui::macroValueText(rack, *r, 2) == "6.00 Hz");  // log law between 2 and 8 Hz at 100.6 / 127
    CHECK(ui::macroValueText(rack, *r, 5) == "600 ms");   // times sweep on a log law too: 1..6000 ms at 93.4 reads 600 ms
    CHECK(ui::macroValueText(rack, *r, 6) == "72");       // two shelves (both Channel EQ gains): the macro's value
    CHECK(ui::macroValueText(rack, *r, 7) == "14 %");     // the device On switch next to Dry/Wet does not count
    CHECK(ui::macroValueText(rack, *r, 9) == "0");        // unmapped
    r->setParameter(0, 0.0f);
    CHECK(ui::macroValueText(rack, *r, 0) == "30.0 Hz");
}

TEST_CASE("rack face: chain mixer solo, activator and volume act on the audio and are captured into the set (UI-66)")
{
    std::vector<devices::RackProcessor::Chain> chains(2);
    devices::RackProcessor rack("r", false, false, std::move(chains)); // effect rack: empty chains pass the input
    rack.prepare(48000, 64);
    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    const auto run = [&] {
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 64; ++i) audio.setSample(ch, i, 0.25f);
        rack.process(audio, midi);
        return audio.getSample(0, 10);
    };
    CHECK_NEAR(run(), 0.5f, 1e-6);
    rack.chainMix(1)->solo.store(true);
    CHECK_NEAR(run(), 0.25f, 1e-6); // only the soloed chain sounds
    rack.chainMix(1)->solo.store(false);
    rack.chainMix(0)->volume.store(0.5f);
    rack.chainMix(1)->active.store(false);
    CHECK_NEAR(run(), 0.125f, 1e-6);
    CHECK(rack.chainMix(0)->meter.takePeak()[0] > 0.1f); // post-fader chain meter
    CHECK(rack.chainMix(2) == nullptr);

    auto model = devices::nativeDeviceModel("AudioEffectGroupDevice");
    model.chains.resize(2);
    devices::captureDeviceState(model, &rack);
    CHECK_NEAR(model.chains[0].volume, 0.5, 1e-6);
    CHECK(! model.chains[1].speakerOn);
    CHECK(! model.chains[1].solo);
}

TEST_CASE("rack face: strip, chain list, zones, Rand and Map drive the rack through its edits (UI-66)")
{
    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].kind = model::TrackKind::Midi;
    song.tracks[0].devices = {pianoRack()};
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    graph.graph->prepare(48000, 512);
    ui::UiContext context;
    context.song = &song;
    context.graph = graph.graph.get();
    context.controls = graph.graph->controls;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    context.theme.metrics.detailHeight = 220;
    std::vector<std::string> labels;
    context.editDevice = [&](int track, int device, const auto& operation, const std::string& label) {
        CHECK(track == 0);
        operation(song.tracks[0].devices[(size_t) device]);
        labels.push_back(label);
    };
    ui::MainLayout root(context);
    root.setBounds({0, 0, 2400, 800});
    root.layout();
    const auto face = [&]() -> ui::RackFace& {
        std::vector<ui::RackFace*> faces;
        collect(root, faces);
        REQUIRE(faces.size() == 1);
        return *faces[0];
    };
    auto* rack = face().rack();
    REQUIRE(rack);
    CHECK(face().chainsShown());
    CHECK(face().macrosShown());
    CHECK(! face().devicesShown());

    // Selecting a chain shows its devices to the right of the rack.
    click(root, at(face(), face().chainNameBounds(0)));
    CHECK(context.selectedRackId == 77);
    CHECK(context.selectedRackChain == 0);
    root.layout();
    CHECK(face().devicesShown());

    // Activator, exclusive solo, volume drag and double-click reset act on the live chain mixer.
    click(root, at(face(), face().speakerBounds(1)));
    CHECK(! rack->chainMix(1)->active.load());
    click(root, at(face(), face().soloBounds(0)));
    click(root, at(face(), face().soloBounds(1)));
    CHECK(! rack->chainMix(0)->solo.load());
    CHECK(rack->chainMix(1)->solo.load());
    auto volume = at(face(), face().volumeBounds(0));
    static_cast<ui::RootWidget&>(root).dispatchMouseDown(volume);
    auto lower = volume;
    lower.y += 40;
    static_cast<ui::RootWidget&>(root).dispatchMouseDrag(lower);
    static_cast<ui::RootWidget&>(root).dispatchMouseUp(lower);
    CHECK(rack->chainMix(0)->volume.load() < 0.9f);
    volume.clicks = 2;
    static_cast<ui::RootWidget&>(root).dispatchMouseDown(volume);
    CHECK_NEAR(rack->chainMix(0)->volume.load(), 1.0f, 1e-6);

    // Macro boxes: drag turns the macro (and its mapped parameters).
    auto macro = at(face(), face().macroBounds(1));
    static_cast<ui::RootWidget&>(root).dispatchMouseDown(macro);
    auto up = macro;
    up.y -= 60;
    static_cast<ui::RootWidget&>(root).dispatchMouseDrag(up);
    static_cast<ui::RootWidget&>(root).dispatchMouseUp(up);
    CHECK(rack->getParameter(1) > 10.0f);

    // Key zone editor: drag the selected chain's low edge up an octave.
    click(root, at(face(), face().zoneTabBounds(ui::RackFace::Zones::Key)));
    CHECK(face().zones() == ui::RackFace::Zones::Key);
    const float widthWithZones = face().width();
    auto edge = at(face(), face().zoneBarBounds(0));
    edge.x = face().boundsInRoot().x + face().zoneBarBounds(0).x + 1;
    static_cast<ui::RootWidget&>(root).dispatchMouseDown(edge);
    auto moved = edge;
    moved.x += face().zoneRulerBounds().w * 12.0f / 128.0f;
    static_cast<ui::RootWidget&>(root).dispatchMouseDrag(moved);
    static_cast<ui::RootWidget&>(root).dispatchMouseUp(moved);
    REQUIRE(! labels.empty());
    CHECK(labels.back() == "Edit Chain Zone");
    CHECK(song.tracks[0].devices[0].chains[0].keyMin == 12);
    CHECK(song.tracks[0].devices[0].chains[0].keyMax == 127);
    click(root, at(face(), face().zoneTabBounds(ui::RackFace::Zones::Hide)));
    CHECK(face().width() < widthWithZones);

    // Rand: one undoable edit; the excluded macro keeps its value, the others land in 0..127.
    const double reverbBefore = song.tracks[0].devices[0].param("MacroControls.7", -1);
    face().randomizeMacros(1234);
    CHECK(labels.back() == "Randomize Macros");
    CHECK_NEAR(song.tracks[0].devices[0].param("MacroControls.7", -1), reverbBefore, 1e-9);
    bool changed = false;
    for (int i = 0; i < 7; ++i)
    {
        const double v = song.tracks[0].devices[0].param("MacroControls." + std::to_string(i), -1);
        CHECK(v >= 0.0);
        CHECK(v <= 127.0);
        changed = changed || std::abs(v - (i == 0 ? 127.0 : 0.0)) > 1e-6;
    }
    CHECK(changed);

    // Map: choose macro 4 (Attack), then click the Simpler's mappable control under the overlay.
    face().toggleMapping();
    CHECK(context.rackMapId == 77);
    CHECK(face().mapping());
    click(root, at(face(), face().macroBounds(3)));
    CHECK(context.rackMacroToMap == 3);
    root.layout();
    std::vector<ui::Knob*> knobs;
    for (auto* knob : (collect(root, knobs), knobs))
        if (knob->onRightClick && knob->visible && knob->width() > 0)
        {
            ui::MouseEvent e;
            const auto b = knob->boundsInRoot();
            e.x = b.centreX();
            e.y = b.centreY();
            root.tickTree(); // the overlay shows in Map mode
            click(root, e);
            break;
        }
    REQUIRE(! labels.empty());
    CHECK(labels.back() == "Map Rack Macro");
    int mappedToFour = 0;
    for (const auto& p : song.tracks[0].devices[0].chains[0].devices[0].parameters)
        if (p.macroIndex == 3) ++mappedToFour;
    CHECK(mappedToFour == 1);

    // Strip: + shows one more macro; the chain list button folds the list away.
    click(root, at(face(), face().stripBounds(ui::RackFace::Strip::MoreMacros)));
    CHECK(song.tracks[0].devices[0].numVisibleMacros == 9);
    root.layout();
    const float before = face().width();
    click(root, at(face(), face().stripBounds(ui::RackFace::Strip::ChainList)));
    root.layout();
    CHECK(! face().chainsShown());
    CHECK(face().width() < before);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests");
    dir.createDirectory();
    CHECK(gfx::renderToPng(root, 2400, 800, 1, dir.getChildFile("rack_face.png").getFullPathName().toStdString()));
}
