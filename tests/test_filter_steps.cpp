// DEV-24: native filters whose coefficients move while audio plays (automation, knobs, internal modulation) must not
// overshoot. A transposed direct form keeps coefficient-weighted signals in its state, so a large cutoff step on a loud
// signal gave an inverted transient past full scale; the state-variable form (SvfCoeffs / SvfState in Biquad.h) keeps
// its integrators continuous. Each case steps a cutoff a long way mid-signal on a loud saw and checks that the output
// peak stays at the level the filter reaches with either setting held (the saw itself peaks at 0.8).
#include "TestFramework.h"
#include "devices/native/AutoFilter.h"
#include "devices/native/Bands.h"
#include "devices/native/Delay.h"
#include "devices/native/EqEight.h"
#include "devices/native/Redux.h"
#include "devices/native/Utility.h"
#include "devices/native/WavetableEffects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace sonora;

namespace
{
constexpr double kRate = 44100.0;
constexpr int kBlock = 512, kBlocks = 12, kStepBlock = 5;
constexpr int kPeriod = 630; // ~70 Hz: the saw's falling edges land mid-block, away from the step

// Naive saw at +-0.8 (left); the right channel carries half of it so stereo paths see different channels.
float saw(int frame) { return 0.8f * (2.0f * (float) (frame % kPeriod) / kPeriod - 1.0f); }

double peakFrom(const std::vector<float>& data, int start)
{
    double peak = 0;
    for (size_t i = (size_t) start; i < data.size(); ++i)
        peak = std::max(peak, (double) std::abs(data[i]));
    return peak;
}

using BlockProcess = std::function<void(float*, float*, int)>;

// Renders kBlocks blocks of the saw through `process`; `step` (if set) runs before block kStepBlock.
std::vector<float> renderSaw(const BlockProcess& process, const std::function<void()>& step)
{
    std::vector<float> out;
    std::vector<float> l((size_t) kBlock), r((size_t) kBlock);
    for (int b = 0; b < kBlocks; ++b)
    {
        if (b == kStepBlock && step)
            step();
        for (int i = 0; i < kBlock; ++i)
        {
            l[(size_t) i] = saw(b * kBlock + i);
            r[(size_t) i] = 0.5f * l[(size_t) i];
        }
        process(l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i)
            out.push_back(std::max(std::abs(l[(size_t) i]), std::abs(r[(size_t) i])));
    }
    return out;
}

struct StepResult
{
    double stepped = 0, held = 0; // peak after the step / larger settled peak with either setting held throughout
};

// `run(startAtB, stepToB)` renders one fresh device. Settled peaks skip the first block (start-up transient).
template <typename Run>
StepResult measure(Run run)
{
    StepResult result;
    result.stepped = peakFrom(run(false, true), kStepBlock * kBlock);
    result.held = std::max(peakFrom(run(false, false), kBlock), peakFrom(run(true, false), kBlock));
    return result;
}

// `make` builds a fresh device at setting A; `toB` moves it to setting B.
template <typename Make, typename ToB>
StepResult measureNative(Make make, ToB toB)
{
    return measure([&](bool startAtB, bool stepToB) {
        auto device = make();
        device->prepare(kRate, kBlock);
        if (startAtB)
            toB(*device);
        juce::MidiBuffer midi;
        return renderSaw([&](float* l, float* r, int n) {
            float* channels[2] = {l, r};
            juce::AudioBuffer<float> buffer(channels, 2, n);
            device->process(buffer, midi);
        }, stepToB ? std::function<void()>([&] { toB(*device); }) : std::function<void()>());
    });
}

// The stepped render may not exceed what either setting reaches when held, plus a little for the transition itself;
// the transposed direct form overshot that by far on these steps. (Held peaks can exceed the saw's 0.8: the naive
// saw's edges ring through steep filters and high-passes pass the whole edge, so the bound is relative.)
void checkNoOvershoot(const char* what, const StepResult& r, double allowance = 1.08)
{
    std::printf("  %s: stepped peak %.3f, held peak %.3f\n", what, r.stepped, r.held);
    CHECK(r.held > 0.05); // the filter passes the saw at all
    CHECK(r.stepped < r.held * allowance + 0.02);
}
} // namespace

TEST_CASE("eq eight: a band frequency step on a loud saw does not overshoot (DEV-24)")
{
    using E = devices::EqEight;
    checkNoOvershoot("EQ Eight", measureNative([] {
        auto e = std::make_unique<E>();
        e->setParameter(E::bandParam(0, E::On), 1.0f);
        e->setParameter(E::bandParam(0, E::Type), (float) E::FilterType::HighCut48);
        e->setParameter(E::bandParam(0, E::Freq), 8000.0f);
        e->setParameter(E::bandParam(0, E::Q), 0.71f);
        return e;
    }, [](E& e) { e.setParameter(E::bandParam(0, E::Freq), 300.0f); }));
}

TEST_CASE("auto filter 2: a frequency step on a loud saw does not overshoot (DEV-24)")
{
    using A = devices::AutoFilter2;
    checkNoOvershoot("Auto Filter 2", measureNative([] {
        auto a = std::make_unique<A>();
        a->setParameter(A::Frequency, 8000.0f);
        a->setParameter(A::Slope, 1.0f);
        return a;
    }, [](A& a) { a.setParameter(A::Frequency, 300.0f); }));
}

TEST_CASE("eq three: a crossover step on a loud saw does not overshoot (DEV-24)")
{
    using E = devices::EqThree;
    checkNoOvershoot("EQ Three", measureNative([] {
        auto e = std::make_unique<E>();
        e->setParameter(E::GainMid, 0.0f); // mid and high killed: the output is the low crossover's low-pass side
        e->setParameter(E::GainHigh, 0.0f);
        e->setParameter(E::FreqLow, 500.0f);
        e->setParameter(E::Slope, 1.0f);
        return e;
    }, [](E& e) { e.setParameter(E::FreqLow, 50.0f); }));
}

TEST_CASE("redux: a rate step with pre and post filters on a loud saw does not overshoot (DEV-24)")
{
    using R = devices::Redux;
    checkNoOvershoot("Redux", measureNative([] {
        auto x = std::make_unique<R>();
        x->setParameter(R::Bits, 16.0f);
        x->setParameter(R::Rate, 40000.0f);
        x->setParameter(R::PreFilter, 1.0f);
        x->setParameter(R::PostFilter, 1.0f);
        x->setParameter(R::DryWet, 1.0f);
        return x;
    }, [](R& x) { x.setParameter(R::Rate, 1500.0f); }));
}

TEST_CASE("delay: a filter frequency step on a loud saw does not overshoot (DEV-24)")
{
    using D = devices::Delay;
    checkNoOvershoot("Delay", measureNative([] {
        auto d = std::make_unique<D>();
        d->setParameter(D::SyncL, 0.0f);
        d->setParameter(D::SyncR, 0.0f);
        d->setParameter(D::TimeL, 0.005f);
        d->setParameter(D::TimeR, 0.005f);
        d->setParameter(D::Feedback, 0.0f);
        d->setParameter(D::DryWetMode, 0.0f);
        d->setParameter(D::DryWet, 1.0f);
        d->setParameter(D::FilterOn, 1.0f);
        d->setParameter(D::FilterBandwidth, 6.0f); // corners at f / 8 and f * 8
        d->setParameter(D::FilterFrequency, 4000.0f);
        return d;
    }, [](D& d) { d.setParameter(D::FilterFrequency, 100.0f); }));
}

TEST_CASE("utility: a bass mono frequency step on a loud saw does not overshoot (DEV-24)")
{
    using U = devices::Utility;
    checkNoOvershoot("Utility", measureNative([] {
        auto u = std::make_unique<U>();
        u->setParameter(U::BassMono, 1.0f);
        u->setParameter(U::BassMonoAudition, 1.0f); // output only the summed low band: the crossover alone
        u->setParameter(U::BassMonoFrequency, 500.0f);
        return u;
    }, [](U& u) { u.setParameter(U::BassMonoFrequency, 50.0f); }));
}

namespace
{
using WE = devices::WavetableEffects;

WE::Module wavetableModule(WE::ModuleId id, WE::Type type)
{
    WE::Module result;
    result.id = id;
    result.type = type;
    for (const auto& parameter : WE::parameterDescriptors(type))
        result.values.push_back(parameter.initial);
    return result;
}

void setWavetable(WE& effects, WE::Type type, std::string_view id, float value)
{
    const int index = WE::parameterIndex(type, id);
    REQUIRE(index >= 0);
    REQUIRE(effects.setParameter(1, index, value));
}

// One Serum 2 FX module on the main bus; `set(effects, toB)` applies setting A or B.
template <typename Set>
StepResult measureWavetable(WE::Type type, Set set)
{
    return measure([&](bool startAtB, bool stepToB) {
        WE effects;
        WE::Configuration configuration;
        configuration.buses[0].push_back(wavetableModule(1, type));
        std::string error;
        REQUIRE(effects.prepare(configuration, kRate, error));
        set(effects, startAtB);
        std::array<std::vector<float>, 2 * WE::busCount> silent;
        return renderSaw([&](float* l, float* r, int n) {
            std::array<WE::StereoBuffer, WE::busCount> buses{};
            buses[0] = {l, r};
            for (size_t bus = 1; bus < (size_t) WE::busCount; ++bus)
            {
                silent[bus * 2].assign((size_t) n, 0.0f);
                silent[bus * 2 + 1].assign((size_t) n, 0.0f);
                buses[bus] = {silent[bus * 2].data(), silent[bus * 2 + 1].data()};
            }
            effects.process(buses, n);
        }, stepToB ? std::function<void()>([&] { set(effects, true); }) : std::function<void()>());
    });
}
} // namespace

TEST_CASE("wavetable fx: EQ and Utility filter steps on a loud saw do not overshoot (DEV-24)")
{
    checkNoOvershoot("Wavetable FX EQ", measureWavetable(WE::Type::Eq, [](WE& e, bool toB) {
        setWavetable(e, WE::Type::Eq, "Band1Type", 4.0f); // low-pass
        setWavetable(e, WE::Type::Eq, "Band1Q", 0.71f);
        setWavetable(e, WE::Type::Eq, "Band1Freq", toB ? 300.0f : 12000.0f);
    }));
    checkNoOvershoot("Wavetable FX Utility low-pass", measureWavetable(WE::Type::Utility, [](WE& e, bool toB) {
        setWavetable(e, WE::Type::Utility, "LowPass", toB ? 300.0f : 16000.0f);
    }));
    // A falling high-pass corner keeps the integrators' snapshot of the signal, which then decays at the new corner: a
    // slow swell above both held peaks even for a continuous filter (measured 1.85 vs 1.51 held; the direct form: 4.4).
    checkNoOvershoot("Wavetable FX Utility high-pass", measureWavetable(WE::Type::Utility, [](WE& e, bool toB) {
        setWavetable(e, WE::Type::Utility, "HighPass", toB ? 20.0f : 400.0f);
    }), 1.3);
}
