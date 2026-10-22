// COM-8: resources/supporters.txt parsing for the About window and the website.
#include "TestFramework.h"
#include "core/Supporters.h"

using namespace sonora;

TEST_CASE("supporters: tiers in order, comments and bad lines skipped, names trimmed and capped")
{
    const auto groups = model::parseSupporters(
        "# comment\n"
        "supporter | Lucas R.\n"
        "  Studio|  Example Studio  \n"
        "producer | Ana B.\r\n"
        "no bar here\n"
        "gold | Unknown Tier\n"
        "supporter |   \n"
        "supporter | Mika T.\n"
        "coffee | " + std::string(80, 'x') + "\n");
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].tier == model::SupporterTier::Studio);
    CHECK(groups[0].names == std::vector<std::string>{"Example Studio"});
    CHECK(groups[1].tier == model::SupporterTier::Producer);
    CHECK(groups[1].names == std::vector<std::string>{"Ana B."});
    CHECK(groups[2].names == (std::vector<std::string>{"Lucas R.", "Mika T."}));
    CHECK(groups[3].tier == model::SupporterTier::Coffee);
    CHECK(groups[3].names.front().size() == 60);
    CHECK(model::parseSupporters("# only comments\n\n").empty());
    CHECK(std::string(model::supporterTierTitle(model::SupporterTier::Producer)) == "Producers");
}

TEST_CASE("supporters: a long UTF-8 name is cut on a whole character")
{
    std::string name;
    for (int i = 0; i < 40; ++i) name += "\xC3\xA9"; // é, two bytes each: 80 bytes
    const auto groups = model::parseSupporters("supporter | " + name);
    REQUIRE(groups.size() == 1);
    const auto& cut = groups[0].names.front();
    CHECK(cut.size() <= 60);
    CHECK(cut.size() % 2 == 0); // never half a character
}
