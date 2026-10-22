#include "TestFramework.h"
#include "core/Edit.h"
#include "devices/DeviceFactory.h"
#include "devices/native/CurveLfo.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include <numbers>

using namespace sonora;
namespace
{
using L = devices::CurveLfo;
using Graph = dsp::CurveGraph;

juce::AudioBuffer<float> constant(int n, float level)
{
    juce::AudioBuffer<float> audio(2, n);
    for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < n; ++s) audio.setSample(ch, s, level);
    return audio;
}
// Quarter-note synced, anchored, saw-up volume curve with no smoothing; 120 BPM at 48 kHz = 24000 samples per cycle.
void sawVolume(L& lfo)
{
    lfo.prepare(48000, 4800);
    lfo.setParameter(L::Smooth, 0);
    lfo.setGraph(0, Graph::preset(3));
}
double rms(const juce::AudioBuffer<float>& a, int from, int count) { return a.getRMSLevel(0, from, count); }
}

TEST_CASE("curve graph: segments, tension, vertical steps and presets")
{
    Graph g = Graph::preset(3);
    CHECK_NEAR(g.eval(0.25f), 0.25, 1e-6); CHECK_NEAR(g.eval(1), 1, 1e-6);
    g.curve[0] = 0.5f; CHECK(g.eval(0.5f) < 0.2f);   // ease-in starts slowly
    g.curve[0] = -0.5f; CHECK(g.eval(0.5f) > 0.8f);  // ease-out starts quickly
    const Graph square = Graph::preset(5);
    CHECK_NEAR(square.eval(0.49f), 1, 1e-6); CHECK_NEAR(square.eval(0.51f), 0, 1e-6);
    for (int kind = 0; kind < Graph::kShapeCount; ++kind)
    {
        const Graph preset = Graph::preset(kind);
        CHECK(preset.count >= 2); CHECK(preset.count <= Graph::kMaxPoints);
        CHECK_NEAR(preset.x[0], 0, 0); CHECK_NEAR(preset.x[preset.count - 1], 1, 1e-6);
        for (int i = 1; i < preset.count; ++i) CHECK(preset.x[i] >= preset.x[i - 1]);
    }
    Graph torn = Graph::preset(3); torn.x[1] = -3; torn.count = 99; // half-written edits never produce NaN or out-of-range values
    for (float p = 0; p <= 1; p += 0.05f) { const float v = torn.eval(p); CHECK(std::isfinite(v)); CHECK(v >= 0); CHECK(v <= 1); }
}

TEST_CASE("curve LFO: anchored synced volume follows the graph and is block-size invariant")
{
    L whole, split; sawVolume(whole); sawVolume(split);
    juce::MidiBuffer midi;
    auto a = constant(4800, 1), b = constant(4800, 1);
    whole.setHostInfo({120, 0, true, 48000}); whole.process(a, midi);
    for (int from = 0; from < 4800; from += 96)
    {
        juce::AudioBuffer<float> block(b.getArrayOfWritePointers(), 2, from, 96);
        split.setHostInfo({120, (double) from / 24000, true, 48000}); split.process(block, midi);
    }
    CHECK_NEAR(a.getSample(0, 0), 0, 1e-6); CHECK_NEAR(a.getSample(0, 1200), 0.05, 1e-4);
    for (int s = 0; s < 4800; ++s) { CHECK_NEAR(a.getSample(0, s), b.getSample(0, s), 1e-5); CHECK_NEAR(a.getSample(1, s), a.getSample(0, s), 1e-7); }
    // Half way through the quarter note the saw is at one half.
    whole.setHostInfo({120, 0.5, true, 48000}); auto c = constant(64, 1); whole.process(c, midi); CHECK_NEAR(c.getSample(0, 0), 0.5, 1e-4);
    // Depth 0, Amount 0 and bypass are exact pass-throughs.
    whole.setParameter(L::Depth, 0); c = constant(200, 0.7f); whole.process(c, midi); CHECK_NEAR(c.getSample(0, 100), 0.7, 1e-6);
    whole.setParameter(L::Depth, 1); whole.setParameter(L::amountParam(L::Vol), 0); c = constant(200, 0.7f); whole.process(c, midi); CHECK_NEAR(c.getSample(0, 100), 0.7, 1e-6);
    whole.setParameter(L::amountParam(L::Vol), 1); whole.setBypassed(true); c = constant(200, 0.7f); whole.process(c, midi); CHECK_NEAR(c.getSample(0, 100), 0.7, 1e-6);
}

TEST_CASE("curve LFO: warp, phase, PWM, swing, rate variants and offset move the playhead as documented")
{
    L lfo; sawVolume(lfo); juce::MidiBuffer midi;
    lfo.setHostInfo({120, 0.25, true, 48000});   // a quarter of the cycle
    auto audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.25, 1e-4);
    lfo.setParameter(L::warpParam(0), 4);        // 2x: the saw plays twice per cycle
    audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.5, 1e-4);
    lfo.setParameter(L::warpParam(0), 3);
    lfo.setParameter(L::Phase, 0.25f);           // shifts the graph a quarter cycle
    audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.5, 1e-4);
    lfo.setParameter(L::Phase, 0);
    lfo.setParameter(L::Pwm, 1);                 // squeezes the first half: the midpoint moves to 0.95
    lfo.setHostInfo({120, 0.5, true, 48000}); audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.5 * 0.5 / 0.95, 1e-3);
    lfo.setParameter(L::Pwm, 0);
    lfo.setParameter(L::Swing, 0.25f);           // first cycle of each pair takes 75% of the time
    lfo.setHostInfo({120, 0.75, true, 48000}); audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.75 / 1.5, 1e-3);
    lfo.setParameter(L::Swing, 0);
    // Rate list: 1/4 dotted only counts when the dotted switch is on.
    CHECK_NEAR(L::rateBeats(15, false, false), 1, 0); CHECK_NEAR(L::rateBeats(16, false, false), 1, 0);
    CHECK_NEAR(L::rateBeats(16, true, false), 1.5, 1e-9); CHECK_NEAR(L::rateBeats(17, false, true), 2.0 / 3, 1e-9);
    // Offset of 1200 samples runs the LFO 1200 samples (0.05 cycle) late.
    lfo.setParameter(L::Offset, 1200); lfo.setHostInfo({120, 0.25, true, 48000});
    audio = constant(8, 1); lfo.process(audio, midi); CHECK_NEAR(audio.getSample(0, 0), 0.30, 1e-4);
}

TEST_CASE("curve LFO: pan, graph sources and free-running Hz timing")
{
    L lfo; lfo.prepare(1000, 1000); lfo.setParameter(L::Smooth, 0);
    Graph high = Graph::preset(3); high.y[0] = high.y[1] = 1; lfo.setGraph(4, high);   // constant 1
    lfo.setParameter(L::amountParam(L::Vol), 0);
    lfo.setParameter(L::sourceParam(L::Pan), 4); lfo.setParameter(L::amountParam(L::Pan), 1);
    juce::MidiBuffer midi; auto audio = constant(32, 1); lfo.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 16), 0, 1e-6); CHECK_NEAR(audio.getSample(1, 16), 1, 1e-6); // hard right
    lfo.setParameter(L::amountParam(L::Pan), -1); audio = constant(32, 1); lfo.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 16), 1, 1e-6); CHECK_NEAR(audio.getSample(1, 16), 0, 1e-6);
    lfo.setParameter(L::amountParam(L::Pan), 0);
    L hz; hz.prepare(8000, 8000); hz.setParameter(L::Smooth, 0); hz.setParameter(L::Sync, 0); hz.setParameter(L::Hertz, 2);
    hz.setHostInfo({120, 0, true, 8000}); // the LFO only runs while the song plays
    hz.setGraph(0, Graph::preset(3)); audio = constant(6000, 1); hz.process(audio, midi); // 2 Hz at 8 kHz = 4000 samples per cycle
    CHECK_NEAR(audio.getSample(0, 1000), 0.25, 1e-3); CHECK_NEAR(audio.getSample(0, 3000), 0.75, 1e-3); CHECK_NEAR(audio.getSample(0, 5000), 0.25, 1e-3);
}

TEST_CASE("curve LFO: MIDI notes pick graphs, retrigger, run an envelope and gate")
{
    auto freeSaw = [](L& lfo, float hertz) { // 10 Hz = 4800 samples; the song plays
        sawVolume(lfo); lfo.setParameter(L::Sync, 0); lfo.setParameter(L::Hertz, hertz); lfo.setHostInfo({120, 0, true, 48000});
    };
    L select; freeSaw(select, 10);
    select.setGraph(1, Graph::preset(4));                                                   // falling saw on graph 2
    juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1, 24 + 1, (juce::uint8) 100), 0); // octave 2 = Vol, key 1 = graph 2
    auto audio = constant(100, 1); select.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 0), 1, 1e-4); CHECK_NEAR(audio.getSample(0, 99), 1 - 99.0 / 4800, 1e-3);
    // Without the note the same position follows the rising graph 1.
    L plain; freeSaw(plain, 10); midi.clear(); audio = constant(100, 1); plain.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 99), 99.0 / 4800, 1e-3);
    // Retrigger: free-running until the note, then the cycle restarts at the note's sample.
    L retrig; freeSaw(retrig, 10); retrig.setParameter(L::NoteRetrig, 1);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 500);
    audio = constant(3000, 1); retrig.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 499), 499.0 / 4800, 1e-3); CHECK_NEAR(audio.getSample(0, 500), 0, 1e-4);
    CHECK_NEAR(audio.getSample(0, 2900), 0.5, 0.01);
    // Envelope mode idles at the end of the graph, runs once from the note, then holds there.
    L env; freeSaw(env, 10); env.setParameter(L::NoteRetrig, 2);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 100), 1000);
    audio = constant(7000, 1); env.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 500), 1, 1e-6); CHECK_NEAR(audio.getSample(0, 1000), 0, 1e-4);
    CHECK_NEAR(audio.getSample(0, 1000 + 2400), 0.5, 0.01); CHECK_NEAR(audio.getSample(0, 6500), 1, 1e-6);
    // Gate: the LFO stops affecting the audio after the last note-off.
    L gate; freeSaw(gate, 1); gate.setParameter(L::NoteGate, 1);
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0); midi.addEvent(juce::MidiMessage::noteOff(1, 64), 2000);
    audio = constant(6000, 1); gate.process(audio, midi);
    CHECK(audio.getSample(0, 1000) < 0.1f); CHECK_NEAR(audio.getSample(0, 5000), 1, 1e-3);
    // Note -> Cutoff and Vel -> PWM disable graph selection by notes below middle C.
    L override; freeSaw(override, 10); override.setParameter(L::NoteCutoff, 1);
    override.setGraph(1, Graph::preset(4)); midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 25, (juce::uint8) 100), 0);
    audio = constant(100, 1); override.process(audio, midi); CHECK_NEAR(audio.getSample(0, 99), 99.0 / 4800, 1e-3);
}

TEST_CASE("curve LFO: a stopped transport freezes the LFO unless notes hold it; the scope follows the cycle")
{
    // Free-running 10 Hz saw: stopped, the level stays where it was; playing, it moves.
    L lfo; sawVolume(lfo); lfo.setParameter(L::Sync, 0); lfo.setParameter(L::Hertz, 10);
    juce::MidiBuffer midi;
    lfo.setHostInfo({120, 0, true, 48000}); auto audio = constant(1200, 1); lfo.process(audio, midi);
    CHECK_NEAR(audio.getSample(0, 1199), 1199.0 / 4800, 1e-3); CHECK(lfo.displayRunning());
    lfo.setHostInfo({120, 0, false, 48000});
    const auto revision = lfo.displayRevision();
    for (int i = 0; i < 3; ++i) { audio = constant(1200, 1); lfo.process(audio, midi); }
    CHECK_NEAR(audio.getSample(0, 0), 1200.0 / 4800, 1e-3); CHECK_NEAR(audio.getSample(0, 1199), 1200.0 / 4800, 1e-3);
    CHECK_NEAR(lfo.displayPhase(), 0.25, 1e-3); CHECK(! lfo.displayRunning());
    audio = constant(1200, 1); lfo.process(audio, midi);
    CHECK(lfo.displayRevision() - revision <= 1u); // nothing to redraw while frozen
    // A held note runs it while stopped; the note-off freezes it again.
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 100), 0); midi.addEvent(juce::MidiMessage::noteOff(1, 64), 600);
    audio = constant(1200, 1); lfo.process(audio, midi); midi.clear();
    CHECK_NEAR(audio.getSample(0, 599), (1200.0 + 599) / 4800, 1e-3); CHECK_NEAR(audio.getSample(0, 1199), (1200.0 + 600) / 4800, 1e-3);
    // Anchored sync while stopped: parked at the song position the transport reports.
    L anchored; sawVolume(anchored); anchored.setHostInfo({120, 0.5, false, 48000});
    for (int i = 0; i < 2; ++i) { audio = constant(64, 1); anchored.process(audio, midi); }
    CHECK_NEAR(audio.getSample(0, 63), 0.5, 1e-4);

    // Scope: input and output peaks per slice of the cycle; the saw volume shapes the output envelope.
    L scope; sawVolume(scope); scope.setHostInfo({120, 0, true, 48000});
    juce::AudioBuffer<float> tone(2, 24000);
    for (int s = 0; s < 24000; ++s) for (int ch = 0; ch < 2; ++ch) tone.setSample(ch, s, 0.8f * (float) std::sin(s * 0.05));
    scope.process(tone, midi);
    const int quarter = L::kScopeBins / 4;
    CHECK_NEAR(scope.scopeInput(quarter), 0.8, 0.02); CHECK_NEAR(scope.scopeInput(3 * quarter), 0.8, 0.02);
    CHECK_NEAR(scope.scopeOutput(quarter), 0.8 * 0.25, 0.02); CHECK_NEAR(scope.scopeOutput(3 * quarter), 0.8 * 0.75, 0.02);
}

TEST_CASE("curve LFO: cutoff curve emits bounded CC output and the filter module shapes the audio")
{
    L lfo; lfo.prepare(48000, 4800); lfo.setParameter(L::Smooth, 0);
    lfo.setGraph(0, Graph::preset(3)); lfo.setParameter(L::amountParam(L::Cut), 1); lfo.setParameter(L::amountParam(L::Vol), 0);
    lfo.setParameter(L::Cutoff, 0.5f); lfo.setParameter(L::CcOut, 2);                // CC 1
    lfo.setHostInfo({120, 0, true, 48000}); juce::MidiBuffer midi; auto audio = constant(4800, 0);
    lfo.process(audio, midi);
    int count = 0, last = -1; bool rising = true;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage(); REQUIRE(m.isController()); CHECK(m.getControllerNumber() == 1);
        CHECK(m.getControllerValue() >= 0); CHECK(m.getControllerValue() <= 127);
        if (count == 0) CHECK(meta.samplePosition == 0);
        if (m.getControllerValue() < last) rising = false;
        last = m.getControllerValue(); ++count;
    }
    CHECK(count > 3); CHECK(count <= 32); CHECK(rising);
    lfo.setParameter(L::CcOut, 0); midi.clear(); audio = constant(256, 0); lfo.process(audio, midi); CHECK(midi.getNumEvents() == 0);
    // Filter: a 10 kHz tone is removed by a low filter at ~80 Hz and kept by a high filter.
    auto tone = [](int n) { juce::AudioBuffer<float> a(2, n); for (int s = 0; s < n; ++s) for (int ch = 0; ch < 2; ++ch) a.setSample(ch, s, (float) std::sin(2 * std::numbers::pi * 10000.0 * s / 48000)); return a; };
    for (const auto& [type, passes] : {std::pair{0, false}, std::pair{3, false}, std::pair{5, true}, std::pair{7, true}})
    {
        L filter; filter.prepare(48000, 4800); filter.setParameter(L::amountParam(L::Vol), 0); filter.setParameter(L::Smooth, 0);
        filter.setParameter(L::FilterOn, 1); filter.setParameter(L::FilterType, (float) type); filter.setParameter(L::Cutoff, 0.2f);
        filter.setParameter(L::Resonance, 0.1f); juce::MidiBuffer none; auto input = tone(4800); filter.process(input, none);
        if (passes) CHECK(rms(input, 2400, 2000) > 0.5); else CHECK(rms(input, 2400, 2000) < 0.05);
    }
    // Every type stays finite and bounded on noise at extreme resonance and cutoff.
    juce::Random random(7);
    for (int type = 0; type < devices::CurveFilter::TypeCount; ++type)
        for (float cutoff : {0.0f, 0.5f, 1.0f})
        {
            L filter; filter.prepare(48000, 4800); filter.setParameter(L::amountParam(L::Vol), 0);
            filter.setParameter(L::FilterOn, 1); filter.setParameter(L::FilterType, (float) type); filter.setParameter(L::Cutoff, cutoff);
            filter.setParameter(L::Resonance, 1); filter.setParameter(L::FilterVar, 1);
            juce::AudioBuffer<float> noise(2, 4800); for (int ch = 0; ch < 2; ++ch) for (int s = 0; s < 4800; ++s) noise.setSample(ch, s, random.nextFloat() * 2 - 1);
            juce::MidiBuffer none; filter.process(noise, none);
            for (int s = 0; s < 4800; ++s) { REQUIRE(std::isfinite(noise.getSample(0, s))); CHECK(std::abs(noise.getSample(0, s)) < 60); }
        }
}

TEST_CASE("curve LFO: graphs, routing and selection survive state capture, .als save and reload")
{
    auto model = devices::nativeDeviceModel("SonoraCurveLfo");
    CHECK(model.displayName == "Sonora Curve LFO");
    devices::BuildContext context;
    auto processor = devices::createDevice(model, context); auto* lfo = dynamic_cast<L*>(processor.get()); REQUIRE(lfo);
    Graph custom = Graph::preset(6); custom.curve[0] = 0.4f; lfo->setGraph(7, custom);
    lfo->setParameter(L::sourceParam(L::Pan), 7); lfo->setParameter(L::amountParam(L::Pan), -0.5f);
    lfo->setParameter(L::warpParam(7), 5); lfo->setParameter(L::FilterType, (float) devices::CurveFilter::Comb);
    devices::captureDeviceState(model, processor.get());

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_curve_lfo_tests");
    dir.createDirectory();
    const auto source = dir.getChildFile("Source.als"), destination = dir.getChildFile("Curve LFO.als");
    io::NewSetOptions options; options.midiTracks = 0; options.audioTracks = 1; options.returnTracks = 0;
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(options), source, error));
    auto loaded = io::loadAls(source); REQUIRE(loaded); auto& song = *loaded.song;
    model::allocateDeviceIds(song, model);
    model.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(model));
    model::EditHistory history; history.perform(song, model::insertDevice(0, model));
    REQUIRE(io::saveAls(song, destination, io::AlsSaveOptions{}).ok);
    auto again = io::loadAls(destination); REQUIRE(again);
    REQUIRE(again.song->tracks[0].devices.size() == 1);
    const auto& reloaded = again.song->tracks[0].devices[0];
    CHECK(reloaded.type == "SonoraCurveLfo");
    auto restored = devices::createDevice(reloaded, context); auto* back = dynamic_cast<L*>(restored.get()); REQUIRE(back);
    const Graph g = back->graph(7);
    CHECK(g.count == custom.count); CHECK_NEAR(g.curve[0], 0.4, 1e-6);
    for (int i = 0; i < custom.count; ++i) { CHECK_NEAR(g.x[i], custom.x[i], 1e-6); CHECK_NEAR(g.y[i], custom.y[i], 1e-6); }
    CHECK_NEAR(back->getParameter(L::sourceParam(L::Pan)), 7, 0); CHECK_NEAR(back->getParameter(L::amountParam(L::Pan)), -0.5, 1e-6);
    CHECK_NEAR(back->getParameter(L::warpParam(7)), 5, 0); CHECK_NEAR(back->getParameter(L::FilterType), devices::CurveFilter::Comb, 0);
}
