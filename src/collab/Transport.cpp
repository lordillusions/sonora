#include "collab/Transport.h"

namespace sonora::collab
{

class MemoryTransport : public Transport
{
public:
    MemoryTransport(std::shared_ptr<MemoryHub> hub, std::string self) : hub_(std::move(hub)), self_(std::move(self)) {}
    ~MemoryTransport() override
    {
        const bool host = self_ == hostPeer;
        std::vector<std::string> others;
        {
            const std::lock_guard<std::mutex> l(hub_->lock_);
            hub_->endpoints_[self_].open = false;
            for (const auto& [name, endpoint] : hub_->endpoints_)
                if (name != self_ && endpoint.open && (host || name == hostPeer)) others.push_back(name);
        }
        for (const auto& other : others)
        {
            TransportEvent e;
            e.kind = TransportEvent::Kind::Closed;
            e.peer = host ? hostPeer : self_;
            e.text = "left";
            hub_->push(other, std::move(e));
        }
    }

    void send(const PeerId& peer, Channel channel, std::string bytes) override
    {
        TransportEvent e;
        e.kind = TransportEvent::Kind::Message;
        e.peer = self_ == hostPeer ? hostPeer : self_;
        e.channel = channel;
        e.data = std::move(bytes);
        hub_->push(self_ == hostPeer ? peer : hostPeer, std::move(e));
    }
    size_t buffered(const PeerId&, Channel) const override { return 0; }
    void disconnect(const PeerId& peer, const std::string& reason) override
    {
        TransportEvent e;
        e.kind = TransportEvent::Kind::Closed;
        e.peer = self_ == hostPeer ? hostPeer : self_;
        e.text = reason;
        const std::string target = self_ == hostPeer ? peer : hostPeer;
        hub_->push(target, e);
        TransportEvent mine;
        mine.kind = TransportEvent::Kind::Closed;
        mine.peer = peer;
        mine.text = reason;
        hub_->push(self_, std::move(mine));
        const std::lock_guard<std::mutex> l(hub_->lock_);
        if (self_ != hostPeer) hub_->endpoints_[self_].open = false;
        else hub_->endpoints_[peer].open = false;
    }
    std::vector<TransportEvent> poll() override { return hub_->drain(self_); }
    std::string inviteLink() const override { return "sonora://join/memory?lan=127.0.0.1%3A1&ticket=memory#AAAAAAAAAAAAAAAAAAAAAA"; }

private:
    std::shared_ptr<MemoryHub> hub_;
    std::string self_;
};

std::unique_ptr<Transport> MemoryHub::hostSide()
{
    {
        const std::lock_guard<std::mutex> l(lock_);
        endpoints_[hostPeer] = {};
    }
    return std::make_unique<MemoryTransport>(shared_from_this(), hostPeer);
}

std::unique_ptr<Transport> MemoryHub::guestSide(const std::string& safetyCode)
{
    std::string name;
    {
        const std::lock_guard<std::mutex> l(lock_);
        name = "g" + std::to_string(nextGuest_++);
        endpoints_[name] = {};
    }
    TransportEvent toHost;
    toHost.kind = TransportEvent::Kind::Connected;
    toHost.peer = name;
    toHost.safetyCode = safetyCode;
    push(hostPeer, std::move(toHost));
    TransportEvent toGuest;
    toGuest.kind = TransportEvent::Kind::Connected;
    toGuest.peer = hostPeer;
    toGuest.safetyCode = safetyCode;
    push(name, std::move(toGuest));
    return std::make_unique<MemoryTransport>(shared_from_this(), name);
}

void MemoryHub::setBlackhole(bool on)
{
    const std::lock_guard<std::mutex> l(lock_);
    blackhole_ = on;
}

size_t MemoryHub::delivered() const
{
    const std::lock_guard<std::mutex> l(lock_);
    return delivered_;
}

void MemoryHub::push(const std::string& endpoint, TransportEvent event)
{
    const std::lock_guard<std::mutex> l(lock_);
    if (blackhole_ && event.kind == TransportEvent::Kind::Message) return;
    auto it = endpoints_.find(endpoint);
    if (it == endpoints_.end() || ! it->second.open) return;
    it->second.inbox.push_back(std::move(event));
}

std::vector<TransportEvent> MemoryHub::drain(const std::string& endpoint)
{
    const std::lock_guard<std::mutex> l(lock_);
    auto it = endpoints_.find(endpoint);
    if (it == endpoints_.end()) return {};
    std::vector<TransportEvent> out(std::make_move_iterator(it->second.inbox.begin()), std::make_move_iterator(it->second.inbox.end()));
    it->second.inbox.clear();
    delivered_ += out.size();
    return out;
}

} // namespace sonora::collab
