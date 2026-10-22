// MG Low ladder (dsp/LadderFilter.h): analytic response, resonance, the two measured nonlinearities and anchor points measured on
// the reference (small numbers only: no audio or presets are stored), plus the native synth and Serum import wiring.

#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/LadderFilter.h"

#include <cmath>
#include <complex>
#include <vector>

using namespace sonora;

namespace
{
constexpr double kFs = 96000.0; // the engine's internal voice rate at 2x quality

struct Measured { double gain; double phaseDeg; };

// Steady-state gain / phase of a sine through the ladder.
Measured sineResponse(double fc, float res, int taps, double hz, double amplitude, float drive = 1.0f, bool exactPoles = true, double fs = kFs)
{
    dsp::LadderFilter filter;
    // exactPoles cancels the measured 0.3 % tuning (poles exactly at fc); otherwise fc is the displayed vendor cutoff
    filter.setup(fs, (float) (exactPoles ? fc / dsp::LadderFilter::kCutoffTuning : fc), res, taps);
    const int settle = (int) (fs * 0.15), measure = (int) (fs * 0.2);
    std::complex<double> acc = 0.0;
    for (int n = 0; n < settle + measure; ++n)
    {
        const double phase = 2.0 * 3.14159265358979323846 * hz * n / fs;
        const float y = filter.process((float) (amplitude * std::sin(phase)) * drive);
        if (n >= settle) acc += (double) y * std::exp(std::complex<double>(0, -phase));
    }
    acc *= 2.0 / measure;
    // acc = amplitude/2j * H ... compare with the input sine: H = acc / (amplitude * -j)
    const auto h = acc / std::complex<double>(0, -amplitude);
    return {std::abs(h) / (drive > 0 ? drive : 1.0), std::arg(h) * 180.0 / 3.14159265358979323846};
}

// Analytic TPT cascade (taps poles) with feedback 4 * res around four poles.
std::complex<double> analytic(double fc, double res, int taps, double hz, double fs = kFs)
{
    const double g = std::tan(3.14159265358979323846 * fc / fs);
    const std::complex<double> z = std::exp(std::complex<double>(0, 2.0 * 3.14159265358979323846 * hz / fs));
    const std::complex<double> pole = (g / (1 + g)) * (1.0 + 1.0 / z) / (1.0 - ((1 - g) / (1 + g)) / z);
    std::complex<double> tap = 1.0, loop = 1.0;
    for (int i = 0; i < taps; ++i) tap *= pole;
    for (int i = 0; i < 4; ++i) loop *= pole;
    return tap / (1.0 + 4.0 * res * loop);
}
} // namespace

TEST_CASE("ladder: small-signal response is the TPT cascade with feedback 4 * resonance, for every tap")
{
    for (int taps = 1; taps <= 4; ++taps)
        for (const double res : {0.0, 0.35, 0.8})
            for (const double hz : {110.0, 400.0, 900.0, 2500.0})
            {
                const auto got = sineResponse(1000.0, (float) res, taps, hz, 0.01);
                const auto want = analytic(1000.0, res, taps, hz);
                CHECK_NEAR(got.gain, std::abs(want), 0.004 * std::max(1.0, std::abs(want)));
                double dphase = got.phaseDeg - std::arg(want) * 180.0 / 3.14159265358979323846;
                while (dphase > 180) dphase -= 360;
                while (dphase < -180) dphase += 360;
                CHECK(std::abs(dphase) < 0.6);
            }
}

TEST_CASE("ladder: DC gain is one without resonance and 1/(1+k) with it; the tap changes the slope only")
{
    for (const float res : {0.0f, 0.5f})
    {
        const auto low = sineResponse(5000.0, res, 4, 20.0, 0.01);
        CHECK_NEAR(low.gain, 1.0 / (1.0 + 4.0 * res), 0.01);
    }
    const double a1 = sineResponse(1000.0, 0.0f, 1, 8000.0, 0.01).gain, a4 = sineResponse(1000.0, 0.0f, 4, 8000.0, 0.01).gain;
    CHECK(a1 > a4 * 20.0); // 6 dB/oct against 24 dB/oct three octaves above the cutoff
}

TEST_CASE("ladder: vendor anchor points (MG Low 24 / 12, 96 kHz internal rate)")
{
    // |H| of MG Low 24 without resonance at the displayed cutoff 2064 Hz: 1111.9 Hz -> -4.45 dB, 850.3 Hz -> -2.81 dB (measured).
    CHECK_NEAR(sineResponse(2064.19, 0.0f, 4, 1111.9, 0.01, 1.0f, false).gain, 0.5991, 0.012);
    CHECK_NEAR(sineResponse(2064.19, 0.0f, 4, 850.3, 0.01, 1.0f, false).gain, 0.7238, 0.012);
    CHECK_NEAR(sineResponse(425.0, 0.0f, 4, 261.6, 0.01, 1.0f, false).gain, 0.5255, 0.01);
    // Resonance 0.9 at 880 Hz with the cutoff at 936.6 Hz: small-signal gain 2.1286 (measured), phase near -176 deg.
    CHECK_NEAR(sineResponse(936.6, 0.9f, 4, 880.0, 0.003, 1.0f, false).gain, 2.1286, 0.07);
    // MG Low 12 with resonance 0.5: stage-2 tap, loop from stage 4 (the fits agreed within 1 %).
    CHECK_NEAR(sineResponse(936.6, 0.5f, 2, 880.0, 0.003).gain, std::abs(analytic(936.6, 0.5, 2, 880.0)), 0.02);
}

TEST_CASE("ladder: input soft clipper (cubic, +-3.31) and drive law")
{
    // Passband (cutoff far above the signal): output = clip(drive * x). Internal units: knee 4.966, maximum 3.310.
    const float knee = dsp::LadderFilter::kVoiceKnee;
    for (const float z : {0.1f, 1.0f, 2.5f, 4.0f})
        CHECK_NEAR(dsp::LadderFilter::inputClip(z), z - z * z * z / (3 * knee * knee), 1e-6);
    CHECK_NEAR(dsp::LadderFilter::inputClip(50.0f), 2.0f / 3.0f * knee, 1e-4);
    CHECK_NEAR(dsp::LadderFilter::inputClip(-50.0f), -2.0f / 3.0f * knee, 1e-4);
    // Measured: with drive 100 % the 0.0556 (output units) = 0.1573 internal sine reaches 0.813 output = 2.30 internal.
    const auto driven = sineResponse(22000.0, 0.0f, 4, 100.0, 0.1573, dsp::ladderDriveGain(1.0f));
    CHECK_NEAR(driven.gain * 0.1573 * dsp::ladderDriveGain(1.0f), 2.30, 0.07);
    CHECK_NEAR(dsp::ladderDriveGain(0.0f), 1.0, 1e-6);
    CHECK_NEAR(dsp::ladderDriveGain(0.5f), 5.061, 1e-3);
    CHECK_NEAR(dsp::ladderDriveGain(1.0f), 15.9874, 1e-3);
    float last = 0;
    for (int i = 0; i <= 100; ++i)
    {
        const float g = dsp::ladderDriveGain((float) i / 100);
        CHECK(g >= last);
        last = g;
    }
}

TEST_CASE("ladder: resonance compresses once the loop signal reaches the limiter, and stays bounded at full resonance")
{
    // Measured at resonance 0.9 near the cutoff (880 Hz / 936.6 Hz): gain 2.128 for small signals, 1.732 at x = 1.0027 internal.
    const float small = (float) sineResponse(936.6, 0.9f, 4, 880.0, 0.01, 1.0f, false).gain;
    const float large = (float) sineResponse(936.6, 0.9f, 4, 880.0, 1.0027, 1.0f, false).gain;
    CHECK(small > 2.0f);
    CHECK_NEAR(large / small, 1.7322 / 2.1286, 0.04);
    dsp::LadderFilter filter;
    filter.setup(kFs, 900.0f, 1.0f, 4);
    float peak = 0;
    for (int n = 0; n < (int) kFs; ++n)
    {
        const float y = filter.process(8.0f * std::sin(2.0f * 3.14159265f * 880.0f * (float) n / (float) kFs));
        CHECK(std::isfinite(y));
        peak = std::max(peak, std::abs(y));
    }
    CHECK(peak < 40.0f);
}

TEST_CASE("ladder: reset clears the state and coefficient updates keep it (smooth sweeps)")
{
    dsp::LadderFilter filter;
    filter.setup(kFs, 800.0f, 0.4f, 4);
    for (int n = 0; n < 2000; ++n) filter.process(std::sin(0.05f * (float) n));
    const float before = filter.process(0.5f);
    filter.setup(kFs, 801.0f, 0.4f, 4); // control-rate update: no click
    const float after = filter.process(0.5f);
    CHECK(std::abs(after - before) < 0.05f);
    filter.reset();
    CHECK_NEAR(filter.process(0.0f), 0.0f, 1e-9);
}

TEST_CASE("native synth: MG Low modes filter the generated oscillator and the vendor drive/mode map stays stable")
{
    using W = devices::WavetableSynth;
    CHECK(W::NumFilterTypes == 83);
    CHECK((int) W::MgLow6 == 7);
    auto make = [&](int mode, float cutoff, float resonance) {
        auto synth = std::make_unique<W>();
        synth->prepare(48000.0, 512);
        synth->setParameter(W::SubOn, 0);
        synth->setParameter(W::NoiseOn, 0);
        synth->setParameter(W::oscParam(0, W::On), 1);
        synth->setParameter(W::oscParam(0, W::Table), W::Basic);
        synth->setParameter(W::oscParam(0, W::Unison), 1);
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), (float) mode);
        synth->setParameter(W::filterParam(0, W::Cutoff), cutoff);
        synth->setParameter(W::filterParam(0, W::Resonance), resonance);
        synth->setParameter(W::envParam(0, W::Attack), 0);
        synth->setParameter(W::envParam(0, W::Sustain), 1);
        synth->setParameter(W::envParam(0, W::Release), 0);
        return synth;
    };
    auto energy = [&](W& synth) {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        double sum = 0;
        for (int block = 0; block < 40; ++block)
        {
            juce::AudioBuffer<float> audio(2, 512);
            audio.clear();
            synth.process(audio, midi);
            midi.clear();
            if (block >= 10)
                for (int i = 0; i < 512; ++i) { const double v = audio.getSample(0, i); sum += v * v; CHECK(std::isfinite(v)); }
        }
        return sum;
    };
    auto sixDb = make(W::MgLow6, 600.0f, 0.0f), twentyFourDb = make(W::MgLow24, 600.0f, 0.0f);
    const double e6 = energy(*sixDb), e24 = energy(*twentyFourDb);
    CHECK(e6 > e24 * 1.5); // the steeper ladder removes more of the saw's harmonics
    auto open = make(W::MgLow24, 18000.0f, 0.0f);
    CHECK(energy(*open) > e24 * 2.0);
    auto resonant = make(W::MgLow24, 600.0f, 0.9f);
    CHECK(std::abs(energy(*resonant) - e24) > 0.1 * e24); // resonance reshapes the spectrum (peak at the cutoff, weaker fundamental)
}
