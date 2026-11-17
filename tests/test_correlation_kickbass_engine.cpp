// PLG-24c: Sonora Correlation's Kick x Bass through the real engine: the kick track routed to the sidechain of the
// Correlation on the bass track, as the app's Sidechain section sets it.
#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Correlation.h"
#include "engine/AudioEngine.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <cmath>

using namespace sonora;
namespace
{
using C = devices::Correlation;
constexpr double kPi = 3.14159265358979323846;

// A K-B-B-B beat at 145 BPM as two instruments: the kick's 50 Hz body on beats, the bass on the 16ths after them.
class Part final : public devices::DeviceProcessor
{
public:
    Part(bool kick, double phase) : phase_(phase), kick_(kick) {}
    std::string name() const override { return kick_ ? "kick" : "bass"; }
    bool isInstrument() const override { return true; }
    void prepare(double rate, int) override { rate_ = rate; }
    void process(juce::AudioBuffer<float>& a, juce::MidiBuffer&) override
    {
        const double beat = 60.0 / 145.0, sixteenth = beat / 4.0;
        for (int i = 0; i < a.getNumSamples(); ++i, ++at_)
        {
            const double t = (double) at_ / rate_, in = std::fmod(t, beat);
            double v = 0;
            if (kick_) v = in <= 0.2 ? 0.8 * std::min(1.0, in / 0.002) * std::exp(-in / 0.045) * std::sin(2.0 * kPi * 50.0 * in) : 0.0;
            else
            {
                const int step = (int) (in / sixteenth);
                const double note = in - step * sixteenth;
                if (step > 0 && note <= 0.09)
                    v = 0.5 * std::min(1.0, note / 0.003) * std::min(1.0, (0.09 - note) / 0.01) * std::sin(2.0 * kPi * 50.0 * in + phase_);
            }
            for (int ch = 0; ch < 2; ++ch) a.setSample(ch, i, a.getSample(ch, i) + (float) v);
        }
    }
    double phase_;

private:
    bool kick_;
    double rate_ = 44100;
    int64_t at_ = 0;
};

C* kickBassSong(engine::AudioEngine& eng, double bassPhase)
{
    model::Song song;
    for (int i = 0; i < 2; ++i)
    {
        model::Track t;
        t.id = 30 + i;
        t.kind = model::TrackKind::Audio;
        t.name = i == 0 ? "Kick" : "Bass";
        song.tracks.push_back(t);
    }
    auto meter = devices::nativeDeviceModel("SonoraCorrelation");
    meter.sidechain.on = true;
    meter.sidechain.sourceTrackId = 30;
    song.tracks[1].devices.push_back(meter);
    engine::SampleCache samples;
    devices::BuildContext ctx;
    auto built = engine::buildGraph(song, ctx, samples);
    if (built.graph->nodes[1].sidechains.size() != 1) return nullptr;
    auto* device = dynamic_cast<C*>(built.graph->nodes[1].processorForModelDevice[0]);
    if (device == nullptr) return nullptr;
    device->setParameter(C::Mode, (float) C::MeasureKickBass);
    device->setParameter(C::Sync, 1.0f);
    device->setParameter(C::IntegrationNote, 5.0f); // 1/2
    built.graph->nodes[0].devices.insert(built.graph->nodes[0].devices.begin(), std::make_unique<Part>(true, 0.0));
    built.graph->nodes[1].devices.insert(built.graph->nodes[1].devices.begin(), std::make_unique<Part>(false, bassPhase));
    eng.installGraph(std::move(built.graph));
    return device;
}
} // namespace

TEST_CASE("correlation kick x bass through the engine: the kick track on the sidechain, in phase and inverted")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    for (const double phase : {0.0, kPi})
    {
        plugins::PluginHost host;
        midi::MidiManager midi;
        engine::AudioEngine eng(host, midi);
        auto* meter = kickBassSong(eng, phase);
        REQUIRE(meter != nullptr);
        CHECK(meter->takesSidechain());
        std::vector<float> l(512), r(512);
        float* out[2] = {l.data(), r.data()};
        eng.transport().play();
        for (int b = 0; b < 300; ++b) eng.renderBlock(out, 2, 512); // ~3.5 s at 44.1 kHz
        const auto reading = meter->reading();
        std::printf("    bass phase %.0f deg: sidechain %d kick %d bass %d overlap %d c %+.2f sum %+.1f dB\n", phase * 180.0 / kPi,
                    (int) reading.sidechain, (int) reading.kick, (int) reading.bass, (int) reading.overlap, reading.correlation, reading.sumDb);
        CHECK(reading.kickBass);
        CHECK(reading.sidechain);
        CHECK(reading.kick);
        CHECK(reading.bass);
        CHECK(reading.overlap);
        if (phase < 1.0) CHECK(reading.correlation > 0.7f);
        else CHECK(reading.correlation < -0.7f);
        eng.transport().stop();
    }
}
