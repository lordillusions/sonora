#include "TestFramework.h"

#include <set>
#include <thread>
#include <atomic>

#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"

#include <juce_core/juce_core.h>

using namespace sonora;

namespace
{
juce::File fixture(const char* name)
{
    return juce::File(SONORA_TEST_FIXTURES).getChildFile(name);
}
} // namespace

TEST_CASE("hex blob decoding ignores whitespace")
{
    auto bytes = io::decodeHexBlob("0A0B\n\t\t0c ff");
    REQUIRE(bytes.size() == 4);
    CHECK(bytes[0] == 0x0A);
    CHECK(bytes[2] == 0x0C);
    CHECK(bytes[3] == 0xFF);
}

TEST_CASE("als: minimal fixture loads")
{
    auto r = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(r);
    const auto& s = *r.song;

    CHECK(s.creator == "Ableton Live 12.1.5");
    CHECK_NEAR(s.tempo, 128.0, 1e-9);
    CHECK(s.timeSigNumerator == 4);
    CHECK(s.timeSigDenominator == 4);
    CHECK(s.loopOn);
    CHECK_NEAR(s.loopStart, 4.0, 1e-9);

    REQUIRE(s.tracks.size() == 1);
    REQUIRE(s.returns.size() == 1);
    const auto& t = s.tracks[0];
    CHECK(t.kind == model::TrackKind::Midi);
    CHECK(t.name == "1-Synth");
    CHECK(t.colorIndex == 19);
    CHECK_NEAR(t.mixer.volume, 0.5011872, 1e-7);
    CHECK_NEAR(t.mixer.pan, -0.25, 1e-9);
    CHECK(t.mixer.volumeTargetId == 100);
    REQUIRE(t.mixer.sends.size() == 1);
    CHECK_NEAR(t.mixer.sends[0], 0.5, 1e-9);

    CHECK(t.mixer.speakerTargetId == 102);
    REQUIRE(t.mixer.sendTargetIds.size() == 1);
    CHECK(t.mixer.sendTargetIds[0] == 103);

    // Automation: float and bool events, sorted, with the stepped flag for bool/enum
    REQUIRE(t.automation.size() == 5);
    CHECK(t.automation[0].targetId == 100);
    CHECK(! t.automation[0].stepped);
    REQUIRE(t.automation[0].points.size() == 3);
    CHECK_NEAR(t.automation[0].points[2].value, 0.5, 1e-9);
    CHECK(t.automation[1].stepped);
    REQUIRE(t.automation[1].points.size() == 2);
    CHECK_NEAR(t.automation[1].points[0].value, 1.0, 1e-9); // BoolEvent "true"
    CHECK_NEAR(t.automation[1].points[1].value, 0.0, 1e-9); // BoolEvent "false"

    // Session
    REQUIRE(t.slots.size() == 2);
    CHECK(! t.slots[0].clip.has_value());
    REQUIRE(t.slots[1].clip.has_value());
    CHECK(t.slots[1].clip->name == "Session Clip");
    CHECK(t.slots[1].clip->notes.size() == 1);

    // Arrangement, notes sorted by start
    REQUIRE(t.arrangementClips.size() == 1);
    const auto& c = t.arrangementClips[0];
    CHECK_NEAR(c.currentStart, 4.0, 1e-9);
    CHECK_NEAR(c.currentEnd, 12.0, 1e-9);
    REQUIRE(c.notes.size() == 2);
    CHECK_NEAR(c.notes[0].start, 0.75, 1e-9);
    CHECK(c.notes[0].pitch == 65);

    // Devices
    REQUIRE(t.devices.size() == 2);
    const auto& plug = t.devices[0];
    CHECK(plug.kind == model::DeviceKind::PluginVst3);
    REQUIRE(plug.plugin.has_value());
    CHECK(plug.plugin->name == "SomeSynth");
    CHECK(plug.plugin->state.size() == 4);
    CHECK(plug.plugin->vst3Uid[3] == 0xFFFFFFFFu);

    const auto& eq = t.devices[1];
    CHECK(eq.kind == model::DeviceKind::Native);
    CHECK(eq.displayName == "EQ Eight");
    CHECK_NEAR(eq.param("Bands.0/ParameterA/Freq", -1), 80.0, 1e-9);
    CHECK_NEAR(eq.param("Bands.0/ParameterA/Mode", -1), 1.0, 1e-9);

    // Scenes
    REQUIRE(s.scenes.size() == 2);
    CHECK(s.scenes[1].name == "Drop");
    REQUIRE(s.scenes[1].tempo.has_value());
    CHECK_NEAR(*s.scenes[1].tempo, 140.0, 1e-9);

    // Chorus-Ensemble is not implemented -> warning, but load succeeds.
    bool warned = false;
    for (const auto& w : s.warnings)
        warned |= w.find("Chorus2") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("als: missing file reports error")
{
    auto r = io::loadAls(juce::File("/nonexistent/x.als"));
    CHECK(! r);
    CHECK(! r.error.empty());
}

// ---------------------------------------------------------------- writer (ALS-10)

#include "core/Edit.h"
#include "io/als/AlsWriter.h"

namespace
{
int countElements(const juce::XmlElement& e)
{
    int n = 1;
    for (const auto* c : e.getChildIterator())
        n += countElements(*c);
    return n;
}
} // namespace

TEST_CASE("als writer: an untouched song saves to an equivalent set (nothing lost)")
{
    const auto fixtureFile = fixture("minimal_set.xml");
    auto loaded = io::loadAls(fixtureFile);
    REQUIRE(loaded);
    std::string err;
    auto original = io::readAlsDom(fixtureFile, err);
    REQUIRE(original != nullptr);

    juce::TemporaryFile out(".als");
    const auto saved = io::saveAls(*loaded.song, out.getFile());
    REQUIRE(saved.ok);
    CHECK(saved.clipsAdded == 0);
    CHECK(saved.clipsRemoved == 0);

    auto reloaded = io::readAlsDom(out.getFile(), err); // gzip written, gzip read
    REQUIRE(reloaded != nullptr);
    CHECK(countElements(*reloaded) == countElements(*original)); // every element survived
    CHECK(original->isEquivalentTo(reloaded.get(), false));       // and nothing changed at all
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks.size() == loaded.song->tracks.size());
    CHECK_NEAR(again.song->tempo, loaded.song->tempo, 1e-9);
    CHECK(again.song->tracks[0].arrangementClips.size() == 1);
    CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == 2);
}

TEST_CASE("als writer: edits land in the file and reload identically")
{
    const auto fixtureFile = fixture("minimal_set.xml");
    auto loaded = io::loadAls(fixtureFile);
    REQUIRE(loaded);
    model::Song& song = *loaded.song;
    model::EditHistory h;

    h.perform(song, model::setTempo(song, 133.0));
    h.perform(song, model::setLoop(song, false, 2.0, 12.0));
    // Mixer + device edits are plain data changes.
    song.tracks[0].mixer.volume = 0.25;
    song.tracks[0].mixer.pan = 0.5;
    song.tracks[0].mixer.speakerOn = false;
    for (auto& p : song.tracks[0].devices[1].parameters)
        if (p.path == "Bands.0/ParameterA/Freq")
            p.value = 120.0;
    song.tracks[0].devices[1].enabled = false;

    // Arrangement: move the clip, add a note, duplicate it.
    const int uid = song.tracks[0].arrangementClips[0].uid;
    model::Clip moved = model::movedClip(song.tracks[0].arrangementClips[0], 16.0);
    moved.name = "Moved";
    moved.notes.push_back({3.0, 0.5, 48, 80.0f});
    h.perform(song, model::modifyClip(song, 0, uid, moved, "Edit Clip"));
    h.perform(song, model::duplicateClip(song, 0, uid));
    REQUIRE(song.tracks[0].arrangementClips.size() == 2);

    juce::TemporaryFile out(".als");
    const auto saved = io::saveAls(song, out.getFile());
    REQUIRE(saved.ok);
    CHECK(saved.clipsAdded == 1);

    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto& s2 = *again.song;
    CHECK_NEAR(s2.tempo, 133.0, 1e-9);
    CHECK(! s2.loopOn);
    CHECK_NEAR(s2.loopStart, 2.0, 1e-9);
    CHECK_NEAR(s2.loopLength, 12.0, 1e-9);
    CHECK_NEAR(s2.tracks[0].mixer.volume, 0.25, 1e-9);
    CHECK_NEAR(s2.tracks[0].mixer.pan, 0.5, 1e-9);
    CHECK(! s2.tracks[0].mixer.speakerOn);
    CHECK_NEAR(s2.tracks[0].devices[1].param("Bands.0/ParameterA/Freq", -1), 120.0, 1e-9);
    CHECK(! s2.tracks[0].devices[1].enabled);

    REQUIRE(s2.tracks[0].arrangementClips.size() == 2);
    const auto& a = s2.tracks[0].arrangementClips[0];
    const auto& b = s2.tracks[0].arrangementClips[1];
    CHECK_NEAR(a.currentStart, 16.0, 1e-9);        // the moved clip
    CHECK_NEAR(a.currentEnd, 24.0, 1e-9);
    CHECK(a.name == "Moved");
    CHECK(a.notes.size() == 3);
    CHECK_NEAR(b.currentStart, 24.0, 1e-9);        // the duplicate right after it
    CHECK(b.notes.size() == 3);                    // cloned with its notes
    CHECK(a.xmlId != b.xmlId);                     // distinct elements in the file

    // Notes keep their ids; the new one got a fresh one.
    std::vector<int> ids;
    for (const auto& n : a.notes)
        ids.push_back(n.noteId);
    CHECK(std::set<int>(ids.begin(), ids.end()).size() == ids.size());
}

TEST_CASE("als writer: launch settings and clip signature save and reload; untouched defaults add no elements")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    model::Song& song = *loaded.song;
    model::EditHistory h;
    const int uid = song.tracks[0].arrangementClips[0].uid;

    {
        juce::TemporaryFile plain(".als");
        REQUIRE(io::saveAls(song, plain.getFile()).ok);
        auto reread = io::loadAls(plain.getFile());
        REQUIRE(reread);
        const auto& c = reread.song->tracks[0].arrangementClips[0];
        CHECK(c.launchMode == song.tracks[0].arrangementClips[0].launchMode);
        CHECK(c.launchQuantisation == song.tracks[0].arrangementClips[0].launchQuantisation);
    }

    model::Clip edited = song.tracks[0].arrangementClips[0];
    edited.launchMode = 2;
    edited.launchQuantisation = 9;
    edited.legato = true;
    edited.velocityAmount = 0.5;
    edited.timeSigNumerator = 7;
    edited.timeSigDenominator = 8;
    h.perform(song, model::modifyClip(song, 0, uid, edited, "Edit Clip"));

    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto& c = again.song->tracks[0].arrangementClips[0];
    CHECK(c.launchMode == 2);
    CHECK(c.launchQuantisation == 9);
    CHECK(c.legato);
    CHECK_NEAR(c.velocityAmount, 0.5, 1e-9);
    CHECK(c.timeSigNumerator == 7);
    CHECK(c.timeSigDenominator == 8);
}

TEST_CASE("als writer: deleting a clip removes its element, saving over the source keeps a backup")
{
    juce::TemporaryFile dirHolder;
    const auto dir = dirHolder.getFile();
    dir.createDirectory();
    const auto copy = dir.getChildFile("set.xml");
    fixture("minimal_set.xml").copyFileTo(copy);

    auto loaded = io::loadAls(copy);
    REQUIRE(loaded);
    model::Song& song = *loaded.song;
    model::EditHistory h;
    h.perform(song, model::deleteClip(song, 0, song.tracks[0].arrangementClips[0].uid));

    const auto saved = io::saveAls(song, copy); // same file: must create Backup/
    REQUIRE(saved.ok);
    CHECK(saved.clipsRemoved == 1);
    CHECK(dir.getChildFile("Backup").isDirectory());
    CHECK(dir.getChildFile("Backup").getNumberOfChildFiles(juce::File::findFiles) == 1);

    auto again = io::loadAls(copy);
    REQUIRE(again);
    CHECK(again.song->tracks[0].arrangementClips.empty());
}

TEST_CASE("als writer: clips can be copied or moved to another track, and new empty MIDI clips can be created")
{
    auto loaded = io::loadAls(fixture("two_tracks.xml"));
    REQUIRE(loaded);
    model::Song& song = *loaded.song;
    REQUIRE(song.tracks.size() == 2);
    model::EditHistory h;
    const int uid = song.tracks[0].arrangementClips[0].uid;

    // Copy to the second track at beat 32, and create a fresh MIDI clip on it at beat 0.
    h.perform(song, model::copyClipToTrack(song, 0, uid, 1, 32.0));
    model::Clip fresh;
    REQUIRE(model::makeEmptyMidiClip(song, 0.0, 4.0, fresh));
    fresh.name = "New";
    h.perform(song, model::insertClip(1, fresh, "Insert MIDI Clip"));
    // Then move the original over as well (the clone must be taken before the original is removed).
    h.perform(song, model::moveClipToTrack(song, 0, uid, 1, 64.0));
    CHECK(song.tracks[0].arrangementClips.empty());
    REQUIRE(song.tracks[1].arrangementClips.size() == 3);

    juce::TemporaryFile out(".als");
    const auto saved = io::saveAls(song, out.getFile());
    REQUIRE(saved.ok);
    CHECK(saved.clipsAdded == 3);
    CHECK(saved.clipsRemoved == 1);

    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].arrangementClips.empty());
    const auto& clips = again.song->tracks[1].arrangementClips;
    REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentStart, 0.0, 1e-9);   // the new empty clip
    CHECK(clips[0].notes.empty());
    CHECK(clips[0].name == "New");
    CHECK_NEAR(clips[1].currentStart, 32.0, 1e-9);  // the copy keeps the original's notes
    CHECK(clips[1].notes.size() == 2);
    CHECK_NEAR(clips[2].currentStart, 64.0, 1e-9);  // the moved clip
    CHECK(clips[2].notes.size() == 2);
}

TEST_CASE("als writer: automation edits preserve event types, new targets and untouched lanes")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory h;
    auto volume = song.tracks[0].automation[0];
    volume.points.push_back({12, 0.25});
    h.perform(song, model::setAutomation(song, 0, volume));
    auto active = song.tracks[0].automation[1];
    REQUIRE(active.eventType == "BoolEvent");
    active.points.back().time = 18;
    h.perform(song, model::setAutomation(song, 0, active));
    model::AutomationEnvelope pan;
    pan.targetId = song.tracks[0].mixer.panTargetId;
    pan.points = {{-63072000, 0}, {2, -0.5}, {6, 0.75}};
    h.perform(song, model::setAutomation(song, 0, pan));
    model::AutomationEnvelope enumerated;
    enumerated.targetId = 999;
    enumerated.stepped = true;
    enumerated.eventType = "EnumEvent";
    enumerated.points = {{0, 1}, {4, 3}};
    h.perform(song, model::setAutomation(song, 0, enumerated));
    juce::TemporaryFile out(".als");
    REQUIRE(io::saveAls(song, out.getFile()).ok);
    auto again = io::loadAls(out.getFile());
    REQUIRE(again);
    const auto& envs = again.song->tracks[0].automation;
    REQUIRE(envs.size() == song.tracks[0].automation.size());
    for (const auto& expected : song.tracks[0].automation)
    {
        const auto it = std::find_if(envs.begin(), envs.end(), [&](const auto& e) { return e.targetId == expected.targetId; });
        REQUIRE(it != envs.end());
        CHECK(it->stepped == expected.stepped);
        CHECK(it->eventType == expected.eventType);
        REQUIRE(it->points.size() == expected.points.size());
        for (size_t i = 0; i < expected.points.size(); ++i)
        {
            CHECK_NEAR(it->points[i].time, expected.points[i].time, 1e-9);
            CHECK_NEAR(it->points[i].value, expected.points[i].value, 1e-9);
        }
    }
}

TEST_CASE("als writer: session clips, scenes, locators and track names save and preserve untouched XML")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    auto clip = song.tracks[0].arrangementClips[0];
    clip.xmlId = -1;
    clip.cloneOfXmlId = 0;
    clip.cloneOfTrackId = song.tracks[0].id;
    clip.name = "Recorded session";
    clip.notes.push_back({2, 0.5, 70, 100});
    history.perform(song, model::setSessionClip(song, 0, 2, clip));
    song.scenes[1].name = "Renamed scene";
    song.scenes[1].tempo = 150;
    song.tracks[0].name = "Renamed track";
    song.locators = {{4, "Intro"}, {12, "Drop"}};
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].slots.size() == 3);
    REQUIRE(again.song->tracks[0].slots[2].clip);
    CHECK(again.song->tracks[0].slots[2].clip->name == "Recorded session");
    CHECK(again.song->tracks[0].slots[2].clip->notes.size() == 3);
    CHECK(again.song->tracks[0].name == "Renamed track");
    REQUIRE(again.song->scenes.size() == 3);
    CHECK(again.song->scenes[1].name == "Renamed scene");
    CHECK_NEAR(*again.song->scenes[1].tempo, 150, 1e-9);
    REQUIRE(again.song->locators.size() == 2);
    CHECK(again.song->locators[1].name == "Drop");
    CHECK_NEAR(again.song->locators[1].time, 12, 1e-9);
    // A second unchanged save must retain every element and attribute.
    juce::TemporaryFile saved2(".als");
    REQUIRE(io::saveAls(*again.song, saved2.getFile()).ok);
    std::string error;
    auto dom1 = io::readAlsDom(saved.getFile(), error), dom2 = io::readAlsDom(saved2.getFile(), error);
    CHECK(dom1->isEquivalentTo(dom2.get(), false));
    // A session-only clip can serve as a template for a new arrangement clip.
    REQUIRE(again.song->tracks[0].slots[1].clip);
    auto fromSession = *again.song->tracks[0].slots[1].clip;
    fromSession.xmlId = fromSession.cloneOfXmlId = -1;
    fromSession.cloneOfSessionSlot = 1;
    fromSession.cloneOfTrackId = again.song->tracks[0].id;
    fromSession.currentStart = fromSession.time = 24;
    fromSession.currentEnd = 28;
    fromSession.name = "From session";
    model::EditHistory h2;
    h2.perform(*again.song, model::insertClip(0, fromSession, "Copy Session Clip"));
    REQUIRE(io::saveAls(*again.song, saved2.getFile()).ok);
    auto copied = io::loadAls(saved2.getFile());
    REQUIRE(copied);
    CHECK(copied.song->tracks[0].arrangementClips.back().name == "From session");
}

TEST_CASE("als writer: undo after saving removes new automation and new session slots from the next save")
{
    auto loaded = io::loadAls(fixture("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    model::AutomationEnvelope env;
    env.targetId = 999;
    env.points = {{0, 0.25}, {4, 0.75}};
    history.perform(song, model::setAutomation(song, 0, env));
    auto clip = song.tracks[0].arrangementClips[0];
    clip.xmlId = -1;
    clip.cloneOfXmlId = 0;
    clip.cloneOfTrackId = song.tracks[0].id;
    history.perform(song, model::setSessionClip(song, 0, 0, clip));
    juce::TemporaryFile first(".als"), second(".als");
    REQUIRE(io::saveAls(song, first.getFile()).ok);
    song.sourcePath = first.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    history.undo(song);
    history.undo(song);
    REQUIRE(io::saveAls(song, second.getFile()).ok);
    auto again = io::loadAls(second.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].automation.empty());
    CHECK(again.song->tracks[0].slots.empty());
    CHECK(again.song->tracks[1].slots.empty());
    CHECK(again.song->scenes.empty());
}

TEST_CASE("als writer: save undo redo retains deleted clip templates")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    const auto original = song.tracks[0].arrangementClips[0];
    history.perform(song, model::deleteClip(song, 0, original.uid));
    juce::TemporaryFile saved(".als");
    auto save = [&] {
        const auto result = io::saveAls(song, saved.getFile());
        song.sourcePath = saved.getFile().getFullPathName().toStdString();
        io::refreshClipIdentities(song);
        return result.ok;
    };
    REQUIRE(save());
    history.undo(song);
    REQUIRE(save());
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].arrangementClips.size() == 1);
    CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == original.notes.size());
    history.redo(song);
    REQUIRE(save());
    again = io::loadAls(saved.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].arrangementClips.empty());
}

TEST_CASE("als writer: reordered clips retain their own unknown XML through save undo redo")
{
    std::string error;
    auto root = io::readAlsDom(fixture("minimal_set.xml"), error);
    REQUIRE(root != nullptr);
    auto* events = root->getChildByName("LiveSet")->getChildByName("Tracks")->getFirstChildElement()
        ->getChildByName("DeviceChain")->getChildByName("MainSequencer")->getChildByName("ClipTimeable")
        ->getChildByName("ArrangerAutomation")->getChildByName("Events");
    auto* first = events->getChildByName("MidiClip");
    first->createNewChildElement("UnparsedMetadata")->setAttribute("Value", "first");
    auto second = std::make_unique<juce::XmlElement>(*first);
    second->setAttribute("Id", 42);
    second->setAttribute("Time", 20);
    second->getChildByName("CurrentStart")->setAttribute("Value", 20);
    second->getChildByName("CurrentEnd")->setAttribute("Value", 28);
    second->getChildByName("UnparsedMetadata")->setAttribute("Value", "second");
    events->addChildElement(second.release());
    auto loaded = io::parseAls(*root, fixture("minimal_set.xml").getFullPathName().toStdString());
    REQUIRE(loaded);
    juce::TemporaryFile saved(".xml");
    REQUIRE(saved.getFile().replaceWithText(root->toString()));
    auto& song = *loaded.song;
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    model::EditHistory history;
    const int uid = song.tracks[0].arrangementClips[0].uid;
    history.perform(song, model::modifyClip(song, 0, uid,
        model::movedClip(song.tracks[0].arrangementClips[0], 32), "Move"));
    auto checkSave = [&](bool moved) {
        REQUIRE(io::saveAls(song, saved.getFile()).ok);
        io::refreshClipIdentities(song);
        auto dom = io::readAlsDom(saved.getFile(), error);
        REQUIRE(dom != nullptr);
        auto reread = io::parseAls(*dom, song.sourcePath);
        REQUIRE(reread);
        REQUIRE(reread.song->tracks[0].arrangementClips.size() == 2);
        auto* es = dom->getChildByName("LiveSet")->getChildByName("Tracks")->getFirstChildElement()
            ->getChildByName("DeviceChain")->getChildByName("MainSequencer")->getChildByName("ClipTimeable")
            ->getChildByName("ArrangerAutomation")->getChildByName("Events");
        CHECK(es->getFirstChildElement()->getChildByName("UnparsedMetadata")->getStringAttribute("Value")
              == (moved ? "second" : "first"));
        CHECK(es->getFirstChildElement()->getNextElement()->getChildByName("UnparsedMetadata")
                  ->getStringAttribute("Value") == (moved ? "first" : "second"));
    };
    checkSave(true);
    history.undo(song);
    checkSave(false);
    history.redo(song);
    checkSave(true);
}

TEST_CASE("als writer: undo a saved audio replacement restores source warp metadata")
{
    auto loaded = io::loadAls(fixture("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto original = song.tracks[1].arrangementClips[0];
    auto take = original;
    take.sample.absolutePath = "/tmp/new-take.wav";
    take.sample.relativePath.clear();
    take.warped = false;
    take.warpMarkers.clear();
    model::EditHistory history;
    history.perform(song, model::modifyClip(song, 1, original.uid, take, "Replace Audio"));
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    history.undo(song);
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    const auto& restored = again.song->tracks[1].arrangementClips[0];
    CHECK(restored.sample.absolutePath == original.sample.absolutePath);
    CHECK(restored.warped);
    CHECK(restored.warpMarkers.size() == original.warpMarkers.size());
}

TEST_CASE("als writer: a clip without a save template fails without replacing the destination")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    model::EditHistory history;
    history.perform(*loaded.song, model::insertClip(0, {}, "Invalid Clip"));
    juce::TemporaryFile saved(".als");
    REQUIRE(saved.getFile().replaceWithText("existing destination"));
    const auto result = io::saveAls(*loaded.song, saved.getFile());
    CHECK(! result.ok);
    CHECK(! result.error.empty());
    CHECK(saved.getFile().loadFileAsString() == "existing destination");
}

TEST_CASE("als writer: inserted native and VST3 devices survive saves undo and redo")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto originalCount = song.tracks[0].devices.size();
    model::Device native;
    native.id = 12345;
    native.type = "StereoGain";
    native.displayName = "Utility";
    native.parameters = {{"Gain", 0.25, -1, {}}, {"Balance", 0.5, -1, {}}};
    native.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(native));
    model::Device plugin;
    plugin.id = 12346;
    plugin.type = "PluginDevice";
    plugin.kind = model::DeviceKind::PluginVst3;
    plugin.displayName = "Test VST3";
    plugin.plugin.emplace();
    plugin.plugin->name = plugin.displayName;
    plugin.plugin->vst3Uid = {0x1234, 0x5678, 0x9abc, 0xdef0};
    plugin.plugin->state = {1, 2, 3, 4};
    plugin.plugin->controllerState = {5, 6};
    plugin.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(plugin));
    model::EditHistory history;
    history.perform(song, model::compose("Insert Devices", {model::insertDevice(0, native), model::insertDevice(0, plugin)}));
    juce::TemporaryFile saved(".als");
    auto save = [&] {
        const auto result = io::saveAls(song, saved.getFile());
        song.sourcePath = saved.getFile().getFullPathName().toStdString();
        io::refreshClipIdentities(song);
        return result.ok;
    };
    REQUIRE(save());
    // Runtime state changes are patched without rebuilding unrelated device XML.
    song.tracks[0].devices.back().plugin->state = {8, 7, 6};
    REQUIRE(save());
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].devices.size() == originalCount + 2);
    CHECK(again.song->tracks[0].devices[originalCount].enabled);
    CHECK_NEAR(again.song->tracks[0].devices[originalCount].param("Gain", 0), 0.25, 1e-9);
    CHECK(again.song->tracks[0].devices.back().plugin->state == std::vector<uint8_t>({8, 7, 6}));
    CHECK(again.song->tracks[0].devices.back().plugin->controllerState == plugin.plugin->controllerState);
    auto pluginXml = juce::XmlDocument::parse(juce::String(io::deviceTemplate(plugin)));
    REQUIRE(pluginXml != nullptr);
    auto* info = pluginXml->getChildByName("PluginDesc")->getChildByName("Vst3PluginInfo");
    CHECK(info->hasAttribute("Id"));
    CHECK(info->getChildByName("Preset")->getChildByName("Vst3Preset")->hasAttribute("Id"));
    history.undo(song);
    REQUIRE(save());
    again = io::loadAls(saved.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].devices.size() == originalCount);
    history.redo(song);
    REQUIRE(save());
    again = io::loadAls(saved.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].devices.size() == originalCount + 2);
    CHECK(again.song->tracks[0].devices.back().plugin->state == std::vector<uint8_t>({8, 7, 6}));
}

TEST_CASE("als writer: authored sample templates support empty audio tracks and session replacement undo")
{
    auto loaded = io::loadAls(fixture("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    model::Clip sample;
    sample.kind = model::Clip::Kind::Audio;
    sample.sample.absolutePath = "/tmp/dropped.wav";
    sample.sampleRate = 48000;
    sample.lengthSamples = 96000;
    sample.currentStart = sample.time = 8;
    sample.currentEnd = 12;
    sample.loop = {0, 2, 0, false};
    sample.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(sample));
    model::EditHistory history;
    history.perform(song, model::compose("Drop sample", {model::insertClip(1, sample, "Insert Sample"), model::setSessionClip(song, 1, 0, sample)}));
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[1].arrangementClips.size() == 2);
    const auto& clip = again.song->tracks[1].arrangementClips.back();
    CHECK(clip.sample.absolutePath == sample.sample.absolutePath);
    CHECK_NEAR(clip.loop.loopEnd, 2, 1e-9);
    REQUIRE(clip.warpMarkers.size() == 2);
    CHECK_NEAR(clip.warpMarkers.back().seconds, 2, 1e-9);
    auto xml = juce::XmlDocument::parse(juce::String(*sample.sourceXml));
    REQUIRE(xml != nullptr);
    CHECK(xml->getChildByName("SampleRef")->getChildByName("DefaultSampleRate")->getStringAttribute("Value") == "48000");
    REQUIRE(again.song->tracks[1].slots[0].clip);
    CHECK_NEAR(again.song->tracks[1].slots[0].clip->currentStart, 0, 1e-9);
    CHECK_NEAR(again.song->tracks[1].slots[0].clip->currentEnd, 4, 1e-9);
    history.undo(song);
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    history.redo(song);
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
}

TEST_CASE("als writer: Save As preserves relative sample origins across undo and another save")
{
    juce::TemporaryFile holder;
    const auto directory = holder.getFile();
    REQUIRE(directory.createDirectory());
    const auto source = directory.getChildFile("source.xml");
    REQUIRE(fixture("recording_set.xml").copyFileTo(source));
    const auto media = directory.getChildFile("old.wav");
    REQUIRE(media.replaceWithText("original media")); // parser/path test; no audio decoding
    const auto newDirectory = directory.getChildFile("New Project");
    REQUIRE(newDirectory.createDirectory());
    REQUIRE(newDirectory.getChildFile("old.wav").replaceWithText("different media"));
    auto loaded = io::loadAls(source);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto original = song.tracks[1].arrangementClips[0];
    CHECK(io::resolveSamplePath(song, original.sample) == media.getFullPathName().toStdString());
    model::EditHistory history;
    auto changed = original;
    changed.name = "Edited before Save As";
    history.perform(song, model::modifyClip(song, 1, original.uid, changed, "Rename"));
    const auto saved = newDirectory.getChildFile("saved.als");
    REQUIRE(io::saveAls(song, saved, true).ok);
    song.sourcePath = saved.getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    CHECK(io::resolveSamplePath(song, song.tracks[1].arrangementClips[0].sample) == media.getFullPathName().toStdString());
    history.undo(song);
    REQUIRE(io::saveAls(song, saved, true).ok);
    auto again = io::loadAls(saved);
    REQUIRE(again);
    const auto& clip = again.song->tracks[1].arrangementClips[0];
    CHECK(clip.name == original.name);
    CHECK(clip.sample.relativePath.empty());
    CHECK(clip.sample.absolutePath == media.getFullPathName().toStdString());
    CHECK(io::resolveSamplePath(*again.song, clip.sample) == media.getFullPathName().toStdString());
    history.redo(song);
    REQUIRE(io::saveAls(song, saved, true).ok);
    directory.deleteRecursively();
}

TEST_CASE("als writer: cross-track move and duplicate survive save undo redo cycles")
{
    auto loaded = io::loadAls(fixture("two_tracks.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto original = song.tracks[0].arrangementClips[0];
    model::EditHistory history;
    history.perform(song, model::moveClipToTrack(song, 0, original.uid, 1, 16));
    const int movedUid = song.tracks[1].arrangementClips[0].uid;
    history.perform(song, model::duplicateClip(song, 1, movedUid));
    juce::TemporaryFile saved(".als");
    auto save = [&] {
        const auto result = io::saveAls(song, saved.getFile());
        song.sourcePath = saved.getFile().getFullPathName().toStdString();
        io::refreshClipIdentities(song);
        return result.ok;
    };
    REQUIRE(save());
    history.undo(song); history.undo(song);
    REQUIRE(save());
    auto again = io::loadAls(saved.getFile());
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].arrangementClips.size() == 1);
    CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == original.notes.size());
    CHECK(again.song->tracks[1].arrangementClips.empty());
    history.redo(song); history.redo(song);
    REQUIRE(save());
    again = io::loadAls(saved.getFile());
    REQUIRE(again);
    CHECK(again.song->tracks[0].arrangementClips.empty());
    REQUIRE(again.song->tracks[1].arrangementClips.size() == 2);
    CHECK_NEAR(again.song->tracks[1].arrangementClips[0].currentStart, 16, 1e-9);
    CHECK(again.song->tracks[1].arrangementClips[1].notes.size() == original.notes.size());
}

TEST_CASE("als writer: a failed backup prevents overwriting the source")
{
    juce::TemporaryFile holder;
    const auto directory = holder.getFile();
    REQUIRE(directory.createDirectory());
    const auto source = directory.getChildFile("source.xml");
    REQUIRE(fixture("minimal_set.xml").copyFileTo(source));
    const auto original = source.loadFileAsString();
    REQUIRE(directory.getChildFile("Backup").replaceWithText("blocks the backup directory"));
    auto loaded = io::loadAls(source);
    REQUIRE(loaded);
    loaded.song->tempo = 155;
    const auto result = io::saveAls(*loaded.song, source);
    CHECK(! result.ok);
    CHECK(result.error.find("backup") != std::string::npos);
    CHECK(source.loadFileAsString() == original);
    directory.deleteRecursively();
}

TEST_CASE("als: concurrent readers and edits share unique clip identities")
{
    std::string error;
    const auto file = fixture("two_tracks.xml");
    auto root = io::readAlsDom(file, error);
    REQUIRE(root != nullptr);
    std::array<std::vector<int>, 4> batches;
    std::array<std::thread, 4> workers;
    std::atomic<bool> failed{false};
    for (size_t i = 0; i < workers.size(); ++i)
        workers[i] = std::thread([&, i] {
            for (int n = 0; n < 8; ++n)
            {
                auto loaded = io::parseAls(*root, file.getFullPathName().toStdString());
                if (! loaded) { failed.store(true); return; }
                for (const auto& track : loaded.song->tracks)
                    for (const auto& clip : track.arrangementClips) batches[i].push_back(clip.uid);
            }
        });
    std::vector<int> allocated;
    for (int i = 0; i < 64; ++i) allocated.push_back(model::allocateClipUid());
    for (auto& worker : workers) worker.join();
    CHECK(! failed.load());
    for (const auto& batch : batches) allocated.insert(allocated.end(), batch.begin(), batch.end());
    CHECK(allocated.size() > 64);
    CHECK(std::set<int>(allocated.begin(), allocated.end()).size() == allocated.size());
}

TEST_CASE("als writer: fresh clip content resets inherited end markers across saves")
{
    auto loaded = io::loadAls(fixture("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    auto clip = song.tracks[1].arrangementClips[0];
    auto source = juce::XmlDocument::parse(juce::String(*clip.sourceXml));
    REQUIRE(source != nullptr);
    source->getChildByName("Loop")->createNewChildElement("OutMarker")->setAttribute("Value", 99);
    clip.sourceXml = std::make_shared<const std::string>(source->toString().toStdString());
    clip.resetSourceMarkers = true;
    clip.loop.loopEnd = 1.5;
    model::EditHistory history;
    history.perform(song, model::setSessionClip(song, 1, 0, clip));
    juce::TemporaryFile saved(".als");
    for (int n = 0; n < 2; ++n)
    {
        REQUIRE(io::saveAls(song, saved.getFile()).ok);
        song.sourcePath = saved.getFile().getFullPathName().toStdString();
        io::refreshClipIdentities(song);
        std::string error;
        auto dom = io::readAlsDom(saved.getFile(), error);
        REQUIRE(dom != nullptr);
        auto* track = dom->getChildByName("LiveSet")->getChildByName("Tracks")->getFirstChildElement()->getNextElement();
        auto* slot = track->getChildByName("DeviceChain")->getChildByName("MainSequencer")->getChildByName("ClipSlotList")->getFirstChildElement();
        auto* loop = slot->getChildByName("ClipSlot")->getChildByName("Value")->getChildByName("AudioClip")->getChildByName("Loop");
        CHECK_NEAR(loop->getChildByName("OutMarker")->getDoubleAttribute("Value"), 1.5, 1e-9);
    }
}

TEST_CASE("input settings: author missing routing and monitoring, save undo redo without disturbing clips")
{
    auto loaded = io::loadAls(fixture("recording_set.xml"));
    REQUIRE(loaded);
    auto& song = *loaded.song;
    CHECK(! song.tracks[0].inputRoutingPresent);
    model::EditHistory history;
    history.perform(song, model::setTrackInput(song, 0, "MidiIn/External.All/1", "Ext: All Ins", "2", 0));
    history.perform(song, model::setTrackInput(song, 1, "AudioIn/External/M1", "Ext. In", "2", 2));
    juce::TemporaryFile saved(".als");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    io::refreshClipIdentities(song);
    auto again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[0].inputRouting == "MidiIn/External.All/1");
    CHECK(again.song->tracks[0].monitoring == 0);
    CHECK(again.song->tracks[1].inputRouting == "AudioIn/External/M1");
    CHECK(again.song->tracks[1].monitoring == 2);
    REQUIRE(history.undo(song)); REQUIRE(history.undo(song));
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(! again.song->tracks[0].inputRoutingPresent);
    CHECK(! again.song->tracks[1].monitoringPresent);
    CHECK(again.song->tracks[0].arrangementClips[0].notes.size() == loaded.song->tracks[0].arrangementClips[0].notes.size());
    REQUIRE(history.redo(song)); REQUIRE(history.redo(song));
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[1].inputLowerDisplay == "2");
    CHECK(again.song->tracks[1].monitoring == 2);
}

TEST_CASE("punch flags: retained XML and authored fields survive save undo redo")
{
    auto loaded = io::loadAls(fixture("minimal_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    history.perform(song, model::setPunch(song, true, true));
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->punchIn); CHECK(again.song->punchOut);
    CHECK(again.song->punchInPresent); CHECK(again.song->punchOutPresent);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(! again.song->punchIn); CHECK(! again.song->punchOut);
    CHECK(again.song->punchInPresent == song.punchInPresent); CHECK(again.song->punchOutPresent == song.punchOutPresent);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(again.song->punchIn && again.song->punchOut);
}

TEST_CASE("take lanes: parse independent clips, append, save undo redo and preserve unknown metadata")
{
    std::string error;
    auto dom = io::readAlsDom(fixture("recording_set.xml"), error); REQUIRE(dom);
    auto* track = dom->getChildByName("LiveSet")->getChildByName("Tracks")->getFirstChildElement();
    auto* lanes = track->createNewChildElement("TakeLanes");
    auto* list = lanes->createNewChildElement("TakeLanes");
    lanes->createNewChildElement("AreTakeLanesFolded")->setAttribute("Value", "true");
    lanes->createNewChildElement("FutureContainer")->setAttribute("Value", "retain");
    auto* lane = list->createNewChildElement("TakeLane"); lane->setAttribute("Id", 7);
    lane->createNewChildElement("Name")->setAttribute("Value", "Original take");
    lane->createNewChildElement("Height")->setAttribute("Value", 51);
    lane->createNewChildElement("FutureLane")->setAttribute("Value", "retain");
    auto* events = lane->createNewChildElement("ClipAutomation")->createNewChildElement("Events");
    events->createNewChildElement("FutureEvent")->setAttribute("Value", "retain");
    auto* original = track->getChildByName("DeviceChain")->getChildByName("MainSequencer")->getChildByName("ClipTimeable")->getChildByName("ArrangerAutomation")->getChildByName("Events")->getFirstChildElement();
    events->addChildElement(new juce::XmlElement(*original));
    auto loaded = io::parseAls(*dom, fixture("recording_set.xml").getFullPathName().toStdString()); REQUIRE(loaded);
    auto& song = *loaded.song; REQUIRE(song.tracks[0].takeLanes.size() == 1);
    CHECK(song.tracks[0].takeLanes[0].id == 7); CHECK(song.tracks[0].takeLanes[0].name == "Original take");
    REQUIRE(song.tracks[0].takeLanes[0].clips.size() == 1);
    CHECK(song.tracks[0].takeLanes[0].clips[0].uid != song.tracks[0].arrangementClips[0].uid);
    CHECK(song.tracks[0].nextTakeLaneId == 8);
    model::TakeLane added; added.id = 7; added.name = "New take"; added.clips = {song.tracks[0].arrangementClips[0]};
    added.clips[0].notes = {{0, 1, 61, 99}};
    model::EditHistory history; history.perform(song, model::appendTakeLanes(song, 0, {added}));
    const int uid = song.tracks[0].takeLanes.back().clips[0].uid;
    CHECK(song.tracks[0].takeLanes.back().id == 8);
    juce::TemporaryFile source(".als"); REQUIRE(dom->writeTo(source.getFile()));
    song.sourcePath = source.getFile().getFullPathName().toStdString();
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    auto again = io::loadAls(saved.getFile()); REQUIRE(again); REQUIRE(again.song->tracks[0].takeLanes.size() == 2);
    REQUIRE(again.song->tracks[0].takeLanes.back().clips[0].notes.size() == 1);
    CHECK(again.song->tracks[0].takeLanes.back().clips[0].notes[0].pitch == 61);
    auto roundtrip = io::readAlsDom(saved.getFile(), error); REQUIRE(roundtrip);
    CHECK(roundtrip->toString().contains("FutureContainer")); CHECK(roundtrip->toString().contains("FutureLane")); CHECK(roundtrip->toString().contains("FutureEvent"));
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(again.song->tracks[0].takeLanes.size() == 1);
    REQUIRE(history.redo(song)); CHECK(song.tracks[0].takeLanes.back().clips[0].uid == uid);
    REQUIRE(io::saveAls(song, saved.getFile()).ok); again = io::loadAls(saved.getFile()); REQUIRE(again);
    CHECK(again.song->tracks[0].takeLanes.size() == 2);
    auto broken = added; broken.clips[0].sourceXml.reset();
    history.perform(song, model::appendTakeLanes(song, 0, {broken}));
    juce::MemoryBlock before; REQUIRE(saved.getFile().loadFileAsData(before));
    CHECK(! io::saveAls(song, saved.getFile()).ok);
    juce::MemoryBlock after; REQUIRE(saved.getFile().loadFileAsData(after)); CHECK(before == after);
}

TEST_CASE("take lanes: Save As anchors original sample origins and undo removes an authored container")
{
    auto loaded = io::loadAls(fixture("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto base = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("sonora-take-origin", "", false);
    const auto oldFolder = base.getChildFile("old"), newFolder = base.getChildFile("new");
    REQUIRE(oldFolder.createDirectory()); REQUIRE(newFolder.createDirectory());
    const auto original = oldFolder.getChildFile("take.wav"), impostor = newFolder.getChildFile("take.wav");
    REQUIRE(original.replaceWithText("original")); REQUIRE(impostor.replaceWithText("impostor"));
    auto clip = song.tracks[1].arrangementClips[0];
    clip.sample.absolutePath.clear(); clip.sample.relativePath = "take.wav"; clip.sample.relativePathType = 3;
    clip.sample.sourceDirectory = oldFolder.getFullPathName().toStdString();
    auto retained = juce::XmlDocument::parse(juce::String(io::audioClipTemplate(clip))); REQUIRE(retained);
    auto* ref = retained->getChildByName("SampleRef")->getChildByName("FileRef");
    ref->getChildByName("RelativePath")->setAttribute("Value", "take.wav");
    ref->getChildByName("RelativePathType")->setAttribute("Value", 3);
    clip.sourceXml = std::make_shared<const std::string>(retained->toString().toStdString());
    model::TakeLane lane; lane.name = "Recorded audio"; lane.clips = {clip};
    model::EditHistory history; history.perform(song, model::appendTakeLanes(song, 1, {lane}));
    const auto saved = newFolder.getChildFile("copy.als"); REQUIRE(io::saveAls(song, saved, true).ok);
    song.sourcePath = saved.getFullPathName().toStdString();
    auto again = io::loadAls(saved); REQUIRE(again); REQUIRE(again.song->tracks[1].takeLanes.size() == 1);
    CHECK(io::resolveSamplePath(*again.song, again.song->tracks[1].takeLanes[0].clips[0].sample) == original.getFullPathName().toStdString());
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved, true).ok);
    again = io::loadAls(saved); REQUIRE(again); CHECK(again.song->tracks[1].takeLanes.empty());
    std::string error; auto dom = io::readAlsDom(saved, error); REQUIRE(dom);
    auto* tracks = dom->getChildByName("LiveSet")->getChildByName("Tracks");
    CHECK(tracks->getFirstChildElement()->getNextElement()->getChildByName("TakeLanes") == nullptr);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, saved, true).ok);
    again = io::loadAls(saved); REQUIRE(again); CHECK(again.song->tracks[1].takeLanes.size() == 1);
    base.deleteRecursively();
}

TEST_CASE("take editing: names order audition and independent comp edits survive save undo reload")
{
    auto loaded = io::loadAls(fixture("recording_set.xml")); REQUIRE(loaded);
    auto& song = *loaded.song; model::EditHistory history;
    song.tracks[0].arrangementClips[0].notes = {{0, 2, 60, 100}};
    auto clip = song.tracks[0].arrangementClips[0]; clip.takeId = 12;
    model::TakeLane lane; lane.name = "Original"; lane.clips = {clip};
    history.perform(song, model::appendTakeLanes(song, 0, {lane, lane}));
    const int id = song.tracks[0].takeLanes[0].id, second = song.tracks[0].takeLanes[1].id;
    const auto address = model::ClipLocation::take(0, song.tracks[0].takeLanes[0].clips[0].uid, id);
    history.perform(song, model::renameTakeLane(song, 0, id, "Chosen ♬"));
    history.perform(song, model::moveTakeLane(song, 0, id, 1));
    history.perform(song, model::setNotes(song, address, {{0, 2, 73, 88}}));
    history.perform(song, model::auditionTakeLane(song, 0, id));
    history.perform(song, model::compTakeRange(song, 0, id, 0.5, 1.5));
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    song.sourcePath = saved.getFile().getFullPathName().toStdString();
    auto again = io::loadAls(saved.getFile()); REQUIRE(again);
    REQUIRE(again.song->tracks[0].takeLanes.size() == 2);
    CHECK(again.song->tracks[0].takeLanes[0].id == second);
    const auto* parsed = model::findTakeLane(*again.song, 0, id); REQUIRE(parsed);
    CHECK(parsed->name == "Chosen ♬"); CHECK(parsed->audition); CHECK(parsed->clips[0].notes[0].pitch == 73);
    CHECK(again.song->tracks[0].arrangementClips[1].takeId == 12);
    CHECK_NEAR(again.song->tracks[0].arrangementClips[1].loop.startRelative, 0.5, 1e-9);
    const auto originalPitch = clip.notes[0].pitch;
    CHECK(again.song->tracks[0].arrangementClips[0].notes[0].pitch == originalPitch);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(again.song->tracks[0].arrangementClips.size() == 1);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(! model::findTakeLane(*again.song, 0, id)->audition);
    REQUIRE(history.redo(song)); REQUIRE(history.redo(song));
    history.perform(song, model::deleteTakeLane(song, 0, id)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(! model::findTakeLane(*again.song, 0, id));
    CHECK(again.song->tracks[0].arrangementClips[1].takeId == 12); // deleting a source does not delete its comp copy
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    again = io::loadAls(saved.getFile()); REQUIRE(again); CHECK(model::findTakeLane(*again.song, 0, id)->audition);
}

#include "devices/DeviceFactory.h"
#include "core/DeviceCatalog.h"

TEST_CASE("device chain order and instrument replacement: save reload undo redo preserve IDs state and automation")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded); auto& song = *loaded.song;
    const auto oldInstrument = song.tracks[0].devices[0];
    const auto eq = song.tracks[0].devices[1];
    model::EditHistory history;
    for (const auto* type : {"Delay", "Reverb"})
    {
        auto device = devices::nativeDeviceModel(type); model::allocateDeviceIds(song, device);
        device.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(device));
        history.perform(song, model::insertTrackDevice(song, 0, std::move(device)));
    }
    history.clear();
    const int reverbId = song.tracks[0].devices[3].id;
    history.perform(song, model::moveAudioEffect(song, 0, reverbId, 1));
    juce::TemporaryFile file(".als");
    auto reload = [&]() {
        REQUIRE(io::saveAls(song, file.getFile()).ok);
        song.sourcePath = file.getFile().getFullPathName().toStdString();
        auto read = io::loadAls(file.getFile()); REQUIRE(read); return std::move(read.song);
    };
    auto read = reload();
    CHECK(read->tracks[0].devices[1].id == reverbId);
    CHECK(read->tracks[0].devices[2].id == eq.id);
    CHECK(read->tracks[0].devices[2].enabledTargetId == eq.enabledTargetId);
    CHECK(read->tracks[0].devices[2].parameters[0].automationTargetId == eq.parameters[0].automationTargetId);
    CHECK(read->tracks[0].devices[0].plugin->state == oldInstrument.plugin->state);
    auto replacement = devices::nativeDeviceModel("OriginalSimpler"); model::allocateDeviceIds(song, replacement);
    const int replacementId = replacement.id;
    replacement.sourceXml = std::make_shared<const std::string>(io::deviceTemplate(replacement));
    history.perform(song, model::insertTrackDevice(song, 0, replacement));
    read = reload(); CHECK(read->tracks[0].devices[0].id == replacementId);
    CHECK(read->tracks[0].devices.size() == 4); CHECK(read->tracks[0].devices[1].id == reverbId);
    REQUIRE(history.undo(song)); read = reload();
    CHECK(read->tracks[0].devices[0].id == oldInstrument.id);
    CHECK(read->tracks[0].devices[0].plugin->state == oldInstrument.plugin->state);
    REQUIRE(history.undo(song)); read = reload(); CHECK(read->tracks[0].devices[1].id == eq.id);
    REQUIRE(history.redo(song)); REQUIRE(history.redo(song)); read = reload();
    CHECK(read->tracks[0].devices[0].id == replacementId); CHECK(read->tracks[0].devices[1].id == reverbId);
}

#include "core/Scales.h"

TEST_CASE("scale: set and clip scales read, save and reload; edits undo; new clips take the set scale (UI-30b)")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded);
    auto song = *loaded.song;
    CHECK(song.scaleRoot == 0); CHECK(song.scaleName == 0); CHECK(! song.inKey); // the fixture has none

    model::EditHistory history;
    auto command = model::setScale(song, 2, 2); // D Dorian
    REQUIRE(command.apply);
    history.perform(song, std::move(command));
    CHECK(song.scaleRoot == 2); CHECK(song.scaleName == 2);
    CHECK(history.undoLabel() == "Change Scale");
    CHECK(! model::setScale(song, 14, 2).apply); // 14 == 2 (mod 12): unchanged, no history entry
    history.undo(song);
    CHECK(song.scaleRoot == 0); CHECK(song.scaleName == 0);
    history.redo(song);
    song.inKey = true;
    REQUIRE(! song.tracks.empty() && ! song.tracks[0].arrangementClips.empty());
    auto& clip = song.tracks[0].arrangementClips[0];
    clip.scaleRoot = 9; clip.scaleName = 24; clip.inKey = true; // A Hirajoshi

    juce::TemporaryFile saved(".xml");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    const auto xml = saved.getFile().loadFileAsString();
    CHECK(xml.contains("<InKey Value=\"true\""));
    auto reloaded = io::loadAls(saved.getFile());
    REQUIRE(reloaded);
    CHECK(reloaded.song->scaleRoot == 2); CHECK(reloaded.song->scaleName == 2); CHECK(reloaded.song->inKey);
    const auto& back = reloaded.song->tracks[0].arrangementClips[0];
    CHECK(back.scaleRoot == 9); CHECK(back.scaleName == 24); CHECK(back.inKey);

    model::Clip fresh;
    REQUIRE(model::makeEmptyMidiClip(*reloaded.song, 16, 4, fresh));
    CHECK(fresh.scaleRoot == 2); CHECK(fresh.scaleName == 2); CHECK(fresh.inKey);

    // Scale membership and degrees follow the whole table.
    CHECK(model::inScale(62 + 3, 2, 2));   // F in D Dorian
    CHECK(! model::inScale(62 + 4, 2, 2)); // F# is not
    CHECK(model::inScale(69 + 7, 9, 24));  // E in A Hirajoshi (0 2 3 7 8)
    CHECK(! model::inScale(69 + 5, 9, 24));
    CHECK(model::scaleAt(99).name == "Major"); // unknown index (e.g. a custom scale) falls back
}

TEST_CASE("scale: Live 11 sets store RootNote and the scale's name; both are read and kept in that layout")
{
    auto xml = juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml").loadFileAsString();
    const int end = xml.lastIndexOf("</LiveSet>");
    REQUIRE(end > 0);
    xml = xml.substring(0, end) + "<ScaleInformation><RootNote Value=\"4\" /><Name Value=\"Dorian\" /></ScaleInformation>\n" + xml.substring(end);
    juce::TemporaryFile source(".xml");
    REQUIRE(source.getFile().replaceWithText(xml));
    auto loaded = io::loadAls(source.getFile());
    REQUIRE(loaded);
    CHECK(loaded.song->scaleRoot == 4);
    CHECK(loaded.song->scaleName == 2);
    auto song = *loaded.song;
    song.scaleName = model::scaleIndexByName("phrygian");
    CHECK(song.scaleName == 5);
    juce::TemporaryFile saved(".xml");
    REQUIRE(io::saveAls(song, saved.getFile()).ok);
    const auto written = saved.getFile().loadFileAsString();
    CHECK(written.contains("<RootNote Value=\"4\""));
    CHECK(written.contains("<Name Value=\"Phrygian\""));
    CHECK(! written.contains("<Root Value=")); // no Live 12 children mixed in
}
