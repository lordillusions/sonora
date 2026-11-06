// DEV-28b: the imported Wavetable's reference face (WavetableFace.cpp).
#include "TestFramework.h"

#include "devices/DeviceFactory.h"
#include "devices/native/WavetableInstrument.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/RootWidget.h"
#include "ui/views/ReferenceFace.h"

#include <set>

using namespace sonora;
using W = devices::WavetableInstrument;

namespace
{
ui::MouseEvent at(ui::Rect r, float x = 0.5f, float y = 0.5f)
{
    ui::MouseEvent e;
    e.x = r.x + r.w * x;
    e.y = r.y + r.h * y;
    return e;
}
void click(ui::RootWidget& root, ui::MouseEvent e) { root.dispatchMouseDown(e); root.dispatchMouseUp(e); }
ui::Widget* withTooltip(ui::Widget& parent, const std::string& tooltip)
{
    for (const auto& child : parent.children())
    {
        if (child->tooltip == tooltip) return child.get();
        if (auto* found = withTooltip(*child, tooltip)) return found;
    }
    return nullptr;
}
} // namespace

TEST_CASE("Wavetable face: measured width, every parameter reachable, expanded view, PNGs")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("wavetable_set.xml"));
    REQUIRE(loaded);
    const auto& model = loaded.song->tracks[0].devices[0];
    REQUIRE(model.type == "InstrumentVector");
    auto processor = devices::createNative(model, nullptr);
    REQUIRE(processor != nullptr);
    ui::UiContext context;
    context.showChoices = [](const auto&, const auto&, const auto&) {};
    ui::RootWidget root(context);
    std::set<int> tagged;
    auto* face = ui::addReferenceFace(root, model, processor.get(), [&](ui::Widget*, int parameter) { tagged.insert(parameter); });
    REQUIRE(face != nullptr);
    CHECK_NEAR(face->preferredWidth(197), 1143, 0.01); // the capture's device body: x 5..1148 for 197 px
    const float body = ui::ReferenceFace::kDefaultBody;
    const auto place = [&] {
        root.setBounds({0, 0, face->preferredWidth(body), 18 + body});
        face->setBounds({0, 18, face->preferredWidth(body), body});
        face->layout();
    };
    place();
    for (int p = 0; p < W::MatrixFirst; ++p)
        if (p != W::HiQ && ! tagged.contains(p)) { std::printf("    untagged %s\n", processor->parameters()[(size_t) p].id.c_str()); CHECK(false); } // HiQ lives in the reference's context menu
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora-device-faces");
    REQUIRE(gfx::renderToPng(root, (int) root.width(), (int) root.height(), 2, folder.getChildFile("Wavetable.png").getFullPathName().toStdString()));

    // The Osc 2 tab switches the oscillator panel; the expand switch opens the envelopes and LFOs to the right.
    auto* expand = withTooltip(root, "Show the envelopes and LFOs");
    REQUIRE(expand != nullptr);
    click(root, at(expand->boundsInRoot()));
    CHECK(face->preferredWidth(197) > 2400);
    place();
    REQUIRE(gfx::renderToPng(root, (int) root.width(), (int) root.height(), 2, folder.getChildFile("Wavetable-expanded.png").getFullPathName().toStdString()));

    // Dragging a matrix cell changes its amount; the LFO shape glyphs pick the shape.
    auto* cell = withTooltip(*face, "Drag to set the modulation amount, double-click to clear it");
    REQUIRE(cell != nullptr);
    const float before = processor->getParameter(W::matrixParam(W::TOsc1Pos, W::SrcAmp));
    auto down = at(cell->boundsInRoot());
    root.dispatchMouseDown(down);
    auto up = down;
    up.y -= 40;
    root.dispatchMouseDrag(up);
    root.dispatchMouseUp(up);
    CHECK(processor->getParameter(W::matrixParam(W::TOsc1Pos, W::SrcAmp)) > before + 0.1f);
    auto* square = withTooltip(*face, "Square");
    REQUIRE(square != nullptr);
    click(root, at(square->boundsInRoot()));
    CHECK_NEAR(processor->getParameter(W::L1Type), 3, 0);
}
