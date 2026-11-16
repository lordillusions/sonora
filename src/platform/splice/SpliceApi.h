#pragma once
// SPL-1: Splice data-plane calls over HTTPS with a Bearer access token (docs/SPLICE_INTEROP.md). Worker-thread only.
// No Splice code is used; these are ordinary authenticated requests to Splice's public GraphQL/REST with the user's token.

#include "platform/splice/SpliceModel.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace sonora::platform::splice
{

// Hosts (overridable in tests). Production values from docs/SPLICE_INTEROP.md.
struct SpliceEndpoints
{
    juce::String graphql = "https://surfaces-graphql.splice.com/graphql";
    juce::String api = "https://api.splice.com";
    juce::String userAgent = "Sonora";
};

class SpliceApi
{
public:
    explicit SpliceApi(SpliceEndpoints endpoints = {}) : endpoints_(std::move(endpoints)) {}

    // Each call takes the access token and a cancel flag; all block and must run off the audio/message thread.
    SpliceSearchResult searchSamples(const std::string& token, const SpliceSearchQuery& query,
                                     const std::atomic<bool>& cancelled) const;
    SpliceCost costEstimate(const std::string& token, const std::vector<std::string>& uuids,
                            const std::atomic<bool>& cancelled) const;
    int creditBalance(const std::string& token, const std::atomic<bool>& cancelled) const; // -1 on failure
    // Returns the licensed asset's file hash (the download id), or empty on failure. Idempotent for an owned asset.
    std::string purchaseAsset(const std::string& token, const std::string& uuid, const std::atomic<bool>& cancelled) const;
    // The presigned WAV URL and its pack-relative path for an owned sample (GET /v2/premium/samples/<hash>).
    struct DownloadInfo { std::string url, path; int64_t size = 0; bool ok = false; };
    DownloadInfo premiumSample(const std::string& token, const std::string& fileHash,
                               const std::atomic<bool>& cancelled) const;

    // Parsing split out for unit tests (no network): the raw GraphQL/REST JSON bodies.
    static SpliceSearchResult parseSearch(const juce::String& json);
    static SpliceCost parseCost(const juce::String& json);
    static std::string parsePurchase(const juce::String& json, const std::string& uuid);
    static DownloadInfo parsePremium(const juce::String& json);

    // SPL-12: true once (then cleared) after Splice rejected the access token (HTTP 401 or a GraphQL auth error), so the
    // caller can refresh and retry, and ask the user to sign in again when the refresh is refused too.
    bool takeUnauthorized() const { return unauthorized_.exchange(false); }
    static bool isAuthError(int status, const std::string& graphqlError);

private:
    juce::var graphql(const std::string& token, const char* operationName, const char* query, const juce::var& variables,
                      const std::atomic<bool>& cancelled, std::string& error) const;
    SpliceEndpoints endpoints_;
    mutable std::atomic<bool> unauthorized_{false};
};

} // namespace sonora::platform::splice
