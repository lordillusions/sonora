#include "TestFramework.h"

#include "core/AppDataFolder.h"
#include "plugins/PluginHost.h"

using namespace sonora;

namespace
{
// Tests share one process: always put the stable folder back.
struct RestoreStableFolder
{
    ~RestoreStableFolder() { core::setAppDataFolderName(core::kStableDataFolder); }
};

// Parent of the data folder: Application Support on macOS, the OS application-data directory elsewhere (COM-7).
juce::File applicationSupport()
{
    const auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if defined(__APPLE__)
    return base.getChildFile("Application Support");
#else
    return base;
#endif
}
} // namespace

TEST_CASE("app data folder: stable default is Application Support/Sonora")
{
    CHECK(core::appDataFolderName() == "Sonora");
    CHECK(core::appDataDirectory() == applicationSupport().getChildFile("Sonora"));
    CHECK(plugins::PluginHost::cacheFile() == applicationSupport().getChildFile("Sonora/PluginCache.xml"));
}

TEST_CASE("app data folder: nightly moves every per-user file")
{
    RestoreStableFolder restore;
    CHECK(core::setAppDataFolderName("Sonora Nightly"));
    CHECK(core::appDataFolderName() == "Sonora Nightly");
    const auto nightly = applicationSupport().getChildFile("Sonora Nightly");
    CHECK(core::appDataDirectory() == nightly);
    CHECK(plugins::PluginHost::cacheFile() == nightly.getChildFile("PluginCache.xml"));
    CHECK(! plugins::PluginHost::cacheFile().isAChildOf(applicationSupport().getChildFile("Sonora")));
}

TEST_CASE("app data folder: invalid names are refused and keep the current folder")
{
    RestoreStableFolder restore;
    for (const char* bad : {"", ".", "..", ".hidden", "../Sonora", "a/b", "a:b", " Sonora", "Sonora ", "tab\tname"})
    {
        CHECK(! core::isValidDataFolderName(bad));
        CHECK(! core::setAppDataFolderName(bad));
        CHECK(core::appDataFolderName() == "Sonora");
    }
    CHECK(! core::isValidDataFolderName(std::string(65, 'x')));
    CHECK(core::isValidDataFolderName(std::string(64, 'x')));
    CHECK(core::isValidDataFolderName("Sonora Nightly"));
}
