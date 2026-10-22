// macOS HttpClient: ephemeral NSURLSession. Compiled with ARC (-fobjc-arc, see src/CMakeLists.txt).
// Moved unchanged from agent/AgentHttp.mm (COM-7); the provider-specific framing stays in the agent.

#include "platform/HttpClient.h"

#import <Foundation/Foundation.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

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
