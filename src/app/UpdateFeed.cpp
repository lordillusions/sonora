// COM-4l: feed parsing and the EdDSA check for the Linux updater (UpdateFeed.h).
#include "app/UpdateFeed.h"

#include <juce_core/juce_core.h>

#include <algorithm>

#if SONORA_HAVE_MONOCYPHER
#include "monocypher-ed25519.h"
#endif

namespace sonora::app
{
namespace
{
juce::String childText(const juce::XmlElement& item, const char* tag)
{
    if (const auto* child = item.getChildByName(tag)) return child->getAllSubText().trim();
    return {};
}

std::vector<uint8_t> fromBase64(std::string_view text)
{
    juce::MemoryOutputStream out;
    if (! juce::Base64::convertFromBase64(out, juce::String(std::string(text)).trim())) return {};
    const auto* bytes = static_cast<const uint8_t*>(out.getData());
    return std::vector<uint8_t>(bytes, bytes + out.getDataSize());
}
} // namespace

std::vector<FeedItem> parseFeed(const std::string& xml)
{
    std::vector<FeedItem> items;
    const auto root = juce::parseXML(juce::String::fromUTF8(xml.c_str()));
    if (root == nullptr) return items;
    const auto* channel = root->getChildByName("channel");
    if (channel == nullptr) return items;
    for (const auto* item : channel->getChildWithTagNameIterator("item"))
    {
        const auto version = childText(*item, "sparkle:version"), title = childText(*item, "title"),
                   tag = childText(*item, "sparkle:channel");
        for (const auto* enclosure : item->getChildWithTagNameIterator("enclosure"))
        {
            FeedItem entry;
            entry.version = (version.isNotEmpty() ? version : enclosure->getStringAttribute("sparkle:version")).toStdString();
            entry.title = title.toStdString();
            entry.channel = tag.toStdString();
            entry.os = enclosure->getStringAttribute("sparkle:os").toStdString();
            entry.url = enclosure->getStringAttribute("url").toStdString();
            entry.signature = enclosure->getStringAttribute("sparkle:edSignature").toStdString();
            entry.length = enclosure->getStringAttribute("length").getLargeIntValue();
            if (! entry.url.empty() && ! entry.version.empty()) items.push_back(std::move(entry));
        }
    }
    return items;
}

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& text) {
        long long value = 0;
        size_t i = 0;
        while (i < text.size() && text[i] != '.')
        {
            if (text[i] >= '0' && text[i] <= '9') value = value * 10 + (text[i] - '0');
            ++i;
        }
        text.remove_prefix(std::min(text.size(), i + 1));
        return value;
    };
    while (! a.empty() || ! b.empty())
    {
        const auto x = next(a), y = next(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::optional<FeedItem> newestUpdate(const std::vector<FeedItem>& items, std::string_view os, std::string_view currentVersion,
                                     const std::vector<std::string>& allowedChannels)
{
    std::optional<FeedItem> best;
    for (const auto& item : items)
    {
        if (! item.os.empty() && item.os != os) continue;
        if (! item.channel.empty()
            && std::find(allowedChannels.begin(), allowedChannels.end(), item.channel) == allowedChannels.end())
            continue;
        if (compareVersions(item.version, currentVersion) <= 0) continue;
        if (! best || compareVersions(item.version, best->version) > 0) best = item;
    }
    return best;
}

bool edSignatureCheckAvailable()
{
    return SONORA_HAVE_MONOCYPHER != 0;
}

bool verifyEdSignature(std::string_view publicKeyBase64, const void* data, size_t size, std::string_view signatureBase64)
{
    const auto key = fromBase64(publicKeyBase64), signature = fromBase64(signatureBase64);
    if (key.size() != 32 || signature.size() != 64 || (data == nullptr && size > 0)) return false;
#if SONORA_HAVE_MONOCYPHER
    return crypto_ed25519_check(signature.data(), key.data(), static_cast<const uint8_t*>(data), size) == 0;
#else
    return false;
#endif
}

} // namespace sonora::app
