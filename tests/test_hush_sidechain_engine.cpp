// PLG-14i / PLG-22: the built-in Hush hears its sidechain through the real engine (reported: "sidechain on or off, listen on,
// nothing changes").
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/HushSession.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/RootWidget.h"
#include "ui/views/HushFace.h"

#include <cmath>

using namespace sonora;
namespace
{
class Tone final : public devices::DeviceProcessor
{
public:
    Tone(float hz, float level) : hz_(hz), level_(level) {}
    std::string name() const override { return "tone"; }
    bool isInstrument() const override { return true; }
    void prepare(double rate, int) override { rate_ = rate; }
    void process(juce::AudioBuffer<float>& a, juce::MidiBuffer&) override
    {
        for (int i = 0; i < a.getNumSamples(); ++i, phase_ += hz_ / rate_)
            for (int ch = 0; ch < 2; ++ch) a.setSample(ch, i, a.getSample(ch, i) + level_ * (float) std::sin(6.283185307 * phase_));
    }
    float hz_, level_;
    double rate_ = 44100, phase_ = 0;
};
} // namespace

TEST_CASE("hush sidechain: through the engine, Sidechain listen plays the routed track")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    plugins::PluginHost host;
    midi::MidiManager midi;
    engine::AudioEngine eng(host, midi);
    model::Song song;
    for (int i = 0; i < 2; ++i)
    {
        model::Track t;
        t.id = 20 + i;
        t.kind = model::TrackKind::Audio;
        t.name = i == 0 ? "Kick" : "Bass";
        song.tracks.push_back(t);
    }
    auto hush = devices::nativeDeviceModel("SonoraHush");
    hush.sidechain.on = true;
    hush.sidechain.sourceTrackId = 20;
    for (auto& p : hush.parameters)
    {
        if (p.path == "Sidechain_on") p.value = 1;
        if (p.path == "Sidechain_listen") p.value = 1;
    }
    song.tracks[1].devices.push_back(hush);
    // Bass mutes its own output: what reaches the mix from track 1 is only what Hush puts out.
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    REQUIRE(built.graph->nodes[1].sidechains.size() == 1);
    auto* device = dynamic_cast<devices::HushDevice*>(built.graph->nodes[1].processorForModelDevice[0]);
    REQUIRE(device != nullptr);
    CHECK(device->getParameter(devices::Hush::Sidechain) > 0.5f);
    CHECK(device->getParameter(devices::Hush::SidechainListen) > 0.5f);
    built.graph->nodes[0].devices.insert(built.graph->nodes[0].devices.begin(), std::make_unique<Tone>(1000.0f, 0.5f));
    built.graph->nodes[1].devices.insert(built.graph->nodes[1].devices.begin(), std::make_unique<Tone>(100.0f, 0.0f));
    built.graph->nodes[0].outputEnabled = false; // only the bass track (Hush listening to the kick) reaches the mix
    eng.installGraph(std::move(built.graph));
    std::vector<float> l(512), r(512);
    float* out[2] = {l.data(), r.data()};
    eng.transport().play();
    float peak = 0;
    for (int b = 0; b < 200; ++b)
    {
        eng.renderBlock(out, 2, 512);
        if (b > 100) for (float v : l) peak = std::max(peak, std::abs(v));
    }
    std::printf("    peak of the bass track with Sidechain listen: %.4f\n", peak);
    CHECK(peak > 0.2f); // the kick's tone, heard through Hush's sidechain listen
}

TEST_CASE("hush sidechain: the reduction follows the sidechain (a 1 kHz key cuts 1 kHz out of noise), and the face shows it")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // Noise on the main input, a loud 1 kHz tone on the sidechain: with Sidechain on, Hush hears the tone's resonance and
    // cuts 1 kHz from the noise; off, it hears smooth noise and barely cuts there.
    const auto reductionAt1k = [](bool sidechainOn, devices::HushDevice& hush) {
        hush.prepare(48000, 512);
        hush.setParameter(devices::Hush::Sidechain, sidechainOn ? 1.0f : 0.0f);
        juce::AudioBuffer<float> key(2, 512), audio(2, 512);
        juce::Random random(4);
        juce::MidiBuffer midi;
        double phase = 0;
        for (int block = 0; block < 300; ++block)
        {
            for (int i = 0; i < 512; ++i, phase += 1000.0 / 48000.0)
                for (int ch = 0; ch < 2; ++ch)
                {
                    key.setSample(ch, i, 0.6f * (float) std::sin(6.283185307 * phase));
                    audio.setSample(ch, i, random.nextFloat() * 0.4f - 0.2f);
                }
            hush.setSidechain(&key, 1.0f);
            hush.process(audio, midi);
        }
        float most = 0;
        for (int p = 0; p < devices::Hush::kDisplayPoints; ++p)
            if (std::abs(devices::Hush::displayFrequency(p) - 1000.0f) < 60.0f) most = std::max(most, hush.displayReduction(0, p));
        return most;
    };
    devices::HushDevice on, off;
    const float withKey = reductionAt1k(true, on), withoutKey = reductionAt1k(false, off);
    std::printf("    reduction at 1 kHz: sidechain on %.1f dB, off %.1f dB\n", withKey, withoutKey);
    CHECK(withKey > withoutKey + 3.0f);
    CHECK(on.detectingSidechain());
    CHECK(on.sidechainConnected());
    CHECK(! off.detectingSidechain());
    // The detector's spectrum peaks at the key's 1 kHz.
    int loudest = 0;
    for (int p = 1; p < devices::Hush::kDisplayPoints; ++p)
        if (on.displayDetector(p) > on.displayDetector(loudest)) loudest = p;
    CHECK(std::abs(devices::Hush::displayFrequency(loudest) - 1000.0f) < 120.0f);

    ui::UiContext context;
    ui::RootWidget root(context);
    auto* face = root.add<ui::HushFace>(on.session());
    root.setBounds({0, 0, 1050, 621});
    face->setBounds({0, 0, 1050, 621});
    root.layout();
    root.tickTree();
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_hush_sidechain.png");
    CHECK(gfx::renderToPng(root, 1050, 621, 1.0f, png.getFullPathName().toStdString()));
}
