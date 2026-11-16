// DEV-28a: Operator's reference face (OperatorFace.cpp): reachability, strip / panel pages, gestures through real input
// dispatch, the value texts of the capture, PNGs of every page.
#include "TestFramework.h"

#include "devices/native/Operator.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/RootWidget.h"
#include "ui/views/ReferenceFace.h"
#include "ui/widgets/Knob.h"

#include <map>
#include <set>

using namespace sonora;
using O = devices::Operator;

namespace
{
ui::MouseEvent point(float x, float y)
{
    ui::MouseEvent e;
    e.x = x;
    e.y = y;
    return e;
}
ui::MouseEvent centre(ui::Widget* w) { const auto r = w->boundsInRoot(); return point(r.x + r.w * 0.5f, r.y + r.h * 0.5f); }
void click(ui::RootWidget& root, ui::MouseEvent e) { root.dispatchMouseDown(e); root.dispatchMouseUp(e); }
void drag(ui::RootWidget& root, ui::MouseEvent from, float dx, float dy)
{
    root.dispatchMouseDown(from);
    auto to = from;
    to.x += dx;
    to.y += dy;
    root.dispatchMouseDrag(to);
    root.dispatchMouseUp(to);
}
bool shown(ui::Widget* w)
{
    for (; w != nullptr; w = w->parent()) if (! w->visible) return false;
    return true;
}
} // namespace

TEST_CASE("Operator face: measured width, every parameter reachable, strip and panel pages, gestures, value texts, PNGs")
{
    O op;
    model::Device model;
    model.type = "Operator";
    std::function<void(int)> pick;
    ui::UiContext context;
    context.showChoices = [&](const auto&, const auto&, auto chosen) { pick = chosen; };
    ui::RootWidget root(context);
    std::map<int, std::vector<ui::Widget*>> widgets; // every widget tagged with each parameter
    auto* face = ui::addReferenceFace(root, model, &op, [&](ui::Widget* w, int parameter) { widgets[parameter].push_back(w); });
    REQUIRE(face != nullptr);
    CHECK(ui::operatorFacePage(*face) == 0); // oscillator A, as in the capture
    CHECK_NEAR(face->preferredWidth(342), 1450, 0.01); // the capture's device box: x 12..1462 for a 342-px body

    // Laid out at the capture's scale under a 34-px title band, so the display's top (capture y 16) fits above the body:
    // root x = capture x - 12, root y = capture y - 10.
    const float body = 342, width = face->preferredWidth(body);
    root.setBounds({0, 0, width, 34 + body});
    face->setBounds({0, 34, width, body});
    face->layout();
    const auto at = [](float x, float y) { return point(x - 12, y - 10); };
    const auto first = [&](int parameter) -> ui::Widget* {
        for (auto* w : widgets[parameter]) if (shown(w)) return w;
        return widgets[parameter].empty() ? nullptr : widgets[parameter].front();
    };

    // Every parameter has a control, except the ones the display's graphs edit (stage slopes, the oscillator and LFO
    // envelopes' end level, the User wave's partials), the legacy filter leaves and the modulation routing whose
    // destinations are not identified (DEV-28a2).
    std::set<int> exempt{O::FilterLegacyType, O::FilterLegacyQ, O::LfoDestB, O::LfoDestBAmount, O::PitchDestB, O::PitchDestBAmount};
    for (int i = O::MidiModFirst; i < O::NumParams; ++i) exempt.insert(i);
    for (int e = 0; e < O::kOperators + O::kOtherEnvs; ++e)
    {
        const int base = e < O::kOperators ? O::envParam(e, 0) : O::otherEnvParam(e - O::kOperators, 0);
        for (int field : {O::AttackSlope, O::DecaySlope, O::ReleaseSlope}) exempt.insert(base + field);
        if (e < O::kOperators || e - O::kOperators == O::EnvLfo) exempt.insert(base + O::ReleaseLevel);
    }
    for (int i = 0; i < O::NumParams; ++i)
        if (! widgets.contains(i) && ! exempt.contains(i)) { std::printf("    untagged %s\n", op.parameters()[(size_t) i].id.c_str()); CHECK(false); }

    // The capture's value texts.
    CHECK(ui::operatorValueText(op, O::opParam(0, O::OpCoarse), op.getParameter(O::opParam(0, O::OpCoarse))) == "1");
    CHECK(ui::operatorValueText(op, O::opParam(0, O::OpVolume), 1) == "0.0 dB");
    CHECK(ui::operatorValueText(op, O::opParam(1, O::OpVolume), 0.0003162f) == "-\xE2\x88\x9E dB");
    CHECK(ui::operatorValueText(op, O::envParam(0, O::AttackTime), 0.1f) == "0.00 ms");
    CHECK(ui::operatorValueText(op, O::envParam(0, O::DecayTime), 1000) == "1.00 s");
    CHECK(ui::operatorValueText(op, O::envParam(0, O::ReleaseTime), 400) == "400 ms");
    CHECK(ui::operatorValueText(op, O::opParam(0, O::OpVelScale), 50) == "50 %");
    CHECK(ui::operatorValueText(op, O::opParam(0, O::OpWave), 0) == "Sin");
    CHECK(ui::operatorValueText(op, O::LfoRate, 100) == "100.00");
    CHECK(ui::operatorValueText(op, O::LfoAmount, 0.1f) == "10 %");
    CHECK(ui::operatorValueText(op, O::FilterFreq, 12000) == "12.0 kHz");
    CHECK(ui::operatorValueText(op, O::FilterRes, 0.277777791f) == "28 %");
    CHECK(ui::operatorValueText(op, O::Transpose, 0) == "0 st");
    CHECK(ui::operatorValueText(op, O::Tone, 0.7f) == "70 %");
    CHECK(ui::operatorValueText(op, O::Volume, 0.1258925349f) == "-18 dB");

    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    folder.createDirectory();
    const auto png = [&](const char* name) {
        root.layout();
        face->layout();
        return gfx::renderToPng(root, (int) root.width(), (int) root.height(), 1, folder.getChildFile(name).getFullPathName().toStdString());
    };
    REQUIRE(png("Operator-OscA.png"));

    // Oscillator A's page: its envelope controls show, B's do not.
    REQUIRE(first(O::envParam(0, O::AttackTime)) != nullptr);
    CHECK(shown(first(O::envParam(0, O::AttackTime))));
    CHECK(! shown(widgets[O::envParam(1, O::AttackTime)].front()));

    // A click on strip B's background selects it: the display shows B's page.
    click(root, at(428, 260));
    CHECK(ui::operatorFacePage(*face) == 1);
    CHECK(shown(widgets[O::envParam(1, O::AttackTime)].front()));
    CHECK(! shown(widgets[O::envParam(0, O::AttackTime)].front()));
    REQUIRE(png("Operator-OscB.png"));

    // Touching a control of strip D selects it too.
    drag(root, centre(first(O::opParam(3, O::OpFine))), 0, -10);
    CHECK(ui::operatorFacePage(*face) == 3);
    CHECK(op.getParameter(O::opParam(3, O::OpFine)) > 0);

    // Panels: LFO, Filter, Pitch, Global pages.
    const char* names[] = {"Operator-Lfo.png", "Operator-Filter.png", "Operator-Pitch.png", "Operator-Global.png"};
    const int pageControls[] = {O::LfoRateKey, O::FilterFreqVel, O::PitchAmountA, O::GlideOn};
    for (int i = 0; i < 4; ++i)
    {
        click(root, at(1046, 84 + 86.0f * (float) i));
        CHECK(ui::operatorFacePage(*face) == 4 + i);
        CHECK(shown(widgets[pageControls[i]].front()));
        CHECK(! shown(widgets[O::envParam(3, O::AttackTime)].front()));
        REQUIRE(png(names[i]));
    }

    // Global page: a click on the fifth algorithm diagram chooses it.
    CHECK_NEAR(op.getParameter(O::Algorithm), 0, 0);
    click(root, at(454 + 51.8f * 4 + 24, 80));
    CHECK_NEAR(op.getParameter(O::Algorithm), 4, 0);

    // The letter square switches its oscillator.
    auto* letterD = widgets[O::opParam(3, O::OpOn)].front();
    CHECK(op.getParameter(O::opParam(3, O::OpOn)) > 0.5f);
    click(root, centre(letterD));
    CHECK(op.getParameter(O::opParam(3, O::OpOn)) < 0.5f);
    click(root, centre(letterD));
    CHECK(op.getParameter(O::opParam(3, O::OpOn)) > 0.5f);

    // Knob drag and wheel (strip A's Coarse and Level).
    auto* coarseA = first(O::opParam(0, O::OpCoarse));
    const float coarse = op.getParameter(O::opParam(0, O::OpCoarse));
    drag(root, centre(coarseA), 0, -40);
    CHECK(op.getParameter(O::opParam(0, O::OpCoarse)) > coarse + 2);
    auto wheel = centre(first(O::opParam(0, O::OpVolume)));
    wheel.wheelDeltaY = -1;
    root.dispatchMouseWheel(wheel);
    CHECK(op.getParameter(O::opParam(0, O::OpVolume)) < 0.99f);

    // Fixed swaps Coarse / Fine for Freq / Multi.
    CHECK(shown(coarseA));
    CHECK(! shown(widgets[O::opParam(0, O::OpFixedFreq)].front()));
    click(root, centre(first(O::opParam(0, O::OpFixed))));
    CHECK(op.getParameter(O::opParam(0, O::OpFixed)) > 0.5f);
    face->layout();
    CHECK(! shown(coarseA));
    CHECK(shown(widgets[O::opParam(0, O::OpFixedFreq)].front()));
    CHECK(shown(widgets[O::opParam(0, O::OpFixedMulti)].front()));
    CHECK(! shown(widgets[O::opParam(0, O::OpFine)].front()));
    click(root, centre(first(O::opParam(0, O::OpFixed))));
    face->layout();
    CHECK(shown(coarseA));

    // Envelope handles (oscillator A's page; the top handles sit in the display's part over the title band).
    click(root, at(428, 340));
    CHECK(ui::operatorFacePage(*face) == 0);
    const float decay = op.getParameter(O::envParam(0, O::DecayTime));
    drag(root, at(574, 34.5f), 30, 0); // Decay end: right = longer
    CHECK(op.getParameter(O::envParam(0, O::DecayTime)) > decay * 2);
    drag(root, at(460, 34.5f), 20, 40); // Peak: right = longer attack, down = lower peak
    CHECK(op.getParameter(O::envParam(0, O::AttackTime)) > 1);
    CHECK(op.getParameter(O::envParam(0, O::DecayLevel)) < 0.9f);
    const float initial = op.getParameter(O::envParam(0, O::AttackLevel));
    drag(root, at(460, 150), 0, -60); // Initial: up = higher
    CHECK(op.getParameter(O::envParam(0, O::AttackLevel)) > initial + 0.01f);
    // The release segment's middle bends it (its slope).
    const float slope = op.getParameter(O::envParam(0, O::ReleaseSlope));
    {
        const float mx = (829 + 829 + 16.4f * std::log(400.0f)) * 0.5f, my = 34.5f + 115.5f * O::slopeCurve(0.5f, slope);
        drag(root, at(mx, my), 0, -30);
    }
    CHECK(op.getParameter(O::envParam(0, O::ReleaseSlope)) < slope - 0.2f);
    REQUIRE(png("Operator-OscA-edited.png"));

    // The Oscillator header square shows the partials; drawing one makes the wave User.
    click(root, at(833, 177));
    drag(root, at(450 + 574.0f / 16 * 2.5f, 100), 0, 0);
    CHECK_NEAR(op.getParameter(O::opParam(0, O::OpWave)), O::kWaveforms - 1, 0);
    CHECK(op.getParameter(O::harmonicParam(0, 2)) > 0.3f);
    REQUIRE(png("Operator-OscA-partials.png"));
    click(root, at(457, 177));

    // Filter page: dragging in the response moves cutoff and resonance.
    click(root, at(1046, 170));
    CHECK(ui::operatorFacePage(*face) == 5);
    drag(root, at(900, 120), -60, -20);
    CHECK(op.getParameter(O::FilterFreq) < 2000);
    CHECK(op.getParameter(O::FilterRes) > 0.3f);

    // A display chooser opens its list (oscillator A's Loop mode).
    click(root, at(428, 340));
    pick = nullptr;
    click(root, centre(first(O::envParam(0, O::LoopMode))));
    REQUIRE(pick != nullptr);
    pick(1);
    CHECK_NEAR(op.getParameter(O::envParam(0, O::LoopMode)), 1, 0);
    face->layout();
    CHECK(shown(widgets[O::envParam(0, O::LoopTime)].front()));
}
