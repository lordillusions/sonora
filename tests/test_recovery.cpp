// PRJ-4 crash recovery: run folders and their lock, stale-lock detection, rotation, the autosave throttle and a recovered
// set saved back to its own file.
#include "TestFramework.h"

#include "core/Edit.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"
#include "io/recovery/RecoveryStore.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#if ! defined(_WIN32)
 #include <unistd.h>
#endif

using namespace sonora;
namespace rec = sonora::io::recovery;

namespace
{
juce::File freshRoot(const char* name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_recovery_tests").getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

constexpr int kDeadPid = 99999999; // above every OS's pid range

// A folder as a crashed (or running) run leaves it: a lock with `pid`, a copy and its recovery.xml.
juce::File fakeRun(const juce::File& root, const juce::String& name, int pid, juce::int64 savedAt, bool withCopy = true, bool declined = false)
{
    const auto folder = root.getChildFile(name);
    REQUIRE(folder.createDirectory());
    if (pid != 0) REQUIRE(folder.getChildFile(rec::kLockFile).replaceWithText("pid=" + juce::String(pid) + "\n"));
    if (withCopy)
    {
        REQUIRE(folder.getChildFile(rec::kAutosaveFile).replaceWithText("not really a set"));
        juce::XmlElement xml("SonoraRecovery");
        xml.setAttribute("set", rec::kAutosaveFile);
        xml.setAttribute("document", "/tmp/" + name + ".als");
        xml.setAttribute("name", name);
        xml.setAttribute("savedAt", (double) savedAt);
        if (declined) xml.setAttribute("declined", true);
        REQUIRE(folder.getChildFile(rec::kInfoFile).replaceWithText(xml.toString()));
    }
    return folder;
}

int parentPid()
{
#if defined(_WIN32)
    return 0;
#else
    return (int) getppid();
#endif
}

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
} // namespace

TEST_CASE("recovery: a run writes its lock with its pid on start and a clean quit removes its folder")
{
    const auto root = freshRoot("lock");
    rec::Store store(root);
    REQUIRE(store.start("9.9.9", juce::Time::currentTimeMillis()));
    REQUIRE(store.folder().isAChildOf(root));
    const auto lock = store.folder().getChildFile(rec::kLockFile);
    REQUIRE(lock.existsAsFile());
    CHECK(lock.loadFileAsString().contains("pid=" + juce::String(rec::currentProcessId())));
    CHECK(lock.loadFileAsString().contains("version=9.9.9"));
    CHECK(rec::processAlive(rec::currentProcessId()));
    CHECK(! rec::processAlive(kDeadPid));
    CHECK(! rec::processAlive(0));

    // A copy is recorded atomically; clearing the document removes it but keeps a file still in use.
    REQUIRE(store.autosaveFile().replaceWithText("copy"));
    REQUIRE(store.recoveredFile().replaceWithText("base"));
    rec::Entry info;
    info.setFile = store.autosaveFile();
    info.documentPath = "/Users/x/Song Project/Song.als";
    info.setName = "Song";
    info.savedAtMs = 1234;
    REQUIRE(store.commit(info));
    const auto read = rec::Store::read(store.folder());
    CHECK(read.setFile == store.autosaveFile());
    CHECK(read.documentPath == info.documentPath);
    CHECK(read.setName == "Song");
    CHECK(read.savedAtMs == 1234);
    rec::Entry outside = info;
    outside.setFile = root.getChildFile("elsewhere.als");
    CHECK(! store.commit(outside)); // a copy outside the run's folder is never recorded
    store.clearDocument(store.recoveredFile());
    CHECK(! store.autosaveFile().existsAsFile());
    CHECK(store.recoveredFile().existsAsFile());
    CHECK(! store.folder().getChildFile(rec::kInfoFile).existsAsFile());
    CHECK(lock.existsAsFile()); // still running

    const auto folder = store.folder();
    store.finish();
    CHECK(! folder.exists());
    CHECK(! store.started());
}

TEST_CASE("recovery: a lock whose pid is dead (or never refreshed) is a crash; a live run is left alone")
{
    const auto root = freshRoot("stale");
    const auto now = juce::Time::currentTimeMillis();
    fakeRun(root, "dead", kDeadPid, now - 1000);
    fakeRun(root, "no lock", 0, now - 2000);
    const auto alive = parentPid();
    if (alive > 0)
    {
        fakeRun(root, "alive", alive, now - 500);
        const auto quiet = fakeRun(root, "alive but quiet", alive, now - 3000);
        REQUIRE(quiet.getChildFile(rec::kLockFile).setLastModificationTime(juce::Time(now - rec::kStaleLockMs - 60000)));
    }
    // An earlier run with this process's pid (a pid reused after a restart) is not alive.
    fakeRun(root, "own pid", rec::currentProcessId(), now - 4000);

    rec::Store store(root);
    REQUIRE(store.start("1.0", now));
    const auto found = store.recoverable(now);
    std::vector<std::string> names;
    for (const auto& e : found) names.push_back(e.setName);
    if (alive > 0)
    {
        REQUIRE(found.size() == 4);
        CHECK(names == std::vector<std::string>({"dead", "no lock", "alive but quiet", "own pid"})); // newest first
    }
    else
        CHECK(found.size() == 3);
    for (const auto& e : store.scan(now))
        CHECK(e.running == (e.setName == "alive"));
    CHECK(found.front().hasCopy());
    CHECK(found.front().documentPath == "/tmp/dead.als");
    // Our own folder is never offered.
    for (const auto& e : found) CHECK(e.folder != store.folder());
    store.finish();
}

TEST_CASE("recovery: rotation keeps the newest copies, drops empty and old declined folders, never touches a live run")
{
    const auto root = freshRoot("rotation");
    const auto now = juce::Time::currentTimeMillis();
    const juce::int64 minute = 60 * 1000;
    for (int i = 0; i < 7; ++i) fakeRun(root, "copy " + juce::String(i), kDeadPid, now - (i + 1) * minute);
    fakeRun(root, "empty", kDeadPid, now, false);
    fakeRun(root, "declined recently", kDeadPid, now - 30 * 1000, true, true);
    fakeRun(root, "declined long ago", kDeadPid, now - rec::kKeepDeclinedMs - minute, true, true);
    const auto alive = parentPid();
    if (alive > 0) fakeRun(root, "running", alive, now - 365LL * 24 * 60 * minute, false);

    rec::Store store(root);
    REQUIRE(store.start("1.0", now));
    const int removed = store.prune(now);
    CHECK(! root.getChildFile("empty").exists());
    CHECK(! root.getChildFile("declined long ago").exists());
    CHECK(root.getChildFile("declined recently").exists());
    if (alive > 0) CHECK(root.getChildFile("running").exists());
    CHECK(store.folder().exists()); // our own folder
    // Five copies survive: the recent declined one and the four newest others.
    int kept = 0;
    for (int i = 0; i < 7; ++i) kept += root.getChildFile("copy " + juce::String(i)).exists() ? 1 : 0;
    CHECK(kept == rec::kKeepCopies - 1);
    CHECK(root.getChildFile("copy 0").exists());
    CHECK(! root.getChildFile("copy 6").exists());
    CHECK(removed == 2 + (7 - kept));

    // A declined copy is kept but not offered again; declining removes its lock.
    auto offers = store.recoverable(now);
    CHECK(offers.size() == (size_t) kept);
    for (const auto& e : offers) CHECK(! e.declined);
    REQUIRE(rec::Store::decline(offers.front()));
    CHECK(! offers.front().folder.getChildFile(rec::kLockFile).existsAsFile());
    CHECK(store.recoverable(now).size() == (size_t) kept - 1);
    CHECK(offers.front().folder.exists());
    store.finish();
}

TEST_CASE("recovery: autosave waits for an idle pause or the interval, never while recording or rendering")
{
    rec::AutosaveInputs in;
    in.documentOpen = in.dirty = in.changedSinceAutosave = true;
    in.sinceLastChangeMs = 6000;
    in.sincePendingMs = 6000;
    CHECK(rec::autosaveDue(in));

    auto with = [&](auto change) { auto copy = in; change(copy); return rec::autosaveDue(copy); };
    CHECK(! with([](auto& x) { x.recording = true; }));
    CHECK(! with([](auto& x) { x.rendering = true; }));
    CHECK(! with([](auto& x) { x.busy = true; }));
    CHECK(! with([](auto& x) { x.dirty = false; }));
    CHECK(! with([](auto& x) { x.changedSinceAutosave = false; })); // nothing new since the last copy
    CHECK(! with([](auto& x) { x.documentOpen = false; }));
    CHECK(! with([](auto& x) { x.sinceLastAutosaveMs = 3000; }));   // too soon after the previous copy
    // A big set whose copy took 5 s to write is copied at most every 50 s (the writer busy a tenth of the time).
    CHECK(! with([](auto& x) { x.lastWriteMs = 5000; x.sinceLastAutosaveMs = 30000; }));
    CHECK(with([](auto& x) { x.lastWriteMs = 5000; x.sinceLastAutosaveMs = 51000; }));
    // Still editing: waits for a pause...
    CHECK(! with([](auto& x) { x.sinceLastChangeMs = 1000; x.sincePendingMs = 30000; }));
    // ...but not for longer than the interval.
    CHECK(with([](auto& x) { x.sinceLastChangeMs = 1000; x.sincePendingMs = 61000; }));
    CHECK(! with([](auto& x) { x.sinceLastChangeMs = 1000; x.sincePendingMs = 61000; x.recording = true; }));
}

TEST_CASE("recovery: an autosaved set with a take recorded after the last save recovers as the set and saves back to its file")
{
    const auto root = freshRoot("roundtrip");
    const auto project = root.getChildFile("Song Project");
    const auto original = project.getChildFile("Song.als");
    REQUIRE(project.createDirectory());
    std::string error;
    REQUIRE(io::writeAlsDom(*io::createNewSetDom(), original, error));
    auto loaded = io::loadAls(original);
    REQUIRE(loaded);
    auto& song = *loaded.song;
    song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());

    // Edits after the last save, among them a take the recorder wrote into the project (a project-relative reference).
    const auto take = writeTinyWav(project.getChildFile("Samples/Recorded/take.wav"));
    model::EditHistory history;
    history.perform(song, model::setTempo(song, 133.0));
    history.perform(song, model::renameTrack(song, 0, "Recovered Track"));
    const int audioTrack = 2;
    REQUIRE(song.tracks[(size_t) audioTrack].kind == model::TrackKind::Audio);
    model::Clip clip;
    clip.kind = model::Clip::Kind::Audio;
    clip.resetSourceMarkers = true;
    clip.name = "Take";
    clip.sample.absolutePath = take.getFullPathName().toStdString();
    clip.sample.relativePath = "Samples/Recorded/take.wav";
    clip.sample.relativePathType = 3;
    clip.sample.sourceDirectory = project.getFullPathName().toStdString();
    clip.sampleRate = 44100.0;
    clip.lengthSamples = 441;
    clip.loop = {0, 0.01, 0, false};
    clip.time = clip.currentStart = 4.0;
    clip.currentEnd = 8.0;
    clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
    history.perform(song, model::insertClip(audioTrack, std::move(clip), "Record"));

    // The autosave: a copy of the model into this run's folder, references as they are.
    const auto recoveryRoot = root.getChildFile("Recovery");
    rec::Store crashed(recoveryRoot);
    REQUIRE(crashed.start("1.0"));
    const model::Song copy = song;
    const auto written = io::saveAls(copy, crashed.autosaveFile(), io::AlsSaveOptions{});
    REQUIRE(written.ok);
    rec::Entry info;
    info.setFile = crashed.autosaveFile();
    info.documentPath = original.getFullPathName().toStdString();
    info.setName = "Song";
    info.savedAtMs = juce::Time::currentTimeMillis();
    info.documentModifiedMs = original.getLastModificationTime().toMilliseconds();
    REQUIRE(crashed.commit(info));
    // The user's file was never touched.
    {
        auto untouched = io::loadAls(original);
        REQUIRE(untouched);
        CHECK_NEAR(untouched.song->tempo, 120.0, 1e-9);
        CHECK(untouched.song->tracks[(size_t) audioTrack].arrangementClips.empty());
    }

    // The next launch (the crashed run's pid is this process's: not alive for another folder).
    rec::Store next(recoveryRoot);
    REQUIRE(next.start("1.0"));
    const auto offers = next.recoverable();
    REQUIRE(offers.size() == 1);
    const auto entry = offers.front();
    CHECK(entry.documentPath == info.documentPath);
    CHECK(! entry.documentChangedSince());
    REQUIRE(entry.setFile.copyFileTo(next.recoveredFile()));
    auto recovered = io::loadAls(next.recoveredFile(), entry.documentPath);
    REQUIRE(recovered);
    auto& r = *recovered.song;
    CHECK(r.sourcePath == original.getFullPathName().toStdString());
    CHECK(r.patchSourcePath == next.recoveredFile().getFullPathName().toStdString());
    CHECK_NEAR(r.tempo, 133.0, 1e-9);
    CHECK(r.tracks[0].name == "Recovered Track");
    REQUIRE(r.tracks[(size_t) audioTrack].arrangementClips.size() == 1);
    CHECK(io::resolveSamplePath(r, r.tracks[(size_t) audioTrack].arrangementClips[0].sample) == take.getFullPathName().toStdString());

    // Save As into a new project (an untitled set's first save) brings the take from the set's folder, not the recovery's.
    const auto elsewhere = root.getChildFile("Elsewhere Project").getChildFile("Elsewhere.als");
    REQUIRE(elsewhere.getParentDirectory().createDirectory());
    io::AlsSaveOptions saveAs;
    saveAs.portableReferences = saveAs.copyProjectSamples = true;
    const auto asResult = io::saveAls(r, elsewhere, saveAs);
    REQUIRE(asResult.ok);
    CHECK(asResult.samplesCopied == 1);
    CHECK(elsewhere.getParentDirectory().getChildFile("Samples/Recorded/take.wav").existsAsFile());

    // Save: over the set's own file, with the previous version in its Backup folder.
    io::AlsSaveOptions save;
    save.portableReferences = true;
    const auto saved = io::saveAls(r, original, save);
    REQUIRE(saved.ok);
    CHECK(project.getChildFile("Backup").findChildFiles(juce::File::findFiles, false, "*.als").size() == 1);
    CHECK(! recoveryRoot.getChildFile("Song.als").exists()); // nothing written next to the copies
    auto reopened = io::loadAls(original);
    REQUIRE(reopened);
    CHECK_NEAR(reopened.song->tempo, 133.0, 1e-9);
    CHECK(reopened.song->tracks[0].name == "Recovered Track");
    CHECK(reopened.song->patchSourcePath.empty());
    REQUIRE(reopened.song->tracks[(size_t) audioTrack].arrangementClips.size() == 1);
    const auto& ref = reopened.song->tracks[(size_t) audioTrack].arrangementClips[0].sample;
    CHECK(ref.relativePathType == 3);
    CHECK(ref.relativePath == "Samples/Recorded/take.wav");
    CHECK(io::resolveSamplePath(*reopened.song, ref) == take.getFullPathName().toStdString());

    // A set saved again after the copy was made is reported.
    REQUIRE(original.setLastModificationTime(juce::Time(info.savedAtMs + 60000)));
    CHECK(rec::Store::read(entry.folder).documentChangedSince());
    crashed.finish();
    next.finish();
}
