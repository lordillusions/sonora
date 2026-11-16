#include "core/Privacy.h"
#include "platform/FolderList.h"

namespace sonora::core
{

namespace
{
constexpr int kMaxDepth = 64; // a runaway walk (deep link chains) stops here

// Guarded places under the home folder, most specific first (a path takes the first one that holds it).
constexpr const char* kGuardedInHome[] = {
    "Desktop", "Documents", "Downloads", // Files and Folders
    "Pictures",                          // the Photos library lives here
    "Movies/TV",                         // the TV library (Media & Apple Music)
    "Music/Music", "Music/iTunes",       // the Apple Music library (Media & Apple Music)
    "Library/Mobile Documents",          // iCloud Drive
    "Library/CloudStorage",              // Dropbox, OneDrive, Google Drive (File Provider)
    "Library/Containers", "Library/Group Containers", // other apps' data
    "Library",                           // Mail, Messages, Safari, Calendars, Contacts... (Full Disk Access)
};

juce::File resolvedLink(juce::File file)
{
    for (int i = 0; i < 8 && file.isSymbolicLink(); ++i)
    {
        const auto target = file.getLinkedTarget();
        if (target == file) break;
        file = target;
    }
    return file.isSymbolicLink() ? juce::File() : file;
}
} // namespace

bool isMediaLibraryPackage(const juce::File& folder)
{
    for (const char* extension : {"photoslibrary", "photolibrary", "migratedphotolibrary", "aplibrary", "musiclibrary", "tvlibrary"})
        if (folder.hasFileExtension(extension)) return true;
    return folder.getFileName() == "Photo Booth Library";
}

juce::File promptGuardedArea(const juce::File& path, const juce::File& home)
{
    if (home == juce::File()) return {};
    for (const char* name : kGuardedInHome)
    {
        const auto area = home.getChildFile(name);
        if (path == area || path.isAChildOf(area)) return area;
    }
    // Removable and network volumes: every volume under /Volumes is its own place. The folder is found from the home
    // folder (/Users/<name> -> /Volumes) so tests can use a fake tree; it does not exist on Linux or Windows.
    const auto volumes = home.getParentDirectory().getParentDirectory().getChildFile("Volumes");
    if (path.isAChildOf(volumes))
    {
        auto volume = path;
        while (volume.getParentDirectory() != volumes) volume = volume.getParentDirectory();
        return volume;
    }
    return {};
}

void walkFolder(const juce::File& root, const juce::File& home, const std::function<bool(const juce::File&, bool isDirectory)>& visit,
                const std::function<bool()>& cancelled)
{
    if (! root.isDirectory()) return;
    const auto rootArea = promptGuardedArea(root, home);
    const auto allowed = [&](const juce::File& folder) {
        if (isMediaLibraryPackage(folder)) return false;
        const auto area = promptGuardedArea(folder, home);
        return area == juce::File() || area == rootArea;
    };
    struct Pending { juce::File folder; int depth; };
    std::vector<Pending> pending{{root, 0}};
    std::vector<juce::File> linked; // folders reached through a link (each is walked once)
    while (! pending.empty())
    {
        const auto [folder, depth] = pending.back();
        pending.pop_back();
        // platform::listFolder, not JUCE's iterator: that one asks Foundation about every child, and asking about
        // ~/Music/Music alone makes macOS prompt for the Apple Music library (seen in the TCC log, COM-15).
        for (const auto& entry : platform::listFolder(folder))
        {
            if (cancelled && cancelled()) return;
            const auto& file = entry.file;
            if (entry.link)
            {
                // Decided from the target's path first; only an allowed target is asked whether it is a folder.
                const auto target = resolvedLink(file);
                if (target == juce::File() || ! allowed(target)) continue;
                if (! target.isDirectory())
                {
                    if (target.existsAsFile() && ! visit(file, false)) return;
                    continue;
                }
                if (! allowed(file)) continue;
                if (target == root || target.isAChildOf(root) || root.isAChildOf(target)) continue; // walked already, or a cycle
                if (std::any_of(linked.begin(), linked.end(), [&](const juce::File& l) { return target == l || target.isAChildOf(l) || l.isAChildOf(target); }))
                    continue;
                linked.push_back(target);
            }
            else if (! entry.directory)
            {
                if (! visit(file, false)) return;
                continue;
            }
            else if (! allowed(file))
                continue;
            if (! visit(file, true)) return;
            if (depth < kMaxDepth) pending.push_back({file, depth + 1});
        }
    }
}

std::vector<juce::File> projectScanRoots(const juce::File& home, const std::vector<juce::File>& chosen, const std::vector<juce::File>& recentSets)
{
    std::vector<juce::File> roots;
    const auto add = [&roots](const juce::File& folder) {
        if (folder == juce::File()) return;
        for (const auto& root : roots)
            if (folder == root || folder.isAChildOf(root)) return;
        std::erase_if(roots, [&](const juce::File& root) { return root.isAChildOf(folder); });
        roots.push_back(folder);
    };
    add(home.getChildFile("Music"));
    for (const auto& folder : chosen) add(folder);
    for (const auto& set : recentSets)
        if (const auto folder = set.getParentDirectory(); ! isPromptGuarded(folder, home)) add(folder);
    return roots;
}

AccessStep accessStep(SystemAnswer answer, AccessRequest request, bool notNowRemembered)
{
    if (answer == SystemAnswer::Granted) return AccessStep::Proceed;
    if (answer == SystemAnswer::Denied) return AccessStep::Refuse;
    switch (request)
    {
        case AccessRequest::Automatic: return AccessStep::Skip;
        case AccessRequest::Browsing: return notNowRemembered ? AccessStep::Skip : AccessStep::Explain;
        case AccessRequest::UserAction: return AccessStep::Explain;
    }
    return AccessStep::Skip;
}

AccessExplanation accessExplanation(Access access)
{
    AccessExplanation e;
    switch (access)
    {
        case Access::Microphone:
            e.title = "Use your audio inputs?";
            e.message = "Recording, input monitoring and your voice in a shared session come in through your audio "
                        "interface or microphone. Sonora opens an input only for those; playing a set never needs one.\n\n"
                        "macOS will ask you next whether Sonora may use the microphone.";
            e.deniedText = "Sonora may not use audio inputs. Allow it in System Settings > Privacy & Security > Microphone.";
            e.settingsKey = "privacyNotNowMicrophone";
            break;
        case Access::Camera:
            e.title = "Show your camera in this session?";
            e.message = "The people in this shared session see your camera while you have it on. It starts off in every "
                        "session and stops when you turn it off or leave.\n\n"
                        "macOS will ask you next whether Sonora may use the camera.";
            e.deniedText = "Sonora may not use the camera. Allow it in System Settings > Privacy & Security > Camera.";
            e.settingsKey = "privacyNotNowCamera";
            break;
        case Access::LocalNetwork:
            e.title = "Find collaborators on your network?";
            e.message = "Shared sessions connect straight to the other people's computers when they are on the same "
                        "Wi-Fi or local network, and the Join list shows sessions nearby. Sonora uses the local network "
                        "only for shared sessions.\n\n"
                        "macOS will ask you next whether Sonora may find devices on your local network.";
            e.deniedText = "Sonora may not use the local network. Allow it in System Settings > Privacy & Security > Local Network.";
            e.settingsKey = "privacyNotNowLocalNetwork";
            break;
    }
    return e;
}

} // namespace sonora::core
