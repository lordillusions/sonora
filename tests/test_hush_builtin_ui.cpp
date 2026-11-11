// PLG-14g: the built-in Sonora Hush shows the VST3's face in the device chain (scaled to the chain's height) and in its
// own window, and both edit the running device.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/HushSession.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/HushFace.h"
#include "ui/views/MainLayout.h"

using namespace sonora;
namespace
{
using H = devices::Hush;
template <typename T> void collect(ui::Widget& w, std::vector<T*>& found)
{
    if (auto* p = dynamic_cast<T*>(&w)) found.push_back(p);
    for (const auto& child : w.children()) collect(*child, found);
}
ui::MouseEvent at(ui::Widget& widget, float x, float y, int clicks = 1)
{
    ui::MouseEvent e;
    const auto origin = widget.boundsInRoot();
    e.x = origin.x + x;
    e.y = origin.y + y;
    e.clicks = clicks;
    return e;
}
juce::File snapshotFolder()
{
    auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_tests");
    folder.createDirectory();
    return folder;
}
} // namespace

TEST_CASE("hush built-in face: the device chain shows the Hush face at the chain's height, and it edits the device")
{
    juce::ScopedJuceInitialiser_GUI init;
    model::Song song;
    song.tracks.resize(1);
    song.tracks[0].id = 3;
    song.tracks[0].name = "Vocal";
    auto hushModel = devices::nativeDeviceModel("SonoraHush");
    hushModel.id = 21;
    song.tracks[0].devices = {hushModel};
    engine::SampleCache samples;
    devices::BuildContext build;
    auto graph = engine::buildGraph(song, build, samples);
    graph.graph->prepare(48000, 512);
    auto* hush = dynamic_cast<devices::HushDevice*>(graph.graph->nodes[0].processorForModelDevice[0]);
    REQUIRE(hush != nullptr);
    // Some audio through it, so the reduction graph has something to show.
    {
        juce::AudioBuffer<float> audio(2, 512);
        juce::MidiBuffer midi;
        juce::Random rng(2);
        for (int block = 0; block < 120; ++block)
        {
            for (int s = 0; s < 512; ++s)
            {
                const float t = (float) (block * 512 + s) / 48000.0f;
                const float v = 0.08f * (rng.nextFloat() * 2 - 1) + 0.4f * std::sin(6.2831853f * 3000.0f * t)
                                + 0.3f * std::sin(6.2831853f * 700.0f * t);
                audio.setSample(0, s, v);
                audio.setSample(1, s, v);
            }
            hush->process(audio, midi);
        }
    }

    ui::UiContext context;
    context.song = &song;
    context.graph = graph.graph.get();
    context.controls = graph.graph->controls;
    context.browserVisible = false;
    context.detailTab = ui::DetailTab::Devices;
    ui::MainLayout root(context);
    root.setBounds({0, 0, 1500, 820});
    root.layout();
    std::vector<ui::HushFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.height() > 100.0f);
    CHECK_NEAR(face.width() / face.height(), ui::HushFace::kWidth / ui::HushFace::kHeight, 0.01); // keeps the VST3's aspect
    const float s = face.scale();

    // Clicks through the root reach the face (soft/hard switch, bypass, a band node on the graph).
    auto click = [&](float x, float y, int clicks = 1) {
        auto e = at(face, x * s, y * s, clicks);
        root.dispatchMouseDown(e);
        root.dispatchMouseUp(e);
    };
    click(105, 104); // "hard"
    CHECK_NEAR(hush->getParameter(H::Mode), 1.0, 1e-6);
    click(200, ui::HushFace::kHeight - 17); // bypass
    CHECK(hush->getParameter(H::Bypass) >= 0.5f);
    click(200, ui::HushFace::kHeight - 17);
    // A knob drag in the chain.
    auto knob = at(face, 87 * s, 196 * s);
    root.dispatchMouseDown(knob);
    knob.y -= 30;
    root.dispatchMouseDrag(knob);
    root.dispatchMouseUp(knob);
    CHECK(hush->getParameter(H::Depth) > 5.5f);
    // The face's own undo (toolbar) brings it back.
    click(662, 17);
    CHECK_NEAR(hush->getParameter(H::Depth), 5.0, 1e-4);
    // Double-click on the graph adds a band, as in the VST3.
    click(face.freqToX(1000.0f), face.zeroY() - 30.0f, 2);
    CHECK(face.selectedBand() == 3);
    CHECK(hush->getParameter(H::bandParam(3, H::BandUsed)) >= 0.5f);

    // MIDI map mode: the control under the pointer is the device parameter, like any device knob.
    const auto target = face.mappingTargetAt(105 * s, 104 * s);
    REQUIRE(target.has_value());
    CHECK(target->kind == midi::ControlTarget::Kind::DeviceParameter);
    CHECK(target->param == "Mode");
    CHECK(target->track == 0);
    CHECK(target->device == 0);

    // Size menu offers its own window in the chain.
    face.menuHost = nullptr;
    click(886, 17);
    CHECK(std::find(face.lastMenu.begin(), face.lastMenu.end(), "Open in a window") != face.lastMenu.end());
    bool opened = false;
    context.openHushWindow = [&](const ui::UiContext::WavetableKey& key) {
        opened = key.trackId == 3 && key.deviceId == 21 && key.chain == -1;
    };
    face.lastMenuResult(10);
    CHECK(opened);

    const auto png = snapshotFolder().getChildFile("sonora_hush_builtin_chain.png");
    CHECK(gfx::renderToPng(root, 1500, 820, 1.0f, png.getFullPathName().toStdString()));

    // Its own window: the same session at the VST3's size, following the instance until it is deleted.
    ui::HushWindowView window(context, {3, 21, -1, -1});
    bool gone = false;
    window.onGone = [&] { gone = true; };
    window.setBounds({0, 0, 1050, 621});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.title() == "Sonora Hush/Vocal");
    CHECK(&window.face()->session() == &hush->session());
    const float k = window.face()->scale();
    CHECK_NEAR(k, 1050.0 / 900.0, 1e-4);
    auto e = at(*window.face(), 60.0f * k, 104.0f * k); // "soft"
    window.dispatchMouseDown(e);
    window.dispatchMouseUp(e);
    CHECK_NEAR(hush->getParameter(H::Mode), 0.0, 1e-6);
    const auto windowPng = snapshotFolder().getChildFile("sonora_hush_builtin_window.png");
    CHECK(gfx::renderToPng(window, 1050, 621, 1.0f, windowPng.getFullPathName().toStdString()));
    // A window shorter than the face's aspect (clamped to the screen, or resized): the whole face still fits, bottom
    // buttons included, centred (it used to scale by the width only and lose the footer).
    window.setBounds({0, 0, 1050, 520});
    window.layout();
    CHECK(window.face()->bounds().bottom() <= 520.5f);
    CHECK(window.face()->bounds().y >= 0.0f);
    CHECK_NEAR(window.face()->width() / window.face()->height(), ui::HushFace::kWidth / ui::HushFace::kHeight, 1e-3);
    CHECK_NEAR(window.face()->bounds().centreX(), 525.0, 1.0);
    const auto shortPng = snapshotFolder().getChildFile("sonora_hush_builtin_window_short.png");
    CHECK(gfx::renderToPng(window, 1050, 520, 1.0f, shortPng.getFullPathName().toStdString()));
    window.setBounds({0, 0, 1050, 621});
    window.layout();
    song.tracks[0].devices.clear();
    auto rebuilt = engine::buildGraph(song, build, samples);
    context.graph = rebuilt.graph.get();
    context.controls = rebuilt.graph->controls;
    window.sync();
    CHECK(gone);
    CHECK(window.face() == nullptr);
}
