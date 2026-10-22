#include "collab/Net.h"

#include "core/Sha256.h"

#include <juce_core/juce_core.h>

#if SONORA_COLLAB_NET
#include <rtc/rtc.hpp>
#endif

#if defined(__APPLE__)
#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <dns_sd.h>
#endif

#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace sonora::collab
{

std::vector<std::string> lanAddresses()
{
    std::vector<std::string> out;
    juce::Array<juce::IPAddress> all;
    juce::IPAddress::findAllAddresses(all, false);
    for (const auto& a : all)
    {
        const auto s = a.toString().toStdString();
        if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0 || s == "0.0.0.0") continue;
        // Private ranges first: those are the ones a collaborator in the same room can reach.
        const bool privateRange = s.rfind("192.168.", 0) == 0 || s.rfind("10.", 0) == 0 || s.rfind("172.", 0) == 0;
        if (privateRange) out.insert(out.begin(), s);
        else out.push_back(s);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ Bonjour

#if defined(__APPLE__)
namespace
{
constexpr const char* serviceType = "_sonora-collab._tcp";

class Advertiser
{
public:
    Advertiser(int port, const std::string& hostName, const std::string& sessionName)
    {
        queue_ = dispatch_queue_create("sonora.collab.bonjour", DISPATCH_QUEUE_SERIAL);
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        TXTRecordSetValue(&txt, "name", static_cast<uint8_t>(std::min<size_t>(hostName.size(), 200)), hostName.data());
        TXTRecordSetValue(&txt, "session", static_cast<uint8_t>(std::min<size_t>(sessionName.size(), 200)), sessionName.data());
        const auto instance = (hostName.empty() ? std::string("Sonora") : hostName).substr(0, 60);
        if (DNSServiceRegister(&ref_, 0, 0, instance.c_str(), serviceType, nullptr, nullptr, htons(static_cast<uint16_t>(port)),
                               TXTRecordGetLength(&txt), TXTRecordGetBytesPtr(&txt), nullptr, nullptr) == kDNSServiceErr_NoError)
            DNSServiceSetDispatchQueue(ref_, queue_);
        else ref_ = nullptr;
        TXTRecordDeallocate(&txt);
    }
    ~Advertiser()
    {
        if (ref_) dispatch_sync(queue_, ^{ DNSServiceRefDeallocate(ref_); });
    }

private:
    DNSServiceRef ref_ = nullptr;
    dispatch_queue_t queue_;
};
} // namespace

struct LanBrowser::Impl
{
    dispatch_queue_t queue = dispatch_queue_create("sonora.collab.browse", DISPATCH_QUEUE_SERIAL);
    DNSServiceRef browse = nullptr;
    std::map<std::string, DNSServiceRef> resolving; // instance name -> resolver (only touched on `queue`)
    mutable std::mutex lock;
    std::map<std::string, LanSession> found;

    static std::string txtValue(uint16_t length, const unsigned char* txt, const char* key)
    {
        uint8_t size = 0;
        const void* value = TXTRecordGetValuePtr(length, txt, key, &size);
        return value ? std::string(static_cast<const char*>(value), size) : std::string();
    }

    static void onResolve(DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType error, const char*, const char* host,
                          uint16_t port, uint16_t txtLength, const unsigned char* txt, void* context)
    {
        auto* r = static_cast<std::pair<Impl*, std::string>*>(context);
        if (error == kDNSServiceErr_NoError)
        {
            LanSession s;
            s.service = r->second;
            s.hostName = txtValue(txtLength, txt, "name");
            s.sessionName = txtValue(txtLength, txt, "session");
            std::string target = host ? host : "";
            if (! target.empty() && target.back() == '.') target.pop_back();
            s.address = target + ":" + std::to_string(ntohs(port));
            const std::lock_guard<std::mutex> l(r->first->lock);
            r->first->found[s.service] = s;
        }
    }

    static void onBrowse(DNSServiceRef, DNSServiceFlags flags, uint32_t interface, DNSServiceErrorType error, const char* name,
                         const char* type, const char* domain, void* context)
    {
        auto* self = static_cast<Impl*>(context);
        if (error != kDNSServiceErr_NoError || ! name) return;
        const std::string instance = name;
        if ((flags & kDNSServiceFlagsAdd) == 0)
        {
            if (auto it = self->resolving.find(instance); it != self->resolving.end())
            {
                DNSServiceRefDeallocate(it->second);
                self->resolving.erase(it);
            }
            const std::lock_guard<std::mutex> l(self->lock);
            self->found.erase(instance);
            return;
        }
        if (self->resolving.count(instance)) return;
        auto* ctx = new std::pair<Impl*, std::string>(self, instance); // owned by the resolver for the browser's lifetime
        DNSServiceRef resolver = nullptr;
        if (DNSServiceResolve(&resolver, 0, interface, name, type, domain, onResolve, ctx) == kDNSServiceErr_NoError)
        {
            DNSServiceSetDispatchQueue(resolver, self->queue);
            self->resolving[instance] = resolver;
            self->contexts.push_back(std::unique_ptr<std::pair<Impl*, std::string>>(ctx));
        }
        else delete ctx;
    }
    std::vector<std::unique_ptr<std::pair<Impl*, std::string>>> contexts;
};

LanBrowser::LanBrowser() : impl_(std::make_unique<Impl>())
{
    auto* impl = impl_.get();
    if (DNSServiceBrowse(&impl->browse, 0, 0, serviceType, nullptr, Impl::onBrowse, impl) == kDNSServiceErr_NoError)
        DNSServiceSetDispatchQueue(impl->browse, impl->queue);
    else impl->browse = nullptr;
}

LanBrowser::~LanBrowser()
{
    auto* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        for (auto& [name, ref] : impl->resolving) DNSServiceRefDeallocate(ref);
        impl->resolving.clear();
        if (impl->browse) DNSServiceRefDeallocate(impl->browse);
        impl->browse = nullptr;
    });
}

std::vector<LanSession> LanBrowser::sessions() const
{
    const std::lock_guard<std::mutex> l(impl_->lock);
    std::vector<LanSession> out;
    for (const auto& [name, s] : impl_->found) out.push_back(s);
    return out;
}
#else
struct LanBrowser::Impl {};
LanBrowser::LanBrowser() : impl_(std::make_unique<Impl>()) {}
LanBrowser::~LanBrowser() = default;
std::vector<LanSession> LanBrowser::sessions() const { return {}; }
#endif

// ------------------------------------------------------------------------------------------------ WebRTC

#if SONORA_COLLAB_NET
bool networkAvailable() { return true; }

namespace
{
constexpr size_t frameBytes = 60 * 1024;
constexpr size_t maxMessageBytes = 512u << 20;
constexpr const char* labels[3] = {"ops", "live", "assets"};
constexpr uint8_t frameWhole = 0, frameFirst = 1, frameMiddle = 2, frameLast = 3;

std::string toJson(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }
juce::var parseJson(const std::string& s) { return juce::JSON::parse(juce::String::fromUTF8(s.c_str(), static_cast<int>(s.size()))); }
std::string str(const juce::var& v) { return v.toString().toStdString(); }

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : fields) o->setProperty(k, v);
    return juce::var(o);
}

std::string closeMessage(int code, const std::string& reason, bool host)
{
    switch (code)
    {
        case 4401: return host ? "The relay did not accept this host. Check the relay address and your account."
                               : "This invite is no longer valid: the session ended or the link is wrong.";
        case 4402: return "Hosting on this relay needs an active Sonora Cloud subscription.";
        case 4002: return "The session is full.";
        case 4003: return "The host removed you from the session.";
        case 4001: return "The session ended.";
        case 4004: return "This session was opened again somewhere else.";
        default: break;
    }
    return reason.empty() ? std::string("The connection to the relay closed.") : "The relay closed the connection: " + reason;
}

int64_t connectTimeoutMs()
{
    // Probes shorten it; ICE through TURN can take a while on real networks.
    const auto env = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_CONNECT_TIMEOUT_MS", {}).getLargeIntValue();
    return env > 0 ? env : 30000;
}

// Same-machine tests (two app instances, or a sandbox without Local Network access): ICE on one address only.
void applyTestBinding(rtc::Configuration& config)
{
    const auto bind = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_BIND_ADDRESS", {}).toStdString();
    if (! bind.empty()) config.bindAddress = bind;
}

void initLibrary()
{
    static std::once_flag once;
    std::call_once(once, [] { rtc::InitLogger(rtc::LogLevel::Error); });
}

struct Link
{
    PeerId id;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::array<std::shared_ptr<rtc::DataChannel>, 3> dc;
    std::array<std::string, 3> partial;
    std::array<size_t, 3> expected{};
    bool connected = false, closed = false;
    std::string secret;             // authenticates this link's signalling
    std::string remote;             // relay: the peer's relay id
    std::shared_ptr<rtc::WebSocket> ws; // LAN host: this guest's signalling socket
    bool approved = true;
    std::string name;
    int badSignals = 0;
};

class RtcTransport : public Transport
{
public:
    struct Core;
    explicit RtcTransport(std::shared_ptr<Core> core) : core_(std::move(core)) {}
    ~RtcTransport() override;
    void send(const PeerId& peer, Channel channel, std::string bytes) override;
    size_t buffered(const PeerId& peer, Channel channel) const override;
    void disconnect(const PeerId& peer, const std::string& reason) override;
    std::vector<TransportEvent> poll() override;
    void approveJoin(const PeerId& peer, bool allow) override;
    std::string inviteLink() const override;

    std::shared_ptr<Core> core_;
};

struct RtcTransport::Core : std::enable_shared_from_this<Core>
{
    std::mutex lock;
    std::deque<TransportEvent> events;
    std::map<PeerId, std::shared_ptr<Link>> links;
    std::vector<std::shared_ptr<Link>> graveyard; // closed links, destroyed on the message thread
    bool host = false, lan = false;
    std::atomic<bool> shutdown{false};
    Invite invite;
    std::string name, hostToken, hostKey, sessionName;
    std::string selfId, hostId; // relay ids
    std::shared_ptr<rtc::WebSocket> ws;
    std::unique_ptr<rtc::WebSocketServer> server;
    rtc::Configuration config;
    std::atomic<int> closingCode{0};
    std::string closingReason;
    int nextLan = 1;
    std::string inviteText;
    int64_t reconnectAtMs = 0;
    int64_t connectDeadlineMs = 0; // guest: give up when the link is not up by then
    std::thread worker;
#if defined(__APPLE__)
    std::unique_ptr<Advertiser> advertiser;
#endif

    void push(TransportEvent e)
    {
        const std::lock_guard<std::mutex> l(lock);
        events.push_back(std::move(e));
    }
    void status(const std::string& text)
    {
        TransportEvent e;
        e.kind = TransportEvent::Kind::Status;
        e.text = text;
        push(std::move(e));
    }
    void fail(const std::string& text)
    {
        TransportEvent e;
        e.kind = TransportEvent::Kind::Failed;
        e.text = text;
        push(std::move(e));
    }

    // Who a link's signalling is from/to, as the tag binds it.
    std::pair<std::string, std::string> ends(const Link& link, bool outgoing) const
    {
        std::string self = lan ? (host ? "host" : "guest") : selfId;
        std::string other = lan ? (host ? "guest" : "host") : link.remote;
        return outgoing ? std::pair{self, other} : std::pair{other, self};
    }

    void sendSignal(const std::shared_ptr<Link>& link, const juce::var& payload)
    {
        const auto p = toJson(payload);
        const auto [from, to] = ends(*link, true);
        const auto tag = signalTag(link->secret, invite.sessionId, from, to, p);
        auto data = object({{"p", juce::String::fromUTF8(p.c_str())}, {"t", juce::String(tag)}});
        juce::var envelope = lan ? object({{"type", "signal"}, {"data", data}})
                                 : object({{"type", "signal"}, {"to", juce::String(link->remote)}, {"data", data}});
        const auto text = toJson(envelope);
        auto socket = lan && host ? link->ws : ws;
        try
        {
            if (socket && socket->isOpen()) socket->send(text);
        }
        catch (const std::exception&) {}
    }

    void receiveSignal(const std::shared_ptr<Link>& link, const juce::var& data)
    {
        const auto p = str(data["p"]);
        const auto tag = str(data["t"]);
        const auto [from, to] = ends(*link, false);
        if (! verifySignal(link->secret, invite.sessionId, from, to, p, tag))
        {
            if (++link->badSignals > 5) markClosed(link, "unauthenticated signalling");
            return;
        }
        const auto payload = parseJson(p);
        const auto kind = str(payload["kind"]);
        try
        {
            if (kind == "offer" && host)
            {
                ensurePeerConnection(link, false);
                link->pc->setRemoteDescription(rtc::Description(str(payload["sdp"]), "offer"));
            }
            else if (kind == "answer" && ! host && link->pc)
                link->pc->setRemoteDescription(rtc::Description(str(payload["sdp"]), "answer"));
            else if (kind == "candidate" && link->pc)
                link->pc->addRemoteCandidate(rtc::Candidate(str(payload["candidate"]), str(payload["mid"])));
        }
        catch (const std::exception& e)
        {
            status(std::string("Signalling error: ") + e.what());
        }
    }

    void ensurePeerConnection(const std::shared_ptr<Link>& link, bool offerer)
    {
        if (link->pc) return;
        applyTestBinding(config);
        link->pc = std::make_shared<rtc::PeerConnection>(config);
        std::weak_ptr<Core> weak = shared_from_this();
        std::weak_ptr<Link> weakLink = link;
        link->pc->onLocalDescription([weak, weakLink](rtc::Description d) {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (! core || ! l) return;
            core->sendSignal(l, object({{"kind", juce::String(d.typeString())}, {"sdp", juce::String(std::string(d))}}));
        });
        link->pc->onLocalCandidate([weak, weakLink](rtc::Candidate c) {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (! core || ! l) return;
            core->sendSignal(l, object({{"kind", "candidate"}, {"candidate", juce::String(std::string(c))}, {"mid", juce::String(c.mid())}}));
        });
        link->pc->onStateChange([weak, weakLink](rtc::PeerConnection::State s) {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (! core || ! l) return;
            if (s == rtc::PeerConnection::State::Failed)
                core->markClosed(l, l->connected ? "the connection was lost" : "could not connect directly (a TURN relay may be needed)");
            else if (s == rtc::PeerConnection::State::Closed || s == rtc::PeerConnection::State::Disconnected)
                core->markClosed(l, "the connection was lost");
        });
        if (offerer)
        {
            for (int i = 0; i < 3; ++i)
            {
                rtc::DataChannelInit init;
                if (i == static_cast<int>(Channel::Live))
                {
                    init.reliability.unordered = true;
                    init.reliability.maxRetransmits = 0u;
                }
                attach(link, link->pc->createDataChannel(labels[i], init));
            }
        }
        else
            link->pc->onDataChannel([weak, weakLink](std::shared_ptr<rtc::DataChannel> dc) {
                auto core = weak.lock();
                auto l = weakLink.lock();
                if (core && l) core->attach(l, std::move(dc));
            });
    }

    void attach(const std::shared_ptr<Link>& link, std::shared_ptr<rtc::DataChannel> dc)
    {
        int index = -1;
        for (int i = 0; i < 3; ++i)
            if (dc->label() == labels[i]) index = i;
        if (index < 0) { dc->close(); return; }
        {
            const std::lock_guard<std::mutex> l(lock);
            link->dc[(size_t) index] = dc;
        }
        std::weak_ptr<Core> weak = shared_from_this();
        std::weak_ptr<Link> weakLink = link;
        dc->onOpen([weak, weakLink] {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (core && l) core->checkOpen(l);
        });
        dc->onMessage(
            [weak, weakLink, index](rtc::binary data) {
                auto core = weak.lock();
                auto l = weakLink.lock();
                if (core && l) core->frame(l, index, reinterpret_cast<const char*>(data.data()), data.size());
            },
            [](rtc::string) {});
        dc->onClosed([weak, weakLink] {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (core && l) core->markClosed(l, "the connection closed");
        });
        if (dc->isOpen()) checkOpen(link);
    }

    void checkOpen(const std::shared_ptr<Link>& link)
    {
        std::string code;
        {
            const std::lock_guard<std::mutex> l(lock);
            if (link->connected || link->closed) return;
            for (const auto& dc : link->dc)
                if (! dc || ! dc->isOpen()) return;
            link->connected = true;
        }
        try
        {
            const auto local = link->pc->localDescription();
            const auto localFp = local ? local->fingerprint() : std::nullopt;
            const auto remoteFp = link->pc->remoteFingerprint();
            if (localFp) code = safetyCode(link->secret, localFp->value, remoteFp.value);
        }
        catch (const std::exception&) {}
        TransportEvent e;
        e.kind = TransportEvent::Kind::Connected;
        e.peer = link->id;
        e.safetyCode = code;
        e.text = link->name;
        push(std::move(e));
    }

    void frame(const std::shared_ptr<Link>& link, int channel, const char* data, size_t size)
    {
        if (size == 0) return;
        const uint8_t kind = static_cast<uint8_t>(data[0]);
        ++data;
        --size;
        auto& partial = link->partial[(size_t) channel];
        auto& expected = link->expected[(size_t) channel];
        auto deliver = [&](std::string bytes) {
            TransportEvent e;
            e.kind = TransportEvent::Kind::Message;
            e.peer = link->id;
            e.channel = static_cast<Channel>(channel);
            e.data = std::move(bytes);
            push(std::move(e));
        };
        if (kind == frameWhole)
        {
            deliver(std::string(data, size));
            return;
        }
        if (channel == static_cast<int>(Channel::Live)) return; // unordered: never fragmented
        if (kind == frameFirst)
        {
            if (size < 4) { markClosed(link, "malformed frame"); return; }
            uint32_t total = 0;
            std::memcpy(&total, data, 4);
            if (total > maxMessageBytes) { markClosed(link, "message too large"); return; }
            expected = total;
            partial.clear();
            partial.reserve(std::min<size_t>(total, 64u << 20));
            partial.append(data + 4, size - 4);
        }
        else if (kind == frameMiddle || kind == frameLast)
        {
            if (expected == 0 || partial.size() + size > expected) { markClosed(link, "malformed frame"); return; }
            partial.append(data, size);
            if (kind == frameLast)
            {
                if (partial.size() != expected) { markClosed(link, "malformed frame"); return; }
                expected = 0;
                deliver(std::move(partial));
                partial.clear();
            }
        }
        else markClosed(link, "malformed frame");
    }

    void markClosed(const std::shared_ptr<Link>& link, const std::string& reason)
    {
        {
            const std::lock_guard<std::mutex> l(lock);
            if (link->closed) return;
            link->closed = true;
            if (auto it = links.find(link->id); it != links.end() && it->second == link)
            {
                graveyard.push_back(link);
                links.erase(it);
            }
        }
        TransportEvent e;
        e.kind = TransportEvent::Kind::Closed;
        e.peer = link->id;
        e.text = reason;
        push(std::move(e));
    }

    // ---- relay signalling

    void connectRelay()
    {
        auto origin = invite.relay;
        if (origin.rfind("https://", 0) == 0) origin = "wss://" + origin.substr(8);
        else if (origin.rfind("http://", 0) == 0) origin = "ws://" + origin.substr(7);
        rtc::WebSocket::Configuration c;
        c.pingInterval = std::chrono::milliseconds(20000);
        c.connectionTimeout = std::chrono::milliseconds(10000);
        auto socket = std::make_shared<rtc::WebSocket>(c);
        std::weak_ptr<Core> weak = shared_from_this();
        socket->onOpen([weak] {
            auto core = weak.lock();
            if (! core) return;
            juce::var hello = core->host
                ? object({{"type", "hello"}, {"protocol", 1}, {"role", "host"},
                          {"token", juce::String(core->hostToken.empty() ? core->hostKey : core->hostToken)},
                          {"name", juce::String::fromUTF8(core->name.c_str())}})
                : object({{"type", "hello"}, {"protocol", 1}, {"role", "guest"}, {"ticket", juce::String(core->invite.ticket)},
                          {"name", juce::String::fromUTF8(core->name.c_str())}});
            try { core->ws->send(toJson(hello)); } catch (const std::exception&) {}
        });
        socket->onMessage([](rtc::binary) {}, [weak](rtc::string text) {
            if (auto core = weak.lock()) core->relayMessage(parseJson(text));
        });
        socket->onClosed([weak] {
            if (auto core = weak.lock()) core->relayClosed();
        });
        socket->onError([weak](std::string error) {
            if (auto core = weak.lock()) core->status("Relay connection error: " + error);
        });
        ws = socket;
        try { socket->open(origin + "/v1/signal?session=" + invite.sessionId); }
        catch (const std::exception& e) { fail(std::string("Can't reach the relay: ") + e.what()); }
    }

    void relayMessage(const juce::var& m)
    {
        const auto type = str(m["type"]);
        if (type == "welcome")
        {
            selfId = str(m["peerId"]);
            configureTurn(m["turn"]);
            if (host) { status({}); return; }
            if (auto* peers = m["peers"].getArray())
                for (const auto& p : *peers)
                    if (str(p["role"]) == "host") hostId = str(p["peerId"]);
            if (! hostId.empty()) startGuestLink();
            else status("Waiting for the host...");
        }
        else if (type == "peer-joined")
        {
            const auto& peer = m["peer"];
            if (! host && str(peer["role"]) == "host")
            {
                hostId = str(peer["peerId"]);
                std::shared_ptr<Link> existing;
                {
                    const std::lock_guard<std::mutex> l(lock);
                    if (auto it = links.find(hostPeer); it != links.end()) existing = it->second;
                }
                if (existing && ! existing->connected) markClosed(existing, "the host reconnected");
                if (! existing || ! existing->connected) startGuestLink();
                else existing->remote = hostId;
            }
        }
        else if (type == "peer-left")
        {
            if (! host) return;
            std::shared_ptr<Link> gone;
            {
                const std::lock_guard<std::mutex> l(lock);
                for (auto& [id, link] : links)
                    if (link->remote == str(m["peerId"]) && ! link->connected) gone = link;
            }
            if (gone) markClosed(gone, "left");
        }
        else if (type == "signal")
        {
            const auto from = str(m["from"]);
            std::shared_ptr<Link> link;
            {
                const std::lock_guard<std::mutex> l(lock);
                if (host)
                {
                    auto it = links.find(from);
                    if (it == links.end())
                    {
                        auto created = std::make_shared<Link>();
                        created->id = from;
                        created->remote = from;
                        created->secret = invite.secret;
                        it = links.emplace(from, created).first;
                    }
                    link = it->second;
                }
                else if (from == hostId)
                    if (auto it = links.find(hostPeer); it != links.end()) link = it->second;
            }
            if (link) receiveSignal(link, m["data"]);
        }
        else if (type == "closing")
        {
            closingCode = static_cast<int>(m["code"]);
            closingReason = str(m["reason"]);
        }
    }

    void configureTurn(const juce::var& turn)
    {
        if (! turn.isObject()) return;
        const auto user = str(turn["username"]), credential = str(turn["credential"]);
        if (auto* urls = turn["urls"].getArray())
            for (const auto& u : *urls)
            {
                auto url = str(u);
                rtc::IceServer::RelayType relayType = rtc::IceServer::RelayType::TurnUdp;
                if (url.find("transport=tcp") != std::string::npos) relayType = rtc::IceServer::RelayType::TurnTcp;
                if (url.rfind("turns:", 0) == 0) relayType = rtc::IceServer::RelayType::TurnTls;
                const auto colon = url.find(':');
                auto rest = url.substr(colon + 1);
                rest = rest.substr(0, rest.find('?'));
                const auto portSep = rest.rfind(':');
                const auto hostName = rest.substr(0, portSep);
                const auto port = portSep == std::string::npos ? uint16_t(3478) : static_cast<uint16_t>(std::atoi(rest.substr(portSep + 1).c_str()));
                try
                {
                    config.iceServers.emplace_back(hostName, port, user, credential, relayType);
                    if (relayType == rtc::IceServer::RelayType::TurnUdp) config.iceServers.emplace_back("stun:" + hostName + ":" + std::to_string(port));
                }
                catch (const std::exception&) {}
            }
    }

    void startGuestLink()
    {
        auto link = std::make_shared<Link>();
        link->id = hostPeer;
        link->remote = hostId;
        link->secret = invite.secret;
        {
            const std::lock_guard<std::mutex> l(lock);
            links[hostPeer] = link;
        }
        status("Connecting to the host...");
        {
            const std::lock_guard<std::mutex> l(lock);
            connectDeadlineMs = juce::Time::currentTimeMillis() + connectTimeoutMs();
        }
        try { ensurePeerConnection(link, true); }
        catch (const std::exception& e) { fail(std::string("Can't start the connection: ") + e.what()); }
    }

    void relayClosed()
    {
        if (shutdown) return;
        bool anyConnected = false;
        {
            const std::lock_guard<std::mutex> l(lock);
            for (const auto& [id, link] : links) anyConnected = anyConnected || link->connected;
        }
        const bool fatal = closingCode == 4401 || closingCode == 4402 || closingCode == 4001 || closingCode == 4002 || closingCode == 4003
                        || closingCode == 4004 || closingCode == 4000;
        if (host)
        {
            if (fatal) { fail(closeMessage(closingCode, closingReason, true)); return; }
            status("Relay connection lost; reconnecting...");
            const std::lock_guard<std::mutex> l(lock);
            reconnectAtMs = juce::Time::currentTimeMillis() + 3000;
            return;
        }
        if (! anyConnected) fail(closingCode ? closeMessage(closingCode, closingReason, false) : "Could not reach the relay.");
        else if (closingCode == 4003 || closingCode == 4001) fail(closeMessage(closingCode, closingReason, false));
    }

    // ---- LAN signalling

    void startLanServer(int port, const std::string& displayName)
    {
        rtc::WebSocketServer::Configuration c;
        c.port = static_cast<uint16_t>(port);
        c.maxMessageSize = 256 * 1024;
        try { server = std::make_unique<rtc::WebSocketServer>(c); }
        catch (const std::exception&)
        {
            c.port = 0;
            server = std::make_unique<rtc::WebSocketServer>(c);
        }
        std::weak_ptr<Core> weak = shared_from_this();
        server->onClient([weak](std::shared_ptr<rtc::WebSocket> client) {
            if (auto core = weak.lock()) core->lanClient(std::move(client));
        });
        auto addresses = lanAddresses();
        const auto forced = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_LAN_ADDRESS", {}).toStdString(); // probes: loopback
        if (! forced.empty()) addresses.insert(addresses.begin(), forced);
        invite.lanAddress = (addresses.empty() ? std::string("127.0.0.1") : addresses.front()) + ":" + std::to_string(server->port());
        inviteText = formatInvite(invite);
#if defined(__APPLE__)
        advertiser = std::make_unique<Advertiser>(server->port(), displayName, sessionName);
#else
        (void) displayName;
#endif
    }

    void lanClient(std::shared_ptr<rtc::WebSocket> client)
    {
        auto peer = std::make_shared<Link>();
        peer->ws = client;
        peer->approved = false;
        {
            // Held from the first moment: a server-side socket nobody owns is closed mid-handshake.
            const std::lock_guard<std::mutex> l(lock);
            peer->id = "lan-" + std::to_string(nextLan++);
            links[peer->id] = peer;
        }
        std::weak_ptr<Core> weak = shared_from_this();
        std::weak_ptr<Link> weakLink = peer;
        client->onMessage([](rtc::binary) {}, [weak, weakLink](rtc::string text) {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (core && l) core->lanHostMessage(l, parseJson(text));
        });
        client->onClosed([weak, weakLink] {
            auto core = weak.lock();
            auto l = weakLink.lock();
            if (core && l && ! l->connected) core->markClosed(l, "left before connecting");
        });
    }

    void lanHostMessage(const std::shared_ptr<Link>& peer, const juce::var& m)
    {
        const auto type = str(m["type"]);
        if (type == "hello" && peer->name.empty())
        {
            peer->name = str(m["name"]).substr(0, 64);
            if (peer->name.empty()) peer->name = "Guest";
            if (static_cast<int>(m["protocol"]) != 1)
            {
                closeLan(peer, 4000, "Protocol mismatch");
                return;
            }
            const auto ticket = str(m["ticket"]);
            const bool invited = ! ticket.empty() && crypto::equalBytes(ticket, invite.ticket);
            if (invited) approve(peer, invite.secret);
            else
            {
                try { peer->ws->send(toJson(object({{"type", "pending"}}))); } catch (const std::exception&) {}
                TransportEvent e;
                e.kind = TransportEvent::Kind::JoinRequest;
                e.peer = peer->id;
                e.text = peer->name;
                push(std::move(e));
            }
        }
        else if (type == "signal" && peer->approved) receiveSignal(peer, m["data"]);
    }

    void approve(const std::shared_ptr<Link>& peer, const std::string& secret)
    {
        peer->approved = true;
        peer->secret = secret;
        try { peer->ws->send(toJson(object({{"type", "welcome"}, {"session", juce::String(invite.sessionId)}}))); }
        catch (const std::exception&) {}
    }

    void closeLan(const std::shared_ptr<Link>& peer, int code, const std::string& reason)
    {
        try
        {
            peer->ws->send(toJson(object({{"type", "closing"}, {"code", code}, {"reason", juce::String(reason)}})));
            peer->ws->close();
        }
        catch (const std::exception&) {}
    }

    void connectLan(const std::string& address, const std::string& ticket)
    {
        rtc::WebSocket::Configuration c;
        c.connectionTimeout = std::chrono::milliseconds(8000);
        auto socket = std::make_shared<rtc::WebSocket>(c);
        std::weak_ptr<Core> weak = shared_from_this();
        socket->onOpen([weak, ticket] {
            auto core = weak.lock();
            if (! core) return;
            try
            {
                core->ws->send(toJson(object({{"type", "hello"}, {"protocol", 1}, {"role", "guest"}, {"ticket", juce::String(ticket)},
                                              {"name", juce::String::fromUTF8(core->name.c_str())}})));
            }
            catch (const std::exception&) {}
        });
        socket->onMessage([](rtc::binary) {}, [weak](rtc::string text) {
            auto core = weak.lock();
            if (! core) return;
            const auto m = parseJson(text);
            const auto type = str(m["type"]);
            if (type == "pending") core->status("Waiting for the host to let you in...");
            else if (type == "welcome")
            {
                if (core->invite.sessionId.empty()) core->invite.sessionId = str(m["session"]);
                core->startGuestLink();
            }
            else if (type == "signal")
            {
                std::shared_ptr<Link> link;
                {
                    const std::lock_guard<std::mutex> l(core->lock);
                    if (auto it = core->links.find(hostPeer); it != core->links.end()) link = it->second;
                }
                if (link) core->receiveSignal(link, m["data"]);
            }
            else if (type == "closing")
            {
                core->closingCode = static_cast<int>(m["code"]);
                core->closingReason = str(m["reason"]);
            }
        });
        socket->onClosed([weak] {
            auto core = weak.lock();
            if (! core || core->shutdown) return;
            bool connected = false;
            {
                const std::lock_guard<std::mutex> l(core->lock);
                if (auto it = core->links.find(hostPeer); it != core->links.end()) connected = it->second->connected;
            }
            if (! connected)
                core->fail(core->closingReason.empty() ? std::string("Could not reach the host on the local network.") : core->closingReason);
        });
        ws = socket;
        try { socket->open("ws://" + address + "/signal"); }
        catch (const std::exception& e) { fail(std::string("Can't reach the host: ") + e.what()); }
    }
};

RtcTransport::~RtcTransport()
{
    core_->shutdown = true;
    if (core_->worker.joinable()) core_->worker.join();
    std::map<PeerId, std::shared_ptr<Link>> links;
    std::vector<std::shared_ptr<Link>> graveyard;
    {
        const std::lock_guard<std::mutex> l(core_->lock);
        links.swap(core_->links);
        graveyard.swap(core_->graveyard);
    }
    for (auto& [id, link] : links)
        if (link->pc) try { link->pc->close(); } catch (const std::exception&) {}
    for (auto& link : graveyard)
        if (link->pc) try { link->pc->close(); } catch (const std::exception&) {}
    try
    {
        if (core_->ws) core_->ws->close();
        if (core_->server) core_->server->stop();
    }
    catch (const std::exception&) {}
#if defined(__APPLE__)
    core_->advertiser.reset();
#endif
}

void RtcTransport::send(const PeerId& peer, Channel channel, std::string bytes)
{
    std::shared_ptr<rtc::DataChannel> dc;
    {
        const std::lock_guard<std::mutex> l(core_->lock);
        auto it = core_->links.find(peer);
        if (it == core_->links.end() || ! it->second->connected) return;
        dc = it->second->dc[static_cast<size_t>(channel)];
    }
    if (! dc) return;
    try
    {
        auto sendFrame = [&](uint8_t kind, const char* data, size_t size, const std::string& prefix = {}) {
            rtc::binary frame(1 + prefix.size() + size);
            frame[0] = static_cast<std::byte>(kind);
            if (! prefix.empty()) std::memcpy(frame.data() + 1, prefix.data(), prefix.size());
            if (size) std::memcpy(frame.data() + 1 + prefix.size(), data, size);
            dc->send(std::move(frame));
        };
        if (bytes.size() <= frameBytes || channel == Channel::Live)
        {
            if (bytes.size() > frameBytes) return; // live values are small; drop rather than fragment
            sendFrame(frameWhole, bytes.data(), bytes.size());
            return;
        }
        if (bytes.size() > maxMessageBytes) return;
        const auto total = static_cast<uint32_t>(bytes.size());
        std::string prefix(4, '\0');
        std::memcpy(prefix.data(), &total, 4);
        size_t offset = std::min(bytes.size(), frameBytes - 4);
        sendFrame(frameFirst, bytes.data(), offset, prefix);
        while (offset < bytes.size())
        {
            const size_t n = std::min(frameBytes, bytes.size() - offset);
            sendFrame(offset + n == bytes.size() ? frameLast : frameMiddle, bytes.data() + offset, n);
            offset += n;
        }
    }
    catch (const std::exception&) {}
}

size_t RtcTransport::buffered(const PeerId& peer, Channel channel) const
{
    const std::lock_guard<std::mutex> l(core_->lock);
    auto it = core_->links.find(peer);
    if (it == core_->links.end()) return 0;
    const auto& dc = it->second->dc[static_cast<size_t>(channel)];
    return dc ? dc->bufferedAmount() : 0;
}

void RtcTransport::disconnect(const PeerId& peer, const std::string& reason)
{
    std::shared_ptr<Link> link;
    {
        const std::lock_guard<std::mutex> l(core_->lock);
        if (auto it = core_->links.find(peer); it != core_->links.end()) link = it->second;
    }
    if (! link) return;
    if (core_->host && ! core_->lan && core_->ws && core_->ws->isOpen())
        try { core_->ws->send(toJson(object({{"type", "kick"}, {"peer", juce::String(link->remote)}}))); } catch (const std::exception&) {}
    if (core_->lan && core_->host && link->ws) core_->closeLan(link, 4003, reason);
    core_->markClosed(link, reason);
}

std::vector<TransportEvent> RtcTransport::poll()
{
    std::vector<std::shared_ptr<Link>> dead;
    std::vector<TransportEvent> out;
    bool reconnect = false;
    {
        const std::lock_guard<std::mutex> l(core_->lock);
        dead.swap(core_->graveyard);
        out.assign(std::make_move_iterator(core_->events.begin()), std::make_move_iterator(core_->events.end()));
        core_->events.clear();
        if (core_->reconnectAtMs != 0 && juce::Time::currentTimeMillis() >= core_->reconnectAtMs)
        {
            core_->reconnectAtMs = 0;
            reconnect = true;
        }
        if (! core_->host && core_->connectDeadlineMs != 0)
        {
            auto it = core_->links.find(hostPeer);
            if (it != core_->links.end() && it->second->connected) core_->connectDeadlineMs = 0;
            else if (juce::Time::currentTimeMillis() >= core_->connectDeadlineMs)
            {
                core_->connectDeadlineMs = 0;
                TransportEvent e;
                e.kind = TransportEvent::Kind::Failed;
                e.text = "Could not connect to the host. The invite may be damaged, or the network needs a TURN relay.";
                out.push_back(std::move(e));
            }
        }
    }
    for (auto& link : dead)
    {
        for (auto& dc : link->dc)
            if (dc) { dc->resetCallbacks(); try { dc->close(); } catch (const std::exception&) {} }
        if (link->pc) { link->pc->resetCallbacks(); try { link->pc->close(); } catch (const std::exception&) {} }
        if (link->ws) try { link->ws->close(); } catch (const std::exception&) {}
    }
    if (reconnect && ! core_->shutdown)
    {
        core_->closingCode = 0;
        core_->connectRelay();
    }
    return out;
}

void RtcTransport::approveJoin(const PeerId& peer, bool allow)
{
    std::shared_ptr<Link> link;
    {
        const std::lock_guard<std::mutex> l(core_->lock);
        if (auto it = core_->links.find(peer); it != core_->links.end()) link = it->second;
    }
    if (! link || link->approved) return;
    if (allow) core_->approve(link, std::string()); // no invite secret: the safety code is what proves the link
    else
    {
        core_->closeLan(link, 4401, "The host declined your request to join.");
        core_->markClosed(link, "declined");
    }
}

std::string RtcTransport::inviteLink() const
{
    const std::lock_guard<std::mutex> l(core_->lock);
    return core_->inviteText;
}
} // namespace

std::unique_ptr<Transport> openHost(const HostNetOptions& options)
{
    initLibrary();
    auto core = std::make_shared<RtcTransport::Core>();
    core->host = true;
    core->lan = options.mode == HostNetOptions::Mode::Lan;
    core->name = options.displayName;
    core->sessionName = options.sessionName;
    core->hostToken = options.hostToken;
    core->invite = makeInvite();
    auto transport = std::make_unique<RtcTransport>(core);
    if (core->lan)
    {
        core->invite.sessionId = crypto::base64Url(crypto::randomBytes(9));
        try { core->startLanServer(options.lanPort, options.displayName); }
        catch (const std::exception& e) { core->fail(std::string("Can't listen on the local network: ") + e.what()); }
        return transport;
    }
    auto relay = options.relay;
    while (! relay.empty() && relay.back() == '/') relay.pop_back();
    core->invite.relay = relay;
    core->status("Contacting the relay...");
    std::weak_ptr<RtcTransport::Core> weak = core;
    core->worker = std::thread([weak, relay, token = options.hostToken, ticket = core->invite.ticket] {
        const auto body = toJson(object({{"protocol", 1}, {"guestTicketSha256", juce::String(ticketHash(ticket))}, {"maxParticipants", 4}}));
        juce::String headers = "Content-Type: application/json\r\n";
        if (! token.empty()) headers << "Authorization: Bearer " << juce::String(token) << "\r\n";
        int status = 0;
        juce::StringPairArray responseHeaders;
        auto url = juce::URL(juce::String(relay) + "/v1/sessions").withPOSTData(juce::String::fromUTF8(body.c_str()));
        auto stream = url.createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
                                                .withExtraHeaders(headers)
                                                .withConnectionTimeoutMs(10000)
                                                .withStatusCode(&status)
                                                .withHttpRequestCmd("POST"));
        auto owner = weak.lock();
        if (! owner || owner->shutdown) return;
        if (! stream) { owner->fail("Can't reach the relay at " + relay + "."); return; }
        const auto response = parseJson(stream->readEntireStreamAsString().toStdString());
        if (status != 201)
        {
            const auto code = str(response["error"]["code"]);
            if (status == 402 || code == "subscription_required") owner->fail("Hosting on this relay needs an active Sonora Cloud subscription.");
            else if (status == 401) owner->fail("The relay did not accept your account. Sign in again or check the relay address.");
            else if (status == 429) owner->fail("You already host the maximum number of sessions on this relay.");
            else owner->fail("The relay refused to create a session (" + std::to_string(status) + " " + code + ").");
            return;
        }
        {
            const std::lock_guard<std::mutex> l(owner->lock);
            owner->invite.sessionId = str(response["sessionId"]);
            owner->hostKey = str(response["hostKey"]);
            owner->inviteText = formatInvite(owner->invite);
        }
        owner->status("Connecting to the relay...");
        owner->connectRelay();
    });
    return transport;
}

std::unique_ptr<Transport> openGuest(const Invite& invite, const std::string& name)
{
    initLibrary();
    auto core = std::make_shared<RtcTransport::Core>();
    core->host = false;
    core->lan = invite.lan();
    core->name = name;
    core->invite = invite;
    auto transport = std::make_unique<RtcTransport>(core);
    if (core->lan)
    {
        core->status("Connecting to the host...");
        core->connectLan(invite.lanAddress, invite.ticket);
    }
    else
    {
        core->status("Connecting to the relay...");
        core->connectRelay();
    }
    return transport;
}

std::unique_ptr<Transport> openGuestByAddress(const std::string& address, const std::string& name)
{
    initLibrary();
    auto core = std::make_shared<RtcTransport::Core>();
    core->host = false;
    core->lan = true;
    core->name = name;
    auto transport = std::make_unique<RtcTransport>(core);
    core->status("Asking the host to let you in...");
    core->connectLan(address, std::string());
    return transport;
}

#else
bool networkAvailable() { return false; }
std::unique_ptr<Transport> openHost(const HostNetOptions&) { return nullptr; }
std::unique_ptr<Transport> openGuest(const Invite&, const std::string&) { return nullptr; }
std::unique_ptr<Transport> openGuestByAddress(const std::string&, const std::string&) { return nullptr; }
#endif

} // namespace sonora::collab
