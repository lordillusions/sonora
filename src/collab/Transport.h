#pragma once
// COL-3: the peer links of one collab session, independent of how they are made.
//
// The host side holds one link per guest; a guest holds one link, to the host (peer id "host"). Each link has three
// channels: Ops (reliable, ordered: edits, snapshots, control), Live (unreliable, unordered: presence, live values,
// clock) and Assets (reliable, ordered, separate so big files never delay edits). Implementations deliver events to
// poll(), which the session calls on the message thread; send() may be called from the message thread only.
//
// RtcTransport (Net.h) uses WebRTC data channels; MemoryHub links sessions in one process for tests and probes.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sonora::collab
{

enum class Channel : uint8_t
{
    Ops = 0,
    Live = 1,
    Assets = 2,
};

using PeerId = std::string;
inline const PeerId hostPeer = "host";

struct TransportEvent
{
    enum class Kind
    {
        Connected,   // a link is open (guest side: to the host)
        Message,     // data on a channel
        Closed,      // a link closed; text says why
        JoinRequest, // LAN: someone without an invite asks to join (name in text); answer with approveJoin
        Status,      // progress text for the UI ("Connecting to relay...")
        Failed,      // the transport as a whole failed (text says why); the session ends or reconnects
    };
    Kind kind = Kind::Status;
    PeerId peer;
    Channel channel = Channel::Ops;
    std::string data;
    std::string text;
    std::string safetyCode; // Connected: four words both ends can compare ("" when the transport can't prove it)
};

class Transport
{
public:
    virtual ~Transport() = default;
    virtual void send(const PeerId& peer, Channel channel, std::string bytes) = 0;
    // Bytes queued but not yet sent on a channel (backpressure for assets).
    virtual size_t buffered(const PeerId& peer, Channel channel) const = 0;
    virtual void disconnect(const PeerId& peer, const std::string& reason) = 0;
    virtual std::vector<TransportEvent> poll() = 0;
    virtual void approveJoin(const PeerId& /*peer*/, bool /*allow*/) {}
    // The invite link guests use (host side; "" until the transport knows it).
    virtual std::string inviteLink() const { return {}; }
};

// In-process links for tests and probes: messages are delivered in order per channel at the next poll().
class MemoryHub : public std::enable_shared_from_this<MemoryHub>
{
public:
    static std::shared_ptr<MemoryHub> create() { return std::shared_ptr<MemoryHub>(new MemoryHub()); }
    std::unique_ptr<Transport> hostSide();
    // A guest that connects at once. The returned transport's peer for the host is hostPeer.
    std::unique_ptr<Transport> guestSide(const std::string& safetyCode = "test-safety-code");
    // Drops every message instead of delivering it while true (simulates a dead link).
    void setBlackhole(bool on);
    size_t delivered() const;

private:
    MemoryHub() = default;
    friend class MemoryTransport;
    struct Endpoint
    {
        std::deque<TransportEvent> inbox;
        bool open = true;
    };
    void push(const std::string& endpoint, TransportEvent event);
    std::vector<TransportEvent> drain(const std::string& endpoint);
    mutable std::mutex lock_;
    std::map<std::string, Endpoint> endpoints_; // "host" and "g<n>"
    int nextGuest_ = 1;
    bool blackhole_ = false;
    size_t delivered_ = 0;
};

} // namespace sonora::collab
