// COM-4: update channel and safe-restart rules (app/UpdatePolicy.h), shared by every updater backend.
#include "TestFramework.h"
#include "app/UpdatePolicy.h"

using namespace sonora;
using app::InstallBlocker;
using app::UpdateChannel;

TEST_CASE("updates: channel names round-trip and unknown names fall back")
{
    for (auto channel : {UpdateChannel::Stable, UpdateChannel::Beta, UpdateChannel::Nightly})
        CHECK(app::channelFromName(app::channelName(channel)) == channel);
    CHECK(app::channelFromName("") == UpdateChannel::Stable);
    CHECK(app::channelFromName("Beta") == UpdateChannel::Stable); // stored names are lower case
    CHECK(app::channelFromName("weird", UpdateChannel::Beta) == UpdateChannel::Beta);
}

TEST_CASE("updates: stable builds offer stable and beta, nightly builds only their own feed")
{
    CHECK((app::selectableChannels(false) == std::vector<UpdateChannel>{UpdateChannel::Stable, UpdateChannel::Beta}));
    CHECK(app::selectableChannels(true) == std::vector<UpdateChannel>{UpdateChannel::Nightly});
    CHECK(app::allowedFeedChannels(UpdateChannel::Stable).empty()); // untagged items only
    CHECK(app::allowedFeedChannels(UpdateChannel::Beta) == std::vector<std::string>{"beta"});
    CHECK(app::allowedFeedChannels(UpdateChannel::Nightly).empty()); // the nightly feed is separate and untagged
}

TEST_CASE("updates: restart waits for recording, renders and playback, then offers a save")
{
    app::UpdateSessionState state;
    CHECK(app::installBlocker(state) == InstallBlocker::None);
    CHECK(! app::waitsForSession(InstallBlocker::None));

    state.dirty = true;
    CHECK(app::installBlocker(state) == InstallBlocker::UnsavedChanges);
    CHECK(! app::waitsForSession(InstallBlocker::UnsavedChanges)); // resolved by the save prompt, not by waiting

    state.playing = true;
    CHECK(app::installBlocker(state) == InstallBlocker::Playing);
    state.exporting = true;
    CHECK(app::installBlocker(state) == InstallBlocker::Exporting);
    state.recording = true;
    CHECK(app::installBlocker(state) == InstallBlocker::Recording);
    for (auto blocker : {InstallBlocker::Recording, InstallBlocker::Exporting, InstallBlocker::Playing})
    {
        CHECK(app::waitsForSession(blocker));
        CHECK(! app::blockerMessage(blocker).empty());
    }
    CHECK(app::blockerMessage(InstallBlocker::None).empty());
}
