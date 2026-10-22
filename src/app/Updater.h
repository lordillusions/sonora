#pragma once
// COM-4: application updates behind a small platform-neutral interface. macOS uses Sparkle 2 (UpdaterSparkle.mm); other
// platforms and builds without a configured feed get the inert stub (UpdaterStub.cpp) until COM-7 adds Velopack (Windows)
// or Flatpak (Linux). Everything runs on the message thread, never near the audio callback. See docs/UPDATES.md.

#include "app/UpdatePolicy.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::app
{

class Updater
{
public:
    // What the updater needs from the app. All callbacks run on the message thread.
    struct Host
    {
        std::function<UpdateSessionState()> session;
        // Offers to save unsaved changes before a restart; done(true) means the app may quit now (saved or discarded).
        std::function<void(std::function<void(bool)> done)> prepareForRestart;
        std::function<void(const std::string&)> status; // one line in the status bar
        std::function<void()> changed;                  // availability/pending state changed: refresh menus and Settings
        std::function<std::string(const std::string&)> loadSetting;
        std::function<void(const std::string&, const std::string&)> storeSetting;
    };

    // The platform implementation, or the stub when this build has no updater.
    static std::unique_ptr<Updater> create(Host host);
    virtual ~Updater() = default;

    virtual bool available() const = 0;              // false: updates are off in this build (see unavailableReason)
    virtual std::string unavailableReason() const = 0;
    virtual std::string backendName() const = 0;     // "Sparkle 2.9.6", "none"
    virtual std::string currentVersion() const = 0;  // "0.1.0 (100)"

    virtual bool canCheckNow() const = 0;
    virtual void checkNow() = 0;                     // user-initiated: always shows a result
    virtual bool automaticChecks() const = 0;
    virtual void setAutomaticChecks(bool enabled) = 0;
    virtual UpdateChannel channel() const = 0;
    virtual void setChannel(UpdateChannel channel) = 0;
    virtual std::vector<UpdateChannel> channels() const = 0;

    // An update was downloaded but the restart was postponed (busy session or "Later").
    virtual bool installPending() const = 0;
    virtual void installPendingNow() = 0;            // asks again: waits for the session, offers save, then restarts

    bool canInstallNow() const { return host_.session && installBlocker(host_.session()) == InstallBlocker::None; }

protected:
    explicit Updater(Host host) : host_(std::move(host)) {}
    Host host_;
};

} // namespace sonora::app
