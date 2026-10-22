// Delay and Reverb parity checks (DEV-8c / DEV-6c).
//
// Parameter ids, defaults and value ranges come from Live 12's schema, its default device files and the values Live wrote
// in real sets (docs/NATIVE_DELAY_REVERB.md). The DSP itself is Sonora's own, so these tests pin down behaviour that
// the parameters promise (time, feedback, modes, freeze, stereo, ...), not Live's exact sound.

#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/Delay.h"
#include "devices/native/Reverb.h"
#include "dsp/ModLfo.h"

#include <algorithm>
#include <functional>
#include <map>

using namespace sonora;

namespace
{
using Signal = std::vector<float>;

// Feeds a device block by block (512 samples) and keeps the running sample position across calls.
struct Stream
{
    explicit Stream(devices::DeviceProcessor& d) : dev(d) { dev.prepare(48000.0, 512); }

    std::pair<Signal, Signal> run(int n, const std::function<float(long, int)>& input = {})
    {
        Signal l, r;
        for (int done = 0; done < n; done += 512)
        {
            const int count = std::min(512, n - done);
            juce::AudioBuffer<float> b(2, 512);
            b.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < count; ++i)
                    b.setSample(ch, i, input ? input(pos + i, ch) : 0.0f);
            b.setSize(2, count, true);
            dev.process(b, midi);
            for (int i = 0; i < count; ++i)
            {
                l.push_back(b.getSample(0, i));
                r.push_back(b.getSample(1, i));
            }
            pos += count;
        }
        return {l, r};
    }
    std::pair<Signal, Signal> impulse(int n, float left = 1.0f, float right = 1.0f)
    {
        const long start = pos;
        return run(n, [=](long p, int ch) { return p == start ? (ch == 0 ? left : right) : 0.0f; });
    }

    devices::DeviceProcessor& dev;
    juce::MidiBuffer midi;
    long pos = 0;
};

int peakAt(const Signal& x, int from, int to)
{
    int best = from;
    for (int i = from; i < to && i < (int) x.size(); ++i)
        if (std::fabs(x[(size_t) i]) > std::fabs(x[(size_t) best]))
            best = i;
    return best;
}

float peakIn(const Signal& x, int from, int to)
{
    return std::fabs(x[(size_t) peakAt(x, from, to)]);
}

double energy(const Signal& x, int from, int to)
{
    double e = 0;
    for (int i = from; i < to && i < (int) x.size(); ++i)
        e += (double) x[(size_t) i] * x[(size_t) i];
    return e;
}

int indexOf(const devices::DeviceProcessor& d, const std::string& id)
{
    const auto& p = d.parameters();
    for (size_t i = 0; i < p.size(); ++i)
        if (p[i].id == id)
            return (int) i;
    return -1;
}

// Delay with free times, no filter, classic law, fully wet.
void plainDelay(devices::Delay& d, float leftMs, float rightMs, float feedback)
{
    using D = devices::Delay;
    d.setParameter(D::SyncL, 0.0f);
    d.setParameter(D::SyncR, 0.0f);
    d.setParameter(D::TimeL, leftMs * 0.001f); // DelayLine_TimeL/R: seconds (measured, DEV-22b)
    d.setParameter(D::TimeR, rightMs * 0.001f);
    d.setParameter(D::Feedback, feedback);
    d.setParameter(D::FilterOn, 0.0f);
    d.setParameter(D::DryWetMode, 0.0f);
    d.setParameter(D::DryWet, 1.0f);
}

// Reverb with the input and damping stages out of the way, fully wet.
void plainReverb(devices::Reverb& r, float decayMs)
{
    using R = devices::Reverb;
    r.setParameter(R::DecayTime, decayMs);
    r.setParameter(R::MixDirect, 1.0f);
    r.setParameter(R::BandLowOn, 0.0f);
    r.setParameter(R::BandHighOn, 0.0f);
    r.setParameter(R::ShelfHighOn, 0.0f);
    r.setParameter(R::ShelfLowOn, 0.0f);
    r.setParameter(R::ChorusOn, 0.0f);
    r.setParameter(R::SpinOn, 0.0f);
}
} // namespace

// ---------------------------------------------------------------- parameter catalogue

TEST_CASE("delay/reverb: every schema parameter exists with Live's default value")
{
    // {.als element, value in Live's default device file}
    const std::vector<std::pair<std::string, double>> delay = {
        {"DelayLine_SmoothingMode", 0}, {"DelayLine_Link", 1}, {"DelayLine_PingPong", 0}, {"DelayLine_SyncL", 1},
        {"DelayLine_SyncR", 1}, {"DelayLine_SimpleDelayTimeL", 100}, {"DelayLine_SimpleDelayTimeR", 100},
        {"DelayLine_SyncedSixteenthL", 2}, {"DelayLine_SyncedSixteenthR", 3}, {"DelayLine_OffsetL", 0},
        {"DelayLine_OffsetR", 0}, {"DelayLine_TimeL", 0.3749999404}, {"DelayLine_TimeR", 0.3749999404},
        {"DelayLine_PingPongDelayTimeL", 1}, {"DelayLine_PingPongDelayTimeR", 1}, {"DelayLine_CompatibilityMode", 0},
        {"Feedback", 0.5}, {"Freeze", 0}, {"Filter_On", 1}, {"Filter_Frequency", 1000}, {"Filter_Bandwidth", 8},
        {"Modulation_TimeMode", 0}, {"Modulation_Frequency", 0.5}, {"Modulation_Time", 1}, {"Modulation_SyncedRate", 4},
        {"Modulation_Sixteenth", 16}, {"Modulation_Waveform", 1}, {"Modulation_Morph", 0},
        {"Modulation_AmountTime", 0}, {"Modulation_AmountFilter", 0}, {"DryWet", 0.5}, {"DryWetMode", 1},
        {"EcoProcessing", 0}};
    const std::vector<std::pair<std::string, double>> reverb = {
        {"PreDelay", 2.5}, {"BandHighOn", 0}, {"BandLowOn", 1}, {"BandFreq", 830}, {"BandWidth", 5.85}, {"SpinOn", 1},
        {"EarlyReflectModFreq", 0.2977}, {"EarlyReflectModDepth", 17.5}, {"DiffuseDelay", 0.5}, {"ShelfHighOn", 1},
        {"HighFilterType", 0}, {"ShelfHiFreq", 4500}, {"ShelfHiGain", 0.7}, {"ShelfLowOn", 0}, {"ShelfLoFreq", 90},
        {"ShelfLoGain", 0.75}, {"ChorusOn", 1}, {"SizeModFreq", 0.02}, {"SizeModDepth", 0.02}, {"DecayTime", 1200},
        {"AllPassGain", 0.6}, {"AllPassSize", 0.4}, {"FreezeOn", 0}, {"FlatOn", 1}, {"CutOn", 1}, {"RoomSize", 100},
        {"SizeSmoothing", 2}, {"StereoSeparation", 100}, {"RoomType", 1}, {"MixReflect", 1}, {"MixDiffuse", 1},
        {"MixDirect", 0.55}, {"StereoSeparationOnDrySignal", 0}};

    devices::Delay d;
    for (const auto& [id, expected] : delay)
    {
        const int index = indexOf(d, id);
        CHECK(index >= 0);
        if (index >= 0)
            CHECK_NEAR(d.getParameter(index), expected, 1e-3);
    }
    CHECK(d.parameters().size() == delay.size());

    devices::Reverb r;
    for (const auto& [id, expected] : reverb)
    {
        const int index = indexOf(r, id);
        CHECK(index >= 0);
        if (index >= 0)
            CHECK_NEAR(r.getParameter(index), expected, 1e-3);
    }
    CHECK(r.parameters().size() == reverb.size());

    // Enum orders (Live's menus; the manual lists them in this order).
    CHECK(d.parameters()[(size_t) devices::Delay::SmoothingMode].choices == (std::vector<std::string>{"Repitch", "Fade", "Jump"}));
    CHECK(d.parameters()[(size_t) devices::Delay::ModWaveform].choices.size() == 7);
    CHECK(d.parameters()[(size_t) devices::Delay::ModTimeMode].choices.size() == 6);
    CHECK(r.parameters()[(size_t) devices::Reverb::SizeSmoothing].choices == (std::vector<std::string>{"None", "Slow", "Fast"}));
    CHECK(r.parameters()[(size_t) devices::Reverb::Density].choices.size() == 4);
}

TEST_CASE("delay/reverb: values Live wrote in real sets load without clamping")
{
    model::Device delay;
    for (const auto& [id, value] : std::vector<std::pair<std::string, double>>{
             {"DelayLine_SyncedSixteenthL", 1}, {"DelayLine_SyncedSixteenthR", 3}, {"DelayLine_OffsetL", 0.02601563931},
             {"DelayLine_OffsetR", -0.00665998}, {"Filter_Frequency", 200.595}, {"Filter_Bandwidth", 2.40132},
             {"Feedback", 0.7045}, {"Modulation_AmountTime", 0.5546875}, {"Modulation_AmountFilter", 0.488189}})
    {
        model::Parameter p;
        p.path = id;
        p.value = value;
        delay.parameters.push_back(p);
    }
    devices::Delay d;
    d.loadFromModel(delay);
    CHECK_NEAR(d.getParameter(devices::Delay::SyncedL), 1.0, 1e-6);
    CHECK_NEAR(d.getParameter(devices::Delay::SyncedR), 3.0, 1e-6);
    CHECK_NEAR(d.getParameter(devices::Delay::OffsetL), 0.02601563931, 1e-6);
    CHECK_NEAR(d.getParameter(devices::Delay::OffsetR), -0.00665998, 1e-6);
    CHECK_NEAR(d.getParameter(devices::Delay::FilterFrequency), 200.595, 1e-3);
    CHECK_NEAR(d.getParameter(devices::Delay::FilterBandwidth), 2.40132, 1e-5);
    CHECK_NEAR(d.getParameter(devices::Delay::ModAmountTime), 0.5546875, 1e-6);

    model::Device reverb;
    for (const auto& [id, value] : std::vector<std::pair<std::string, double>>{
             {"ShelfLoFreq", 11961.9}, {"ShelfLoGain", 0.2}, {"ShelfHiGain", 0.2}, {"ShelfHiFreq", 12179.4},
             {"StereoSeparation", 120}, {"DecayTime", 24263.7}, {"RoomSize", 470.286}, {"AllPassGain", 0.001},
             {"AllPassSize", 0.3591268957}, {"EarlyReflectModDepth", 22.4152}, {"SizeModDepth", 2.49559},
             {"SizeModFreq", 2.13619}, {"MixReflect", 1.9953}, {"MixDiffuse", 0.03}, {"BandWidth", 0.904762},
             {"DiffuseDelay", 0.1666666716}, {"RoomType", 3}, {"SizeSmoothing", 0}})
    {
        model::Parameter p;
        p.path = id;
        p.value = value;
        reverb.parameters.push_back(p);
    }
    devices::Reverb r;
    r.loadFromModel(reverb);
    for (const auto& p : reverb.parameters)
        CHECK_NEAR(r.getParameter(indexOf(r, p.path)) / p.value, 1.0, 1e-5); // float storage: compare relatively
}

TEST_CASE("delay/reverb: new devices are authored with the complete parameter list")
{
    const auto delay = devices::nativeDeviceModel("Delay");
    const auto reverb = devices::nativeDeviceModel("Reverb");
    auto has = [](const model::Device& d, const char* path) {
        return std::any_of(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == path; });
    };
    CHECK(has(delay, "DelayLine_SyncedSixteenthL"));
    CHECK(has(delay, "Modulation_Waveform"));
    CHECK(delay.plain.count("DryWetMode") == 1); // plain leaves of the device element (not Manual parameters)
    CHECK(delay.plain.count("DelayLine_CompatibilityMode") == 1);
    CHECK(has(reverb, "StereoSeparationOnDrySignal"));
    CHECK(has(reverb, "RoomType"));
    CHECK(has(reverb, "AllPassGain"));
    CHECK(delay.parameters.size() + delay.plain.size() == 33);
    CHECK(reverb.parameters.size() == 33);
}

// ---------------------------------------------------------------- Delay

TEST_CASE("delay: sync index is zero based, offset is a fraction, Link plays the left settings on both sides")
{
    using D = devices::Delay;
    D d;
    d.setParameter(D::FilterOn, 0.0f);
    d.setParameter(D::DryWetMode, 0.0f);
    d.setParameter(D::DryWet, 1.0f);
    d.setParameter(D::Feedback, 0.0f);
    devices::HostInfo host;
    host.tempo = 120.0; // one sixteenth = 125 ms = 6000 samples
    d.setHostInfo(host);
    Stream s(d);
    auto [l, r] = s.impulse(34000);
    // Defaults (Live's own): L index 2 == 3 sixteenths, R index 3 == 4 sixteenths, Link on. Measured (DEV-22b): with
    // Link on, the right side plays the left side's time, so both repeat after 3 sixteenths.
    CHECK(std::abs(peakAt(l, 1000, 34000) - 18000) <= 10);
    CHECK(std::abs(peakAt(r, 1000, 34000) - 18000) <= 10);
    D unlinked;
    unlinked.setParameter(D::FilterOn, 0.0f);
    unlinked.setParameter(D::DryWetMode, 0.0f);
    unlinked.setParameter(D::DryWet, 1.0f);
    unlinked.setParameter(D::Feedback, 0.0f);
    unlinked.setParameter(D::Link, 0.0f);
    unlinked.setHostInfo(host);
    Stream s3(unlinked);
    auto [ul, ur] = s3.impulse(34000);
    CHECK(std::abs(peakAt(ul, 1000, 34000) - 18000) <= 10);
    CHECK(std::abs(peakAt(ur, 1000, 34000) - 24000) <= 10);

    D swing;
    swing.setParameter(D::FilterOn, 0.0f);
    swing.setParameter(D::DryWetMode, 0.0f);
    swing.setParameter(D::DryWet, 1.0f);
    swing.setParameter(D::Feedback, 0.0f);
    swing.setParameter(D::Link, 0.0f);
    swing.setParameter(D::SyncedL, 3.0f);
    swing.setParameter(D::SyncedR, 3.0f);
    swing.setParameter(D::OffsetL, 0.25f);   // +25 % later
    swing.setParameter(D::OffsetR, -0.25f);  // 25 % earlier
    swing.setHostInfo(host);
    Stream s2(swing);
    auto [sl, sr] = s2.impulse(34000);
    CHECK(std::abs(peakAt(sl, 1000, 34000) - 30000) <= 10);
    CHECK(std::abs(peakAt(sr, 1000, 34000) - 18000) <= 10);
}

TEST_CASE("delay: Link moves both sides when editing, stored values stay independent")
{
    using D = devices::Delay;
    D d; // Link on, L index 2, R index 3
    d.setParameterLinked(D::SyncedL, 5.0f);
    CHECK_NEAR(d.getParameter(D::SyncedL), 5.0, 1e-6);
    CHECK_NEAR(d.getParameter(D::SyncedR), 6.0, 1e-6); // moved by the same step, not copied
    d.setParameterLinked(D::OffsetR, 0.1f);
    CHECK_NEAR(d.getParameter(D::OffsetL), 0.1, 1e-6);
    d.setParameterLinked(D::TimeL, 0.75f);             // 0.375 -> 0.75 s: both doubled
    CHECK_NEAR(d.getParameter(D::TimeR), 0.75, 1e-3);
    d.setParameterLinked(D::SyncL, 0.0f);
    CHECK_NEAR(d.getParameter(D::SyncR), 0.0, 1e-6);

    d.setParameter(D::Link, 0.0f);
    d.setParameterLinked(D::SyncedL, 1.0f);
    CHECK_NEAR(d.getParameter(D::SyncedR), 6.0, 1e-6); // unlinked: untouched
}

TEST_CASE("delay: smoothing modes Repitch, Fade and Jump handle a time change differently")
{
    using D = devices::Delay;
    auto run = [](int mode) {
        D d;
        plainDelay(d, 10.0f, 10.0f, 0.0f);
        d.setParameter(D::SmoothingMode, (float) mode);
        Stream s(d);
        s.run(2048);
        d.setParameter(D::TimeL, 0.04f);
        d.setParameter(D::TimeR, 0.04f);
        return s.impulse(6000).first;
    };
    const auto jump = run(2);
    CHECK(std::abs(peakAt(jump, 100, 5000) - 1920) <= 3);   // new time at once
    CHECK(peakIn(jump, 100, 1000) < 0.01f);

    const auto repitch = run(0);
    const int glided = peakAt(repitch, 100, 5000);
    CHECK(glided > 600);                                      // the delay is still gliding up towards 1920...
    CHECK(glided < 1500);                                     // ...so the echo arrives early
    CHECK(peakIn(repitch, 100, 5000) > 0.5f);

    const auto fade = run(1);
    CHECK(peakIn(fade, 470, 490) > 0.7f);                     // old tap still audible
    CHECK(peakIn(fade, 1910, 1930) > 0.55f);                  // new tap fading in
}

TEST_CASE("delay: the band-pass sits in the loop, so even the first echo is filtered")
{
    using D = devices::Delay;
    D open, narrow;
    plainDelay(open, 10.0f, 10.0f, 0.0f);
    plainDelay(narrow, 10.0f, 10.0f, 0.0f);
    narrow.setParameter(D::FilterOn, 1.0f);
    narrow.setParameter(D::FilterFrequency, 150.0f);
    narrow.setParameter(D::FilterBandwidth, 0.5f);
    Stream a(open), b(narrow);
    const auto o = a.impulse(2048).first;
    const auto n = b.impulse(2048).first;
    CHECK(peakIn(o, 100, 1500) > 0.9f);
    CHECK(peakIn(n, 100, 1500) < 0.3f);

    // Wide default (width 8): barely touches a mid-frequency signal.
    D wide;
    plainDelay(wide, 10.0f, 10.0f, 0.0f);
    wide.setParameter(D::FilterOn, 1.0f);
    Stream c(wide);
    const auto tone = c.run(9600, [](long p, int) { return 0.5f * std::sin(0.13f * (float) p); }).first;
    CHECK(peakIn(tone, 4000, 9600) > 0.4f);
}

TEST_CASE("delay: freeze keeps looping the buffer and ignores new input")
{
    using D = devices::Delay;
    D d;
    plainDelay(d, 10.0f, 10.0f, 0.9f);
    Stream s(d);
    s.impulse(1536);
    d.setParameter(D::Freeze, 1.0f);
    s.run(24000);
    const auto early = s.run(1024).first;
    s.run(40000);
    const auto late = s.run(1024).first;
    const float a = peakIn(early, 0, 1024), b = peakIn(late, 0, 1024);
    CHECK(a > 0.1f);
    CHECK_NEAR(b / a, 1.0, 0.02);                           // no decay at all
    // New input is ignored while frozen.
    const auto burst = s.run(2048, [](long, int) { return 0.8f; }).first;
    (void) burst;
    const auto after = s.run(1024).first;
    CHECK(peakIn(after, 0, 1024) < a * 1.05f);
}

TEST_CASE("delay: ping pong alternates, equal loudness keeps a constant power mix")
{
    using D = devices::Delay;
    D d;
    plainDelay(d, 20.0f, 20.0f, 0.8f);
    d.setParameter(D::PingPong, 1.0f);
    Stream s(d);
    auto [l, r] = s.impulse(6000);
    CHECK(peakIn(l, 900, 1020) > 0.4f);
    CHECK(peakIn(r, 900, 1020) < 0.01f);
    CHECK(peakIn(r, 1860, 1980) > 0.1f);
    CHECK(peakIn(l, 2820, 2940) > 0.05f);

    // Equal-loudness law: noise-like dry and wet add in power, so the level stays flat over Dry/Wet.
    auto level = [](float wet) {
        D e;
        plainDelay(e, 7.0f, 7.0f, 0.0f);
        e.setParameter(D::DryWetMode, 1.0f);
        e.setParameter(D::DryWet, wet);
        Stream st(e);
        unsigned seed = 7;
        const auto out = st.run(48000, [&seed](long, int) {
            seed = seed * 1664525u + 1013904223u;
            return (float) (seed >> 8) / 8388608.0f - 1.0f;
        }).first;
        return std::sqrt(energy(out, 4800, 48000) / 43200.0);
    };
    const double dry = level(0.0f), mid = level(0.5f), wet = level(1.0f);
    CHECK_NEAR(mid / dry, 1.0, 0.05);
    CHECK_NEAR(wet / dry, 1.0, 0.05);
}

TEST_CASE("delay: LFO modulates the delay time, zero amount leaves it untouched")
{
    using D = devices::Delay;
    auto render = [](float amount, int wave) {
        D d;
        plainDelay(d, 10.0f, 10.0f, 0.0f);
        d.setParameter(D::ModFrequency, 5.0f);
        d.setParameter(D::ModWaveform, (float) wave);
        d.setParameter(D::ModAmountTime, amount);
        Stream s(d);
        return s.run(48000, [](long p, int) { return 0.5f * std::sin(0.05f * (float) p); }).first;
    };
    const auto plain = render(0.0f, 0), flat = render(0.0f, 4);
    for (size_t i = 0; i < plain.size(); i += 997)
        CHECK_NEAR(plain[i], flat[i], 1e-6);               // amount 0: the wave is irrelevant
    for (int wave = 0; wave < 7; ++wave)
    {
        const auto mod = render(1.0f, wave);
        double diff = 0;
        float peak = 0;
        for (size_t i = 4800; i < mod.size(); ++i)
        {
            diff += std::fabs((double) mod[i] - plain[i]);
            peak = std::max(peak, std::fabs(mod[i]));
        }
        CHECK(diff > 1.0);
        CHECK(std::isfinite(peak));
        CHECK(peak < 1.01f);
    }
}

TEST_CASE("delay: tempo synced LFO locks to the song position while playing")
{
    using D = devices::Delay;
    // Synced to a quarter note: the cycle length is one beat, so the modulation at beat 0 and beat 4 is identical.
    auto render = [](double beats) {
        D d;
        plainDelay(d, 10.0f, 10.0f, 0.0f);
        d.setParameter(D::ModTimeMode, 2.0f);
        d.setParameter(D::ModSyncedRate, 4.0f);
        d.setParameter(D::ModWaveform, 1.0f);
        d.setParameter(D::ModAmountTime, 1.0f);
        devices::HostInfo host;
        host.tempo = 120.0;
        host.playing = true;
        host.beats = beats;
        d.setHostInfo(host);
        Stream s(d);
        return s.run(2048, [](long p, int) { return 0.5f * std::sin(0.05f * (float) p); }).first;
    };
    const auto at0 = render(0.0), at4 = render(4.0), atQuarter = render(0.25);
    double same = 0, different = 0;
    for (size_t i = 1000; i < at0.size(); ++i)
    {
        same += std::fabs((double) at0[i] - at4[i]);
        different += std::fabs((double) at0[i] - atQuarter[i]);
    }
    CHECK(same < 1e-3);
    CHECK(different > 1.0);
}

TEST_CASE("modulation lfo: waveforms stay in range and Morph reshapes them")
{
    using L = dsp::ModLfo;
    auto collect = [](int wave, float morph, int count = 1000) {
        L lfo;
        lfo.reset(11);
        Signal v;
        for (int i = 0; i < count; ++i)
            v.push_back(lfo.next(0.01, wave, morph));
        return v;
    };
    for (int wave = 0; wave < L::NumWaves; ++wave)
        for (float morph : {0.0f, 0.5f, 1.0f})
            for (float v : collect(wave, morph))
            {
                CHECK(v >= -1.001f);
                CHECK(v <= 1.001f);
            }
    auto dutyHigh = [](const Signal& v) {
        int high = 0;
        for (float x : v)
            high += x > 0.0f ? 1 : 0;
        return (double) high / (double) v.size();
    };
    CHECK_NEAR(dutyHigh(collect(L::Square, 0.0f)), 0.5, 0.02);
    CHECK(dutyHigh(collect(L::Square, 1.0f)) > 0.9);      // Morph widens the pulse
    const auto tri = collect(L::Triangle, 0.0f);
    CHECK_NEAR(*std::max_element(tri.begin(), tri.end()), 1.0, 0.05);
    CHECK_NEAR(*std::min_element(tri.begin(), tri.end()), -1.0, 0.05);

    // S&H holds its value between wraps; with Morph it glides.
    const auto steps = collect(L::SampleHold, 0.0f, 250);
    int changes = 0;
    for (size_t i = 1; i < steps.size(); ++i)
        changes += std::fabs(steps[i] - steps[i - 1]) > 1e-6f ? 1 : 0;
    CHECK(changes <= 3);                                   // 2.5 cycles -> at most 2-3 new values
    const auto glide = collect(L::SampleHold, 1.0f, 250);
    int moving = 0;
    for (size_t i = 1; i < glide.size(); ++i)
        moving += std::fabs(glide[i] - glide[i - 1]) > 1e-6f ? 1 : 0;
    CHECK(moving > 100);

    // Wander moves smoothly.
    const auto wander = collect(L::Wander, 0.0f, 4000);
    float maxStep = 0;
    for (size_t i = 1; i < wander.size(); ++i)
        maxStep = std::max(maxStep, std::fabs(wander[i] - wander[i - 1]));
    CHECK(maxStep < 0.1f);
}

// ---------------------------------------------------------------- Reverb

TEST_CASE("reverb: Shape delays the onset of the diffuse field")
{
    using R = devices::Reverb;
    auto onset = [](float shape) {
        R r;
        plainReverb(r, 1500.0f);
        r.setParameter(R::MixReflect, 0.0f);       // only the diffuse tail
        r.setParameter(R::Shape, shape);
        Stream s(r);
        const auto out = s.impulse(24000).first;
        for (int i = 0; i < (int) out.size(); ++i)
            if (std::fabs(out[(size_t) i]) > 1e-4f)
                return i;
        return (int) out.size();
    };
    CHECK(onset(1.0f) > onset(0.0f) + 1200);
}

TEST_CASE("reverb: Shape shortens the early reflections")
{
    using R = devices::Reverb;
    auto earlyTail = [](float shape) {
        R r;
        plainReverb(r, 1500.0f);
        r.setParameter(R::MixDiffuse, 0.0f);       // only the early reflections
        r.setParameter(R::Shape, shape);
        Stream s(r);
        const auto out = s.impulse(24000).first;
        return energy(out, 2400, 24000) / std::max(1e-12, energy(out, 0, 2400));
    };
    CHECK(earlyTail(0.0f) > earlyTail(1.0f) * 1.5);
}

TEST_CASE("reverb: Freeze Cut stops the input from adding, Flat bypasses the shelves while frozen")
{
    using R = devices::Reverb;
    auto frozenLevel = [](bool cut) {
        R r;
        plainReverb(r, 600.0f);
        r.setParameter(R::MixReflect, 0.0f);
        r.setParameter(R::CutOn, cut ? 1.0f : 0.0f);
        Stream s(r);
        s.run(9600, [](long p, int) { return 0.3f * std::sin(0.2f * (float) p); });
        r.setParameter(R::FreezeOn, 1.0f);
        const auto out = s.run(48000, [](long p, int) { return 0.3f * std::sin(0.31f * (float) p); }).first;
        return std::sqrt(energy(out, 24000, 48000) / 24000.0);
    };
    const double withCut = frozenLevel(true), withoutCut = frozenLevel(false);
    CHECK(withCut > 0.001);                                   // still sustaining the earlier sound
    CHECK(withoutCut > withCut * 1.2);                        // new input keeps feeding the frozen tail
    CHECK(std::isfinite(withoutCut));

    auto highContent = [](bool flat) {
        R r;
        plainReverb(r, 1200.0f);
        r.setParameter(R::MixReflect, 0.0f);
        r.setParameter(R::ShelfHighOn, 1.0f);
        r.setParameter(R::ShelfHiFreq, 1500.0f);
        r.setParameter(R::ShelfHiGain, 0.2f);
        r.setParameter(R::FlatOn, flat ? 1.0f : 0.0f);
        Stream s(r);
        unsigned seed = 3;
        s.run(12000, [&seed](long, int) {
            seed = seed * 1664525u + 1013904223u;
            return 0.5f * ((float) (seed >> 8) / 8388608.0f - 1.0f);
        });
        r.setParameter(R::FreezeOn, 1.0f);
        const auto out = s.run(96000).first;
        double e = 0;
        for (size_t i = 72000; i + 1 < out.size(); ++i)
        {
            const double d = (double) out[i + 1] - out[i];
            e += d * d;
        }
        return e;
    };
    CHECK(highContent(true) > highContent(false) * 3.0);      // shelves keep eating the highs unless Flat is on
}

TEST_CASE("reverb: Stereo applies to the dry signal only when asked to")
{
    using R = devices::Reverb;
    auto dryOut = [](bool onDry) {
        R r;
        plainReverb(r, 800.0f);
        r.setParameter(R::MixDirect, 0.0f);
        r.setParameter(R::StereoSeparation, 0.0f);
        r.setParameter(R::StereoOnDry, onDry ? 1.0f : 0.0f);
        Stream s(r);
        return s.impulse(256, 1.0f, 0.0f);
    };
    const auto [l0, r0] = dryOut(false);
    CHECK_NEAR(l0[0], 1.0, 1e-3);
    CHECK_NEAR(r0[0], 0.0, 1e-3);                            // untouched left-only dry
    const auto [l1, r1] = dryOut(true);
    CHECK_NEAR(l1[0], 0.5, 1e-3);                            // width 0 folds it to mono
    CHECK_NEAR(r1[0], 0.5, 1e-3);
}

TEST_CASE("reverb: Density, Scale and Diffusion change the diffuse field, all stay stable")
{
    using R = devices::Reverb;
    auto render = [](float density, float scale, float diffusion) {
        R r;
        plainReverb(r, 2500.0f);
        r.setParameter(R::MixReflect, 0.0f);
        r.setParameter(R::Density, density);
        r.setParameter(R::Scale, scale);
        r.setParameter(R::Diffusion, diffusion);
        Stream s(r);
        return s.impulse(96000).first;
    };
    const auto base = render(1.0f, 0.4f, 0.6f);
    for (const auto& variant : {render(0.0f, 0.4f, 0.6f), render(3.0f, 0.4f, 0.6f), render(1.0f, 1.0f, 0.6f),
                                render(1.0f, 0.4f, 0.0f), render(1.0f, 0.4f, 0.96f)})
    {
        double diff = 0;
        float peak = 0;
        for (size_t i = 0; i < variant.size(); ++i)
        {
            diff += std::fabs((double) variant[i] - base[i]);
            peak = std::max(peak, std::fabs(variant[i]));
        }
        CHECK(diff > 1e-3);
        CHECK(std::isfinite(peak));
        CHECK(peak < 3.0f);
    }
}

TEST_CASE("reverb: Size Smoothing glides a Size change, None jumps")
{
    using R = devices::Reverb;
    // Change Size from 100 to 200, then fire an impulse straight away. The first sound of the diffuse field comes out of
    // the shortest delay line: with None it already has its new, longer length; Slow and Fast are still on the way.
    auto onset = [](int smoothing) {
        R r;
        plainReverb(r, 3000.0f);
        r.setParameter(R::MixReflect, 0.0f);
        r.setParameter(R::Shape, 0.0f);
        r.setParameter(R::SizeSmoothing, (float) smoothing);
        Stream s(r);
        s.run(2048);
        r.setParameter(R::RoomSize, 200.0f);
        const auto out = s.impulse(24000).first;
        for (int i = 0; i < (int) out.size(); ++i)
        {
            CHECK(std::isfinite(out[(size_t) i]));
            if (std::fabs(out[(size_t) i]) > 1e-5f)
                return i;
        }
        return (int) out.size();
    };
    const int none = onset(0), slow = onset(1), fast = onset(2);
    CHECK(none > fast + 200);
    CHECK(fast > slow);
}

TEST_CASE("reverb: Cut high filter type is steeper than the shelf")
{
    using R = devices::Reverb;
    auto highEnergy = [](float type) {
        R r;
        plainReverb(r, 2000.0f);
        r.setParameter(R::MixReflect, 0.0f);
        r.setParameter(R::ShelfHighOn, 1.0f);
        r.setParameter(R::HighFilterType, type);
        r.setParameter(R::ShelfHiFreq, 1000.0f);
        r.setParameter(R::ShelfHiGain, 0.2f);
        Stream s(r);
        const auto out = s.impulse(72000).first;
        double e = 0;
        for (size_t i = 8000; i + 1 < out.size(); ++i)
        {
            const double d = (double) out[i + 1] - out[i];
            e += d * d;
        }
        return e;
    };
    CHECK(highEnergy(1.0f) < highEnergy(0.0f));
}

TEST_CASE("reverb: Spin modulates the early reflections")
{
    using R = devices::Reverb;
    auto render = [](bool spin) {
        R r;
        plainReverb(r, 800.0f);
        r.setParameter(R::MixDiffuse, 0.0f);
        r.setParameter(R::SpinOn, spin ? 1.0f : 0.0f);
        r.setParameter(R::SpinRate, 4.0f);
        r.setParameter(R::SpinAmount, 100.0f);
        Stream s(r);
        return s.run(48000, [](long p, int) { return 0.4f * std::sin(0.07f * (float) p); }).first;
    };
    const auto a = render(false), b = render(true);
    double diff = 0;
    for (size_t i = 12000; i < a.size(); ++i)
        diff += std::fabs((double) a[i] - b[i]);
    CHECK(diff > 1.0);
}
