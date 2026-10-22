// COM-4: inert updater for builds without Sparkle (non-Apple platforms, -DSONORA_WITH_SPARKLE=OFF).
// COM-7 replaces it with Velopack (Windows) or Flatpak (Linux).
#include "app/Updater.h"

namespace sonora::app
{
namespace
{
class StubUpdater final : public Updater
{
public:
    explicit StubUpdater(Host host) : Updater(std::move(host)) {}
    bool available() const override { return false; }
    std::string unavailableReason() const override { return "This build has no updater"; }
    std::string backendName() const override { return "none"; }
    std::string currentVersion() const override { return {}; }
    bool canCheckNow() const override { return false; }
    void checkNow() override {}
    bool automaticChecks() const override { return false; }
    void setAutomaticChecks(bool) override {}
    UpdateChannel channel() const override { return UpdateChannel::Stable; }
    void setChannel(UpdateChannel) override {}
    std::vector<UpdateChannel> channels() const override { return {}; }
    bool installPending() const override { return false; }
    void installPendingNow() override {}
};
} // namespace

std::unique_ptr<Updater> Updater::create(Host host) { return std::make_unique<StubUpdater>(std::move(host)); }

} // namespace sonora::app
