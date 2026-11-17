// PLG-27: the Sonora Seraph face (ProFace kit) in the VST3 window and in the app's device chain, driven by real mouse
// events: sections, the vowel strip, sliders, the preset row, the gate lane, the choosers; state; PNGs of both layouts.
#include "ProFaceTestKit.h"
#include "devices/native/Seraph.h"
#include "plugin/SeraphPlugin.h"
#include "plugin/SeraphPluginEditor.h"
#include "ui/views/SeraphFace.h"

#include <cmath>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using S = devices::Seraph;
constexpr double kRate = 48000.0;

void clickAt(ui::RootWidget& root, ui::ProFace& face, float x, float y, bool shift = false)
{
    auto down = designAt(face, x, y);
    down.shift = shift;
    root.dispatchMouseDown(down);
    root.dispatchMouseUp(down);
}

void dragAcross(ui::RootWidget& root, ui::ProFace& face, float x0, float y0, float x1, float y1)
{
    root.dispatchMouseDown(designAt(face, x0, y0));
    for (int i = 1; i <= 12; ++i)
        root.dispatchMouseDrag(designAt(face, x0 + (x1 - x0) * (float) i / 12.0f, y0 + (y1 - y0) * (float) i / 12.0f));
    root.dispatchMouseUp(designAt(face, x1, y1));
}

float centreX(const ui::Rect& r) { return r.x + r.w * 0.5f; }
float centreY(const ui::Rect& r) { return r.y + r.h * 0.5f; }
} // namespace

TEST_CASE("seraph plugin: the full face by mouse (sections, vowel, sliders, presets, gate lane, choosers), sound and state")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::SeraphPlugin p;
    CHECK(p.getParameters().size() == S::NumParams);
    CHECK(p.acceptsMidi());
    CHECK(p.getNumPrograms() == (int) S::presets().size());
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::SeraphPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::SeraphFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    CHECK(face.layoutMode() == ui::ProFace::Layout::Full);
    auto& d = p.device();

    // sections, singers, mode
    REQUIRE(click(view, face, "Women"));
    CHECK(std::lround(d.getParameter(S::Section)) == S::Women);
    REQUIRE(click(view, face, "7"));
    CHECK(std::lround(d.getParameter(S::Singers)) == 7);
    REQUIRE(click(view, face, "Legato"));
    CHECK(std::lround(d.getParameter(S::Mode)) == S::Legato);
    REQUIRE(click(view, face, "Poly"));

    // the vowel strip: a click on O, then a drag to U
    REQUIRE(face.controlCentre("Vowel").has_value());
    const float vowelY = face.controlCentre("Vowel")->second;
    clickAt(view, face, face.vowelX(3.0f), vowelY);
    CHECK_NEAR(d.getParameter(S::Vowel), 3.0f, 0.02f);
    dragAcross(view, face, face.vowelX(3.0f), vowelY, face.vowelX(4.0f), vowelY);
    CHECK_NEAR(d.getParameter(S::Vowel), 4.0f, 0.02f);
    // a slider: Hum at three quarters
    const auto hum = face.controlCentre("HUM");
    REQUIRE(hum.has_value());
    clickAt(view, face, hum->first + 380 * 0.25f, hum->second);
    CHECK_NEAR(d.getParameter(S::Hum), 0.75f, 0.02f);
    // a knob drag
    const float tension = d.getParameter(S::Tension);
    REQUIRE(drag(view, face, "TENSION", -60));
    CHECK(d.getParameter(S::Tension) > tension + 0.1f);

    // the preset bar: next from the first preset, the menu by family, the dice
    const auto& list = S::presets();
    auto& bar = face.presetBar();
    d.applyPreset(0);
    clickAt(view, face, centreX(bar.nextRect()), centreY(bar.nextRect()));
    CHECK(d.matchingPreset() == 1);
    clickAt(view, face, centreX(bar.nameRect()), centreY(bar.nameRect()));
    REQUIRE(bar.menuOpen());
    const auto categories = bar.categories();
    const auto monks = std::find(categories.begin(), categories.end(), std::string("Monks"));
    REQUIRE(monks != categories.end());
    const auto row = bar.categoryRow((int) (monks - categories.begin()));
    clickAt(view, face, centreX(row), centreY(row));
    const auto first = bar.presetRow(0);
    clickAt(view, face, centreX(first), centreY(first));
    CHECK(! bar.menuOpen());
    REQUIRE(d.matchingPreset() >= 0);
    CHECK(std::string(list[(size_t) d.matchingPreset()].category) == "Monks");
    clickAt(view, face, centreX(bar.previousRect()), centreY(bar.previousRect()));
    CHECK(std::string(list[(size_t) d.matchingPreset()].category) == "Angels");
    const int beforeDice = d.matchingPreset();
    for (int i = 0; i < 4 && d.matchingPreset() == beforeDice; ++i) clickAt(view, face, centreX(bar.diceRect()), centreY(bar.diceRect()));
    CHECK(d.matchingPreset() >= 0);
    CHECK(d.matchingPreset() != beforeDice);

    // gate: on, a pattern by the chooser (its rate comes with it), then painted steps start Custom from it
    d.applyPreset(0);
    REQUIRE(click(view, face, "Gate on"));
    CHECK(d.getParameter(S::Gate) > 0.5f);
    const auto pattern = face.patternRect();
    clickAt(view, face, pattern.x + pattern.w - 10, centreY(pattern));
    CHECK(std::lround(d.getParameter(S::GatePattern)) == 1);
    for (int i = 0; i < 15; ++i) clickAt(view, face, pattern.x + pattern.w - 10, centreY(pattern)); // to Triplet Roll (16)
    CHECK(std::lround(d.getParameter(S::GatePattern)) == 16);
    CHECK(std::lround(d.getParameter(S::GateRate)) == dsp::TranceGate::SixteenthTriplet);
    CHECK(d.gateSteps() == 24);
    clickAt(view, face, pattern.x + 8, centreY(pattern));
    CHECK(std::lround(d.getParameter(S::GatePattern)) == 15); // Quarter Pulse: 1 0 0 0
    auto& gate = face.gateLane();
    const auto lane = gate.bounds;
    const auto c0 = gate.cellRect(0), c3 = gate.cellRect(3);
    dragAcross(view, face, centreX(c0), lane.y + lane.h * 0.5f, centreX(c3), lane.y + lane.h * 0.5f);
    CHECK(std::lround(d.getParameter(S::GatePattern)) == S::kCustomPattern);
    CHECK(d.customGateSteps() == 16);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(d.customGateLevel(i), 0.5f, 0.1f);
    CHECK_NEAR(d.customGateLevel(4), 1.0f, 1e-6); // the rest of Quarter Pulse kept
    CHECK_NEAR(d.customGateLevel(5), 0.0f, 1e-6);
    auto alt = designAt(face, centreX(gate.cellRect(5)), lane.y + lane.h - 3);
    alt.alt = true;
    view.dispatchMouseDown(alt);
    view.dispatchMouseUp(alt);
    CHECK_NEAR(d.customGateLevel(5), 1.0f, 1e-6);

    // chant: on, the phrase and the scale choosers, a key
    REQUIRE(click(view, face, "Chant on"));
    CHECK(d.getParameter(S::Chant) > 0.5f);
    clickAt(view, face, face.phraseRect().x + face.phraseRect().w - 10, centreY(face.phraseRect()));
    CHECK(std::lround(d.getParameter(S::PhraseParam)) == 1);
    const int scale = (int) std::lround(d.getParameter(S::Scale));
    clickAt(view, face, face.scaleRect().x + face.scaleRect().w - 10, centreY(face.scaleRect()));
    CHECK(std::lround(d.getParameter(S::Scale)) == scale + 1);
    REQUIRE(click(view, face, "E"));
    CHECK(std::lround(d.getParameter(S::Key)) == 4);
    REQUIRE(click(view, face, "5th"));
    CHECK(std::lround(d.getParameter(S::Organum)) == S::Fifth);

    // it sings: a chord through the plug-in, the stalls fill and the gate's play head moves
    d.setParameter(S::Chant, 0);
    juce::AudioBuffer<float> block(2, 512);
    float peak = 0;
    for (int i = 0; i < 60; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            for (int k : {57, 60, 64}) midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8) 100), 0);
        block.clear();
        p.processBlock(block, midi);
        peak = std::max(peak, block.getMagnitude(0, 512));
        if (i % 4 == 0) view.tickTree();
    }
    CHECK(peak > 0.01f);
    CHECK(d.activeSingers() == 27); // Seraphim Aah: 6 women a note and 3 more a fifth above (Organum), 3 notes
    CHECK(face.headline().find("27 singing") != std::string::npos);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_seraph_plugin.png").getFullPathName().toStdString()));

    // state: parameters, the custom gate and the preset name
    p.reportEditorChanges();
    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::SeraphPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    for (int i = 0; i < S::NumParams; ++i) CHECK_NEAR(restored.device().getParameter(i), d.getParameter(i), 1e-5);
    CHECK(restored.device().gateText() == d.gateText());
    CHECK(restored.device().presetName() == d.presetName());
    // the host's programs are the presets
    restored.setCurrentProgram(40);
    CHECK(restored.getCurrentProgram() == 40);
    CHECK(restored.getProgramName(40) == juce::String(list[40].name));
    // the picture for the download page: Gated Angels singing an A minor chord
    for (size_t i = 0; i < list.size(); ++i)
        if (std::string(list[i].name) == "Gated Angels") d.applyPreset((int) i);
    d.setParameter(S::Key, 9);
    for (int i = 0; i < 90; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0) midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        if (i == 2)
            for (int k : {57, 60, 64, 69}) midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8) 100), 0);
        block.clear();
        p.processBlock(block, midi);
        if (i % 4 == 0) view.tickTree();
    }
    view.tickTree();
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_seraph_site.png").getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("seraph: the compact face in the app's chain, and its own window")
{
    Chain chain(model::TrackKind::Midi, {devices::nativeDeviceModel("SonoraSeraph")});
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::SeraphFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    REQUIRE(click(root, face, "Men"));
    auto* device = dynamic_cast<S*>(chain.processor());
    REQUIRE(device != nullptr);
    CHECK(std::lround(device->getParameter(S::Section)) == S::Men);
    const float vowelY = face.controlCentre("Vowel")->second;
    clickAt(root, face, face.vowelX(4.0f), vowelY);
    CHECK_NEAR(device->getParameter(S::Vowel), 4.0f, 0.03f);
    REQUIRE(click(root, face, "Gate"));
    CHECK(device->getParameter(S::Gate) > 0.5f);
    // sing so the stalls show something
    juce::AudioBuffer<float> block(2, 512);
    for (int i = 0; i < 20; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            for (int k : {45, 52, 57}) midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8) 100), 0);
        block.clear();
        device->process(block, midi);
    }
    root.tickTree();
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, testFile("sonora_seraph_chain.png").getFullPathName().toStdString()));

    ui::ProFaceWindowView window(chain.context, {1, 10, -1, -1});
    window.setBounds({0, 0, ui::SeraphFace::kFull.w, ui::SeraphFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);
    CHECK(gfx::renderToPng(window, (int) ui::SeraphFace::kFull.w, (int) ui::SeraphFace::kFull.h, 1.0f,
                           testFile("sonora_seraph_window.png").getFullPathName().toStdString()));
}

namespace
{
// A sung note from Seraph's own Voices engine, written to a WAV (the Real voices tests need a recording; nothing recorded
// is shipped or committed).
juce::File writeSungNote(const std::string& name, int section, int key, float vowel)
{
    S s;
    s.setParameter(S::Section, (float) section);
    for (int p : {S::Space, S::Vibrato, S::Drift, S::Detune, S::Timing, S::Scoop, S::Human, S::Breath, S::Width})
        s.setParameter(p, 0.0f);
    s.setParameter(S::Vowel, vowel);
    s.setParameter(S::Fold, 0);
    s.prepare(kRate, 512);
    juce::AudioBuffer<float> out(2, (int) (2.5 * kRate));
    out.clear();
    for (int start = 0; start < out.getNumSamples(); start += 512)
    {
        const int len = std::min(512, out.getNumSamples() - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, key, 0.8f), 0);
        if (start <= (int) (2.3 * kRate) && start + len > (int) (2.3 * kRate)) midi.addEvent(juce::MidiMessage::noteOff(1, key), 0);
        s.process(view, midi);
    }
    const auto file = testFile(name);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer(out, 0, out.getNumSamples());
    return file;
}
} // namespace

TEST_CASE("seraph plugin: Real voices: Voices / Real by click, a sung note dropped on the window sings, a noise file is refused")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::SeraphPlugin p;
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::SeraphPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::SeraphFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    auto& d = p.device();
    REQUIRE(click(view, face, "Real"));
    CHECK(std::lround(d.getParameter(S::Source)) == S::RealSource);
    view.tickTree();
    CHECK(face.helpLine().find("drop a sung note") != std::string::npos);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_seraph_real_empty.png").getFullPathName().toStdString()));

    // a noise file: refused, with the reason on the face
    juce::AudioBuffer<float> noise(1, (int) (2 * kRate));
    juce::Random random(4);
    for (int i = 0; i < noise.getNumSamples(); ++i) noise.setSample(0, i, random.nextFloat() - 0.5f);
    const auto noiseFile = testFile("sonora_seraph_noise.wav");
    {
        noiseFile.deleteFile();
        std::unique_ptr<juce::OutputStream> stream(noiseFile.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(noise, 0, noise.getNumSamples());
    }
    window->filesDropped({noiseFile.getFullPathName()}, 10, 10);
    CHECK(d.realVoices().empty());
    CHECK(face.realMessage.find("no stable pitch") != std::string::npos);

    // two sung notes dropped on the window
    const auto man = writeSungNote("sonora_seraph_man_oh_A2.wav", S::SoloBass, 45, 3);
    const auto woman = writeSungNote("sonora_seraph_woman_ah_E4.wav", S::SoloAlto, 64, 0);
    window->filesDropped({man.getFullPathName(), woman.getFullPathName()}, 10, 10);
    REQUIRE(d.realVoices().size() == 2);
    CHECK(d.realActive());
    CHECK(face.realMessage.empty());
    juce::AudioBuffer<float> block(2, 512);
    float peak = 0;
    for (int i = 0; i < 60; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0)
            for (int k : {48, 55, 64}) midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8) 100), 0);
        block.clear();
        p.processBlock(block, midi);
        peak = std::max(peak, block.getMagnitude(0, 512));
        if (i % 4 == 0) view.tickTree();
    }
    CHECK(peak > 0.01f);
    int real = 0;
    for (int v = 0; v < S::kMaxSingers; ++v) real += d.singer(v).real != nullptr ? 1 : 0;
    CHECK(real == d.activeSingers());
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_seraph_real.png").getFullPathName().toStdString()));
    // the state keeps the files
    p.reportEditorChanges();
    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::SeraphPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK(restored.device().realPaths() == d.realPaths());
    // Clear
    REQUIRE(click(view, face, "Clear"));
    CHECK(d.realVoices().empty());
    p.editorBeingDeleted(editor.get());
    editor.reset();
    man.deleteFile();
    woman.deleteFile();
    noiseFile.deleteFile();
}

TEST_CASE("seraph: a sung note dropped from the Browser on the chain's face is one undoable Load Voice edit")
{
    Chain chain(model::TrackKind::Midi, {devices::nativeDeviceModel("SonoraSeraph")});
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::SeraphFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    REQUIRE(face.onVoicesDropped != nullptr);
    const auto note = writeSungNote("sonora_seraph_browser_oh.wav", S::SoloTenor, 52, 3);
    ui::BrowserDrag drag;
    drag.kind = ui::BrowserDrag::Kind::Sample;
    drag.value = note.getFullPathName().toStdString();
    const auto e = designAt(face, 300, 80);
    chain.context.browserDrag = drag;
    root.dispatchMouseDrag(e);
    root.dispatchMouseUp(e);
    REQUIRE(chain.edits.size() == 1);
    CHECK(chain.edits[0] == "Load Voice");
    model::Device edited = chain.song.tracks[0].devices[0];
    chain.editFns[0](edited);
    REQUIRE(edited.nativeContent != nullptr);
    CHECK(edited.nativeContent->format == S::kContentFormat);
    const std::string text(edited.nativeContent->data.begin(), edited.nativeContent->data.end());
    CHECK(text.find("real " + note.getFullPathName().toStdString()) != std::string::npos);
    CHECK(text.find("gate ") != std::string::npos); // the live content kept
    bool real = false;
    for (const auto& prm : edited.parameters) real = real || (prm.path == "Source" && std::lround(prm.value) == S::RealSource);
    CHECK(real);
    // the edited model builds a device singing it
    std::vector<std::string> warnings;
    auto rebuilt = devices::createNative(edited, &warnings);
    auto* seraph = dynamic_cast<S*>(rebuilt.get());
    REQUIRE(seraph != nullptr);
    CHECK(warnings.empty());
    CHECK(seraph->realActive());
    note.deleteFile();
}

namespace
{
// A stand-in Seraph Voices pack (synthesised notes, the pack's layout) in a packs root of its own.
juce::File makeStandInPack()
{
    const auto root = testFile("sonora-seraph-packs");
    root.deleteRecursively();
    const auto pack = root.getChildFile("Seraph Voices");
    pack.createDirectory();
    pack.getChildFile("pack.json").replaceWithText(R"({"format": 1, "id": "sonora-seraph-voices", "name": "Seraph Voices", "version": "1.0"})");
    pack.getChildFile("ATTRIBUTION.txt").replaceWithText(juce::String(S::kPackCredit) + "\n\nDetails...\n");
    struct Note { const char* singer; int section, key; float vowel; const char* file; };
    for (const Note& n : {Note{"Female 1", S::SoloAlto, 64, 0, "Female 1 ah E4.wav"}, Note{"Female 1", S::SoloAlto, 69, 3, "Female 1 oh A4.wav"},
                          Note{"Male 1", S::SoloBass, 45, 3, "Male 1 oh A2.wav"}, Note{"Male 1", S::SoloTenor, 52, 0, "Male 1 ah E3.wav"}})
    {
        const auto written = writeSungNote("sonora_seraph_pack_note.wav", n.section, n.key, n.vowel);
        pack.getChildFile(n.singer).createDirectory();
        written.moveFileTo(pack.getChildFile(n.singer).getChildFile(n.file));
    }
    return root;
}
} // namespace

TEST_CASE("seraph: the Seraph Voices pack: its singers on the face, Real Choir presets, the credit, and the download offer")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto root = makeStandInPack();
    const auto found = S::findPackVoices(root.getFullPathName().toStdString());
    REQUIRE(! found.folder.empty());
    REQUIRE(found.singers.size() == 2);
    CHECK(found.singers[0].name == "Female 1");
    CHECK(found.singers[1].files.size() == 2);
    CHECK(found.credit == S::kPackCredit);

    // the presets: Recorded Men loads the men only, Recorded Choir everyone
    const auto& list = S::presets();
    auto indexOf = [&](const char* name) {
        for (size_t i = 0; i < list.size(); ++i)
            if (std::string(list[i].name) == name) return (int) i;
        return -1;
    };
    {
        S s;
        s.packsRoot = root.getFullPathName().toStdString();
        s.applyPreset(indexOf("Recorded Men"));
        CHECK(s.realVoices().size() == 2);
        for (const auto& v : s.realVoices()) // the folder name, not a "/" search: Windows paths use "\\"
            CHECK(juce::File(juce::String::fromUTF8(v->path.c_str())).getParentDirectory().getFileName() == "Male 1");
        CHECK(s.packVoicesLoaded());
        CHECK(s.matchingPreset() == indexOf("Recorded Men"));
        s.applyPreset(indexOf("Recorded Choir"));
        CHECK(s.realVoices().size() == 4);
        CHECK(s.packWanted().empty());
    }
    // no pack installed: the preset says so and the synthesis sings meanwhile
    {
        S s;
        s.packsRoot = testFile("sonora-no-packs-here").getFullPathName().toStdString();
        s.applyPreset(indexOf("Recorded Women"));
        CHECK(s.realVoices().empty());
        CHECK(s.packWanted() == "Recorded Women");
    }

    // the face in the VST3 window: the singer chips, the credit and Copy credit; and without the pack, Get the pack
    plugin::SeraphPlugin p;
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::SeraphPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, ui::SeraphFace::kFull.w, ui::SeraphFace::kFull.h});
    view.layout();
    auto& face = window->face();
    face.packsRoot = root.getFullPathName().toStdString();
    REQUIRE(click(view, face, "Real"));
    view.tickTree();
    REQUIRE(face.controlCentre("W1").has_value());
    REQUIRE(click(view, face, "W1"));
    CHECK(p.device().realVoices().size() == 2);
    CHECK(p.device().packVoicesLoaded());
    for (int i = 0; i < 40; ++i) view.tickTree(); // the face notices the pack voices and shows the credit
    CHECK(face.controlCentre("Copy credit").has_value());
    REQUIRE(click(view, face, "M1"));
    CHECK(p.device().realVoices().size() == 4);
    for (int i = 0; i < 40; ++i) view.tickTree();
    CHECK(gfx::renderToPng(view, (int) ui::SeraphFace::kFull.w, (int) ui::SeraphFace::kFull.h, 2.0f,
                           testFile("sonora_seraph_pack.png").getFullPathName().toStdString()));
    // without the pack: the download is offered (the app wires it to the Browser's pack store)
    bool asked = false;
    face.onGetPack = [&asked] { asked = true; };
    face.packsRoot = testFile("sonora-no-packs-here").getFullPathName().toStdString();
    p.device().clearRealVoices();
    for (int i = 0; i < 40; ++i) view.tickTree();
    REQUIRE(click(view, face, "Get the Seraph Voices pack"));
    CHECK(asked);
    p.editorBeingDeleted(editor.get());
    editor.reset();
    root.deleteRecursively();
}
