#pragma once
// COM-7: one blocking HTTPS POST for non-realtime callers (the native agent's provider calls), and (LIB-1) a resumable GET.
//
//   macOS:  HttpClientMac.mm  — ephemeral NSURLSession (no cache, no cookies).
//   others: HttpClientJuce.cpp — juce::URL / WebInputStream (WinINet on Windows, libcurl on Linux).
//
// Both: redirects are never followed (credentials must not travel to another origin), the body is
// capped at maxResponseBytes, and the call returns within ~50 ms of `cancelled` becoming true.
// Never call from the audio or message thread.

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

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

// LIB-1: a resumable GET into a file, for sample packs (docs/SAMPLE_PACKS.md). No credentials are sent, so redirects are
// followed (GitHub release assets redirect to their storage host), up to maxRedirects and only to https or to a loopback
// http address (local tests). When `destination` already holds N bytes the request asks for "Range: bytes=N-":
//   206  the new bytes are appended (resumed = true)
//   200  the server ignored the range: the file is rewritten from the start
//   416  nothing is written (the file is probably complete: the caller checks its hash)
//   other statuses leave the file as it was; outcome Completed with that status.
// The file never grows beyond maxBytes (TooLarge). `progress(fileBytes, expectedTotal)` runs on the calling thread about
// every 100 ms (expectedTotal 0 = unknown). Returns within ~50 ms of `cancelled` becoming true, keeping what was written.
struct DownloadRequest
{
    juce::String url;
    juce::File destination;
    int64_t maxBytes = int64_t(4) << 30;
    int idleTimeoutSeconds = 30; // no byte for this long = TimedOut
    int maxRedirects = 5;
};

struct DownloadResult
{
    HttpResponse::Outcome outcome = HttpResponse::Outcome::NetworkFailed;
    int status = 0;
    bool resumed = false;
    int64_t fileBytes = 0; // the destination's size afterwards
};

DownloadResult downloadToFile(const DownloadRequest& request, const std::function<void(int64_t, int64_t)>& progress,
                              const std::atomic<bool>& cancelled);

// True for https URLs and for http to localhost / 127.0.0.1 / [::1] (the redirect rule above).
bool downloadUrlAllowed(const juce::String& url);
// "bytes 100-199/1000" -> first byte 100, total 1000 ("*" or a missing total = 0). -1 when malformed.
int64_t contentRangeStart(const juce::String& header, int64_t& total);

} // namespace sonora::platform
