// DEV-28j: Tension's reference face (TensionFace.cpp): reachability, tabs, gestures through real input dispatch, and the
// reference's value texts.
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/Tension.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/RootWidget.h"
#include "ui/views/ReferenceFace.h"

#include <map>

using namespace sonora;
using T = devices::Tension;

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
void drag(ui::RootWidget& root, ui::MouseEvent from, float dy)
{
    root.dispatchMouseDown(from);
    auto to = from;
    to.y += dy;
    root.dispatchMouseDrag(to);
    root.dispatchMouseUp(to);
}
ui::Widget* withTooltip(ui::Widget& parent, const std::string& tooltip)
{
    for (const auto& child : parent.children())
    {
        if (child->visible && child->tooltip == tooltip) return child.get();
        if (auto* found = withTooltip(*child, tooltip)) return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE("Tension face: measured width, every parameter reachable, tabs, gestures, value texts, PNGs")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("tension_set.xml"));
    REQUIRE(loaded);
    const auto& model = loaded.song->tracks[0].devices[0];
    REQUIRE(model.type == "StringStudio");
    auto processor = devices::createNative(model, nullptr);
    REQUIRE(processor != nullptr);
    auto& p = *processor;
    CHECK_NEAR(p.getParameter(T::ExcType), T::Plectrum, 0);
    CHECK_NEAR(p.getParameter(T::CurrentTab), 0, 0);

    std::function<void(int)> pick;
    ui::UiContext context;
    context.showChoices = [&](const auto&, const auto&, auto chosen) { pick = chosen; };
    ui::RootWidget root(context);
    std::map<int, ui::Widget*> widgets; // the first widget tagged with each parameter
    auto* face = ui::addReferenceFace(root, model, processor.get(), [&](ui::Widget* w, int parameter) { widgets.emplace(parameter, w); });
    REQUIRE(face != nullptr);
    CHECK_NEAR(face->preferredWidth(342), 1324, 0.01); // the capture's device box: x 18..1342 for a 342-px body

    // Laid out at the capture's scale (1 unit per capture px under the box's 18-unit title band).
    const float body = 342, width = face->preferredWidth(body);
    root.setBounds({0, 0, width, 18 + body});
    face->setBounds({0, 18, width, body});
    face->layout();
    for (int i = 0; i < T::NumParams; ++i)
        if (! widgets.contains(i)) { std::printf("    untagged %s\n", p.parameters()[(size_t) i].id.c_str()); CHECK(false); }

    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    folder.createDirectory();
    REQUIRE(gfx::renderToPng(root, (int) root.width(), (int) root.height(), 1, folder.getChildFile("Tension-String.png").getFullPathName().toStdString()));

    // A knob drag on the String tab changes its parameter.
    auto* exciterX = widgets[T::ExcX];
    REQUIRE(exciterX->visible);
    const float before = p.getParameter(T::ExcX);
    const auto ring = exciterX->boundsInRoot();
    drag(root, point(ring.x + 22, ring.y + 40), -40);
    CHECK(p.getParameter(T::ExcX) > before + 0.1f);

    // The exciter chooser sets the type; the knobs take the type's names (Bow: Force / Friction, Damping greyed).
    CHECK(exciterX->tooltip == "Protrusion");
    click(root, centre(widgets[T::ExcType]));
    REQUIRE(pick != nullptr);
    pick(T::Bow);
    CHECK_NEAR(p.getParameter(T::ExcType), T::Bow, 0);
    face->layout();
    CHECK(exciterX->tooltip == "Force");
    CHECK(widgets[T::ExcStiffness]->tooltip == "Friction");
    pick = nullptr;
    click(root, centre(widgets[T::ExcType]));
    REQUIRE(pick != nullptr);
    pick(T::Hammer);
    face->layout();
    CHECK(exciterX->tooltip == "Mass");
    CHECK(widgets[T::ExcStiffness]->tooltip == "Stiffness");

    // Section switch and a Key box (drag up raises the amount, the wheel lowers it).
    click(root, centre(widgets[T::DamperOn]));
    CHECK(p.getParameter(T::DamperOn) > 0.5f);
    drag(root, centre(widgets[T::DamperMassKey]), -30);
    const float key = p.getParameter(T::DamperMassKey);
    CHECK(key > 0.1f);
    auto wheel = centre(widgets[T::DamperMassKey]);
    wheel.wheelDeltaY = -1;
    root.dispatchMouseWheel(wheel);
    CHECK(p.getParameter(T::DamperMassKey) < key);

    // The title tab switches to Filter/Global (CurrentTab, saved with the set); String controls hide.
    auto* globalTab = withTooltip(root, "Show the Filter/Global tab");
    REQUIRE(globalTab != nullptr);
    click(root, centre(globalTab));
    CHECK_NEAR(p.getParameter(T::CurrentTab), 1, 0);
    CHECK(! exciterX->visible);
    CHECK(widgets[T::FilterFreq]->visible);
    REQUIRE(gfx::renderToPng(root, (int) root.width(), (int) root.height(), 1, folder.getChildFile("Tension-Global.png").getFullPathName().toStdString()));

    // The filter display: dragging in it moves cutoff (x) and resonance (y); the Freq box drags too.
    root.dispatchMouseDown(point(150 - 18, 18 + 200 - 54));
    root.dispatchMouseUp(point(150 - 18, 18 + 200 - 54));
    CHECK_NEAR(p.getParameter(T::FilterFreq), (150.0f - 34) / 232, 0.01);
    CHECK_NEAR(p.getParameter(T::FilterRes), (216.0f - 200) / 101, 0.01);
    const float cutoff = p.getParameter(T::FilterFreq);
    drag(root, centre(widgets[T::FilterFreq]), -50);
    CHECK(p.getParameter(T::FilterFreq) > cutoff + 0.1f);

    // Filter type and LFO shape choosers, the Hz / sync buttons and a pressure target.
    pick = nullptr;
    click(root, centre(widgets[T::FilterType]));
    REQUIRE(pick != nullptr);
    pick(9);
    CHECK_NEAR(p.getParameter(T::FilterType), 9, 0);
    pick = nullptr;
    click(root, centre(widgets[T::PressureTarget2]));
    REQUIRE(pick != nullptr);
    pick(T::TCutoff);
    CHECK_NEAR(p.getParameter(T::PressureTarget2), T::TCutoff, 0);
    auto* hz = widgets[T::LfoSyncOn];
    const auto hzBox = hz->boundsInRoot();
    click(root, point(hzBox.x + hzBox.w + 10, hzBox.y + hzBox.h * 0.5f)); // the sync (metronome) button right of Hz
    CHECK_NEAR(p.getParameter(T::LfoSyncOn), 1, 0);
    CHECK(! widgets[T::LfoRate]->visible);
    CHECK(widgets[T::LfoSync]->visible);
    click(root, centre(widgets[T::UnisonOn]));
    CHECK(p.getParameter(T::UnisonOn) > 0.5f);

    // Back to String.
    click(root, centre(withTooltip(root, "Show the String tab")));
    CHECK_NEAR(p.getParameter(T::CurrentTab), 0, 0);
    CHECK(exciterX->visible);

    // The reference's value texts (Tension's value laws).
    const auto text = [&](int parameter, float v) { return ui::tensionValueText(p, parameter, v); };
    CHECK(text(T::FilterFreq, 0.5f) == "812 Hz");
    CHECK(text(T::FilterFreq, 1.0f) == "22.0 kHz");
    CHECK(text(T::Volume, 0.705f) == "0.0 dB");
    CHECK(text(T::Volume, 0.0f) == "-\xE2\x88\x9E dB");
    CHECK(text(T::VibratoDelay, 0.5f) == "509 ms");
    CHECK(text(T::FenvRelease, 1.0f) == "15.0 s");
    CHECK(text(T::VibratoRate, 0.5f) == "5.1 Hz");
    CHECK(text(T::FenvAttack, 0.0f) == "5 ms");
    CHECK(text(T::FenvDecay, 0.5f) == "283 ms");
    CHECK(text(T::UnisonDelay, 0.5f) == "14 ms");
    CHECK(text(T::ExcPosition, 0.5f) == "25 %");
    CHECK(text(T::ExcX, 0.5f) == "50 %");
    CHECK(text(T::BodyMix, 0.0f) == "-100 %");
    CHECK(text(T::FineTune, 0.5f) == "0.00");
    CHECK(text(T::ExcXKey, 0.0f) == "0.00");
    CHECK(text(T::ExcXKey, -1.0f / 11.0f) == "-0.16");
    CHECK(text(T::PressureAmount1, -1.0f / 11.0f) == "-9.1 %");
    CHECK(text(T::PressureAmount1, 0.25f) == "25 %");
    CHECK(text(T::LfoSync, 23) == "1/32t");
    CHECK(text(T::Transpose, -12) == "-12 st");
    CHECK(text(T::ExcType, T::HammerBouncing) == "Hammer (bouncing)");
}
