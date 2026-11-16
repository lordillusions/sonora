// COM-15: the microphone, the camera and the local network are asked for only when a feature needs them, and Sonora
// says why in its own prompt (PromptWindow, UI-44) before macOS shows its system prompt. "Not Now" is remembered in the
// settings: background requests (a set opened with armed tracks, the Collaborate panel, the audio settings window) then
// stay quiet, while a person's own action (arming a track, Record, turning the mic or camera on, starting a session)
// explains again. Folder scans are scoped in core/Privacy (walkFolder), see docs/PLATFORMS.md "Privacy prompts".
#include "app/AppController.h"
#include "app/PromptWindow.h"
#include "platform/MediaAccess.h"
#include "util/Log.h"

namespace sonora::app
{

namespace
{
constexpr const char* kLocalNetworkContinued = "privacyContinuedLocalNetwork";

platform::MediaKind mediaKind(core::Access access)
{
    return access == core::Access::Camera ? platform::MediaKind::Camera : platform::MediaKind::Microphone;
}

const char* accessName(core::Access access)
{
    switch (access)
    {
        case core::Access::Microphone: return "microphone";
        case core::Access::Camera: return "camera";
        case core::Access::LocalNetwork: return "local network";
    }
    return "?";
}
} // namespace

bool AppController::localNetworkAllowed()
{
    return ! platform::localNetworkPromptExists() || settings().getBoolValue(kLocalNetworkContinued, false)
           || juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_AUTOTEST", {}).isNotEmpty(); // the autotest stands in for the person
}

void AppController::withAccess(core::Access access, core::AccessRequest request, std::function<void(AccessResult)> done)
{
    const auto text = core::accessExplanation(access);
    core::SystemAnswer answer = core::SystemAnswer::NotAsked;
    if (access == core::Access::LocalNetwork)
    {
        if (localNetworkAllowed()) answer = core::SystemAnswer::Granted; // macOS decides on the first packet; nothing to read
    }
    else
        switch (platform::mediaAccess(mediaKind(access)))
        {
            case platform::MediaAccess::Granted: answer = core::SystemAnswer::Granted; break;
            case platform::MediaAccess::Denied: answer = core::SystemAnswer::Denied; break;
            case platform::MediaAccess::NotDetermined: answer = core::SystemAnswer::NotAsked; break;
        }
    const auto notNowKey = juce::String(text.settingsKey);
    switch (core::accessStep(answer, request, settings().getBoolValue(notNowKey, false)))
    {
        case core::AccessStep::Proceed: done(AccessResult::Granted); return;
        case core::AccessStep::Refuse: done(AccessResult::Denied); return;
        case core::AccessStep::Skip: done(AccessResult::NotNow); return;
        case core::AccessStep::Explain: break;
    }

    auto shared = std::make_shared<std::function<void(AccessResult)>>(std::move(done));
    auto answered = [this, access, notNowKey, shared, alive = std::weak_ptr<bool>(alive_)](int button) {
        if (alive.expired()) return;
        if (button != 1)
        {
            logf("privacy: %s: Not Now", accessName(access));
            settings().setValue(notNowKey, true);
            settings().saveIfNeeded();
            (*shared)(AccessResult::NotNow);
            return;
        }
        logf("privacy: %s: Continue", accessName(access));
        settings().removeValue(notNowKey);
        if (access == core::Access::LocalNetwork)
        {
            settings().setValue(kLocalNetworkContinued, true); // the system prompt follows the first LAN packet
            settings().saveIfNeeded();
            (*shared)(AccessResult::Granted);
            return;
        }
        settings().saveIfNeeded();
        platform::requestMediaAccess(mediaKind(access), [shared, alive](bool granted) {
            if (! alive.expired()) (*shared)(granted ? AccessResult::Granted : AccessResult::Denied);
        });
    };

    PromptOptions options;
    options.state.title = text.title;
    options.state.message = text.message;
    options.state.buttons = {{text.notNowLabel, false, false}, {text.continueLabel, true, false}};
    options.defaultButton = 1;
    options.cancelButton = 0;
    options.shortcuts = {0, 0};
    if (const auto autotest = juce::SystemStats::getEnvironmentVariable("SONORA_PRIVACY_AUTOTEST", {}); autotest.isNotEmpty())
    {
        // Headless checks: 0 = Not Now, 1 = Continue (which shows the system prompt when macOS has not asked yet).
        logf("privacy: autotest answers %d to \"%s\"", autotest.getIntValue(), text.title.c_str());
        juce::MessageManager::callAsync([answered, index = autotest.getIntValue()] { answered(index); });
        return;
    }
    logf("privacy: explaining %s access", accessName(access));
    if (! PromptWindow::show(std::move(options), answered))
    {
        logf("privacy: another prompt is open; %s not asked", accessName(access));
        (*shared)(AccessResult::NotNow); // nothing remembered: the next action asks again
    }
}

} // namespace sonora::app
