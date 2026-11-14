// PLG-24: the Sonora Correlation face (ProFace kit) in the VST3 window and in the app's device chain: the verdict, the
// meter, the scope, the bands, and the controls by real clicks.
#include "ProFaceTestKit.h"
#include "devices/native/Correlation.h"
#include "plugin/CorrelationPlugin.h"
#include "plugin/CorrelationPluginEditor.h"
#include "ui/views/CorrelationFace.h"

#include <cmath>
#include <random>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using C = devices::Correlation;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// A wide mix: a mono 220 Hz lead, decorrelated noise pads (wide) and, when `invertedBass`, a 55 Hz bass inverted on the right.
struct Mix
{
    std::mt19937 random{11};
    std::uniform_real_distribution<float> noise{-1.0f, 1.0f};
    bool invertedBass = false;
    float padL = 0, padR = 0;
    std::pair<float, float> at(int s)
    {
        const double t = (double) s / kRate;
        const float lead = 0.25f * (float) std::sin(2 * kPi * 220.0 * t) * (0.6f + 0.4f * (float) std::sin(2 * kPi * 0.7 * t));
        padL = 0.97f * padL + 0.03f * noise(random);
        padR = 0.97f * padR + 0.03f * noise(random);
        const float bass = 0.3f * (float) std::sin(2 * kPi * 55.0 * t);
        return {lead + 2.5f * padL + bass, lead + 2.5f * padR + (invertedBass ? -bass : bass)};
    }
};

void feed(juce::AudioProcessor& p, Mix& mix, int& at, double seconds, ui::RootWidget& view, bool* untouched = nullptr)
{
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    for (int done = 0, i = 0; done < (int) (seconds * kRate); done += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
        {
            const auto [l, r] = mix.at(at + s);
            block.setSample(0, s, l);
            block.setSample(1, s, r);
        }
        juce::AudioBuffer<float> input(block);
        p.processBlock(block, midi);
        if (untouched != nullptr)
            for (int s = 0; s < 512; ++s)
                if (std::abs(block.getSample(0, s) - input.getSample(0, s)) > 0.0f || std::abs(block.getSample(1, s) - input.getSample(1, s)) > 0.0f) *untouched = false;
        at += 512;
        if (i % 4 == 0) view.tickTree();
    }
    view.tickTree();
}
} // namespace

TEST_CASE("correlation plugin: the verdict, meter, scope and bands in the full face; controls by click; state")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CorrelationPlugin p;
    CHECK(p.getParameters().size() == C::NumParams);
    CHECK(p.getBusCount(true) == 1);
    p.prepareToPlay(kRate, 512);
    CHECK(p.getLatencySamples() == 0);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::CorrelationPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::CorrelationFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    view.tickTree();
    CHECK(face.layoutMode() == ui::ProFace::Layout::Full);
    CHECK(face.verdict().title == juce::String::fromUTF8("Waiting for audio\xE2\x80\xA6").toStdString());

    // A wide, healthy mix: mono-compatible, passed through untouched.
    Mix mix;
    int at = 0;
    bool untouched = true;
    feed(p, mix, at, 3.0, view, &untouched);
    CHECK(untouched);
    const auto healthy = p.device().reading();
    CHECK(healthy.signal);
    CHECK(healthy.correlation > 0.3f);
    CHECK(face.verdict().title.find("Mono-compatible") == 0);
    CHECK(face.scopePoints() == ui::CorrelationFace::kScopeRead);
    CHECK(face.autoZoomDb() >= 0.0f);
    CHECK(std::abs(face.zoomDb() - face.autoZoomDb()) <= 0.0f);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_healthy.png").getFullPathName().toStdString()));

    // The same mix with the bass inverted on one side: the low end is called out, with its band.
    mix.invertedBass = true;
    feed(p, mix, at, 3.0, view);
    CHECK(p.device().reading().lowEnd < -0.5f);
    CHECK(face.verdict().title.find("Low end out of phase") == 0);
    CHECK(face.verdict().detail.find("55 Hz") != std::string::npos || face.verdict().detail.find("50 Hz") != std::string::npos ||
          face.verdict().detail.find("63 Hz") != std::string::npos);
    const int worst = face.worstBand(true);
    REQUIRE(worst >= 0);
    CHECK(face.bandReadout(worst).find("Out of phase") != std::string::npos);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_plugin.png").getFullPathName().toStdString()));

    // The pointer over a band reads it in the help line.
    const float bandX = 410.0f + (float) worst * (676.0f / (float) C::kBands) + 8.0f; // CorrelationFace::bandsPlot, 30 px of labels
    REQUIRE(face.bandAt(bandX, 380) == worst);
    view.dispatchMouseMove(designAt(face, bandX, 380));
    CHECK(face.helpLine() == face.bandReadout(worst));

    REQUIRE(click(view, face, "Polar"));
    CHECK(std::lround(p.device().getParameter(C::ScopeMode)) == C::Polar);
    REQUIRE(click(view, face, "Mono"));
    CHECK(std::lround(p.device().getParameter(C::Listen)) == C::ListenMono);
    feed(p, mix, at, 0.1, view);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_polar_mono.png").getFullPathName().toStdString()));
    REQUIRE(click(view, face, "Stereo"));
    CHECK(std::lround(p.device().getParameter(C::Listen)) == C::ListenStereo);
    const float integration = p.device().getParameter(C::Integration);
    REQUIRE(drag(view, face, "INTEGRATION", -40));
    CHECK(p.device().getParameter(C::Integration) > integration);
    // Turning Zoom takes over from Auto at the auto value.
    CHECK(p.device().getParameter(C::ScopeAuto) >= 0.5f);
    REQUIRE(drag(view, face, "ZOOM", -10));
    CHECK(p.device().getParameter(C::ScopeAuto) < 0.5f);
    CHECK(p.device().getParameter(C::ScopeZoom) > 0.0f);
    REQUIRE(click(view, face, "Freeze"));
    CHECK(p.device().frozen());
    const auto seen = p.device().samplesSeen();
    feed(p, mix, at, 0.2, view);
    CHECK(p.device().samplesSeen() == seen);
    REQUIRE(click(view, face, "Freeze"));
    CHECK(! p.device().frozen());
    REQUIRE(click(view, face, "Reset"));
    feed(p, mix, at, 0.05, view);
    CHECK(p.device().historyCount() <= 3u);
    p.reportEditorChanges(); // the host hears the editor's edits

    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::CorrelationPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    for (int i = 0; i < C::NumParams; ++i) CHECK_NEAR(restored.device().getParameter(i), p.device().getParameter(i), 1e-4);
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("correlation: the compact face in the app's chain; the window shows the full face")
{
    Chain chain(model::TrackKind::Audio, {devices::nativeDeviceModel("SonoraCorrelation")});
    chain.song.tracks[0].name = "Mix Bus";
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1400, 900});
    root.layout();
    std::vector<ui::CorrelationFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    CHECK(face.mappingTarget != nullptr);
    auto* meter = dynamic_cast<C*>(chain.processor());
    REQUIRE(meter != nullptr);
    Mix mix;
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    for (int at = 0, i = 0; at < (int) (2.0 * kRate); at += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
        {
            const auto [l, r] = mix.at(at + s);
            block.setSample(0, s, l);
            block.setSample(1, s, r);
        }
        meter->process(block, midi);
        if (i % 4 == 0) root.tickTree();
    }
    root.tickTree();
    CHECK(face.verdict().title.find("Mono-compatible") == 0);
    REQUIRE(click(root, face, "Side"));
    CHECK(std::lround(meter->getParameter(C::Listen)) == C::ListenSide);
    REQUIRE(click(root, face, "Freeze"));
    CHECK(meter->frozen());
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, testFile("sonora_correlation_chain.png").getFullPathName().toStdString()));

    ui::ProFaceWindowView window(chain.context, {1, 10, -1, -1});
    window.setBounds({0, 0, ui::CorrelationFace::kFull.w, ui::CorrelationFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);
    CHECK(window.title() == "Sonora Correlation/Mix Bus");
}
