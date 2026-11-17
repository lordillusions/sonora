// UI-93: dials know their unit (st, ct, ms, Hz, dB, %, BPM, pan) from the text they print, move in steps that make sense
// for it, and take typed values in a field over the value (a click on the value, or a number typed on a selected dial).
#include "ProFaceTestKit.h"
#include "devices/native/Stutter.h"
#include "devices/native/Utility.h"
#include "ui/views/DeviceFace.h"
#include "plugin/StutterPlugin.h"
#include "plugin/StutterPluginEditor.h"
#include "ui/RootWidget.h"
#include "ui/ValueEntry.h"
#include "ui/views/StutterFace.h"
#include "ui/widgets/Knob.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>

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

ui::KeyEvent typed(uint32_t ch)
{
    ui::KeyEvent k;
    k.keyCode = (int) ch;
    k.textChar = ch;
    return k;
}
ui::KeyEvent keyCode(int code)
{
    ui::KeyEvent k;
    k.keyCode = code;
    return k;
}
void type(ui::RootWidget& root, const char* text)
{
    for (; *text; ++text) root.dispatchKey(typed((uint32_t) *text));
}
ui::MouseEvent at(float x, float y)
{
    ui::MouseEvent e;
    e.x = x;
    e.y = y;
    return e;
}
} // namespace

TEST_CASE("value entry: readings name their unit; seconds, kHz, pan sides, commas and -inf")
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

TEST_CASE("value entry: semitones and cents move in whole steps (also with Shift) and typed values are whole")
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

TEST_CASE("value entry: ms steps by size, seconds read as ms, a bare number keeps the unit shown now")
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

TEST_CASE("value entry: BPM in whole beats per minute, Hz with three digits, % in whole percent")
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

TEST_CASE("value entry: non-linear text is searched; words and choices match by name; a constant text is not typeable")
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

TEST_CASE("value entry: a click on a knob's value opens the field; typing, Return, Escape, a click elsewhere")
{
    juce::ScopedJuceInitialiser_GUI init;
    ui::UiContext context;
    ui::RootWidget root(context);
    float value = 0;
    ui::Knob::Source source;
    source.get = [&] { return value; };
    source.set = [&](float v) { value = v; };
    source.minValue = -24; source.maxValue = 24;
    source.format = [](float v) { return printed("%+.0f st", v); };
    auto* knob = root.add<ui::Knob>("Pitch", source);
    knob->style = ui::Knob::Style::Device;
    root.setBounds({0, 0, 300, 200});
    knob->setBounds({20, 20, 60, 63.5f});
    const auto field = knob->valueBounds().translated(20, 20);

    // Click on the value (no drag): the field opens with the shown text selected; typing replaces it.
    root.dispatchMouseDown(at(field.centreX(), field.centreY()));
    root.dispatchMouseUp(at(field.centreX(), field.centreY()));
    REQUIRE(context.valueEntry.active());
    CHECK(context.valueEntry.edit.text == "+0 st");
    type(root, "7.4");
    root.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK(! context.valueEntry.active());
    CHECK_NEAR(value, 7, 1e-6);

    // Escape leaves the value; unchanged text committed by a click elsewhere leaves it too.
    root.dispatchMouseDown(at(field.centreX(), field.centreY()));
    root.dispatchMouseUp(at(field.centreX(), field.centreY()));
    type(root, "3");
    root.dispatchKey(keyCode(juce::KeyPress::escapeKey));
    CHECK(! context.valueEntry.active());
    CHECK_NEAR(value, 7, 1e-6);
    root.dispatchMouseDown(at(field.centreX(), field.centreY()));
    root.dispatchMouseUp(at(field.centreX(), field.centreY()));
    root.dispatchMouseDown(at(250, 180));
    root.dispatchMouseUp(at(250, 180));
    CHECK(! context.valueEntry.active());
    CHECK_NEAR(value, 7, 1e-6);

    // A number typed on the selected knob opens the field with that digit; a click elsewhere commits it.
    root.dispatchMouseDown(at(50, 45));
    root.dispatchMouseUp(at(50, 45));
    CHECK(! context.valueEntry.active()); // a click on the ring itself only selects
    CHECK(root.dispatchUserKey(typed('-')));
    REQUIRE(context.valueEntry.active());
    type(root, "5");
    CHECK(context.valueEntry.edit.text == "-5");
    root.dispatchMouseDown(at(250, 180));
    CHECK(! context.valueEntry.active());
    CHECK_NEAR(value, -5, 1e-6);

    // Dragging moves in whole semitones; the arrow keys step one.
    root.dispatchMouseDown(at(50, 45));
    root.dispatchMouseDrag(at(50, 45 - 13));
    CHECK_NEAR(value, std::round(value), 1e-6);
    CHECK(value > -5);
    root.dispatchMouseUp(at(50, 45 - 13));
    const float dragged = value;
    root.dispatchKey(keyCode(juce::KeyPress::upKey));
    CHECK_NEAR(value, dragged + 1, 1e-6);

    // The field follows the knob: deleting it closes the field without committing.
    root.dispatchMouseDown(at(field.centreX(), field.centreY()));
    root.dispatchMouseUp(at(field.centreX(), field.centreY()));
    REQUIRE(context.valueEntry.active());
    root.removeAllChildren();
    CHECK(! context.valueEntry.active());
}

TEST_CASE("value entry: Sonora Stutter's VST3 face types Pitch and Grain and drags Pitch in whole semitones")
{
    juce::ScopedJuceInitialiser_GUI init;
    using S = devices::Stutter;
    plugin::StutterPlugin p;
    p.prepareToPlay(48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::StutterPluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    view.setBounds({0, 0, ui::StutterFace::kFull.w, ui::StutterFace::kFull.h});
    view.layout();
    auto& face = window->face();
    auto& context = view.context();

    auto clickValue = [&](int param) {
        const auto r = face.knobValueRect(param);
        REQUIRE(r.has_value());
        view.dispatchMouseDown(testkit::designAt(face, r->centreX(), r->centreY()));
        view.dispatchMouseUp(testkit::designAt(face, r->centreX(), r->centreY()));
    };
    clickValue(S::Pitch);
    REQUIRE(context.valueEntry.active());
    type(view, "-7");
    view.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK_NEAR(p.device().getParameter(S::Pitch), -7, 1e-6);

    clickValue(S::Grain);
    REQUIRE(context.valueEntry.active());
    type(view, "0.25 s");
    view.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK_NEAR(p.device().getParameter(S::Grain), 250, 1e-3);
    CHECK(face.knobLaw(S::Grain).unit() == ui::ValueUnit::Milliseconds);

    // A digit typed after clicking a knob opens the field for it.
    CHECK(view.dispatchUserKey(typed('3')));
    REQUIRE(context.valueEntry.active());
    view.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK_NEAR(p.device().getParameter(S::Grain), 10, 1e-4); // "3" ms, clamped to Grain's 10 ms minimum

    REQUIRE(testkit::drag(view, face, "PITCH", -37));
    const float pitch = p.device().getParameter(S::Pitch);
    CHECK(pitch > -7);
    CHECK_NEAR(pitch, std::round(pitch), 1e-6);
}

TEST_CASE("value entry: Utility's Gain and Balance take typed dB and pan; the open field renders over the value")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto model = devices::nativeDeviceModel("StereoGain");
    devices::BuildContext build;
    auto processor = devices::createDevice(model, build);
    ui::UiContext context;
    ui::RootWidget root(context);
    auto* face = root.add<ui::DeviceFace>(model, processor.get(), 0, 0, -1, -1, std::function<void(ui::Widget*, int)>{});
    root.setBounds({0, 0, 260, 200});
    face->setBounds({10, 10, face->preferredWidth(), 164});
    face->layout();
    std::vector<ui::Knob*> knobs;
    testkit::collect(root, knobs);
    auto byLabel = [&](const std::string& label) -> ui::Knob* {
        for (auto* k : knobs) if (k->label() == label) return k;
        return nullptr;
    };
    auto* gain = byLabel("Gain");
    auto* balance = byLabel("Balance");
    REQUIRE(gain != nullptr);
    REQUIRE(balance != nullptr);
    auto openOn = [&](ui::Knob* k) {
        const auto r = k->valueBounds().translated(k->boundsInRoot().x, k->boundsInRoot().y);
        root.dispatchMouseDown(at(r.centreX(), r.centreY()));
        root.dispatchMouseUp(at(r.centreX(), r.centreY()));
    };
    using U = devices::Utility;
    openOn(gain);
    REQUIRE(context.valueEntry.active());
    type(root, "-6");
    const auto png = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("value-entry-utility.png");
    CHECK(gfx::renderToPng(root, 260, 200, 2, png.getFullPathName().toStdString()));
    root.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK_NEAR(20 * std::log10(processor->getParameter(U::Gain)), -6, 0.01);
    openOn(balance);
    type(root, "20L");
    root.dispatchKey(keyCode(juce::KeyPress::returnKey));
    CHECK_NEAR(processor->getParameter(U::Balance), -0.4, 1e-4);
}
