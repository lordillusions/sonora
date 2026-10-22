#include "control/LocalEndpoint.h"
#include "core/AppDataFolder.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <mutex>

namespace sonora::control
{
struct LocalEndpoint::Shared
{
    std::atomic<bool> active{true};
    std::atomic<int> processing{0};
    std::atomic<int64_t> requests{0}, lastRequestMs{0};
    std::function<juce::var(const juce::var&)> handler;
};
namespace
{
juce::var denied(const char* code, const char* message)
{ return object({{"ok", false}, {"error", object({{"code", code}, {"message", message}})}}); }
bool privateDirectory(const juce::File& directory)
{
    const auto path = directory.getFullPathName().toStdString();
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 077) == 0;
}
bool privateWrite(const juce::File& file, const juce::String& text)
{
    // Parent is private and filenames are unpredictable; create with exclusive, owner-only permissions.
    const auto path = file.getFullPathName().toStdString();
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const auto bytes = text.toRawUTF8();
    const auto length = text.getNumBytesAsUTF8();
    const bool ok = ::write(fd, bytes, length) == static_cast<ssize_t>(length);
    ::close(fd); return ok;
}
// Same-user check and no-SIGPIPE writes differ between macOS (getpeereid, SO_NOSIGPIPE) and Linux (COM-7).
bool peerIsCurrentUser(int client)
{
#if defined(__linux__)
    ucred peer{}; socklen_t size = sizeof(peer);
    return ::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.uid == ::geteuid();
#else
    uid_t user = 0; gid_t group = 0;
    return ::getpeereid(client, &user, &group) == 0 && user == ::geteuid();
#endif
}
#if defined(__linux__)
constexpr int sendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
void suppressSigPipe(int) {}
#else
constexpr int sendFlags = MSG_DONTWAIT;
void suppressSigPipe(int client) { const int noSigPipe = 1; ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)); }
#endif
bool boundedJson(const std::string& text)
{
    int depth = 0; bool quoted = false, escaped = false;
    for (const char c : text)
    {
        if (quoted)
        { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > 64) return false; }
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return ! quoted && depth == 0;
}
}
LocalEndpoint::LocalEndpoint() : Thread("Sonora local control") {}
LocalEndpoint::~LocalEndpoint() { disable(); }
LocalEndpoint::Activity LocalEndpoint::activity() const
{
    return shared_ ? Activity{shared_->processing.load(), shared_->requests.load(), shared_->lastRequestMs.load()} : Activity{};
}
void LocalEndpoint::disable()
{
    if (shared_) shared_->active = false;
    signalThreadShouldExit();
    if (listener_ >= 0) ::shutdown(listener_, SHUT_RDWR);
    stopThread(6500);
    if (listener_ >= 0) ::close(listener_);
    listener_ = -1;
    for (const auto& file : {socket_, registry_, credential_})
        if (file != juce::File()) ::unlink(file.getFullPathName().toRawUTF8());
    shared_.reset(); token_.clear();
}
ApplyResult LocalEndpoint::enable(const std::string& instanceId, const std::string& documentId,
                                  std::function<juce::var(const juce::var&)> handler)
{
    disable();
    const juce::File folder("/tmp/sonora-control-" + juce::String(static_cast<int>(::geteuid())));
    if (! privateDirectory(folder)) return ApplyResult::failure("ipc_permissions", "Cannot create private control directory");
    const auto launch = juce::Uuid().toString();
    socket_ = folder.getChildFile(launch.substring(0, 16) + ".sock");
    credential_ = folder.getChildFile(launch + ".credential");
    const auto registryDir = core::appDataDirectory().getChildFile("Automation/instances");
    if (! registryDir.getParentDirectory().createDirectory() || ! privateDirectory(registryDir))
        return ApplyResult::failure("ipc_permissions", "Cannot create private registry");
    registry_ = registryDir.getChildFile(juce::String(instanceId) + ".json");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto path = socket_.getFullPathName().toStdString();
    if (path.size() >= sizeof(address.sun_path)) return ApplyResult::failure("ipc_path", "Control socket path is too long");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener_ < 0 || ::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(listener_, 4) < 0)
    { disable(); return ApplyResult::failure("ipc_start", "Cannot start local control socket"); }
    ::chmod(path.c_str(), 0600);
    token_ = juce::Uuid().toString().toStdString() + juce::Uuid().toString().toStdString();
    if (! privateWrite(credential_, juce::String(token_)) || ! privateWrite(registry_, juce::JSON::toString(object({
        {"instanceId", instanceId}, {"documentId", documentId}, {"socket", socket_.getFullPathName()},
        {"credentialFile", credential_.getFullPathName()}, {"pid", static_cast<int>(::getpid())}, {"apiVersion", "1.0"}}))))
    { disable(); return ApplyResult::failure("ipc_registry", "Cannot publish private control registry"); }
    shared_ = std::make_shared<Shared>(); shared_->handler = std::move(handler);
    startThread(); return ApplyResult::success();
}
void LocalEndpoint::run()
{
    while (! threadShouldExit())
    {
        pollfd incoming{listener_, POLLIN, 0};
        if (::poll(&incoming, 1, 100) <= 0) continue;
        const int client = ::accept(listener_, nullptr, nullptr);
        if (client < 0) continue;
        if (! peerIsCurrentUser(client)) { ::close(client); continue; }
        suppressSigPipe(client);
        std::string body;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        while (! threadShouldExit() && body.size() <= 1024 * 1024 && body.find('\n') == std::string::npos
               && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            pollfd readable{client, POLLIN, 0};
            if (::poll(&readable, 1, 100) <= 0) continue;
            char bytes[4096]; const auto count = ::read(client, bytes, sizeof(bytes));
            if (count <= 0) break;
            body.append(bytes, static_cast<size_t>(count));
        }
        juce::var response = denied("invalid_request", "Expected bounded newline-delimited JSON");
        bool authenticated = false;
        if (body.size() <= 1024 * 1024 && body.find('\n') != std::string::npos && boundedJson(body))
        {
            const auto request = juce::JSON::parse(juce::String::fromUTF8(body.data(), static_cast<int>(body.find('\n'))));
            response = denied("unauthorized", "Local control credential rejected");
            if (request["token"].toString().toStdString() == token_)
            {
                authenticated = true; ++shared_->processing; ++shared_->requests;
                shared_->lastRequestMs = juce::Time::currentTimeMillis();
                struct Pending { std::mutex mutex; juce::var result; bool done = false; std::atomic<bool> expired{false}; };
                auto pending = std::make_shared<Pending>();
                const auto shared = shared_;
                juce::MessageManager::callAsync([shared, pending, input = request["request"]] {
                    if (! shared->active || pending->expired) return;
                    auto output = shared->handler(input);
                    const std::lock_guard lock(pending->mutex); pending->result = output; pending->done = true;
                });
                // Plugin construction can be slow. Timeout is an unknown outcome, never a safe automatic retry.
                const auto finishBy = juce::Time::getMillisecondCounterHiRes() + 120000;
                response = denied("outcome_unknown", "Control request did not finish; reconcile before retrying");
                while (! threadShouldExit() && juce::Time::getMillisecondCounterHiRes() < finishBy)
                {
                    { const std::lock_guard lock(pending->mutex); if (pending->done) { response = pending->result; break; } }
                    wait(10);
                }
                pending->expired = true;
            }
        }
        const auto output = juce::JSON::toString(response, true) + "\n";
        const char* bytes = output.toRawUTF8(); size_t remaining = output.getNumBytesAsUTF8();
        const auto sendBy = juce::Time::getMillisecondCounterHiRes() + 5000;
        while (remaining > 0 && ! threadShouldExit() && juce::Time::getMillisecondCounterHiRes() < sendBy)
        {
            pollfd writable{client, POLLOUT, 0}; if (::poll(&writable, 1, 100) <= 0) continue;
            const auto sent = ::send(client, bytes, remaining, sendFlags);
            if (sent < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (sent <= 0) break;
            bytes += sent; remaining -= static_cast<size_t>(sent);
        }
        ::close(client);
        if (authenticated) --shared_->processing;
    }
}
} // namespace sonora::control
