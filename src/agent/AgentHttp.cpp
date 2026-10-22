#include "agent/AgentRunner.h"
#include "platform/HttpClient.h"

namespace sonora::agent
{
HttpResult post(const Config& config, const juce::String& key, const juce::var& payload, const std::atomic<bool>& cancelled)
{
    if (! validate(config).ok || key.isEmpty() || key.containsAnyOf("\r\n")) return {{}, "Invalid endpoint or credential"};
    platform::HttpRequest request;
    request.url = requestUrl(config);
    request.headers.set("Content-Type", "application/json");
    if (config.protocol == Protocol::Anthropic)
    {
        request.headers.set("x-api-key", key);
        request.headers.set("anthropic-version", "2023-06-01");
    }
    else request.headers.set("Authorization", "Bearer " + key);
    request.body = juce::JSON::toString(payload, true);
    request.requestTimeoutSeconds = 60; request.totalTimeoutSeconds = 65;
    request.maxResponseBytes = 4 * 1024 * 1024;
    const auto response = platform::postNoRedirect(request, cancelled);
    using Outcome = platform::HttpResponse::Outcome;
    switch (response.outcome)
    {
        case Outcome::Cancelled: return {{}, "Cancelled"};
        case Outcome::InvalidUrl: return {{}, "Invalid provider URL"};
        case Outcome::TimedOut: return {{}, "Provider request timed out"};
        case Outcome::TooLarge: return {{}, "Provider response exceeded 4 MiB"};
        case Outcome::NetworkFailed: case Outcome::Completed: break;
    }
    const bool failed = response.outcome == Outcome::NetworkFailed;
    const auto status = response.status;
    // Do not copy provider error bodies/URLs/headers into logs: providers may echo sensitive request data.
    if (failed && status == 0) return {{}, "Provider network request failed"};
    if (status < 200 || status >= 300) return {{}, "Provider returned HTTP " + juce::String(status) + ". Check key, model, endpoint and account limits."};
    if (failed) return {{}, "Provider network request failed"};
    try { return {parseJson(response.body), {}}; }
    catch (const std::exception&) { return {{}, "Provider returned malformed or oversized JSON"}; }
}
}
