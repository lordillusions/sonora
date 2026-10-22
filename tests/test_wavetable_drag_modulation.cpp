#include "TestFramework.h"
#include "devices/native/WavetableModRouting.h"
#include "devices/native/WavetableSynth.h"
#include "plugins/SerumLibrary.h"
#include "plugins/SerumPatch.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <numbers>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
namespace R = devices::wavetable_routing;

float render(W& synth, int samples, std::vector<float>* out = nullptr)
{
    juce::AudioBuffer<float> audio(2, samples); audio.clear();
    juce::MidiBuffer midi; synth.process(audio, midi);
    float peak = 0;
    for (int i = 0; i < samples; ++i)
    {
        peak = std::max(peak, std::abs(audio.getSample(0, i)));
        if (out) out->push_back(audio.getSample(0, i));
    }
    return peak;
}
} // namespace

TEST_CASE("wavetable drag modulation: controls map to matrix destinations; non-targets refuse")
{
    CHECK(R::destinationForParameter(W::oscParam(0, W::Position)) == W::Osc1Position);
    CHECK(R::destinationForParameter(W::oscParam(2, W::Level)) == W::Osc3Level);
    CHECK(R::destinationForParameter(W::oscParam(1, W::Semi)) == W::Osc2Pitch);
    CHECK(R::destinationForParameter(W::oscParam(1, W::Fine)) == W::Osc2Pitch);
    CHECK(R::destinationForParameter(W::coarseParam(1)) == W::Osc2Pitch);
    CHECK(R::destinationForParameter(W::oscParam(0, W::Warp2Amount)) == W::Osc1Warp2);
    CHECK(R::destinationForParameter(W::filterParam(1, W::Cutoff)) == W::Filter2Cutoff);
    CHECK(R::destinationForParameter(W::filterParam(0, W::Resonance)) == W::Filter1Resonance);
    CHECK(R::destinationForParameter(W::envParam(2, W::Sustain)) == W::Env3Sustain);
    CHECK(R::destinationForParameter(W::Volume) == W::Amplitude);
    CHECK(R::destinationForParameter(W::sourceRoutingParam(3, W::SourceBus2)) == W::sourceSendDestination(3, 1));
    for (int parameter : {W::oscParam(0, W::Unison), W::oscParam(0, W::Table), W::modParam(0, W::ModAmount), W::macroParam(0), W::arpParam(W::ArpGate)})
        CHECK(R::destinationForParameter(parameter) == W::DestOff);
    CHECK(R::isModulationSource(W::Lfo1)); CHECK(! R::isModulationSource(W::SourceOff)); CHECK(! R::isModulationSource(W::NumSources));

    W synth;
    // Travel per unit amount follows the editor knob taper: 8 octaves over the 20 Hz..20 kHz log knob, ±1 pan = half the travel.
    CHECK_NEAR(R::travelPerAmount(synth, W::filterParam(0, W::Cutoff)), 8 / std::log2(1000.0f), 1e-5);
    CHECK_NEAR(R::travelPerAmount(synth, W::oscParam(0, W::Position)), 1, 1e-6);
    CHECK_NEAR(R::travelPerAmount(synth, W::oscParam(0, W::OscPan)), 0.5, 1e-6);
    CHECK_NEAR(R::travelPerAmount(synth, W::oscParam(0, W::Semi)), 1, 1e-6);
    CHECK_NEAR(R::travelPerAmount(synth, W::oscParam(0, W::Octave)), 0.25, 1e-6);
    CHECK_NEAR(R::travelPerAmount(synth, W::oscParam(0, W::Unison)), 0, 0);
    CHECK_NEAR(R::defaultAmount(synth, W::filterParam(0, W::Cutoff)) * R::travelPerAmount(synth, W::filterParam(0, W::Cutoff)), 0.35, 1e-5);
}

TEST_CASE("wavetable drag modulation: connect reuses or fills slots, rescales and disconnects cleanly")
{
    W synth;
    int slot = -1;
    const int position = W::oscParam(0, W::Position), cutoff = W::filterParam(0, W::Cutoff);
    R::apply(synth, R::connect(synth, W::Lfo1, position, 0.4f, slot));
    CHECK(slot == 0);
    auto routes = R::routesForParameter(synth, position);
    REQUIRE(routes.size() == 1);
    CHECK(routes[0].slot == 0); CHECK(routes[0].source == W::Lfo1); CHECK(routes[0].destination == W::Osc1Position);
    CHECK_NEAR(routes[0].amount, 0.4, 1e-6);
    CHECK(! routes[0].bipolar); // Serum convention: a dropped LFO starts unipolar (Forward)
    CHECK_NEAR(synth.getParameter(W::lfoParam(0, W::Bipolar)), 0, 0);
    CHECK_NEAR(synth.getParameter(W::outParam(0)), 1, 0);

    // The same source on the same destination (another control of that oscillator's pitch included) updates the slot in place.
    R::apply(synth, R::connect(synth, W::Lfo1, position, -0.2f, slot));
    CHECK(slot == 0); CHECK(R::routesForParameter(synth, position).size() == 1);
    CHECK_NEAR(synth.getParameter(W::modParam(0, W::ModAmount)), -0.2, 1e-6);

    // A half-configured slot (source only) is not free; the next connect skips it.
    synth.setParameter(W::modParam(1, W::ModSource), (float) W::Velocity);
    R::apply(synth, R::connect(synth, W::Env2, cutoff, 0.5f, slot));
    CHECK(slot == 2);
    routes = R::routesForParameter(synth, cutoff);
    REQUIRE(routes.size() == 1); CHECK(routes[0].source == W::Env2); CHECK(! routes[0].bipolar);
    R::apply(synth, R::connect(synth, W::Macro3, cutoff, 0.25f, slot));
    CHECK(slot == 3); CHECK(R::routesForParameter(synth, cutoff).size() == 2);

    R::apply(synth, R::setAmount(3, 0.75f));
    CHECK_NEAR(R::routesForParameter(synth, cutoff)[1].amount, 0.75, 1e-6);
    R::apply(synth, R::setAmount(3, 3.0f));
    CHECK_NEAR(synth.getParameter(W::modParam(3, W::ModAmount)), 1, 0); // clamped to the slot's range

    R::apply(synth, R::disconnect(2));
    routes = R::routesForParameter(synth, cutoff);
    REQUIRE(routes.size() == 1); CHECK(routes[0].source == W::Macro3);
    CHECK(R::freeSlot(synth) == 2);
    CHECK_NEAR(synth.getParameter(W::modParam(2, W::ModSource)), W::SourceOff, 0);
    CHECK_NEAR(synth.getParameter(W::modParam(2, W::ModDestination)), W::DestOff, 0);

    // Refusals: a control that is not a destination, an invalid source, a full matrix.
    CHECK(R::connect(synth, W::Lfo1, W::oscParam(0, W::Unison), 0.5f, slot).empty()); CHECK(slot == -1);
    CHECK(R::connect(synth, W::SourceOff, position, 0.5f, slot).empty()); CHECK(slot == -1);
    for (int s = 0; s < W::kModSlots; ++s) synth.setParameter(W::modParam(s, W::ModSource), (float) W::Velocity);
    CHECK(R::connect(synth, W::Lfo2, W::oscParam(1, W::Level), 0.5f, slot).empty()); CHECK(slot == -1);
}

TEST_CASE("wavetable drag modulation: a dropped route changes the sound; envelope stages modulate in own patches")
{
    const auto prepared = [](W& synth) {
        synth.prepare(48000, 512);
        synth.setParameter(W::filterParam(0, W::FilterOn), 0);
        synth.setParameter(W::oscParam(0, W::Position), 0.5f); // Default Shapes square
        synth.queueUiMidi(0x90, 60, 100);
    };
    W plain, muted;
    prepared(plain); prepared(muted);
    int slot = -1;
    muted.setParameter(W::macroParam(0), 1);
    R::apply(muted, R::connect(muted, W::Macro1, W::oscParam(0, W::Level), -1, slot));
    const float loud = render(plain, 4800), quiet = render(muted, 4800);
    CHECK(loud > 0.05f);
    CHECK(quiet < loud * 0.05f);

    // Fixed -> Env 1 Attack lengthens the attack along the knob's fifth-power taper (previously ignored in own patches).
    W fast, slow;
    prepared(fast); prepared(slow);
    R::apply(slow, R::connect(slow, W::Fixed, W::envParam(0, W::Attack), 0.5f, slot));
    std::vector<float> a, b;
    render(fast, 2400, &a); render(slow, 2400, &b);
    float early = 0, earlySlow = 0;
    for (size_t i = 0; i < 1200; ++i) { early = std::max(early, std::abs(a[i])); earlySlow = std::max(earlySlow, std::abs(b[i])); }
    CHECK(early > 0.05f);
    CHECK(earlySlow < early * 0.2f);
}

TEST_CASE("wavetable UI MIDI: the editor queue plays notes, wheels and bends without an armed track")
{
    W synth; synth.prepare(48000, 256);
    CHECK(synth.queueUiMidi(0x90, 60, 100));
    CHECK(render(synth, 256) > 0.0f);
    CHECK(synth.activeVoiceCount() == 1);
    CHECK(synth.noteLit(60)); CHECK(! synth.noteLit(61));
    CHECK(synth.queueUiMidi(0xb0, 1, 127)); CHECK(synth.queueUiMidi(0xe0, 127, 127));
    render(synth, 256);
    CHECK_NEAR(synth.modWheelDisplay(), 1, 1e-6);
    CHECK(synth.pitchBendDisplay() > 0.99f);
    CHECK(synth.queueUiMidi(0x80, 60, 0));
    render(synth, 256);
    CHECK(! synth.noteLit(60));
    // A full queue refuses rather than blocking or overwriting.
    int accepted = 0;
    for (int i = 0; i < 400; ++i) accepted += synth.queueUiMidi(0xb0, 1, (uint8_t) (i & 127)) ? 1 : 0;
    CHECK(accepted == 256);
    render(synth, 64);
    CHECK(synth.queueUiMidi(0xb0, 1, 0));
}

TEST_CASE("wavetable arpeggiator: held keys replay in pattern order at the tempo-synced rate")
{
    const auto sequence = [](int mode, int octaves, float gate, std::vector<int> chord, int steps) {
        W synth; synth.prepare(48000, 512);
        devices::HostInfo host; host.tempo = 120; synth.setHostInfo(host);
        synth.setParameter(W::arpParam(W::ArpOn), 1); synth.setParameter(W::arpParam(W::ArpMode), (float) mode);
        synth.setParameter(W::arpParam(W::ArpRate), 3); // 1/16 at 120 BPM = 6000 samples
        synth.setParameter(W::arpParam(W::ArpOctaves), (float) octaves); synth.setParameter(W::arpParam(W::ArpGate), gate);
        for (int note : chord) synth.queueUiMidi(0x90, (uint8_t) note, 90);
        std::vector<int> played; int last = -2, silentBlocks = 0;
        while ((int) played.size() < steps)
        {
            render(synth, 500);
            const int note = synth.arpNoteDisplay();
            if (note < 0) ++silentBlocks;
            if (note >= 0 && note != last) played.push_back(note);
            last = note;
        }
        return std::pair(played, silentBlocks);
    };
    CHECK(sequence(W::ArpUp, 1, 1, {64, 60, 67}, 6).first == (std::vector<int>{60, 64, 67, 60, 64, 67}));
    CHECK(sequence(W::ArpDown, 1, 1, {60, 64, 67}, 4).first == (std::vector<int>{67, 64, 60, 67}));
    CHECK(sequence(W::ArpUpDown, 1, 1, {60, 64, 67}, 6).first == (std::vector<int>{60, 64, 67, 64, 60, 64}));
    CHECK(sequence(W::ArpAsPlayed, 2, 1, {67, 60}, 5).first == (std::vector<int>{67, 60, 79, 72, 67}));
    CHECK(sequence(W::ArpUp, 2, 1, {60, 64}, 5).first == (std::vector<int>{60, 64, 72, 76, 60}));
    const auto [gated, gaps] = sequence(W::ArpUp, 1, 0.25f, {60}, 3);
    CHECK(gated.size() == 3); CHECK(gaps >= 10); // 1/4 gate: silent for most of each 6000-sample step

    // Step timing: the second step starts 6000 samples after the first.
    W synth; synth.prepare(48000, 512);
    synth.setParameter(W::arpParam(W::ArpOn), 1); synth.setParameter(W::arpParam(W::ArpRate), 3);
    synth.queueUiMidi(0x90, 60, 90); synth.queueUiMidi(0x90, 72, 90);
    int samples = 0;
    while (synth.arpNoteDisplay() != 72 && samples < 20000) { render(synth, 100); samples += 100; }
    CHECK(samples >= 6000); CHECK(samples <= 6100);
    // Releasing every key ends the sounding note; switching the arp off stops it.
    synth.queueUiMidi(0x80, 60, 0); synth.queueUiMidi(0x80, 72, 0);
    render(synth, 64);
    CHECK(synth.arpNoteDisplay() == -1);
    synth.queueUiMidi(0x90, 62, 90); render(synth, 64); CHECK(synth.arpNoteDisplay() == 62);
    synth.setParameter(W::arpParam(W::ArpOn), 0); render(synth, 64);
    CHECK(synth.arpNoteDisplay() == -1);
}

TEST_CASE("wavetable Default Shapes: nine stepped frames from the elementary formulas (and the installed table when present)")
{
    const auto table = W::defaultShapesTable();
    REQUIRE(table);
    CHECK(table->frames() == 9); CHECK(table->frameLength() == 2048); CHECK(table->channels() == 1);
    CHECK(table->frameLayout() == devices::WavetableFrameLayout::Steps);
    const auto& samples = table->originalSamples();
    const auto at = [&](int frame, int i) { return samples[(size_t) (frame * 2048 + i)]; };
    CHECK_NEAR(at(0, 512), 0.5, 1e-6); CHECK_NEAR(at(0, 1023), 1023.0 / 1024, 1e-6); CHECK_NEAR(at(0, 1024), -1, 0);   // saw
    CHECK_NEAR(at(1, 512), 1, 1e-6);                                                                                  // sine
    CHECK_NEAR(at(2, 512), 1, 1e-6); CHECK_NEAR(at(2, 1536), -1, 1e-6);                                               // triangle
    CHECK_NEAR(at(3, 0), 1, 0); CHECK_NEAR(at(3, 1024), -1, 0);                                                       // split ramp
    CHECK_NEAR(at(4, 1023), 1, 0); CHECK_NEAR(at(4, 1024), -1, 0);                                                    // square
    for (int frame = 5; frame < 9; ++frame)                                                                           // pulses
    {
        const int width = 2048 >> (frame - 3);
        CHECK_NEAR(at(frame, 1024 - width - 1), -1, 0); CHECK_NEAR(at(frame, 1024 - width), 1, 0); CHECK_NEAR(at(frame, 1024), -1, 0);
    }

    // A new oscillator starts on the saw and steps through whole frames.
    W synth;
    CHECK(std::string(W::tableName(W::Basic)) == "Default Shapes");
    CHECK(synth.tableFrameCount(0) == 9);
    CHECK_NEAR(synth.tableFramePosition(0), 0, 0);
    CHECK_NEAR(synth.waveformAt(0, 0.25f), 0.5, 0.02);
    synth.setParameter(W::oscParam(0, W::Position), 0.5f); CHECK_NEAR(synth.tableFramePosition(0), 4, 0);
    CHECK_NEAR(synth.waveformAt(0, 0.25f), 1, 0.08); CHECK_NEAR(synth.tableFrameAt(0, 4, 0.75f), -1, 0);
    synth.setParameter(W::oscParam(0, W::Position), 1); CHECK_NEAR(synth.tableFramePosition(0), 8, 0);
    synth.setParameter(W::oscParam(0, W::Table), W::Harmonic); CHECK(synth.tableFrameCount(0) == 10);

    // Local compatibility check against the user's installed table, read here only; nothing from it is stored.
    const juce::File installed("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Tables/S2 Tables/Default Shapes.wav");
    if (! installed.existsAsFile()) return;
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(installed));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->lengthInSamples == 9 * 2048);
    juce::AudioBuffer<float> file(1, 9 * 2048);
    REQUIRE(reader->read(&file, 0, 9 * 2048, 0, true, false));
    float error = 0;
    for (int i = 0; i < 9 * 2048; ++i) error = std::max(error, std::abs(file.getSample(0, i) - samples[(size_t) i]));
    CHECK(error < 1e-6f);
}

TEST_CASE("wavetable live state: the newest voice publishes route values, envelope stages, LFO phase and output peaks")
{
    W synth; synth.prepare(48000, 512);
    int slot = -1;
    R::apply(synth, R::connect(synth, W::Lfo1, W::oscParam(0, W::Position), 0.5f, slot));
    synth.setParameter(W::lfoParam(0, W::SyncOn), 0); synth.setParameter(W::lfoParam(0, W::Rate), 4);
    render(synth, 512);
    auto live = synth.liveState();
    CHECK(! live.active); CHECK(live.note == -1); CHECK_NEAR(live.route[(size_t) slot], 0, 0); CHECK(live.envelopeStage[0] == 0);
    synth.queueUiMidi(0x90, 64, 100);
    render(synth, 512);
    live = synth.liveState();
    CHECK(live.active); CHECK(live.note == 64);
    CHECK(live.envelopeStage[0] >= 2); CHECK(live.envelopeLevel[0] > 0);
    CHECK(live.outputPeak[0] > 0.01f);
    std::vector<float> routeValues, phases;
    for (int block = 0; block < 12; ++block)
    {
        render(synth, 512); live = synth.liveState();
        routeValues.push_back(live.route[(size_t) slot]); phases.push_back(live.lfoPhase[0]);
        for (size_t other = 0; other < live.route.size(); ++other) if ((int) other != slot) CHECK_NEAR(live.route[other], 0, 0);
    }
    CHECK(*std::max_element(routeValues.begin(), routeValues.end()) - *std::min_element(routeValues.begin(), routeValues.end()) > 0.2f); // 4 Hz LFO moves
    CHECK(std::abs(phases.back() - phases.front()) > 1e-6f);
    for (int block = 0; block < 60; ++block) render(synth, 512);
    CHECK(synth.liveState().envelopeStage[0] == 5); // sustaining
    synth.queueUiMidi(0x80, 64, 0);
    render(synth, 512); CHECK(synth.liveState().envelopeStage[0] == 6);
    for (int block = 0; block < 100; ++block) render(synth, 512);
    live = synth.liveState();
    CHECK(! live.active); CHECK_NEAR(live.route[(size_t) slot], 0, 0);
    // Modulated preview helpers: an explicit position selects its frame; warps apply to the preview.
    CHECK_NEAR(synth.tableFramePosition(0, 0.5f), 4, 0);
    CHECK_NEAR(synth.waveformAt(0, 0.25f, 0.5f, 0, 0), 1, 0.08);
    synth.setParameter(W::oscParam(0, W::Warp1Mode), W::Mirror);
    CHECK(std::abs(synth.waveformAt(0, 0.6f, 0.0f, 1, 0) - synth.waveformAt(0, 0.6f, 0.0f, 0, 0)) > 0.05f);
}

TEST_CASE("wavetable mod direction: forward, backward and both sides; a bipolar LFO converts without changing its other routes")
{
    W synth; synth.prepare(48000, 512);
    int first = -1, second = -1;
    // An existing route from a bipolar LFO (made in the matrix, not by a drop).
    synth.setParameter(W::modParam(0, W::ModSource), (float) W::Lfo2); synth.setParameter(W::modParam(0, W::ModDestination), (float) W::Filter1Cutoff);
    synth.setParameter(W::modParam(0, W::ModAmount), 0.3f); first = 0;
    CHECK(R::direction(synth, first) == R::Direction::Both);
    const auto before = synth.waveformAt(0, 0.1f); (void) before;
    R::apply(synth, R::connect(synth, W::Lfo2, W::oscParam(0, W::Position), 0.4f, second));
    CHECK(second == 1);
    CHECK_NEAR(synth.getParameter(W::lfoParam(1, W::Bipolar)), 0, 0);       // LFO 2 now unipolar
    CHECK(R::direction(synth, first) == R::Direction::Both);                 // ...and its earlier route still swings both ways
    CHECK(R::direction(synth, second) == R::Direction::Forward);
    R::apply(synth, R::setDirection(synth, second, R::Direction::Backward));
    CHECK(R::direction(synth, second) == R::Direction::Backward); CHECK_NEAR(synth.getParameter(W::modParam(second, W::ModAmount)), -0.4, 1e-6);
    R::apply(synth, R::setDirection(synth, second, R::Direction::Both));
    CHECK(R::direction(synth, second) == R::Direction::Both); CHECK_NEAR(synth.getParameter(W::modParam(second, W::ModAmount)), 0.4, 1e-6);
    R::apply(synth, R::setDirection(synth, second, R::Direction::Forward));
    CHECK(R::direction(synth, second) == R::Direction::Forward); CHECK_NEAR(synth.getParameter(W::modParam(second, W::ModBipolar)), 0, 0);

    // Spans in knob travel: forward [at, at+d], backward [at-d, at], both [at-d, at+d], clamped.
    const int position = W::oscParam(0, W::Position);
    R::Route route; route.slot = second; route.amount = 0.25f;
    auto sweep = R::span(synth, position, route, 0.5f); CHECK_NEAR(sweep.from, 0.5, 1e-6); CHECK_NEAR(sweep.to, 0.75, 1e-6);
    route.amount = -0.25f; sweep = R::span(synth, position, route, 0.5f); CHECK_NEAR(sweep.from, 0.25, 1e-6); CHECK_NEAR(sweep.to, 0.5, 1e-6);
    route.amount = 0.25f; route.bipolar = true; sweep = R::span(synth, position, route, 0.9f); CHECK_NEAR(sweep.from, 0.65, 1e-6); CHECK_NEAR(sweep.to, 1, 0);

    // Unipolar conversion is inaudible in the native domain: same render before and after for the earlier route.
    W a, b;
    for (W* s : {&a, &b})
    {
        s->prepare(48000, 512);
        s->setParameter(W::lfoParam(1, W::SyncOn), 0); s->setParameter(W::lfoParam(1, W::Rate), 3);
        s->setParameter(W::modParam(0, W::ModSource), (float) W::Lfo2); s->setParameter(W::modParam(0, W::ModDestination), (float) W::Filter1Cutoff);
        s->setParameter(W::modParam(0, W::ModAmount), 0.5f); s->setParameter(W::filterParam(0, W::Cutoff), 800);
        s->queueUiMidi(0x90, 48, 100);
    }
    R::apply(b, R::unipolarLfo(b, 1));
    std::vector<float> x, y; render(a, 9600, &x); render(b, 9600, &y);
    float error = 0; for (size_t i = 0; i < x.size(); ++i) error = std::max(error, std::abs(x[i] - y[i]));
    CHECK(error < 1e-5f);
}

TEST_CASE("wavetable velocity and note curves: identity by default, three-point shaping feeds the Velocity/Key Track sources")
{
    W synth; synth.prepare(48000, 512);
    for (float input : {0.0f, 0.3f, 0.77f, 1.0f})
    { CHECK_NEAR(synth.curveSourceAt(W::VelocityCurve, input), input, 1e-6); CHECK_NEAR(synth.curveSourceAt(W::NoteCurve, input), input, 1e-6); }
    synth.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveStart), 0.2f);
    synth.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidX), 0.25f);
    synth.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidY), 0.8f);
    synth.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveEnd), 1.0f);
    CHECK_NEAR(synth.curveSourceAt(W::VelocityCurve, 0), 0.2, 1e-6);
    CHECK_NEAR(synth.curveSourceAt(W::VelocityCurve, 0.125f), 0.5, 1e-5);
    CHECK_NEAR(synth.curveSourceAt(W::VelocityCurve, 0.25f), 0.8, 1e-5);
    CHECK_NEAR(synth.curveSourceAt(W::VelocityCurve, 0.625f), 0.9, 1e-5);
    // Velocity -> OSC A Level through the curve: a soft note (velocity 32) becomes as loud as the curve says.
    const auto peak = [](bool shaped) {
        W s; s.prepare(48000, 512); s.setParameter(W::filterParam(0, W::FilterOn), 0);
        s.setParameter(W::oscParam(0, W::Level), 0);
        int slot = -1; R::apply(s, R::connect(s, W::Velocity, W::oscParam(0, W::Level), 1, slot));
        if (shaped) { s.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidX), 0.25f); s.setParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidY), 1); }
        s.queueUiMidi(0x90, 60, 32);
        return render(s, 4800);
    };
    CHECK(peak(true) > peak(false) * 2);
}

TEST_CASE("serum library: installed presets and tables are found at runtime; tables keep the reference frame law")
{
    namespace P = plugins;
    const auto tables = P::serumLibrary("Tables"), presets = P::serumLibrary("Presets");
    if (P::serumLibraryRoots().empty()) { CHECK(tables.empty()); CHECK(presets.empty()); return; } // not installed here
    REQUIRE(tables.size() > 100); REQUIRE(presets.size() > 100);
    for (const auto& item : tables) CHECK(juce::File(juce::String(item.path)).hasFileExtension("wav"));
    for (size_t i = 1; i < presets.size(); ++i)
        CHECK(juce::String(presets[i - 1].folder).compareNatural(juce::String(presets[i].folder)) <= 0);
    const auto shapes = std::find_if(tables.begin(), tables.end(), [](const auto& item) { return item.name == "Default Shapes"; });
    REQUIRE(shapes != tables.end()); CHECK(shapes->folder == "S2 Tables");
    CHECK(P::serumTableFlag(juce::File(juce::String(shapes->path))) == 0);
    const auto loaded = P::loadSerumTable(juce::File(juce::String(shapes->path)));
    REQUIRE(loaded); CHECK(loaded.data->frames() == 9); CHECK(loaded.data->frameLayout() == devices::WavetableFrameLayout::Steps);
    // Stepping wraps and finds by name or path.
    const int index = (int) (shapes - tables.begin());
    CHECK(P::serumLibraryStep(tables, "Default Shapes", 1) == (index + 1) % (int) tables.size());
    CHECK(P::serumLibraryStep(tables, tables.front().path, -1) == (int) tables.size() - 1);
    CHECK(P::serumLibraryStep(tables, "no such table", 1) == -1);

    // Library presets go through the bounded Serum importer: sample every 25th file across all folders.
    int tried = 0, imported = 0;
    for (size_t i = 0; i < presets.size(); i += 25)
    {
        juce::MemoryBlock block; const juce::File file(juce::String(presets[i].path));
        if (! file.loadFileAsData(block)) continue;
        const auto* first = static_cast<const uint8_t*>(block.getData());
        const std::vector<uint8_t> bytes(first, first + block.getSize());
        const auto result = P::importSerumPatch(bytes, file.getParentDirectory().getFullPathName().toStdString());
        ++tried; if (result) ++imported; else std::printf("    preset not imported: %s (%s)\n", presets[i].name.c_str(), result.error.c_str());
    }
    std::printf("    library presets imported: %d / %d sampled\n", imported, tried);
    CHECK(imported == tried);
}

TEST_CASE("serum preset import: shape warps map to native stand-ins and keep the preset's own labels")
{
    namespace P = plugins;
    const juce::File pad("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets/Factory/Pad/PD - Big Analog Pad.SerumPreset");
    if (! pad.existsAsFile()) return;
    juce::MemoryBlock block; REQUIRE(pad.loadFileAsData(block));
    const auto* first = static_cast<const uint8_t*>(block.getData());
    const auto imported = P::importSerumPatch(std::vector<uint8_t>(first, first + block.getSize()), pad.getParentDirectory().getFullPathName().toStdString());
    REQUIRE(imported);
    float mode = -1;
    for (const auto& p : imported.device.parameters) if (p.path == "Osc.1/Warp1Mode") mode = (float) p.value;
    CHECK_NEAR(mode, W::Pulse, 0); // kPWM -> Pulse (approximate)
    CHECK(std::any_of(imported.warnings.begin(), imported.warnings.end(), [](const auto& w) { return w.find("approximated") != std::string::npos; }));
}

TEST_CASE("sfz import: an end/loop_end written as the frame count is clamped to the file instead of dropping the region")
{
    const juce::File directory("/tmp/sonora_tests/sonora_sfz_end_clamp"); REQUIRE(directory.createDirectory().wasOk());
    const auto wav = directory.getChildFile("tone.wav");
    {
        juce::AudioBuffer<float> audio(1, 1000);
        for (int i = 0; i < 1000; ++i) audio.setSample(0, i, std::sin((float) i * 0.1f) * 0.5f);
        wav.deleteFile();
        juce::WavAudioFormat format;
        auto stream = wav.createOutputStream(); REQUIRE(stream != nullptr);
        std::unique_ptr<juce::OutputStream> output(stream.release());
        auto writer = format.createWriterFor(output, juce::AudioFormatWriterOptions{}.withSampleRate(44100).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(writer != nullptr); REQUIRE(writer->writeFromAudioSampleBuffer(audio, 0, 1000));
    }
    const auto media = devices::OscillatorMedia::fromSfzText(
        "<region> key=60 sample=tone.wav end=1000 loop_start=100 loop_end=1000 loop_mode=loop_continuous", directory, "Clamp", true);
    REQUIRE(media);
    REQUIRE(media.data->zones().size() == 1);
    CHECK(media.data->zones().front().end == 1000); CHECK(media.data->zones().front().loopEnd == 1000);
}

TEST_CASE("wavetable POLY and imported fine/detune/octave/mix targets")
{
    // POLY limits own patches: a third held note restarts the oldest voice.
    W synth; synth.prepare(48000, 512);
    synth.setParameter(W::PolyphonyParam, 2);
    for (int note : {60, 64, 67}) synth.queueUiMidi(0x90, (uint8_t) note, 100);
    render(synth, 256);
    CHECK(synth.activeVoiceCount() == 2);
    CHECK(! synth.noteLit(60)); CHECK(synth.noteLit(64)); CHECK(synth.noteLit(67));
    synth.setImportedPolyphony(12); CHECK_NEAR(synth.getParameter(W::PolyphonyParam), 12, 0);

    // New import destinations accept only their own law.
    CHECK(synth.setImportedModulationDomain(W::Osc1Fine, W::ImportedModulationDomain::FineCents200));
    CHECK(synth.setImportedModulationDomain(W::Osc2Detune, W::ImportedModulationDomain::DetuneCurve));
    CHECK(synth.setImportedModulationDomain(W::Osc3Octave, W::ImportedModulationDomain::OctaveSteps));
    CHECK(synth.setImportedModulationDomain(W::Filter2Mix, W::ImportedModulationDomain::Unit));
    CHECK(! synth.setImportedModulationDomain(W::Osc1Fine, W::ImportedModulationDomain::OctaveSteps));
    CHECK(std::string(W::destinationName(W::Osc1Fine)) == "A Fine");

    // Fixed -> A Octave at +1/8 of the normalized range raises the oscillator one octave (zero crossings double).
    const auto crossings = [](bool routed) {
        W s; s.prepare(48000, 512); s.setParameter(W::filterParam(0, W::FilterOn), 0);
        s.setParameter(W::oscParam(0, W::Position), 0.2f); // sine frame
        if (routed)
        {
            REQUIRE(s.setImportedModulationDomain(W::Osc1Octave, W::ImportedModulationDomain::OctaveSteps));
            s.setParameter(W::modParam(0, W::ModSource), (float) W::Fixed); s.setParameter(W::modParam(0, W::ModDestination), (float) W::Osc1Octave);
            s.setParameter(W::modParam(0, W::ModAmount), 0.125f);
        }
        s.queueUiMidi(0x90, 57, 100);
        std::vector<float> out; render(s, 9600, &out);
        int count = 0; for (size_t i = 4801; i < out.size(); ++i) count += (out[i - 1] < 0) != (out[i] < 0);
        return count;
    };
    const int plain = crossings(false), raised = crossings(true);
    CHECK(plain > 30); CHECK_NEAR((double) raised / plain, 2.0, 0.1);
}

TEST_CASE("wavetable noise colours and chaos LFOs: generated spectra differ, attractors stay bounded and import from presets")
{
    // Noise colours: pink and brown carry progressively less high-frequency energy than white (first-difference energy ratio).
    const auto roughness = [](int type) {
        W s; s.prepare(48000, 512);
        for (int o = 0; o < 3; ++o) s.setParameter(W::oscParam(o, W::On), 0);
        s.setParameter(W::filterParam(0, W::FilterOn), 0); s.setParameter(W::NoiseOn, 1); s.setParameter(W::NoiseLevel, 1);
        s.setParameter(W::NoiseColor, 0); s.setParameter(W::NoiseTypeParam, (float) type);
        s.queueUiMidi(0x90, 60, 100);
        std::vector<float> out; render(s, 9600, &out);
        double energy = 0, difference = 0;
        for (size_t i = 4801; i < out.size(); ++i) { energy += out[i] * out[i]; difference += (out[i] - out[i - 1]) * (out[i] - out[i - 1]); }
        return energy > 0 ? difference / energy : -1.0;
    };
    const double white = roughness(W::NoiseWhite), pink = roughness(W::NoisePink), brown = roughness(W::NoiseBrown), geiger = roughness(W::NoiseGeiger);
    CHECK(white > 1.5); CHECK(pink < white * 0.6); CHECK(brown < pink * 0.5); CHECK(geiger > 0);

    // Chaos LFOs: bounded, aperiodic-looking motion; Random S&H holds steps.
    for (int type : {(int) W::LfoLorenz, (int) W::LfoRossler, (int) W::LfoRandomHold})
    {
        W s; s.prepare(48000, 256);
        s.setParameter(W::lfoTypeParam(0), (float) type); s.setParameter(W::lfoParam(0, W::SyncOn), 0); s.setParameter(W::lfoParam(0, W::Rate), 4);
        s.setParameter(W::lfoParam(0, W::Mode), W::Global); s.setParameter(W::lfoParam(0, W::Bipolar), 0);
        s.queueUiMidi(0x90, 60, 100);
        float low = 1, high = 0; int changes = 0; float last = -1;
        for (int block = 0; block < 400; ++block)
        {
            render(s, 256);
            const float value = s.liveState().lfoValue[0];
            CHECK(value >= 0); CHECK(value <= 1);
            low = std::min(low, value); high = std::max(high, value);
            if (std::abs(value - last) > 1e-6f) ++changes;
            last = value;
        }
        CHECK(high - low > 0.3f);
        if (type == W::LfoRandomHold) { CHECK(changes > 5); CHECK(changes < 200); } else CHECK(changes > 300);
    }
    CHECK(W::chaosTrajectory(W::LfoLorenz).size() > 1000);
    float left = 1, right = 0;
    for (const auto& point : W::chaosTrajectory(W::LfoLorenz)) { left = std::min(left, point[0]); right = std::max(right, point[0]); }
    CHECK(left < 0.3f); CHECK(right > 0.7f); // both wings

    // Import: PL - Alive uses a Lorenz LFO 1 and white noise.
    const juce::File alive("/Library/Audio/Presets/Xfer Records/Serum 2 Presets/Presets/Factory/Pluck/PL - Alive.SerumPreset");
    if (! alive.existsAsFile()) return;
    juce::MemoryBlock block; REQUIRE(alive.loadFileAsData(block));
    const auto* first = static_cast<const uint8_t*>(block.getData());
    const auto imported = plugins::importSerumPatch(std::vector<uint8_t>(first, first + block.getSize()), alive.getParentDirectory().getFullPathName().toStdString());
    REQUIRE(imported);
    float lfoType = -1, noiseType = -1;
    for (const auto& p : imported.device.parameters)
    {
        if (p.path == "Lfo.0/Type") lfoType = (float) p.value;
        if (p.path == "Noise/Type") noiseType = (float) p.value;
    }
    CHECK_NEAR(lfoType, W::LfoLorenz, 0); CHECK_NEAR(noiseType, W::NoiseWhite, 0);
}
