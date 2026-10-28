#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sonora::ui
{

struct BrowserTag
{
    std::string group, value;
    auto operator<=>(const BrowserTag&) const = default;
};

// Local library metadata, independent of the song and its undo history (PLG-10).
class BrowserTags
{
public:
    using Tags = std::vector<BrowserTag>;
    Tags tagsFor(const std::string& key, Tags defaults) const;
    Tags knownTags() const;
    bool hasChanges(const std::string& key) const { return changes_.contains(key); }
    bool set(const std::string& key, BrowserTag tag, bool enabled);
    static bool matches(const Tags& tags, const Tags& filters);
    std::string toJson() const;
    void loadJson(const std::string& json);
    uint64_t revision() const { return revision_; }

private:
    struct Changes { Tags added, removed; };
    std::map<std::string, Changes> changes_;
    uint64_t revision_ = 0;
};

} // namespace sonora::ui
