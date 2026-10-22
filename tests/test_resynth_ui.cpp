#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Resynth.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "ui/views/ResynthEditor.h"
#include "ui/views/DetailView.h"
#include "ui/views/MainLayout.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/TrackMenu.h"
#include "ui/widgets/Buttons.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/Splitter.h"
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;
namespace
{
std::shared_ptr<const model::ResynthModel> ownModel()
{
    auto model = std::make_shared<model::ResynthModel>(); model->source.resize(48000, 0.3f);
    model::ResynthNote note; note.end = 1; note.root = 33; note.confidence = 0.98f; note.sustain = 0.7f;
    model::ResynthFrame first; first.cosine[0] = 0.5f; first.pitch = 33; first.confidence = 0.98f;
    auto last = first; last.time = 1; note.frames = {first, last}; model->notes = {note}; return model;
}
ui::TextButton* button(ui::Widget& root, const char* text)
{
    for (const auto& widget : root.children())
        if (auto* found = dynamic_cast<ui::TextButton*>(widget.get()); found && found->label() == text) return found;
    return nullptr;
}
void click(ui::RootWidget& root, ui::Rect bounds)
{
    ui::MouseEvent event; event.x = bounds.centreX(); event.y = bounds.centreY(); root.dispatchMouseDown(event); root.dispatchMouseUp(event);
}
}
TEST_CASE("resynth UI: source synth hybrid, imports, phrase action and mapped controls are reachable")
{
    juce::ScopedJuceInitialiser_GUI gui;
    devices::Resynth processor(ownModel()); processor.prepare(48000, 512);
    ui::UiContext context; ui::RootWidget root(context);
    int tagged = 0;
    auto* editor = root.add<ui::ResynthEditor>(&processor, 2, 3, 4, 5, [&](auto*, int) { ++tagged; });
    root.setBounds({0, 0, 1080, 368}); editor->setBounds({0, 0, 1080, 368}); editor->layout();
    CHECK(tagged > 50);
    for (const auto& pair : {std::pair{"Reference", 2}, std::pair{"Synth", 0}, std::pair{"Hybrid", 1}})
    { auto* control = button(*editor, pair.first); REQUIRE(control); click(root, control->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::Mode), pair.second, 0); }
    int imports = 0, phrases = 0;
    context.importResynth = [&](int track, int device, const auto& path, int chain, int child) {
        ++imports; CHECK(track == 2); CHECK(device == 3); CHECK(chain == 4); CHECK(child == 5); CHECK(path == "/tmp/own.wav");
    };
    context.createResynthPhrase = [&](int track, int device, int chain, int child) { ++phrases; CHECK(track == 2 && device == 3 && chain == 4 && child == 5); };
    CHECK(editor->browserDrop({ui::BrowserDrag::Kind::Sample, "/tmp/own.wav", "Own"}, {})); CHECK(imports == 1);
    CHECK(! editor->browserDrop({ui::BrowserDrag::Kind::NativeDevice, "Utility", "Utility"}, {}));
    REQUIRE(button(*editor, "Create MIDI")); click(root, button(*editor, "Create MIDI")->bounds()); CHECK(phrases == 1);
    click(root, button(*editor, "Auto key map")->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::Example), 1, 0);
    click(root, button(*editor, "Auto key map")->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::Example), 0, 0);
    click(root, button(*editor, "Audition")->bounds());
    juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer midi; processor.process(audio, midi); CHECK(audio.getMagnitude(0, 512) > 0.1f);
    click(root, button(*editor, "Audition")->bounds());
}
TEST_CASE("resynth UI: note edge trim commits one saved structural model edit")
{
    juce::ScopedJuceInitialiser_GUI gui;
    devices::Resynth processor(ownModel()); ui::UiContext context; ui::RootWidget root(context);
    auto* editor = root.add<ui::ResynthEditor>(&processor, 0, 0, -1, -1);
    root.setBounds({0, 0, 1080, 368}); editor->setBounds({0, 0, 1080, 368});
    auto device = devices::nativeDeviceModel("SonoraResynth"); std::string error; device.nativeContent = model::encodeResynth(*ownModel(), error);
    int edits = 0; context.editDevice = [&](int track, int index, const auto& operation, const auto&) { ++edits; CHECK(track == 0 && index == 0); operation(device); };
    const auto bounds = editor->waveformBounds(); ui::MouseEvent event; event.x = bounds.x + 1; event.y = bounds.centreY();
    root.dispatchMouseDown(event); event.x = bounds.x + bounds.w * 0.2f; root.dispatchMouseDrag(event); root.dispatchMouseUp(event);
    CHECK(edits == 1); const auto model = model::decodeResynth(*device.nativeContent, error); REQUIRE(model);
    CHECK_NEAR(model->notes[0].start, 0.2, 1e-6); CHECK_NEAR(model->notes[0].end, 1, 0);
}

TEST_CASE("resynth UI: audio clip menu dispatches stem conversion with its exact identity")
{
    model::Song song; model::Track track; track.kind = model::TrackKind::Audio;
    model::Clip clip; clip.kind = model::Clip::Kind::Audio; clip.uid = model::allocateClipUid(); track.arrangementClips.push_back(clip);
    song.tracks.push_back(track); ui::UiContext context; context.song = &song;
    context.applyEdit = [&](model::EditCommand command) { if (command.apply) command.apply(song); };
    int called = 0; context.resynthesizeClip = [&](const model::ClipLocation& location) { ++called; CHECK(location.track == 0); CHECK(location.uid == clip.uid); };
    auto actions = ui::clipContextActions(context, model::ClipLocation::arrangement(0, clip.uid));
    const auto found = std::find_if(actions.begin(), actions.end(), [](const auto& action) { return action.label == "Resynthesize to New MIDI Track"; });
    REQUIRE(found != actions.end()); CHECK(found->enabled); REQUIRE(found->run); found->run(); CHECK(called == 1);
}

TEST_CASE("resynth UI: harmonic gestures, pages and source removal preserve a playable model")
{
    juce::ScopedJuceInitialiser_GUI gui;
    devices::Resynth processor(ownModel()); ui::UiContext context; ui::RootWidget root(context);
    auto* editor = root.add<ui::ResynthEditor>(&processor, 0, 0, -1, -1);
    root.setBounds({0, 0, 1080, 368}); editor->setBounds({0, 0, 1080, 368});
    const auto spectrum = editor->spectrumBounds(); ui::MouseEvent event;
    event.x = spectrum.x + spectrum.w * 2.5f / 96; event.y = spectrum.centreY();
    root.dispatchMouseDown(event); event.y -= 20; root.dispatchMouseDrag(event); root.dispatchMouseUp(event);
    CHECK_NEAR(processor.getParameter(devices::Resynth::Harmonic0 + 2), 1.4, 1e-6);
    click(root, button(*editor, "Reset spectrum")->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::Harmonic0 + 2), 1, 0);
    REQUIRE(button(*editor, "Tempo sync")); CHECK(! button(*editor, "Tempo sync")->visible);
    click(root, button(*editor, "Motion & voices")->bounds()); CHECK(button(*editor, "Tempo sync")->visible);
    click(root, button(*editor, "Tempo sync")->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::LfoSync), 1, 0);
    click(root, button(*editor, "Poly 8")->bounds()); CHECK_NEAR(processor.getParameter(devices::Resynth::PlayMode), 2, 0);
    auto device = devices::nativeDeviceModel("SonoraResynth"); std::string error;
    device.nativeContent = model::encodeResynth(*ownModel(), error); const auto previous = device.nativeContent;
    model::Song song; model::Track track; track.devices.push_back(device); song.tracks.push_back(track); model::EditHistory history;
    int edits = 0; context.editDevice = [&](int, int, const auto& operation, const auto& label) {
        ++edits; auto changed = song.tracks[0].devices[0]; operation(changed);
        history.perform(song, model::replaceDevice(song, 0, 0, std::move(changed), label)); device = song.tracks[0].devices[0];
    };
    click(root, button(*editor, "Remove recording")->bounds()); CHECK(edits == 1);
    const auto stripped = model::decodeResynth(*device.nativeContent, error); REQUIRE(stripped);
    CHECK(stripped->source.empty()); CHECK_NEAR(stripped->duration(), 1, 0); CHECK(previous->data.size() > device.nativeContent->data.size() + 190000);
    CHECK(history.undo(song)); CHECK(song.tracks[0].devices[0].nativeContent == previous);
    CHECK(history.redo(song)); CHECK(model::decodeResynth(*song.tracks[0].devices[0].nativeContent, error)->source.empty());
    devices::Resynth sourceFree(stripped); sourceFree.prepare(48000, 512);
    ui::RootWidget secondRoot(context); auto* second = secondRoot.add<ui::ResynthEditor>(&sourceFree, 0, 0, -1, -1);
    secondRoot.setBounds({0, 0, 1080, 368}); second->setBounds({0, 0, 1080, 368});
    CHECK(! button(*second, "Reference")->isEnabled()); CHECK(! button(*second, "Hybrid")->isEnabled());
    click(secondRoot, button(*second, "Reference")->bounds()); CHECK_NEAR(sourceFree.getParameter(devices::Resynth::Mode), 0, 0);
    click(secondRoot, button(*second, "Audition")->bounds());
    juce::AudioBuffer<float> audio(2, 512); audio.clear(); juce::MidiBuffer midi; sourceFree.process(audio, midi); CHECK(audio.getMagnitude(0, 512) > 0.1f);
}

TEST_CASE("resynth UI: fixed editor dimensions ignore panel resizing and preserve other editor sizes")
{
    juce::ScopedJuceInitialiser_GUI gui;
    model::Song song; model::Track first, second; first.id = 100; second.id = 101;
    first.devices.push_back(devices::nativeDeviceModel("SonoraWavetable")); first.devices[0].id = 200;
    second.devices.push_back(devices::nativeDeviceModel("SonoraResynth")); second.devices[0].id = 201;
    song.tracks = {first, second};
    devices::BuildContext build; engine::SampleCache samples; auto graph = engine::buildGraph(song, build, samples); REQUIRE(graph.graph);
    ui::UiContext context; context.song = &song; context.graph = graph.graph.get(); context.controls = graph.graph->controls;
    context.wavetableFloatingDefault = false;
    ui::MainLayout root(context); root.setBounds({0, 0, 1500, 800});
    ui::DetailView* detail = nullptr;
    for (const auto& child : root.children()) if (auto* found = dynamic_cast<ui::DetailView*>(child.get())) detail = found;
    REQUIRE(detail); CHECK_NEAR(detail->height(), 200, 0);
    ui::Splitter* divider = nullptr;
    for (const auto& child : root.children())
        if (auto* found = dynamic_cast<ui::Splitter*>(child.get()); found && found->cursorAt(0, 0) == ui::Cursor::ResizeVertical) divider = found;
    REQUIRE(divider); CHECK(divider->visible);
    context.theme.metrics.detailHeight = 520;
    context.selectedTrack = 1; root.tick(); CHECK_NEAR(detail->height(), 404, 0); CHECK(! divider->visible);
    ui::ResynthEditor* editor = nullptr;
    std::function<void(ui::Widget&)> find = [&](auto& widget) {
        if (auto* found = dynamic_cast<ui::ResynthEditor*>(&widget)) editor = found;
        for (const auto& child : widget.children()) find(*child);
    };
    find(root); REQUIRE(editor);
    CHECK_NEAR(editor->width(), 1080, 0); CHECK_NEAR(editor->height(), 368, 0);
    const auto bounds = detail->boundsInRoot(); ui::MouseEvent event; event.x = 800; event.y = bounds.y - 3;
    root.dispatchMouseDown(event); event.y -= 100; root.dispatchMouseDrag(event); root.dispatchMouseUp(event);
    CHECK_NEAR(detail->height(), 404, 0); CHECK_NEAR(context.theme.metrics.detailHeight, 520, 0);
    divider->setSize(600); CHECK_NEAR(detail->height(), 404, 0); CHECK_NEAR(context.theme.metrics.detailHeight, 520, 0);
    root.setBounds({0, 0, 900, 560});
    CHECK_NEAR(detail->height(), 404, 0); CHECK_NEAR(editor->width(), 1080, 0); CHECK_NEAR(editor->height(), 368, 0);
    root.setBounds({0, 0, 1600, 1000});
    CHECK_NEAR(detail->height(), 404, 0); CHECK_NEAR(editor->width(), 1080, 0); CHECK_NEAR(editor->height(), 368, 0);
    context.selectedTrack = 0; root.tick(); CHECK_NEAR(detail->height(), 520, 0); CHECK(divider->visible);
    context.selectedTrack = 1; context.detailTab = ui::DetailTab::Clip; root.tick();
    CHECK_NEAR(detail->height(), 520, 0); CHECK(divider->visible);
}

TEST_CASE("resynth UI: Dry bass removes generated noise and source attack with a short MIDI release")
{
    juce::ScopedJuceInitialiser_GUI gui; using R = devices::Resynth;
    R processor(ownModel()); processor.prepare(48000, 512);
    processor.setParameter(R::Mode, 1); processor.setParameter(R::Noise, 1); processor.setParameter(R::Release, 800);
    ui::UiContext context; ui::RootWidget root(context);
    auto* editor = root.add<ui::ResynthEditor>(&processor, 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
    root.setBounds({0, 0, 1080, 368}); editor->setBounds({0, 0, 1080, 368}); editor->layout();
    auto* dry = button(*editor, "Dry bass"); REQUIRE(dry); click(root, dry->bounds());
    CHECK_NEAR(processor.getParameter(R::Mode), 0, 0); CHECK_NEAR(processor.getParameter(R::Noise), 0, 0);
    CHECK_NEAR(processor.getParameter(R::HybridAmount), 0, 0); CHECK_NEAR(processor.getParameter(R::Release), 30, 0);
    CHECK_NEAR(processor.getParameter(R::Fundamental), 1, 0); CHECK(processor.model()->source == ownModel()->source);
}
