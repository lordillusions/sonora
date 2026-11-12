// PLG-18 / PLG-23: the Sonora Stutter face (ProFace kit) in the VST3 window, the app's device chain and its own window,
// driven with real input.
#include "ProFaceTestKit.h"
#include "devices/native/Stutter.h"
#include "plugin/StutterPlugin.h"
#include "plugin/StutterPluginEditor.h"
#include "ui/views/StutterFace.h"
#include "ui/widgets/Buttons.h"

#include <cmath>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using S = devices::Stutter;
constexpr double kRate = 48000.0;

// A "voice": a 220 Hz tone with a syllable envelope every 250 ms.
juce::AudioBuffer<float> voice(int samples)
{
    juce::AudioBuffer<float> a(2, samples);
    for (int s = 0; s < samples; ++s)
    {
        const float t = (float) s / (float) kRate, syllable = std::fmod(t, 0.25f);
        const float v = 0.5f * std::sin(6.2831853f * 220.0f * t) * std::min(1.0f, syllable * 40.0f) * std::exp(-syllable * 6.0f);
        a.setSample(0, s, v);
        a.setSample(1, s, v);
    }
    return a;
}
} // namespace

TEST_CASE("stutter plugin: the full face; modes, Motion and the hold pad with real clicks; Macro hands over to the knobs")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::StutterPlugin p;
    CHECK(p.getParameters().size() == S::NumParams);
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::StutterPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    CHECK(window->getWidth() > 800);
    auto& view = window->view();
    const auto size = ui::StutterFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    CHECK(face.layoutMode() == ui::ProFace::Layout::Full);
    CHECK_NEAR(face.scale(), 1.0f, 1e-4);

    REQUIRE(click(view, face, "Freeze"));
    CHECK(std::lround(p.device().getParameter(S::ModeParam)) == S::Freeze);
    REQUIRE(click(view, face, "Sync to tempo"));
    CHECK(p.device().getParameter(S::Sync) > 0.5f);
    REQUIRE(click(view, face, "Free"));
    REQUIRE(click(view, face, "4 Bars"));
    CHECK(std::lround(p.device().getParameter(S::Capture)) == 3);
    const float motion = p.device().getParameter(S::Motion);
    REQUIRE(drag(view, face, "MOTION", -60));
    CHECK(p.device().getParameter(S::Motion) > motion + 0.1f);
    CHECK(p.device().getParameter(S::Macro) > 0.5f);

    // Hold the pad while the voice plays: it stutters; the display shows the grains.
    const auto pad = face.controlCentre("HOLD");
    REQUIRE(pad.has_value());
    auto audio = voice(96000);
    for (int start = 0, block = 0; start < 96000; start += 512, ++block)
    {
        if (start == 48128) view.dispatchMouseDown(designAt(face, pad->first, pad->second));
        if (block % 4 == 0) view.tickTree();
        juce::AudioBuffer<float> chunk(audio.getArrayOfWritePointers(), 2, start, std::min(512, 96000 - start));
        juce::MidiBuffer midi;
        p.processBlock(chunk, midi);
    }
    view.tickTree();
    CHECK(face.view().engaged);
    CHECK(face.view().grains.size() > 4);
    CHECK(face.statusLine().find("grains") != std::string::npos);
    const auto png = testFile("sonora_stutter_plugin.png");
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, png.getFullPathName().toStdString()));
    view.dispatchMouseUp(designAt(face, pad->first, pad->second));
    juce::AudioBuffer<float> chunk(2, 512);
    juce::MidiBuffer midi;
    p.processBlock(chunk, midi);
    view.tickTree();
    CHECK(! face.view().engaged);

    // A grain knob turned by hand takes over from Motion, from where Motion had it.
    const float speedBefore = p.device().settings().speed;
    REQUIRE(drag(view, face, "SPEED", -10));
    CHECK(p.device().getParameter(S::Macro) < 0.5f);
    CHECK(p.device().getParameter(S::Speed) > speedBefore);
    // Hover help.
    const auto every = face.controlCentre("EVERY");
    REQUIRE(every.has_value());
    view.dispatchMouseMove(designAt(face, every->first, every->second));
    CHECK(face.helpLine().rfind("Every:", 0) == 0);
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("stutter: the compact face in the app's chain, mapped, with a window button; the window shows the full face")
{
    Chain chain(model::TrackKind::Audio, {devices::nativeDeviceModel("SonoraStutter")});
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1400, 900});
    root.layout();
    std::vector<ui::StutterFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    CHECK_NEAR(face.width() / face.height(), ui::StutterFace::kCompact.w / ui::StutterFace::kCompact.h, 0.01);
    CHECK(face.mappingTarget != nullptr);
    const auto motion = face.controlCentre("MOTION");
    REQUIRE(motion.has_value());
    const auto local = face.designToLocal(motion->first, motion->second);
    const auto target = face.mappingTargetAt(local.x, local.y);
    REQUIRE(target.has_value());
    CHECK(target->param == "Motion");
    std::vector<ui::IconButton*> icons;
    collect(root, icons);
    bool windowButton = false;
    for (auto* b : icons) windowButton = windowButton || b->tooltip == "Open the editor in its own window";
    CHECK(windowButton);
    REQUIRE(click(root, face, "Shatter"));
    CHECK(std::lround(chain.processor()->getParameter(S::ModeParam)) == S::Shatter);
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, testFile("sonora_stutter_chain.png").getFullPathName().toStdString()));

    ui::ProFaceWindowView window(chain.context, {1, 10, -1, -1});
    window.setBounds({0, 0, ui::StutterFace::kFull.w, ui::StutterFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);
    CHECK(window.title() == "Sonora Stutter/Bass");
}
