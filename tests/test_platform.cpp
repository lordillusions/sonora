// COM-7: platform seams (docs/PLATFORMS.md). Runs on every platform; the file secret store only where it is the backend.
#include "TestFramework.h"
#include "platform/CNumeric.h"
#include "platform/HttpClient.h"
#include "platform/Paths.h"
#include "platform/SecretStore.h"

#include <atomic>
#include <cerrno>
#include <clocale>
#include <cstring>
#include <mutex>
#include <thread>
#if ! defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace sonora;

TEST_CASE("platform: C numeric locale parses '.' decimals regardless of the process locale")
{
    const platform::CNumericLocale c;
    REQUIRE(c.valid());
    // Try a comma-decimal locale when the system has one; the parse must not change.
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    const bool switched = std::setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr || std::setlocale(LC_NUMERIC, "de_DE") != nullptr;
    const char* text = "1.25e2xyz";
    char* end = nullptr;
    errno = 0;
    const double value = c.strtod(text, &end);
    CHECK(errno == 0);
    CHECK_NEAR(value, 125.0, 1e-12);
    CHECK(end == text + std::strlen("1.25e2"));
    errno = 0;
    const char* huge = "1e999";
    (void) c.strtod(huge, &end);
    CHECK(errno == ERANGE);
    if (switched) std::setlocale(LC_NUMERIC, previous.c_str());
}

TEST_CASE("platform: app data folder is per user and named Sonora")
{
    const auto folder = platform::appDataFolder();
    CHECK(folder.getFileName() == "Sonora");
    CHECK(folder.isAChildOf(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)));
#if defined(__APPLE__)
    CHECK(folder.getParentDirectory().getFileName() == "Application Support"); // unchanged macOS location
#endif
}

TEST_CASE("platform: HTTP client rejects bad URLs and honours an early cancel without network")
{
    std::atomic<bool> cancelled{false};
    platform::HttpRequest request;
    request.url = "not a url";
    CHECK(platform::postNoRedirect(request, cancelled).outcome == platform::HttpResponse::Outcome::InvalidUrl);
    cancelled = true;
    request.url = "https://example.invalid/v1";
    CHECK(platform::postNoRedirect(request, cancelled).outcome == platform::HttpResponse::Outcome::Cancelled);
}

namespace
{
// Minimal loopback HTTP/1.1 server: one request per connection, responses chosen by path.
class FixtureServer final : juce::Thread
{
public:
    FixtureServer() : juce::Thread("http fixture")
    {
        listening_ = listener_.createListener(0, "127.0.0.1");
        startThread();
    }
    ~FixtureServer() override
    {
        signalThreadShouldExit();
        listener_.close();
        stopThread(4000);
    }
    bool listening() const { return listening_; }
    juce::String url(const char* path) const { return "http://127.0.0.1:" + juce::String(listener_.getBoundPort()) + path; }
    std::atomic<bool> leaked{false};
    juce::String lastHeaders, lastBody;
    std::mutex mutex;

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> client(listener_.waitForNextConnection());
            if (client == nullptr) continue;
            std::string request;
            char buffer[4096];
            size_t headerEnd = std::string::npos;
            while (headerEnd == std::string::npos && client->waitUntilReady(true, 2000) == 1)
            {
                const int n = client->read(buffer, (int) sizeof buffer, false);
                if (n <= 0) break;
                request.append(buffer, (size_t) n);
                headerEnd = request.find("\r\n\r\n");
            }
            if (headerEnd == std::string::npos) continue;
            const juce::String headers(request.substr(0, headerEnd));
            const auto lengthLine = headers.fromFirstOccurrenceOf("content-length:", false, true).upToFirstOccurrenceOf("\r\n", false, false);
            const size_t length = (size_t) lengthLine.trim().getLargeIntValue();
            while (request.size() < headerEnd + 4 + length && client->waitUntilReady(true, 2000) == 1)
            {
                const int n = client->read(buffer, (int) sizeof buffer, false);
                if (n <= 0) break;
                request.append(buffer, (size_t) n);
            }
            const auto path = headers.fromFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf(" ", false, false);
            {
                const std::lock_guard lock(mutex);
                lastHeaders = headers;
                lastBody = juce::String(request.substr(headerEnd + 4));
            }
            juce::String status = "200 OK", extra, body = "{\"ok\":true}";
            if (path == "/redirect") { status = "302 Found"; extra = "Location: " + url("/leak") + "\r\n"; body = "{}"; }
            else if (path == "/leak") leaked = true;
            else if (path == "/unauthorized") { status = "401 Unauthorized"; body = "{\"error\":\"no\"}"; }
            else if (path == "/big") body = juce::String::repeatedString("x", 64 * 1024);
            else if (path == "/slow") { for (int i = 0; i < 300 && ! threadShouldExit(); ++i) juce::Thread::sleep(10); }
            const auto response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: "
                + juce::String(body.getNumBytesAsUTF8()) + "\r\nConnection: close\r\n" + extra + "\r\n" + body;
            client->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
        }
    }
    juce::StreamingSocket listener_;
    bool listening_ = false;
};
} // namespace

TEST_CASE("platform: HTTP client posts headers and body, refuses redirects, caps size, reports status and cancels")
{
    FixtureServer server;
    REQUIRE(server.listening());
    std::atomic<bool> cancelled{false};
    platform::HttpRequest request;
    request.headers.set("Content-Type", "application/json");
    request.headers.set("x-api-key", "fixture-key");
    request.body = "{\"prompt\":\"hi\"}";
    request.totalTimeoutSeconds = 10;
    request.maxResponseBytes = 4096;
    using Outcome = platform::HttpResponse::Outcome;

    request.url = server.url("/ok");
    auto response = platform::postNoRedirect(request, cancelled);
    CHECK(response.outcome == Outcome::Completed);
    CHECK(response.status == 200);
    CHECK(response.body == "{\"ok\":true}");
    {
        const std::lock_guard lock(server.mutex);
        CHECK(server.lastHeaders.startsWith("POST /ok "));
        CHECK(server.lastHeaders.containsIgnoreCase("x-api-key: fixture-key"));
        CHECK(server.lastBody == request.body);
    }

    request.url = server.url("/redirect");
    response = platform::postNoRedirect(request, cancelled);
    CHECK(response.status == 302); // reported, never followed: credentials must not reach another URL
    CHECK(! server.leaked);

    request.url = server.url("/unauthorized");
    CHECK(platform::postNoRedirect(request, cancelled).status == 401);

    request.url = server.url("/big");
    CHECK(platform::postNoRedirect(request, cancelled).outcome == Outcome::TooLarge);

    request.url = server.url("/slow");
    std::thread canceller([&] { juce::Thread::sleep(200); cancelled = true; });
    const auto start = juce::Time::getMillisecondCounterHiRes();
    CHECK(platform::postNoRedirect(request, cancelled).outcome == Outcome::Cancelled);
    CHECK(juce::Time::getMillisecondCounterHiRes() - start < 2000.0);
    canceller.join();
}

#if ! defined(__APPLE__)
TEST_CASE("platform: file secret store round trip, scoping, removal and owner-only file")
{
    const auto folder = juce::File::createTempFile("sonora-secrets");
    platform::SecretStore store(folder);
    const platform::SecretId a{"org.sonora.test", "openai:Responses:https://api.example.com/v1"};
    const platform::SecretId b{"org.sonora.test", "anthropic:Anthropic:https://api.example.com"};
    juce::String value;
    CHECK(! store.contains(a));
    CHECK(store.read(a, value).code == platform::SecretStatus::Code::NotFound);
    REQUIRE(store.write(a, "sk-one").ok());
    REQUIRE(store.write(b, "sk-two").ok());
    REQUIRE(store.write(a, "sk-three").ok()); // overwrite
    CHECK(store.read(a, value).ok() && value == "sk-three");
    CHECK(platform::SecretStore(folder).read(b, value).ok() && value == "sk-two"); // persisted
    CHECK(store.remove(a).ok());
    CHECK(! store.contains(a));
    CHECK(store.remove(a).ok()); // removing a missing secret is fine
    CHECK(store.contains(b));
    CHECK(folder.findChildFiles(juce::File::findFiles, false, "*.tmp").isEmpty());
#if ! defined(_WIN32)
    struct stat info{};
    REQUIRE(::stat(folder.getChildFile("Secrets.json").getFullPathName().toRawUTF8(), &info) == 0);
    CHECK((info.st_mode & 077) == 0);
#endif
    folder.deleteRecursively();
}
#endif
