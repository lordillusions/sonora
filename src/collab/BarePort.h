#pragma once
// A CollabSession::Port on a bare Song, with no engine or UI: tests and tools/collab_probe use it to run real
// sessions headlessly. The app's Port (app/AppCollab.cpp) adds engine refresh, live values and the transport.

#include "collab/CollabSession.h"
#include "io/als/AlsReader.h"

namespace sonora::collab
{

class BarePort : public CollabSession::Port
{
public:
    std::unique_ptr<model::Song> document;
    std::vector<std::string> notices, arrived;
    std::vector<TransportMsg> transports;
    std::vector<LiveValue> live;
    std::vector<ChatEntry> chats; // lines from others (chatArrived)
    std::vector<std::pair<AuthorId, std::string>> voices; // RTP packets (voiceArrived)
    std::string endReason;
    bool ended_ = false;
    CollabSession* session = nullptr; // set after creating the session (guests call snapshotInstalled from installSong)

    model::Song* song() override { return document.get(); }
    void applyObjects(const std::vector<ObjectState>& states) override
    {
        if (document) collab::applyObjects(*document, states);
    }
    std::string resolveSample(const model::FileRef& ref) override { return document ? io::resolveProjectSamplePath(*document, ref) : std::string(); }
    void installSong(std::unique_ptr<model::Song> song) override
    {
        document = std::move(song);
        if (session) session->snapshotInstalled();
    }
    void assetsArrived(const std::vector<std::string>& hashes) override { arrived.insert(arrived.end(), hashes.begin(), hashes.end()); }
    void applyLive(const std::vector<LiveValue>& values) override { live.insert(live.end(), values.begin(), values.end()); }
    void applyTransport(const TransportMsg& m, int64_t) override { transports.push_back(m); }
    void chatArrived(const ChatEntry& e) override { chats.push_back(e); }
    void voiceArrived(AuthorId speaker, const std::string& rtp) override { voices.push_back({speaker, rtp}); }
    void notify(const std::string& text) override { notices.push_back(text); }
    void ended(const std::string& reason) override
    {
        ended_ = true;
        endReason = reason;
    }
};

} // namespace sonora::collab
