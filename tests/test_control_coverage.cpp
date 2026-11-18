#include "TestFramework.h"
#include "agent/AgentProtocol.h"
#include "control/ControlService.h"
#include "core/Op.h"
#include "io/als/AlsWriter.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include <set>

// AGT-7e: the complete control surface (inspect, structure/song/clip edits, returns and main, live actions, history steps),
// headless against the model only.
using namespace sonora;
namespace
{
using control::object;
using Fields = std::initializer_list<control::Field>;

struct Rig
{
    std::unique_ptr<model::Song> song = io::parseAls(*io::createNewSetDom(), "/tmp/control-coverage.als").song;
    model::EditHistory history;
    std::string document = "doc-cov", busy;
    int externalVersion = 0, syncs = 0, keyCounter = 0;
    std::vector<control::LiveMixer> mixer;
    std::map<std::string, control::LiveHandler> live;
    std::function<juce::var()> status;
    control::Backend backend()
    {
        control::Backend b;
        b.state = [this] {
            control::State s; s.song = song.get(); s.documentId = document; s.busy = busy;
            s.revision = std::to_string(history.version()) + ":" + std::to_string(externalVersion);
            s.liveMixer = mixer; s.dirty = history.isDirty(); s.path = song->sourcePath;
            s.history = {history.canUndo(), history.canRedo(), history.undoLabel(), history.redoLabel()};
            return s;
        };
        b.apply = [this](model::EditCommand c) { history.perform(*song, std::move(c)); return control::ApplyResult::success(); };
        b.undo = [this] { history.undo(*song); return control::ApplyResult::success(); };
        b.syncModel = [this] { ++syncs; };
        b.status = status;
        b.live = live;
        b.historyStep = [this](bool undo, std::string& label) {
            label = undo ? history.undoLabel() : history.redoLabel();
            if (undo) history.undo(*song); else history.redo(*song);
            return control::ApplyResult::success();
        };
        return b;
    }
    juce::var envelope(control::ControlService& s, const char* op, juce::var args, bool guarded)
    {
        auto request = object({{"apiVersion", "1.0"}, {"operation", op}, {"arguments", args},
                               {"target", object({{"instanceId", s.instanceId()}, {"documentId", document}})}});
        if (guarded)
        {
            request.getDynamicObject()->setProperty("expectedRevision", juce::String(std::to_string(history.version()) + ":" + std::to_string(externalVersion)));
            request.getDynamicObject()->setProperty("idempotencyKey", juce::String("k" + std::to_string(++keyCounter)));
        }
        return request;
    }
    juce::var query(control::ControlService& s, const char* op, juce::var args = object({})) { return s.execute(envelope(s, op, args, false)); }
    juce::var mutate(control::ControlService& s, const char* op, juce::var args) { return s.execute(envelope(s, op, args, true)); }
    juce::var edit(control::ControlService& s, std::initializer_list<juce::var> commands)
    {
        juce::Array<juce::var> list;
        for (const auto& c : commands) list.add(c);
        return mutate(s, "edits.apply", object({{"commands", list}}));
    }
    const model::Track* firstKind(model::TrackKind kind) const
    {
        for (const auto& t : song->tracks) if (t.kind == kind) return &t;
        return nullptr;
    }
};
juce::var cmd(const char* type, Fields fields = {})
{
    auto out = object({{"type", type}});
    for (const auto& f : fields) out.getDynamicObject()->setProperty(f.name, f.value);
    return out;
}
juce::var ids(std::initializer_list<int> values)
{
    juce::Array<juce::var> out;
    for (const int v : values) out.add(v);
    return juce::var(out);
}
bool ok(const juce::var& r)
{
    const bool good = static_cast<bool>(r["ok"]);
    if (! good && std::getenv("SONORA_TEST_VERBOSE")) std::printf("  control error: %s / %s\n", r["error"]["code"].toString().toRawUTF8(), r["error"]["message"].toString().toRawUTF8());
    return good;
}
std::string code(const juce::var& r) { return r["error"]["code"].toString().toStdString(); }
model::Clip midiClip(double start = 0, double end = 8)
{
    model::Clip clip; clip.uid = model::allocateClipUid(); clip.kind = model::Clip::Kind::Midi;
    clip.sourceXml = std::make_shared<const std::string>(io::midiClipTemplate());
    clip.currentStart = clip.time = start; clip.currentEnd = end; clip.loop = {0, end - start, 0, true}; clip.name = "c";
    model::MidiNote note; note.pitch = 60; note.duration = 1; clip.notes.push_back(note);
    return clip;
}
} // namespace

TEST_CASE("control coverage: inspect reports song state, mixer, main track, scenes and history")
{
    Rig r; control::ControlService s(r.backend());
    r.song->scenes.resize(2); r.song->scenes[1].name = "Chorus"; r.song->scenes[1].tempo = 140.0;
    r.mixer.resize(r.song->tracks.size() + r.song->returns.size() + 1);
    r.mixer[0].pan = -.5; r.mixer[0].active = false; r.mixer[0].armed = true; r.mixer[0].volume = .25; r.mixer[0].monitoring = 2;
    auto clip = midiClip(); clip.disabled = true; clip.colorIndex = 11;
    auto& track = r.song->tracks[0]; track.arrangementClips.push_back(clip);
    const auto inspected = s.execute(object({{"apiVersion", "1.0"}, {"operation", "project.inspect"}}));
    REQUIRE(ok(inspected)); const auto d = inspected["data"];
    CHECK(static_cast<int>(d["timeSignature"]["numerator"]) == 4); CHECK(static_cast<int>(d["quantization"]) == r.song->globalQuantisation);
    CHECK(! static_cast<bool>(d["loop"]["enabled"])); CHECK(d["punch"].hasProperty("in")); CHECK(d["scale"].hasProperty("root"));
    CHECK(d["scenes"].size() >= 2); CHECK(d["scenes"][1]["name"].toString() == "Chorus"); CHECK_NEAR(static_cast<double>(d["scenes"][1]["tempo"]), 140.0, 1e-9);
    CHECK(! static_cast<bool>(d["dirty"])); CHECK(! static_cast<bool>(d["history"]["canUndo"]));
    const auto t0 = d["tracks"][0];
    CHECK_NEAR(static_cast<double>(t0["mixer"]["pan"]), -.5, 1e-9); CHECK(static_cast<bool>(t0["mixer"]["muted"])); CHECK(static_cast<bool>(t0["mixer"]["armed"]));
    CHECK(t0["monitoring"].toString() == "off"); CHECK(t0.hasProperty("color")); CHECK(t0.hasProperty("takeLanes")); CHECK(t0.hasProperty("frozen"));
    CHECK(t0["clips"][0]["kind"].toString() == "midi"); CHECK(static_cast<bool>(t0["clips"][0]["muted"])); CHECK(static_cast<int>(t0["clips"][0]["color"]) == 11);
    CHECK(d["mainTrack"].hasProperty("trackId")); CHECK(d["mainTrack"]["mixer"].hasProperty("gainLinear")); CHECK(d["mainTrack"]["devices"].isArray());
    REQUIRE(d["returnTracks"].size() >= 1); CHECK(d["returnTracks"][0]["mixer"]["sends"].isArray()); CHECK(d["returnTracks"][0].hasProperty("devices"));
    CHECK(d["tracks"][0]["mixer"]["sends"].size() == static_cast<int>(r.song->returns.size()));
}
TEST_CASE("control coverage: nested rack devices are inspected with enabled, kind and role")
{
    Rig r; control::ControlService s(r.backend());
    model::Device inner; inner.id = 90; inner.kind = model::DeviceKind::Native; inner.type = "StereoGain"; inner.displayName = "Utility"; inner.enabled = false;
    model::Device rack; rack.id = 91; rack.kind = model::DeviceKind::Rack; rack.type = "AudioEffectGroupDevice"; rack.displayName = "Rack";
    rack.chains.resize(1); rack.chains[0].name = "Chain"; rack.chains[0].devices = {inner};
    r.song->tracks[0].devices = {rack};
    const auto d = s.execute(object({{"apiVersion", "1.0"}, {"operation", "project.inspect"}}))["data"]["tracks"][0]["devices"][0];
    CHECK(d["kind"].toString() == "rack"); CHECK(static_cast<bool>(d["enabled"])); REQUIRE(d["chains"].size() == 1);
    const auto child = d["chains"][0]["devices"][0];
    CHECK(static_cast<int>(child["deviceId"]) == 90); CHECK(! static_cast<bool>(child["enabled"])); CHECK(child["kind"].toString() == "native");
    CHECK(child["role"].toString() == "audioEffect");
}
TEST_CASE("control coverage: automation.get pages one lane, main tempo and return lanes are addressable")
{
    Rig r; control::ControlService s(r.backend());
    const int track = r.song->tracks[0].id;
    juce::Array<juce::var> points;
    for (int i = 0; i < 10; ++i) points.add(object({{"beat", i * 4.0}, {"gainDb", -i * 3.0}}));
    REQUIRE(ok(r.edit(s, {cmd("automation.set", {{"trackId", track}, {"target", "volume"}, {"points", points}})})));
    const auto page = r.query(s, "automation.get", object({{"trackId", track}, {"target", "volume"}, {"offset", 4}, {"limit", 3}}));
    REQUIRE(ok(page)); const auto d = page["data"];
    CHECK(static_cast<int>(d["pointCount"]) == 10); CHECK(d["points"].size() == 3); CHECK_NEAR(static_cast<double>(d["points"][0]["beat"]), 16.0, 1e-9);
    CHECK(static_cast<bool>(d["hasMore"])); CHECK(d["target"].toString() == "volume");
    CHECK(! ok(r.query(s, "automation.get", object({{"trackId", track}, {"target", "volume"}, {"bogus", 1}}))));
    CHECK(code(r.query(s, "automation.get", object({{"trackId", 99999}, {"target", "volume"}}))) == "target_replaced");
    // Main track: tempo lane. Return track: its own volume.
    const int main = model::mainTrackId(*r.song), ret = r.song->returns[0].id;
    REQUIRE(r.song->main.mixer.tempoTargetId >= 0);
    REQUIRE(ok(r.edit(s, {cmd("automation.set", {{"trackId", main}, {"target", "tempo"}, {"points", juce::Array<juce::var>{object({{"beat", 0}, {"value", 100}}), object({{"beat", 32}, {"value", 140}})}}}),
                          cmd("automation.set", {{"trackId", ret}, {"target", "volume"}, {"points", juce::Array<juce::var>{object({{"beat", 0}, {"gainDb", -6}})}}})})));
    const auto tempo = r.query(s, "automation.get", object({{"trackId", main}, {"target", "tempo"}}));
    REQUIRE(ok(tempo)); CHECK(static_cast<int>(tempo["data"]["pointCount"]) == 2); CHECK(tempo["data"]["target"].toString() == "tempo");
    CHECK(static_cast<bool>(r.query(s, "automation.get", object({{"trackId", ret}, {"target", "volume"}}))["data"]["exists"]));
    REQUIRE(r.history.canUndo()); r.history.undo(*r.song);
    for (const auto& lane : r.song->main.automation) CHECK(lane.targetId != r.song->main.mixer.tempoTargetId || lane.points.size() < 2); // the batch is undone
    CHECK(code(r.edit(s, {cmd("automation.set", {{"trackId", track}, {"target", "tempo"}, {"points", juce::Array<juce::var>{object({{"beat", 0}, {"value", 100}})}}})})) == "invalid_request");
}
TEST_CASE("control coverage: status.get combines app status with history labels")
{
    Rig r; r.status = [] { return object({{"transport", object({{"playing", true}, {"positionBeats", 12.5}})}}); };
    control::ControlService s(r.backend());
    REQUIRE(ok(r.edit(s, {cmd("tempo.set", {{"bpm", 130}})})));
    const auto d = r.query(s, "status.get")["data"];
    CHECK(static_cast<bool>(d["transport"]["playing"])); CHECK(static_cast<bool>(d["liveStatus"])); CHECK(static_cast<bool>(d["history"]["canUndo"]));
    CHECK(d["history"]["undoLabel"].toString() == "Agent edit"); CHECK(static_cast<bool>(d["dirty"]));
    Rig bare; control::ControlService headless(bare.backend());
    CHECK(! static_cast<bool>(bare.query(headless, "status.get")["data"]["liveStatus"]));
}
TEST_CASE("control coverage: capabilities.get is generated from the tables the dispatcher uses")
{
    Rig r; r.live["mixer.set"] = [](const juce::var&, juce::var&) { return control::ApplyResult::success(); };
    control::ControlService s(r.backend());
    const auto caps = s.execute(object({{"apiVersion", "1.0"}, {"operation", "capabilities.get"}}))["data"];
    const auto has = [](const juce::var& list, const char* name) {
        for (int i = 0; i < list.size(); ++i) if (list[i].toString() == name) return true;
        return false;
    };
    for (const char* op : {"project.inspect", "automation.get", "status.get", "history.step", "devices.move", "edits.apply"}) CHECK(has(caps["operations"], op));
    CHECK(has(caps["operations"], "mixer.set")); CHECK(has(caps["liveActions"], "mixer.set")); CHECK(! has(caps["operations"], "transport.set")); // no handler
    CHECK(! has(caps["operations"], "clips.import_audio")); // no importAudio hook
    // Every advertised edit type is routed by the dispatcher (a bare command fails validation, never "unsupported").
    for (int i = 0; i < caps["editTypes"].size(); ++i)
    {
        const auto type = caps["editTypes"][i].toString();
        const auto result = r.edit(s, {object({{"type", type}})});
        CHECK(code(result) != "unsupported_operation");
    }
    CHECK(code(r.edit(s, {cmd("nonsense.type")})) == "unsupported_operation");
    for (int i = 0; i < caps["operations"].size(); ++i)
        if (caps["operations"][i].toString() != "project.inspect" && caps["operations"][i].toString() != "capabilities.get")
            CHECK(code(r.query(s, caps["operations"][i].toString().toRawUTF8(), object({{"zzz", 1}}))) != "unsupported_operation");
}
TEST_CASE("control coverage: song edits apply, undo and treat an unchanged value as a no-op")
{
    Rig r; control::ControlService s(r.backend());
    REQUIRE(ok(r.edit(s, {cmd("time_signature.set", {{"numerator", 7}, {"denominator", 8}}), cmd("quantization.set", {{"value", 7}}),
                          cmd("scale.set", {{"root", 3}, {"name", 1}}), cmd("punch.set", {{"in", true}, {"out", true}}),
                          cmd("loop.set", {{"on", true}, {"startBeat", 8}, {"lengthBeats", 16}})})));
    CHECK(r.song->timeSigNumerator == 7); CHECK(r.song->timeSigDenominator == 8); CHECK(r.song->globalQuantisation == 7);
    CHECK(r.song->scaleRoot == 3); CHECK(r.song->scaleName == 1); CHECK(r.song->punchIn); CHECK(r.song->punchOut);
    CHECK(r.song->loopOn); CHECK_NEAR(r.song->loopStart, 8, 1e-9); CHECK_NEAR(r.song->loopLength, 16, 1e-9);
    r.history.undo(*r.song);
    CHECK(r.song->timeSigNumerator == 4); CHECK(r.song->globalQuantisation == 4); CHECK(r.song->scaleRoot == 0); CHECK(! r.song->punchIn); CHECK(! r.song->loopOn);
    REQUIRE(ok(r.edit(s, {cmd("quantization.set", {{"value", 4}})})));
    CHECK(code(r.edit(s, {cmd("time_signature.set", {{"numerator", 4}, {"denominator", 3}})})) == "invalid_request");
    CHECK(! ok(r.edit(s, {cmd("scale.set", {{"root", 12}, {"name", 0}})})));
    CHECK(! ok(r.edit(s, {cmd("quantization.set", {{"value", 14}})})));
    CHECK(! ok(r.edit(s, {cmd("punch.set")})));
    CHECK(! ok(r.edit(s, {cmd("quantization.set", {{"value", 3}, {"extra", 1}})})));
}
TEST_CASE("control coverage: tracks are created, grouped, moved, duplicated and deleted by ID with undo")
{
    Rig r; control::ControlService s(r.backend());
    const auto before = r.song->tracks.size(); const int first = r.song->tracks[0].id;
    auto created = r.edit(s, {cmd("track.create", {{"kind", "midi"}, {"name", "Lead"}, {"color", 9}, {"afterTrackId", first}}),
                              cmd("track.create", {{"kind", "audio"}, {"name", "Vox"}}), cmd("track.create", {{"kind", "return"}, {"name", "Hall"}})});
    REQUIRE(ok(created)); const auto tracks = created["data"]["tracks"];
    REQUIRE(tracks.size() == 3); CHECK(tracks[0]["kind"].toString() == "midi"); CHECK(tracks[1]["kind"].toString() == "audio"); CHECK(tracks[2]["kind"].toString() == "return");
    CHECK(r.song->tracks.size() == before + 2); CHECK(r.song->tracks[1].name == "Lead"); CHECK(r.song->tracks[1].colorIndex == 9);
    CHECK(r.song->tracks.back().name == "Vox"); CHECK(r.song->returns.back().name == "Hall");
    CHECK(r.song->tracks[1].id == static_cast<int>(tracks[0]["trackId"])); CHECK(r.song->returns.back().id == static_cast<int>(tracks[2]["trackId"]));
    CHECK(r.syncs == 0); // plain creation needs no live sync
    const int leadId = r.song->tracks[1].id, voxId = r.song->tracks.back().id;
    // A later command still finds its track by ID after the earlier one inserted a track in front.
    REQUIRE(ok(r.edit(s, {cmd("track.create", {{"kind", "audio"}, {"afterTrackId", first}}), cmd("track.set_hidden", {{"trackIds", ids({voxId})}, {"hidden", true}}),
                          cmd("track.rename", {{"trackId", leadId}, {"name", "Lead 2"}})})));
    for (const auto& t : r.song->tracks) CHECK(t.hidden == (t.id == voxId));
    CHECK(r.song->tracks[2].name == "Lead 2");
    r.history.undo(*r.song); r.history.undo(*r.song); r.history.undo(*r.song); CHECK(r.song->tracks.size() == before);
    // Group, ungroup, move, duplicate, delete.
    const int a = r.song->tracks[0].id, b = r.song->tracks[1].id;
    auto grouped = r.edit(s, {cmd("track.group", {{"trackIds", ids({a, b})}, {"name", "Pair"}})});
    REQUIRE(ok(grouped)); CHECK(r.syncs == 1); const int groupId = grouped["data"]["tracks"][0]["trackId"];
    const auto group = std::find_if(r.song->tracks.begin(), r.song->tracks.end(), [groupId](const auto& t) { return t.id == groupId; });
    REQUIRE(group != r.song->tracks.end()); CHECK(group->kind == model::TrackKind::Group); CHECK(group->name == "Pair");
    for (const auto& t : r.song->tracks) if (t.id == a || t.id == b) CHECK(t.groupId == groupId);
    REQUIRE(ok(r.edit(s, {cmd("track.ungroup", {{"trackId", groupId}})}))); for (const auto& t : r.song->tracks) CHECK(t.groupId == -1);
    REQUIRE(ok(r.edit(s, {cmd("track.move", {{"trackIds", ids({a})}})}))); CHECK(r.song->tracks.back().id == a);
    // Duplication clones the track XML from the file the set came from: give the model a real source.
    const auto source = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("control-dup-" + juce::Uuid().toString() + ".als");
    std::string writeError; REQUIRE(io::writeAlsDom(*io::createNewSetDom(), source, writeError)); r.song->sourcePath = source.getFullPathName().toStdString();
    auto dup = r.edit(s, {cmd("track.duplicate", {{"trackIds", ids({b})}})});
    REQUIRE(ok(dup)); CHECK(dup["data"]["tracks"].size() == 1); CHECK(r.song->tracks.size() == before + 1);
    const int copyId = dup["data"]["tracks"][0]["trackId"];
    auto del = r.edit(s, {cmd("track.delete", {{"trackIds", ids({copyId})}})});
    REQUIRE(ok(del)); CHECK(r.song->tracks.size() == before); CHECK(del["data"]["deletedTrackIds"].size() == 1);
    r.history.undo(*r.song); CHECK(r.song->tracks.size() == before + 1);
    CHECK(code(r.edit(s, {cmd("track.delete", {{"trackIds", ids({model::mainTrackId(*r.song)})}})})) == "invalid_request");
    CHECK(code(r.edit(s, {cmd("track.delete", {{"trackIds", ids({987654})}})})) == "target_replaced");
    CHECK(code(r.edit(s, {cmd("track.ungroup", {{"trackId", a}})})) == "invalid_request");
    source.deleteFile();
}
TEST_CASE("control coverage: return tracks stop at twelve and a failing later command commits nothing")
{
    Rig r; control::ControlService s(r.backend());
    while (r.song->returns.size() < model::maxReturnTracks)
        REQUIRE(ok(r.edit(s, {cmd("track.create", {{"kind", "return"}})})));
    CHECK(code(r.edit(s, {cmd("track.create", {{"kind", "return"}})})) == "capacity");
    const auto tracks = r.song->tracks.size(); const auto versions = r.history.version();
    CHECK(! ok(r.edit(s, {cmd("track.create", {{"kind", "midi"}}), cmd("track.rename", {{"trackId", 424242}, {"name", "x"}})})));
    CHECK(r.song->tracks.size() == tracks); CHECK(r.history.version() == versions);
}
TEST_CASE("control coverage: track flags and monitoring")
{
    Rig r; control::ControlService s(r.backend());
    const int id = r.song->tracks[0].id, ret = r.song->returns[0].id;
    REQUIRE(ok(r.edit(s, {cmd("track.set_deactivated", {{"trackIds", ids({id, ret})}, {"deactivated", true}}), cmd("track.set_monitoring", {{"trackId", id}, {"mode", "off"}}),
                          cmd("track.set_color", {{"trackIds", ids({id})}, {"color", 20}})})));
    CHECK(r.song->tracks[0].deactivated); CHECK(r.song->returns[0].deactivated); CHECK(r.song->tracks[0].monitoring == 2); CHECK(r.song->tracks[0].colorIndex == 20);
    r.history.undo(*r.song); CHECK(! r.song->tracks[0].deactivated); CHECK(r.song->tracks[0].monitoring != 2);
    CHECK(! ok(r.edit(s, {cmd("track.set_monitoring", {{"trackId", id}, {"mode", "loud"}})})));
    CHECK(! ok(r.edit(s, {cmd("track.set_monitoring", {{"trackId", ret}, {"mode", "in"}})})));
    REQUIRE(ok(r.edit(s, {cmd("track.set_hidden", {{"trackIds", ids({id})}, {"hidden", false}})}))); // already visible: no-op success
}
TEST_CASE("control coverage: clip.update trims, mutes, recolours and rejects audio-only fields on MIDI")
{
    Rig r; control::ControlService s(r.backend());
    auto& track = r.song->tracks[0]; auto clip = midiClip(0, 8); track.arrangementClips = {clip, midiClip(16, 24)};
    auto session = midiClip(0, 4); session.name = "s"; track.slots[0].clip = session;
    const int id = track.id;
    REQUIRE(ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}, {"name", "Hook"}, {"color", 14}, {"muted", true}, {"endBeat", 6},
                                              {"loop", object({{"enabled", false}})}})})));
    const auto& changed = r.song->tracks[0].arrangementClips[0];
    CHECK(changed.name == "Hook"); CHECK(changed.colorIndex == 14); CHECK(changed.disabled); CHECK_NEAR(changed.currentEnd, 6, 1e-9); CHECK(! changed.loop.enabled);
    r.history.undo(*r.song); CHECK(r.song->tracks[0].arrangementClips[0].name == "c"); CHECK_NEAR(r.song->tracks[0].arrangementClips[0].currentEnd, 8, 1e-9);
    CHECK(code(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}, {"endBeat", 20}})})) == "region_occupied");
    CHECK(code(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}, {"gainDb", -3}})})) == "unsupported_clip");
    CHECK(! ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}})})));
    CHECK(! ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}, {"launchMode", "gate"}})}))); // Arrangement clip
    REQUIRE(ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", session.uid}, {"scene", 0}, {"launchMode", "toggle"}, {"launchQuantization", 5}, {"legato", true}})})));
    CHECK(r.song->tracks[0].slots[0].clip->launchMode == 2); CHECK(r.song->tracks[0].slots[0].clip->launchQuantisation == 5); CHECK(r.song->tracks[0].slots[0].clip->legato);
    CHECK(code(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", 777777}, {"name", "x"}})})) == "target_replaced");
    CHECK(! ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", clip.uid}, {"color", 70}})})));
    model::Clip audio = midiClip(40, 44); audio.kind = model::Clip::Kind::Audio; r.song->tracks[0].arrangementClips.push_back(audio);
    REQUIRE(ok(r.edit(s, {cmd("clip.update", {{"trackId", id}, {"clipId", audio.uid}, {"gainDb", -6}, {"pitchCoarse", 2}})})));
    CHECK_NEAR(r.song->tracks[0].arrangementClips.back().gain, std::pow(10.0, -6.0 / 20.0), 1e-9); CHECK(r.song->tracks[0].arrangementClips.back().pitchCoarse == 2);
}
TEST_CASE("control coverage: split, regions, linked duplicates and scene copies are undoable")
{
    Rig r; control::ControlService s(r.backend());
    auto& track = r.song->tracks[0]; auto clip = midiClip(0, 8); track.arrangementClips = {clip}; const int id = track.id;
    REQUIRE(ok(r.edit(s, {cmd("clip.split", {{"trackIds", ids({id})}, {"beat", 4}})}))); CHECK(r.song->tracks[0].arrangementClips.size() == 2);
    r.history.undo(*r.song); CHECK(r.song->tracks[0].arrangementClips.size() == 1);
    CHECK(code(r.edit(s, {cmd("clip.split", {{"trackIds", ids({id})}, {"beat", 50}})})) == "invalid_request");
    REQUIRE(ok(r.edit(s, {cmd("region.duplicate", {{"firstTrackId", id}, {"lastTrackId", id}, {"startBeat", 0}, {"endBeat", 8}})})));
    CHECK(r.song->tracks[0].arrangementClips.size() == 2); CHECK_NEAR(r.song->tracks[0].arrangementClips.back().currentStart, 8, 1e-9);
    REQUIRE(ok(r.edit(s, {cmd("region.delete", {{"firstTrackId", id}, {"lastTrackId", id}, {"startBeat", 8}, {"endBeat", 16}})}))); CHECK(r.song->tracks[0].arrangementClips.size() == 1);
    REQUIRE(ok(r.edit(s, {cmd("clip.duplicate_linked", {{"trackId", id}, {"clipId", clip.uid}})})));
    CHECK(r.song->tracks[0].arrangementClips.size() == 2); const int group = r.song->tracks[0].arrangementClips[0].linkGroup; CHECK(group >= 0);
    REQUIRE(ok(r.edit(s, {cmd("clip.unlink", {{"trackId", id}, {"clipId", clip.uid}})}))); CHECK(r.song->tracks[0].arrangementClips[0].linkGroup < 0);
    CHECK(code(r.edit(s, {cmd("clip.unlink", {{"trackId", id}, {"clipId", clip.uid}})})) == "invalid_request");
    REQUIRE(ok(r.edit(s, {cmd("clip.copy_to_scene", {{"trackId", id}, {"clipId", clip.uid}, {"toScene", 2}})})));
    REQUIRE(r.song->tracks[0].slots.size() > 2); REQUIRE(r.song->tracks[0].slots[2].clip); CHECK(r.song->tracks[0].slots[2].clip->notes.size() == 1);
    CHECK(code(r.edit(s, {cmd("clip.copy_to_scene", {{"trackId", id}, {"clipId", clip.uid}, {"toScene", 2}})})) == "region_occupied");
    const int audio = [&] { for (const auto& t : r.song->tracks) if (t.kind == model::TrackKind::Audio) return t.id; return -1; }();
    CHECK(code(r.edit(s, {cmd("clip.copy_to_scene", {{"trackId", id}, {"clipId", clip.uid}, {"toScene", 3}, {"toTrackId", audio}})})) == "invalid_request");
    r.history.undo(*r.song); CHECK(! r.song->tracks[0].slots[2].clip);
}
TEST_CASE("control coverage: take lanes rename, reorder, duplicate, audition, comp and delete")
{
    Rig r; control::ControlService s(r.backend());
    auto& track = r.song->tracks[0]; track.arrangementClips = {midiClip(0, 8)}; const int id = track.id;
    model::TakeLane one, two; one.id = 3; one.name = "one"; one.clips = {midiClip(0, 8)}; two.id = 4; two.name = "two"; two.clips = {midiClip(0, 8)};
    track.takeLanes = {one, two}; track.nextTakeLaneId = 5; track.takeLanesNeedPatch = true;
    REQUIRE(ok(r.edit(s, {cmd("take_lane.rename", {{"trackId", id}, {"laneId", 3}, {"name", "verse"}})}))); CHECK(r.song->tracks[0].takeLanes[0].name == "verse");
    REQUIRE(ok(r.edit(s, {cmd("take_lane.move", {{"trackId", id}, {"laneId", 3}, {"delta", 1}})}))); CHECK(r.song->tracks[0].takeLanes[1].id == 3);
    REQUIRE(ok(r.edit(s, {cmd("take_lane.duplicate", {{"trackId", id}, {"laneId", 4}})}))); CHECK(r.song->tracks[0].takeLanes.size() == 3);
    REQUIRE(ok(r.edit(s, {cmd("take_lane.audition", {{"trackId", id}, {"laneId", 4}})})));
    REQUIRE(ok(r.edit(s, {cmd("take_lane.audition", {{"trackId", id}})})));
    REQUIRE(ok(r.edit(s, {cmd("take_lane.comp", {{"trackId", id}, {"laneId", 3}, {"startBeat", 0}, {"endBeat", 4}})})));
    REQUIRE(ok(r.edit(s, {cmd("take_lane.delete", {{"trackId", id}, {"laneId", 3}})}))); for (const auto& lane : r.song->tracks[0].takeLanes) CHECK(lane.id != 3);
    CHECK(code(r.edit(s, {cmd("take_lane.delete", {{"trackId", id}, {"laneId", 3}})})) == "target_replaced");
    CHECK(! ok(r.edit(s, {cmd("take_lane.move", {{"trackId", id}, {"laneId", 4}, {"delta", 0}})})));
}
TEST_CASE("control coverage: devices insert on returns and main, replace an instrument, move and reorder")
{
    Rig r; auto b = r.backend(); int prepared = 0;
    b.prepareDevice = [&](const std::string& id, model::Device& device, juce::var& report) {
        ++prepared; device = model::Device{}; device.kind = model::DeviceKind::Native;
        device.type = id == "native:StereoGain" ? "StereoGain" : id == "native:Eq8" ? "Eq8" : "OriginalSimpler"; device.displayName = device.type;
        report = object({{"kind", "native"}}); return control::ApplyResult::success();
    };
    control::ControlService s(std::move(b));
    const int ret = r.song->returns[0].id, main = model::mainTrackId(*r.song);
    auto inserted = r.mutate(s, "devices.insert", object({{"trackId", ret}, {"catalogId", "native:StereoGain"}}));
    REQUIRE(ok(inserted)); CHECK(r.song->returns[0].devices.size() == 1);
    REQUIRE(ok(r.mutate(s, "devices.insert", object({{"trackId", main}, {"catalogId", "native:Eq8"}})))); CHECK(r.song->main.devices.size() == 1);
    CHECK(code(r.mutate(s, "devices.insert", object({{"trackId", main}, {"catalogId", "native:Simpler"}}))) == "incompatible_track");
    // Replace an instrument on a MIDI track.
    const auto* midi = r.firstKind(model::TrackKind::Midi); REQUIRE(midi != nullptr); const int midiId = midi->id;
    REQUIRE(ok(r.mutate(s, "devices.insert", object({{"trackId", midiId}, {"catalogId", "native:Simpler"}}))));
    CHECK(code(r.mutate(s, "devices.insert", object({{"trackId", midiId}, {"catalogId", "native:Simpler"}}))) == "instrument_conflict");
    const auto replaced = r.mutate(s, "devices.insert", object({{"trackId", midiId}, {"catalogId", "native:Simpler"}, {"replaceInstrument", true}}));
    REQUIRE(ok(replaced)); CHECK(static_cast<bool>(replaced["data"]["replacedInstrument"]));
    size_t instruments = 0;
    for (const auto& t : r.song->tracks) if (t.id == midiId) for (const auto& d : t.devices) instruments += model::deviceRole(d) == model::DeviceRole::Instrument;
    CHECK(instruments == 1); CHECK(r.syncs >= 1);
    r.history.undo(*r.song); // one undo step restores the single original instrument
    instruments = 0; for (const auto& t : r.song->tracks) if (t.id == midiId) for (const auto& d : t.devices) instruments += model::deviceRole(d) == model::DeviceRole::Instrument;
    CHECK(instruments == 1);
    // Reorder two audio effects on the return, then move one to the main track.
    REQUIRE(ok(r.mutate(s, "devices.insert", object({{"trackId", ret}, {"catalogId", "native:Eq8"}})))); REQUIRE(r.song->returns[0].devices.size() == 2);
    const int first = r.song->returns[0].devices[0].id;
    REQUIRE(ok(r.mutate(s, "devices.move", object({{"trackId", ret}, {"deviceId", first}, {"position", 1}})))); CHECK(r.song->returns[0].devices[1].id == first);
    const auto moved = r.mutate(s, "devices.move", object({{"trackId", ret}, {"deviceId", first}, {"toTrackId", main}}));
    REQUIRE(ok(moved)); CHECK(r.song->returns[0].devices.size() == 1); CHECK(r.song->main.devices.size() == 2);
    CHECK(static_cast<int>(moved["data"]["deviceId"]) == r.song->main.devices.back().id || static_cast<int>(moved["data"]["deviceId"]) == r.song->main.devices.front().id);
    CHECK(! ok(r.mutate(s, "devices.move", object({{"trackId", ret}, {"deviceId", first}}))));
    CHECK(code(r.mutate(s, "devices.move", object({{"trackId", ret}, {"deviceId", 424242}, {"position", 0}}))) == "target_replaced");
}
TEST_CASE("control coverage: history.step undoes only the labelled step")
{
    Rig r; control::ControlService s(r.backend());
    REQUIRE(ok(r.edit(s, {cmd("tempo.set", {{"bpm", 99}})})));
    CHECK(code(r.mutate(s, "history.step", object({{"direction", "undo"}, {"expectedLabel", "Something else"}}))) == "undo_conflict");
    CHECK_NEAR(r.song->tempo, 99, 1e-9);
    REQUIRE(ok(r.mutate(s, "history.step", object({{"direction", "undo"}, {"expectedLabel", "Agent edit"}})))); CHECK_NEAR(r.song->tempo, 120, 1e-9);
    CHECK(code(r.mutate(s, "history.step", object({{"direction", "undo"}, {"expectedLabel", "Agent edit"}}))) == "undo_conflict");
    REQUIRE(ok(r.mutate(s, "history.step", object({{"direction", "redo"}, {"expectedLabel", "Agent edit"}})))); CHECK_NEAR(r.song->tempo, 99, 1e-9);
    CHECK(! ok(r.mutate(s, "history.step", object({{"direction", "sideways"}, {"expectedLabel", "x"}}))));
}
TEST_CASE("control coverage: live actions validate against the model and reach only their handler")
{
    Rig r; juce::var seen; int calls = 0;
    for (const char* name : {"mixer.set", "transport.set", "session.launch", "recording.set", "project.save", "plugins.scan", "devices.set_enabled"})
        r.live[name] = [&seen, &calls](const juce::var& args, juce::var& receipt) { seen = args; ++calls; receipt = object({{"previous", 1}}); return control::ApplyResult::success(); };
    control::ControlService s(r.backend());
    const int track = r.song->tracks[0].id, ret = r.song->returns[0].id;
    const int before = r.history.version();
    auto done = r.query(s, "mixer.set", object({{"trackId", track}, {"gainDb", -6}, {"pan", .25}, {"muted", true}, {"solo", false}, {"armed", true},
        {"sends", juce::Array<juce::var>{object({{"sendIndex", 0}, {"gainDb", -12}})}}}));
    REQUIRE(ok(done)); CHECK(done["status"].toString() == "applied"); CHECK(! static_cast<bool>(done["data"]["undoable"])); CHECK(static_cast<int>(seen["trackIndex"]) == 0);
    CHECK(r.history.version() == before); CHECK(calls == 1);
    REQUIRE(ok(r.query(s, "mixer.set", object({{"trackId", ret}, {"gainDb", -3}})))); CHECK(static_cast<int>(seen["trackIndex"]) == static_cast<int>(r.song->tracks.size()));
    REQUIRE(ok(r.query(s, "mixer.set", object({{"trackId", model::mainTrackId(*r.song)}, {"pan", 0}})))); CHECK(static_cast<int>(seen["trackIndex"]) == static_cast<int>(r.song->tracks.size() + r.song->returns.size()));
    const int ok1 = calls;
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", track}})))); // nothing to set
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", track}, {"pan", 2}}))));
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", track}, {"gainDb", 12}}))));
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", track}, {"bogus", 1}}))));
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", model::mainTrackId(*r.song)}, {"solo", true}}))));
    CHECK(! ok(r.query(s, "mixer.set", object({{"trackId", ret}, {"armed", true}}))));
    CHECK(code(r.query(s, "mixer.set", object({{"trackId", 31337}, {"pan", 0}}))) == "target_replaced");
    CHECK(code(r.query(s, "mixer.set", object({{"trackId", track}, {"sends", juce::Array<juce::var>{object({{"sendIndex", 11}, {"gainDb", 0}})}}}))) == "invalid_request");
    CHECK(calls == ok1);
    model::AutomationEnvelope envelope; envelope.targetId = r.song->tracks[0].mixer.panTargetId; envelope.points = {{0, .5}}; r.song->tracks[0].automation.push_back(envelope);
    CHECK(code(r.query(s, "mixer.set", object({{"trackId", track}, {"pan", 0}}))) == "automation_conflict");
    CHECK(ok(r.query(s, "mixer.set", object({{"trackId", track}, {"gainDb", -1}}))));
    // Live actions never take the mutation envelope.
    auto guarded = r.envelope(s, "mixer.set", object({{"trackId", track}, {"gainDb", -1}}), true);
    CHECK(! ok(s.execute(guarded)));
    CHECK(! ok(r.query(s, "transport.set", object({{"action", "rewind"}})))); CHECK(! ok(r.query(s, "transport.set")));
    REQUIRE(ok(r.query(s, "transport.set", object({{"action", "play"}, {"positionBeats", 8}, {"metronome", true}}))));
    CHECK(! ok(r.query(s, "session.launch", object({{"action", "clip"}, {"trackId", track}, {"scene", 0}})))); // empty slot
    CHECK(! ok(r.query(s, "session.launch", object({{"action", "stop_all"}, {"scene", 0}}))));
    REQUIRE(ok(r.query(s, "session.launch", object({{"action", "scene"}, {"scene", 1}}))));
    REQUIRE(ok(r.query(s, "session.launch", object({{"action", "stop_track"}, {"trackId", track}}))));
    REQUIRE(ok(r.query(s, "recording.set", object({{"mode", "session"}, {"recording", true}})))); CHECK(! ok(r.query(s, "recording.set", object({{"mode", "tape"}, {"recording", true}}))));
    REQUIRE(ok(r.query(s, "project.save"))); CHECK(! ok(r.query(s, "project.save", object({{"path", "/tmp/x.als"}}))));
    REQUIRE(ok(r.query(s, "plugins.scan", object({{"includeAudioUnits", true}}))));
    model::Device device; device.id = 5; device.enabledTargetId = 4242; r.song->tracks[0].devices = {device};
    REQUIRE(ok(r.query(s, "devices.set_enabled", object({{"trackId", track}, {"deviceId", 5}, {"enabled", false}})))); CHECK(static_cast<int>(seen["deviceIndex"]) == 0);
    CHECK(code(r.query(s, "devices.set_enabled", object({{"trackId", track}, {"deviceId", 6}, {"enabled", false}}))) == "target_replaced");
    r.song->tracks[0].automation.clear(); model::AutomationEnvelope onOff; onOff.targetId = 4242; onOff.points = {{0, 1}}; r.song->tracks[0].automation.push_back(onOff);
    CHECK(code(r.query(s, "devices.set_enabled", object({{"trackId", track}, {"deviceId", 5}, {"enabled", false}}))) == "automation_conflict");
    // Without a handler the action is unsupported, not silently ignored.
    Rig bare; control::ControlService headless(bare.backend());
    CHECK(code(bare.query(headless, "mixer.set", object({{"trackId", bare.song->tracks[0].id}, {"pan", 0}}))) == "unsupported_operation");
}
TEST_CASE("control coverage: audio import validates files and tracks before the hook runs")
{
    Rig r; auto b = r.backend(); int imports = 0;
    b.importAudio = [&](const std::vector<std::string>& paths, int, double, int, juce::var& receipt) { ++imports; receipt = object({{"imported", static_cast<int>(paths.size())}}); return control::ApplyResult::success(); };
    control::ControlService s(std::move(b));
    const auto text = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("not-audio-" + juce::Uuid().toString() + ".txt");
    text.replaceWithText("x");
    CHECK(! ok(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{text.getFullPathName()}}}))));
    CHECK(! ok(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{"relative/a.wav"}}}))));
    CHECK(! ok(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{"/definitely/missing.wav"}}}))));
    CHECK(! ok(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{}}}))));
    const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("import-" + juce::Uuid().toString() + ".wav");
    wav.replaceWithText("RIFF"); // the extension decides what the app accepts; the hook (not the service) decodes
    const auto midiTrack = r.firstKind(model::TrackKind::Midi)->id;
    CHECK(code(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{wav.getFullPathName()}}, {"trackId", midiTrack}}))) == "incompatible_track");
    CHECK(! ok(r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{wav.getFullPathName()}}, {"startBeat", 4}, {"scene", 1}}))));
    CHECK(imports == 0);
    const auto result = r.mutate(s, "clips.import_audio", object({{"paths", juce::Array<juce::var>{wav.getFullPathName()}}, {"startBeat", 4}}));
    REQUIRE(ok(result)); CHECK(imports == 1); CHECK(static_cast<int>(result["data"]["imported"]) == 1); CHECK(result["data"]["undoToken"].isString());
    text.deleteFile(); wav.deleteFile();
}
TEST_CASE("control coverage: native agent tools cover every new operation and live tools send no revision guard")
{
    Rig r; int calls = 0;
    r.live["mixer.set"] = [&calls](const juce::var&, juce::var& receipt) { ++calls; receipt = object({{"previous", 1}}); return control::ApplyResult::success(); };
    control::ControlService s(r.backend());
    const auto tools = agent::tools(); std::set<std::string> names;
    for (const auto& tool : *tools.getArray()) names.insert(tool["name"].toString().toStdString());
    for (const char* name : {"sonora_status_get", "sonora_automation_get", "sonora_mixer_set", "sonora_transport_set", "sonora_session_launch", "sonora_recording_set",
                             "sonora_clips_import_audio", "sonora_devices_move", "sonora_devices_set_enabled", "sonora_plugins_scan", "sonora_project_save", "sonora_history_step"})
        CHECK(names.count(name) == 1);
    for (const auto& tool : *tools.getArray())
    {
        if (tool["name"].toString() == "sonora_mixer_set") CHECK(! tool["parameters"]["properties"].hasProperty("expectedRevision"));
        if (tool["name"].toString() == "sonora_devices_move") CHECK(tool["parameters"]["properties"].hasProperty("expectedRevision"));
    }
    const auto result = agent::executeTool(s, {"c1", "sonora_mixer_set", object({{"trackId", r.song->tracks[0].id}, {"gainDb", -3}})}, r.document, "key-1");
    CHECK(ok(result)); CHECK(calls == 1);
    const auto status = agent::executeTool(s, {"c2", "sonora_status_get", object({})}, r.document, "key-2");
    CHECK(ok(status)); CHECK(status["data"].hasProperty("history"));
}
TEST_CASE("control coverage: the revision a live action returns is accepted by the next guarded mutation")
{
    Rig r; auto b = r.backend();
    // Like the app's markHandEdited(): the hook advances the history version before the receipt's revision is read.
    b.live["mixer.set"] = [&r](const juce::var&, juce::var& receipt) { ++r.externalVersion; receipt = object({{"changed", true}}); return control::ApplyResult::success(); };
    control::ControlService s(std::move(b));
    const auto live = r.query(s, "mixer.set", object({{"trackId", r.song->tracks[0].id}, {"gainDb", -2}}));
    REQUIRE(ok(live)); CHECK(live["revision"].toString() == juce::String(std::to_string(r.history.version()) + ":" + std::to_string(r.externalVersion)));
    auto request = r.envelope(s, "edits.apply", object({{"commands", juce::Array<juce::var>{cmd("tempo.set", {{"bpm", 111}})}}}), true);
    request.getDynamicObject()->setProperty("expectedRevision", live["revision"]);
    CHECK(ok(s.execute(request))); CHECK_NEAR(r.song->tempo, 111, 1e-9);
}
TEST_CASE("control coverage: export, copy, view, editor and media actions validate before any hook runs")
{
    Rig r; int calls = 0; juce::var seen;
    for (const char* name : {"project.save_copy", "project.export_audio", "project.export_cancel", "view.set", "devices.open_editor", "media.run"})
        r.live[name] = [&](const juce::var& args, juce::var&) { seen = args; ++calls; return control::ApplyResult::success(); };
    control::ControlService s(r.backend());
    const auto temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto unique = juce::Uuid().toString();
    const auto wav = temp.getChildFile("export-" + unique + ".wav").getFullPathName();
    const auto taken = temp.getChildFile("taken-" + unique + ".wav");
    taken.replaceWithText("x");
    const int track = r.song->tracks[0].id;
    const auto exportArgs = [&](juce::var destination) { return object({{"destination", destination}, {"target", "main"}, {"startBeat", 0}, {"endBeat", 16}, {"format", "wav"}, {"bitDepth", 24}}); };
    REQUIRE(ok(r.query(s, "project.export_audio", exportArgs(wav)))); CHECK(calls == 1);
    const int valid = calls;
    CHECK(code(r.query(s, "project.export_audio", exportArgs(taken.getFullPathName()))) == "destination_exists");
    CHECK(! ok(r.query(s, "project.export_audio", exportArgs("relative.wav"))));
    CHECK(! ok(r.query(s, "project.export_audio", exportArgs(temp.getChildFile("missing-" + unique).getChildFile("a.wav").getFullPathName()))));
    CHECK(! ok(r.query(s, "project.export_audio", exportArgs(temp.getChildFile("x-" + unique + ".flac").getFullPathName())))); // extension vs format
    auto flip = exportArgs(wav); flip.getDynamicObject()->setProperty("endBeat", 0); CHECK(! ok(r.query(s, "project.export_audio", flip)));
    auto deep = exportArgs(temp.getChildFile("d-" + unique + ".flac").getFullPathName()); deep.getDynamicObject()->setProperty("format", "flac"); deep.getDynamicObject()->setProperty("bitDepth", 32);
    CHECK(! ok(r.query(s, "project.export_audio", deep)));
    auto trackless = exportArgs(wav); trackless.getDynamicObject()->setProperty("target", "track"); CHECK(! ok(r.query(s, "project.export_audio", trackless)));
    auto tailed = exportArgs(wav); tailed.getDynamicObject()->setProperty("tailSeconds", 2); tailed.getDynamicObject()->setProperty("autoTail", true); CHECK(! ok(r.query(s, "project.export_audio", tailed)));
    auto single = exportArgs(wav); single.getDynamicObject()->setProperty("target", "track"); single.getDynamicObject()->setProperty("trackId", track);
    REQUIRE(ok(r.query(s, "project.export_audio", single))); CHECK(static_cast<int>(seen["trackIndex"]) == 0);
    auto stems = exportArgs(temp.getChildFile("stems-" + unique).getFullPathName()); stems.getDynamicObject()->setProperty("target", "stems");
    REQUIRE(ok(r.query(s, "project.export_audio", stems)));
    CHECK(calls == valid + 2);
    const auto copy = temp.getChildFile("copy-" + unique + ".als").getFullPathName();
    REQUIRE(ok(r.query(s, "project.save_copy", object({{"path", copy}}))));
    CHECK(! ok(r.query(s, "project.save_copy", object({{"path", wav}})))); // not .als
    CHECK(code(r.query(s, "project.save_copy", object({{"path", taken.getFullPathName()}}))) == "destination_exists");
    REQUIRE(ok(r.query(s, "project.export_cancel")));
    // view.set
    REQUIRE(ok(r.query(s, "view.set", object({{"mainView", "session"}, {"detail", "hidden"}, {"selectTrackId", track}, {"mixerVisible", true}}))));
    CHECK(static_cast<int>(seen["selectTrackIndex"]) == 0);
    CHECK(! ok(r.query(s, "view.set"))); CHECK(! ok(r.query(s, "view.set", object({{"mainView", "tabs"}}))));
    CHECK(code(r.query(s, "view.set", object({{"selectTrackId", 424242}}))) == "target_replaced");
    // media.run
    auto audio = midiClip(0, 4); audio.kind = model::Clip::Kind::Audio; r.song->tracks[2].arrangementClips = {audio};
    auto midi = midiClip(8, 12); r.song->tracks[0].arrangementClips = {midi};
    const int audioTrack = r.song->tracks[2].id;
    REQUIRE(ok(r.query(s, "media.run", object({{"action", "reverse"}, {"trackId", audioTrack}, {"clipId", audio.uid}}))));
    REQUIRE(ok(r.query(s, "media.run", object({{"action", "audio_to_midi"}, {"trackId", audioTrack}, {"clipId", audio.uid}, {"mode", "drums"}}))));
    CHECK(! ok(r.query(s, "media.run", object({{"action", "audio_to_midi"}, {"trackId", audioTrack}, {"clipId", audio.uid}}))));
    CHECK(code(r.query(s, "media.run", object({{"action", "reverse"}, {"trackId", track}, {"clipId", midi.uid}}))) == "unsupported_clip");
    CHECK(! ok(r.query(s, "media.run", object({{"action", "separate_stems"}, {"trackId", audioTrack}, {"clipId", audio.uid}}))));
    CHECK(! ok(r.query(s, "media.run", object({{"action", "freeze"}, {"trackId", audioTrack}, {"clipId", audio.uid}}))));
    CHECK(! ok(r.query(s, "media.run", object({{"action", "unfreeze"}, {"trackId", audioTrack}})))); // not frozen
    REQUIRE(ok(r.query(s, "media.run", object({{"action", "bounce_new_track"}, {"trackId", audioTrack}}))));
    r.song->tracks[2].frozen = true;
    REQUIRE(ok(r.query(s, "media.run", object({{"action", "unfreeze"}, {"trackId", audioTrack}}))));
    CHECK(code(r.query(s, "media.run", object({{"action", "bounce_in_place"}, {"trackId", audioTrack}}))) == "track_frozen");
    // devices.open_editor: hosted plug-ins only
    model::Device plugin; plugin.id = 7; plugin.kind = model::DeviceKind::PluginVst3; plugin.plugin.emplace(); model::Device native; native.id = 8;
    r.song->tracks[0].devices = {plugin, native};
    REQUIRE(ok(r.query(s, "devices.open_editor", object({{"trackId", track}, {"deviceId", 7}})))); CHECK(static_cast<int>(seen["deviceIndex"]) == 0);
    CHECK(code(r.query(s, "devices.open_editor", object({{"trackId", track}, {"deviceId", 8}}))) == "unsupported_device");
    CHECK(code(r.query(s, "devices.open_editor", object({{"trackId", track}, {"deviceId", 9}}))) == "target_replaced");
    taken.deleteFile();
}
