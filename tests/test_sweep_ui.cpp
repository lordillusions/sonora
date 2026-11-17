// PLG-19 / PLG-23: the Sonora Sweep face (ProFace kit) in the VST3 window and in the app's device chain: types, sources,
// Preview, and samples dropped from the Browser.
#include "ProFaceTestKit.h"
#include "devices/native/Sweep.h"
#include "plugin/SweepPlugin.h"
#include "plugin/SweepPluginEditor.h"
#include "ui/views/SweepFace.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using W = devices::Sweep;
constexpr double kRate = 48000.0;

juce::File writeCrash()
{
    const auto file = testFile("sonora_sweep_crash.wav");
    file.deleteFile();
    juce::AudioBuffer<float> crash(2, 48000);
    juce::Random random(3);
    for (int s = 0; s < 48000; ++s)
        for (int ch = 0; ch < 2; ++ch) crash.setSample(ch, s, (random.nextFloat() * 2 - 1) * 0.6f * std::exp(-(float) s / 15000.0f));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer(crash, 0, 48000);
    return file;
}
} // namespace

TEST_CASE("sweep plugin: the full face; types, sources and Up/Down by click; a dropped sample; Preview sounds")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::SweepPlugin p;
    CHECK(p.getParameters().size() == W::NumParams);
    CHECK(p.acceptsMidi());
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::SweepPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::SweepFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    CHECK(face.onChooseSample != nullptr);

    REQUIRE(click(view, face, "Reverse Crash"));
    CHECK(std::lround(p.device().getParameter(W::Source)) == W::Metal);
    CHECK(p.device().matchingType() == 5);
    REQUIRE(click(view, face, "Pink"));
    CHECK(std::lround(p.device().getParameter(W::Source)) == W::Pink);
    CHECK(p.device().matchingType() == -1);
    REQUIRE(click(view, face, "\xE2\x96\xBC  Down"));
    CHECK(std::lround(p.device().getParameter(W::Direction)) == W::Down);
    REQUIRE(click(view, face, "\xE2\x96\xB2  Up"));
    REQUIRE(click(view, face, "2 Bars"));
    CHECK(std::lround(p.device().getParameter(W::LengthParam)) == 3);
    REQUIRE(click(view, face, "Supersaw Rise"));
    CHECK(std::lround(p.device().getParameter(W::Chord)) == 2);
    CHECK(face.headline().rfind("Riser", 0) == 0);
    CHECK(face.renderedMax().size() > 100);

    // Preview: one sweep without MIDI.
    view.dispatchMouseDown(designAt(face, 1084, 30));
    view.dispatchMouseUp(designAt(face, 1084, 30));
    juce::AudioBuffer<float> block(2, 512);
    float peak = 0;
    for (int i = 0; i < 40; ++i)
    {
        juce::MidiBuffer midi;
        block.clear();
        p.processBlock(block, midi);
        peak = std::max(peak, block.getMagnitude(0, 512));
        if (i % 4 == 0) view.tickTree();
    }
    CHECK(peak > 0.01f);
    CHECK(p.device().progress() >= 0.0f);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_sweep_plugin.png").getFullPathName().toStdString()));

    // A sample dropped on the window (the Finder or the host's browser).
    const auto crash = writeCrash();
    window->filesDropped({crash.getFullPathName()}, 10, 10);
    CHECK(p.device().hasSample());
    CHECK(std::lround(p.device().getParameter(W::Source)) == W::Sample);
    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::SweepPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK(restored.device().samplePath() == crash.getFullPathName().toStdString());

    // PLG-19d: a reverb swell type, the character pad and the Region by mouse; Save WAV is offered.
    CHECK(face.onSaveWav != nullptr);
    CHECK(face.onDragOut != nullptr);
    REQUIRE(face.controlCentre("Save WAV\xE2\x80\xA6").has_value());
    REQUIRE(click(view, face, "Vocal Swell"));
    CHECK(p.device().reverbSwell());
    CHECK(face.headline().rfind("Reverb swell", 0) == 0);
    CHECK(std::lround(p.device().getParameter(W::SwellMode)) == W::ReverbSwell);
    view.dispatchMouseDown(designAt(face, 248 + 168 * 0.9f, 404 + 160 * 0.1f)); // the pad's top right: Bloom
    view.dispatchMouseDrag(designAt(face, 248 + 168 * 0.8f, 404 + 160 * 0.2f));
    view.dispatchMouseUp(designAt(face, 248 + 168 * 0.8f, 404 + 160 * 0.2f));
    CHECK(std::abs(p.device().getParameter(W::CharacterX) - 0.8f) < 0.02f);
    CHECK(std::abs(p.device().getParameter(W::CharacterY) - 0.8f) < 0.02f);
    CHECK(p.device().matchingType() == -1);
    view.dispatchMouseDown(designAt(face, 28 + 2, 660)); // the Region's start handle, dragged to 20 %
    view.dispatchMouseDrag(designAt(face, 28 + 182 * 0.2f, 660));
    view.dispatchMouseUp(designAt(face, 28 + 182 * 0.2f, 660));
    CHECK(std::abs(p.device().getParameter(W::RegionStart) - 0.2f) < 0.02f);
    CHECK(std::abs(p.device().getParameter(W::RegionEnd) - 1.0f) < 1e-6f);
    for (int i = 0; i < 8; ++i) view.tickTree(); // the face prepares the sample again once the settings rest
    CHECK(std::abs(p.device().hitSeconds() - 0.8) < 0.01);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_sweep_plugin_swell.png").getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
    crash.deleteFile();
}

TEST_CASE("sweep: the compact face in the app's chain takes a sample dropped from the Browser as one undoable edit")
{
    Chain chain(model::TrackKind::Midi, {devices::nativeDeviceModel("SonoraSweep")});
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::SweepFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    REQUIRE(face.onSampleDropped != nullptr);
    REQUIRE(click(root, face, "Whoosh"));
    CHECK(std::lround(chain.processor()->getParameter(W::Source)) == W::Whoosh);

    ui::BrowserDrag drag;
    drag.kind = ui::BrowserDrag::Kind::Sample;
    drag.value = "/tmp/vocal.wav";
    const auto e = designAt(face, 150, 80);
    chain.context.browserDrag = drag; // a drag from the Browser, released over the face's display
    root.dispatchMouseDrag(e);
    root.dispatchMouseUp(e);
    REQUIRE(chain.edits.size() == 1);
    CHECK(chain.edits[0] == "Load Sample");
    model::Device edited = chain.song.tracks[0].devices[0];
    chain.editFns[0](edited);
    REQUIRE(edited.nativeContent != nullptr);
    CHECK(edited.nativeContent->format == "SonoraSweep/Sample");
    CHECK(std::string(edited.nativeContent->data.begin(), edited.nativeContent->data.end()) == "/tmp/vocal.wav");
    bool sample = false;
    for (const auto& prm : edited.parameters) sample = sample || (prm.path == "Source" && std::lround(prm.value) == W::Sample);
    CHECK(sample);
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, testFile("sonora_sweep_chain.png").getFullPathName().toStdString()));

    ui::ProFaceWindowView window(chain.context, {1, 10, -1, -1});
    window.setBounds({0, 0, ui::SweepFace::kFull.w, ui::SweepFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);

    // PLG-19d: dragging the display out renders the sweep next to the set and starts a sample drag (a track takes it).
    const auto project = testFile("sonora_sweep_dragout");
    project.deleteRecursively();
    chain.song.sourcePath = project.getChildFile("Set.als").getFullPathName().toStdString();
    auto* big = dynamic_cast<ui::SweepFace*>(window.face());
    REQUIRE(big != nullptr);
    REQUIRE(big->onDragOut != nullptr);
    CHECK(big->onSaveWav != nullptr);
    window.dispatchMouseDown(designAt(*big, 600, 180));
    window.dispatchMouseDrag(designAt(*big, 620, 190));
    window.dispatchMouseDrag(designAt(*big, 700, 260));
    REQUIRE(chain.context.browserDrag.has_value());
    CHECK(chain.context.browserDrag->kind == ui::BrowserDrag::Kind::Sample);
    const juce::File rendered(juce::String::fromUTF8(chain.context.browserDrag->value.c_str()));
    CHECK(rendered.existsAsFile());
    CHECK(rendered.isAChildOf(project.getChildFile("Samples/Processed/Sweep")));
    CHECK(rendered.getFileName() == "Sweep riser.wav");
    chain.context.browserDrag.reset();
    project.deleteRecursively();
    CHECK(gfx::renderToPng(window, (int) ui::SweepFace::kFull.w, (int) ui::SweepFace::kFull.h, 1.0f,
                           testFile("sonora_sweep_window.png").getFullPathName().toStdString()));
}

// The owner's crash (0.1.51): a sample dragged from the Browser onto Sweep in the device chain. The app's editDevice
// rebuilds the views inside the edit (MainWindow's modelChanged lays the window out again synchronously, to drop pointers
// into the old graph), so the face that took the drop is deleted before its browserDrop returns; it then called repaint()
// on itself and walked freed parent pointers. Run with MallocScribble=1 to see the old code crash deterministically.
TEST_CASE("sweep: a Browser drop on the chain face survives the app rebuilding the chain inside the edit")
{
    Chain chain(model::TrackKind::Midi, {devices::nativeDeviceModel("SonoraSweep")});
    ui::MainLayout root(chain.context);
    const auto crash = writeCrash();
    int nextId = 100, rebuilds = 0;
    // What AppController::editDevice + afterEdit + MainWindow's modelChanged do: a new device id (a fresh processor),
    // a new graph, and the views rebuilt before the call returns.
    chain.context.editDevice = [&](int track, int device, std::function<void(model::Device&)> fn, std::string label) {
        chain.edits.push_back(label);
        auto& d = chain.song.tracks[(size_t) track].devices[(size_t) device];
        fn(d);
        d.id = nextId++;
        chain.context.graph = nullptr;
        root.layout(); // the app drops view pointers before processors die
        chain.graph = engine::buildGraph(chain.song, chain.build, chain.samples);
        chain.graph.graph->prepare(48000.0, 512);
        chain.context.graph = chain.graph.graph.get();
        chain.context.controls = chain.graph.graph->controls;
        root.layout();
        root.markDirty();
        ++rebuilds;
    };
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::SweepFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto* before = faces[0];
    REQUIRE(before->onSampleDropped != nullptr);

    ui::BrowserDrag drag;
    drag.kind = ui::BrowserDrag::Kind::Sample;
    drag.value = crash.getFullPathName().toStdString();
    drag.label = "sonora_sweep_crash";
    const auto e = designAt(*before, 150, 80);
    chain.context.browserDrag = drag;
    root.dispatchMouseDrag(e);
    root.dispatchMouseUp(e);
    REQUIRE(rebuilds == 1);
    CHECK(chain.edits.size() == 1);
    faces.clear();
    collect(root, faces);
    REQUIRE(faces.size() == 1); // the face that took the drop was replaced: the new one shows the new processor
    auto* sweep = dynamic_cast<devices::Sweep*>(chain.processor());
    REQUIRE(sweep != nullptr);
    CHECK(&faces[0]->sweep() == sweep);
    CHECK(sweep->hasSample());
    CHECK(std::lround(sweep->getParameter(W::Source)) == W::Sample);
    root.tickTree();
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, testFile("sonora_sweep_chain_dropped.png").getFullPathName().toStdString()));

    // The same drop on the face in its own window (ProFaceWindowView) and a second drop on the new chain face.
    const auto e2 = designAt(*faces[0], 150, 80);
    chain.context.browserDrag = drag;
    root.dispatchMouseDrag(e2);
    root.dispatchMouseUp(e2);
    CHECK(rebuilds == 2);
    crash.deleteFile();
}
