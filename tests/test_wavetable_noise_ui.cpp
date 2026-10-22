#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/OscillatorMedia.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableSynth.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "midi/MidiManager.h"
#include "ui/RootWidget.h"
#include "ui/views/WavetableEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <set>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
const juce::File directory("/tmp/sonora_tests/sonora_wavetable_noise_ui_tests");
juce::File ownNoiseFile(int channels = 2)
{
    REQUIRE(directory.createDirectory().wasOk()); auto file = directory.getChildFile(channels == 2 ? "Own Stereo Noise.wav" : "Unsupported Four Channel.wav");
    REQUIRE(file.deleteFile()); std::unique_ptr<juce::OutputStream> stream = file.createOutputStream(); REQUIRE(stream);
    juce::WavAudioFormat format; auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(channels).withBitsPerSample(24)); REQUIRE(writer);
    juce::AudioBuffer<float> samples(channels, 64);
    for (int channel = 0; channel < channels; ++channel) for (int frame = 0; frame < 64; ++frame)
        samples.setSample(channel, frame, static_cast<float>((channel == 0 ? .6 * std::sin(frame * 6.283185307179586 / 63) : .35 * std::cos(frame * 6.283185307179586 / 63))));
    REQUIRE(writer->writeFromAudioSampleBuffer(samples, 0, samples.getNumSamples())); writer.reset(); return file;
}
juce::var content(const model::Device& device)
{
    REQUIRE(device.nativeContent);
    return juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(device.nativeContent->data.data()), static_cast<int>(device.nativeContent->data.size())));
}
struct NoiseUi
{
    juce::ScopedJuceInitialiser_GUI init;
    model::Song song;
    model::EditHistory history;
    midi::MidiManager midi;
    ui::UiContext context;
    std::unique_ptr<devices::DeviceProcessor> processor;
    std::vector<std::unique_ptr<devices::DeviceProcessor>> retired;
    std::unique_ptr<ui::RootWidget> root;
    W* synth = nullptr;
    Editor* editor = nullptr;
    int chain = -1, child = -1, edits = 0, selection = 1;
    std::string fileChoice;
    bool missing = false;
    explicit NoiseUi(float height = 700, bool nested = false, bool expectMissing = false)
    {
        missing = expectMissing; fileChoice = ownNoiseFile().getFullPathName().toStdString();
        const auto source = directory.getChildFile(nested ? "Rack Source.als" : "Top Source.als");
        io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
        std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error)); auto loaded = io::loadAls(source); REQUIRE(loaded); song = std::move(*loaded.song);
        auto target = devices::nativeDeviceModel("SonoraWavetable"); target.id = nested ? 77 : 42; target.displayName = "Own Noise Patch";
        REQUIRE(devices::storeWavetableVoiceRate(target, 1, error)); auto value = content(target); value.getDynamicObject()->setProperty("retainedSourceBytes", "AQIDBA==");
        auto payload = std::make_shared<model::NativeContent>(*target.nativeContent); const auto encoded = juce::JSON::toString(value, true);
        const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.toRawUTF8()); payload->data.assign(bytes, bytes + encoded.getNumBytesAsUTF8()); payload->controllerData = {0, 1, 127, 128, 255}; target.nativeContent = payload;
        model::Device initial = target;
        if (nested)
        {
            initial = devices::nativeDeviceModel("InstrumentGroupDevice"); initial.id = 42; initial.chains.resize(1);
            initial.chains[0].devices = {devices::nativeDeviceModel("SonoraWavetable"), devices::nativeDeviceModel("StereoGain"), std::move(target)};
            initial.chains[0].devices[0].id = 88; initial.chains[0].devices[1].id = 89; chain = 0; child = 2;
        }
        model::allocateDeviceIds(song, initial); initial.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(initial)); history.perform(song, model::insertDevice(0, std::move(initial))); history.clear();
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        context.showChoices = [this](const auto&, const auto&, auto choose) { choose(selection); };
        context.editDevice = [this](int track, int device, const auto& edit, const std::string& label) {
            CHECK(track == 0); CHECK(device == 0); CHECK(label == "Import Noise Sample" || label == "Use Generated Noise");
            devices::captureDeviceState(targetIn(song.tracks[0].devices[0]), processor.get());
            auto replacement = song.tracks[0].devices[0]; const int oldId = replacement.id; edit(replacement); replacement.id = -1; model::allocateDeviceIds(song, replacement);
            if (const auto view = context.wavetableFxViews.find(oldId); view != context.wavetableFxViews.end()) context.wavetableFxViews[replacement.id] = view->second;
            history.perform(song, model::replaceDevice(song, 0, 0, std::move(replacement), label)); ++edits; missing = false; refresh();
        };
        refresh();
    }
    model::Device& targetIn(model::Device& value) const { return chain < 0 ? value : value.chains[static_cast<size_t>(chain)].devices[static_cast<size_t>(child)]; }
    model::Device& target() { return targetIn(song.tracks[0].devices[0]); }
    void refresh()
    {
        std::vector<std::string> warnings; auto next = devices::createNative(target(), &warnings); REQUIRE(next); CHECK(warnings.empty());
        auto* wavetable = dynamic_cast<W*>(next.get()); REQUIRE(wavetable); if (missing) wavetable->setNoiseSampleExpected(true); next->prepare(48000, 512);
        if (processor) retired.push_back(std::move(processor)); processor = std::move(next); synth = wavetable;
        root->removeAllChildren(); editor = root->add<Editor>(synth, 0, 0, chain, child); editor->setBounds(root->localBounds()); editor->layout();
        editor->onChooseNoiseFile = [this](auto complete) { complete(fileChoice); };
    }
    ui::MouseEvent at(ui::Rect area) const { ui::MouseEvent event; event.x = area.centreX(); event.y = area.centreY(); return event; }
    void click(ui::Rect area, int clicks = 1)
    { REQUIRE(! area.isEmpty()); auto event = at(area); event.clicks = clicks; root->dispatchMouseDown(event); root->dispatchMouseUp(event); }
    void drag(int parameter, float pixels, bool fine = false)
    {
        const auto area = editor->parameterCell(parameter); REQUIRE(! area.isEmpty()); auto down = at(area); down.shift = fine; root->dispatchMouseDown(down);
        auto moved = down; moved.y -= pixels; root->dispatchMouseDrag(moved); root->dispatchMouseUp(moved);
    }
    void noiseInspector()
    {
        if (! editor->inspectorOpen()) click(editor->noiseMoreCell()); else click(editor->sectionCell(Editor::Section::Noise));
        REQUIRE(editor->section() == Editor::Section::Noise); REQUIRE(! editor->noisePlot().isEmpty());
    }
    void drop(const std::string& path)
    { context.browserDrag = ui::BrowserDrag{ui::BrowserDrag::Kind::Sample, path, "Own sample"}; root->dispatchMouseUp(at(editor->noisePlot())); }
    juce::Image snapshot(const char* name)
    {
        const auto file = directory.getChildFile(name); REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1, file.getFullPathName().toStdString()));
        auto image = juce::ImageFileFormat::loadFrom(file); REQUIRE(image.isValid()); return image;
    }
    void saveAndReload(const char* name, bool sample)
    {
        const auto file = directory.getChildFile(name); REQUIRE(io::saveAls(song, file, io::AlsSaveOptions{}).ok); auto loaded = io::loadAls(file); REQUIRE(loaded);
        REQUIRE(loaded.song->tracks[0].devices.size() == 1); auto& reloaded = targetIn(loaded.song->tracks[0].devices[0]);
        CHECK(reloaded.displayName == "Own Noise Patch"); CHECK(reloaded.nativeContent->controllerData == target().nativeContent->controllerData);
        CHECK(content(reloaded).getProperty("retainedSourceBytes", {}).toString() == "AQIDBA==");
        auto next = devices::createNative(reloaded); auto* wavetable = dynamic_cast<W*>(next.get()); REQUIRE(wavetable);
        if (sample) { REQUIRE(wavetable->noiseSample()); CHECK(wavetable->noiseSampleName() == "Own Stereo Noise"); CHECK(wavetable->noiseSample()->pcmFingerprint() == synth->noiseSample()->pcmFingerprint()); }
        else { CHECK(wavetable->noiseSample() == nullptr); CHECK(! wavetable->expectsNoiseSample()); }
        CHECK_NEAR(wavetable->getParameter(W::NoisePhase), synth->getParameter(W::NoisePhase), 0.000001);
    }
};
void checkPcmPreview(const NoiseUi& rig, const juce::Image& image)
{
    const auto plot = rig.editor->noisePlot(); REQUIRE(! plot.isEmpty()); REQUIRE(rig.synth->noiseSample());
    const int channels = rig.synth->noiseSample()->channels();
    for (int channel = 0; channel < channels; ++channel) for (const float position : {.125f, .375f, .625f, .875f})
    {
        const float h = plot.h / static_cast<float>(channels), x = plot.x + position * plot.w;
        const float y = plot.y + (static_cast<float>(channel) + .5f) * h - rig.synth->noiseWaveformAt(position, channel) * h * .4f;
        bool stroke = false;
        for (int dx = -2; dx <= 2; ++dx) for (int dy = -3; dy <= 3; ++dy)
        {
            const int px = static_cast<int>(std::lround(x)) + dx, py = static_cast<int>(std::lround(y)) + dy;
            if (px < 0 || px >= image.getWidth() || py < 0 || py >= image.getHeight()) continue;
            const auto color = image.getPixelAt(px, py);
            // Thin antialiased blue strokes have less green coverage than cyan.
            // Require the lane's color near the independently predicted PCM point.
            stroke |= color.getRed() < 150 && color.getGreen() > (channel == 0 ? 110 : 80)
                && color.getBlue() > (channel == 0 ? 120 : 110);
        }
        CHECK(stroke);
    }
}
} // namespace

TEST_CASE("wavetable Noise UI: file picker clones immutable PCM and native scalar gestures MIDI and the stereo preview use actual data")
{
    NoiseUi rig; rig.snapshot("Generated Noise.png"); auto* old = rig.synth; const auto original = rig.target().nativeContent; const auto bytes = original->data;
    rig.synth->setParameter(W::NoiseOn, 1); rig.click(rig.editor->noiseImportCell()); CHECK(rig.edits == 1); CHECK(rig.synth != old); CHECK(old->noiseSample() == nullptr);
    REQUIRE(rig.synth->noiseSample()); CHECK(rig.synth->noiseSampleName() == "Own Stereo Noise"); CHECK(rig.editor->parameterCell(W::NoiseColor).isEmpty()); CHECK_NEAR(rig.synth->getParameter(W::NoiseOn), 1, 0);
    for (int parameter : {W::NoisePitch, W::NoiseFine, W::NoiseKeytrack, W::NoisePhase, W::NoiseRandom}) REQUIRE(! rig.editor->parameterCell(parameter).isEmpty());
    rig.drag(W::NoisePitch, 20); CHECK_NEAR(rig.synth->getParameter(W::NoisePitch), 24, 0.00002);
    rig.drag(W::NoiseFine, 10, true); CHECK_NEAR(rig.synth->getParameter(W::NoiseFine), 1.92, 0.00002);
    rig.drag(W::NoisePhase, 50); CHECK_NEAR(rig.synth->getParameter(W::NoisePhase), .25, 0.000001);
    rig.drag(W::NoiseRandom, 20); CHECK_NEAR(rig.synth->getParameter(W::NoiseRandom), .1, 0.000001);
    rig.click(rig.editor->parameterCell(W::NoiseKeytrack)); CHECK_NEAR(rig.synth->getParameter(W::NoiseKeytrack), 1, 0);
    rig.click(rig.editor->parameterCell(W::NoisePitch), 2); CHECK_NEAR(rig.synth->getParameter(W::NoisePitch), 0, 0);
    for (int parameter : {W::NoisePitch, W::NoiseFine, W::NoiseKeytrack, W::NoisePhase, W::NoiseRandom})
    {
        rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(parameter)); REQUIRE(rig.context.mapSelection);
        CHECK(rig.context.mapSelection->param == rig.synth->parameters()[static_cast<size_t>(parameter)].id); rig.midi.cancelLearn(); rig.context.midiMapMode = false;
    }
    checkPcmPreview(rig, rig.snapshot("Sample Noise.png")); rig.noiseInspector(); checkPcmPreview(rig, rig.snapshot("Sample Noise Inspector.png"));
    CHECK(rig.target().nativeContent->controllerData == original->controllerData); CHECK(original->data == bytes);
    auto* sampleOwner = rig.synth; rig.selection = 0; rig.click(rig.editor->noiseAssetCell()); CHECK(rig.edits == 2); CHECK(rig.synth->noiseSample() == nullptr); CHECK(! rig.synth->expectsNoiseSample());
    REQUIRE(sampleOwner->noiseSample()); CHECK(! rig.editor->parameterCell(W::NoiseColor).isEmpty()); rig.snapshot("Generated Noise Inspector.png");
}

TEST_CASE("wavetable Noise UI: minimum rack drop targets the correct child and preserves source controls immutable undo and ALS reload")
{
    NoiseUi rig(500, true); const auto other = rig.song.tracks[0].devices[0].chains[0].devices[0];
    rig.synth->setParameter(W::NoisePhase, .2f); rig.synth->setParameter(W::sourceRoutingParam(3, W::SourceBus1), .37f); rig.drop(rig.fileChoice);
    CHECK(rig.edits == 1); REQUIRE(rig.synth->noiseSample()); CHECK(rig.synth->getParameter(W::mediaParam(0, W::MediaMode)) < .5f);
    CHECK_NEAR(rig.synth->getParameter(W::NoisePhase), .2, 0.000001); CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(3, W::SourceBus1)), .37, 0.000001);
    CHECK(rig.song.tracks[0].devices[0].chains[0].devices[0].nativeContent == other.nativeContent);
    // A switched-off module is veiled on the dashboard (UI-28e); show the Noise preview at full contrast.
    rig.synth->setParameter(W::NoiseOn, 1);
    const auto sampleContent = rig.target().nativeContent; checkPcmPreview(rig, rig.snapshot("Minimum Sample Noise.png")); rig.saveAndReload("Noise Rack.als", true);
    rig.noiseInspector(); const auto scalar = rig.editor->parameterCell(W::NoiseRandom); REQUIRE(! scalar.isEmpty());
    CHECK(scalar.y >= rig.editor->noisePlot().bottom()); rig.root->setBounds({0, 0, 1080, 210}); rig.editor->setBounds(rig.root->localBounds()); rig.editor->layout();
    checkPcmPreview(rig, rig.snapshot("Compact Sample Noise.png")); const int page = rig.editor->page(); rig.selection = 0; rig.click(rig.editor->noiseAssetCell());
    CHECK(rig.edits == 2); CHECK(rig.editor->section() == Editor::Section::Noise); CHECK(rig.editor->page() == page); rig.saveAndReload("Generated Rack.als", false);
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); REQUIRE(rig.synth->noiseSample()); CHECK(rig.target().nativeContent->controllerData == sampleContent->controllerData); CHECK(rig.editor->section() == Editor::Section::Noise);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->noiseSample() == nullptr); rig.snapshot("Compact Generated Noise.png");
}

TEST_CASE("wavetable Noise UI: compact inspector reaches all base controls and invalid or cancelled file choices never replace the sample")
{
    NoiseUi rig(210); std::set<int> reached;
    for (int section = 0; section < static_cast<int>(Editor::Section::Count); ++section)
    {
        rig.click(rig.editor->sectionCell(static_cast<Editor::Section>(section)));
        for (int instance = 0; instance < rig.editor->instanceCount(); ++instance)
        {
            for (int count = 0; count < rig.editor->instanceCount() && rig.editor->selectedInstance() != instance; ++count) rig.click(rig.editor->selectorCell(1));
            for (int page = 0; page < rig.editor->pageCount(); ++page)
            { for (int parameter : rig.editor->visibleParameters()) reached.insert(parameter); if (rig.editor->pageCount() > 1) rig.click(rig.editor->pageCell(1)); }
        }
    }
    CHECK(reached.size() == static_cast<size_t>(W::ParameterCount)); rig.noiseInspector(); rig.fileChoice.clear(); rig.click(rig.editor->noiseImportCell()); CHECK(rig.edits == 0);
    rig.fileChoice = ownNoiseFile(4).getFullPathName().toStdString(); const auto before = rig.target().nativeContent; rig.click(rig.editor->noiseImportCell());
    CHECK(rig.edits == 0); CHECK(rig.target().nativeContent == before); CHECK(rig.context.statusText == "Oscillator media file requires one or two channels");
    rig.fileChoice = ownNoiseFile().getFullPathName().toStdString(); rig.click(rig.editor->noiseImportCell()); REQUIRE(rig.synth->noiseSample()); const auto* sample = rig.synth->noiseSample();
    rig.drop("/tmp/sonora_tests/missing-noise-file.wav"); CHECK(rig.edits == 1); CHECK(rig.synth->noiseSample() == sample);
    CHECK(rig.context.statusText == "Oscillator media file does not exist");
}

TEST_CASE("wavetable Noise UI: an expected missing sample stays unavailable until explicit Generated selection")
{
    NoiseUi rig(500, false, true); CHECK(rig.synth->noiseSample() == nullptr); CHECK(rig.synth->expectsNoiseSample()); CHECK(rig.synth->noiseSampleName() == "Missing Noise Sample");
    CHECK(rig.editor->parameterCell(W::NoiseColor).isEmpty()); rig.snapshot("Missing Noise.png"); rig.selection = 0; rig.click(rig.editor->noiseAssetCell());
    CHECK(rig.edits == 1); CHECK(! rig.synth->expectsNoiseSample()); CHECK(rig.synth->noiseSampleName() == "Generated Noise"); CHECK(! rig.editor->parameterCell(W::NoiseColor).isEmpty());
}
