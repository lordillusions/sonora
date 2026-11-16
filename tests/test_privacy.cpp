// COM-15: what Sonora's scans may touch without a macOS privacy prompt, and when it explains an access first.
#include "TestFramework.h"

#include "core/Privacy.h"
#include "engine/AudioEngine.h"

#include <algorithm>

using namespace sonora;

namespace
{
// A fake tree: <root>/Users/me is the home folder, <root>/Volumes holds the other volumes.
struct FakeHome
{
    juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("sonora-privacy-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    juce::File home = root.getChildFile("Users/me");

    FakeHome()
    {
        for (const char* file : {"Music/Samples/kick.wav", "Music/Music/Media.localized/song.wav", "Music/iTunes/old.wav",
                                 "Music/Old.photoslibrary/p.wav", "Music/Ableton/User Library/Presets/a.adv",
                                 "Music/Sonora Projects/New Project/New.als", "Music/.hidden/h.wav",
                                 "Documents/Set Project/Set.als", "Desktop/d.wav", "Downloads/dl.wav",
                                 "Pictures/Photos Library.photoslibrary/originals/a.wav", "Pictures/Shots/s.wav",
                                 "Library/Containers/com.other.app/Data/x.wav", "Library/Audio/Presets/lib.wav"})
            write(home.getChildFile(file));
        write(root.getChildFile("Volumes/Drive/Loops/drive.wav"));
        write(root.getChildFile("Elsewhere/snare.wav"));
        write(root.getChildFile("Projects/Song Project/Song.als"));
        link(home.getChildFile("Music/LinkToDocuments"), home.getChildFile("Documents"));
        link(home.getChildFile("Music/LinkToHome"), home);        // up the tree: a cycle
        link(home.getChildFile("Music/Samples/Again"), home.getChildFile("Music")); // up again
        link(home.getChildFile("Music/LinkToElsewhere"), root.getChildFile("Elsewhere"));
        link(home.getChildFile("Music/LinkToDrive"), root.getChildFile("Volumes/Drive"));
        link(home.getChildFile("Music/LinkToPhotos"), home.getChildFile("Pictures/Photos Library.photoslibrary"));
    }
    ~FakeHome() { root.deleteRecursively(); }

    static void write(const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        file.replaceWithText("x");
    }
    bool linksWork = true; // Windows needs a privilege to make links: the link checks are skipped there
    void link(const juce::File& at, const juce::File& target) { linksWork &= target.createSymbolicLink(at, true); }

    // Paths under `from`, relative to the fake root, of everything walkFolder reports.
    std::vector<std::string> walk(const juce::File& from) const
    {
        std::vector<std::string> seen;
        core::walkFolder(from, home, [&](const juce::File& file, bool) {
            seen.push_back(file.getRelativePathFrom(root).toStdString());
            return true;
        });
        std::sort(seen.begin(), seen.end());
        return seen;
    }
};

bool has(const std::vector<std::string>& paths, const std::string& path) { return std::find(paths.begin(), paths.end(), path) != paths.end(); }
bool anyContains(const std::vector<std::string>& paths, const std::string& part)
{
    return std::any_of(paths.begin(), paths.end(), [&](const std::string& p) { return p.find(part) != std::string::npos; });
}
} // namespace

TEST_CASE("privacy: guarded places are found from the home folder")
{
    const juce::File home("/Users/me");
    CHECK(core::promptGuardedArea(juce::File("/Users/me/Desktop/a.wav"), home) == juce::File("/Users/me/Desktop"));
    CHECK(core::promptGuardedArea(juce::File("/Users/me/Documents"), home) == juce::File("/Users/me/Documents"));
    CHECK(core::promptGuardedArea(juce::File("/Users/me/Music/Music/Media"), home) == juce::File("/Users/me/Music/Music"));
    CHECK(core::promptGuardedArea(juce::File("/Users/me/Library/Containers/x"), home) == juce::File("/Users/me/Library/Containers"));
    CHECK(core::promptGuardedArea(juce::File("/Users/me/Library/Audio/Presets"), home) == juce::File("/Users/me/Library"));
    CHECK(core::promptGuardedArea(juce::File("/Volumes/Drive/Loops/a.wav"), home) == juce::File("/Volumes/Drive"));
    CHECK(! core::isPromptGuarded(juce::File("/Users/me/Music/Samples"), home));
    CHECK(! core::isPromptGuarded(juce::File("/Users/me/Projects/x.als"), home));
    CHECK(! core::isPromptGuarded(juce::File("/Users/me"), home));
    CHECK(core::isMediaLibraryPackage(juce::File("/Users/me/Pictures/Photos Library.photoslibrary")));
    CHECK(core::isMediaLibraryPackage(juce::File("/x/Music Library.musiclibrary")));
    CHECK(! core::isMediaLibraryPackage(juce::File("/x/Drums")));
}

TEST_CASE("privacy: a walk of ~/Music never enters the Music, Photos or other guarded places")
{
    FakeHome fake;
    const auto seen = fake.walk(fake.home.getChildFile("Music"));
    CHECK(has(seen, "Users/me/Music/Samples/kick.wav"));
    CHECK(has(seen, "Users/me/Music/Sonora Projects/New Project/New.als"));
    if (fake.linksWork) CHECK(has(seen, "Users/me/Music/LinkToElsewhere/snare.wav")); // a link to an ordinary folder outside is followed
    CHECK(! anyContains(seen, "Music/Music"));       // the Apple Music library (Media & Apple Music prompt)
    CHECK(! anyContains(seen, "iTunes"));
    CHECK(! anyContains(seen, "photoslibrary"));     // Photos prompt
    CHECK(! anyContains(seen, "LinkToPhotos"));
    CHECK(! anyContains(seen, "Set.als"));           // the link into Documents is not followed
    CHECK(! anyContains(seen, "LinkToDocuments"));
    CHECK(! anyContains(seen, "drive.wav"));         // nor the one onto another volume
    CHECK(! anyContains(seen, "LinkToHome"));        // nor the ones back up the tree
    CHECK(! anyContains(seen, "Samples/Again"));
    CHECK(! anyContains(seen, ".hidden"));
}

TEST_CASE("privacy: a walk of the home folder skips Desktop, Documents, Downloads, Pictures and Library")
{
    FakeHome fake;
    const auto seen = fake.walk(fake.home);
    CHECK(has(seen, "Users/me/Music/Samples/kick.wav"));
    for (const char* part : {"Desktop", "Documents", "Downloads", "Pictures", "Library/Containers", "Library/Audio", "Music/Music", "iTunes"})
        CHECK(! anyContains(seen, part));
}

TEST_CASE("privacy: a chosen folder inside a guarded place is walked, without leaving it")
{
    FakeHome fake;
    auto seen = fake.walk(fake.home.getChildFile("Documents"));
    CHECK(has(seen, "Users/me/Documents/Set Project/Set.als"));
    seen = fake.walk(fake.root.getChildFile("Volumes/Drive"));
    CHECK(has(seen, "Volumes/Drive/Loops/drive.wav"));
    seen = fake.walk(fake.home.getChildFile("Pictures"));
    CHECK(has(seen, "Users/me/Pictures/Shots/s.wav"));
    CHECK(! anyContains(seen, "photoslibrary")); // a media library stays shut even there
    seen = fake.walk(fake.home.getChildFile("Library/Audio"));
    CHECK(has(seen, "Users/me/Library/Audio/Presets/lib.wav"));
}

TEST_CASE("privacy: a walk stops when asked")
{
    FakeHome fake;
    int visits = 0;
    core::walkFolder(fake.home.getChildFile("Music"), fake.home, [&](const juce::File&, bool) { return ++visits < 2; });
    CHECK(visits == 2);
    visits = 0;
    core::walkFolder(fake.home.getChildFile("Music"), fake.home, [&](const juce::File&, bool) { ++visits; return true; }, [] { return true; });
    CHECK(visits == 0);
}

TEST_CASE("privacy: the Projects list walks Music, chosen folders and recent sets outside guarded places")
{
    const juce::File home("/Users/me");
    const auto roots = core::projectScanRoots(home,
        {juce::File("/Users/me/Music/Ableton/User Library"), juce::File("/Users/me/Documents/Samples"), juce::File("/Users/me/Music/Sonora Projects")},
        {juce::File("/Users/me/Documents/Beat Project/Beat.als"), juce::File("/Users/me/Desktop/Demo.als"),
         juce::File("/Volumes/Drive/Gig Project/Gig.als"), juce::File("/Users/me/Projects/Song Project/Song.als"),
         juce::File("/Users/me/Music/Audio Library/Projects/X Project/X.als")});
    CHECK(roots.size() == 3);
    CHECK(std::find(roots.begin(), roots.end(), juce::File("/Users/me/Music")) != roots.end());
    CHECK(std::find(roots.begin(), roots.end(), juce::File("/Users/me/Documents/Samples")) != roots.end()); // chosen: walked
    CHECK(std::find(roots.begin(), roots.end(), juce::File("/Users/me/Projects/Song Project")) != roots.end());
    for (const auto& root : roots)
    {
        CHECK(root != juce::File("/Users/me/Documents"));
        CHECK(root != juce::File("/Users/me/Desktop"));
        CHECK(! root.getFullPathName().contains("Beat Project"));
        CHECK(! root.getFullPathName().startsWith("/Volumes"));
    }
}

TEST_CASE("privacy: Sonora explains only for a person's action, never on its own")
{
    using core::AccessRequest, core::AccessStep, core::SystemAnswer;
    for (bool notNow : {false, true})
        for (auto request : {AccessRequest::Automatic, AccessRequest::Browsing, AccessRequest::UserAction})
        {
            CHECK(core::accessStep(SystemAnswer::Granted, request, notNow) == AccessStep::Proceed);
            CHECK(core::accessStep(SystemAnswer::Denied, request, notNow) == AccessStep::Refuse);
        }
    CHECK(core::accessStep(SystemAnswer::NotAsked, AccessRequest::Automatic, false) == AccessStep::Skip); // a set with armed tracks
    CHECK(core::accessStep(SystemAnswer::NotAsked, AccessRequest::Browsing, false) == AccessStep::Explain);
    CHECK(core::accessStep(SystemAnswer::NotAsked, AccessRequest::Browsing, true) == AccessStep::Skip); // Not Now is remembered
    CHECK(core::accessStep(SystemAnswer::NotAsked, AccessRequest::UserAction, true) == AccessStep::Explain);
    std::vector<std::string> keys;
    for (auto access : {core::Access::Microphone, core::Access::Camera, core::Access::LocalNetwork})
    {
        const auto text = core::accessExplanation(access);
        CHECK(! text.title.empty());
        CHECK(text.message.find("macOS will ask you next") != std::string::npos);
        CHECK(text.deniedText.find("System Settings > Privacy & Security") != std::string::npos);
        CHECK(text.continueLabel == "Continue");
        CHECK(text.notNowLabel == "Not Now");
        keys.push_back(text.settingsKey);
    }
    std::sort(keys.begin(), keys.end());
    CHECK(std::unique(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("privacy: the saved audio state opens the output only and remembers the input")
{
    juce::String input, channels;
    juce::XmlElement split("DEVICESETUP");
    split.setAttribute("deviceType", "CoreAudio");
    split.setAttribute("audioOutputDeviceName", "Speakers");
    split.setAttribute("audioInputDeviceName", "Interface");
    split.setAttribute("audioDeviceInChans", "11");
    split.setAttribute("audioDeviceOutChans", "11");
    auto out = engine::AudioEngine::withoutInputs(split, input, channels);
    CHECK(input == "Interface");
    CHECK(channels == "11");
    CHECK(out->getStringAttribute("audioOutputDeviceName") == "Speakers");
    CHECK(out->getStringAttribute("audioInputDeviceName").isEmpty());
    CHECK(! out->hasAttribute("audioDeviceInChans")); // JUCE then opens no input channel (0 needed)
    CHECK(out->getStringAttribute("audioDeviceOutChans") == "11");

    juce::XmlElement combined("DEVICESETUP");
    combined.setAttribute("audioDeviceName", "Duo Capture");
    out = engine::AudioEngine::withoutInputs(combined, input, channels);
    CHECK(input == "Duo Capture");
    CHECK(channels.isEmpty());
    CHECK(! out->hasAttribute("audioDeviceName"));
    CHECK(out->getStringAttribute("audioOutputDeviceName") == "Duo Capture");
    CHECK(out->getStringAttribute("audioInputDeviceName").isEmpty());
}
