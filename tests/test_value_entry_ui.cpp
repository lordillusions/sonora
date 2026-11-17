// UI-93: dials take typed values in a field over the value (a click on the value, or a number typed on a selected dial)
// and move in their unit's steps. The unit laws themselves: test_value_law.cpp (also in the headless build).
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
