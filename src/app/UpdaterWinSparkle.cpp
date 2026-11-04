// COM-4l: Windows updates through WinSparkle (third_party/winsparkle, WinSparkle.dll next to Sonora.exe).
//
// WinSparkle reads appcast-windows.xml (Sparkle format), shows its own update window, downloads the installer, verifies
// the EdDSA signature with the same key as macOS, asks Sonora whether it may close, and runs the Inno Setup installer with
// the item's sparkle:installerArguments (/SILENT ...); packaging/windows/sonora.iss starts Sonora again after a silent
// install. Sonora answers "may I close?" like the macOS restart: not while recording, rendering or playing, and only after
// unsaved changes were offered a save. Configuration is compiled in (src/CMakeLists.txt). See docs/UPDATES.md.
#include "app/Updater.h"
#include "util/Log.h"

#include <juce_events/juce_events.h>

#include <cstdlib>
#include <future>
#include <mutex>

#include <winsparkle.h>

namespace sonora::app
{
namespace
{
constexpr const char* feedUrl = SONORA_UPDATE_FEED_URL_STRING;
constexpr const char* publicKey = SONORA_UPDATE_PUBLIC_KEY_STRING;
constexpr const char* appVersion = SONORA_APP_VERSION_STRING;
} // namespace

class WinSparkleUpdater final : public Updater
{
public:
    explicit WinSparkleUpdater(Host host) : Updater(std::move(host))
    {
        automatic_ = ! host_.loadSetting || host_.loadSetting("updateAutomaticChecks") != "0";
        if (std::string(feedUrl).empty() || std::string(publicKey).empty())
        {
            reason_ = "Updates are off in this build (no update feed or signing key configured)";
            logf("updates: off, no feed/key compiled in (development build)");
            return;
        }
        instance() = this;
        win_sparkle_set_appcast_url(feedUrl);
        if (win_sparkle_set_eddsa_public_key(publicKey) == 0)
        {
            reason_ = "Updates are off in this build (the signing key is not valid)";
            logf("updates: off, WinSparkle rejected the public key");
            instance() = nullptr;
            return;
        }
        win_sparkle_set_app_details(L"Sonora", L"Sonora", version_.toWideCharPointer());
        win_sparkle_set_app_build_version(version_.toWideCharPointer());
        win_sparkle_set_registry_path("Software\\Sonora\\WinSparkle");
        win_sparkle_set_automatic_check_for_updates(automatic_ ? 1 : 0); // also skips WinSparkle's first-run question
        win_sparkle_set_update_check_interval(24 * 60 * 60);
        win_sparkle_set_can_shutdown_callback(&canShutdown);
        win_sparkle_set_shutdown_request_callback(&shutdownRequest);
        win_sparkle_set_error_callback(&error);
        win_sparkle_set_did_find_update_callback(&foundUpdate);
        win_sparkle_set_did_not_find_update_callback(&noUpdate);
        win_sparkle_init();
        running_ = true;
        logf("updates: %s, feed %s, automatic checks %s, version %s", backendName().c_str(), feedUrl, automatic_ ? "on" : "off",
             appVersion);
#if SONORA_UPDATE_LOCAL_TEST
        if (const char* mode = std::getenv("SONORA_UPDATE_AUTOTEST"); mode != nullptr && *mode != 0)
        {
            logf("updates: autotest, check and install");
            win_sparkle_check_update_with_ui_and_install();
        }
#endif
    }

    ~WinSparkleUpdater() override
    {
        answerShutdown(false); // WinSparkle's thread may be waiting for an answer; cleanup joins it
        if (running_) win_sparkle_cleanup();
        instance() = nullptr;
        alive_.reset();
    }

    bool available() const override { return running_; }
    std::string unavailableReason() const override { return reason_; }
    std::string backendName() const override { return std::string("WinSparkle ") + WIN_SPARKLE_VERSION_STRING; }
    std::string currentVersion() const override { return appVersion; }

    bool canCheckNow() const override { return running_; }
    void checkNow() override
    {
        if (running_) win_sparkle_check_update_with_ui();
    }
    bool automaticChecks() const override { return running_ && automatic_; }
    void setAutomaticChecks(bool enabled) override
    {
        automatic_ = enabled;
        if (running_) win_sparkle_set_automatic_check_for_updates(enabled ? 1 : 0);
        if (host_.storeSetting) host_.storeSetting("updateAutomaticChecks", enabled ? "1" : "0");
        if (host_.changed) host_.changed();
    }
    // WinSparkle has no channels: Windows follows the stable feed only.
    UpdateChannel channel() const override { return UpdateChannel::Stable; }
    void setChannel(UpdateChannel) override {}
    std::vector<UpdateChannel> channels() const override { return {UpdateChannel::Stable}; }

    // WinSparkle keeps the downloaded installer and its own "Install update" window; nothing is pending on Sonora's side.
    bool installPending() const override { return false; }
    void installPendingNow() override {}

private:
    static WinSparkleUpdater*& instance()
    {
        static WinSparkleUpdater* updater = nullptr;
        return updater;
    }

    // Callbacks arrive on WinSparkle's thread; everything Sonora does runs on the message thread.
    static void onMessageThread(std::function<void(WinSparkleUpdater&)> work)
    {
        juce::MessageManager::callAsync([work = std::move(work)] {
            if (auto* self = instance()) work(*self);
        });
    }

    static int __cdecl canShutdown()
    {
        std::future<bool> answer;
        {
            auto* self = instance();
            if (self == nullptr) return 0;
            std::lock_guard<std::mutex> lock(self->shutdownMutex_);
            self->shutdownAnswer_ = std::promise<bool>();
            self->shutdownAsked_ = true;
            answer = self->shutdownAnswer_.get_future();
        }
        onMessageThread([](WinSparkleUpdater& self) { self.askToClose(); });
        return answer.get() ? 1 : 0;
    }

    static void __cdecl shutdownRequest()
    {
        onMessageThread([](WinSparkleUpdater&) {
            logf("updates: installer started, quitting");
            if (auto* app = juce::JUCEApplicationBase::getInstance()) app->systemRequestedQuit(); // restart already approved
        });
    }

    static void __cdecl error() { onMessageThread([](WinSparkleUpdater& self) { self.status("Could not check for updates"); }); }
    static void __cdecl foundUpdate() { logf("updates: WinSparkle found an update"); }
    static void __cdecl noUpdate() { logf("updates: no update"); }

    void askToClose()
    {
        const auto blocker = host_.session ? installBlocker(host_.session()) : InstallBlocker::None;
        if (waitsForSession(blocker))
        {
            status(blockerMessage(blocker));
            answerShutdown(false);
            return;
        }
        if (! host_.prepareForRestart) { answerShutdown(true); return; }
        host_.prepareForRestart([this, alive = std::weak_ptr<bool>(alive_)](bool proceed) {
            if (! alive.expired()) answerShutdown(proceed);
        });
    }

    void answerShutdown(bool proceed)
    {
        std::lock_guard<std::mutex> lock(shutdownMutex_);
        if (! shutdownAsked_) return;
        shutdownAsked_ = false;
        shutdownAnswer_.set_value(proceed);
    }

    void status(const std::string& text)
    {
        logf("updates: %s", text.c_str());
        if (host_.status) host_.status(text);
    }

    std::string reason_;
    juce::String version_ {appVersion}; // WinSparkle keeps the wide string pointers
    bool running_ = false, automatic_ = true;
    std::mutex shutdownMutex_;
    std::promise<bool> shutdownAnswer_;
    bool shutdownAsked_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

std::unique_ptr<Updater> Updater::create(Host host) { return std::make_unique<WinSparkleUpdater>(std::move(host)); }

} // namespace sonora::app
