#include "collab/CollabSession.h"

#include "collab/Video.h"

#include "core/ModelCodec.h"
#include "core/Sha256.h"
#include "io/als/AlsReader.h"

#include <algorithm>
#include <cctype>

namespace sonora::collab
{
namespace
{
// COL-3c: the transport's send queue is first in, first out across channels, so asset bytes waiting there delay every
// edit behind them (1 MiB was seconds over a long path). Keep it short; the SCTP send buffer below it is shared fairly.
constexpr size_t maxAssetBacklog = 64u << 10;  // bytes queued on the assets channel before we wait
constexpr uint64_t requestWindow = 2u << 20;   // chunk requests in flight per guest, all downloads together (> rate x RTT)
constexpr size_t parallelDownloads = 8;        // small samples don't leave the window empty
constexpr int64_t clockPeriodMs = 2000;
constexpr int64_t presencePeriodMs = 50;
constexpr int64_t maxSourceFileBytes = 512ll << 20;
constexpr AuthorId maxAuthor = 11;            // clip UID ranges: 1e9 + (author - 1) * 1e8 stays below INT_MAX
constexpr size_t maxLiveValuesPerMessage = 400;
constexpr size_t chatKept = 500;              // lines each replica keeps
constexpr size_t chatHistorySent = 100;       // lines a joining guest gets
constexpr size_t chatBurst = 20;              // lines per person per chatWindowMs, more are refused
constexpr int64_t chatWindowMs = 10000;

int clipUidBase(AuthorId author) { return 1000000000 + static_cast<int>(author - 1) * 100000000; }

std::string cleanName(std::string name, AuthorId author)
{
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; }), name.end());
    while (! name.empty() && name.front() == ' ') name.erase(0, 1);
    while (! name.empty() && name.back() == ' ') name.pop_back();
    if (name.size() > maxNameLength)
    {
        name.resize(maxNameLength);
        while (! name.empty() && (static_cast<unsigned char>(name.back()) & 0xc0) == 0x80) name.pop_back(); // whole UTF-8 characters
        if (! name.empty() && (static_cast<unsigned char>(name.back()) & 0xc0) == 0xc0) name.pop_back();
    }
    return name.empty() ? "Guest " + std::to_string(author) : name;
}
} // namespace

int64_t CollabSession::now() { return juce::Time::currentTimeMillis(); }

CollabSession::CollabSession(Port& port, std::unique_ptr<Transport> transport, Options options, bool host)
    : port_(port), transport_(std::move(transport)), options_(std::move(options)), host_(host)
{
    options_.name = cleanName(options_.name, host ? hostAuthor : 0);
}

CollabSession::~CollabSession()
{
    *alive_ = false;
    pool_.removeAllJobs(true, 10000);
    if (resolverInstalled_) io::setSamplePathOverride(nullptr);
}

std::unique_ptr<CollabSession> CollabSession::host(Port& port, std::unique_ptr<Transport> transport, Options options)
{
    std::unique_ptr<CollabSession> s(new CollabSession(port, std::move(transport), std::move(options), true));
    s->self_ = hostAuthor;
    s->policy_ = s->options_.policy;
    s->hostFeatures_ = s->options_.features;
    s->features_[hostAuthor] = s->options_.features;
    Participant me;
    me.id = hostAuthor;
    me.name = s->options_.name;
    me.role = Role::Host;
    me.color = 0;
    s->participants_.push_back(me);
    if (auto* song = port.song()) s->sequencer_.reset(*song);
    s->status_ = "Preparing samples...";
    std::vector<model::FileRef> refs;
    if (auto* song = port.song()) refs = sampleRefs(*song);
    s->hashRefs(refs);
    return s;
}

std::unique_ptr<CollabSession> CollabSession::guest(Port& port, std::unique_ptr<Transport> transport, Options options,
                                                    std::function<std::unique_ptr<Transport>()> reconnect)
{
    std::unique_ptr<CollabSession> s(new CollabSession(port, std::move(transport), std::move(options), false));
    s->reconnect_ = std::move(reconnect);
    s->status_ = "Connecting...";
    return s;
}

// ------------------------------------------------------------------------------------------------ sending

WireDictionary* CollabSession::wireFor(const PeerId& peer)
{
    if (! host_) return hostWire_.get();
    auto* p = peerById(peer);
    return p ? p->wire.get() : nullptr;
}

void CollabSession::send(const PeerId& peer, Channel channel, const Message& message)
{
    if (! transport_) return;
    auto* wire = wireFor(peer);
    const auto before = wire ? wire->referencedBytes : 0;
    auto bytes = encodeMessage(message, wire);
    stats_.bytesOut += bytes.size();
    if (wire) stats_.referencedBytes += wire->referencedBytes - before;
    transport_->send(peer, channel, std::move(bytes));
}

void CollabSession::broadcast(Channel channel, const Message& message, const PeerId& except)
{
    if (! transport_) return;
    if (usesDictionary(message)) // each link has its own dictionary
    {
        for (const auto& [id, peer] : peers_)
            if (peer->welcomed && id != except) send(id, channel, message);
        return;
    }
    const auto bytes = encodeMessage(message);
    for (const auto& [id, peer] : peers_)
        if (peer->welcomed && id != except)
        {
            stats_.bytesOut += bytes.size();
            transport_->send(id, channel, bytes);
        }
}

CollabSession::Peer* CollabSession::peerByAuthor(AuthorId author)
{
    for (auto& [id, peer] : peers_)
        if (peer->welcomed && peer->author == author) return peer.get();
    return nullptr;
}

CollabSession::Peer* CollabSession::peerById(const PeerId& id)
{
    auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : it->second.get();
}

Applier CollabSession::applier()
{
    return [this](const std::vector<ObjectState>& states) { if (! states.empty()) port_.applyObjects(states); };
}

// ------------------------------------------------------------------------------------------------ polling

void CollabSession::poll()
{
    if (state_ == State::Ended) return;
    if (! host_ && state_ == State::Reconnecting && ! transport_ && now() >= reconnectAtMs_ && reconnect_)
    {
        ++reconnectAttempts_;
        status_ = "Reconnecting (attempt " + std::to_string(reconnectAttempts_) + ")...";
        transport_ = reconnect_();
        port_.changed();
    }
    if (transport_)
        for (auto& event : transport_->poll())
        {
            handle(event);
            if (state_ == State::Ended) return;
        }
    flushPresence();
    if (host_)
    {
        collectHashes();
        serveAssets();
    }
    else if (state_ == State::Active)
    {
        requestAssets();
        sendUploads();
        clockTick();
    }
}

void CollabSession::handle(const TransportEvent& e)
{
    using Kind = TransportEvent::Kind;
    switch (e.kind)
    {
        case Kind::Connected:
            if (host_)
            {
                auto peer = std::make_unique<Peer>();
                peer->peer = e.peer;
                peer->safetyCode = e.safetyCode;
                peer->connectedMs = now();
                peer->wire = std::make_unique<WireDictionary>(wirePool_);
                peers_[e.peer] = std::move(peer);
            }
            else
            {
                hostSafetyCode_ = e.safetyCode;
                hostWire_ = std::make_unique<WireDictionary>(wirePool_);
                mediaStateSent_ = false;
                status_ = "Joining...";
                HelloMsg hello;
                hello.appVersion = options_.appVersion;
                hello.name = options_.name;
                hello.features = options_.features;
                if (self_ != 0 && ! rejoinToken_.empty())
                {
                    hello.rejoinAs = self_;
                    hello.rejoinToken = rejoinToken_;
                    hello.lastSeq = client_.seq();
                }
                send(hostPeer, Channel::Ops, hello);
            }
            port_.changed();
            break;
        case Kind::Message:
        {
            stats_.bytesIn += e.data.size();
            if (e.channel == Channel::Media)
            {
                receiveMedia(e.peer, e.data); // RTP, not a protocol message
                break;
            }
            Message message;
            auto* wire = wireFor(e.peer);
            const auto before = wire ? wire->referencedBytes : 0;
            try { message = decodeMessage(e.data, wire); }
            catch (const model::CodecError& error)
            {
                ++stats_.dropped;
                if (host_) { if (transport_) transport_->disconnect(e.peer, std::string("protocol error: ") + error.what()); }
                else end(std::string("The host sent something this version of Sonora can't read (") + error.what() + ").");
                return;
            }
            if (wire) stats_.referencedBytes += wire->referencedBytes - before;
            if (host_)
            {
                if (auto* peer = peerById(e.peer)) handleHost(*peer, std::move(message));
            }
            else handleGuest(std::move(message));
            break;
        }
        case Kind::Closed:
            if (host_)
            {
                joinRequests_.erase(std::remove_if(joinRequests_.begin(), joinRequests_.end(), [&](const auto& r) { return r.first == e.peer; }),
                                    joinRequests_.end());
                auto it = peers_.find(e.peer);
                if (it == peers_.end()) { port_.changed(); break; }
                const AuthorId author = it->second->author;
                const bool welcomed = it->second->welcomed;
                if (it->second->upload) { it->second->upload.reset(); it->second->uploadFile.deleteFile(); }
                peers_.erase(it);
                awaitingSnapshot_.erase(std::remove(awaitingSnapshot_.begin(), awaitingSnapshot_.end(), e.peer), awaitingSnapshot_.end());
                if (welcomed && ! peerByAuthor(author))
                {
                    presence_.erase(author);
                    media_.erase(author);
                    for (auto p = participants_.begin(); p != participants_.end(); ++p)
                        if (p->id == author)
                        {
                            departed_[author] = *p;
                            port_.notify(p->name + " left the session");
                            participants_.erase(p);
                            break;
                        }
                    broadcastParticipants();
                }
                port_.changed();
            }
            else connectionLost(e.text.empty() ? "The connection to the host was lost." : e.text);
            break;
        case Kind::JoinRequest:
            if (host_)
            {
                joinRequests_.push_back({e.peer, e.text});
                port_.notify(e.text + " asks to join");
                port_.changed();
            }
            break;
        case Kind::Status:
            status_ = e.text;
            port_.changed();
            break;
        case Kind::Failed:
            if (host_) end(e.text);
            else connectionLost(e.text);
            break;
    }
}

// ------------------------------------------------------------------------------------------------ host

void CollabSession::handleHost(Peer& peer, Message&& message)
{
    if (! peer.welcomed)
    {
        if (auto* hello = std::get_if<HelloMsg>(&message)) welcome(peer, *hello);
        else if (transport_) transport_->disconnect(peer.peer, "expected hello");
        return;
    }
    auto* song = port_.song();
    const auto* who = participant(peer.author);
    const Role role = who ? who->role : Role::Listener;
    std::visit([&](auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, IntentMsg>)
        {
            ++stats_.intents;
            if (! song) return;
            if (role == Role::Listener)
            {
                RejectMsg r;
                r.intentId = m.intentId;
                r.code = "not_allowed";
                r.message = "You can listen but not edit in this session";
                hostReject(peer, r);
                return;
            }
            const auto allowed = referencedPaths(*song);
            auto result = sequencer_.receiveIntent(peer.author, m, *song, applier(), allowed);
            if (result.commit) sendCommit(*result.commit);
            if (result.reject) hostReject(peer, *result.reject);
        }
        else if constexpr (std::is_same_v<T, UndoMsg>)
        {
            if (! song || role == Role::Listener) return;
            auto result = m.redo ? sequencer_.redo(peer.author, *song, applier()) : sequencer_.undo(peer.author, *song, applier());
            if (result.commit) sendCommit(*result.commit);
            if (result.reject && result.reject->code != "nothing")
            {
                NoticeMsg n;
                n.text = conflictText(*result.reject);
                send(peer.peer, Channel::Ops, n);
            }
        }
        else if constexpr (std::is_same_v<T, PresenceMsg>)
        {
            m.author = peer.author;
            presence_[peer.author] = {m, now()};
            broadcast(Channel::Live, m, peer.peer);
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, LiveMsg>)
        {
            if (role == Role::Listener) return;
            m.author = peer.author;
            port_.applyLive(m.values);
            broadcast(Channel::Live, m, peer.peer);
        }
        else if constexpr (std::is_same_v<T, PluginStateMsg>)
        {
            if (role == Role::Listener) return;
            m.author = peer.author;
            port_.applyPluginState(m);
            broadcast(Channel::Ops, m, peer.peer);
        }
        else if constexpr (std::is_same_v<T, TransportMsg>)
        {
            m.author = peer.author;
            if (m.action == TransportMsg::Action::Play && m.beat < 0.0)
            {
                // A guest that just joined catches up with playback already running.
                const auto state = port_.transportState();
                if (! state.playing || ! song) return;
                TransportMsg join;
                join.action = TransportMsg::Action::Play;
                join.hostTimeMs = now() + options_.transportLeadMs;
                join.beat = state.beat + static_cast<double>(options_.transportLeadMs) / 60000.0 * song->tempo;
                join.author = hostAuthor;
                send(peer.peer, Channel::Ops, join);
                return;
            }
            scheduleTransport(m);
        }
        else if constexpr (std::is_same_v<T, ClockMsg>)
        {
            if (m.pong) return;
            ClockMsg pong;
            pong.pong = true;
            pong.sentMs = m.sentMs;
            pong.hostMs = now();
            send(peer.peer, Channel::Live, pong);
        }
        else if constexpr (std::is_same_v<T, AssetRequestMsg>)
        {
            if (peer.assetQueue.size() < 256) peer.assetQueue.push_back(m);
        }
        else if constexpr (std::is_same_v<T, UploadMsg>)
            receiveUpload(peer, m);
        else if constexpr (std::is_same_v<T, DropRequestMsg>)
            receiveDrop(peer, m);
        else if constexpr (std::is_same_v<T, ByeMsg>)
        {
            if (transport_) transport_->disconnect(peer.peer, m.reason.empty() ? "left" : m.reason);
        }
        else if constexpr (std::is_same_v<T, ChatMsg>)
        {
            // Only peers that announced chat send it; a host without chat reacts like an older Sonora.
            if ((options_.features & FeatureChat) && (peer.features & FeatureChat)) receiveChat(peer, m);
            else if (transport_) transport_->disconnect(peer.peer, "unexpected chat");
        }
        else if constexpr (std::is_same_v<T, MediaStateMsg>)
        {
            if ((options_.features & (FeatureVoice | FeatureVideo)) && (peer.features & (FeatureVoice | FeatureVideo)))
            {
                m.author = peer.author;
                receiveMediaState(m);
            }
            else if (transport_) transport_->disconnect(peer.peer, "unexpected media state");
        }
        else
        {
            if (transport_) transport_->disconnect(peer.peer, std::string("unexpected ") + messageName(message));
        }
    }, message);
}

void CollabSession::welcome(Peer& peer, const HelloMsg& hello)
{
    auto refuse = [&](const std::string& reason) {
        ByeMsg bye;
        bye.reason = reason;
        send(peer.peer, Channel::Ops, bye);
        if (transport_) transport_->disconnect(peer.peer, reason);
    };
    if (hello.protocol != protocolVersion)
        return refuse("This session needs a matching Sonora version (collab protocol " + std::to_string(protocolVersion)
                      + ", yours is " + std::to_string(hello.protocol) + ").");
    size_t connected = 0;
    for (const auto& [id, p] : peers_) connected += p->welcomed ? 1 : 0;
    if (connected + 1 >= options_.maxParticipants) return refuse("The session is full.");

    AuthorId author = 0;
    bool rejoin = false;
    if (hello.rejoinAs != 0)
        if (auto it = rejoinTokens_.find(hello.rejoinAs);
            it != rejoinTokens_.end() && crypto::equalBytes(it->second, hello.rejoinToken) && ! peerByAuthor(hello.rejoinAs))
        {
            author = hello.rejoinAs;
            rejoin = true;
        }
    if (author == 0)
    {
        if (nextAuthor_ > maxAuthor) return refuse("Too many people have joined this session; the host can start a new one.");
        author = nextAuthor_++;
    }
    peer.author = author;
    peer.welcomed = true;
    peer.features = hello.features;
    features_[author] = hello.features;
    peer.rejoinToken = crypto::base64Url(crypto::randomBytes(24));
    rejoinTokens_[author] = peer.rejoinToken;

    Participant p;
    p.id = author;
    p.name = cleanName(hello.name, author);
    p.role = Role::Editor;
    p.color = static_cast<uint8_t>(author % 8);
    if (auto d = departed_.find(author); d != departed_.end())
    {
        p.role = d->second.role;
        departed_.erase(d);
    }
    setParticipant(p);

    WelcomeMsg w;
    w.you = author;
    w.role = p.role;
    w.sessionName = options_.sessionName;
    w.participants = participants_;
    w.clipUidBase = clipUidBase(author);
    w.rejoinToken = peer.rejoinToken;
    std::optional<std::vector<CommitMsg>> missed;
    if (rejoin) missed = sequencer_.since(hello.lastSeq);
    w.snapshotFollows = ! missed.has_value();
    send(peer.peer, Channel::Ops, w);
    if (missed)
        for (const auto& c : *missed) send(peer.peer, Channel::Ops, c);
    else if (initialHashDone_) sendSnapshot(peer);
    else awaitingSnapshot_.push_back(peer.peer);
    broadcastParticipants();
    sendChatHistory(peer);
    sendMediaStates(peer);
    port_.notify(p.name + (rejoin ? " is back" : " joined the session"));
    port_.changed();
}

void CollabSession::sendSnapshot(Peer& peer)
{
    auto* song = port_.song();
    if (! song) return;
    port_.prepareSnapshot();
    auto snap = sequencer_.snapshot(*song);
    SnapshotMsg m;
    m.seq = snap.seq;
    m.objects = std::move(snap.objects);
    m.revisions = std::move(snap.revisions);
    m.document = model::codec::encodeDocument(*song);
    const juce::File source(juce::String::fromUTF8(song->sourcePath.c_str()));
    m.sourceName = source.getFileName().toStdString();
    if (source.existsAsFile() && source.getSize() <= maxSourceFileBytes)
    {
        juce::MemoryBlock block;
        if (source.loadFileAsData(block)) m.sourceFile.assign(static_cast<const char*>(block.getData()), block.getSize());
    }
    m.assets = catalog_->entries();
    m.transport = port_.transportState();
    send(peer.peer, Channel::Ops, m);
}

void CollabSession::setParticipant(const Participant& p)
{
    for (auto& existing : participants_)
        if (existing.id == p.id) { existing = p; return; }
    participants_.push_back(p);
}

void CollabSession::broadcastParticipants()
{
    ParticipantsMsg m;
    m.participants = participants_;
    broadcast(Channel::Ops, m);
    broadcastPolicy();
}

void CollabSession::broadcastFeature(uint32_t feature, Channel channel, const Message& message, const PeerId& except)
{
    if (! transport_) return;
    const auto bytes = encodeMessage(message);
    for (const auto& [id, peer] : peers_)
        if (peer->welcomed && (peer->features & feature) && id != except)
        {
            stats_.bytesOut += bytes.size();
            transport_->send(id, channel, bytes);
        }
}

void CollabSession::broadcastPolicy()
{
    if (! host_ || ! (options_.features & FeatureChat)) return; // an older host never sends one
    PolicyMsg m;
    m.hostFeatures = options_.features;
    m.policy = policy_;
    for (const auto& p : participants_) m.features.push_back({p.id, featuresOf(p.id)});
    broadcastFeature(FeatureChat, Channel::Ops, m);
}

void CollabSession::sendChatHistory(Peer& peer)
{
    if (! (options_.features & FeatureChat) || ! (peer.features & FeatureChat)) return;
    const size_t from = chat_.size() > chatHistorySent ? chat_.size() - chatHistorySent : 0;
    for (size_t i = from; i < chat_.size(); ++i)
    {
        const auto& e = chat_[i];
        if (e.id == 0) continue;
        ChatMsg m;
        m.author = e.author;
        m.id = e.id;
        m.timeMs = e.timeMs;
        m.history = true;
        m.text = e.text;
        send(peer.peer, Channel::Ops, m);
    }
}

void CollabSession::receiveChat(Peer& peer, const ChatMsg& m)
{
    auto refuse = [&](const std::string& why) {
        ChatMsg r; // id 0 tells the sender its line was not passed on
        r.author = peer.author;
        r.clientId = m.clientId;
        r.timeMs = now();
        r.text = why;
        send(peer.peer, Channel::Ops, r);
    };
    if (! policy_.chat) return refuse("The host turned chat off for this session.");
    const int64_t t = now();
    while (! peer.chatTimes.empty() && t - peer.chatTimes.front() > chatWindowMs) peer.chatTimes.pop_front();
    if (peer.chatTimes.size() >= chatBurst) return refuse("Too many messages at once; wait a moment.");
    const auto text = cleanChatText(m.text);
    if (text.empty()) return refuse("Empty message.");
    peer.chatTimes.push_back(t);
    ChatMsg out;
    out.author = peer.author;
    out.id = nextChatId_++;
    out.clientId = m.clientId;
    out.timeMs = t;
    out.text = text;
    addChat(out, true);
    broadcastFeature(FeatureChat, Channel::Ops, out);
}

void CollabSession::addChat(const ChatMsg& m, bool notify)
{
    ChatEntry e;
    e.author = m.author;
    if (const auto* p = participant(m.author))
    {
        e.name = p->name;
        e.color = p->color;
    }
    else
    {
        e.name = m.author == hostAuthor ? "Host" : "Guest " + std::to_string(m.author);
        e.color = static_cast<uint8_t>(m.author % 8);
    }
    e.id = m.id;
    e.clientId = m.clientId;
    e.timeMs = host_ ? m.timeMs : m.timeMs - clockOffset_;
    e.history = m.history;
    e.text = m.text;
    lastChatId_ = std::max(lastChatId_, m.id);
    chat_.push_back(std::move(e));
    while (chat_.size() > chatKept) chat_.pop_front();
    if (notify && m.author != self_ && ! m.history) port_.chatArrived(chat_.back());
    port_.changed();
}

void CollabSession::sendCommit(const CommitMsg& commit)
{
    ++stats_.commits;
    broadcast(Channel::Ops, commit);
    hashRefs(sampleRefs(commit.objects));
    port_.changed();
}

void CollabSession::hostReject(Peer& peer, const RejectMsg& reject)
{
    ++stats_.rejects;
    send(peer.peer, Channel::Ops, reject);
}

void CollabSession::hashRefs(const std::vector<model::FileRef>& refs)
{
    std::vector<std::pair<model::FileRef, std::string>> jobs;
    for (const auto& ref : refs)
        if (! catalog_->knows(ref)) jobs.push_back({ref, port_.resolveSample(ref)});
    const bool initial = ! initialHashDone_ && hashing_->total == 0;
    {
        const std::lock_guard<std::mutex> l(hashing_->lock);
        hashing_->total += jobs.size();
    }
    if (jobs.empty())
    {
        if (initial) collectHashes();
        return;
    }
    pool_.addJob([work = std::move(jobs), result = hashing_] {
        for (const auto& [ref, path] : work)
        {
            std::optional<AssetEntry> entry;
            if (! path.empty()) entry = hashAsset(ref, juce::File(juce::String::fromUTF8(path.c_str())));
            const std::lock_guard<std::mutex> l(result->lock);
            ++result->completed;
            if (entry) result->done.push_back({*entry, path});
        }
    });
}

void CollabSession::collectHashes()
{
    std::vector<std::pair<AssetEntry, std::string>> done;
    size_t total = 0, completed = 0;
    {
        const std::lock_guard<std::mutex> l(hashing_->lock);
        done.swap(hashing_->done);
        total = hashing_->total;
        completed = hashing_->completed;
    }
    AssetAddMsg added;
    for (const auto& [entry, path] : done)
        if (catalog_->add(entry, path)) added.assets.push_back(entry);
    if (! initialHashDone_)
    {
        progress_ = total == 0 ? -1.0f : static_cast<float>(completed) / static_cast<float>(total);
        if (completed < total) return;
        initialHashDone_ = true;
        state_ = State::Active;
        status_.clear();
        progress_ = -1.0f;
        for (const auto& id : awaitingSnapshot_)
            if (auto* peer = peerById(id)) sendSnapshot(*peer);
        awaitingSnapshot_.clear();
        port_.changed();
        return;
    }
    if (! added.assets.empty()) broadcast(Channel::Ops, added);
}

void CollabSession::serveAssets()
{
    for (auto& [id, peer] : peers_)
        while (! peer->assetQueue.empty() && transport_ && transport_->buffered(id, Channel::Assets) < maxAssetBacklog)
        {
            const auto request = peer->assetQueue.front();
            peer->assetQueue.pop_front();
            AssetChunkMsg chunk;
            chunk.hash = request.hash;
            chunk.offset = request.offset;
            const auto entry = catalog_->byHash(request.hash);
            const auto path = catalog_->localPath(request.hash);
            std::optional<std::string> bytes;
            if (entry && ! path.empty() && request.offset < entry->size)
                bytes = readAssetChunk(juce::File(juce::String::fromUTF8(path.c_str())), request.offset);
            if (! entry || ! bytes || bytes->empty()) chunk.missing = true;
            else
            {
                chunk.total = entry->size;
                chunk.bytes = std::move(*bytes);
            }
            send(id, Channel::Assets, chunk);
        }
}

void CollabSession::receiveUpload(Peer& peer, const UploadMsg& m)
{
    const auto* who = participant(peer.author);
    if (! who || who->role == Role::Listener) return;
    if (m.offset == 0)
    {
        if (peer.upload) { peer.upload.reset(); peer.uploadFile.deleteFile(); }
        if (m.total == 0 || m.total > maxAssetBytes || options_.projectDirectory == juce::File()) return;
        auto folder = options_.projectDirectory.getChildFile("Samples").getChildFile("Collab");
        folder.createDirectory();
        const auto name = juce::String::fromUTF8(safeFileName(m.name).c_str());
        peer.uploadFile = folder.getChildFile(name).getNonexistentSibling(false);
        peer.upload = std::make_unique<juce::FileOutputStream>(peer.uploadFile);
        if (! peer.upload->openedOk()) { peer.upload.reset(); return; }
        peer.uploadId = m.uploadId;
        peer.uploadTotal = m.total;
        peer.uploadWritten = 0;
    }
    if (! peer.upload || m.uploadId != peer.uploadId || m.offset != peer.uploadWritten || m.total != peer.uploadTotal
        || peer.uploadWritten + m.bytes.size() > peer.uploadTotal)
    {
        if (peer.upload) { peer.upload.reset(); peer.uploadFile.deleteFile(); }
        return;
    }
    peer.upload->write(m.bytes.data(), m.bytes.size());
    peer.uploadWritten += m.bytes.size();
    if (peer.uploadWritten == peer.uploadTotal) { peer.upload->flush(); peer.upload.reset(); }
}

void CollabSession::receiveDrop(Peer& peer, const DropRequestMsg& m)
{
    if (m.uploadId != peer.uploadId || peer.upload || peer.uploadWritten != peer.uploadTotal || ! peer.uploadFile.existsAsFile()) return;
    const auto* who = participant(peer.author);
    if (! who || who->role == Role::Listener) return;
    const auto label = port_.performDrop(peer.author, peer.uploadFile, m);
    if (label.empty())
    {
        NoticeMsg n;
        n.text = "The host could not add " + peer.uploadFile.getFileName().toStdString() + " to the set.";
        send(peer.peer, Channel::Ops, n);
    }
    peer.uploadId = 0;
}

// ------------------------------------------------------------------------------------------------ guest

void CollabSession::handleGuest(Message&& message)
{
    // Until the joined set is installed, edits and asset news wait (they apply to the snapshot's state).
    if (state_ == State::Joining && (std::holds_alternative<CommitMsg>(message) || std::holds_alternative<RejectMsg>(message)
                                     || std::holds_alternative<AssetAddMsg>(message) || std::holds_alternative<TransportMsg>(message)))
    {
        buffered_.push_back(std::move(message));
        return;
    }
    if ((! (options_.features & FeatureChat) && (std::holds_alternative<PolicyMsg>(message) || std::holds_alternative<ChatMsg>(message)))
        || (! (options_.features & (FeatureVoice | FeatureVideo)) && std::holds_alternative<MediaStateMsg>(message)))
    {
        // Emulating an older Sonora (tests, probes): its decoder fails on these types.
        end(std::string("The host sent something this version of Sonora can't read (unknown message type ") + messageName(message) + ").");
        return;
    }
    std::visit([&](auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, WelcomeMsg>)
        {
            const bool rejoin = self_ != 0 && self_ == m.you;
            self_ = m.you;
            client_.setSelf(m.you);
            rejoinToken_ = m.rejoinToken;
            participants_ = m.participants;
            model::reserveClipUidsFrom(m.clipUidBase);
            reconnectAttempts_ = 0;
            if (! m.snapshotFollows && rejoin)
            {
                if (auto* song = port_.song())
                    if (const size_t lost = client_.dropPending(*song, applier()); lost > 0)
                        port_.notify(std::to_string(lost) + (lost == 1 ? " edit" : " edits") + " made while reconnecting could not be sent");
                state_ = State::Active;
                status_.clear();
            }
            else
            {
                state_ = State::Joining;
                status_ = "Receiving the set...";
            }
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, SnapshotMsg>)
        {
            std::unique_ptr<model::Song> song;
            try
            {
                song = std::make_unique<model::Song>(songFromObjects(m.objects));
                model::codec::decodeDocument(m.document, *song);
            }
            catch (const model::CodecError& e)
            {
                end(std::string("The set from the host could not be read (") + e.what() + ").");
                return;
            }
            catalog_->clear();
            downloads_.clear();
            requested_.clear();
            wanted_.clear();
            downloadedBytes_ = downloadTotal_ = 0;
            for (const auto& entry : m.assets)
            {
                catalog_->add(entry);
                const auto cached = cachedAssetFile(entry.hash, entry.extension);
                if (cached.existsAsFile() && static_cast<uint64_t>(cached.getSize()) == entry.size)
                    catalog_->setLocalPath(entry.hash, cached.getFullPathName().toStdString());
            }
            for (const auto& hash : catalog_->missing())
            {
                wanted_.push_back(hash);
                if (auto e = catalog_->byHash(hash)) downloadTotal_ += e->size;
            }
            if (! resolverInstalled_)
            {
                auto catalog = catalog_;
                io::setSamplePathOverride(std::make_shared<const io::SamplePathOverride>(
                    [catalog](const model::Song&, const model::FileRef& ref) { return catalog->resolve(ref); }));
                resolverInstalled_ = true;
            }
            sourceFile_ = std::move(m.sourceFile);
            sourceName_ = m.sourceName;
            const auto transport = m.transport;
            snapshot_ = std::move(m);
            snapshot_->sourceFile.clear();
            state_ = State::Joining;
            status_ = "Opening the set...";
            port_.changed();
            joinPlayback_ = transport.playing;
            port_.installSong(std::move(song));
        }
        else if constexpr (std::is_same_v<T, CommitMsg>)
        {
            auto* song = port_.song();
            if (! song || m.seq <= client_.seq()) return;
            const bool own = m.author == self_ && m.intentId != 0;
            const size_t before = client_.pending();
            client_.receiveCommit(m, *song, applier());
            ++stats_.commits;
            const size_t expected = own && before > 0 ? before - 1 : before;
            if (client_.pending() < expected)
                port_.notify("Your edit was undone: " + displayName(m.author) + " changed the same thing first");
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, RejectMsg>)
        {
            auto* song = port_.song();
            if (! song) return;
            ++stats_.rejects;
            const auto label = client_.receiveReject(m, *song, applier());
            if (! label.empty()) port_.notify("\"" + label + "\" was undone: " + conflictText(m));
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, ParticipantsMsg>)
        {
            const Role before = role();
            participants_ = m.participants;
            for (auto it = presence_.begin(); it != presence_.end();)
                it = participant(it->first) ? std::next(it) : presence_.erase(it);
            for (auto it = media_.begin(); it != media_.end();)
                it = participant(it->first) ? std::next(it) : media_.erase(it);
            if (role() != before)
                port_.notify(role() == Role::Listener ? "The host made you a listener: you can follow along but not edit"
                                                      : "The host lets you edit now");
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, PresenceMsg>)
        {
            if (m.author == self_) return;
            presence_[m.author] = {m, now()};
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, LiveMsg>)
        {
            if (m.author != self_) port_.applyLive(m.values);
        }
        else if constexpr (std::is_same_v<T, PluginStateMsg>)
        {
            if (m.author != self_) port_.applyPluginState(m);
        }
        else if constexpr (std::is_same_v<T, TransportMsg>)
        {
            if (followTransport_) port_.applyTransport(m, m.hostTimeMs - clockOffset_);
        }
        else if constexpr (std::is_same_v<T, ClockMsg>)
        {
            if (! m.pong) return;
            const int64_t t = now();
            const int64_t rtt = t - m.sentMs;
            if (rtt < 0 || rtt > 30000) return;
            clockSamples_.push_back({rtt, m.hostMs - (m.sentMs + t) / 2});
            while (clockSamples_.size() > 8) clockSamples_.pop_front();
            const auto best = std::min_element(clockSamples_.begin(), clockSamples_.end());
            roundTrip_ = best->first;
            clockOffset_ = best->second;
        }
        else if constexpr (std::is_same_v<T, AssetChunkMsg>)
            receiveChunk(m);
        else if constexpr (std::is_same_v<T, AssetAddMsg>)
        {
            for (const auto& entry : m.assets)
                if (catalog_->add(entry))
                {
                    const auto cached = cachedAssetFile(entry.hash, entry.extension);
                    if (cached.existsAsFile() && static_cast<uint64_t>(cached.getSize()) == entry.size)
                    {
                        catalog_->setLocalPath(entry.hash, cached.getFullPathName().toStdString());
                        port_.assetsArrived({entry.hash});
                    }
                    else if (std::find(wanted_.begin(), wanted_.end(), entry.hash) == wanted_.end() && ! downloads_.count(entry.hash))
                    {
                        wanted_.push_back(entry.hash);
                        downloadTotal_ += entry.size;
                    }
                }
        }
        else if constexpr (std::is_same_v<T, NoticeMsg>)
            port_.notify(m.text);
        else if constexpr (std::is_same_v<T, PolicyMsg>)
        {
            const bool chatBefore = policy_.chat, voiceBefore = policy_.voice, videoBefore = policy_.video;
            hostFeatures_ = m.hostFeatures;
            policy_ = m.policy;
            features_.clear();
            for (const auto& [id, bits] : m.features)
                if (features_.size() < 64) features_[id] = bits;
            if (policyKnown_ && chatBefore != policy_.chat)
                port_.notify(policy_.chat ? "The host turned chat on" : "The host turned chat off");
            if (policyKnown_ && voiceBefore != policy_.voice && (options_.features & FeatureVoice))
                port_.notify(policy_.voice ? "The host turned voice on" : "The host turned voice off");
            if (policyKnown_ && videoBefore != policy_.video && (options_.features & FeatureVideo))
                port_.notify(policy_.video ? "The host turned the camera on" : "The host turned the camera off");
            policyKnown_ = true;
            if (! mediaStateSent_ && (hostFeatures_ & (FeatureVoice | FeatureVideo)) && (options_.features & (FeatureVoice | FeatureVideo)))
            {
                mediaStateSent_ = true; // a reconnect tells the host our switches again
                const auto mine = mediaState(self_);
                if (mine.micOn || mine.cameraOn) setMediaState(mine);
            }
            port_.changed();
        }
        else if constexpr (std::is_same_v<T, ChatMsg>)
            receiveChatAsGuest(m);
        else if constexpr (std::is_same_v<T, MediaStateMsg>)
        {
            if (m.author != self_) receiveMediaState(m);
        }
        else if constexpr (std::is_same_v<T, ByeMsg>)
        {
            reconnect_ = nullptr; // a deliberate goodbye: don't come back
            end(m.reason.empty() ? "The host ended the session." : m.reason);
        }
    }, message);
}

void CollabSession::receiveChatAsGuest(const ChatMsg& m)
{
    if (m.author == self_ && ! m.history)
        for (auto& e : chat_)
            if (e.id == 0 && e.failed.empty() && e.clientId == m.clientId)
            {
                // The host passed our line on (its order and time), or refused it (id 0, the reason as text).
                if (m.id == 0) e.failed = m.text.empty() ? std::string("not sent") : m.text;
                else
                {
                    e.id = m.id;
                    e.timeMs = m.timeMs - clockOffset_;
                    lastChatId_ = std::max(lastChatId_, m.id);
                }
                port_.changed();
                return;
            }
    if (m.id == 0) return;
    if (m.history && m.id <= lastChatId_) return; // already here (a reconnect resends the history)
    addChat(m, true);
}

void CollabSession::snapshotInstalled()
{
    auto* song = port_.song();
    if (! song || ! snapshot_) return;
    client_.reset(*song, snapshot_->seq, snapshot_->revisions);
    snapshot_->objects.clear();
    state_ = State::Active;
    status_.clear();
    nextClockMs_ = 0;
    port_.changed();
    if (joinPlayback_ && followTransport_)
    {
        // The host was playing: ask where it will be shortly, and start there.
        TransportMsg join;
        join.action = TransportMsg::Action::Play;
        join.beat = -1.0;
        send(hostPeer, Channel::Ops, join);
    }
    joinPlayback_ = false;
    auto queued = std::move(buffered_);
    buffered_.clear();
    for (auto& m : queued)
    {
        handleGuest(std::move(m));
        if (state_ == State::Ended) return;
    }
}

void CollabSession::requestAssets()
{
    uint64_t inFlight = 0;
    for (const auto& [hash, download] : downloads_)
        inFlight += std::min(requested_[hash], download->entry().size) - download->written();
    while (downloads_.size() < parallelDownloads && ! wanted_.empty() && inFlight < requestWindow)
    {
        const auto hash = wanted_.front();
        wanted_.pop_front();
        const auto entry = catalog_->byHash(hash);
        if (! entry || ! catalog_->localPath(hash).empty()) continue;
        auto download = std::make_unique<AssetDownload>(*entry, collabCacheDirectory());
        if (entry->size == 0)
        {
            std::string error;
            if (auto path = download->finish(error); ! path.empty())
            {
                catalog_->setLocalPath(hash, path);
                port_.assetsArrived({hash});
            }
            continue;
        }
        requested_[hash] = 0;
        downloads_[hash] = std::move(download);
    }
    for (auto& [hash, download] : downloads_)
    {
        auto& next = requested_[hash];
        while (next < download->entry().size && inFlight < requestWindow)
        {
            AssetRequestMsg r;
            r.hash = hash;
            r.offset = next;
            send(hostPeer, Channel::Assets, r);
            inFlight += std::min<uint64_t>(assetChunkBytes, download->entry().size - next);
            next += assetChunkBytes;
        }
    }
    const bool busy = ! downloads_.empty() || ! wanted_.empty();
    const float p = downloadTotal_ > 0 ? static_cast<float>(static_cast<double>(downloadedBytes_) / static_cast<double>(downloadTotal_)) : -1.0f;
    if (busy && (std::abs(p - progress_) > 0.01f || status_.empty()))
    {
        progress_ = p;
        status_ = "Downloading samples (" + std::to_string(downloadedBytes_ >> 20) + " of " + std::to_string(downloadTotal_ >> 20) + " MB)...";
        port_.changed();
    }
    else if (! busy && progress_ >= 0.0f)
    {
        progress_ = -1.0f;
        status_.clear();
        port_.changed();
    }
}

void CollabSession::receiveChunk(const AssetChunkMsg& chunk)
{
    auto it = downloads_.find(chunk.hash);
    if (it == downloads_.end()) return;
    if (chunk.missing)
    {
        port_.notify("The host can't read one of the samples; it stays silent here");
        requested_.erase(chunk.hash);
        downloads_.erase(it);
        return;
    }
    if (! it->second->append(chunk))
    {
        requested_.erase(chunk.hash);
        downloads_.erase(it);
        return;
    }
    downloadedBytes_ += chunk.bytes.size();
    if (! it->second->complete()) return;
    std::string error;
    const auto path = it->second->finish(error);
    requested_.erase(chunk.hash);
    downloads_.erase(it);
    if (path.empty())
    {
        port_.notify("A sample from the host arrived damaged (" + error + ")");
        return;
    }
    catalog_->setLocalPath(chunk.hash, path);
    port_.assetsArrived({chunk.hash});
}

void CollabSession::sendUploads()
{
    while (! uploads_.empty() && transport_ && transport_->buffered(hostPeer, Channel::Assets) < maxAssetBacklog)
    {
        auto& u = uploads_.front();
        if (u.offset >= u.total)
        {
            send(hostPeer, Channel::Assets, u.drop);
            uploads_.pop_front();
            continue;
        }
        auto bytes = readAssetChunk(u.file, u.offset);
        if (! bytes || bytes->empty())
        {
            port_.notify("Could not read " + u.file.getFileName().toStdString());
            uploads_.pop_front();
            continue;
        }
        UploadMsg m;
        m.uploadId = u.id;
        m.name = u.file.getFileName().toStdString();
        m.offset = u.offset;
        m.total = u.total;
        u.offset += bytes->size();
        m.bytes = std::move(*bytes);
        send(hostPeer, Channel::Assets, m);
    }
}

void CollabSession::clockTick()
{
    const int64_t t = now();
    if (t < nextClockMs_) return;
    ClockMsg ping;
    ping.sentMs = t;
    send(hostPeer, Channel::Live, ping);
    ++clockPings_;
    nextClockMs_ = t + (clockPings_ < 6 ? 200 : clockPeriodMs);
}

void CollabSession::connectionLost(const std::string& reason)
{
    if (state_ == State::Ended) return;
    transport_.reset();
    if (reconnect_ && state_ == State::Active && reconnectAttempts_ < 6)
    {
        state_ = State::Reconnecting;
        reconnectAtMs_ = now() + (int64_t(1000) << std::min(reconnectAttempts_, 4));
        status_ = "Connection lost. Reconnecting...";
        port_.notify("Connection to the host lost; reconnecting");
        port_.changed();
        return;
    }
    if (reconnect_ && state_ == State::Reconnecting && reconnectAttempts_ < 6)
    {
        reconnectAtMs_ = now() + (int64_t(1000) << std::min(reconnectAttempts_, 4));
        return;
    }
    end(reason);
}

// ------------------------------------------------------------------------------------------------ app actions

void CollabSession::localEdit(const std::string& label) { localEditBy(self_, label); }

void CollabSession::localEditBy(AuthorId author, const std::string& label)
{
    auto* song = port_.song();
    if (! song || state_ == State::Ended) return;
    if (host_)
    {
        if (auto commit = sequencer_.captureLocal(*song, label, author)) sendCommit(*commit);
        return;
    }
    if (state_ != State::Active && state_ != State::Reconnecting) return;
    if (auto intent = client_.captureLocal(*song, label))
    {
        ++stats_.intents;
        if (transport_ && state_ == State::Active) send(hostPeer, Channel::Ops, *intent);
    }
}

bool CollabSession::undo()
{
    auto* song = port_.song();
    if (! song || state_ != State::Active) return false;
    if (! host_)
    {
        UndoMsg m;
        send(hostPeer, Channel::Ops, m);
        return true;
    }
    auto result = sequencer_.undo(hostAuthor, *song, applier());
    if (result.commit) sendCommit(*result.commit);
    if (result.reject && result.reject->code != "nothing") port_.notify(conflictText(*result.reject));
    return result.commit.has_value();
}

bool CollabSession::redo()
{
    auto* song = port_.song();
    if (! song || state_ != State::Active) return false;
    if (! host_)
    {
        UndoMsg m;
        m.redo = true;
        send(hostPeer, Channel::Ops, m);
        return true;
    }
    auto result = sequencer_.redo(hostAuthor, *song, applier());
    if (result.commit) sendCommit(*result.commit);
    if (result.reject && result.reject->code != "nothing") port_.notify(conflictText(*result.reject));
    return result.commit.has_value();
}

std::string CollabSession::undoLabel() const { return host_ ? sequencer_.undoLabel(hostAuthor) : std::string(); }
std::string CollabSession::redoLabel() const { return host_ ? sequencer_.redoLabel(hostAuthor) : std::string(); }

void CollabSession::sendPresence(PresenceMsg p)
{
    if (state_ != State::Active) return;
    p.author = self_;
    pendingPresence_ = p; // at most 20 per second; the latest always goes out (flushPresence)
    flushPresence();
}

void CollabSession::flushPresence()
{
    if (! pendingPresence_ || state_ != State::Active) return;
    const int64_t t = now();
    if (t - lastPresenceMs_ < presencePeriodMs) return;
    lastPresenceMs_ = t;
    if (host_) broadcast(Channel::Live, *pendingPresence_);
    else send(hostPeer, Channel::Live, *pendingPresence_);
    pendingPresence_.reset();
}

void CollabSession::sendLive(std::vector<LiveValue> values)
{
    if (state_ != State::Active || values.empty() || role() == Role::Listener) return;
    for (size_t start = 0; start < values.size(); start += maxLiveValuesPerMessage)
    {
        LiveMsg m;
        m.author = self_;
        m.values.assign(values.begin() + static_cast<std::ptrdiff_t>(start),
                        values.begin() + static_cast<std::ptrdiff_t>(std::min(values.size(), start + maxLiveValuesPerMessage)));
        if (host_) broadcast(Channel::Live, m);
        else send(hostPeer, Channel::Live, m);
    }
}

void CollabSession::sendPluginState(PluginStateMsg m)
{
    if (state_ != State::Active || role() == Role::Listener) return;
    m.author = self_;
    if (host_) broadcast(Channel::Ops, m);
    else send(hostPeer, Channel::Ops, m);
}

void CollabSession::requestTransport(TransportMsg::Action action, double beat)
{
    TransportMsg m;
    m.action = action;
    m.beat = beat;
    m.author = self_;
    if (host_) { scheduleTransport(m); return; }
    if (! followTransport_ || state_ != State::Active) { port_.applyTransport(m, now()); return; }
    send(hostPeer, Channel::Ops, m);
}

void CollabSession::scheduleTransport(TransportMsg m)
{
    m.hostTimeMs = m.action == TransportMsg::Action::Stop ? now() : now() + options_.transportLeadMs;
    broadcast(Channel::Ops, m);
    port_.applyTransport(m, m.hostTimeMs);
}

bool CollabSession::uploadAndDrop(const juce::File& file, int trackId, double beat, int scene)
{
    if (host_ || ! canEdit() || ! file.existsAsFile()) return false;
    const auto size = static_cast<uint64_t>(file.getSize());
    if (size == 0 || size > maxAssetBytes) return false;
    Upload u;
    u.id = nextUploadId_++;
    u.file = file;
    u.total = size;
    u.drop.uploadId = u.id;
    u.drop.trackId = trackId;
    u.drop.beat = beat;
    u.drop.scene = scene;
    uploads_.push_back(std::move(u));
    port_.notify("Sending " + file.getFileName().toStdString() + " to the host...");
    return true;
}

std::string CollabSession::sendChat(const std::string& raw)
{
    if (const auto why = chatUnavailableReason(); ! why.empty()) return why;
    const auto text = cleanChatText(raw);
    if (text.empty()) return "Nothing to send.";
    ChatMsg m;
    m.author = self_;
    m.clientId = nextClientChatId_++;
    m.timeMs = now();
    m.text = text;
    if (host_)
    {
        m.id = nextChatId_++;
        addChat(m, false);
        broadcastFeature(FeatureChat, Channel::Ops, m);
        return {};
    }
    ChatEntry e;
    e.author = self_;
    e.name = options_.name;
    if (const auto* p = participant(self_)) e.color = p->color;
    e.clientId = m.clientId;
    e.timeMs = m.timeMs;
    e.text = text;
    chat_.push_back(std::move(e));
    while (chat_.size() > chatKept) chat_.pop_front();
    send(hostPeer, Channel::Ops, m);
    port_.changed();
    return {};
}

std::string CollabSession::chatUnavailableReason() const
{
    if (! (options_.features & FeatureChat)) return "This build has no chat.";
    if (state_ == State::Ended) return "The session has ended.";
    if (state_ == State::Reconnecting) return "Reconnecting...";
    if (! host_ && (state_ == State::Starting || ! policyKnown_))
        return state_ == State::Starting || state_ == State::Joining ? "Connecting..." : "The host's Sonora is older and has no chat.";
    if (! (hostFeatures_ & FeatureChat)) return "The host's Sonora is older and has no chat.";
    if (! policy_.chat) return host_ ? "Chat is off for this session." : "The host turned chat off for this session.";
    return {};
}

void CollabSession::setPolicy(const SessionPolicy& p)
{
    if (! host_ || p == policy_) return;
    policy_ = p;
    broadcastPolicy();
    port_.changed();
}

// ------------------------------------------------------------------------------------------------ voice (COL-7)

void CollabSession::receiveMedia(const PeerId& from, const std::string& bytes)
{
    // Voice (PT 111) or video (PT 96); the SSRC names the speaker.
    const auto packet = decodeRtp(bytes);
    const bool video = packet && packet->payloadType == videoPayloadType;
    const uint32_t feature = video ? FeatureVideo : FeatureVoice;
    auto& in = video ? stats_.videoIn : stats_.voiceIn;
    auto& dropped = video ? stats_.videoDropped : stats_.voiceDropped;
    auto& forwarded = video ? stats_.videoForwarded : stats_.voiceForwarded;
    ++in;
    std::optional<AuthorId> speaker;
    if (packet && (options_.features & feature))
        speaker = video ? videoSpeaker(packet->ssrc) : packet->payloadType == voicePayloadType ? voiceSpeaker(packet->ssrc) : std::nullopt;
    if (! speaker || ! (video ? policy_.video : policy_.voice) || state_ != State::Active || (video && transport_ && transport_->relayed(from)))
    {
        ++dropped;
        return;
    }
    if (host_)
    {
        // A guest speaks only for itself; its packets go unchanged to everyone else with the feature (video: not over TURN).
        const auto* peer = peerById(from);
        if (! peer || ! peer->welcomed || ! (peer->features & feature) || *speaker != peer->author) { ++dropped; return; }
        for (const auto& [id, other] : peers_)
            if (id != from && other->welcomed && (other->features & feature) && transport_ && ! (video && transport_->relayed(id)))
            {
                transport_->send(id, Channel::Media, bytes);
                ++forwarded;
            }
    }
    else if (*speaker == self_ || ! participant(*speaker)) { ++dropped; return; }
    if (video) port_.videoArrived(*speaker, bytes);
    else port_.voiceArrived(*speaker, bytes);
}

void CollabSession::sendVideo(const std::string& rtp)
{
    if (! transport_ || ! videoAvailable()) return;
    stats_.bytesOut += rtp.size();
    if (! host_)
    {
        transport_->send(hostPeer, Channel::Media, rtp);
        return;
    }
    for (const auto& [id, peer] : peers_)
        if (peer->welcomed && (peer->features & FeatureVideo) && ! transport_->relayed(id)) transport_->send(id, Channel::Media, rtp);
}

std::string CollabSession::videoUnavailableReason() const
{
    if (! (options_.features & FeatureVideo)) return "This build has no camera support (macOS only for now).";
    if (state_ == State::Ended) return "The session has ended.";
    if (state_ == State::Reconnecting) return "Reconnecting...";
    if (! host_ && (state_ == State::Starting || state_ == State::Joining)) return "Connecting...";
    if (! host_ && (! policyKnown_ || ! (hostFeatures_ & FeatureVideo))) return "The host's Sonora has no camera support.";
    if (! policy_.video) return host_ ? "Camera is off for this session." : "The host turned the camera off for this session.";
    if (! host_ && transport_ && ! transport_->mediaReady(hostPeer)) return "No media connection to the host (the network may block it).";
    if (! host_ && transport_ && transport_->relayed(hostPeer))
        return "Camera needs a direct connection; it stays off through the sharing server's relay.";
    return {};
}

void CollabSession::sendVoice(const std::string& rtp)
{
    if (! transport_ || ! voiceAvailable()) return;
    stats_.bytesOut += rtp.size();
    if (! host_)
    {
        transport_->send(hostPeer, Channel::Media, rtp);
        return;
    }
    for (const auto& [id, peer] : peers_)
        if (peer->welcomed && (peer->features & FeatureVoice)) transport_->send(id, Channel::Media, rtp);
}

std::string CollabSession::voiceUnavailableReason() const
{
    if (! (options_.features & FeatureVoice)) return "This build has no voice.";
    if (state_ == State::Ended) return "The session has ended.";
    if (state_ == State::Reconnecting) return "Reconnecting...";
    if (! host_ && (state_ == State::Starting || state_ == State::Joining)) return "Connecting...";
    if (! host_ && (! policyKnown_ || ! (hostFeatures_ & FeatureVoice))) return "The host's Sonora is older and has no voice.";
    if (! policy_.voice) return host_ ? "Voice is off for this session." : "The host turned voice off for this session.";
    if (! host_ && transport_ && ! transport_->mediaReady(hostPeer)) return "No audio connection to the host (the network may block it).";
    return {};
}

void CollabSession::setMediaState(MediaState state)
{
    media_[self_] = state;
    if (state_ == State::Ended || ! (options_.features & (FeatureVoice | FeatureVideo))) return;
    MediaStateMsg m;
    m.author = self_;
    m.micOn = state.micOn;
    m.cameraOn = state.cameraOn;
    if (host_) broadcastFeature(FeatureVoice | FeatureVideo, Channel::Ops, m);
    else if (transport_ && state_ == State::Active && (hostFeatures_ & (FeatureVoice | FeatureVideo))) send(hostPeer, Channel::Ops, m);
    port_.changed();
}

CollabSession::MediaState CollabSession::mediaState(AuthorId id) const
{
    const auto it = media_.find(id);
    return it == media_.end() ? MediaState{} : it->second;
}

void CollabSession::receiveMediaState(MediaStateMsg m)
{
    if (! participant(m.author)) return;
    media_[m.author] = {m.micOn, m.cameraOn};
    if (host_) broadcastFeature(FeatureVoice | FeatureVideo, Channel::Ops, m);
    port_.changed();
}

void CollabSession::sendMediaStates(Peer& peer)
{
    if (! (options_.features & (FeatureVoice | FeatureVideo)) || ! (peer.features & (FeatureVoice | FeatureVideo))) return;
    for (const auto& [id, state] : media_)
    {
        if (id == peer.author || ! participant(id)) continue;
        MediaStateMsg m;
        m.author = id;
        m.micOn = state.micOn;
        m.cameraOn = state.cameraOn;
        send(peer.peer, Channel::Ops, m);
    }
}

uint32_t CollabSession::featuresOf(AuthorId id) const
{
    if (id == self_) return options_.features;
    const auto it = features_.find(id);
    return it == features_.end() ? 0 : it->second;
}

void CollabSession::setRole(AuthorId who, Role r)
{
    if (! host_ || who == hostAuthor || r == Role::Host) return;
    for (auto& p : participants_)
        if (p.id == who) { p.role = r; break; }
    broadcastParticipants();
    port_.changed();
}

void CollabSession::kick(AuthorId who)
{
    if (! host_ || who == hostAuthor) return;
    if (auto* peer = peerByAuthor(who))
    {
        ByeMsg bye;
        bye.reason = "The host removed you from the session.";
        send(peer->peer, Channel::Ops, bye);
        rejoinTokens_.erase(who);
        if (transport_) transport_->disconnect(peer->peer, "removed by the host");
    }
}

void CollabSession::approveJoin(const PeerId& peer, bool allow)
{
    joinRequests_.erase(std::remove_if(joinRequests_.begin(), joinRequests_.end(), [&](const auto& r) { return r.first == peer; }),
                        joinRequests_.end());
    if (transport_) transport_->approveJoin(peer, allow);
    port_.changed();
}

void CollabSession::leave(const std::string& reason)
{
    if (state_ == State::Ended) return;
    ByeMsg bye;
    bye.reason = host_ ? "The host ended the session." : options_.name + " " + reason;
    if (host_) broadcast(Channel::Ops, bye);
    else if (state_ == State::Active || state_ == State::Joining) send(hostPeer, Channel::Ops, bye);
    reconnect_ = nullptr;
    end(host_ ? "You ended the session." : "You left the session.");
}

void CollabSession::end(const std::string& reason)
{
    if (state_ == State::Ended) return;
    state_ = State::Ended;
    status_ = reason;
    progress_ = -1.0f;
    uploads_.clear();
    downloads_.clear();
    transport_.reset();
    if (resolverInstalled_)
    {
        io::setSamplePathOverride(nullptr);
        resolverInstalled_ = false;
    }
    if (! endedNotified_)
    {
        endedNotified_ = true;
        port_.ended(reason);
    }
}

// ------------------------------------------------------------------------------------------------ queries

Role CollabSession::role() const
{
    if (host_) return Role::Host;
    if (const auto* p = participant(self_)) return p->role;
    return Role::Listener;
}

const Participant* CollabSession::participant(AuthorId id) const
{
    for (const auto& p : participants_)
        if (p.id == id) return &p;
    return nullptr;
}

std::string CollabSession::displayName(AuthorId id) const
{
    if (id == self_) return "you";
    if (const auto* p = participant(id)) return p->name;
    return id == hostAuthor ? "the host" : "someone";
}

std::string CollabSession::inviteLink() const { return host_ && transport_ ? transport_->inviteLink() : std::string(); }

std::string CollabSession::safetyCode(AuthorId author) const
{
    if (! host_) return hostSafetyCode_;
    for (const auto& [id, peer] : peers_)
        if (peer->welcomed && peer->author == author) return peer->safetyCode;
    return {};
}

uint64_t CollabSession::seq() const { return host_ ? sequencer_.seq() : client_.seq(); }

std::string CollabSession::conflictText(const RejectMsg& r) const
{
    if (r.code == "conflict" && r.hasConflictAuthor)
    {
        const auto name = displayName(r.conflictAuthor);
        return (name == "you" ? std::string("you") : name) + " changed it first";
    }
    if (r.code == "depends_on_rejected") return "it built on an edit that was not accepted";
    return r.message.empty() ? std::string("the host did not accept it") : r.message;
}

} // namespace sonora::collab
