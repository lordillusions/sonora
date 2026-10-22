#include "TestFramework.h"
#include "devices/DeviceFactory.h"
#include "devices/native/WavetableModRouting.h"
#include "devices/native/WavetableSynth.h"
#include "engine/EngineGraph.h"
#include "engine/SampleCache.h"
#include "gfx/SkiaCanvasComponent.h"
#include "midi/MidiManager.h"
#include "plugins/SerumLibrary.h"
#include "ui/views/MainLayout.h"
#include "ui/views/WavetableEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>

// UI-28: drag-to-modulate, modulation rings and the editor keyboard footer, driven through RootWidget input events.
using namespace sonora;
namespace
{
using W = devices::WavetableSynth;
using Editor = ui::WavetableEditor;
namespace R = devices::wavetable_routing;

template <typename T> void collect(ui::Widget& widget, std::vector<T*>& result)
{
    if (auto* found = dynamic_cast<T*>(&widget)) result.push_back(found);
    for (const auto& child : widget.children()) collect(*child, result);
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI init;
    midi::MidiManager midi;
    model::Song song;
    engine::SampleCache samples;
    std::unique_ptr<engine::EngineGraph> graph;
    ui::UiContext context;
    std::unique_ptr<ui::MainLayout> root;
    Editor* editor = nullptr;
    W* processor = nullptr;
    std::vector<std::string> menu;
    int pick = -1;

    Rig()
    {
        song.tracks.resize(1); song.tracks[0].kind = model::TrackKind::Midi;
        song.tracks[0].devices = {devices::nativeDeviceModel("SonoraWavetable")};
        song.tracks[0].devices[0].id = 42;
        devices::BuildContext build; graph = engine::buildGraph(song, build, samples).graph;
        REQUIRE(graph); graph->prepare(48000, 512);
        processor = dynamic_cast<W*>(graph->nodes[0].processorForModelDevice[0]); REQUIRE(processor);
        context.song = &song; context.graph = graph.get(); context.controls = graph->controls; context.midi = &midi;
        context.browserVisible = false; context.detailTab = ui::DetailTab::Devices;
        context.theme.metrics.detailHeight = 700;
        context.showChoices = [this](const std::vector<std::string>& labels, const std::vector<bool>&, std::function<void(int)> choose) {
            menu = labels; if (pick >= 0) choose(pick);
        };
        root = std::make_unique<ui::MainLayout>(context); root->setBounds({0, 0, 1400, 1000}); root->layout();
        std::vector<Editor*> editors; collect(*root, editors); REQUIRE(editors.size() == 1); editor = editors[0];
    }
    ui::MouseEvent at(ui::Rect cell, float fx = 0.5f, float fy = 0.5f) const
    {
        const auto origin = editor->boundsInRoot();
        ui::MouseEvent event; event.x = origin.x + cell.x + cell.w * fx; event.y = origin.y + cell.y + cell.h * fy;
        return event;
    }
    void click(ui::Rect cell) { REQUIRE(! cell.isEmpty()); auto e = at(cell); root->dispatchMouseDown(e); root->dispatchMouseUp(e); }
    // Press on `from`, move in steps to `to`, release there: the same path a pointer takes.
    void drag(ui::Rect source, ui::Rect target, bool checkHover = false, int expectedTarget = -1)
    {
        REQUIRE(! source.isEmpty()); REQUIRE(! target.isEmpty());
        auto down = at(source), up = at(target);
        root->dispatchMouseDown(down);
        for (int step = 1; step <= 4; ++step)
        {
            auto move = down; move.x += (up.x - down.x) * (float) step / 4; move.y += (up.y - down.y) * (float) step / 4;
            root->dispatchMouseDrag(move);
        }
        if (checkHover)
        {
            CHECK(editor->modulationDragSource() >= 0);
            const auto origin = editor->boundsInRoot();
            CHECK(editor->modulationTargetAt(up.x - origin.x, up.y - origin.y) == expectedTarget);
        }
        root->dispatchMouseUp(up);
    }
    void pump(int frames = 64) { juce::AudioBuffer<float> audio(2, frames); juce::MidiBuffer events; processor->process(audio, events); }
};
} // namespace

TEST_CASE("wavetable drag modulation UI: dropping LFO/ENV/macro handles on knobs creates rings; rings rescale; right-click removes")
{
    Rig rig;
    const int position = W::oscParam(0, W::Position), cutoff = W::filterParam(0, W::Cutoff), level = W::oscParam(1, W::Level);
    REQUIRE(! rig.editor->parameterCell(position).isEmpty());
    REQUIRE(! rig.editor->modulationSourceCell(W::Lfo1).isEmpty());
    REQUIRE(! rig.editor->modulationSourceCell(W::Env2).isEmpty());
    REQUIRE(! rig.editor->modulationSourceCell(W::Macro3).isEmpty());
    CHECK(rig.editor->modulationRingCell(position).isEmpty() == false); // geometry exists even before a route

    // LFO 1 -> OSC A wavetable position: hover highlights the target, the drop writes a matrix route with the default depth.
    rig.drag(rig.editor->modulationSourceCell(W::Lfo1), rig.editor->parameterCell(position), true, position);
    CHECK(rig.editor->modulationDragSource() == -1);
    auto routes = R::routesForParameter(*rig.processor, position);
    REQUIRE(routes.size() == 1);
    CHECK(routes[0].source == W::Lfo1); CHECK(routes[0].destination == W::Osc1Position);
    CHECK_NEAR(routes[0].amount, R::defaultAmount(*rig.processor, position), 1e-6);
    CHECK(rig.context.statusText.find("LFO 1") != std::string::npos);
    // The Matrix page sees the same slot.
    CHECK_NEAR(rig.processor->getParameter(W::modParam(routes[0].slot, W::ModSource)), W::Lfo1, 0);

    // Dragging the ring up deepens the route (150 px = full knob travel); the knob value itself stays put.
    const float before = rig.processor->getParameter(position);
    auto grab = rig.at(rig.editor->modulationRingCell(position, 0)); rig.root->dispatchMouseDown(grab);
    auto lifted = grab; lifted.y -= 30; rig.root->dispatchMouseDrag(lifted); rig.root->dispatchMouseUp(lifted);
    CHECK_NEAR(R::routesForParameter(*rig.processor, position)[0].amount, R::defaultAmount(*rig.processor, position) + 0.2f, 1e-4);
    CHECK_NEAR(rig.processor->getParameter(position), before, 0);
    // Dragging the knob body still turns the knob.
    auto body = rig.at(rig.editor->parameterCell(position)); rig.root->dispatchMouseDown(body);
    auto turned = body; turned.y -= 20; rig.root->dispatchMouseDrag(turned); rig.root->dispatchMouseUp(turned);
    CHECK(rig.processor->getParameter(position) > before);

    // Dropping the same source again reuses its slot; ENV 2 and a macro get their own slots and rings.
    rig.drag(rig.editor->modulationSourceCell(W::Lfo1), rig.editor->parameterCell(position));
    CHECK(R::routesForParameter(*rig.processor, position).size() == 1);
    rig.drag(rig.editor->modulationSourceCell(W::Env2), rig.editor->parameterCell(cutoff), true, cutoff);
    rig.drag(rig.editor->modulationSourceCell(W::Macro3), rig.editor->parameterCell(level));
    REQUIRE(R::routesForParameter(*rig.processor, cutoff).size() == 1);
    CHECK(R::routesForParameter(*rig.processor, cutoff)[0].source == W::Env2);
    REQUIRE(R::routesForParameter(*rig.processor, level).size() == 1);
    CHECK(R::routesForParameter(*rig.processor, level)[0].source == W::Macro3);
    rig.drag(rig.editor->modulationSourceCell(W::Lfo2), rig.editor->parameterCell(position));
    CHECK(R::routesForParameter(*rig.processor, position).size() == 2);
    CHECK(! rig.editor->modulationRingCell(position, 1).isEmpty());

    // Not a destination (Unison) or empty space: nothing is written.
    const int unison = W::oscParam(0, W::Unison);
    REQUIRE(! rig.editor->parameterCell(unison).isEmpty());
    rig.drag(rig.editor->modulationSourceCell(W::Lfo3), rig.editor->parameterCell(unison), true, -1);
    rig.drag(rig.editor->modulationSourceCell(W::Lfo3), rig.editor->oscillatorPlot(1));
    for (int slot = 0; slot < W::kModSlots; ++slot)
        CHECK(std::lround(rig.processor->getParameter(W::modParam(slot, W::ModSource))) != W::Lfo3);

    // Right-click lists one removal per route plus Remove All / Return to Default; choosing the first removes LFO 1 only.
    rig.pick = 0;
    auto context = rig.at(rig.editor->parameterCell(position)); context.right = true;
    rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context);
    REQUIRE(rig.menu.size() == 4);
    CHECK(rig.menu[0].find("LFO 1") != std::string::npos); CHECK(rig.menu[2] == "Remove All Modulation");
    routes = R::routesForParameter(*rig.processor, position);
    REQUIRE(routes.size() == 1); CHECK(routes[0].source == W::Lfo2);
    rig.pick = 1; // now: [Remove LFO 2, Remove All, Return to Default]
    rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context);
    CHECK(R::routesForParameter(*rig.processor, position).empty());
    CHECK(R::routesForParameter(*rig.processor, cutoff).size() == 1); // other controls keep theirs

    // The dropped routes are audible: Macro 3 at 1 with the default (positive) depth raises OSC B's level.
    rig.processor->setParameter(W::macroParam(2), 1);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    rig.drag(rig.editor->modulationSourceCell(W::Lfo1), rig.editor->parameterCell(position));
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("Rings.png").getFullPathName().toStdString()));
}

TEST_CASE("wavetable keyboard UI: slim keys play directly; pitch bend springs back, mod wheel holds; ARP view plays the arpeggiator")
{
    Rig rig;
    REQUIRE(! rig.editor->keyboardCell(Editor::kLowestKey).isEmpty());
    REQUIRE(! rig.editor->keyboardCell(Editor::kHighestKey).isEmpty());
    CHECK(rig.editor->keyboardCell(Editor::kLowestKey - 1).isEmpty());
    CHECK(rig.editor->keyboardCell(60).h <= 30); // slimmer than the old 42 px footer
    CHECK(! rig.editor->arpView());

    // Velocity follows the press depth on the key.
    auto shallow = rig.at(rig.editor->keyboardCell(64), 0.5f, 0.1f); rig.root->dispatchMouseDown(shallow); rig.pump();
    CHECK(rig.processor->noteLit(64)); CHECK(rig.processor->activeVoiceCount() == 1);
    rig.root->dispatchMouseUp(shallow); rig.pump(); CHECK(! rig.processor->noteLit(64));

    // Pitch bend: up while held, centre on release.
    auto bend = rig.at(rig.editor->pitchBendCell(), 0.5f, 0.5f); rig.root->dispatchMouseDown(bend);
    auto bendUp = rig.at(rig.editor->pitchBendCell(), 0.5f, 0.0f); rig.root->dispatchMouseDrag(bendUp); rig.pump();
    CHECK(rig.processor->pitchBendDisplay() > 0.95f);
    rig.root->dispatchMouseUp(bendUp); rig.pump();
    CHECK_NEAR(rig.processor->pitchBendDisplay(), 0, 1e-6);
    // Mod wheel: stays where it is left.
    auto wheel = rig.at(rig.editor->modWheelCell(), 0.5f, 0.95f); rig.root->dispatchMouseDown(wheel);
    auto wheelUp = rig.at(rig.editor->modWheelCell(), 0.5f, 0.5f); rig.root->dispatchMouseDrag(wheelUp); rig.root->dispatchMouseUp(wheelUp); rig.pump();
    CHECK_NEAR(rig.processor->modWheelDisplay(), 0.5, 0.05);

    // ARP view: the arp controls replace the left of the keyboard; enabling it arpeggiates a held key.
    rig.click(rig.editor->keyboardViewCell(true));
    CHECK(rig.editor->arpView());
    for (int parameter : {W::arpParam(W::ArpOn), W::arpParam(W::ArpMode), W::arpParam(W::ArpRate), W::arpParam(W::ArpOctaves), W::arpParam(W::ArpGate)})
        CHECK(! rig.editor->parameterCell(parameter).isEmpty());
    REQUIRE(! rig.editor->keyboardCell(60).isEmpty());
    CHECK(rig.editor->keyboardCell(60).x > rig.editor->parameterCell(W::arpParam(W::ArpGate)).right());
    rig.click(rig.editor->parameterCell(W::arpParam(W::ArpOn)));
    CHECK_NEAR(rig.processor->getParameter(W::arpParam(W::ArpOn)), 1, 0);
    rig.processor->setParameter(W::arpParam(W::ArpOctaves), 2);
    auto held = rig.at(rig.editor->keyboardCell(60), 0.5f, 0.8f); rig.root->dispatchMouseDown(held);
    std::vector<int> played; int last = -2;
    for (int block = 0; block < 300 && played.size() < 3; ++block)
    {
        rig.pump(512);
        const int note = rig.processor->arpNoteDisplay();
        if (note >= 0 && note != last) played.push_back(note);
        last = note;
    }
    CHECK(played == (std::vector<int>{60, 72, 60}));
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("ArpView.png").getFullPathName().toStdString()));
    rig.root->dispatchMouseUp(held); rig.pump();
    CHECK(rig.processor->arpNoteDisplay() == -1);
    rig.click(rig.editor->keyboardViewCell(false)); CHECK(! rig.editor->arpView());
    CHECK(rig.editor->parameterCell(W::arpParam(W::ArpOn)).isEmpty());
}

TEST_CASE("wavetable live UI: a sounding note animates the filter curve, LFO playhead and knob markers")
{
    Rig rig;
    const int cutoff = W::filterParam(0, W::Cutoff);
    rig.drag(rig.editor->modulationSourceCell(W::Env2), rig.editor->parameterCell(cutoff));
    REQUIRE(R::routesForParameter(*rig.processor, cutoff).size() == 1);
    rig.processor->setParameter(W::modParam(R::routesForParameter(*rig.processor, cutoff)[0].slot, W::ModAmount), -0.6f);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    const auto render = [&](const char* name) {
        const auto file = directory.getChildFile(name);
        REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, file.getFullPathName().toStdString()));
        return juce::ImageFileFormat::loadFrom(file);
    };
    const auto differs = [&](const juce::Image& a, const juce::Image& b, ui::Rect cell) {
        const auto origin = rig.editor->boundsInRoot(); int changed = 0;
        for (int y = (int) (origin.y + cell.y); y < (int) (origin.y + cell.bottom()); ++y)
            for (int x = (int) (origin.x + cell.x); x < (int) (origin.x + cell.right()); ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) ++changed;
        return changed;
    };
    rig.pump(512);
    const auto idle = render("LiveIdle.png");
    auto key = rig.at(rig.editor->keyboardCell(60), 0.5f, 0.8f); rig.root->dispatchMouseDown(key);
    for (int block = 0; block < 10; ++block) rig.pump(512);
    CHECK(rig.processor->liveState().active);
    const auto playing = render("LivePlaying.png");
    CHECK(differs(idle, playing, rig.editor->lfoPlot()) > 20);                         // playhead on the LFO curve
    CHECK(differs(idle, playing, rig.editor->parameterCell(cutoff)) > 5);              // live marker on the Cutoff ring
    CHECK(differs(idle, playing, rig.editor->modulationSourceCell(W::Env2)) > 5);      // ENV 2 handle activity
    const auto filterPanel = rig.editor->parameterCell(W::filterParam(0, W::FilterMode));
    CHECK(differs(idle, playing, {filterPanel.x - 70, filterPanel.bottom() + 8, 140, 50}) > 20); // the response curve moved
    rig.root->dispatchMouseUp(key);
}

TEST_CASE("wavetable ring readout UI: hover shows source, amount and range; right-click picks a direction; double-click toggles both sides")
{
    Rig rig;
    const int cutoff = W::filterParam(0, W::Cutoff);
    rig.drag(rig.editor->modulationSourceCell(W::Env1), rig.editor->parameterCell(cutoff));
    const int slot = R::routesForParameter(*rig.processor, cutoff).at(0).slot;
    // Hover the ring: the readout names the route and its plain range.
    auto hover = rig.at(rig.editor->modulationRingCell(cutoff, 0)); rig.root->dispatchMouseMove(hover);
    CHECK(rig.editor->ringTip().parameter == cutoff); CHECK(rig.editor->ringTip().slot == slot);
    const auto [title, range] = rig.editor->ringReadout(cutoff, slot);
    CHECK(title.find("Env 1") != std::string::npos); CHECK(title.find(std::to_string((int) std::lround(R::defaultAmount(*rig.processor, cutoff) * 100)) + "%") != std::string::npos);
    CHECK(range.find("Range:") == 0); CHECK(range.find("kHz") != std::string::npos);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("RingReadout.png").getFullPathName().toStdString()));
    // Right-click on the ring: Forward / Backward / Both / Remove.
    rig.pick = 1; auto context = hover; context.right = true;
    rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context);
    REQUIRE(rig.menu.size() == 4); CHECK(rig.menu[0].find("✓") == 0);
    CHECK(R::direction(*rig.processor, slot) == R::Direction::Backward);
    rig.pick = 2; rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context);
    CHECK(R::direction(*rig.processor, slot) == R::Direction::Both);
    auto twice = hover; twice.clicks = 2; rig.root->dispatchMouseDown(twice); rig.root->dispatchMouseUp(twice);
    CHECK(R::direction(*rig.processor, slot) == R::Direction::Forward);
    rig.pick = 3; rig.root->dispatchMouseDown(context); rig.root->dispatchMouseUp(context);
    CHECK(R::routesForParameter(*rig.processor, cutoff).empty());
    // Moving away clears the readout.
    auto away = rig.at(rig.editor->envelopePlot()); rig.root->dispatchMouseMove(away);
    CHECK(rig.editor->ringTip().slot == -1);
}

TEST_CASE("wavetable VELO/NOTE UI: tabs show the curve, points drag, double-click resets, handles modulate")
{
    Rig rig;
    rig.click(rig.editor->voiceTabCell(1));
    CHECK(rig.editor->voiceTab() == 1);
    REQUIRE(! rig.editor->curveSourcePlot().isEmpty());
    CHECK(rig.editor->parameterCell(W::voiceModeParam(W::MonoOn)).isEmpty());
    // Drag the middle point up-left.
    const auto plot = rig.editor->curveSourcePlot();
    auto down = rig.at(rig.editor->curveSourcePoint(1)); rig.root->dispatchMouseDown(down);
    auto up = rig.at({plot.x + plot.w * 0.25f, plot.y + plot.h * 0.1f, 0, 0}); rig.root->dispatchMouseDrag(up); rig.root->dispatchMouseUp(up);
    CHECK_NEAR(rig.processor->getParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidX)), 0.25, 0.02);
    CHECK_NEAR(rig.processor->getParameter(W::curveSourceParam(W::VelocityCurve, W::CurveMidY)), 0.9, 0.02);
    // The end point only moves vertically.
    auto end = rig.at(rig.editor->curveSourcePoint(2)); rig.root->dispatchMouseDown(end);
    auto lower = end; lower.x -= 30; lower.y += plot.h * 0.5f; rig.root->dispatchMouseDrag(lower); rig.root->dispatchMouseUp(lower);
    CHECK_NEAR(rig.processor->getParameter(W::curveSourceParam(W::VelocityCurve, W::CurveEnd)), 0.5, 0.03);
    // The Velocity handle drags like any other source.
    REQUIRE(! rig.editor->modulationSourceCell(W::Velocity).isEmpty());
    rig.click(rig.editor->voiceTabCell(2)); CHECK(rig.editor->voiceTab() == 2);
    REQUIRE(! rig.editor->modulationSourceCell(W::Keytrack).isEmpty());
    rig.drag(rig.editor->modulationSourceCell(W::Velocity), rig.editor->parameterCell(W::oscParam(0, W::Level)));
    REQUIRE(R::routesForParameter(*rig.processor, W::oscParam(0, W::Level)).size() == 1);
    CHECK(R::routesForParameter(*rig.processor, W::oscParam(0, W::Level))[0].source == W::Velocity);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    rig.click(rig.editor->voiceTabCell(1));
    auto key = rig.at(rig.editor->keyboardCell(60), 0.5f, 0.6f); rig.root->dispatchMouseDown(key); rig.pump(512);
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("VeloCurve.png").getFullPathName().toStdString()));
    rig.root->dispatchMouseUp(key);
    auto reset = rig.at(rig.editor->curveSourcePlot()); reset.clicks = 2; rig.root->dispatchMouseDown(reset); rig.root->dispatchMouseUp(reset);
    CHECK_NEAR(rig.processor->getParameter(W::curveSourceParam(W::VelocityCurve, W::CurveEnd)), 1, 0);
    rig.click(rig.editor->voiceTabCell(0)); CHECK(! rig.editor->parameterCell(W::voiceModeParam(W::MonoOn)).isEmpty());
}

TEST_CASE("wavetable library UI: arrows step through the installed Serum tables and presets once one is loaded")
{
    const auto tables = plugins::serumLibrary("Tables"), presets = plugins::serumLibrary("Presets");
    if (tables.empty() || presets.empty()) return; // Serum 2 not installed on this machine
    Rig rig;
    std::vector<std::string> tablePaths, presetPaths;
    rig.context.importWavetable = [&](int, int, int, const std::string& path, int, int) { tablePaths.push_back(path); };
    rig.context.importWavetablePreset = [&](int, int, const std::string& path, int, int) { presetPaths.push_back(path); };
    const int table = W::oscParam(0, W::Table);
    // Load a library table into OSC A directly, then step with the arrows.
    const auto shapes = std::find_if(tables.begin(), tables.end(), [](const auto& item) { return item.name == "Default Shapes"; });
    REQUIRE(shapes != tables.end());
    rig.processor->setCustomTable(0, plugins::loadSerumTable(juce::File(juce::String(shapes->path))).data);
    rig.root->layout();
    const auto cell = rig.editor->parameterCell(table);
    auto right = rig.at(cell, 0.98f); rig.root->dispatchMouseDown(right); rig.root->dispatchMouseUp(right);
    REQUIRE(tablePaths.size() == 1);
    CHECK(tablePaths[0] == tables[(size_t) ((shapes - tables.begin() + 1) % (long) tables.size())].path);
    // Presets: no library preset yet -> built-in cycle; after a library load the arrows walk the library.
    rig.editor->loadLibraryPreset(presets[3].path);
    REQUIRE(presetPaths.size() == 1);
    rig.click(rig.editor->presetCell(1)); rig.click(rig.editor->presetCell(-1)); rig.click(rig.editor->presetCell(-1));
    REQUIRE(presetPaths.size() == 4);
    CHECK(presetPaths[1] == presets[4].path); CHECK(presetPaths[2] == presets[3].path); CHECK(presetPaths[3] == presets[2].path);
}

TEST_CASE("wavetable envelope graph UI: dragging the points sets times and sustain; curve handles bend segments; off modules dim")
{
    Rig rig;
    const int envelope = 0;
    const auto value = [&](W::EnvParam p) { return rig.processor->getParameter(W::envParam(envelope, p)); };
    for (int point : {0, 2, 3, 4, 5, 6}) REQUIRE(! rig.editor->envelopePoint(point).isEmpty());
    CHECK(rig.editor->envelopePoint(1).isEmpty()); // no hold yet
    const auto plot = rig.editor->envelopePlot();
    const auto dragPoint = [&](int point, float dx, float dy) {
        auto down = rig.at(rig.editor->envelopePoint(point)); rig.root->dispatchMouseDown(down);
        auto move = down; move.x += dx; move.y += dy; rig.root->dispatchMouseDrag(move); rig.root->dispatchMouseUp(move);
    };
    // Attack end to the right lengthens the attack; it follows the pointer on a frozen time axis.
    const float attack = value(W::Attack), decay = value(W::Decay), release = value(W::Release), sustain = value(W::Sustain);
    dragPoint(0, 40, 0);
    CHECK(value(W::Attack) > attack + 0.05f);
    // Decay/sustain point: right = longer decay, down = lower sustain.
    dragPoint(2, 30, plot.h * 0.3f);
    CHECK(value(W::Decay) > decay); CHECK(value(W::Sustain) < sustain);
    // Release end to the left shortens it; it cannot go below zero.
    dragPoint(3, -2000, 0);
    CHECK(value(W::Release) < release); CHECK_NEAR(value(W::Release), 0, 0);
    // Curve handles: dragging up raises the segment's midpoint.
    for (int handle : {4, 5})
    {
        const float before = rig.editor->envelopePoint(handle).centreY();
        dragPoint(handle, 0, -25);
        CHECK(rig.editor->envelopePoint(handle).centreY() < before - 3);
    }
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    // Sub, Noise and a filter switched off are veiled like an off oscillator; the power switch stays clear.
    rig.processor->setParameter(W::SubOn, 0); rig.processor->setParameter(W::NoiseOn, 0); rig.processor->setParameter(W::filterParam(1, W::FilterOn), 0);
    const auto shot = directory.getChildFile("EnvelopeDrag.png");
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, shot.getFullPathName().toStdString()));
    rig.processor->setParameter(W::SubOn, 1); rig.processor->setParameter(W::NoiseOn, 1); rig.processor->setParameter(W::filterParam(1, W::FilterOn), 1);
    const auto lit = directory.getChildFile("ModulesOn.png");
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, lit.getFullPathName().toStdString()));
    const auto off = juce::ImageFileFormat::loadFrom(shot), on = juce::ImageFileFormat::loadFrom(lit);
    const auto origin = rig.editor->boundsInRoot();
    const auto brightness = [&](const juce::Image& image, ui::Rect cell) {
        double sum = 0; int count = 0;
        for (int y = (int) (origin.y + cell.y); y < (int) (origin.y + cell.bottom()); y += 2)
            for (int x = (int) (origin.x + cell.x); x < (int) (origin.x + cell.right()); x += 2) { sum += image.getPixelAt(x, y).getBrightness(); ++count; }
        return sum / std::max(1, count);
    };
    for (int parameter : {(int) W::SubLevel, (int) W::NoiseLevel, W::filterParam(1, W::Cutoff)})
        CHECK(brightness(off, rig.editor->parameterCell(parameter)) < brightness(on, rig.editor->parameterCell(parameter)) * 0.8);
}

TEST_CASE("wavetable imported display UI: table names, Serum warp labels and the Unison stepper")
{
    Rig rig;
    const auto tables = plugins::serumLibrary("Tables");
    const auto shapes = std::find_if(tables.begin(), tables.end(), [](const auto& item) { return item.name == "Default Shapes"; });
    if (shapes != tables.end())
    {
        rig.processor->setCustomTable(0, plugins::loadSerumTable(juce::File(juce::String(shapes->path))).data);
        CHECK(rig.processor->customTableName(0).find("Default Shapes") != std::string::npos);
    }
    rig.processor->setImportedWarpLabel(1, 0, {W::Bend, "BEND+", true});
    rig.processor->setImportedWarpLabel(1, 1, {W::WarpOff, "DIST TUBE", false});
    rig.processor->setParameter(W::oscParam(1, W::Warp1Mode), W::Bend);
    rig.processor->setParameter(W::oscParam(1, W::On), 1);
    rig.root->layout();
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("ImportedLabels.png").getFullPathName().toStdString()));
    // Unison: the ▲ ▼ halves of the box step the voice count.
    const int unison = W::oscParam(0, W::Unison);
    const auto cell = rig.editor->parameterCell(unison);
    REQUIRE(! cell.isEmpty());
    const float before = rig.processor->getParameter(unison);
    const float boxRight = cell.centreX() + std::min(cell.w - 4, 44.0f) * 0.5f;
    auto upArrow = rig.at({boxRight - 6, cell.y, 1, cell.h - 12}, 0, 0.35f); rig.root->dispatchMouseDown(upArrow); rig.root->dispatchMouseUp(upArrow);
    CHECK_NEAR(rig.processor->getParameter(unison), before + 1, 0);
    rig.root->dispatchMouseDown(upArrow); rig.root->dispatchMouseUp(upArrow);
    auto downArrow = rig.at({boxRight - 6, cell.y, 1, cell.h - 12}, 0, 0.65f); rig.root->dispatchMouseDown(downArrow); rig.root->dispatchMouseUp(downArrow);
    CHECK_NEAR(rig.processor->getParameter(unison), before + 1, 0);
}

TEST_CASE("wavetable LFO/POLY/FX rack UI: Serum LFO modes and rate, POLY stepper, stacked FX panels select on click")
{
    Rig rig;
    // LFO mode buttons drive the Trigger parameter.
    for (int mode : {0, 2, 1})
    {
        rig.click(rig.editor->lfoModeCell(mode));
        CHECK_NEAR(rig.processor->getParameter(W::lfoParam(0, W::Mode)), mode, 0);
    }
    // BPM shows a division RATE knob; HZ shows the free rate.
    REQUIRE(rig.processor->getParameter(W::lfoParam(0, W::SyncOn)) > 0.5f);
    const auto bpmRate = rig.editor->lfoRateCell(); REQUIRE(! bpmRate.isEmpty());
    const float division = rig.processor->getParameter(W::lfoParam(0, W::Division));
    auto down = rig.at(bpmRate); rig.root->dispatchMouseDown(down);
    auto up = down; up.y -= 30; rig.root->dispatchMouseDrag(up); rig.root->dispatchMouseUp(up);
    CHECK(rig.processor->getParameter(W::lfoParam(0, W::Division)) > division);
    CHECK_NEAR(rig.processor->getParameter(W::lfoParam(0, W::Division)), std::round(rig.processor->getParameter(W::lfoParam(0, W::Division))), 0);
    rig.click(rig.editor->parameterCell(W::lfoParam(0, W::SyncOn)));
    CHECK(rig.processor->getParameter(W::lfoParam(0, W::SyncOn)) < 0.5f);
    CHECK(! rig.editor->parameterCell(W::lfoParam(0, W::Rate)).isEmpty());
    // POLY stepper.
    const auto poly = rig.editor->parameterCell(W::PolyphonyParam); REQUIRE(! poly.isEmpty());
    const float boxRight = poly.centreX() + std::min(poly.w - 4, 44.0f) * 0.5f;
    auto downArrow = rig.at({boxRight - 6, poly.y, 1, poly.h - 12}, 0, 0.65f); rig.root->dispatchMouseDown(downArrow); rig.root->dispatchMouseUp(downArrow);
    CHECK_NEAR(rig.processor->getParameter(W::PolyphonyParam), W::kVoices - 1, 0);
}

TEST_CASE("wavetable noise/LFO type UI: generated colours from the Noise menu; chaos LFOs draw their attractor and lock curve editing")
{
    Rig rig;
    rig.pick = 3; // Pink
    rig.click(rig.editor->noiseAssetCell());
    REQUIRE(rig.menu.size() == 6); CHECK(rig.menu[2] == "White");
    CHECK_NEAR(rig.processor->getParameter(W::NoiseTypeParam), W::NoisePink, 0);
    rig.processor->setParameter(W::lfoTypeParam(0), W::LfoLorenz); rig.root->layout();
    CHECK(rig.editor->parameterCell(W::lfoParam(0, W::Shape)).isEmpty()); // no curve shape for an attractor
    REQUIRE(! rig.editor->parameterCell(W::lfoTypeParam(0)).isEmpty());
    const auto before = rig.processor->lfoCurve(0);
    auto edit = rig.at(rig.editor->lfoPlot(), 0.5f, 0.2f); rig.root->dispatchMouseDown(edit);
    auto moved = edit; moved.y += 30; rig.root->dispatchMouseDrag(moved); rig.root->dispatchMouseUp(moved);
    CHECK(rig.processor->lfoCurve(0).count == before.count);
    rig.processor->setParameter(W::lfoParam(0, W::Mode), W::Global);
    rig.pump(512); rig.pump(512);
    const juce::File directory("/tmp/sonora_tests/sonora_wavetable_drag_tests"); REQUIRE(directory.createDirectory().wasOk());
    REQUIRE(gfx::renderToPng(*rig.root, 1400, 1000, 1, directory.getChildFile("ChaosLfo.png").getFullPathName().toStdString()));
}
