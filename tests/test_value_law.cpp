// UI-93: what a dial's value text means (ui/ValueLaw): units, steps, typed text. Pure C++, so it is in the headless test
// list too: the Linux / Windows x64 CI must land on the same values as arm64 (v0.1.50 found EQ Eight's Freq at 1725 Hz on
// x64 and 1730 Hz on arm64 before readings became exact and the step search tolerant).
#include "TestFramework.h"
#include "ui/ValueLaw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace sonora;
namespace
{
std::string printed(const char* format, double v)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, format, v);
    return buffer;
}
// Three significant digits, as the device faces print ms / Hz ("12.3 ms", "123 ms", "1.23 s").
std::string significant(double v)
{
    const double a = std::abs(v);
    return printed(a >= 99.95 ? "%.0f" : a >= 9.995 ? "%.1f" : "%.2f", v);
}
std::string milliseconds(float v) { return v >= 1000 ? significant(v / 1000) + " s" : significant(v) + " ms"; }
std::string hertz(float v) { return v >= 1000 ? significant(v / 1000) + " kHz" : significant(v) + " Hz"; }

ui::ValueLaw law(std::function<std::string(float)> format, float lo, float hi, bool log = false)
{
    ui::DialRange range;
    range.format = std::move(format);
    range.minValue = lo;
    range.maxValue = hi;
    range.logarithmic = log;
    return ui::ValueLaw::detect(range);
}
} // namespace

TEST_CASE("value law: readings name their unit; seconds, kHz, pan sides, commas and -inf")
{
    using U = ui::ValueUnit;
    auto r = ui::readValue("1.20 kHz");
    REQUIRE(r); CHECK(r->unit == U::Hertz); CHECK_NEAR(r->value, 1200, 1e-9); CHECK_NEAR(r->resolution, 10, 1e-9);
    r = ui::readValue("1.5 s"); REQUIRE(r); CHECK(r->unit == U::Milliseconds); CHECK_NEAR(r->value, 1500, 1e-9);
    r = ui::readValue("250ms"); REQUIRE(r); CHECK(r->unit == U::Milliseconds); CHECK_NEAR(r->value, 250, 1e-9);
    r = ui::readValue("-6.0 dB"); REQUIRE(r); CHECK(r->unit == U::Decibels); CHECK_NEAR(r->value, -6, 1e-9);
    r = ui::readValue("-inf dB"); REQUIRE(r); CHECK(r->unit == U::Decibels); CHECK(std::isinf(r->value) && r->value < 0);
    r = ui::readValue("50%"); REQUIRE(r); CHECK(r->unit == U::Percent); CHECK_NEAR(r->value, 50, 1e-9);
    r = ui::readValue("+7 st"); REQUIRE(r); CHECK(r->unit == U::Semitones); CHECK_NEAR(r->value, 7, 1e-9);
    r = ui::readValue("-12 ct"); REQUIRE(r); CHECK(r->unit == U::Cents); CHECK_NEAR(r->value, -12, 1e-9);
    r = ui::readValue("128.00 BPM"); REQUIRE(r); CHECK(r->unit == U::Bpm); CHECK_NEAR(r->value, 128, 1e-9);
    r = ui::readValue("20L"); REQUIRE(r); CHECK(r->unit == U::Pan); CHECK_NEAR(r->value, -20, 1e-9);
    r = ui::readValue("C"); REQUIRE(r); CHECK(r->unit == U::Pan); CHECK_NEAR(r->value, 0, 1e-9);
    r = ui::readValue("40S"); REQUIRE(r); CHECK(r->unit == U::Pan); CHECK_NEAR(r->value, 40, 1e-9);
    // A typed number without a unit is read in the unit the dial shows now; a Brazilian comma is a decimal point.
    r = ui::readValue("1,5", U::Milliseconds, 1000); REQUIRE(r); CHECK(! r->unitWritten); CHECK_NEAR(r->value, 1500, 1e-9);
    r = ui::readValue("2k", U::Hertz); REQUIRE(r); CHECK_NEAR(r->value, 2000, 1e-9);
    CHECK(! ui::readValue("1/16"));
    CHECK(! ui::readValue("Off"));
    CHECK(ui::valueUnitFromName("st") == U::Semitones);
    CHECK(ui::valueUnitFromName("ms") == U::Milliseconds);
    CHECK(ui::valueUnitFromName("lin") == U::None);
}

TEST_CASE("value law: semitones and cents move in whole steps (also with Shift) and typed values are whole")
{
    auto st = law([](float v) { return printed("%+.0f st", v); }, -48, 48);
    CHECK(st.numeric()); CHECK(st.linear()); CHECK(st.unit() == ui::ValueUnit::Semitones);
    CHECK_NEAR(st.quantize(12.4f, false), 12, 1e-6);
    CHECK_NEAR(st.quantize(-3.6f, true), -4, 1e-6);
    CHECK_NEAR(*st.fromText("7.6", 0), 8, 1e-6);
    CHECK_NEAR(*st.fromText("-60", 0), -48, 1e-6); // clamped
    CHECK_NEAR(st.nudge(5, 1, false), 6, 1e-6);
    // Detune stored as -1..1, shown as cents: the step is one cent of the shown value.
    auto ct = law([](float v) { return printed("%.0f ct", v * 100); }, -1, 1);
    CHECK(ct.unit() == ui::ValueUnit::Cents); CHECK(ct.linear());
    CHECK_NEAR(ct.quantize(0.1234f, false), 0.12, 1e-6);
    CHECK_NEAR(*ct.fromText("-37", 0), -0.37, 1e-6);
}

TEST_CASE("value law: ms steps by size, seconds read as ms, a bare number keeps the unit shown now")
{
    auto ms = law(milliseconds, 0.1f, 5000, true);
    CHECK(ms.unit() == ui::ValueUnit::Milliseconds); CHECK(ms.linear());
    CHECK_NEAR(ms.quantize(12.37f, false), 12, 1e-4);        // whole ms from 10 ms
    CHECK_NEAR(ms.quantize(12.37f, true), 12.4, 1e-4);       // Shift: a tenth
    CHECK_NEAR(ms.quantize(3.456f, false), 3.5, 1e-4);       // tenths below 10 ms
    CHECK_NEAR(ms.quantize(1234, false), 1230, 1e-3);        // 10 ms from a second
    CHECK_NEAR(*ms.fromText("1.5 s", 100), 1500, 1e-3);
    CHECK_NEAR(*ms.fromText("250", 100), 250, 1e-3);         // shows "100 ms": 250 ms
    CHECK_NEAR(*ms.fromText("3", 2000), 3000, 1e-3);         // shows "2.00 s": 3 s
    CHECK(! ms.fromText("5 dB", 100));                       // a unit the dial does not have
    CHECK_NEAR(ms.nudge(10, -1, false), 9.9, 1e-4);          // down across 10 ms takes the smaller step
}

TEST_CASE("value law: BPM in whole beats per minute, Hz with three digits, % in whole percent")
{
    auto bpm = law([](float v) { return printed("%.2f BPM", v); }, 20, 999);
    CHECK(bpm.unit() == ui::ValueUnit::Bpm);
    CHECK_NEAR(bpm.quantize(120.37f, false), 120, 1e-4);
    CHECK_NEAR(bpm.quantize(120.37f, true), 120.4, 1e-4);
    CHECK_NEAR(*bpm.fromText("128.25", 120), 128.25, 1e-4);
    auto hz = law(hertz, 20, 20000, true);
    CHECK(hz.unit() == ui::ValueUnit::Hertz);
    CHECK_NEAR(hz.quantize(1234.5f, false), 1230, 1e-2);
    CHECK_NEAR(hz.quantize(55.57f, false), 55.6, 1e-3);
    CHECK_NEAR(*hz.fromText("2k", 500), 2000, 1e-2);
    CHECK_NEAR(*hz.fromText("2", 1200), 2000, 1e-2);         // shows "1.20 kHz"
    CHECK_NEAR(*hz.fromText("500 Hz", 1200), 500, 1e-2);
    auto pct = law([](float v) { return printed("%.0f %%", v * 100); }, 0, 1);
    CHECK(pct.unit() == ui::ValueUnit::Percent);
    CHECK_NEAR(pct.quantize(0.437f, false), 0.44, 1e-6);
    CHECK_NEAR(*pct.fromText("25", 0.5f), 0.25, 1e-6);
}

TEST_CASE("value law: non-linear text is searched; words and choices match by name; a constant text is not typeable")
{
    // Utility's Gain: dB along the knob, linear amplitude in the last 5 %.
    auto gain = law([](float pos) {
        const float g = pos >= 0.025f ? std::pow(10.0f, (pos - 0.5f) * 70 / 20) : std::pow(10.0f, -33.25f / 20) * pos / 0.025f;
        return g <= 0.0001f ? std::string("-inf dB") : printed("%.2f dB", 20 * std::log10(g));
    }, 0, 1);
    CHECK(gain.unit() == ui::ValueUnit::Decibels);
    CHECK_NEAR(*gain.fromText("-6", 0.5f), (-6.0 + 35) / 70, 1e-4);
    CHECK_NEAR(*gain.fromText("-inf", 0.5f), 0, 1e-6);
    const float quiet = *gain.fromText("-40", 0.5f); // below -33.25 dB: the amplitude part, not -inf
    CHECK(quiet > 0.001f && quiet < 0.025f);
    CHECK_NEAR(20 * std::log10(std::pow(10.0f, -33.25f / 20) * quiet / 0.025f), -40, 0.02);
    auto pan = law([](float v) { return std::abs(v) < 0.01f ? std::string("C") : printed("%.0f", std::abs(v) * 50) + (v < 0 ? "L" : "R"); }, -1, 1);
    CHECK(pan.unit() == ui::ValueUnit::Pan);
    CHECK_NEAR(*pan.fromText("20L", 0), -0.4, 1e-6);
    CHECK_NEAR(*pan.fromText("c", 0.3f), 0, 1e-6);
    CHECK_NEAR(*pan.fromText("10", 0), 0.2, 1e-6);
    static const char* kWaves[] = {"Sine", "Saw", "Square"};
    auto wave = law([](float v) { return std::string(kWaves[std::clamp((int) std::lround(v), 0, 2)]); }, 0, 2);
    CHECK(! wave.numeric());
    CHECK_NEAR(*wave.fromText("square", 0), 2, 1e-6);
    CHECK_NEAR(*wave.fromText("sa", 0), 1, 1e-6); // prefix
    CHECK(! wave.fromText("tri", 0));
    auto constant = law([](float) { return std::string("12 st"); }, 0, 1);
    CHECK(! constant.numeric());
    CHECK(! constant.fromText("5", 0));
}


TEST_CASE("value law: readings are exact, and a log knob lands mid-step on every CPU (EQ Eight Freq)")
{
    // "1.73 kHz" is exactly 1730 (173 * 1000 / 100), not 1 + 0.7 + 0.03 scaled, whose last bit depends on FMA.
    CHECK_NEAR(ui::readValue("1.73 kHz")->value, 1730.0, 0); // exactly
    CHECK_NEAR(ui::readValue("0.3 s")->value, 300.0, 0);
    CHECK_NEAR(ui::readValue("-12.5 dB")->value, -12.5, 0);
    // ReferenceFace::knob with Law::logarithmic(10, 22000): the knob's value is its position, the text is the Hz.
    const auto fromNorm = [](float n) { return 10.0f * std::pow(2200.0f, n); };
    const auto toNorm = [](float v) { return std::log(v / 10.0f) / std::log(2200.0f); };
    auto freq = law([&](float n) {
        const float v = std::clamp(fromNorm(std::clamp(n, 0.0f, 1.0f)), 10.0f, 22000.0f);
        return v >= 1000 ? significant(v / 1000) + " kHz" : significant(v) + " Hz";
    }, 0, 1);
    CHECK(! freq.linear());
    CHECK(freq.unit() == ui::ValueUnit::Hertz);
    // The UI test's drag: 800 Hz + 20 px = 1727.16 Hz -> the middle of the "1.73 kHz" step (x64 found its lower edge).
    CHECK_NEAR(fromNorm(freq.quantize(toNorm(800) + 0.1f, false)), 1730, 0.05);
    CHECK_NEAR(fromNorm(freq.quantize(toNorm(1234), false)), 1230, 0.05);
    CHECK_NEAR(fromNorm(freq.quantize(toNorm(5555), false)), 5550, 0.05);
    CHECK_NEAR(fromNorm(*freq.fromText("1.5k", 0.5f)), 1500, 0.05);
    CHECK_NEAR(fromNorm(*freq.fromText("440", toNorm(300))), 440, 0.05);
}
