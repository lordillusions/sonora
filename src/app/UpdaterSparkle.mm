// COM-4: macOS updates through Sparkle 2 (third_party/sparkle, embedded in Sonora.app/Contents/Frameworks).
//
// Sparkle downloads, verifies the EdDSA signature, applies a delta when the appcast offers one for this version (falling back
// to the full archive) and relaunches. Sonora adds: channels (Info.plist SonoraUpdateChannel + Settings), quiet scheduled
// checks while playing/recording, and a postponed restart until the session is idle and unsaved changes were offered a save.
// Configuration comes from Info.plist (SUFeedURL, SUPublicEDKey), written by src/CMakeLists.txt. See docs/UPDATES.md.
#include "app/Updater.h"
#include "util/Log.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>

#import <AppKit/AppKit.h>
#import <Sparkle/Sparkle.h>

namespace sonora::app
{
class SparkleUpdater;
}

@interface SonoraSparkleDelegate : NSObject <SPUUpdaterDelegate, SPUStandardUserDriverDelegate>
@property(nonatomic, assign) sonora::app::SparkleUpdater* owner;
@end

namespace sonora::app
{
namespace
{
std::string toStd(NSString* text)
{
    const char* utf8 = text != nil ? [text UTF8String] : nullptr;
    return utf8 != nullptr ? std::string(utf8) : std::string();
}

std::string plistString(NSString* key)
{
    id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
    return [value isKindOfClass:[NSString class]] ? toStd((NSString*) value) : std::string();
}
} // namespace

class SparkleUpdater final : public Updater, private juce::Timer
{
public:
    explicit SparkleUpdater(Host host) : Updater(std::move(host))
    {
        nightlyBuild_ = plistString(@"SonoraUpdateChannel") == "nightly";
        channel_ = nightlyBuild_ ? UpdateChannel::Nightly
                                 : channelFromName(host_.loadSetting ? host_.loadSetting("updateChannel") : std::string());
        if (channel_ == UpdateChannel::Nightly && ! nightlyBuild_) channel_ = UpdateChannel::Stable;

        if (plistString(@"SUFeedURL").empty() || plistString(@"SUPublicEDKey").empty())
        {
            reason_ = "Updates are off in this build (no update feed or signing key configured)";
            logf("updates: off, Info.plist has no SUFeedURL/SUPublicEDKey (development build)");
            return;
        }
        delegate_ = [[SonoraSparkleDelegate alloc] init];
        delegate_.owner = this;
        controller_ = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO updaterDelegate:delegate_
                                                                  userDriverDelegate:delegate_];
        NSError* error = nil;
        if (! [controller_.updater startUpdater:&error])
        {
            reason_ = "The updater could not start: " + toStd(error.localizedDescription);
            logf("updates: Sparkle failed to start: %s", toStd(error.localizedDescription).c_str());
            controller_ = nil;
            return;
        }
        logf("updates: %s, feed %s, channel %s, automatic checks %s, version %s", backendName().c_str(),
             toStd(controller_.updater.feedURL.absoluteString).c_str(), channelName(channel_),
             controller_.updater.automaticallyChecksForUpdates ? "on" : "off", currentVersion().c_str());
#if SONORA_UPDATE_LOCAL_TEST
        // Local update test builds only (SONORA_UPDATE_ALLOW_LOCAL_HTTP): download in the background and install at once,
        // so docs/UPDATES.md can verify delta/full updates without clicking Sparkle's window.
        if (const char* mode = std::getenv("SONORA_UPDATE_AUTOTEST"); mode != nullptr && *mode != 0)
        {
            autotest_ = true;
            controller_.updater.automaticallyDownloadsUpdates = YES;
            const bool busy = std::string(mode) == "busy"; // MainWindow starts playback first
            logf("updates: autotest, background check%s", busy ? " in 5 s" : "");
            SPUUpdater* updater = controller_.updater;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (busy ? 5 : 0) * (int64_t) NSEC_PER_SEC),
                           dispatch_get_main_queue(), ^{ [updater checkForUpdatesInBackground]; });
        }
#endif
    }

    ~SparkleUpdater() override
    {
        stopTimer();
        delegate_.owner = nullptr;
    }

    bool available() const override { return controller_ != nil; }
    std::string unavailableReason() const override { return reason_; }
    std::string backendName() const override
    {
        NSBundle* sparkle = [NSBundle bundleForClass:[SPUUpdater class]];
        return "Sparkle " + toStd([sparkle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]);
    }
    std::string currentVersion() const override
    {
        const auto version = plistString(@"CFBundleShortVersionString"), build = plistString(@"CFBundleVersion");
        return build.empty() || build == version ? version : version + " (" + build + ")";
    }

    bool canCheckNow() const override { return controller_ != nil && controller_.updater.canCheckForUpdates; }
    void checkNow() override
    {
        if (installPending()) { installPendingNow(); return; }
        if (canCheckNow()) [controller_ checkForUpdates:nil];
    }
    bool automaticChecks() const override { return controller_ != nil && controller_.updater.automaticallyChecksForUpdates; }
    void setAutomaticChecks(bool enabled) override
    {
        if (controller_ == nil) return;
        controller_.updater.automaticallyChecksForUpdates = enabled; // Sparkle persists it in the app's user defaults
        changed();
    }
    UpdateChannel channel() const override { return channel_; }
    void setChannel(UpdateChannel channel) override
    {
        if (nightlyBuild_ || channel == UpdateChannel::Nightly) return; // nightly is a separate app with its own feed
        channel_ = channel;
        if (host_.storeSetting) host_.storeSetting("updateChannel", channelName(channel));
        changed();
    }
    std::vector<UpdateChannel> channels() const override { return selectableChannels(nightlyBuild_); }

    bool installPending() const override { return install_ != nil; }
    void installPendingNow() override { waited_ = false; tryInstall(); }

    // ---- SonoraSparkleDelegate
    NSSet<NSString*>* allowedChannels() const
    {
        NSMutableSet<NSString*>* set = [NSMutableSet set];
        for (const auto& name : allowedFeedChannels(channel_))
            if (NSString* tag = [NSString stringWithUTF8String:name.c_str()]) [set addObject:tag];
        return set;
    }

    // Sparkle asks before it quits to install. YES keeps the handler until the session allows a restart.
    bool postponeRelaunch(SUAppcastItem* item, void (^handler)(void))
    {
        const auto blocker = host_.session ? installBlocker(host_.session()) : InstallBlocker::None;
        logf("updates: %s ready to install, %s", toStd(item.displayVersionString).c_str(),
             blocker == InstallBlocker::None ? "restarting" : blockerMessage(blocker).c_str());
        if (blocker == InstallBlocker::None) return false;
        install_ = [handler copy];
        pendingVersion_ = toStd(item.displayVersionString);
        waited_ = false;
        std::weak_ptr<bool> alive = alive_;
        dispatch_async(dispatch_get_main_queue(), ^{ if (! alive.expired()) tryInstall(); }); // after Sparkle's delegate call returns
        return true;
    }

    // A scheduled check shows Sparkle's window only right after launch (immediate focus) with an idle session. Any other time
    // a window appearing mid-session would take Space/Return meant for the transport; the status bar says an update exists.
    bool showScheduledUpdate(bool immediateFocus) const
    {
        return immediateFocus && ! (host_.session && waitsForSession(installBlocker(host_.session())));
    }
    void scheduledUpdateNotShown(SUAppcastItem* item)
    {
        status("Sonora " + toStd(item.displayVersionString) + " is available: Help > Check for Updates...");
    }

    void changed() { if (host_.changed) host_.changed(); }
    bool autotest() const { return autotest_; }

private:
    void status(const std::string& text)
    {
        logf("updates: %s", text.c_str());
        if (host_.status) host_.status(text);
    }

    void tryInstall()
    {
        if (install_ == nil) return;
        const auto blocker = host_.session ? installBlocker(host_.session()) : InstallBlocker::None;
        if (waitsForSession(blocker))
        {
            status(blockerMessage(blocker));
            waited_ = true;
            startTimer(500);
            changed();
            return;
        }
        stopTimer();
        if (waited_)
        {
            // The session just became idle. Never pop a dialog here: Space/Return meant for the transport would answer it.
            // The user restarts from Help > Check for Updates..., or Sparkle installs the update when Sonora quits.
            waited_ = false;
            status("Sonora " + pendingVersion_ + " is ready: Help > Check for Updates... restarts now, or it installs on quit");
            changed();
            return;
        }
        offerSaveThenInstall();
    }

    void offerSaveThenInstall()
    {
        if (! host_.prepareForRestart) { install(); return; }
        host_.prepareForRestart([this, alive = std::weak_ptr<bool>(alive_)](bool proceed) {
            if (alive.expired()) return;
            if (proceed) install(); else postponed();
        });
    }

    void install()
    {
        logf("updates: restart approved, handing over to Sparkle");
        auto handler = install_;
        install_ = nil;
        changed();
        if (handler != nil) handler(); // Sparkle quits the app (normal quit path), installs and relaunches
    }

    void postponed()
    {
        status("Sonora " + pendingVersion_ + " postponed: Help > Check for Updates... restarts now, or it installs on quit");
        changed();
    }

    void timerCallback() override
    {
        if (host_.session && ! waitsForSession(installBlocker(host_.session()))) tryInstall();
    }

    SPUStandardUpdaterController* controller_ = nil;
    SonoraSparkleDelegate* delegate_ = nil;
    void (^install_)(void) = nil;
    std::string pendingVersion_, reason_;
    UpdateChannel channel_ = UpdateChannel::Stable;
    bool nightlyBuild_ = false, waited_ = false, autotest_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

std::unique_ptr<Updater> Updater::create(Host host) { return std::make_unique<SparkleUpdater>(std::move(host)); }

} // namespace sonora::app

@implementation SonoraSparkleDelegate

- (NSSet<NSString*>*)allowedChannelsForUpdater:(SPUUpdater*)updater
{
    (void) updater;
    return _owner != nullptr ? _owner->allowedChannels() : [NSSet set];
}

- (BOOL)updater:(SPUUpdater*)updater shouldPostponeRelaunchForUpdate:(SUAppcastItem*)item untilInvokingBlock:(void (^)(void))installHandler
{
    (void) updater;
    return _owner != nullptr && _owner->postponeRelaunch(item, installHandler);
}

- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item
{
    (void) updater;
    sonora::logf("updates: found %s (build %s)", item.displayVersionString.UTF8String, item.versionString.UTF8String);
}

- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater error:(NSError*)error
{
    (void) updater;
    sonora::logf("updates: no update (%s)", error.localizedDescription.UTF8String);
}

- (void)updater:(SPUUpdater*)updater willDownloadUpdate:(SUAppcastItem*)item withRequest:(NSMutableURLRequest*)request
{
    (void) updater;
    (void) item;
    const bool delta = [request.URL.pathExtension isEqualToString:@"delta"];
    sonora::logf("updates: downloading %s update %s", delta ? "delta" : "full", request.URL.absoluteString.UTF8String);
}

- (void)updater:(SPUUpdater*)updater failedToDownloadUpdate:(SUAppcastItem*)item error:(NSError*)error
{
    (void) updater;
    sonora::logf("updates: download of %s failed: %s", item.displayVersionString.UTF8String, error.localizedDescription.UTF8String);
}

- (void)updater:(SPUUpdater*)updater didExtractUpdate:(SUAppcastItem*)item
{
    (void) updater;
    sonora::logf("updates: %s extracted", item.displayVersionString.UTF8String);
}

- (void)updater:(SPUUpdater*)updater willInstallUpdate:(SUAppcastItem*)item
{
    (void) updater;
    sonora::logf("updates: installing %s", item.displayVersionString.UTF8String);
}

- (void)updaterWillRelaunchApplication:(SPUUpdater*)updater
{
    (void) updater;
    sonora::logf("updates: relaunching");
}

- (void)updater:(SPUUpdater*)updater didAbortWithError:(NSError*)error
{
    (void) updater;
    sonora::logf("updates: aborted: %s", error.localizedDescription.UTF8String);
}

- (void)updater:(SPUUpdater*)updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)updateCheck error:(NSError*)error
{
    (void) updater;
    (void) updateCheck;
    (void) error;
    if (_owner != nullptr) _owner->changed();
}

#if SONORA_UPDATE_LOCAL_TEST
- (BOOL)updater:(SPUUpdater*)updater willInstallUpdateOnQuit:(SUAppcastItem*)item immediateInstallationBlock:(void (^)(void))immediateInstallHandler
{
    (void) updater;
    if (_owner == nullptr || ! _owner->autotest()) return NO;
    sonora::logf("updates: autotest, installing %s now", item.displayVersionString.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{ immediateInstallHandler(); });
    return YES;
}
#endif

// ---- SPUStandardUserDriverDelegate
- (BOOL)supportsGentleScheduledUpdateReminders
{
    return YES;
}

- (BOOL)standardUserDriverShouldHandleShowingScheduledUpdate:(SUAppcastItem*)update andInImmediateFocus:(BOOL)immediateFocus
{
    (void) update;
    return _owner == nullptr || _owner->showScheduledUpdate(immediateFocus);
}

- (void)standardUserDriverWillHandleShowingUpdate:(BOOL)handleShowingUpdate forUpdate:(SUAppcastItem*)update state:(SPUUserUpdateState*)state
{
    (void) state;
    if (! handleShowingUpdate && _owner != nullptr) _owner->scheduledUpdateNotShown(update);
}

@end
