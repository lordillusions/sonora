#include "ui/BrowserTags.h"

#include <juce_core/juce_core.h>
#include <algorithm>

namespace sonora::ui
{
namespace
{
BrowserTag clean(BrowserTag tag)
{
    tag.group = juce::String(tag.group).trim().toStdString();
    tag.value = juce::String(tag.value).trim().toStdString();
    return tag;
}

bool same(const BrowserTag& a, const BrowserTag& b)
{
    return juce::String(a.group).equalsIgnoreCase(juce::String(b.group)) && juce::String(a.value).equalsIgnoreCase(juce::String(b.value));
}

bool contains(const BrowserTags::Tags& tags, const BrowserTag& tag)
{
    return std::any_of(tags.begin(), tags.end(), [&](const auto& value) { return same(value, tag); });
}
}

BrowserTags::Tags BrowserTags::tagsFor(const std::string& key, Tags defaults) const
{
    const auto found = changes_.find(key);
    Tags result;
    for (auto tag : defaults)
    {
        tag = clean(std::move(tag));
        if (! tag.value.empty() && ! contains(result, tag)
            && (found == changes_.end() || ! contains(found->second.removed, tag))) result.push_back(std::move(tag));
    }
    if (found != changes_.end())
        for (const auto& tag : found->second.added)
            if (! contains(result, tag)) result.push_back(tag);
    return result;
}

bool BrowserTags::set(const std::string& key, BrowserTag tag, bool enabled)
{
    tag = clean(std::move(tag));
    if (key.empty() || tag.group.empty() || juce::String(tag.group).length() > 80 || tag.value.empty() || juce::String(tag.value).length() > 80) return false;
    auto& changes = changes_[key];
    auto& insert = enabled ? changes.added : changes.removed;
    auto& erase = enabled ? changes.removed : changes.added;
    std::erase_if(erase, [&](const auto& value) { return same(value, tag); });
    if (contains(insert, tag)) return false;
    insert.push_back(std::move(tag));
    return true;
}

BrowserTags::Tags BrowserTags::knownTags() const
{
    Tags tags;
    for (const auto& [key, changes] : changes_)
    {
        (void) key;
        for (const auto& tag : changes.added) if (! contains(tags, tag)) tags.push_back(tag);
        for (const auto& tag : changes.removed) if (! contains(tags, tag)) tags.push_back(tag);
    }
    return tags;
}

bool BrowserTags::matches(const Tags& tags, const Tags& filters)
{
    // OR within a facet (e.g. VST3 or AU), AND between facets (e.g. Format and Creator).
    for (const auto& filter : filters)
    {
        bool matched = false;
        for (const auto& alternative : filters)
            if (juce::String(filter.group).equalsIgnoreCase(juce::String(alternative.group)) && contains(tags, alternative)) matched = true;
        if (! matched) return false;
    }
    return true;
}

std::string BrowserTags::toJson() const
{
    const auto encode = [](const Tags& tags) {
        juce::Array<juce::var> array;
        for (const auto& tag : tags) array.add(juce::var(juce::Array<juce::var>{tag.group, tag.value}));
        return juce::var(array);
    };
    juce::Array<juce::var> entries;
    for (const auto& [key, changes] : changes_)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty("key", juce::String(key));
        entry->setProperty("added", encode(changes.added));
        entry->setProperty("removed", encode(changes.removed));
        entries.add(juce::var(entry));
    }
    return juce::JSON::toString(juce::var(entries), true).toStdString();
}

void BrowserTags::loadJson(const std::string& json)
{
    changes_.clear();
    const auto data = juce::JSON::parse(juce::String(json));
    if (const auto* entries = data.getArray())
        for (const auto& entry : *entries)
        {
            if (! entry["key"].isString()) continue;
            const auto key = entry["key"].toString().toStdString();
            for (const bool enabled : {false, true})
                if (const auto* tags = entry[enabled ? "added" : "removed"].getArray())
                    for (const auto& tag : *tags)
                        if (tag.isArray() && tag.size() == 2 && tag[0].isString() && tag[1].isString())
                            set(key, {tag[0].toString().toStdString(), tag[1].toString().toStdString()}, enabled);
        }
}

} // namespace sonora::ui
