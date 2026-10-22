// Portable HttpClient (Windows, Linux): juce::WebInputStream. WinINet on Windows (no cookies, no
// auto-redirect), libcurl on Linux (JUCE_USE_CURL=1, see cmake/SonoraJuce.cmake).

#include "platform/HttpClient.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sonora::platform
{
namespace
{
struct UploadListener final : juce::WebInputStream::Listener
{
    explicit UploadListener(const std::atomic<bool>& c) : cancelled(c) {}
    bool postDataSendProgress(juce::WebInputStream&, int, int) override { return ! cancelled; }
    const std::atomic<bool>& cancelled;
};

// Cancels the blocking connect/read when the caller cancels or the total deadline passes.
class Watchdog
{
public:
    Watchdog(juce::WebInputStream& stream, const std::atomic<bool>& cancelled, int totalSeconds)
        : thread_([this, &stream, &cancelled, totalSeconds] {
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(totalSeconds);
              std::unique_lock lock(mutex_);
              while (! finished_)
              {
                  if (cancelled || std::chrono::steady_clock::now() >= deadline)
                  {
                      expired_ = ! cancelled;
                      stream.cancel();
                      return;
                  }
                  wake_.wait_for(lock, std::chrono::milliseconds(50));
              }
          })
    {
    }
    ~Watchdog()
    {
        { const std::lock_guard lock(mutex_); finished_ = true; }
        wake_.notify_all();
        thread_.join();
    }
    bool expired() const
    {
        const std::lock_guard lock(mutex_);
        return expired_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool finished_ = false, expired_ = false;
    std::thread thread_;
};
} // namespace

HttpResponse postNoRedirect(const HttpRequest& request, const std::atomic<bool>& cancelled)
{
    HttpResponse result;
    if (cancelled) { result.outcome = HttpResponse::Outcome::Cancelled; return result; }
    const juce::URL url(request.url);
    if (! url.isWellFormed() || url.getScheme().isEmpty() || url.getDomain().isEmpty())
    {
        result.outcome = HttpResponse::Outcome::InvalidUrl;
        return result;
    }

    juce::String headers;
    const auto names = request.headers.getAllKeys();
    const auto values = request.headers.getAllValues();
    for (int i = 0; i < names.size(); ++i)
        headers << names[i] << ": " << values[i] << "\r\n";

    juce::WebInputStream stream(url.withPOSTData(request.body), true);
    stream.withExtraHeaders(headers)
        .withConnectionTimeout(request.requestTimeoutSeconds * 1000)
        .withNumRedirectsToFollow(0);

    juce::MemoryOutputStream body;
    bool failed = false, tooLarge = false;
    {
        Watchdog watchdog(stream, cancelled, request.totalTimeoutSeconds);
        UploadListener listener(cancelled);
        if (! stream.connect(&listener) || stream.isError())
            failed = true;
        result.status = stream.getStatusCode();
        char buffer[8192];
        while (! failed && ! cancelled)
        {
            const int n = stream.read(buffer, sizeof(buffer));
            if (n < 0) { failed = true; break; }
            if (n == 0)
            {
                if (! stream.isExhausted()) failed = true;
                break;
            }
            if (static_cast<int64_t>(body.getDataSize()) + n > request.maxResponseBytes) { tooLarge = true; break; }
            body.write(buffer, static_cast<size_t>(n));
        }
        if (cancelled) { result.outcome = HttpResponse::Outcome::Cancelled; return result; }
        if (watchdog.expired()) { result.outcome = HttpResponse::Outcome::TimedOut; return result; }
    }
    result.body = body.toUTF8();
    result.outcome = tooLarge ? HttpResponse::Outcome::TooLarge
                   : failed   ? HttpResponse::Outcome::NetworkFailed
                              : HttpResponse::Outcome::Completed;
    return result;
}
} // namespace sonora::platform
