// UI-3b: Arrangement locators in the model, the op layer, the .als reader/writer and the control API.
#include "TestFramework.h"
#include "control/ControlService.h"
#include "core/ModelCodec.h"
#include "core/OpJson.h"
#include "engine/Transport.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"

using namespace sonora;
namespace
{
model::Locator locatorAt(double time, const char* name, int id)
{
    model::Locator l;
    l.time = time; l.name = name; l.id = id;
    return l;
}

model::EditCommand command(const model::Song& song, model::OpPayload payload)
{
    return model::opToCommand(song, {std::move(payload)});
}

std::string errorCode(const model::Song& song, model::OpPayload payload)
{
    try { command(song, std::move(payload)); }
    catch (const model::OpError& error) { return error.code; }
    return "accepted";
}

juce::XmlElement* locatorList(juce::XmlElement& root)
{
    return root.getChildByName("LiveSet")->getChildByName("Locators")->getChildByName("Locators");
}

juce::XmlElement* addXmlLocator(juce::XmlElement& list, int id, double time, const char* name)
{
    auto* e = list.createNewChildElement("Locator");
    e->setAttribute("Id", id);
    e->createNewChildElement("LomId")->setAttribute("Value", 0);
    e->createNewChildElement("Time")->setAttribute("Value", time);
    e->createNewChildElement("Name")->setAttribute("Value", name);
    e->createNewChildElement("Annotation")->setAttribute("Value", "");
    e->createNewChildElement("IsSongStart")->setAttribute("Value", "false");
    return e;
}
} // namespace

TEST_CASE("locators: add, move, rename, colour and delete by id keep the list sorted and undo exactly")
{
    model::Song song;
    model::EditHistory history;
    history.perform(song, command(song, model::ops::AddLocator{16, std::nullopt, -1}));
    history.perform(song, command(song, model::ops::AddLocator{4, std::string("Intro"), 12}));
    REQUIRE(song.locators.size() == 2);
    CHECK(song.locators[0].name == "Intro"); CHECK(song.locators[0].id == 1); CHECK(song.locators[0].colorIndex == 12);
    CHECK(song.locators[1].name == "1"); CHECK(song.locators[1].id == 0); // unnamed ones are numbered like the Set button's
    CHECK(model::defaultLocatorName(song) == "3");

    model::ops::UpdateLocator move; move.locatorId = 1; move.beat = 32;
    auto moved = command(song, move);
    CHECK(moved.label == "Move Locator");
    history.perform(song, std::move(moved));
    CHECK(song.locators[0].id == 0); CHECK(song.locators[1].id == 1); CHECK_NEAR(song.locators[1].time, 32, 1e-12);

    model::ops::UpdateLocator edit; edit.locatorId = 0; edit.name = "Drop"; edit.color = 3; edit.infoText = "big"; edit.songStart = true;
    history.perform(song, command(song, edit));
    CHECK(song.locators[0].name == "Drop"); CHECK(song.locators[0].colorIndex == 3); CHECK(song.locators[0].annotation == "big");
    CHECK(song.locators[0].isSongStart);
    model::ops::UpdateLocator start; start.locatorId = 1; start.songStart = true;
    history.perform(song, command(song, start));
    CHECK(! song.locators[0].isSongStart); CHECK(song.locators[1].isSongStart); // only one song start

    history.perform(song, command(song, model::ops::DeleteLocator{0}));
    REQUIRE(song.locators.size() == 1); CHECK(song.locators[0].id == 1);
    // A deleted id is never reused while a later id exists: the next locator takes 2.
    history.perform(song, command(song, model::ops::AddLocator{8, std::string("Verse"), -1}));
    CHECK(model::findLocator(song, 2) != nullptr);

    for (int i = 0; i < 3; ++i) REQUIRE(history.undo(song)); // the new locator, the delete, the song start move
    REQUIRE(song.locators.size() == 2);
    CHECK(song.locators[0].name == "Drop"); CHECK(song.locators[0].isSongStart); CHECK(! song.locators[1].isSongStart);
    while (history.undo(song)) {}
    CHECK(song.locators.empty());
    while (history.redo(song)) {}
    REQUIRE(song.locators.size() == 2); CHECK(song.locators[0].name == "Verse"); CHECK(song.locators[1].id == 1);
}

TEST_CASE("locators: ops reject unknown ids, taken beats, bad colours and empty updates without changing the song")
{
    model::Song song;
    song.locators = {locatorAt(4, "A", 0), locatorAt(8, "B", 5)};
    CHECK(errorCode(song, model::ops::DeleteLocator{3}) == "target_replaced");
    CHECK(errorCode(song, model::ops::AddLocator{8, std::nullopt, -1}) == "invalid_request");   // a locator is there
    CHECK(errorCode(song, model::ops::AddLocator{-1, std::nullopt, -1}) == "invalid_request");
    CHECK(errorCode(song, model::ops::AddLocator{2, std::nullopt, 70}) == "invalid_request");
    CHECK(errorCode(song, model::ops::AddLocator{2, std::string(257, 'x'), -1}) == "invalid_request");
    model::ops::UpdateLocator empty; empty.locatorId = 0;
    CHECK(errorCode(song, empty) == "invalid_request");
    model::ops::UpdateLocator onto; onto.locatorId = 0; onto.beat = 8;
    CHECK(errorCode(song, onto) == "invalid_request");
    model::ops::UpdateLocator same; same.locatorId = 5; same.beat = 8; // its own beat is fine
    CHECK(errorCode(song, same) == "accepted");
    CHECK(model::nextLocatorId(song) == 6);
    CHECK(song.locators.size() == 2);
}

TEST_CASE("locators: wire form round-trips and the strict decoder rejects unknown fields")
{
    model::ops::UpdateLocator update;
    update.locatorId = 7; update.beat = 1.0 / 3.0; update.name = "Br\xc3\xbc" "cke"; update.infoText = "x"; update.color = -1; update.songStart = false;
    for (const model::Op& op : {model::Op{model::ops::AddLocator{2.5, std::string("Intro"), 9}}, model::Op{model::ops::AddLocator{4, std::nullopt, -1}},
                                model::Op{update}, model::Op{model::ops::DeleteLocator{7}}})
    {
        const auto text = model::opMessageToString(model::opToJson(op));
        const auto again = model::opMessageToString(model::opToJson(model::opFromJson(juce::JSON::parse(text))));
        CHECK(text == again);
    }
    const auto decoded = std::get<model::ops::UpdateLocator>(model::opFromJson(model::opToJson({update})).payload);
    CHECK(*decoded.name == "Br\xc3\xbc" "cke"); CHECK(juce::exactlyEqual(*decoded.beat, 1.0 / 3.0)); CHECK(*decoded.color == -1);
    bool rejected = false;
    try { model::opFromJson(juce::JSON::parse(R"({"type":"locator.delete","locatorId":1,"beat":2})")); }
    catch (const model::OpError& error) { rejected = error.code == "invalid_request"; }
    CHECK(rejected);
}

TEST_CASE("locators: collab snapshots carry id, info text, song start and colour")
{
    model::Song song;
    auto l = locatorAt(12.5, "Chorus", 4); l.annotation = "loud"; l.isSongStart = true; l.colorIndex = 21;
    song.locators = {l};
    const auto bytes = model::codec::encodeSongHeader(song, model::codec::Form::Full);
    model::Song decoded;
    model::codec::decodeSongHeader(bytes, decoded);
    REQUIRE(decoded.locators.size() == 1);
    CHECK(decoded.locators[0].id == 4); CHECK(decoded.locators[0].annotation == "loud");
    CHECK(decoded.locators[0].isSongStart); CHECK(decoded.locators[0].colorIndex == 21);
}

TEST_CASE("locators: the .als keeps Ids, unknown children and info text; colours live in SonoraLocators as LiveSet's last child")
{
    std::string error;
    auto dom = io::createNewSetDom();
    auto* list = locatorList(*dom);
    addXmlLocator(*list, 7, 16, "Drop")->createNewChildElement("KeyMidi")->setAttribute("Value", "mapped");
    auto* intro = addXmlLocator(*list, 3, 0, "Intro");
    intro->getChildByName("Annotation")->setAttribute("Value", "count-in");
    intro->getChildByName("IsSongStart")->setAttribute("Value", "true");
    addXmlLocator(*list, 3, 8, "Duplicate id"); // repaired, not dropped
    juce::TemporaryFile file(".als");
    REQUIRE(io::writeAlsDom(*dom, file.getFile(), error));

    auto loaded = io::loadAls(file.getFile()); REQUIRE(loaded);
    auto song = *loaded.song;
    REQUIRE(song.locators.size() == 3);
    CHECK(song.locators[0].name == "Intro"); CHECK(song.locators[0].id == 3); CHECK(song.locators[0].annotation == "count-in");
    CHECK(song.locators[0].isSongStart);
    CHECK(song.locators[1].name == "Duplicate id"); CHECK(song.locators[1].id == 8);
    CHECK(song.locators[2].name == "Drop"); CHECK(song.locators[2].id == 7);

    model::EditHistory history;
    history.perform(song, command(song, model::ops::DeleteLocator{3}));
    model::ops::UpdateLocator colour; colour.locatorId = 7; colour.color = 25; colour.name = "Drop!";
    history.perform(song, command(song, colour));
    history.perform(song, command(song, model::ops::AddLocator{2, std::string("New"), 4}));
    REQUIRE(io::saveAls(song, file.getFile()).ok);

    const auto saved = io::readAlsDom(file.getFile(), error); REQUIRE(saved);
    auto* savedList = locatorList(*saved);
    REQUIRE(savedList->getNumChildElements() == 3);
    CHECK(savedList->getChildElement(0)->getChildByName("Name")->getStringAttribute("Value") == "New");
    CHECK(savedList->getChildElement(1)->getIntAttribute("Id") == 8);
    auto* drop = savedList->getChildElement(2);
    CHECK(drop->getIntAttribute("Id") == 7);
    CHECK(drop->getChildByName("Name")->getStringAttribute("Value") == "Drop!");
    CHECK(drop->getChildByName("KeyMidi") != nullptr); // the deleted locator's XML did not move onto another one
    const int newId = savedList->getChildElement(0)->getIntAttribute("Id");
    CHECK(newId == 9);
    const auto* liveSet = saved->getChildByName("LiveSet");
    const auto* last = liveSet->getChildElement(liveSet->getNumChildElements() - 1);
    REQUIRE(last->hasTagName("SonoraLocators"));
    CHECK(last->getNumChildElements() == 2);

    auto reopened = io::loadAls(file.getFile()); REQUIRE(reopened);
    REQUIRE(reopened.song->locators.size() == 3);
    CHECK(reopened.song->locators[0].colorIndex == 4); CHECK(reopened.song->locators[1].colorIndex == -1);
    CHECK(reopened.song->locators[2].colorIndex == 25);

    // Clearing every colour removes the extension element again.
    auto plain = *reopened.song;
    for (auto& l : plain.locators) l.colorIndex = -1;
    REQUIRE(io::saveAls(plain, file.getFile()).ok);
    const auto cleared = io::readAlsDom(file.getFile(), error); REQUIRE(cleared);
    CHECK(cleared->getChildByName("LiveSet")->getChildByName("SonoraLocators") == nullptr);
}

TEST_CASE("locators: the transport performs a scheduled jump at the first block from its boundary, and stop cancels it")
{
    engine::Transport transport;
    transport.seekNow(1.5); transport.play(); transport.beginBlock();
    transport.scheduleJump(4, 32);
    CHECK_NEAR(transport.scheduledJumpTarget(), 32, 1e-12);
    transport.advanceTo(3.9);
    auto block = transport.beginBlock();
    CHECK(! block.jumped); CHECK_NEAR(block.position, 3.9, 1e-12);
    transport.advanceTo(4.01);
    block = transport.beginBlock();
    CHECK(block.jumped); CHECK_NEAR(block.position, 32, 1e-12); CHECK(transport.scheduledJumpTarget() < 0);
    transport.scheduleJump(36, 0);
    transport.stop(); transport.beginBlock();
    CHECK(transport.scheduledJumpTarget() < 0);
}

TEST_CASE("control: locator add/update/delete through edits.apply, inspect lists them, guarded undo restores")
{
    auto song = io::parseAls(*io::createNewSetDom(), "/tmp/control-locators.als").song;
    model::EditHistory history;
    control::Backend backend;
    backend.state = [&] { control::State s; s.song = song.get(); s.documentId = "doc"; s.revision = std::to_string(history.version()); return s; };
    backend.apply = [&](model::EditCommand c) { history.perform(*song, std::move(c)); return control::ApplyResult::success(); };
    backend.undo = [&] { history.undo(*song); return control::ApplyResult::success(); };
    control::ControlService service(std::move(backend));
    const auto request = [&](juce::Array<juce::var> commands, const char* key) {
        return control::object({{"apiVersion", "1.0"}, {"operation", "edits.apply"}, {"arguments", control::object({{"commands", commands}})},
            {"target", control::object({{"instanceId", service.instanceId()}, {"documentId", "doc"}})},
            {"expectedRevision", juce::String(history.version())}, {"idempotencyKey", key}});
    };
    const auto added = service.execute(request({control::object({{"type", "locator.add"}, {"beat", 16}, {"name", "Drop"}, {"color", 5}}),
                                                control::object({{"type", "locator.add"}, {"beat", 4}})}, "add"));
    REQUIRE(static_cast<bool>(added["ok"]));
    REQUIRE(added["data"]["locatorIds"].size() == 2);
    const int drop = added["data"]["locatorIds"][0], second = added["data"]["locatorIds"][1];
    auto inspected = service.inspect()["data"]["locators"];
    REQUIRE(inspected.size() == 2);
    CHECK(inspected[0]["name"].toString() == "2"); CHECK(static_cast<int>(inspected[0]["locatorId"]) == second);
    CHECK(static_cast<int>(inspected[1]["color"]) == 5); CHECK_NEAR(static_cast<double>(inspected[1]["beat"]), 16, 1e-12);
    CHECK(history.version() == 1); // one undo step for the batch

    const auto changed = service.execute(request({control::object({{"type", "locator.update"}, {"locatorId", drop}, {"name", "Chorus"}, {"beat", 24}, {"songStart", true}}),
                                                  control::object({{"type", "locator.delete"}, {"locatorId", second}})}, "edit"));
    REQUIRE(static_cast<bool>(changed["ok"]));
    REQUIRE(song->locators.size() == 1);
    CHECK(song->locators[0].name == "Chorus"); CHECK_NEAR(song->locators[0].time, 24, 1e-12); CHECK(song->locators[0].isSongStart);

    const auto missing = service.execute(request({control::object({{"type", "locator.delete"}, {"locatorId", 999}})}, "missing"));
    CHECK(missing["error"]["code"].toString() == "target_replaced");
    const auto taken = service.execute(request({control::object({{"type", "locator.add"}, {"beat", 24}})}, "taken"));
    CHECK(taken["error"]["code"].toString() == "invalid_request");
    CHECK(song->locators.size() == 1);

    const auto undo = service.submit("undo");
    REQUIRE(static_cast<bool>(undo["ok"]));
    REQUIRE(song->locators.size() == 2); CHECK(song->locators[1].name == "Drop");
    const auto types = service.execute(control::object({{"apiVersion", "1.0"}, {"operation", "capabilities.get"}}))["data"]["editTypes"];
    CHECK(types.getArray()->contains("locator.update"));
}
