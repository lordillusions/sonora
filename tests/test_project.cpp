#include "TestFramework.h"
#include "app/SaveChanges.h"

#include "core/Edit.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <set>

using namespace sonora;

namespace
{
juce::File freshDirectory(const char* name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_project_tests").getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

std::unique_ptr<model::Song> writeNewSet(const juce::File& file)
{
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), file, error));
    auto loaded = io::loadAls(file);
    REQUIRE((bool) loaded);
    loaded.song->midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
    return std::move(loaded.song);
}

void collectPointeeIds(const juce::XmlElement& e, std::multiset<int>& ids)
{
    if (e.hasTagName("AutomationTarget") || e.hasTagName("ModulationTarget") || e.hasTagName("Pointee")
        || e.getTagName().startsWith("ControllerTargets."))
        ids.insert(e.getIntAttribute("Id", -1));
    for (const auto* child : e.getChildIterator())
        collectPointeeIds(*child, ids);
}
} // namespace

TEST_CASE("save changes: native Save waits for successful Save As before continuing to quit")
{
    const auto dir = freshDirectory("save-before-quit");
    auto song = writeNewSet(dir.getChildFile("Untitled.als"));
    song->tempo = 137.0;
    const auto destination = dir.getChildFile("Saved Project.als");
    std::function<void(bool)> completion;
    int saveRequests = 0, quitRequests = 0;
    app::finishSaveChanges(0, [&](std::function<void(bool)> done) {
        ++saveRequests;
        completion = std::move(done); // the native file chooser is asynchronous
    }, [&] {
        ++quitRequests;
        auto saved = io::loadAls(destination);
        REQUIRE(saved);
        CHECK_NEAR(saved.song->tempo, 137.0, 1e-9);
    });
    CHECK(saveRequests == 1);
    CHECK(quitRequests == 0);
    REQUIRE(completion);
    completion(io::saveAls(*song, destination).ok);
    CHECK(quitRequests == 1);
}

TEST_CASE("save changes: failed or cancelled saves stay open; discard continues without saving")
{
    int saveRequests = 0, quitRequests = 0;
    auto failedSave = [&](std::function<void(bool)> done) { ++saveRequests; done(false); };
    auto quit = [&] { ++quitRequests; };
    app::finishSaveChanges(0, failedSave, quit);
    CHECK(saveRequests == 1); CHECK(quitRequests == 0);
    for (const int cancel : {2, -1})
        app::finishSaveChanges(cancel, failedSave, quit);
    CHECK(saveRequests == 1); CHECK(quitRequests == 0);
    app::finishSaveChanges(1, failedSave, quit);
    CHECK(saveRequests == 1); CHECK(quitRequests == 1);
    app::finishSaveChanges(0, [](std::function<void(bool)> done) { done(true); }, quit);
    CHECK(quitRequests == 2); // an already-named document saves synchronously
}

TEST_CASE("new project: the generated set has Live's default layout and unique automation targets")
{
    const auto dir = freshDirectory("layout");
    const auto song = writeNewSet(dir.getChildFile("Untitled.als"));
    REQUIRE(song != nullptr);
    REQUIRE(song->tracks.size() == 4);
    CHECK(song->tracks[0].kind == model::TrackKind::Midi);
    CHECK(song->tracks[1].kind == model::TrackKind::Midi);
    CHECK(song->tracks[2].kind == model::TrackKind::Audio);
    CHECK(song->tracks[3].kind == model::TrackKind::Audio);
    CHECK(song->returns.size() == 2);
    CHECK(song->scenes.size() == 8);
    CHECK_NEAR(song->tempo, 120.0, 1e-9);
    CHECK(song->timeSigNumerator == 4);
    CHECK(song->timeSigDenominator == 4);
    CHECK(song->tracks[0].slots.size() == 8);
    CHECK(song->tracks[0].mixer.sends.size() == 2);
    CHECK_NEAR(song->tracks[0].mixer.volume, 1.0, 1e-9);
    CHECK(song->tracks[0].mixer.volumeTargetId >= 0);
    CHECK(song->tracks[0].name == "1-MIDI");

    std::set<int> trackIds;
    for (const auto& t : song->tracks) trackIds.insert(t.id);
    for (const auto& t : song->returns) trackIds.insert(t.id);
    CHECK(trackIds.size() == 6);

    std::string error;
    auto dom = io::readAlsDom(dir.getChildFile("Untitled.als"), error);
    REQUIRE(dom != nullptr);
    std::multiset<int> ids;
    collectPointeeIds(*dom, ids);
    const std::set<int> unique(ids.begin(), ids.end());
    CHECK(unique.size() == ids.size());
    CHECK(song->nextPointeeId > *unique.rbegin());
}

TEST_CASE("new project: first MIDI clips come from the authored template and survive save and reload")
{
    const auto dir = freshDirectory("midiclip");
    auto song = writeNewSet(dir.getChildFile("Untitled.als"));
    REQUIRE(song != nullptr);

    model::Clip arrangement;
    REQUIRE(model::makeEmptyMidiClip(*song, 4.0, 8.0, arrangement));
    arrangement.notes.push_back({1.0, 0.5, 60, 100.0f});
    arrangement.name = "Arrangement Clip";
    model::Clip session;
    REQUIRE(model::makeEmptyMidiClip(*song, 0.0, 4.0, session));
    session.name = "Session Clip";

    model::EditHistory history;
    history.perform(*song, model::insertClip(0, arrangement, "Insert MIDI Clip"));
    history.perform(*song, model::setSessionClip(*song, 1, 2, session));

    const auto destination = dir.getChildFile("Saved Project").getChildFile("Saved.als");
    REQUIRE(destination.getParentDirectory().createDirectory());
    io::AlsSaveOptions options;
    options.portableReferences = options.copyProjectSamples = true;
    const auto saved = io::saveAls(*song, destination, options);
    REQUIRE(saved.ok);

    auto again = io::loadAls(destination);
    REQUIRE(again);
    REQUIRE(again.song->tracks[0].arrangementClips.size() == 1);
    const auto& clip = again.song->tracks[0].arrangementClips[0];
    CHECK_NEAR(clip.currentStart, 4.0, 1e-9);
    CHECK_NEAR(clip.currentEnd, 12.0, 1e-9);
    CHECK(clip.name == "Arrangement Clip");
    REQUIRE(clip.notes.size() == 1);
    CHECK(clip.notes[0].pitch == 60);
    REQUIRE(again.song->tracks[1].slots.size() == 8);
    REQUIRE(again.song->tracks[1].slots[2].clip.has_value());
    CHECK(again.song->tracks[1].slots[2].clip->name == "Session Clip");
}

namespace
{
juce::File writeTinyWav(const juce::File& file)
{
    REQUIRE(file.getParentDirectory().createDirectory());
    juce::WavAudioFormat wav;
    {
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
        REQUIRE(stream != nullptr);
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> buffer(1, 441);
        for (int i = 0; i < 441; ++i) buffer.setSample(0, i, std::sin(0.1f * (float) i) * 0.5f);
        writer->writeFromAudioSampleBuffer(buffer, 0, 441);
    }
    return file;
}

// Adds an arrangement audio clip that references `sample` (absolute path) to the first audio track's DOM.
void addAudioClip(juce::XmlElement& root, const juce::File& sample)
{
    auto* tracks = root.getChildByName("LiveSet")->getChildByName("Tracks");
    if (auto* track = tracks->getChildByName("AudioTrack"))
    {
        auto* events = track->getChildByName("DeviceChain")->getChildByName("MainSequencer")->getChildByName("Sample")
                           ->getChildByName("ArrangerAutomation")->getChildByName("Events");
        auto clip = juce::XmlDocument::parse(juce::String::fromUTF8(io::audioClipTemplate([] {
            model::Clip c;
            c.kind = model::Clip::Kind::Audio;
            c.currentStart = 0;
            c.currentEnd = 4;
            c.loop = {0.0, 4.0, 0.0, false};
            c.name = "Tiny";
            return c;
        }()).c_str()));
        REQUIRE(clip != nullptr);
        auto* ref = clip->getChildByName("SampleRef")->getChildByName("FileRef");
        auto setValue = [&](const char* tag, const juce::String& v) {
            auto* e = ref->getChildByName(tag);
            if (! e) e = ref->createNewChildElement(tag);
            e->setAttribute("Value", v);
        };
        setValue("Path", sample.getFullPathName());
        setValue("RelativePath", "");
        setValue("RelativePathType", "0");
        events->addChildElement(clip.release());
        return;
    }
}
} // namespace


TEST_CASE("collect all and save: external samples move into Samples/Imported and references become project-relative")
{
    const auto root = freshDirectory("collect");
    const auto external = writeTinyWav(root.getChildFile("elsewhere").getChildFile("kick.wav"));
    REQUIRE(external.existsAsFile());

    // A set that references the external sample by absolute path.
    const auto original = root.getChildFile("Original Project").getChildFile("Original.als");
    original.getParentDirectory().createDirectory();
    auto dom = io::createNewSetDom();
    addAudioClip(*dom, external);
    std::string error;
    REQUIRE(io::writeAlsDom(*dom, original, error));
    auto loaded = io::loadAls(original);
    REQUIRE(loaded);
    REQUIRE(loaded.song->tracks[2].arrangementClips.size() == 1);

    const auto project = root.getChildFile("Collected Project");
    const auto destination = project.getChildFile("Collected.als");
    project.createDirectory();
    io::AlsSaveOptions options;
    options.portableReferences = options.copyProjectSamples = options.collectAll = true;
    const auto result = io::saveAls(*loaded.song, destination, options);
    REQUIRE(result.ok);
    CHECK(result.samplesCopied == 1);
    CHECK(result.samplesMissing == 0);
    CHECK(result.movedSamples.size() == 1);
    const auto copy = project.getChildFile("Samples/Imported/kick.wav");
    CHECK(copy.existsAsFile());
    CHECK(copy.getSize() == external.getSize());
    CHECK(external.existsAsFile()); // collecting copies, never moves

    auto again = io::loadAls(destination);
    REQUIRE(again);
    const auto& ref = again.song->tracks[2].arrangementClips[0].sample;
    CHECK(ref.relativePathType == 3);
    CHECK(ref.relativePath == "Samples/Imported/kick.wav");
    CHECK(io::resolveSamplePath(*again.song, ref) == copy.getFullPathName().toStdString());

    // Collecting twice is idempotent: nothing is copied again and the reference stays put.
    const auto second = io::saveAls(*again.song, destination, options);
    REQUIRE(second.ok);
    CHECK(second.samplesCopied == 0);
    CHECK(second.movedSamples.empty());
}

TEST_CASE("save as keeps a project's recordings with it and references stay relative")
{
    const auto root = freshDirectory("saveas");
    const auto source = root.getChildFile("Scratch Project").getChildFile("Untitled.als");
    source.getParentDirectory().createDirectory();
    const auto recorded = writeTinyWav(source.getParentDirectory().getChildFile("Samples/Recorded/take.wav"));
    REQUIRE(recorded.existsAsFile());

    auto dom = io::createNewSetDom();
    addAudioClip(*dom, recorded);
    // Make the reference project-relative, as the recorder writes it.
    auto* tracks = dom->getChildByName("LiveSet")->getChildByName("Tracks");
    for (auto* ref = tracks->getChildByName("AudioTrack"); ref != nullptr; ref = nullptr)
    {
        std::function<void(juce::XmlElement&)> visit = [&](juce::XmlElement& e) {
            if (e.hasTagName("FileRef"))
            {
                e.getChildByName("RelativePath")->setAttribute("Value", "Samples/Recorded/take.wav");
                e.getChildByName("RelativePathType")->setAttribute("Value", 3);
            }
            for (auto* c : e.getChildIterator()) visit(*c);
        };
        visit(*ref);
    }
    std::string error;
    REQUIRE(io::writeAlsDom(*dom, source, error));
    auto loaded = io::loadAls(source);
    REQUIRE(loaded);

    const auto destination = root.getChildFile("My Song Project").getChildFile("My Song.als");
    destination.getParentDirectory().createDirectory();
    io::AlsSaveOptions options;
    options.portableReferences = options.copyProjectSamples = true;
    const auto result = io::saveAls(*loaded.song, destination, options);
    REQUIRE(result.ok);
    CHECK(result.samplesCopied == 1);
    CHECK(destination.getParentDirectory().getChildFile("Samples/Recorded/take.wav").existsAsFile());

    auto again = io::loadAls(destination);
    REQUIRE(again);
    const auto& ref = again.song->tracks[2].arrangementClips[0].sample;
    CHECK(ref.relativePathType == 3);
    CHECK(ref.relativePath == "Samples/Recorded/take.wav");
    CHECK(! io::resolveSamplePath(*again.song, ref).empty());
}

namespace
{
// Children of the group at `index`: the tracks that follow it with that group id.
std::vector<std::string> childNames(const model::Song& song, int index)
{
    std::vector<std::string> names;
    for (const auto& t : song.tracks)
        if (t.groupId == song.tracks[(size_t) index].id)
            names.push_back(t.name);
    return names;
}
} // namespace

TEST_CASE("track groups: selecting a contiguous range groups it, undo restores routing and order")
{
    const auto dir = freshDirectory("groups");
    auto song = writeNewSet(dir.getChildFile("Groups.als"));
    REQUIRE(song->tracks.size() == 4);
    int first = -1, count = 0;
    CHECK(! model::groupableRange(*song, {0, 2}, first, count));   // not contiguous
    CHECK(! model::groupableRange(*song, {}, first, count));
    CHECK(model::groupableRange(*song, {2, 1}, first, count));
    CHECK(first == 1);
    CHECK(count == 2);

    int pointeeEnd = 0;
    const int pointeeBefore = song->nextPointeeId;
    auto group = io::createGroupTrack(*song, "Group", 5, pointeeEnd);
    CHECK(group.id > 0);
    CHECK(pointeeEnd > pointeeBefore);
    CHECK(group.mixer.volumeTargetId >= pointeeBefore); // its automation ids come from the song's allocator
    CHECK(group.mixer.sends.size() == song->returns.size());
    const std::string secondName = song->tracks[1].name;
    model::EditHistory history;
    history.perform(*song, model::groupTracks(*song, first, count, std::move(group), pointeeEnd));
    REQUIRE(song->tracks.size() == 5);
    CHECK(song->tracks[1].kind == model::TrackKind::Group);
    CHECK(song->tracks[2].name == secondName);
    CHECK(song->tracks[2].groupId == song->tracks[1].id);
    CHECK(song->tracks[3].groupId == song->tracks[1].id);
    CHECK(song->tracks[2].outputRouting == "AudioOut/GroupTrack");
    CHECK(song->tracks[0].groupId == -1);
    CHECK(song->nextPointeeId >= pointeeEnd);
    CHECK(childNames(*song, 1).size() == 2);

    history.undo(*song);
    REQUIRE(song->tracks.size() == 4);
    CHECK(song->tracks[1].name == secondName);
    CHECK(song->tracks[1].groupId == -1);
    CHECK(song->tracks[1].outputRouting == "AudioOut/Main");
    history.redo(*song);
    CHECK(song->tracks.size() == 5);
}

TEST_CASE("track groups: a saved group reloads with its children, output routing and unique ids, and ungrouping saves back")
{
    const auto dir = freshDirectory("groups_save");
    auto song = writeNewSet(dir.getChildFile("Groups.als"));
    model::EditHistory history;
    int pointeeEnd = 0;
    auto group = io::createGroupTrack(*song, "Drums", 7, pointeeEnd);
    history.perform(*song, model::groupTracks(*song, 2, 2, std::move(group), pointeeEnd));

    const auto saved = dir.getChildFile("Saved.als");
    REQUIRE(io::saveAls(*song, saved).ok);
    auto again = io::loadAls(saved);
    REQUIRE((bool) again);
    const auto& tracks = again.song->tracks;
    REQUIRE(tracks.size() == 5);
    CHECK(tracks[2].kind == model::TrackKind::Group);
    CHECK(tracks[2].name == "Drums");
    CHECK(tracks[2].colorIndex == 7);
    CHECK(tracks[3].groupId == tracks[2].id);
    CHECK(tracks[4].groupId == tracks[2].id);
    CHECK(tracks[3].outputRouting == "AudioOut/GroupTrack");
    CHECK(tracks[1].groupId == -1);
    CHECK(tracks[1].outputRouting == "AudioOut/Main");
    CHECK(again.song->returns.size() == 2);
    CHECK(tracks[2].mixer.sends.size() == 2);

    // every automation target and track id in the saved file is unique
    std::string error;
    auto dom = io::readAlsDom(saved, error);
    REQUIRE(dom != nullptr);
    std::multiset<int> ids;
    collectPointeeIds(*dom, ids);
    for (int id : ids) CHECK(ids.count(id) == 1);
    std::set<int> trackIds;
    for (const auto& t : tracks) trackIds.insert(t.id);
    CHECK(trackIds.size() == tracks.size());
    // the scene launch slots of the group: one per scene
    const auto* liveSet = dom->getChildByName("LiveSet");
    const auto* groupX = [&]() -> const juce::XmlElement* {
        for (const auto* tx : liveSet->getChildByName("Tracks")->getChildIterator())
            if (tx->hasTagName("GroupTrack")) return tx;
        return nullptr;
    }();
    REQUIRE(groupX != nullptr);
    int slots = 0;
    for (const auto* slot : groupX->getChildByName("Slots")->getChildIterator()) { (void) slot; ++slots; }
    CHECK(slots == (int) again.song->scenes.size());

    // saving again changes nothing, and the loaded set can be ungrouped and saved
    model::EditHistory second;
    second.perform(*again.song, model::ungroupTrack(*again.song, 2));
    REQUIRE(again.song->tracks.size() == 4);
    CHECK(again.song->tracks[2].groupId == -1);
    CHECK(again.song->tracks[2].outputRouting == "AudioOut/Main");
    const auto flat = dir.getChildFile("Flat.als");
    REQUIRE(io::saveAls(*again.song, flat).ok);
    auto last = io::loadAls(flat);
    REQUIRE((bool) last);
    CHECK(last.song->tracks.size() == 4);
    for (const auto& t : last.song->tracks)
    {
        CHECK(t.groupId == -1);
        CHECK(t.kind != model::TrackKind::Group);
    }

    // undo after the first save: the next save drops the group again (the file is rebuilt from the model)
    history.undo(*song);
    const auto undone = dir.getChildFile("Undone.als");
    REQUIRE(io::saveAls(*song, undone).ok);
    auto reloaded = io::loadAls(undone);
    REQUIRE((bool) reloaded);
    CHECK(reloaded.song->tracks.size() == 4);
}

TEST_CASE("track groups: nested groups need their children, and children of one group can be grouped inside it")
{
    const auto dir = freshDirectory("groups_nested");
    auto song = writeNewSet(dir.getChildFile("Groups.als"));
    model::EditHistory history;
    int pointeeEnd = 0;
    history.perform(*song, model::groupTracks(*song, 0, 4, io::createGroupTrack(*song, "All", 1, pointeeEnd), pointeeEnd));
    REQUIRE(song->tracks.size() == 5);
    int first = 0, count = 0;
    CHECK(! model::groupableRange(*song, {0, 1}, first, count)); // the group without all of its children
    CHECK(model::groupableRange(*song, {0, 1, 2, 3, 4}, first, count));
    CHECK(model::groupableRange(*song, {3, 4}, first, count));     // two children of the same group
    history.perform(*song, model::groupTracks(*song, 3, 2, io::createGroupTrack(*song, "Sub", 2, pointeeEnd), pointeeEnd));
    REQUIRE(song->tracks.size() == 6);
    CHECK(song->tracks[3].kind == model::TrackKind::Group);
    CHECK(song->tracks[3].groupId == song->tracks[0].id); // nested in the first group
    CHECK(song->tracks[4].groupId == song->tracks[3].id);
    CHECK(song->tracks[4].outputRouting == "AudioOut/GroupTrack"); // it played into the outer group, now into the inner one
}

TEST_CASE("track groups: a non-adjacent selection groups, the tracks in between stay out, and the order survives save and reload")
{
    const auto dir = freshDirectory("groups_gap");
    auto song = writeNewSet(dir.getChildFile("Groups.als"));
    std::vector<int> members;
    CHECK(model::groupableSelection(*song, {0, 2}, members));
    CHECK(members == std::vector<int>({0, 2}));
    CHECK(! model::groupableSelection(*song, {}, members));
    std::vector<model::TrackId> ids;
    for (const auto& t : song->tracks) ids.push_back(t.id);
    model::EditHistory history;
    int pointeeEnd = 0;
    history.perform(*song, model::groupTracks(*song, members, io::createGroupTrack(*song, "Gapped", 3, pointeeEnd), pointeeEnd));
    REQUIRE(song->tracks.size() == 5);
    CHECK(song->tracks[1].id == ids[0]);
    CHECK(song->tracks[2].id == ids[2]);
    CHECK(song->tracks[3].id == ids[1]);
    CHECK(song->tracks[4].id == ids[3]);
    CHECK(song->tracks[3].groupId == -1);

    const auto saved = dir.getChildFile("Saved.als");
    REQUIRE(io::saveAls(*song, saved).ok);
    auto again = io::loadAls(saved);
    REQUIRE((bool) again);
    REQUIRE(again.song->tracks.size() == 5);
    CHECK(again.song->tracks[0].kind == model::TrackKind::Group);
    for (size_t i = 1; i < 5; ++i) CHECK(again.song->tracks[i].id == song->tracks[i].id);
    CHECK(again.song->tracks[1].groupId == again.song->tracks[0].id);
    CHECK(again.song->tracks[2].groupId == again.song->tracks[0].id);
    CHECK(again.song->tracks[3].groupId == -1);

    history.undo(*song);
    for (size_t i = 0; i < 4; ++i) CHECK(song->tracks[i].id == ids[i]);
}

TEST_CASE("stems: new audio tracks with one clip each are one undoable edit, save, reload with unique ids and play their sample")
{
    const auto dir = freshDirectory("stems_tracks");
    auto song = writeNewSet(dir.getChildFile("Stems.als"));
    const size_t before = song->tracks.size();
    const auto wav = dir.getChildFile("Samples/Imported/vocals.wav");
    writeTinyWav(wav);

    model::EditHistory history;
    std::vector<model::Track> tracks;
    std::vector<model::EditCommand> parts;
    int next = 0, id = 0;
    for (const auto& t : song->tracks) id = std::max(id, t.id + 1);
    for (const auto& r : song->returns) id = std::max(id, r.id + 1);
    int first = std::max(1, song->nextPointeeId);
    const char* names[] = {"Vocals", "Drums"};
    for (int i = 0; i < 2; ++i)
    {
        tracks.push_back(io::createAudioTrack(*song, names[i], 5 + i, id + i, first, next));
        first = next;
    }
    REQUIRE(tracks[0].kind == model::TrackKind::Audio);
    CHECK(tracks[0].slots.size() == song->scenes.size());
    CHECK(tracks[1].id == tracks[0].id + 1);
    parts.push_back(model::insertTracks(tracks, next, "Insert Tracks"));
    for (int i = 0; i < 2; ++i)
    {
        model::Clip clip;
        clip.kind = model::Clip::Kind::Audio;
        clip.resetSourceMarkers = true;
        clip.name = names[i];
        clip.colorIndex = 5 + i;
        clip.sample.absolutePath = wav.getFullPathName().toStdString();
        clip.sampleRate = 44100.0;
        clip.lengthSamples = 441;
        clip.loop = {0, 0.01, 0, false};
        clip.time = clip.currentStart = 8.0;
        clip.currentEnd = 12.0;
        clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
        parts.push_back(model::insertClip((int) before + i, std::move(clip), "Insert Sample"));
    }
    auto edit = model::compose("Separate Stems", std::move(parts));
    CHECK(edit.structureChanged);
    history.perform(*song, std::move(edit));
    REQUIRE(song->tracks.size() == before + 2);
    CHECK(song->tracks[before].name == "Vocals");
    CHECK(song->tracks[before + 1].arrangementClips.size() == 1);
    CHECK(song->nextPointeeId >= next);

    const auto saved = dir.getChildFile("Saved.als");
    const auto result = io::saveAls(*song, saved);
    REQUIRE(result.ok);
    auto again = io::loadAls(saved);
    REQUIRE((bool) again);
    REQUIRE(again.song->tracks.size() == before + 2);
    CHECK(again.song->tracks[before].kind == model::TrackKind::Audio);
    CHECK(again.song->tracks[before].name == "Vocals");
    CHECK(again.song->tracks[before + 1].name == "Drums");
    REQUIRE(again.song->tracks[before].arrangementClips.size() == 1);
    CHECK_NEAR(again.song->tracks[before].arrangementClips[0].currentStart, 8.0, 1e-9);
    CHECK(again.song->tracks[before].arrangementClips[0].sample.absolutePath.find("vocals.wav") != std::string::npos);
    CHECK(again.song->returns.size() == 2); // returns stay after the new tracks
    CHECK(again.song->tracks[before].mixer.sends.size() == 2);

    // every automation / pointee id in the saved document is still unique
    auto xml = juce::XmlDocument::parse(juce::GZIPDecompressorInputStream(saved.createInputStream().release(), true, juce::GZIPDecompressorInputStream::gzipFormat).readEntireStreamAsString());
    REQUIRE(xml != nullptr);
    std::multiset<int> ids;
    collectPointeeIds(*xml, ids);
    for (int pid : ids)
        CHECK(ids.count(pid) == 1);

    history.undo(*song);
    CHECK(song->tracks.size() == before);
    history.redo(*song);
    CHECK(song->tracks.size() == before + 2);
}

TEST_CASE("menus: launch quantization and inserted track position survive undo redo and ALS reload")
{
    juce::TemporaryFile source(".als"); std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), source.getFile(), error));
    auto loaded = io::loadAls(source.getFile()); REQUIRE(loaded);
    auto song = *loaded.song;
    model::EditHistory history;
    const auto originalIds = std::vector<int>{song.tracks[0].id, song.tracks[1].id, song.tracks[2].id, song.tracks[3].id};
    int end = 0;
    auto track = io::createMidiTrack(song, "Inserted MIDI", 3, end);
    const auto insertedId = track.id;
    history.perform(song, model::insertTracks({track}, end, "Insert MIDI Track", 2));
    CHECK(song.tracks.size() == 5); CHECK(song.tracks[2].id == insertedId); CHECK(song.tracks[3].id == originalIds[2]);
    history.perform(song, model::setGlobalQuantisation(song, 11));
    juce::TemporaryFile saved(".als"); REQUIRE(io::saveAls(song, saved.getFile()).ok);
    auto reread = io::loadAls(saved.getFile()); REQUIRE(reread);
    CHECK(reread.song->globalQuantisation == 11); CHECK(reread.song->tracks[2].name == "Inserted MIDI");
    CHECK(reread.song->tracks[2].id == insertedId);
    REQUIRE(history.undo(song)); CHECK(song.globalQuantisation == 4);
    REQUIRE(history.undo(song)); REQUIRE(song.tracks.size() == 4);
    for (size_t i = 0; i < originalIds.size(); ++i) CHECK(song.tracks[i].id == originalIds[i]);
    REQUIRE(history.redo(song)); REQUIRE(history.redo(song));
    CHECK(song.tracks[2].id == insertedId); CHECK(song.globalQuantisation == 11);
    REQUIRE(io::saveAls(song, saved.getFile()).ok); reread = io::loadAls(saved.getFile()); REQUIRE(reread);
    CHECK(reread.song->globalQuantisation == 11); CHECK(reread.song->tracks[2].id == insertedId);
}

TEST_CASE("menus: track rename and locator creation retain names and positions through history and save")
{
    juce::TemporaryFile file(".als"); std::string error;
    auto dom = io::createNewSetDom();
    auto* locators = dom->getChildByName("LiveSet")->getChildByName("Locators")->getChildByName("Locators");
    auto* intro = locators->createNewChildElement("Locator"); intro->setAttribute("Id", 1);
    intro->createNewChildElement("Time")->setAttribute("Value", "0");
    intro->createNewChildElement("Name")->setAttribute("Value", "Intro");
    REQUIRE(io::writeAlsDom(*dom, file.getFile(), error));
    auto loaded = io::loadAls(file.getFile()); REQUIRE(loaded); auto song = *loaded.song;
    model::EditHistory history;
    const auto original = song.tracks[1].name;
    history.perform(song, model::renameTrack(song, 1, "Bass & Lead"));
    history.perform(song, model::addLocator(song, 8.5, "Verse"));
    REQUIRE(io::saveAls(song, file.getFile()).ok);
    auto reopened = io::loadAls(file.getFile()); REQUIRE(reopened);
    CHECK(reopened.song->tracks[1].name == "Bass & Lead"); REQUIRE(reopened.song->locators.size() == 2);
    CHECK_NEAR(reopened.song->locators[1].time, 8.5, 1e-9); CHECK(reopened.song->locators[1].name == "Verse");
    const auto savedDom = io::readAlsDom(file.getFile(), error); REQUIRE(savedDom);
    const auto* savedLocators = savedDom->getChildByName("LiveSet")->getChildByName("Locators")->getChildByName("Locators");
    CHECK(savedLocators->getChildElement(0)->getIntAttribute("Id") == 1);
    CHECK(savedLocators->getChildElement(1)->getIntAttribute("Id") != 1);
    REQUIRE(history.undo(song)); CHECK(song.locators.size() == 1);
    REQUIRE(history.undo(song)); CHECK(song.tracks[1].name == original);
    REQUIRE(history.redo(song)); REQUIRE(history.redo(song));
    CHECK(song.tracks[1].name == "Bass & Lead"); CHECK(song.locators[1].name == "Verse");
}

TEST_CASE("track colors: multi-track context color saves, undoes and rejects stale track identities")
{
    const auto dir = freshDirectory("track_colors");
    auto song = writeNewSet(dir.getChildFile("Original.als"));
    REQUIRE(song->tracks.size() >= 2);
    model::EditHistory history;
    const int first = song->tracks[0].colorIndex, second = song->tracks[1].colorIndex;
    history.perform(*song, model::setTrackColor(*song, {0, 1}, 24));
    CHECK(song->tracks[0].colorIndex == 24); CHECK(song->tracks[1].colorIndex == 24);
    const auto output = dir.getChildFile("Colored.als");
    REQUIRE(io::saveAls(*song, output).ok);
    auto loaded = io::loadAls(output); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].colorIndex == 24); CHECK(loaded.song->tracks[1].colorIndex == 24);
    history.undo(*song);
    CHECK(song->tracks[0].colorIndex == first); CHECK(song->tracks[1].colorIndex == second);
    history.redo(*song); CHECK(song->tracks[1].colorIndex == 24);
    auto stale = model::setTrackColor(*song, {0}, 9);
    song->tracks[0].id += 1000;
    stale.apply(*song); CHECK(song->tracks[0].colorIndex == 24);
    CHECK(! model::setTrackColor(*song, {0}, 70).apply);
}

TEST_CASE("track numbering: automatic names follow groups and nested groups through history and save")
{
    const auto dir = freshDirectory("automatic_track_names");
    auto song = writeNewSet(dir.getChildFile("Original.als"));
    model::EditHistory history;
    REQUIRE(song->tracks.size() == 4);
    CHECK(model::displayTrackName(*song, 0) == "1 MIDI");
    CHECK(model::displayTrackName(*song, 1) == "2 MIDI");
    CHECK(model::displayTrackName(*song, 2) == "3 Audio");
    int end = 0;
    auto group = io::createGroupTrack(*song, "Group", 3, end);
    CHECK(group.nameIsAutomatic);
    history.perform(*song, model::groupTracks(*song, {1, 2}, std::move(group), end));
    CHECK(model::displayTrackName(*song, 1) == "2 Group");
    CHECK(model::displayTrackName(*song, 2) == "3 MIDI");
    CHECK(model::displayTrackName(*song, 3) == "4 Audio");
    CHECK(model::displayTrackName(*song, 4) == "5 Audio");
    REQUIRE(history.undo(*song)); CHECK(model::displayTrackName(*song, 1) == "2 MIDI");
    REQUIRE(history.redo(*song)); CHECK(model::displayTrackName(*song, 2) == "3 MIDI");
    REQUIRE(io::saveAls(*song, dir.getChildFile("GroupSelection.als")).ok);
    std::vector<int> members; REQUIRE(model::groupableSelection(*song, {1}, members));
    auto outer = io::createGroupTrack(*song, "Group", 4, end);
    history.perform(*song, model::groupTracks(*song, members, std::move(outer), end));
    CHECK(model::displayTrackName(*song, 1) == "2 Group");
    CHECK(model::displayTrackName(*song, 2) == "3 Group");
    CHECK(model::displayTrackName(*song, 3) == "4 MIDI");
    REQUIRE(song->tracks[2].groupId == song->tracks[1].id);
    const auto saved = dir.getChildFile("Grouped.als"); REQUIRE(io::saveAls(*song, saved).ok);
    auto reloaded = io::loadAls(saved); REQUIRE(reloaded);
    for (int i = 0; i < (int) song->tracks.size(); ++i)
    {
        CHECK(reloaded.song->tracks[(size_t) i].nameIsAutomatic);
        CHECK(model::displayTrackName(*reloaded.song, i) == model::displayTrackName(*song, i));
    }
    history.perform(*song, model::ungroupTrack(*song, 1));
    CHECK(model::displayTrackName(*song, 1) == "2 Group");
    CHECK(model::displayTrackName(*song, 2) == "3 MIDI");
}

TEST_CASE("track numbering: custom numeric names stay literal and leading hash templates stay dynamic")
{
    const auto dir = freshDirectory("track_name_templates");
    auto song = writeNewSet(dir.getChildFile("Original.als"));
    model::EditHistory history;
    history.perform(*song, model::renameTrack(*song, 1, "909 Kick"));
    CHECK(! song->tracks[1].nameIsAutomatic); CHECK(model::displayTrackName(*song, 1) == "909 Kick");
    REQUIRE(history.undo(*song)); CHECK(song->tracks[1].nameIsAutomatic);
    history.perform(*song, model::renameTrack(*song, 1, "# Leads"));
    CHECK(model::displayTrackName(*song, 1) == "2 Leads");
    history.perform(*song, model::renameTrack(*song, 2, "3-Audio"));
    CHECK(model::displayTrackName(*song, 2) == "3-Audio"); // explicit numeric text is not an automatic prefix
    int end = 0; auto group = io::createGroupTrack(*song, "Drums", 5, end);
    CHECK(! group.nameIsAutomatic);
    history.perform(*song, model::groupTracks(*song, {1, 2}, std::move(group), end));
    CHECK(model::displayTrackName(*song, 1) == "Drums");
    CHECK(model::displayTrackName(*song, 2) == "3 Leads");
    CHECK(model::displayTrackName(*song, 3) == "3-Audio");
    const auto saved = dir.getChildFile("Named.als"); REQUIRE(io::saveAls(*song, saved).ok);
    auto reloaded = io::loadAls(saved); REQUIRE(reloaded);
    CHECK(model::displayTrackName(*reloaded.song, 2) == "3 Leads");
    CHECK(model::displayTrackName(*reloaded.song, 3) == "3-Audio");
    history.perform(*song, model::renameTrack(*song, 2, "### Layers"));
    CHECK(model::displayTrackName(*song, 2) == "003 Layers");
    history.perform(*song, model::renameTrack(*song, 2, ""));
    CHECK(model::displayTrackName(*song, 2) == "3 MIDI");
}

TEST_CASE("track duplication: complete edited channel has independent clips, devices and automation through save undo redo")
{
    const auto dir = freshDirectory("duplicate_track");
    const auto file = dir.getChildFile("Duplicate.als");
    REQUIRE(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml").copyFileTo(file));
    auto loaded = io::loadAls(file); REQUIRE(loaded);
    auto& song = *loaded.song;
    song.tracks[0].mixer.volume = 0.27;
    song.tracks[0].devices[0].plugin->state = {7, 8, 9, 10};
    song.tracks[0].arrangementClips[0].notes[0].pitch = 72;
    song.tracks[0].slots[1].clip->name = "Edited Session";
    model::TakeLane lane; lane.id = 8; lane.name = "Preserved Take";
    auto take = song.tracks[0].arrangementClips[0]; take.uid = model::allocateClipUid();
    lane.clips.push_back(take); song.tracks[0].takeLanes.push_back(lane); song.tracks[0].takeLanesNeedPatch = true;
    auto prepared = io::prepareTrackDuplication(song, {0});
    CHECK(prepared.error.empty()); REQUIRE(prepared.tracks.size() == 1);
    const auto original = song.tracks[0];
    const auto copy = prepared.tracks[0];
    CHECK(copy.id != original.id);
    CHECK(copy.arrangementClips[0].uid != original.arrangementClips[0].uid);
    CHECK(copy.slots[1].clip->uid != original.slots[1].clip->uid);
    CHECK(copy.arrangementClips[0].notes[0].pitch == 72);
    CHECK(copy.slots[1].clip->name == "Edited Session");
    REQUIRE(copy.takeLanes.size() == 1); CHECK(copy.takeLanes[0].name == "Preserved Take");
    CHECK(copy.takeLanes[0].clips[0].uid != take.uid);
    CHECK_NEAR(copy.mixer.volume, 0.27, 1e-9);
    CHECK(copy.devices[0].plugin->state == original.devices[0].plugin->state);
    CHECK(copy.mixer.volumeTargetId != original.mixer.volumeTargetId);
    CHECK(copy.automation[0].targetId == copy.mixer.volumeTargetId);
    CHECK(copy.devices[1].parameters[0].automationTargetId != original.devices[1].parameters[0].automationTargetId);
    model::EditHistory history;
    history.perform(song, model::insertTracks(std::move(prepared.tracks), prepared.pointeeEnd, "Duplicate Tracks", prepared.before));
    REQUIRE(song.tracks.size() == 3);
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    auto saved = io::loadAls(file); REQUIRE(saved); CHECK(saved.song->tracks.size() == 3);
    CHECK(saved.song->tracks[1].devices[0].plugin->state == original.devices[0].plugin->state);
    CHECK(saved.song->tracks[1].automation[0].targetId == copy.mixer.volumeTargetId);
    CHECK(saved.song->tracks[1].takeLanes[0].clips[0].notes[0].pitch == 72);
    REQUIRE(history.undo(song)); REQUIRE(song.tracks.size() == 2);
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    auto undone = io::loadAls(file); REQUIRE(undone); CHECK(undone.song->tracks.size() == 2);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, file).ok);
    auto redone = io::loadAls(file); REQUIRE(redone); REQUIRE(redone.song->tracks.size() == 3);
    CHECK(redone.song->tracks[1].arrangementClips[0].notes[0].pitch == 72);
    std::string error; auto dom = io::readAlsDom(file, error); REQUIRE(dom);
    std::multiset<int> targets; collectPointeeIds(*dom, targets);
    CHECK(std::set<int>(targets.begin(), targets.end()).size() == targets.size());
    CHECK(io::prepareTrackDuplication(song, {-1}).tracks.empty());
    CHECK(io::prepareTrackDuplication(song, {(int) song.tracks.size()}).tracks.empty());
}

TEST_CASE("track duplication: nested groups rebind parents and internal sidechain routes without changing originals")
{
    const auto dir = freshDirectory("duplicate_nested_group");
    const auto file = dir.getChildFile("Nested.als");
    auto song = writeNewSet(file); model::EditHistory history;
    int end = 0;
    auto inner = io::createGroupTrack(*song, "Inner", 12, end);
    history.perform(*song, model::groupTracks(*song, 0, 2, inner, end));
    auto outer = io::createGroupTrack(*song, "Outer", 22, end);
    history.perform(*song, model::groupTracks(*song, 0, 4, outer, end));
    std::string error; auto dom = io::readAlsDom(file, error); REQUIRE(dom);
    REQUIRE(io::patchAlsDom(*song, *dom).ok);
    auto* tracks = dom->getChildByName("LiveSet")->getChildByName("Tracks");
    auto* child = tracks->getChildByAttribute("Id", juce::String(song->tracks[2].id)); REQUIRE(child);
    child->createNewChildElement("RetainedRouting")->createNewChildElement("Target")
        ->setAttribute("Value", "AudioIn/Track." + juce::String(song->tracks[3].id) + "/PreFxOut");
    REQUIRE(io::writeAlsDom(*dom, file, error));
    auto prepared = io::prepareTrackDuplication(*song, {0});
    CHECK(prepared.error.empty()); REQUIRE(prepared.tracks.size() == 5);
    CHECK(prepared.tracks[1].groupId == prepared.tracks[0].id);
    CHECK(prepared.tracks[2].groupId == prepared.tracks[1].id);
    CHECK(prepared.tracks[3].groupId == prepared.tracks[1].id);
    CHECK(prepared.tracks[4].groupId == prepared.tracks[0].id);
    CHECK(prepared.tracks[2].sourceXml->find("AudioIn/Track." + std::to_string(prepared.tracks[3].id) + "/PreFxOut") != std::string::npos);
    CHECK(prepared.before == 5);
    history.perform(*song, model::insertTracks(std::move(prepared.tracks), prepared.pointeeEnd, "Duplicate Tracks", prepared.before));
    REQUIRE(io::saveAls(*song, file).ok); io::refreshClipIdentities(*song);
    auto saved = io::loadAls(file); REQUIRE(saved); REQUIRE(saved.song->tracks.size() == 11);
    CHECK(saved.song->tracks[6].groupId == saved.song->tracks[5].id);
    CHECK(saved.song->tracks[7].groupId == saved.song->tracks[6].id);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, file).ok);
    auto undone = io::loadAls(file); REQUIRE(undone); CHECK(undone.song->tracks.size() == 6);
}

TEST_CASE("duplicate region save: source offsets, split tails, automation and undo survive save reload")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    const auto dir = freshDirectory("duplicate-region");
    const auto file = dir.getChildFile("Range.als");
    model::EditHistory history;
    history.perform(song, model::duplicateArrangementRegion(song, 0, 0, 6, 8));
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    auto reloaded = io::loadAls(file); REQUIRE(reloaded);
    const auto& clips = reloaded.song->tracks[0].arrangementClips;
    REQUIRE(clips.size() == 3);
    CHECK_NEAR(clips[0].currentEnd, 8, 1e-9);
    CHECK_NEAR(clips[1].currentStart, 8, 1e-9); CHECK_NEAR(clips[1].currentEnd, 10, 1e-9);
    CHECK_NEAR(clips[1].loop.startRelative, 2, 1e-9);
    CHECK_NEAR(clips[2].currentStart, 10, 1e-9); CHECK_NEAR(clips[2].loop.startRelative, 6, 1e-9);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, file).ok);
    reloaded = io::loadAls(file); REQUIRE(reloaded);
    REQUIRE(reloaded.song->tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(reloaded.song->tracks[0].arrangementClips[0].currentStart, 4, 1e-9);
    CHECK_NEAR(reloaded.song->tracks[0].arrangementClips[0].currentEnd, 12, 1e-9);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, file).ok);
    reloaded = io::loadAls(file); REQUIRE(reloaded); CHECK(reloaded.song->tracks[0].arrangementClips.size() == 3);
}

TEST_CASE("move region save: partial multi-channel moves and cross-channel identities survive save undo redo")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml")); REQUIRE(loaded);
    auto& song = *loaded.song;
    model::EditHistory history;
    history.perform(song, model::copyClipToTrack(song, 0, song.tracks[0].arrangementClips[0].uid, 1, 6));
    song.tracks[0].automation.clear(); song.tracks[1].automation.clear(); history.clear();
    const auto file = freshDirectory("move-region").getChildFile("Moved.als");
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    history.perform(song, model::transferArrangementRegion(song, 0, 1, 5, 13, 17));
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    auto reload = io::loadAls(file); REQUIRE(reload);
    const auto& first = reload.song->tracks[0].arrangementClips; const auto& second = reload.song->tracks[1].arrangementClips;
    REQUIRE(first.size() == 2); REQUIRE(second.size() == 2);
    CHECK_NEAR(first[0].currentStart, 4, 1e-9); CHECK_NEAR(first[0].currentEnd, 5, 1e-9);
    CHECK_NEAR(first[1].currentStart, 17, 1e-9); CHECK_NEAR(first[1].loop.startRelative, 1, 1e-9);
    CHECK_NEAR(second[0].currentStart, 13, 1e-9); CHECK_NEAR(second[0].loop.startRelative, 7, 1e-9);
    CHECK_NEAR(second[1].currentStart, 18, 1e-9);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    reload = io::loadAls(file); REQUIRE(reload); REQUIRE(reload.song->tracks[0].arrangementClips.size() == 1);
    CHECK_NEAR(reload.song->tracks[0].arrangementClips[0].currentEnd, 12, 1e-9);
    REQUIRE(history.redo(song)); REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    reload = io::loadAls(file); REQUIRE(reload); CHECK(reload.song->tracks[1].arrangementClips.size() == 2);
    REQUIRE(history.undo(song)); history.clear();
    const int uid = song.tracks[0].arrangementClips[0].uid;
    history.perform(song, model::transferArrangementRegion(song, 0, 0, 4, 12, 16, 1));
    CHECK(song.tracks[1].arrangementClips.back().uid == uid);
    REQUIRE(io::saveAls(song, file).ok); io::refreshClipIdentities(song);
    reload = io::loadAls(file); REQUIRE(reload); CHECK(reload.song->tracks[0].arrangementClips.empty());
    REQUIRE(reload.song->tracks[1].arrangementClips.size() == 2);
    CHECK_NEAR(reload.song->tracks[1].arrangementClips.back().currentStart, 16, 1e-9);
    REQUIRE(history.undo(song)); REQUIRE(io::saveAls(song, file).ok);
    reload = io::loadAls(file); REQUIRE(reload); CHECK(reload.song->tracks[0].arrangementClips.size() == 1);
    CHECK(reload.song->tracks[1].arrangementClips.size() == 1);
}

TEST_CASE("editor workflow save: reordered grouped tracks and authored per-note expression survive save reload and undo")
{
    const auto directory = freshDirectory("editor-workflow-35");
    auto song = writeNewSet(directory.getChildFile("Source.als")); model::EditHistory history;
    model::Clip clip; REQUIRE(model::makeEmptyMidiClip(*song, 0, 4, clip));
    model::MidiNote note; note.noteId = 11; note.duration = 2; clip.notes = {note};
    clip.noteExpressions = {{11, -2, {{0, 0}, {1, 12}}}, {11, 74, {{0, 25.4}, {1.5, 101.6}}}, {11, -1, {{0, 0}, {2, 127}}}};
    clip.noteExpressionsEdited = true; song->tracks[0].arrangementClips.push_back(clip);
    int pointee = 0; auto group = io::createGroupTrack(*song, "Group", 4, pointee);
    const auto groupId = group.id; history.perform(*song, model::groupTracks(*song, 1, 1, group, pointee));
    auto move = model::moveTracks(*song, {0}, 2, groupId); REQUIRE(move.apply); history.perform(*song, std::move(move));
    const auto destination = directory.getChildFile("Moved.als"); REQUIRE(io::saveAls(*song, destination).ok);
    auto loaded = io::loadAls(destination); REQUIRE(loaded); CHECK(loaded.song->tracks[0].id == groupId);
    REQUIRE(loaded.song->tracks[1].arrangementClips.size() == 1);
    const auto& again = loaded.song->tracks[1].arrangementClips[0]; REQUIRE(again.noteExpressions.size() == 3);
    CHECK(again.notes[0].noteId == 11); CHECK(again.noteExpressions[1].cc == 74); CHECK_NEAR(again.noteExpressions[1].points[1].value, 101.6, 1e-6);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, directory.getChildFile("Undone.als")).ok);
    loaded = io::loadAls(directory.getChildFile("Undone.als")); REQUIRE(loaded); CHECK(loaded.song->tracks[0].id == song->tracks[0].id);
    REQUIRE(history.redo(*song)); CHECK(song->tracks[1].groupId == groupId);
}

TEST_CASE("MPE save history: Save Undo Save removes new lanes and Redo restores them without stale expression")
{
    const auto directory = freshDirectory("mpe-history-35");
    auto song = writeNewSet(directory.getChildFile("Source.als"));
    model::Clip clip; REQUIRE(model::makeEmptyMidiClip(*song, 0, 4, clip));
    model::MidiNote note; note.noteId = 7; note.duration = 2; clip.notes = {note};
    model::EditHistory history; history.perform(*song, model::insertClip(0, clip, "Insert MIDI Clip")); history.clear();
    clip = song->tracks[0].arrangementClips[0];
    const auto at = model::ClipLocation::arrangement(0, clip.uid);
    const auto path = directory.getChildFile("Edited.als"); REQUIRE(io::saveAls(*song, path).ok);
    auto changed = clip;
    changed.noteExpressions = {{7, -2, {{0, 0}, {1, 12}}}}; changed.noteExpressionsEdited = true;
    history.perform(*song, model::modifyClip(*song, at, changed, "Edit Expression")); REQUIRE(io::saveAls(*song, path).ok);
    auto loaded = io::loadAls(path); REQUIRE(loaded); REQUIRE(loaded.song->tracks[0].arrangementClips[0].noteExpressions.size() == 1);
    auto next = changed; next.noteExpressions.push_back({7, 74, {{0, 64}, {1, 127}}});
    history.perform(*song, model::modifyClip(*song, at, next, "Edit Slide")); REQUIRE(io::saveAls(*song, path).ok);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, path).ok);
    loaded = io::loadAls(path); REQUIRE(loaded); CHECK(loaded.song->tracks[0].arrangementClips[0].noteExpressions.size() == 1);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, path).ok);
    loaded = io::loadAls(path); REQUIRE(loaded); CHECK(loaded.song->tracks[0].arrangementClips[0].noteExpressions.empty());
    REQUIRE(history.redo(*song)); REQUIRE(io::saveAls(*song, path).ok);
    loaded = io::loadAls(path); REQUIRE(loaded); CHECK(loaded.song->tracks[0].arrangementClips[0].noteExpressions.size() == 1);
}

TEST_CASE("track deletion: mixed channels and groups delete atomically, protect Main and undo after save")
{
    const auto dir = freshDirectory("delete-tracks");
    const auto file = dir.getChildFile("Set.als");
    auto song = writeNewSet(file);
    int end = 0;
    auto group = io::createGroupTrack(*song, "Group", 5, end);
    model::EditHistory history;
    history.perform(*song, model::groupTracks(*song, 0, 2, std::move(group), end));
    const auto ids = [&] { std::vector<int> out; for (const auto& t : song->tracks) out.push_back(t.id); for (const auto& t : song->returns) out.push_back(t.id); return out; }();
    CHECK(! model::deleteTracks(*song, {(int) song->tracks.size() + (int) song->returns.size()}).apply);
    CHECK(! model::deleteTracks(*song, {0, 999}).apply);
    CHECK(! model::deleteTracks(*song, {}).apply);
    auto command = model::deleteTracks(*song, {0, (int) song->tracks.size()});
    REQUIRE(command.apply); CHECK(command.structureChanged);
    history.perform(*song, std::move(command));
    CHECK(song->tracks.size() == 2); CHECK(song->returns.size() == 1);
    REQUIRE(io::saveAls(*song, file).ok);
    auto deleted = io::loadAls(file); REQUIRE(deleted);
    CHECK(deleted.song->tracks.size() == 2); CHECK(deleted.song->returns.size() == 1);
    REQUIRE(history.undo(*song));
    REQUIRE(io::saveAls(*song, file).ok);
    auto restored = io::loadAls(file); REQUIRE(restored);
    std::vector<int> restoredIds;
    for (const auto& t : restored.song->tracks) restoredIds.push_back(t.id);
    for (const auto& t : restored.song->returns) restoredIds.push_back(t.id);
    CHECK(restoredIds == ids);
    CHECK(restored.song->tracks[1].groupId == restored.song->tracks[0].id);
    REQUIRE(history.redo(*song)); CHECK(song->tracks.size() == 2); CHECK(song->returns.size() == 1);
}

TEST_CASE("track deletion: return removal preserves surviving sends targets envelopes and pre flags through save undo redo")
{
    const auto dir = freshDirectory("delete-return");
    const auto file = dir.getChildFile("Set.als");
    auto song = writeNewSet(file);
    auto& track = song->tracks[0];
    track.mixer.sends = {0.25, 0.75};
    const auto targets = track.mixer.sendTargetIds;
    REQUIRE(targets.size() == 2);
    track.automation = {{targets[0], false, {{0, 0.25}, {4, 0.5}}}, {targets[1], false, {{0, 0.75}}}};
    model::Clip clip; REQUIRE(model::makeEmptyMidiClip(*song, 0, 4, clip));
    clip.envelopes = track.automation;
    track.arrangementClips.push_back(clip); track.slots[0].clip = clip;
    model::TakeLane lane; lane.id = 4; lane.clips = {clip}; track.takeLanes = {lane}; track.takeLanesNeedPatch = true;
    REQUIRE(io::saveAls(*song, file).ok);
    std::string xmlError;
    auto decorated = io::readAlsDom(file, xmlError); REQUIRE(decorated);
    auto* live = decorated->getChildByName("LiveSet");
    auto* xmlTrack = live->getChildByName("Tracks")->getChildElement(0);
    xmlTrack->getChildByName("DeviceChain")->getChildByName("Mixer")->getChildByName("Sends")->getChildElement(1)->setAttribute("Keep", "send B");
    live->getChildByName("SendsPre")->getChildElement(1)->setAttribute("Keep", "pre B");
    REQUIRE(io::writeAlsDom(*decorated, file, xmlError));
    auto withUnknown = io::loadAls(file); REQUIRE(withUnknown); song = std::move(withUnknown.song);
    model::EditHistory history;
    history.perform(*song, model::deleteTracks(*song, {(int) song->tracks.size()}));
    CHECK(song->tracks[0].mixer.sends == std::vector<double>({0.75}));
    CHECK(song->tracks[0].mixer.sendTargetIds == std::vector<int>({targets[1]}));
    CHECK(song->tracks[0].automation.size() == 1);
    CHECK(song->tracks[0].arrangementClips[0].envelopes.size() == 1);
    CHECK(song->tracks[0].slots[0].clip->envelopes.size() == 1);
    CHECK(song->tracks[0].takeLanes[0].clips[0].envelopes.size() == 1);
    REQUIRE(io::saveAls(*song, file).ok);
    auto loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].mixer.sendTargetIds == std::vector<int>({targets[1]}));
    CHECK(loaded.song->tracks[0].mixer.sends == std::vector<double>({0.75}));
    CHECK(loaded.song->sendPreXml.size() == 1);
    REQUIRE(loaded.song->tracks[0].mixer.sendSourceXml.size() == 1);
    CHECK(loaded.song->tracks[0].mixer.sendSourceXml[0]->find("send B") != std::string::npos);
    CHECK(loaded.song->sendPreXml[0]->find("pre B") != std::string::npos);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, file).ok);
    loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(loaded.song->tracks[0].mixer.sendTargetIds == targets);
    CHECK(loaded.song->tracks[0].mixer.sends == std::vector<double>({0.25, 0.75}));
    CHECK(loaded.song->tracks[0].automation.size() == 2);
    CHECK(loaded.song->tracks[0].arrangementClips[0].envelopes.size() == 2);
    CHECK(loaded.song->sendPreXml.size() == 2);
    REQUIRE(history.redo(*song)); REQUIRE(io::saveAls(*song, file).ok);
    loaded = io::loadAls(file); REQUIRE(loaded); CHECK(loaded.song->returns.size() == 1);
    history.perform(*song, model::deleteTracks(*song, {(int) song->tracks.size()}));
    REQUIRE(io::saveAls(*song, file).ok);
    loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(loaded.song->returns.empty()); CHECK(loaded.song->tracks[0].mixer.sends.empty()); CHECK(loaded.song->sendPreXml.empty());
}

TEST_CASE("track deletion: routed input output and sidechain references clear and restore after saving")
{
    const auto dir = freshDirectory("delete-routing"); const auto file = dir.getChildFile("Set.als");
    auto song = writeNewSet(file);
    std::string error; auto dom = io::readAlsDom(file, error); REQUIRE(dom);
    auto* tracks = dom->getChildByName("LiveSet")->getChildByName("Tracks");
    auto* source = tracks->getChildElement(0); auto* target = tracks->getChildElement(1);
    const auto id = source->getIntAttribute("Id");
    auto* chain = target->getChildByName("DeviceChain");
    const std::string input = "MidiIn/Track." + std::to_string(id);
    const std::string output = "AudioOut/Track." + std::to_string(id);
    chain->getChildByName("MidiInputRouting")->getChildByName("Target")->setAttribute("Value", juce::String(input));
    chain->getChildByName("AudioOutputRouting")->getChildByName("Target")->setAttribute("Value", juce::String(output));
    auto device = juce::XmlDocument::parse("<Compressor2 Id='999'><SideChain><OnOff><Manual Value='true'/></OnOff><RoutedInput><Routable><Target Value='AudioIn/Track."
                                         + juce::String(id) + "/PostMixerOut'/></Routable><Volume><Manual Value='1'/></Volume></RoutedInput></SideChain></Compressor2>");
    REQUIRE(device); chain->getChildByName("DeviceChain")->getChildByName("Devices")->addChildElement(device.release());
    REQUIRE(io::writeAlsDom(*dom, file, error)); auto original = io::loadAls(file); REQUIRE(original); song = std::move(original.song);
    REQUIRE(song->tracks[1].devices[0].sidechain.on);
    model::EditHistory history; history.perform(*song, model::deleteTracks(*song, {0}));
    CHECK(song->tracks[0].inputRouting == "MidiIn/None"); CHECK(song->tracks[0].outputRouting == "AudioOut/Main");
    CHECK(! song->tracks[0].devices[0].sidechain.on); CHECK(song->tracks[0].devices[0].sidechain.sourceTrackId == -1);
    REQUIRE(io::saveAls(*song, file).ok); auto loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(! loaded.song->tracks[0].devices[0].sidechain.on); CHECK(loaded.song->tracks[0].devices[0].sidechain.sourceTrackId == -1);
    REQUIRE(history.undo(*song)); REQUIRE(io::saveAls(*song, file).ok); loaded = io::loadAls(file); REQUIRE(loaded);
    CHECK(loaded.song->tracks[1].inputRouting == input); CHECK(loaded.song->tracks[1].outputRouting == output);
    CHECK(loaded.song->tracks[1].devices[0].sidechain.on); CHECK(loaded.song->tracks[1].devices[0].sidechain.sourceTrackId == id);
    CHECK(loaded.song->tracks[1].devices[0].sidechain.tap == model::Sidechain::Tap::PostMixer);
}
