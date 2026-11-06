#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "plugin/HushPlugin.h"
#include "plugin/HushPluginEditor.h"

#include <algorithm>

using namespace sonora;
namespace
{
using H = devices::Hush;
}

TEST_CASE("hush plugin: host parameters, text formats and a state round trip")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::HushPlugin p;
    CHECK(p.getParameters().size() == H::kParams);
    auto* freq = p.getParameters()[H::bandParam(1, H::BandFrequency)];
    CHECK(freq->getCurrentValueAsText() == "4.0 kHz");
    freq->setValue(freq->getValueForText("6k3"));
    CHECK_NEAR(p.hush().getParameter(H::bandParam(1, H::BandFrequency)), 6300.0, 1.0);
    CHECK(p.getParameters()[H::Focus]->getCurrentValueAsText() == "even");
    CHECK(p.getParameters()[H::MaxCut]->getCurrentValueAsText() == "off");
    CHECK(p.getParameters()[H::DetailTiltLow]->getCurrentValueAsText() == "1.0x");
    p.getParameters()[H::Depth]->setValue(0.8f);
    CHECK_NEAR(p.hush().getParameter(H::Depth), 8.0, 1e-4);

    p.session().selectSlot(1);
    p.hush().setParameter(H::Depth, 2.0f);
    p.session().selectSlot(0);
    CHECK_NEAR(p.hush().getParameter(H::Depth), 8.0, 1e-4);
    const auto state = p.session().encodeState();
    plugin::HushPlugin restored;
    std::string error;
    CHECK(restored.session().decodeState(state.data(), state.size(), error));
    CHECK_NEAR(restored.hush().getParameter(H::Depth), 8.0, 1e-4);
    CHECK_NEAR(restored.hush().getParameter(H::bandParam(1, H::BandFrequency)), 6300.0, 1.0);
    restored.session().selectSlot(1);
    CHECK_NEAR(restored.hush().getParameter(H::Depth), 2.0, 1e-4);
    CHECK(! restored.session().decodeState("junk", 4, error)); // never crash on a bad state
}

TEST_CASE("hush plugin: presets, undo and redo")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::HushPlugin p;
    CHECK(p.getNumPrograms() >= 10);
    p.setCurrentProgram(1); // Flat start
    CHECK(p.session().presetName() == "Flat start");
    CHECK(p.session().presetCategory() == "sonora");
    CHECK_NEAR(p.hush().getParameter(H::Depth), 0.0, 1e-6);
    CHECK(p.hush().getParameter(H::bandParam(0, H::BandUsed)) < 0.5f);
    CHECK(! p.session().presetModified());
    p.session().pushUndo();
    p.session().setFromEditor(H::Depth, 6.0f);
    CHECK(p.session().presetModified());
    CHECK(p.session().undo());
    CHECK_NEAR(p.hush().getParameter(H::Depth), 0.0, 1e-6);
    CHECK(p.session().redo());
    CHECK_NEAR(p.hush().getParameter(H::Depth), 6.0, 1e-6);
    // A preset file in the same JSON layout (plain values by parameter name).
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_hush_test.preset");
    file.replaceWithText(R"({"name": "Test", "category": 3, "params": {"Depth": 7.5, "Mode": 1, "Band 1 used": 1,
        "Band 1 shape": 3, "Band 1 frequency": 2500, "Stereo link": 40, "Unknown": 3}})");
    plugin::HushPreset preset;
    CHECK(devices::HushSession::readPresetFile(file, preset));
    p.session().loadPreset(preset);
    CHECK_NEAR(p.hush().getParameter(H::Depth), 7.5, 1e-6);
    CHECK_NEAR(p.hush().getParameter(H::Mode), 1.0, 1e-6);
    CHECK_NEAR(p.hush().getParameter(H::bandParam(0, H::BandShape)), 3.0, 1e-6);
    CHECK_NEAR(p.hush().getParameter(H::Link), 40.0, 1e-6);
    CHECK(p.hush().getParameter(H::bandParam(1, H::BandUsed)) >= 0.5f); // missing ids take their defaults (band 2 is on)
    file.deleteFile();
}

TEST_CASE("hush plugin: the installed reference's presets are offered and load")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::HushPlugin p;
    const auto presets = p.session().allPresets();
    CHECK(presets.front().name == "Factory default");
    if (! devices::HushSession::referencePresetFolders()[0].isDirectory()) return; // reference not installed here
    const auto found = std::find_if(presets.begin(), presets.end(), [](const plugin::HushPreset& x) { return x.name == "General de-ess"; });
    REQUIRE(found != presets.end());
    CHECK(found->category == "vocals");
    p.session().loadPreset(*found);
    CHECK_NEAR(p.hush().getParameter(H::Mode), 1.0, 1e-6);
    CHECK_NEAR(p.hush().getParameter(H::bandParam(1, H::BandShape)), 6.0, 1e-6); // high shelf
    CHECK(p.session().presetCategory() == "vocals");
    int reference = 0;
    for (const auto& x : presets) reference += x.file.isAChildOf(devices::HushSession::referencePresetFolders()[0]) ? 1 : 0;
    CHECK(reference >= 60);
}

TEST_CASE("hush plugin: the editor takes clicks, keys and wheel through the root like a host window")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::HushPlugin p;
    p.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> audio(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng(3);
    for (int block = 0; block < 200; ++block)
    {
        for (int s = 0; s < 512; ++s)
        {
            const float t = (float) (block * 512 + s) / 48000.0f;
            const float v = 0.05f * (rng.nextFloat() * 2 - 1) + 0.4f * std::sin(6.2831853f * 3000.0f * t) + 0.3f * std::sin(6.2831853f * 700.0f * t);
            audio.setSample(0, s, v);
            audio.setSample(1, s, v);
        }
        p.processBlock(audio, midi);
    }
    CHECK(p.hush().maxReduction(0) > 1.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditorAndMakeActive());
    auto* window = dynamic_cast<plugin::HushPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    CHECK(window->getWidth() == 1050); // the reference's default size
    CHECK(window->getHeight() == 621);
    auto& view = window->view();
    auto& face = view.face();
    // Menus are checked through lastMenu/lastMenuResult; a real popup needs a desktop window, and JUCE's X11 window
    // setup writes WM_PROTOCOLS without checking the atom exists, which kills the process on a bare Xvfb (no WM).
    face.menuHost = nullptr;
    view.setBounds({0, 0, plugin::HushView::kWidth, plugin::HushView::kHeight});
    view.layout();
    const auto click = [&](float x, float y, int clicks = 1) {
        ui::MouseEvent e;
        e.x = x;
        e.y = y;
        e.clicks = clicks;
        view.dispatchMouseDown(e);
        view.dispatchMouseUp(e);
    };
    click(105, 104); // "hard"
    CHECK_NEAR(p.hush().getParameter(H::Mode), 1.0, 1e-6);
    click(200, view.height() - 17); // bypass
    CHECK(p.hush().getParameter(H::Bypass) >= 0.5f);
    click(200, view.height() - 17);
    CHECK(p.hush().getParameter(H::Bypass) < 0.5f);
    // Delta, clicked like a host window does: the output becomes exactly what Hush removes. A second instance with the
    // same settings and delta off gets the same audio; delta must equal the dry signal (delayed by the latency) minus it.
    click(296, view.height() - 17);
    REQUIRE(p.hush().getParameter(H::Delta) >= 0.5f);
    {
        plugin::HushPlugin q;
        juce::MemoryBlock state;
        p.getStateInformation(state);
        q.setStateInformation(state.getData(), (int) state.getSize());
        q.hush().setParameter(H::Delta, 0.0f);
        q.prepareToPlay(48000, 512);
        const int latency = p.getLatencySamples();
        REQUIRE(latency == q.getLatencySamples());
        std::vector<float> dry, delta, wet;
        juce::AudioBuffer<float> other(2, 512);
        for (int block = 0; block < 160; ++block)
        {
            for (int s = 0; s < 512; ++s)
            {
                const float t = (float) ((200 + block) * 512 + s) / 48000.0f;
                const float v = 0.05f * (rng.nextFloat() * 2 - 1) + 0.4f * std::sin(6.2831853f * 3000.0f * t) + 0.3f * std::sin(6.2831853f * 700.0f * t);
                audio.setSample(0, s, v);
                audio.setSample(1, s, v);
                other.setSample(0, s, v);
                other.setSample(1, s, v);
                dry.push_back(v);
            }
            p.processBlock(audio, midi);
            q.processBlock(other, midi);
            for (int s = 0; s < 512; ++s)
            {
                delta.push_back(audio.getSample(0, s));
                wet.push_back(other.getSample(0, s));
            }
        }
        double err = 0, ref = 0;
        for (size_t i = 120 * 512; i < delta.size(); ++i) // both have settled on the same audio by then
        {
            const double expected = dry[i - (size_t) latency] - wet[i];
            err += (delta[i] - expected) * (delta[i] - expected);
            ref += expected * expected;
        }
        CHECK(ref > 0.0);
        CHECK(err < ref * 0.01); // within -20 dB of the removed part
    }
    click(296, view.height() - 17);
    CHECK(p.hush().getParameter(H::Delta) < 0.5f);
    click(864, 17); // settings
    CHECK(std::find(face.lastMenu.begin(), face.lastMenu.end(), "Show frequency near cursor") != face.lastMenu.end());
    const bool before = plugin::HushSettings::get().frequencyNearCursor.load();
    face.lastMenuResult(6);
    CHECK(plugin::HushSettings::get().frequencyNearCursor.load() != before);
    face.lastMenuResult(6); // back as it was (the settings file is the user's)
    click(400, 17); // preset field
    CHECK(std::find(face.lastMenu.begin(), face.lastMenu.end(), "Save as...") != face.lastMenu.end());
    click(748, 17); // a/b: b
    CHECK(p.session().activeSlot() == 1);
    click(724, 17); // a
    CHECK(p.session().activeSlot() == 0);
    // Double-click on the graph adds a bell at the pointer.
    click(face.freqToX(1000.0f), face.zeroY() - 30.0f, 2);
    CHECK(face.selectedBand() == 3);
    CHECK_NEAR(p.hush().getParameter(H::bandParam(3, H::BandFrequency)), 1000.0, 15.0);
    // Knob drag through the root.
    ui::MouseEvent e;
    e.x = 87;
    e.y = 196;
    view.dispatchMouseDown(e);
    e.y -= 36;
    view.dispatchMouseDrag(e);
    view.dispatchMouseUp(e);
    CHECK(p.hush().getParameter(H::Depth) > 6.5f);
    // Wheel over the detail knob.
    const float detail = p.hush().getParameter(H::Detail);
    ui::MouseEvent w;
    w.x = 87;
    w.y = 345;
    w.wheelDeltaY = 0.2f;
    view.dispatchMouseWheel(w);
    CHECK(p.hush().getParameter(H::Detail) > detail);
    // Typing a value: double-click the mix box, type, Enter.
    click(400, view.height() - 17, 2);
    for (const char ch : std::string("50"))
    {
        ui::KeyEvent k;
        k.keyCode = ch;
        k.textChar = (uint32_t) ch;
        view.dispatchKey(k);
    }
    ui::KeyEvent enter;
    enter.keyCode = juce::KeyPress::returnKey;
    view.dispatchKey(enter);
    CHECK_NEAR(p.hush().getParameter(H::Mix), 50.0, 1e-4);
    // Undo through the toolbar button brings the mix back.
    click(662, 17);
    CHECK_NEAR(p.hush().getParameter(H::Mix), 100.0, 1e-4);
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_hush_plugin.png");
    CHECK(gfx::renderToPng(view, (int) plugin::HushView::kWidth, (int) plugin::HushView::kHeight, 2.0f, png.getFullPathName().toStdString()));
    p.session().sidePanelOpen = true;
    view.layout();
    const auto png2 = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_hush_plugin_side.png");
    CHECK(gfx::renderToPng(view, (int) plugin::HushView::kWidth, (int) plugin::HushView::kHeight, 2.0f, png2.getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("hush plugin and the built-in device: the same state bytes and the same sound")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::HushPlugin p;
    p.getParameters()[H::Depth]->setValue(0.75f);
    p.getParameters()[H::Mode]->setValue(1.0f);
    auto* freq = p.getParameters()[H::bandParam(1, H::BandFrequency)];
    freq->setValue(freq->getValueForText("2k5"));
    p.session().copyToOtherSlot();
    juce::MemoryBlock state;
    p.getStateInformation(state);

    // A set saved with the VST3 opens on the built-in device with the same values, and saves back the same bytes.
    devices::HushDevice builtIn;
    std::string error;
    REQUIRE(builtIn.session().decodeState(state.getData(), state.getSize(), error));
    for (int i = 0; i < H::kParams; ++i) CHECK_NEAR(builtIn.getParameter(i), p.hush().getParameter(i), 1e-6);
    const auto bytes = builtIn.session().encodeState();
    CHECK(bytes.size() == state.getSize());
    CHECK(std::equal(bytes.begin(), bytes.end(), static_cast<const uint8_t*>(state.getData())));
    // And the other way round.
    builtIn.session().setFromEditor(H::Detail, 8.0f);
    const auto edited = builtIn.session().encodeState();
    plugin::HushPlugin q;
    q.setStateInformation(edited.data(), (int) edited.size());
    CHECK_NEAR(q.hush().getParameter(H::Detail), 8.0, 1e-6);
    CHECK(q.buildLog.empty());

    // The same input through the VST3's processBlock and the built-in device gives the same output.
    p.hush().setParameter(H::Detail, 8.0f);
    p.prepareToPlay(48000, 512);
    builtIn.prepare(48000, 512);
    CHECK(builtIn.latencySamples() == p.getLatencySamples());
    juce::AudioBuffer<float> a(2, 512), b(2, 512);
    juce::MidiBuffer midi;
    juce::Random rng(5);
    double maxDiff = 0;
    for (int block = 0; block < 120; ++block)
    {
        for (int s = 0; s < 512; ++s)
        {
            const float t = (float) (block * 512 + s) / 48000.0f;
            const float v = 0.05f * (rng.nextFloat() * 2 - 1) + 0.4f * std::sin(6.2831853f * 3000.0f * t);
            a.setSample(0, s, v);
            a.setSample(1, s, -0.5f * v);
        }
        b.makeCopyOf(a);
        p.processBlock(a, midi);
        builtIn.process(b, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int s = 0; s < 512; ++s) maxDiff = std::max(maxDiff, (double) std::abs(a.getSample(ch, s) - b.getSample(ch, s)));
    }
    CHECK(maxDiff < 1e-6);
    CHECK(builtIn.maxReduction(0) > 1.0f);
}
