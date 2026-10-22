#pragma once
// COM-7: one blocking HTTPS POST for non-realtime callers (the native agent's provider calls).
//
//   macOS:  HttpClientMac.mm  — ephemeral NSURLSession (no cache, no cookies).
//   others: HttpClientJuce.cpp — juce::URL / WebInputStream (WinINet on Windows, libcurl on Linux).
//
// Both: redirects are never followed (credentials must not travel to another origin), the body is
// capped at maxResponseBytes, and the call returns within ~50 ms of `cancelled` becoming true.
// Never call from the audio or message thread.

#include <juce_core/juce_core.h>

#include <atomic>

namespace sonora::platform
{

struct HttpRequest
{
    juce::String url;
    juce::StringPairArray headers; // name -> value
    juce::String body;             // sent as UTF-8
    int requestTimeoutSeconds = 60; // idle timeout per read
    int totalTimeoutSeconds = 65;   // whole request
    int64_t maxResponseBytes = 4 * 1024 * 1024;
};

struct HttpResponse
{
    enum class Outcome
    {
        Completed,    // a response arrived (status may still be an HTTP error)
        InvalidUrl,
        NetworkFailed,
        TooLarge,
        TimedOut,
        Cancelled
    };
    Outcome outcome = Outcome::NetworkFailed;
    int status = 0; // HTTP status, 0 when none was received
    juce::String body;
};

HttpResponse postNoRedirect(const HttpRequest& request, const std::atomic<bool>& cancelled);

} // namespace sonora::platform
