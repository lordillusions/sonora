#include "TestFramework.h"
#include "control/ControlService.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsTemplates.h"

using namespace sonora;
namespace
{
struct Harness
{
    std::unique_ptr<model::Song> song = io::parseAls(*io::createNewSetDom(), "/tmp/control-test.als").song;
    model::EditHistory history;
    std::string document = "doc-1", busy;
    int externalVersion = 0;
    control::Backend backend()
    {
        control::Backend b;
        b.state = [this] { control::State s; s.song = song.get(); s.documentId = document;
            s.revision = std::to_string(history.version()) + ":" + std::to_string(externalVersion); s.busy = busy; return s; };
        b.apply = [this](model::EditCommand c) { history.perform(*song, std::move(c)); return control::ApplyResult::success(); };
        b.undo = [this] { history.undo(*song); return control::ApplyResult::success(); };
        return b;
    }
    juce::var request(control::ControlService& service, const char* operation, juce::var args, const char* key = "test-1")
    {
        return control::object({{"apiVersion", "1.0"}, {"operation", operation}, {"arguments", args},
            {"target", control::object({{"instanceId", service.instanceId()}, {"documentId", document}})},
            {"expectedRevision", std::to_string(history.version()) + ":" + std::to_string(externalVersion)}, {"idempotencyKey", key}});
    }
};
juce::var tempo(double bpm)
{ return control::object({{"commands", juce::Array<juce::var>{control::object({{"type", "tempo.set"}, {"bpm", bpm}})}}}); }
}

TEST_CASE("control: stale document, revision, busy and unknown fields never mutate")
{
    Harness h; control::ControlService s(h.backend());
    auto r = h.request(s, "edits.apply", tempo(128));
    ++h.externalVersion; CHECK(! static_cast<bool>(s.execute(r)["ok"]));
    r = h.request(s, "edits.apply", tempo(128)); h.document = "doc-2";
    CHECK(! static_cast<bool>(s.execute(r)["ok"]));
    r = h.request(s, "edits.apply", tempo(128)); h.busy = "Recording";
    CHECK(! static_cast<bool>(s.execute(r)["ok"])); h.busy.clear();
    r.getDynamicObject()->setProperty("approved", true);
    CHECK(! static_cast<bool>(s.execute(r)["ok"]));
    CHECK_NEAR(h.song->tempo, 120, 1e-9); CHECK(! h.history.canUndo());
}
TEST_CASE("control: retries are idempotent and manual changes protect undo")
{
    Harness h; control::ControlService s(h.backend());
    auto r = h.request(s, "edits.apply", tempo(128));
    auto result = s.execute(r); REQUIRE(static_cast<bool>(result["ok"]));
    CHECK(static_cast<bool>(s.execute(r)["ok"])); CHECK(h.history.version() == 1);
    auto changed = h.request(s, "edits.apply", tempo(130));
    CHECK(! static_cast<bool>(s.execute(changed)["ok"]));
    ++h.externalVersion;
    CHECK(! static_cast<bool>(s.submit("undo")["ok"])); CHECK_NEAR(h.song->tempo, 128, 1e-9);
}
TEST_CASE("control: staged batch failures leave no partial tempo or gain edits")
{
    Harness h; control::ControlService s(h.backend());
    const auto items = juce::Array<juce::var>{control::object({{"type", "tempo.set"}, {"bpm", 128}}),
        control::object({{"type", "track.set_gain"}, {"trackId", h.song->tracks[0].id}, {"gainDb", 100}})};
    CHECK(! static_cast<bool>(s.execute(h.request(s, "edits.apply", control::object({{"commands", items}})))["ok"]));
    CHECK_NEAR(h.song->tempo, 120, 1e-9); CHECK(! h.history.canUndo());
    auto good = s.submit("set tempo to 128"); REQUIRE(static_cast<bool>(good["ok"]));
    CHECK(static_cast<bool>(s.submit("undo")["ok"])); CHECK_NEAR(h.song->tempo, 120, 1e-9);
}
TEST_CASE("control: Serum missing fails without creating a track; unsupported prompts are explicit")
{
    Harness h; control::ControlService s(h.backend()); const auto count = h.song->tracks.size();
    auto result = s.submit("Please make me a supersaw lead chord progression with Serum 2, in D# scale, with 4 bars");
    CHECK(! static_cast<bool>(result["ok"])); CHECK(result["error"]["code"].toString() == "missing_plugin");
    CHECK(h.song->tracks.size() == count); CHECK(! h.history.canUndo());
    CHECK(s.submit("make a violin")["error"]["code"].toString() == "unsupported_prompt");
    CHECK(s.submit("Do not make a Serum 2 supersaw in D# minor with 4 bars")["error"]["code"].toString() == "unsupported_prompt");
}
TEST_CASE("control: gain uses live before-value, propagates independently of clips and rejects automation")
{
    Harness h; auto backend = h.backend();
    auto original = backend.state;
    backend.state = [original] { auto state = original(); state.liveGains = {.5, 1, 1, 1}; return state; };
    control::ControlService s(std::move(backend));
    const auto args = control::object({{"commands", juce::Array<juce::var>{control::object({
        {"type", "track.set_gain"}, {"trackId", h.song->tracks[0].id}, {"gainDb", -12}})}}});
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "edits.apply", args))["ok"]));
    REQUIRE(h.history.nextUndo()); CHECK(h.history.nextUndo()->tracks.empty());
    CHECK(h.history.nextUndo()->gainTracks == std::vector<int>{0});
    REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); CHECK_NEAR(h.song->tracks[0].mixer.volume, .5, 1e-9);
    model::AutomationEnvelope envelope; envelope.targetId = h.song->tracks[0].mixer.volumeTargetId;
    envelope.points = {{0, .5}}; h.song->tracks[0].automation.push_back(envelope);
    const auto rejected = s.execute(h.request(s, "edits.apply", args, "automated"));
    CHECK(rejected["error"]["code"].toString() == "automation_conflict");
}
TEST_CASE("control: Serum factory workflow authors 12 scale notes and survives undo redo")
{
    Harness h; auto backend = h.backend();
    backend.prepareSerum = [](model::Device& device, juce::var& report) {
        device.type = "PluginDevice"; device.displayName = "Serum 2"; report = control::object({{"testFactory", true}});
        return control::ApplyResult::success();
    };
    control::ControlService s(std::move(backend)); const auto count = h.song->tracks.size();
    const auto result = s.submit("make a Serum 2 supersaw in D# minor with 4 bars");
    REQUIRE(static_cast<bool>(result["ok"])); REQUIRE(h.song->tracks.size() == count + 1);
    const auto& track = h.song->tracks.back(); REQUIRE(track.arrangementClips.size() == 1);
    const auto clip = track.arrangementClips[0]; CHECK(clip.notes.size() == 12); CHECK_NEAR(clip.currentEnd, 16, 1e-9);
    CHECK(clip.notes[0].pitch == 63); CHECK(clip.notes[9].pitch == 61); CHECK(clip.notes[10].pitch == 65);
    REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); CHECK(h.song->tracks.size() == count);
    h.history.redo(*h.song); CHECK(h.song->tracks.back().arrangementClips[0].uid == clip.uid);
}

namespace
{
model::Clip controlClip()
{
    model::Clip clip; clip.uid = model::allocateClipUid(); clip.kind = model::Clip::Kind::Midi;
    clip.sourceXml = std::make_shared<const std::string>(io::midiClipTemplate());
    clip.currentEnd = 16; clip.loop = {0, 16, 0, true};
    model::MidiNote note; note.noteId = 17; note.pitch = 63; note.velocity = 88; clip.notes.push_back(note);
    return clip;
}
juce::var newNote(int pitch, double start = 0)
{ return control::object({{"pitch", pitch}, {"startBeat", start}, {"durationBeats", .5}, {"velocity", 97.5}}); }
juce::var noteBatch(juce::var item)
{ return control::object({{"commands", juce::Array<juce::var>{std::move(item)}}}); }
}
TEST_CASE("control: note add update delete replacement, paging and retry share one guarded history")
{
    Harness h; auto clip = controlClip(); h.song->tracks[0].arrangementClips.push_back(clip);
    control::ControlService s(h.backend()); const int track = h.song->tracks[0].id;
    auto add = h.request(s, "edits.apply", noteBatch(control::object({{"type", "notes.add"}, {"trackId", track}, {"clipId", clip.uid},
        {"notes", juce::Array<juce::var>{newNote(70, 1), newNote(66, .5)}}})), "add");
    REQUIRE(static_cast<bool>(s.execute(add)["ok"])); CHECK(static_cast<bool>(s.execute(add)["ok"])); CHECK(h.history.version() == 1);
    auto query = control::object({{"trackId", track}, {"clipId", clip.uid}, {"offset", 1}, {"limit", 1}});
    const auto page = s.execute(h.request(s, "clips.get", query)); REQUIRE(static_cast<bool>(page["ok"]));
    CHECK(static_cast<int>(page["data"]["notes"][0]["noteIndex"]) == 1); CHECK(static_cast<bool>(page["data"]["hasMore"]));
    auto update = noteBatch(control::object({{"type", "notes.update"}, {"trackId", track}, {"clipId", clip.uid},
        {"updates", juce::Array<juce::var>{control::object({{"noteIndex", 0}, {"velocity", 62.25}, {"enabled", false},
            {"probability", .25}, {"releaseVelocity", 23}, {"velocityDeviation", -7}})}}}));
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "edits.apply", update, "update"))["ok"]));
    const auto& changed = h.song->tracks[0].arrangementClips[0].notes[0];
    CHECK(changed.noteId == 17); CHECK(! changed.enabled); CHECK_NEAR(changed.velocity, 62.25, 1e-6); CHECK_NEAR(changed.probability, .25, 1e-6);
    REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); CHECK(h.song->tracks[0].arrangementClips[0].notes[0].enabled);
    auto del = noteBatch(control::object({{"type", "notes.delete"}, {"trackId", track}, {"clipId", clip.uid}, {"noteIndices", juce::Array<juce::var>{0, 2}}}));
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "edits.apply", del, "delete"))["ok"]));
    CHECK(h.song->tracks[0].arrangementClips[0].notes.size() == 1); CHECK(h.song->tracks[0].arrangementClips[0].notes[0].pitch == 66);
    auto clear = noteBatch(control::object({{"type", "notes.replace"}, {"trackId", track}, {"clipId", clip.uid}, {"notes", juce::Array<juce::var>{}}}));
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "edits.apply", clear, "clear"))["ok"]));
    CHECK(h.song->tracks[0].arrangementClips[0].notes.empty()); REQUIRE(static_cast<bool>(s.submit("undo")["ok"]));
    CHECK(h.song->tracks[0].arrangementClips[0].notes.size() == 1);
}
TEST_CASE("control: invalid note batches and stale locations cannot partially commit")
{
    Harness h; auto clip = controlClip(); h.song->tracks[0].arrangementClips.push_back(clip);
    control::ControlService s(h.backend()); const int track = h.song->tracks[0].id;
    const std::vector<juce::var> invalid = {
        control::object({{"type", "notes.add"}, {"trackId", track}, {"clipId", clip.uid}, {"notes", juce::Array<juce::var>{newNote(128)}}}),
        control::object({{"type", "notes.update"}, {"trackId", track}, {"clipId", clip.uid}, {"updates", juce::Array<juce::var>{control::object({{"noteIndex", 0}})}}}),
        control::object({{"type", "notes.delete"}, {"trackId", track}, {"clipId", clip.uid}, {"noteIndices", juce::Array<juce::var>{0, 0}}}),
        control::object({{"type", "notes.delete"}, {"trackId", track}, {"clipId", clip.uid}, {"noteIndices", juce::Array<juce::var>{8}}}),
        control::object({{"type", "notes.replace"}, {"trackId", track}, {"clipId", clip.uid}, {"scene", 0}, {"laneId", 0}, {"notes", juce::Array<juce::var>{}}})
    };
    int key = 0;
    for (const auto& bad : invalid)
    {
        auto batch = control::object({{"commands", juce::Array<juce::var>{control::object({{"type", "tempo.set"}, {"bpm", 150}}), bad}}});
        const auto request = h.request(s, "edits.apply", batch, std::to_string(++key).c_str());
        CHECK(! static_cast<bool>(s.execute(request)["ok"])); CHECK_NEAR(h.song->tempo, 120, 1e-9);
        CHECK(h.song->tracks[0].arrangementClips[0].notes.size() == 1); CHECK(! h.history.canUndo());
    }
    h.song->tracks[0].arrangementClips[0].kind = model::Clip::Kind::Audio;
    auto noMidi = noteBatch(control::object({{"type", "notes.add"}, {"trackId", track}, {"clipId", clip.uid}, {"notes", juce::Array<juce::var>{newNote(60)}}}));
    CHECK(s.execute(h.request(s, "edits.apply", noMidi, "audio"))["error"]["code"].toString() == "unsupported_clip");
}
TEST_CASE("control: Arrangement Session and take MIDI edits save reload and undo after save")
{
    Harness h; auto arrangement = controlClip(), session = controlClip(), take = controlClip();
    h.song->tracks[0].arrangementClips = {arrangement}; h.song->tracks[0].slots[0].clip = session;
    model::TakeLane lane; lane.id = 9; lane.name = "Agent test"; lane.clips = {take};
    h.song->tracks[0].takeLanes = {lane}; h.song->tracks[0].takeLanesNeedPatch = true;
    control::ControlService s(h.backend()); const int track = h.song->tracks[0].id;
    juce::Array<juce::var> edits;
    for (int i = 0; i < 3; ++i)
    {
        auto item = control::object({{"type", "notes.update"}, {"trackId", track}, {"clipId", i == 0 ? arrangement.uid : i == 1 ? session.uid : take.uid},
            {"updates", juce::Array<juce::var>{control::object({{"noteIndex", 0}, {"pitch", 70 + i}, {"velocity", 42.5}, {"durationBeats", 2.25}})}}});
        if (i == 1) item.getDynamicObject()->setProperty("scene", 0);
        if (i == 2) item.getDynamicObject()->setProperty("laneId", 9);
        edits.add(item);
    }
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "edits.apply", control::object({{"commands", edits}})))["ok"]));
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("control-notes-" + juce::Uuid().toString() + ".als");
    const auto source = file.getSiblingFile(file.getFileNameWithoutExtension() + "-source.als"); std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), source, error)); h.song->sourcePath = source.getFullPathName().toStdString();
    const auto saved = io::saveAls(*h.song, file); if (! saved.ok) std::printf("save error: %s\n", saved.error.c_str());
    REQUIRE(saved.ok); auto reload = io::loadAls(file); REQUIRE(reload);
    const auto check = [](const model::Clip& c, int pitch) { REQUIRE(c.notes.size() == 1); CHECK(c.notes[0].pitch == pitch);
        CHECK(c.notes[0].noteId == 17); CHECK_NEAR(c.notes[0].velocity, 42.5, 1e-6); CHECK_NEAR(c.notes[0].duration, 2.25, 1e-6); };
    check(reload.song->tracks[0].arrangementClips[0], 70); REQUIRE(reload.song->tracks[0].slots[0].clip);
    check(*reload.song->tracks[0].slots[0].clip, 71); REQUIRE(reload.song->tracks[0].takeLanes.size() == 1);
    check(reload.song->tracks[0].takeLanes[0].clips[0], 72);
    REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); REQUIRE(io::saveAls(*h.song, file).ok);
    reload = io::loadAls(file); REQUIRE(reload); CHECK(reload.song->tracks[0].arrangementClips[0].notes[0].pitch == 63);
    CHECK(reload.song->tracks[0].slots[0].clip->notes[0].pitch == 63); CHECK(reload.song->tracks[0].takeLanes[0].clips[0].notes[0].pitch == 63);
    h.history.redo(*h.song); REQUIRE(io::saveAls(*h.song, file).ok); reload = io::loadAls(file); REQUIRE(reload);
    check(reload.song->tracks[0].takeLanes[0].clips[0], 72); file.deleteFile(); source.deleteFile();
}
TEST_CASE("control: parameter preparation is guarded and undo captures the live opaque before-state")
{
    Harness h; model::Device device; device.kind = model::DeviceKind::PluginVst3; device.id = 44;
    device.plugin.emplace(); device.plugin->state = {1}; h.song->tracks[0].devices = {device}; h.song->nextDeviceId = 45;
    auto b = h.backend(); bool fail = true; int preparations = 0;
    b.prepareParameters = [&](int, int, const auto&, model::Device& before, model::Device& after, juce::var&) {
        ++preparations; before = device; before.plugin->state = {7}; after = before; after.plugin->state = {9};
        return fail ? control::ApplyResult::failure("plugin_write_failed", "test") : control::ApplyResult::success();
    };
    control::ControlService s(std::move(b));
    auto args = control::object({{"trackId", h.song->tracks[0].id}, {"deviceId", 44}, {"parameters", juce::Array<juce::var>{
        control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .3}})}}});
    CHECK(! static_cast<bool>(s.execute(h.request(s, "devices.parameters.set", args, "failed"))["ok"])); CHECK(! h.history.canUndo());
    fail = false; auto request = h.request(s, "devices.parameters.set", args, "good"); auto result = s.execute(request);
    REQUIRE(static_cast<bool>(result["ok"])); CHECK(static_cast<int>(result["data"]["deviceId"]) == 45);
    CHECK(static_cast<bool>(s.execute(request)["ok"])); CHECK(preparations == 2);
    CHECK(h.song->tracks[0].devices[0].plugin->state == std::vector<uint8_t>{9});
    REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); CHECK(h.song->tracks[0].devices[0].plugin->state == std::vector<uint8_t>{7});
    CHECK(h.song->tracks[0].devices[0].id == 44); h.history.redo(*h.song); CHECK(h.song->tracks[0].devices[0].id == 45);
}
TEST_CASE("control: plugin automation in Session and take envelopes rejects writes before preparation")
{
    Harness h; model::Device device; device.kind = model::DeviceKind::PluginVst3; device.id = 44; device.plugin.emplace();
    model::PluginParam parameter; parameter.parameterId = 0; parameter.name = "Volume"; parameter.automationTargetId = 999;
    device.pluginParams = {parameter}; h.song->tracks[0].devices = {device};
    auto clip = controlClip(); model::AutomationEnvelope env; env.targetId = 999; env.points = {{0, .2}}; clip.envelopes = {env};
    h.song->tracks[0].slots[0].clip = clip;
    control::ControlService s(h.backend());
    auto args = control::object({{"trackId", h.song->tracks[0].id}, {"deviceId", 44}, {"parameters", juce::Array<juce::var>{
        control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .3}})}}});
    CHECK(s.execute(h.request(s, "devices.parameters.set", args, "session"))["error"]["code"].toString() == "automation_conflict");
    h.song->tracks[0].slots[0].clip.reset(); model::TakeLane lane; lane.id = 0; lane.clips = {clip}; h.song->tracks[0].takeLanes = {lane};
    CHECK(s.execute(h.request(s, "devices.parameters.set", args, "take"))["error"]["code"].toString() == "automation_conflict");
    CHECK(! h.history.canUndo());
}

TEST_CASE("control: VST3 manual parameter bases persist alongside opaque state")
{
    Harness h; model::Device d; d.id = 44; d.kind = model::DeviceKind::PluginVst3; d.type = "PluginDevice";
    d.displayName = "State-only test"; d.plugin.emplace(); d.plugin->name = d.displayName; d.plugin->state = {1, 2, 3};
    auto xml = juce::parseXML(juce::String(io::deviceTemplate(d))); REQUIRE(xml);
    auto* list = xml->getChildByName("ParameterList"); if (! list) list = xml->createNewChildElement("ParameterList");
    auto* parameter = list->createNewChildElement("PluginFloatParameter");
    parameter->setAttribute("Id", 0); parameter->createNewChildElement("ParameterId")->setAttribute("Value", 0);
    parameter->createNewChildElement("ParameterName")->setAttribute("Value", "Volume");
    auto* value = parameter->createNewChildElement("ParameterValue"); value->createNewChildElement("Manual")->setAttribute("Value", .35);
    value->createNewChildElement("AutomationTarget")->setAttribute("Id", 999);
    model::PluginParam p; p.parameterId = 0; p.name = "Volume"; p.value = .27; p.valueEdited = true; p.automationTargetId = 999; d.pluginParams = {p};
    // Unsupported imported entries can share the sentinel ID -1; untouched manuals must remain distinct.
    for (int i = 0; i < 2; ++i)
    {
        auto copy = std::make_unique<juce::XmlElement>(*parameter); copy->setAttribute("Id", i + 1);
        copy->getChildByName("ParameterId")->setAttribute("Value", -1);
        copy->getChildByName("ParameterValue")->getChildByName("Manual")->setAttribute("Value", i == 0 ? 1.0 : .1234567687);
        copy->getChildByName("ParameterValue")->getChildByName("AutomationTarget")->setAttribute("Id", 1000 + i);
        list->addChildElement(copy.release());
        model::PluginParam unknown; unknown.parameterId = -1; unknown.value = i == 0 ? 1.0 : .1234567687;
        unknown.automationTargetId = 1000 + i; d.pluginParams.push_back(unknown);
    }
    d.sourceXml = std::make_shared<const std::string>(xml->toString().toStdString()); h.song->tracks[0].devices = {d};
    const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("control-param-" + juce::Uuid().toString() + ".als");
    std::string error; REQUIRE(io::writeAlsDom(*io::createNewSetDom(), source, error)); h.song->sourcePath = source.getFullPathName().toStdString();
    const auto saved = source.getSiblingFile(source.getFileNameWithoutExtension() + "-edited.als");
    REQUIRE(io::saveAls(*h.song, saved).ok); auto loaded = io::loadAls(saved); REQUIRE(loaded);
    REQUIRE(loaded.song->tracks[0].devices.size() == 1); const auto& restored = loaded.song->tracks[0].devices[0];
    REQUIRE(restored.plugin); CHECK(restored.plugin->state == d.plugin->state); REQUIRE(restored.pluginParams.size() == 3);
    CHECK_NEAR(restored.pluginParams[0].value, .27, 1e-9);
    CHECK_NEAR(restored.pluginParams[1].value, 1, 1e-9); CHECK_NEAR(restored.pluginParams[2].value, .1234567687, 1e-9); source.deleteFile(); saved.deleteFile();
}
TEST_CASE("control: opaque editor changes guard VST3 preparation and whole-state undo")
{
    Harness h; model::Device device; device.id = 44; device.kind = model::DeviceKind::PluginVst3;
    device.plugin.emplace(); device.plugin->state = {1}; h.song->tracks[0].devices = {device}; h.song->nextDeviceId = 45;
    std::vector<uint8_t> live = {1}; bool changeDuringPrepare = true;
    auto b = h.backend(); const auto apply = b.apply; const auto undo = b.undo;
    b.apply = [&, apply](model::EditCommand c) { auto result = apply(std::move(c)); live = h.song->tracks[0].devices[0].plugin->state; return result; };
    b.undo = [&, undo] { auto result = undo(); live = h.song->tracks[0].devices[0].plugin->state; return result; };
    b.captureDevice = [&](int, int, model::Device& out) {
        out = h.song->tracks[0].devices[0]; out.plugin->state = live; return control::ApplyResult::success();
    };
    b.prepareParameters = [&](int, int, const auto&, model::Device& before, model::Device& after, juce::var&) {
        before = h.song->tracks[0].devices[0]; before.plugin->state = live; after = before; after.plugin->state = {9};
        if (changeDuringPrepare) live = {8};
        return control::ApplyResult::success();
    };
    control::ControlService s(std::move(b));
    auto args = control::object({{"trackId", h.song->tracks[0].id}, {"deviceId", 44}, {"parameters", juce::Array<juce::var>{
        control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .3}})}}});
    CHECK(s.execute(h.request(s, "devices.parameters.set", args, "changed"))["error"]["code"].toString() == "stale_revision");
    CHECK(! h.history.canUndo()); live = {1}; changeDuringPrepare = false;
    REQUIRE(static_cast<bool>(s.execute(h.request(s, "devices.parameters.set", args, "committed"))["ok"]));
    live = {8}; CHECK(s.submit("undo")["error"]["code"].toString() == "undo_conflict");
    CHECK(h.song->tracks[0].devices[0].plugin->state == std::vector<uint8_t>{9});
    live = {9}; REQUIRE(static_cast<bool>(s.submit("undo")["ok"])); CHECK(live == std::vector<uint8_t>{1});
}
TEST_CASE("control: failed live VST3 readback rolls back the transaction and retains the failure receipt")
{
    Harness h; model::Device device; device.id = 44; device.kind = model::DeviceKind::PluginVst3;
    device.plugin.emplace(); device.plugin->state = {1}; h.song->tracks[0].devices = {device}; h.song->nextDeviceId = 45;
    auto b = h.backend();
    b.captureDevice = [&](int, int, model::Device& out) { out = h.song->tracks[0].devices[0]; return control::ApplyResult::success(); };
    b.prepareParameters = [&](int, int, const auto&, model::Device& before, model::Device& after, juce::var& report) {
        before = device; after = before; after.plugin->state = {9};
        report = control::object({{"parameters", juce::Array<juce::var>{control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .3}})}}});
        return control::ApplyResult::success();
    };
    b.parameters = [](int, int) { return juce::var(juce::Array<juce::var>{control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .8}})}); };
    control::ControlService s(std::move(b));
    auto args = control::object({{"trackId", h.song->tracks[0].id}, {"deviceId", 44}, {"parameters", juce::Array<juce::var>{
        control::object({{"parameterId", "0"}, {"name", "Volume"}, {"normalized", .3}})}}});
    const auto request = h.request(s, "devices.parameters.set", args);
    CHECK(s.execute(request)["error"]["code"].toString() == "plugin_commit_failed");
    CHECK(h.song->tracks[0].devices[0].id == 44); CHECK(h.song->tracks[0].devices[0].plugin->state == std::vector<uint8_t>{1});
    const int version = h.history.version(); CHECK(! static_cast<bool>(s.execute(request)["ok"])); CHECK(h.history.version() == version);
    CHECK(! h.history.isDirty());
}

TEST_CASE("control: edits address stale tracks consistently as target_replaced through the shared op path")
{
    Harness h; control::ControlService s(h.backend());
    const auto missing = [&](juce::var command, const char* key) {
        const auto args = control::object({{"commands", juce::Array<juce::var>{command}}});
        return s.execute(h.request(s, "edits.apply", args, key))["error"]["code"].toString();
    };
    CHECK(missing(control::object({{"type", "track.set_gain"}, {"trackId", 987654}, {"gainDb", -3}}), "gain") == "target_replaced");
    CHECK(missing(control::object({{"type", "notes.delete"}, {"trackId", 987654}, {"clipId", 1}, {"noteIndices", juce::Array<juce::var>{0}}}), "delete") == "target_replaced");
    CHECK(missing(control::object({{"type", "notes.add"}, {"trackId", 987654}, {"clipId", 1},
        {"notes", juce::Array<juce::var>{control::object({{"pitch", 60}, {"startBeat", 0}, {"durationBeats", 1}, {"velocity", 90}})}}}), "add") == "target_replaced");
    CHECK(missing(control::object({{"type", "track.rename"}, {"trackId", h.song->tracks[0].id}, {"name", "x"}}), "rename") == "unsupported_operation");
    CHECK(! h.history.canUndo());
}
