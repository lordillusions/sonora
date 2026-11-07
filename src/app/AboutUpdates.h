#pragma once
// COM-4 + COM-8: what About's update row shows for the app's updater (label, status, enabled). Header-only so the
// brand tests can check it with a fake updater (tests/test_brand_ui.cpp).

#include "app/Updater.h"
#include "gfx/Brand.h"

#include <cctype>
#include <string>

namespace sonora::app
{

inline gfx::brand::AboutUpdateRow aboutUpdateRow(const Updater* updater)
{
    gfx::brand::AboutUpdateRow row;
    if (updater == nullptr || ! updater->available())
    {
        row.status = updater != nullptr ? updater->unavailableReason() : "This build has no updater";
        return row;
    }
    if (updater->installPending())
    {
        row.label = "Install and Restart...";
        row.status = "An update is downloaded. Sonora restarts when nothing is playing, recording or rendering.";
        row.enabled = true;
        return row;
    }
    std::string channel = channelName(updater->channel());
    if (! channel.empty()) channel[0] = (char) std::toupper((unsigned char) channel[0]);
    row.enabled = updater->canCheckNow(); // false while a check is already running
    row.status = (updater->automaticChecks() ? "Checks automatically every day" : "Automatic checks are off (Settings > Updates)")
                 + std::string(" · ") + channel + " channel";
    return row;
}

} // namespace sonora::app
