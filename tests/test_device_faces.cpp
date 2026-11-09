#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "core/DeviceCatalog.h"
#include "devices/native/AutoFilter.h"
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
#include "ui/views/SamplerFace.h"
#include "devices/native/EqEight.h"
#include "devices/native/Limiter.h"
#include "devices/native/Bands.h"
#include "devices/native/ChannelEq.h"
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
#include "ui/Theme.h"
#include "include/core/SkSurface.h"

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
    for (const auto& search : {Search{"operator", "SonoraFm", ui::BrowserPanel::Instruments}, Search{"wavetable", "SonoraVector", ui::BrowserPanel::Instruments}, Search{"impulse", "SonoraImpulse", ui::BrowserPanel::Drums},
                              Search{"swiss", "max:Swiss Army Meter", ui::BrowserPanel::AudioEffects}})
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
TEST_CASE("device faces: Simpler flags are the Start / Length / Loop controls and the display follows them (DEV-4c)")
{
    using S = devices::Simpler;
    model::Device model = devices::nativeDeviceModel("OriginalSimpler");
    model::SamplePart part; part.sampleEnd = 10000; model.sampleParts.push_back(part);
    auto data = std::make_shared<engine::SampleData>(); data->audio.setSize(2, 10000); data->audio.clear(); data->sampleRate = 44100;
    S::Zone zone; zone.part = part; zone.slot = engine::SampleSlot::makeReady(data);
    S processor({zone}, S::Mode::Classic, 2);
    processor.setParameter(S::Snap, 0); processor.setParameter(S::LoopOn, 0);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, &processor, 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
    root.setBounds({0, 0, 700, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
    int commits = 0; context.editDevice = [&](int, int, const auto&, const auto&) { ++commits; };
    const auto r = face->graphBounds();
    auto marks = face->sampleMarks();
    CHECK(std::abs(marks.start - r.x) < 0.5f); CHECK(std::abs(marks.end - r.right()) < 0.5f);
    CHECK(marks.loopStart < 0); // the loop shows only with Loop on
    // Knobs move the flags: Start 20 %, Length 50 % -> 20 % .. 70 % of the display.
    processor.setParameter(S::SampleStartMod, 0.2f); processor.setParameter(S::SampleLengthMod, 0.5f);
    marks = face->sampleMarks();
    CHECK(std::abs(marks.start - (r.x + 0.2f * r.w)) < 0.5f); CHECK(std::abs(marks.end - (r.x + 0.7f * r.w)) < 0.5f);
    processor.setParameter(S::LoopOn, 1); processor.setParameter(S::LoopLengthMod, 0.5f); processor.setParameter(S::LoopFadeMod, 0.5f);
    marks = face->sampleMarks();
    CHECK(std::abs(marks.loopStart - (r.x + 0.45f * r.w)) < 0.5f); // the last half of the region
    CHECK(marks.loopFade >= 0); CHECK(marks.loopFade < marks.loopStart);
    // Dragging the end flag sets Length (no structural edit), the start flag keeps the end where it is.
    auto e = localEvent(r, 0, 0.5f); e.x = marks.end; root.dispatchMouseDown(e);
    e.x = r.x + 0.9f * r.w; root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(std::abs(processor.getParameter(S::SampleLengthMod) - 0.7f) < 0.01f); CHECK(commits == 0);
    e.x = face->sampleMarks().start; root.dispatchMouseDown(e);
    e.x = r.x + 0.4f * r.w; root.dispatchMouseDrag(e); root.dispatchMouseUp(e);
    CHECK(std::abs(processor.getParameter(S::SampleStartMod) - 0.4f) < 0.01f);
    CHECK(std::abs(processor.getParameter(S::SampleLengthMod) - 0.5f) < 0.01f); // the end stays at 90 %
    CHECK(std::abs(face->sampleMarks().end - (r.x + 0.9f * r.w)) < 0.5f);
    CHECK(model.sampleParts[0].sampleStart == 0); CHECK(! model.samplePartsEdited); // the zone itself is untouched
    // Cmd + wheel zooms around the pointer; the flags follow the view.
    auto wheel = localEvent(r, 0.5f, 0.5f); wheel.cmd = true; wheel.wheelDeltaY = 1;
    CHECK(face->mouseWheel(wheel));
    const auto [from, to] = face->sampleView();
    CHECK(to - from < 0.81); CHECK(from > 0.0);
    CHECK(std::abs(face->sampleMarks().start - (r.x + (float) ((0.4 - from) / (to - from)) * r.w)) < 0.5f);
}
TEST_CASE("device faces: Simpler One-Shot draws its fades and Slicing its slices (DEV-4c)")
{
    using S = devices::Simpler;
    auto data = std::make_shared<engine::SampleData>(); data->audio.setSize(2, 44100); data->audio.clear(); data->sampleRate = 44100;
    model::SamplePart part; part.sampleEnd = 44100;
    S::Zone zone; zone.part = part; zone.slot = engine::SampleSlot::makeReady(data);
    {
        model::Device model = devices::nativeDeviceModel("OriginalSimpler"); model.sampleParts = {part}; model.plain["Globals/PlaybackMode"] = 1;
        S processor({zone}, S::Mode::OneShot, 2);
        processor.setParameter(S::Snap, 0); processor.setParameter(S::FadeIn, 100); processor.setParameter(S::FadeOut, 250);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = root.add<ui::DeviceFace>(model, &processor, 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
        root.setBounds({0, 0, 700, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
        const auto r = face->graphBounds();
        auto marks = face->sampleMarks();
        CHECK(std::abs(marks.fadeIn - (r.x + 0.1f * r.w)) < 0.5f);   // 100 ms of a 1 s sample
        CHECK(std::abs(marks.fadeOut - (r.x + 0.75f * r.w)) < 0.5f); // Trigger: 250 ms before the end
        processor.setParameter(S::FadeIn, 300);
        CHECK(std::abs(face->sampleMarks().fadeIn - (r.x + 0.3f * r.w)) < 0.5f); // follows the control at once
        processor.setParameter(S::OneShotGate, 1);
        CHECK(face->sampleMarks().fadeOut > r.x + 0.99f * r.w); // Gate: the fade starts on release
        CHECK(marks.slices.empty()); CHECK(marks.loopStart < 0);
        // One-Shot shows the fades, not the loop; the envelope knobs give way to Transpose.
        std::set<std::string> shown;
        for (const auto& child : face->children()) if (child->visible) if (auto* knob = dynamic_cast<ui::Knob*>(child.get())) shown.insert(knob->label());
        CHECK(shown.contains("Transp")); CHECK(! shown.contains("Attack")); CHECK(shown.contains("Volume"));
    }
    {
        part.slicingStyle = 2; part.slicingRegions = 4; zone.part = part;
        model::Device model = devices::nativeDeviceModel("OriginalSimpler"); model.sampleParts = {part}; model.plain["Globals/PlaybackMode"] = 2;
        S processor({zone}, S::Mode::Slice, 2);
        processor.setParameter(S::Snap, 0);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = root.add<ui::DeviceFace>(model, &processor, 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
        root.setBounds({0, 0, 700, 164}); face->setBounds({0, 0, face->preferredWidth(), 164}); face->layout();
        const auto r = face->graphBounds();
        const auto marks = face->sampleMarks();
        REQUIRE(marks.slices.size() == 4);
        for (int i = 0; i < 4; ++i) CHECK(std::abs(marks.slices[(size_t) i] - (r.x + r.w * (float) i / 4)) < 0.5f);
        CHECK(! marks.manualSlices);
        std::set<std::string> shown;
        for (const auto& child : face->children()) if (child->visible) if (auto* knob = dynamic_cast<ui::Knob*>(child.get())) shown.insert(knob->label());
        CHECK(! shown.contains("Decay")); CHECK(shown.contains("Transp"));
    }
}
TEST_CASE("device faces: all native parameters remain reachable and inside the compact face across tabs")
{
    for (const auto* type : {"SonoraFm", "OriginalSimpler", "SonoraVector", "SonoraImpulse", "SonoraEcho", "Redux2", "Gate", "AutoPan"} /* Sampler: test_sampler_face_ui */)
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
            // Slicing shows Playback, Trigger / Gate and the fades (DEV-4c).
            auto sliceModel = model; sliceModel.plain["Globals/PlaybackMode"] = 2;
            auto slicing = devices::createDevice(sliceModel, build);
            auto* sliceFace = root.add<ui::DeviceFace>(sliceModel, slicing.get(), 0, 0, -1, -1, [&](auto* widget, int parameter) { tagControl(widget, parameter, slicing.get()); });
            sliceFace->setBounds({0, 0, sliceFace->preferredWidth(), 164}); sliceFace->layout();
            for (const auto& child : sliceFace->children())
                if (child->visible && child->midiTarget)
                {
                    seen.insert(child->midiTarget->param);
                    CHECK(child->bounds().right() <= sliceFace->width() + 0.01f);
                }
            sliceFace->visible = false;
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
        // Limiter, Compressor (UI-42) and the Sampler (DEV-14b): matched to captures of the reference.
        if (i == 3 || i == 5 || i == 6) REQUIRE(find<ui::ReferenceFace>(root));
        else if (i != 1 && i != 2) REQUIRE(find<ui::DeviceFace>(root));
        REQUIRE(gfx::renderToPng(root, 1440, 700, 2, dir.getChildFile(types[i] + ".png").getFullPathName().toStdString()));
        if (i == 3)
        {
            auto* face = find<ui::ReferenceFace>(root); REQUIRE(face);
            ui::selectSamplerFacePage(*face, 2); // Filter / Global
            CHECK(ui::samplerFacePage(*face) == 2);
            REQUIRE(gfx::renderToPng(root, 1440, 700, 2, dir.getChildFile(types[i] + "-filter.png").getFullPathName().toStdString()));
        }
        if (i == 4)
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

TEST_CASE("device faces: Live 12 reference faces exist for EQ Eight, EQ Three, Compressor, Limiter, Auto Filter and Channel EQ only")
{
    for (const auto* type : {"Eq8", "FilterEQ3", "Compressor2", "Limiter", "AutoFilter2", "ChannelEq", "AutoFilter", "GlueCompressor", "Reverb", "SonoraFm"})
    {
        auto model = devices::nativeDeviceModel(type); devices::BuildContext build; auto processor = devices::createDevice(model, build);
        REQUIRE(processor);
        ui::UiContext context; ui::RootWidget root(context);
        auto* face = ui::addReferenceFace(root, model, processor.get(), {});
        const std::string name = type;
        CHECK((face != nullptr) == (name == "Eq8" || name == "FilterEQ3" || name == "Compressor2" || name == "Limiter" || name == "AutoFilter2"
                                    || name == "ChannelEq"));
        if (! face) continue;
        // Widths are the capture's device widths (Compressor with its sidechain section folded), scaled with the body.
        // Auto Filter's capture is at another zoom: 886 wide for a 342-px body (UI-43b).
        // Channel EQ (DEV-30): 290 wide for a 198-px body.
        const float expected = name == "Eq8" ? 675 : name == "FilterEQ3" ? 210 : name == "Compressor2" ? 574 : name == "Limiter" ? 397
                             : name == "ChannelEq" ? 290 * 239 / 198.0f : 886 * 239 / 342.0f;
        CHECK_NEAR(face->preferredWidth(239), expected, 0.01);
        CHECK_NEAR(face->preferredWidth(119.5f), expected / 2, 0.01);
    }
}

TEST_CASE("device faces: reference faces reach every shown parameter, inside the face, with MIDI targets")
{
    for (const auto* type : {"Eq8", "FilterEQ3", "Compressor2", "Limiter", "AutoFilter2", "ChannelEq"})
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
        if (auto* filter = dynamic_cast<devices::AutoFilter2*>(processor.get()))
        {
            using A = devices::AutoFilter2;
            // Type Morph shows the morph amount, DJ its own knob; Mode reaches the time and synced rates; a press on the
            // Envelope knob puts attack and release on the display bar.
            for (float filterType : {4.0f, 5.0f, 0.0f}) { filter->setParameter(A::FilterType, filterType); face->layout(); inspect(); }
            for (float mode : {1.0f, 2.0f, 0.0f}) { filter->setParameter(A::LfoTimeMode, mode); face->layout(); inspect(); }
            for (const auto& child : face->children())
                if (child->visible && child->midiTarget && child->midiTarget->param == filter->parameters()[A::EnvAmount].id)
                { click(root, localEvent(child->boundsInRoot())); break; }
            inspect();
        }
        for (const auto& parameter : processor->parameters()) CHECK(seen.contains(parameter.id) || hidden.contains(parameter.id));
    }
}

TEST_CASE("device faces: Auto Filter display drags frequency and resonance; the bar follows the touched modulator (UI-43b)")
{
    using A = devices::AutoFilter2;
    auto model = devices::nativeDeviceModel("AutoFilter2"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    auto* filter = dynamic_cast<A*>(processor.get()); REQUIRE(filter);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = referenceFace(root, model, processor.get(), [&](auto* widget, int parameter) { tagControl(widget, parameter, processor.get()); });
    const auto r = face->graphBounds();
    // Measured axis: 20 Hz .. 20 kHz across the display (1 kHz at capture x 510.5 of 176..768); resonance 0 at the handle row.
    const auto xOf = [&](float hz) { return r.x + r.w * (510.5f + 196.0f * std::log10(hz / 1000.0f) - 176.0f) / 592.0f; };
    ui::MouseEvent e; e.x = xOf(1000); e.y = r.y + r.h * (202.5f - 40.0f) / 176.0f;
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    CHECK_NEAR(filter->getParameter(A::Frequency), 1000, 10);
    CHECK_NEAR(filter->getParameter(A::Resonance), 0, 0.01);
    e.x = xOf(4000); e.y = r.y + r.h * (127.5f - 40.0f) / 176.0f; // half the travel up
    root.dispatchMouseDown(e); root.dispatchMouseUp(e);
    CHECK_NEAR(filter->getParameter(A::Frequency), 4000, 40);
    CHECK_NEAR(filter->getParameter(A::Resonance), 0.5f, 0.01);
    // The bar shows the LFO's settings until the Envelope knob is touched; then attack and release.
    auto shown = [&](int parameter) {
        for (const auto& child : face->children())
            if (child->visible && child->midiTarget && child->midiTarget->param == filter->parameters()[(size_t) parameter].id) return child.get();
        return (ui::Widget*) nullptr;
    };
    CHECK(shown(A::EnvAttack) == nullptr);
    REQUIRE(shown(A::EnvAmount));
    click(root, localEvent(shown(A::EnvAmount)->boundsInRoot()));
    CHECK(shown(A::EnvAttack)); CHECK(shown(A::EnvRelease));
    click(root, localEvent(shown(A::LfoAmount)->boundsInRoot()));
    CHECK(shown(A::EnvAttack) == nullptr);
    // Rate follows Mode; Clip and the 12 / 24 buttons edit the processor.
    filter->setParameter(A::LfoTimeMode, 1); face->layout();
    CHECK(shown(A::LfoTime)); CHECK(shown(A::LfoFrequency) == nullptr);
    click(root, localEvent(shown(A::SoftClip)->boundsInRoot())); CHECK(filter->getParameter(A::SoftClip) > 0.5f);
    filter->setParameter(A::SoftClip, 0);

    // The output spectrum behind the curve: noise through a low-pass at 1 kHz fills the display left of the cutoff.
    filter->prepare(48000, 512);
    root.setBounds({0, 0, 900, 360}); root.layout();
    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi; juce::Random random(7);
    for (int block = 0; block < 60; ++block)
    {
        for (int i = 0; i < 512; ++i) { const float v = random.nextFloat() * 0.6f - 0.3f; audio.setSample(0, i, v); audio.setSample(1, i, v); }
        filter->process(audio, midi);
        if (block % 6 == 0) { juce::Thread::sleep(32); root.tickTree(); }
    }
    auto* afFace = dynamic_cast<ui::ReferenceFace*>(face);
    REQUIRE(afFace != nullptr);
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests");
    dir.createDirectory();
    CHECK(gfx::renderToPng(root, 900, 360, 2, dir.getChildFile("autofilter_spectrum.png").getFullPathName().toStdString()));
}

TEST_CASE("device faces: the browser's Auto Filter is the 12.4 device; saving one raises a Live 12 header to 12.4 (UI-43b)")
{
    CHECK(model::insertedDeviceTag("AutoFilter2", "12.0_12120") == "AutoFilter2");
    CHECK(model::insertedDeviceTag("AutoFilter2", "") == "AutoFilter2");
    CHECK(model::insertedDeviceTag("AutoFilter2", "11.0_433") == "AutoFilter"); // Live 11 sets keep the legacy device
    CHECK(model::insertedDeviceTag("Eq8", "11.0_433") == "Eq8");

    const auto header = [](const juce::File& file) {
        std::string unread;
        auto dom = io::readAlsDom(file, unread);
        return dom ? (dom->getStringAttribute("MinorVersion") + " " + dom->getStringAttribute("SchemaChangeCount")).toStdString() : std::string();
    };
    for (const char* minor : {"12.0_12120", "11.0_433"})
        for (bool withFilter : {true, false})
        {
            io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 1; options.returnTracks = 0;
            auto dom = io::createNewSetDom(options);
            dom->setAttribute("MinorVersion", minor);
            juce::TemporaryFile source(".als"), saved(".als");
            std::string error; REQUIRE(io::writeAlsDom(*dom, source.getFile(), error));
            auto loaded = io::loadAls(source.getFile()); REQUIRE(loaded); auto& song = *loaded.song;
            const std::string type = withFilter ? std::string(model::insertedDeviceTag("AutoFilter2", song.schemaVersion)) : "Eq8";
            auto device = devices::nativeDeviceModel(type); model::allocateDeviceIds(song, device);
            device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
            song.tracks[1].devices.push_back(device);
            REQUIRE(io::saveAls(song, saved.getFile(), {}).ok);
            const bool raised = withFilter && std::string(minor) == "12.0_12120";
            if (raised) // disposable review set for opening in the reference (like the face review sets above)
            {
                const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
                REQUIRE(dir.createDirectory().wasOk());
                REQUIRE(io::saveAls(song, dir.getChildFile("Auto Filter 12.4.als"), {}).ok);
            }
            CHECK(header(saved.getFile()) == (raised ? std::string("12.0_12402 1") : std::string(minor) + " 4"));
            auto reopened = io::loadAls(saved.getFile()); REQUIRE(reopened);
            REQUIRE(reopened.song->tracks[1].devices.size() == 1);
            CHECK(reopened.song->tracks[1].devices[0].type == type);
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

TEST_CASE("device faces: EQ Eight draws real audio spectrum, with a working analyzer switch")
{
    using E = devices::EqEight;
    auto model = devices::nativeDeviceModel("Eq8"); devices::BuildContext build; auto processor = devices::createDevice(model, build);
    auto* eq = dynamic_cast<E*>(processor.get()); REQUIRE(eq); eq->prepare(48000, 2048);
    eq->setParameter(E::bandParam(0, E::On), 1); eq->setParameter(E::bandParam(0, E::Freq), 160);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = referenceFace(root, model, eq, {}, 239);
    root.tickTree();
    juce::AudioBuffer<float> audio(2, 2048); juce::MidiBuffer midi;
    uint32_t random = 42;
    for (int frame = 0; frame < 12; ++frame)
    {
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            const double seconds = (frame * audio.getNumSamples() + i) / 48000.0;
            random = random * 1664525u + 1013904223u;
            float signal = ((float) (random >> 8) / 16777216.0f - 0.5f) * 0.06f;
            for (int harmonic = 1; harmonic <= 32; ++harmonic)
                signal += (float) (0.22 / harmonic * std::sin(6.283185307179586 * 173 * harmonic * seconds));
            audio.setSample(0, i, signal); audio.setSample(1, i, -signal * 0.55f);
        }
        eq->process(audio, midi);
        juce::Thread::sleep(35); root.tickTree();
    }
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    REQUIRE(folder.createDirectory().wasOk());
    const auto on = folder.getChildFile("EqEight-spectrum.png"), off = folder.getChildFile("EqEight-spectrum-off.png");
    REQUIRE(gfx::renderToPng(root, (int) face->width(), (int) face->height(), 2, on.getFullPathName().toStdString()));
    float localX = 0, localY = 0;
    auto* button = dynamic_cast<ui::TextButton*>(face->findAt(face->at(649, 41, 678, 58).centreX(),
                                    face->at(649, 41, 678, 58).centreY(), localX, localY));
    REQUIRE(button); REQUIRE(button->isOn); CHECK(button->isOn());
    click(root, localEvent(button->boundsInRoot())); CHECK(! button->isOn());
    REQUIRE(gfx::renderToPng(root, (int) face->width(), (int) face->height(), 2, off.getFullPathName().toStdString()));
    const auto imageOn = juce::ImageFileFormat::loadFrom(on), imageOff = juce::ImageFileFormat::loadFrom(off);
    REQUIRE(imageOn.isValid()); REQUIRE(imageOff.isValid());
    const auto graph = face->graphBounds();
    int changed = 0;
    for (int y = (int) (graph.y * 2) + 4; y < (int) (graph.bottom() * 2) - 4; ++y)
        for (int x = (int) (graph.x * 2) + 4; x < (int) (graph.right() * 2) - 4; ++x)
            if (imageOn.getPixelAt(x, y) != imageOff.getPixelAt(x, y)) ++changed;
    CHECK(changed > 10000); // filled spectrum is present behind the unchanged response and handles
    // UI-55: the output level is the chain meter after the device, so the face has no meter of its own.
    int green = 0;
    for (int y = 0; y < imageOn.getHeight(); ++y)
        for (int x = 0; x < imageOn.getWidth(); ++x)
        {
            const auto pixel = imageOn.getPixelAt(x, y);
            if (pixel.getGreen() > 150 && pixel.getRed() < 80 && pixel.getBlue() < 140) ++green;
        }
    CHECK(green == 0);
    click(root, localEvent(button->boundsInRoot())); CHECK(button->isOn());
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

TEST_CASE("device faces: Channel EQ switch, frequency readout, knobs and macro-mapped shelves (DEV-30)")
{
    using C = devices::ChannelEq;
    auto model = devices::nativeDeviceModel("ChannelEq");
    for (auto& p : model.parameters) // the piano rack's Tone macro drives both shelves
        if (p.path == "LowShelfGain" || p.path == "HighShelfGain") { p.macroIndex = 6; p.rangeMin = 0.18; p.rangeMax = 5.6; }
    devices::BuildContext build; auto processor = devices::createDevice(model, build);
    ui::UiContext context; ui::RootWidget root(context);
    auto* face = referenceFace(root, model, processor.get(), [&](auto* widget, int parameter) { tagControl(widget, parameter, processor.get()); });
    // HP 80 Hz key (capture x 1076.5..1101.5, y 133.5..150) toggles the high-pass.
    click(root, localEvent(face->at(1076.5f, 133.5f, 1101.5f, 150)));
    CHECK_NEAR(processor->getParameter(C::HighpassOn), 1, 0);
    click(root, localEvent(face->at(1076.5f, 133.5f, 1101.5f, 150)));
    CHECK_NEAR(processor->getParameter(C::HighpassOn), 0, 0);
    // The readout drags the Mid frequency on its log law; a double click returns to 1.5 kHz.
    auto readout = localEvent(face->at(1136, 133.5f, 1201, 150));
    auto drag = readout;
    root.dispatchMouseDown(drag); drag.y -= 50; root.dispatchMouseDrag(drag); root.dispatchMouseUp(drag);
    CHECK_NEAR(processor->getParameter(C::MidFrequency), 1500 * std::pow(7500.0 / 120.0, 0.25), 1); // a quarter of the travel up
    readout.clicks = 2; root.dispatchMouseDown(readout); root.dispatchMouseUp(readout);
    CHECK_NEAR(processor->getParameter(C::MidFrequency), 1500, 0.01);
    // Knobs: Low / Mid / High / Output with dB text; the mapped shelves ignore hand edits, Mid and Output turn.
    std::vector<ui::Knob*> knobs; collect(*face, knobs);
    REQUIRE(knobs.size() == 4);
    processor->setParameter(C::LowShelfGain, std::pow(10.0f, -2.0f / 20));
    CHECK(knobs[0]->mapped && knobs[0]->mapped());
    CHECK(! knobs[1]->mapped);
    for (auto* knob : knobs)
    {
        auto k = localEvent(knob->boundsInRoot()); root.dispatchMouseDown(k); k.y -= 50; root.dispatchMouseDrag(k); root.dispatchMouseUp(k);
    }
    CHECK_NEAR(20 * std::log10(processor->getParameter(C::LowShelfGain)), -2, 0.001);
    CHECK_NEAR(processor->getParameter(C::HighShelfGain), 1, 0);
    CHECK_NEAR(20 * std::log10(processor->getParameter(C::MidGain)), 6, 0.05);  // 50 px of 200 = a quarter of +-12 dB
    CHECK_NEAR(20 * std::log10(processor->getParameter(C::Gain)), 6, 0.05);
    // The graph spans the measured display; painting it (curve and spectrum) leaves the face intact.
    CHECK(face->graphBounds().w > face->width() * 0.9f);
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul((int) face->width() + 1, (int) face->height() + 1));
    const ui::Theme theme;
    root.paintTree(*surface->getCanvas(), theme);
    CHECK(face->visible);
}
