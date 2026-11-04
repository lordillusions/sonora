// COM-4l: Linux updates for the two packages Sonora ships (docs/UPDATES.md "Windows and Linux").
//
// The feed (appcast-linux.xml, Sparkle format, same EdDSA key as macOS) has one item per package kind. The updater checks
// it once a day (or from Help > Check for Updates...), downloads the matching file, verifies length and signature, then:
// - AppImage ($APPIMAGE): replaces the AppImage file in place (rename; the running copy keeps its mounted image), so the
//   next start is the new version; "Install and Restart" relaunches it.
// - .deb (/opt/sonora): keeps the package and installs it with `pkexec apt-get install`, which asks for the password.
// Restarts wait for recording, renders and playback and offer to save first, as on macOS (UpdatePolicy.h).
// Configuration is compiled in (SONORA_UPDATE_FEED_URL / SONORA_UPDATE_PUBLIC_KEY, src/CMakeLists.txt).
#include "app/UpdateFeed.h"
#include "app/Updater.h"
#include "util/Log.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace sonora::app
{
namespace
{
constexpr const char* feedUrl = SONORA_UPDATE_FEED_URL_STRING;
constexpr const char* publicKey = SONORA_UPDATE_PUBLIC_KEY_STRING;
constexpr const char* appVersion = SONORA_APP_VERSION_STRING;
constexpr const char* debExecutable = "/opt/sonora/Sonora";
constexpr int64_t checkIntervalSeconds = 24 * 60 * 60;

enum class Package { None, AppImage, Deb };

// Starts `path` once this process has exited (Sonora allows one instance), detached from it.
bool relaunchAfterExit(const std::string& path)
{
    const std::string pid = std::to_string(getpid());
    // The AppImage runtime's variables describe the old mount, which is gone by then; the autotest runs once.
    const char* script = "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; "
                         "unset APPDIR APPIMAGE ARGV0 OWD SONORA_UPDATE_AUTOTEST; exec \"$2\"";
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0)
    {
        // Only async-signal-safe calls between fork and exec.
        setsid();
        const int null = open("/dev/null", O_RDWR);
        if (null >= 0)
        {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        execl("/bin/sh", "sh", "-c", script, "sh", pid.c_str(), path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
}

// Downloads url into file; checks the HTTP status and the expected length. Runs on a worker thread.
std::string download(const std::string& url, const juce::File& file, int64_t expectedLength)
{
    int status = 0;
    auto in = juce::URL(juce::String(url)).createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                                  .withConnectionTimeoutMs(20000)
                                                                  .withNumRedirectsToFollow(8)
                                                                  .withStatusCode(&status));
    if (in == nullptr || status != 200) return "could not download " + url + " (HTTP " + std::to_string(status) + ")";
    file.deleteFile();
    {
        juce::FileOutputStream out(file);
        if (! out.openedOk()) return "could not write " + file.getFullPathName().toStdString();
        if (out.writeFromInputStream(*in, -1) < 0 || ! out.getStatus().wasOk()) return "download of " + url + " failed";
    }
    if (expectedLength > 0 && file.getSize() != expectedLength)
        return "download of " + url + " is " + std::to_string(file.getSize()) + " bytes, the feed says "
               + std::to_string(expectedLength);
    return {};
}
} // namespace

class LinuxUpdater final : public Updater, private juce::Timer
{
public:
    explicit LinuxUpdater(Host host) : Updater(std::move(host))
    {
        channel_ = channelFromName(host_.loadSetting ? host_.loadSetting("updateChannel") : std::string());
        if (channel_ == UpdateChannel::Nightly) channel_ = UpdateChannel::Stable;
        automatic_ = ! host_.loadSetting || host_.loadSetting("updateAutomaticChecks") != "0";

        if (std::string(feedUrl).empty() || std::string(publicKey).empty())
        {
            reason_ = "Updates are off in this build (no update feed or signing key configured)";
            logf("updates: off, no feed/key compiled in (development build)");
            return;
        }
        if (! edSignatureCheckAvailable())
        {
            reason_ = "Updates are off in this build (no Ed25519 implementation; run scripts/bootstrap.sh)";
            logf("updates: off, built without Monocypher");
            return;
        }
        if (const char* appImage = std::getenv("APPIMAGE"); appImage != nullptr && *appImage != 0)
        {
            package_ = Package::AppImage;
            target_ = juce::File(juce::String::fromUTF8(appImage));
        }
        else if (juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName() == debExecutable)
        {
            package_ = Package::Deb;
            target_ = juce::File(debExecutable);
        }
        else
        {
            reason_ = "Updates work in the AppImage and in the .deb package (/opt/sonora); this copy is neither";
            logf("updates: off, not running from an AppImage or /opt/sonora");
            return;
        }
        logf("updates: Linux %s updater, feed %s, channel %s, automatic checks %s, version %s",
             package_ == Package::AppImage ? "AppImage" : ".deb", feedUrl, channelName(channel_), automatic_ ? "on" : "off",
             appVersion);
#if SONORA_UPDATE_LOCAL_TEST
        // Local update tests only (docs/UPDATES.md): check at once, download and restart without asking.
        if (const char* mode = std::getenv("SONORA_UPDATE_AUTOTEST"); mode != nullptr && *mode != 0)
        {
            autotest_ = true;
            logf("updates: autotest, checking now");
            juce::Timer::callAfterDelay(500, [this, alive = std::weak_ptr<bool>(alive_)] { if (! alive.expired()) check(true); });
        }
#endif
        startTimer(15 * 1000); // first scheduled check shortly after launch, then hourly
    }

    ~LinuxUpdater() override
    {
        stopTimer();
        alive_.reset();
    }

    bool available() const override { return package_ != Package::None; }
    std::string unavailableReason() const override { return reason_; }
    std::string backendName() const override
    {
        return package_ == Package::AppImage ? "Sonora (AppImage)" : package_ == Package::Deb ? "Sonora (.deb)" : "none";
    }
    std::string currentVersion() const override { return appVersion; }

    bool canCheckNow() const override { return available() && ! busy_; }
    void checkNow() override
    {
        if (! available() || busy_) return;
        if (installPending()) { installPendingNow(); return; }
        if (found_) { fetch(*found_); return; } // a scheduled check found it: download now
        check(true);
    }
    bool automaticChecks() const override { return available() && automatic_; }
    void setAutomaticChecks(bool enabled) override
    {
        automatic_ = enabled;
        if (host_.storeSetting) host_.storeSetting("updateAutomaticChecks", enabled ? "1" : "0");
        changed();
    }
    UpdateChannel channel() const override { return channel_; }
    void setChannel(UpdateChannel channel) override
    {
        if (channel == UpdateChannel::Nightly) return;
        channel_ = channel;
        found_.reset();
        if (host_.storeSetting) host_.storeSetting("updateChannel", channelName(channel));
        changed();
    }
    std::vector<UpdateChannel> channels() const override { return selectableChannels(false); }

    bool installPending() const override { return ! pendingVersion_.empty(); }
    void installPendingNow() override
    {
        waited_ = false;
        tryInstall();
    }

private:
    void changed() { if (host_.changed) host_.changed(); }
    void status(const std::string& text)
    {
        logf("updates: %s", text.c_str());
        if (host_.status) host_.status(text);
    }

    // Runs work on a worker thread and then done(result) on the message thread, unless this updater is gone.
    template <typename Work, typename Done>
    void inBackground(Work work, Done done)
    {
        busy_ = true;
        changed();
        std::thread([work = std::move(work), done = std::move(done), this, alive = std::weak_ptr<bool>(alive_)]() mutable {
            auto result = work();
            juce::MessageManager::callAsync([result = std::move(result), done = std::move(done), this, alive]() mutable {
                if (alive.expired()) return;
                busy_ = false;
                done(std::move(result));
                changed();
            });
        }).detach();
    }

    void check(bool userInitiated)
    {
        if (busy_) return;
        if (userInitiated) status("Checking for updates...");
        if (host_.storeSetting) host_.storeSetting("updateLastCheck", std::to_string((long long) std::time(nullptr)));
        inBackground(
            [] {
                int code = 0;
                auto in = juce::URL(juce::String(feedUrl)).createInputStream(
                    juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs(20000)
                        .withStatusCode(&code));
                std::pair<int, std::string> result {code, {}};
                if (in != nullptr && code == 200) result.second = in->readEntireStreamAsString().toStdString();
                return result;
            },
            [this, userInitiated](std::pair<int, std::string> result) {
                if (result.first != 200)
                {
                    status("Could not check for updates (HTTP " + std::to_string(result.first) + ")");
                    return;
                }
                const auto items = parseFeed(result.second);
                found_ = newestUpdate(items, packageOs(), appVersion, allowedFeedChannels(channel_));
                if (! found_)
                {
                    if (userInitiated) status(std::string("Sonora ") + appVersion + " is up to date");
                    else logf("updates: no update (%zu items in the feed)", items.size());
                    return;
                }
                logf("updates: found %s", found_->version.c_str());
                if (userInitiated) fetch(*found_);
                else status("Sonora " + found_->version + " is available: Help > Check for Updates... installs it");
            });
    }

    std::string packageOs() const { return package_ == Package::AppImage ? "linux-x64-appimage" : "linux-x64-deb"; }

    juce::File downloadFile(const FeedItem& item) const
    {
        if (package_ == Package::AppImage) return target_.getSiblingFile("." + target_.getFileName() + ".update");
        return juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("sonora_" + juce::String(item.version) + "_amd64.deb");
    }

    void fetch(const FeedItem& item)
    {
        if (package_ == Package::AppImage && ! target_.getParentDirectory().hasWriteAccess())
        {
            status("Sonora " + item.version + " is available, but " + target_.getParentDirectory().getFullPathName().toStdString()
                   + " is read-only: download it from sonoradaw.com");
            return;
        }
        status("Downloading Sonora " + item.version + "...");
        const auto file = downloadFile(item);
        inBackground(
            [item, file] {
                if (auto error = download(item.url, file, item.length); ! error.empty()) return error;
                juce::MemoryBlock data;
                if (! file.loadFileAsData(data)) return "could not read " + file.getFullPathName().toStdString();
                if (! verifyEdSignature(publicKey, data.getData(), data.getSize(), item.signature))
                    return std::string("the download's signature is not valid: it was not installed");
                return std::string();
            },
            [this, item, file](std::string error) {
                if (! error.empty())
                {
                    file.deleteFile();
                    status("Update failed: " + error);
                    return;
                }
                if (package_ == Package::AppImage)
                {
                    // Replace the AppImage now: the next start is the new version even without "Install and Restart".
                    file.setExecutePermission(true);
                    if (! file.moveFileTo(target_))
                    {
                        file.deleteFile();
                        status("Update failed: could not replace " + target_.getFullPathName().toStdString());
                        return;
                    }
                }
                else
                {
                    debPackage_ = file;
                }
                found_.reset();
                pendingVersion_ = item.version;
                logf("updates: %s downloaded and verified", item.version.c_str());
                tryInstall();
            });
    }

    void tryInstall()
    {
        if (! installPending()) return;
        const auto blocker = host_.session ? installBlocker(host_.session()) : InstallBlocker::None;
        if (waitsForSession(blocker) && ! autotest_)
        {
            status(blockerMessage(blocker));
            waited_ = true;
            startTimer(500);
            changed();
            return;
        }
        if (waited_)
        {
            // The session just became idle: no dialog here (Space/Return meant for the transport would answer it).
            waited_ = false;
            status("Sonora " + pendingVersion_ + " is ready: Help > Check for Updates... installs it and restarts");
            changed();
            return;
        }
        if (autotest_ || ! host_.prepareForRestart) { install(); return; }
        host_.prepareForRestart([this, alive = std::weak_ptr<bool>(alive_)](bool proceed) {
            if (alive.expired()) return;
            if (proceed) install();
            else status("Sonora " + pendingVersion_ + " postponed: Help > Check for Updates... installs it and restarts");
        });
    }

    void install()
    {
        if (package_ == Package::AppImage)
        {
            restart(target_.getFullPathName().toStdString());
            return;
        }
        status("Installing Sonora " + pendingVersion_ + " (your password is needed)...");
        const auto package = debPackage_.getFullPathName().toStdString();
        inBackground(
            [package] {
                juce::ChildProcess apt;
                if (! apt.start(juce::StringArray {"pkexec", "/usr/bin/apt-get", "install", "-y", juce::String(package)}))
                    return std::string("pkexec is not available: install ") + package + " with your package manager";
                const auto output = apt.readAllProcessOutput();
                if (apt.getExitCode() != 0) return "installation was cancelled or failed (" + output.trim().toStdString() + ")";
                return std::string();
            },
            [this](std::string error) {
                if (! error.empty())
                {
                    status("Sonora " + pendingVersion_ + " was not installed: " + error);
                    return;
                }
                debPackage_.deleteFile();
                restart(debExecutable);
            });
    }

    void restart(const std::string& executable)
    {
        logf("updates: restarting into %s (%s)", pendingVersion_.c_str(), executable.c_str());
        pendingVersion_.clear();
        if (! relaunchAfterExit(executable)) status("Updated: quit and start Sonora again to use the new version");
        if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit();
    }

    void timerCallback() override
    {
        if (installPending() && waited_)
        {
            if (! host_.session || ! waitsForSession(installBlocker(host_.session()))) tryInstall();
            return;
        }
        startTimer(60 * 60 * 1000);
        if (! automatic_ || busy_ || found_ || installPending()) return;
        const auto last = host_.loadSetting ? std::atoll(host_.loadSetting("updateLastCheck").c_str()) : 0;
        if ((int64_t) std::time(nullptr) - (int64_t) last >= checkIntervalSeconds) check(false);
    }

    Package package_ = Package::None;
    juce::File target_, debPackage_;
    std::optional<FeedItem> found_;
    std::string reason_, pendingVersion_;
    UpdateChannel channel_ = UpdateChannel::Stable;
    bool automatic_ = true, waited_ = false, autotest_ = false;
    std::atomic<bool> busy_ {false};
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

std::unique_ptr<Updater> Updater::create(Host host) { return std::make_unique<LinuxUpdater>(std::move(host)); }

} // namespace sonora::app
