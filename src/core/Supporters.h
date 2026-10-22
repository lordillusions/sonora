#pragma once
// Supporters credited in the About window and on the website (COM-8): resources/supporters.txt, one "tier | name" per
// line. Only people who asked to be listed are in the file. No audio, no UI.

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::model
{
enum class SupporterTier { Studio, Producer, Supporter, Coffee };
inline constexpr std::array<SupporterTier, 4> supporterTiers{SupporterTier::Studio, SupporterTier::Producer,
                                                              SupporterTier::Supporter, SupporterTier::Coffee};
const char* supporterTierTitle(SupporterTier); // "Studio", "Producers", "Supporters", "Coffees"

struct SupporterGroup
{
    SupporterTier tier;
    std::vector<std::string> names; // file order
};
// Groups in tier order (Studio first), empty tiers left out. Unknown tiers, blank names and comments are skipped;
// names are trimmed and limited to 60 characters.
std::vector<SupporterGroup> parseSupporters(std::string_view text);
} // namespace sonora::model
