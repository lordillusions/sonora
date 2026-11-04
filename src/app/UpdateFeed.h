#pragma once
// COM-4l: the Sparkle-format feed read by the Linux updater (UpdaterLinux.cpp), and the EdDSA check of what it downloads.
// macOS (Sparkle) and Windows (WinSparkle) parse and verify feeds themselves. scripts/release/write_platform_feeds.py writes
// appcast-linux.xml: one <item> per package kind, told apart by the enclosure's sparkle:os ("linux-x64-appimage",
// "linux-x64-deb"). Unit-tested in tests/test_update_feed.cpp. See docs/UPDATES.md "Windows and Linux".

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::app
{

struct FeedItem
{
    std::string version;   // sparkle:version, compared with compareVersions
    std::string title;
    std::string os;        // sparkle:os of the enclosure ("" = any)
    std::string url;
    std::string signature; // sparkle:edSignature, base64 Ed25519 over the whole file
    std::string channel;   // sparkle:channel ("" = stable)
    int64_t length = 0;
};

// Every item with an enclosure, in feed order. Malformed XML gives an empty list.
std::vector<FeedItem> parseFeed(const std::string& xml);

// Dotted numeric versions: "0.1.10" > "0.1.9", "1.0" == "1.0.0". A non-numeric part compares as 0.
int compareVersions(std::string_view a, std::string_view b);

// The newest item for this package kind that is newer than the running version and on an allowed channel.
std::optional<FeedItem> newestUpdate(const std::vector<FeedItem>& items, std::string_view os, std::string_view currentVersion,
                                     const std::vector<std::string>& allowedChannels);

// Ed25519 (Sparkle's EdDSA) over data, with a base64 public key and signature. False for malformed input.
bool verifyEdSignature(std::string_view publicKeyBase64, const void* data, size_t size, std::string_view signatureBase64);
bool edSignatureCheckAvailable(); // false when the build has no Ed25519 implementation (Monocypher, scripts/bootstrap.sh)

} // namespace sonora::app
