// Monitor DSP checks: BS.1770 / EBU R128 loudness against reference signals, true peak, loudness range,
// stereo correlation, spectrum and oscilloscope, and the threaded MasterMonitor around them.

#include "TestFramework.h"
#include "dsp/Analyzers.h"
#include "dsp/Loudness.h"
#include "engine/MasterMonitor.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

using namespace sonora;

namespace
{
constexpr double kTwoPi = 6.283185307179586;

// A sine of peak amplitude `amp` in dBFS, same in both channels.
std::pair<std::vector<float>, std::vector<float>> sine(double sampleRate, double seconds, double hz, double ampDb, double phase = 0.0)
{
    const size_t n = (size_t) (sampleRate * seconds);
    const double amp = std::pow(10.0, ampDb / 20.0);
    std::vector<float> l(n);
    for (size_t i = 0; i < n; ++i)
        l[i] = (float) (amp * std::sin(kTwoPi * hz * (double) i / sampleRate + phase));
    return {l, l};
}

void feed(dsp::LoudnessMeter& meter, const std::vector<float>& l, const std::vector<float>& r)
{
    for (size_t i = 0; i < l.size(); i += 480)
    {
        const int n = (int) std::min<size_t>(480, l.size() - i);
        meter.process(l.data() + i, r.data() + i, n);
    }
}
} // namespace

TEST_CASE("loudness: a stereo 1 kHz sine at -23 dBFS reads -23 LUFS at 48 and 44.1 kHz (EBU TECH 3341 case 1)")
{
    for (const double sr : {48000.0, 44100.0, 96000.0})
    {
        dsp::LoudnessMeter meter(sr);
        const auto [l, r] = sine(sr, 20.0, 1000.0, -23.0);
        feed(meter, l, r);
        CHECK_NEAR(meter.momentary(), -23.0f, 0.1f);
        CHECK_NEAR(meter.shortTerm(), -23.0f, 0.1f);
        CHECK_NEAR(meter.integrated(), -23.0f, 0.1f);
        CHECK_NEAR(meter.samplePeakDb(), -23.0f, 0.05f);
    }
}

TEST_CASE("loudness: K-weighting makes low frequencies quieter and presence louder")
{
    auto loudnessAt = [](double hz) {
        dsp::LoudnessMeter meter(48000.0);
        const auto [l, r] = sine(48000.0, 6.0, hz, -20.0);
        feed(meter, l, r);
        return meter.integrated();
    };
    const float low = loudnessAt(60.0), mid = loudnessAt(1000.0), high = loudnessAt(8000.0);
    CHECK(low < mid - 0.5f); // RLB high-pass
    CHECK(high > mid + 3.0f); // head-related shelf (+4 dB above 2 kHz)
    CHECK_NEAR(mid, -20.0f, 0.15f);
}

TEST_CASE("loudness: gating ignores silence and quiet passages, momentary/short-term follow the signal")
{
    dsp::LoudnessMeter meter(48000.0);
    auto [l, r] = sine(48000.0, 10.0, 1000.0, -20.0);
    feed(meter, l, r);
    const std::vector<float> silence(480000, 0.0f);
    feed(meter, silence, silence);
    CHECK_NEAR(meter.integrated(), -20.0f, 0.15f); // absolute gate removes the silent blocks
    CHECK(meter.momentary() < -60.0f);              // while the momentary reading has fallen
    CHECK(meter.momentaryMax() > -20.5f);

    dsp::LoudnessMeter loudAndQuiet(48000.0);
    const auto loud = sine(48000.0, 10.0, 1000.0, -20.0);
    const auto quiet = sine(48000.0, 10.0, 1000.0, -45.0); // more than 10 LU under: removed by the relative gate
    feed(loudAndQuiet, loud.first, loud.second);
    feed(loudAndQuiet, quiet.first, quiet.second);
    CHECK_NEAR(loudAndQuiet.integrated(), -20.0f, 0.15f);

    meter.reset();
    CHECK(meter.integrated() <= dsp::kSilenceDb);
    CHECK(meter.momentary() <= dsp::kSilenceDb);
}

TEST_CASE("loudness: true peak sees the inter-sample peak a sample peak misses")
{
    dsp::LoudnessMeter meter(48000.0);
    // fs/4 with a 45 degree phase: every sample is +-0.707 of the true amplitude 1.0
    const auto [l, r] = sine(48000.0, 1.0, 12000.0, 0.0, kTwoPi / 8.0);
    feed(meter, l, r);
    CHECK_NEAR(meter.samplePeakDb(), -3.0f, 0.1f);
    CHECK(meter.truePeakDb() > -0.8f);
    CHECK(meter.truePeakDb() < 0.8f);
}

TEST_CASE("loudness: loudness range of alternating -20 / -30 LUFS passages is about 10 LU")
{
    dsp::LoudnessMeter meter(48000.0);
    for (int i = 0; i < 12; ++i)
    {
        const auto [l, r] = sine(48000.0, 6.0, 1000.0, i % 2 == 0 ? -20.0 : -30.0);
        feed(meter, l, r);
    }
    CHECK(meter.loudnessRange() > 7.0f);
    CHECK(meter.loudnessRange() < 11.0f);
    dsp::LoudnessMeter steady(48000.0);
    const auto [l, r] = sine(48000.0, 30.0, 1000.0, -20.0);
    feed(steady, l, r);
    CHECK(steady.loudnessRange() < 0.5f);
}

TEST_CASE("stereo meter: correlation, balance and width")
{
    dsp::StereoMeter meter(48000.0);
    const auto [a, b] = sine(48000.0, 1.0, 440.0, -6.0);
    meter.process(a.data(), b.data(), (int) a.size());
    CHECK_NEAR(meter.correlation(), 1.0f, 0.01f);
    CHECK_NEAR(meter.balance(), 0.0f, 0.01f);
    CHECK(meter.width() < 0.01f);

    std::vector<float> inverted(a.size());
    for (size_t i = 0; i < a.size(); ++i) inverted[i] = -a[i];
    meter.process(a.data(), inverted.data(), (int) a.size());
    meter.process(a.data(), inverted.data(), (int) a.size());
    CHECK_NEAR(meter.correlation(), -1.0f, 0.01f);
    CHECK(meter.width() > 0.99f);

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> n1(96000), n2(96000);
    for (auto& v : n1) v = dist(rng);
    for (auto& v : n2) v = dist(rng);
    meter.process(n1.data(), n2.data(), (int) n1.size());
    CHECK(std::abs(meter.correlation()) < 0.1f);

    std::vector<float> silent(48000, 0.0f);
    for (int i = 0; i < 4; ++i) // left only, long enough for the 300 ms window to forget the right channel
        meter.process(a.data(), silent.data(), (int) a.size());
    CHECK(meter.balance() < -0.95f);

    std::array<std::pair<float, float>, dsp::StereoMeter::kGoniometerPoints> points{};
    meter.goniometer(points);
    float maxMid = 0;
    for (const auto& p : points) maxMid = std::max(maxMid, std::abs(p.second));
    CHECK(maxMid > 0.3f);
}

TEST_CASE("spectrum: a 1 kHz sine peaks in the bar around 1 kHz at about its level, bars follow release")
{
    dsp::SpectrumAnalyzer spectrum(48000.0);
    const auto [l, r] = sine(48000.0, 0.5, 1000.0, -6.0);
    spectrum.push(l.data(), r.data(), (int) l.size());
    spectrum.analyze(0.033);
    int best = 0;
    for (int i = 0; i < dsp::SpectrumAnalyzer::kBars; ++i)
        if (spectrum.bars()[(size_t) i] > spectrum.bars()[(size_t) best]) best = i;
    CHECK(std::abs(dsp::SpectrumAnalyzer::barFrequency(best) - 1000.0f) < 120.0f);
    CHECK(spectrum.bars()[(size_t) best] > -8.5f);
    CHECK(spectrum.bars()[(size_t) best] < -4.0f);
    CHECK(spectrum.bars()[(size_t) 2] < -60.0f); // far from the tone
    int curveBest = 0;
    for (int i = 0; i < dsp::SpectrumAnalyzer::kCurvePoints; ++i)
        if (spectrum.curve()[(size_t) i] > spectrum.curve()[(size_t) curveBest]) curveBest = i;
    CHECK(std::abs(dsp::SpectrumAnalyzer::curveFrequency(curveBest) - 1000.0f) < 100.0f);

    const std::vector<float> silence(24000, 0.0f);
    spectrum.push(silence.data(), silence.data(), (int) silence.size());
    const float before = spectrum.bars()[(size_t) best];
    spectrum.analyze(0.033);
    CHECK(spectrum.bars()[(size_t) best] < before);       // released
    CHECK(spectrum.bars()[(size_t) best] > before - 3.0f); // but not instantly
    CHECK(spectrum.barPeaks()[(size_t) best] >= spectrum.bars()[(size_t) best]);
}

TEST_CASE("scope: triggered traces start on a rising zero crossing and show the waveform extremes")
{
    dsp::Scope scope(48000.0);
    const auto [l, r] = sine(48000.0, 0.3, 100.0, -6.0, 0.3);
    scope.push(l.data(), r.data(), (int) l.size());
    std::vector<dsp::Scope::Column> trace;
    scope.trace(240, 40.0, true, trace);
    REQUIRE((int) trace.size() == 240);
    float lo = 1, hi = -1;
    for (const auto& c : trace) { lo = std::min(lo, c.minL); hi = std::max(hi, c.maxL); }
    CHECK_NEAR(hi, 0.5f, 0.03f);
    CHECK_NEAR(lo, -0.5f, 0.03f);
    CHECK(trace.front().minL > -0.06f); // begins just after an upward crossing
    CHECK(trace.front().maxL < 0.2f);
    scope.trace(240, 40.0, false, trace);
    CHECK(trace.size() == 240);
}

TEST_CASE("master monitor: the analysis thread measures what the audio thread pushes, and reset restarts integration")
{
    engine::MasterMonitor monitor;
    monitor.prepare(48000.0);
    monitor.setEnabled(true);
    const auto [l, r] = sine(48000.0, 4.0, 1000.0, -23.0);
    for (size_t i = 0; i < l.size(); i += 512) // like an audio callback
    {
        const int n = (int) std::min<size_t>(512, l.size() - i);
        monitor.push(l.data() + i, r.data() + i, n);
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // faster than real time, slower than the ring fills
    }
    engine::MonitorSnapshot s;
    for (int wait = 0; wait < 200; ++wait)
    {
        s = monitor.snapshot();
        if (s.integratedSeconds > 3.9 && s.integratedLufs > -30.0f)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_NEAR(s.momentaryLufs, -23.0f, 0.3f);
    CHECK_NEAR(s.integratedLufs, -23.0f, 0.3f);
    CHECK_NEAR(s.correlation, 1.0f, 0.02f);
    CHECK(s.scope.size() == (size_t) engine::MasterMonitor::kScopeColumns);
    CHECK(s.serial > 0);

    monitor.resetLoudness();
    for (int wait = 0; wait < 100; ++wait)
    {
        s = monitor.snapshot();
        if (s.integratedLufs <= dsp::kSilenceDb) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(s.integratedLufs <= dsp::kSilenceDb);
    monitor.setEnabled(false);
    const auto frozen = monitor.snapshot().serial;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(monitor.snapshot().serial == frozen); // no analysis while disabled
}
