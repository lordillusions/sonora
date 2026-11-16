#pragma once
// SPL-2: Sonora's own Auth0 login to Splice (docs/SPLICE_INTEROP.md). Device-authorization flow (RFC 8628): Sonora opens
// the verification URL in a window/browser and polls for the token; the rotated refresh token lives in Sonora's keychain,
// never the Splice app's. Worker-thread only. No Splice code is used; this is a standard OAuth client of a public client id.

#include <atomic>
#include <functional>
#include <string>

#include <juce_core/juce_core.h>

namespace sonora::platform { class SecretStore; }

namespace sonora::platform::splice
{

struct Auth0Config
{
    juce::String domain = "auth.splice.com";
    juce::String clientId = "L2JVBs6YHYe7OeK3eeyOXoPNcNTkPWgv";
    juce::String audience = "https://splice.com";
    juce::String scope = "openid profile offline_access";
    juce::String userAgent = "Sonora";
};

struct DeviceCodeStart
{
    std::string deviceCode;             // kept to poll; not shown
    std::string userCode;               // shown to the user
    std::string verificationUri;        // base
    std::string verificationUriComplete; // opened in the login window
    int intervalSeconds = 5;
    int expiresInSeconds = 900;
    bool ok = false;
    std::string error;
};

// A minted session: a short-lived access token plus the refresh token to store.
struct TokenSet
{
    std::string accessToken;
    std::string refreshToken;
    int64_t expiresAtMs = 0;
    bool ok = false;
    std::string error; // "pending" while the user has not approved yet (device-code polling)
};

class SpliceAuth
{
public:
    explicit SpliceAuth(Auth0Config config = {}) : config_(std::move(config)) {}

    DeviceCodeStart startDeviceCode(const std::atomic<bool>& cancelled) const;
    // One poll of the device-code token endpoint. error == "pending" means keep polling after the interval.
    TokenSet pollDeviceToken(const std::string& deviceCode, const std::atomic<bool>& cancelled) const;
    // Exchange a stored refresh token for a fresh access token (rotates the refresh token).
    TokenSet refresh(const std::string& refreshToken, const std::atomic<bool>& cancelled) const;

    // Parsing split out for tests.
    static DeviceCodeStart parseDeviceCode(const juce::String& json);
    static TokenSet parseToken(const juce::String& json, int status);

private:
    Auth0Config config_;
};

} // namespace sonora::platform::splice
