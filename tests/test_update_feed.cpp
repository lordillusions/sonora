// COM-4l: the Linux updater's feed rules and EdDSA check (app/UpdateFeed.h).
#include "TestFramework.h"
#include "app/UpdateFeed.h"

#include <cstdio>
#include <cstring>

using namespace sonora;

namespace
{
const char* feed = R"(<?xml version="1.0" encoding="utf-8"?>
<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" version="2.0">
  <channel>
    <title>Sonora</title>
    <item>
      <title>Sonora 0.1.9</title>
      <sparkle:version>0.1.9</sparkle:version>
      <enclosure url="https://example.org/v0.1.9/Sonora-0.1.9-x86_64.AppImage" length="21625336" type="application/octet-stream"
                 sparkle:os="linux-x64-appimage" sparkle:edSignature="c2ln"/>
    </item>
    <item>
      <title>Sonora 0.1.9</title>
      <sparkle:version>0.1.9</sparkle:version>
      <enclosure url="https://example.org/v0.1.9/sonora_0.1.9_amd64.deb" length="12688178" type="application/octet-stream"
                 sparkle:os="linux-x64-deb" sparkle:edSignature="ZGVi"/>
    </item>
    <item>
      <title>Sonora 0.1.10 beta</title>
      <sparkle:version>0.1.10</sparkle:version>
      <sparkle:channel>beta</sparkle:channel>
      <enclosure url="https://example.org/v0.1.10/Sonora-0.1.10-x86_64.AppImage" length="1" sparkle:os="linux-x64-appimage"/>
    </item>
    <item>
      <title>Sonora 0.1.8</title>
      <enclosure url="https://example.org/v0.1.8/Sonora-0.1.8-x86_64.AppImage" length="1" sparkle:version="0.1.8"
                 sparkle:os="linux-x64-appimage"/>
    </item>
  </channel>
</rss>)";

// Signed with a throwaway key (docs/UPDATES.md local test): once by CryptoKit (randomized), once by Sparkle's sign_update.
const char* testKey = "+nrLfZddhI99HKcr7Q/XRTkoXra85SZYMTn0o9hFbqY=";
const char* message = "Sonora 0.1.8 update";
const char* cryptoKitSignature = "CxFgT+UXb29QwCMwzLJPTKVrRGot+vqftS7yP11GqZeHs0z0f9w4L/Oz2I9tjwu6PkR0xwFUIhzZNiriMloZAg==";
const char* sparkleSignature = "+h+/Ek3up8ZVVq/yE51mx2kLVwcsnylwi2DRv5xql+ZJDuGq42QaQcmfpjN7+Al1OL3j4a9fNG/czVNf1XV5BQ==";
} // namespace

TEST_CASE("update feed: versions compare numerically, part by part")
{
    CHECK(app::compareVersions("0.1.10", "0.1.9") > 0);
    CHECK(app::compareVersions("0.1.9", "0.1.10") < 0);
    CHECK(app::compareVersions("1.0", "1.0.0") == 0);
    CHECK(app::compareVersions("0.2", "0.1.99") > 0);
    CHECK(app::compareVersions("", "0.0.1") < 0);
}

TEST_CASE("update feed: items carry version, package kind, size and signature")
{
    const auto items = app::parseFeed(feed);
    CHECK(items.size() == 4);
    CHECK(items[0].version == "0.1.9");
    CHECK(items[0].os == "linux-x64-appimage");
    CHECK(items[0].length == 21625336);
    CHECK(items[0].signature == "c2ln");
    CHECK(items[1].url == "https://example.org/v0.1.9/sonora_0.1.9_amd64.deb");
    CHECK(items[2].channel == "beta");
    CHECK(items[3].version == "0.1.8"); // the older layout: sparkle:version on the enclosure
    CHECK(app::parseFeed("<rss><channel><item>").empty());
    CHECK(app::parseFeed("").empty());
}

TEST_CASE("update feed: the newest item for this package and channel, only when newer")
{
    const auto items = app::parseFeed(feed);
    const auto appImage = app::newestUpdate(items, "linux-x64-appimage", "0.1.7", {});
    CHECK(appImage.has_value() && appImage->version == "0.1.9" && appImage->signature == "c2ln");
    const auto deb = app::newestUpdate(items, "linux-x64-deb", "0.1.7", {});
    CHECK(deb.has_value() && deb->url.find(".deb") != std::string::npos);
    const auto beta = app::newestUpdate(items, "linux-x64-appimage", "0.1.7", {"beta"});
    CHECK(beta.has_value() && beta->version == "0.1.10");
    CHECK(! app::newestUpdate(items, "linux-x64-appimage", "0.1.9", {}).has_value()); // up to date
    CHECK(! app::newestUpdate(items, "linux-x64-deb", "0.2.0", {}).has_value());       // never a downgrade
    CHECK(! app::newestUpdate(items, "windows-x64", "0.1.7", {}).has_value());
}

TEST_CASE("update feed: Ed25519 signatures from CryptoKit and Sparkle verify, anything else does not")
{
    if (! app::edSignatureCheckAvailable())
    {
        std::printf("  (built without Monocypher, scripts/bootstrap.sh: signature checks skipped)\n");
        return;
    }
    const auto size = std::strlen(message);
    CHECK(app::verifyEdSignature(testKey, message, size, cryptoKitSignature));
    CHECK(app::verifyEdSignature(testKey, message, size, sparkleSignature));
    CHECK(! app::verifyEdSignature(testKey, message, size - 1, sparkleSignature));
    const std::string tampered = std::string(message) + "!";
    CHECK(! app::verifyEdSignature(testKey, tampered.data(), tampered.size(), sparkleSignature));
    CHECK(! app::verifyEdSignature("k5ZffUC+tRT4esVUT54IBgZzK2QtIyNNYRp+6SjUj3A=", message, size, sparkleSignature)); // other key
    CHECK(! app::verifyEdSignature(testKey, message, size, "c2ln"));
    CHECK(! app::verifyEdSignature("bm90IGEga2V5", message, size, sparkleSignature));
}
