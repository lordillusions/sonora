// UI-65: every plugin parameter is automatable, automated devices/parameters are marked, several lanes per track,
// Show All Automation, and Automation Arm records edits made in a plugin's own window.
#include "TestFramework.h"

#include "core/Edit.h"
#include "core/ModelCodec.h"
#include "devices/DeviceProcessor.h"
#include "engine/Automation.h"
#include "engine/AutomationCapture.h"
#include "engine/EngineGraph.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/AutomationEditor.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <map>

using namespace sonora;

namespace
{
// A hosted plugin as the engine sees one: parameters by index, host ids and edits reported from its window.
class FakePlugin final : public devices::DeviceProcessor
{
public:
    explicit FakePlugin(int count)
    {
        for (int i = 0; i < count; ++i)
        {
            infos_.push_back(devices::makeParam(std::to_string(i), "P" + std::to_string(i), 0, 1, 0.5f));
            values_.push_back(0.5f);
            edits_.push_back({0, 0.5f, 0.5f, false});
        }
    }
    std::string name() const override { return "Fake"; }
    bool isHostedPlugin() const override { return true; }
    void prepare(double, int) override {}
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    const std::vector<devices::ParameterInfo>& parameters() const override { return infos_; }
    float getParameter(int i) const override { return values_[(size_t) i]; }
    void setParameter(int i, float v) override { values_[(size_t) i] = v; }
    int findParameterByHostId(int id, const std::string&) const override { return id >= 1000 ? id - 1000 : -1; }
    bool hostParameter(int i, int& id, std::string& name) const override
    {
        if (i == 3) return false; // not automatable
        id = 1000 + i; name = "Param " + std::to_string(i); return true;
    }
    uint32_t handEditTotal() const override { return total_; }
    HandEdit handEdit(int i) const override { return edits_[(size_t) i]; }
    bool takeHandEdit(int i) override
    {
        const bool edited = edits_[(size_t) i].serial != seen_[i];
        seen_[i] = edits_[(size_t) i].serial;
        return edited;
    }
    void edit(int i, float value, bool gesture = true)
    {
        auto& e = edits_[(size_t) i];
        e.before = e.value; e.value = value; e.gesture = gesture; ++e.serial; ++total_;
        values_[(size_t) i] = value;
    }

private:
    std::vector<devices::ParameterInfo> infos_;
    std::vector<float> values_;
    std::vector<HandEdit> edits_;
    std::map<int, uint32_t> seen_;
    uint32_t total_ = 0;
};

model::Device pluginDevice(int id)
{
    model::Device d;
    d.kind = model::DeviceKind::PluginVst3; d.type = "PluginDevice"; d.displayName = "Synth"; d.id = id;
    d.enabledTargetId = 50 + id; d.plugin.emplace(); d.plugin->name = "Synth";
    return d;
}

struct Rig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    engine::EngineGraph graph;
    FakePlugin plugin{300};
    std::vector<std::string> labels;
    std::vector<bool> ticks;
    std::function<void(int)> choose;
    std::unique_ptr<ui::RootWidget> root;
    ui::AutomationEditor* editor = nullptr;
    Rig()
    {
        song.tracks.resize(1); song.nextPointeeId = 1000;
        auto& track = song.tracks[0]; track.kind = model::TrackKind::Audio;
        track.mixer.volumeTargetId = 100; track.mixer.panTargetId = 101; track.mixer.speakerTargetId = 102;
        auto d = pluginDevice(1);
        // Live's list: one configured slot and two unused ones (ParameterId -1, stale names).
        d.pluginParams = {{1005, "Param 5", 0.25, 200}, {-1, "old name", 0.1, 201}, {-1, "", 0.1, 202}};
        track.devices.push_back(d);
        graph.nodes.resize(1);
        graph.nodes[0].processorForModelDevice = {&plugin};
        graph.controls = std::make_shared<engine::ControlBank>(1);
        ctx.song = &song; ctx.graph = &graph;
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.showChoices = [this](const auto& names, const auto& selected, auto pick) { labels = names; ticks = selected; choose = std::move(pick); };
        root = std::make_unique<ui::RootWidget>(ctx); root->setBounds({0, 0, 900, 70});
        editor = root->add<ui::AutomationEditor>(0, 0); editor->setBounds(root->localBounds());
    }
    void click(float x, float y, bool cmd = false)
    {
        ui::MouseEvent e; e.x = x; e.y = y; e.cmd = cmd; root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
    void chooser(bool device) { click(900 - ctx.arrangementHeaderWidth() + 20, device ? 10.0f : 30.0f); }
    int index(const std::string& label) const
    {
        for (size_t i = 0; i < labels.size(); ++i) if (labels[i] == label) return (int) i;
        return -1;
    }
};
} // namespace

TEST_CASE("automation (UI-65): a loaded plugin offers every automatable parameter, not only the ones the set lists")
{
    Rig rig;
    const auto targets = ui::AutomationEditor::targets(rig.ctx, 0);
    int plugin = 0;
    bool listed = false, stale = false, pending = false;
    for (const auto& t : targets)
    {
        if (t.group != "1 Synth" || t.name == "On") continue;
        ++plugin;
        if (t.name == "Param 5") { listed = t.id == 200; CHECK_NEAR(t.base, 0.25, 1e-9); }
        if (t.name == "old name") stale = true;
        if (t.name == "Param 7") pending = t.id == ui::AutomationEditor::pendingTargetId(0, 7);
    }
    CHECK(plugin == 299); // 300 parameters, one not automatable; the unused slots are not offered
    CHECK(listed); CHECK(! stale); CHECK(pending);

    // The parameter chooser of a big plugin lists them all, then the "automated only" switch.
    rig.chooser(true); REQUIRE(rig.index("1 Synth") >= 0); rig.choose(rig.index("1 Synth"));
    rig.chooser(false);
    CHECK(rig.labels.size() == 1 + 299 + 1); // On, the parameters, the switch
    CHECK(rig.labels.back() == "Show Automated Parameters Only");
}

TEST_CASE("automation (UI-65): the first breakpoint of an unlisted plugin parameter takes a free slot, then a new entry")
{
    Rig rig;
    auto& device = rig.song.tracks[0].devices[0];
    rig.chooser(true); rig.choose(rig.index("1 Synth"));
    rig.chooser(false); REQUIRE(rig.index("Param 7") >= 0); rig.choose(rig.index("Param 7"));
    ui::MouseEvent point; point.x = 160; point.y = 30; point.clicks = 2;
    rig.root->dispatchMouseDown(point); rig.root->dispatchMouseUp(point);
    REQUIRE(rig.song.tracks[0].automation.size() == 1);
    CHECK(rig.song.tracks[0].automation[0].targetId == 201); // Live's first unused slot
    CHECK(device.pluginParams[1].parameterId == 1007);
    CHECK(device.pluginParams[1].name == "Param 7");
    CHECK(device.pluginParams[1].slotEdited);
    CHECK(rig.history.undoLabel() == "Edit Automation");
    CHECK(rig.editor->shownTarget() == 201);
    // Marked in both choosers (Live's LED).
    rig.chooser(false); CHECK(rig.index("\xE2\x97\x8F Param 7") >= 0); CHECK(rig.index("Param 9") >= 0);
    rig.chooser(true); CHECK(rig.index("\xE2\x97\x8F 1 Synth") >= 0); CHECK(rig.index("Mixer") >= 0);
    // Show Automated Parameters Only: the device chooser keeps the automated device (and the shown one).
    rig.choose((int) rig.labels.size() - 1); CHECK(rig.ctx.automationAutomatedOnly);
    rig.chooser(true); CHECK(rig.index("Mixer") < 0); CHECK(rig.index("\xE2\x97\x8F 1 Synth") >= 0);
    rig.chooser(false); CHECK(rig.labels.size() == 2); // Param 7 and the switch
    rig.choose(1); CHECK(! rig.ctx.automationAutomatedOnly);

    // Undo gives the slot back; the ids are never reused.
    rig.history.undo(rig.song);
    CHECK(rig.song.tracks[0].automation.empty());
    CHECK(device.pluginParams[1].parameterId == -1);

    // With no free slot left, a new entry with a fresh target (and the id after it for its ModulationTarget).
    model::EditCommand assign;
    int next = rig.song.nextPointeeId;
    auto ids = model::pluginParameterTargets(rig.song, next, 0, 0, {{1007, "Param 7", 0.5}, {1008, "Param 8", 0.5}, {1009, "Param 9", 0.5}}, assign);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 201); CHECK(ids[1] == 202); CHECK(ids[2] == 1000); CHECK(next == 1002);
    rig.history.perform(rig.song, std::move(assign));
    CHECK(device.pluginParams.size() == 4); CHECK(rig.song.nextPointeeId == 1002);
    ids = model::pluginParameterTargets(rig.song, next, 0, 0, {{1005, "Param 5", 0}}, assign);
    CHECK(ids[0] == 200); CHECK(! assign.apply); // already listed
}

TEST_CASE("automation (UI-65): a parameter Sonora adds to a plugin's list is saved where Live reads it")
{
    // A plugin Sonora inserted has an empty <ParameterList>; Live's sets keep 128 slots. Both must reopen automated.
    juce::TemporaryFile empty(".als"); std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), empty.getFile(), error));
    auto loaded = io::loadAls(empty.getFile()); REQUIRE(loaded);
    auto& s = *loaded.song;
    REQUIRE(! s.tracks.empty());
    auto d = pluginDevice(77); d.plugin->state = {1, 2, 3};
    d.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(d));
    s.tracks[0].devices.push_back(d);
    model::EditHistory history;
    int next = s.nextPointeeId;
    model::EditCommand assign;
    const auto ids = model::pluginParameterTargets(s, next, 0, (int) s.tracks[0].devices.size() - 1, {{123456789, "Cutoff", 0.4}}, assign);
    REQUIRE(ids.size() == 1); REQUIRE(ids[0] >= 0);
    model::AutomationEnvelope env; env.targetId = ids[0]; env.points = {{-63072000.0, 0.4}, {0.0, 0.1}, {4.0, 0.9}};
    history.perform(s, model::compose("Edit Automation", {std::move(assign), model::setAutomation(s, 0, env)}));
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(s, saved.getFile()).ok);
    auto reopened = io::loadAls(saved.getFile()); REQUIRE(reopened);
    const auto& track = reopened.song->tracks[0];
    const auto& device = track.devices.back();
    REQUIRE(device.pluginParams.size() == 1);
    CHECK(device.pluginParams[0].parameterId == 123456789);
    CHECK(device.pluginParams[0].name == "Cutoff");
    CHECK(device.pluginParams[0].automationTargetId == ids[0]);
    CHECK_NEAR(device.pluginParams[0].value, 0.4, 1e-9);
    bool found = false;
    for (const auto& lane : track.automation) if (lane.targetId == ids[0]) found = lane.points.size() == 3;
    CHECK(found);
    CHECK(reopened.song->nextPointeeId >= ids[0] + 2); // the ModulationTarget id is taken too

    // A list like Live's (unused slots with ParameterId -1): the slot keeps its targets and gets the id, name and position.
    {
        auto live = pluginDevice(78); live.plugin->state = {4, 5};
        auto xml = juce::parseXML(juce::String(io::deviceTemplate(live))); REQUIRE(xml);
        auto* list = xml->getChildByName("ParameterList"); if (! list) list = xml->createNewChildElement("ParameterList");
        for (int i = 0; i < 2; ++i)
        {
            auto* entry = list->createNewChildElement("PluginFloatParameter"); entry->setAttribute("Id", i);
            entry->createNewChildElement("ParameterName")->setAttribute("Value", i == 0 ? "Drive" : "stale");
            entry->createNewChildElement("ParameterId")->setAttribute("Value", i == 0 ? 42 : -1);
            entry->createNewChildElement("VisualIndex")->setAttribute("Value", i == 0 ? 0 : 1073741823);
            auto* value = entry->createNewChildElement("ParameterValue");
            value->createNewChildElement("Manual")->setAttribute("Value", 0.5);
            value->createNewChildElement("AutomationTarget")->setAttribute("Id", 900 + i);
            live.pluginParams.push_back({i == 0 ? 42 : -1, i == 0 ? "Drive" : "stale", 0.5, 900 + i});
        }
        live.sourceXml = std::make_shared<const std::string>(xml->toString().toStdString());
        s.tracks[0].devices.push_back(live);
        s.nextPointeeId = std::max(s.nextPointeeId, 902);
        next = s.nextPointeeId;
        const int index = (int) s.tracks[0].devices.size() - 1;
        const auto slot = model::pluginParameterTargets(s, next, 0, index, {{-7, "Tone", 0.3}}, assign); // a VST3 id above INT_MAX
        REQUIRE(slot[0] == 901);
        model::AutomationEnvelope lane; lane.targetId = 901; lane.points = {{-63072000.0, 0.3}, {2.0, 0.7}};
        history.perform(s, model::compose("Edit Automation", {std::move(assign), model::setAutomation(s, 0, lane)}));
        juce::TemporaryFile again(".als");
        REQUIRE(io::saveAls(s, again.getFile()).ok);
        auto back = io::loadAls(again.getFile()); REQUIRE(back);
        const auto& params = back.song->tracks[0].devices.back().pluginParams;
        REQUIRE(params.size() == 2);
        CHECK(params[0].parameterId == 42); CHECK(params[1].parameterId == -7); CHECK(params[1].name == "Tone");
        CHECK(params[1].automationTargetId == 901);
        juce::MemoryBlock packed; REQUIRE(again.getFile().loadFileAsData(packed));
        juce::MemoryInputStream raw(packed, false);
        juce::GZIPDecompressorInputStream unzip(&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
        const auto text = unzip.readEntireStreamAsString();
        REQUIRE(text.length() > 1000);
        CHECK(text.contains("ParameterId Value=\"-7\""));
        CHECK(text.contains("VisualIndex Value=\"1\"")); // after Drive, the one configured parameter
    }

    // The model codec keeps the flag for collaboration snapshots.
    const auto decoded = model::codec::decodeDevices(model::codec::encodeDevices(s.tracks[0].devices, model::codec::Form::Full));
    bool flagged = false;
    for (const auto& candidate : decoded) if (candidate.id == 77) flagged = candidate.pluginParams[0].slotEdited;
    CHECK(flagged);
}

TEST_CASE("automation (UI-65): several lanes per track, + and - buttons, Show All Automation and a control's menu")
{
    Rig rig;
    rig.song.tracks.emplace_back(); // a second track without automation gets no lane
    rig.song.tracks[1].kind = model::TrackKind::Audio; rig.song.tracks[1].mixer.volumeTargetId = 400;
    auto& track = rig.song.tracks[0];
    model::AutomationEnvelope volume; volume.targetId = 100; volume.points = {{-63072000.0, 1.0}, {0, 0.5}, {4, 1.0}};
    model::AutomationEnvelope param; param.targetId = 200; param.points = {{-63072000.0, 0.2}, {0, 0.2}, {8, 0.8}};
    track.automation = {param, volume};
    CHECK((ui::AutomationEditor::automatedTargets(rig.ctx, 0) == std::vector<int>{100, 200})); // chooser order
    CHECK(rig.ctx.automationLaneCount(0) == 1);

    // Shift+A equivalent: every automated target in its own lane.
    ui::AutomationEditor::toggleAllAutomation(rig.ctx);
    CHECK(rig.ctx.showsAutomation(0)); CHECK(! rig.ctx.showsAutomation(1));
    CHECK((rig.ctx.automationLaneTargets[0] == std::vector<int>{100, 200}));
    CHECK_NEAR(rig.ctx.automationHeight(0), 2 * ui::UiContext::automationLaneHeight, 1e-3);
    ui::AutomationEditor::toggleAllAutomation(rig.ctx);
    CHECK(! rig.ctx.automationVisible);

    // The lane's + button adds a lane below (next unshown target), - hides it again.
    rig.ctx.automationLaneTargets.clear();
    rig.ctx.toggleAutomationLane(0);
    rig.root->layout(); rig.editor->setBounds(rig.root->localBounds());
    const float plusX = 900 - rig.ctx.arrangementHeaderWidth() + ui::arrangementTitleWidth(rig.ctx) + 12;
    rig.click(plusX, 12);
    REQUIRE(rig.ctx.automationLaneCount(0) == 2);
    CHECK(rig.ctx.automationLaneTargets[0][0] == 100); // the first lane shows the first automated target
    CHECK(rig.ctx.automationLaneTargets[0][1] == 200);
    auto* second = rig.root->add<ui::AutomationEditor>(0, 1); second->setBounds(rig.root->localBounds());
    ui::MouseEvent minus; minus.x = plusX + 18; minus.y = 12;
    second->mouseDown(minus);
    CHECK(rig.ctx.automationLaneCount(0) == 1);
    second->visible = false; // the view hides editors past the lane count
    rig.click(plusX + 18, 12); // the last lane: hides the track's automation
    CHECK(! rig.ctx.showsAutomation(0));

    // A control's context menu target: plugin parameter 7 by its index, the mixer by kind.
    midi::ControlTarget knob; knob.kind = midi::ControlTarget::Kind::DeviceParameter; knob.track = 0; knob.device = 0; knob.param = "7";
    int where = -1;
    CHECK(ui::AutomationEditor::targetFor(rig.ctx, knob, where) == ui::AutomationEditor::pendingTargetId(0, 7));
    knob.param = "5"; CHECK(ui::AutomationEditor::targetFor(rig.ctx, knob, where) == 200);
    midi::ControlTarget fader; fader.kind = midi::ControlTarget::Kind::TrackVolume; fader.track = 0;
    CHECK(ui::AutomationEditor::targetFor(rig.ctx, fader, where) == 100);
    ui::AutomationEditor::showTarget(rig.ctx, 0, 200, true);
    CHECK(rig.ctx.showsAutomation(0)); CHECK(rig.ctx.automationLaneTargets[0].back() == 200);
    // The controls' LED (Live): the fader and the plugin knob drive automated parameters, pan and parameter 7 do not.
    CHECK(ui::isControlAutomated(rig.ctx, fader));
    CHECK(ui::isControlAutomated(rig.ctx, knob));
    midi::ControlTarget pan = fader; pan.kind = midi::ControlTarget::Kind::TrackPan;
    CHECK(! ui::isControlAutomated(rig.ctx, pan));
    knob.param = "7"; CHECK(! ui::isControlAutomated(rig.ctx, knob)); knob.param = "5";
    auto clear = model::clearAutomation(rig.song, 0, 200); REQUIRE(clear.apply);
    rig.history.perform(rig.song, std::move(clear));
    CHECK(track.automation.size() == 1);
    CHECK(! model::clearAutomation(rig.song, 0, 200).apply);
    CHECK(! ui::isControlAutomated(rig.ctx, knob));
}

TEST_CASE("automation arm (UI-65): edits in a plugin's window are recorded; a preset change is not")
{
    Rig rig;
    rig.song.tracks[0].automation.clear();
    engine::AutomationCapture capture;
    capture.begin();
    capture.poll(rig.song, rig.graph, 0.0); // first look: nothing recorded
    rig.plugin.edit(7, 0.6f);
    capture.poll(rig.song, rig.graph, 1.0);
    rig.plugin.edit(7, 0.9f);
    rig.plugin.edit(5, 0.3f, false);
    capture.poll(rig.song, rig.graph, 2.0);
    for (int i = 10; i < 40; ++i) rig.plugin.edit(i, 0.1f, false); // a preset load: many at once, no gesture
    capture.poll(rig.song, rig.graph, 3.0);
    CHECK(capture.touched());
    auto touched = capture.finish(rig.song, rig.graph, 4.0);
    REQUIRE(touched.size() == 2);
    const engine::AutomationCapture::TouchedEnvelope* p7 = nullptr;
    const engine::AutomationCapture::TouchedEnvelope* p5 = nullptr;
    for (const auto& t : touched) (t.index == 7 ? p7 : p5) = &t;
    REQUIRE(p7); REQUIRE(p5);
    CHECK(p7->hostedPlugin); CHECK(p7->envelope.targetId == -1); CHECK(p7->hostParameterId == 1007);
    CHECK(p7->hostParameterName == "Param 7");
    CHECK(p5->envelope.targetId == 200); // listed already
    CHECK(p7->envelope.points.front().time < 0); CHECK_NEAR(p7->envelope.points.front().value, 0.5, 1e-6);
    CHECK_NEAR(p7->envelope.points.back().time, 4.0, 1e-9); CHECK_NEAR(p7->envelope.points.back().value, 0.9, 1e-6);
    bool sawFirst = false;
    for (const auto& p : p7->envelope.points) if (std::fabs(p.time - 1.0) < 1e-9 && std::fabs(p.value - 0.6) < 1e-6) sawFirst = true;
    CHECK(sawFirst);
}

TEST_CASE("automation override (UI-65): a hand edit in a plugin's window takes the parameter from its lane")
{
    FakePlugin plugin(8);
    engine::TrackControls controls;
    engine::AutomationBinding b;
    b.kind = engine::AutomationBinding::Kind::DeviceParam; b.index = 2; b.device = &plugin; b.hostedPlugin = true; b.deviceSlot = 0;
    model::AutomationEnvelope env; env.targetId = 1; env.points = {{0, 0.0}, {8, 1.0}};
    b.lane = engine::AutomationLane(env);
    const std::vector<engine::AutomationBinding> bindings{b};
    plugin.edit(2, 0.9f); // before the lane wrote anything: not an override
    engine::applyAutomation(bindings, controls, 2.0);
    CHECK_NEAR(plugin.getParameter(2), 0.25, 1e-6);
    const int key = engine::automationOverrideKey(b.kind, b.index, b.deviceSlot);
    CHECK(! engine::isAutomationOverridden(controls, key));
    engine::applyAutomation(bindings, controls, 4.0);
    CHECK_NEAR(plugin.getParameter(2), 0.5, 1e-6);
    plugin.edit(2, 0.1f);
    engine::applyAutomation(bindings, controls, 6.0);
    CHECK(engine::isAutomationOverridden(controls, key));
    CHECK_NEAR(plugin.getParameter(2), 0.1, 1e-6); // left where the hand put it
    controls.automationOverrides[(size_t) key / 64].store(0); controls.automationReEnable.store(true);
    engine::applyAutomation(bindings, controls, 6.0);
    CHECK_NEAR(plugin.getParameter(2), 0.75, 1e-6);
}
