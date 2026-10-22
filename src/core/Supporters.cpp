#include "core/Supporters.h"

#include <algorithm>
#include <cctype>

namespace sonora::model
{
namespace
{
std::string trim(std::string_view s)
{
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return std::string(s.substr(first, last - first + 1));
}

std::string lower(std::string s)
{
    for (auto& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}
} // namespace

const char* supporterTierTitle(SupporterTier tier)
{
    switch (tier)
    {
        case SupporterTier::Studio: return "Studio";
        case SupporterTier::Producer: return "Producers";
        case SupporterTier::Supporter: return "Supporters";
        case SupporterTier::Coffee: return "Coffees";
    }
    return "";
}

std::vector<SupporterGroup> parseSupporters(std::string_view text)
{
    std::vector<SupporterGroup> groups;
    for (auto tier : supporterTiers) groups.push_back({tier, {}});
    size_t start = 0;
    while (start <= text.size())
    {
        const auto end = std::min(text.find('\n', start), text.size());
        const auto line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line.front() == '#') continue;
        const auto bar = line.find('|');
        if (bar == std::string::npos) continue;
        const auto tier = lower(trim(std::string_view(line).substr(0, bar)));
        auto name = trim(std::string_view(line).substr(bar + 1));
        if (name.empty()) continue;
        if (name.size() > 60)
        {
            name.resize(60);
            while (! name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back(); // whole UTF-8
            if (! name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();
        }
        for (size_t i = 0; i < supporterTiers.size(); ++i)
        {
            const std::string key = lower(supporterTierTitle(supporterTiers[i]));
            const bool match = (i == 0 && tier == "studio") || (i == 1 && tier == "producer") || (i == 2 && tier == "supporter")
                               || (i == 3 && tier == "coffee") || tier == key;
            if (match) { groups[i].names.push_back(name); break; }
        }
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const auto& g) { return g.names.empty(); }), groups.end());
    return groups;
}
} // namespace sonora::model
