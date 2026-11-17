// PLG-24c: Sonora Correlation's Kick x Bass face in the VST3 window (the kick on the sidechain bus, the host at 145 BPM)
// and in the app's device chain: the verdicts, the value big with decimals, the beat view, the low bands with their values,
// Listen Bass inverted, Sync, Kick From, by real clicks and drags.
#include "ProFaceTestKit.h"
#include "devices/native/Correlation.h"
#include "plugin/CorrelationPlugin.h"
#include "plugin/CorrelationPluginEditor.h"
#include "ui/views/CorrelationFace.h"

#include <cmath>

using namespace sonora;
using namespace sonora::testkit;
namespace
{
using C = devices::Correlation;
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTempo = 145.0;
constexpr double kBeat = 60.0 / kTempo;
constexpr double kSixteenth = kBeat / 4.0;

struct Head final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm(kTempo);
        p.setIsPlaying(true);
        return p;
    }
};

// A psytrance K-B-B-B beat below 160 Hz: the kick's 50 Hz body fading with `tail`, the bass on the three 16ths after it.
struct Beat
{
    double tail = 0.045, phase = 0.0;
    float kick(double t) const
    {
        const double in = std::fmod(t, kBeat);
        if (in > 0.2) return 0.0f;
        return (float) (0.8 * std::min(1.0, in / 0.002) * std::exp(-in / tail) * std::sin(2.0 * kPi * 50.0 * in));
    }
    float bass(double t) const
    {
        const double in = std::fmod(t, kBeat);
        const int step = (int) (in / kSixteenth);
        const double note = in - step * kSixteenth;
        if (step == 0 || note > 0.09) return 0.0f;
        return (float) (0.5 * std::min(1.0, note / 0.003) * std::min(1.0, (0.09 - note) / 0.01) * std::sin(2.0 * kPi * 50.0 * in + phase));
    }
};

// The plug-in's buffer: the bass on the main input (0, 1), the kick on the sidechain (2, 3) unless `sidechain` is false.
void feed(juce::AudioProcessor& p, const Beat& beat, int& at, double seconds, ui::RootWidget& view, bool sidechain = true)
{
    juce::AudioBuffer<float> block(sidechain ? 4 : 2, 512);
    juce::MidiBuffer midi;
    for (int done = 0, i = 0; done < (int) (seconds * kRate); done += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
        {
            const double t = (double) (at + s) / kRate;
            for (int ch = 0; ch < 2; ++ch) block.setSample(ch, s, beat.bass(t));
            if (sidechain)
                for (int ch = 2; ch < 4; ++ch) block.setSample(ch, s, beat.kick(t));
        }
        p.processBlock(block, midi);
        at += 512;
        if (i % 4 == 0) view.tickTree();
    }
    view.tickTree();
}
} // namespace

TEST_CASE("correlation plugin kick x bass: verdicts from the sidechain, the big value, beat view, bands; Listen, Sync, Kick From")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::CorrelationPlugin p;
    Head head;
    p.setPlayHead(&head);
    p.prepareToPlay(kRate, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::CorrelationPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::CorrelationFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    view.tickTree();
    REQUIRE(click(view, face, juce::String::fromUTF8("Kick \xC3\x97 Bass").toStdString()));
    CHECK(std::lround(p.device().getParameter(C::Mode)) == C::MeasureKickBass);
    view.tickTree(); // the face rebuilds for Kick x Bass
    CHECK(face.kickBass());
    CHECK(face.controlCentre("Bass").has_value());
    CHECK(face.controlCentre("Sidechain").has_value());
    CHECK(! face.controlCentre("Polar").has_value());

    // No sidechain yet: it says how to route the kick.
    Beat beat;
    int at = 0;
    feed(p, beat, at, 1.0, view, false);
    CHECK(face.verdict().title.find("No kick") == 0);
    CHECK(face.pairWord() == "no kick");

    // The kick on the sidechain, in phase where its tail meets the bass.
    feed(p, beat, at, 3.0, view);
    auto r = p.device().reading();
    CHECK(r.sidechain);
    CHECK(r.overlap);
    CHECK(r.correlation > 0.8f);
    CHECK(face.verdict().title.find("In phase where they meet (+0.") == 0);
    CHECK(face.pairWord() == "in phase");
    CHECK(face.beatPoints() > 300);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_kickbass_in_phase.png").getFullPathName().toStdString()));

    // The bass inverted: out of phase, flip the bass.
    beat.phase = kPi;
    feed(p, beat, at, 3.0, view);
    r = p.device().reading();
    CHECK(r.correlation < -0.8f);
    CHECK(face.verdict().title.find("Out of phase where they meet (-0.") == 0);
    CHECK(face.verdict().detail.find("Flip the bass's polarity") != std::string::npos);
    CHECK(face.pairWord() == "out of phase");
    // The pointer over the 50 Hz band: the help line reads it, the callout shows its value.
    const auto [first, count] = face.bandSpan();
    CHECK(first == 1);
    CHECK(count == 11);
    const float slot = (398.0f - 36.0f) / (float) count; // bandsPlot (Kick x Bass): 30 px of labels, 6 px right
    const float bandX = 440.0f + 30.0f + slot * (4.0f - (float) first + 0.5f);
    REQUIRE(face.bandAt(bandX, 600) == 4);
    view.dispatchMouseMove(designAt(face, bandX, 600));
    view.tickTree();
    CHECK(face.helpLine() == face.bandReadout(4));
    CHECK(face.bandReadout(4).find("They cancel here.") != std::string::npos);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_kickbass_plugin.png").getFullPathName().toStdString()));
    view.dispatchMouseMove(designAt(face, 600, 400));

    // Listen Bass inverted flips the bass's polarity (the kick track keeps playing in the host).
    REQUIRE(click(view, face, juce::String::fromUTF8("Bass \xC3\x98").toStdString()));
    CHECK(std::lround(p.device().getParameter(C::Listen)) == C::ListenBassInverted);
    {
        juce::AudioBuffer<float> block(4, 512);
        juce::MidiBuffer midi;
        for (int round = 0; round < 3; ++round) // past the 10 ms crossfade
        {
            for (int s = 0; s < 512; ++s)
            {
                const double t = (double) (at + s) / kRate;
                for (int ch = 0; ch < 2; ++ch) block.setSample(ch, s, beat.bass(t));
                for (int ch = 2; ch < 4; ++ch) block.setSample(ch, s, beat.kick(t));
            }
            juce::AudioBuffer<float> input(block);
            p.processBlock(block, midi);
            at += 512;
            if (round < 2) continue;
            int bad = 0;
            for (int s = 0; s < 512; ++s)
                for (int ch = 0; ch < 2; ++ch)
                    if (std::abs(block.getSample(ch, s) + input.getSample(ch, s)) > 1e-6f) ++bad;
            CHECK(bad == 0);
        }
    }
    REQUIRE(click(view, face, "Bass"));
    CHECK(std::lround(p.device().getParameter(C::Listen)) == C::ListenStereo);

    // Sync: Integration as a note value at the host's tempo.
    REQUIRE(click(view, face, "note"));
    CHECK(p.device().getParameter(C::Sync) >= 0.5f);
    view.tickTree();
    const float note = p.device().getParameter(C::IntegrationNote);
    REQUIRE(drag(view, face, "INTEGRATION", 40)); // down: a shorter note
    const float shorter = p.device().getParameter(C::IntegrationNote);
    CHECK(shorter < note);
    CHECK(std::abs(shorter - std::round(shorter)) <= 0.0f); // whole note values
    feed(p, beat, at, 0.05, view);
    CHECK_NEAR(p.device().integrationMs(), C::noteBeats((int) std::lround(shorter)) * 60000.0 / kTempo, 0.01);

    // Typed values (UI-93 dials): the note knob takes a note value by name, the ms knob a number.
    auto& context = view.context();
    const auto typeInto = [&](int param, const char* text) {
        const auto field = face.knobValueRect(param);
        REQUIRE(field.has_value());
        view.dispatchMouseDown(designAt(face, field->centreX(), field->centreY()));
        view.dispatchMouseUp(designAt(face, field->centreX(), field->centreY()));
        REQUIRE(context.valueEntry.active());
        for (; *text; ++text)
        {
            ui::KeyEvent k;
            k.keyCode = (int) *text;
            k.textChar = (uint32_t) *text;
            view.dispatchKey(k);
        }
        ui::KeyEvent enter;
        enter.keyCode = juce::KeyPress::returnKey;
        view.dispatchKey(enter);
    };
    typeInto(C::IntegrationNote, "1/16");
    CHECK(std::lround(p.device().getParameter(C::IntegrationNote)) == 2);
    REQUIRE(click(view, face, "ms"));
    view.tickTree(); // the knob is Integration in ms again
    typeInto(C::Integration, "45");
    CHECK_NEAR(p.device().getParameter(C::Integration), 45.0, 1e-3);
    REQUIRE(click(view, face, "note"));
    view.tickTree();

    // A short kick that is gone before the bass: they take turns.
    beat = Beat{0.008, 0.0};
    feed(p, beat, at, 3.0, view);
    CHECK(face.verdict().title == "Kick and bass take turns");
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_correlation_kickbass_turns.png").getFullPathName().toStdString()));

    // Kick From Left: the pan trick on a group; Listen offers the mono sums.
    REQUIRE(click(view, face, "Left (bass right)"));
    CHECK(std::lround(p.device().getParameter(C::KickFrom)) == C::KickLeft);
    view.tickTree();
    CHECK(face.controlCentre(juce::String::fromUTF8("Mono, bass \xC3\x98").toStdString()).has_value());
    CHECK(! face.controlCentre(juce::String::fromUTF8("Bass \xC3\x98").toStdString()).has_value());

    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::CorrelationPlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    for (int i = 0; i < C::NumParams; ++i) CHECK_NEAR(restored.device().getParameter(i), p.device().getParameter(i), 1e-4);
    p.setPlayHead(nullptr);
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("correlation kick x bass: the compact face in the app's chain (beat view, value, low bands with values)")
{
    Chain chain(model::TrackKind::Audio, {devices::nativeDeviceModel("SonoraCorrelation")});
    chain.song.tracks[0].name = "Bass";
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1400, 900});
    root.layout();
    std::vector<ui::CorrelationFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    auto* meter = dynamic_cast<C*>(chain.processor());
    REQUIRE(meter != nullptr);
    CHECK(meter->takesSidechain()); // the chain shows its Sidechain section
    REQUIRE(click(root, face, juce::String::fromUTF8("K\xC3\x97" "B").toStdString()));
    CHECK(std::lround(meter->getParameter(C::Mode)) == C::MeasureKickBass);
    root.tickTree();
    CHECK(face.kickBass());
    Beat beat{0.045, kPi};
    juce::AudioBuffer<float> block(2, 512), kick(2, 512);
    juce::MidiBuffer midi;
    devices::HostInfo host;
    host.tempo = kTempo;
    for (int at = 0, i = 0; at < (int) (3.0 * kRate); at += 512, ++i)
    {
        for (int s = 0; s < 512; ++s)
        {
            const double t = (double) (at + s) / kRate;
            for (int ch = 0; ch < 2; ++ch)
            {
                block.setSample(ch, s, beat.bass(t));
                kick.setSample(ch, s, beat.kick(t));
            }
        }
        meter->setHostInfo(host);
        meter->setSidechain(&kick, 1.0f); // what the engine does each block for a routed sidechain
        meter->process(block, midi);
        if (i % 4 == 0) root.tickTree();
    }
    root.tickTree();
    CHECK(face.pairWord() == "out of phase");
    CHECK(face.beatPoints() > 300);
    REQUIRE(click(root, face, juce::String::fromUTF8("Bass \xC3\x98").toStdString()));
    CHECK(std::lround(meter->getParameter(C::Listen)) == C::ListenBassInverted);
    CHECK(gfx::renderToPng(root, 1400, 900, 1.0f, testFile("sonora_correlation_kickbass_chain.png").getFullPathName().toStdString()));
}
