#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/FmSynth.h"
#include "devices/native/Compressor.h"
#include "devices/native/VectorSynth.h"
#include "devices/native/Impulse.h"
#include "devices/native/Echo.h"
#include "devices/native/Tuner.h"
#include "devices/native/SwissArmyMeter.h"
#include "devices/native/Utility.h"
#include "devices/native/Simpler.h"
#include "engine/EngineGraph.h"
#include "ui/views/DeviceFace.h"
#include "ui/views/ReferenceFace.h"
#include "devices/native/EqEight.h"
#include "devices/native/Limiter.h"
#include "devices/native/Bands.h"
#include "ui/views/NativeEditors.h"
#include "ui/views/MainLayout.h"
#include "ui/views/BrowserPanel.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "ui/RootWidget.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"

using namespace sonora;
namespace
{
template <typename T> T* find(ui::Widget& root)
{
    if (auto* found = dynamic_cast<T*>(&root)) return found;
    for (const auto& child : root.children()) if (auto* found = find<T>(*child)) return found;
    return nullptr;
}
ui::MouseEvent localEvent(ui::Rect r, float x = 0.5f, float y = 0.5f)
{
    ui::MouseEvent e; e.x = r.x + r.w * x; e.y = r.y + r.h * y; return e;
}
void click(ui::RootWidget& root, ui::MouseEvent e) { root.dispatchMouseDown(e); root.dispatchMouseUp(e); }
void tagControl(ui::Widget* w, int p, devices::DeviceProcessor* processor)
{
    w->midiTarget = midi::ControlTarget{midi::ControlTarget::Kind::DeviceParameter, 0, 0, 0, processor->parameters()[(size_t) p].id};
}
}
TEST_CASE("device faces: FM operator selection exposes its own envelope and graph gestures edit that operator")
{
    auto model = devices::nativeDeviceModel("SonoraFm"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, [&](auto* w, int p) { tagControl(w, p, processor.get()); });
    root.setBounds({0, 0, 580, 180}); face->setBounds({0, 0, 570, 164}); face->layout();
    click(root, localEvent({174, 12, 16, 18})); CHECK(face->selectedSection() == 3);
    const float original = processor->getParameter(devices::FmSynth::oscParam(0, devices::FmSynth::Sustain));
    click(root, localEvent(face->graphBounds(), 0.4f, 0.7f));
    CHECK_NEAR(processor->getParameter(devices::FmSynth::oscParam(3, devices::FmSynth::Sustain)), 0.3, 1e-6);
    CHECK_NEAR(processor->getParameter(devices::FmSynth::oscParam(0, devices::FmSynth::Sustain)), original, 0);
    auto right = localEvent(face->graphBounds(), 0.1f, 0.1f); right.right = true; click(root, right);
    CHECK_NEAR(processor->getParameter(devices::FmSynth::oscParam(3, devices::FmSynth::Sustain)), 0.3, 1e-6);
}
TEST_CASE("device faces: browser searches find FM and compact Wavetable and Impulse also appears in Drums")
{
    struct Search { const char* query; const char* type; int category; };
    for (const auto& search : {Search{"operator", "SonoraFm", 3}, Search{"wavetable", "SonoraVector", 3}, Search{"impulse", "SonoraImpulse", 2},
                              Search{"swiss", "max:Swiss Army Meter", 4}})
    {
        ui::UiContext context; context.detailVisible = false;
        ui::MainLayout root(context); root.setBounds({0, 0, 1200, 700}); root.layout();
        auto* browser = find<ui::BrowserPanel>(root); REQUIRE(browser);
        browser->showCategory(search.category); browser->focusSearch();
        for (char character : std::string(search.query)) { ui::KeyEvent key; key.textChar = (uint32_t) character; root.dispatchKey(key); }
        auto bounds = browser->listRowBounds(1); const auto position = browser->boundsInRoot();
        bounds.x += position.x; bounds.y += position.y;
        auto start = localEvent(bounds); root.dispatchMouseDown(start);
        auto end = start; end.x += 20; root.dispatchMouseDrag(end);
        REQUIRE(context.browserDrag); CHECK(context.browserDrag->value == search.type);
        root.dispatchMouseUp(end);
    }
}
TEST_CASE("device faces: vector position and Echo sync are live controls")
{
    for (const auto* type : {"SonoraVector", "SonoraEcho"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
        root.setBounds({0, 0, 800, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
        if (dynamic_cast<devices::Echo*>(processor.get()))
        {
            auto wheel = localEvent({0, 25, 58, 56}); wheel.wheelDeltaY = 1; root.dispatchMouseWheel(wheel);
            CHECK_NEAR(processor->getParameter(devices::Echo::DivisionL), 4, 0);
            CHECK_NEAR(processor->getParameter(devices::Echo::DivisionR), 3, 0);
        }
        auto e = localEvent(face->graphBounds(), 0.25f, 0.2f); click(root, e);
        if (auto* vector = dynamic_cast<devices::VectorSynth*>(processor.get())) CHECK_NEAR(vector->getParameter(devices::VectorSynth::Position1), 0.8, 1e-6);
        else
        {
            CHECK_NEAR(processor->getParameter(devices::Echo::SyncL), 0, 0);
            CHECK_NEAR(processor->getParameter(devices::Echo::TimeL), 1000.75, 1e-4);
            CHECK_NEAR(processor->getParameter(devices::Echo::SyncR), 1, 0);
            e.shift = true; click(root, e); CHECK_NEAR(processor->getParameter(devices::Echo::SyncR), 0, 0);
        }
    }
}
TEST_CASE("device faces: sparse Impulse pad drops address slot eight independently of sample vector order")
{
    auto model = devices::nativeDeviceModel("SonoraImpulse"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 2, 3, 4, std::function<void(ui::Widget*, int)>{});
    root.setBounds({0, 0, 600, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
    int addressedSlot = -1;
    context.dropDeviceItem = [&](const auto&, int track, int device, int chain, int slot, int child) { CHECK(track == 0); CHECK(device == 2); CHECK(chain == 3); CHECK(child == 4); addressedSlot = slot; };
    ui::BrowserDrag sample{ui::BrowserDrag::Kind::Sample, "/tmp/own-hit.wav", "Own Hit"};
    CHECK(face->browserDrop(sample, localEvent({455, 0, 61, 42}))); CHECK(addressedSlot == 7);
    click(root, localEvent({455, 0, 61, 42})); CHECK(face->selectedSection() == 7);
    CHECK(face->browserDrop(sample, localEvent(face->graphBounds()))); CHECK(addressedSlot == 7);
}
TEST_CASE("device faces: sample boundary editing commits once and preserves the other boundary")
{
    model::Device model = devices::nativeDeviceModel("OriginalSimpler");
    model::SamplePart part; part.sampleEnd = 1000; model.sampleParts.push_back(part);
    auto data = std::make_shared<engine::SampleData>(); data->audio.setSize(2, 1000); data->audio.clear();
    devices::Simpler::Zone zone; zone.part = part; zone.slot = engine::SampleSlot::makeReady(data);
    devices::Simpler processor({zone}, devices::Simpler::Mode::Classic, 2);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, &processor, 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
    root.setBounds({0, 0, 600, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
    int commits = 0; context.editDevice = [&](int, int, const auto& operation, const auto&) { ++commits; operation(model); };
    auto e = localEvent(face->graphBounds(), 0, 0.3f); root.dispatchMouseDown(e);
    e.x += face->graphBounds().w * 0.25f; root.dispatchMouseDrag(e);
    CHECK(commits == 0); root.dispatchMouseUp(e); CHECK(commits == 1);
    CHECK(model.sampleParts[0].sampleStart == 250); CHECK(model.sampleParts[0].sampleEnd == 1000); CHECK(model.samplePartsEdited);
}
TEST_CASE("device faces: all native parameters remain reachable and inside the compact face across tabs")
{
    for (const auto* type : {"SonoraFm", "OriginalSimpler", "MultiSampler", "SonoraVector", "SonoraImpulse", "SonoraEcho", "Redux2", "Gate", "AutoPan"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, [&](auto* widget, int parameter) { tagControl(widget, parameter, processor.get()); });
        root.setBounds({0, 0, 800, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
        std::set<std::string> seen;
        auto inspect = [&] {
            for (const auto& child : face->children())
                if (child->visible && child->midiTarget)
                {
                    seen.insert(child->midiTarget->param);
                    CHECK(child->bounds().x >= 0); CHECK(child->bounds().y >= 0);
                    CHECK(child->bounds().right() <= face->width() + 0.01f); CHECK(child->bounds().bottom() <= face->height() + 0.01f);
                }
        };
        inspect();
        if (face->kind() == ui::DeviceFace::Kind::Simpler)
        {
            using S = devices::Simpler;
            face->selectPage(1); inspect(); processor->setParameter(S::LfoSync, 1); face->layout(); inspect(); processor->setParameter(S::LfoSync, 0);
            for (int section = 0; section < 4; ++section) { face->selectSection(section); inspect(); } // Controls display views
            face->selectSection(0);
            // Drive appears with a non-Clean circuit, Morph and the band-pass circuit chooser with the Morph type.
            processor->setParameter(S::FilterCircuit, 1); face->layout(); inspect();
            processor->setParameter(S::FilterType, 4); face->layout(); inspect();
            processor->setParameter(S::FilterCircuit, 0); processor->setParameter(S::FilterType, 0);
            face->selectPage(1); processor->setParameter(S::AmpLoopMode, 2); face->layout(); inspect(); // Beat: repeat division
            processor->setParameter(S::AmpLoopMode, 0); face->selectPage(0);
            // One-Shot mode shows its fades and trigger mode instead of the loop controls.
            auto oneShotModel = model; oneShotModel.plain["Globals/PlaybackMode"] = 1;
            auto oneShot = devices::createDevice(oneShotModel, build);
            auto* oneShotFace = root.add<ui::DeviceFace>(oneShotModel, oneShot.get(), 0, 0, -1, -1, [&](auto* widget, int parameter) { tagControl(widget, parameter, oneShot.get()); });
            oneShotFace->setBounds({0, 0, oneShotFace->preferredWidth(), 164}); oneShotFace->layout();
            for (const auto& child : oneShotFace->children())
                if (child->visible && child->midiTarget) seen.insert(child->midiTarget->param);
            oneShotFace->visible = false;
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            std::vector<ui::TextButton*> tabs;
            for (const auto& child : face->children()) if (auto* button = dynamic_cast<ui::TextButton*>(child.get())) tabs.push_back(button);
            for (auto* tab : tabs) if (tab->visible) { click(root, localEvent(tab->bounds())); inspect(); }
        }
        if (dynamic_cast<devices::Echo*>(processor.get()))
        {
            processor->setParameter(devices::Echo::SyncL, 0); processor->setParameter(devices::Echo::SyncR, 0); face->layout(); inspect();
        }
        for (const auto& parameter : processor->parameters()) CHECK(seen.contains(parameter.id) || ! face->applies(parameter.id));
    }
}
TEST_CASE("device faces: every face renders at normal detail height and creates disposable visual review sets")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces"); REQUIRE(dir.createDirectory().wasOk());
    io::NewSetOptions options; options.midiTracks = 14; options.audioTracks = options.returnTracks = 0;
    const auto source = dir.getChildFile("Source.als"), destination = dir.getChildFile("Faces.als");
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    const auto wav = dir.getChildFile("Own Wave.wav"); wav.deleteFile();
    juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> stream(wav.createOutputStream());
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24)); REQUIRE(writer);
    juce::AudioBuffer<float> audio(2, 24000);
    for (int i = 0; i < audio.getNumSamples(); ++i)
        for (int ch = 0; ch < 2; ++ch) audio.setSample(ch, i, (float) (std::sin(i * 0.07) * std::exp(-(double) i / 7000) * 0.8));
    REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples())); writer.reset();
    model::SamplePart part; part.name = "Own Wave"; part.sample.absolutePath = wav.getFullPathName().toStdString(); part.sampleRate = 48000;
    part.sampleEnd = part.lengthSamples = part.loopEnd = 24000; part.loopStart = 1200; part.loopMode = 1;
    const std::vector<std::string> types{"SonoraFm", "DrumGroupDevice", "InstrumentGroupDevice", "MultiSampler", "OriginalSimpler", "Limiter", "Compressor2", "SonoraVector", "SonoraImpulse", "SonoraEcho", "Redux2", "Tuner", "Gate", "AutoPan"};
    for (size_t i = 0; i < types.size(); ++i)
    {
        auto model = devices::nativeDeviceModel(types[i]);
        if (types[i] == "OriginalSimpler" || types[i] == "MultiSampler") { model.sampleParts = {part}; model.samplePartsEdited = true; }
        if (types[i] == "SonoraImpulse")
        {
            for (int slot = 0; slot < 8; ++slot) { auto hit = part; hit.rootKey = hit.keyMin = hit.keyMax = devices::Impulse::notes[(size_t) slot]; hit.loopMode = 0; hit.name = "Hit " + std::to_string(slot + 1); model.sampleParts.push_back(hit); }
            model.samplePartsEdited = true;
        }
        if (model.kind == model::DeviceKind::Rack)
        {
            for (int slot = 0; slot < 4; ++slot)
            {
                model::Device::Chain chain; chain.name = slot == 0 ? "Kick" : slot == 1 ? "Snare" : "Hat " + std::to_string(slot);
                chain.receivingNote = 36 + slot; chain.devices = {devices::nativeDeviceModel("OriginalSimpler")};
                chain.devices[0].sampleParts = {part}; chain.devices[0].samplePartsEdited = true;
                model.chains.push_back(chain);
            }
            model.chainsEdited = true;
        }
        model::allocateDeviceIds(song, model); model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
        song.tracks[i].name = model.displayName; song.tracks[i].devices = {model}; song.tracks[i].mixer.volume = 0.07;
        if (i < 5 || i == 7 || i == 8)
        {
            song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
            model::Clip clip; REQUIRE(model::makeEmptyMidiClip(song, 0, 4, clip)); clip.name = "Own verification notes"; clip.colorIndex = song.tracks[i].colorIndex;
            for (int note = 0; note < 4; ++note) clip.notes.push_back({(double) note, 0.4, i == 1 ? 36 + note : i == 8 ? devices::Impulse::notes[(size_t) note] : 60 + note * 2, 100});
            song.tracks[i].arrangementClips.push_back(std::move(clip));
        }
    }
    REQUIRE(io::saveAls(song, destination, {}).ok);
    engine::SampleCache samples; devices::BuildContext build; auto graph = engine::buildGraph(song, build, samples); graph.graph->prepare(48000, 512);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.controls = graph.graph->controls;
    context.browserVisible = false; context.detailTab = ui::DetailTab::Devices; context.theme.metrics.detailHeight = 220;
    ui::MainLayout root(context); root.setBounds({0, 0, 1440, 700});
    for (size_t i = 0; i < types.size(); ++i)
    {
        context.selectedTrack = (int) i; root.layout();
        if (i == 5 || i == 6) REQUIRE(find<ui::ReferenceFace>(root)); // Limiter, Compressor: matched to the Live 12 capture (UI-42)
        else if (i != 1 && i != 2) REQUIRE(find<ui::DeviceFace>(root));
        REQUIRE(gfx::renderToPng(root, 1440, 700, 2, dir.getChildFile(types[i] + ".png").getFullPathName().toStdString()));
        if (i == 3 || i == 4)
        {
            auto* face = find<ui::DeviceFace>(root); REQUIRE(face);
            if (i == 4) face->selectPage(1);
            for (const auto& child : face->children())
                if (auto* button = dynamic_cast<ui::TextButton*>(child.get()); button && button->label() == "Filter / Env")
                { click(root, localEvent(button->boundsInRoot())); break; }
            REQUIRE(gfx::renderToPng(root, 1440, 700, 2, dir.getChildFile(types[i] + "-filter.png").getFullPathName().toStdString()));
        }
    }
    for (const auto* type : {"Tuner", "Redux2"})
    {
        auto device = devices::nativeDeviceModel(type); model::allocateDeviceIds(song, device);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device)); song.tracks[4].devices.push_back(device);
    }
    REQUIRE(io::saveAls(song, dir.getChildFile("Expanded.als"), {}).ok);

}

TEST_CASE("device faces: Tuner reads live pitch and its history and spelling menus change real saved settings")
{
    auto model = devices::nativeDeviceModel("Tuner"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    REQUIRE(processor); processor->prepare(48000, 512);
    juce::AudioBuffer<float> audio(2, 24000);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 24000; ++i) audio.setSample(ch, i, 0.5f * (float) std::sin(2 * dsp::kPi * 442 * i / 48000));
    juce::MidiBuffer midi; processor->process(audio, midi);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, [](auto*, int) {});
    root.setBounds({0, 0, 300, 164}); face->setBounds({0, 0, 300, 164});
    context.showChoices = [&](const auto& labels, const auto&, auto chosen) { CHECK(labels.size() == 3); chosen(1); };
    auto right = localEvent(face->graphBounds()); right.right = true; click(root, right);
    CHECK_NEAR(processor->getParameter(devices::Tuner::Spelling), 1, 0);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    REQUIRE(gfx::renderToPng(root, 300, 164, 3, folder.getChildFile("Tuner-active.png").getFullPathName().toStdString()));
    context.showChoices = [&](const auto& labels, const auto&, auto chosen) { CHECK(labels.size() == 2); chosen(1); };
    click(root, localEvent({0, 144, 62, 18})); CHECK_NEAR(processor->getParameter(devices::Tuner::ViewMode), 1, 0);
    for (int i = 0; i < 100; ++i) root.tickTree();
    REQUIRE(gfx::renderToPng(root, 300, 164, 3, folder.getChildFile("Tuner-history.png").getFullPathName().toStdString()));
}

TEST_CASE("device faces: Swiss Army Meter follows the original's dark layout and its reset circle restarts the measurement")
{
    model::Device model;
    model.kind = model::DeviceKind::MaxForLive; model.type = "MxDeviceAudioEffect";
    model.maxPatch = model.displayName = "Swiss Army Meter";
    devices::BuildContext build; auto processor = devices::createDevice(model, build);
    auto* meter = dynamic_cast<devices::SwissArmyMeter*>(processor.get());
    REQUIRE(meter != nullptr); REQUIRE(ui::DeviceFace::supports(meter));
    meter->prepare(48000, 512);
    devices::HostInfo host; host.playing = true; juce::MidiBuffer midi;
    for (int block = 0; block < 400; ++block) // ~4.3 s of a -12 dBFS sine, slightly wider on the right
    {
        juce::AudioBuffer<float> audio(2, 512);
        for (int i = 0; i < 512; ++i)
        {
            const double phase = 2 * dsp::kPi * 440 * (block * 512 + i) / 48000;
            audio.setSample(0, i, 0.25f * (float) std::sin(phase)); audio.setSample(1, i, 0.25f * (float) std::sin(phase + 0.6));
        }
        meter->setHostInfo(host); meter->process(audio, midi);
    }
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, [](auto*, int) {});
    CHECK(face->kind() == ui::DeviceFace::Kind::Meter);
    root.setBounds({0, 0, 118, 164}); face->setBounds({0, 0, 117.4f, 164}); face->layout();
    CHECK_NEAR(face->preferredWidth(), 117.4, 1e-3);
    for (int i = 0; i < 5; ++i) root.tickTree();
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    REQUIRE(gfx::renderToPng(root, 118, 164, 3, folder.getChildFile("SwissArmyMeter-playing.png").getFullPathName().toStdString()));
    CHECK(meter->reading().integrated > -30);
    click(root, localEvent({100.5f, 144.5f, 11.2f, 11.2f})); // the reset circle
    juce::AudioBuffer<float> silence(2, 64); silence.clear(); meter->setHostInfo(host); meter->process(silence, midi);
    CHECK(meter->reading().integrated <= dsp::kSilenceDb);
}

TEST_CASE("device faces: Utility follows the reference layout and its controls edit the real parameters")
{
    auto model = devices::nativeDeviceModel("StereoGain"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    REQUIRE(ui::DeviceFace::supports(processor.get()));
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, [](auto*, int) {});
    CHECK(face->kind() == ui::DeviceFace::Kind::Utility);
    root.setBounds({0, 0, 200, 164}); face->setBounds({0, 0, 125.4f, 164}); face->layout();
    CHECK_NEAR(face->preferredWidth(), 125.4, 1e-3);
    using U = devices::Utility;
    click(root, localEvent({2.1f, 18.6f, 24.9f, 11.7f})); CHECK_NEAR(processor->getParameter(U::PhaseInvertL), 1, 0);
    click(root, localEvent({70.8f, 145.7f, 29.2f, 11.6f})); CHECK_NEAR(processor->getParameter(U::Mute), 1, 0);
    click(root, localEvent({43.8f, 146.5f, 12.1f, 12.1f})); CHECK_NEAR(processor->getParameter(U::BassMonoAudition), 1, 0);
    context.showChoices = [&](const auto& labels, const auto& ticks, auto chosen) { CHECK(labels.size() == 4); CHECK(ticks[1]); chosen(3); };
    click(root, localEvent({2.1f, 34.1f, 53.8f, 12.1f})); CHECK_NEAR(processor->getParameter(U::ChannelMode), 3, 0);

    // Gain: 0 dB points up (double-click resets there); a full upward drag reaches +35 dB.
    processor->setParameter(U::Gain, 0.25f);
    const ui::Rect gain{67.3f, 23.9f, 58, 51};
    auto ring = localEvent(gain, 0.5f, 0.55f); auto twice = ring; twice.clicks = 2;
    root.dispatchMouseDown(twice); root.dispatchMouseUp(twice); CHECK_NEAR(processor->getParameter(U::Gain), 1.0, 1e-5);
    root.dispatchMouseDown(ring); auto up = ring; up.y -= 120; root.dispatchMouseDrag(up); root.dispatchMouseUp(up);
    CHECK_NEAR(dsp::gainToDb(processor->getParameter(U::Gain)), 35.0, 0.01);

    // Width: 100 % points up; the right half spans 100-400 %. Clicking the label switches to Mid/Side.
    const ui::Rect width{0, 53.7f, 58, 51};
    auto widthRing = localEvent(width, 0.5f, 0.55f);
    root.dispatchMouseDown(widthRing); auto half = widthRing; half.y -= 50; root.dispatchMouseDrag(half); root.dispatchMouseUp(half);
    CHECK_NEAR(processor->getParameter(U::StereoWidth), 2.5, 1e-4); // position 0.75
    click(root, localEvent(width, 0.5f, 0.08f)); CHECK_NEAR(processor->getParameter(U::MidSideOn), 1, 0);
    root.dispatchMouseDown(widthRing); auto down = widthRing; down.y += 50; root.dispatchMouseDrag(down); root.dispatchMouseUp(down);
    CHECK_NEAR(processor->getParameter(U::StereoWidth), 2.5, 1e-4); // untouched in Mid/Side mode
    CHECK_NEAR(processor->getParameter(U::MidSideBalance), 0.5, 1e-4);

    processor->setParameter(U::Gain, dsp::dbToGain(-3.89f));
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    REQUIRE(gfx::renderToPng(root, 126, 164, 3, folder.getChildFile("Utility.png").getFullPathName().toStdString()));
}

namespace
{
template <typename T> void collect(ui::Widget& root, std::vector<T*>& out)
{
    if (auto* found = dynamic_cast<T*>(&root)) out.push_back(found);
    for (const auto& child : root.children()) collect(*child, out);
}
// The face inside a device box as DetailView places it: whole body under an 18-unit title, 176 tall by default.
ui::ReferenceFace* referenceFace(ui::RootWidget& root, const model::Device& model, devices::DeviceProcessor* processor,
                                 std::function<void(ui::Widget*, int)> tag, float bodyHeight = ui::ReferenceFace::kDefaultBody)
{
    auto* face = ui::addReferenceFace(root, model, processor, std::move(tag));
    REQUIRE(face != nullptr);
    root.setBounds({0, 0, 1000, bodyHeight});
    face->setBounds({0, 0, face->preferredWidth(bodyHeight), bodyHeight});
    face->layout();
    return face;
}
}

TEST_CASE("device faces: Live 12 reference faces exist for EQ Eight, EQ Three, Compressor and Limiter only")
{
    for (const auto* type : {"Eq8", "FilterEQ3", "Compressor2", "Limiter", "GlueCompressor", "Reverb", "SonoraFm"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        REQUIRE(processor);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = ui::addReferenceFace(root, model, processor.get(), {});
        const std::string name = type;
        CHECK((face != nullptr) == (name == "Eq8" || name == "FilterEQ3" || name == "Compressor2" || name == "Limiter"));
        if (! face) continue;
        // Widths are the capture's device widths (Compressor with its sidechain section folded), scaled with the body.
        const float expected = name == "Eq8" ? 675 : name == "FilterEQ3" ? 210 : name == "Compressor2" ? 574 : 397;
        CHECK_NEAR(face->preferredWidth(239), expected, 0.01);
        CHECK_NEAR(face->preferredWidth(119.5f), expected / 2, 0.01);
    }
}

TEST_CASE("device faces: reference faces reach every shown parameter, inside the face, with MIDI targets")
{
    for (const auto* type : {"Eq8", "FilterEQ3", "Compressor2", "Limiter"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        ui::UiContext context; ui::RootWidget root(context);
        context.showChoices = [](const auto&, const auto&, const auto&) {};
        auto* face = referenceFace(root, model, processor.get(), [&](auto* widget, int parameter) { tagControl(widget, parameter, processor.get()); });
        std::set<std::string> seen;
        auto inspect = [&] {
            for (const auto& child : face->children())
                if (child->visible && child->midiTarget)
                {
                    seen.insert(child->midiTarget->param);
                    CHECK(child->bounds().x >= -0.01f); CHECK(child->bounds().y >= -0.01f);
                    CHECK(child->bounds().right() <= face->width() + 0.01f); CHECK(child->bounds().bottom() <= face->height() + 0.01f);
                }
        };
        inspect();
        std::set<std::string> hidden; // parameters the reference face keeps out of view
        if (auto* eq = dynamic_cast<devices::EqEight*>(processor.get()))
        {
            using E = devices::EqEight;
            hidden = {eq->parameters()[E::kPrecision].id, eq->parameters()[E::kAdaptiveQFactor].id};
            // Selecting each band (its number) retargets the Freq / Gain / Q knobs; the Edit switch reaches set B.
            for (int set = 0; set < 2; ++set)
            {
                if (set == 1)
                {
                    eq->setParameter(E::kMode, 1);
                    std::vector<ui::TextButton*> buttons; collect(*face, buttons);
                    for (auto* b : buttons) if (b->label() == "L") { click(root, localEvent(b->boundsInRoot())); break; }
                }
                for (int band = 0; band < E::kBands; ++band)
                {
                    for (const auto& child : face->children())
                        if (child->midiTarget && child->midiTarget->param == eq->parameters()[(size_t) E::bandParamSet(set == 1, band, E::On)].id)
                        { auto e = localEvent(child->boundsInRoot(), 0.85f, 0.5f); click(root, e); break; }
                    inspect();
                }
            }
        }
        if (auto* compressor = dynamic_cast<devices::Compressor*>(processor.get()))
        {
            compressor->setParameter(devices::Compressor::Model, 2); face->layout(); inspect(); // Expand: the expansion ratio
            compressor->setParameter(devices::Compressor::Model, 0); face->layout();
        }
        for (const auto& parameter : processor->parameters()) CHECK(seen.contains(parameter.id) || hidden.contains(parameter.id));
    }
}

TEST_CASE("device faces: EQ Eight display selects, drags and switches off bands; its controls follow the selection")
{
    using E = devices::EqEight;
    auto model = devices::nativeDeviceModel("Eq8"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    auto* eq = dynamic_cast<E*>(processor.get()); REQUIRE(eq);
    for (int b = 0; b < E::kBands; ++b) eq->setParameter(E::bandParam(b, E::On), b == 1 || b == 4 ? 1.0f : 0.0f);
    eq->setParameter(E::bandParam(1, E::Type), 3); eq->setParameter(E::bandParam(1, E::Freq), 200); eq->setParameter(E::bandParam(1, E::GainDb), 0);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = referenceFace(root, model, processor.get(), [&](auto* widget, int parameter) { tagControl(widget, parameter, processor.get()); });
    const auto r = face->graphBounds();
    const auto xOf = [&](float hz) { return r.x + r.w * std::log(hz / 10.0f) / std::log(2200.0f); };
    // Press on band 2's handle and drag it up and right: frequency and gain follow, and the knobs now address band 2.
    ui::MouseEvent e; e.x = xOf(200); e.y = r.centreY();
    root.dispatchMouseDown(e);
    e.x = xOf(800); e.y = r.centreY() - r.h / 30 * 6; root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK_NEAR(eq->getParameter(E::bandParam(1, E::Freq)), 800, 1);
    CHECK_NEAR(eq->getParameter(E::bandParam(1, E::GainDb)), 6, 0.05);
    std::vector<ui::Knob*> knobs; collect(*face, knobs); REQUIRE(knobs.size() == 3);
    CHECK(knobs[0]->midiTarget->param == eq->parameters()[(size_t) E::bandParam(1, E::Freq)].id);
    // A click away from every handle keeps the selection; a double-click on a handle switches its band off.
    e.x = xOf(30); e.y = r.y + 4; click(root, e);
    CHECK(knobs[1]->midiTarget->param == eq->parameters()[(size_t) E::bandParam(1, E::GainDb)].id);
    e.x = xOf(800); e.y = r.centreY() - r.h / 30 * 6; e.clicks = 2; root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    CHECK(eq->getParameter(E::bandParam(1, E::On)) < 0.5f);
    CHECK(eq->getParameter(E::bandParam(4, E::On)) > 0.5f);
    // The Freq knob drags the selected band along its logarithmic law (10 Hz .. 22 kHz).
    eq->setParameter(E::bandParam(1, E::On), 1);
    const auto knob = knobs[0]->boundsInRoot();
    ui::MouseEvent k; k.x = knob.centreX(); k.y = knob.centreY(); root.dispatchMouseDown(k); k.y -= 20; root.dispatchMouseDrag(k); root.dispatchMouseUp(k);
    CHECK_NEAR(eq->getParameter(E::bandParam(1, E::Freq)), 800 * std::pow(2200.0, 0.1), 2);
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces"); REQUIRE(folder.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(root, (int) face->width(), (int) face->height(), 2, folder.getChildFile("EqEight-reference.png").getFullPathName().toStdString()));
}

TEST_CASE("device faces: Compressor threshold follows both display views and the sidechain section folds out")
{
    using C = devices::Compressor;
    auto model = devices::nativeDeviceModel("Compressor2"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = referenceFace(root, model, processor.get(), {});
    // Activity view (default): the drag sets the threshold at the pointer's height (+6.5 dB top, -36 dB bottom).
    auto r = face->graphBounds();
    ui::MouseEvent e; e.x = r.centreX(); e.y = r.y + r.h * (6.5f + 12) / 42.5f; click(root, e);
    CHECK_NEAR(dsp::gainToDb(processor->getParameter(C::Threshold)), -12, 0.05);
    // Transfer view: the threshold follows the pointer across the input axis (-60 .. 0 dB).
    std::vector<ui::TextButton*> buttons; collect(*face, buttons);
    click(root, localEvent(face->at(1150.5f, 243, 1179, 269))); // the footer's second view button: transfer curve
    e.x = r.x + r.w * 0.25f; click(root, e);
    CHECK_NEAR(dsp::gainToDb(processor->getParameter(C::Threshold)), -45, 0.05);
    // Peak / RMS / Expand are one choice; the folded sidechain section widens the face by 83 capture pixels.
    for (auto* b : buttons) if (b->label() == "RMS") click(root, localEvent(b->boundsInRoot()));
    CHECK_NEAR(processor->getParameter(C::Model), 1, 0);
    const float folded = face->preferredWidth(face->height());
    click(root, localEvent(face->at(929, 39, 945.5f, 55.5f)));
    CHECK_NEAR(face->preferredWidth(face->height()) - folded, 83 * face->unit(), 0.01);
}

TEST_CASE("device faces: Limiter ceiling drag uses the written scale; EQ Three slope and kills are live")
{
    {
        using L = devices::Limiter;
        auto model = devices::nativeDeviceModel("Limiter"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = referenceFace(root, model, processor.get(), {});
        // "6" is written at capture y 154: dragging the pointer there puts the ceiling at -6 dB.
        const auto six = face->at(1700, 154, 1700, 154);
        ui::MouseEvent e; e.x = face->graphBounds().centreX(); e.y = six.y; click(root, e);
        CHECK_NEAR(processor->getParameter(L::Ceiling), -6, 0.06);
        // Disabled modes do nothing; Link switches the stereo link.
        std::vector<ui::TextButton*> buttons; collect(*face, buttons);
        for (auto* b : buttons) if (b->label() == "Soft Clip") { CHECK(b->isEnabled); CHECK(! b->isEnabled()); }
        const float link = processor->getParameter(L::LinkChannels);
        click(root, localEvent(face->at(1806, 244, 1888, 260)));
        CHECK_NEAR(processor->getParameter(L::LinkChannels), 1 - link, 0);
    }
    {
        using Q = devices::EqThree;
        auto model = devices::nativeDeviceModel("FilterEQ3"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = referenceFace(root, model, processor.get(), {});
        click(root, localEvent(face->at(784, 227, 822, 245))); CHECK_NEAR(processor->getParameter(Q::Slope), 1, 0);
        click(root, localEvent(face->at(784, 205, 822, 222.5f))); CHECK_NEAR(processor->getParameter(Q::Slope), 0, 0);
        click(root, localEvent(face->at(784, 129, 824, 147.5f))); CHECK_NEAR(processor->getParameter(Q::MidOn), 0, 0);
        // The gain knob's travel is dB-linear from -54 dB with -inf at the bottom: 0 dB sits at 0.9.
        std::vector<ui::Knob*> knobs; collect(*face, knobs); REQUIRE(! knobs.empty());
        auto k = localEvent(knobs[0]->boundsInRoot()); root.dispatchMouseDown(k); k.y += 400; root.dispatchMouseDrag(k); root.dispatchMouseUp(k);
        CHECK_NEAR(processor->getParameter(Q::GainLow), 0, 0);
    }
}
