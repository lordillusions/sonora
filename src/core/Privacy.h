#pragma once
// COM-15: what Sonora may touch without asking, and how it asks for the rest (docs/PLATFORMS.md "Privacy prompts").
//
// macOS shows a privacy prompt the first time an app reads certain places (Desktop, Documents, Downloads, the Photos
// and Apple Music libraries, iCloud Drive, other apps' data, removable and network volumes) or opens the microphone,
// the camera or the local network. Sonora's background scans (Projects list, sound index, browser search) walk only
// folders the person chose plus ~/Music, and never step into one of those places on their own: walkFolder() prunes
// them. Opening a set, or adding a folder to Places, is the person's choice and is walked as is.
//
// The microphone, camera and local network are asked for only when a feature needs them, and Sonora explains why in
// its own prompt first (accessExplanation); only "Continue" lets macOS ask (accessStep decides).
#include <juce_core/juce_core.h>

#include <functional>
#include <string>
#include <vector>

namespace sonora::core
{

// ------------------------------------------------------------------------------------------------ folders

// A media library package another app owns (Photos, Apple Music, TV, Aperture, Photo Booth): never scanned.
bool isMediaLibraryPackage(const juce::File& folder);

// The top folder of the macOS-guarded place that holds `path` (home/Desktop, home/Documents, home/Downloads,
// home/Pictures, home/Movies/TV, home/Music/Music, home/Music/iTunes, home/Library, /Volumes/<volume>), or an empty
// path when none does. `home` is the user's home folder (a parameter so tests can use a fake one).
juce::File promptGuardedArea(const juce::File& path, const juce::File& home);
inline bool isPromptGuarded(const juce::File& path, const juce::File& home) { return promptGuardedArea(path, home) != juce::File(); }

// Lists everything under `root` (not the root itself), depth first. Hidden entries are skipped; so are media library
// packages and every folder in a guarded place other than the root's own (a walk of ~/Music never enters
// ~/Music/Music; a walk of a Place inside ~/Documents stays inside ~/Documents). Symbolic links to folders are
// followed only when they lead outside the root, to an allowed place, and not back up the tree (no cycles).
// `visit(file, isDirectory)` returning false stops the walk; so does `cancelled()` returning true.
void walkFolder(const juce::File& root, const juce::File& home, const std::function<bool(const juce::File&, bool isDirectory)>& visit,
                const std::function<bool()>& cancelled = {});

// Folders the Projects list walks at launch: ~/Music, the chosen folders (Places, User Library) and the folders of
// recent sets that are not in a guarded place. Folders inside another root are dropped.
std::vector<juce::File> projectScanRoots(const juce::File& home, const std::vector<juce::File>& chosen, const std::vector<juce::File>& recentSets);

// ------------------------------------------------------------------------------------------------ explained access

enum class Access
{
    Microphone,   // audio inputs: recording, input monitoring, the collab mic
    Camera,       // collab video
    LocalNetwork, // collab sessions and the nearby sessions list
};

enum class SystemAnswer
{
    Granted,  // allowed in System Settings: nothing to ask
    Denied,   // refused (or restricted): only System Settings can change it
    NotAsked, // macOS has not asked yet
};

// Who wants the access, which decides whether Sonora may show its explanation.
enum class AccessRequest
{
    Automatic, // e.g. a set opened with armed tracks: never shows a prompt
    Browsing,  // e.g. opening the Collaborate panel or the audio settings: explains unless the person said Not Now before
    UserAction, // e.g. arming a track, Record, turning the mic or camera on, starting a session: always explains
};

enum class AccessStep
{
    Proceed, // use it
    Refuse,  // tell the person where to allow it
    Explain, // show accessExplanation; Continue asks macOS
    Skip,    // not now: leave the feature off without a prompt
};

AccessStep accessStep(SystemAnswer answer, AccessRequest request, bool notNowRemembered);

struct AccessExplanation
{
    std::string title, message;
    std::string continueLabel = "Continue", notNowLabel = "Not Now";
    std::string deniedText; // status text when System Settings refuses it
    std::string settingsKey; // the remembered Not Now
};
AccessExplanation accessExplanation(Access access);

} // namespace sonora::core
