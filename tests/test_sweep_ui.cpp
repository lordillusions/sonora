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
    view.dispatchMouseDown(designAt(face, 1014, 30));
    view.dispatchMouseUp(designAt(face, 1014, 30));
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
    CHECK(gfx::renderToPng(window, (int) ui::SweepFace::kFull.w, (int) ui::SweepFace::kFull.h, 1.0f,
                           testFile("sonora_sweep_window.png").getFullPathName().toStdString()));
}
