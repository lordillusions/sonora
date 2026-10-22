#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "devices/native/EqEight.h"
#include "devices/native/Utility.h"
#include "io/als/AlsReader.h"
#include "plugins/PluginHost.h"

#include <cstdlib>

using namespace sonora;
using namespace sonora::devices;

namespace
{
juce::AudioBuffer<float> sine(double freq, double sr, int n)
{
    juce::AudioBuffer<float> b(2, n);
    for (int i = 0; i < n; ++i)
    {
        const float s = (float) std::sin(2.0 * M_PI * freq * i / sr);
        b.setSample(0, i, s);
        b.setSample(1, i, s);
    }
    return b;
}

float rmsTail(const juce::AudioBuffer<float>& b, int ch)
{
    const int n = b.getNumSamples();
    return b.getRMSLevel(ch, n / 2, n / 2);
}
} // namespace

TEST_CASE("utility: gain and mute")
{
    Utility u;
    u.prepare(48000, 512);
    u.setParameter(Utility::Gain, 0.5f);
    juce::MidiBuffer midi;
    auto b = sine(1000, 48000, 4800);
    const float before = rmsTail(b, 0);
    for (int i = 0; i < 4800; i += 480)
    {
        juce::AudioBuffer<float> blk(b.getArrayOfWritePointers(), 2, i, 480);
        u.process(blk, midi);
    }
    CHECK_NEAR(rmsTail(b, 0), before * 0.5f, 0.01);

    u.setParameter(Utility::Mute, 1.0f);
    auto c = sine(1000, 48000, 4800);
    for (int i = 0; i < 4800; i += 480)
    {
        juce::AudioBuffer<float> blk(c.getArrayOfWritePointers(), 2, i, 480);
        u.process(blk, midi);
    }
    CHECK(rmsTail(c, 0) < 1e-4f);
}

TEST_CASE("utility: width 0 makes mono")
{
    Utility u;
    u.prepare(48000, 512);
    u.setParameter(Utility::StereoWidth, 0.0f);
    juce::AudioBuffer<float> b(2, 64);
    for (int i = 0; i < 64; ++i)
    {
        b.setSample(0, i, 1.0f);
        b.setSample(1, i, -1.0f);
    }
    juce::MidiBuffer midi;
    u.process(b, midi);
    CHECK_NEAR(b.getSample(0, 63), 0.0, 1e-6);
    CHECK_NEAR(b.getSample(1, 63), 0.0, 1e-6);
}

TEST_CASE("eq eight: bell boosts at its centre frequency")
{
    EqEight eq;
    eq.prepare(48000, 512);
    // Only band 4 on: bell at 1 kHz, +12 dB.
    for (int b = 0; b < EqEight::kBands; ++b)
        eq.setParameter(EqEight::bandParam(b, EqEight::On), b == 3 ? 1.0f : 0.0f);
    eq.setParameter(EqEight::bandParam(3, EqEight::Type), 3.0f);
    eq.setParameter(EqEight::bandParam(3, EqEight::Freq), 1000.0f);
    eq.setParameter(EqEight::bandParam(3, EqEight::GainDb), 12.0f);
    eq.setParameter(EqEight::bandParam(3, EqEight::Q), 1.0f);

    auto b = sine(1000, 48000, 9600);
    const float before = rmsTail(b, 0);
    juce::MidiBuffer midi;
    eq.process(b, midi);
    const float gainDb = juce::Decibels::gainToDecibels(rmsTail(b, 0) / before);
    CHECK_NEAR(gainDb, 12.0, 0.3);
    CHECK_NEAR(juce::Decibels::gainToDecibels(eq.responseAt(1000.0)), 12.0, 0.1);
    CHECK_NEAR(juce::Decibels::gainToDecibels(eq.responseAt(50.0)), 0.0, 0.3);
}

TEST_CASE("eq eight: low cut 48 removes lows")
{
    EqEight eq;
    eq.prepare(48000, 512);
    for (int b = 0; b < EqEight::kBands; ++b)
        eq.setParameter(EqEight::bandParam(b, EqEight::On), b == 0 ? 1.0f : 0.0f);
    eq.setParameter(EqEight::bandParam(0, EqEight::Type), 0.0f);
    eq.setParameter(EqEight::bandParam(0, EqEight::Freq), 500.0f);
    eq.setParameter(EqEight::bandParam(0, EqEight::Q), 0.71f);
    juce::MidiBuffer midi;
    auto b = eq.responseAt(62.5); // 3 octaves below -> ~ -144 dB ideal; just check it is very low
    CHECK(juce::Decibels::gainToDecibels(b) < -60.0);
    CHECK_NEAR(juce::Decibels::gainToDecibels(eq.responseAt(8000.0)), 0.0, 0.5);
}

TEST_CASE("factory: native devices from model")
{
    model::Device d;
    d.kind = model::DeviceKind::Native;
    d.type = "StereoGain";
    d.parameters.push_back({"Gain", 0.25, -1, {}});
    auto dev = createNative(d);
    REQUIRE(dev != nullptr);
    CHECK(dev->name() == "Utility");
    CHECK_NEAR(dev->getParameter(Utility::Gain), 0.25, 1e-6);

    d.type = "Chorus2"; // not implemented yet
    CHECK(createNative(d) == nullptr);
}

TEST_CASE("drum rack routes notes to the matching pad only")
{
    struct Counter : DeviceProcessor
    {
        int* hits;
        explicit Counter(int* h) : hits(h) {}
        std::string name() const override { return "counter"; }
        bool isInstrument() const override { return true; }
        void prepare(double, int) override {}
        void process(juce::AudioBuffer<float>&, juce::MidiBuffer& m) override
        {
            for (const auto e : m)
                if (e.getMessage().isNoteOn())
                    *hits += e.getMessage().getNoteNumber() == 60 ? 1 : 100;
        }
    };
    int kick = 0, snare = 0;
    std::vector<RackProcessor::Chain> chains(2);
    chains[0].devices.push_back(std::make_unique<Counter>(&kick));
    chains[0].receivingNote = 36;
    chains[1].devices.push_back(std::make_unique<Counter>(&snare));
    chains[1].receivingNote = 38;
    RackProcessor rack("Drum Rack", true, true, std::move(chains));
    rack.prepare(48000, 64);

    juce::AudioBuffer<float> audio(2, 64);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0);
    rack.process(audio, midi);
    CHECK(kick == 1);  // received, transposed to sendingNote 60
    CHECK(snare == 0);
}

// ---------------------------------------------------------------- out-of-process scanner (PLG-3)

#if ! defined(_WIN32) // the fake scanners are /bin/sh scripts (COM-7: Windows runs the real scanner only)
namespace
{
// Writes an executable shell script that stands in for sonora_plugin_scanner.
juce::File fakeScanner(const juce::File& dir, const char* name, const juce::String& body)
{
    auto f = dir.getChildFile(name);
    f.replaceWithText("#!/bin/sh\n" + body + "\n", false, false, "\n"); // LF: a CR breaks the shebang
    f.setExecutePermission(true);
    return f;
}
} // namespace

TEST_CASE("plugin scanner: hang is killed and blacklisted, crash is blacklisted, success is added")
{
    juce::ScopedJuceInitialiser_GUI juceInit; // KnownPluginList broadcasts changes
    juce::TemporaryFile dirHolder;
    const auto dir = dirHolder.getFile();
    dir.createDirectory();

    plugins::PluginHost host;
    host.scanTimeoutMs = 2000; // generous: a loaded machine can take a while to start /bin/sh

    // 1. Hangs forever.
    ::setenv("SONORA_PLUGIN_SCANNER", fakeScanner(dir, "hang.sh", "sleep 30").getFullPathName().toRawUTF8(), 1);
    juce::String err;
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    CHECK(! host.scanFile("VST3", "/x/Hang.vst3", err));
    CHECK(juce::Time::getMillisecondCounterHiRes() - t0 < 10000.0);
    CHECK(err.contains("Timed out"));

    CHECK(host.failedPlugins().contains("/x/Hang.vst3"));

    // 2. Crashes (non-zero exit, no output).
    ::setenv("SONORA_PLUGIN_SCANNER", fakeScanner(dir, "crash.sh", "exit 139").getFullPathName().toRawUTF8(), 1);
    CHECK(! host.scanFile("VST3", "/x/Crash.vst3", err));
    CHECK(host.failedPlugins().contains("/x/Crash.vst3"));

    // 3. Finds nothing (e.g. macOS refused to load it) -> blacklisted so it is not retried.
    ::setenv("SONORA_PLUGIN_SCANNER",
             fakeScanner(dir, "empty.sh", "echo '<PLUGINS/>' > \"$3\"").getFullPathName().toRawUTF8(), 1);
    CHECK(! host.scanFile("VST3", "/x/Empty.vst3", err));
    CHECK(host.failedPlugins().contains("/x/Empty.vst3"));

    // 4. Works: one description in the output file.
    const juce::String xml = "<PLUGINS><PLUGIN name=\"Good\" format=\"VST3\" category=\"Fx\" manufacturer=\"Acme\" "
                             "version=\"1\" file=\"/x/Good.vst3\" uniqueId=\"1\" isInstrument=\"0\" fileTime=\"1\" "
                             "infoUpdateTime=\"1\" numInputs=\"2\" numOutputs=\"2\" isShell=\"0\" hasARAExtension=\"0\" "
                             "uid=\"1\"/></PLUGINS>";
    ::setenv("SONORA_PLUGIN_SCANNER",
             fakeScanner(dir, "ok.sh", "echo '" + xml + "' > \"$3\"").getFullPathName().toRawUTF8(), 1);
    CHECK(host.scanFile("VST3", "/x/Good.vst3", err));
    REQUIRE(host.knownPlugins().getNumTypes() == 1);
    CHECK(host.knownPlugins().getTypes()[0].name == "Good");
    CHECK(! host.failedPlugins().contains("/x/Good.vst3"));

    ::unsetenv("SONORA_PLUGIN_SCANNER");
}
#endif

// ---------------------------------------------------------------- Simpler (DEV-4)

#include "devices/native/Simpler.h"

namespace
{
// A 1 s, 44.1 kHz stereo sine at 440 Hz (amplitude 0.5) wrapped as a loaded sample.
std::shared_ptr<engine::SampleSlot> sineSample(double hz = 440.0, int frames = 44100)
{
    auto d = std::make_shared<engine::SampleData>();
    d->sampleRate = 44100.0;
    d->audio.setSize(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * (float) hz * (float) i / 44100.0f);
        d->audio.setSample(0, i, v);
        d->audio.setSample(1, i, v);
    }
    return engine::SampleSlot::makeReady(d);
}

std::unique_ptr<devices::Simpler> makeSimpler(devices::Simpler::Mode mode, int keyMin = 0, int keyMax = 127, int root = 69,
                                              std::shared_ptr<engine::SampleSlot> slot = nullptr)
{
    devices::Simpler::Zone z;
    z.part.keyMin = keyMin;
    z.part.keyMax = keyMax;
    z.part.rootKey = root;
    z.slot = slot ? slot : sineSample();
    auto s = std::make_unique<devices::Simpler>(std::vector<devices::Simpler::Zone>{z}, mode, 5);
    s->prepare(44100.0, 512);
    // A "flat" Simpler: the reference defaults (-12 dB, 35 % velocity, 22 kHz filter, loop, snap) are tested separately.
    using S = devices::Simpler;
    for (auto [param, value] : {std::pair{S::Volume, 0.0f}, {S::VelToVolume, 0.0f}, {S::FilterOn, 0.0f}, {S::LoopOn, 0.0f}, {S::Snap, 0.0f}})
        s->setParameter((int) param, value);
    return s;
}

void setP(devices::Simpler& s, devices::Simpler::Param p, float v)
{
    s.setParameter((int) p, v);
}

// Renders `blocks` x 512 samples; `onFirst` is added to the first block.
std::vector<float> renderSimpler(devices::Simpler& s, juce::MidiBuffer first, int blocks, std::function<void(int, juce::MidiBuffer&)> perBlock = {})
{
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf(2, 512);
        buf.clear();
        juce::MidiBuffer m;
        if (b == 0)
            m = first;
        if (perBlock)
            perBlock(b, m);
        s.process(buf, m);
        for (int i = 0; i < 512; ++i)
            out.push_back(buf.getSample(0, i));
    }
    return out;
}

double hzOf(const std::vector<float>& x, int from, int to)
{
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[(size_t) i - 1] <= 0.0f && x[(size_t) i] > 0.0f)
            ++crossings;
    return crossings / ((to - from) / 44100.0);
}

double peak(const std::vector<float>& x, int from, int to)
{
    double p = 0.0;
    for (int i = from; i < to; ++i)
        p = std::max(p, (double) std::fabs(x[(size_t) i]));
    return p;
}

juce::MidiBuffer noteOn(int note, int vel = 127)
{
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) vel), 0);
    return m;
}
} // namespace

TEST_CASE("simpler: plays the sample at its root key and transposes by semitones")
{
    auto s = makeSimpler(devices::Simpler::Mode::Classic);
    auto out = renderSimpler(*s, noteOn(69), 8);
    CHECK_NEAR(hzOf(out, 600, 3500), 440.0, 8.0);
    CHECK(peak(out, 600, 3500) > 0.45); // sample amplitude 0.5, sustain at 0 dB

    auto up = makeSimpler(devices::Simpler::Mode::Classic);
    out = renderSimpler(*up, noteOn(81), 4); // one octave above the root
    CHECK_NEAR(hzOf(out, 600, 1800), 880.0, 25.0);

    auto tr = makeSimpler(devices::Simpler::Mode::Classic);
    setP(*tr, devices::Simpler::Transpose, -12.0f);
    out = renderSimpler(*tr, noteOn(69), 4);
    CHECK_NEAR(hzOf(out, 600, 1800), 220.0, 15.0);
}

TEST_CASE("simpler: key zones, velocity zones and unloaded samples")
{
    auto s = makeSimpler(devices::Simpler::Mode::Classic, 60, 64, 60);
    auto out = renderSimpler(*s, noteOn(70), 2); // outside 60..64
    CHECK(peak(out, 0, 1024) < 1e-6);
    out = renderSimpler(*s, noteOn(62), 2);
    CHECK(peak(out, 0, 1024) > 0.1);

    auto notLoaded = std::make_shared<engine::SampleSlot>(); // still decoding
    auto n = makeSimpler(devices::Simpler::Mode::Classic, 0, 127, 69, notLoaded);
    out = renderSimpler(*n, noteOn(69), 2);
    CHECK(peak(out, 0, 1024) < 1e-9);
    CHECK(n->activeVoices() == 0);
}

namespace
{
// A falling saw (amplitude 0.8, like an analog saw recording) as a loaded sample.
std::shared_ptr<engine::SampleSlot> sawSample(double hz = 70.0, int frames = 88200)
{
    auto d = std::make_shared<engine::SampleData>();
    d->sampleRate = 44100.0;
    d->audio.setSize(2, frames);
    for (int i = 0; i < frames; ++i)
    {
        const double phase = std::fmod(hz * i / 44100.0, 1.0);
        const float v = (float) (0.8 - 1.6 * phase);
        d->audio.setSample(0, i, v);
        d->audio.setSample(1, i, v);
    }
    return engine::SampleSlot::makeReady(d);
}

double maxStep(const std::vector<float>& x, int from, int to)
{
    double m = 0.0;
    for (int i = from + 1; i < to; ++i)
        m = std::max(m, (double) std::fabs(x[(size_t) i] - x[(size_t) i - 1]));
    return m;
}
} // namespace

TEST_CASE("biquad: the state-variable form matches the direct form and survives coefficient jumps")
{
    using T = devices::BiquadCoeffs::Type;
    juce::Random rnd(7);
    std::vector<float> noise(4096);
    for (auto& v : noise) v = rnd.nextFloat() - 0.5f;
    for (auto type : {T::LowPass, T::HighPass, T::Peak, T::LowShelf, T::HighShelf, T::Notch, T::BandPass})
        for (double freq : {40.0, 1000.0, 15000.0})
        {
            const auto c = devices::BiquadCoeffs::make(type, 44100.0, freq, 2.0, 9.0);
            const auto s = devices::SvfCoeffs::fromBiquad(c);
            devices::BiquadState df;
            devices::SvfState svf;
            double worst = 0.0;
            for (float x : noise) worst = std::max(worst, (double) std::fabs(df.process(c, x) - svf.process(s, x)));
            CHECK(worst < 1e-4);
        }

    // A 6 kHz -> 300 Hz cutoff step on a loud saw (an automation jump): the direct form overshoots far past the input,
    // the state-variable form does not.
    const auto bright = devices::BiquadCoeffs::make(T::LowPass, 44100.0, 6000.0, 0.5, 0.0);
    const auto dark = devices::BiquadCoeffs::make(T::LowPass, 44100.0, 300.0, 0.5, 0.0);
    devices::BiquadState df;
    devices::SvfState svf;
    const auto brightSvf = devices::SvfCoeffs::fromBiquad(bright), darkSvf = devices::SvfCoeffs::fromBiquad(dark);
    double dfPeak = 0.0, svfPeak = 0.0;
    for (int i = 0; i < 4000; ++i)
    {
        const float x = (float) (0.8 - 1.6 * std::fmod(70.0 * i / 44100.0, 1.0));
        const bool after = i >= 520; // just before the saw's first edge (630 frames)
        dfPeak = std::max(dfPeak, (double) std::fabs(df.process(after ? dark : bright, x)));
        svfPeak = std::max(svfPeak, (double) std::fabs(svf.process(after ? darkSvf : brightSvf, x)));
    }
    CHECK(svfPeak < 0.85);
    CHECK(dfPeak > svfPeak);
}

TEST_CASE("simpler: a cutoff automation step on a sounding note does not click and reaches the note")
{
    // Regression (becker_professor_offbeat set): Freq automation jumping 6116 -> 319 Hz while a saw note sounds made a
    // full-scale pop. The filter envelope is off here, so the new cutoff must still reach the sounding voice.
    using S = devices::Simpler;
    auto s = makeSimpler(S::Mode::Classic, 0, 127, 60, sawSample());
    setP(*s, S::FilterOn, 1.0f);
    setP(*s, S::FilterEnvOn, 0.0f);
    setP(*s, S::FilterFreq, 6116.0f);
    auto out = renderSimpler(*s, noteOn(60), 10, [&](int block, juce::MidiBuffer&) {
        if (block == 3) setP(*s, S::FilterFreq, 319.0f); // mid-note (the saw's edges fall every 630 frames)
    });
    const double bright = maxStep(out, 0, 1536), dark = maxStep(out, 3072, (int) out.size());
    CHECK(bright > 0.15);                          // the saw edges pass
    CHECK(peak(out, 0, (int) out.size()) < 0.9);   // no overshoot past the saw's own 0.8
    CHECK(dark < bright * 0.25);                   // the new cutoff applies to the held note
}

TEST_CASE("simpler: velocity scales volume, volume and pan parameters apply")
{
    auto s = makeSimpler(devices::Simpler::Mode::Classic);
    setP(*s, devices::Simpler::VelToVolume, 1.0f);
    auto loud = renderSimpler(*s, noteOn(69, 127), 2);
    s->reset();
    auto soft = renderSimpler(*s, noteOn(69, 32), 2);
    CHECK(peak(soft, 200, 1000) < peak(loud, 200, 1000) * 0.4);

    // Measured reference law: 0.4 dB per velocity step around velocity 70, scaled by Vel>Vol.
    CHECK_NEAR(20.0 * std::log10(peak(loud, 200, 1000) / peak(soft, 200, 1000)), 0.4 * (127 - 32), 0.3);

    s->reset();
    setP(*s, devices::Simpler::VelToVolume, 0.0f);
    auto flat = renderSimpler(*s, noteOn(69, 127), 2);
    s->reset();
    setP(*s, devices::Simpler::Volume, -6.0f);
    auto minus6 = renderSimpler(*s, noteOn(69, 127), 2);
    CHECK_NEAR(peak(minus6, 200, 1000) / peak(flat, 200, 1000), 0.501, 0.02);

    // Equal-power pan with +3 dB at the extremes, unity in the centre.
    s->reset();
    setP(*s, devices::Simpler::Volume, 0.0f);
    setP(*s, devices::Simpler::Pan, -1.0f);
    juce::AudioBuffer<float> stereo(2, 1024);
    stereo.clear();
    juce::MidiBuffer on = noteOn(69, 127);
    s->process(stereo, on);
    CHECK_NEAR(stereo.getMagnitude(0, 200, 800) / peak(flat, 200, 1000), std::sqrt(2.0), 0.03);
    CHECK(stereo.getMagnitude(1, 200, 800) < 1e-5f);
}

TEST_CASE("simpler: new devices start from the reference default Simpler")
{
    devices::Simpler::Zone z;
    z.slot = sineSample();
    devices::Simpler s({z}, devices::Simpler::Mode::Classic, 5);
    using S = devices::Simpler;
    CHECK_NEAR(s.getParameter(S::Volume), -12.0, 1e-6);
    CHECK_NEAR(s.getParameter(S::VelToVolume), 0.35, 1e-6);
    CHECK(s.getParameter(S::FilterOn) > 0.5f);
    CHECK(s.getParameter(S::FilterSlope) > 0.5f);
    CHECK(s.getParameter(S::LoopOn) > 0.5f);
    CHECK(s.getParameter(S::Retrigger) > 0.5f);
    CHECK_NEAR(s.getParameter(S::Voices), 5.0, 1e-6); // the sixth chooser entry: 6 voices
    CHECK_NEAR(s.getParameter(S::DecaySlope), 1.0, 1e-6);
}

TEST_CASE("simpler: envelope slopes follow the measured curves and releases end at -60 dB")
{
    using S = devices::Simpler;
    auto s = makeSimpler(S::Mode::Classic, 0, 127, 69, sineSample(440.0, 44100 * 3));
    setP(*s, S::Attack, 1000.0f);
    setP(*s, S::AttackSlope, 0.0f); // linear: half way after half the time
    auto linear = renderSimpler(*s, noteOn(69), 90);
    CHECK_NEAR(peak(linear, 22050 - 200, 22050 + 200), 0.25, 0.01);
    s->reset();
    setP(*s, S::AttackSlope, 1.0f); // exponential approach to the -84 dB floor: 1 - e^-9.67x
    auto convex = renderSimpler(*s, noteOn(69), 30);
    CHECK_NEAR(peak(convex, 4410 - 100, 4410 + 100) / 0.5, 1.0 - std::exp(-0.967), 0.02);
    s->reset();
    setP(*s, S::AttackSlope, -0.5f); // blend: 0.5 x + 0.5 e^(-9.115 (1 - x))
    auto concave = renderSimpler(*s, noteOn(69), 90);
    CHECK_NEAR(peak(concave, 39690 - 100, 39690 + 100) / 0.5, 0.45 + 0.5 * std::exp(-0.9115), 0.02);

    auto r = makeSimpler(S::Mode::Classic, 0, 127, 69, sineSample(440.0, 44100 * 3));
    setP(*r, S::Release, 1000.0f); // slope +1: dB-linear, the voice ends once it falls below -60 dB (~0.71 s)
    auto released = renderSimpler(*r, noteOn(69), 120, [](int b, juce::MidiBuffer& m) {
        if (b == 10) m.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    });
    CHECK(peak(released, 5120 + 26000, 5120 + 29000) > 0.0002);
    CHECK(peak(released, 5120 + 33000, 5120 + 36000) < 1e-6);
}

TEST_CASE("simpler: classic releases on note-off, one-shot ignores it")
{
    auto classic = makeSimpler(devices::Simpler::Mode::Classic);
    setP(*classic, devices::Simpler::Release, 20.0f);
    auto out = renderSimpler(*classic, noteOn(69), 10, [](int b, juce::MidiBuffer& m) {
        if (b == 2)
            m.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    });
    CHECK(peak(out, 1024, 1500) > 0.3);       // sounding while held
    CHECK(peak(out, 512 * 6, 512 * 10) < 1e-3); // silent well after the 20 ms release
    CHECK(classic->activeVoices() == 0);

    auto oneShot = makeSimpler(devices::Simpler::Mode::OneShot);
    out = renderSimpler(*oneShot, noteOn(69), 10, [](int b, juce::MidiBuffer& m) {
        if (b == 2)
            m.addEvent(juce::MidiMessage::noteOff(1, 69), 0);
    });
    CHECK(peak(out, 512 * 6, 512 * 10) > 0.3); // still playing
}

TEST_CASE("simpler: sample end stops the voice, sustain loop keeps it going")
{
    auto shortSample = sineSample(440.0, 2000);
    auto s = makeSimpler(devices::Simpler::Mode::Classic, 0, 127, 69, shortSample);
    auto out = renderSimpler(*s, noteOn(69), 6);
    CHECK(peak(out, 512 * 5, 512 * 6) < 1e-6); // 2000 frames long: over after ~45 ms
    CHECK(s->activeVoices() == 0);

    // Simpler loops the last LoopLength of its region (here frames 400-2000)
    auto looped = makeSimpler(devices::Simpler::Mode::Classic, 0, 127, 69, shortSample);
    setP(*looped, devices::Simpler::LoopOn, 1.0f);
    setP(*looped, devices::Simpler::LoopLengthMod, 0.8f);
    out = renderSimpler(*looped, noteOn(69), 20);
    CHECK(peak(out, 512 * 18, 512 * 20) > 0.3); // 230 ms later it is still looping
    CHECK(looped->activeVoices() == 1);

    // Sampler zones keep their own sustain loop
    devices::Simpler::Zone z;
    z.part.rootKey = 69;
    z.part.loopMode = 1;
    z.part.loopStart = 200;
    z.part.loopEnd = 1800;
    z.slot = shortSample;
    devices::Simpler sampler({z}, devices::Simpler::Mode::Classic, 5, true);
    sampler.prepare(44100.0, 512);
    out = renderSimpler(sampler, noteOn(69), 20);
    CHECK(peak(out, 512 * 18, 512 * 20) > 0.1);
    CHECK(sampler.activeVoices() == 1);
}

TEST_CASE("simpler: low-pass filter darkens the sound, fed from model parameters")
{
    auto bright = sineSample(8000.0, 44100);
    auto open = makeSimpler(devices::Simpler::Mode::Classic, 0, 127, 69, bright);
    auto a = renderSimpler(*open, noteOn(69), 4);

    auto closed = makeSimpler(devices::Simpler::Mode::Classic, 0, 127, 69, bright);
    setP(*closed, devices::Simpler::FilterOn, 1.0f);
    setP(*closed, devices::Simpler::FilterFreq, 500.0f);
    auto b = renderSimpler(*closed, noteOn(69), 4);
    CHECK(peak(a, 1000, 2000) > 0.4);
    CHECK(peak(b, 1000, 2000) < 0.1); // 8 kHz sits far above the 500 Hz cutoff

    // Parameters are addressed by their .als paths, so automation / MIDI mapping / loadFromModel work.
    bool found = false;
    for (const auto& info : closed->parameters())
        found |= info.id == "Filter/Slot/Value/SimplerFilter/Freq";
    CHECK(found);
}

TEST_CASE("simpler: measured morph mix, velocity -> cutoff and LFO -> cutoff depth")
{
    using S = devices::Simpler;
    // Steady-state peak of a sine through the filter (24 dB, resonance 0, cutoff 1 kHz).
    const auto level = [](double hz, std::function<void(S&)> setup, int velocity = 127) {
        auto s = makeSimpler(S::Mode::Classic, 0, 127, 60, sineSample(hz, 44100)); // note 60: key tracking adds nothing
        setP(*s, S::FilterOn, 1.0f); setP(*s, S::FilterFreq, 1000.0f); setup(*s);
        const auto out = renderSimpler(*s, noteOn(60, velocity), 24);
        return peak(out, 8000, 12000) / 0.5;
    };
    // Morph 0.125 = 0.5 LP + 0.866 BP per stage: -6 dB per stage below the cutoff and at the cutoff (-12 dB total).
    const auto morph = [](float x) { return [x](S& s) { setP(s, S::FilterType, 4.0f); setP(s, S::FilterMorph, x); }; };
    CHECK_NEAR(level(100.0, morph(0.125f)), 0.25, 0.02);
    CHECK_NEAR(level(1000.0, morph(0.125f)), 0.25, 0.02);
    CHECK_NEAR(level(1000.0, morph(0.0f)), 0.25, 0.02); // LP: -6 dB per stage at the cutoff
    CHECK(level(100.0, morph(0.5f)) < 0.001);            // HP
    CHECK_NEAR(level(100.0, morph(0.625f)), 0.25, 0.03); // HP + notch / 2: half the low-pass path per stage
    // Velocity 82 opens a 1 kHz cutoff by an octave (one semitone per step around 70): 0.8^2 at half the cutoff.
    const auto vel = [](S& s) { setP(s, S::FilterVelMod, 1.0f); };
    CHECK_NEAR(level(1000.0, vel, 70), 0.25, 0.02);
    CHECK_NEAR(level(1000.0, vel, 82), 0.64, 0.03);
    // ModByLfo is in semitones (reference range 0-24).
    auto s = makeSimpler(S::Mode::Classic);
    CHECK_NEAR(s->parameters()[(size_t) S::FilterLfoMod].maxValue, 24.0, 1e-6);
}

TEST_CASE("simpler: measured circuit models, drive, LFO key and amplitude loop modes")
{
    using S = devices::Simpler;
    const auto response = [](S& s, double hz) {
        devices::BiquadCoeffs stages[2];
        const int count = s.filterStages(1000.0, stages);
        double gain = 1;
        for (int i = 0; i < count; ++i) gain *= stages[i].magnitudeAt(hz, 44100.0);
        return 20 * std::log10(gain);
    };
    auto s = makeSimpler(S::Mode::Classic);
    setP(*s, S::FilterOn, 1.0f); setP(*s, S::FilterRes, 0.75f);
    const double clean = response(*s, 1000.0);
    setP(*s, S::FilterCircuit, 1.0f);
    CHECK(response(*s, 1000.0) < clean - 4.0); // OSR: much weaker resonance (measured +9.7 vs +15.6 dB at r = 0.75)
    setP(*s, S::FilterCircuit, 4.0f); setP(*s, S::FilterRes, 0.5f);
    // PRD ladder, k = 1.93 at r = 0.5: unity pass band, stop band raised by 20 log10(1 + k) against Clean.
    CHECK_NEAR(response(*s, 30.0), 0.0, 0.1);
    const double prdStop = response(*s, 8000.0);
    setP(*s, S::FilterCircuit, 0.0f);
    CHECK_NEAR(prdStop - response(*s, 8000.0), 20 * std::log10(1 + 1.929), 1.0);

    // Drive: a gain into the circuit's clipper (linear below 1.54, ceiling ~1.75); Clean ignores it.
    const auto peakWith = [](float circuit, float drive) {
        auto d = makeSimpler(S::Mode::Classic, 0, 127, 60, sineSample(100.0, 44100));
        setP(*d, S::FilterOn, 1.0f); setP(*d, S::FilterCircuit, circuit); setP(*d, S::FilterDrive, drive);
        return peak(renderSimpler(*d, noteOn(60), 24), 6000, 12000);
    };
    CHECK_NEAR(peakWith(0, 24), 0.5, 0.03);
    CHECK_NEAR(peakWith(1, 6), 1.0, 0.06);
    CHECK(peakWith(1, 24) < 2.0); // +0.2 bias: positive side limited at 1.54, negative at 1.95

    // LFO Key: rate x (1 + key 2^((note - 60) / 12)): a 2 Hz volume LFO runs at 6 Hz one octave above C3.
    auto lfo = makeSimpler(S::Mode::Classic, 0, 127, 72, sineSample(523.25, 44100));
    for (auto [param, value] : {std::pair{S::LfoOn, 1.0f}, {S::LfoVolume, 1.0f}, {S::LfoRate, 2.0f}, {S::LfoKeyScale, 1.0f}}) setP(*lfo, param, value);
    const auto out = renderSimpler(*lfo, noteOn(72), 86); // 1 s
    int minima = 0;
    for (int b = 1; b + 1 < 86; ++b)
    {
        const double here = peak(out, b * 512, b * 512 + 512), before = peak(out, b * 512 - 512, b * 512), after = peak(out, b * 512 + 512, b * 512 + 1024);
        if (here < before && here <= after && here < 0.1) ++minima;
    }
    CHECK(minima >= 5); CHECK(minima <= 7);

    // Amplitude Trigger mode: the release follows the decay even while the key is held.
    auto trig = makeSimpler(S::Mode::Classic);
    for (auto [param, value] : {std::pair{S::Attack, 10.0f}, {S::Decay, 50.0f}, {S::Sustain, 0.5f}, {S::Release, 20.0f}, {S::AmpLoopMode, 4.0f}}) setP(*trig, param, value);
    CHECK(peak(renderSimpler(*trig, noteOn(69), 40), 15000, 20000) < 0.001);
    // Loop mode restarts the attack after the decay and the Loop Time fall.
    auto loop = makeSimpler(S::Mode::Classic);
    for (auto [param, value] : {std::pair{S::Attack, 10.0f}, {S::Decay, 50.0f}, {S::Sustain, 0.2f}, {S::DecaySlope, 0.0f}, {S::AmpLoopMode, 1.0f}, {S::AmpLoopTime, 40.0f}}) setP(*loop, param, value);
    const auto looped = renderSimpler(*loop, noteOn(69), 16); // 186 ms: two 100 ms cycles
    CHECK(peak(looped, 4300, 4400) < 0.02);         // the 40 ms fall reaches 0 at 100 ms (4410 samples)
    CHECK(peak(looped, 4410 + 441, 4410 + 900) > 0.3); // then the attack restarts
}

TEST_CASE("simpler: loads zones and settings from a Live set")
{
    // The real set that has Simpler devices is not in the repo; build the model by hand.
    model::Device d;
    d.kind = model::DeviceKind::Native;
    d.type = "OriginalSimpler";
    d.parameters.push_back({"VolumeAndPan/Volume", -8.56, -1, {}});
    d.plain["Globals/PlaybackMode"] = 1.0;
    model::SamplePart part;
    part.keyMin = 60;
    part.keyMax = 60;
    part.rootKey = 60;
    part.sample.relativePath = "does/not/exist.wav";
    d.sampleParts.push_back(part);

    std::vector<std::string> log;
    devices::BuildContext ctx;
    ctx.log = &log;
    engine::SampleCache cache;
    model::Song song;
    ctx.samples = &cache;
    ctx.song = &song;
    auto dev = devices::createDevice(d, ctx);
    REQUIRE(dev != nullptr);
    CHECK(dev->name() == "Simpler");
    CHECK(dev->isInstrument());
    CHECK_NEAR(dev->getParameter((int) devices::Simpler::Volume), -8.56, 1e-4);
    REQUIRE(log.size() == 1);
    CHECK(log[0].find("Simpler sample not found") != std::string::npos);
}

// ---------------------------------------------------------------- Racks (DEV-5)

namespace
{
// Test instrument: while any note is held it outputs a constant `level`; remembers every note-on.
class Gate final : public devices::DeviceProcessor
{
public:
    explicit Gate(float level) : params_{devices::makeParam("Level", "Level", 0.0f, 10.0f, level)}, level_(level) {}
    std::string name() const override { return "gate"; }
    bool isInstrument() const override { return true; }
    void prepare(double, int) override {}
    void reset() override { held_ = 0; }
    const std::vector<devices::ParameterInfo>& parameters() const override { return params_; }
    float getParameter(int) const override { return level_; }
    void setParameter(int, float v) override { level_ = v; }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        for (int i = 0; i < audio.getNumSamples(); ++i)
        {
            for (const auto m : midi)
                if (m.samplePosition == i)
                {
                    if (m.getMessage().isNoteOn())
                        ++held_, ++noteOns, lastNote = m.getMessage().getNoteNumber();
                    else if (m.getMessage().isNoteOff())
                        held_ = std::max(0, held_ - 1);
                }
            if (held_ > 0)
            {
                audio.addSample(0, i, level_);
                audio.addSample(1, i, level_);
            }
        }
    }
    int noteOns = 0, lastNote = -1;

private:
    std::vector<devices::ParameterInfo> params_;
    float level_;
    int held_ = 0;
};

float lastSample(devices::RackProcessor& rack, juce::MidiBuffer midi)
{
    juce::AudioBuffer<float> buf(2, 64);
    buf.clear();
    rack.process(buf, midi);
    return buf.getSample(0, 63);
}

devices::RackProcessor::Chain gateChain(float level, Gate** out = nullptr)
{
    devices::RackProcessor::Chain c;
    auto g = std::make_unique<Gate>(level);
    if (out)
        *out = g.get();
    c.devices.push_back(std::move(g));
    return c;
}
} // namespace

TEST_CASE("rack: macro sweeps the mapped parameter between its range")
{
    Gate* gate = nullptr;
    std::vector<devices::RackProcessor::Chain> chains;
    chains.push_back(gateChain(1.0f, &gate));
    devices::RackProcessor rack("r", true, false, std::move(chains));
    rack.setMacros({"Level"}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {{0, gate, 0, 0.0f, 2.0f}}, 4);
    rack.prepare(44100.0, 64);

    CHECK_NEAR(gate->getParameter(0), 0.0, 1e-6); // macro 0 starts at 0
    rack.setParameter(0, 127.0f);
    CHECK_NEAR(gate->getParameter(0), 2.0, 1e-5);
    rack.setParameter(0, 63.5f);
    CHECK_NEAR(gate->getParameter(0), 1.0, 1e-5);
    CHECK(rack.parameters().size() == 17);
    CHECK(rack.parameters()[0].name == "Level");
    CHECK(rack.displayParameters().size() == 5); // four visible macros and the chain selector

    // The audio follows: macro at max -> level 2.
    rack.setParameter(0, 127.0f);
    juce::MidiBuffer on;
    on.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    CHECK_NEAR(lastSample(rack, on), 2.0, 1e-5);
}

TEST_CASE("rack: key/velocity zones and the chain selector decide which chains play")
{
    std::vector<devices::RackProcessor::Chain> chains;
    chains.push_back(gateChain(1.0f));
    chains[0].keyMax = 59;
    chains[0].selMin = chains[0].selMax = 0;
    chains.push_back(gateChain(10.0f));
    chains[1].keyMin = 60;
    chains[1].velMin = 50;
    chains[1].selMin = chains[1].selMax = 1;
    devices::RackProcessor rack("zones", true, false, std::move(chains));
    rack.prepare(44100.0, 64);

    juce::MidiBuffer low, high, quiet;
    low.addEvent(juce::MidiMessage::noteOn(1, 40, (juce::uint8) 100), 0);
    high.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 100), 0);
    quiet.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 20), 0);

    // Selector 0: only chain 1 is active, and only for keys <= 59.
    CHECK_NEAR(lastSample(rack, low), 1.0, 1e-6);
    rack.reset();
    CHECK_NEAR(lastSample(rack, high), 0.0, 1e-6); // chain 2 is not selected, chain 1 ignores key 72

    // Selector 1: chain 2 only, needs velocity >= 50.
    rack.setParameter(devices::RackProcessor::kChainSelector, 1.0f);
    CHECK_NEAR(lastSample(rack, high), 10.0, 1e-5);
    std::vector<devices::RackProcessor::Chain> again;
    again.push_back(gateChain(10.0f));
    again[0].velMin = 50;
    again[0].keyMin = 60;
    devices::RackProcessor r2("vel", true, false, std::move(again));
    r2.prepare(44100.0, 64);
    CHECK_NEAR(lastSample(r2, quiet), 0.0, 1e-6); // velocity 20 < 50
}

TEST_CASE("drum rack: pads in a choke group cut each other off")
{
    Gate *a = nullptr, *b = nullptr;
    std::vector<devices::RackProcessor::Chain> chains;
    chains.push_back(gateChain(1.0f, &a));
    chains[0].receivingNote = 36;
    chains[0].sendingNote = 60;
    chains[0].chokeGroup = 1;
    chains.push_back(gateChain(1.0f, &b));
    chains[1].receivingNote = 37;
    chains[1].sendingNote = 60;
    chains[1].chokeGroup = 1;
    chains.push_back(gateChain(100.0f)); // pad 3 is not in the group
    chains[2].receivingNote = 38;
    chains[2].sendingNote = 60;
    devices::RackProcessor rack("drums", true, true, std::move(chains));
    rack.prepare(44100.0, 64);

    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0);    // pad A rings
    CHECK_NEAR(lastSample(rack, m), 1.0, 1e-6);
    juce::MidiBuffer m2;
    m2.addEvent(juce::MidiMessage::noteOn(1, 37, (juce::uint8) 100), 0);   // pad B chokes pad A
    CHECK_NEAR(lastSample(rack, m2), 1.0, 1e-6); // only B sounds now (A would make it 2.0)
    CHECK(a->noteOns == 1);
    CHECK(b->noteOns == 1);
    CHECK(b->lastNote == 60); // transposed to the pad's sending note
}

TEST_CASE("rack: zones, selector, macros and choke groups are read from the .als and built")
{
    auto r = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(r);
    const auto& rackModel = r.song->returns[0].devices[1];
    CHECK(rackModel.kind == model::DeviceKind::Rack);
    CHECK(rackModel.macroNames[0] == "Boost");
    CHECK(rackModel.numVisibleMacros == 4);
    REQUIRE(rackModel.chains.size() == 1);
    const auto& chain = rackModel.chains[0];
    CHECK(chain.keyMin == 24);
    CHECK(chain.keyMax == 72);
    CHECK(chain.velMin == 10);
    CHECK(chain.velMax == 100);
    CHECK(chain.selMax == 63);
    REQUIRE(chain.devices.size() == 1);
    const auto& eqParam = chain.devices[0].parameters;
    bool macroFound = false;
    for (const auto& p : eqParam)
        if (p.path == "GlobalGain")
        {
            macroFound = true;
            CHECK(p.macroIndex == 0);
            CHECK_NEAR(p.rangeMin, -12.0, 1e-9);
            CHECK_NEAR(p.rangeMax, 12.0, 1e-9);
            CHECK(! p.midi.mapped()); // a macro, not a hardware mapping
        }
    CHECK(macroFound);

    devices::BuildContext ctx;
    auto dev = devices::createDevice(rackModel, ctx);
    REQUIRE(dev != nullptr);
    CHECK(dev->parameters()[0].name == "Boost");
    CHECK_NEAR(dev->getParameter(0), 64.0, 1e-6);
    auto* rack = dynamic_cast<devices::RackProcessor*>(dev.get());
    REQUIRE(rack != nullptr);
    dev->prepare(44100.0, 64);
    auto* eq = rack->chains()[0].devices[0].get();
    int gain = -1;
    for (size_t i = 0; i < eq->parameters().size(); ++i)
        if (eq->parameters()[i].id == "GlobalGain")
            gain = (int) i;
    REQUIRE(gain >= 0);
    CHECK_NEAR(eq->getParameter(gain), -12.0 + 24.0 * 64.0 / 127.0, 1e-3); // macro default applied at build
    dev->setParameter(0, 127.0f);
    CHECK_NEAR(eq->getParameter(gain), 12.0, 1e-3);
}

TEST_CASE("utility: bass mono, dc filter and mid/side balance")
{
    auto render = [](Utility& u, std::function<float(int, int)> gen, int n = 8192) {
        u.prepare(48000.0, n);
        juce::AudioBuffer<float> b(2, n);
        for (int i = 0; i < n; ++i)
        {
            b.setSample(0, i, gen(0, i));
            b.setSample(1, i, gen(1, i));
        }
        juce::MidiBuffer m;
        u.process(b, m);
        return b;
    };
    auto sineAt = [](float hz, float phase) { return [=](int, int i) { return 0.5f * std::sin(2.0f * 3.14159265f * hz * (float) i / 48000.0f + phase); }; };

    // 60 Hz hard-left signal: with Bass Mono at 120 Hz it ends up equally in both channels.
    Utility bm;
    bm.setParameter(Utility::BassMono, 1.0f);
    bm.setParameter(Utility::BassMonoFrequency, 120.0f);
    auto b = render(bm, [&](int ch, int i) { return ch == 0 ? sineAt(60.0f, 0.0f)(0, i) : 0.0f; });
    CHECK(std::fabs(rmsTail(b, 0) - rmsTail(b, 1)) < 0.12 * rmsTail(b, 0)); // the high-pass leg leaves a little 60 Hz on the left
    // 4 kHz hard-left stays hard-left (above the crossover).
    Utility bm2;
    bm2.setParameter(Utility::BassMono, 1.0f);
    auto b2 = render(bm2, [&](int ch, int i) { return ch == 0 ? sineAt(4000.0f, 0.0f)(0, i) : 0.0f; });
    CHECK(rmsTail(b2, 1) < 0.05 * rmsTail(b2, 0));

    // DC filter removes a constant offset.
    Utility dc;
    dc.setParameter(Utility::DcFilter, 1.0f);
    auto b3 = render(dc, [](int, int) { return 0.4f; }, 48000);
    CHECK(std::fabs(b3.getSample(0, 47999)) < 0.01f);

    // Mid/Side balance 0 keeps only the mids: a hard-panned signal becomes centred half-level. It applies only in
    // Mid/Side mode (MidSideBalanceOn), where the width knob is replaced, as in the reference.
    Utility ms;
    ms.setParameter(Utility::MidSideOn, 1.0f);
    ms.setParameter(Utility::MidSideBalance, 0.0f);
    ms.setParameter(Utility::StereoWidth, 4.0f); // ignored in Mid/Side mode
    auto b4 = render(ms, [&](int ch, int i) { return ch == 0 ? sineAt(1000.0f, 0.0f)(0, i) : 0.0f; });
    CHECK(std::fabs(rmsTail(b4, 0) - rmsTail(b4, 1)) < 0.01 * rmsTail(b4, 0) + 1e-6);
    Utility widthMode;
    widthMode.setParameter(Utility::MidSideBalance, 0.0f); // ignored while the knob is Width
    auto b5 = render(widthMode, [&](int ch, int i) { return ch == 0 ? sineAt(1000.0f, 0.0f)(0, i) : 0.0f; });
    CHECK(rmsTail(b5, 1) < 0.01 * rmsTail(b5, 0));

    // Bass Mono audition plays only the summed low band.
    Utility audition;
    audition.setParameter(Utility::BassMono, 1.0f);
    audition.setParameter(Utility::BassMonoAudition, 1.0f);
    auto high = render(audition, [&](int, int i) { return sineAt(4000.0f, 0.0f)(0, i); });
    CHECK(rmsTail(high, 0) < 0.01f);
    Utility audition2;
    audition2.setParameter(Utility::BassMono, 1.0f);
    audition2.setParameter(Utility::BassMonoAudition, 1.0f);
    auto low = render(audition2, [&](int, int i) { return sineAt(40.0f, 0.0f)(0, i); });
    CHECK(rmsTail(low, 0) > 0.3f);
    Utility auditionOff; // audition does nothing without Bass Mono
    auditionOff.setParameter(Utility::BassMonoAudition, 1.0f);
    auto plain = render(auditionOff, [&](int, int i) { return sineAt(4000.0f, 0.0f)(0, i); });
    CHECK(rmsTail(plain, 0) > 0.3f);
}

TEST_CASE("utility: Mid/Side mode and audition load from and save to their plain leaves; gain reaches +35 dB")
{
    auto model = devices::nativeDeviceModel("StereoGain");
    CHECK(model.plain.count("MidSideBalanceOn") == 1);
    CHECK(model.plain.count("BassMonoAudition") == 1);
    model.plain["MidSideBalanceOn"] = 1;
    devices::BuildContext build;
    auto processor = devices::createDevice(model, build);
    REQUIRE(processor);
    CHECK_NEAR(processor->getParameter(Utility::MidSideOn), 1, 0);
    processor->setParameter(Utility::Gain, 56.23f);
    CHECK_NEAR(processor->getParameter(Utility::Gain), 56.23, 1e-4);
}

TEST_CASE("eq eight: left/right and mid/side modes use band set A and B on different channels")
{
    auto makeEq = [](int mode) {
        auto eq = std::make_unique<EqEight>();
        eq->prepare(48000, 512);
        for (int b = 0; b < EqEight::kBands; ++b)
        {
            eq->setParameter(EqEight::bandParam(b, EqEight::On), b == 3 ? 1.0f : 0.0f);
            eq->setParameter(EqEight::bandParamB(b, EqEight::On), 0.0f);
        }
        eq->setParameter(EqEight::bandParam(3, EqEight::Type), 3.0f);
        eq->setParameter(EqEight::bandParam(3, EqEight::Freq), 1000.0f);
        eq->setParameter(EqEight::bandParam(3, EqEight::GainDb), 12.0f);
        eq->setParameter(EqEight::bandParam(3, EqEight::Q), 1.0f);
        eq->setParameter(EqEight::kMode, (float) mode);
        return eq;
    };
    auto stereoSine = [](float l, float r) {
        auto b = sine(1000, 48000, 9600);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = b.getSample(0, i);
            b.setSample(0, i, v * l);
            b.setSample(1, i, v * r);
        }
        return b;
    };
    juce::MidiBuffer midi;

    // Stereo: A on both channels.
    auto st = makeEq(0);
    auto b = stereoSine(1, 1);
    const float ref = rmsTail(b, 0);
    st->process(b, midi);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 0) / ref), 12.0, 0.3);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 1) / ref), 12.0, 0.3);

    // Left/Right: A boosts the left channel only (set B is empty).
    auto lr = makeEq(1);
    b = stereoSine(1, 1);
    lr->process(b, midi);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 0) / ref), 12.0, 0.3);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 1) / ref), 0.0, 0.3);

    // Mid/Side: A boosts mid only. A mono signal (pure mid) gets +12 dB, an anti-phase one (pure side) none.
    auto ms = makeEq(2);
    b = stereoSine(1, 1);
    ms->process(b, midi);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 0) / ref), 12.0, 0.3);
    auto ms2 = makeEq(2);
    b = stereoSine(1, -1);
    ms2->process(b, midi);
    CHECK_NEAR(juce::Decibels::gainToDecibels(rmsTail(b, 0) / ref), 0.0, 0.3);

    // Adaptive Q narrows a boosted band: response 2 octaves away is lower with it on.
    auto plain = makeEq(0);
    auto adaptive = makeEq(0);
    adaptive->setParameter(EqEight::kAdaptiveQ, 1.0f);
    adaptive->setParameter(EqEight::kAdaptiveQFactor, 1.3f);
    CHECK(adaptive->responseAt(4000.0) < plain->responseAt(4000.0));
    CHECK_NEAR(adaptive->responseAt(1000.0), plain->responseAt(1000.0), 0.05); // the centre gain is unchanged
}

// ---------------------------------------------------------------- Dynamics (DEV-7, DEV-12)

#include "devices/native/Compressor.h"
#include "devices/native/Limiter.h"

namespace
{
// Processes `seconds` of a 1 kHz sine of the given amplitude in 512-sample blocks.
std::vector<float> runSine(devices::DeviceProcessor& dev, float amp, double seconds = 1.0, double sr = 48000.0)
{
    dev.prepare(sr, 512);
    std::vector<float> out;
    const int total = (int) (seconds * sr);
    juce::MidiBuffer m;
    for (int pos = 0; pos < total; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        for (int i = 0; i < 512; ++i)
        {
            const float v = amp * std::sin(2.0f * 3.14159265f * 1000.0f * (float) (pos + i) / (float) sr);
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
        dev.process(b, m);
        for (int i = 0; i < 512; ++i)
            out.push_back(b.getSample(0, i));
    }
    return out;
}
double peakOf(const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0;
    for (size_t i = from; i < to && i < x.size(); ++i)
        p = std::max(p, (double) std::fabs(x[i]));
    return p;
}
} // namespace

TEST_CASE("compressor: static curve, attack/release, makeup and dry/wet")
{
    // The gain computer: 4:1 above -20 dB.
    CHECK_NEAR(devices::compressorCurveDb(-6.0f, -20.0f, 4.0f, 0.0f), -16.5, 1e-4);
    CHECK_NEAR(devices::compressorCurveDb(-30.0f, -20.0f, 4.0f, 0.0f), -30.0, 1e-4);
    CHECK_NEAR(devices::compressorCurveDb(-20.0f, -20.0f, 4.0f, 6.0f), -20.0 + (0.25 - 1.0) * 9.0 / 12.0, 1e-4); // inside the knee

    devices::Compressor c;
    c.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    c.setParameter(devices::Compressor::Ratio, 4.0f);
    c.setParameter(devices::Compressor::Knee, 0.0f);
    c.setParameter(devices::Compressor::Attack, 1.0f);
    c.setParameter(devices::Compressor::Release, 100.0f);
    auto out = runSine(c, 0.5f);
    // -6 dB in -> -16.5 dB out: 10.5 dB of reduction once the attack has settled.
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000) / 0.5), -10.5, 0.6);
    CHECK(c.gainReductionDb() < -9.0f);

    devices::Compressor mk;
    mk.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    mk.setParameter(devices::Compressor::Ratio, 4.0f);
    mk.setParameter(devices::Compressor::Knee, 0.0f);
    mk.setParameter(devices::Compressor::Attack, 1.0f);
    mk.setParameter(devices::Compressor::Release, 100.0f);
    mk.setParameter(devices::Compressor::Gain, 6.0f);
    out = runSine(mk, 0.5f);
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000) / 0.5), -4.5, 0.6);

    devices::Compressor dry;
    dry.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    dry.setParameter(devices::Compressor::Ratio, 10.0f);
    dry.setParameter(devices::Compressor::DryWet, 0.0f);
    out = runSine(dry, 0.5f);
    CHECK_NEAR(peakOf(out, 24000, 48000), 0.5, 0.01); // 0 % wet == untouched

    devices::Compressor quiet;
    quiet.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    out = runSine(quiet, 0.05f); // -26 dB: below threshold
    CHECK_NEAR(peakOf(out, 24000, 48000), 0.05, 0.003);
}

TEST_CASE("compressor: measured makeup law and infinite ratio (DEV-22b)")
{
    // Auto makeup = 0.7 x -threshold x (1 - 1/ratio), knee-independent (Brick Wall 0.699, Brute 0.700 measured).
    CHECK_NEAR(devices::compressorAutoMakeupDb(-20.0f, 4.0f), 10.5, 1e-4);
    CHECK_NEAR(devices::compressorAutoMakeupDb(-31.93f, 12.6f), 20.57, 0.01);
    CHECK_NEAR(devices::compressorAutoMakeupDb(0.0f, 2.0f), 0.0, 1e-6);
    CHECK_NEAR(devices::compressorAutoMakeupDb(-9.7f, std::numeric_limits<float>::infinity()), 6.79, 1e-3);

    // The stored ratio FLT_MAX clamps to the 100:1 maximum, which acts as infinite: -6 dB in -> threshold out.
    devices::Compressor inf;
    inf.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    inf.setParameter(devices::Compressor::Ratio, 3.4e38f);
    inf.setParameter(devices::Compressor::Knee, 0.0f);
    inf.setParameter(devices::Compressor::Attack, 0.01f);
    inf.setParameter(devices::Compressor::Release, 1000.0f);
    auto out = runSine(inf, 0.5f);
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000)), -20.0, 0.1);

    devices::Compressor mk; // makeup on: -20 dB threshold, 4:1 -> output peak -16.5 + 10.5 = -6 dB for a -6 dB input
    mk.setParameter(devices::Compressor::Threshold, juce::Decibels::decibelsToGain(-20.0f));
    mk.setParameter(devices::Compressor::Ratio, 4.0f);
    mk.setParameter(devices::Compressor::Knee, 0.0f);
    mk.setParameter(devices::Compressor::Attack, 0.01f);
    mk.setParameter(devices::Compressor::Release, 1000.0f);
    mk.setParameter(devices::Compressor::GainCompensation, 1.0f);
    out = runSine(mk, 0.5f);
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000)), -6.02, 0.1);
}

TEST_CASE("compressor: look-ahead reports its latency and delays the signal")
{
    devices::Compressor c;
    c.setParameter(devices::Compressor::LookAhead, 2.0f); // 10 ms
    c.prepare(48000.0, 512);
    CHECK(c.latencySamples() == 480);
    auto out = runSine(c, 0.1f, 0.2); // below threshold: only the delay remains
    CHECK(peakOf(out, 0, 400) < 1e-6);   // silence while the delay line fills
    CHECK(peakOf(out, 500, 1500) > 0.09);
}

TEST_CASE("glue compressor: ratio/attack tables, range limit and reduction")
{
    CHECK_NEAR(devices::GlueCompressor::ratioValue(2), 10.0, 1e-6);
    CHECK_NEAR(devices::GlueCompressor::attackMs(3), 1.0, 1e-6);
    CHECK(devices::GlueCompressor::releaseMs(5) < 0.0f); // Auto

    devices::GlueCompressor g;
    g.setParameter(devices::GlueCompressor::Threshold, -20.0f);
    g.setParameter(devices::GlueCompressor::Ratio, 1.0f); // 4:1
    g.setParameter(devices::GlueCompressor::Attack, 0.0f);
    g.setParameter(devices::GlueCompressor::Release, 0.0f);
    auto out = runSine(g, 0.5f);
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000) / 0.5), -10.4, 0.8);

    devices::GlueCompressor limited;
    limited.setParameter(devices::GlueCompressor::Threshold, -40.0f);
    limited.setParameter(devices::GlueCompressor::Ratio, 2.0f);
    limited.setParameter(devices::GlueCompressor::Range, 5.0f);
    limited.setParameter(devices::GlueCompressor::Attack, 0.0f);
    out = runSine(limited, 0.5f);
    CHECK_NEAR(juce::Decibels::gainToDecibels(peakOf(out, 24000, 48000) / 0.5), -5.0, 0.5); // range caps the reduction
}

TEST_CASE("limiter: never exceeds the ceiling, passes quiet audio, reports look-ahead latency")
{
    devices::Limiter lim;
    lim.setParameter(devices::Limiter::Ceiling, -6.0f);
    auto out = runSine(lim, 2.0f); // +6 dBFS in
    const double ceiling = juce::Decibels::decibelsToGain(-6.0);
    CHECK(peakOf(out, 0, out.size()) <= ceiling + 1e-4);          // hard guarantee, even on the first peak
    CHECK(peakOf(out, 24000, 48000) > ceiling * 0.85);            // but not crushed far below it
    CHECK(lim.latencySamples() == 144);                           // 3 ms at 48 kHz

    devices::Limiter pass;
    pass.setParameter(devices::Limiter::Ceiling, -0.3f);
    out = runSine(pass, 0.25f);
    CHECK_NEAR(peakOf(out, 24000, 48000), 0.25, 0.005);

    devices::Limiter gain;
    gain.setParameter(devices::Limiter::Gain, 6.0f);
    gain.setParameter(devices::Limiter::Ceiling, 0.0f);
    out = runSine(gain, 0.25f);
    CHECK_NEAR(peakOf(out, 24000, 48000), 0.25 * 1.995, 0.01);
}

// ---------------------------------------------------------------- EQ Three, Multiband Dynamics (DEV-11, DEV-10)

#include "devices/native/Bands.h"

namespace
{
// Level (linear rms) of a sine at `hz` after the device, measured over the second half of 1 s.
double responseOf(devices::DeviceProcessor& dev, float hz, float amp = 0.25f)
{
    const double sr = 48000.0;
    dev.prepare(sr, 512);
    juce::MidiBuffer m;
    double sum = 0.0;
    int count = 0;
    for (int pos = 0; pos < 48000; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        for (int i = 0; i < 512; ++i)
        {
            const float v = amp * std::sin(2.0f * 3.14159265f * hz * (float) (pos + i) / (float) sr);
            b.setSample(0, i, v);
            b.setSample(1, i, v);
        }
        dev.process(b, m);
        if (pos >= 24000)
            for (int i = 0; i < 512; ++i, ++count)
                sum += (double) b.getSample(0, i) * b.getSample(0, i);
    }
    return std::sqrt(sum / count) / (amp / std::sqrt(2.0)); // 1.0 == unity gain
}
} // namespace

TEST_CASE("eq three: measured Butterworth tree (DEV-22b), +3 dB bumps at the crossovers, kills, 48 dB slope")
{
    // Expected values: digital Butterworth tree low = LP(lo), mid = LP(hi)(HP(lo)), high = HP(hi)(HP(lo)),
    // evaluated independently (scipy) at 48 kHz with the default 250 Hz / 2.5 kHz crossovers.
    devices::EqThree flat;
    CHECK_NEAR(responseOf(flat, 40.0f), 1.0007, 0.01);
    CHECK_NEAR(responseOf(flat, 250.0f), 1.4024, 0.01); // not flat: the bands are -3 dB and in phase at a crossover
    CHECK_NEAR(responseOf(flat, 800.0f), 1.0164, 0.01);
    CHECK_NEAR(responseOf(flat, 2500.0f), 1.4141, 0.01);
    CHECK_NEAR(responseOf(flat, 9000.0f), 1.0037, 0.01);

    devices::EqThree kill;
    kill.setParameter(devices::EqThree::LowOn, 0.0f);
    CHECK(responseOf(kill, 60.0f) < 0.01);
    CHECK_NEAR(responseOf(kill, 1000.0f), 1.0245, 0.01);
    CHECK_NEAR(responseOf(kill, 9000.0f), 1.0037, 0.01);

    devices::EqThree minimum; // the knob's -70 dB minimum is a kill in the reference
    minimum.setParameter(devices::EqThree::GainLow, 0.00031622799f);
    CHECK(responseOf(minimum, 60.0f) < 0.01);

    devices::EqThree mid;
    mid.setParameter(devices::EqThree::GainMid, 0.5f); // -6 dB
    CHECK_NEAR(responseOf(mid, 800.0f), 0.5165, 0.01);
    CHECK_NEAR(responseOf(mid, 40.0f), 1.0, 0.01);

    devices::EqThree gentle, steep;
    gentle.setParameter(devices::EqThree::GainLow, 0.0f);
    steep.setParameter(devices::EqThree::GainLow, 0.0f);
    steep.setParameter(devices::EqThree::Slope, 1.0f);
    CHECK_NEAR(responseOf(gentle, 500.0f), 0.9996, 0.01);
    CHECK_NEAR(responseOf(steep, 500.0f), 1.0, 0.01);
    CHECK_NEAR(responseOf(steep, 125.0f), 0.0039, 0.002); // one octave below: 8th-order Butterworth, ~-48 dB
    devices::EqThree steepFlat;
    steepFlat.setParameter(devices::EqThree::Slope, 1.0f);
    CHECK_NEAR(responseOf(steepFlat, 250.0f), 1.3687, 0.01);

    devices::EqThreeSplit split;
    split.setup(48000.0, 250.0, 2500.0, false);
    CHECK_NEAR(split.response(250.0, 48000.0, 1, 1, 1), 1.4024, 1e-3);
    CHECK_NEAR(split.response(800.0, 48000.0, 1, 0.5f, 1), 0.5165, 1e-3);
}

TEST_CASE("multiband dynamics: gain curve, neutral pass-through, per-band compression")
{
    using M = devices::MultibandDynamics;
    // Above-threshold compression: ratio value -0.5 == slope 0.5 (2:1). 10 dB over -> 5 dB reduction.
    CHECK_NEAR(M::bandGainDb(-10.0f, -60.0f, -20.0f, 0.0f, -0.5f, 0.0f), -5.0, 1e-4);
    CHECK_NEAR(M::bandGainDb(-30.0f, -60.0f, -20.0f, 0.0f, -0.5f, 0.0f), 0.0, 1e-4);          // inside the window
    // Below ratios (measured sign, DEV-22b): positive = upward compression, negative = downward expansion.
    CHECK_NEAR(M::bandGainDb(-40.0f, -30.0f, -10.0f, 0.5f, 0.0f, 0.0f), 5.0, 1e-4);           // upward compression
    CHECK_NEAR(M::bandGainDb(-40.0f, -30.0f, -10.0f, -0.5f, 0.0f, 0.0f), -5.0, 1e-4);         // downward expansion
    CHECK_NEAR(M::bandGainDb(-40.0f, -30.0f, -10.0f, -3.0f, 0.0f, 0.0f), -30.0, 1e-4);        // 1:4 expansion (range -3)
    // The below-threshold action fades out between -54 and -58 dB, so silence is never boosted.
    CHECK_NEAR(M::bandGainDb(-56.0f, -50.0f, -10.0f, 1.0f, 0.0f, 0.0f), 3.0, 1e-4);           // half weight
    CHECK_NEAR(M::bandGainDb(-120.0f, -50.0f, -10.0f, 1.0f, 0.0f, 0.0f), 0.0, 1e-4);
    CHECK_NEAR(M::bandGainDb(-5.0f, -60.0f, -20.0f, 0.0f, 0.5f, 0.0f), 7.5, 1e-4);            // expansion above

    M neutral;
    for (float hz : {60.0f, 500.0f, 5000.0f})
        CHECK_NEAR(responseOf(neutral, hz), 1.0, 0.05);

    // Compress only the mid band hard: a 1 kHz tone drops, 60 Hz and 9 kHz stay.
    M comp;
    comp.setParameter(M::bandParam(M::Mid, M::AboveThreshold), -40.0f);
    comp.setParameter(M::bandParam(M::Mid, M::AboveRatio), -0.8f); // 5:1
    comp.setParameter(M::bandParam(M::Mid, M::BandAttack), 1.0f);
    comp.setParameter(M::bandParam(M::Mid, M::BandRelease), 50.0f);
    const double mid = responseOf(comp, 1000.0f);
    CHECK(mid < 0.5);
    CHECK(comp.bandMeterDb(M::Mid) < -6.0f); // (the meter shows the last block, so read it right after the run)
    CHECK_NEAR(responseOf(comp, 60.0f), 1.0, 0.06);
    CHECK_NEAR(responseOf(comp, 9000.0f), 1.0, 0.06);

    // A deactivated band is not processed.
    M off;
    off.setParameter(M::bandParam(M::Mid, M::AboveThreshold), -40.0f);
    off.setParameter(M::bandParam(M::Mid, M::AboveRatio), -0.8f);
    off.setParameter(M::bandParam(M::Mid, M::BandActive), 0.0f);
    CHECK_NEAR(responseOf(off, 1000.0f), 1.0, 0.05);

    // Output gain applies to everything.
    M louder;
    louder.setParameter(M::OutputGain, 6.0f);
    CHECK_NEAR(responseOf(louder, 500.0f), 1.995, 0.1);
}

// ---------------------------------------------------------------- Delay, Auto Filter (DEV-8, DEV-9)

#include "devices/native/AutoFilter.h"
#include "devices/native/Delay.h"

namespace
{
// Feeds one impulse (left & right) at sample 0 and returns the two output channels.
std::pair<std::vector<float>, std::vector<float>> impulseResponse(devices::DeviceProcessor& dev, int total, float inL = 1.0f, float inR = 1.0f)
{
    dev.prepare(48000.0, 512);
    std::vector<float> l, r;
    juce::MidiBuffer m;
    for (int pos = 0; pos < total; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        b.clear();
        if (pos == 0)
        {
            b.setSample(0, 0, inL);
            b.setSample(1, 0, inR);
        }
        dev.process(b, m);
        for (int i = 0; i < 512; ++i)
        {
            l.push_back(b.getSample(0, i));
            r.push_back(b.getSample(1, i));
        }
    }
    return {l, r};
}
int argMax(const std::vector<float>& x, int from, int to)
{
    int best = from;
    for (int i = from; i < to && i < (int) x.size(); ++i)
        if (std::fabs(x[(size_t) i]) > std::fabs(x[(size_t) best]))
            best = i;
    return best;
}
} // namespace

namespace
{
// A Delay in the plain configuration most tests want: free time, no filter, classic mix, fully wet, both sides set.
void plainDelay(devices::Delay& d, float leftMs, float rightMs, float feedback, float wet = 1.0f)
{
    using D = devices::Delay;
    d.setParameter(D::SyncL, 0.0f);
    d.setParameter(D::SyncR, 0.0f);
    d.setParameter(D::TimeL, leftMs * 0.001f); // DelayLine_TimeL/R: seconds (measured, DEV-22b)
    d.setParameter(D::TimeR, rightMs * 0.001f);
    d.setParameter(D::Feedback, feedback);
    d.setParameter(D::FilterOn, 0.0f);
    d.setParameter(D::DryWetMode, 0.0f);
    d.setParameter(D::DryWet, wet);
}
} // namespace

TEST_CASE("delay: free time, feedback decay, dry/wet")
{
    devices::Delay d;
    plainDelay(d, 10.0f, 10.0f, 0.5f);
    auto [l, r] = impulseResponse(d, 4096);
    CHECK(std::fabs(l[0]) < 1e-6);                      // fully wet: the dry impulse is gone
    CHECK(argMax(l, 100, 700) >= 478);
    CHECK(argMax(l, 100, 700) <= 482);                   // first echo at 10 ms = 480 samples
    const float first = std::fabs(l[(size_t) argMax(l, 100, 700)]);
    const float second = std::fabs(l[(size_t) argMax(l, 800, 1200)]);
    CHECK_NEAR(first, 1.0, 0.05);
    CHECK_NEAR(second / first, 0.5, 0.05);               // feedback 0.5
    CHECK(std::fabs(r[(size_t) argMax(r, 100, 700)]) > 0.9);

    devices::Delay half;
    plainDelay(half, 10.0f, 10.0f, 0.0f, 0.5f);
    auto [l2, r2] = impulseResponse(half, 1024);
    CHECK_NEAR(l2[0], 0.5, 1e-3); // classic law: 50 % dry
    CHECK_NEAR(l2[(size_t) argMax(l2, 100, 700)], 0.5, 0.05);

    // Equal-loudness law (Live's default): constant power, so 50 % is 0.707 of each.
    devices::Delay equal;
    plainDelay(equal, 10.0f, 10.0f, 0.0f, 0.5f);
    equal.setParameter(devices::Delay::DryWetMode, 1.0f);
    auto [l3, r3] = impulseResponse(equal, 1024);
    CHECK_NEAR(l3[0], 0.7071, 1e-3);
    CHECK_NEAR(l3[(size_t) argMax(l3, 100, 700)], 0.7071, 0.05);
}

TEST_CASE("delay: tempo sync follows the host tempo, ping-pong alternates channels")
{
    devices::Delay s;
    s.setParameter(devices::Delay::SyncL, 1.0f);
    s.setParameter(devices::Delay::SyncedL, 3.0f); // index 3 == 4 sixteenths == 1 beat
    s.setParameter(devices::Delay::Feedback, 0.0f);
    s.setParameter(devices::Delay::FilterOn, 0.0f);
    s.setParameter(devices::Delay::DryWetMode, 0.0f);
    s.setParameter(devices::Delay::DryWet, 1.0f);
    devices::HostInfo host;
    host.tempo = 120.0; // one beat = 500 ms = 24000 samples
    s.setHostInfo(host);
    auto [l, r] = impulseResponse(s, 30000);
    const int at = argMax(l, 1000, 30000);
    CHECK(at >= 23990);
    CHECK(at <= 24010);
    host.tempo = 60.0;
    devices::Delay slow;
    slow.setParameter(devices::Delay::SyncL, 1.0f);
    slow.setParameter(devices::Delay::SyncedL, 3.0f);
    slow.setParameter(devices::Delay::Feedback, 0.0f);
    slow.setParameter(devices::Delay::FilterOn, 0.0f);
    slow.setParameter(devices::Delay::DryWetMode, 0.0f);
    slow.setParameter(devices::Delay::DryWet, 1.0f);
    slow.setHostInfo(host);
    auto [l2, r2] = impulseResponse(slow, 52000);
    const int at2 = argMax(l2, 1000, 52000);
    CHECK(at2 >= 47990);
    CHECK(at2 <= 48010); // half the tempo, twice the time

    devices::Delay pp;
    plainDelay(pp, 20.0f, 20.0f, 0.8f);
    pp.setParameter(devices::Delay::PingPong, 1.0f);
    auto [pl, pr] = impulseResponse(pp, 4096);
    CHECK(std::fabs(pl[(size_t) argMax(pl, 800, 1100)]) > 0.3); // first echo on the left (20 ms = 960 samples)
    CHECK(std::fabs(pr[(size_t) argMax(pr, 800, 1100)]) < 0.01);
    CHECK(std::fabs(pr[(size_t) argMax(pr, 1800, 2100)]) > 0.1); // second echo on the right
}

TEST_CASE("auto filter: low-pass / high-pass response, slope, lfo modulation")
{
    using A = devices::AutoFilter;
    const float pitch1k = 69.0f + 12.0f * std::log2(1000.0f / 440.0f);
    CHECK_NEAR(A::pitchToHz(69.0f), 440.0, 1e-3);
    CHECK_NEAR(A::pitchToHz(pitch1k), 1000.0, 0.5);

    A lp;
    lp.setParameter(A::Cutoff, pitch1k);
    CHECK_NEAR(responseOf(lp, 100.0f), 1.0, 0.05);
    const double at4k = responseOf(lp, 4000.0f);
    CHECK(at4k < 0.12); // 2 octaves above the cutoff, 12 dB/oct: about -24 dB

    A lp24;
    lp24.setParameter(A::Cutoff, pitch1k);
    lp24.setParameter(A::Slope, 1.0f);
    CHECK(responseOf(lp24, 4000.0f) < at4k * 0.3);

    A hp;
    hp.setParameter(A::FilterType, 1.0f);
    hp.setParameter(A::Cutoff, pitch1k);
    CHECK(responseOf(hp, 100.0f) < 0.15);
    CHECK_NEAR(responseOf(hp, 8000.0f), 1.0, 0.1);

    A notch;
    notch.setParameter(A::FilterType, 3.0f);
    notch.setParameter(A::Cutoff, pitch1k);
    CHECK(responseOf(notch, 1000.0f) < 0.2);
    CHECK_NEAR(responseOf(notch, 100.0f), 1.0, 0.1);

    // An LFO sweeping the cutoff makes the level of a 1 kHz tone wobble over time.
    A mod;
    mod.setParameter(A::Cutoff, pitch1k);
    mod.setParameter(A::LfoAmount, 1.0f);
    mod.setParameter(A::LfoFrequency, 4.0f);
    mod.prepare(48000.0, 512);
    juce::MidiBuffer m;
    float lo = 10.0f, hi = 0.0f;
    for (int pos = 0; pos < 24000; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        for (int i = 0; i < 512; ++i)
            b.setSample(0, i, 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * (float) (pos + i) / 48000.0f)), b.setSample(1, i, b.getSample(0, i));
        mod.process(b, m);
        const float level = b.getMagnitude(0, 0, 512);
        if (pos > 4096)
        {
            lo = std::min(lo, level);
            hi = std::max(hi, level);
        }
    }
    CHECK(hi > lo * 1.5f);
}

// ---------------------------------------------------------------- Reverb (DEV-6)

#include "devices/native/Reverb.h"

namespace
{
// Reverberation time (s) estimated with Schroeder backward integration: fit of the -5..-25 dB
// part of the energy decay curve, extrapolated to -60 dB.
double estimateRt60(const std::vector<float>& ir, double sr)
{
    std::vector<double> edc(ir.size());
    double acc = 0.0;
    for (size_t i = ir.size(); i-- > 0;)
    {
        acc += (double) ir[i] * ir[i];
        edc[i] = acc;
    }
    const double total = edc[0];
    int t5 = -1, t25 = -1;
    for (size_t i = 0; i < edc.size(); ++i)
    {
        const double db = 10.0 * std::log10(std::max(edc[i] / total, 1e-12));
        if (t5 < 0 && db <= -5.0)
            t5 = (int) i;
        if (t25 < 0 && db <= -25.0)
        {
            t25 = (int) i;
            break;
        }
    }
    if (t5 < 0 || t25 < 0 || t25 <= t5)
        return -1.0;
    return 3.0 * (double) (t25 - t5) / sr; // 20 dB took (t25 - t5), 60 dB takes three times as long
}

std::pair<std::vector<float>, std::vector<float>> reverbIr(devices::Reverb& r, double seconds)
{
    r.prepare(48000.0, 512);
    std::vector<float> l, rr;
    juce::MidiBuffer m;
    const int total = (int) (seconds * 48000.0);
    for (int pos = 0; pos < total; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        b.clear();
        if (pos == 0)
            b.setSample(0, 0, 1.0f), b.setSample(1, 0, 1.0f);
        r.process(b, m);
        for (int i = 0; i < 512; ++i)
            l.push_back(b.getSample(0, i)), rr.push_back(b.getSample(1, i));
    }
    return {l, rr};
}

void setupWet(devices::Reverb& r, float decayMs)
{
    r.setParameter(devices::Reverb::DecayTime, decayMs);
    r.setParameter(devices::Reverb::MixDirect, 1.0f);
    r.setParameter(devices::Reverb::BandLowOn, 0.0f);
    r.setParameter(devices::Reverb::BandHighOn, 0.0f);
    r.setParameter(devices::Reverb::ShelfHighOn, 0.0f);
    r.setParameter(devices::Reverb::ChorusOn, 0.0f);
}
} // namespace

TEST_CASE("reverb: decay time sets the reverberation time, longer decay is longer")
{
    devices::Reverb shortRev, longRev;
    setupWet(shortRev, 800.0f);
    setupWet(longRev, 2400.0f);
    auto [sl, sr] = reverbIr(shortRev, 4.0);
    auto [ll, lr] = reverbIr(longRev, 8.0);
    const double rtShort = estimateRt60(sl, 48000.0), rtLong = estimateRt60(ll, 48000.0);
    CHECK(rtShort > 0.4);
    CHECK(rtShort < 1.4);   // 0.8 s requested
    CHECK(rtLong > 1.4);
    CHECK(rtLong < 4.0);    // 2.4 s requested
    CHECK(rtLong > rtShort * 2.0);

    // Stable: no blow-up, finite output.
    float peak = 0.0f;
    for (float v : ll)
        peak = std::max(peak, std::fabs(v));
    CHECK(peak < 2.0f);
    CHECK(std::isfinite(peak));
}

TEST_CASE("reverb: pre-delay, dry/wet, stereo separation, shelves and freeze")
{
    // Pre-delay: nothing comes out before it elapses.
    devices::Reverb pre;
    setupWet(pre, 1000.0f);
    pre.setParameter(devices::Reverb::PreDelay, 50.0f);
    auto [pl, pr] = reverbIr(pre, 0.5);
    double early = 0.0;
    for (int i = 0; i < 2300; ++i) // 48 ms
        early += (double) pl[(size_t) i] * pl[(size_t) i];
    CHECK(early < 1e-9);
    double later = 0.0;
    for (int i = 2400; i < 12000; ++i)
        later += (double) pl[(size_t) i] * pl[(size_t) i];
    CHECK(later > 1e-6);

    // Dry/wet 0: output is the input only.
    devices::Reverb dry;
    setupWet(dry, 1000.0f);
    dry.setParameter(devices::Reverb::MixDirect, 0.0f);
    auto [dl, dr] = reverbIr(dry, 0.2);
    CHECK_NEAR(dl[0], 1.0, 1e-6);
    double tail = 0.0;
    for (size_t i = 1; i < dl.size(); ++i)
        tail += (double) dl[i] * dl[i];
    CHECK(tail < 1e-12);

    // Stereo separation 0 makes the late tail mono; 100 % decorrelates the channels.
    auto correlation = [](const std::vector<float>& a, const std::vector<float>& b, int from, int to) {
        double ab = 0, aa = 0, bb = 0;
        for (int i = from; i < to; ++i)
            ab += (double) a[(size_t) i] * b[(size_t) i], aa += (double) a[(size_t) i] * a[(size_t) i], bb += (double) b[(size_t) i] * b[(size_t) i];
        return ab / std::sqrt(aa * bb + 1e-30);
    };
    devices::Reverb mono, wide;
    setupWet(mono, 1500.0f);
    setupWet(wide, 1500.0f);
    mono.setParameter(devices::Reverb::MixReflect, 0.0f);
    wide.setParameter(devices::Reverb::MixReflect, 0.0f);
    mono.setParameter(devices::Reverb::StereoSeparation, 0.0f);
    auto [ml, mr] = reverbIr(mono, 1.5);
    auto [wl, wr] = reverbIr(wide, 1.5);
    CHECK(correlation(ml, mr, 4800, 60000) > 0.99);
    CHECK(correlation(wl, wr, 4800, 60000) < 0.6);

    // High-shelf damping: a darker tail (less high-frequency energy) with the shelf on.
    auto highEnergy = [](const std::vector<float>& x) {
        double e = 0;
        for (size_t i = 5000; i + 1 < x.size() && i < 60000; ++i)
        {
            const double d = (double) x[i + 1] - x[i]; // first difference emphasises the highs
            e += d * d;
        }
        return e;
    };
    devices::Reverb bright, dark;
    setupWet(bright, 2000.0f);
    setupWet(dark, 2000.0f);
    dark.setParameter(devices::Reverb::ShelfHighOn, 1.0f);
    dark.setParameter(devices::Reverb::ShelfHiFreq, 2000.0f);
    dark.setParameter(devices::Reverb::ShelfHiGain, 0.2f);
    auto [bl, br] = reverbIr(bright, 1.5);
    auto [kl, kr] = reverbIr(dark, 1.5);
    CHECK(highEnergy(kl) < highEnergy(bl) * 0.5);

    // Freeze keeps the tail alive: after 3 s of a 0.4 s reverb it would be silent, frozen it is not.
    devices::Reverb frozen;
    setupWet(frozen, 400.0f);
    frozen.prepare(48000.0, 512);
    juce::MidiBuffer m;
    float lastPeak = 0.0f;
    for (int pos = 0; pos < 48000 * 3; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        b.clear();
        if (pos < 8192) // excite
            for (int i = 0; i < 512; ++i)
                b.setSample(0, i, 0.3f * std::sin(0.3f * (float) (pos + i))), b.setSample(1, i, b.getSample(0, i));
        if (pos == 8192)
            frozen.setParameter(devices::Reverb::FreezeOn, 1.0f);
        frozen.process(b, m);
        if (pos > 48000 * 2)
            lastPeak = std::max(lastPeak, b.getMagnitude(0, 512));
    }
    CHECK(lastPeak > 0.01f);
}

TEST_CASE("Simpler: sustain holds key-up voices until pedal release and reset controllers releases them")
{
    auto s = makeSimpler(devices::Simpler::Mode::Classic); setP(*s, devices::Simpler::Release, 20);
    juce::MidiBuffer first;
    first.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    first.addEvent(juce::MidiMessage::noteOn(1, 69, (juce::uint8) 100), 0);
    first.addEvent(juce::MidiMessage::noteOff(1, 69), 128);
    auto output = renderSimpler(*s, first, 14, [](int block, juce::MidiBuffer& m) {
        if (block == 7) m.addEvent(juce::MidiMessage::controllerEvent(1, 64, 0), 64);
    });
    CHECK(peak(output, 3 * 512, 6 * 512) > 0.01); CHECK(peak(output, 12 * 512, 14 * 512) < 1e-4);
    output = renderSimpler(*s, first, 14, [](int block, juce::MidiBuffer& m) {
        if (block == 7) m.addEvent(juce::MidiMessage::controllerEvent(1, 121, 0), 64);
    });
    CHECK(peak(output, 3 * 512, 6 * 512) > 0.01); CHECK(peak(output, 12 * 512, 14 * 512) < 1e-4);
}

#include "devices/native/ClassicEffects.h"

namespace
{
// Runs `dev` at 44.1 kHz over `seconds` of a stereo signal given per sample index; returns the left output and
// (optionally) the left input, so callers can read the device's instantaneous gain.
template <class Signal>
std::vector<float> runStereo(devices::DeviceProcessor& dev, double seconds, Signal signal, std::vector<float>* input = nullptr)
{
    const double sr = 44100.0;
    dev.prepare(sr, 256);
    std::vector<float> out;
    juce::MidiBuffer m;
    const int total = (int) (seconds * sr);
    for (int pos = 0; pos < total; pos += 256)
    {
        juce::AudioBuffer<float> b(2, 256);
        for (int i = 0; i < 256; ++i)
        {
            const auto [l, r] = signal(pos + i);
            b.setSample(0, i, l);
            b.setSample(1, i, r);
            if (input != nullptr) input->push_back(l);
        }
        dev.process(b, m);
        for (int i = 0; i < 256; ++i)
            out.push_back(b.getSample(0, i));
    }
    return out;
}
std::pair<float, float> sineAt(int i, float amp, float hz = 1000.0f)
{
    const float v = amp * std::sin(2.0f * 3.14159265f * hz * (float) i / 44100.0f);
    return {v, v};
}
// Peak of |out| / peak of |in| over [from, to) samples, in dB.
double gainDbOver(const std::vector<float>& out, const std::vector<float>& in, size_t from, size_t to)
{
    double po = 0, pi = 0;
    for (size_t i = from; i < to && i < out.size(); ++i)
    {
        po = std::max(po, (double) std::fabs(out[i]));
        pi = std::max(pi, (double) std::fabs(in[i]));
    }
    return 20.0 * std::log10(std::max(po, 1e-12) / std::max(pi, 1e-12));
}
} // namespace

TEST_CASE("gate: measured laws (DEV-22b): RMS detector, dB floor/return, linear attack/release, flip, silence reset")
{
    using G = devices::Gate;
    const auto make = [](float thresholdDb, float floorDb) {
        auto g = std::make_unique<G>();
        g->setParameter(G::Threshold, juce::Decibels::decibelsToGain(thresholdDb));
        g->setParameter(G::Attack, 0.02f);
        g->setParameter(G::Hold, 0.0f);
        g->setParameter(G::Release, 50.0f);
        g->setParameter(G::Return, 0.0f);
        g->setParameter(G::Floor, floorDb);
        return g;
    };
    // Detector = power envelope (+0.75 dB threshold offset). Measured with a C3 sine against a -30 dB threshold:
    // a peak 3.9 dB above the threshold opens, 0.7 dB above does not (the detector reads ~RMS, not peak).
    {
        auto open = make(-20.0f, -40.0f), closed = make(-20.0f, -40.0f);
        std::vector<float> in1, in2;
        auto o1 = runStereo(*open, 0.3, [](int i) { return sineAt(i, juce::Decibels::decibelsToGain(-16.1f), 261.63f); }, &in1);
        auto o2 = runStereo(*closed, 0.3, [](int i) { return sineAt(i, juce::Decibels::decibelsToGain(-19.3f), 261.63f); }, &in2);
        CHECK_NEAR(gainDbOver(o1, in1, 8820, 13230), 0.0, 0.05);
        CHECK_NEAR(gainDbOver(o2, in2, 8820, 13230), -40.0, 0.05); // floor in dB
    }
    // Release: after the signal drops below the threshold the gain falls linearly-exponentially to the floor,
    // g = floor + (1 - floor) exp(-t / Release): the excess over the floor shrinks by e every 50 ms.
    {
        auto gate = make(-20.0f, -40.0f);
        std::vector<float> in;
        auto out = runStereo(*gate, 1.0, [](int i) { return sineAt(i, i < 22050 ? 0.5f : 0.02f); }, &in);
        const auto excess = [&](size_t at) { return juce::Decibels::decibelsToGain(gainDbOver(out, in, at, at + 45)) - 0.01; };
        CHECK_NEAR(excess(22050 + 6615 + 2205) / excess(22050 + 6615), std::exp(-1.0), 0.01); // well after closing
    }
    // Attack: the gain rises towards 1 with tau = Attack (10 ms), and -75 dB floor means -inf.
    {
        auto gate = make(-20.0f, -75.0f);
        gate->setParameter(G::Attack, 10.0f);
        std::vector<float> in;
        auto out = runStereo(*gate, 1.0, [](int i) { return sineAt(i, i < 22050 ? 0.02f : 0.5f); }, &in);
        CHECK(gainDbOver(out, in, 20000, 22000) < -120.0);
        const auto deficit = [&](size_t at) { return 1.0 - juce::Decibels::decibelsToGain(gainDbOver(out, in, at, at + 45)); };
        CHECK_NEAR(deficit(22050 + 441 + 441) / deficit(22050 + 441), std::exp(-1.0), 0.03);
    }
    // Flip ducks loud passages to the floor (with Attack) and recovers with Release; it starts from gain 1.
    {
        auto gate = make(-20.0f, -40.0f);
        gate->setParameter(G::Flip, 1.0f);
        std::vector<float> in;
        auto out = runStereo(*gate, 1.0, [](int i) { return sineAt(i, i < 22050 ? 0.02f : 0.5f); }, &in);
        CHECK_NEAR(gainDbOver(out, in, 0, 441), 0.0, 0.05);
        CHECK_NEAR(gainDbOver(out, in, 30000, 31000), -40.0, 0.05);
    }
    // Digital silence suspends the gate: the next (quiet, closed) passage starts from gain 0 and rises to the
    // -8 dB floor with tau = Attack (150 ms), as measured on the reference's Soft Reducer preset.
    {
        auto gate = make(-15.0f, -8.0f);
        gate->setParameter(G::Attack, 150.0f);
        std::vector<float> in;
        auto out = runStereo(*gate, 1.0, [](int i) { return i < 4410 ? std::pair{0.0f, 0.0f} : sineAt(i, 0.05f); }, &in);
        const double floor = juce::Decibels::decibelsToGain(-8.0);
        const double g = juce::Decibels::decibelsToGain(gainDbOver(out, in, 4410 + 6615 - 22, 4410 + 6615 + 23));
        CHECK_NEAR(g / floor, 1.0 - std::exp(-1.0), 0.02);
    }
}

TEST_CASE("compressor: measured laws (DEV-22b): knee width, channel-mean detector, 24 ms RMS, expand, lin envelope")
{
    using C = devices::Compressor;
    const auto make = [](float thresholdDb, float ratio, float knee) {
        auto c = std::make_unique<C>();
        c->setParameter(C::Threshold, juce::Decibels::decibelsToGain(thresholdDb));
        c->setParameter(C::Ratio, ratio);
        c->setParameter(C::Knee, knee);
        c->setParameter(C::Attack, 0.01f);
        c->setParameter(C::Release, 1000.0f);
        return c;
    };
    const auto steadyGain = [](C& c, float l, float r) {
        std::vector<float> in;
        auto out = runStereo(c, 1.0, [l, r](int i) { auto [a, b] = sineAt(i, 1.0f); return std::pair{a * l, b * r}; }, &in);
        return gainDbOver(out, in, 30000, 44000);
    };
    // Knee 6 dB spans threshold +-6 dB: at the threshold the reduction is (1/4 - 1) 6^2 / 24 = -1.125 dB.
    auto knee = make(-20.0f, 4.0f, 6.0f);
    CHECK_NEAR(steadyGain(*knee, 0.1f, 0.1f), -1.125, 0.03);
    // Linked detector = (|L| + |R|) / 2: a -6 dB sine on the left only is detected at -12 dB.
    auto oneSide = make(-20.0f, 4.0f, 0.0f);
    CHECK_NEAR(steadyGain(*oneSide, 0.5f, 0.0f), -(-12.04 + 20.0) * 0.75, 0.05);
    // RMS: 24 ms mean of (L^2 + R^2) / 2 reads a sine 3 dB below its peak.
    auto rms = make(-20.0f, 4.0f, 0.0f);
    rms->setParameter(C::Model, 1.0f);
    CHECK_NEAR(steadyGain(*rms, 0.5f, 0.5f), -(-9.03 + 20.0) * 0.75, 0.1);
    // Expand: upward expansion above the threshold by (in - T) (1 - 1/ExpansionRatio).
    auto expand = make(-20.0f, 4.0f, 0.0f);
    expand->setParameter(C::Model, 2.0f);
    expand->setParameter(C::ExpansionRatio, 1.15f);
    CHECK_NEAR(steadyGain(*expand, 0.5f, 0.5f), (-6.02 + 20.0) * (1.0 - 1.0 / 1.15), 0.03);
    // Lin envelope reaches the same steady state as the default log envelope.
    auto lin = make(-20.0f, 4.0f, 0.0f);
    lin->setParameter(C::LogEnvelope, 0.0f);
    CHECK_NEAR(steadyGain(*lin, 0.5f, 0.5f), -(-6.02 + 20.0) * 0.75, 0.05);
}

#include "devices/native/Reverb.h"

TEST_CASE("delay: measured laws (DEV-22b): seconds time, Link, ping-pong routing, Butterworth filter corners")
{
    using D = devices::Delay;
    auto impulseAt = [](D& d, int length) {
        d.prepare(44100.0, 256);
        juce::MidiBuffer m;
        std::vector<float> l, r;
        for (int pos = 0; pos < length; pos += 256)
        {
            juce::AudioBuffer<float> b(2, 256);
            b.clear();
            if (pos == 0) { b.setSample(0, 0, 1.0f); b.setSample(1, 0, 1.0f); }
            d.process(b, m);
            for (int i = 0; i < 256; ++i) { l.push_back(b.getSample(0, i)); r.push_back(b.getSample(1, i)); }
        }
        return std::pair{l, r};
    };
    auto setup = [](D& d) {
        d.setParameter(D::SyncL, 0); d.setParameter(D::SyncR, 0); d.setParameter(D::FilterOn, 0);
        d.setParameter(D::DryWetMode, 0); d.setParameter(D::DryWet, 1); d.setParameter(D::Feedback, 0.5f);
    };
    // DelayLine_TimeL in seconds: 0.2 s = 8820 samples, unity first echo, feedback multiplies each repeat.
    D d; setup(d); d.setParameter(D::TimeL, 0.2f); d.setParameter(D::TimeR, 0.3f); // Link on: R plays L's time
    auto [l, r] = impulseAt(d, 20000);
    CHECK_NEAR(l[8820], 1.0, 1e-4);
    CHECK_NEAR(l[17640], 0.5, 1e-4);
    CHECK_NEAR(r[8820], 1.0, 1e-4);
    // Ping pong: equal-power mono sum into the left line, the left output feeds the right line unattenuated.
    D pp; setup(pp); pp.setParameter(D::TimeL, 0.1f); pp.setParameter(D::TimeR, 0.1f); pp.setParameter(D::PingPong, 1);
    auto [pl, pr] = impulseAt(pp, 20000);
    CHECK_NEAR(pl[4410], 1.41421, 1e-3);
    CHECK_NEAR(pr[8820], 1.41421, 1e-3);
    CHECK_NEAR(pl[13230], 0.70711, 1e-3);
    // Filter: Butterworth high-pass at f / 2^(w/2) and low-pass at f * 2^(w/2): width 2 at 1 kHz = -3 dB at 500 Hz.
    D f; setup(f); f.setParameter(D::Feedback, 0); f.setParameter(D::TimeL, 0.01f); f.setParameter(D::FilterOn, 1);
    f.setParameter(D::FilterFrequency, 1000); f.setParameter(D::FilterBandwidth, 2);
    f.prepare(44100.0, 512);
    juce::MidiBuffer m;
    double in = 0, out = 0;
    for (int pos = 0; pos < 44100; pos += 512)
    {
        juce::AudioBuffer<float> b(2, 512);
        for (int i = 0; i < 512; ++i) { const float v = 0.5f * std::sin(2.0f * 3.14159265f * 500.0f * (float) (pos + i) / 44100.0f); b.setSample(0, i, v); b.setSample(1, i, v); }
        f.process(b, m);
        if (pos > 22050)
            for (int i = 0; i < 512; ++i) { out += (double) b.getSample(0, i) * b.getSample(0, i); in += 0.125; }
    }
    CHECK_NEAR(10.0 * std::log10(out / in), -3.0, 0.15);
}

TEST_CASE("glue compressor: fitted static curve and soft clip (DEV-22b)")
{
    // 4:1 at -20 dB: the reference reduces a -10 dB sine peak by ~6.9 dB (fit within 0.1 dB).
    CHECK_NEAR(devices::glueReductionDb(-10.1f, -20.0f, 1), -6.91, 0.15);
    CHECK_NEAR(devices::glueReductionDb(-7.3f, -20.0f, 2), -8.15, 0.15);  // 10:1
    CHECK_NEAR(devices::glueReductionDb(-29.3f, -20.0f, 0), -1.75, 0.15); // 2:1 starts well below the threshold
    CHECK_NEAR(devices::glueSoftClip(0.5f), 0.5, 1e-6);
    CHECK_NEAR(devices::glueSoftClip(1.54f), 1.178, 0.01);                // measured ceiling ~1.18
}

TEST_CASE("auto filter 2: Clean filter laws match the reference (DEV-22b)")
{
    using A = devices::AutoFilter2;
    auto gainAt = [](A& a, float hz) {
        a.prepare(44100.0, 512);
        juce::MidiBuffer m;
        double in = 0, out = 0;
        for (int pos = 0; pos < 44100; pos += 512)
        {
            juce::AudioBuffer<float> b(2, 512);
            for (int i = 0; i < 512; ++i) { const float v = 0.25f * std::sin(2.0f * 3.14159265f * hz * (float) (pos + i) / 44100.0f); b.setSample(0, i, v); b.setSample(1, i, v); }
            a.process(b, m);
            if (pos > 22050)
                for (int i = 0; i < 512; ++i) { out += (double) b.getSample(0, i) * b.getSample(0, i); in += 0.25 * 0.25 * 0.5; }
        }
        return 10.0 * std::log10(out / in);
    };
    A lp; lp.setParameter(A::Frequency, 1000); lp.setParameter(A::Slope, 1);
    CHECK_NEAR(gainAt(lp, 1000), -12.0, 0.1);  // two Q = 0.5 stages: -6 dB each at the cutoff
    A lp12; lp12.setParameter(A::Frequency, 1000); lp12.setParameter(A::Slope, 0);
    CHECK_NEAR(gainAt(lp12, 2000), -14.0, 0.15);
    A res; res.setParameter(A::Frequency, 1000); res.setParameter(A::Resonance, 0.5f);
    CHECK_NEAR(A::damping(0.5f), 0.2, 1e-6);   // Q 2.5 (measured 2.49), second stage Q 0.833
    CHECK_NEAR(gainAt(res, 921), 7.0, 0.4);    // measured peak +7.0 dB at 921 Hz
    A out; out.setParameter(A::Frequency, 20000); out.setParameter(A::Output, 0.5f);
    CHECK_NEAR(gainAt(out, 100), -6.02, 0.1);  // Output is a linear gain
    A morph; morph.setParameter(A::Frequency, 1000); morph.setParameter(A::FilterType, 4); morph.setParameter(A::Morph, 0.5f);
    CHECK_NEAR(gainAt(morph, 1000), -12.0, 0.1); // Morph 0.5 == band-pass (stage peak gain Q)
}

TEST_CASE("reverb: measured input filter flags and decay law (DEV-22b)")
{
    using R = devices::Reverb;
    // BandLowOn switches the low-pass (high cut), BandHighOn the high-pass (low cut).
    auto level = [](bool lowOn, bool highOn, float hz) {
        R r;
        r.setParameter(R::MixDirect, 1.0f);
        r.setParameter(R::BandLowOn, lowOn ? 1.0f : 0.0f);
        r.setParameter(R::BandHighOn, highOn ? 1.0f : 0.0f);
        r.setParameter(R::BandFreq, 1000.0f);
        r.setParameter(R::BandWidth, 2.0f);
        r.prepare(44100.0, 512);
        juce::MidiBuffer m;
        double sum = 0;
        for (int pos = 0; pos < 88200; pos += 512)
        {
            juce::AudioBuffer<float> b(2, 512);
            for (int i = 0; i < 512; ++i) { const float v = 0.25f * std::sin(2.0f * 3.14159265f * hz * (float) (pos + i) / 44100.0f); b.setSample(0, i, v); b.setSample(1, i, v); }
            r.process(b, m);
            if (pos > 44100)
                for (int i = 0; i < 512; ++i) sum += (double) b.getSample(0, i) * b.getSample(0, i);
        }
        return 10.0 * std::log10(sum + 1e-30);
    };
    CHECK(level(true, false, 8000) < level(false, false, 8000) - 6.0);  // "Low" flag cuts highs
    CHECK(level(false, true, 125) < level(false, false, 125) - 6.0);    // "High" flag cuts lows
}
