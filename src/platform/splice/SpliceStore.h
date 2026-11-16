#pragma once
// SPL-2/5: token lifecycle + convenience around SpliceApi/SpliceAuth (docs/SPLICE_INTEROP.md). Thread-safe for the
// access token; every call blocks and must run off the audio/message thread. The app (AppSplice) drives it from a worker.

#include "platform/splice/SpliceApi.h"
#include "platform/splice/SpliceAuth.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace sonora::platform { class SecretStore; }

namespace sonora::platform::splice
{

class SpliceStore
{
public:
    SpliceStore(SpliceEndpoints endpoints, Auth0Config auth, SecretStore& secrets);

    bool loggedIn() const;                 // a refresh token is stored
    // A valid access token, refreshing when near expiry. Empty when logged out or the refresh failed (then clears login).
    std::string accessToken(const std::atomic<bool>& cancelled);
    void signOut();

    // SPL-12: run `call(token)` with a valid access token. When Splice rejects the token (SpliceApi::takeUnauthorized),
    // drop it, refresh and retry once. Returns false (and sets needsLogin) when there is no usable sign-in left: no
    // stored refresh token, the refresh was refused, or the fresh token was rejected too.
    template <typename Call>
    bool withToken(const std::atomic<bool>& cancelled, Call&& call)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto token = accessToken(cancelled);
            if (token.empty()) return false; // accessToken() already set needsLogin unless it was a network blip
            api_.takeUnauthorized();
            call(token);
            if (! api_.takeUnauthorized()) return true;
            invalidateAccessToken();
        }
        needsLogin_ = true;
        return false;
    }
    void invalidateAccessToken();
    // True once (then cleared) after a call found that the user must sign in again.
    bool takeNeedsLogin() { return needsLogin_.exchange(false); }

    // Device-authorization login: start, then poll until ok / error (error "pending" means keep going after the interval).
    DeviceCodeStart startLogin(const std::atomic<bool>& cancelled);
    TokenSet pollLogin(const std::string& deviceCode, const std::atomic<bool>& cancelled); // stores the refresh token on ok

    SpliceApi& api() { return api_; }

    // Fetch the scrambled preview and return plain mp3 bytes (empty on failure).
    std::vector<uint8_t> fetchPreview(const std::string& url, const std::atomic<bool>& cancelled) const;
    // Download the licensed WAV (presigned S3 url) to `dest`. Returns true on a complete 2xx download.
    bool downloadWav(const std::string& url, const juce::File& dest, const std::atomic<bool>& cancelled) const;

    const SpliceEndpoints& endpoints() const { return endpoints_; }

private:
    void storeRefresh(const std::string& token);
    std::string loadRefresh() const;

    SpliceEndpoints endpoints_;
    SpliceApi api_;
    SpliceAuth auth_;
    SecretStore& secrets_;

    mutable std::mutex mutex_;
    std::string accessToken_;
    int64_t accessExpiresAtMs_ = 0;
    std::string refreshToken_; // cached copy of the keychain value
    bool loaded_ = false;
    std::atomic<bool> needsLogin_{false};
};

} // namespace sonora::platform::splice
