#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
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

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
juce::var content(const model::Device& device)
{
    REQUIRE(device.nativeContent);
    return juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char*>(device.nativeContent->data.data()), static_cast<int>(device.nativeContent->data.size())));
}
struct QualityUi
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
    int chain = -1, child = -1, selection = 1, edits = 0;
    std::vector<std::string> choices;
    std::vector<bool> ticked;
    std::shared_ptr<const model::NativeContent> beforeEdit;
    explicit QualityUi(float height = 700, bool nested = false)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_quality_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        const auto source = directory.getChildFile(nested ? "Source Rack.als" : "Source Top.als");
        io::NewSetOptions options; options.midiTracks = 1; options.audioTracks = 0; options.returnTracks = 0;
        std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
        auto loaded = io::loadAls(source); REQUIRE(loaded); song = std::move(*loaded.song);
        auto target = devices::nativeDeviceModel("SonoraWavetable"); target.id = nested ? 77 : 42; target.displayName = "Own Quality Patch";
        std::vector<float> samples(64);
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * 6.283185307179586 / 64));
        const auto table = devices::WavetableData::fromSamples(samples, 1, 64, 48000, "Own quality table"); REQUIRE(table);
        REQUIRE(devices::storeWavetableTable(target, 1, *table.data, error)); REQUIRE(devices::storeWavetableVoiceRate(target, 1, error));
        auto value = content(target); value.getDynamicObject()->setProperty("retainedSourceBytes", "AQIDBA==");
        auto payload = std::make_shared<model::NativeContent>(*target.nativeContent); const auto encoded = juce::JSON::toString(value, true);
        const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.toRawUTF8()); payload->data.assign(bytes, bytes + encoded.getNumBytesAsUTF8());
        payload->controllerData = {0, 1, 127, 128, 255}; payload->sourceDirectory = directory.getFullPathName().toStdString(); target.nativeContent = payload;
        model::Device initial = target;
        if (nested)
        {
            initial = devices::nativeDeviceModel("InstrumentGroupDevice"); initial.id = 42; initial.chains.resize(2);
            initial.chains[0].devices = {devices::nativeDeviceModel("StereoGain")}; initial.chains[0].devices[0].id = 90;
            initial.chains[1].devices = {devices::nativeDeviceModel("SonoraWavetable"), devices::nativeDeviceModel("StereoGain"), devices::nativeDeviceModel("StereoGain"), std::move(target)};
            initial.chains[1].devices[0].id = 88; initial.chains[1].devices[1].id = 91; initial.chains[1].devices[2].id = 92; chain = 1; child = 3;
        }
        model::allocateDeviceIds(song, initial); initial.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(initial));
        history.perform(song, model::insertDevice(0, std::move(initial))); history.clear();
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        context.showChoices = [this](const auto& offered, const auto& selected, auto pick) { choices = offered; ticked = selected; pick(selection); };
        context.editDevice = [this](int track, int device, const auto& edit, const std::string& label) {
            CHECK(track == 0); CHECK(device == 0); CHECK(label == "Change Voice Quality");
            devices::captureDeviceState(targetIn(song.tracks[0].devices[0]), processor.get()); beforeEdit = this->target().nativeContent;
            auto replacement = song.tracks[0].devices[0]; const int oldId = replacement.id; edit(replacement);
            replacement.id = -1; model::allocateDeviceIds(song, replacement);
            if (const auto saved = context.wavetableFxViews.find(oldId); saved != context.wavetableFxViews.end()) context.wavetableFxViews[replacement.id] = saved->second;
            history.perform(song, model::replaceDevice(song, 0, 0, std::move(replacement), label)); ++edits; refresh();
        };
        refresh();
    }
    model::Device& targetIn(model::Device& device) const { return chain < 0 ? device : device.chains[static_cast<size_t>(chain)].devices[static_cast<size_t>(child)]; }
    model::Device& target() { return targetIn(song.tracks[0].devices[0]); }
    void refresh()
    {
        std::vector<std::string> warnings; auto next = devices::createNative(target(), &warnings); REQUIRE(next); CHECK(warnings.empty()); next->prepare(48000, 512);
        if (processor) retired.push_back(std::move(processor)); processor = std::move(next); synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        root->removeAllChildren(); editor = root->add<Editor>(synth, 0, 0, chain, child); editor->setBounds(root->localBounds()); editor->layout();
    }
    void click(ui::Rect area)
    {
        REQUIRE(! area.isEmpty()); ui::MouseEvent event; event.x = area.centreX(); event.y = area.centreY(); root->dispatchMouseDown(event); root->dispatchMouseUp(event);
    }
    void global() { click(editor->workflowCell(Editor::Page::Global)); REQUIRE(! editor->voiceQualityCell().isEmpty()); }
    void snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_quality_ui_tests");
        REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1, directory.getChildFile(name).getFullPathName().toStdString()));
    }
    void saveAndReload(const char* name, int factor)
    {
        const auto file = juce::File("/tmp/sonora_tests/sonora_wavetable_quality_ui_tests").getChildFile(name);
        REQUIRE(io::saveAls(song, file, io::AlsSaveOptions{}).ok); auto loaded = io::loadAls(file); REQUIRE(loaded);
        REQUIRE(loaded.song->tracks[0].devices.size() == 1); auto& reloaded = targetIn(loaded.song->tracks[0].devices[0]);
        CHECK(reloaded.displayName == "Own Quality Patch"); CHECK(reloaded.nativeContent->controllerData == target().nativeContent->controllerData);
        CHECK(content(reloaded).getProperty("retainedSourceBytes", {}).toString() == "AQIDBA==");
        auto next = devices::createNative(reloaded); auto* wavetable = dynamic_cast<W*>(next.get()); REQUIRE(wavetable); next->prepare(48000, 512);
        CHECK(wavetable->requestedVoiceRateFactor() == factor); CHECK(wavetable->voiceRateFactor() == factor); REQUIRE(wavetable->customTable(1)); CHECK(wavetable->customTable(1)->name() == "Own quality table");
    }
};
} // namespace

TEST_CASE("wavetable quality UI: Global choice clones prepared voice quality keeps the published processor unchanged and saves undo redo")
{
    QualityUi rig; rig.global(); const auto original = rig.target().nativeContent; const auto originalBytes = original->data; auto* old = rig.synth;
    rig.synth->setParameter(W::SubPhase, .37f); const auto parameters = rig.synth->parameters().size(); rig.snapshot("Global Quality 1x.png");
    rig.click(rig.editor->voiceQualityCell()); CHECK(rig.edits == 1); CHECK(rig.choices == std::vector<std::string>({"1×", "2×"})); CHECK(rig.ticked == std::vector<bool>({true, false}));
    CHECK(rig.synth != old); CHECK(old->requestedVoiceRateFactor() == 1); CHECK(old->voiceRateFactor() == 1);
    CHECK(rig.synth->requestedVoiceRateFactor() == 2); CHECK(rig.synth->voiceRateFactor() == 2); CHECK(rig.synth->parameters().size() == parameters);
    CHECK(rig.editor->workflow() == Editor::Page::Global); CHECK(! rig.editor->inspectorOpen()); CHECK_NEAR(rig.synth->getParameter(W::SubPhase), .37, 0.000001);
    CHECK(rig.target().nativeContent != original); CHECK(original->data == originalBytes); CHECK(rig.target().nativeContent->controllerData == original->controllerData);
    rig.snapshot("Global Quality 2x.png"); rig.saveAndReload("Quality Top.als", 2);
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(rig.synth->voiceRateFactor() == 1); CHECK(rig.target().nativeContent == rig.beforeEdit);
    CHECK(rig.editor->workflow() == Editor::Page::Global); rig.saveAndReload("Undo Top.als", 1);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->voiceRateFactor() == 2); rig.saveAndReload("Redo Top.als", 2);
    rig.selection = 1; rig.click(rig.editor->voiceQualityCell()); CHECK(rig.edits == 1);
    rig.selection = -1; rig.click(rig.editor->voiceQualityCell()); CHECK(rig.edits == 1);
    rig.selection = 0; rig.click(rig.editor->voiceQualityCell()); CHECK(rig.edits == 2); CHECK(rig.synth->voiceRateFactor() == 1);
}

TEST_CASE("wavetable quality UI: compact rack choice preserves exact child source controller live parameters page and unmapped configuration")
{
    QualityUi rig(210, true); rig.global();
    while (rig.editor->pageCount() > 1 && rig.editor->parameterCell(W::SubPhase).isEmpty()) rig.click(rig.editor->pageCell(1));
    const int page = rig.editor->page(); const auto other = rig.song.tracks[0].devices[0].chains[1].devices[0];
    rig.synth->setParameter(W::sourceRoutingParam(4, W::SourceBus1), .4f); rig.snapshot("Compact Quality 1x.png");
    rig.context.midiMapMode = true; rig.click(rig.editor->voiceQualityCell()); CHECK(! rig.context.mapSelection); rig.context.midiMapMode = false;
    CHECK(rig.edits == 1); CHECK(rig.editor->workflow() == Editor::Page::Global); CHECK(rig.editor->section() == Editor::Section::Main);
    CHECK(rig.editor->inspectorOpen()); CHECK(rig.editor->page() == page); CHECK(rig.synth->voiceRateFactor() == 2);
    CHECK_NEAR(rig.synth->getParameter(W::sourceRoutingParam(4, W::SourceBus1)), .4, 0.000001);
    CHECK(rig.song.tracks[0].devices[0].chains[1].devices[0].nativeContent == other.nativeContent); CHECK(rig.song.tracks[0].devices[0].chains[1].devices[0].displayName == other.displayName);
    CHECK(content(rig.target()).getProperty("retainedSourceBytes", {}).toString() == "AQIDBA=="); rig.snapshot("Compact Quality 2x.png"); rig.saveAndReload("Quality Rack.als", 2);
    REQUIRE(rig.history.undo(rig.song)); rig.refresh(); CHECK(rig.synth->voiceRateFactor() == 1); CHECK(rig.editor->page() == page);
    REQUIRE(rig.history.redo(rig.song)); rig.refresh(); CHECK(rig.synth->voiceRateFactor() == 2); CHECK(rig.editor->page() == page);
    rig.root->setBounds({0, 0, 1080, 500}); rig.editor->setBounds(rig.root->localBounds()); rig.editor->layout();
    CHECK(! rig.editor->voiceQualityCell().isEmpty()); CHECK(rig.editor->voiceQualityCell().bottom() <= rig.editor->parameterCell(W::Volume).y);
    rig.snapshot("Minimum Quality 2x.png");
}

TEST_CASE("wavetable quality UI: saved inspector view is clamped and malformed content never commits a quality edit")
{
    QualityUi rig(210); auto& view = rig.context.wavetableFxViews[rig.target().id];
    view.inspectorState = true; view.inspector = true; view.workflow = static_cast<int>(Editor::Page::Global); view.section = static_cast<int>(Editor::Section::Main); view.instance = 999; view.page = 999;
    rig.refresh(); CHECK(rig.editor->selectedInstance() == 0); CHECK(rig.editor->page() == rig.editor->pageCount() - 1); REQUIRE(! rig.editor->voiceQualityCell().isEmpty());
    auto invalid = std::make_shared<model::NativeContent>(); invalid->format = "sonora-wavetable-v1"; invalid->data = {'b', 'a', 'd'}; rig.target().nativeContent = invalid;
    rig.click(rig.editor->voiceQualityCell()); CHECK(rig.edits == 0); CHECK(rig.target().nativeContent == invalid); CHECK(rig.synth->voiceRateFactor() == 1);
    CHECK(rig.context.statusText == "Invalid native wavetable content");
}
