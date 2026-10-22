#pragma once
// COM-4: platform-neutral update rules shared by every updater backend (Sparkle on macOS today; Velopack / Flatpak are
// COM-7). Pure logic, no JUCE and no platform API, so it is unit-tested (tests/test_updates.cpp). See docs/UPDATES.md.

#include <string>
#include <string_view>
#include <vector>

namespace sonora::app
{

// Stable is the default. Beta items live in the stable appcast, tagged <sparkle:channel>beta</sparkle:channel>.
// Nightly is a separate app (own bundle id, settings and feed) so it can never overwrite a stable install.
enum class UpdateChannel { Stable, Beta, Nightly };

inline const char* channelName(UpdateChannel channel)
{
    switch (channel)
    {
        case UpdateChannel::Beta: return "beta";
        case UpdateChannel::Nightly: return "nightly";
        case UpdateChannel::Stable: break;
    }
    return "stable";
}

inline UpdateChannel channelFromName(std::string_view name, UpdateChannel fallback = UpdateChannel::Stable)
{
    if (name == "stable") return UpdateChannel::Stable;
    if (name == "beta") return UpdateChannel::Beta;
    if (name == "nightly") return UpdateChannel::Nightly;
    return fallback;
}

// Channels a build offers in Settings. A nightly build follows only its own feed.
inline std::vector<UpdateChannel> selectableChannels(bool nightlyBuild)
{
    if (nightlyBuild) return {UpdateChannel::Nightly};
    return {UpdateChannel::Stable, UpdateChannel::Beta};
}

// Feed channel tags (besides untagged items, which every channel sees) a channel accepts.
inline std::vector<std::string> allowedFeedChannels(UpdateChannel channel)
{
    if (channel == UpdateChannel::Beta) return {"beta"};
    return {};
}

// What the session is doing when an update wants to restart the app.
struct UpdateSessionState
{
    bool recording = false;
    bool exporting = false; // Export Audio, freeze/bounce and other offline renders
    bool playing = false;
    bool dirty = false; // unsaved changes in the open set
};

enum class InstallBlocker { None, Recording, Exporting, Playing, UnsavedChanges };

// The first reason the app may not restart now. Recording/rendering/playback postpone the restart until the user asks
// again (no dialog pops up when the session goes idle); unsaved changes are offered a save first. Never a forced restart.
inline InstallBlocker installBlocker(const UpdateSessionState& state)
{
    if (state.recording) return InstallBlocker::Recording;
    if (state.exporting) return InstallBlocker::Exporting;
    if (state.playing) return InstallBlocker::Playing;
    if (state.dirty) return InstallBlocker::UnsavedChanges;
    return InstallBlocker::None;
}

// Only these wait for the session; unsaved changes are resolved by a save prompt.
inline bool waitsForSession(InstallBlocker blocker)
{
    return blocker == InstallBlocker::Recording || blocker == InstallBlocker::Exporting || blocker == InstallBlocker::Playing;
}

inline std::string blockerMessage(InstallBlocker blocker)
{
    switch (blocker)
    {
        case InstallBlocker::Recording: return "Update downloaded: after recording, Help > Check for Updates... installs it";
        case InstallBlocker::Exporting: return "Update downloaded: after the render, Help > Check for Updates... installs it";
        case InstallBlocker::Playing: return "Update downloaded: after playback, Help > Check for Updates... installs it";
        case InstallBlocker::UnsavedChanges: return "Update downloaded: save or discard changes to install it";
        case InstallBlocker::None: break;
    }
    return {};
}

} // namespace sonora::app
