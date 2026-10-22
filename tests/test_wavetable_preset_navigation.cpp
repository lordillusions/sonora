// UI-28i: Sonora Prism preset navigation inside a library bank ("<" / ">"), group jumps ("<<" / ">>") and the
// browser's bank / group / search filter. Synthetic items; no installed library needed.
#include "TestFramework.h"
#include "plugins/SerumLibrary.h"

using namespace sonora;
namespace
{
plugins::SerumLibraryItem item(std::string folder, std::string name)
{
    return {"/lib/" + (folder.empty() ? "" : folder + "/") + name + ".SerumPreset", std::move(name), std::move(folder)};
}
// The order serumLibrary() produces: folder by folder, then name.
std::vector<plugins::SerumLibraryItem> library()
{
    return {
        item("", "Loose"),
        item("Factory/Bass", "Bass A"), item("Factory/Bass", "Bass B"), item("Factory/Bass/Reese", "Reese 1"),
        item("Factory/Bass Extra", "Extra 1"),
        item("Factory/Pad", "Pad A"), item("Factory/Pad", "Pad B"),
        item("Solo", "Only 1"), item("Solo", "Only 2"),
        item("User", "Mine"), item("User/Leads", "Lead 1"),
    };
}
std::string nameAt(const std::vector<plugins::SerumLibraryItem>& items, int index)
{
    return index >= 0 && (size_t) index < items.size() ? items[(size_t) index].name : "<none>";
}
} // namespace

TEST_CASE("preset navigation: banks and groups come from the first two folders")
{
    const auto items = library();
    CHECK(plugins::serumLibraryBank(items[0]).empty());
    CHECK(plugins::serumLibraryBank(items[3]) == "Factory");
    CHECK(plugins::serumLibraryGroup(items[3]) == "Bass"); // deeper folders stay in their group
    CHECK(plugins::serumLibraryGroup(items[9]).empty());   // directly in the bank
    const auto banks = plugins::serumLibraryBanks(items);
    REQUIRE(banks.size() == 4);
    CHECK(banks[0].empty()); CHECK(banks[1] == "Factory"); CHECK(banks[2] == "Solo"); CHECK(banks[3] == "User");
    const auto groups = plugins::serumLibraryGroups(items, "Factory");
    REQUIRE(groups.size() == 3);
    CHECK(groups[0] == "Bass"); CHECK(groups[1] == "Bass Extra"); CHECK(groups[2] == "Pad");
    CHECK(plugins::serumLibraryGroups(items, "Solo").size() == 1);
    CHECK(plugins::serumLibraryGroups(items, "Nope").empty());
    // Path wins over a duplicate display name.
    CHECK(plugins::serumLibraryFind(items, items[5].path) == 5);
    CHECK(plugins::serumLibraryFind(items, "Pad B") == 6);
    CHECK(plugins::serumLibraryFind(items, "missing") == -1);
}

TEST_CASE("preset navigation: < and > stay inside the bank and wrap at its ends")
{
    const auto items = library();
    const auto step = [&](const std::string& from, int direction) { return nameAt(items, plugins::serumLibraryStepInBank(items, from, direction)); };
    CHECK(step("Bass A", 1) == "Bass B");
    CHECK(step("Reese 1", 1) == "Extra 1"); // across groups, inside the bank
    CHECK(step("Pad B", 1) == "Bass A");    // last of Factory wraps to its first, never into "Solo"
    CHECK(step("Bass A", -1) == "Pad B");
    CHECK(step("Only 2", 1) == "Only 1");
    CHECK(step("Lead 1", 1) == "Mine");
    CHECK(step("Loose", 1) == "Loose"); // a one-item bank stays put
    CHECK(plugins::serumLibraryStepInBank(items, "missing", 1) == -1);
    CHECK(plugins::serumLibraryStepInBank({}, "Bass A", 1) == -1);
}

TEST_CASE("preset navigation: << and >> jump to the first preset of the neighbouring group")
{
    const auto items = library();
    const auto jump = [&](const std::string& from, int direction) { return nameAt(items, plugins::serumLibraryStepGroup(items, from, direction)); };
    CHECK(jump("Bass B", 1) == "Extra 1");
    CHECK(jump("Reese 1", 1) == "Extra 1"); // a subfolder belongs to its group
    CHECK(jump("Extra 1", 1) == "Pad A");
    CHECK(jump("Pad B", 1) == "Bass A");    // wraps inside the bank
    CHECK(jump("Pad B", -1) == "Extra 1");
    CHECK(jump("Bass B", -1) == "Pad A");   // wraps backwards to the last group's first preset
    CHECK(jump("Mine", 1) == "Lead 1");     // the unnamed group of loose bank files counts
    CHECK(jump("Lead 1", 1) == "Mine");
    // One group or none: nothing to jump to, the buttons disable.
    CHECK(plugins::serumLibraryStepGroup(items, "Only 1", 1) == -1);
    CHECK(plugins::serumLibraryStepGroup(items, "Loose", -1) == -1);
    CHECK(plugins::serumLibraryStepGroup(items, "missing", 1) == -1);
}

TEST_CASE("preset navigation: the browser filter limits to bank and group and matches every search word")
{
    const auto items = library();
    const auto names = [&](const std::vector<int>& indices) {
        std::string joined;
        for (const int index : indices) joined += (joined.empty() ? "" : ",") + nameAt(items, index);
        return joined;
    };
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "")) == "Bass A,Bass B,Reese 1,Extra 1,Pad A,Pad B");
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::string("Pad"), "")) == "Pad A,Pad B");
    CHECK(names(plugins::serumLibraryFilter(items, "User", std::string(), "")) == "Mine");
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "bass")) == "Bass A,Bass B,Reese 1,Extra 1"); // folder matches
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "REESE")) == "Reese 1");                     // case-insensitive
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "  b  bass ")) == "Bass A,Bass B,Reese 1,Extra 1");
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "bass b")) == "Bass A,Bass B,Reese 1,Extra 1");
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "pad b")) == "Pad B");
    CHECK(names(plugins::serumLibraryFilter(items, "Factory", std::nullopt, "factory")).empty()); // the bank name itself is not searched
    CHECK(names(plugins::serumLibraryFilter(items, "Solo", std::nullopt, "lead")).empty());
}
