// macOS HttpClient: ephemeral NSURLSession. Compiled with ARC (-fobjc-arc, see src/CMakeLists.txt).
// Moved unchanged from agent/AgentHttp.mm (COM-7); the provider-specific framing stays in the agent.

#include "platform/HttpClient.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace sonora::platform
{
struct HttpState
{
    std::mutex mutex;
    std::condition_variable ready;
    NSMutableData* data = [[NSMutableData alloc] init];
    bool done = false, failed = false, tooLarge = false;
    NSInteger status = 0;
    int64_t maxBytes = 0;
};
} // namespace sonora::platform

@interface SonoraHttpDelegate : NSObject<NSURLSessionDataDelegate>
{
@public std::shared_ptr<sonora::platform::HttpState> state;
}
@end
@implementation SonoraHttpDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completionHandler
{
    (void)session; (void)task; (void)response; (void)request;
    completionHandler(nil); // Credentials never follow redirects, including cross-origin redirects.
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler
{
    (void)session; (void)task;
    { const std::lock_guard lock(state->mutex); state->status = [(NSHTTPURLResponse*)response statusCode]; }
    completionHandler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    (void)session;
    const std::lock_guard lock(state->mutex);
    if (static_cast<int64_t>(state->data.length + data.length) > state->maxBytes) { state->tooLarge = true; [task cancel]; }
    else [state->data appendData:data];
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error
{
    (void)session; (void)task;
    { const std::lock_guard lock(state->mutex); state->failed = error != nil; state->done = true; }
    state->ready.notify_all();
}
@end

namespace sonora::platform
{
HttpResponse postNoRedirect(const HttpRequest& request, const std::atomic<bool>& cancelled)
{
    @autoreleasepool
    {
        HttpResponse result;
        if (cancelled) { result.outcome = HttpResponse::Outcome::Cancelled; return result; }
        auto* urlString = [NSString stringWithUTF8String:request.url.toRawUTF8()];
        auto* url = urlString ? [NSURL URLWithString:urlString] : nil;
        // Same rule as the portable client: a scheme and a host (agent::validate already requires https).
        if (! url || url.scheme.length == 0 || url.host.length == 0) { result.outcome = HttpResponse::Outcome::InvalidUrl; return result; }
        auto state = std::make_shared<HttpState>();
        state->maxBytes = request.maxResponseBytes;
        auto* delegate = [[SonoraHttpDelegate alloc] init]; delegate->state = state;
        auto* settings = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        settings.timeoutIntervalForRequest = request.requestTimeoutSeconds;
        settings.timeoutIntervalForResource = request.totalTimeoutSeconds;
        settings.URLCache = nil; settings.HTTPCookieStorage = nil;
        auto* session = [NSURLSession sessionWithConfiguration:settings delegate:delegate delegateQueue:nil];
        auto* urlRequest = [NSMutableURLRequest requestWithURL:url]; urlRequest.HTTPMethod = @"POST";
        const auto names = request.headers.getAllKeys();
        const auto values = request.headers.getAllValues();
        for (int i = 0; i < names.size(); ++i)
        {
            auto* value = [NSString stringWithUTF8String:values[i].toRawUTF8()];
            auto* name = [NSString stringWithUTF8String:names[i].toRawUTF8()];
            if (value && name) [urlRequest setValue:value forHTTPHeaderField:name];
        }
        urlRequest.HTTPBody = [NSData dataWithBytes:request.body.toRawUTF8() length:request.body.getNumBytesAsUTF8()];
        auto* task = [session dataTaskWithRequest:urlRequest]; [task resume];
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(request.totalTimeoutSeconds);
        std::unique_lock lock(state->mutex);
        while (! state->done && ! cancelled && std::chrono::steady_clock::now() < deadline)
            state->ready.wait_for(lock, std::chrono::milliseconds(50));
        if (! state->done || cancelled)
        {
            lock.unlock(); [session invalidateAndCancel];
            result.outcome = cancelled ? HttpResponse::Outcome::Cancelled : HttpResponse::Outcome::TimedOut;
            return result;
        }
        result.body = juce::String::fromUTF8(static_cast<const char*>(state->data.bytes), static_cast<int>(state->data.length));
        result.status = static_cast<int>(state->status);
        const bool failed = state->failed, tooLarge = state->tooLarge;
        lock.unlock(); [session finishTasksAndInvalidate];
        result.outcome = tooLarge ? HttpResponse::Outcome::TooLarge
                       : failed   ? HttpResponse::Outcome::NetworkFailed
                                  : HttpResponse::Outcome::Completed;
        return result;
    }
}
} // namespace sonora::platform

// ---- LIB-1: resumable GET into a file (HttpClient.h downloadToFile)
namespace sonora::platform
{
struct DownloadState
{
    std::mutex mutex;
    std::condition_variable ready;
    FILE* file = nullptr;
    bool done = false, failed = false, timedOut = false, tooLarge = false, refused = false, resumed = false;
    NSInteger status = 0;
    int redirects = 0, maxRedirects = 0;
    int64_t startOffset = 0, fileBytes = 0, expected = 0, maxBytes = 0;
    std::string path, range;
};
} // namespace sonora::platform

@interface SonoraDownloadDelegate : NSObject<NSURLSessionDataDelegate>
{
@public std::shared_ptr<sonora::platform::DownloadState> state;
}
@end
@implementation SonoraDownloadDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completionHandler
{
    (void)session; (void)task; (void)response;
    const auto target = juce::String::fromUTF8(request.URL.absoluteString.UTF8String);
    std::string range;
    {
        const std::lock_guard lock(state->mutex);
        if (++state->redirects > state->maxRedirects || ! sonora::platform::downloadUrlAllowed(target)) { completionHandler(nil); return; }
        range = state->range;
    }
    NSMutableURLRequest* next = [request mutableCopy];
    if (! range.empty()) [next setValue:[NSString stringWithUTF8String:range.c_str()] forHTTPHeaderField:@"Range"];
    completionHandler(next);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler
{
    (void)session; (void)task;
    auto* http = (NSHTTPURLResponse*)response;
    const std::lock_guard lock(state->mutex);
    state->status = http.statusCode;
    const int64_t length = response.expectedContentLength; // -1 = unknown
    if (state->status == 206)
    {
        int64_t total = 0;
        auto* header = [http valueForHTTPHeaderField:@"Content-Range"];
        const auto first = header ? sonora::platform::contentRangeStart(juce::String::fromUTF8(header.UTF8String), total) : -1;
        if (first != state->startOffset) { state->failed = true; completionHandler(NSURLSessionResponseCancel); return; }
        state->file = std::fopen(state->path.c_str(), "ab");
        state->resumed = true;
        state->expected = total > 0 ? total : length >= 0 ? state->startOffset + length : 0;
    }
    else if (state->status == 200)
    {
        state->file = std::fopen(state->path.c_str(), "wb");
        state->fileBytes = 0;
        state->expected = length >= 0 ? length : 0;
    }
    else
    {
        state->refused = true; // not an error: the caller reads the status
        completionHandler(NSURLSessionResponseCancel);
        return;
    }
    if (state->file == nullptr) { state->failed = true; completionHandler(NSURLSessionResponseCancel); return; }
    completionHandler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    (void)session;
    const std::lock_guard lock(state->mutex);
    if (state->file == nullptr) return;
    if (state->fileBytes + static_cast<int64_t>(data.length) > state->maxBytes) { state->tooLarge = true; [task cancel]; return; }
    __block bool ok = true;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL* stop) {
        if (std::fwrite(bytes, 1, range.length, state->file) != range.length) { ok = false; *stop = YES; }
    }];
    if (! ok) { state->failed = true; [task cancel]; return; }
    state->fileBytes += static_cast<int64_t>(data.length);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error
{
    (void)session; (void)task;
    {
        const std::lock_guard lock(state->mutex);
        if (state->file != nullptr) { if (std::fclose(state->file) != 0) state->failed = true; state->file = nullptr; }
        if (error != nil && ! state->refused && ! state->tooLarge)
        {
            state->failed = true;
            state->timedOut = [error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorTimedOut;
        }
        state->done = true;
    }
    state->ready.notify_all();
}
@end

namespace sonora::platform
{
DownloadResult downloadToFile(const DownloadRequest& request, const std::function<void(int64_t, int64_t)>& progress,
                              const std::atomic<bool>& cancelled)
{
    @autoreleasepool
    {
        DownloadResult result;
        result.fileBytes = request.destination.existsAsFile() ? request.destination.getSize() : 0;
        if (cancelled) { result.outcome = HttpResponse::Outcome::Cancelled; return result; }
        auto* urlString = [NSString stringWithUTF8String:request.url.toRawUTF8()];
        auto* url = urlString ? [NSURL URLWithString:urlString] : nil;
        if (! url || ! downloadUrlAllowed(request.url)) { result.outcome = HttpResponse::Outcome::InvalidUrl; return result; }
        if (result.fileBytes > request.maxBytes) { result.outcome = HttpResponse::Outcome::TooLarge; return result; }
        auto state = std::make_shared<DownloadState>();
        state->maxRedirects = request.maxRedirects;
        state->maxBytes = request.maxBytes;
        state->startOffset = state->fileBytes = result.fileBytes;
        state->path = request.destination.getFullPathName().toStdString();
        if (state->startOffset > 0) state->range = "bytes=" + std::to_string(state->startOffset) + "-";
        auto* delegate = [[SonoraDownloadDelegate alloc] init]; delegate->state = state;
        auto* settings = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        settings.timeoutIntervalForRequest = request.idleTimeoutSeconds;
        settings.timeoutIntervalForResource = 7 * 24 * 3600;
        settings.URLCache = nil; settings.HTTPCookieStorage = nil;
        auto* session = [NSURLSession sessionWithConfiguration:settings delegate:delegate delegateQueue:nil];
        auto* urlRequest = [NSMutableURLRequest requestWithURL:url];
        if (! state->range.empty()) [urlRequest setValue:[NSString stringWithUTF8String:state->range.c_str()] forHTTPHeaderField:@"Range"];
        [urlRequest setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"]; // byte offsets must be file offsets
        auto* task = [session dataTaskWithRequest:urlRequest]; [task resume];
        auto lastReport = std::chrono::steady_clock::now();
        std::unique_lock lock(state->mutex);
        while (! state->done && ! cancelled)
        {
            state->ready.wait_for(lock, std::chrono::milliseconds(50));
            const auto now = std::chrono::steady_clock::now();
            if (progress && now - lastReport >= std::chrono::milliseconds(100))
            {
                lastReport = now;
                const auto bytes = state->fileBytes, expected = state->expected;
                lock.unlock(); progress(bytes, expected); lock.lock();
            }
        }
        if (! state->done)
        {
            lock.unlock();
            [task cancel];
            // Wait for didCompleteWithError so the file is closed before the caller touches it.
            lock.lock();
            state->ready.wait_for(lock, std::chrono::seconds(2), [&] { return state->done; });
            result.fileBytes = state->fileBytes;
            lock.unlock(); [session invalidateAndCancel];
            result.outcome = HttpResponse::Outcome::Cancelled;
            return result;
        }
        result.status = static_cast<int>(state->status);
        result.resumed = state->resumed;
        result.fileBytes = state->fileBytes;
        result.outcome = state->tooLarge ? HttpResponse::Outcome::TooLarge
                       : state->timedOut ? HttpResponse::Outcome::TimedOut
                       : state->failed   ? HttpResponse::Outcome::NetworkFailed
                                         : HttpResponse::Outcome::Completed;
        if (progress && result.outcome == HttpResponse::Outcome::Completed) { const auto e = state->expected; lock.unlock(); progress(result.fileBytes, e); }
        else lock.unlock();
        [session finishTasksAndInvalidate];
        return result;
    }
}
} // namespace sonora::platform
