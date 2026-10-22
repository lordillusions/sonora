#pragma once
// COL-2..6: one collab session as seen from this replica (host or guest), on the message thread.
//
// CollabSession runs the protocol: participants and roles, the join snapshot, edits (Sequencer on the host, Client on a
// guest), per-user undo, asset sync, presence, live values, plugin state and the shared transport with clock sync.
// Everything that touches the engine, the UI or the open document goes through Port, which the app implements
// (AppCollab.cpp) and tests/probes implement on a bare Song.

#include "collab/Assets.h"
#include "collab/Protocol.h"
#include "collab/Transport.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace sonora::collab
{

class CollabSession
{
public:
    class Port
    {
    public:
        virtual ~Port() = default;
        virtual model::Song* song() = 0;
        // Apply object states to the open song and refresh engine and UI. Remote changes never enter local history.
        virtual void applyObjects(const std::vector<ObjectState>&) = 0;
        // Host: bring live values (mixer, device parameters, plugin state) into the model before a snapshot.
        virtual void prepareSnapshot() {}
        // Host: where a FileRef lives on this machine ("" when missing).
        virtual std::string resolveSample(const model::FileRef&) = 0;
        // Guest: replace the open document with the joined set. Call CollabSession::snapshotInstalled() when done.
        virtual void installSong(std::unique_ptr<model::Song>) = 0;
        // Guest: samples with these hashes are now on disk; reload what uses them.
        virtual void assetsArrived(const std::vector<std::string>& /*hashes*/) {}
        virtual void applyLive(const std::vector<LiveValue>&) {}
        virtual void applyPluginState(const PluginStateMsg&) {}
        // Play / stop / seek at `localTimeMs` (this machine's juce::Time::currentTimeMillis clock).
        virtual void applyTransport(const TransportMsg&, int64_t /*localTimeMs*/) {}
        virtual TransportState transportState() { return {}; }
        // Host: perform a guest's file drop (the file is already in the project folder). Return a label or "" on failure.
        virtual std::string performDrop(AuthorId /*author*/, const juce::File& /*file*/, const DropRequestMsg&) { return {}; }
        // UI: participants, presence, status or progress changed.
        virtual void changed() {}
        // A short message for the user ("Ana's edit was undone: you changed that clip first").
        virtual void notify(const std::string& /*text*/) {}
        // The session ended (left, kicked, host ended it, connection lost for good).
        virtual void ended(const std::string& /*reason*/) {}
    };

    struct Options
    {
        std::string name = "Host";          // this person's display name
        std::string sessionName;            // host: shown to guests ("Song.als")
        std::string appVersion;
        size_t maxParticipants = 4;
        int64_t transportLeadMs = 150;      // scheduled starts happen this far ahead
        juce::File projectDirectory;        // host: where guests' uploads are stored (Samples/Collab)
    };

    enum class State
    {
        Starting,     // host: hashing samples; guest: connecting
        Joining,      // guest: receiving the snapshot / installing the set
        Active,
        Reconnecting, // guest: link lost, trying again
        Ended,
    };

    static std::unique_ptr<CollabSession> host(Port&, std::unique_ptr<Transport>, Options);
    // `reconnect` makes a fresh transport for the same invite after the link drops (nullptr: no reconnect).
    static std::unique_ptr<CollabSession> guest(Port&, std::unique_ptr<Transport>, Options,
                                                std::function<std::unique_ptr<Transport>()> reconnect = {});
    ~CollabSession();

    // Message thread, often (the app calls it every 10 ms).
    void poll();

    // ---- from the app
    // An edit was applied to this replica's song (after the existing EditCommand path).
    void localEdit(const std::string& label);
    // Host: a local edit attributed to a guest (their dropped file).
    void localEditBy(AuthorId author, const std::string& label);
    bool undo();
    bool redo();
    std::string undoLabel() const;
    std::string redoLabel() const;
    void sendPresence(PresenceMsg);
    void sendLive(std::vector<LiveValue>);
    void sendPluginState(PluginStateMsg);
    // Shared transport. A guest asks the host, which schedules it for everyone.
    void requestTransport(TransportMsg::Action, double beat);
    void setFollowTransport(bool on) { followTransport_ = on; }
    bool followTransport() const { return followTransport_; }
    // Guest: upload a local file and ask the host to drop it.
    bool uploadAndDrop(const juce::File& file, int trackId, double beat, int scene);
    // Host only.
    void setRole(AuthorId who, Role role);
    void kick(AuthorId who);
    void approveJoin(const PeerId& peer, bool allow);
    void leave(const std::string& reason = "left the session");
    // Guest: the joined song is installed (Port::installSong finished).
    void snapshotInstalled();

    // ---- state for the UI
    bool isHost() const { return host_; }
    State state() const { return state_; }
    AuthorId self() const { return self_; }
    Role role() const;
    bool canEdit() const { return state_ == State::Active && role() != Role::Listener; }
    std::vector<Participant> participants() const { return participants_; }
    const Participant* participant(AuthorId) const;
    std::string displayName(AuthorId) const;
    struct RemotePresence
    {
        PresenceMsg presence;
        int64_t receivedMs = 0;
    };
    const std::map<AuthorId, RemotePresence>& presence() const { return presence_; }
    std::string status() const { return status_; }
    float progress() const { return progress_; } // < 0: none
    std::string inviteLink() const;
    std::string safetyCode(AuthorId) const;     // with the host (guest) or with a guest (host)
    std::vector<std::pair<PeerId, std::string>> joinRequests() const { return joinRequests_; }
    int64_t clockOffsetMs() const { return clockOffset_; } // host clock - this clock
    int64_t roundTripMs() const { return roundTrip_; }
    uint64_t seq() const;
    size_t pendingEdits() const { return host_ ? 0 : client_.pending(); }
    size_t downloadsPending() const { return downloads_.size() + wanted_.size() + uploads_.size(); }
    const AssetCatalog& assets() const { return *catalog_; }
    // Guest: the host's saved set, for "Save a Copy" (empty before the snapshot).
    const std::string& sourceFile() const { return sourceFile_; }
    const std::string& sourceName() const { return sourceName_; }
    // Statistics for probes.
    struct Stats
    {
        uint64_t commits = 0, rejects = 0, intents = 0, bytesIn = 0, bytesOut = 0, dropped = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    CollabSession(Port&, std::unique_ptr<Transport>, Options, bool host);

    struct Peer
    {
        PeerId peer;
        AuthorId author = 0;
        bool welcomed = false;
        std::string safetyCode;
        std::string rejoinToken;
        int64_t connectedMs = 0;
        // Upload in progress (guest drop).
        uint64_t uploadId = 0;
        juce::File uploadFile;
        std::unique_ptr<juce::FileOutputStream> upload;
        uint64_t uploadTotal = 0, uploadWritten = 0;
        // Asset requests waiting for channel space.
        std::deque<AssetRequestMsg> assetQueue;
    };

    void send(const PeerId&, Channel, const Message&);
    void broadcast(Channel, const Message&, const PeerId& except = {});
    void handle(const TransportEvent&);
    void handleHost(Peer&, Message&&);
    void handleGuest(Message&&);
    void welcome(Peer&, const HelloMsg&);
    void sendSnapshot(Peer&);
    void broadcastParticipants();
    void setParticipant(const Participant&);
    Peer* peerByAuthor(AuthorId);
    Peer* peerById(const PeerId&);
    Applier applier();
    void sendCommit(const CommitMsg&);
    void hostReject(Peer&, const RejectMsg&);
    void hashRefs(const std::vector<model::FileRef>&);
    void collectHashes();
    void serveAssets();
    void requestAssets();
    void receiveChunk(const AssetChunkMsg&);
    void clockTick();
    void flushPresence();
    void scheduleTransport(TransportMsg);
    void receiveUpload(Peer&, const UploadMsg&);
    void sendUploads();
    void receiveDrop(Peer&, const DropRequestMsg&);
    void end(const std::string& reason);
    void connectionLost(const std::string& reason);
    std::string conflictText(const RejectMsg&) const;
    static int64_t now();

    Port& port_;
    std::unique_ptr<Transport> transport_;
    Options options_;
    bool host_;
    State state_ = State::Starting;
    AuthorId self_ = hostAuthor;
    std::vector<Participant> participants_;
    std::map<AuthorId, RemotePresence> presence_;
    std::string status_;
    float progress_ = -1.0f;
    Stats stats_;

    // Host
    Sequencer sequencer_;
    std::map<PeerId, std::unique_ptr<Peer>> peers_;
    AuthorId nextAuthor_ = 1;
    std::map<AuthorId, std::string> rejoinTokens_;
    std::map<AuthorId, Participant> departed_; // roles and colours kept for a rejoin
    std::vector<std::pair<PeerId, std::string>> joinRequests_;
    struct HashResult
    {
        std::mutex lock;
        std::vector<std::pair<AssetEntry, std::string>> done; // entry, local path
        size_t total = 0, completed = 0;
    };
    std::shared_ptr<HashResult> hashing_ = std::make_shared<HashResult>();
    bool initialHashDone_ = false;
    std::vector<PeerId> awaitingSnapshot_;

    // Guest
    Client client_;
    std::function<std::unique_ptr<Transport>()> reconnect_;
    std::string rejoinToken_, hostSafetyCode_;
    std::deque<Message> buffered_; // commits etc. that arrive while the snapshot installs
    std::optional<SnapshotMsg> snapshot_;
    std::string sourceFile_, sourceName_;
    std::map<std::string, std::unique_ptr<AssetDownload>> downloads_; // hash -> download
    std::map<std::string, uint64_t> requested_;                       // hash -> next offset to request
    std::deque<std::string> wanted_;                                  // hashes still to fetch, in order
    uint64_t downloadedBytes_ = 0, downloadTotal_ = 0;
    int64_t reconnectAtMs_ = 0;
    int reconnectAttempts_ = 0;
    struct Upload
    {
        uint64_t id = 0;
        juce::File file;
        uint64_t offset = 0, total = 0;
        DropRequestMsg drop;
    };
    std::deque<Upload> uploads_;
    uint64_t nextUploadId_ = 1;
    bool resolverInstalled_ = false;
    bool joinPlayback_ = false;

    // Both
    std::shared_ptr<AssetCatalog> catalog_ = std::make_shared<AssetCatalog>();
    juce::ThreadPool pool_{1};
    bool followTransport_ = true;
    int64_t clockOffset_ = 0, roundTrip_ = -1, nextClockMs_ = 0;
    int clockPings_ = 0;
    std::deque<std::pair<int64_t, int64_t>> clockSamples_; // (rtt, offset)
    int64_t lastPresenceMs_ = 0;
    std::optional<PresenceMsg> pendingPresence_;
    bool endedNotified_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace sonora::collab
