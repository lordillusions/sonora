#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableSynth.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "ui/RootWidget.h"
#include "ui/views/WavetableEditor.h"
#include "ui/views/WavetableRoutingEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <set>

using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
struct SubUi
{
    juce::ScopedJuceInitialiser_GUI init;
    model::Song song;
    midi::MidiManager midi;
    ui::UiContext context;
    std::unique_ptr<devices::DeviceProcessor> processor;
    W* synth = nullptr;
    std::unique_ptr<ui::RootWidget> root;
    Editor* editor = nullptr;
    explicit SubUi(float height = 700, bool imported = false)
    {
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        song.tracks[0].devices = {devices::nativeDeviceModel("SonoraWavetable")}; song.tracks[0].devices[0].id = 42;
        processor = devices::createNative(song.tracks[0].devices[0]); synth = dynamic_cast<W*>(processor.get()); REQUIRE(synth);
        synth->setImportedSubProfile(imported); synth->prepare(48000, 512); synth->setParameter(W::SubOn, 1);
        context.song = &song; context.midi = &midi; root = std::make_unique<ui::RootWidget>(context); root->setBounds({0, 0, 1080, height});
        editor = root->add<Editor>(synth, 0, 0); editor->setBounds(root->localBounds()); editor->layout();
    }
    ui::MouseEvent at(ui::Rect area) const { ui::MouseEvent event; event.x = area.centreX(); event.y = area.centreY(); return event; }
    void click(ui::Rect area, int clicks = 1)
    {
        REQUIRE(! area.isEmpty()); auto event = at(area); event.clicks = clicks; root->dispatchMouseDown(event); root->dispatchMouseUp(event);
    }
    void drag(int parameter, float pixels, bool fine = false)
    {
        const auto area = editor->parameterCell(parameter); REQUIRE(! area.isEmpty()); auto down = at(area); down.shift = fine; root->dispatchMouseDown(down);
        auto moved = down; moved.y -= pixels; root->dispatchMouseDrag(moved); root->dispatchMouseUp(moved);
    }
    void reveal(int parameter)
    {
        for (int page = 0; page < editor->pageCount() && editor->parameterCell(parameter).isEmpty(); ++page) click(editor->pageCell(1));
        REQUIRE(! editor->parameterCell(parameter).isEmpty());
    }
    juce::Image snapshot(const char* name)
    {
        const juce::File directory("/tmp/sonora_tests/sonora_wavetable_sub_ui_tests"); REQUIRE(directory.createDirectory().wasOk());
        const auto file = directory.getChildFile(name);
        REQUIRE(gfx::renderToPng(*root, static_cast<int>(root->width()), static_cast<int>(root->height()), 1, file.getFullPathName().toStdString()));
        auto image = juce::ImageFileFormat::loadFrom(file); REQUIRE(image.isValid()); return image;
    }
};
bool cyanNear(const juce::Image& image, float x, float y)
{
    for (int dx = -2; dx <= 2; ++dx) for (int dy = -3; dy <= 3; ++dy)
    {
        const int px = static_cast<int>(std::lround(x)) + dx, py = static_cast<int>(std::lround(y)) + dy;
        if (px < 0 || px >= image.getWidth() || py < 0 || py >= image.getHeight()) continue;
        const auto color = image.getPixelAt(px, py);
        if (color.getRed() < 150 && color.getGreen() > 130 && color.getBlue() > 150) return true;
    }
    return false;
}
void checkPreparedPreview(const SubUi& rig, const juce::Image& image)
{
    const auto plot = rig.editor->subPlot(); REQUIRE(! plot.isEmpty());
    for (const float phase : {.125f, .375f, .625f, .875f})
        CHECK(cyanNear(image, plot.x + phase * plot.w, plot.centreY() - rig.synth->subWaveformAt(phase) * plot.h * .4f));
}
} // namespace

TEST_CASE("wavetable Sub UI: phase Continue octave bounds reset MIDI and native preview respond to real dashboard gestures")
{
    SubUi rig; REQUIRE(! rig.editor->parameterCell(W::SubPhase).isEmpty()); REQUIRE(! rig.editor->parameterCell(W::SubContinuous).isEmpty());
    rig.drag(W::SubPhase, 50); CHECK_NEAR(rig.synth->getParameter(W::SubPhase), .25, 0.000001);
    rig.drag(W::SubPhase, 10, true); CHECK_NEAR(rig.synth->getParameter(W::SubPhase), .26, 0.000001);
    rig.click(rig.editor->parameterCell(W::SubPhase), 2); CHECK_NEAR(rig.synth->getParameter(W::SubPhase), 0, 0);
    rig.drag(W::SubPhase, 50); checkPreparedPreview(rig, rig.snapshot("Sub Phase.png"));
    rig.click(rig.editor->parameterCell(W::SubContinuous)); CHECK_NEAR(rig.synth->getParameter(W::SubContinuous), 1, 0);
    checkPreparedPreview(rig, rig.snapshot("Sub Continue.png"));
    rig.click(rig.editor->parameterCell(W::SubContinuous), 2); CHECK_NEAR(rig.synth->getParameter(W::SubContinuous), 0, 0);
    rig.synth->setParameter(W::SubOctave, -4); rig.drag(W::SubOctave, 200); CHECK_NEAR(rig.synth->getParameter(W::SubOctave), 4, 0);
    rig.drag(W::SubOctave, -200); CHECK_NEAR(rig.synth->getParameter(W::SubOctave), -4, 0);
    rig.click(rig.editor->parameterCell(W::SubOctave), 2); CHECK_NEAR(rig.synth->getParameter(W::SubOctave), -1, 0);
    for (int parameter : {W::SubPhase, W::SubContinuous})
    {
        rig.context.midiMapMode = true; rig.click(rig.editor->parameterCell(parameter)); REQUIRE(rig.context.mapSelection);
        CHECK(rig.context.mapSelection->param == rig.synth->parameters()[static_cast<size_t>(parameter)].id);
        rig.midi.cancelLearn(); rig.context.midiMapMode = false;
    }
    auto captured = rig.song.tracks[0].devices[0]; devices::captureDeviceState(captured, rig.processor.get());
    CHECK_NEAR(captured.param("Sub/Phase", -1), .25, 0.000001); CHECK_NEAR(captured.param("Sub/Continuous", -1), 0, 0);
    auto restored = devices::createNative(captured); REQUIRE(restored); CHECK_NEAR(restored->getParameter(W::SubPhase), .25, 0.000001);
}

TEST_CASE("wavetable Sub UI: the rendered preview follows the prepared imported sine polarity scale phase and continuous policy")
{
    SubUi rig(700, true); rig.drag(W::SubPhase, 50);
    CHECK(rig.synth->subWaveformAt(.125f) < 0); checkPreparedPreview(rig, rig.snapshot("Imported Sub Phase.png"));
    rig.click(rig.editor->parameterCell(W::SubContinuous)); checkPreparedPreview(rig, rig.snapshot("Imported Sub Continue.png"));
}

TEST_CASE("wavetable Sub UI: minimum dashboard controls stay separate and Routing opens the exact Sub source without changing legacy Direct")
{
    SubUi rig(500); std::vector<ui::Rect> cells;
    for (int parameter : std::array<int, 7>{W::SubOn, W::SubWave, W::SubOctave, W::SubPhase, W::SubPan, W::SubLevel, W::SubContinuous})
    {
        const auto area = rig.editor->parameterCell(parameter); REQUIRE(! area.isEmpty());
        CHECK(area.x >= 4); CHECK(area.right() <= 96); CHECK(area.bottom() <= rig.editor->subRoutingCell().y);
        cells.push_back(area);
    }
    cells.push_back(rig.editor->subPlot()); cells.push_back(rig.editor->subRoutingCell());
    for (size_t first = 0; first < cells.size(); ++first) for (size_t second = first + 1; second < cells.size(); ++second)
    {
        const auto a = cells[first], b = cells[second];
        CHECK(a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y);
    }
    rig.drag(W::SubPhase, 50); rig.click(rig.editor->parameterCell(W::SubContinuous)); rig.snapshot("Minimum Sub.png");
    rig.synth->setParameter(W::SourceRoutingOn, 1); rig.synth->setParameter(W::SubDirect, 1); rig.click(rig.editor->subRoutingCell());
    CHECK(rig.editor->workflow() == Editor::Page::Mix); REQUIRE(rig.editor->routingEditor()->visible);
    CHECK(! rig.editor->routingEditor()->parameterCell(4, ui::WavetableRoutingEditor::Field::Destination).isEmpty());
    CHECK(rig.editor->parameterCell(W::SubPhase).isEmpty()); CHECK_NEAR(rig.synth->getParameter(W::SubDirect), 1, 0);
    CHECK_NEAR(rig.synth->getParameter(W::SourceRoutingOn), 1, 0); rig.snapshot("Sub Source Routing.png");
}

TEST_CASE("wavetable Sub UI: all base controls remain reachable in the compact inspector and new Sub controls appear in Global")
{
    SubUi rig(210); std::set<int> reached;
    for (int section = 0; section < static_cast<int>(Editor::Section::Count); ++section)
    {
        rig.click(rig.editor->sectionCell(static_cast<Editor::Section>(section)));
        for (int instance = 0; instance < rig.editor->instanceCount(); ++instance)
        {
            for (int count = 0; count < rig.editor->instanceCount() && rig.editor->selectedInstance() != instance; ++count) rig.click(rig.editor->selectorCell(1));
            REQUIRE(rig.editor->selectedInstance() == instance);
            for (int page = 0; page < rig.editor->pageCount(); ++page)
            {
                for (int parameter : rig.editor->visibleParameters()) reached.insert(parameter);
                if (rig.editor->pageCount() > 1) rig.click(rig.editor->pageCell(1));
            }
        }
    }
    CHECK(reached.size() == static_cast<size_t>(W::ParameterCount)); CHECK(reached.contains(W::SubPhase)); CHECK(reached.contains(W::SubContinuous)); CHECK(reached.contains(W::SubDirect));
    rig.click(rig.editor->sectionCell(Editor::Section::Main)); rig.reveal(W::SubPhase); rig.drag(W::SubPhase, 50);
    CHECK_NEAR(rig.synth->getParameter(W::SubPhase), .25, 0.000001); rig.reveal(W::SubContinuous); rig.click(rig.editor->parameterCell(W::SubContinuous));
    CHECK_NEAR(rig.synth->getParameter(W::SubContinuous), 1, 0); rig.snapshot("Compact Sub Controls.png");
    rig.root->setBounds({0, 0, 1080, 500}); rig.editor->setBounds(rig.root->localBounds()); rig.editor->layout();
    rig.click(rig.editor->workflowCell(Editor::Page::Global)); CHECK(! rig.editor->parameterCell(W::SubPhase).isEmpty()); CHECK(! rig.editor->parameterCell(W::SubContinuous).isEmpty());
    rig.snapshot("Global Sub Controls.png");
}
