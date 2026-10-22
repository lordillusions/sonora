// DEV-18i: laws measured on the hosted Serum 2 (LFO rate, macro -> macro, master tuning, filter level / drive, voice amp,
// noise / sub volume, the normal voice filter family). Constants come from those measurements; the checks keep the native
// engine on them.
#include "TestFramework.h"
#include "devices/native/WavetableSynth.h"
#include "dsp/DiodeLadderFilter.h"
#include "dsp/MeasuredCombFilter.h"
#include "dsp/MeasuredMultiFilter.h"
#include "dsp/MeasuredSvfFilter.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;

std::vector<float> render(W& synth, int samples)
{
    std::vector<float> out;
    juce::AudioBuffer<float> audio(2, 256);
    for (int done = 0; done < samples; done += 256)
    {
        audio.clear(); juce::MidiBuffer midi; synth.process(audio, midi);
        for (int i = 0; i < 256; ++i) out.push_back(audio.getSample(0, i));
    }
    out.resize(static_cast<size_t>(samples));
    return out;
}
double rms(const std::vector<float>& x, size_t from)
{
    double sum = 0;
    for (size_t i = from; i < x.size(); ++i) sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(x.size() - from));
}
int crossings(const std::vector<float>& x, size_t from)
{
    int count = 0;
    for (size_t i = from + 1; i < x.size(); ++i) count += (x[i - 1] < 0) != (x[i] < 0);
    return count;
}
void route(W& synth, int slot, int source, int destination, float amount, W::ImportedModulationDomain domain)
{
    REQUIRE(synth.setImportedModulationDomain(destination, domain));
    synth.setParameter(W::modParam(slot, W::ModSource), static_cast<float>(source));
    synth.setParameter(W::modParam(slot, W::ModDestination), static_cast<float>(destination));
    synth.setParameter(W::modParam(slot, W::ModAmount), amount);
}
std::unique_ptr<W> sine(int note = 57)
{
    auto synth = std::make_unique<W>();
    synth->prepare(48000, 256);
    synth->setParameter(W::filterParam(0, W::FilterOn), 0);
    synth->setParameter(W::oscParam(0, W::Position), 0.2f); // sine frame of Default Shapes
    synth->queueUiMidi(0x90, static_cast<uint8_t>(note), 100);
    return synth;
}
} // namespace

TEST_CASE("serum parity: measured normal filter family (TPT sections, resonance law, gain compensation, Var)")
{
    using F = dsp::MeasuredSvfFilter;
    const auto sineGain = [](int shape, float hz, float cutoff, float resonance, float var) {
        F filter; filter.setup(96000, cutoff, resonance, var, shape);
        double in = 0, out = 0;
        for (int i = 0; i < 96000; ++i)
        {
            const float x = std::sin(2 * std::numbers::pi_v<float> * hz * static_cast<float>(i) / 96000);
            const float y = filter.process(x);
            if (i >= 48000) { in += static_cast<double>(x) * x; out += static_cast<double>(y) * y; }
        }
        return std::sqrt(out / in);
    };
    // Low 12 passband: g0 / (1 + gc r) (measured 0.968 / (1 + 4 r)); Var 1 removes the compensation.
    CHECK_NEAR(sineGain(F::Low12, 50, 1000, 0.5f, 0), 0.978 / (1 + 4.037 * 0.5), 0.01);
    CHECK_NEAR(sineGain(F::Low12, 50, 1000, 0.5f, 1), 0.978, 0.01);
    // 24 dB slope two octaves above the cutoff, highs pass a high-pass, the notch removes the cutoff.
    CHECK(sineGain(F::Low24, 4000, 1000, 0, 0) < 0.08);
    CHECK(sineGain(F::Low12, 4000, 1000, 0, 0) > 0.05);
    CHECK(sineGain(F::High12, 50, 1000, 0, 0) < 0.01);
    CHECK_NEAR(sineGain(F::High12, 16000, 1000, 0, 0), 0.962, 0.03);
    CHECK(sineGain(F::Notch12, 1000, 1000, 0.25f, 0) < 0.05);
    CHECK(sineGain(F::Band12, 1000, 1000, 0.5f, 0) > 5 * sineGain(F::Band12, 100, 1000, 0.5f, 0));
    // The analog magnitude used by the editor agrees with the running filter well below the cutoff.
    for (int shape = 0; shape < F::ShapeCount; ++shape)
        CHECK_NEAR(F::magnitude(shape, 300, 3000, 0.3f, 0), sineGain(shape, 300, 3000, 0.3f, 0), 0.03);
    // Resonance 1 rings without blowing up.
    F ringing; ringing.setup(96000, 500, 1, 0, F::Low24);
    float peak = 0;
    for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(ringing.process(i == 0 ? 1.0f : 0.0f)));
    CHECK(std::isfinite(peak)); CHECK(peak < 10);
}

TEST_CASE("serum parity: LFO rate routes follow the quartic rate law and synced LFOs snap to allowed divisions")
{
    // Free 1 Hz LFO, Fixed -> LFO 1 Rate +0.25: Hz = 100 (1/100^(1/4) + 0.25)^4 = 10.28 (hosted: 10.2797).
    const auto phaseAfter = [](bool routed, bool synced) {
        W synth; synth.prepare(48000, 256);
        synth.setParameter(W::lfoParam(0, W::Mode), W::Retrigger);
        synth.setParameter(W::lfoParam(0, W::SyncOn), synced ? 1.0f : 0.0f);
        synth.setParameter(W::lfoParam(0, W::Rate), synced ? 6.25f : 1.0f); // synced: the stored rate of 1/4
        synth.setParameter(W::lfoParam(0, W::Division), static_cast<float>(dsp::LfoTempoDivisions::fromStoredRate(6.25, false, false)));
        if (routed) route(synth, 0, W::Fixed, W::Lfo1Rate, synced ? 0.1f : 0.25f, W::ImportedModulationDomain::Unit);
        synth.queueUiMidi(0x90, 60, 100);
        render(synth, 2304);
        return synth.liveState().lfoPhase[0];
    };
    const double seconds = 2304.0 / 48000;
    CHECK_NEAR(phaseAfter(false, false), 1.0 * seconds, 0.01);
    const double expected = 100 * std::pow(std::pow(0.01, 0.25) + 0.25, 4) * seconds;
    CHECK_NEAR(phaseAfter(true, false), expected - std::floor(expected), 0.01);
    // 1/4 at 120 BPM (2 Hz) + 0.1 lands on bin 25 (1/16 dotted), not allowed without Dotted -> 1/8 (4 Hz, hosted 3.9997).
    CHECK_NEAR(phaseAfter(true, true), 4.0 * seconds, 0.01);
}

TEST_CASE("serum parity: macro -> macro, master tuning and voice amp routes")
{
    // Macro 2 = stored 0 + 0.5 x Macro 1 (at 1) sounds like Macro 2 stored at 0.5.
    const auto level = [](float stored, bool chained) {
        auto synth = sine();
        synth->setParameter(W::oscParam(0, W::Level), 0);
        synth->setParameter(W::macroParam(0), 1);
        synth->setParameter(W::macroParam(1), stored);
        if (chained) route(*synth, 0, W::Macro1, W::Macro2Value, 0.5f, W::ImportedModulationDomain::Unit);
        route(*synth, 1, W::Macro2, W::Osc1Level, 1, W::ImportedModulationDomain::SquaredLevel);
        return rms(render(*synth, 9600), 4800);
    };
    const double direct = level(0.5f, false), chained = level(0, true);
    CHECK(direct > 0.01); CHECK_NEAR(chained / direct, 1, 0.01);
    // Master tuning: 128 semitones per unit -> 12/128 is one octave.
    const auto tuned = [](float amount) {
        auto synth = sine();
        if (amount != 0.0f) route(*synth, 0, W::Fixed, W::MasterTuning, amount, W::ImportedModulationDomain::CoarsePitch128);
        return crossings(render(*synth, 9600), 4800);
    };
    CHECK_NEAR(static_cast<double>(tuned(12.0f / 128)) / tuned(0), 2.0, 0.05);
    // Voice amp: gain 2 u around the stored 0.5 -> +0.25 is x1.5 (hosted +3.52 dB).
    const auto amp = [](bool routed) {
        auto synth = sine();
        if (routed) route(*synth, 0, W::Fixed, W::VoiceAmp, 0.25f, W::ImportedModulationDomain::Unit);
        return rms(render(*synth, 9600), 4800);
    };
    CHECK_NEAR(amp(true) / amp(false), 1.5, 0.01);
}

TEST_CASE("serum parity: imported mod wheel position, sub volume and filter output level routes")
{
    // The stored wheel position plays as the wheel until CC 1 arrives.
    {
        auto synth = sine();
        synth->setParameter(W::oscParam(0, W::Level), 0);
        route(*synth, 0, W::ModWheel, W::Osc1Level, 1, W::ImportedModulationDomain::SquaredLevel);
        CHECK(rms(render(*synth, 4800), 2400) < 1e-6);
        synth->setImportedModWheel(50);
        synth->reset(); synth->queueUiMidi(0x90, 57, 100);
        CHECK(rms(render(*synth, 4800), 2400) > 0.01);
    }
    // Sub volume: squared level like the oscillators (0.25 + 0.25 normalized -> 0.5625, x2.25).
    const auto sub = [](bool routed) {
        auto synth = sine();
        synth->setParameter(W::oscParam(0, W::On), 0);
        synth->setParameter(W::SubOn, 1); synth->setParameter(W::SubLevel, 0.25f);
        if (routed) route(*synth, 0, W::Fixed, W::SubAmplitude, 0.25f, W::ImportedModulationDomain::SquaredLevel);
        else REQUIRE(synth->setImportedModulationDomain(W::SubAmplitude, W::ImportedModulationDomain::SquaredLevel));
        return rms(render(*synth, 9600), 4800);
    };
    CHECK_NEAR(sub(true) / sub(false), 2.25, 0.02);
    // Filter output level: 4 u^2 with u = sqrt(stored / 4) plus the route (0.25 -> u 0.25 + 0.25 -> level 1, x4).
    const auto filtered = [](bool routed) {
        auto synth = sine();
        synth->setParameter(W::SourceRoutingOn, 1);
        synth->setParameter(W::sourceRoutingParam(0, W::SourceDestination), W::RouteToFilters);
        synth->setParameter(W::sourceRoutingParam(0, W::SourceFilterBalance), -1);
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), W::SerumLow12);
        synth->setParameter(W::filterParam(0, W::Cutoff), 20000);
        synth->setParameter(W::filterOutputParam(0), 0.25f);
        if (routed) route(*synth, 0, W::Fixed, W::Filter1Level, 0.25f, W::ImportedModulationDomain::Unit);
        return rms(render(*synth, 9600), 4800);
    };
    const double quiet = filtered(false);
    CHECK(quiet > 1e-4); CHECK_NEAR(filtered(true) / quiet, 4, 0.05);
}

TEST_CASE("serum parity: distortion warps shape the table sample like the hosted reference")
{
    // A sine through Dist Tube at full amount: hosted harmonics h3 -9.8 dB, h5 -14.5 dB re h1; amount 0 stays a sine.
    const auto harmonics = [](float amount) {
        auto synth = sine(36);
        synth->setParameter(W::oscParam(0, W::Warp1Mode), W::DistTube);
        synth->setParameter(W::oscParam(0, W::Warp1Amount), amount);
        const auto out = render(*synth, 48000);
        const double f0 = 440 * std::exp2((36 - 69) / 12.0);
        std::array<double, 6> magnitude{};
        for (size_t k = 0; k < magnitude.size(); ++k)
        {
            double re = 0, im = 0;
            for (size_t i = 24000; i < out.size(); ++i)
            {
                const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * static_cast<double>(i - 24000) / 24000.0);
                const double phase = 2 * std::numbers::pi * static_cast<double>(k + 1) * f0 * static_cast<double>(i) / 48000;
                re += w * out[i] * std::cos(phase); im += w * out[i] * std::sin(phase);
            }
            magnitude[k] = std::hypot(re, im);
        }
        return magnitude;
    };
    const auto full = harmonics(1), clean = harmonics(0);
    CHECK_NEAR(20 * std::log10(full[2] / full[0]), -9.8, 0.5);
    CHECK_NEAR(20 * std::log10(full[4] / full[0]), -14.5, 0.5);
    CHECK(20 * std::log10(clean[2] / clean[0]) < -50);
    CHECK(std::string(W::isDistortionWarp(W::DistSoftSat) ? "yes" : "no") == "yes");
}

TEST_CASE("serum parity: LFO point buses move the assigned curve point (x shown here) per voice")
{
    // Envelope-mode 1 Hz triangle (peak at x = 0.5) drives A Level; Fixed -> point bus 1 (+0.25) moves the peak to x = 0.75.
    const auto peakTime = [](bool moved) {
        auto synth = sine();
        synth->setImportedLfoProfile(true);
        dsp::CurveGraph triangle; triangle.count = 3;
        triangle.x[0] = 0; triangle.y[0] = 0; triangle.x[1] = 0.5f; triangle.y[1] = 1; triangle.x[2] = 1; triangle.y[2] = 0;
        REQUIRE(synth->setLfoCurve(0, triangle));
        synth->setParameter(W::lfoParam(0, W::Shape), W::CustomLfoShape);
        synth->setParameter(W::lfoParam(0, W::Mode), W::OneShot);
        synth->setParameter(W::lfoParam(0, W::SyncOn), 0); synth->setParameter(W::lfoParam(0, W::Rate), 1);
        synth->setParameter(W::oscParam(0, W::Level), 0);
        route(*synth, 0, W::Lfo1, W::Osc1Level, 1, W::ImportedModulationDomain::SquaredLevel);
        if (moved)
        {
            route(*synth, 1, W::Fixed, W::LfoPointBus1, 0.25f, W::ImportedModulationDomain::Unit);
            synth->setImportedLfoPointAssignments({{0, 0, 1, 0}});
        }
        synth->reset(); synth->queueUiMidi(0x90, 57, 100);
        const auto out = render(*synth, 48000);
        size_t best = 0; float peak = 0;
        for (size_t block = 0; block + 480 <= out.size(); block += 480)
        {
            float p = 0; for (size_t i = block; i < block + 480; ++i) p = std::max(p, std::abs(out[i]));
            if (p > peak) { peak = p; best = block; }
        }
        return static_cast<double>(best) / 48000;
    };
    CHECK_NEAR(peakTime(false), 0.5, 0.03);
    CHECK_NEAR(peakTime(true), 0.75, 0.03);
}

TEST_CASE("serum parity: semitone routes step like the 25-value reference control; filter balance and blend add linearly")
{
    // Hosted: +2 % -> +1 st, +5 % -> +1, +10 % -> +3, -5 % -> -1 (floor(25 u) - 12 around the stored 0).
    const auto ratio = [](float amount) {
        auto synth = sine();
        route(*synth, 0, W::Fixed, W::Osc1Semi, amount, W::ImportedModulationDomain::Unit);
        const int routed = crossings(render(*synth, 9600), 4800);
        auto plain = sine();
        return static_cast<double>(routed) / crossings(render(*plain, 9600), 4800);
    };
    CHECK_NEAR(ratio(0.02f), std::exp2(1 / 12.0), 0.02);
    CHECK_NEAR(ratio(0.10f), std::exp2(3 / 12.0), 0.02);
    CHECK_NEAR(ratio(-0.05f), std::exp2(-1 / 12.0), 0.02);
    CHECK(std::string(W::destinationName(W::Source5Balance)) == "Sub Filter Balance");
    CHECK(std::string(W::destinationName(W::Osc3Width)) == "C Width");
}

namespace
{
// RMS gain of a sine through `process` at 96 kHz (second half of one second).
template <typename Process> double sineGain96(float hz, Process&& process)
{
    double in = 0, out = 0;
    for (int i = 0; i < 96000; ++i)
    {
        const float x = std::sin(2 * std::numbers::pi_v<float> * hz * static_cast<float>(i) / 96000);
        const float y = process(x);
        if (i >= 48000) { in += static_cast<double>(x) * x; out += static_cast<double>(y) * y; }
    }
    return std::sqrt(out / in);
}
double db(double gain) { return 20 * std::log10(gain); }
} // namespace

TEST_CASE("serum parity: peak, dual and morph filter types (mixed SVF sections, Var laws)")
{
    using F = dsp::MeasuredMultiFilter;
    const auto gain = [](int shape, float hz, float cutoff, float resonance, float var) {
        F filter; filter.setup(96000, cutoff, resonance, var, shape);
        return sineGain96(hz, [&](float x) { return filter.process(x); });
    };
    // Peak 12 at resonance 0: (s^2 + s + 1) / (s^2 + 2 s + 1) = -6 dB at the cutoff; Var restores the resonance level loss.
    CHECK_NEAR(db(gain(F::Peak12, 1000, 1000, 0, 0)), -6.02, 0.3);
    CHECK_NEAR(gain(F::Peak12, 60, 1000, 0.5f, 0), 1.0 / 3, 0.02);
    CHECK_NEAR(gain(F::Peak12, 60, 1000, 0.5f, 1), 1.0, 0.03);
    // Dual types: the second cutoff is MIDI note 128 x Var, independent of the cutoff (measured 330 Hz at Var 0.5).
    CHECK_NEAR(F::dualFrequency(0.5f), 329.63f, 0.05f);
    CHECK_NEAR(F::dualFrequency(0.25f), 51.91f, 0.05f);
    // LH 12, cutoff 2 kHz, Var 0.5: a band between 330 Hz and 2 kHz.
    CHECK(gain(F::LH12, 900, 2000, 0, 0.5f) > 0.6);
    CHECK(gain(F::LH12, 60, 2000, 0, 0.5f) < 0.05);
    CHECK(gain(F::LH12, 12000, 2000, 0, 0.5f) < 0.05);
    // The notch of LN 12 ignores resonance (measured k = 2), NN 12's does not.
    CHECK_NEAR(gain(F::LN12, 380, 6000, 0.9f, 0.5f), gain(F::LN12, 380, 6000, 0, 0.5f), 0.05);
    CHECK(gain(F::NN12, 380, 6000, 0.9f, 0.5f) > 2 * gain(F::NN12, 380, 6000, 0, 0.5f));
    // Morph L/B/H 12 at Var 0.25 is half low-pass, half (unnormalised) band-pass; Var 0 / 1 are the pure low / high outputs.
    CHECK_NEAR(gain(F::LBH12, 100, 1000, 0, 0), 1, 0.02);
    CHECK(gain(F::LBH12, 100, 1000, 0, 1) < 0.02);
    CHECK_NEAR(F::magnitude(F::LBH12, 1000, 1000, 0, 0.25f), std::abs(std::complex<float>(0.5f, 0.5f) / std::complex<float>(0, 2)), 0.01);
    // The analog magnitude used by the editor agrees with the running filter well below the top octave.
    for (int shape = 0; shape < F::ShapeCount; ++shape)
        for (float var : {0.2f, 0.6f})
            CHECK_NEAR(F::magnitude(shape, 300, 3000, 0.3f, var), gain(shape, 300, 3000, 0.3f, var), 0.04);
    // Resonance 1 rings without blowing up.
    F ringing; ringing.setup(96000, 500, 1, 0.5f, F::BPN24);
    float peak = 0;
    for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(ringing.process(i == 0 ? 1.0f : 0.0f)));
    CHECK(std::isfinite(peak)); CHECK(peak < 10);
}

TEST_CASE("serum parity: Ladder Acid is the measured coupled diode ladder")
{
    using F = dsp::DiodeLadderFilter;
    const auto gain = [](float hz, float cutoff, float resonance) {
        F filter; filter.setup(96000, cutoff, resonance);
        return sineGain96(hz, [&](float x) { return filter.process(x * 0.05f) / 0.05f; });
    };
    // Hosted Serum 2, cutoff 0.5 (425 Hz), resonance 50 % (harmonics 9 / 31 / 61 of C1): +6.71 / -23.29 / -42.05 dB.
    CHECK_NEAR(db(gain(294.33f, 425, 0.5f)), 6.71, 0.5);
    CHECK_NEAR(db(gain(1013.8f, 425, 0.5f)), -23.29, 0.5);
    CHECK_NEAR(db(gain(1994.89f, 425, 0.5f)), -42.05, 0.7);
    // Resonance 0: -5.4 dB at 65 Hz, -12.8 dB at 164 Hz (the 0.12 w pole of the coupled ladder).
    CHECK_NEAR(db(gain(65.4f, 425, 0)), -5.4, 0.5);
    CHECK_NEAR(db(gain(163.5f, 425, 0)), -12.8, 0.5);
    for (float hz : {100.0f, 400.0f, 1500.0f}) CHECK_NEAR(db(F::magnitude(hz, 425, 0.5f)), db(gain(hz, 425, 0.5f)), 0.3);
    // Inverted output (as the reference): a DC step settles at -(0.99 + 0.945 k) / (1 + k).
    F step; step.setup(96000, 1000, 0.5f);
    float last = 0;
    for (int i = 0; i < 20000; ++i) last = step.process(0.1f);
    const float k = F::kFeedback * 0.5f;
    CHECK_NEAR(last, 0.1f * F::outputGain(k) / (1 + k), 0.002f);
}

TEST_CASE("serum parity: comb, flanger and phaser types (delay of 1 / cutoff, feedback r, measured mixes)")
{
    using F = dsp::MeasuredCombFilter;
    std::vector<float> line(static_cast<size_t>(F::lineLength(96000)));
    const auto gain = [&](int shape, float hz, float cutoff, float resonance, float var) {
        F filter; filter.setup(96000, cutoff, resonance, var, shape, static_cast<int>(line.size()));
        return sineGain96(hz, [&](float x) { return filter.process(x * 0.05f, line.data(), static_cast<int>(line.size()) - 1) / 0.05f; });
    };
    // Comb +, r = 0.5: x / 3 + 0.25 z / (1 - 0.5 z): 0.833 on the teeth (multiples of the cutoff), 0.167 between them.
    CHECK_NEAR(gain(F::CombP, 850, 425, 0.5f, 0), 0.8333, 0.02);
    CHECK_NEAR(gain(F::CombP, 637.5f, 425, 0.5f, 0), 0.1667, 0.01);
    CHECK_NEAR(gain(F::CombN, 637.5f, 425, 0.5f, 0), 0.8333, 0.02); // "-" puts the teeth between the multiples
    // Resonance 0: a comb is a flat x 0.5, a flanger notches the odd multiples of half the cutoff.
    CHECK_NEAR(gain(F::CombP, 1234, 425, 0, 0), 0.5, 0.01);
    CHECK(gain(F::FlangeP, 637.5f, 425, 0, 0) < 0.02);
    CHECK_NEAR(gain(F::FlangeP, 850, 425, 0, 0), 1.0, 0.02);
    // Phaser 12, resonance 0: 0.5 (1 + A^2) notches at the allpass corner (~cutoff / 2).
    CHECK(gain(F::Phase12P, static_cast<float>(F::allpassCorner(1000)), 1000, 0, 0) < 0.02);
    CHECK_NEAR(F::allpassCorner(425) / 425, 0.5, 0.005);
    // L6 at Var 0 removes the wet path: only the dry x / (2 (1 + r)) remains.
    CHECK_NEAR(gain(F::CombL6P, 850, 425, 0.5f, 0), 1.0 / 3, 0.01);
    // The editor magnitude follows the running filter.
    for (int shape : {F::CombP, F::FlangeN, F::Phase24P, F::Phase48N, F::CombH6P})
        for (float hz : {300.0f, 1100.0f})
            CHECK_NEAR(F::magnitude(shape, hz, 425, 0.4f, 0.5f), gain(shape, hz, 425, 0.4f, 0.5f), 0.03);
    // A recycled voice does not replay the previous note's delay line.
    F filter; filter.setup(96000, 425, 0.9f, 0, F::CombP, static_cast<int>(line.size()));
    for (int i = 0; i < 2000; ++i) filter.process(0.5f, line.data(), static_cast<int>(line.size()) - 1);
    filter.reset();
    float echo = 0;
    for (int i = 0; i < 2000; ++i) echo = std::max(echo, std::abs(filter.process(0, line.data(), static_cast<int>(line.size()) - 1)));
    CHECK(echo < 1e-9f);
    // Full resonance stays bounded.
    F loud; loud.setup(96000, 200, 1, 0, F::FlangeP, static_cast<int>(line.size()));
    float peak = 0;
    for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(loud.process(std::sin(0.013f * static_cast<float>(i)), line.data(), static_cast<int>(line.size()) - 1)));
    CHECK(std::isfinite(peak)); CHECK(peak < 20);
}

TEST_CASE("serum parity: an inverted aux source is 1 - curve(aux), curve first")
{
    // Hosted (BA - Analog Classics 3, Macro 8 at 0 / .25 / .5 / .75, aux curve on and off): 1 - curve(x) matched within
    // 0-1.8 dB, curve(1 - x) missed by 10-21 dB. An unset inverted aux (0) therefore passes the whole route.
    const auto level = [](bool inverted, float macro, int auxiliary = W::Macro8) {
        auto synth = sine();
        synth->setParameter(W::oscParam(0, W::Level), 0);
        synth->setParameter(W::macroParam(7), macro);
        route(*synth, 0, W::Fixed, W::Osc1Level, 1, W::ImportedModulationDomain::SquaredLevel);
        synth->setParameter(W::modParam(0, W::AuxSource), static_cast<float>(auxiliary));
        synth->setAuxiliaryInverted(0, inverted);
        return rms(render(*synth, 9600), 4800);
    };
    const double open = level(true, 0), plain = level(false, 1);
    CHECK(open > 0.01); CHECK(level(false, 0) < 1e-6); CHECK(level(true, 1) < 1e-6);
    CHECK_NEAR(open / plain, 1, 0.01);
    // Hosted: with no aux source the flag changes nothing (the route is not closed by an inverted "1").
    CHECK_NEAR(level(true, 0, W::SourceOff) / plain, 1, 0.01);
    // The preset's aux flex curve: (0, 0) -> (1, 0.716), linear.
    W synth;
    dsp::CurveGraph curve; curve.count = 2;
    curve.x[0] = 0; curve.y[0] = 0; curve.x[1] = 1; curve.y[1] = 0.7161668f;
    REQUIRE(synth.setModulationCurve(5, true, curve, true, W::CurveLaw::ImportedExponential));
    const float shaped = synth.modulationCurveAt(5, true, 0.75f);
    synth.setAuxiliaryInverted(5, true); CHECK(synth.auxiliaryInverted(5));
    CHECK_NEAR(synth.modulationCurveAt(5, true, 0), 1, 1e-6);
    CHECK_NEAR(synth.modulationCurveAt(5, true, 0.75f), 1 - shaped, 1e-6);
    CHECK_NEAR(synth.modulationCurveAt(5, true, 1), 1 - 0.7161668, 1e-6);
    CHECK_NEAR(synth.modulationCurveAt(5, false, 0.25f), 0.25, 1e-6); // the main source is not inverted
    // Editing the curve keeps the inversion (it is a separate slot flag).
    REQUIRE(synth.setModulationCurve(5, true, dsp::CurveGraph::preset(3), false, W::CurveLaw::ImportedExponential));
    CHECK_NEAR(synth.modulationCurveAt(5, true, 0.3f), 0.7, 1e-6);
}

TEST_CASE("serum parity: Ladder EMS is linear (drive is a plain gain, nothing saturates)")
{
    // Hosted Serum 2 (BA - Analog Classics 4 bisect, C2 saw, resonance 47 %): the output follows the input level 1:1 up to
    // 24 dB of drive and output peaks of 2.6 at cutoff 0.25 and 1.0; drive 50 % / 100 % add +14 / +24 dB (the ladder table).
    const auto sawRms = [](float amplitude, float resonance) {
        dsp::StaggeredLadder filter; filter.setup(48000, 120, resonance);
        double sum = 0;
        for (int i = 0; i < 48000; ++i)
        {
            const float phase = static_cast<float>(std::fmod(65.41 * i / 48000.0, 1.0));
            const float y = filter.process(amplitude * (2 * phase - 1));
            if (i >= 24000) sum += static_cast<double>(y) * y;
        }
        return std::sqrt(sum / 24000);
    };
    for (float resonance : {0.0f, 0.47f, 1.0f})
    {
        const double quiet = sawRms(0.05f, resonance), hot = sawRms(40, resonance);
        CHECK(std::isfinite(hot) && quiet > 0);
        CHECK_NEAR(hot / quiet, 800, 0.8);
    }
    auto make = [](float driveDb) {
        auto synth = std::make_unique<W>();
        synth->prepare(48000, 256);
        synth->setParameter(W::SubOn, 0);
        synth->setParameter(W::NoiseOn, 0);
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), static_cast<float>(W::SerumLadderEms));
        synth->setParameter(W::filterParam(0, W::Cutoff), 150);
        synth->setParameter(W::filterParam(0, W::Resonance), 0.47f);
        synth->setParameter(W::filterParam(0, W::Drive), driveDb);
        synth->queueUiMidi(0x90, 36, 100);
        return synth;
    };
    auto clean = make(0), driven = make(18);
    const double ratio = rms(render(*driven, 24000), 12000) / rms(render(*clean, 24000), 12000);
    CHECK_NEAR(db(ratio), 18, 0.05);
}

TEST_CASE("serum parity: imported filter keytrack centres on note 32; transpose routes step like a 49-value control")
{
    // Hosted Serum 2: keytrack on moves the cutoff 1 octave per octave and leaves it unchanged at MIDI note 32 (fitted 31.99 at
    // notes 36/48/60/72).
    const auto level = [](int note, float cutoff, bool keytrack) {
        auto synth = std::make_unique<W>();
        synth->prepare(48000, 256);
        synth->setImportedVoiceProfile(true);
        synth->setParameter(W::SubOn, 0);
        synth->setParameter(W::NoiseOn, 0);
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), static_cast<float>(W::SerumLow12));
        synth->setParameter(W::filterParam(0, W::Cutoff), cutoff);
        synth->setParameter(W::filterParam(0, W::Resonance), 0);
        synth->setParameter(W::filterParam(0, W::FilterKeytrack), keytrack ? 1.0f : 0.0f);
        synth->queueUiMidi(0x90, static_cast<uint8_t>(note), 100);
        return rms(render(*synth, 24000), 12000);
    };
    CHECK_NEAR(db(level(W::kImportedKeytrackCenter, 200, true)), db(level(W::kImportedKeytrackCenter, 200, false)), 0.01);
    CHECK_NEAR(db(level(56, 200, true)), db(level(56, 800, false)), 0.05);
    // Fixed -> Transpose (stored 0): amounts -0.2377 / -0.10 / +0.10 / +0.02 play -12 / -5 / +5 / +1 semitones.
    const auto ratio = [](float amount) {
        auto synth = sine(57);
        route(*synth, 0, W::Fixed, W::GlobalTranspose, amount, W::ImportedModulationDomain::Unit);
        const int routed = crossings(render(*synth, 19200), 4800);
        auto plain = sine(57);
        return static_cast<double>(routed) / crossings(render(*plain, 19200), 4800);
    };
    CHECK_NEAR(ratio(-0.2377f), 0.5, 0.01);
    CHECK_NEAR(ratio(-0.10f), std::exp2(-5 / 12.0), 0.01);
    CHECK_NEAR(ratio(0.10f), std::exp2(5 / 12.0), 0.01);
    CHECK_NEAR(ratio(0.02f), std::exp2(1 / 12.0), 0.01);
    CHECK(std::string(W::destinationName(W::GlobalTranspose)) == "Transpose");
}

TEST_CASE("serum parity: detune routes move in the square-root domain, Unison Range scales cents; stored voice amp; pass-through drive")
{
    // Hosted Serum 2 (unison 2, peak split at the 8th harmonic = 2 x per-voice cents): stored 0.1 + route 0.1 -> 44.71 ct,
    // + 0.2 -> 76.59, 0.2 - 0.1 -> 28.42, 0.02 + 0.0157 -> 3.91; range 1 / 4 / 48 halve / double / x24 the range-2 cents.
    const auto stored = [](double d) { return static_cast<float>(200 * std::pow(d, 1.25)); };
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.1), 0.1f, 1), 44.71, 0.05);
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.1), 0.2f, 1), 76.59, 0.05);
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.2), -0.1f, 1), 28.42, 0.05);
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.02), 0.0157f, 1), 3.91, 0.05);
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.1), 0, 0.5f), 11.26, 0.05);
    CHECK_NEAR(2 * W::importedDetuneCents(stored(0.01), 0, 24), 30.36, 0.05);
    CHECK_NEAR(W::importedDetuneCents(stored(0.1), 0, 0), 0, 1e-6);
    // Stored Global Voice Amp: gain 2 u (0.25 plays 6 dB below the 0.5 default).
    auto quiet = sine(); quiet->setImportedVoiceAmp(0.25f);
    auto unity = sine();
    CHECK_NEAR(db(rms(render(*quiet, 9600), 4800) / rms(render(*unity, 9600), 4800)), -6.02, 0.05);
    // Pass-through types (Diffuser measured): Drive is the ladder gain into the voice soft clipper; no drive = level only.
    const auto through = [](float driveDb, float level) {
        auto synth = sine();
        synth->setParameter(W::filterParam(0, W::FilterOn), 1);
        synth->setParameter(W::filterParam(0, W::FilterMode), static_cast<float>(W::FilterThrough));
        synth->setParameter(W::filterParam(0, W::Drive), driveDb);
        synth->setParameter(W::oscParam(0, W::Level), level);
        return rms(render(*synth, 9600), 4800);
    };
    CHECK_NEAR(db(through(18, 0.01f) / through(0, 0.01f)), 18, 0.1);
    CHECK(db(through(24, 0.75f) / through(0, 0.75f)) < 23); // a hot input saturates
}
